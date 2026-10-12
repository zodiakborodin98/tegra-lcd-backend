/* SPDX-License-Identifier: GPL-2.0-or-later
 * Bounded whole-process fbdev-ioctl bookkeeping budget (extracted from dc.c so
 * the policy is testable with an injected monotonic clock).
 *
 * The budget is a *bookkeeping* bound only, NOT a kernel ioctl preemption
 * guarantee: a kernel ioctl can block uninterruptibly and we cannot force it to
 * return. We simply stop issuing further FB ioctls once our own measured
 * CLOCK_MONOTONIC time since the first FB ioctl reaches the budget.
 *
 * The budget is process-global and one-shot: the timer starts on the first
 * check and is NEVER reset, and the budget may only be raised BEFORE that first
 * check (i.e. before the first FB ioctl). The exhaustion check is never removed.
 */
#ifndef TP_FB_BUDGET_H
#define TP_FB_BUDGET_H

/* Normal default: 60 s of measured whole-process FB ioctl time. */
#define TP_FB_IOCTL_BUDGET_MS_DEFAULT 60000u
/* Hard ceiling accepted by the setter: 1800 s soak + 60 s setup/teardown. */
#define TP_FB_IOCTL_BUDGET_MS_MAX     1860000u

typedef struct {
    unsigned int budget_ms;  /* active budget (default until explicitly raised) */
    int          active;     /* set once the timer has started                  */
    double       t0_ms;      /* first-check monotonic instant (ms)              */
} tp_fb_budget;

/* Initialise to the normal default, inactive. */
void tp_fb_budget_init(tp_fb_budget *b);

/* Raise the budget. Allowed ONLY before the first check (inactive). Rejects
 * (returns -1, no state change) any value below the default or above the max,
 * and any attempt after activation. Returns 0 on success. */
int tp_fb_budget_set(tp_fb_budget *b, unsigned int budget_ms);

/* Derive a --soak budget in ms from a validated duration: duration_s * 1000
 * plus a finite setup/teardown allowance (TP_FB_IOCTL_BUDGET_MS_DEFAULT).
 * Returns 0 and *out_ms on success; -1 (no state touched) for a NULL out, a
 * non-finite or negative duration, or a result above the max. */
int tp_fb_budget_soak_ms(double duration_s, unsigned int *out_ms);

/* Check the budget at a supplied monotonic instant (ms). Starts the timer on
 * the first call. Returns 0 while within budget, -1 once exhausted (logging the
 * refusal). The timer is never reset and the check is never removed. */
int tp_fb_budget_check(tp_fb_budget *b, double now_ms);

#endif /* TP_FB_BUDGET_H */
