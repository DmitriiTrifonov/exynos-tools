# Roadmap

0. (done) Scaffold, license, clean-room rules.
1. (done) Device capabilities: dump what the Flip 7 driver actually reports (docs/device-caps.md).
2. (done, research) Integration: how Winlator/GameNative-style launchers load a layer.
3. (done, device-tested in embedded mode) Pass-through layer with logging; works with a test app.
4. (done, device-tested) Honest capability reporting (BC features/formats only when emulation exists).
5. BCn emulation: format substitution, upload hooks, CPU decode first, compute later.
6. Tests: reference decode comparison, test app, dEQP subset, DXVK/vkd3d smoke tests.
7. Extras: host_image_copy, Vulkan 1.4 reporting, pipeline cache; benchmarks.
