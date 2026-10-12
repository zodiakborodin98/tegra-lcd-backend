/* SPDX-License-Identifier: GPL-2.0-or-later
 * tegra-present-probe -- M3i: absolute-deadline frame pacing and bounded,
 * honest timing statistics (pure host-testable maths, no syscalls here).
 *
 * The pacing model is deliberately absolute: frame `seq` (0-based) targets the
 * monotonic instant `t_start + seq * period`. Because every target is derived
 * from the single run origin, an overrun on one frame can never accumulate into
 * drift; it only shows up as that frame's lateness. There is no per-frame
 * relative sleep and no unconditional sleep after work.
 */
#ifndef TP_PACING_H
#define TP_PACING_H

#include <stdint.h>

/* Lateness at or below this is treated as "on time" (scheduler jitter). */
#define TP_PACE_TOL_S        0.001

typedef struct {
    double   t_start;       /* monotonic seconds of frame 0's target          */
    double   period;        /* nominal seconds between frame targets          */
    int      frames;        /* total frames in the run                        */
    int      seq;           /* next frame index to observe                    */
    double   tol;           /* on-time tolerance (s)                          */
    unsigned late_frames;   /* submits later than target by > tol            */
    unsigned missed;        /* submits later than target + period            */
    double   max_lateness;  /* max(submit - target), clamped >= 0 (s)         */
    double   sum_lateness;  /* sum of positive lateness (s)                   */
} tp_pacing;

void   tp_pacing_init(tp_pacing *p, double t_start, double period, int frames);
/* Absolute target instant for frame `seq` (t_start + seq*period). */
double tp_pacing_target(const tp_pacing *p, int seq);
/* Fold a frame's FLIP4 submit time into the counters; returns lateness (>=0). */
double tp_pacing_observe(tp_pacing *p, int seq, double submit_time);

/* ------------------- bounded per-operation timing stats ------------------- */
/* Fixed log2-spaced milliseconds histogram; bounded memory, no samples kept.
 * Bucket i covers [base*2^i, base*2^(i+1)) ms; samples at/above the top bucket
 * are counted in `over`. This is a lossy summary by design (honest buckets,
 * not claimed percentiles). */
#define TP_TIMING_BUCKETS 24
#define TP_TIMING_BASE_MS 0.0625   /* bucket 0 => [0, 0.125) ms */

typedef struct {
    unsigned count;
    double   sum;
    double   min;
    double   max;
    unsigned hist[TP_TIMING_BUCKETS];
    unsigned over;
} tp_timing;

void   tp_timing_reset(tp_timing *t);
void   tp_timing_add(tp_timing *t, double ms);
double tp_timing_avg(const tp_timing *t);
/* bucket index for a sample, or -1 when it overflows the table. */
int    tp_timing_bucket_for(double ms);
/* upper bound (ms) of bucket i (exclusive); used for reporting. */
double tp_timing_bucket_hi_ms(int i);

/* ------------------- GPU timestamp wrap delta (pure) ---------------------- */
/* Unsigned difference (b - a) reduced modulo 2^valid_bits: correct for every
 * counter width, including the full 64-bit counter (no special-case that would
 * report a fake 0). a and b are expected to be already masked to valid_bits. */
uint64_t tp_ts_masked_delta(uint64_t a, uint64_t b, uint32_t valid_bits);

/* ------------------- bounded integer min/max/final tracker ----------------- */
typedef struct {
    int      have;
    long     min;
    long     max;
    long     final;
    unsigned n;
} tp_rangei;

void tp_rangei_reset(tp_rangei *r);
void tp_rangei_add(tp_rangei *r, long v);

#endif /* TP_PACING_H */
