---
name: libretro-core-dev
description: >-
  Use this skill when building, testing, or modifying the Cub Spiele.Com C99 libretro core
  (SM8521, LCD, audio, ZIP BIOS loader, touchscreen input, savestates, or Buildbot/CI files).
---

# Cub Spiele.Com Libretro Core Workflow

## Critical Libretro & Hardware Invariants
1. **Pure C99 & Buildbot Portability**: Keep `-std=c99 -Wall -Wextra` warning-free with zero external dependencies. Always test both shared (`platform=win`) and static (`STATIC_LINKING=1`) builds.
2. **Portable Savestates (`CSP2`)**: Use the explicit little-endian serializer in `src/sm8521.c` (never raw struct `memcpy`) and flush the touch tap queue in `retro_unserialize()`.
3. **BIOS Loading (`src/zip_bios.c`)**: Match `gamecom.zip` entries by uncompressed size (`4096` / `262144`) so both MAME `.bin` and No-Intro `.tgc` archives work out of the box.
4. **Audio & Frame Pacing**: Default to `60hz_realtime` (`81,920` cycles -> `735` PCM samples/frame) and keep `SGDA ($004E)` cycle-accurate inside `advance_audio_samples()`.
5. **Touchscreen Input**: Preserve `-0x8000` out-of-viewport filtering, `16x16` hardware cell highlight, and the FIFO tap queue (`3` press / `2` release frames) so rapid clicks never drop.
6. **Libretro Upstream**: New `.info` updates go to `libretro/libretro-super` (`dist/info/cubspielecom_libretro.info`).

## Verification Checklist
1. Build shared DLL and static archive (`STATIC_LINKING=1`) with zero warnings.
2. Compile and run `tests/unit_tests.c` (CPU invariants, savestates, Libretro ABI).
3. Run `tests/smoke_test.c` against `internal.bin` / `external.bin` (or `gamecom.zip`).
4. Copy `cubspielecom_libretro.dll` and `cubspielecom_libretro.info` to `S:\RetroArch\cores\` and `S:\RetroArch\info\` when RetroArch is closed.
