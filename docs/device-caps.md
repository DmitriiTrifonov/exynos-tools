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
One UI version and build number: not recorded yet.

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
