/* SPDX-License-Identifier: GPL-2.0-or-later
 * Kernel sync_file retirement gate for reuse mode.
 *
 * A slot may be recycled ONLY when its own retained FLIP4 post_syncpt_fd
 * reports POLLIN with none of POLLERR/POLLHUP/POLLNVAL AND a successful
 * kernel fence-status query with status > 0 proven by a well-formed,
 * non-empty fence description.  Anything unknown or status <= 0 fails closed
 * for reuse.  eventfd / timing / vblank counts are never accepted as
 * retirement evidence.
 *
 * Two kernel ABI generations exist for the fence fd and the running L4T kernel
 * provides BOTH frameworks (see reports/M3g-*).  The query order is:
 *   1. modern SYNC_IOC_FILE_INFO (dma-buf sync_file, drivers/dma-buf/);
 *   2. ONLY when that returns ENOTTY, the legacy SYNC_IOC_FENCE_INFO (Android
 *      staging sync retained by NVIDIA, drivers/staging/android/).
 * A modern failure for any other reason (EINVAL/EFAULT/...), a negative or
 * unknown status, or a malformed/truncated/empty legacy reply is a hard fail:
 * there is no fallback from errors and no "assume retired".
 *
 * Legacy golden source (GPL-2.0), pinned and preserved under
 * logs/artifacts/m3g/sources/:
 *   - drivers/staging/android/uapi/sync.h        (struct sync_fence_info_data,
 *                                                 struct sync_pt_info,
 *                                                 SYNC_IOC_FENCE_INFO)
 *   - drivers/staging/android/sync.c             (sync_fence_poll,
 *                                                 sync_fence_ioctl_fence_info,
 *                                                 sync_fill_pt_info)
 *   - drivers/video/tegra/host/nvhost_sync.c     (sync_fence_create caller)
 */
#ifndef TP_SYNC_RELEASE_H
#define TP_SYNC_RELEASE_H

#include "tp.h"
#include <poll.h>
#include <sys/ioctl.h>

/* ------------------------------------------------------------------------- *
 * Legacy Android-staging sync fence-info ABI (L4T 4.9.140 / r32.7.4).
 *
 * ADAPTED (not verbatim) from the Linux L4T kernel UAPI header
 *   drivers/staging/android/uapi/sync.h
 *     Copyright (C) 2012 Google, Inc.
 *     License: GPL-2.0 (original kernel UAPI header)
 * Public origin: OE4T/linux-tegra-4.9 @ branch l4t/l4t-r32.7.4-4.9,
 *   commit 78abfa9d9f4c9bc47c91b64a4f1a02952b67db0e, file sha256
 *   d90f5f6a2e77371d9de7ba91595883ceb84e302b98453e37db09f7124679d9eb
 *   https://github.com/OE4T/linux-tegra-4.9/blob/l4t/l4t-r32.7.4-4.9/drivers/staging/android/uapi/sync.h
 * The field layout and ioctl number are kept ABI-identical so the query and
 * struct size are not guessed, but the type names are ADAPTED (tp_ prefix)
 * and the trailing flexible-array members (pt_info[0] / driver_data[0]) are
 * omitted: this decoder walks the records by their in-record len chain inside
 * a bounded byte buffer.  See third_party/PROVENANCE.md.  The kernel clamps
 * the requested size to 4096 and fills one struct sync_pt_info per fence.
 * ------------------------------------------------------------------------- */
#define TP_SYNC_IOC_MAGIC '>'

struct tp_sync_pt_info {
    uint32_t len;            /* == sizeof + driver_data length              */
    char     obj_name[32];
    char     driver_name[32];
    int32_t  status;         /* 0 active, 1 signaled, <0 error              */
    uint64_t timestamp_ns;
};

struct tp_sync_fence_info_data {
    uint32_t len;            /* in: caller buffer size; out: filled length  */
    char     name[32];
    int32_t  status;         /* out: 1 signaled, 0 active, <0 error         */
};

#define TP_SYNC_IOC_FENCE_INFO \
    _IOWR(TP_SYNC_IOC_MAGIC, 2, struct tp_sync_fence_info_data)

/* Bounded legacy query buffer: the kernel clamps any request to 4096; this is
 * a fixed stack buffer (no dynamic allocation).  Holds the FLIP4 post fence
 * (a single nvhost point of 80 + 8 bytes) plus slack for several points. */
#define TP_LEGACY_INFO_MAX 1024

typedef enum {
    TP_SYNC_API_NONE   = 0,  /* no query completed                          */
    TP_SYNC_API_MODERN = 1,  /* SYNC_IOC_FILE_INFO (dma-buf sync_file)      */
    TP_SYNC_API_LEGACY = 2   /* SYNC_IOC_FENCE_INFO (staging android sync)  */
} tp_sync_api;

typedef enum {
    TP_REL_NOT_READY = 0,   /* fence not yet signaled: retry within budget */
    TP_REL_RETIRED   = 1,   /* kernel reports signaled                   */
    TP_REL_FAIL      = 2    /* error/hup/invalid/unsupported/status<=0   */
} tp_release_verdict;

typedef struct {
    int      verdict;      /* tp_release_verdict                        */
    short    revents;      /* raw poll(2) revents (0 when not observed) */
    int      api;          /* tp_sync_api used for the status query     */
    int      info_rc;      /* query ioctl rc (0 = success)              */
    int      info_errno;   /* errno captured right after that ioctl      */
    int      status;       /* raw aggregate fence status                 */
    unsigned num_fences;   /* modern num_fences; legacy parsed point count */
    int      legacy_rc;    /* legacy FENCE_INFO rc (-E* on failure)      */
    int      legacy_errno; /* legacy FENCE_INFO errno                    */
    unsigned num_points;   /* legacy: validated sync_pt_info records     */
} tp_release_result;

/* Decoded/validated legacy reply (pure). */
typedef struct {
    int      status;       /* aggregate status from the header           */
    unsigned num_points;   /* number of well-formed point records        */
    int      bad;          /* 1 if the reply was rejected for an invalid  */
                           /* aggregate/point status (see tp_legacy_decode) */
} tp_legacy_decoded;

/* Pure decision function: no syscalls. Given an observed poll(2) result and a
 * sync_file info query, classify the fence. */
tp_release_verdict tp_release_decide(int poll_rc, short revents,
                                     int info_rc, int info_errno,
                                     int status, unsigned num_fences);

/* Pure policy: the legacy query may be attempted ONLY when the modern query
 * failed with ENOTTY.  Never from EINVAL/EFAULT/other errors, never on a
 * successful modern reply. */
int tp_release_should_try_legacy(int info_rc, int info_errno);

/* Pure, bounds-checked decoder for a legacy SYNC_IOC_FENCE_INFO reply buffer.
 * The status domain is 0 active / 1 signaled / <0 error (the kernel emits
 * nothing else).  Accepts only a well-formed, non-empty, in-bounds
 * sync_pt_info chain whose own `len` chain terminates exactly at the header's
 * `len`, with consistent statuses: an aggregate >1 is rejected, any point
 * status <0 or >1 is rejected, and when the aggregate is 1 EVERY point must
 * be 1 (aggregate1 + point0 is a contradiction and is rejected).
 * aggregate0/point0 decodes as NOT_READY; aggregate0/point1 decodes (may be
 * transient aggregate lag) and must not retire.  Never reads past `len`.
 * Returns 0 when well-formed, -1 otherwise (out->status still carries the raw
 * aggregate status for logging; out->bad is set on an invalid-status reject). */
int tp_legacy_decode(const void *buf, unsigned len, tp_legacy_decoded *out);

/* "modern"/"legacy"/"none" for logging. */
const char *tp_sync_api_name(int api);

/* Real Linux retirement gate: one finite poll(2) wait with a total budget of
 * budget_ms (EINTR-aware, never blocks past the deadline), followed by the
 * bounded kernel fence-status query described above on POLLIN. Writes the raw
 * evidence into *out when non-NULL. */
tp_release_verdict tp_release_poll_info(int fd, int budget_ms,
                                        tp_release_result *out);

#endif /* TP_SYNC_RELEASE_H */
