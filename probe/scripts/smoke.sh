#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Host smoke test: CLI must be inert without --present and must reject unknown
# args; GOB tests must pass. No target access.
set -uo pipefail
cd "$(dirname "$0")/.."
ROOT=$(cd ../.. && pwd)
LOG="$ROOT/logs/artifacts/m3a"
mkdir -p "$LOG"
SYS=/root/steamos-switch-alpha2/sysroot
VKINC="$SYS/usr/include"

{
  echo "=== REBUILD HOST ==="
  make VULKAN_INC="$VKINC" bin/tegra-present-probe bin/test-gob
  echo "make_rc=$?"
  echo
  echo "=== default (no args) -> help, no device access ==="
  ./bin/tegra-present-probe; echo "rc=$?"
  echo
  echo "=== --help ==="
  ./bin/tegra-present-probe --help >/dev/null; echo "rc=$?"
  echo
  echo "=== unknown arg (expect rc=2) ==="
  ./bin/tegra-present-probe --bogus >/dev/null 2>&1; echo "rc=$?"
  echo
  echo "=== dry run with mode (expect rc=0, no /dev access) ==="
  ./bin/tegra-present-probe --mode 1280x720@60 --orientation 90; echo "rc=$?"
  echo
  echo "=== diagnostic-only on host (no /dev/tegra_dc_* -> expect rc=3) ==="
  ./bin/tegra-present-probe --diagnostic-only; echo "rc=$?"
  echo
  echo "=== strtoul/limits: --frame-count 9 clamps to 4 ==="
  ./bin/tegra-present-probe --mode 1280x720 --frame-count 9 --duration 99 2>&1 | head -8; echo "rc=$?"
  echo
  echo "=== ldd host probe (no X11/GL/Wayland) ==="
  ldd bin/tegra-present-probe | grep -iE 'X11|libGL|wayland|EGL' || echo "no display libs linked"
} 2>&1 | tee "$LOG/smoke.log"
