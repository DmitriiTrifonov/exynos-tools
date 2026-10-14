# Sources

| Source | Used for | License / status |
|---|---|---|
| Vulkan specification (Khronos) | API semantics, features, formats | public spec |
| Vulkan loader and layer interface docs (Khronos) | layer negotiation and dispatch | public docs |
| Android Vulkan documentation | layer discovery on Android | public docs |
| Khronos Data Format Specification | BC1-BC7 block layouts | public spec |
| README of WearyConcern1165/ExynosTools | requirement level only: what such a layer does | read README only, no code |
| Samsung Community threads on Exynos 2500 (BCn, Vulkan 1.4, host_image_copy) | reported driver gaps, to verify on device | public posts |
| Vortek Internals Part 1 (leegao.github.io) | Winlator Vulkan architecture, ICD/IPC layout | public article |
| brunodev85/vortek README | what Vortek is; license LGPL-2.1 (do not copy code) | README only |
| libadrenotools README (bylaws) | custom driver loading, BSD-2-Clause | README only |
| source.android.com "Implement Vulkan" | Android layer discovery rules | public docs |
| GameNative release notes / READMEs | which stack GameNative builds on | README level |
| bcdec (iOrange), third_party/bcdec/bcdec.h | CPU reference decoder for tests only; MIT or Unlicense (dual) | vendored, license text kept in the file |
| Khronos Data Format Specification / Vulkan spec, BC4/BC5 sections | decode shader (src/layer/shaders/bc45.comp) | public spec |
| bcdec BC7 partition tables, via scripts/gen-bc-tables.py -> src/layer/shaders/bc7_tables.glsl, bc6h_tables.glsl | tables in the BC7 decode shader; MIT or Unlicense (bcdec), generated file carries a pointer to its source | permissive, attribution kept here and in the generated header |
| Microsoft BC7 format reference (learn.microsoft.com, direct3d11/bc7-format) | mode table for src/layer/shaders/bc7.comp (as cited by bcdec) | public docs |
| Microsoft BC6H format reference (learn.microsoft.com, direct3d11/bc6h-format) | mode layouts as encoded in bcdec; src/layer/shaders/bc6h.comp | public docs |
