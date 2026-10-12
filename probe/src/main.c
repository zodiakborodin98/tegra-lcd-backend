/* SPDX-License-Identifier: GPL-2.0-or-later
 * tegra-present-probe -- standalone first-frame/fence discovery probe.
 *
 * Default invocation is a dry run (help, no display access). Real display
 * ioctls require an explicit --present. --diagnostic-only never takes a
 * window and never flips.
 */
#define _POSIX_C_SOURCE 200809L
#include "tp.h"
#include "dc.h"
#include "fb_budget.h"
#include "gpu.h"
#include "reuse.h"
#include "pacing.h"
#include "procstat.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

/* ------------------------------------------------------------------ config */

void tp_config_defaults(tp_config *c)
{
    memset(c, 0, sizeof(*c));
    c->present = 0;
    c->diagnostic_only = 0;
    c->enable_head = 0;
    c->verbose = 0;
    c->dc = -1;
    c->window = 0;
    c->window_explicit = 0;
    c->logical_w = 1280;
    c->logical_h = 720;
    c->refresh_hz = 60;
    c->physical_w = 0;
    c->physical_h = 0;
    c->orientation = TP_ORIENT_90;
    c->block_height_log2 = 4;
    c->frame_count = 4;
    c->duration_s = 5.0;
    c->reuse = 0;
    c->soak = 0;
    c->frame_count_explicit = 0;
    c->duration_explicit = 0;
}

void tp_print_help(const char *argv0)
{
    fprintf(stderr,
        "tegra-present-probe -- first-frame/fence discovery for direct Tegra DC present\n"
        "\n"
        "Usage: %s [options]\n"
        "\n"
        "  --present              actually present (GET_WINDOW + FLIP4). Without it the\n"
        "                         tool only prints this help / a dry-run report.\n"
        "  --diagnostic-only      read-only capability report; never GET_WINDOW/FLIP4\n"
        "  --enable-head          with --present --dc 0 --window 0 only: if DC0 is\n"
        "                         disabled, enable the existing head via FBIOBLANK\n"
        "                         (UNBLANK) on /dev/fb0 (no modeset, no pixels) and\n"
        "                         restore the prior blank state on exit. Never used by\n"
        "                         the default dry run or --diagnostic-only.\n"
        "  --dc N                 DC head to use (default: autodetect active head)\n"
        "  --window N             window index to claim (default: 0)\n"
        "  --mode WxH[@Hz]        logical render size + refresh (default 1280x720@60)\n"
        "  --physical WxH         physical panel size (default: autodetect /sys fb0)\n"
        "  --orientation D        0|90|180|270 clockwise (default 90 for portrait panel)\n"
        "  --block-height-log2 N  GOB block height log2 (default 4)\n"
        "  --frame-count N        frames to present: default mode 1..%d (default 4);\n"
        "                         with --reuse %d..%d (default %d); with --soak\n"
        "                         %d..%d (default %d)\n"
        "  --duration SEC         total budget seconds, <=30 (default 5; with\n"
        "                         --reuse default %g; with --soak <=%g, default %g)\n"
        "  --reuse                opt-in bounded 3-slot scanout reuse: EXACTLY 3\n"
        "                         persistent buffers in a round-robin ring, each\n"
        "                         recycled only after its own FLIP4 post fence\n"
        "                         reports a validated kernel retirement. Requires\n"
        "                         --present. Preserves the write-once default.\n"
        "  --soak                 EXPLICIT long-run soak: requires --present and\n"
        "                         --reuse. Raises the reuse ceiling to %d frames /\n"
        "                         %g s (60 fps) with defaults %d / %g. Still exactly\n"
        "                         3 persistent buffers, strict own-fence retirement\n"
        "                         and the duration+10 s hard wall. No --verbose.\n"
        "  --verbose              verbose per-flip/fence logging\n"
        "  --help                 show this help\n"
        "\n"
        "Default mode never rewrites/recycles a buffer after FLIP4.\n",
        argv0, TP_MAX_SCANOUT_BUFFERS, TP_REUSE_MIN_FRAMES, TP_REUSE_MAX_FRAMES,
        TP_REUSE_DEFAULT_FRAMES, TP_REUSE_MIN_FRAMES, TP_SOAK_MAX_FRAMES,
        TP_SOAK_DEFAULT_FRAMES, TP_REUSE_DEFAULT_DURATION, TP_SOAK_MAX_DURATION,
        TP_SOAK_DEFAULT_DURATION, TP_SOAK_MAX_FRAMES, TP_SOAK_MAX_DURATION,
        TP_SOAK_DEFAULT_FRAMES, TP_SOAK_DEFAULT_DURATION);
}

static int parse_orient(const char *s, tp_orient *o)
{
    if (!strcmp(s, "0"))   { *o = TP_ORIENT_0;   return 0; }
    if (!strcmp(s, "90"))  { *o = TP_ORIENT_90;  return 0; }
    if (!strcmp(s, "180")) { *o = TP_ORIENT_180; return 0; }
    if (!strcmp(s, "270")) { *o = TP_ORIENT_270; return 0; }
    return -1;
}

/* Strict integer parse: full-string, no junk, no overflow past int. */
static int parse_int_strict(const char *s, long *out)
{
    if (!s || !*s)
        return -1;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < INT_MIN || v > INT_MAX)
        return -1;
    *out = v;
    return 0;
}

/* Strict finite double parse: full-string, no junk, no overflow. */
static int parse_double_strict(const char *s, double *out)
{
    if (!s || !*s)
        return -1;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (errno != 0 || end == s || *end != '\0' || !isfinite(v))
        return -1;
    *out = v;
    return 0;
}

/* "WxH[@Hz]": no trailing junk, no integer overflow. */
static int parse_mode(const char *s, int *w, int *h, int *hz)
{
    if (!s)
        return -1;
    const char *p = s;
    char *end = NULL;
    errno = 0;
    long W = strtol(p, &end, 10);
    if (errno != 0 || end == p || *end != 'x')
        return -1;
    p = end + 1;
    errno = 0;
    long H = strtol(p, &end, 10);
    if (errno != 0 || end == p)
        return -1;
    p = end;
    long Hz = 0;
    if (*p == '@') {
        p++;
        errno = 0;
        Hz = strtol(p, &end, 10);
        if (errno != 0 || end == p)
            return -1;
        p = end;
    }
    if (*p != '\0')
        return -1;
    if (W < INT_MIN || W > INT_MAX || H < INT_MIN || H > INT_MAX ||
        Hz < 0 || Hz > INT_MAX)
        return -1;
    *w = (int)W; *h = (int)H; *hz = (int)Hz;
    return 0;
}

/* "WxH": no trailing junk, no integer overflow. */
static int parse_size(const char *s, int *w, int *h)
{
    if (!s)
        return -1;
    const char *p = s;
    char *end = NULL;
    errno = 0;
    long W = strtol(p, &end, 10);
    if (errno != 0 || end == p || *end != 'x')
        return -1;
    p = end + 1;
    errno = 0;
    long H = strtol(p, &end, 10);
    if (errno != 0 || end == p || *end != '\0')
        return -1;
    if (W < INT_MIN || W > INT_MAX || H < INT_MIN || H > INT_MAX)
        return -1;
    *w = (int)W; *h = (int)H;
    return 0;
}

int tp_config_parse(tp_config *c, int argc, char **argv)
{
    if (argc < 2)
        return 1; /* default = help */

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        long n = 0;

        if (!strcmp(a, "--help") || !strcmp(a, "-h"))
            return 1;
        else if (!strcmp(a, "--present"))
            c->present = 1;
        else if (!strcmp(a, "--diagnostic-only"))
            c->diagnostic_only = 1;
        else if (!strcmp(a, "--enable-head"))
            c->enable_head = 1;
        else if (!strcmp(a, "--reuse"))
            c->reuse = 1;
        else if (!strcmp(a, "--soak"))
            c->soak = 1;
        else if (!strcmp(a, "--verbose"))
            c->verbose = 1;
        else if (!strcmp(a, "--dc") && v) {
            if (parse_int_strict(v, &n) != 0) { tp_log("bad --dc '%s'", v); return -1; }
            c->dc = (int)n; i++;
        } else if (!strcmp(a, "--window") && v) {
            if (parse_int_strict(v, &n) != 0) { tp_log("bad --window '%s'", v); return -1; }
            c->window = (int)n; c->window_explicit = 1; i++;
        } else if (!strcmp(a, "--mode") && v) {
            int w = 0, h = 0, hz = 0;
            if (parse_mode(v, &w, &h, &hz) != 0) {
                tp_log("bad --mode '%s' (want WxH[@Hz])", v);
                return -1;
            }
            c->logical_w = w; c->logical_h = h;
            if (hz > 0) c->refresh_hz = hz;
            i++;
        } else if (!strcmp(a, "--physical") && v) {
            int w = 0, h = 0;
            if (parse_size(v, &w, &h) != 0) {
                tp_log("bad --physical '%s' (want WxH)", v);
                return -1;
            }
            c->physical_w = w; c->physical_h = h; i++;
        } else if (!strcmp(a, "--orientation") && v) {
            if (parse_orient(v, &c->orientation) != 0) {
                tp_log("bad --orientation '%s'", v);
                return -1;
            }
            i++;
        } else if (!strcmp(a, "--block-height-log2") && v) {
            if (parse_int_strict(v, &n) != 0) { tp_log("bad --block-height-log2 '%s'", v); return -1; }
            c->block_height_log2 = (int)n; i++;
        } else if (!strcmp(a, "--frame-count") && v) {
            if (parse_int_strict(v, &n) != 0) { tp_log("bad --frame-count '%s'", v); return -1; }
            c->frame_count = (int)n; c->frame_count_explicit = 1; i++;
        } else if (!strcmp(a, "--duration") && v) {
            double d = 0.0;
            if (parse_double_strict(v, &d) != 0) { tp_log("bad --duration '%s'", v); return -1; }
            c->duration_s = d; c->duration_explicit = 1; i++;
        } else {
            tp_log("unknown argument '%s'", a);
            return -1;
        }
    }

    /* ---- strict pre-device validation ---- */
    if (c->logical_w < 1 || c->logical_w > 4096 ||
        c->logical_h < 1 || c->logical_h > 4096) {
        tp_log("logical %dx%d out of range 1..4096", c->logical_w, c->logical_h);
        return -1;
    }
    if (c->physical_w != 0 || c->physical_h != 0) {
        if (c->physical_w < 1 || c->physical_w > 4096 ||
            c->physical_h < 1 || c->physical_h > 4096) {
            tp_log("physical %dx%d out of range 1..4096",
                   c->physical_w, c->physical_h);
            return -1;
        }
        int ew = 0, eh = 0;
        tp_orient_physical_size(c->orientation, c->logical_w, c->logical_h,
                                &ew, &eh);
        if (c->physical_w != ew || c->physical_h != eh) {
            tp_log("physical %dx%d inconsistent with logical %dx%d + orientation %d "
                   "(expected %dx%d); refusing",
                   c->physical_w, c->physical_h, c->logical_w, c->logical_h,
                   (int)c->orientation, ew, eh);
            return -1;
        }
    }
    if (c->dc < -1 || c->dc > 3) {
        tp_log("--dc %d out of range 0..3 (-1 = autodetect)", c->dc);
        return -1;
    }
    if (c->window < 0 || c->window > 5) {
        tp_log("--window %d out of range 0..5", c->window);
        return -1;
    }
    double dur_max = c->soak ? TP_SOAK_MAX_DURATION : 30.0;
    if (!isfinite(c->duration_s) || c->duration_s < 0.1 || c->duration_s > dur_max) {
        tp_log("--duration out of range 0.1..%g (got %g)", dur_max, c->duration_s);
        return -1;
    }
    if (c->block_height_log2 < 1 || c->block_height_log2 > 8) {
        tp_log("block-height-log2 out of range 1..8");
        return -1;
    }
    if (c->reuse) {
        /* Reuse is a bounded hypothesis test with a hard frame-count range so
         * the run cannot become an unbounded soak. --soak raises the ceiling
         * explicitly; both are rejected, never clamped. */
        int fc_max = c->soak ? TP_SOAK_MAX_FRAMES : TP_REUSE_MAX_FRAMES;
        if (c->frame_count < TP_REUSE_MIN_FRAMES ||
            c->frame_count > fc_max) {
            tp_log("%s requires --frame-count %d..%d (got %d)",
                   c->soak ? "--soak" : "--reuse",
                   TP_REUSE_MIN_FRAMES, fc_max, c->frame_count);
            return -1;
        }
    } else {
        if (c->frame_count < 1) {
            tp_log("--frame-count must be >= 1");
            return -1;
        }
        if (c->frame_count > TP_MAX_SCANOUT_BUFFERS) {
            tp_log("frame-count clamped to %d (no buffer reuse in this milestone)",
                   TP_MAX_SCANOUT_BUFFERS);
            c->frame_count = TP_MAX_SCANOUT_BUFFERS;
        }
    }
    /* --enable-head is deliberately a bounded prototype: DC0/fb0 only. */
    if (c->enable_head) {
        if (c->diagnostic_only) {
            tp_log("--enable-head is invalid with --diagnostic-only");
            return -1;
        }
        if (!c->present) {
            tp_log("--enable-head requires --present");
            return -1;
        }
        if (c->dc != 0) {
            tp_log("--enable-head is a bounded prototype: requires --dc 0 (fb0)");
            return -1;
        }
        if (c->window != 0) {
            tp_log("--enable-head requires --window 0 (fb0)");
            return -1;
        }
    }
    /* --soak is an explicit, opt-in long-run ceiling raise. It never changes
     * the write-once/reuse mechanics; it only lifts the frame/duration caps and
     * supplies long-run defaults. Refuse anything ambiguous. */
    if (c->soak) {
        if (c->diagnostic_only) {
            tp_log("--soak is invalid with --diagnostic-only");
            return -1;
        }
        if (!c->present) {
            tp_log("--soak requires --present");
            return -1;
        }
        if (!c->reuse) {
            tp_log("--soak requires --reuse (3 persistent buffers only)");
            return -1;
        }
        if (c->verbose) {
            tp_log("--soak is not compatible with --verbose (bounded 1 Hz stats only)");
            return -1;
        }
    }
    /* --reuse is an explicit opt-in that only makes sense while presenting. */
    if (c->reuse) {
        if (c->diagnostic_only) {
            tp_log("--reuse is invalid with --diagnostic-only");
            return -1;
        }
        if (!c->present) {
            tp_log("--reuse requires --present");
            return -1;
        }
        if (!c->frame_count_explicit)
            c->frame_count = c->soak ? TP_SOAK_DEFAULT_FRAMES
                                     : TP_REUSE_DEFAULT_FRAMES;
        if (!c->duration_explicit)
            c->duration_s = c->soak ? TP_SOAK_DEFAULT_DURATION
                                    : TP_REUSE_DEFAULT_DURATION;
    }
    return 0;
}

double tp_frame_period(const tp_config *c)
{
    int n = c->frame_count;
    if (n < 1)
        n = 1;
    double p = c->duration_s / (double)n;
    return (p < 0.0) ? 0.0 : p;
}

/* ------------------------------------------------------------------- misc */

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Bounded absolute-deadline wait: sleeps the remaining monotonic delta to
 * deadline_s in one nanosleep (re-derived after every EINTR), returning 0 when
 * the deadline is reached, -1 when the stop flag is set, -2 on a sleep error.
 * deadline_s is an absolute CLOCK_MONOTONIC instant (same clock as now_s()).
 * No busy wait; no unconditional sleep after work. */
static int tp_sleep_until(double deadline_s)
{
    for (;;) {
        if (g_stop)
            return -1;
        double d = deadline_s - now_s();
        if (d <= 0.0)
            return 0;
        struct timespec ts;
        ts.tv_sec = (time_t)d;
        ts.tv_nsec = (long)((d - (double)ts.tv_sec) * 1e9);
        if (ts.tv_nsec < 0)
            ts.tv_nsec = 0;
        if (ts.tv_nsec > 999999999L)
            ts.tv_nsec = 999999999L;
        if (nanosleep(&ts, NULL) == 0)
            return 0;
        if (errno != EINTR)
            return -2;
    }
}

/* Verbose-only bounded timing summary: fixed log2-ms buckets over bounded
 * memory; these are bins, not claimed percentiles. */
static void vlog_timing(const char *name, const tp_timing *t)
{
    if (t->count == 0)
        return;
    tp_vlog("timing %s: n=%u avg=%.3fms min=%.3fms max=%.3fms", name, t->count,
            tp_timing_avg(t), t->min, t->max);
    for (int i = 0; i < TP_TIMING_BUCKETS; i++)
        if (t->hist[i])
            tp_vlog("  %s bucket < %.4f ms: %u", name,
                    tp_timing_bucket_hi_ms(i), t->hist[i]);
    if (t->over)
        tp_vlog("  %s >= %.4f ms: %u", name,
                tp_timing_bucket_hi_ms(TP_TIMING_BUCKETS - 1), t->over);
}

static int read_fb_geometry(int *w, int *h)
{
    FILE *f = fopen("/sys/class/graphics/fb0/virtual_size", "r");
    if (!f)
        return -1;
    int r = fscanf(f, "%d,%d", w, h);
    fclose(f);
    return r == 2 ? 0 : -1;
}

/* A post-flip fence fd only signals success when poll() reports POLLIN and
 * none of POLLERR/POLLHUP/POLLNVAL. Error/hangup bits are never success.
 * *revents_out receives the raw revents (0 when nothing is ready). */
static int fd_poll(int fd, short *revents_out)
{
    struct pollfd pf;
    pf.fd = fd;
    pf.events = POLLIN;
    pf.revents = 0;
    int r = poll(&pf, 1, 0);
    if (revents_out)
        *revents_out = (r > 0) ? pf.revents : 0;
    if (r <= 0)
        return 0;
    if ((pf.revents & POLLIN) &&
        !(pf.revents & (POLLERR | POLLHUP | POLLNVAL)))
        return 1;
    return 0;
}

static void poll_all(const int *post_fd, const int *have, int last, uint32_t vc,
                     const char *where)
{
    for (int j = 0; j <= last; j++) {
        if (!have[j])
            continue;
        short rev = 0;
        int sig = fd_poll(post_fd[j], &rev);
        tp_vlog("  poll[%s] frame=%d post_fd=%d vblank=%u revents=0x%04x signaled=%d",
                where, j, post_fd[j], vc, (unsigned)rev, sig);
    }
}

/* --------------------------------------------------------------- diagnostic */

static int run_diagnostic(const tp_config *c, int dcn)
{
    (void)c;
    tp_dc dc;
    tp_dc_init(&dc);
    if (tp_dc_open_readonly(&dc, dcn) != 0)
        return -1;

    struct tegra_dc_ext_status st;
    memset(&st, 0, sizeof(st));
    if (tp_dc_get_status(&dc, &st) == 0)
        tp_log("status: flags=0x%08x enabled=%d", st.flags,
               !!(st.flags & TEGRA_DC_EXT_FLAGS_ENABLED));

    uint32_t sid = TP_INVALID_SYNCPT;
    if (tp_dc_get_vblank_syncpt(&dc, &sid) == 0)
        tp_log("vblank syncpt id = %u", sid);

    if (sid != TP_INVALID_SYNCPT) {
        uint32_t v0 = 0, v1 = 0;
        double t0 = now_s();
        int ok0 = (tp_dc_syncpt_read(&dc, sid, &v0) == 0);
        struct timespec ts = { 1, 0 };
        nanosleep(&ts, NULL);
        int ok1 = (tp_dc_syncpt_read(&dc, sid, &v1) == 0);
        if (ok0 && ok1)
            tp_log("vblank syncpt sampled over %.2fs: %u -> %u (delta %u)",
                   now_s() - t0, v0, v1, v1 - v0);
    }

    int w = 0, h = 0;
    if (read_fb_geometry(&w, &h) == 0)
        tp_log("fb0 virtual_size = %dx%d", w, h);

    tp_dc_close(&dc);
    return 0;
}

/* ----------------------------------------------------------------- present */

/* Fill one FLIP4 window attribute from the config + the claimed window. */
static void fill_flip_attr(const tp_config *c, const tp_dc *dc, int buff_fd,
                           struct tegra_dc_ext_flip_windowattr *win)
{
    memset(win, 0, sizeof(*win));
    win->index = (__s32)dc->win_index;
    win->buff_id = (__u32)buff_fd;
    win->blend = TEGRA_DC_EXT_BLEND_NONE;
    win->offset = 0;
    win->stride = tp_gob_pitch((uint32_t)c->physical_w);
    win->pixformat = TEGRA_DC_EXT_FMT_T_A8R8G8B8;
    win->x = 0; win->y = 0;
    win->w = (__u32)((uint32_t)c->physical_w << 12);
    win->h = (__u32)((uint32_t)c->physical_h << 12);
    win->out_x = 0; win->out_y = 0;
    win->out_w = (__u32)c->physical_w;
    win->out_h = (__u32)c->physical_h;
    win->z = 255;
    win->swap_interval = 1;
    win->pre_syncpt_id = TP_INVALID_SYNCPT;
    win->flags |= TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR;
    win->block_height_log2 = (__u8)c->block_height_log2;
    win->global_alpha = 255;
}

/* Count this process's open fds via /proc/self/fd is provided by
 * src/procstat.c (tp_proc_fd_count / tp_proc_rss_kb): bounded, allocation-free
 * and excluding the counting opendir() fd. */

/* Absolute-deadline paced reuse loop. Every frame's FLIP4 targets the fixed
 * monotonic instant t_start + frame_seq*period; the frame is fully prepared
 * (fence gate, GPU render, GPU fence) first and ONLY the submission is paced,
 * by waiting on the absolute target immediately before FLIP4. This is the
 * conservative choice: preparation can never make us submit early, and an
 * overrun only makes that one frame late. Because targets are absolute there is
 * no accumulated drift, and there is no unconditional sleep after work. Pacing
 * is scheduling only: it is NOT latch/retirement proof and 60 Hz is never
 * claimed from the configured period. Every retirement/GPU-ownership/error
 * guard from the accepted M3g2 reuse path is preserved. */
static int run_reuse_frames(tp_gpu *g, tp_dc *dc, const tp_config *c,
                            double period, tp_reuse *r, int *ok_frames)
{
    double t_start = now_s();
    double t_limit = t_start + c->duration_s + 10.0; /* hard wall bound */
    double next_stats = t_start + 1.0;
    int rc = 0;

    tp_pacing pace;
    tp_pacing_init(&pace, t_start, period, c->frame_count);

    tp_timing t_gate, t_submit, t_wait, t_flip, t_gpu;
    tp_timing_reset(&t_gate);
    tp_timing_reset(&t_submit);
    tp_timing_reset(&t_wait);
    tp_timing_reset(&t_flip);
    tp_timing_reset(&t_gpu);
    unsigned gpu_ts_n = 0;

    /* Bounded resource telemetry: warmup baseline, once/second min/max, final. */
    tp_rangei fd_rng, rss_rng, live_rng;
    tp_rangei_reset(&fd_rng);
    tp_rangei_reset(&rss_rng);
    tp_rangei_reset(&live_rng);
    int fd_warmup = -1, rss_warmup = -1, warmup_done = 0;
    double t_warmup = 0.0;

    while (r->frame_seq < c->frame_count) {
        if (g_stop) {
            tp_log("reuse: stop signal at frame %d", r->frame_seq);
            rc = -1;
            break;
        }
        if (now_s() > t_limit) {
            tp_log("reuse: wall bound (%.1fs) reached at frame %d; stopping",
                   now_s() - t_start, r->frame_seq);
            r->errors++;
            rc = -1;
            break;
        }

        int seq = r->frame_seq;
        double target = tp_pacing_target(&pace, seq);

        /* Peek the round-robin slot, gate its OWN fence to completion, then
         * let begin() re-validate the guards with the slot now retired. */
        int peek = tp_reuse_peek_slot(r);
        if (peek < 0) {
            r->errors++;
            rc = -1;
            break;
        }

        /* Fence gate: retire the slot's OWN pending post fence BEFORE any GPU
         * write to it. One finite wait (<= TP_REUSE_POLL_BUDGET_MS); the target
         * slot is never the current front, so this cannot deadlock. */
        int old_fd = (r->slot[peek].state == TP_SLOT_SUBMITTED)
                     ? r->slot[peek].post_fd : -1;
        if (old_fd >= 0) {
            tp_release_result rr;
            double tg0 = now_s();
            int v = tp_dc_release_wait(old_fd, TP_REUSE_POLL_BUDGET_MS, &rr);
            tp_timing_add(&t_gate, (now_s() - tg0) * 1000.0);
            if (v != TP_REL_RETIRED) {
                tp_log("reuse: slot %d post_fd=%d NOT retired (api=%s "
                       "verdict=%d revents=0x%04x info_rc=%d info_errno=%d "
                       "status=%d n_fences=%u points=%u legacy_rc=%d); "
                       "failing closed, no overwrite",
                       peek, old_fd, tp_sync_api_name(rr.api), v,
                       (unsigned)rr.revents, rr.info_rc, rr.info_errno,
                       rr.status, rr.num_fences, rr.num_points, rr.legacy_rc);
                r->errors++;
                rc = -1;
                break;
            }
            if (tp_reuse_mark_retired(r, peek) != 0) {
                r->errors++;
                rc = -1;
                break;
            }
            close(old_fd); /* exactly once, only after validated retirement */
            tp_vlog("reuse: t=%.3f frame=%d slot=%d retired post_fd=%d "
                    "api=%s revents=0x%04x status=%d n_fences=%u points=%u",
                    now_s() - t_start, r->frame_seq, peek, old_fd,
                    tp_sync_api_name(rr.api), (unsigned)rr.revents, rr.status,
                    rr.num_fences, rr.num_points);
        }

        int slot = -1, acquire = 0;
        if (tp_reuse_begin(r, &slot, &acquire) != 0 || slot != peek) {
            tp_log("reuse: slot guard refused at frame %d (slot=%d peek=%d "
                   "last_submitted=%d)", r->frame_seq, slot, peek,
                   r->last_submitted);
            r->errors++;
            rc = -1;
            break;
        }

        int frame_seq = r->frame_seq;
        double ts0 = now_s();
        if (tp_gpu_render_frame_ex(g, c, slot, frame_seq, acquire) != 0) {
            tp_log("reuse: render frame %d slot %d failed", frame_seq, slot);
            r->errors++;
            rc = -1;
            break;
        }
        tp_timing_add(&t_submit, (now_s() - ts0) * 1000.0);

        double tw0 = now_s();
        int w = tp_gpu_wait_frame(g, slot, TP_GPU_FENCE_TIMEOUT_NS);
        tp_timing_add(&t_wait, (now_s() - tw0) * 1000.0);
        /* Per-frame detail only under --verbose. The wait time here is a host
         * wait, NOT GPU compute time. */
        tp_vlog("reuse: frame %d slot %d GPU fence %s (acquire=%d)", frame_seq,
                slot, w == 0 ? "signaled" : (w == 1 ? "TIMEOUT" : "error"),
                acquire);
        if (w != 0) {
            tp_log("reuse: frame %d slot %d GPU fence FAILED (verdict=%d)",
                   frame_seq, slot, w);
            r->errors++;
            rc = -1;
            break;
        }

        /* Optional GPU GOB-compute time: read ONLY now, after the fence has
         * signalled, without WAIT_BIT (never an unbounded wait). */
        uint64_t gpu_ns = 0;
        int tsv = tp_gpu_timestamp_read(g, slot, &gpu_ns);
        if (tsv == 0) {
            tp_timing_add(&t_gpu, (double)gpu_ns / 1e6);
            gpu_ts_n++;
        } else if (g->unsafe) {
            /* DEVICE_LOST on the query read: never ignored. Abort BEFORE the
             * paced FLIP4 and before any further submit. */
            tp_log("reuse: GPU DEVICE_LOST on timestamp read at frame %d slot %d; "
                   "aborting before FLIP4", frame_seq, slot);
            r->errors++;
            rc = -1;
            break;
        } else if (tsv < 0) {
            tp_vlog("reuse: GPU timestamp read failed at frame %d slot %d "
                    "(optional metric skipped)", frame_seq, slot);
        }

        /* Pace ONLY the submission: wait on the absolute deadline immediately
         * before FLIP4. Frame 0's target is t_start, so it never sleeps. */
        int sw = tp_sleep_until(target);
        if (sw != 0) {
            if (sw == -1)
                tp_log("reuse: stop signal while pacing frame %d", frame_seq);
            else
                tp_log("reuse: pacing sleep failed (%d) at frame %d", sw,
                       frame_seq);
            r->errors++;
            rc = -1;
            break;
        }

        struct tegra_dc_ext_flip_windowattr win;
        fill_flip_attr(c, dc, g->out[slot].fd, &win);
        int pf = -1;
        double tf0 = now_s();
        int fr = tp_dc_flip4(dc, &win, &pf);
        tp_timing_add(&t_flip, (now_s() - tf0) * 1000.0);
        if (fr != 0 || pf < 0) {
            tp_log("reuse: FLIP4 frame %d slot %d failed (post_fd=%d)",
                   frame_seq, slot, pf);
            r->errors++;
            rc = -1;
            break;
        }
        if (tp_reuse_mark_submitted(r, slot, pf) != 0) {
            close(pf); /* we own pf; do not leak it on a bookkeeping failure */
            r->errors++;
            rc = -1;
            break;
        }
        double submit_t = now_s();
        double late = tp_pacing_observe(&pace, seq, submit_t);
        tp_vlog("reuse: t=%.3f frame_seq=%d slot=%d export_fd=%d post_fd=%d "
                "acquire=%d release=1 live_post_fds=%d target=%.3f late_ms=%.3f",
                now_s() - t_start, frame_seq, slot, g->out[slot].fd, pf,
                acquire, r->live_post_fds, target - t_start, late * 1000.0);

        /* Warmup baseline (first completed frame, i.e. after lazy init). */
        if (!warmup_done) {
            fd_warmup = tp_proc_fd_count();
            rss_warmup = tp_proc_rss_kb();
            t_warmup = now_s() - t_start;
            warmup_done = 1;
        }

        double nowt = now_s();
        if (nowt >= next_stats) { /* bounded output rate (~1/s) */
            int fd = tp_proc_fd_count();
            int rss = tp_proc_rss_kb();
            tp_rangei_add(&fd_rng, fd);
            tp_rangei_add(&rss_rng, rss);
            tp_rangei_add(&live_rng, r->live_post_fds);
            tp_log("reuse stats: t=%.2f submitted=%u reused=%u retired=%u "
                   "errors=%u live_post=%d fd=%d rss_kb=%d late_frames=%u "
                   "missed=%u max_late_ms=%.3f",
                   nowt - t_start, r->submitted, r->reused, r->retired,
                   r->errors, r->live_post_fds, fd, rss, pace.late_frames,
                   pace.missed, pace.max_lateness * 1000.0);
            next_stats = nowt + 1.0;
        }
    }

    /* Final resource sample and a compact, honest summary. The fd/RSS totals
     * include lazily opened driver/loader handles: endpoints are NOT proof of
     * stability or of a leak, and no 60 Hz claim is made from the period. */
    int fd_final = tp_proc_fd_count();
    int rss_final = tp_proc_rss_kb();
    tp_rangei_add(&fd_rng, fd_final);
    tp_rangei_add(&rss_rng, rss_final);
    tp_rangei_add(&live_rng, r->live_post_fds);

    *ok_frames = (int)r->submitted;

    vlog_timing("gate_ms", &t_gate);
    vlog_timing("submit_cpu_ms", &t_submit);
    vlog_timing("gpu_wait_ms", &t_wait);
    vlog_timing("flip4_ms", &t_flip);
    vlog_timing("gpu_gob_ms", &t_gpu);

    double avg_late_ms = (pace.seq > 0)
                         ? pace.sum_lateness * 1000.0 / (double)pace.seq : 0.0;
    tp_log("M3i reuse summary: frames=%u submitted=%u reused=%u retired=%u "
           "errors=%u elapsed=%.3fs period=%.4fs late_frames=%u missed=%u "
           "max_late_ms=%.3f avg_late_ms=%.3f",
           (unsigned)c->frame_count, r->submitted, r->reused, r->retired,
           r->errors, now_s() - t_start, period, pace.late_frames, pace.missed,
           pace.max_lateness * 1000.0, avg_late_ms);
    tp_log("M3i resources: fd warmup=%d min=%ld max=%ld final=%d (n=%u) | "
           "rss_kb warmup=%d min=%ld max=%ld final=%d (n=%u) | "
           "live_post_fds min=%ld max=%ld final=%d | t_warmup=%.3fs "
           "gpu_ts_frames=%u",
           fd_warmup, fd_rng.have ? fd_rng.min : -1L,
           fd_rng.have ? fd_rng.max : -1L, fd_final, fd_rng.n, rss_warmup,
           rss_rng.have ? rss_rng.min : -1L,
           rss_rng.have ? rss_rng.max : -1L, rss_final, rss_rng.n,
           live_rng.have ? live_rng.min : -1L,
           live_rng.have ? live_rng.max : -1L, r->live_post_fds, t_warmup,
           gpu_ts_n);
    tp_log("M3i host_and_gpu_ms (avg/max): gate=%.3f/%.3f submit=%.3f/%.3f "
           "gpu_wait=%.3f/%.3f flip4=%.3f/%.3f gpu_gob(GPU)=%.3f/%.3f (n=%u)",
           tp_timing_avg(&t_gate), t_gate.max, tp_timing_avg(&t_submit),
           t_submit.max, tp_timing_avg(&t_wait), t_wait.max,
           tp_timing_avg(&t_flip), t_flip.max, tp_timing_avg(&t_gpu),
           t_gpu.max, t_gpu.count);

    if (rc == 0 && !g_stop && r->submitted == (unsigned)c->frame_count)
        return 0;
    return -1;
}

static int run_present(const tp_config *c, int dcn)
{
    /* --soak: raise the FB ioctl *bookkeeping* budget to cover the render
     * duration plus a finite setup/teardown allowance (normal runs keep the
     * 60 s default). Bookkeeping only: no kernel ioctl preemption is claimed,
     * and the probe wall bound / systemd hard max are unchanged. This must
     * precede the first FB ioctl; an invalid value or a late setter call is
     * rejected and we fail closed here. */
    if (c->soak) {
        unsigned int fb_budget_ms = 0;
        if (tp_fb_budget_soak_ms(c->duration_s, &fb_budget_ms) != 0 ||
            tp_fb_set_budget_ms(fb_budget_ms) != 0) {
            tp_log("ERROR: could not set the --soak FB ioctl budget for "
                   "duration %.1fs; refusing", c->duration_s);
            return -1;
        }
    }

    int rc = -1;
    int ok_frames = 0;   /* frames rendered + fence-signaled + FLIP4 submitted */
    tp_gpu g;
    tp_gpu_zero(&g);
    tp_dc dc;
    tp_dc_init(&dc);
    int post_fd[TP_MAX_SCANOUT_BUFFERS];
    int have[TP_MAX_SCANOUT_BUFFERS];
    for (int i = 0; i < TP_MAX_SCANOUT_BUFFERS; i++) {
        post_fd[i] = -1;
        have[i] = 0;
    }
    tp_reuse reuse;
    tp_reuse_init(&reuse);
    int fd_start = tp_proc_fd_count();
    int fd_after_resources = -1;
    int fd_pre_cleanup = -1;

    if (tp_gpu_init(&g, c) != 0) goto out;
    if (tp_gpu_create_pipelines(&g) != 0) goto out;
    if (tp_gpu_create_pattern_image(&g, c) != 0) goto out;
    if (tp_gpu_create_scanout(&g, c) != 0) goto out;

    /* Open the DC control fd. The window is claimed only later, after the
     * head is confirmed ENABLED and advancing (see below). */
    if (tp_dc_open_present(&dc, dcn) != 0) goto out;

    /* Prior head state. */
    struct tegra_dc_ext_status st;
    memset(&st, 0, sizeof(st));
    if (tp_dc_get_status(&dc, &st) != 0) {
        tp_log("ERROR: prior head state is unknown; refusing to change it");
        goto out;
    }
    int enabled = !!(st.flags & TEGRA_DC_EXT_FLAGS_ENABLED);
    tp_log("head state: dc=%d enabled=%d (prior)", dcn, enabled);

    if (!enabled) {
        if (!c->enable_head) {
            tp_log("ERROR: DC%d head is disabled and --enable-head was not given; "
                   "refusing (would require fbdev FBIOBLANK(UNBLANK) on /dev/fb0)",
                   dcn);
            goto out;
        }
        /* Enable the existing head via fbdev control only (no modeset, no
         * pixels). Do this BEFORE GET_WINDOW: any framebuffer restore the
         * unblank path performs must not touch a window we already own. */
        if (tp_fb_open(&dc) != 0) goto out;
        /* An ioctl error does not prove no partial hardware change occurred. */
        dc.head_changed = 1;
        if (tp_fb_unblank(&dc) != 0) {
            tp_log("ERROR: FBIOBLANK(UNBLANK) failed on /dev/fb0; refusing");
            goto out;
        }
        if (tp_dc_wait_ready(&dc, 1000) != 0) {
            tp_log("ERROR: unblank returned 0 but the head is not ENABLED with an "
                   "advancing vblank within 1s; failing and restoring");
            goto out;
        }
    } else {
        /* Already enabled: leave the head state alone (no unblank, no restore).
         * Readiness is still a sanity check before claiming a window. */
        if (tp_dc_wait_ready(&dc, 1000) != 0) {
            tp_log("ERROR: head reported ENABLED but no advancing vblank within 1s; "
                   "refusing to claim a window");
            goto out;
        }
    }

    /* Acquire the window only now: status ENABLED + advancing vblank confirmed. */
    if (tp_dc_acquire_window(&dc, c->window) != 0) goto out;
    tp_dc_ctrl_open(&dc);

    fd_after_resources = tp_proc_fd_count();
    tp_log("fds after resources: /proc_self_fd=%d exported_scanout=%d "
           "post_fence_live=%d (driver fds are the per-flip post fence fds)",
           fd_after_resources, g.nbuf, reuse.live_post_fds);

    double period = tp_frame_period(c);

    if (c->reuse) {
        if (c->soak)
            tp_log("soak mode: frames=%d duration=%.1fs period=%.4fs hard_wall=%.1fs "
                   "(3 persistent buffers, strict own-fence retirement, 1 Hz stats)",
                   c->frame_count, c->duration_s, period, c->duration_s + 10.0);
        int rok = run_reuse_frames(&g, &dc, c, period, &reuse, &ok_frames);
        tp_log("reuse done: submitted=%u reused=%u retired=%u errors=%u "
               "live_post_fds=%d", reuse.submitted, reuse.reused, reuse.retired,
               reuse.errors, reuse.live_post_fds);
        rc = (rok == 0) ? 0 : -1;
        goto out;
    }

    tp_log("present start: dc=%d window=%d logical=%dx%d physical=%dx%d orient=%d "
           "bhl2=%d frames=%d period=%.4fs/frame",
           dcn, dc.win_index, c->logical_w, c->logical_h, c->physical_w,
           c->physical_h, (int)c->orientation, c->block_height_log2,
           c->frame_count, period);

    for (int i = 0; i < g.nbuf && !g_stop; i++) {
        if (tp_gpu_render_frame(&g, c, i) != 0)
            break;
        int w = tp_gpu_wait_frame(&g, i, 2000000000ULL);
        tp_log("frame %d: GPU fence %s", i,
               w == 0 ? "signaled" : (w == 1 ? "TIMEOUT" : "error"));
        if (w != 0)
            break;

        struct tegra_dc_ext_flip_windowattr win;
        fill_flip_attr(c, &dc, g.out[i].fd, &win);

        int pf = -1;
        if (tp_dc_flip4(&dc, &win, &pf) != 0)
            break;
        post_fd[i] = pf;
        have[i] = 1;
        ok_frames++;
        tp_log("frame %d: FLIP4 buff_fd=%d stride=%u post_syncpt_fd=%d",
               i, g.out[i].fd, win.stride, pf);
        poll_all(post_fd, have, i, 0, "just-flipped");

        /* vblank-paced hold; sample the real vblank syncpt counter and poll
         * all retained fences. No SYNCPT_WAITEX: its timeout ABI is
         * unverified; a bounded nanosleep paces the sampling instead. */
        double t0 = now_s();
        uint32_t vc = 0;      /* observed vblank advances */
        uint32_t vlast = 0;
        int vhave = 0;
        for (;;) {
            if (g_stop)
                break;
            double el = now_s() - t0;
            if (el >= period)
                break;
            if (dc.have_vblank_syncpt) {
                uint32_t v = 0;
                if (tp_dc_syncpt_read(&dc, dc.vblank_syncpt, &v) == 0) {
                    if (!vhave) { vlast = v; vhave = 1; }
                    else if (v != vlast) { vc += (v - vlast); vlast = v; }
                }
            }
            struct timespec ts = { 0, 16000000 }; /* 16 ms bounded sample */
            nanosleep(&ts, NULL);
            poll_all(post_fd, have, i, vc, "vblank");
        }
    }

    /* Drain: bounded poll for any retained post-flip fence still unsignaled. */
    if (have[0]) {
        for (int t = 0; t < 40 && !g_stop; t++) {
            int all = 1;
            for (int j = 0; j < g.nbuf; j++)
                if (have[j] && !fd_poll(post_fd[j], NULL))
                    all = 0;
            if (all)
                break;
            struct timespec ts = { 0, 50000000 };
            nanosleep(&ts, NULL);
            poll_all(post_fd, have, g.nbuf - 1, 0, "drain");
        }
    }

    tp_log("present done; retained post-flip fds: %d %d %d %d",
           post_fd[0], post_fd[1], post_fd[2], post_fd[3]);
    /* Success only when every requested frame rendered, its GPU fence
     * signaled, and its FLIP4 was submitted. Cancellation or any failed
     * render/wait/FLIP stays nonzero. */
    if (ok_frames == c->frame_count && !g_stop)
        rc = 0;

out:
    /* Teardown order matters:
     *  1. bounded GPU completion (no unbounded vkDeviceWaitIdle),
     *  2. close the DC fd WHILE STILL OWNING the window (no PUT_WINDOW) so the
     *     kernel's flush/blank/unpin runs,
     *  3. if this run changed disabled->enabled, restore the prior fb0 blank
     *     level (fb0 stays open for this),
     *  4. then close fb0,
     *  5. keep exported memory alive until the DC close finished, then close the
     *     post-flip fds and destroy only fully-completed Vulkan resources.
     * If any fence is still pending (GPU timeout / device loss), close the DC
     * and exit nonzero WITHOUT destroying pending Vulkan resources. */
    fd_pre_cleanup = tp_proc_fd_count();
    tp_log("fds before cleanup: /proc_self_fd=%d exported_scanout=%d "
           "live_post=%d", fd_pre_cleanup, g.nbuf, reuse.live_post_fds);
    int gpu_safe = (tp_gpu_quiesce(&g, TP_GPU_FENCE_TIMEOUT_NS) == 0);
    tp_dc_close(&dc);
    if (dc.head_changed) {
        if (tp_fb_restore(&dc) != 0) /* only after the DC fd is released */
            rc = -1;
    }
    tp_fb_close(&dc);
    for (int i = 0; i < TP_MAX_SCANOUT_BUFFERS; i++)
        if (have[i] && post_fd[i] >= 0)
            close(post_fd[i]);
    /* Reuse mode: close every still-retained per-slot post fence fd exactly
     * once. The last front fence may stay pending; teardown never waits for a
     * successor flip and never reuses after this point. */
    for (int i = 0; i < reuse.n_slots; i++) {
        if (reuse.slot[i].state == TP_SLOT_SUBMITTED &&
            reuse.slot[i].post_fd >= 0) {
            close(reuse.slot[i].post_fd);
            reuse.slot[i].post_fd = -1;
            reuse.slot[i].state = TP_SLOT_AVAILABLE;
            reuse.live_post_fds = tp_reuse_live_fds(&reuse);
        }
    }
    tp_gpu_destroy(&g, gpu_safe);
    if (!gpu_safe)
        rc = -1;
    tp_log("fds after cleanup: /proc_self_fd=%d (start=%d resources=%d "
           "pre_cleanup=%d)", tp_proc_fd_count(), fd_start, fd_after_resources,
           fd_pre_cleanup);
    return rc;
}

/* -------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    tp_config c;
    tp_config_defaults(&c);

    int pr = tp_config_parse(&c, argc, argv);
    if (pr == 1) { tp_print_help(argv[0]); return 0; }
    if (pr < 0)  { tp_print_help(argv[0]); return 2; }

    tp_set_verbose(c.verbose);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    /* Dry run (default) touches no device at all. */
    if (!c.present && !c.diagnostic_only) {
        tp_log("dry run: --present not given; nothing submitted, no display change.");
        tp_print_help(argv[0]);
        return 0;
    }

    /* Resolve the physical panel size pre-device (no /dev access), then
     * enforce orientation consistency: a mismatch is rejected rather than
     * concealed by a shader clamp. */
    if (c.physical_w <= 0 || c.physical_h <= 0) {
        int w = 0, h = 0;
        if (read_fb_geometry(&w, &h) == 0) {
            c.physical_w = w; c.physical_h = h;
            tp_log("physical from fb0: %dx%d", w, h);
        } else {
            tp_orient_physical_size(c.orientation, c.logical_w, c.logical_h,
                                    &c.physical_w, &c.physical_h);
            tp_log("physical derived from mode+orientation: %dx%d",
                   c.physical_w, c.physical_h);
        }
    }

    int ew = 0, eh = 0;
    tp_orient_physical_size(c.orientation, c.logical_w, c.logical_h, &ew, &eh);
    if (c.physical_w != ew || c.physical_h != eh) {
        tp_log("ERROR: physical %dx%d != orientation-implied %dx%d for logical "
               "%dx%d orient %d; refusing (no shader clamp)",
               c.physical_w, c.physical_h, ew, eh, c.logical_w, c.logical_h,
               (int)c.orientation);
        return 2;
    }

    int dcn = c.dc;
    if (dcn < 0 && tp_dc_autodetect(&dcn) != 0) {
        tp_log("no usable /dev/tegra_dc_* candidate; pass --dc N");
        return 3;
    }

    if (c.diagnostic_only) {
        tp_log("diagnostic-only: no GET_WINDOW, no FLIP4, no display change");
        return run_diagnostic(&c, dcn) == 0 ? 0 : 4;
    }

    return run_present(&c, dcn) == 0 ? 0 : 5;
}
