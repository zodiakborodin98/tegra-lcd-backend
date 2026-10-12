// SPDX-License-Identifier: GPL-2.0-or-later
//
// M7f - persistent Tegra L4T presenter (see tegra_l4t_presenter.h).
//
// The helper algorithms below (GOB block-linear math, DC open/GET_WINDOW/head
// enable/FLIP4/close-while-owning, kernel fence-status retirement and the GOB
// Vulkan conversion on the existing Gamescope VkDevice) are reused from
// tegra_l4t_first_frame.cpp (proven M7c3/M7d) / tools/tegra-present-probe.
// They are kept behaviourally identical here; this translation unit only adds
// the persistent, multi-frame driver around them.

#include "Backends/tegra_l4t/tegra_l4t_presenter.h"

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
#include <optional>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

#include <linux/sync_file.h>

#ifndef FBIOBLANK
#define FBIOBLANK 0x4611 /* _IO('F', 0x11) */
#endif

namespace gamescope
{
    // =====================================================================
    // logging
    // =====================================================================
    static void pl_log( const char *pFmt, ... )
    {
        va_list args;
        va_start( args, pFmt );
        fprintf( stderr, "[tegra-l4t-pres] " );
        vfprintf( stderr, pFmt, args );
        fprintf( stderr, "\n" );
        va_end( args );
        fflush( stderr );
    }

    // =====================================================================
    // GOB block-linear math (same as tegra_l4t_first_frame.cpp)
    // =====================================================================
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
    // kernel fence-status retirement (same as tegra_l4t_first_frame.cpp)
    // =====================================================================
    enum PlVerdict { PL_REL_NOT_READY = 0, PL_REL_RETIRED = 1, PL_REL_FAIL = 2 };

    #define PL_SYNC_IOC_MAGIC '>'
    struct pl_sync_pt_info
    {
        uint32_t len;
        char     obj_name[32];
        char     driver_name[32];
        int32_t  status;
        uint64_t timestamp_ns;
    };
    struct pl_sync_fence_info_data
    {
        uint32_t len;
        char     name[32];
        int32_t  status;
    };
    #define PL_SYNC_IOC_FENCE_INFO _IOWR( PL_SYNC_IOC_MAGIC, 2, struct pl_sync_fence_info_data )
    #define PL_LEGACY_INFO_MAX 1024

    static double pl_mono_ms()
    {
        struct timespec ts;
        clock_gettime( CLOCK_MONOTONIC, &ts );
        return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
    }

    static PlVerdict pl_release_poll( int fd, int budget_ms )
    {
        if ( fd < 0 )
            return PL_REL_FAIL;
        if ( budget_ms < 0 )
            budget_ms = 0;

        double deadline = pl_mono_ms() + (double)budget_ms;

        for (;;)
        {
            double remain_d = deadline - pl_mono_ms();
            int remain = ( remain_d > 0.0 ) ? (int)remain_d : 0;
            if ( remain <= 0 )
                return PL_REL_NOT_READY;

            struct pollfd pf;
            pf.fd = fd;
            pf.events = POLLIN;
            pf.revents = 0;
            int r = poll( &pf, 1, remain );
            if ( r < 0 )
            {
                if ( errno == EINTR )
                    continue;
                return PL_REL_FAIL;
            }
            if ( r == 0 )
                return PL_REL_NOT_READY;

            short rev = pf.revents;
            if ( rev & ( POLLERR | POLLHUP | POLLNVAL ) )
                return PL_REL_FAIL;
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
                    return PL_REL_FAIL;
                if ( info.status < 0 )
                    return PL_REL_FAIL;
                if ( info.status == 0 )
                    continue;
                return PL_REL_RETIRED;
            }
            if ( ie != ENOTTY )
                return PL_REL_FAIL;

            unsigned char raw[PL_LEGACY_INFO_MAX];
            memset( raw, 0, sizeof( raw ) );
            ( (struct pl_sync_fence_info_data *)raw )->len = (uint32_t)sizeof( raw );
            errno = 0;
            int lrc = ioctl( fd, PL_SYNC_IOC_FENCE_INFO, raw );
            if ( lrc != 0 )
                return PL_REL_FAIL;

            uint32_t total = 0, plen = 0;
            int32_t agg = 0, pstat = 0;
            memcpy( &total, raw + offsetof( struct pl_sync_fence_info_data, len ), sizeof( total ) );
            memcpy( &agg, raw + offsetof( struct pl_sync_fence_info_data, status ), sizeof( agg ) );
            if ( total < sizeof( struct pl_sync_fence_info_data ) || total > sizeof( raw ) )
                return PL_REL_FAIL;
            if ( agg > 1 )
                return PL_REL_FAIL;
            unsigned off = (unsigned)sizeof( struct pl_sync_fence_info_data );
            unsigned n = 0;
            while ( off < total )
            {
                if ( total - off < sizeof( struct pl_sync_pt_info ) )
                    return PL_REL_FAIL;
                memcpy( &plen, raw + off + offsetof( struct pl_sync_pt_info, len ), sizeof( plen ) );
                memcpy( &pstat, raw + off + offsetof( struct pl_sync_pt_info, status ), sizeof( pstat ) );
                if ( plen < sizeof( struct pl_sync_pt_info ) || plen > total - off )
                    return PL_REL_FAIL;
                if ( pstat < 0 || pstat > 1 )
                    return PL_REL_FAIL;
                if ( agg == 1 && pstat != 1 )
                    return PL_REL_FAIL;
                off += plen;
                n++;
            }
            if ( off != total || n == 0 )
                return PL_REL_FAIL;
            if ( agg == 1 )
                return PL_REL_RETIRED;
            if ( agg < 0 )
                return PL_REL_FAIL;
            continue;
        }
    }

    // =====================================================================
    // DC (same algorithms as tegra_l4t_first_frame.cpp)
    // =====================================================================
    struct PlDc
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

    static int pl_dc_autodetect()
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
            pl_log( "autodetect %s: status=%s flags=0x%08x syncpt=%u delta=%u",
                    path, have_status ? "ok" : "n/a", st.flags, sid, delta );

            if ( enabled && enabled_fallback < 0 )
                enabled_fallback = nn;
            if ( delta > best_delta ) { best_delta = delta; best = nn; }
            close( fd );
        }
        if ( ctrl_fd >= 0 )
            close( ctrl_fd );

        if ( best >= 0 && best_delta > 0 ) { pl_log( "autodetect: head %d (advancing vblank)", best ); return best; }
        if ( enabled_fallback >= 0 )       { pl_log( "autodetect: head %d (enabled fallback)", enabled_fallback ); return enabled_fallback; }
        if ( best >= 0 )                    return best;
        pl_log( "autodetect: no /dev/tegra_dc_* candidate" );
        return -1;
    }

    static int pl_dc_open_present( PlDc &d, int dc_num )
    {
        snprintf( d.path, sizeof( d.path ), "/dev/tegra_dc_%d", dc_num );
        d.fd = open( d.path, O_RDWR | O_CLOEXEC );
        if ( d.fd < 0 ) { pl_log( "open %s failed: %s", d.path, strerror( errno ) ); return -1; }
        d.dc_num = dc_num;
        return 0;
    }

    static int pl_dc_get_status( PlDc &d, struct tegra_dc_ext_status *st )
    {
        return ioctl( d.fd, TEGRA_DC_EXT_GET_STATUS, st ) < 0 ? -1 : 0;
    }

    static int pl_dc_get_vblank_syncpt( PlDc &d, uint32_t *id )
    {
        return ioctl( d.fd, TEGRA_DC_EXT_GET_VBLANK_SYNCPT, id ) < 0 ? -1 : 0;
    }

    static int pl_dc_ctrl_open( PlDc &d )
    {
        if ( d.ctrl_fd >= 0 ) return 0;
        d.ctrl_fd = open( "/dev/nvhost-ctrl", O_RDWR | O_CLOEXEC );
        return d.ctrl_fd < 0 ? -1 : 0;
    }

    static int pl_dc_syncpt_read( PlDc &d, uint32_t id, uint32_t *value )
    {
        struct nvhost_ctrl_syncpt_read_args a;
        memset( &a, 0, sizeof( a ) );
        a.id = id;
        if ( pl_dc_ctrl_open( d ) < 0 ) return -1;
        if ( ioctl( d.ctrl_fd, NVHOST_IOCTL_CTRL_SYNCPT_READ, &a ) < 0 ) return -1;
        *value = a.value;
        return 0;
    }

    static int pl_dc_wait_ready( PlDc &d, int budget_ms )
    {
        if ( !d.have_vblank_syncpt )
        {
            uint32_t sid = 0;
            if ( pl_dc_get_vblank_syncpt( d, &sid ) == 0 ) { d.vblank_syncpt = sid; d.have_vblank_syncpt = true; }
        }
        double t0 = pl_mono_ms();
        bool have = false;
        uint32_t first = 0;
        for (;;)
        {
            struct tegra_dc_ext_status st;
            memset( &st, 0, sizeof( st ) );
            if ( pl_dc_get_status( d, &st ) == 0 &&
                 ( st.flags & TEGRA_DC_EXT_FLAGS_ENABLED ) && d.have_vblank_syncpt )
            {
                uint32_t v = 0;
                if ( pl_dc_syncpt_read( d, d.vblank_syncpt, &v ) == 0 )
                {
                    if ( !have ) { first = v; have = true; }
                    else if ( v != first ) return 0;
                }
            }
            if ( pl_mono_ms() - t0 >= (double)budget_ms )
                break;
            struct timespec ts = { 0, 16000000 };
            nanosleep( &ts, nullptr );
        }
        pl_log( "head not ready within %d ms", budget_ms );
        return -1;
    }

    static int pl_dc_acquire_window( PlDc &d, int win_index )
    {
        if ( ioctl( d.fd, TEGRA_DC_EXT_GET_WINDOW, (unsigned long)win_index ) < 0 )
        {
            int e = errno;
            pl_log( "GET_WINDOW %d on %s failed: %s%s", win_index, d.path, strerror( e ),
                    e == EBUSY ? " (EBUSY: do NOT steal)" : "" );
            return -1;
        }
        d.win_index = win_index;
        d.owns_window = true;
        uint32_t sid = 0;
        if ( pl_dc_get_vblank_syncpt( d, &sid ) == 0 ) { d.vblank_syncpt = sid; d.have_vblank_syncpt = true; }
        pl_log( "acquired window %d on %s, vblank syncpt=%u", win_index, d.path, d.vblank_syncpt );
        return 0;
    }

    static int pl_fb_open( PlDc &d )
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
        if ( d.fb_fd < 0 ) { pl_log( "open /dev/fb0 failed: %s", strerror( errno ) ); return -1; }
        pl_log( "opened /dev/fb0 (prior blank level %s)", d.fb_blank_known ? std::to_string( d.fb_blank_value ).c_str() : "unknown" );
        return 0;
    }

    static int pl_fb_unblank( PlDc &d )
    {
        if ( d.fb_fd < 0 ) return -1;
        int r = ioctl( d.fb_fd, FBIOBLANK, 0 /* FB_BLANK_UNBLANK */ );
        pl_log( "FBIOBLANK(UNBLANK) -> %d (%s)", r, r == 0 ? "ok" : strerror( errno ) );
        return r == 0 ? 0 : -1;
    }

    static int pl_fb_restore( PlDc &d )
    {
        if ( d.fb_fd < 0 ) return -1;
        int level;
        if ( d.fb_blank_known && ( d.fb_blank_value == 2 || d.fb_blank_value == 3 || d.fb_blank_value == 4 ) )
            level = d.fb_blank_value;
        else
            level = 4; /* FB_BLANK_POWERDOWN */
        errno = 0;
        int r = ioctl( d.fb_fd, FBIOBLANK, level );
        pl_log( "FBIOBLANK(%d) restore -> %d (%s)", level, r, r == 0 ? "ok" : strerror( errno ) );
        return r == 0 ? 0 : -1;
    }

    static void pl_fb_close( PlDc &d )
    {
        if ( d.fb_fd >= 0 ) { close( d.fb_fd ); d.fb_fd = -1; }
    }

    // Close the DC fd WHILE STILL OWNING the window (no PUT_WINDOW).
    static bool pl_dc_close( PlDc &d )
    {
        bool ok = true;
        if ( d.ctrl_fd >= 0 ) { close( d.ctrl_fd ); d.ctrl_fd = -1; }
        if ( d.fd >= 0 )
        {
            pl_log( "closing %s %s", d.path, d.owns_window ? "(still owning window, no PUT_WINDOW)" : "(no window claimed)" );
            errno = 0;
            if ( close( d.fd ) != 0 )
            {
                pl_log( "close(%s) FAILED: %s (scanout ownership unknown; NOT freeing scanout memory)",
                        d.path, strerror( errno ) );
                ok = false;
            }
            d.fd = -1;
        }
        return ok;
    }

    [[noreturn]] static void pl_fail_closed( PlDc &dc, const char *pWhy, int code = 3 )
    {
        pl_log( "FATAL: %s; GPU/DC status unknown -> best-effort safe DC close, then immediate exit %d (no destructors, no Vulkan frees)", pWhy, code );
        (void)pl_dc_close( dc );
        if ( dc.head_changed )
            pl_fb_restore( dc );
        pl_fb_close( dc );
        _exit( code );
    }

    static int pl_dc_flip4( PlDc &d, struct tegra_dc_ext_flip_windowattr *win, int *post_fd )
    {
        struct tegra_dc_ext_flip_4 flip;
        memset( &flip, 0, sizeof( flip ) );
        flip.win = (__u64)(uintptr_t)win;
        flip.win_num = 1;
        flip.post_syncpt_fd = -1;
        if ( post_fd ) *post_fd = -1;
        if ( ioctl( d.fd, TEGRA_DC_EXT_FLIP4, &flip ) < 0 )
        {
            pl_log( "FLIP4 on %s failed: %s", d.path, strerror( errno ) );
            return -1;
        }
        if ( post_fd ) *post_fd = flip.post_syncpt_fd;
        return 0;
    }

    // =====================================================================
    // GOB conversion on the existing Gamescope VkDevice/queue
    // =====================================================================
    struct PlScanout
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

    struct PlGob
    {
        VkCommandPool       pool = VK_NULL_HANDLE;
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        VkPipelineLayout    pl = VK_NULL_HANDLE;
        VkPipeline          pipe = VK_NULL_HANDLE;
        VkDescriptorPool    dpool = VK_NULL_HANDLE;
        PlScanout           out[3];
        int                 nbuf = 0;
    };

    static uint32_t pl_find_memtype( uint32_t bits, VkMemoryPropertyFlags props, bool require )
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

    static bool pl_gob_init( PlGob &g, int physical_w, int physical_h, int block_height_log2 )
    {
        VkDevice dev = g_device.device();
        uint32_t qfam = g_device.queueFamily();

        g.nbuf = 3;

        VkCommandPoolCreateInfo cp{};
        cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cp.queueFamilyIndex = qfam;
        if ( g_device.vk.CreateCommandPool( dev, &cp, nullptr, &g.pool ) != VK_SUCCESS )
        { pl_log( "CreateCommandPool failed" ); return false; }

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
        { pl_log( "CreateDescriptorSetLayout failed" ); return false; }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = 32;
        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &g.dsl;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if ( g_device.vk.CreatePipelineLayout( dev, &plci, nullptr, &g.pl ) != VK_SUCCESS )
        { pl_log( "CreatePipelineLayout failed" ); return false; }

        VkShaderModuleCreateInfo sm{};
        sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        sm.codeSize = gob_swizzle_spv_size;
        sm.pCode = gob_swizzle_spv;
        VkShaderModule mod = VK_NULL_HANDLE;
        if ( g_device.vk.CreateShaderModule( dev, &sm, nullptr, &mod ) != VK_SUCCESS )
        { pl_log( "CreateShaderModule(gob) failed" ); return false; }

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
        { pl_log( "CreateComputePipelines(gob) failed: %d", pr ); return false; }

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
        { pl_log( "CreateDescriptorPool failed" ); return false; }

        size_t sz = gob_padded_size( (uint32_t)physical_w, (uint32_t)physical_h, block_height_log2 );
        pl_log( "gob scanout: %d buffers, physical %dx%d, GOB pitch=%u alloc=%zu (+25%%)",
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
            { pl_log( "CreateBuffer[%d] failed", i ); return false; }

            VkMemoryRequirements mr;
            g_device.vk.GetBufferMemoryRequirements( dev, g.out[i].buf, &mr );
            uint32_t mt = pl_find_memtype( mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, true );
            if ( mt == UINT32_MAX )
                mt = pl_find_memtype( mr.memoryTypeBits, (VkMemoryPropertyFlags)0, false );
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
            { pl_log( "scanout[%d] memory alloc/bind failed", i ); return false; }

            VkMemoryGetFdInfoKHR gfi{};
            gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
            gfi.memory = g.out[i].mem;
            gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
            if ( g_device.vk.GetMemoryFdKHR( dev, &gfi, &g.out[i].fd ) != VK_SUCCESS || g.out[i].fd < 0 )
            { pl_log( "GetMemoryFdKHR[%d] failed", i ); return false; }

            VkDescriptorSetAllocateInfo ds{};
            ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            ds.descriptorPool = g.dpool;
            ds.descriptorSetCount = 1;
            ds.pSetLayouts = &g.dsl;
            if ( g_device.vk.AllocateDescriptorSets( dev, &ds, &g.out[i].set ) != VK_SUCCESS )
            { pl_log( "AllocateDescriptorSets(gob %d) failed", i ); return false; }

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
            { pl_log( "CreateFence[%d] failed", i ); return false; }

            VkCommandBufferAllocateInfo ca{};
            ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            ca.commandPool = g.pool;
            ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            ca.commandBufferCount = 1;
            if ( g_device.vk.AllocateCommandBuffers( dev, &ca, &g.out[i].cmd ) != VK_SUCCESS )
            { pl_log( "AllocateCommandBuffers[%d] failed", i ); return false; }

            pl_log( "scanout[%d]: fd=%d mreq=%u", i, g.out[i].fd, (unsigned)mr.size );
        }
        return true;
    }

    static VkImageView pl_make_rgba8_view( CVulkanTexture *pTex )
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

    static bool pl_gob_record( PlGob &g, int slot, CVulkanTexture *pComposite,
                               VkImageView compositeView, int physical_w, int physical_h,
                               int block_height_log2, int orientation,
                               int logical_w, int logical_h, bool need_acquire )
    {
        VkDevice dev = g_device.device();
        uint32_t qfam = g_device.queueFamily();
        VkCommandBuffer cb = g.out[slot].cmd;

        if ( g_device.vk.ResetFences( dev, 1, &g.out[slot].fence ) != VK_SUCCESS )
        { pl_log( "ResetFences[%d] failed", slot ); return false; }
        if ( g_device.vk.ResetCommandBuffer( cb, 0 ) != VK_SUCCESS )
        { pl_log( "ResetCommandBuffer[%d] failed", slot ); return false; }

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if ( g_device.vk.BeginCommandBuffer( cb, &bi ) != VK_SUCCESS )
        { pl_log( "BeginCommandBuffer[%d] failed", slot ); return false; }

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
        { pl_log( "EndCommandBuffer[%d] failed", slot ); return false; }

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        if ( g_device.vk.QueueSubmit( g_device.queue(), 1, &si, g.out[slot].fence ) != VK_SUCCESS )
        { pl_log( "QueueSubmit(gob %d) failed", slot ); return false; }
        return true;
    }

    static bool pl_gob_wait( PlGob &g, int slot, uint64_t timeout_ns )
    {
        VkResult r = g_device.vk.WaitForFences( g_device.device(), 1, &g.out[slot].fence, VK_TRUE, timeout_ns );
        return r == VK_SUCCESS;
    }

    static void pl_gob_destroy( PlGob &g, bool safe )
    {
        VkDevice dev = g_device.device();
        if ( !safe )
        {
            pl_log( "gob destroy: pending work; skipping Vulkan frees" );
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
    // Presenter
    // =====================================================================
    struct CTegraL4TPresenter::Impl
    {
        PlDc   dc{};
        PlGob  gob{};
        int    physical_w = 0;
        int    physical_h = 0;
        int    logical_w = 0;
        int    logical_h = 0;
        int    orientation = 90;
        int    block_height_log2 = 4;
        uint64_t frame_idx = 0;
        bool   gob_live = false;
        bool   dc_live = false;
    };

    CTegraL4TPresenter::~CTegraL4TPresenter()
    {
        Shutdown();
    }

    bool CTegraL4TPresenter::Init( int logical_w, int logical_h, int orientation,
                                   int block_height_log2, int dc, int window, int /*refresh_mhz*/ )
    {
        if ( m_bReady )
            return true;

        if ( logical_w <= 0 || logical_h <= 0 || ( orientation != 0 && orientation != 90 && orientation != 180 && orientation != 270 ) )
        { pl_log( "invalid geometry %dx%d orient %d", logical_w, logical_h, orientation ); return false; }

        Impl *p = new Impl();
        p->logical_w = logical_w;
        p->logical_h = logical_h;
        p->orientation = orientation;
        p->block_height_log2 = block_height_log2;

        int pw = 0, ph = 0;
        FILE *f = fopen( "/sys/class/graphics/fb0/virtual_size", "r" );
        if ( f ) { int w = 0, h = 0; if ( fscanf( f, "%d,%d", &w, &h ) == 2 && w > 0 && h > 0 ) { pw = w; ph = h; } fclose( f ); }
        if ( pw <= 0 || ph <= 0 )
            orient_physical_size( orientation, logical_w, logical_h, &pw, &ph );
        /* M7g orient180h1 (ONE edit): force orientation 180; fb0 physical and the
         * consistency check below stay. Entry runs with -W 720 -H 1280. */
        orientation = 180;
        p->orientation = orientation;
        int ew, eh;
        orient_physical_size( orientation, logical_w, logical_h, &ew, &eh );
        if ( pw != ew || ph != eh )
        {
            pl_log( "physical %dx%d does not match logical %dx%d @ orient %d (=> %dx%d)",
                    pw, ph, logical_w, logical_h, orientation, ew, eh );
            delete p;
            return false;
        }
        p->physical_w = pw;
        p->physical_h = ph;

        if ( !pl_gob_init( p->gob, pw, ph, block_height_log2 ) )
        { delete p; return false; }
        p->gob_live = true;

        int dcn = dc;
        if ( dcn < 0 ) dcn = pl_dc_autodetect();
        if ( dcn < 0 ) { pl_log( "no DC head found" ); pl_gob_destroy( p->gob, true ); delete p; return false; }
        if ( pl_dc_open_present( p->dc, dcn ) != 0 ) { pl_gob_destroy( p->gob, true ); delete p; return false; }
        p->dc_live = true;

        struct tegra_dc_ext_status st;
        memset( &st, 0, sizeof( st ) );
        if ( pl_dc_get_status( p->dc, &st ) != 0 )
        { pl_log( "prior head state unknown; refusing" ); pl_dc_close( p->dc ); pl_fb_close( p->dc ); pl_gob_destroy( p->gob, true ); delete p; return false; }
        bool enabled = ( st.flags & TEGRA_DC_EXT_FLAGS_ENABLED ) != 0;
        pl_log( "head state: dc=%d enabled=%d (prior)", dcn, enabled ? 1 : 0 );

        if ( !enabled )
        {
            if ( pl_fb_open( p->dc ) != 0 ) goto fail;
            p->dc.head_changed = true;
            if ( pl_fb_unblank( p->dc ) != 0 ) goto fail;
            if ( pl_dc_wait_ready( p->dc, 1000 ) != 0 ) goto fail;
        }
        else
        {
            if ( pl_dc_wait_ready( p->dc, 1000 ) != 0 ) goto fail;
        }

        if ( pl_dc_acquire_window( p->dc, window ) != 0 ) goto fail;
        pl_dc_ctrl_open( p->dc );

        m_pImpl = p;
        m_bReady = true;
        pl_log( "presenter ready: physical %dx%d orient %d bhl2=%d dc=%d window=%d", pw, ph, orientation, block_height_log2, dcn, window );
        return true;

    fail:
        pl_dc_close( p->dc );
        if ( p->dc.head_changed ) pl_fb_restore( p->dc );
        pl_fb_close( p->dc );
        pl_gob_destroy( p->gob, true );
        delete p;
        return false;
    }

    int CTegraL4TPresenter::PresentFrame( const FrameInfo_t *pFrameInfo )
    {
        if ( !m_bReady || m_pImpl == nullptr )
        {
            pl_log( "PresentFrame before Init" );
            return -1;
        }
        Impl *p = m_pImpl;

        int slot = (int)( p->frame_idx % (uint64_t)p->gob.nbuf );

        // Never overwrite a slot whose own post fence has not retired.
        if ( p->gob.out[slot].post_fd >= 0 )
        {
            m_uReuseWaits++;
            PlVerdict v = pl_release_poll( p->gob.out[slot].post_fd, 2000 );
            if ( v != PL_REL_RETIRED )
                pl_fail_closed( p->dc, "a retained slot's post fence did not retire before reuse" );
            close( p->gob.out[slot].post_fd );
            p->gob.out[slot].post_fd = -1;
        }

        // Real composite of the compositor's frame into the RGBA8 output ring.
        std::optional<uint64_t> seq = vulkan_composite( (FrameInfo_t *)pFrameInfo, nullptr, false );
        if ( !seq.has_value() )
            pl_fail_closed( p->dc, "vulkan_composite failed while presenting" );
        vulkan_wait( *seq, true );
        m_uComposite++;

        CVulkanTexture *pTex = vulkan_get_last_output_image( false, false ).get();
        if ( pTex == nullptr )
            pl_fail_closed( p->dc, "no composite output image" );
        if ( pTex->format() != VK_FORMAT_R8G8B8A8_UNORM )
            pl_fail_closed( p->dc, "composite output is not R8G8B8A8_UNORM" );

        VkImageView view = pl_make_rgba8_view( pTex );
        if ( view == VK_NULL_HANDLE )
            pl_fail_closed( p->dc, "rgba8 view creation failed" );

        bool need_acquire = p->gob.out[slot].ever_flipped;
        if ( !pl_gob_record( p->gob, slot, pTex, view, p->physical_w, p->physical_h,
                             p->block_height_log2, p->orientation, p->logical_w, p->logical_h, need_acquire ) )
            pl_fail_closed( p->dc, "GOB record/submit status unknown" );

        if ( !pl_gob_wait( p->gob, slot, 2000000000ull ) )
            pl_fail_closed( p->dc, "GOB fence timeout/error (submission status unknown)" );

        g_device.vk.DestroyImageView( g_device.device(), view, nullptr );

        struct tegra_dc_ext_flip_windowattr win;
        memset( &win, 0, sizeof( win ) );
        win.index = (__s32)p->dc.win_index;
        win.buff_id = (__u32)p->gob.out[slot].fd;
        win.blend = TEGRA_DC_EXT_BLEND_NONE;
        win.offset = 0;
        win.stride = gob_pitch( (uint32_t)p->physical_w );
        win.pixformat = TEGRA_DC_EXT_FMT_T_A8R8G8B8;
        win.x = 0; win.y = 0;
        win.w = (__u32)( (uint32_t)p->physical_w << 12 );
        win.h = (__u32)( (uint32_t)p->physical_h << 12 );
        win.out_x = 0; win.out_y = 0;
        win.out_w = (__u32)p->physical_w;
        win.out_h = (__u32)p->physical_h;
        win.z = 255;
        win.swap_interval = 1;
        win.pre_syncpt_id = 0xFFFFFFFFu;
        win.flags |= TEGRA_DC_EXT_FLIP_FLAG_BLOCKLINEAR;
        win.block_height_log2 = (__u8)p->block_height_log2;
        win.global_alpha = 255;

        int pf = -1;
        if ( pl_dc_flip4( p->dc, &win, &pf ) != 0 )
            pl_fail_closed( p->dc, "FLIP4 failed" );

        p->gob.out[slot].post_fd = pf;
        p->gob.out[slot].ever_flipped = true;
        m_uFlip++;
        p->frame_idx++;

        // M7g run #31 fix (L2): frame -> flip linkage. One line per presented
        // (flipped) frame, echoing the layer-0 texture handle + WxH used as the
        // join key against L1 ("compose layer commit ... tex <ptr> WxH"). Log-only.
        {
            const int nLayerCount = pFrameInfo->layers.count();
            CVulkanTexture *pLayer0Tex = nLayerCount > 0 ? pFrameInfo->layers.get( 0 ).tex.get() : nullptr;
            pl_log( "present frame=%" PRIu64 " slot=%d seq=%" PRIu64 " flip=%" PRIu64 " layers=%d tex0=%p %ux%u",
                    p->frame_idx, slot, *seq, m_uFlip, nLayerCount, (void *)pLayer0Tex,
                    pLayer0Tex ? pLayer0Tex->width() : 0u, pLayer0Tex ? pLayer0Tex->height() : 0u );
        }
        return 0;
    }

    bool CTegraL4TPresenter::Shutdown()
    {
        if ( m_pImpl == nullptr )
            return true;
        Impl *p = m_pImpl;
        m_pImpl = nullptr;
        m_bReady = false;

        bool dc_closed_ok = pl_dc_close( p->dc );
        if ( p->dc.head_changed )
        {
            if ( pl_fb_restore( p->dc ) != 0 )
                dc_closed_ok = false;
        }
        pl_fb_close( p->dc );

        for ( int s = 0; s < p->gob.nbuf; s++ )
            if ( p->gob.out[s].post_fd >= 0 ) { close( p->gob.out[s].post_fd ); p->gob.out[s].post_fd = -1; }

        if ( !dc_closed_ok )
        {
            // M7c3 rule: do NOT free anything and do NOT return normally when the
            // DC/scanout state is unconfirmed. Signal the caller to exit nonzero
            // without destructors; intentionally leak the impl/presenter state.
            pl_log( "DC close/head restoration unconfirmed; NOT freeing scanout memory; teardown FAILED" );
            pl_gob_destroy( p->gob, false );
            return false;
        }

        pl_gob_destroy( p->gob, true );
        pl_log( "presenter shutdown: composite=%" PRIu64 " flip=%" PRIu64 " reuse_waits=%" PRIu64,
                m_uComposite, m_uFlip, m_uReuseWaits );
        delete p;
        return true;
    }
}
