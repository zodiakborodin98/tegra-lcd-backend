/* SPDX-License-Identifier: GPL-2.0-or-later
 * Minimal Vulkan device layer for the probe: one compute pipeline writes a
 * GPU-only test pattern into a storage image, a second (reference GOB) compute
 * pipeline converts it into persistent, exportable STORAGE_BUFFER memory that
 * FLIP4 can scan out. No CPU framebuffer copy; no VkImage OPTIMAL export as
 * scanout. Only COMPUTE_SHADER / ALL_COMMANDS barriers are used.
 */
#ifndef TP_GPU_H
#define TP_GPU_H

#include "tp.h"
#include <vulkan/vulkan.h>

typedef struct {
    VkInstance       instance;
    VkPhysicalDevice phys;
    VkDevice         device;
    VkQueue          queue;
    uint32_t         qfam;
    VkCommandPool    cpool;

    VkFormat         src_format;

    VkImage          img;
    VkDeviceMemory   img_mem;
    VkImageView      img_view;

    VkDescriptorSetLayout dsl_pattern;
    VkDescriptorSetLayout dsl_gob;
    VkPipelineLayout      pl_pattern;
    VkPipelineLayout      pl_gob;
    VkPipeline            pipe_pattern;
    VkPipeline            pipe_gob;
    VkDescriptorPool      dpool;
    VkDescriptorSet       img_set;

    int nbuf;
    struct {
        VkBuffer       buf;
        VkDeviceMemory mem;
        VkDescriptorSet set;
        int            fd;          /* exported OPAQUE_FD (dma-buf) */
    } out[TP_MAX_SCANOUT_BUFFERS];
    VkFence         fence[TP_MAX_SCANOUT_BUFFERS];
    VkCommandBuffer cmd[TP_MAX_SCANOUT_BUFFERS];

    /* Optional GPU timing (M3i): a persistent timestamp query pool with 2
     * queries per scanout slot, created only when the selected queue family
     * reports timestampValidBits > 0 and timestampPeriod > 0. Results are read
     * WITHOUT VK_QUERY_RESULT_WAIT_BIT, never with an unbounded wait. */
    VkQueryPool     ts_pool;
    uint32_t        ts_valid_bits;   /* 0 => GPU timestamps unavailable      */
    double          ts_period_ns;    /* ns per timestamp tick                */
    int             ts_enabled;

    /* Submitted-fence tracking: a fence is "armed" once vkQueueSubmit has
     * been issued for the frame and "done" once it has been observed
     * signaled. Destroy only touches fully-completed resources. */
    uint8_t         fence_armed[TP_MAX_SCANOUT_BUFFERS];
    uint8_t         fence_done[TP_MAX_SCANOUT_BUFFERS];
    /* Set once any GPU fence wait times out/errors or a vkQueueSubmit reports
     * device-loss. Once unsafe, quiesce never waits again ("no second wait
     * after error") and destroy must skip Vulkan teardown. */
    int             unsafe;
} tp_gpu;

void tp_gpu_zero(tp_gpu *g);
int  tp_gpu_init(tp_gpu *g, const tp_config *c);
int  tp_gpu_create_pipelines(tp_gpu *g);
int  tp_gpu_create_pattern_image(tp_gpu *g, const tp_config *c);
int  tp_gpu_create_scanout(tp_gpu *g, const tp_config *c);
int  tp_gpu_render_frame(tp_gpu *g, const tp_config *c, int idx);
/* Reuse-mode variant: renders into slot `slot` with the monotonic `frame_seq`
 * used for the pattern colour marker so a recycled buffer visibly changes.
 * need_acquire != 0 records an EXTERNAL->compute ownership acquire before any
 * GPU write (a previously released buffer); the first local use passes 0. */
int  tp_gpu_render_frame_ex(tp_gpu *g, const tp_config *c, int slot,
                            int frame_seq, int need_acquire);
int  tp_gpu_wait_frame(tp_gpu *g, int idx, uint64_t timeout_ns);
/* Bounded wait for every armed-but-unconfirmed fence. Returns 0 when all
 * submitted work is complete, -1 if any is still pending (timeout/device
 * loss). Never calls vkDeviceWaitIdle. */
int  tp_gpu_quiesce(tp_gpu *g, uint64_t timeout_ns);

/* GPU GOB-compute timing (M3i). available() reports whether the timestamp
 * query pool exists. read() must be called ONLY after the slot's GPU fence has
 * already been observed signaled; it reads the two timestamps around the GOB
 * dispatch WITHOUT WAIT_BIT. Returns 0 and stores the delta in ns; 1 when the
 * pool is unavailable or the results are not yet ready; -1 on a real error. */
int  tp_gpu_timestamp_available(const tp_gpu *g);
int  tp_gpu_timestamp_read(tp_gpu *g, int slot, uint64_t *out_ns);
/* safe != 0: destroy all Vulkan resources (all submitted work confirmed
 * complete). safe == 0: leave pending GPU resources for process teardown. */
void tp_gpu_destroy(tp_gpu *g, int safe);

#endif /* TP_GPU_H */
