/* SPDX-License-Identifier: GPL-2.0-or-later
 * Stub tp_dc_* + tp_fb_* implementation for the host lifecycle tests. No /dev
 * access, no ioctl. Records close/claim/unblank/restore order into the shared
 * stub log and honours a set of failure/cancel controls (see tests/stub_gpu.c).
 *
 * Controls (env):
 *   TP_STUB_OPEN_PRESENT_FAIL   1 -> tp_dc_open_present fails
 *   TP_STUB_ACQUIRE_FAIL        1 -> tp_dc_acquire_window fails
 *   TP_STUB_STATUS_ENABLED      0/1 prior GET_STATUS enabled flag (default 1)
 *   TP_STUB_GET_STATUS_FAIL     1 -> tp_dc_get_status fails (prior state unknown)
 *   TP_STUB_STATUS_AFTER_UNBLANK  enabled flag after a successful unblank
 *   TP_STUB_VBLANK_ADVANCE      0 -> tp_dc_wait_ready reports "not advancing"
 *   TP_STUB_FB_OPEN_FAIL        1 -> tp_fb_open fails
 *   TP_STUB_UNBLANK_RESULT      0 ok / nonzero fail for tp_fb_unblank
 *   TP_STUB_UNBLANK_PARTIAL     1 -> unblank errors yet may have partly enabled
 *   TP_STUB_RESTORE_RESULT      0 ok / nonzero fail for tp_fb_restore
 *   TP_STUB_PRIOR_BLANK         prior fb0 blank level reported by tp_fb_open
 *   TP_STUB_FB_BUDGET_FAIL      1 -> tp_fb_set_budget_ms fails (fail closed)
 */
#include "tpstub.h"
#include "dc.h"

#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>

static int g_flip_calls;
static int g_unblanked;
static unsigned g_fb_budget_ms;
static int g_fb_budget_calls;

void tpstub_dc_reset(void)
{
    g_flip_calls = 0;
    g_unblanked = 0;
    g_fb_budget_ms = 0;
    g_fb_budget_calls = 0;
}

unsigned tpstub_fb_budget_ms(void) { return g_fb_budget_ms; }
int tpstub_fb_budget_calls(void) { return g_fb_budget_calls; }

static int envi(const char *k, int d)
{
    const char *v = getenv(k);
    return v ? atoi(v) : d;
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

int tp_dc_autodetect(int *out_num) { *out_num = 0; return 0; }

int tp_dc_open_readonly(tp_dc *d, int dc_num)
{
    d->dc_num = dc_num;
    d->fd = 3;
    return 0;
}

int tp_dc_open_present(tp_dc *d, int dc_num)
{
    if (envi("TP_STUB_OPEN_PRESENT_FAIL", 0)) {
        tpstub_event("dc_open_fail");
        return -1;
    }
    d->dc_num = dc_num;
    d->fd = 4;
    tpstub_event("dc_open");
    return 0;
}

int tp_dc_acquire_window(tp_dc *d, int win_index)
{
    if (envi("TP_STUB_ACQUIRE_FAIL", 0)) {
        tpstub_event("window_acquire_fail");
        return -1;
    }
    d->win_index = win_index;
    d->owns_window = 1;
    tpstub_window_acquire();
    return 0;
}

int tp_dc_ctrl_open(tp_dc *d) { d->ctrl_fd = 5; return 0; }

int tp_dc_get_status(tp_dc *d, struct tegra_dc_ext_status *st)
{
    (void)d;
    if (envi("TP_STUB_GET_STATUS_FAIL", 0)) {
        tpstub_event("get_status_fail");
        return -1;
    }
    int en = envi("TP_STUB_STATUS_ENABLED", 1);
    if (g_unblanked)
        en = envi("TP_STUB_STATUS_AFTER_UNBLANK", 1);
    st->flags = en ? TEGRA_DC_EXT_FLAGS_ENABLED : 0u;
    return 0;
}

int tp_dc_get_vblank_syncpt(tp_dc *d, uint32_t *id)
{
    (void)d;
    *id = 7;
    return 0;
}

int tp_dc_syncpt_read(tp_dc *d, uint32_t id, uint32_t *value)
{
    (void)d; (void)id;
    *value = 1;
    return 0;
}

int tp_dc_wait_ready(tp_dc *d, int budget_ms)
{
    (void)budget_ms;
    struct tegra_dc_ext_status st;
    memset(&st, 0, sizeof(st));
    tp_dc_get_status(d, &st);
    if (!(st.flags & TEGRA_DC_EXT_FLAGS_ENABLED)) {
        tpstub_event("wait_ready_disabled");
        return -1;
    }
    if (!envi("TP_STUB_VBLANK_ADVANCE", 1)) {
        tpstub_event("wait_ready_vblank_stuck");
        return -1;
    }
    tpstub_event("wait_ready_ok");
    return 0;
}

int tp_dc_flip4(tp_dc *d, struct tegra_dc_ext_flip_windowattr *win, int *post_fd)
{
    (void)d; (void)win;
    if (post_fd)
        *post_fd = -1;
    int cur = g_flip_calls++;
    if (cur == envi("TP_STUB_FLIP_FAIL_IDX", -1))
        return -1;
    if (post_fd)
        *post_fd = 20 + cur;
    tpstub_flip();
    int ca = envi("TP_STUB_CANCEL_AFTER_FLIPS", 0);
    if (ca > 0 && (cur + 1) >= ca)
        raise(SIGINT);
    return 0;
}

int tp_dc_release_wait(int post_fd, int budget_ms, tp_release_result *out)
{
    (void)post_fd; (void)budget_ms;
    if (out)
        memset(out, 0, sizeof(*out));
    const char *m = getenv("TP_STUB_RELEASE_MODE");
    int v = TP_REL_RETIRED;
    if (m && !strcmp(m, "notready"))
        v = TP_REL_NOT_READY;
    else if (m && !strcmp(m, "fail"))
        v = TP_REL_FAIL;
    else if (m && !strcmp(m, "notty"))
        v = TP_REL_FAIL;
    if (out) {
        out->verdict = v;
        out->revents = (v == TP_REL_RETIRED) ? POLLIN : 0;
        out->info_rc = (m && !strcmp(m, "notty")) ? -1 : 0;
        out->info_errno = (m && !strcmp(m, "notty")) ? ENOTTY : 0;
        out->status = (v == TP_REL_RETIRED) ? 1 : 0;
        out->num_fences = (v == TP_REL_RETIRED) ? 1u : 0u;
    }
    tpstub_release(v == TP_REL_RETIRED ? "release_retired" :
                   (v == TP_REL_NOT_READY ? "release_notready" : "release_fail"));
    return v;
}

int tp_fb_set_budget_ms(unsigned int budget_ms)
{
    if (envi("TP_STUB_FB_BUDGET_FAIL", 0)) {
        tpstub_event("fb_budget_fail");
        return -1;
    }
    g_fb_budget_ms = budget_ms;
    g_fb_budget_calls++;
    tpstub_event("fb_budget_set");
    return 0;
}

int tp_fb_open(tp_dc *d)
{
    if (envi("TP_STUB_FB_OPEN_FAIL", 0)) {
        tpstub_event("fb_open_fail");
        return -1;
    }
    d->fb_fd = 6;
    d->fb_blank_known = 1;
    d->fb_blank_value = envi("TP_STUB_PRIOR_BLANK", 4);
    tpstub_event("fb_open");
    return 0;
}

int tp_fb_unblank(tp_dc *d)
{
    (void)d;
    int r = envi("TP_STUB_UNBLANK_RESULT", 0);
    if (r == 0 || envi("TP_STUB_UNBLANK_PARTIAL", 0))
        g_unblanked = 1;
    tpstub_unblank();
    return r == 0 ? 0 : -1;
}

int tp_fb_restore(tp_dc *d)
{
    (void)d;
    tpstub_restore();
    return envi("TP_STUB_RESTORE_RESULT", 0) ? -1 : 0;
}

void tp_fb_close(tp_dc *d)
{
    d->fb_fd = -1;
    tpstub_event("fb_close");
}

void tp_dc_close(tp_dc *d)
{
    tpstub_dc_close();
    d->fd = -1;
    d->ctrl_fd = -1;
}
