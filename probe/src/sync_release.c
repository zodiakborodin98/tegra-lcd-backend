/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sync_release.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <linux/sync_file.h>

/* ABI guards: the legacy layout and ioctl must match the pinned L4T source
 * (drivers/staging/android/uapi/sync.h) exactly; the modern ioctl is taken
 * from the target sysroot's linux/sync_file.h. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(struct tp_sync_fence_info_data) == 40,
               "legacy header must be 40 bytes (len:u32 + name[32] + status:s32)");
_Static_assert(sizeof(struct tp_sync_pt_info) == 80,
               "legacy point record must be 80 bytes incl 8-byte timestamp align");
_Static_assert((unsigned)TP_SYNC_IOC_FENCE_INFO == 0xC0283E02u,
               "SYNC_IOC_FENCE_INFO must be _IOWR('>',2,40)");
_Static_assert((unsigned)SYNC_IOC_FILE_INFO == 0xC0383E04u,
               "SYNC_IOC_FILE_INFO must be _IOWR('>',4,struct sync_file_info)");
#endif

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

const char *tp_sync_api_name(int api)
{
    switch (api) {
    case TP_SYNC_API_MODERN: return "modern";
    case TP_SYNC_API_LEGACY: return "legacy";
    default:                 return "none";
    }
}

int tp_release_should_try_legacy(int info_rc, int info_errno)
{
    /* Only an outright "this ioctl is not supported" from the modern query
     * may trigger the legacy query.  Never a successful reply, never another
     * error class. */
    return (info_rc != 0 && info_errno == ENOTTY) ? 1 : 0;
}

int tp_legacy_decode(const void *buf, unsigned len, tp_legacy_decoded *out)
{
    if (out) {
        out->status = 0;
        out->num_points = 0;
        out->bad = 0;
    }
    if (!buf || len < sizeof(struct tp_sync_fence_info_data))
        return -1;

    const unsigned char *p = buf;
    uint32_t total;
    int32_t  agg;
    memcpy(&total, p + offsetof(struct tp_sync_fence_info_data, len),
           sizeof(total));
    memcpy(&agg, p + offsetof(struct tp_sync_fence_info_data, status),
           sizeof(agg));
    if (out)
        out->status = agg;

    if (total < sizeof(struct tp_sync_fence_info_data))   /* shorter than hdr */
        return -1;
    if (total > len)                                      /* truncated reply  */
        return -1;
    /* Status domain (L4T drivers/staging/android/sync.c sync_fill_pt_info and
     * sync_fence_ioctl_fence_info): every aggregate/point status is 0 (active),
     * 1 (signaled) or <0 (error); the kernel never emits anything >1.  A value
     * >1 is therefore unknown/corrupt and fails closed here.  A negative
     * aggregate still decodes so the verdict layer can FAIL it explicitly. */
    if (agg > 1) {                                        /* unknown aggregate */
        if (out)
            out->bad = 1;
        return -1;
    }

    unsigned off = (unsigned)sizeof(struct tp_sync_fence_info_data);
    unsigned n = 0;
    while (off < total) {
        if (total - off < sizeof(struct tp_sync_pt_info))
            return -1;                                    /* not enough to read len */
        uint32_t plen;
        int32_t  pstat;
        memcpy(&plen, p + off + offsetof(struct tp_sync_pt_info, len),
               sizeof(plen));
        memcpy(&pstat, p + off + offsetof(struct tp_sync_pt_info, status),
               sizeof(pstat));
        if (plen < sizeof(struct tp_sync_pt_info))        /* malformed record */
            return -1;
        if (plen > total - off)                           /* overruns the reply */
            return -1;
        if (pstat < 0 || pstat > 1) {                     /* error/unknown pt */
            if (out)
                out->bad = 1;
            return -1;
        }
        if (agg == 1 && pstat != 1) {                     /* contradiction:   */
            /* A signaled aggregate (1) requires EVERY point signaled; a 0
             * point under a signaled fence is a stale/contradictory reply
             * (e.g. the aggregate latched late).  Never retire on it. */
            if (out)
                out->bad = 1;
            return -1;
        }
        off += plen;
        n++;
    }
    if (off != total)                                     /* ragged tail      */
        return -1;
    if (n == 0)                                           /* empty points     */
        return -1;
    if (out)
        out->num_points = n;
    return 0;
}

tp_release_verdict tp_release_decide(int poll_rc, short revents,
                                     int info_rc, int info_errno,
                                     int status, unsigned num_fences)
{
    (void)info_errno;
    if (poll_rc < 0)
        return TP_REL_FAIL;                 /* poll error */
    if (poll_rc == 0)
        return TP_REL_NOT_READY;            /* nothing ready within budget */
    if (revents & (POLLERR | POLLHUP | POLLNVAL))
        return TP_REL_FAIL;                 /* error/hup/invalid fd */
    if (!(revents & POLLIN))
        return TP_REL_NOT_READY;
    if (info_rc != 0)
        return TP_REL_FAIL;                 /* ENOTTY/unknown/error: fail closed */
    if (num_fences == 0)
        return TP_REL_FAIL;                 /* nothing to prove */
    if (status < 0)
        return TP_REL_FAIL;                 /* error fence */
    if (status == 0)
        return TP_REL_NOT_READY;            /* still active */
    return TP_REL_RETIRED;                  /* status > 0: signaled */
}

/* Legacy SYNC_IOC_FENCE_INFO query.  Returns a verdict and fills *out.  The
 * caller has already seen POLLIN with no error bits. */
static tp_release_verdict legacy_query(int fd, short rev, tp_release_result *out)
{
    union {
        struct tp_sync_fence_info_data hdr;
        unsigned char                  raw[TP_LEGACY_INFO_MAX];
    } u;
    memset(&u, 0, sizeof(u));
    u.hdr.len = (uint32_t)sizeof(u.raw);   /* request the whole bounded buffer */

    errno = 0;
    int lrc = ioctl(fd, TP_SYNC_IOC_FENCE_INFO, u.raw);
    int le = errno;
    if (out) {
        out->api = TP_SYNC_API_LEGACY;
        out->legacy_rc = lrc;
        out->legacy_errno = le;
        out->info_rc = lrc;
        out->info_errno = le;
    }
    if (lrc != 0)
        return TP_REL_FAIL;                /* ENOTTY/EINVAL/...: no legacy info */

    tp_legacy_decoded d;
    if (tp_legacy_decode(u.raw, (unsigned)sizeof(u.raw), &d) != 0) {
        if (out) {
            out->status = d.status;
            out->num_points = 0;
            out->num_fences = 0;
        }
        return TP_REL_FAIL;                /* malformed/truncated/empty: fail */
    }
    if (out) {
        out->status = d.status;
        out->num_points = d.num_points;
        out->num_fences = d.num_points;    /* legacy has no num_fences field */
    }
    return tp_release_decide(1, rev, 0, 0, d.status, d.num_points);
}

tp_release_verdict tp_release_poll_info(int fd, int budget_ms,
                                        tp_release_result *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
        out->verdict = TP_REL_NOT_READY;
        out->api = TP_SYNC_API_NONE;
    }
    if (fd < 0) {
        if (out)
            out->verdict = TP_REL_FAIL;
        return TP_REL_FAIL;
    }
    if (budget_ms < 0)
        budget_ms = 0;
    double deadline = mono_ms() + (double)budget_ms;

    for (;;) {
        double remain_d = deadline - mono_ms();
        int remain = (remain_d > 0.0) ? (int)remain_d : 0;
        if (remain <= 0) {
            if (out)
                out->verdict = TP_REL_NOT_READY;
            return TP_REL_NOT_READY;
        }
        struct pollfd pf;
        pf.fd = fd;
        pf.events = POLLIN;
        pf.revents = 0;
        int r = poll(&pf, 1, remain);
        if (r < 0) {
            int e = errno;
            if (e == EINTR)
                continue;                    /* bounded: re-check deadline */
            if (out) {
                out->revents = 0;
                out->verdict = TP_REL_FAIL;
            }
            return TP_REL_FAIL;
        }
        if (r == 0) {                        /* budget exhausted */
            if (out)
                out->verdict = TP_REL_NOT_READY;
            return TP_REL_NOT_READY;
        }

        short rev = pf.revents;
        if (out)
            out->revents = rev;
        if (rev & (POLLERR | POLLHUP | POLLNVAL)) {
            if (out)
                out->verdict = TP_REL_FAIL;
            return TP_REL_FAIL;
        }
        if (!(rev & POLLIN))
            continue;                        /* bounded re-poll */

        /* POLLIN seen: ask the kernel for the real fence status.  The modern
         * dma-buf sync_file query is tried first; only its ENOTTY (an older
         * kernel that kept the Android staging fence) selects the legacy query.
         * Struct is ABI-matching linux/sync_file.h and zero-initialised. */
        struct sync_file_info info;
        memset(&info, 0, sizeof(info));
        info.num_fences = 0;                 /* request the count */
        errno = 0;
        int irc = ioctl(fd, SYNC_IOC_FILE_INFO, &info);
        int ie = errno;
        if (out) {
            out->api = TP_SYNC_API_MODERN;
            out->info_rc = irc;
            out->info_errno = ie;
            out->status = info.status;
            out->num_fences = info.num_fences;
        }

        tp_release_verdict v;
        if (irc == 0) {
            v = tp_release_decide(1, rev, 0, 0, info.status,
                                  info.num_fences);
        } else if (tp_release_should_try_legacy(irc, ie)) {
            v = legacy_query(fd, rev, out);  /* modern unsupported only */
        } else {
            v = TP_REL_FAIL;                 /* other modern error: no fallback */
        }

        if (v == TP_REL_RETIRED) {
            /* The validated aggregate status is the acceptance criterion; a
             * second query with a per-fence array is not required. */
            if (out)
                out->verdict = v;
            return v;
        }
        if (v == TP_REL_FAIL) {
            if (out)
                out->verdict = v;
            return v;
        }
        /* NOT_READY: keep polling within the remaining budget. */
        if (mono_ms() >= deadline) {
            if (out)
                out->verdict = TP_REL_NOT_READY;
            return TP_REL_NOT_READY;
        }
    }
}
