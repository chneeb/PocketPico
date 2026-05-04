# PocketPico — CLAUDE.md

## Project Overview
Game Boy (DMG) emulator (peanut-gb) running on a PicoCalc device with RP2350 (Raspberry Pi Pico 2).
Forked from TheKiwil/PocketPico which is itself a fork of slintak/PocketPico (originally RP2040 + ILI9225).

**Upstream remote:** https://github.com/TheKiwil/PocketPico.git

## Hardware
- **MCU:** RP2350 (Pico 2), dual Cortex-M33, overclocked to 300 MHz
- **Display:** ILI9488 3.5" LCD, 320×480, driven via PIO0 + DMA
- **Keyboard:** I2C keyboard (PicoCalc QWERTY), polled via hardware alarm 0 every 1ms
- **SD card:** SPI0 (GPIO 16–19), FatFS, stores `.gb` ROM files and save states
- **Audio:** I2S via PIO1, GPIO 26 (data) / 27 (BCLK) / 28 (LRCLK), 32768 Hz stereo
- **Flash:** W25Q series, 1MB firmware + ROM stored at `FLASH_TARGET_OFFSET = 0x100000` (1MB offset)

## Build
```
cmake --build build
# output: build/PocketPico.uf2
```
SDK version 2.1.1, toolchain 14_2_Rel1. Board: `pico2` (PICO_PLATFORM=rp2350).
Binary type: `copy_to_ram` — all code executes from SRAM.

## Architecture
- **Core 0:** Emulation (peanut-gb), LCD scanline rendering, SD card, file selector
- **Core 1:** Audio — minigb_apu + I2S DMA output (`core1_audio`)
- **ROM access:** `const uint8_t *rom = (XIP_BASE + FLASH_TARGET_OFFSET)` for bank switching; `rom_bank0[65536]` caches the first 64KB in RAM (used by `gb_rom_read` for addr < 64KB)

## Key Fixes Applied (vs upstream)

### ROM loading from SD card
**Problem:** After writing a ROM to flash, XIP cache served stale pre-write data. `flash_flush_cache()` is a no-op for the XIP cache on RP2350 (it only removes the QSPI CS force). `xip_cache_invalidate_all()` also did not reliably fix it.

**Fix (`load_cart_rom_file`):** During the flash write loop, simultaneously copy each sector's buffer directly into `rom_bank0`. After a successful load, `rom_bank0_ready = true` skips the `memcpy(rom_bank0, rom, ...)` in main — the data came from SD card and never touches the stale XIP cache.

`flash_safe_execute` + `flash_safe_execute_core_init()` on core1 are used for multicore-safe flash writes.

### LCD scanline flicker (race condition)
**Problem:** `lcd_draw_line_bis` rendered new pixels into `pixels_buffer` before calling `finish_write_data(false)`, so the CPU wrote to the buffer while the previous scanline's DMA was still reading it.

**Fix:** Moved `finish_write_data(false)` to be the first statement in both `lcd_draw_line` and `lcd_draw_line_bis` — DMA is guaranteed complete before the buffer is overwritten.

### I2C keyboard starvation
The original I2C timeout was 500ms per operation; the 1ms interrupt fired every tick, causing the CPU to spend most of its time in I2C timeouts. Changed to 10ms (10000µs).

### flash_safe_execute on core1
Added `flash_safe_execute_core_init()` at the start of `core1_audio` so core0 can safely lock out core1 during flash operations.

## ROM Loading Workflow
1. Copy `.gb` files to the root of a FAT32 micro SD card
2. Power on → file selector appears
3. Select ROM with A or B → "Loading..." → game starts
4. Press Start in selector to resume last loaded ROM (reads from flash directly)

## Gotchas
- `copy_to_ram` means all code runs from SRAM — `__no_inline_not_in_flash_func` still used for flash operation callbacks but is largely redundant
- `PARAM_ASSERTIONS_DISABLE_ALL=1` — all SDK assertions disabled
- `PICO_ENTER_USB_BOOT_ON_EXIT=1` — device enters BOOTSEL on firmware exit/crash
- `xip_cache_invalidate_all()` is called before the game loop and before any resume-from-flash path, but its effectiveness on this hardware is unreliable for post-write cache invalidation — hence the SD-direct fill of `rom_bank0`
- `ENABLE_DEBUG 1` is set — DBG_INFO outputs to USB serial. Do not add `stdio_flush()` in hot paths (update_lcd, draw line callbacks)
