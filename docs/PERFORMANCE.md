# Upload performance on the Galaxy Z Flip 7 (Xclipse 950)

Measured with `build/bc_bench` (GPU timestamps, median of 7 submits), `EXYNOS_TOOLS_BC=emulate`,
2026-10-15. Raw output: `docs/data/flip7-bc-bench-2026-10-15.txt`. Numbers move by ~10% between
runs (clocks/thermals). Data is random with the block modes spread evenly, which is a harder mix
than real textures (real BC7 content is dominated by a few modes).

| format | 4096x4096 upload | Mtexel/s | 256x256 upload (x256 in one cmdbuf) | 64x64 upload |
|---|---|---|---|---|
| BC3 (native, baseline) | 0.8-1.3 ms | 12000-21000 | 0.002 ms | 0.0002 ms |
| BC4 | 2.1 ms | 7900 | 0.016 ms | 0.012 ms |
| BC5 | 3.8 ms | 4400 | 0.022 ms | 0.016 ms |
| BC6H | 7.5 ms | 2200 | 0.073 ms | 0.045 ms |
| BC7 | 8.0 ms | 2100 | 0.039 ms | 0.007 ms |

Practical reading: a 2048x2048 BC7 texture costs about 2 ms of GPU time to decode, 1024x1024 about
0.5 ms. Loading a few hundred such textures is a fraction of a second of GPU work, then it is free
(sampling reads ordinary RGBA8/RGBA16F). VRAM use of an emulated texture is that of the decoded
format (4x BC7 size, 8x for BC6H); the BC data itself stays in the app's staging buffer only.

## Fixed cost per copy
A copy of a tiny texture costs 7-12 us of GPU time for BC4/5/7 (two barriers, a dispatch and a
transfer-stage fill; native copies cost 0.2 us) and 45 us for BC6H, plus 5-20 us of CPU time in
command recording. This is noticeable only for thousands of small copies per frame.

## What was tried
- BC7 started as one invocation per block with per-thread arrays: 13.6 ms for 4096x4096 and 81 us for
  a tiny copy. Rewriting it as one invocation per texel with positional bit reads and no per-thread
  arrays gave 8.0 ms and 7 us (1.7x and 10x).
- The same rewrite made BC6H 3.5x slower (24.7 ms), and parsing the header once per block in shared
  memory did not help (27 ms). The BC6H header parse is long and sequential, and with 16 invocations per
  block only one lane in 16 works during it. The per-block shader (one invocation per block, 16 texels
  written in a loop, no temporary arrays) stays: 7 ms.

## Ideas not done yet
- Skip rebinding our pipeline and restore the app's compute state lazily (only before the app's next
  compute command) when several uploads follow each other.
- Narrower barrier stages for the leading barrier.
- Hard-code the BC6H mode decoding instead of table-driven field loops (the constant-table loads are
  a likely source of the 45 us fixed cost).
- Real-game timings (loading, streaming) once the layer is wired into a launcher.
