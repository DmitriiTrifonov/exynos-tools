# exynos-tools

A clean-room Vulkan layer that sits in front of the Samsung Xclipse driver and fills in
what the driver lacks (starting with BCn texture compression), so that Windows-game
translation stacks (DXVK, vkd3d-proton under Winlator/GameNative-style launchers) can run
on Exynos devices. First target: Galaxy Z Flip 7 (Exynos 2500, Xclipse 950).

Status: planning. Nothing here works yet. See [docs/ROADMAP.md](docs/ROADMAP.md).

This project is clean-room: see [docs/CLEANROOM.md](docs/CLEANROOM.md).

License: MIT.
