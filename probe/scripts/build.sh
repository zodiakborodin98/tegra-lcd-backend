#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# M3a build driver: host build + GOB tests + aarch64 cross build + ELF checks.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(cd ../.. && pwd)
LOG="$ROOT/logs/artifacts/m3a"
mkdir -p "$LOG"
SYS=/root/steamos-switch-alpha2/sysroot
VKINC="$SYS/usr/include"

{
  echo "=== TOOLCHAIN ==="
  gcc --version | head -1
  aarch64-linux-gnu-gcc --version | head -1
  glslangValidator --version | head -1
  echo
  echo "=== CLEAN ==="
  make clean
  echo
  echo "=== HOST BUILD (x86_64) ==="
  make -j2 VULKAN_INC="$VKINC"
  echo "host_make_rc=$?"
  cp -f bin/tegra-present-probe bin/tegra-present-probe.x86_64 2>/dev/null
  echo
  echo "=== GO TESTS ==="
  make test VULKAN_INC="$VKINC"
  echo "tests_rc=$?"
  echo
  echo "=== HOST FILE ==="
  file bin/tegra-present-probe.x86_64 bin/test-gob
  echo
  echo "=== HOST DYNAMIC NEEDED (must have no X11/GL/GLX/Wayland) ==="
  readelf -d bin/tegra-present-probe.x86_64 | grep -E 'NEEDED|RPATH|RUNPATH' || true
  echo
  echo "=== CROSS BUILD (aarch64) ==="
  rm -f bin/tegra-present-probe
  make CC=aarch64-linux-gnu-gcc SYSROOT="$SYS" bin/tegra-present-probe
  echo "cross_make_rc=$?"
  cp -f bin/tegra-present-probe bin/tegra-present-probe.aarch64 2>/dev/null
  echo
  echo "=== CROSS file ==="
  file bin/tegra-present-probe.aarch64
  echo
  echo "=== CROSS readelf -h ==="
  readelf -h bin/tegra-present-probe.aarch64 | head -18
  echo
  echo "=== CROSS readelf -d ==="
  readelf -d bin/tegra-present-probe.aarch64 | grep -E 'NEEDED|RPATH|RUNPATH' || true
} 2>&1 | tee "$LOG/build.log"

echo "build.log written to $LOG/build.log"
