# PocketPico — CLAUDE.md

## Project Overview
Game Boy (DMG) emulator (peanut-gb) running on a PicoCalc device with RP2350 (Raspberry Pi Pico 2).
Forked from TheKiwil/PocketPico which is itself a fork of slintak/PocketPico (originally RP2040 + ILI9225).

**Upstream remote:** https://github.com/TheKiwil/PocketPico.git

## Hardware
- **MCU:** RP2350 (Pico 2), dual Cortex-M33, overclocked to 300 MHz
- **Display:** ILI9488 3.5" LCD, physical 320×480, driven via PIO0 + DMA. `WIDTH=320, HEIGHT=320` in code — only the first 320 rows are used. Game image (2× scaled: 320×288) is centered vertically with 16-pixel margins top and bottom.
- **Keyboard:** I2C keyboard (PicoCalc QWERTY), polled via hardware alarm 0 (`TIMER0_IRQ_0`). Timer fires every 1ms (`TICKSPERSEC=1000µs`); I2C read/write done every 16ms (`KEYCHECKTIME=16`).
- **SD card:** SPI0 (GPIO 16–19), FatFS, stores `.gb` ROM files and save states (in `gb/`, see ROM Loading Workflow)
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

### LCD scanline flicker / jumpy row
**Problem:** Earlier scanline rendering had two separate hazards:
1. CPU/DMA overlap: `lcd_draw_line_bis` rendered new pixels into `pixels_buffer` while the previous row DMA could still be reading it.
2. Panel scanout tearing: even after row ping-pong buffers removed CPU/DMA overlap, the visible "jumpy row" remained because the LCD was updated as 288 separate row DMA transactions with no TE/VSYNC sync.

**Fix:** `FULL_FRAME_LCD_STAGING=1` stages each 2x-scaled Game Boy frame into the existing full `pixels_buffer`. `lcd_draw_line_bis` fills two physical rows per Game Boy line in RAM, then on `line == LCD_HEIGHT - 1` performs one contiguous `start_write_data()` for the full 320x288 game image. Hardware test confirmed this fixes the jump while preserving good apparent frame rate; sound still needs a quick check.

### LCD last-row corruption / frame boundary fix
**Problem:** The original code had two bugs in `lcd_draw_line` and `lcd_draw_line_bis`:
1. `line == 0` called `start_window()` but never sent pixel data, so GB line 0 was skipped. Lines 1–143 wrote 2 rows each = 286 rows in a 288-row window.
2. `line == LCD_HEIGHT` (144) was the trigger for `finish_write_data(true)` / next `start_window` — but peanut-gb never calls the draw callback with `line=144` (that's VBLANK). So CS was never deasserted between frames, and the next frame's CASET/RASET command bytes landed as pixel data in the two unfilled rows.

**Fix (`lcd_draw_line` and `lcd_draw_line_bis`):** Write pixel data for `line == 0` immediately after `start_window()`; changed the trigger from `line == LCD_HEIGHT` (dead code) to `line == LCD_HEIGHT - 1`; call `finish_write_data(true)` on the last line to deassert CS cleanly.

### I2C keyboard starvation
The original I2C timeout was 500ms per operation; the 1ms interrupt fired every tick, causing the CPU to spend most of its time in I2C timeouts. Changed to 10ms (10000µs).

### flash_safe_execute on core1
Added `flash_safe_execute_core_init()` at the start of `core1_audio` so core0 can safely lock out core1 during flash operations.

## ROM Loading Workflow
1. Copy `.gb` files to the `gb/` folder of a FAT32 micro SD card (the card root is used as a fallback, see below)
2. Power on → file selector appears
3. Select ROM with **A or B** → "Loading..." → ROM written to flash, `rom_bank0` filled from SD buffer → game starts
4. **Start** in selector currently attempts to resume from flash directly (skips `load_cart_rom_file`) — unreliable because XIP cache invalidation is not reliable on RP2350. **TODO:** make Start behave like A/B (load from SD).

### ROM directory and save location (`ROM_DIR "gb"`)
`rom_dir_findfirst` searches `gb/` first and falls back to the card root when that directory is
missing **or** contains no `.gb` files (so an empty `gb/` does not strand the user on a blank menu).
Both locations are never merged — it is `gb/` *or* the root, whichever wins. The selector stores the
full relative path (`gb/Tetris.gb`) so `load_cart_rom_file`'s `f_open` resolves it unchanged;
`rom_basename()` strips the directory for on-screen display and the "Loading %s" line.

Saves follow the ROM directory: `save_dir_prefix` is set by `rom_file_selector_display_page` to the
prefix the scan chose (`"gb/"` or `""`), and all four save functions build their path through
`gb_save_path()`. This also covers the Start/resume path, which selects no individual file.

| What | With ROMs in `gb/` | Root fallback |
|---|---|---|
| Battery RAM save | `gb/TETRIS` (no extension) | `TETRIS` |
| Emulator state | `gb/TETRIS_state.bin` | `TETRIS_state.bin` |

The save name is the **cartridge header title** (ROM bytes `0x134`–`0x143`) from `gb_get_rom_name()`,
not the file name — renaming a `.gb` does not orphan its save, and two ROMs sharing a header title
(e.g. a ROM and its patched variant) overwrite each other's saves.

Hardware-validated. Migration for existing cards: move the `*_state.bin` files **and** the
extension-less RAM saves into `gb/`.

**Key files:**
- `src/main.c:397` — `ROM_DIR` definition
- `src/main.c:405-418` — `save_dir_prefix` and `gb_save_path()`
- `src/main.c:732` — `rom_dir_findfirst()` (subfolder search with root fallback)
- `src/main.c:752` — `rom_basename()` (display-only path stripping)
- `src/main.c:787` — selector points `save_dir_prefix` at the scanned directory

### Header-title buffer fix
`gb_get_rom_name()` copies up to 16 printable chars from the header then appends `'\0'`, but the four
save functions passed a `char[16]` — a full 16-character title overflowed the stack buffer by one
byte. `gb_save_path()` now uses `char title[17]`, and the composed paths are 48-byte buffers built
with `snprintf` instead of `sprintf` (30 bytes worst case: `"gb/"` + 16 + `"_state.bin"` + NUL).

## Gotchas
- `copy_to_ram` means all code runs from SRAM — `__no_inline_not_in_flash_func` still used for flash operation callbacks but is largely redundant
- `PARAM_ASSERTIONS_DISABLE_ALL=1` — all SDK assertions disabled
- `PICO_ENTER_USB_BOOT_ON_EXIT=1` — device enters BOOTSEL on firmware exit/crash
- `xip_cache_invalidate_all()` is called before the game loop and before any resume-from-flash path, but its effectiveness on this hardware is unreliable for post-write cache invalidation — hence the SD-direct fill of `rom_bank0`
- `ENABLE_DEBUG 1` is set — DBG_INFO outputs to USB serial. Do not add `stdio_flush()` in hot paths (update_lcd, draw line callbacks)
- `lcd_draw_line_bis` is the active draw callback (registered via `gb_init_lcd`). `lcd_draw_line` exists but is not used.
- `gb_init` bad-checksum diagnostic: if FSE=999 it means `flash_safe_execute` was never called — the ROM in flash was not updated this session. User pressed Start (resume path) instead of A/B (SD load path), or SD mount/open failed.

## Current LCD Rendering

### Full-frame LCD staging (`FULL_FRAME_LCD_STAGING=1`)
**Problem:** The row-buffered scanline fix restored correct screen size and removed CPU/DMA buffer overlap, but the visible "jumpy row" remained unchanged. A follow-up diagnostic disabled keyboard polling during gameplay; the jump was still unchanged, so I2C/keyboard IRQ polling is not the root cause.

**Approach:** Keep keyboard polling enabled and stage each 2x-scaled Game Boy frame into the existing full `pixels_buffer`. `lcd_draw_line_bis` fills two physical rows per Game Boy line in RAM, then on `line == LCD_HEIGHT - 1` performs one contiguous `start_write_data()` for the full 320x288 game image. This removes the 288 separate row DMA transactions from active gameplay rendering.

**Status:** Implemented and hardware-validated for display size and jump behavior. Builds successfully with `cmake --build build`. Sound has not yet been rechecked after the staging change.

### Row-buffered scanline rendering (`pixels_buf_a/b`)
**Problem:** Single framebuffer means CPU writes new pixels into the buffer while DMA may still be reading from it for the previous scanline. If a 16ms I2C keyboard poll hits mid-transfer, the SPI stream pauses and the display scan drifts — visible as tearing/jumpy rows.

**Approach:** Preserve the original full `pixels_buffer` for selector/menu drawing and `update_lcd()` / `update_full_screen()`. Add two small scanline ping-pong buffers (`pixels_buf_a`, `pixels_buf_b`) sized for one 2x-scaled row (`LCD_WIDTH * 2 * 2` bytes). The LCD callback renders into the selected row buffer and then sends it via DMA.

**Status:** Implemented but currently bypassed by `FULL_FRAME_LCD_STAGING=1`. Screen size was validated on hardware; jump behavior remained unchanged.

**Regression avoided/fixed:** A prior working-tree version replaced `pixels_buffer` with two 115200-byte buffers, but `clear_frame_buff()`, `clear_screen_buff()`, and full-screen updates still needed up to 230400 bytes. That version also initialized `frame_buf` after rendering and sent only one physical row for most Game Boy lines, filling about half of the 320x288 game window. The current working tree restores two `write_data(..., LCD_WIDTH * 2)` calls per source line in `lcd_draw_line_bis`.

**Key files:**
- `src/main.c:26-27` — LCD rendering mode flags
- `src/main.c:88-90` — staged 320x288 game-frame dimensions
- `src/main.c:158-167` — full-frame `pixels_buffer` plus row-sized scanline ping-pong buffers
- `src/main.c:310-390` — `lcd_draw_line_bis` full-frame staging path (active callback)

## Known Issues
- Sound has not yet been rechecked after enabling full-frame LCD staging.
