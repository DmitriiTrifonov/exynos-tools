# exynos-tools

A clean-room Vulkan layer that sits in front of the Samsung Xclipse driver and fills in
what the driver lacks (starting with BCn texture compression), so that Windows-game
translation stacks (DXVK, vkd3d-proton under Winlator/GameNative-style launchers) can run
on Exynos devices. First target: Galaxy Z Flip 7 (Exynos 2500, Xclipse 950).

Status: phase 4 (honest BC capability reporting). By default the layer is a pure pass-through.
With `EXYNOS_TOOLS_BC=advertise` it reports BC4/BC5/BC6H/BC7 (and `textureCompressionBC`) as
supported where the driver lacks them, but creating such images still fails with
`VK_ERROR_FORMAT_NOT_SUPPORTED` until phase 5 adds the actual decoding. See [docs/ROADMAP.md](docs/ROADMAP.md).

## Build and try

Needs CMake, a C/C++ compiler and Vulkan headers >= 1.3 including `vulkan/vk_layer.h`
(pass `-DEXYNOS_VULKAN_HEADERS_PATH=<Vulkan-Headers>/include` if your system lacks it).

    cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build
    scripts/run-probe.sh build

`run-probe.sh` runs `vk_probe` without and with the layer (`VK_LAYER_EXYNOS_TOOLS_xclipse`)
and diffs what the application sees. The layer logs to stderr (logcat on Android);
`EXYNOS_TOOLS_LOG=0` turns the log off.

`scripts/check-advertise.sh build advertise` asserts that the advertised capabilities are
consistent (use `advertise-force` on drivers that already have BC).

Android arm64 builds come from CI (`.github/workflows/build.yml`, NDK). `vk_probe` is also
a handy replacement for `vulkaninfo --show-formats`: it prints the BC and D3D-relevant
format features of the device.

This project is clean-room: see [docs/CLEANROOM.md](docs/CLEANROOM.md).

License: MIT.
