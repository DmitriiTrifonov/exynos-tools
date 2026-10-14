# Using the layer in a launcher

Android's system loader will not load a layer into an unmodified app (docs/INTEGRATION.md), so a
launcher has to load it itself. Winlator-style launchers that ship a "BCn layer" slot already do:
the guest process runs on a Khronos Vulkan loader, finds implicit layers through `VK_LAYER_PATH`
and enables them with an environment variable. That slot can be filled with our layer, no fork of
the launcher needed.

## Bannerlator (GPL-3.0, https://github.com/The412Banner/Bannerlator)
Facts below come from its public docs and sources (README, docs/graphics-wrappers-guide.md,
docs/WRAPPER_MANAGER_PLAN.md, `XServerDisplayActivity.java`, `WrapperManager.java`) and from the
`extra_libs.tzst` asset of its repository; none of its code is used here.

- The "Wrapper + bcn_layer" driver runs [leegao's bcn_layer](https://github.com/leegao/bcn_layer)
  (MIT), a Granite-based BC1-BC7 decoder, on every GPU that is not Qualcomm (`vendorID != 0x5143`;
  Xclipse qualifies). It sets `ENABLE_BCN_COMPUTE=1` (+ `BCN_COMPUTE_AUTO`) which is the enable
  variable of that layer's implicit-layer manifest.
- The layer lives in the container's root as `usr/lib/libbcn_layer.so` and
  `usr/share/vulkan/implicit_layer.d/libbcn_layer.json`; `VK_LAYER_PATH` points at
  `implicit_layer.d`, and `library_path` is a bare file name resolved on the guest library path.
- A user-installed override `filesDir/graphics_driver/leegao_bcn.tzst` (Wrapper Manager, "BCn layer"
  slot) is extracted over the container root at every launch, after the bundled files. The archive
  must contain `usr/lib/libbcn_layer.so`.

`scripts/package-bannerlator.sh` builds exactly such an archive from our Android library: our
`libVkLayer_exynos_tools.so` as `usr/lib/libbcn_layer.so`, and our manifest
(`launcher/bannerlator/libbcn_layer.json`: layer `VK_LAYER_EXYNOS_TOOLS_xclipse`, enabled by
`ENABLE_BCN_COMPUTE=1`). When `EXYNOS_TOOLS_BC` is not set, the layer treats `ENABLE_BCN_COMPUTE=1`
as `emulate`, so no extra setting is needed.

### Try it
1. Install Bannerlator (APK from its GitHub releases), create a container, choose the graphics
   driver "Wrapper + bcn_layer".
2. Baseline first: run a game that needs BC textures with the stock layer and note what happens.
3. Wrapper Manager -> "BCn layer" slot -> Update -> pick `leegao_bcn.tzst` built from this repo.
4. Run the same game again. Set `EXYNOS_TOOLS_LOG=1` in the container's environment variables to
   get `[exynos-tools]` lines in the guest's stderr/log: they show whether the layer loaded and
   whether emulation is on ("BC emulation active (mask 0xff)").

### Things that can go wrong (not tested yet)
- The Wrapper ICD between the layer and the Samsung driver may hide `VK_KHR_push_descriptor` or
  extended storage formats. The layer then stays off and says so ("device lacks push descriptors").
  A descriptor-pool path would fix that.
- Only `vkCmdCopyBufferToImage` (and the `2` forms) can fill an emulated image. Reading such an image
  back, copying one BC image to another, 3D BC textures: not supported yet.
- The layer is a separate implementation; the stock bcn_layer remains the reference for comparison.
