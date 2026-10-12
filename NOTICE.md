# Attribution and source scope

The direct LCD/GOB/FLIP4 material was copied/adapted from
[theofficialgman/vk_layer_tegra_dc_present](https://github.com/theofficialgman/vk_layer_tegra_dc_present)
at `ecfb76aadbdc93cd7008e77bec944724cd70c364`. Its original main source notice is:

```text
SPDX-License-Identifier: GPL-2.0-or-later
Copyright (C) 2026 theofficialgman
Copyright (C) 2026 gavin_darkglider
Copyright (C) 2026 Anthropic (Claude AI assistant contributions)
```

GPL text is in COPYING. This attribution supplements original file headers;
it does not assert that every contributor wrote each adapted line.

The Tegra DC/nvhost UAPI headers retain their NVIDIA GPLv2-or-later notices,
including Robert Morell and Erik Gilling attribution. Legacy Android fence
layouts in sync_release/presenter/first-frame originate from the GPLv2 UAPI:
Copyright (C) 2012 Google, Inc.; OE4T/linux-tegra-4.9 reference
`78abfa9d9f4c9bc47c91b64a4f1a02952b67db0e`, sync.h SHA256
`d90f5f6a2e77371d9de7ba91595883ceb84e302b98453e37db09f7124679d9eb`.
Names/flexible-array details were adapted; no broader kernel-license exception is invented.
See `probe/third_party/PROVENANCE.md` for interface/source details.

Switchroot, CTCaer, theofficialgman, Gavin_Darkglider and the wider Linux/boot
community supplied the Switch kernel/BSP/hardware platform. The historical
kernel source match remains unknown; the local candidate reference pin is
`7d95822acda1f6dab3f3d1d099b43ff9e0d0e626` at
[theofficialgman/switch-l4t-kernel-nvidia](https://github.com/theofficialgman/switch-l4t-kernel-nvidia).

The `gamescope-reference/` source was integrated into Valve's Gamescope by this
project. Upstream Gamescope's BSD and other notices remain in the sibling graphics
repository; no Valve binary is included here. The retained files are reference
components, not proof of an independently usable library.

The wider Steam integration used SildurFX/Switchdeck, Switchdeck-Extras and Ivy's
credited download lineage. No Switchdeck installer/Steam/Proton payload is copied
into this LCD repository. Their GPLv3 and other component terms remain separate.

Publication reconstruction/build-path adaptation prepared 2026-10-12.
Own original work: Copyright (c) 2026 SteamOS-Switch contributors, GPL-2.0-or-later.
Imported files retain their original authorship/grants. No driver/firmware/kernel
image or private evidence is redistributed. This is unofficial community work.
