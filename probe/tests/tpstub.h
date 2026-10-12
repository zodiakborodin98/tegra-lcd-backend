/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host mock-control surface for the lifecycle tests. The real src/gpu.c and
 * src/dc.c are replaced by tests/stub_gpu.c and tests/stub_dc.c so main.c can
 * be exercised with no Vulkan/device. Controls are plain env vars; the stubs
 * record an ordered event log plus a few counters.
 */
#ifndef TPSTUB_H
#define TPSTUB_H

/* gpu-side: clears the event log and gpu counters (call before each case). */
void tpstub_reset(void);
/* dc-side: clears the dc flip counter (call before each case). */
void tpstub_dc_reset(void);

/* Ordering/observation helpers. */
const char *tpstub_events(void);
int tpstub_dc_closes(void);
int tpstub_gpu_destroys(void);
int tpstub_destroy_safe(void);   /* value main passed to tp_gpu_destroy(safe) */
int tpstub_destroy_null(void);   /* 1 if destroy saw a null device */
int tpstub_flips(void);

/* Called by the dc stub to append events into the shared gpu-stub log. */
void tpstub_event(const char *s);
void tpstub_dc_close(void);
void tpstub_flip(void);
void tpstub_unblank(void);
void tpstub_restore(void);
void tpstub_window_acquire(void);
void tpstub_release(const char *verdict);
int  tpstub_unblanks(void);
int  tpstub_restores(void);
int  tpstub_window_acquires(void);
int  tpstub_releases(void);
unsigned tpstub_fb_budget_ms(void);  /* last value passed to tp_fb_set_budget_ms */
int  tpstub_fb_budget_calls(void);   /* number of successful budget sets       */

#endif /* TPSTUB_H */
