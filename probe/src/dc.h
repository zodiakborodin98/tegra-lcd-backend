/* SPDX-License-Identifier: GPL-2.0-or-later
 * Tegra DC (tegra_dc_ext) direct-present backend: open, GET_WINDOW, FLIP4,
 * syncpoint reads, the fbdev head-enable control, and the ownership-preserving
 * teardown. No X11/DRM-atomic.
 */
#ifndef TP_DC_H
#define TP_DC_H

#include "tp.h"
#include "sync_release.h"
#include "fb_budget.h"
#include <sys/types.h>

#ifndef __user
#define __user
#endif
#include <tegra_dc_ext.h>

typedef struct {
    int      fd;             /* /dev/tegra_dc_N (present fd; -1 if closed)   */
    int      ctrl_fd;        /* /dev/nvhost-ctrl (syncpt reads; -1 if closed)*/
    int      fb_fd;          /* /dev/fb0 (head enable/restore; -1 if closed)  */
    int      dc_num;
    int      win_index;
    char     path[64];
    uint32_t vblank_syncpt;  /* from GET_VBLANK_SYNCPT                       */
    int      have_vblank_syncpt;
    int      fb_blank_known; /* prior /sys/class/graphics/fb0/blank readable */
    int      fb_blank_value; /* prior blank level (0..4) when known          */
    int      fb_unblanked;   /* this run issued a successful FBIOBLANK(UNBLANK)*/
    int      head_changed;   /* this run changed disabled->enabled           */
    int      owns_window;    /* GET_WINDOW actually succeeded                */
} tp_dc;

void tp_dc_init(tp_dc *d);

/* Read-only autodetect of the active head. Opens each /dev/tegra_dc_N
 * O_RDONLY, reads GET_STATUS + GET_VBLANK_SYNCPT, samples the vblank syncpt
 * via /dev/nvhost-ctrl and picks the one that advances. No GET_WINDOW/FLIP4,
 * no display ownership, no writes. Returns 0 and *out_num on success. */
int tp_dc_autodetect(int *out_num);

/* Read-only open for --diagnostic-only (no GET_WINDOW). */
int tp_dc_open_readonly(tp_dc *d, int dc_num);

/* Present open: O_RDWR|O_CLOEXEC. Opens the control fd only; the window is
 * NOT claimed here (call tp_dc_acquire_window after the head is confirmed
 * ENABLED and advancing). Call only after explicit --present. */
int tp_dc_open_present(tp_dc *d, int dc_num);

/* Claim a window (GET_WINDOW). Fails cleanly on EBUSY (never steals an Xorg
 * plane). Sets d->owns_window on success and refreshes the vblank syncpt. */
int tp_dc_acquire_window(tp_dc *d, int win_index);

/* Bounded readiness gate: waits up to budget_ms for GET_STATUS to report
 * TEGRA_DC_EXT_FLAGS_ENABLED with the vblank syncpt advancing at least once
 * (READ + nanosleep; no SYNCPT_WAITEX). Returns 0 when ready, -1 on timeout.
 * This is a sanity check, not a proof against every enable-time race. */
int tp_dc_wait_ready(tp_dc *d, int budget_ms);

int tp_dc_ctrl_open(tp_dc *d);
int tp_dc_get_status(tp_dc *d, struct tegra_dc_ext_status *st);
int tp_dc_get_vblank_syncpt(tp_dc *d, uint32_t *id);
int tp_dc_syncpt_read(tp_dc *d, uint32_t id, uint32_t *value);

/* Submits one FLIP4 and returns the kernel-provided post_syncpt_fd in
 * *post_fd (>=0 slot). Caller owns that fd and must retain/close it. */
int tp_dc_flip4(tp_dc *d, struct tegra_dc_ext_flip_windowattr *win, int *post_fd);

/* Reuse-mode retirement gate for one retained FLIP4 post_syncpt_fd: a finite
 * poll(2) wait (<= budget_ms, defaults TP_REUSE_POLL_BUDGET_MS) plus a kernel
 * SYNC_IOC_FILE_INFO status check. Never waits on any other fence. */
int tp_dc_release_wait(int post_fd, int budget_ms, tp_release_result *out);

/* ---- fbdev head control (bounded DC0/fb0 prototype only) ----
 * Enables the already-configured head via fbdev control, not pixels: no
 * mmap/read/write, no PAN, no PUT_VSCREENINFO, no mode change. All FB ioctls
 * share a whole-process bookkeeping budget (default 60 s; see fb_budget.h); a
 * stuck kernel ioctl cannot be force-preempted, only bounded by our own
 * bookkeeping. */

/* Raise the whole-process FB-ioctl bookkeeping budget (see fb_budget.h).
 * Accepted ONLY before the first FB ioctl and only within
 * [TP_FB_IOCTL_BUDGET_MS_DEFAULT, TP_FB_IOCTL_BUDGET_MS_MAX]; returns -1 and
 * leaves the budget unchanged otherwise (fail closed). Bookkeeping bound only:
 * it does not preempt a stuck kernel ioctl, and it is never reset. */
int  tp_fb_set_budget_ms(unsigned int budget_ms);

/* Open /dev/fb0 O_RDWR|O_CLOEXEC and read /sys/class/graphics/fb0/blank
 * (read-only) so the prior blank level can be restored later. */
int  tp_fb_open(tp_dc *d);
/* FBIOBLANK(FB_BLANK_UNBLANK) on the open fb0 fd. Returns 0 on success. */
int  tp_fb_unblank(tp_dc *d);
/* Restore the prior blank level: FBIOBLANK(prior) when the prior value was
 * 2/3/4 (suspend/powerdown), else FB_BLANK_POWERDOWN (logged). */
int  tp_fb_restore(tp_dc *d);
void tp_fb_close(tp_dc *d);

/* Closes the DC fd WHILE STILL OWNING the window (no PUT_WINDOW) so the
 * kernel's close path runs flush/blank/unpin. Never PUT_WINDOW-then-close.
 * The fb0 fd is closed separately (tp_fb_close) so restore can run first. */
void tp_dc_close(tp_dc *d);

#endif /* TP_DC_H */
