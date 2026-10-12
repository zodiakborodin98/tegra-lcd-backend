/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host tests for the extracted FB-ioctl bookkeeping budget (src/fb_budget.c).
 *
 * This is the pure policy: the monotonic instant is injected, so a fake clock
 * drives the whole lifecycle with no device and no real waiting. It asserts the
 * behaviour the M3l fix depends on:
 *   - the normal 60 s default still expires;
 *   - a --soak budget derived from the duration admits the real 120332 ms
 *     cleanup and still refuses beyond the derived bound;
 *   - the 1800 s soak bound is exactly the accepted maximum;
 *   - invalid / overflowing / late configuration fails closed, and the timer is
 *     never reset.
 */
#include "fb_budget.h"

#include <stdio.h>
#include <math.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* --------------------------------------------- derivation (soak budget) */

static void test_soak_derivation(void)
{
    unsigned int ms = 0;

    CHECK(tp_fb_budget_soak_ms(120.0, &ms) == 0 && ms == 180000u,
          "120 s soak => %u expected 180000", ms);
    CHECK(tp_fb_budget_soak_ms(1800.0, &ms) == 0 && ms == 1860000u,
          "1800 s soak => %u expected 1860000", ms);
    CHECK(tp_fb_budget_soak_ms(0.0, &ms) == 0 && ms == 60000u,
          "0 s soak => %u expected 60000", ms);

    /* One that would exceed the accepted ceiling must be refused. */
    CHECK(tp_fb_budget_soak_ms(1800.5, &ms) == -1, "1800.5 s soak accepted");
    CHECK(tp_fb_budget_soak_ms(1e9, &ms) == -1, "1e9 s soak accepted");

    /* Non-finite / negative / NULL are refused (fail closed). */
    CHECK(tp_fb_budget_soak_ms(NAN, &ms) == -1, "NaN duration accepted");
    CHECK(tp_fb_budget_soak_ms(INFINITY, &ms) == -1, "inf duration accepted");
    CHECK(tp_fb_budget_soak_ms(-1.0, &ms) == -1, "negative duration accepted");
    CHECK(tp_fb_budget_soak_ms(120.0, NULL) == -1, "NULL out accepted");
}

/* --------------------------------------------------- setter acceptance */

static void test_setter(void)
{
    tp_fb_budget b;

    tp_fb_budget_init(&b);
    CHECK(b.budget_ms == TP_FB_IOCTL_BUDGET_MS_DEFAULT && !b.active,
          "init not default/inactive (%u/%d)", b.budget_ms, b.active);

    /* Raise is accepted before activation. */
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, 180000u) == 0 && b.budget_ms == 180000u,
          "valid raise rejected");
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, TP_FB_IOCTL_BUDGET_MS_MAX) == 0,
          "max bound raise rejected");

    /* Equal to the default is harmless. */
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, TP_FB_IOCTL_BUDGET_MS_DEFAULT) == 0,
          "default raise rejected");

    /* Below default and above max are refused, leaving the budget unchanged. */
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, 59999u) == -1 && b.budget_ms == 60000u,
          "below-default set not refused (budget=%u)", b.budget_ms);
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, 0u) == -1, "zero set accepted");
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, 1860001u) == -1 && b.budget_ms == 60000u,
          "above-max set not refused (budget=%u)", b.budget_ms);
    tp_fb_budget_init(&b);
    CHECK(tp_fb_budget_set(&b, 0xFFFFFFFFu) == -1,
          "UINT_MAX set accepted (overflow)");
}

/* ------------------------------- normal default expiration (60 s) */

static void test_default_expiry(void)
{
    tp_fb_budget b;
    tp_fb_budget_init(&b);

    CHECK(tp_fb_budget_check(&b, 1000000.0) == 0, "first check not allowed");
    CHECK(b.active == 1 && b.t0_ms == 1000000.0, "first check did not start timer");

    /* Within budget: allowed. Interleaved checks must not reset the timer. */
    CHECK(tp_fb_budget_check(&b, 1050000.0) == 0, "30 s check refused");
    CHECK(tp_fb_budget_check(&b, 1059999.5) == 0, "just-under check refused");
    CHECK(tp_fb_budget_check(&b, 1000000.0) == 0,
          "earlier instant after activation refused (timer reset?)");

    /* At/after the boundary: refused, and it stays refused. */
    CHECK(tp_fb_budget_check(&b, 1060000.0) == -1, "60 s boundary not refused");
    CHECK(tp_fb_budget_check(&b, 1060000.0) == -1, "repeat at boundary allowed");
    CHECK(tp_fb_budget_check(&b, 1100000.0) == -1, "post-boundary allowed");
}

/* ---------------------------- 120 s soak: allow 120332 ms, refuse beyond */

static void test_soak_120(void)
{
    tp_fb_budget b;
    unsigned int ms = 0;
    tp_fb_budget_init(&b);

    CHECK(tp_fb_budget_soak_ms(120.0, &ms) == 0 && ms == 180000u,
          "soak 120 derive failed (%u)", ms);
    CHECK(tp_fb_budget_set(&b, ms) == 0, "soak 120 set rejected");

    CHECK(tp_fb_budget_check(&b, 500.0) == 0, "unblank-time check refused");
    /* The real run's cleanup instant (120332.6 ms after the unblank). */
    CHECK(tp_fb_budget_check(&b, 500.0 + 120332.6) == 0,
          "120332.6 ms cleanup refused at the 180 s soak budget");
    CHECK(tp_fb_budget_check(&b, 500.0 + 179999.0) == 0, "just-under refused");
    CHECK(tp_fb_budget_check(&b, 500.0 + 180000.0) == -1,
          "derived soak boundary not refused");
    CHECK(tp_fb_budget_check(&b, 500.0 + 120332.6 + 60000.0) == -1,
          "beyond derived budget allowed");
}

/* --------------------------------- 1800 s soak exactly at the bound */

static void test_soak_1800(void)
{
    tp_fb_budget b;
    unsigned int ms = 0;
    tp_fb_budget_init(&b);

    CHECK(tp_fb_budget_soak_ms(1800.0, &ms) == 0 && ms == 1860000u,
          "soak 1800 derive failed (%u)", ms);
    CHECK(tp_fb_budget_set(&b, ms) == 0, "soak 1800 set rejected");

    CHECK(tp_fb_budget_check(&b, 0.0) == 0, "soak 1800 first check refused");
    CHECK(tp_fb_budget_check(&b, 1859999.0) == 0, "just-under 1800 refused");
    CHECK(tp_fb_budget_check(&b, 1860000.0) == -1, "1800 boundary not refused");
}

/* -------------------------------------------- late setter fails closed */

static void test_late_set(void)
{
    tp_fb_budget b;
    tp_fb_budget_init(&b);

    CHECK(tp_fb_budget_check(&b, 2000.0) == 0, "activation check refused");
    CHECK(tp_fb_budget_set(&b, 180000u) == -1, "late set accepted");
    CHECK(b.budget_ms == TP_FB_IOCTL_BUDGET_MS_DEFAULT,
          "late set changed the budget (%u)", b.budget_ms);

    /* The budget is still the untouched default, and the original timer stands. */
    CHECK(tp_fb_budget_check(&b, 2000.0 + 59999.0) == 0, "default window lost");
    CHECK(tp_fb_budget_check(&b, 2000.0 + 60000.0) == -1, "default expiry lost");
}

int main(void)
{
    test_soak_derivation();
    test_setter();
    test_default_expiry();
    test_soak_120();
    test_soak_1800();
    test_late_set();
    printf("fb-budget tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
