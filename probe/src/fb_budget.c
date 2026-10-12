/* SPDX-License-Identifier: GPL-2.0-or-later
 * See fb_budget.h. Pure logic; the caller supplies the monotonic instant, so
 * the policy is unit-testable with a fake clock and no device.
 */
#include "fb_budget.h"
#include "tp.h"

#include <math.h>

void tp_fb_budget_init(tp_fb_budget *b)
{
    if (!b)
        return;
    b->budget_ms = TP_FB_IOCTL_BUDGET_MS_DEFAULT;
    b->active = 0;
    b->t0_ms = 0.0;
}

int tp_fb_budget_set(tp_fb_budget *b, unsigned int budget_ms)
{
    if (!b)
        return -1;
    if (b->active) {
        tp_log("FB budget: refusing late set of %u ms after the first FB ioctl",
               budget_ms);
        return -1;
    }
    if (budget_ms < TP_FB_IOCTL_BUDGET_MS_DEFAULT) {
        tp_log("FB budget: refusing %u ms below the %u ms default",
               budget_ms, TP_FB_IOCTL_BUDGET_MS_DEFAULT);
        return -1;
    }
    if (budget_ms > TP_FB_IOCTL_BUDGET_MS_MAX) {
        tp_log("FB budget: refusing %u ms above the %u ms bound",
               budget_ms, TP_FB_IOCTL_BUDGET_MS_MAX);
        return -1;
    }
    b->budget_ms = budget_ms;
    tp_log("FB ioctl bookkeeping budget set to %u ms (bookkeeping bound only; "
           "not a kernel ioctl preemption guarantee)", budget_ms);
    return 0;
}

int tp_fb_budget_soak_ms(double duration_s, unsigned int *out_ms)
{
    if (!out_ms)
        return -1;
    if (!isfinite(duration_s) || duration_s < 0.0)
        return -1;
    double ms = duration_s * 1000.0 + (double)TP_FB_IOCTL_BUDGET_MS_DEFAULT;
    if (!isfinite(ms) || ms < (double)TP_FB_IOCTL_BUDGET_MS_DEFAULT ||
        ms > (double)TP_FB_IOCTL_BUDGET_MS_MAX)
        return -1;
    *out_ms = (unsigned int)ms;
    return 0;
}

int tp_fb_budget_check(tp_fb_budget *b, double now_ms)
{
    if (!b)
        return -1;
    if (!b->active) {
        b->t0_ms = now_ms;
        b->active = 1;
    }
    double el = now_ms - b->t0_ms;
    if (el >= (double)b->budget_ms) {
        tp_log("FB ioctl budget: %.1f ms already spent (>= %u ms); refusing "
               "further FB ioctls", el, b->budget_ms);
        return -1;
    }
    return 0;
}
