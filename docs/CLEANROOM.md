# Clean-room rules

This project is an independent implementation. It is written from public specifications
and from observed behaviour of the hardware, not from other projects' code.

## Not allowed
- Reading, copying or paraphrasing source code of ExynosTools (or any project with the same
  goal) while writing code here. That includes its `src/`, shaders, build scripts and tests.
- Decompiling or disassembling its binaries or any driver binaries.
- Porting code from memory of such sources.

## Allowed
- The Vulkan specification, the Khronos loader/layer interface documentation, Android's
  Vulkan documentation, Khronos Data Format Specification (BCn definitions).
- Output of tools run on our own device (vulkaninfo, Vulkan Caps Viewer JSON, our own logs).
- Public README-level descriptions of what other projects do (requirements, not code).
- Third-party libraries under a license compatible with MIT, added as dependencies and
  listed in SOURCES.md with name, version and license.
- Public bug reports and forum posts about driver behaviour.

## Process
- Every external source that informed a design decision is listed in `SOURCES.md`.
- Every device finding goes in `docs/` with the date, device, firmware and how it was obtained.
- If a contributor has seen the forbidden code, they say so in the PR and do not touch the
  affected component.
