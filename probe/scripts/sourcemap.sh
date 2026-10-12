#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
cd "$(dirname "$0")/.."
echo "== LINE COUNTS =="
wc -l src/*.c src/*.h shaders/*.comp tests/*.c Makefile CMakeLists.txt | sort -n
echo
echo "== ANCHORS (file:line) =="
grep -n "int  tp_gpu_create_scanout\|^int tp_gpu_create_scanout\|VkExportMemoryAllocateInfo\|g_GetMemoryFdKHR(g->device\|vkCmdPipelineBarrier\|int tp_gpu_render_frame\|vkQueueSubmit\|int tp_dc_flip4\|TEGRA_DC_EXT_FLIP4\|void tp_dc_close\|close(d->fd)\|int tp_dc_open_present\|TEGRA_DC_EXT_GET_WINDOW\|uint32_t tp_gob_swizzle\|size_t   tp_gob_exact_size\|size_t tp_gob_exact_size" src/*.c
echo
echo "== SPV SIZES =="
ls -l src/gen/*.spv 2>/dev/null
echo
echo "== SOURCE SHA256 =="
sha256sum src/*.c src/*.h shaders/*.comp tests/*.c third_party/PROVENANCE.md Makefile CMakeLists.txt
