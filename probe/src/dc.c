/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "dc.h"
#include "fb_budget.h"

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>

#include <nvhost_ioctl.h>

/* fbdev blank level ABI constants. <linux/fb.h> is a kernel uapi header that
 * is not reliably present in the cross toolchain's default include set, so the
 * few values we use are pinned locally (they are ABI-fixed). */
#ifndef FBIOBLANK
#define FBIOBLANK 0x4611 /* _IO('F', 0x11) */
#endif
#define TP_FB_BLANK_UNBLANK   0
#define TP_FB_BLANK_POWERDOWN 4

/* Whole-process bookkeeping budget shared by every FB ioctl. A kernel ioctl
 * may itself block uninterruptibly; we cannot force-preempt it, so we bound our
 * own bookkeeping and refuse further FB ioctls once the budget is exhausted.
 * The policy lives in fb_budget.c (testable with an injected clock); normal
 * runs keep the 60 s default, and --soak may raise it once, before the first FB
 * ioctl, via tp_fb_set_budget_ms(). This is bookkeeping, not a kernel ioctl
 * preemption guarantee. */
static tp_fb_budget g_fb_budget = {
    TP_FB_IOCTL_BUDGET_MS_DEFAULT, 0, 0.0
};

static double mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

static int fb_budget_ok(void)
{
    return tp_fb_budget_check(&g_fb_budget, mono_ms());
}

/* See dc.h. Raises the measured whole-process FB ioctl budget that
 * fb_budget_ok() enforces; bookkeeping only. Only accepted before the first FB
 * ioctl (g_fb_budget inactive) and only within [default, max]; otherwise it
 * fails closed and the budget is unchanged. Never resets a running timer and
 * never removes the exhaustion check. */
int tp_fb_set_budget_ms(unsigned int budget_ms)
{
    return tp_fb_budget_set(&g_fb_budget, budget_ms);
}

void tp_dc_init(tp_dc *d)
{
    memset(d, 0, sizeof(*d));
    d->fd = -1;
    d->ctrl_fd = -1;
    d->fb_fd = -1;
    d->dc_num = -1;
    d->vblank_syncpt = TP_INVALID_SYNCPT;
}

static uint32_t sample_syncpt(int ctrl_fd, uint32_t id, int *ok)
{
    struct nvhost_ctrl_syncpt_read_args a;
    memset(&a, 0, sizeof(a));
    a.id = id;
    *ok = 0;
    if (ctrl_fd < 0)
        return 0;
    if (ioctl(ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a) == 0) {
        *ok = 1;
        return a.value;
    }
    return 0;
}

int tp_dc_autodetect(int *out_num)
{
    int best = -1, enabled_fallback = -1;
    uint32_t best_delta = 0;
    int ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR | O_CLOEXEC);

    for (int n = 0; n < 4; n++) {
        char path[64];
        snprintf(path, sizeof(path), "/dev/tegra_dc_%d", n);
        int fd = open(path, O_RDONLY | O_CLOEXEC);
        if (fd < 0)
            continue;

        struct tegra_dc_ext_status st;
        memset(&st, 0, sizeof(st));
        int have_status = (ioctl(fd, TEGRA_DC_EXT_GET_STATUS, &st) == 0);

        uint32_t sid = TP_INVALID_SYNCPT;
        int have_sid = (ioctl(fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, &sid) == 0);

        uint32_t delta = 0;
        if (have_sid && ctrl_fd >= 0) {
            int ok0 = 0, ok1 = 0;
            uint32_t v0 = sample_syncpt(ctrl_fd, sid, &ok0);
            struct timespec ts = { 0, 120000000 }; /* 120 ms */
            nanosleep(&ts, NULL);
            uint32_t v1 = sample_syncpt(ctrl_fd, sid, &ok1);
            if (ok0 && ok1)
                delta = v1 - v0;
        }

        int enabled = have_status && (st.flags & TEGRA_DC_EXT_FLAGS_ENABLED);
        tp_log("autodetect %s: status=%s flags=0x%08x syncpt=%u delta=%u",
               path, have_status ? "ok" : "n/a", st.flags, sid, delta);

        if (enabled && enabled_fallback < 0)
            enabled_fallback = n;
        if (delta > best_delta) {
            best_delta = delta;
            best = n;
        }
        close(fd);
    }
    if (ctrl_fd >= 0)
        close(ctrl_fd);

    if (best >= 0 && best_delta > 0) {
        tp_log("autodetect: selected /dev/tegra_dc_%d (advancing vblank)", best);
        *out_num = best;
        return 0;
    }
    if (enabled_fallback >= 0) {
        tp_log("autodetect: no advancing syncpt; using enabled head %d", enabled_fallback);
        *out_num = enabled_fallback;
        return 0;
    }
    if (best >= 0) {
        *out_num = best;
        return 0;
    }
    tp_log("autodetect: no /dev/tegra_dc_* candidate found");
    return -1;
}

int tp_dc_open_readonly(tp_dc *d, int dc_num)
{
    snprintf(d->path, sizeof(d->path), "/dev/tegra_dc_%d", dc_num);
    d->fd = open(d->path, O_RDONLY | O_CLOEXEC);
    if (d->fd < 0) {
        tp_log("open %s (read-only) failed: %s", d->path, strerror(errno));
        return -1;
    }
    d->dc_num = dc_num;

    struct tegra_dc_ext_status st;
    memset(&st, 0, sizeof(st));
    (void)tp_dc_get_status(d, &st); /* informational only */

    uint32_t sid = TP_INVALID_SYNCPT;
    if (tp_dc_get_vblank_syncpt(d, &sid) == 0) {
        d->vblank_syncpt = sid;
        d->have_vblank_syncpt = 1;
    }
    tp_log("opened %s read-only (no GET_WINDOW, no FLIP4)", d->path);
    return 0;
}

int tp_dc_open_present(tp_dc *d, int dc_num)
{
    snprintf(d->path, sizeof(d->path), "/dev/tegra_dc_%d", dc_num);
    /* O_CLOEXEC: a forked helper inheriting this fd would keep the window
     * claim alive (see reference flip_layer.c). */
    d->fd = open(d->path, O_RDWR | O_CLOEXEC);
    if (d->fd < 0) {
        tp_log("open %s (present) failed: %s", d->path, strerror(errno));
        return -1;
    }
    d->dc_num = dc_num;
    tp_log("opened %s for present (window not yet claimed)", d->path);
    return 0;
}

int tp_dc_acquire_window(tp_dc *d, int win_index)
{
    if (d->fd < 0) {
        tp_log("acquire window %d: %s not open", win_index, d->path);
        return -1;
    }
    if (ioctl(d->fd, TEGRA_DC_EXT_GET_WINDOW, (unsigned long)win_index) < 0) {
        int e = errno;
        tp_log("GET_WINDOW %d on %s failed: %s%s", win_index, d->path,
               strerror(e), e == EBUSY ? " (EBUSY: another owner; do NOT steal)" : "");
        return -1;
    }
    d->win_index = win_index;
    d->owns_window = 1;

    uint32_t sid = TP_INVALID_SYNCPT;
    if (tp_dc_get_vblank_syncpt(d, &sid) == 0) {
        d->vblank_syncpt = sid;
        d->have_vblank_syncpt = 1;
    }
    tp_log("acquired window %d on %s, vblank syncpt=%u",
           win_index, d->path, d->vblank_syncpt);
    return 0;
}

int tp_dc_wait_ready(tp_dc *d, int budget_ms)
{
    if (!d->have_vblank_syncpt) {
        uint32_t sid = TP_INVALID_SYNCPT;
        if (tp_dc_get_vblank_syncpt(d, &sid) == 0) {
            d->vblank_syncpt = sid;
            d->have_vblank_syncpt = 1;
        }
    }
    double t0 = mono_ms();
    int have = 0;
    uint32_t first = 0;
    for (;;) {
        struct tegra_dc_ext_status st;
        memset(&st, 0, sizeof(st));
        if (tp_dc_get_status(d, &st) == 0 &&
            (st.flags & TEGRA_DC_EXT_FLAGS_ENABLED) && d->have_vblank_syncpt) {
            uint32_t v = 0;
            if (tp_dc_syncpt_read(d, d->vblank_syncpt, &v) == 0) {
                if (!have) {
                    first = v;
                    have = 1;
                } else if (v != first) {
                    tp_vlog("head ready: status ENABLED, vblank %u -> %u", first, v);
                    return 0;
                }
            }
        }
        if (mono_ms() - t0 >= (double)budget_ms)
            break;
        struct timespec ts = { 0, 16000000 }; /* 16 ms bounded sample */
        nanosleep(&ts, NULL);
    }
    tp_log("head not ready within %d ms (need status ENABLED and an advancing "
           "vblank; no SYNCPT_WAITEX)", budget_ms);
    return -1;
}

int tp_dc_ctrl_open(tp_dc *d)
{
    if (d->ctrl_fd >= 0)
        return 0;
    d->ctrl_fd = open("/dev/nvhost-ctrl", O_RDWR | O_CLOEXEC);
    if (d->ctrl_fd < 0) {
        tp_log("open /dev/nvhost-ctrl failed: %s", strerror(errno));
        return -1;
    }
    return 0;
}

int tp_dc_get_status(tp_dc *d, struct tegra_dc_ext_status *st)
{
    if (ioctl(d->fd, TEGRA_DC_EXT_GET_STATUS, st) < 0) {
        tp_vlog("GET_STATUS %s failed: %s", d->path, strerror(errno));
        return -1;
    }
    return 0;
}

int tp_dc_get_vblank_syncpt(tp_dc *d, uint32_t *id)
{
    if (ioctl(d->fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, id) < 0) {
        tp_vlog("GET_VBLANK_SYNCPT %s failed: %s", d->path, strerror(errno));
        return -1;
    }
    return 0;
}

int tp_dc_syncpt_read(tp_dc *d, uint32_t id, uint32_t *value)
{
    struct nvhost_ctrl_syncpt_read_args a;
    memset(&a, 0, sizeof(a));
    a.id = id;
    if (tp_dc_ctrl_open(d) < 0)
        return -1;
    if (ioctl(d->ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a) < 0) {
        tp_vlog("SYNCPT_READ id=%u failed: %s", id, strerror(errno));
        return -1;
    }
    *value = a.value;
    return 0;
}

int tp_dc_flip4(tp_dc *d, struct tegra_dc_ext_flip_windowattr *win, int *post_fd)
{
    struct tegra_dc_ext_flip_4 flip;
    memset(&flip, 0, sizeof(flip));
    flip.win = (__u64)(uintptr_t)win;
    flip.win_num = 1;
    flip.post_syncpt_fd = -1; /* kernel writes the output fence fd here */
    if (post_fd)
        *post_fd = -1;

    if (ioctl(d->fd, TEGRA_DC_EXT_FLIP4, &flip) < 0) {
        tp_log("FLIP4 on %s failed: %s", d->path, strerror(errno));
        return -1;
    }
    if (post_fd)
        *post_fd = flip.post_syncpt_fd;
    return 0;
}

int tp_dc_release_wait(int post_fd, int budget_ms, tp_release_result *out)
{
    return (int)tp_release_poll_info(post_fd, budget_ms, out);
}

int tp_fb_open(tp_dc *d)
{
    if (d->fb_fd >= 0)
        return 0;
    /* Read the prior blank level (read-only; may be absent). */
    FILE *f = fopen("/sys/class/graphics/fb0/blank", "r");
    if (f) {
        int v = -1;
        if (fscanf(f, "%d", &v) == 1) {
            d->fb_blank_known = 1;
            d->fb_blank_value = v;
        }
        fclose(f);
    }
    d->fb_fd = open("/dev/fb0", O_RDWR | O_CLOEXEC);
    if (d->fb_fd < 0) {
        tp_log("open /dev/fb0 (O_RDWR) failed: %s", strerror(errno));
        return -1;
    }
    if (d->fb_blank_known)
        tp_log("opened /dev/fb0; prior blank level = %d", d->fb_blank_value);
    else
        tp_log("opened /dev/fb0; prior blank level unreadable "
               "(/sys/class/graphics/fb0/blank)");
    return 0;
}

int tp_fb_unblank(tp_dc *d)
{
    if (d->fb_fd < 0) {
        tp_log("unblank: /dev/fb0 not open");
        return -1;
    }
    if (fb_budget_ok() != 0)
        return -1;
    double t0 = mono_ms();
    int r = ioctl(d->fb_fd, FBIOBLANK, TP_FB_BLANK_UNBLANK);
    int e = errno;
    tp_log("FBIOBLANK(UNBLANK) on /dev/fb0 -> %d (%s) in %.1f ms",
           r, r == 0 ? "ok" : strerror(e), mono_ms() - t0);
    d->fb_unblanked = (r == 0);
    return r == 0 ? 0 : -1;
}

int tp_fb_restore(tp_dc *d)
{
    if (d->fb_fd < 0) {
        tp_log("restore: /dev/fb0 not open; cannot restore head state");
        return -1;
    }
    int level;
    if (d->fb_blank_known &&
        (d->fb_blank_value == 2 || d->fb_blank_value == 3 || d->fb_blank_value == 4)) {
        level = d->fb_blank_value;
        tp_log("restore: restoring prior fb0 blank level %d", level);
    } else {
        level = TP_FB_BLANK_POWERDOWN;
        if (d->fb_blank_known)
            tp_log("restore: prior fb0 blank level %d is not in the restorable "
                   "set {2,3,4}; falling back to FB_BLANK_POWERDOWN to restore "
                   "the disabled state", d->fb_blank_value);
        else
            tp_log("restore: prior fb0 blank level unreadable; falling back to "
                   "FB_BLANK_POWERDOWN to restore the disabled state");
    }
    if (fb_budget_ok() != 0)
        return -1;
    int r = ioctl(d->fb_fd, FBIOBLANK, level);
    tp_log("FBIOBLANK(%d) on /dev/fb0 -> %d (%s)", level, r,
           r == 0 ? "ok" : strerror(errno));
    return r == 0 ? 0 : -1;
}

void tp_fb_close(tp_dc *d)
{
    if (d->fb_fd >= 0) {
        close(d->fb_fd);
        d->fb_fd = -1;
    }
}

void tp_dc_close(tp_dc *d)
{
    if (d->ctrl_fd >= 0) {
        close(d->ctrl_fd);
        d->ctrl_fd = -1;
    }
    if (d->fd >= 0) {
        /* Close while still owning the window. PUT_WINDOW first would clear
         * ownership and bypass the kernel's flush/blank/unpin on close. Only
         * claim ownership in the log when GET_WINDOW actually succeeded. */
        if (d->owns_window)
            tp_log("closing %s while still owning window %d (no PUT_WINDOW)",
                   d->path, d->win_index);
        else
            tp_log("closing %s (no window was claimed)", d->path);
        close(d->fd);
        d->fd = -1;
    }
}
