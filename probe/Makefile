# SPDX-License-Identifier: GPL-2.0-or-later
# Build for the standalone first-frame/fence discovery probe.
#
# Host (x86_64) build (Vulkan headers from the extracted SteamOS sysroot):
#   make
# Target (aarch64) cross build:
#   make CC=aarch64-linux-gnu-gcc SYSROOT=/path/to/target-sysroot \
#        bin/tegra-present-probe
#
# No Xlib/GL/GLX/Wayland libraries are linked; only libvulkan (loader) + libc.

CC        ?= gcc
GLSLANG   ?= glslangValidator
PYTHON    ?= python3

# Extracted SteamOS Alpha1 sysroot (see reports/M2c-*). Holds the Vulkan
# headers (vulkan/vulkan.h) and the aarch64 libvulkan.so.1.
VULKAN_INC ?= /usr/include

SYSROOT ?=
ifneq ($(SYSROOT),)
SYSROOT_FLAG := --sysroot=$(SYSROOT)
endif

CFLAGS   ?= -O2 -g -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers
CPPFLAGS += -Isrc -Isrc/gen -Ithird_party/tegra -I$(VULKAN_INC)
CFLAGS   += $(SYSROOT_FLAG)
LDFLAGS  += $(SYSROOT_FLAG)
LDLIBS    = -l:libvulkan.so.1 -lpthread -ldl -lm

SHADERS  := gob_swizzle pattern
GEN      := $(addprefix src/gen/,$(addsuffix _spv.h,$(SHADERS)))

PROBE_SRC := src/main.c src/gpu.c src/dc.c src/fb_budget.c src/reuse.c src/sync_release.c src/gob_math.c src/pacing.c src/procstat.c src/log.c
TEST_SRC  := tests/test_gob.c src/gob_math.c src/log.c
TEST_PROBE_SRC := tests/test_probe.c tests/stub_gpu.c tests/stub_dc.c src/fb_budget.c src/reuse.c src/sync_release.c src/gob_math.c src/pacing.c src/procstat.c src/log.c
TEST_REUSE_SRC := tests/test_reuse.c src/reuse.c src/sync_release.c src/log.c
TEST_M3I_SRC := tests/test_m3i.c src/pacing.c src/procstat.c
TEST_FBBUDGET_SRC := tests/test_fbbudget.c src/fb_budget.c src/log.c
TEST_PROBE_INC := -Itests

.PHONY: all test clean

all: bin/tegra-present-probe bin/test-gob bin/test-probe bin/test-reuse bin/test-m3i bin/test-fbbudget

src/gen/%_spv.h: shaders/%.comp scripts/spv2header.py
	@mkdir -p src/gen
	$(GLSLANG) -V --target-env vulkan1.1 -o src/gen/$*.spv shaders/$*.comp
	$(PYTHON) scripts/spv2header.py src/gen/$*.spv $@ $*_spv

bin/tegra-present-probe: $(PROBE_SRC) $(GEN)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(PROBE_SRC) $(LDFLAGS) $(LDLIBS)

bin/test-gob: $(TEST_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(TEST_SRC) $(LDFLAGS) -lm

# Host lifecycle/CLI tests: main.c is recompiled with -Dmain=tp_real_main and
# linked against the mock gpu/dc stubs (no Vulkan/device, no libvulkan).
bin/test-probe: $(TEST_PROBE_SRC) src/main.c
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -Dmain=tp_real_main -c src/main.c -o bin/tp_main_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c tests/test_probe.c -o bin/tp_probe_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c tests/stub_gpu.c -o bin/tp_stub_gpu.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c tests/stub_dc.c -o bin/tp_stub_dc.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/fb_budget.c -o bin/tp_fb_budget_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/reuse.c -o bin/tp_reuse_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/sync_release.c -o bin/tp_sync_release_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/gob_math.c -o bin/tp_gob_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/pacing.c -o bin/tp_pacing_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/procstat.c -o bin/tp_procstat_test.o
	$(CC) $(CFLAGS) $(CPPFLAGS) $(TEST_PROBE_INC) -c src/log.c -o bin/tp_log_test.o
	$(CC) -o $@ bin/tp_main_test.o bin/tp_probe_test.o bin/tp_stub_gpu.o bin/tp_stub_dc.o bin/tp_fb_budget_test.o bin/tp_reuse_test.o bin/tp_sync_release_test.o bin/tp_gob_test.o bin/tp_pacing_test.o bin/tp_procstat_test.o bin/tp_log_test.o $(LDFLAGS) -lm

# Extracted FB-ioctl bookkeeping-budget policy, driven by a fake monotonic
# clock. No device, no Vulkan, no real waits.
bin/test-fbbudget: $(TEST_FBBUDGET_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(TEST_FBBUDGET_SRC) $(LDFLAGS) -lm

# Pure reuse-helper tests (state machine + retirement decision). No device.
bin/test-reuse: $(TEST_REUSE_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(TEST_REUSE_SRC) $(LDFLAGS) -lm

# M3i pure pacing/timing maths + /proc resource-sampling tests. No device.
bin/test-m3i: $(TEST_M3I_SRC)
	@mkdir -p bin
	$(CC) $(CFLAGS) $(CPPFLAGS) -o $@ $(TEST_M3I_SRC) $(LDFLAGS) -lm

test: bin/test-gob bin/test-probe bin/test-reuse bin/test-m3i bin/test-fbbudget
	./bin/test-gob
	./bin/test-probe
	./bin/test-reuse
	./bin/test-m3i
	./bin/test-fbbudget

clean:
	rm -rf bin src/gen
