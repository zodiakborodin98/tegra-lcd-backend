# Third-party provenance -- tegra-present-probe

This tool is a standalone first-frame/fence discovery probe. It validates the
direct Tegra DC presentation path on the target hardware without X11, GL/GLX,
Wayland, or any renderer surrogate. Copied/adapted code and headers keep their
origin and license notices.

## Copied / adapted artifacts

| Local path | Origin | Pin | License |
| --- | --- | --- | --- |
| `third_party/tegra/tegra_dc_ext.h` | `theofficialgman/vk_layer_tegra_dc_present` `tegra_dc_ext.h` | `ecfb76aadbdc93cd7008e77bec944724cd70c364` | GPL-2.0-or-later (NVIDIA kernel uapi, GPLv2) |
| `src/sync_release.h` legacy `tp_sync_pt_info` / `tp_sync_fence_info_data` / `TP_SYNC_IOC_FENCE_INFO` definitions | ADAPTED from `OE4T/linux-tegra-4.9` `drivers/staging/android/uapi/sync.h` (Copyright (C) 2012 Google, Inc.) | branch `l4t/l4t-r32.7.4-4.9` tip `78abfa9d9f4c9bc47c91b64a4f1a02952b67db0e`; file sha256 `d90f5f6a2e77371d9de7ba91595883ceb84e302b98453e37db09f7124679d9eb`; public URL <https://github.com/OE4T/linux-tegra-4.9/blob/l4t/l4t-r32.7.4-4.9/drivers/staging/android/uapi/sync.h> | GPL-2.0 (origin: GPL-2.0 kernel UAPI header, Copyright (C) 2012 Google, Inc.) |
| `third_party/tegra/nvhost_ioctl.h` | `tegra-kernel-reference` `include/uapi/linux/nvhost_ioctl.h` | `7d95822acda1f6dab3f3d1d099b43ff9e0d0e626` (pinned local reference checkout; D15 candidate, not proven build#149) | GPL-2.0 (NVIDIA kernel uapi, GPLv2) |
| `shaders/gob_swizzle.comp` | Adapted from `vk_layer_tegra_dc_present` `gob_swizzle.comp` (same pin) | `ecfb76a...` | GPL-2.0-or-later |
| `COPYING` | GPLv2 text copied from `vk_layer_tegra_dc_present/LICENSE` (same pin) | `ecfb76a...` | GPL-2.0 |

Copied headers are verbatim (their own SPDX/copyright headers intact).

The legacy sync-fence ABI definitions in `src/sync_release.h` are **adapted,
not verbatim**: the field layout and ioctl number are kept ABI-identical so the
kernel query and struct sizes (40 / 80 bytes, `_IOWR('>',2,...)` = `0xC0283E02`)
are not guessed, but the type names are rewritten with a `tp_` prefix and the
upstream trailing flexible-array members (`pt_info[0]` / `driver_data[0]`) are
omitted. The original copyright notice (Copyright (C) 2012 Google, Inc.) and
GPL-2.0 origin are recorded inline in `src/sync_release.h` above the
adapted definitions.
`shaders/gob_swizzle.comp` preserves the reference swizzle/block-addressing
formula verbatim and adds only a rotation mapping at the conversion boundary;
the adaptation is documented in the shader header.

## Repository licenses

* `theofficialgman/vk_layer_tegra_dc_present` -- GPL-2.0-or-later (`flip_layer.c`
  SPDX) / repository LICENSE is GPLv2. See `sources.lock.json` at the repo root.
* `tegra-kernel-reference` -- pinned local checkout of NVIDIA/Switchroot kernel
  uapi headers, used read-only for exact struct/ioctl definitions.

## Build glue (ours)

`third_party/tegra/uapi/linux/types.h` is our own shim redirecting
`<uapi/linux/types.h>` to the installed `<linux/types.h>`. It is not upstream.

## New code

`src/`, `shaders/pattern.comp`, `tests/`, `scripts/`, `Makefile`,
`CMakeLists.txt` are original work for the native-gamescope project, licensed
GPL-2.0-or-later to remain compatible with the copied GPL sources.
