# tegra-present-probe

Standalone **first-frame / fence discovery** probe for the direct Tegra DC
present path (M3a). It is deliberately *not* a presenter: it submits at most
`TP_MAX_SCANOUT_BUFFERS` (4) frames and **never reuses a buffer after its
FLIP4 submission**, so the kernel's post-flip fence semantics can be measured
in isolation before any ring/reuse logic exists.

* No Xlib / GL / GLX / host Wayland / renderer surrogate.
* GPU-only test pattern: a compute shader writes a logical storage image, a
  second compute shader (the pinned reference GOB shader) converts it, with
  rotation, into a persistent **exportable STORAGE_BUFFER**.
* No CPU framebuffer copy, no `VkImage` OPTIMAL export as scanout.
* 8-bit SDR, BGRA/32bpp (`TEGRA_DC_EXT_FMT_T_A8R8G8B8`), GOB block-height log2
  4, pitch `ceil(width*4/64)*64`. No HDR/VRR.

## Safety contract (enforced by the CLI)

* Default invocation = dry run: prints help, touches no display device.
* `--present` is required before any `GET_WINDOW` / `FLIP4`.
* `--diagnostic-only` opens the DC read-only and never `GET_WINDOW`/flips and
  never unblanks.
* `--enable-head` (bounded prototype: only with `--present --dc 0 --window 0`)
  enables an already-configured but disabled DC0 head via **fbdev control, not
  pixels**: `FBIOBLANK(FB_BLANK_UNBLANK)` on `/dev/fb0` (no mmap/read/write, no
  `PAN`, no `PUT_VSCREENINFO`, no mode change; 720x1280@90 preserved). It is
  issued **before** `GET_WINDOW`, gated on `GET_STATUS`+advancing vblank, and the
  prior `/sys/class/graphics/fb0/blank` level is restored on exit. If the head
  is already enabled the state is left alone; if it is disabled and the flag is
  absent the probe fails explicitly instead of flipping into `-ENXIO`.
* `GET_WINDOW` fails cleanly on `EBUSY` (never steals an Xorg plane).
* Teardown stops submissions, waits a finite GPU completion, then closes the
  DC fd **while still owning the window** (no `PUT_WINDOW`) so the kernel
  flush/blank/unpin path runs; exported memory stays alive until that close
  returns. `SIGINT`/`SIGTERM` set a stop flag handled by the main thread.
* Bounds: `--frame-count` <= 4, `--duration` <= 30s (default 5s).

## CLI

```
--present              perform GET_WINDOW + FLIP4 (else dry run)
--diagnostic-only      read-only report; never takes a window, never unblanks
--enable-head          present+dc0+fb0 only: FBIOBLANK(UNBLANK) on /dev/fb0 if the
                       head is disabled (no modeset, no pixels); restores on exit
--dc N                 DC head (default: autodetect the advancing head)
--window N             window index (default 0; the X11 reference used 1)
--mode WxH[@Hz]        logical size (default 1280x720@60)
--physical WxH         physical panel size (default: /sys fb0, else derived)
--orientation 0|90|180|270   clockwise (default 90 for the portrait panel)
--block-height-log2 N  GOB block height log2 (default 4)
--frame-count N        1..4 (default 4)
--duration SEC         <= 30 (default 5)
--verbose
```

The logical 1280x720 image is rotated by the GOB shader (not by the DC) to fit
the 720x1280 panel; the user-facing orientation is configurable and validated
against `logical size + orientation => physical size`.

## Layout

```
src/            tp.h, log.c, gob_math.c, dc.[ch], gpu.[ch], main.c
shaders/        gob_swizzle.comp (adapted), pattern.comp (new)
third_party/    tegra/ (verbatim pinned headers), PROVENANCE.md
tests/          test_gob.c (independent GOB vectors, bounds, rotation)
scripts/        spv2header.py
Makefile, CMakeLists.txt, COPYING
```

## Build

Host (x86_64) build; Vulkan headers come from the extracted SteamOS sysroot:

```
make VULKAN_INC=/path/to/target-sysroot/usr/include
make test
```

aarch64 cross build (never target-native compile):

```
make CC=aarch64-linux-gnu-gcc SYSROOT=/path/to/target-sysroot \
     bin/tegra-present-probe
file bin/tegra-present-probe && readelf -d bin/tegra-present-probe
```

Only `libvulkan.so.1`, libc and libdl are linked — no display libraries.

## Attribution

`shaders/gob_swizzle.comp`, `third_party/tegra/{tegra_dc_ext.h,nvhost_ioctl.h}`
and `COPYING` are copied/adapted from pinned references. See
`third_party/PROVENANCE.md` for exact commits, licenses and what changed.

## Known limits (this milestone)

* Measured behavior only; no assumptions about how many vblanks retire a flip.
* Fence semantics are logged (poll of each retained `post_syncpt_fd`), not
  converted into a reuse ring.
* No HDR/VRR/plane-stealing/DM control; the probe does not own the display
  beyond the window it explicitly claims with `--present`.
