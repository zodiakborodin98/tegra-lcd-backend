/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host tests for the M3i pacing/timing maths and the /proc resource samplers.
 * Pure functions plus the real (Linux) /proc readers; no device, no Vulkan.
 * These assert behaviour of the *maths*: absolute deadlines, no drift, overrun
 * accounting, bounded timing buckets, and fd/RSS parsing.
 */
#include "pacing.h"
#include "procstat.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

static int g_pass, g_fail;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ---------------------------------------------------- pacing: basics */

static void test_pacing_basics(void)
{
    tp_pacing p;
    tp_pacing_init(&p, 100.0, 0.02, 50);
    CHECK(p.t_start == 100.0 && fabs(p.period - 0.02) < 1e-12 &&
          p.frames == 50 && p.seq == 0,
          "init fields wrong (t0=%.3f p=%.6f n=%d)", p.t_start, p.period, p.frames);
    CHECK(p.late_frames == 0 && p.missed == 0 && p.max_lateness == 0.0,
          "fresh counters not zero");

    /* Targets are absolute: t_start + seq*period, first frame immediate. */
    CHECK(fabs(tp_pacing_target(&p, 0) - 100.0) < 1e-12, "target(0) != t_start");
    CHECK(fabs(tp_pacing_target(&p, 3) - 100.06) < 1e-9,
          "target(3)=%.6f expected 100.06", tp_pacing_target(&p, 3));
    CHECK(tp_pacing_target(&p, -1) == 0.0, "negative seq target not 0");

    /* A negative period is clamped to 0 (never a negative deadline). */
    tp_pacing_init(&p, 5.0, -1.0, 4);
    CHECK(p.period == 0.0, "negative period not clamped (%f)", p.period);
}

/* ---------------------------------------------------- pacing: no drift */

/* Work faster than the period: every frame is emitted exactly on its absolute
 * target, so lateness never accumulates. */
static void test_pacing_no_drift(void)
{
    tp_pacing p;
    const double t0 = 10.0, period = 1.0 / 60.0, work = 0.004;
    const int n = 600;
    tp_pacing_init(&p, t0, period, n);

    double submit = t0;
    double last_target = t0;
    int worst_after_first = -1;
    for (int i = 0; i < n; i++) {
        double target = tp_pacing_target(&p, i);
        double ready = (i == 0) ? (t0 + work) : (submit + work);
        submit = (ready > target) ? ready : target;      /* wait on the deadline */
        double late = tp_pacing_observe(&p, i, submit);
        if (i == 0) {
            /* frame 0 is immediate but still pays its own work duration */
            CHECK(fabs(late - work) < 1e-12,
                  "frame 0 lateness=%.6f expected work %.6f", late, work);
        } else if (late > 1e-12) {
            worst_after_first = i;
        }
        last_target = target;
    }
    CHECK(worst_after_first == -1, "on-time run reported late frame %d",
          worst_after_first);
    CHECK(p.late_frames == 1 && p.missed == 0,
          "on-time run counted late=%u missed=%u (expected 1 and 0)",
          p.late_frames, p.missed);
    CHECK(fabs(p.max_lateness - work) < 1e-12,
          "on-time max_lateness=%.6f expected just frame 0's work %.6f",
          p.max_lateness, work);
    CHECK(fabs(last_target - (t0 + (n - 1) * period)) < 1e-9,
          "last target drifted: %.9f", last_target);
    CHECK(fabs(submit - last_target) < 1e-12,
          "final submit %.9f is not the last target %.9f", submit, last_target);
}

/* Work slower than the period: lateness grows by (work-period) every frame, but
 * the TARGETS stay put (absolute schedule) and every frame is counted late and
 * missed. Demonstrates overrun accounting without invented positions. */
static void test_pacing_overrun(void)
{
    tp_pacing p;
    const double t0 = 0.0, period = 0.02, work = 0.06; /* 3x overrun */
    const int n = 10;
    tp_pacing_init(&p, t0, period, n);

    double submit = t0;
    for (int i = 0; i < n; i++) {
        double target = tp_pacing_target(&p, i);
        double ready = (i == 0) ? (t0 + work) : (submit + work);
        submit = (ready > target) ? ready : target;
        tp_pacing_observe(&p, i, submit);
        /* The schedule is immutable regardless of overrun. */
        CHECK(fabs(tp_pacing_target(&p, i) - (t0 + i * period)) < 1e-12,
              "target(%d) moved under overrun", i);
    }
    CHECK(p.late_frames == (unsigned)n, "overrun late_frames=%u expected %d",
          p.late_frames, n);
    CHECK(p.missed == (unsigned)n, "overrun missed=%u expected %d", p.missed, n);
    /* submit_i = (i+1)*work ; late_i = (i+1)*work - i*period */
    double last_late = (double)n * work - (double)(n - 1) * period;
    CHECK(fabs(p.max_lateness - last_late) < 1e-9,
          "max_lateness=%.6f expected %.6f", p.max_lateness, last_late);
}

/* Tolerance / clamp / NaN behaviour of observe(). */
static void test_pacing_observe_edges(void)
{
    tp_pacing p;
    tp_pacing_init(&p, 100.0, 0.016, 3);
    CHECK(p.tol > 0.0, "tolerance not positive");

    /* within tolerance => on time (raw lateness is still returned) */
    double late = tp_pacing_observe(&p, 0, 100.0 + p.tol * 0.5);
    CHECK(late > 0.0 && late <= p.tol && p.late_frames == 0,
          "sub-tolerance lateness counted (late=%g n=%u)", late, p.late_frames);

    /* above tolerance but under a period => late, not missed */
    late = tp_pacing_observe(&p, 1, 100.0 + 0.016 + 0.005);
    CHECK(late > 0.0 && p.late_frames == 1 && p.missed == 0,
          "late-not-missed misclassified (late=%g late_n=%u miss=%u)",
          late, p.late_frames, p.missed);

    /* beyond a whole period => missed */
    late = tp_pacing_observe(&p, 2, 100.0 + 2 * 0.016 + 0.05);
    CHECK(p.missed == 1, "missed not counted (%u)", p.missed);

    /* early submit clamps lateness to 0, never negative */
    tp_pacing q;
    tp_pacing_init(&q, 10.0, 0.01, 1);
    CHECK(tp_pacing_observe(&q, 0, 9.0) == 0.0 && q.late_frames == 0,
          "early submit produced lateness/among late");
    CHECK(q.seq == 1, "seq not advanced (seq=%d)", q.seq);
}

/* ---------------------------------------------------- timing stats */

static void test_timing(void)
{
    tp_timing t;
    tp_timing_reset(&t);
    CHECK(t.count == 0 && tp_timing_avg(&t) == 0.0, "reset not clean");

    double samples[] = { 1.0, 2.0, 4.0, 8.0, 0.05, 0.0 };
    int n = (int)(sizeof(samples) / sizeof(samples[0]));
    for (int i = 0; i < n; i++)
        tp_timing_add(&t, samples[i]);

    CHECK(t.count == (unsigned)n, "count=%u expected %d", t.count, n);
    CHECK(fabs(t.min - 0.0) < 1e-12, "min=%g expected 0", t.min);
    CHECK(fabs(t.max - 8.0) < 1e-12, "max=%g expected 8", t.max);
    CHECK(fabs(tp_timing_avg(&t) - (15.05 / 6.0)) < 1e-9,
          "avg=%.6f expected %.6f", tp_timing_avg(&t), 15.05 / 6.0);
    CHECK(t.over == 0, "over=%u expected 0", t.over);

    /* bucket maths */
    CHECK(tp_timing_bucket_for(0.0) == 0, "bucket(0) != 0");
    CHECK(tp_timing_bucket_for(0.0625) == 0, "bucket(base) != 0");
    CHECK(tp_timing_bucket_for(1e9) == -1, "huge sample not over");
    CHECK(fabs(tp_timing_bucket_hi_ms(0) - 0.125) < 1e-12,
          "bucket_hi(0)=%g", tp_timing_bucket_hi_ms(0));

    tp_timing o;
    tp_timing_reset(&o);
    tp_timing_add(&o, 1e9);          /* overflows the table */
    CHECK(o.over == 1 && o.count == 1, "overflow not tracked (over=%u)", o.over);

    /* NaN sample is coerced to 0, never poisons min/max */
    tp_timing z;
    tp_timing_reset(&z);
    tp_timing_add(&z, NAN);
    CHECK(z.count == 1 && z.min == 0.0 && z.max == 0.0,
          "NaN sample not coerced (min=%g max=%g)", z.min, z.max);
}

/* ---------------------------------------------------- ts wrap delta */

/* GPU timestamp wrap: unsigned difference masked to the valid bits. */
static void test_ts_wrap(void)
{
    /* Full 64-bit counter: a genuine wrap must give the span, not a fake 0. */
    uint64_t span = tp_ts_masked_delta(~(uint64_t)0 - 3u, 5u, 64u);
    CHECK(span == 9u, "64-bit wrap delta=%llu expected 9",
          (unsigned long long)span);
    /* 32-bit valid bits: b below a wraps through the 32-bit modulus. */
    uint64_t m32 = (1ULL << 32) - 1ULL;
    CHECK(tp_ts_masked_delta(m32 - 4u, 6u, 32u) == 11u, "32-bit wrap delta wrong");
    /* Monotonic/normal cases: straight subtraction, unaffected by masking. */
    CHECK(tp_ts_masked_delta(100u, 250u, 64u) == 150u, "monotonic delta wrong");
    CHECK(tp_ts_masked_delta(0u, 0u, 64u) == 0u, "zero delta wrong");
    CHECK(tp_ts_masked_delta(1000u, 4000u, 32u) == 3000u, "32-bit monotonic wrong");
}

/* ---------------------------------------------------- range tracker */

static void test_rangei(void)
{
    tp_rangei r;
    tp_rangei_reset(&r);
    CHECK(!r.have && r.n == 0, "range reset not clean");
    tp_rangei_add(&r, 5);
    CHECK(r.min == 5 && r.max == 5 && r.final == 5 && r.n == 1,
          "single add wrong");
    tp_rangei_add(&r, 3);
    tp_rangei_add(&r, 9);
    tp_rangei_add(&r, 4);
    CHECK(r.min == 3 && r.max == 9 && r.final == 4 && r.n == 4,
          "range min=%ld max=%ld final=%ld n=%u", r.min, r.max, r.final, r.n);
}

/* ---------------------------------------------------- /proc samplers */

static void test_procstat_parse(void)
{
    long kb = -1;
    CHECK(tp_parse_vmrss_kb("VmRSS:\t    12345 kB\n", &kb) == 0 && kb == 12345,
          "VmRSS parse failed (kb=%ld)", kb);
    CHECK(tp_parse_vmrss_kb("VmSize:  9 kB\n", &kb) == -1, "wrong field accepted");
    CHECK(tp_parse_vmrss_kb("VmRSS: notanumber\n", &kb) == -1,
          "garbage value accepted");
    CHECK(tp_parse_vmrss_kb(NULL, &kb) == -1, "NULL accepted");
    CHECK(tp_parse_vmrss_kb("VmRSS:\n", &kb) == -1, "empty value accepted");
}

static void test_procstat_real(void)
{
    int fd0 = tp_proc_fd_count();
    CHECK(fd0 >= 1, "fd count implausible (%d)", fd0);

    int f = open("/dev/null", O_RDONLY);
    CHECK(f >= 0, "open /dev/null failed");
    int fd1 = tp_proc_fd_count();
    CHECK(fd1 == fd0 + 1, "fd count did not rise by 1 (before=%d after=%d)",
          fd0, fd1);
    if (f >= 0)
        close(f);
    int fd2 = tp_proc_fd_count();
    CHECK(fd2 == fd0, "fd count did not fall back (before=%d after=%d)", fd0, fd2);

    int rss = tp_proc_rss_kb();
    CHECK(rss > 0, "rss implausible (%d kB)", rss);
}

int main(void)
{
    test_pacing_basics();
    test_pacing_no_drift();
    test_pacing_overrun();
    test_pacing_observe_edges();
    test_timing();
    test_ts_wrap();
    test_rangei();
    test_procstat_parse();
    test_procstat_real();
    printf("m3i tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
