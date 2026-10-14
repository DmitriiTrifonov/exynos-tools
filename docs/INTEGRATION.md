# Phase 2: how a layer gets into the Winlator-style stack

Status: research from public docs/articles only (no code of ExynosTools-like projects read).
Everything marked HYPOTHESIS must be verified by an experiment on the device.

## 1. Android's own rules for Vulkan layers
- Android ships no layers on production images. For non-debuggable apps the loader looks
  for layers only in the app's own native library directory (files named like
  `libVkLayer_*.so`, built with the NDK). Debuggable apps additionally search
  `/data/local/debug/vulkan`; Android 10+ can load layers from a separate APK.
  Which layers are on is chosen per app by system settings.
- Consequence: we cannot drop a layer into a stock, non-debuggable launcher from outside.
  It must be shipped inside the launcher's APK/lib dir, or loaded by the launcher itself.
  (Source: source.android.com "Implement Vulkan".)

## 2. Winlator's Vulkan path (Vortek), per public write-ups
Game (x86, in Wine, under box64, glibc rootfs)
  -> Vortek client ICD `libvulkan_vortek.so` (glibc side, ICD JSON in the rootfs,
     `api_version` 1.1.128)
  -> IPC: Unix socket (`VORTEK_SERVER_PATH`, `/tmp/.vortek/V0`) + two shared-memory ring buffers
  -> Vortek server `libvortekrenderer.so` (Bionic, inside the launcher app process)
  -> `dlopen("libvulkan.so")` (Android system loader) -> Samsung driver.
- The server is where capabilities are rewritten: reported extensions/features can be altered
  and shader modules are patched. Write-ups list BCn emulation (JIT decompression) and
  scaled-format emulation among Vortek's workarounds. Detail is in a "Part 2" we have not read.
- Client reports Vulkan 1.1.128 to the guest, not the device's 1.3.
- Vortek's source is public (LGPL-2.1). Reading it for protocol/integration facts is fine for
  this project; copying its code into an MIT project is not (see CLEANROOM.md).

## 3. Other drivers in the same launchers
- Adreno custom drivers (Turnip) are loaded with libadrenotools (BSD-2-Clause), which is
  Adreno-oriented; its README says other vendors need an issue/support. Mechanism not
  documented in the README; not confirmed to work for Xclipse.
- GameNative is based on Pluvia with Winlator as backend and imported a Vulkan renderer from
  a Winlator fork; exact driver-loading code not examined.

## 4. Candidate insertion points
A. Between Vortek server and system loader (Bionic side). Needs the launcher to load our
   library (fork/patch of the launcher or an agreed hook). Sees real Samsung driver. Closest
   to where BCn is already handled. Cannot be done on a stock APK.
B. In the guest (glibc/aarch64 rootfs) as a normal Khronos layer or ICD wrapper, above Vortek.
   Standard Linux layer mechanism (no Android restrictions). But it sees Vortek's virtual
   device, not Xclipse, so it cannot fix driver gaps below Vortek.
C. As an ICD-style wrapper library that the launcher loads in place of `libvulkan.so`/the
   driver, forwarding to the real Samsung driver (HYPOTHESIS: this is how "driver slot"
   packages work). Requires a loader mechanism we can use for a non-Adreno driver.
D. Standalone test harness (our own Android app with a bundled layer). Not a product, but
   the only fully controllable place to develop and test phases 3-6 on the Flip 7.

## 5. Decision for phase 3
Develop and test against option D first: a small Android app that creates a Vulkan device,
loads our layer from its own lib dir (allowed by Android rules), and runs BC upload tests.
This does not depend on any launcher. Integration into Winlator/GameNative (A or C) comes
after the layer works.

## 6. Experiments needed on the device (open questions)
1. In the launcher you use: which renderer is active (Vortek, Turnip/adrenotools, other)?
   What do Vulkan tools inside the guest report (textureCompressionBC, apiVersion)?
2. Does the launcher expose a custom driver / "driver package" import for non-Adreno devices?
   What files does a package contain?
3. Which launcher and version do you actually use for Windows games (Winlator variant,
   GameNative, Bannerlator, other)?
