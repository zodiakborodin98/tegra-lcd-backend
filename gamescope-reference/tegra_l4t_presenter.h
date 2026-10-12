// SPDX-License-Identifier: GPL-2.0-or-later
//
// M7f - persistent Tegra L4T presenter for the *normal* Gamescope Present path.
//
// This is the M7c3 first-frame vertical promoted into a long-lived presenter:
// the normal Gamescope compositor (steamcompmgr) owns the VkDevice/queue and the
// three-slot scanout ring; every Present() composites the real focused frame,
// converts it block-linear with the proven GOB compute shader on the SAME
// VkDevice/queue, presents it through TEGRA_DC_EXT_FLIP4 and retires the slot's
// own post fence before the slot is reused.
//
// DC open / GET_WINDOW / head enable / FLIP4 / close-while-owning and the
// kernel fence-status retirement are the algorithms of
// tegra_l4t_first_frame.cpp (itself adapted from tools/tegra-present-probe).
#pragma once

#include <stdint.h>

struct FrameInfo_t;

namespace gamescope
{
    // Process-lifetime presenter. One instance is kept by the Tegra L4T
    // connector; Init/Shutdown are idempotent and bounded.
    class CTegraL4TPresenter
    {
    public:
        CTegraL4TPresenter() = default;
        ~CTegraL4TPresenter();

        // Opens the DC, enables the head if needed, claims the scanout window and
        // builds the GOB pipeline + three OPAQUE_FD scanout buffers. Safe to call
        // more than once; returns false (and leaves nothing half-open on success
        // paths) on any failure.
        bool Init( int logical_w, int logical_h, int orientation,
                   int block_height_log2, int dc, int window, int refresh_mhz );

        // Composites pFrameInfo, converts it to block-linear and FLIP4s it.
        // Returns 0 on success. On an unknown GPU/DC status it fails closed.
        int PresentFrame( const FrameInfo_t *pFrameInfo );

        // Closes the DC while still owning the window, restores the prior fb0
        // blank level if this run enabled the head, then frees the GOB ring only
        // if the DC close was confirmed. Returns false (with no Vulkan frees)
        // when the close/restore could not be confirmed; the caller must then
        // exit nonzero without running destructors.
        bool Shutdown();

        bool Ready() const { return m_bReady; }

        // Counters for the bounded launch log (transport/composite/FLIP).
        uint64_t CompositeCount() const { return m_uComposite; }
        uint64_t FlipCount() const { return m_uFlip; }
        uint64_t ReuseWaitCount() const { return m_uReuseWaits; }

    private:
        struct Impl;
        Impl *m_pImpl = nullptr;
        bool m_bReady = false;
        uint64_t m_uComposite = 0;
        uint64_t m_uFlip = 0;
        uint64_t m_uReuseWaits = 0;
    };
}
