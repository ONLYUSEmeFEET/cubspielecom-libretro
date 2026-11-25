# Cub Spiele.Com — Libretro Core (`cubspielecom-libretro`)

A pure C99 **libretro** core for the 1997 **Tiger Game.com** (Sharp SM8521) handheld console, ported directly from **[Cub Spiele.Com](https://github.com/ONLYUSEmeFEET/Cub-Spiele.Com)** with zero browser dependencies.

## Features
- **Cycle-Accurate Sharp SM8521 CPU, MMU, DMA Blitter & Timers** (`src/sm8521.c`)
  - Full 256-opcode SM8500/SM8521 instruction set with datasheet-accurate ALU flags (`FLAG_V`/`FLAG_S`), 16-bit `w(r)` indexed internal RAM addressing, `PS0`/`$001E` mirror sync, and P3 bit 6 cartridge slot isolation
  - 16KB column-major 2-bpp VRAM (`$A000–$DFFF`) and 8KB battery-backed NVRAM (`$E000–$FFFF`)
  - Native LCH/LCV frame timing (~74.20 Hz at 4.9152 MHz) with optional tear-free V-Sync scanline latching
- **200×160 2-bpp STN LCD Renderer** (`src/lcd.c`)
  - Outputs `XRGB8888` directly to RetroArch
  - Selectable palettes (Calibrated Olive Green `#b9c48a`, Classic Grayscale, Pocket Monochrome, High Contrast) + optional STN frame ghosting
- **Pure C99 Hardware Audio & 28mm Speaker DSP** (`src/audio.c`)
  - Cycle-accurate `SG0`/`SG1` 32-step wavetable + `SG2` 15-bit LFSR noise + `SGDA ($004E)` 8-bit DAC + BIOS waveform stream mixer
  - Optional 28mm Game.com internal speaker filter chain (310 Hz HPF -> 1,350 Hz BPF / 1,220 Hz resonance -> soft clip -> 3,450 Hz LPF), outputting 44,100 Hz signed 16-bit stereo PCM
- **Full Libretro Integration** (`src/libretro.c`)
  - `RETRO_DEVICE_JOYPAD` (D-Pad, A/B/C/D, Menu, Pause, Sound) + `RETRO_DEVICE_POINTER` (touchscreen / mouse stylus across the 13×10 hardware touch matrix)
  - `RETRO_MEMORY_SAVE_RAM` (8KB NVRAM `.srm`) & `RETRO_MEMORY_SYSTEM_RAM` (1KB internal RAM)
  - Deterministic save states (`retro_serialize` / `retro_unserialize`)
  - **Dual-Cartridge Subsystem** (`gamecom_dual_cart`) for loading both Slot 1 and Slot 2 cartridges simultaneously via RetroArch's Subsystems menu
  - Supports booting with no cartridge (`supports_no_game = true`) straight into the Game.com BIOS PDA/Solitaire menu

## BIOS Setup
Place the two Game.com BIOS files in RetroArch's `system/gamecom/` (or `system/`) directory:
- `internal.bin` (4 KB)
- `external.bin` (256 KB)

## Building

### Make (MinGW / Linux / macOS)
```bash
make -f Makefile.libretro
```

### CMake
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

## License
MIT License — see [LICENSE](LICENSE).
