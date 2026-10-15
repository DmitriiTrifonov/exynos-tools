# exynos-tools

A clean-room Vulkan layer that sits in front of the Samsung Xclipse driver and fills in
what the driver lacks (starting with BCn texture compression), so that Windows-game
translation stacks (DXVK, vkd3d-proton under Winlator/GameNative-style launchers) can run
on Exynos devices. First target: Galaxy Z Flip 7 (Exynos 2500, Xclipse 950).

Status: phase 5 (BCn emulation) works for all BC formats; hardening and performance are next. By default the layer is a pure pass-through. `EXYNOS_TOOLS_BC` selects:

- `off` (default): nothing changes.
- `advertise` / `advertise-force`: only report BC4/BC5/BC6H/BC7 (and `textureCompressionBC`) as
  supported; for testing the reporting.
- `emulate`: really emulate. BC4, BC5 (UNORM/SNORM), BC6H (UFLOAT/SFLOAT) and BC7 (UNORM/SRGB) work:
  images are backed by R8/R8G8/RGBA8/RGBA16F images and decoded by a compute shader when data is
  uploaded with `vkCmdCopyBufferToImage`. BC1-BC3 stay native. When all of BC1-BC7 are available,
  `textureCompressionBC` is reported true.
- `emulate-force`: like `emulate` but pretends the driver has none of BC4-BC7 (host testing).

Emulated BC images support sampling and upload only (no readback, copies out of them or storage use). See [docs/ROADMAP.md](docs/ROADMAP.md).
See [docs/FLIP7-BANNERLATOR.md](docs/FLIP7-BANNERLATOR.md) for the setup that works on the Galaxy Z Flip 7 in Bannerlator (DXVK version, config, tested games).

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

`build/bc_upload_test` uploads random BC4/BC5/BC6H/BC7 data through the layer and compares the decoded
result with a reference decoder:

    EXYNOS_TOOLS_BC=emulate EXYNOS_PROBE_LAYER=$PWD/build/libVkLayer_exynos_tools.so build/bc_upload_test

`build/bc_bench` measures upload speed (see docs/PERFORMANCE.md for the Flip 7 numbers).

Android arm64 builds come from CI (`.github/workflows/build.yml`, NDK). `vk_probe` is also
a handy replacement for `vulkaninfo --show-formats`: it prints the BC and D3D-relevant
format features of the device.

This project is clean-room: see [docs/CLEANROOM.md](docs/CLEANROOM.md).

License: MIT.
