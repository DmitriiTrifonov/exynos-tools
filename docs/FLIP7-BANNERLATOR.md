# Galaxy Z Flip 7 (Xclipse 950) in Bannerlator: what works

Tested on SM-F766B, Android 16, Bannerlator 3.1.6, graphics driver "Wrapper + bcn_layer"
(slot "BCn layer (leegao)" filled with `leegao_bcn.tzst` from this repo's CI artifact).

## Setup that works
1. **DXVK 2.6.2.1** (arm64ec). DXVK 2.7.x refuses the device ("Device does not support 256 of push
   data"): Xclipse 950 reports `maxPushConstantsSize` = 128.
2. Launch games through **Goldberg** (Steam emulation). Raw launches may exit because the game
   restarts itself through Steam; RealSteam stalled on app ownership.
3. `/storage/emulated/0/wrapper/exynos.conf`:
   ```
   EXYNOS_TOOLS_HIDE_EXT=present_wait,present_id,VK_EXT_graphics_pipeline_library
   EXYNOS_TOOLS_BC=emulate-force
   ```
   - `present_wait` / `present_id`: with them on, `vkWaitForPresentKHR` crashed (0xc0000005 in the
     Unix call) right after DXVK created its swapchain: black screen.
   - `VK_EXT_graphics_pipeline_library`: with it on, `vkCreateGraphicsPipelines` crashed in Unity
     games (Untitled Goose Game) and the 3D scene stayed black (Firewatch).
   - `emulate-force`: the Wrapper already reports `textureCompressionBC`, so plain `emulate` does
     nothing; `-force` makes the layer decode BC4/BC5/BC6H/BC7 itself.
4. Logs: `wrapper/logs/exynos.log` (layer), `Documents/bannerlator/<game>/` (Wine/DXVK, enable in
   Settings -> Log Manager). Statistics lines look like
   `emulated uploads of BC format 143 so far: 128`; they are written whenever the count doubles.

## Results
| Game | Result |
|---|---|
| A Short Hike | renders; only BC1/BC3 (native), nothing to emulate |
| Untitled Goose Game | renders correctly; BC7 emulated (1000x1000 atlas) |
| Firewatch | renders correctly (menu and 3D scene); BC6H emulated, >128 uploads |

Emulated formats seen in real games: BC7 (format 145), BC6H (143). BC4/BC5 are covered by the
bit-exact tests against bcdec but not yet seen in a game log.
