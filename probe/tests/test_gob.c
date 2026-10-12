/* SPDX-License-Identifier: GPL-2.0-or-later
 * GOB math unit tests. Independent known coordinate vectors for the reference
 * swizzle, plus boundary/non-16-aligned widths, size/bounds and rotation maps.
 */
#include "tp.h"

#include <stdio.h>
#include <string.h>

static int g_fail = 0;
static int g_pass = 0;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static void test_swizzle_vectors(void)
{
    /* Hand-derived from the reference formula: result =
     * (x5<<8)|(y2<<7)|(y1<<6)|(x4<<5)|(y0<<4)|(x3<<3)|(x2<<2)|(x1<<1)|x0 */
    struct { uint32_t x, y, e; } v[] = {
        { 0, 0, 0 }, { 1, 0, 1 }, { 2, 0, 2 }, { 4, 0, 4 }, { 8, 0, 8 },
        { 16, 0, 32 }, { 32, 0, 256 },
        { 0, 1, 16 }, { 0, 2, 64 }, { 0, 4, 128 },
        { 63, 7, 511 },                 /* all bits set -> 511 */
        { 4, 4, 128 + 4 },              /* x2 + y2 */
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++)
        CHECK(tp_gob_swizzle(v[i].x, v[i].y) == v[i].e,
              "swizzle(%u,%u)=%u expected %u", v[i].x, v[i].y,
              tp_gob_swizzle(v[i].x, v[i].y), v[i].e);
}

static void test_pitch_and_gobs_wide(void)
{
    struct { uint32_t w, gobs, pitch; } v[] = {
        { 1280, 80, 5120 },  /* 1280*4/64 = 80 exactly */
        { 720,  45, 2880 },  /* 2880/64 = 45 exactly  */
        { 500,  32, 2048 },  /* 2000/64 -> ceil 32    */
        { 16,    1,   64 },  /* one GOB column        */
        { 17,    2,  128 },  /* non-16-aligned width  */
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        CHECK(tp_gob_gobs_wide(v[i].w) == v[i].gobs,
              "gobs_wide(%u)=%u expected %u", v[i].w,
              tp_gob_gobs_wide(v[i].w), v[i].gobs);
        CHECK(tp_gob_pitch(v[i].w) == v[i].pitch,
              "pitch(%u)=%u expected %u", v[i].w,
              tp_gob_pitch(v[i].w), v[i].pitch);
    }
}

/* One 16x8 image at bhl2=4 is exactly one 512-byte GOB; every pixel must map
 * to a distinct 4-byte-aligned slot covering the whole 512 bytes. */
static void test_single_gob_permutation(void)
{
    int seen[512];
    memset(seen, 0, sizeof(seen));
    uint32_t gobs_wide = tp_gob_gobs_wide(16);
    for (uint32_t y = 0; y < 8; y++) {
        for (uint32_t x = 0; x < 16; x++) {
            size_t off = tp_gob_offset(x, y, gobs_wide, 4);
            CHECK(off < 512 && (off % 4) == 0,
                  "offset(%u,%u)=%zu not 4-aligned within one GOB", x, y, off);
            if (off < 512)
                seen[off] = 1;
        }
    }
    int holes = 0;
    for (int i = 0; i < 512; i += 4)
        if (!seen[i])
            holes++;
    CHECK(holes == 0, "single GOB not fully covered (%d holes)", holes);
}

static void test_size_bounds(void)
{
    struct { uint32_t w, h; int bhl2; } v[] = {
        { 720, 1280, 4 }, { 1280, 720, 4 }, { 500, 500, 4 }, { 500, 500, 5 },
    };
    for (size_t i = 0; i < sizeof(v) / sizeof(v[0]); i++) {
        uint32_t w = v[i].w, h = v[i].h;
        uint32_t gobs_wide = tp_gob_gobs_wide(w);
        size_t sz = tp_gob_exact_size(w, h, v[i].bhl2);
        size_t maxoff = 0;
        for (uint32_t y = 0; y < h; y += 7)
            for (uint32_t x = 0; x < w; x += 5) {
                size_t off = tp_gob_offset(x, y, gobs_wide, v[i].bhl2);
                if (off + 4 > maxoff)
                    maxoff = off + 4;
            }
        CHECK(maxoff <= sz, "size(%ux%u,bhl2=%d): max end %zu > size %zu",
              w, h, v[i].bhl2, maxoff, sz);
    }
}

static void test_rotation_map(void)
{
    int sx = 0, sy = 0;
    /* landscape 1280x720 -> portrait 720x1280 at 90 degrees clockwise. */
    CHECK(tp_orient_map_logical(TP_ORIENT_90, 1280, 720, 0, 0, &sx, &sy) == 0 &&
          sx == 0 && sy == 719, "rot90(0,0) -> %d,%d expected 0,719", sx, sy);
    CHECK(tp_orient_map_logical(TP_ORIENT_90, 1280, 720, 719, 0, &sx, &sy) == 0 &&
          sx == 0 && sy == 0, "rot90(719,0) -> %d,%d expected 0,0", sx, sy);
    CHECK(tp_orient_map_logical(TP_ORIENT_90, 1280, 720, 0, 1279, &sx, &sy) == 0 &&
          sx == 1279 && sy == 719, "rot90(0,1279) -> %d,%d expected 1279,719", sx, sy);
    CHECK(tp_orient_map_logical(TP_ORIENT_90, 1280, 720, 719, 1279, &sx, &sy) == 0 &&
          sx == 1279 && sy == 0, "rot90(719,1279) -> %d,%d expected 1279,0", sx, sy);

    CHECK(tp_orient_map_logical(TP_ORIENT_0, 1280, 720, 0, 0, &sx, &sy) == 0 &&
          sx == 0 && sy == 0, "rot0(0,0) wrong");
    CHECK(tp_orient_map_logical(TP_ORIENT_180, 1280, 720, 0, 0, &sx, &sy) == 0 &&
          sx == 1279 && sy == 719, "rot180(0,0) -> %d,%d expected 1279,719", sx, sy);
    CHECK(tp_orient_map_logical(TP_ORIENT_270, 1280, 720, 0, 0, &sx, &sy) == 0 &&
          sx == 1279 && sy == 0, "rot270(0,0) -> %d,%d expected 1279,0", sx, sy);
    CHECK(tp_orient_map_logical(TP_ORIENT_270, 1280, 720, 719, 1279, &sx, &sy) == 0 &&
          sx == 0 && sy == 719, "rot270(719,1279) -> %d,%d expected 0,719", sx, sy);

    int pw = 0, ph = 0;
    tp_orient_physical_size(TP_ORIENT_90, 1280, 720, &pw, &ph);
    CHECK(pw == 720 && ph == 1280, "phys size rot90 = %dx%d expected 720x1280", pw, ph);

    /* Out-of-range physical coordinate must be rejected. */
    CHECK(tp_orient_map_logical(TP_ORIENT_0, 1280, 720, 1280, 0, &sx, &sy) == -1,
          "out-of-range should fail");
}

int main(void)
{
    test_swizzle_vectors();
    test_pitch_and_gobs_wide();
    test_single_gob_permutation();
    test_size_bounds();
    test_rotation_map();

    printf("gob tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
