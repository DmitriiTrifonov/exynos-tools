# Phase 5 design: BCn emulation on Xclipse 950

Measured driver state (docs/device-caps.md): BC1-BC3 native; BC4, BC5, BC6H, BC7 missing.
Device has VK_KHR_push_descriptor, shaderStorageImageExtendedFormats, minStorageBufferOffsetAlignment 4,
128 bytes of push constants. Emulated formats are decoded to 8/16-bit uncompressed images.

## Substitute formats
| BC format | image is created as | sampled as | storage view |
|---|---|---|---|
| BC4_UNORM / SNORM | R8_UNORM / R8_SNORM | same | same |
| BC5_UNORM / SNORM | R8G8_UNORM / R8G8_SNORM | same | same |
| BC7_UNORM | R8G8B8A8_UNORM | same | same |
| BC7_SRGB | R8G8B8A8_UNORM | R8G8B8A8_SRGB (mutable-format view) | R8G8B8A8_UNORM |
| BC6H_UFLOAT / SFLOAT | R16G16B16A16_SFLOAT | same | same |

Decoded precision is 8 bit for BC4/BC5/BC7 (hardware decodes to higher internal precision; accepted
trade-off, tolerance +-1 LSB in tests).

## Moving parts
1. **Image creation** (`vkCreateImage`): the layer creates the image with the substitute format, adds
   STORAGE usage and MUTABLE_FORMAT, drops BLOCK_TEXEL_VIEW_COMPATIBLE and translates any format list.
   The returned handle is the real backing image. Memory requirements are therefore automatically right.
2. **Image views** (`vkCreateImageView`): BC view formats are translated to the substitute sampled format,
   with a usage restriction (SAMPLED) so sRGB views are legal.
3. **Buffers** (`vkCreateBuffer`): STORAGE_BUFFER usage is added to buffers with TRANSFER_SRC, so the decode
   shader can read the app's staging data in place. No companion images, no scratch memory, no lifetime
   tracking. Buffer sizes are recorded to bounds-check each decode.
4. **Upload** (`vkCmdCopyBufferToImage`, and the `2`/`2KHR` forms): for an emulated destination the layer
   records, per region: barrier (app layout -> GENERAL), compute dispatch (one invocation per 4x4 block),
   barrier (GENERAL -> app layout). Push descriptors carry the buffer and a per-mip storage view.
5. **Compute state**: the dispatch overwrites the app's bound compute pipeline, descriptor sets and push
   constants, so the layer shadows those per command buffer (compute bind point only) and restores them.
6. **Device creation**: the layer adds VK_KHR_push_descriptor and shaderStorageImageExtendedFormats if the
   app did not enable them. If the device cannot provide them, emulation stays off and nothing is advertised.

## Not supported (logged, fail safe)
Reads of emulated images by transfer (CopyImageToBuffer, CopyImage, BlitImage source), clears, 1D images,
linear tiling, storage/attachment usage of BC images, sparse residency. Transfer-only queues cannot run the
decode dispatch.

## Modes (EXYNOS_TOOLS_BC)
off (default), advertise / advertise-force (phase 4, reporting only), emulate / emulate-force (real
decoding for the formats implemented so far; `-force` pretends the driver has no BC4-BC7, for host tests).

## Sub-phases
5a  image/view/buffer substitution and tracking
5b  BC4/BC5 compute decode + compute-state restore + test against a reference decoder
5c  BC7
5d  BC6H
5e  remaining copy forms, 3D images, robustness, performance
