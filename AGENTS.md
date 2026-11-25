# Cub Spiele.Com Libretro Core — Agent Instructions (`AGENTS.md`)

## Architecture & Portability Rules
- **Pure C99**: Zero browser, OS-GUI, or C++ runtime dependencies. Must build cleanly with `-std=c99 -Wall -Wextra` via both `Makefile.libretro` and `CMakeLists.txt`.
- **Single-Core Hardware Invariants** (must match `Cub Spiele.Com/src/emulator/sm8521.ts`):
  1. `w(r)` indexed modes (`0x5A`/`0x5B`, `case 0x80`) must mask effective addresses with `& 0xFFFF` so `$0100–$03FF` internal RAM accesses never wrap into `$0000–$00FF` SFRs.
  2. 8-bit `ADC` and 16-bit `ADDW`/`ADCW` signed overflow (`FLAG_V`) uses `((d2 ^ d1 ^ 0x80) & (res ^ d1)) & 0x80` (and `0x8000` for 16-bit); 16-bit logical (`ANDW`/`ORW`/`XORW`) updates `FLAG_S` and clears `FLAG_V`.
  3. Writes to `PS0` (`$001B`) and `PS0_MIRROR` (`$001E`) stay synchronized.
  4. Never pad unused 8KB NVRAM (`$E000–$FFFF`) bytes with `0xFF` (preserves *Lights Out* `$0917` binary progress bytes).
  5. Pure hardware audio only (`SGDA` `$004E` DAC, `SG0`/`SG1`/`SG2`, and BIOS digitized clips).
- **Verification**: Always compile `cubspielecom_libretro.dll` (or `.so`) and run `smoke_test` after any core change.
