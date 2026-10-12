/* SPDX-License-Identifier: GPL-2.0-or-later
 * tegra-present-probe -- standalone first-frame/fence discovery probe.
 * Copyright (C) 2026 native-gamescope project contributors.
 * See third_party/PROVENANCE.md and COPYING.
 */
#ifndef TP_H
#define TP_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define TP_MAX_SCANOUT_BUFFERS 4
#define TP_INVALID_SYNCPT      0xFFFFFFFFu

/* Bounded 3-slot reuse mode (--reuse). */
#define TP_REUSE_SLOTS            3
#define TP_REUSE_MIN_FRAMES       4
#define TP_REUSE_MAX_FRAMES       1800  /* normal (<=30 s) reuse ceiling */
#define TP_REUSE_DEFAULT_FRAMES   120
#define TP_REUSE_DEFAULT_DURATION 10.0
#define TP_REUSE_POLL_BUDGET_MS   250   /* finite own-post-fence poll wait */
#define TP_GPU_FENCE_TIMEOUT_NS   2000000000ULL

/* Explicit long-run soak mode (--soak). Only valid with --present --reuse; it
 * raises the reuse ceilings to a 1800 s / 108000-frame (60 fps) budget. The
 * defaults below apply ONLY when --soak is explicit; normal and normal --reuse
 * behaviour is unchanged. The absolute wall bound (duration + 10 s) and the
 * fixed 3-slot ring stay exactly as before. */
#define TP_SOAK_MAX_FRAMES        108000  /* 60 fps * 1800 s */
#define TP_SOAK_MAX_DURATION      1800.0
#define TP_SOAK_DEFAULT_FRAMES    108000
#define TP_SOAK_DEFAULT_DURATION  1800.0

typedef enum {
    TP_ORIENT_0   = 0,
    TP_ORIENT_90  = 90,
    TP_ORIENT_180 = 180,
    TP_ORIENT_270 = 270,
} tp_orient;

typedef struct {
    int       present;          /* --present: allow GET_WINDOW/FLIP4            */
    int       diagnostic_only;  /* --diagnostic-only: never GET_WINDOW/FLIP4   */
    int       enable_head;      /* --enable-head: fbdev UNBLANK on DC0/fb0 only */
    int       verbose;          /* --verbose                                   */
    int       dc;               /* --dc N; -1 = autodetect                     */
    int       window;           /* --window N (default 0)                      */
    int       window_explicit;
    int       logical_w;        /* --mode WxH (render/logical size)            */
    int       logical_h;
    int       refresh_hz;       /* --mode ...@Hz (informational)               */
    int       physical_w;       /* --physical WxH; 0 = autodetect              */
    int       physical_h;
    tp_orient orientation;      /* --orientation 0|90|180|270                  */
    int       block_height_log2;/* GOB block height log2 (default 4)           */
    int       frame_count;      /* write-once: <=4; reuse: 4..1800             */
    double    duration_s;       /* <= 30 (<= 1800 with --soak)                 */
    int       reuse;            /* --reuse: bounded 3-slot scanout recycling   */
    int       soak;             /* --soak: explicit long-run ceiling raise     */
    int       frame_count_explicit;  /* user passed --frame-count              */
    int       duration_explicit;     /* user passed --duration                 */
} tp_config;

void tp_config_defaults(tp_config *c);
/* Returns 0 on success, 1 when help was requested, -1 on bad argument. */
int  tp_config_parse(tp_config *c, int argc, char **argv);
void tp_print_help(const char *argv0);
/* Per-frame hold period = duration_s / frame_count (no 2 s/frame clamp). */
double tp_frame_period(const tp_config *c);

/* ---- logging ---- */
void tp_set_verbose(int v);
void tp_log(const char *fmt, ...);
void tp_vlog(const char *fmt, ...);

/* ---- GOB block-linear host math (mirrors shaders/gob_swizzle.comp) ---- */
uint32_t tp_gob_swizzle(uint32_t x, uint32_t y);
uint32_t tp_gob_gobs_wide(uint32_t width_px);
uint32_t tp_gob_pitch(uint32_t width_px);      /* ceil(width*4/64)*64 */
size_t   tp_gob_offset(uint32_t px, uint32_t py, uint32_t gobs_wide, int bhl2);
size_t   tp_gob_exact_size(uint32_t width_px, uint32_t height_px, int bhl2);
/* Padded allocation footprint: exact_size + exact_size/4 (+25% safety margin),
 * the pinned reference formula from flip_layer.c:create_gob_dest. */
size_t   tp_gob_padded_size(uint32_t width_px, uint32_t height_px, int bhl2);

/* ---- rotation mapping (mirrors the shader's inverse map) ---- */
void tp_orient_physical_size(tp_orient o, int lw, int lh, int *pw, int *ph);
/* physical (px,py) -> logical (sx,sy); returns 0 if in range, -1 if not. */
int  tp_orient_map_logical(tp_orient o, int lw, int lh, int px, int py, int *sx, int *sy);

#endif /* TP_H */
