/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "pacing.h"

/* No <math.h>: the bucket bounds are powers of two, kept as integer shifts so
 * this translation unit adds NO new shared-library dependency (the probe stays
 * on libvulkan/libc/ld only). */

/* ---------------- absolute-deadline pacing (pure) ---------------- */

void tp_pacing_init(tp_pacing *p, double t_start, double period, int frames)
{
    if (!p)
        return;
    p->t_start = t_start;
    p->period = (period > 0.0) ? period : 0.0;
    p->frames = frames;
    p->seq = 0;
    p->tol = TP_PACE_TOL_S;
    p->late_frames = 0;
    p->missed = 0;
    p->max_lateness = 0.0;
    p->sum_lateness = 0.0;
}

double tp_pacing_target(const tp_pacing *p, int seq)
{
    if (!p || seq < 0)
        return 0.0;
    return p->t_start + (double)seq * p->period;
}

double tp_pacing_observe(tp_pacing *p, int seq, double submit_time)
{
    if (!p)
        return 0.0;
    double target = tp_pacing_target(p, seq);
    double late = submit_time - target;
    if (!(late > 0.0))       /* NaN-safe: <= 0 (and NaN) is on time */
        late = 0.0;

    if (late > p->tol)
        p->late_frames++;
    if (late > p->period)
        p->missed++;
    if (late > p->max_lateness)
        p->max_lateness = late;
    if (late > 0.0)
        p->sum_lateness += late;
    if (seq >= p->seq)
        p->seq = seq + 1;
    return late;
}

/* ---------------- bounded timing stats ---------------- */

void tp_timing_reset(tp_timing *t)
{
    if (!t)
        return;
    t->count = 0;
    t->sum = 0.0;
    t->min = 0.0;
    t->max = 0.0;
    t->over = 0;
    for (int i = 0; i < TP_TIMING_BUCKETS; i++)
        t->hist[i] = 0;
}

int tp_timing_bucket_for(double ms)
{
    if (!(ms > 0.0))            /* 0 and NaN land in bucket 0 */
        return 0;
    /* bucket i covers [base*2^i, base*2^(i+1)); 2^(i+1) fits in unsigned. */
    for (int i = 0; i < TP_TIMING_BUCKETS; i++)
        if (ms < TP_TIMING_BASE_MS * (double)(1u << (i + 1)))
            return i;
    return -1;                  /* above the top bucket */
}

double tp_timing_bucket_hi_ms(int i)
{
    if (i < 0 || i >= TP_TIMING_BUCKETS)
        return 0.0;
    return TP_TIMING_BASE_MS * (double)(1u << (i + 1));
}

void tp_timing_add(tp_timing *t, double ms)
{
    if (!t)
        return;
    if (!(ms >= 0.0))           /* NaN -> 0 */
        ms = 0.0;
    if (t->count == 0 || ms < t->min)
        t->min = ms;
    if (t->count == 0 || ms > t->max)
        t->max = ms;
    t->sum += ms;
    t->count++;
    int b = tp_timing_bucket_for(ms);
    if (b < 0)
        t->over++;
    else
        t->hist[b]++;
}

double tp_timing_avg(const tp_timing *t)
{
    if (!t || t->count == 0)
        return 0.0;
    return t->sum / (double)t->count;
}

/* ---------------- GPU timestamp wrap delta (pure) ---------------- */

uint64_t tp_ts_masked_delta(uint64_t a, uint64_t b, uint32_t valid_bits)
{
    uint64_t mask = (valid_bits >= 64u) ? ~(uint64_t)0
                                        : ((1ULL << valid_bits) - 1ULL);
    return (b - a) & mask;   /* unsigned wrap through the valid-bit modulus */
}

/* ---------------- bounded integer min/max/final tracker ---------------- */

void tp_rangei_reset(tp_rangei *r)
{
    if (!r)
        return;
    r->have = 0;
    r->min = 0;
    r->max = 0;
    r->final = 0;
    r->n = 0;
}

void tp_rangei_add(tp_rangei *r, long v)
{
    if (!r)
        return;
    if (!r->have || v < r->min)
        r->min = v;
    if (!r->have || v > r->max)
        r->max = v;
    r->final = v;
    r->have = 1;
    r->n++;
}
