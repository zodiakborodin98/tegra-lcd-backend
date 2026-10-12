// SPDX-License-Identifier: GPL-2.0-or-later
//
// M7c - bounded opt-in first-frame path for the Tegra L4T backend.
//
// This is the *real* Gamescope vertical for one frame:
//   real vulkan_composite (internal diagnostic scene, RGBA8 output image)
//     -> GOB block-linear conversion on the SAME VkDevice/queue
//     -> TEGRA_DC_EXT_FLIP4 native presentation
//     -> own post-flip fence retirement (kernel fence-status query)
//
// It deliberately does NOT start wlserver/Xwayland/clients. It is bounded to a
// few frames and <= 15s. It is not a standalone probe: the composite is the
// renderer's own vulkan_composite and the buffer is presented through the
// backend. The GOB math/shader, the DC open/GET_WINDOW/FLIP4/close-while-owning
// and the fence-status retirement are adapted from tools/tegra-present-probe
// (see src/Backends/tegra_l4t/PROVENANCE.md).
#pragma once

#include <stdint.h>

namespace gamescope
{
    struct TegraFirstFrameConfig
    {
        int logical_w = 1280;
        int logical_h = 720;
        int physical_w = 0;   // 0 => read /sys/class/graphics/fb0 or derive
        int physical_h = 0;
        int orientation = 90; // degrees clockwise (0/90/180/270)
        int block_height_log2 = 4;
        int frame_count = 3;  // bounded 1..4
        int refresh_mhz = 0;  // 0 => 60 Hz
        int window = 0;
        int dc = -1;          // -1 => autodetect
    };

    // Runs the bounded first-frame path. Returns true ONLY if a real composite
    // was converted by the GOB shader, presented via FLIP4 and its own post
    // fence retired. Any partial failure returns false with an explicit log.
    bool TegraL4TRunFirstFrame( const TegraFirstFrameConfig &cfg );
}
