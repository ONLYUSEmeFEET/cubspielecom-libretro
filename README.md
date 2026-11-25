# Cub Spiele.Com — Libretro Core (`cubspielecom-libretro`)

A pure C99 **libretro** core for the 1997 **Tiger Game.com** (Sharp SM8521) handheld console, ported from **[Cub Spiele.Com](https://github.com/ONLYUSEmeFEET/Cub-Spiele.Com)** with zero external dependencies.

## Features
- **Cycle-Accurate Sharp SM8521 CPU, MMU, DMA Blitter & Timers** (`src/sm8521.c`)
  - Full 256-opcode SM8500/SM8521 instruction set with datasheet-accurate ALU flags (`FLAG_V`/`FLAG_S`), 16-bit `w(r)` indexed internal RAM addressing, `PS0`/`$001E` mirror sync, and P3 bit 6 dual-cartridge slot isolation
  - 16KB column-major 2-bpp VRAM (`$A000–$DFFF`) and 8KB battery-backed NVRAM (`$E000–$FFFF`)
  - Exact 60 Hz real-time pacing (`81,920` SM8521 cycles / `735` PCM samples per frame) + optional native `74.20 Hz` mode for VRR/G-Sync displays
- **200×160 2-bpp STN LCD Renderer** (`src/lcd.c`)
  - Direct `XRGB8888` output with calibrated olive-green (`#b9c48a`) + 3 additional palettes, LCD Contrast Wheel, optional STN ghosting, and active 16×16 hardware touch cell highlight
- **Pure C99 Hardware Audio & 28mm Speaker DSP** (`src/audio.c`)
  - Cycle-accurate `SG0`/`SG1` 32-step wavetable + `SG2` 15-bit LFSR noise + cycle-accurate `SGDA ($004E)` 8-bit DAC + BIOS digitized clip mixer
  - Authentic 28mm Game.com enclosure filter chain (310 Hz HPF -> 1,350 Hz BPF / 1,220 Hz resonance -> soft clip -> 3,450 Hz LPF) outputting 44,100 Hz stereo 16-bit PCM
- **Full Libretro Integration** (`src/libretro.c`)
  - Core Options V2 (with legacy fallback), Memory Maps (`RETRO_ENVIRONMENT_SET_MEMORY_MAPS`), `RETRO_MEMORY_SAVE_RAM` (8KB NVRAM `.srm`), portable little-endian savestates, and no-game BIOS boot
  - **Dual-Cartridge Subsystem** (`gamecom_dual_cart`) for loading Cartridge Slot 1 + Slot 2 simultaneously
  - **Direct `gamecom.zip` BIOS Support** (`src/zip_bios.c`) — loads standard RetroArch/MAME/No-Intro BIOS archives automatically

## BIOS Setup
Place **either** of the following in RetroArch's `system/` or `system/gamecom/` directory:
- **`gamecom.zip`** containing the 4 KB internal BIOS and 256 KB external BIOS (supports both MAME `internal.bin`/`external.bin` and No-Intro `[BIOS] Tiger Game.com (USA) (Internal/External).tgc` names), **or**
- Loose **`internal.bin`** (4 KB) and **`external.bin`** (256 KB) files.

## Controls
| Game.com Input | RetroPad / Device |
| :--- | :--- |
| **D-Pad** | D-Pad |
| **A / B / C / D** | B / A / Y / X |
| **Menu / Pause / Sound** | Select / Start / L |
| **Touchscreen (13×10 Matrix)** | Mouse / Touchscreen (`RETRO_DEVICE_POINTER`) **or** Right Analog Stick + `R2`/`R3` |

## Core Options
- **Display & LCD**: `LCD Color Palette`, `LCD Contrast Wheel`, `STN LCD Ghosting`
- **Touch & Stylus**: `Touch Grid & Cursor Overlay` (`cell_highlight` default), `Touch Cursor Auto-Hide`, `Stylus Click Mode` (`single_click` FIFO tap queue default), `Right Stick Virtual Stylus`
- **System & Audio**: `Fast Boot (Skip 3D Globe)`, `28mm Internal Speaker Filter`, `Tear-Free V-Sync Latch`, `Frame Pacing` (`60hz_realtime` default / `native_74hz`)

## Dual-Cartridge Subsystem
In RetroArch, go to **Main Menu → Load Core → Cub Spiele.Com → Subsystems → Load Dual Cartridge (Slot 1 + Slot 2)** to insert two Game.com cartridges at once and switch between them from the BIOS menu.

## Building & Testing
```bash
# Shared library (Linux / macOS / Windows MinGW)
make -f Makefile.libretro

# Static library (iOS / Switch / Vita / 3DS / Emscripten)
make -f Makefile.libretro STATIC_LINKING=1

# CMake
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build

# Automated unit & ABI tests (no BIOS/ROM required)
gcc -std=c99 -O2 -Iinclude -Isrc tests/unit_tests.c src/sm8521.c src/lcd.c src/audio.c src/zip_bios.c src/libretro.c -o unit_tests -lm && ./unit_tests
```

## License
**BSD-3-Clause** — see [LICENSE](LICENSE). Includes attribution to the MAME SM8500 / Game.com driver authors (Wilbert Pol, Robbbert, MAMEdev).
