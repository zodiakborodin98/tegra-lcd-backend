# Tegra LCD backend — source and direct-present probes

Open source material for direct Tegra X1 LCD presentation used by the SteamOS-on-Switch project.
Credits: theofficialgman and contributors (Tegra DC/GOB reference), Switchroot/CTCaer and
the kernel/boot community (Linux platform), NVIDIA UAPI authors, Valve/Gamescope.

## What is actually available

* `probe/`: standalone direct-present/fence/reuse/pacing diagnostics with host-only tests.
* `gamescope-reference/`: exact accepted presenter, first-frame and orientation180/sx1
  preferred shader sources. These depend on Gamescope's device/queue/ownership interfaces.
* Source pins, imported UAPI notices, GNU GPL text and third-party provenance.

**This is not yet an independently linked reusable LCD library.** The reference sources
are not compiled as a separate SDK and do not automatically replace the accepted backend.
The authoritative compositor implementation remains in the sibling `gamescope-tegra` repository.

## Host verification

On Linux with a native compiler and Vulkan headers:
```sh
make -C probe VULKAN_INC=/path/to/vulkan-headers test
```
Host tests use mocks/pure arithmetic, not the Switch display or GPU. The cross probe build is:
```sh
make -C probe CC=aarch64-linux-gnu-gcc SYSROOT=/path/to/target-sysroot \
    VULKAN_INC=/path/to/target-sysroot/usr/include bin/tegra-present-probe
```
The probe defaults to dry-run. Hardware output requires explicit `--present`; read its
CLI/resource/teardown contract first. Never run concurrently with an owning compositor.

## Next work

Define and test a stable device/queue/frame/fence/ownership interface, then extract a reusable
library without changing orientation or release semantics. Existing EGLStream consumer faults
are not repaired by separating repositories. No new hardware PASS is implied by this export.

Own work is GPL-2.0-or-later; imported material retains its exact original grant. Legacy sync
definitions retain Google attribution and their GPLv2 origin. No driver/firmware/kernel binary,
generated executable, proprietary payload or private evidence is included.

Related repositories: [graphics](https://github.com/zodiakborodin98/gamescope-tegra),
[image builder](https://github.com/zodiakborodin98/steamos-tegra),
[development record](https://github.com/zodiakborodin98/steamos-tegra-research).
