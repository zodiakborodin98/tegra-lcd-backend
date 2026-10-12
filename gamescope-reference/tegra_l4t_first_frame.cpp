// SPDX-License-Identifier: GPL-2.0-or-later
//
// M7c - bounded opt-in first-frame path for the Tegra L4T backend.
// See tegra_l4t_first_frame.h for what this is (and is not).
//
// Adapted/reused from tools/tegra-present-probe (GPL-2.0-or-later):
//   * GOB block-linear math + orientation map: src/gob_math.c
//   * GOB compute shader: shaders/gob_swizzle.comp (gen/gob_swizzle_spv.h)
//   * DC open/GET_WINDOW/FLIP4/fbdev head-enable/close-while-owning: src/dc.c
//   * kernel fence-status retirement (modern + legacy): src/sync_release.c
// The Vulkan GOB submission is NEW here: it runs on the *existing* Gamescope
// VkDevice/queue instead of the probe's own device.

#include "Backends/tegra_l4t/tegra_l4t_first_frame.h"

#include "rendervulkan.hpp"
#include "Backends/TegraL4TBackend.h"

#include "Backends/tegra_l4t/gob_swizzle_spv.h"
#include "Backends/tegra_l4t/tegra_dc_ext.h"
#include "Backends/tegra_l4t/nvhost_ioctl.h"

#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstddef>
#include <cstdarg>
#include <cerrno>
#include <cinttypes>
#include <ctime>
#include <vector>
#include <string>
#include <optional>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <linux/sync_file.h>

// The composite size globals live at global scope (steamcompmgr.cpp). They are
// normally set by the steamcompmgr thread, which this bounded mode never
// starts, so set them directly before the first composite.
extern uint32_t currentOutputWidth;
extern uint32_t currentOutputHeight;

#ifndef FBIOBLANK
#define FBIOBLANK 0x4611 /* _IO('F', 0x11) */
#endif

namespace gamescope
{
    // =====================================================================
    // logging (stderr, flushed: these lines must survive a driver SIGSEGV)
    // =====================================================================
    static void ff_log( const char *pFmt, ... )
    {
        va_list args;
        va_start( args, pFmt );
        fprintf( stderr, "[tegra-l4t-ff] " );
        vfprintf( stderr, pFmt, args );
        fprintf( stderr, "\n" );
        va_end( args );
        fflush( stderr );
    }

    // =====================================================================
    // GOB block-linear math (adapted from gob_math.c)
    // =====================================================================
    [[maybe_unused]] static uint32_t gob_swizzle( uint32_t x, uint32_t y )
    {
        uint32_t x0 = x & 1u, x1 = ( x >> 1 ) & 1u, x2 = ( x >> 2 ) & 1u;
        uint32_t x3 = ( x >> 3 ) & 1u, x4 = ( x >> 4 ) & 1u, x5 = ( x >> 5 ) & 1u;
        uint32_t y0 = y & 1u, y1 = ( y >> 1 ) & 1u, y2 = ( y >> 2 ) & 1u;
        return ( x5 << 8 ) | ( y2 << 7 ) | ( y1 << 6 ) | ( x4 << 5 ) | ( y0 << 4 )
             | ( x3 << 3 ) | ( x2 << 2 ) | ( x1 << 1 ) | x0;
    }

    static uint32_t gob_gobs_wide( uint32_t width_px )
    {
        return ( width_px * 4u + 63u ) / 64u;
    }

    static uint32_t gob_pitch( uint32_t width_px )
    {
        return gob_gobs_wide( width_px ) * 64u;
    }

    static size_t gob_exact_size( uint32_t width_px, uint32_t height_px, int bhl2 )
    {
        uint32_t gobs_wide = gob_gobs_wide( width_px );
        uint32_t gobs_per_block = 1u << bhl2;
        uint32_t rows_per_block = gobs_per_block * 8u;
        uint32_t num_blocks = ( height_px + rows_per_block - 1u ) / rows_per_block;
        return (size_t)num_blocks * (size_t)gobs_wide * gobs_per_block * 512u;
    }

    static size_t gob_padded_size( uint32_t width_px, uint32_t height_px, int bhl2 )
    {
        size_t exact = gob_exact_size( width_px, height_px, bhl2 );
        return exact + exact / 4u;
    }

    static void orient_physical_size( int orientation, int lw, int lh, int *pw, int *ph )
    {
        if ( orientation == 90 || orientation == 270 ) { *pw = lh; *ph = lw; }
        else                                           { *pw = lw; *ph = lh; }
    }

    // =====================================================================
    // kernel fence-status retirement (adapted from sync_release.c)
    //   modern SYNC_IOC_FILE_INFO first; ONLY its ENOTTY selects legacy
    //   SYNC_IOC_FENCE_INFO. Anything unknown fails closed.
    // =====================================================================
    enum FfSyncApi { FF_SYNC_API_NONE = 0, FF_SYNC_API_MODERN = 1, FF_SYNC_API_LEGACY = 2 };
    enum FfVerdict { FF_REL_NOT_READY = 0, FF_REL_RETIRED = 1, FF_REL_FAIL = 2 };

    #define FF_SYNC_IOC_MAGIC '>'
    struct ff_sync_pt_info
    {
        uint32_t len;
        char     obj_name[32];
        char     driver_name[32];
        int32_t  status;
        uint64_t timestamp_ns;
    };
    struct ff_sync_fence_info_data
    {
        uint32_t len;
        char     name[32];
        int32_t  status;
    };
    #define FF_SYNC_IOC_FENCE_INFO _IOWR( FF_SYNC_IOC_MAGIC, 2, struct ff_sync_fence_info_data )
    #define FF_LEGACY_INFO_MAX 1024

    static double ff_mono_ms()
    {
        struct timespec ts;
        clock_gettime( CLOCK_MONOTONIC, &ts );
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    }

    // One bounded poll + kernel status query. Mirrors tp_release_poll_info.
    static FfVerdict ff_release_poll( int fd, int budget_ms )
    {
        if ( fd < 0 )
            return FF_REL_FAIL;
        if ( budget_ms < 0 )
            budget_ms = 0;

        double deadline = ff_mono_ms() + (double)budget_ms;

        for (;;)
        {
            double remain_d = deadline - ff_mono_ms();
            int remain = ( remain_d > 0.0 ) ? (int)remain_d : 0;
            if ( remain <= 0 )
                return FF_REL_NOT_READY;

            struct pollfd pf;
            pf.fd = fd;
            pf.events = POLLIN;
            pf.revents = 0;
            int r = poll( &pf, 1, remain );
            if ( r < 0 )
            {
                if ( errno == EINTR )
                    continue;
                return FF_REL_FAIL;
            }
            if ( r == 0 )
                return FF_REL_NOT_READY;

            short rev = pf.revents;
            if ( rev & ( POLLERR | POLLHUP | POLLNVAL ) )
                return FF_REL_FAIL;
            if ( !( rev & POLLIN ) )
                continue;

            struct sync_file_info info;
            memset( &info, 0, sizeof( info ) );
            errno = 0;
            int irc = ioctl( fd, SYNC_IOC_FILE_INFO, &info );
            int ie = errno;
            if ( irc == 0 )
            {
                if ( info.num_fences == 0 )
                    return FF_REL_FAIL;
                if ( info.status < 0 )
                    return FF_REL_FAIL;
                if ( info.status == 0 )
                    continue;               // still active: bounded re-poll
                return FF_REL_RETIRED;
            }
            if ( ie != ENOTTY )
                return FF_REL_FAIL;         // other modern error: no fallback

            // Legacy Android-staging sync (L4T r32). Fail closed on anything
            // malformed; never assume retired.
            unsigned char raw[FF_LEGACY_INFO_MAX];
            memset( raw, 0, sizeof( raw ) );
            ( (struct ff_sync_fence_info_data *)raw )->len = (uint32_t)sizeof( raw );
            errno = 0;
            int lrc = ioctl( fd, FF_SYNC_IOC_FENCE_INFO, raw );
            if ( lrc != 0 )
                return FF_REL_FAIL;

            uint32_t total = 0, plen = 0;
            int32_t agg = 0, pstat = 0;
            memcpy( &total, raw + offsetof( struct ff_sync_fence_info_data, len ), sizeof( total ) );
            memcpy( &agg, raw + offsetof( struct ff_sync_fence_info_data, status ), sizeof( agg ) );
            if ( total < sizeof( struct ff_sync_fence_info_data ) || total > sizeof( raw ) )
                return FF_REL_FAIL;
            if ( agg > 1 )
                return FF_REL_FAIL;
            unsigned off = (unsigned)sizeof( struct ff_sync_fence_info_data );
            unsigned n = 0;
            while ( off < total )
            {
                if ( total - off < sizeof( struct ff_sync_pt_info ) )
                    return FF_REL_FAIL;
                memcpy( &plen, raw + off + offsetof( struct ff_sync_pt_info, len ), sizeof( plen ) );
                memcpy( &pstat, raw + off + offsetof( struct ff_sync_pt_info, status ), sizeof( pstat ) );
                if ( plen < sizeof( struct ff_sync_pt_info ) || plen > total - off )
                    return FF_REL_FAIL;
                if ( pstat < 0 || pstat > 1 )
                    return FF_REL_FAIL;
                if ( agg == 1 && pstat != 1 )
                    return FF_REL_FAIL;
                off += plen;
                n++;
            }
            if ( off != total || n == 0 )
                return FF_REL_FAIL;
            if ( agg == 1 )
                return FF_REL_RETIRED;
            if ( agg < 0 )
                return FF_REL_FAIL;
            continue;                       // active: bounded re-poll
        }
    }

    // =====================================================================
    // DC (adapted from dc.c)
    // =====================================================================
    struct FfDc
    {
        int      fd = -1;
        int      ctrl_fd = -1;
        int      fb_fd = -1;
        int      dc_num = -1;
        int      win_index = -1;
        char     path[64] = {};
        uint32_t vblank_syncpt = 0;
        bool     have_vblank_syncpt = false;
        bool     fb_blank_known = false;
        int      fb_blank_value = -1;
        bool     owns_window = false;
        bool     head_changed = false;
    };

    static int ff_dc_autodetect()
    {
        int best = -1, enabled_fallback = -1;
        uint32_t best_delta = 0;
        int ctrl_fd = open( "/dev/nvhost-ctrl", O_RDWR | O_CLOEXEC );

        for ( int nn = 0; nn < 4; nn++ )
        {
            char path[64];
            snprintf( path, sizeof( path ), "/dev/tegra_dc_%d", nn );
            int fd = open( path, O_RDONLY | O_CLOEXEC );
            if ( fd < 0 )
                continue;

            struct tegra_dc_ext_status st;
            memset( &st, 0, sizeof( st ) );
            bool have_status = ( ioctl( fd, TEGRA_DC_EXT_GET_STATUS, &st ) == 0 );

            uint32_t sid = 0xFFFFFFFFu;
            bool have_sid = ( ioctl( fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, &sid ) == 0 );

            uint32_t delta = 0;
            if ( have_sid && ctrl_fd >= 0 )
            {
                struct nvhost_ctrl_syncpt_read_args a;
                memset( &a, 0, sizeof( a ) );
                a.id = sid;
                uint32_t v0 = 0, v1 = 0;
                bool ok0 = ( ioctl( ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a ) == 0 );
                if ( ok0 ) v0 = a.value;
                struct timespec ts = { 0, 120000000 };
                nanosleep( &ts, nullptr );
                memset( &a, 0, sizeof( a ) );
                a.id = sid;
                bool ok1 = ( ioctl( ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a ) == 0 );
                if ( ok1 ) v1 = a.value;
                if ( ok0 && ok1 ) delta = v1 - v0;
            }

            bool enabled = have_status && ( st.flags & TEGRA_DC_EXT_FLAGS_ENABLED );
            ff_log( "autodetect %s: status=%s flags=0x%08x syncpt=%u delta=%u",
                    path, have_status ? "ok" : "n/a", st.flags, sid, delta );

            if ( enabled && enabled_fallback < 0 )
                enabled_fallback = nn;
            if ( delta > best_delta ) { best_delta = delta; best = nn; }
            close( fd );
        }
        if ( ctrl_fd >= 0 )
            close( ctrl_fd );

        if ( best >= 0 && best_delta > 0 ) { ff_log( "autodetect: head %d (advancing vblank)", best ); return best; }
        if ( enabled_fallback >= 0 )       { ff_log( "autodetect: head %d (enabled fallback)", enabled_fallback ); return enabled_fallback; }
        if ( best >= 0 )                    return best;
        ff_log( "autodetect: no /dev/tegra_dc_* candidate" );
        return -1;
    }

    static int ff_dc_open_present( FfDc &d, int dc_num )
    {
        snprintf( d.path, sizeof( d.path ), "/dev/tegra_dc_%d", dc_num );
        d.fd = open( d.path, O_RDWR | O_CLOEXEC );
        if ( d.fd < 0 ) { ff_log( "open %s failed: %s", d.path, strerror( errno ) ); return -1; }
        d.dc_num = dc_num;
        return 0;
    }

    static int ff_dc_get_status( FfDc &d, struct tegra_dc_ext_status *st )
    {
        return ioctl( d.fd, TEGRA_DC_EXT_GET_STATUS, st ) < 0 ? -1 : 0;
    }

    static int ff_dc_get_vblank_syncpt( FfDc &d, uint32_t *id )
    {
        return ioctl( d.fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, id ) < 0 ? -1 : 0;
    }

    static int ff_dc_ctrl_open( FfDc &d )
    {
        if ( d.ctrl_fd >= 0 ) return 0;
        d.ctrl_fd = open( "/dev/nvhost-ctrl", O_RDWR | O_CLOEXEC );
        return d.ctrl_fd < 0 ? -1 : 0;
    }

    static int ff_dc_syncpt_read( FfDc &d, uint32_t id, uint32_t *value )
    {
        struct nvhost_ctrl_syncpt_read_args a;
        memset( &a, 0, sizeof( a ) );
        a.id = id;
        if ( ff_dc_ctrl_open( d ) < 0 ) return -1;
        if ( ioctl( d.ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a ) < 0 ) return -1;
        *value = a.value;
        return 0;
    }

    static int ff_dc_wait_ready( FfDc &d, int budget_ms )
    {
        if ( !d.have_vblank_syncpt )
        {
            uint32_t sid = 0;
            if ( ff_dc_get_vblank_syncpt( d, &sid ) == 0 ) { d.vblank_syncpt = sid; d.have_vblank_syncpt = true; }
        }
        double t0 = ff_mono_ms();
        bool have = false;
        uint32_t first = 0;
        for (;;)
        {
            struct tegra_dc_ext_status st;
            memset( &st, 0, sizeof( st ) );
            if ( ff_dc_get_status( d, &st ) == 0 &&
                 ( st.flags & TEGRA_DC_EXT_FLAGS_ENABLED ) && d.have_vblank_syncpt )
            {
                uint32_t v = 0;
                if ( ff_dc_syncpt_read( d, d.vblank_syncpt, &v ) == 0 )
                {
                    if ( !have ) { first = v; have = true; }
                    else if ( v != first ) return 0;
                }
            }
            if ( ff_mono_ms() - t0 >= (double)budget_ms )
                break;
            struct timespec ts = { 0, 16000000 };
            nanosleep( &ts, nullptr );
        }
        ff_log( "head not ready within %d ms", budget_ms );
        return -1;
    }

    static int ff_dc_acquire_window( FfDc &d, int win_index )
    {
        if ( ioctl( d.fd, TEGRA_DC_EXT_GET_WINDOW, (unsigned long)win_index ) < 0 )
        {
            int e = errno;
            ff_log( "GET_WINDOW %d on %s failed: %s%s", win_index, d.path, strerror( e ),
                    e == EBUSY ? " (EBUSY: do NOT steal)" : "" );
            return -1;
        }
        d.win_index = win_index;
        d.owns_window = true;
        uint32_t sid = 0;
        if ( ff_dc_get_vblank_syncpt( d, &sid ) == 0 ) { d.vblank_syncpt = sid; d.have_vblank_syncpt = true; }
        ff_log( "acquired window %d on %s, vblank syncpt=%u", win_index, d.path, d.vblank_syncpt );
        return 0;
    }

    static int ff_fb_open( FfDc &d )
    {
        if ( d.fb_fd >= 0 ) return 0;
        FILE *f = fopen( "/sys/class/graphics/fb0/blank", "r" );
        if ( f )
        {
            int v = -1;
            if ( fscanf( f, "%d", &v ) == 1 ) { d.fb_blank_known = true; d.fb_blank_value = v; }
            fclose( f );
        }
        d.fb_fd = open( "/dev/fb0", O_RDWR | O_CLOEXEC );
        if ( d.fb_fd < 0 ) { ff_log( "open /dev/fb0 failed: %s", strerror( errno ) ); return -1; }
        ff_log( "opened /dev/fb0 (prior blank level %s)", d.fb_blank_known ? std::to_string( d.fb_blank_value ).c_str() : "unknown" );
        return 0;
    }

    static int ff_fb_unblank( FfDc &d )
    {
        if ( d.fb_fd < 0 ) return -1;
        int r = ioctl( d.fb_fd, FBIOBLANK, 0 /* FB_BLANK_UNBLANK */ );
        ff_log( "FBIOBLANK(UNBLANK) -> %d (%s)", r, r == 0 ? "ok" : strerror( errno ) );
        return r == 0 ? 0 : -1;
    }

    static int ff_fb_restore( FfDc &d )
    {
        if ( d.fb_fd < 0 ) return -1;
        int level;
        if ( d.fb_blank_known && ( d.fb_blank_value == 2 || d.fb_blank_value == 3 || d.fb_blank_value == 4 ) )
            level = d.fb_blank_value;
        else
            level = 4; /* FB_BLANK_POWERDOWN */
        errno = 0;
        int r = ioctl( d.fb_fd, FBIOBLANK, level );
        ff_log( "FBIOBLANK(%d) restore -> %d (%s)", level, r, r == 0 ? "ok" : strerror( errno ) );
        return r == 0 ? 0 : -1;
    }

    static void ff_fb_close( FfDc &d )
    {
        if ( d.fb_fd >= 0 ) { close( d.fb_fd ); d.fb_fd = -1; }
    }

    // Close the DC fd WHILE STILL OWNING the window (no PUT_WINDOW) so the
    // kernel's close path runs flush/blank/unpin. Returns true only when the
    // close was confirmed; a failed/unknown close must NOT authorise freeing
    // memory that could still be scanned out.
    static bool ff_dc_close( FfDc &d )
    {
        bool ok = true;
        if ( d.ctrl_fd >= 0 ) { close( d.ctrl_fd ); d.ctrl_fd = -1; }
        if ( d.fd >= 0 )
        {
            ff_log( "closing %s %s", d.path, d.owns_window ? "(still owning window, no PUT_WINDOW)" : "(no window claimed)" );
            errno = 0;
            if ( close( d.fd ) != 0 )
            {
                ff_log( "close(%s) FAILED: %s (scanout ownership unknown; NOT freeing scanout memory)",
                        d.path, strerror( errno ) );
                ok = false;
            }
            d.fd = -1;
        }
        return ok;
    }

    // M7c2: fail-closed teardown for an unknown GOB-submission status or any
    // failure while a flip is live. NO C++ destructors run and NO Vulkan/scanout
    // memory is freed (the GPU or the DC may still reference it). Only a
    // best-effort safe DC close (kernel flush/disable/unpin) is attempted, then
    // the process exits nonzero immediately. This never masks success.
    [[noreturn]] static void ff_fail_closed( FfDc &dc, const char *pWhy, int code = 3 )
    {
        ff_log( "FATAL: %s; GPU/DC status unknown -> best-effort safe DC close, then immediate exit %d (no destructors, no Vulkan frees)", pWhy, code );
        (void)ff_dc_close( dc );
        if ( dc.head_changed )
            ff_fb_restore( dc );
        ff_fb_close( dc );
        _exit( code );
    }

    static int ff_dc_flip4( FfDc &d, struct tegra_dc_ext_flip_windowattr *win, int *post_fd )
    {
        struct tegra_dc_ext_flip_4 flip;
        memset( &flip, 0, sizeof( flip ) );
        flip.win = (__u64)(uintptr_t)win;
        flip.win_num = 1;
        flip.post_syncpt_fd = -1;
        if ( post_fd ) *post_fd = -1;
        if ( ioctl( d.fd, TEGRA_DC_EXT_FLIP4, &flip ) < 0 )
        {
            ff_log( "FLIP4 on %s failed: %s", d.path, strerror( errno ) );
            return -1;
        }
        if ( post_fd ) *post_fd = flip.post_syncpt_fd;
        return 0;
    }

    // =====================================================================
    // Vulkan GOB conversion on the existing Gamescope VkDevice/queue
    // =====================================================================
    struct FfScanout
    {
        VkBuffer       buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        int            fd = -1;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence        fence = VK_NULL_HANDLE;
        VkDescriptorSet set = VK_NULL_HANDLE;
        int            post_fd = -1;
        bool           ever_flipped = false;
    };

    struct FfGob
    {
        VkCommandPool       pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout    pl = VK_NULL_HANDLE;
        VkPipeline          pipe = VK_NULL_HANDLE;
        VkDescriptorPool    dpool = VK_NULL_HANDLE;
        FfScanout           out[3];
        int                 nbuf = 0;
    };

    static uint32_t ff_find_memtype( uint32_t bits, VkMemoryPropertyFlags props, bool require )
    {
        VkPhysicalDeviceMemoryProperties mp;
        g_device.vk.GetPhysicalDeviceMemoryProperties( g_device.physDev(), &mp );
        for ( uint32_t i = 0; i < mp.memoryTypeCount; i++ )
        {
            if ( !( bits & ( 1u << i ) ) )
                continue;
            if ( !require || ( mp.memoryTypes[i].propertyFlags & props ) == props )
                return i;
        }
        return UINT32_MAX;
    }

    static bool ff_gob_init( FfGob &g, int physical_w, int physical_h, int block_height_log2 )
    {
        VkDevice dev = g_device.device();
        uint32_t qfam = g_device.queueFamily();

        g.nbuf = 3;

        VkCommandPoolCreateInfo cp{};
        cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = qfam;
        if ( g_device.vk.CreateCommandPool( dev, &cp, nullptr, &g.pool ) != VK_SUCCESS )
        { ff_log( "CreateCommandPool failed" ); return false; }

        VkDescriptorSetLayoutBinding binds[2]{};
        binds[0].binding = 0;
        binds[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        binds[0].descriptorCount = 1;
        binds[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        binds[1].binding = 1;
        binds[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binds[1].descriptorCount = 1;
        binds[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo dl{};
        dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dl.bindingCount = 2;
        dl.pBindings = binds;
        if ( g_device.vk.CreateDescriptorSetLayout( dev, &dl, nullptr, &g.dsl ) != VK_SUCCESS )
        { ff_log( "CreateDescriptorSetLayout failed" ); return false; }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = 32;
        VkPipelineLayoutCreateInfo pl{};
        pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &g.dsl;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &pcr;
        if ( g_device.vk.CreatePipelineLayout( dev, &pl, nullptr, &g.pl ) != VK_SUCCESS )
        { ff_log( "CreatePipelineLayout failed" ); return false; }

        VkShaderModuleCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = gob_swizzle_spv_size;
        sm.pCode = gob_swizzle_spv;
        VkShaderModule mod = VK_NULL_HANDLE;
        if ( g_device.vk.CreateShaderModule( dev, &sm, nullptr, &mod ) != VK_SUCCESS )
        { ff_log( "CreateShaderModule(gob) failed" ); return false; }

        VkComputePipelineCreateInfo cpc{};
        cpc.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpc.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpc.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpc.stage.module = mod;
        cpc.stage.pName = "main";
        cpc.layout = g.pl;
        VkResult pr = g_device.vk.CreateComputePipelines( dev, VK_NULL_HANDLE, 1, &cpc, nullptr, &g.pipe );
        auto pfnDestroyShaderModule = (PFN_vkDestroyShaderModule)
            g_device.vk.GetDeviceProcAddr( dev, "vkDestroyShaderModule" );
        if ( pfnDestroyShaderModule ) pfnDestroyShaderModule( dev, mod, nullptr );
        if ( pr != VK_SUCCESS )
        { ff_log( "CreateComputePipelines(gob) failed: %d", pr ); return false; }

        VkDescriptorPoolSize ps[2]{};
        ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ps[0].descriptorCount = (uint32_t)g.nbuf;
        ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ps[1].descriptorCount = (uint32_t)g.nbuf;
        VkDescriptorPoolCreateInfo dp{};
        dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dp.maxSets = (uint32_t)g.nbuf;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = ps;
        if ( g_device.vk.CreateDescriptorPool( dev, &dp, nullptr, &g.dpool ) != VK_SUCCESS )
        { ff_log( "CreateDescriptorPool failed" ); return false; }

        size_t sz = gob_padded_size( (uint32_t)physical_w, (uint32_t)physical_h, block_height_log2 );
        ff_log( "gob scanout: %d buffers, physical %dx%d, GOB pitch=%u alloc=%zu (+25%%)",
                g.nbuf, physical_w, physical_h, gob_pitch( (uint32_t)physical_w ), sz );

        for ( int i = 0; i < g.nbuf; i++ )
        {
            VkExternalMemoryBufferCreateInfo ext{};
            ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
            ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkBufferCreateInfo bc{};
            bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bc.pNext = &ext;
            bc.size = sz;
            bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if ( g_device.vk.CreateBuffer( dev, &bc, nullptr, &g.out[i].buf ) != VK_SUCCESS )
            { ff_log( "CreateBuffer[%d] failed", i ); return false; }

            VkMemoryRequirements mr;
            g_device.vk.GetBufferMemoryRequirements( dev, g.out[i].buf, &mr );
            uint32_t mt = ff_find_memtype( mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true );
            if ( mt == UINT32_MAX )
                mt = ff_find_memtype( mr.memoryTypeBits, (VkMemoryPropertyFlags)0, false );
            VkExportMemoryAllocateInfo ex{};
            ex.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
            ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            VkMemoryAllocateInfo ma{};
            ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            ma.pNext = &ex;
            ma.allocationSize = mr.size > sz ? mr.size : sz;
            ma.memoryTypeIndex = mt;
            if ( g_device.vk.AllocateMemory( dev, &ma, nullptr, &g.out[i].mem ) != VK_SUCCESS ||
                 g_device.vk.BindBufferMemory( dev, g.out[i].buf, g.out[i].mem, 0 ) != VK_SUCCESS )
            { ff_log( "scanout[%d] memory alloc/bind failed", i ); return false; }

            VkMemoryGetFdInfoKHR gfi{};
            gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            gfi.memory = g.out[i].mem;
            gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            if ( g_device.vk.GetMemoryFdKHR( dev, &gfi, &g.out[i].fd ) != VK_SUCCESS || g.out[i].fd < 0 )
            { ff_log( "GetMemoryFdKHR[%d] failed", i ); return false; }

            VkDescriptorSetAllocateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ds.descriptorPool = g.dpool;
            ds.descriptorSetCount = 1;
            ds.pSetLayouts = &g.dsl;
            if ( g_device.vk.AllocateDescriptorSets( dev, &ds, &g.out[i].set ) != VK_SUCCESS )
            { ff_log( "AllocateDescriptorSets(gob %d) failed", i ); return false; }

            VkDescriptorBufferInfo dbi{};
            dbi.buffer = g.out[i].buf;
            dbi.offset = 0;
            dbi.range = VK_WHOLE_SIZE;
            VkWriteDescriptorSet w{};
            w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w.dstSet = g.out[i].set;
            w.dstBinding = 1;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &dbi;
            g_device.vk.UpdateDescriptorSets( dev, 1, &w, 0, nullptr );

            VkFenceCreateInfo fc{};
            fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            if ( g_device.vk.CreateFence( dev, &fc, nullptr, &g.out[i].fence ) != VK_SUCCESS )
            { ff_log( "CreateFence[%d] failed", i ); return false; }

            VkCommandBufferAllocateInfo ca{};
            ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ca.commandPool = g.pool;
            ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ca.commandBufferCount = 1;
            if ( g_device.vk.AllocateCommandBuffers( dev, &ca, &g.out[i].cmd ) != VK_SUCCESS )
            { ff_log( "AllocateCommandBuffers[%d] failed", i ); return false; }

            ff_log( "scanout[%d]: fd=%d mreq=%u", i, g.out[i].fd, (unsigned)mr.size );
        }
        return true;
    }

    // Create a fresh rgba8 2D view over the composite output image so the
    // GOB shader's `rgba8` qualifier matches.
    static VkImageView ff_make_rgba8_view( CVulkanTexture *pTex )
    {
        VkImageViewCreateInfo iv{};
        iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        iv.image = pTex->vkImage();
        iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
        iv.format = VK_FORMAT_R8G8B8A8_UNORM;
        iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        iv.subresourceRange.levelCount = 1;
        iv.subresourceRange.layerCount = 1;
        VkImageView v = VK_NULL_HANDLE;
        if ( g_device.vk.CreateImageView( g_device.device(), &iv, nullptr, &v ) != VK_SUCCESS )
            return VK_NULL_HANDLE;
        return v;
    }

    // Record one GOB conversion of the composite output image into scanout
    // slot -> release the buffer to EXTERNAL. need_acquire takes the buffer
    // back from EXTERNAL before any write (own-fence retirement already
    // observed by the caller).
    static bool ff_gob_record( FfGob &g, int slot, CVulkanTexture *pComposite,
                               VkImageView compositeView, int physical_w, int physical_h,
                               int block_height_log2, int orientation,
                               int logical_w, int logical_h, bool need_acquire )
    {
        VkDevice dev = g_device.device();
        uint32_t qfam = g_device.queueFamily();
        VkCommandBuffer cb = g.out[slot].cmd;

        if ( g_device.vk.ResetFences( dev, 1, &g.out[slot].fence ) != VK_SUCCESS )
        { ff_log( "ResetFences[%d] failed", slot ); return false; }
        if ( g_device.vk.ResetCommandBuffer( cb, 0 ) != VK_SUCCESS )
        { ff_log( "ResetCommandBuffer[%d] failed", slot ); return false; }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ( g_device.vk.BeginCommandBuffer( cb, &bi ) != VK_SUCCESS )
        { ff_log( "BeginCommandBuffer[%d] failed", slot ); return false; }

        if ( need_acquire )
        {
            VkBufferMemoryBarrier ab{};
            ab.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            ab.srcAccessMask = 0;
            ab.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            ab.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            ab.dstQueueFamilyIndex = qfam;
            ab.buffer = g.out[slot].buf;
            ab.offset = 0;
            ab.size = VK_WHOLE_SIZE;
            g_device.vk.CmdPipelineBarrier( cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                            0, nullptr, 1, &ab, 0, nullptr );
        }

        // composite-write -> GOB-read barrier (GENERAL stays GENERAL).
        VkImageMemoryBarrier ib{};
        ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        ib.image = pComposite->vkImage();
        ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ib.subresourceRange.levelCount = 1;
        ib.subresourceRange.layerCount = 1;
        ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        g_device.vk.CmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                        0, nullptr, 0, nullptr, 1, &ib );

        VkDescriptorImageInfo dii{};
        dii.imageView = compositeView;
        dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = g.out[slot].set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &dii;
        g_device.vk.UpdateDescriptorSets( dev, 1, &w, 0, nullptr );

        g_device.vk.CmdBindPipeline( cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pipe );
        g_device.vk.CmdBindDescriptorSets( cb, VK_PIPELINE_BIND_POINT_COMPUTE, g.pl,
                                           0, 1, &g.out[slot].set, 0, nullptr );
        uint32_t pc[8] = {
            (uint32_t)physical_w, (uint32_t)physical_h,
            (uint32_t)block_height_log2, gob_gobs_wide( (uint32_t)physical_w ),
            (uint32_t)orientation, (uint32_t)logical_w, (uint32_t)logical_h, 0u
        };
        g_device.vk.CmdPushConstants( cb, g.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, pc );
        g_device.vk.CmdDispatch( cb,
                                 ( (uint32_t)physical_w + 15u ) / 16u,
                                 ( (uint32_t)physical_h + 7u ) / 8u, 1 );

        // Release the scanout buffer to the external consumer (the DC).
        VkBufferMemoryBarrier bb{};
        bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bb.dstAccessMask = 0;
        bb.srcQueueFamilyIndex = qfam;
        bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        bb.buffer = g.out[slot].buf;
        bb.offset = 0;
        bb.size = VK_WHOLE_SIZE;
        g_device.vk.CmdPipelineBarrier( cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                        VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                                        0, nullptr, 1, &bb, 0, nullptr );

        if ( g_device.vk.EndCommandBuffer( cb ) != VK_SUCCESS )
        { ff_log( "EndCommandBuffer[%d] failed", slot ); return false; }

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if ( g_device.vk.QueueSubmit( g_device.queue(), 1, &si, g.out[slot].fence ) != VK_SUCCESS )
        { ff_log( "QueueSubmit(gob %d) failed", slot ); return false; }
        return true;
    }

    static bool ff_gob_wait( FfGob &g, int slot, uint64_t timeout_ns )
    {
        VkResult r = g_device.vk.WaitForFences( g_device.device(), 1, &g.out[slot].fence, VK_TRUE, timeout_ns );
        return r == VK_SUCCESS;
    }

    static void ff_gob_destroy( FfGob &g, bool safe )
    {
        VkDevice dev = g_device.device();
        if ( !safe )
        {
            // Pending GPU work may still reference these; leave them for
            // process teardown (do NOT free possibly-in-use resources).
            ff_log( "gob destroy: pending work; skipping Vulkan frees" );
            return;
        }
        auto pfnDestroyCommandPool = (PFN_vkDestroyCommandPool)
            g_device.vk.GetDeviceProcAddr( dev, "vkDestroyCommandPool" );
        auto pfnDestroyFence = (PFN_vkDestroyFence)
            g_device.vk.GetDeviceProcAddr( dev, "vkDestroyFence" );
        for ( int i = 0; i < g.nbuf; i++ )
        {
            if ( g.out[i].fd >= 0 ) { close( g.out[i].fd ); g.out[i].fd = -1; }
            if ( g.out[i].fence && pfnDestroyFence ) pfnDestroyFence( dev, g.out[i].fence, nullptr );
            if ( g.out[i].buf ) g_device.vk.DestroyBuffer( dev, g.out[i].buf, nullptr );
            if ( g.out[i].mem ) g_device.vk.FreeMemory( dev, g.out[i].mem, nullptr );
        }
        if ( g.pipe ) g_device.vk.DestroyPipeline( dev, g.pipe, nullptr );
        if ( g.pl ) g_device.vk.DestroyPipelineLayout( dev, g.pl, nullptr );
        if ( g.dpool ) g_device.vk.DestroyDescriptorPool( dev, g.dpool, nullptr );
        if ( g.dsl ) g_device.vk.DestroyDescriptorSetLayout( dev, g.dsl, nullptr );
        if ( g.pool && pfnDestroyCommandPool ) pfnDestroyCommandPool( dev, g.pool, nullptr );
    }

    // =====================================================================
    // First-frame orchestration
    // =====================================================================
    bool TegraL4TRunFirstFrame( const TegraFirstFrameConfig &cfg )
    {
        if ( !IsTegraL4TMode() )
        {
            ff_log( "not Tegra L4T mode; refusing" );
            return false;
        }

        ff_log( "==== first-frame begin (M7c) ====" );

        currentOutputWidth = (uint32_t)cfg.logical_w;
        currentOutputHeight = (uint32_t)cfg.logical_h;

        // --- Step 0: compile the ONE pipeline the real composite needs,
        // synchronously, before any DC/GOB work, so a driver-compiler failure is
        // isolated from display bugs. The needed variant is the scoped RGB BLIT:
        // 1 layer, ycbcrMask=0 (Tegra RGB-only profile), 0 blur layers, linear
        // colorspace, Gamma22 output, no ITM, no LUT. (The old V1
        // formerly-failing variant probe was removed: the nearest decision needs
        // only the actual compositor pipeline to compile and render.)
        ff_log( "precompile needed RGB BLIT (layer1 ycbcr0 blur0 cs0 eotf0 itm0 lut0)..." );
        VkPipeline blit = g_device.pipeline( SHADER_TYPE_BLIT, 1u, 0u, 0u,
                                             GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR, EOTF_Gamma22, false, 0u );
        if ( blit == VK_NULL_HANDLE )
        {
            ff_log( "needed RGB BLIT pipeline compile FAILED (returned null)" );
            return false;
        }
        ff_log( "needed RGB BLIT pipeline compiled OK (scoped RGB path: ycbcrMask=0, no YCbCr/LUT)" );

        // --- physical/logical geometry ---
        int physical_w = cfg.physical_w, physical_h = cfg.physical_h;
        if ( physical_w <= 0 || physical_h <= 0 )
        {
            FILE *f = fopen( "/sys/class/graphics/fb0/virtual_size", "r" );
            int w = 0, h = 0;
            if ( f ) { if ( fscanf( f, "%d,%d", &w, &h ) == 2 && w > 0 && h > 0 ) { physical_w = w; physical_h = h; } fclose( f ); }
            if ( physical_w <= 0 || physical_h <= 0 )
                orient_physical_size( cfg.orientation, cfg.logical_w, cfg.logical_h, &physical_w, &physical_h );
            ff_log( "physical panel: %dx%d (orientation %d)", physical_w, physical_h, cfg.orientation );
        }
        int expect_w, expect_h;
        orient_physical_size( cfg.orientation, cfg.logical_w, cfg.logical_h, &expect_w, &expect_h );
        if ( physical_w != expect_w || physical_h != expect_h )
        {
            ff_log( "physical %dx%d does not match logical %dx%d @ orient %d (=> %dx%d)",
                    physical_w, physical_h, cfg.logical_w, cfg.logical_h, cfg.orientation, expect_w, expect_h );
            return false;
        }
        int block_height_log2 = cfg.block_height_log2;

        int frame_count = cfg.frame_count;
        if ( frame_count < 1 ) frame_count = 1;
        if ( frame_count > 4 ) frame_count = 4;

        FfGob gob{};
        FfDc  dc{};
        // True once a GOB submit has been issued or a flip is live. After that
        // point any failure must fail closed (no destructors, no frees).
        bool bAnythingLive = false;
        bool ok = false;
        int  frames_presented = 0;
        VkImageView currentView = VK_NULL_HANDLE;
        CVulkanTexture *currentTex = nullptr;
        gamescope::OwningRc<CVulkanTexture> sceneTex;

        const uint64_t kGobFenceTimeoutNs = 2000000000ull; // 2s

        // --- diagnostic scene (one-time upload; not a per-frame CPU copy) ---
        {
            const uint32_t lw = (uint32_t)cfg.logical_w, lh = (uint32_t)cfg.logical_h;
            std::vector<uint8_t> bits( (size_t)lw * lh * 4u );
            for ( uint32_t y = 0; y < lh; y++ )
            for ( uint32_t x = 0; x < lw; x++ )
            {
                uint8_t r, g, b;
                bool top = y < lh / 2, left = x < lw / 2;
                if ( top && left )       { r = 255; g = 0;   b = 0;   } // TL red
                else if ( top )          { r = 0;   g = 255; b = 0;   } // TR green
                else if ( left )         { r = 0;   g = 0;   b = 255; } // BL blue
                else                     { r = 255; g = 255; b = 255; } // BR white
                if ( x < 16 || x >= lw - 16 || y < 16 || y >= lh - 16 )
                    { r = 0; g = 255; b = 255; }                        // border cyan
                uint8_t *p = &bits[ ( (size_t)y * lw + x ) * 4u ];
                p[0] = r; p[1] = g; p[2] = b; p[3] = 255;               // DRM_FORMAT_ABGR8888 byte order
            }
            sceneTex = vulkan_create_texture_from_bits( lw, lh, lw, lh,
                            DRM_FORMAT_ABGR8888, CVulkanTexture::createFlags{}, bits.data() );
            if ( sceneTex == nullptr )
            { ff_log( "diagnostic scene texture creation failed" ); return false; }
        }

        if ( !ff_gob_init( gob, physical_w, physical_h, block_height_log2 ) )
        { ff_log( "gob init failed" ); goto cleanup; }

        // --- DC: open, prior state, enable head if needed, wait ready, claim window ---
        {
            int dcn = cfg.dc;
            if ( dcn < 0 ) { dcn = ff_dc_autodetect(); }
            if ( dcn < 0 ) { ff_log( "no DC head found" ); goto cleanup; }
            if ( ff_dc_open_present( dc, dcn ) != 0 ) goto cleanup;

            struct tegra_dc_ext_status st;
            memset( &st, 0, sizeof( st ) );
            if ( ff_dc_get_status( dc, &st ) != 0 )
            { ff_log( "prior head state unknown; refusing" ); goto cleanup; }
            bool enabled = ( st.flags & TEGRA_DC_EXT_FLAGS_ENABLED ) != 0;
            ff_log( "head state: dc=%d enabled=%d (prior)", dcn, enabled ? 1 : 0 );

            if ( !enabled )
            {
                if ( ff_fb_open( dc ) != 0 ) goto cleanup;
                dc.head_changed = true;
                if ( ff_fb_unblank( dc ) != 0 ) goto cleanup;
                if ( ff_dc_wait_ready( dc, 1000 ) != 0 ) goto cleanup;
            }
            else
            {
                if ( ff_dc_wait_ready( dc, 1000 ) != 0 ) goto cleanup;
            }

            if ( ff_dc_acquire_window( dc, cfg.window ) != 0 ) goto cleanup;
            ff_dc_ctrl_open( dc );
        }

        // --- frame loop: composite -> GOB -> FLIP4 -> (reuse only after own-fence retirement) ---
        for ( int i = 0; i < frame_count; i++ )
        {
            int slot = i % gob.nbuf;

            // Never overwrite a slot whose own post fence has not retired. This
            // is required ONLY for reuse while still presenting (the last frame
            // is never reused, so it is intentionally NOT awaited anywhere).
            if ( gob.out[slot].post_fd >= 0 )
            {
                FfVerdict v = ff_release_poll( gob.out[slot].post_fd, 500 );
                if ( v != FF_REL_RETIRED )
                    ff_fail_closed( dc, "a retained slot's post fence did not retire before reuse" );
                close( gob.out[slot].post_fd );
                gob.out[slot].post_fd = -1;
            }

            // Real composite of the internal scene into the RGBA8 output ring.
            FrameInfo_t fi{};
            {
                FrameInfo_t::Layer_t *pLayer = fi.layers.push();
                if ( pLayer == nullptr )
                { if ( bAnythingLive ) ff_fail_closed( dc, "layer push failed while a frame is live" ); ff_log( "layer push failed" ); goto cleanup; }
                pLayer->tex = sceneTex;
                pLayer->zpos = 0;
                pLayer->offset = { 0.0f, 0.0f };
                pLayer->scale = { 1.0f, 1.0f };
                pLayer->opacity = 1.0f;
                pLayer->filter = GamescopeUpscaleFilter::LINEAR;
                pLayer->blackBorder = false;
                pLayer->applyColorMgmt = false;
                pLayer->eAlphaBlendingMode = ALPHA_BLENDING_MODE_PREMULTIPLIED;
                pLayer->colorspace = GAMESCOPE_APP_TEXTURE_COLORSPACE_LINEAR;
            }
            fi.useFSRLayer0 = false;
            fi.useNISLayer0 = false;
            fi.useSGSRLayer0 = false;
            fi.blurLayer0 = BLUR_MODE_OFF;
            fi.outputEncodingEOTF = EOTF_Gamma22;
            fi.applyOutputColorMgmt = false;

            std::optional<uint64_t> seq = vulkan_composite( &fi, nullptr, false, nullptr, true );
            if ( !seq.has_value() )
            { if ( bAnythingLive ) ff_fail_closed( dc, "vulkan_composite failed while a frame is live" ); ff_log( "frame %d: vulkan_composite failed", i ); goto cleanup; }
            vulkan_wait( *seq, true );

            currentTex = vulkan_get_last_output_image( false, false ).get();
            if ( currentTex == nullptr )
            { if ( bAnythingLive ) ff_fail_closed( dc, "no composite output image while a frame is live" ); ff_log( "frame %d: no output image", i ); goto cleanup; }

            // M7c2 (item 3): the GOB conversion and the composite shader's
            // `rgba8` output qualifier require an RGBA8 output. Enforce the
            // contract explicitly instead of relying on it implicitly.
            if ( currentTex->format() != VK_FORMAT_R8G8B8A8_UNORM )
            { if ( bAnythingLive ) ff_fail_closed( dc, "composite output is not R8G8B8A8_UNORM" );
              ff_log( "frame %d: composite output format not R8G8B8A8_UNORM; rgba8 contract unmet", i ); goto cleanup; }

            if ( currentView != VK_NULL_HANDLE )
            { g_device.vk.DestroyImageView( g_device.device(), currentView, nullptr ); currentView = VK_NULL_HANDLE; }
            currentView = ff_make_rgba8_view( currentTex );
            if ( currentView == VK_NULL_HANDLE )
            { if ( bAnythingLive ) ff_fail_closed( dc, "rgba8 view creation failed while a frame is live" ); ff_log( "frame %d: rgba8 view creation failed", i ); goto cleanup; }

            bool need_acquire = gob.out[slot].ever_flipped;
            if ( !ff_gob_record( gob, slot, currentTex, currentView, physical_w, physical_h,
                                 block_height_log2, cfg.orientation, cfg.logical_w, cfg.logical_h, need_acquire ) )
            {
                // Submission status is unknown/partial: never run destructors or
                // free resources that a (possibly) submitted GOB job references.
                ff_fail_closed( dc, "GOB record/submit status unknown" );
            }
            bAnythingLive = true;

            if ( !ff_gob_wait( gob, slot, kGobFenceTimeoutNs ) )
            {
                ff_fail_closed( dc, "GOB fence timeout/error (submission status unknown)" );
            }
            ff_log( "frame %d: composite seq=%" PRIu64 " GOB fence signaled", i, (uint64_t)*seq );

            struct tegra_dc_ext_flip_windowattr win;
            memset( &win, 0, sizeof( win ) );
            win.index = (__s32)dc.win_index;
            win.buff_id = (__u32)gob.out[slot].fd;
            win.blend = TEGRA_DC_EXT_BLEND_NONE;
            win.offset = 0;
            win.stride = gob_pitch( (uint32_t)physical_w );
            win.pixformat = TEGRA_DC_EXT_FMT_T_A8R8G8B8;
            win.x = 0; win.y = 0;
            win.w = (__u32)( (uint32_t)physical_w << 12 );
            win.h = (__u32)( (uint32_t)physical_h << 12 );
            win.out_x = 0; win.out_y = 0;
            win.out_w = (__u32)physical_w;
            win.out_h = (__u32)physical_h;
            win.z = 255;
            win.swap_interval = 1;
            win.pre_syncpt_id = 0xFFFFFFFFu;
            win.flags |= TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR;
            win.block_height_log2 = (__u8)block_height_log2;
            win.global_alpha = 255;

            int pf = -1;
            if ( ff_dc_flip4( dc, &win, &pf ) != 0 )
            {
                // GPU work completed, but the FLIP4 outcome is unknown; a flip
                // from an earlier frame may be live. Fail closed.
                ff_fail_closed( dc, "FLIP4 failed" );
            }
            gob.out[slot].post_fd = pf;
            gob.out[slot].ever_flipped = true;
            frames_presented++;
            ff_log( "frame %d: FLIP4 buff_fd=%d stride=%u post_syncpt_fd=%d", i, gob.out[slot].fd, win.stride, pf );
        }

        // --- no drain: the LAST current frame has no replacement, so its own
        // post fence cannot retire while it is still being presented. Insisting
        // on it before closing the DC would deadlock (its retirement is required
        // ONLY for slot reuse while still presenting, handled above). Follow the
        // probe's proven teardown instead (close DC while owning first).
        ok = ( frames_presented > 0 );

    cleanup:
        // Teardown order (adapted from tools/tegra-present-probe):
        //  1. close the DC fd WHILE STILL OWNING the window (no PUT_WINDOW) so the
        //     kernel's flush/blank/unpin runs,
        //  2. if this run changed disabled->enabled, restore the prior fb0 blank
        //     level (fb0 stays open for this),
        //  3. close fb0,
        //  4. release post-flip FDs (never wait for a successor),
        //  5. free scanout Vulkan resources ONLY if the DC close was confirmed; a
        //     failed/unknown close may leave memory still being scanned out.
        bool dc_closed_ok = ff_dc_close( dc );
        if ( dc.head_changed )
        {
            if ( ff_fb_restore( dc ) != 0 )
                dc_closed_ok = false;
        }
        ff_fb_close( dc );

        if ( !dc_closed_ok )
        {
            ff_log( "FATAL: DC close/head restoration unconfirmed; exit 3 without destructors or Vulkan frees" );
            _exit( 3 );
        }

        for ( int s = 0; s < gob.nbuf; s++ )
            if ( gob.out[s].post_fd >= 0 ) { close( gob.out[s].post_fd ); gob.out[s].post_fd = -1; }
        if ( currentView != VK_NULL_HANDLE )
            g_device.vk.DestroyImageView( g_device.device(), currentView, nullptr );

        ff_gob_destroy( gob, dc_closed_ok );

        ff_log( "==== first-frame %s (frames=%d) ====", ok ? "COMPLETE" : "FAILED", frames_presented );
        return ok;
    }
}
