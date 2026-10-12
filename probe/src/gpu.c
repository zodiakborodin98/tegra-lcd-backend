/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#include "gob_swizzle_spv.h"
#include "pattern_spv.h"
#include "pacing.h"   /* tp_ts_masked_delta (pure, host-tested) */

/* vkGetMemoryFdKHR is an extension entry point the loader does not export as a
 * linkable symbol (see reports/M2a-result.json); resolve it via the device. */
static PFN_vkGetMemoryFdKHR g_GetMemoryFdKHR;

void tp_gpu_zero(tp_gpu *g)
{
    memset(g, 0, sizeof(*g));
    for (int i = 0; i < TP_MAX_SCANOUT_BUFFERS; i++)
        g->out[i].fd = -1;
}

static uint32_t find_memtype(VkPhysicalDevice pd, uint32_t bits,
                             VkMemoryPropertyFlags props, int require_props)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i)))
            continue;
        if (!require_props || (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    }
    return UINT32_MAX;
}

static int have_device_ext(VkPhysicalDevice pd, const char *name)
{
    uint32_t n = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL) != VK_SUCCESS)
        return 0;
    VkExtensionProperties *eps = calloc(n ? n : 1, sizeof(*eps));
    if (!eps)
        return 0;
    int found = 0;
    if (vkEnumerateDeviceExtensionProperties(pd, NULL, &n, eps) == VK_SUCCESS) {
        for (uint32_t i = 0; i < n; i++)
            if (strcmp(eps[i].extensionName, name) == 0) {
                found = 1;
                break;
            }
    }
    free(eps);
    return found;
}

/* The scanout buffer is exported as OPAQUE_FD and sized with a +25% safety
 * margin, so it must be exportable AND must not be dedicated-only. Query the
 * exact STORAGE_BUFFER + OPAQUE_FD support and reject otherwise. */
static int check_external_buffer_support(VkPhysicalDevice pd)
{
    VkPhysicalDeviceExternalBufferInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO;
    bi.flags = 0;
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

    VkExternalBufferProperties ebp;
    memset(&ebp, 0, sizeof(ebp));
    ebp.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES;
    vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, &ebp);

    VkExternalMemoryFeatureFlags f =
        ebp.externalMemoryProperties.externalMemoryFeatures;
    if (!(f & VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT)) {
        tp_log("external STORAGE_BUFFER+OPAQUE_FD not EXPORTABLE (features=0x%x)",
               (unsigned)f);
        return -1;
    }
    if (f & VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT) {
        tp_log("external STORAGE_BUFFER+OPAQUE_FD is DEDICATED_ONLY; rejecting "
               "(we need a non-dedicated +25%% allocation)");
        return -1;
    }
    tp_vlog("external STORAGE_BUFFER+OPAQUE_FD supported (features=0x%x, "
            "compatible=0x%x)", (unsigned)f,
            (unsigned)ebp.externalMemoryProperties.compatibleHandleTypes);
    return 0;
}

int tp_gpu_init(tp_gpu *g, const tp_config *c)
{
    VkApplicationInfo ai;
    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.pApplicationName = "tegra-present-probe";
    ai.pEngineName = "tegra-present-probe";
    ai.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo ici;
    memset(&ici, 0, sizeof(ici));
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &ai;
    VkResult r = vkCreateInstance(&ici, NULL, &g->instance);
    if (r != VK_SUCCESS) {
        tp_log("vkCreateInstance failed: %d", r);
        return -1;
    }

    uint32_t n = 0;
    VkResult er = vkEnumeratePhysicalDevices(g->instance, &n, NULL);
    if (er != VK_SUCCESS || n == 0) {
        tp_log("vkEnumeratePhysicalDevices(count) failed: %d (n=%u)", er, n);
        return -1;
    }
    VkPhysicalDevice pds[8];
    uint32_t ncap = (n > 8) ? 8 : n;
    er = vkEnumeratePhysicalDevices(g->instance, &ncap, pds);
    if (er != VK_SUCCESS || ncap == 0) {
        tp_log("vkEnumeratePhysicalDevices(list) failed: %d (n=%u)", er, ncap);
        return -1;
    }
    g->phys = pds[0];

    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(g->phys, &pp);
    tp_log("GPU: %s (api %u.%u.%u driver %u)", pp.deviceName,
           VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion),
           VK_VERSION_PATCH(pp.apiVersion), pp.driverVersion);

    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &qn, NULL);
    if (qn == 0) {
        tp_log("no Vulkan queue families");
        return -1;
    }
    VkQueueFamilyProperties qps[16];
    if (qn > 16)
        qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &qn, qps);
    /* The probe submits only COMPUTE work: the selected family must expose
     * VK_QUEUE_COMPUTE_BIT (GRAPHICS-only families are rejected). */
    int qf = -1;
    for (uint32_t i = 0; i < qn; i++)
        if (qps[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            qf = (int)i;
            break;
        }
    if (qf < 0) {
        tp_log("no queue family with VK_QUEUE_COMPUTE_BIT");
        return -1;
    }
    g->qfam = (uint32_t)qf;
    tp_vlog("queue family %u: flags=0x%x queues=%u", g->qfam,
            qps[qf].queueFlags, qps[qf].queueCount);

    if (!have_device_ext(g->phys, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME)) {
        tp_log("device lacks %s", VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME);
        return -1;
    }
    if (check_external_buffer_support(g->phys) != 0)
        return -1;

    const char *dexts[] = { VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME };
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci;
    memset(&qci, 0, sizeof(qci));
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = g->qfam;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;

    VkPhysicalDeviceFeatures feats;
    memset(&feats, 0, sizeof(feats));
    VkDeviceCreateInfo dci;
    memset(&dci, 0, sizeof(dci));
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = dexts;
    dci.pEnabledFeatures = &feats;
    r = vkCreateDevice(g->phys, &dci, NULL, &g->device);
    if (r != VK_SUCCESS) {
        tp_log("vkCreateDevice failed: %d", r);
        return -1;
    }
    vkGetDeviceQueue(g->device, g->qfam, 0, &g->queue);

    g_GetMemoryFdKHR = (PFN_vkGetMemoryFdKHR)
        vkGetDeviceProcAddr(g->device, "vkGetMemoryFdKHR");
    if (!g_GetMemoryFdKHR) {
        tp_log("vkGetMemoryFdKHR unavailable via vkGetDeviceProcAddr");
        return -1;
    }

    /* Optional GPU timing (M3i): only the opt-in --reuse path adds GPU queries;
     * the accepted write-once path stays query-free. The persistent timestamp
     * query pool is created only when the selected compute queue family reports
     * timestampValidBits > 0 and the device has a positive timestampPeriod.
     * A non-DEVICE_LOST failure is non-fatal (GPU timing reported unavailable,
     * CPU-only timing). VK_ERROR_DEVICE_LOST is NEVER swallowed: it marks the
     * run unsafe and aborts GPU init (fail closed). */
    if (!c->reuse) {
        tp_vlog("GPU timestamps: not set up (write-once path adds no GPU queries)");
    } else {
        uint32_t tsvb = qps[qf].timestampValidBits;
        float tsperiod = pp.limits.timestampPeriod;
        if (tsvb > 0 && tsperiod > 0.0f) {
            VkQueryPoolCreateInfo qpc;
            memset(&qpc, 0, sizeof(qpc));
            qpc.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
            qpc.queryType = VK_QUERY_TYPE_TIMESTAMP;
            qpc.queryCount = (uint32_t)TP_MAX_SCANOUT_BUFFERS * 2u;
            VkResult qr = vkCreateQueryPool(g->device, &qpc, NULL, &g->ts_pool);
            if (qr == VK_SUCCESS) {
                g->ts_valid_bits = tsvb;
                g->ts_period_ns = (double)tsperiod;
                g->ts_enabled = 1;
                tp_log("GPU timestamps: timestampValidBits=%u period=%.4f ns/tick",
                       tsvb, (double)tsperiod);
            } else if (qr == VK_ERROR_DEVICE_LOST) {
                g->ts_pool = VK_NULL_HANDLE;
                g->unsafe = 1;
                tp_log("vkCreateQueryPool DEVICE_LOST -> unsafe; aborting GPU init");
                return -1;
            } else {
                g->ts_pool = VK_NULL_HANDLE;
                tp_log("GPU timestamps unavailable (query pool creation failed: %d); "
                       "CPU timing only", qr);
            }
        } else {
            tp_log("GPU timestamps unavailable (timestampValidBits=%u period=%.4f); "
                   "CPU timing only", tsvb, (double)tsperiod);
        }
    }

    VkCommandPoolCreateInfo cp;
    memset(&cp, 0, sizeof(cp));
    cp.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cp.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cp.queueFamilyIndex = g->qfam;
    if (vkCreateCommandPool(g->device, &cp, NULL, &g->cpool) != VK_SUCCESS) {
        tp_log("vkCreateCommandPool failed");
        return -1;
    }

    g->src_format = VK_FORMAT_R8G8B8A8_UNORM;
    return 0;
}

static int make_compute_pipeline(tp_gpu *g, const uint32_t *code, size_t len,
                                 VkPipelineLayout layout, VkPipeline *out)
{
    VkShaderModuleCreateInfo sm;
    memset(&sm, 0, sizeof(sm));
    sm.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    sm.codeSize = len;
    sm.pCode = code;
    VkShaderModule m;
    VkResult r = vkCreateShaderModule(g->device, &sm, NULL, &m);
    if (r != VK_SUCCESS) {
        tp_log("vkCreateShaderModule failed: %d", r);
        return -1;
    }
    VkComputePipelineCreateInfo cp;
    memset(&cp, 0, sizeof(cp));
    cp.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cp.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cp.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cp.stage.module = m;
    cp.stage.pName = "main";
    cp.layout = layout;
    r = vkCreateComputePipelines(g->device, VK_NULL_HANDLE, 1, &cp, NULL, out);
    vkDestroyShaderModule(g->device, m, NULL);
    if (r != VK_SUCCESS) {
        tp_log("vkCreateComputePipelines failed: %d", r);
        return -1;
    }
    return 0;
}

int tp_gpu_create_pipelines(tp_gpu *g)
{
    VkDescriptorSetLayoutBinding b0;
    memset(&b0, 0, sizeof(b0));
    b0.binding = 0;
    b0.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    b0.descriptorCount = 1;
    b0.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dl;
    memset(&dl, 0, sizeof(dl));
    dl.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dl.bindingCount = 1;
    dl.pBindings = &b0;
    if (vkCreateDescriptorSetLayout(g->device, &dl, NULL, &g->dsl_pattern) != VK_SUCCESS)
        return -1;

    VkDescriptorSetLayoutBinding b1[2];
    memset(b1, 0, sizeof(b1));
    b1[0] = b0;
    b1[1].binding = 1;
    b1[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b1[1].descriptorCount = 1;
    b1[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    dl.bindingCount = 2;
    dl.pBindings = b1;
    if (vkCreateDescriptorSetLayout(g->device, &dl, NULL, &g->dsl_gob) != VK_SUCCESS)
        return -1;

    VkPushConstantRange pcp = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 16 }; /* w,h,frame,pad */
    VkPipelineLayoutCreateInfo pl;
    memset(&pl, 0, sizeof(pl));
    pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &g->dsl_pattern;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcp;
    if (vkCreatePipelineLayout(g->device, &pl, NULL, &g->pl_pattern) != VK_SUCCESS)
        return -1;

    VkPushConstantRange pcg = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 32 }; /* 8 uints */
    pl.pSetLayouts = &g->dsl_gob;
    pl.pPushConstantRanges = &pcg;
    if (vkCreatePipelineLayout(g->device, &pl, NULL, &g->pl_gob) != VK_SUCCESS)
        return -1;

    if (make_compute_pipeline(g, gob_swizzle_spv, gob_swizzle_spv_size,
                              g->pl_gob, &g->pipe_gob) != 0)
        return -1;
    if (make_compute_pipeline(g, pattern_spv, pattern_spv_size,
                              g->pl_pattern, &g->pipe_pattern) != 0)
        return -1;
    return 0;
}

int tp_gpu_create_pattern_image(tp_gpu *g, const tp_config *c)
{
    VkImageCreateInfo ic;
    memset(&ic, 0, sizeof(ic));
    ic.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ic.imageType = VK_IMAGE_TYPE_2D;
    ic.format = g->src_format;
    ic.extent.width = (uint32_t)c->logical_w;
    ic.extent.height = (uint32_t)c->logical_h;
    ic.extent.depth = 1;
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(g->device, &ic, NULL, &g->img) != VK_SUCCESS) {
        tp_log("vkCreateImage (pattern) failed");
        return -1;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(g->device, g->img, &mr);
    uint32_t mt = find_memtype(g->phys, mr.memoryTypeBits,
                               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
    if (mt == UINT32_MAX)
        mt = find_memtype(g->phys, mr.memoryTypeBits, 0, 0);
    VkMemoryAllocateInfo ma;
    memset(&ma, 0, sizeof(ma));
    ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex = mt;
    if (vkAllocateMemory(g->device, &ma, NULL, &g->img_mem) != VK_SUCCESS ||
        vkBindImageMemory(g->device, g->img, g->img_mem, 0) != VK_SUCCESS) {
        tp_log("pattern image memory alloc/bind failed");
        return -1;
    }
    VkImageViewCreateInfo iv;
    memset(&iv, 0, sizeof(iv));
    iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image = g->img;
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = g->src_format;
    iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    iv.subresourceRange.levelCount = 1;
    iv.subresourceRange.layerCount = 1;
    if (vkCreateImageView(g->device, &iv, NULL, &g->img_view) != VK_SUCCESS) {
        tp_log("vkCreateImageView failed");
        return -1;
    }
    tp_log("pattern storage image: %dx%d fmt=%d (%u bytes DEVICE_LOCAL)",
           c->logical_w, c->logical_h, g->src_format, (unsigned)mr.size);
    return 0;
}

int tp_gpu_create_scanout(tp_gpu *g, const tp_config *c)
{
    /* Reuse mode always allocates EXACTLY TP_REUSE_SLOTS persistent scanout
     * buffers regardless of --frame-count (which bounds work, not buffers).
     * The default write-once path is unchanged: min(frame_count, 4). */
    int n = c->reuse ? TP_REUSE_SLOTS : c->frame_count;
    if (n > TP_MAX_SCANOUT_BUFFERS)
        n = TP_MAX_SCANOUT_BUFFERS;
    if (n < 1) {
        tp_log("frame_count must be >= 1");
        return -1;
    }
    g->nbuf = n;

    VkDescriptorPoolSize ps[2];
    ps[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    ps[0].descriptorCount = 1u + (uint32_t)n;
    ps[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    ps[1].descriptorCount = (uint32_t)n;
    VkDescriptorPoolCreateInfo dp;
    memset(&dp, 0, sizeof(dp));
    dp.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dp.maxSets = 1u + (uint32_t)n;
    dp.poolSizeCount = 2;
    dp.pPoolSizes = ps;
    if (vkCreateDescriptorPool(g->device, &dp, NULL, &g->dpool) != VK_SUCCESS) {
        tp_log("vkCreateDescriptorPool failed");
        return -1;
    }

    /* pattern image descriptor */
    VkDescriptorSetAllocateInfo da;
    memset(&da, 0, sizeof(da));
    da.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    da.descriptorPool = g->dpool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &g->dsl_pattern;
    if (vkAllocateDescriptorSets(g->device, &da, &g->img_set) != VK_SUCCESS) {
        tp_log("AllocateDescriptorSets (pattern) failed");
        return -1;
    }
    VkDescriptorImageInfo dii;
    memset(&dii, 0, sizeof(dii));
    dii.imageView = g->img_view;
    dii.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkWriteDescriptorSet w;
    memset(&w, 0, sizeof(w));
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = g->img_set;
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &dii;
    vkUpdateDescriptorSets(g->device, 1, &w, 0, NULL);

    size_t exact = tp_gob_exact_size((uint32_t)c->physical_w,
                                     (uint32_t)c->physical_h, c->block_height_log2);
    size_t sz = tp_gob_padded_size((uint32_t)c->physical_w,
                                   (uint32_t)c->physical_h, c->block_height_log2);
    tp_log("scanout: %d buffers, physical %dx%d, GOB pitch=%u exact=%zu alloc=%zu (+25%%)",
           n, c->physical_w, c->physical_h,
           tp_gob_pitch((uint32_t)c->physical_w), exact, sz);

    for (int i = 0; i < n; i++) {
        VkExternalMemoryBufferCreateInfo ext;
        memset(&ext, 0, sizeof(ext));
        ext.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO;
        ext.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;

        VkBufferCreateInfo bc;
        memset(&bc, 0, sizeof(bc));
        bc.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bc.pNext = &ext;
        bc.size = sz;
        bc.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(g->device, &bc, NULL, &g->out[i].buf) != VK_SUCCESS) {
            tp_log("vkCreateBuffer[%d] failed", i);
            return -1;
        }
        VkMemoryRequirements mr;
        vkGetBufferMemoryRequirements(g->device, g->out[i].buf, &mr);
        uint32_t mt = find_memtype(g->phys, mr.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 1);
        if (mt == UINT32_MAX)
            mt = find_memtype(g->phys, mr.memoryTypeBits, 0, 0);

        VkExportMemoryAllocateInfo ex;
        memset(&ex, 0, sizeof(ex));
        ex.sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO;
        ex.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        VkMemoryAllocateInfo ma;
        memset(&ma, 0, sizeof(ma));
        ma.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ma.pNext = &ex;
        ma.allocationSize = mr.size > sz ? mr.size : sz;
        ma.memoryTypeIndex = mt;
        if (vkAllocateMemory(g->device, &ma, NULL, &g->out[i].mem) != VK_SUCCESS ||
            vkBindBufferMemory(g->device, g->out[i].buf, g->out[i].mem, 0) != VK_SUCCESS) {
            tp_log("scanout[%d] memory alloc/bind failed", i);
            return -1;
        }

        VkMemoryGetFdInfoKHR gfi;
        memset(&gfi, 0, sizeof(gfi));
        gfi.sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR;
        gfi.memory = g->out[i].mem;
        gfi.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
        if (g_GetMemoryFdKHR(g->device, &gfi, &g->out[i].fd) != VK_SUCCESS ||
            g->out[i].fd < 0) {
            tp_log("vkGetMemoryFdKHR[%d] failed", i);
            return -1;
        }

        VkDescriptorSetAllocateInfo ds;
        memset(&ds, 0, sizeof(ds));
        ds.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ds.descriptorPool = g->dpool;
        ds.descriptorSetCount = 1;
        ds.pSetLayouts = &g->dsl_gob;
        if (vkAllocateDescriptorSets(g->device, &ds, &g->out[i].set) != VK_SUCCESS) {
            tp_log("AllocateDescriptorSets (gob %d) failed", i);
            return -1;
        }
        VkDescriptorBufferInfo dbi;
        memset(&dbi, 0, sizeof(dbi));
        dbi.buffer = g->out[i].buf;
        dbi.offset = 0;
        dbi.range = VK_WHOLE_SIZE;
        VkWriteDescriptorSet ws[2];
        memset(ws, 0, sizeof(ws));
        ws[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[0].dstSet = g->out[i].set;
        ws[0].dstBinding = 0;
        ws[0].descriptorCount = 1;
        ws[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws[0].pImageInfo = &dii;
        ws[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[1].dstSet = g->out[i].set;
        ws[1].dstBinding = 1;
        ws[1].descriptorCount = 1;
        ws[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[1].pBufferInfo = &dbi;
        vkUpdateDescriptorSets(g->device, 2, ws, 0, NULL);

        VkFenceCreateInfo fc;
        memset(&fc, 0, sizeof(fc));
        fc.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (vkCreateFence(g->device, &fc, NULL, &g->fence[i]) != VK_SUCCESS) {
            tp_log("vkCreateFence[%d] failed", i);
            return -1;
        }
        VkCommandBufferAllocateInfo ca;
        memset(&ca, 0, sizeof(ca));
        ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ca.commandPool = g->cpool;
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ca.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(g->device, &ca, &g->cmd[i]) != VK_SUCCESS) {
            tp_log("AllocateCommandBuffers[%d] failed", i);
            return -1;
        }
        tp_log("scanout[%d]: fd=%d alloc=%u (mreq=%u)", i, g->out[i].fd,
               (unsigned)(mr.size > sz ? mr.size : sz), (unsigned)mr.size);
    }
    return 0;
}

int tp_gpu_render_frame(tp_gpu *g, const tp_config *c, int idx)
{
    /* Default write-once path: each buffer used once, no EXTERNAL acquire. */
    return tp_gpu_render_frame_ex(g, c, idx, idx, 0);
}

int tp_gpu_render_frame_ex(tp_gpu *g, const tp_config *c, int slot,
                           int frame_seq, int need_acquire)
{
    VkCommandBuffer cb = g->cmd[slot];

    /* Checked resets: a failed fence/command-buffer reset leaves GPU state
     * unknown. Fail before any submit and mark the run unsafe (fail closed;
     * no unbounded wait and no stale fence reused). */
    VkResult rf = vkResetFences(g->device, 1, &g->fence[slot]);
    if (rf != VK_SUCCESS) {
        tp_log("vkResetFences slot %d (frame %d) failed: %d -> unsafe%s",
               slot, frame_seq, rf,
               (rf == VK_ERROR_DEVICE_LOST) ? " (DEVICE_LOST)" : "");
        g->unsafe = 1;
        return -1;
    }
    VkResult rr = vkResetCommandBuffer(cb, 0);
    if (rr != VK_SUCCESS) {
        tp_log("vkResetCommandBuffer slot %d (frame %d) failed: %d -> unsafe%s",
               slot, frame_seq, rr,
               (rr == VK_ERROR_DEVICE_LOST) ? " (DEVICE_LOST)" : "");
        g->unsafe = 1;   /* fences were reset but this slot is not re-armed */
        return -1;
    }

    VkCommandBufferBeginInfo bi;
    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VkResult br = vkBeginCommandBuffer(cb, &bi);
    if (br != VK_SUCCESS) {
        tp_log("vkBeginCommandBuffer slot %d (frame %d) failed: %d -> unsafe%s",
               slot, frame_seq, br,
               (br == VK_ERROR_DEVICE_LOST) ? " (DEVICE_LOST)" : "");
        g->unsafe = 1;
        return -1;
    }

    /* Reset this slot's two timestamp queries at the head of its own command
     * buffer. The slot is never re-submitted before its previous fence retired,
     * so no in-flight query range is reset. */
    if (g->ts_enabled)
        vkCmdResetQueryPool(cb, g->ts_pool, (uint32_t)slot * 2u, 2u);

    /* Reuse acquire: the slot was released to VK_QUEUE_FAMILY_EXTERNAL when it
     * was last submitted; take ownership back BEFORE any GPU write. Full-range
     * buffer barrier EXTERNAL -> compute qfam, srcAccess 0, dstAccess
     * SHADER_WRITE, TOP_OF_PIPE -> COMPUTE. The first local use of a slot
     * (never released) needs no acquire. */
    if (need_acquire) {
        VkBufferMemoryBarrier ab;
        memset(&ab, 0, sizeof(ab));
        ab.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        ab.srcAccessMask = 0;
        ab.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        ab.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        ab.dstQueueFamilyIndex = g->qfam;
        ab.buffer = g->out[slot].buf;
        ab.offset = 0;
        ab.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                             0, NULL, 1, &ab, 0, NULL);
    }

    /* Pattern image: UNDEFINED -> GENERAL (contents fully rewritten below). */
    VkImageMemoryBarrier ib;
    memset(&ib, 0, sizeof(ib));
    ib.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ib.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ib.image = g->img;
    ib.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ib.subresourceRange.levelCount = 1;
    ib.subresourceRange.layerCount = 1;
    ib.srcAccessMask = 0;
    ib.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &ib);

    /* 1) generate the pattern into the logical storage image (GPU only). */
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipe_pattern);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pl_pattern,
                            0, 1, &g->img_set, 0, NULL);
    uint32_t pp[4] = { (uint32_t)c->logical_w, (uint32_t)c->logical_h,
                       (uint32_t)frame_seq,
                       (uint32_t)(c->reuse ? 1u : 0u) }; /* pad: reuse flag */
    vkCmdPushConstants(cb, g->pl_pattern, VK_SHADER_STAGE_COMPUTE_BIT, 0, 16, pp);
    vkCmdDispatch(cb, ((uint32_t)c->logical_w + 15u) / 16u,
                  ((uint32_t)c->logical_h + 7u) / 8u, 1);

    /* 2) barrier: pattern writes visible to the GOB read (COMPUTE->COMPUTE;
     *    never a TRANSFER stage). Layout stays GENERAL. */
    ib.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ib.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ib.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                         0, NULL, 0, NULL, 1, &ib);

    /* 3) reference GOB compute: convert (with rotation) into the exportable
     *    block-linear storage buffer. */
    if (g->ts_enabled)   /* start of GOB: after the pattern barrier completes */
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            g->ts_pool, (uint32_t)slot * 2u);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipe_gob);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g->pl_gob,
                            0, 1, &g->out[slot].set, 0, NULL);
    uint32_t gp[8] = {
        (uint32_t)c->physical_w, (uint32_t)c->physical_h,
        (uint32_t)c->block_height_log2,
        tp_gob_gobs_wide((uint32_t)c->physical_w),
        (uint32_t)c->orientation,
        (uint32_t)c->logical_w, (uint32_t)c->logical_h, 0
    };
    vkCmdPushConstants(cb, g->pl_gob, VK_SHADER_STAGE_COMPUTE_BIT, 0, 32, gp);
    vkCmdDispatch(cb, ((uint32_t)c->physical_w + 15u) / 16u,
                  ((uint32_t)c->physical_h + 7u) / 8u, 1);
    if (g->ts_enabled)   /* end of GOB dispatch */
        vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            g->ts_pool, (uint32_t)slot * 2u + 1u);

    /* 4) release the scanout-buffer writes to the external consumer (the DC)
     *    with a full-range buffer barrier: this frame's own output buffer,
     *    srcQueueFamilyIndex = the compute queue family, dst =
     *    VK_QUEUE_FAMILY_EXTERNAL, SHADER_WRITE -> 0, COMPUTE ->
     *    BOTTOM_OF_PIPE. The default path never reuses a buffer; reuse mode
     *    re-acquires this same slot before its next GPU write. This is a
     *    same-NVIDIA OPAQUE_FD interoperability assumption to validate on
     *    hardware; it does NOT claim a VK_KHR_external_memory FOREIGN handle. */
    VkBufferMemoryBarrier bb;
    memset(&bb, 0, sizeof(bb));
    bb.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bb.dstAccessMask = 0;
    bb.srcQueueFamilyIndex = g->qfam;
    bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    bb.buffer = g->out[slot].buf;
    bb.offset = 0;
    bb.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0,
                         0, NULL, 1, &bb, 0, NULL);

    VkResult er = vkEndCommandBuffer(cb);
    if (er != VK_SUCCESS) {
        tp_log("vkEndCommandBuffer slot %d (frame %d) failed: %d -> unsafe%s",
               slot, frame_seq, er,
               (er == VK_ERROR_DEVICE_LOST) ? " (DEVICE_LOST)" : "");
        g->unsafe = 1;
        return -1;
    }

    VkSubmitInfo si;
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    VkResult r = vkQueueSubmit(g->queue, 1, &si, g->fence[slot]);
    if (r != VK_SUCCESS) {
        tp_log("vkQueueSubmit slot %d (frame %d) failed: %d%s", slot, frame_seq,
               r, (r == VK_ERROR_DEVICE_LOST) ? " (DEVICE_LOST: unsafe)" : "");
        g->unsafe = 1;
        return -1;
    }
    g->fence_armed[slot] = 1;
    g->fence_done[slot] = 0;
    return 0;
}

int tp_gpu_wait_frame(tp_gpu *g, int idx, uint64_t timeout_ns)
{
    VkResult r = vkWaitForFences(g->device, 1, &g->fence[idx], VK_TRUE, timeout_ns);
    if (r == VK_SUCCESS) {
        g->fence_done[idx] = 1;
        return 0;
    }
    if (r == VK_TIMEOUT) {
        tp_log("vkWaitForFences frame %d TIMEOUT (%llu ns) -> unsafe", idx,
               (unsigned long long)timeout_ns);
        g->unsafe = 1;
        return 1;
    }
    tp_log("vkWaitForFences frame %d failed: %d -> unsafe", idx, r);
    g->unsafe = 1;
    return -1;
}

int tp_gpu_timestamp_available(const tp_gpu *g)
{
    if (!g || !g->ts_enabled || g->ts_pool == VK_NULL_HANDLE)
        return 0;
    return (int)g->ts_valid_bits;
}

int tp_gpu_timestamp_read(tp_gpu *g, int slot, uint64_t *out_ns)
{
    if (!g || !g->ts_enabled || g->ts_pool == VK_NULL_HANDLE)
        return 1;
    if (slot < 0 || slot >= TP_MAX_SCANOUT_BUFFERS)
        return -1;

    uint64_t q[2] = { 0, 0 };
    /* No VK_QUERY_RESULT_WAIT_BIT: this must never block. The caller only calls
     * this after the slot's GPU fence has already been observed signaled. */
    VkResult r = vkGetQueryPoolResults(g->device, g->ts_pool,
                                       (uint32_t)slot * 2u, 2u,
                                       sizeof(q), q, sizeof(uint64_t),
                                       VK_QUERY_RESULT_64_BIT);
    if (r == VK_NOT_READY)
        return 1;   /* not ready: optional metric skipped (never blocks) */
    if (r != VK_SUCCESS) {
        if (r == VK_ERROR_DEVICE_LOST) {
            /* Never ignored: this call is made only after the slot's GPU fence
             * already signalled, so DEVICE_LOST is a real loss. Mark unsafe; the
             * caller must abort without FLIP4 or submitting another frame. */
            tp_log("GPU timestamp read slot %d: DEVICE_LOST -> unsafe; aborting",
                   slot);
            g->unsafe = 1;
            return -1;
        }
        tp_log("GPU timestamp read slot %d failed: %d (optional; metric skipped)",
               slot, r);
        return 1;   /* other optional-query failure: skip metric, non-fatal */
    }

    /* Timestamps occupy only ts_valid_bits bits and may wrap there. Mask to the
     * valid bits and take the UNSIGNED difference: (b - a) mod 2^vb is correct
     * for every width, including the full 64-bit counter (the previous b<a
     * branch reported a fake 0 there instead of the wrapped span). */
    uint32_t vb = g->ts_valid_bits;
    uint64_t mask = (vb >= 64u) ? ~(uint64_t)0 : ((1ULL << vb) - 1ULL);
    uint64_t a = q[0] & mask;
    uint64_t b = q[1] & mask;
    uint64_t dt = tp_ts_masked_delta(a, b, vb);
    if (out_ns)
        *out_ns = (uint64_t)((double)dt * g->ts_period_ns);
    return 0;
}

int tp_gpu_quiesce(tp_gpu *g, uint64_t timeout_ns)
{
    if (g->device == VK_NULL_HANDLE)
        return 0; /* instance-without-device: nothing submitted */
    if (g->unsafe) {
        /* A prior wait timed out/errored or submit reported device-loss: do
         * NOT wait a second time on any fence. */
        tp_log("quiesce: GPU state already unsafe; not waiting again");
        return -1;
    }
    for (int i = 0; i < TP_MAX_SCANOUT_BUFFERS; i++) {
        if (!g->fence_armed[i] || g->fence_done[i])
            continue;
        VkResult r = vkWaitForFences(g->device, 1, &g->fence[i], VK_TRUE,
                                     timeout_ns);
        if (r == VK_SUCCESS) {
            g->fence_done[i] = 1;
            continue;
        }
        tp_log("quiesce: frame %d fence %s: %d -> unsafe (no retry)", i,
               (r == VK_TIMEOUT) ? "TIMEOUT" : "failed", r);
        g->unsafe = 1;
        return -1; /* no second wait after an error */
    }
    return 0; /* every armed fence confirmed complete */
}

void tp_gpu_destroy(tp_gpu *g, int safe)
{
    /* Instance created but no device (vkCreateDevice failed or init bailed
     * early): only the instance exists; destroy it cleanly. */
    if (g->device == VK_NULL_HANDLE) {
        if (g->instance)
            vkDestroyInstance(g->instance, NULL);
        return;
    }
    /* Submitted-but-unconfirmed GPU work: do NOT destroy Vulkan resources the
     * GPU may still reference (no unbounded vkDeviceWaitIdle here). Leave them
     * for process teardown; the caller closed the DC and exits nonzero. */
    if (!safe) {
        tp_log("destroy: pending GPU work; skipping Vulkan destroys (process teardown)");
        return;
    }

    for (int i = 0; i < TP_MAX_SCANOUT_BUFFERS; i++) {
        if (g->out[i].fd >= 0)
            close(g->out[i].fd);
        if (g->fence[i])
            vkDestroyFence(g->device, g->fence[i], NULL);
        if (g->out[i].buf)
            vkDestroyBuffer(g->device, g->out[i].buf, NULL);
        if (g->out[i].mem)
            vkFreeMemory(g->device, g->out[i].mem, NULL);
    }
    if (g->pipe_pattern)
        vkDestroyPipeline(g->device, g->pipe_pattern, NULL);
    if (g->pipe_gob)
        vkDestroyPipeline(g->device, g->pipe_gob, NULL);
    if (g->pl_pattern)
        vkDestroyPipelineLayout(g->device, g->pl_pattern, NULL);
    if (g->pl_gob)
        vkDestroyPipelineLayout(g->device, g->pl_gob, NULL);
    if (g->dpool)
        vkDestroyDescriptorPool(g->device, g->dpool, NULL);
    if (g->dsl_pattern)
        vkDestroyDescriptorSetLayout(g->device, g->dsl_pattern, NULL);
    if (g->dsl_gob)
        vkDestroyDescriptorSetLayout(g->device, g->dsl_gob, NULL);
    if (g->img_view)
        vkDestroyImageView(g->device, g->img_view, NULL);
    if (g->img)
        vkDestroyImage(g->device, g->img, NULL);
    if (g->img_mem)
        vkFreeMemory(g->device, g->img_mem, NULL);
    if (g->cpool)
        vkDestroyCommandPool(g->device, g->cpool, NULL);
    if (g->ts_pool)
        vkDestroyQueryPool(g->device, g->ts_pool, NULL);
    vkDestroyDevice(g->device, NULL);
    if (g->instance)
        vkDestroyInstance(g->instance, NULL);
}
