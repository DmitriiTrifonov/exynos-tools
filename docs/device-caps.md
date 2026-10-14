# Device capabilities: Galaxy Z Flip 7 (Exynos 2500, Xclipse 950)

Status: PARTIAL (vulkaninfo --summary collected, see docs/data/summary.txt; full dump still needed).

Original note: The Arch userland used for development cannot see the Samsung
Vulkan driver (no /vendor, /system or Vulkan ICD there), so `vulkaninfo` must be run from
the Android side.

## How to collect
1. Install a Vulkan capability viewer on the phone (e.g. "Vulkan Caps Viewer" by Sascha
   Willems) and use its export to produce a JSON report. Alternatively run `vulkaninfo`
   from an Android shell (adb or Termux) if available.
2. Put the raw report in `docs/data/flip7-vulkan-<date>.json` (or `.txt`).
3. Record below: One UI version, build number, date.

## What to extract
- apiVersion, driverVersion, driverID / driverName, deviceName
- `textureCompressionBC`, `textureCompressionETC2`, `textureCompressionASTC_LDR`
- Format features for BC1-BC7 (UNORM/SRGB, BC6H UFLOAT/SFLOAT): sampled image, blit src,
  transfer src/dst, filter linear
- Presence of: VK_EXT_host_image_copy, VK_KHR_dynamic_rendering, VK_EXT_descriptor_indexing
  features, VK_EXT_extended_dynamic_state{,2,3}, VK_KHR_maintenance4/5/6, timeline semaphores,
  VK_EXT_robustness2, VK_EXT_transform_feedback, VK_EXT_custom_border_color
- Limits DXVK/vkd3d-proton care about (maxPerStageDescriptor*, maxBoundDescriptorSets,
  maxStorageBufferRange, subgroup size and supported ops)
- Memory heaps and types

## Findings
Source: `vulkaninfo --summary` from Termux on the device (docs/data/summary.txt), 2026-10-14.
Device: SM-F766B (Galaxy Z Flip 7), Android 16 (SDK 36), One UI 8.0.5, build BP4A.251205.006.F766BXXSCBZH3.

- Device: Samsung Xclipse 950, integrated GPU, vendorID 0x144d, deviceID 0x3600200
- Driver: Samsung Proprietary (DRIVER_ID_SAMSUNG_PROPRIETARY), version 24.0.555, git f23b0cfde
- Device apiVersion: 1.3.279 (conformance 1.3.5.1). Not 1.4.
- Instance version reported by the loader: 1.4.354 (this is the loader, not the driver)
- Instance layers: none present
- Instance extensions (14): includes VK_EXT_debug_utils, VK_EXT_debug_report,
  VK_KHR_get_physical_device_properties2, VK_KHR_android_surface

### From full dump (docs/data/full.txt, plain `vulkaninfo`)
- Core features: textureCompressionBC = **false**; ETC2 = true; ASTC_LDR = true; ASTC_HDR = true
- No per-format table in this dump: BC format properties are NOT yet verified
  (rerun with `vulkaninfo --show-formats`).
- Missing from the device extension list: VK_EXT_host_image_copy, VK_KHR_maintenance6,
  VK_EXT_mutable_descriptor_type, VK_EXT_fragment_shader_interlock, VK_EXT_multi_draw,
  VK_EXT_depth_bias_control. (checked by reading the list, not by exhaustive grep)
- Present and relevant to DXVK/vkd3d-proton: dynamic_rendering (+local_read), maintenance4/5,
  synchronization2, timeline_semaphore, extended_dynamic_state 1/2/3, robustness2,
  transform_feedback, custom_border_color, descriptor_indexing, descriptor_buffer,
  graphics_pipeline_library, pipeline_library, buffer_device_address, conditional_rendering,
  depth_clip_enable/control, depth_clamp_zero_one, vertex_input_dynamic_state, color_write_enable,
  image_2d_view_of_3d, 4444_formats, border_color_swizzle, calibrated_timestamps,
  fragment_shading_rate, ray_query, ray_tracing_pipeline, acceleration_structure.
- Features: geometryShader, tessellationShader, dualSrcBlend, logicOp, multiDrawIndirect,
  depthClamp, depthBiasClamp, fillModeNonSolid, wideLines, samplerAnisotropy,
  shaderStorageImageRead/WriteWithoutFormat, shaderInt16, sparseBinding,
  extendedDynamicState3 (most fields; RasterizationStream and AlphaToOne = false) all true.
  shaderFloat64 = **false**.
- Limits: maxImageDimension2D 16384; maxBoundDescriptorSets 32; per-stage descriptor limits
  report 4294967295 (unbounded); subgroup size 64 (min 32, max 64 with size control).
- Memory: 2 heaps, 8 GiB device-local (budget ~7.6 GiB) and 64 MiB device-local; 16 memory types.
- extendedDynamicState2LogicOp = false.

## Format support measured with vk_probe (Flip 7, 2026-10-14)
Source: `vk_probe` (CI Android build), run in Termux, using the system Vulkan loader.
Columns: optimalTilingFeatures / linearTilingFeatures / bufferFeatures.

- BC1 (RGB/RGBA, UNORM/SRGB), BC2, BC3 (UNORM/SRGB): **supported natively**,
  optimal 0xd401 (sampled, filter-linear, blit-src, transfer-src/dst), linear 0xc401.
- BC4 (UNORM/SNORM), BC5 (UNORM/SNORM), BC6H (UFLOAT/SFLOAT), BC7 (UNORM/SRGB):
  **all zero, not supported**. So `textureCompressionBC=false` because only BC1-3 are present.
- features: textureCompressionBC=0, ETC2=1, ASTC_LDR=1, shaderFloat64=0
- Depth: D16_UNORM 0xce01, D32_SFLOAT 0xce01, D32_SFLOAT_S8_UINT 0xc601 (no blit-dst);
  **D24_UNORM_S8_UINT and X8_D24_UNORM_PACK32 are absent (0)**. D3D11 D24S8 must fall back.
- B8G8R8A8/R8G8B8A8 UNORM 0xdd83 (buffer 0x58), SRGB 0xdd81; A2B10G10R10_UNORM 0xdd83;
  R16G16B16A16_SFLOAT 0xdd83; R16G16B16A16_UNORM 0xcd83 (one bit fewer than SFLOAT).
- With and without our layer the app-visible output was identical, but see the open
  question: it is not proven that the layer was loaded (empty VK_LOADER_DEBUG output).

## Layer on the real device (2026-10-14)
- Loader mode (VK_INSTANCE_LAYERS) in Termux: the layer is NOT loaded (0 log lines). Termux's
  libvulkan.so is a symlink to /system/lib64/libvulkan.so, i.e. the Android system loader, which
  loads layers only from an app's own lib dir or the debug dir of debuggable apps.
- Embedded mode (vk_probe dlopens the layer and builds the chain, EXYNOS_PROBE_LAYER=...):
  works on Xclipse 950. Instance, 168 device extensions, features, 36 format queries, device,
  queue, waitidle all pass through; app-visible output identical to the plain run.
