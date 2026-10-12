/* SPDX-License-Identifier: GPL-2.0-or-later
 * Host-side GOB block-linear math. The swizzle and block addressing are the
 * reference formula from vk_layer_tegra_dc_present gob_swizzle.comp (pin
 * ecfb76aadbdc93cd7008e77bec944724cd70c364); only the host-side rotation
 * mapping is ours. Used by tests and by the probe (sizing/pitch).
 */
#include "tp.h"

uint32_t tp_gob_swizzle(uint32_t x, uint32_t y)
{
    uint32_t x0 = x & 1u, x1 = (x >> 1) & 1u, x2 = (x >> 2) & 1u;
    uint32_t x3 = (x >> 3) & 1u, x4 = (x >> 4) & 1u, x5 = (x >> 5) & 1u;
    uint32_t y0 = y & 1u, y1 = (y >> 1) & 1u, y2 = (y >> 2) & 1u;
    return (x5 << 8) | (y2 << 7) | (y1 << 6) | (x4 << 5) | (y0 << 4)
         | (x3 << 3) | (x2 << 2) | (x1 << 1) | x0;
}

uint32_t tp_gob_gobs_wide(uint32_t width_px)
{
    /* Round UP to a whole GOB column (16px @ 4bpp): a width not divisible by
     * 16 must not leave a partial GOB column overlapping the next block. */
    return (width_px * 4u + 63u) / 64u;
}

uint32_t tp_gob_pitch(uint32_t width_px)
{
    return tp_gob_gobs_wide(width_px) * 64u;
}

size_t tp_gob_offset(uint32_t px, uint32_t py, uint32_t gobs_wide, int bhl2)
{
    uint32_t gob_col = px / 16u;
    uint32_t gobs_per_block = 1u << bhl2;
    uint32_t rows_per_block = 8u << bhl2;
    uint32_t block_idx = py / rows_per_block;
    uint32_t row_in_block = py % rows_per_block;
    uint32_t gob_row_in_block = row_in_block / 8u;
    uint32_t row_in_gob = row_in_block % 8u;
    /* GOBs within a block are column-major (reference-verified). */
    uint32_t gob_index_in_block = gob_col * gobs_per_block + gob_row_in_block;
    uint32_t bytes_per_block = gobs_wide * gobs_per_block * 512u;
    uint32_t gob_base = block_idx * bytes_per_block + gob_index_in_block * 512u;
    uint32_t byte_x_base = (px % 16u) * 4u;
    return (size_t)gob_base + tp_gob_swizzle(byte_x_base, row_in_gob);
}

size_t tp_gob_exact_size(uint32_t width_px, uint32_t height_px, int bhl2)
{
    uint32_t gobs_wide = tp_gob_gobs_wide(width_px);
    uint32_t gobs_per_block = 1u << bhl2;
    uint32_t rows_per_block = gobs_per_block * 8u;
    uint32_t num_blocks = (height_px + rows_per_block - 1u) / rows_per_block;
    return (size_t)num_blocks * (size_t)gobs_wide * gobs_per_block * 512u;
}

size_t tp_gob_padded_size(uint32_t width_px, uint32_t height_px, int bhl2)
{
    /* Pinned reference (flip_layer.c:create_gob_dest): the exportable
     * STORAGE_BUFFER is sized exact_size + exact_size/4 (+25% safety margin),
     * and the allocation deliberately exceeds the buffer's own minimum so it
     * must NOT be a dedicated allocation. */
    size_t exact = tp_gob_exact_size(width_px, height_px, bhl2);
    return exact + exact / 4u;
}

void tp_orient_physical_size(tp_orient o, int lw, int lh, int *pw, int *ph)
{
    if (o == TP_ORIENT_90 || o == TP_ORIENT_270) {
        *pw = lh;
        *ph = lw;
    } else {
        *pw = lw;
        *ph = lh;
    }
}

int tp_orient_map_logical(tp_orient o, int lw, int lh, int px, int py,
                          int *sx, int *sy)
{
    switch (o) {
    case TP_ORIENT_0:   *sx = px;          *sy = py;          break;
    case TP_ORIENT_90:  *sx = py;          *sy = lh - 1 - px; break;
    case TP_ORIENT_180: *sx = lw - 1 - px; *sy = lh - 1 - py; break;
    case TP_ORIENT_270: *sx = lw - 1 - py; *sy = px;          break;
    default: return -1;
    }
    if (*sx < 0 || *sy < 0 || *sx >= lw || *sy >= lh)
        return -1;
    return 0;
}
