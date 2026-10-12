# SPDX-License-Identifier: GPL-2.0-or-later
cmake_minimum_required(VERSION 3.16)
project(tegra-present-probe C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Path to the extracted SteamOS sysroot include dir (Vulkan headers live there).
set(TP_VULKAN_INC "/usr/include"
    CACHE PATH "Vulkan header include dir (from the target sysroot)")

find_program(GLSLANG_VALIDATOR NAMES glslangValidator REQUIRED)
find_program(PYTHON3 NAMES python3 REQUIRED)

set(GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${GEN_DIR})

set(SHADERS gob_swizzle pattern)
set(GEN_HEADERS "")
foreach(sh ${SHADERS})
  set(spv ${GEN_DIR}/${sh}.spv)
  set(hdr ${GEN_DIR}/${sh}_spv.h)
  add_custom_command(
    OUTPUT ${spv}
    COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 -o ${spv}
            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh}.comp
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${sh}.comp
    COMMENT "Compiling ${sh}.comp -> SPIR-V")
  add_custom_command(
    OUTPUT ${hdr}
    COMMAND ${PYTHON3} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/spv2header.py
            ${spv} ${hdr} ${sh}_spv
    DEPENDS ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/scripts/spv2header.py
    COMMENT "Embedding ${sh}.spv -> C header")
  list(APPEND GEN_HEADERS ${hdr})
endforeach()

set(TP_INCLUDES
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${GEN_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tegra
  ${TP_VULKAN_INC})

add_executable(tegra-present-probe
  src/main.c src/gpu.c src/dc.c src/fb_budget.c src/reuse.c src/sync_release.c
  src/gob_math.c src/pacing.c src/procstat.c src/log.c ${GEN_HEADERS})
target_include_directories(tegra-present-probe PRIVATE ${TP_INCLUDES})
target_link_libraries(tegra-present-probe PRIVATE -l:libvulkan.so.1 pthread dl m)

add_executable(test-gob tests/test_gob.c src/gob_math.c src/log.c)
target_include_directories(test-gob PRIVATE ${TP_INCLUDES})
target_link_libraries(test-gob PRIVATE m)

add_executable(test-reuse tests/test_reuse.c src/reuse.c src/sync_release.c src/log.c)
target_include_directories(test-reuse PRIVATE ${TP_INCLUDES})
target_link_libraries(test-reuse PRIVATE m)

# M3i pure pacing/timing maths + /proc resource-sampling tests. No device.
add_executable(test-m3i tests/test_m3i.c src/pacing.c src/procstat.c)
target_include_directories(test-m3i PRIVATE ${TP_INCLUDES})
target_link_libraries(test-m3i PRIVATE m)

# M3l extracted FB-ioctl bookkeeping-budget policy (fake monotonic clock).
add_executable(test-fbbudget tests/test_fbbudget.c src/fb_budget.c src/log.c)
target_include_directories(test-fbbudget PRIVATE ${TP_INCLUDES})
target_link_libraries(test-fbbudget PRIVATE m)

# Host lifecycle/CLI tests: main.c is compiled with -Dmain=tp_real_main and
# linked against the mock gpu/dc stubs (mirrors the Makefile test-probe target;
# no Vulkan/device, no libvulkan). The rename is scoped to main.c via an OBJECT
# library so test_probe.c keeps its own real `main`.
add_library(tp-probe-main OBJECT src/main.c)
target_compile_definitions(tp-probe-main PRIVATE main=tp_real_main)
target_include_directories(tp-probe-main PRIVATE ${TP_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/tests)

add_executable(test-probe
  tests/test_probe.c tests/stub_gpu.c tests/stub_dc.c
  src/fb_budget.c src/reuse.c src/sync_release.c src/gob_math.c src/pacing.c
  src/procstat.c src/log.c
  $<TARGET_OBJECTS:tp-probe-main>)
target_include_directories(test-probe PRIVATE ${TP_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/tests)
target_link_libraries(test-probe PRIVATE m)

enable_testing()
add_test(NAME gob-math COMMAND test-gob)
add_test(NAME reuse-helpers COMMAND test-reuse)
add_test(NAME m3i-helpers COMMAND test-m3i)
add_test(NAME fb-budget COMMAND test-fbbudget)
add_test(NAME probe-lifecycle COMMAND test-probe)
