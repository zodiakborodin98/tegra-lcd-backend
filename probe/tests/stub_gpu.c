/* SPDX-License-Identifier: GPL-2.0-or-later
 * Stub tp_gpu_* implementation + shared mock-control state for the lifecycle
 * tests. No Vulkan is used: only the tp_gpu struct (zeroed) and the env
 * controls. It mirrors the real gpu.c unsafe bookkeeping so main.c's reaction
 * (close DC, skip destroys, nonzero rc) can be asserted.
 *
 * Controls (env):
 *   TP_STUB_FAIL_STAGE        init|pipelines|pattern|scanout  -> that step fails
 *   TP_STUB_RENDER_FAIL_IDX   index whose render_frame() fails (CPU-side)
 *   TP_STUB_SUBMIT_LOST_IDX   index whose render_frame() fails as DEVICE_LOST
 *   TP_STUB_WAIT_RESULT       0 signal | 1 timeout | -1 error (all frames)
 *   TP_STUB_FLIP_FAIL_IDX     index whose FLIP4 fails (dc stub)
 *   TP_STUB_CANCEL_AFTER_FLIPS  raise SIGINT after N flips (dc stub)
 *   TP_STUB_OPEN_PRESENT_FAIL 1 -> tp_dc_open_present fails (dc stub)
 */
#include "tpstub.h"
#include "gpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static char g_ev[8192];
static int  g_ev_len;
static int  g_dc_closes;
static int  g_gpu_destroys;
static int  g_destroy_safe = 2;
static int  g_destroy_null;
static int  g_flips;
static int  g_unblanks;
static int  g_restores;
static int  g_window_acquires;
static int  g_releases;

void tpstub_reset(void)
{
    g_ev[0] = '\0';
    g_ev_len = 0;
    g_dc_closes = 0;
    g_gpu_destroys = 0;
    g_destroy_safe = 2;
    g_destroy_null = 0;
    g_flips = 0;
    g_unblanks = 0;
    g_restores = 0;
    g_window_acquires = 0;
    g_releases = 0;
}

void tpstub_event(const char *s)
{
    int n = (int)strlen(s);
    if (g_ev_len + n + 2 < (int)sizeof(g_ev)) {
        memcpy(g_ev + g_ev_len, s, (size_t)n);
        g_ev_len += n;
        g_ev[g_ev_len++] = '\n';
        g_ev[g_ev_len] = '\0';
    }
}

const char *tpstub_events(void) { return g_ev; }
int tpstub_dc_closes(void) { return g_dc_closes; }
int tpstub_gpu_destroys(void) { return g_gpu_destroys; }
int tpstub_destroy_safe(void) { return g_destroy_safe; }
int tpstub_destroy_null(void) { return g_destroy_null; }

int tpstub_flips(void) { return g_flips; }
int tpstub_unblanks(void) { return g_unblanks; }
int tpstub_restores(void) { return g_restores; }
int tpstub_window_acquires(void) { return g_window_acquires; }
int tpstub_releases(void) { return g_releases; }
void tpstub_release(const char *verdict)
{
    g_releases++;
    tpstub_event(verdict);
}

void tpstub_dc_close(void) { g_dc_closes++; tpstub_event("dc_close"); }
void tpstub_flip(void) { g_flips++; tpstub_event("flip"); }
void tpstub_unblank(void) { g_unblanks++; tpstub_event("fb_unblank"); }
void tpstub_restore(void) { g_restores++; tpstub_event("fb_restore"); }
void tpstub_window_acquire(void) { g_window_acquires++; tpstub_event("window_acquire"); }

static int envi(const char *k, int d)
{
    const char *v = getenv(k);
    return v ? atoi(v) : d;
}
static int envcmp(const char *k, const char *want)
{
    const char *v = getenv(k);
    return v && !strcmp(v, want);
}

void tp_gpu_zero(tp_gpu *g)
{
    memset(g, 0, sizeof(*g));
    for (int i = 0; i < TP_MAX_SCANOUT_BUFFERS; i++)
        g->out[i].fd = -1;
}

int tp_gpu_init(tp_gpu *g, const tp_config *c)
{
    (void)c;
    if (envcmp("TP_STUB_FAIL_STAGE", "init")) {
        tpstub_event("gpu_init_fail");
        return -1;
    }
    g->device = (VkDevice)(uintptr_t)0x1; /* fake, non-null */
    g->qfam = 0;
    tpstub_event("gpu_init_ok");
    return 0;
}

int tp_gpu_create_pipelines(tp_gpu *g)
{
    (void)g;
    if (envcmp("TP_STUB_FAIL_STAGE", "pipelines")) {
        tpstub_event("gpu_pipelines_fail");
        return -1;
    }
    tpstub_event("gpu_pipelines_ok");
    return 0;
}

int tp_gpu_create_pattern_image(tp_gpu *g, const tp_config *c)
{
    (void)g; (void)c;
    if (envcmp("TP_STUB_FAIL_STAGE", "pattern")) {
        tpstub_event("gpu_pattern_fail");
        return -1;
    }
    tpstub_event("gpu_pattern_ok");
    return 0;
}

int tp_gpu_create_scanout(tp_gpu *g, const tp_config *c)
{
    if (envcmp("TP_STUB_FAIL_STAGE", "scanout")) {
        tpstub_event("gpu_scanout_fail");
        return -1;
    }
    g->nbuf = c->reuse ? TP_REUSE_SLOTS : c->frame_count;
    tpstub_event("gpu_scanout_ok");
    return 0;
}

int tp_gpu_render_frame(tp_gpu *g, const tp_config *c, int idx)
{
    return tp_gpu_render_frame_ex(g, c, idx, idx, 0);
}

int tp_gpu_render_frame_ex(tp_gpu *g, const tp_config *c, int slot,
                           int frame_seq, int need_acquire)
{
    (void)c; (void)frame_seq;
    if (slot == envi("TP_STUB_SUBMIT_LOST_IDX", -1)) {
        g->unsafe = 1; /* vkQueueSubmit DEVICE_LOST */
        tpstub_event("gpu_render_devicelost");
        return -1;
    }
    if (slot == envi("TP_STUB_RENDER_FAIL_IDX", -1)) {
        tpstub_event("gpu_render_fail");
        return -1;
    }
    tpstub_event(need_acquire ? "gpu_render_ok_acquire" : "gpu_render_ok");
    return 0;
}

int tp_gpu_wait_frame(tp_gpu *g, int idx, uint64_t timeout_ns)
{
    (void)idx; (void)timeout_ns;
    int r = envi("TP_STUB_WAIT_RESULT", 0);
    if (r != 0)
        g->unsafe = 1; /* record timeout/error as unsafe, like the real layer */
    tpstub_event(r == 0 ? "gpu_wait_ok" : (r == 1 ? "gpu_wait_timeout" : "gpu_wait_error"));
    return r;
}

int tp_gpu_timestamp_available(const tp_gpu *g)
{
    (void)g;
    return 0;   /* host stubs have no query pool */
}

int tp_gpu_timestamp_read(tp_gpu *g, int slot, uint64_t *out_ns)
{
    (void)g;
    (void)slot;
    (void)out_ns;
    return 1;   /* unavailable: main.c records no GPU time */
}

int tp_gpu_quiesce(tp_gpu *g, uint64_t timeout_ns)
{
    (void)timeout_ns;
    if (g->device == VK_NULL_HANDLE)
        return 0; /* null-device: nothing submitted */
    if (g->unsafe) {
        tpstub_event("gpu_quiesce_unsafe"); /* no second wait after an error */
        return -1;
    }
    tpstub_event("gpu_quiesce_ok");
    return 0;
}

void tp_gpu_destroy(tp_gpu *g, int safe)
{
    g_gpu_destroys++;
    g_destroy_safe = safe;
    if (g->device == VK_NULL_HANDLE)
        g_destroy_null = 1;
    tpstub_event(safe ? "gpu_destroy_safe" : "gpu_destroy_skipped");
}
