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
- **Audio:** **PWM**, not I2S — GPIO 26/27 (PWM slice 5 channels A/B), ~32.7 kHz stereo. `ext/PicoAudio` compiles its I2S/PIO path out entirely because `AUDIO_PWM_PIN` is defined (`audio.h:32`), leaving `#ifndef AUDIO_PWM_PIN` (`audio.c:64`) dead. The DMA writes packed L/R values straight into `pwm_hw->slice[5].cc`, paced by the PWM wrap DREQ. `ext/i2s` is excluded from the build (`CMakeLists.txt:37,41`).
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

### ROM loading from SD card — it was QMI address translation, NOT the XIP cache
This was misdiagnosed for a long time (and the wrong explanation cost three failed fix attempts).
Reading the same byte after writing a ROM whose header byte `0x100` is `0x00`:

| alias | address | value |
|---|---|---|
| cached | `0x10000000` | `2C` |
| nocache | `0x14000000` | `2C` |
| **notrans** | **`0x1c000000`** | **`00`** |
| direct SPI probe (0x03 cmd) | — | `00` |

The write was always fine (erase reads `FF`, program reads back `00`) and the cache was never the
problem — **RP2350's QMI address translation was remapping the reads**, which is why the non-cached
alias behaved identically to the cached one and why invalidating anything made no difference.

**Fix:** `rom` points at `XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + FLASH_TARGET_OFFSET`
(`0x1c000000`), the only alias that bypasses translation. It is also non-cached, so nothing can go
stale either. Reads are slower than the cached view, but only bank-switched reads above 64KB take
that path.

**Consequence:** ROMs larger than 64KB used to execute garbage. `rom_bank0` (64KB in RAM, filled
straight from the SD buffer during the write loop, `rom_bank0_ready = true`) is what made ROMs up to
64KB work all along — it hid the bug rather than fixing it. Keep it: it is still the fast path.

`load_cart_rom_file` also calls `flash_start_xip()` after the write loop, because
`flash_range_erase`/`flash_range_program` call `flash_exit_xip()` on entry but never re-enter XIP;
a flash-resident binary gets away with it, `copy_to_ram` does not.

`ROM verify: N bytes OK` is logged on every load — a full byte-for-byte comparison of the source
file against **both** sources `gb_rom_read` uses (`rom_bank0` below 64KB, flash above). Set
`AUDIO_DEBUG 1` for the raw/alias probes and a `Cart:` line with the MBC state.

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

### flash_safe_execute on core1 — REMOVED (it silently killed all audio)
`core1_audio` used to call `flash_safe_execute_core_init()`. That resolves to
`multicore_lockout_victim_init()` (`pico_flash/flash.c:99`), which installs an exclusive,
permanently-enabled handler on core1's SIO FIFO IRQ. `multicore_lockout_handler`
(`pico_multicore/multicore.c:217`) **reads and discards every FIFO word** that is not the current
lockout request id — including the `AUDIO_CMD_*` commands — so
`multicore_fifo_pop_blocking_inline()` in the audio loop never returned and `audio_callback` never
ran once. The FIFO cannot serve both audio commands and the flash-safe lockout.

**Fix:** dropped the call and set `PICO_FLASH_ASSUME_CORE1_SAFE=1` (`CMakeLists.txt`). Safe because
`copy_to_ram` puts all code and rodata in SRAM, so core1 touches no XIP while core0
erases/programs. `use_irq_only()` then makes core0 skip the handshake, and `flash_safe_execute`
still returns `PICO_OK` (ROM loading logs `FSE s0 ret=0` as before).

Symptoms if reintroduced: total silence, core1 alive and printing, full-speed gameplay (core0 never
blocks because the handler keeps draining), volume hotkeys dead.

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
- `xip_cache_invalidate_all()` is *not* unreliable — that was the old misdiagnosis. It is kept as
  belt-and-braces, but `rom` reads through the non-cached NOTRANSLATE alias, so there is no cache in
  front of ROM reads at all. See "ROM loading from SD card" above before touching any of this.
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

## Audio and Frame Rate

Audio and frame rate are one system here: the APU emits `AUDIO_SAMPLES` per **emulated frame**, so
the true sample rate is `AUDIO_SAMPLES x actual fps`. Every audio problem in this project traced
back to that coupling. Hardware-validated end state: **59 fps, `frame=16740 us`, sound correct**.

### Frame budget (measured, `AUDIO_DEBUG 1`)
| | LCD | emu | pacer | frame | FPS |
|---|---|---|---|---|---|
| Full render | 19.9 ms | 4.4 ms | 0 | 24.9 ms | 40 |
| `frame_skip` + pacer | 9.9 ms | 3.2 ms | 3.6 ms | 16.74 ms | **59** |

The LCD transfer is ~80% of a full-render frame and is already 100% efficient: 320\*288\*2 = 184320
bytes at exactly the configured bit rate (predicted 19.66 ms, measured 19.86 — the gap is
CASET/RASET). There is no software waste to reclaim; only fewer bytes or a faster link helps.

### LCD bit rate — do not raise past /4
`set_spi_speed()` takes the **bit rate**: the PIO program is 2 cycles/bit and `setup_pio()` sets
`div = sys_clk / 2 / speed`. Measured on hardware:
- `SYS_CLK_FREQ/2` (150 Mbit/s) — solid garbage. Also the hardware ceiling (PIO clkdiv >= 1.0).
- `SYS_CLK_FREQ/3` (100 Mbit/s) — 51 fps, but **intermittent distortion**. Too close to the cliff.
- `SYS_CLK_FREQ/4` (75 Mbit/s) — stable. Current setting.

Overclocking `clk_sys` is *not* the lever: the panel is the limit, not the RP2350, and the SPI
divisor reaches the same place without touching voltage or QSPI timing.

### frame_skip + pacer
`frame_skip` renders every other frame (`peanut_gb.h:3797`), halving the LCD cost and leaving
headroom over 59.727 Hz, which the pacer spends on running at true speed. Enabled by default —
**after `gb_init_lcd()`, which resets it to 0 (`peanut_gb.h:4168`)**, and after the state restore.
`select + A` toggles at runtime. Trade-off: ~30 Hz display refresh; games that strobe sprites at
30 Hz may drop them.

The pacer **must advance the deadline by exactly one period per frame**, never resync on overrun.
Work alternates ~22.7 ms (rendered) / ~2.8 ms (skipped) and only the *pair* fits in two periods;
resyncing discards the deficit and caps the emulator near 50 fps. A 4-period lag cap prevents
catch-up bursts when the target is unreachable.

### Adaptive audio rate (`ADAPTIVE_AUDIO_RATE`)
Core0 measures production and core1 adopts it via `audio_target_rate` + `i2s_set_sample_freq()`.
**The measurement cannot live on core1:** once the drain rate falls below production, core1's loop
period reflects the drain, agrees with whatever rate is set, and can never climb back out — a
control loop whose sensor sits downstream of its own actuator. Core0 instead subtracts the time it
spends blocked pushing audio (`audio_block_us`), which measures the rate it *could* sustain. Pacer
waits are deliberately not subtracted — that is genuine frame spacing.

Rate is shaded 1% low (back-pressure is inaudible; starvation clicks) and the apply threshold is 2%
— **the threshold must exceed the shade** or the rate ratchets down every window. At 59 fps the
nominal 32768 is already inside the deadband, so the controller correctly never fires.

### minigb_apu `audio_init` bug (worked around in `core1_audio`)
`audio_init()` replays its register defaults in ascending address order, but `audio_write()` drops
every write while the APU is powered off (`minigb_apu.c:402`) — and NR52 is the **last** register it
writes. So NR50 (master volume) and NR51 (routing) never take effect, `vol_l`/`vol_r` stay 0, and
every channel multiplies out to silence. A game booting from scratch sets these itself; we always
resume from a save state past that code, and `apu_ctx` is not part of the saved state. `core1_audio`
re-applies NR50/NR51 after `audio_init`. Wave RAM was never affected (written after NR52).

Diagnosing: `AUDIO_DEBUG 1` prints `peak` (0 = APU silent), `nr52` (bit 7 = powered), `nr50`
(00 = master volume zero, the bug above).

## Known Issues
- **Bubble Bobble (MBC1, 128KB) boots and shows its intro, but resets back to the intro the moment
  gameplay starts.** Everything on this side is cleared: `ROM verify: 131072 bytes OK`,
  `Flash verify ... OK`, `Cart: type=01 romsz=02 ramsz=00 | mbc=1 cart_ram=0 rom_mask=0007` (all
  correct for MBC1/8 banks), no `INVALID OPCODE` from `gb_error`, and `frame_skip` makes no
  difference. Super Mario Land (64KB) works. Remaining suspects are peanut-gb's MBC1/timing
  emulation or the ROM dump itself — next step is running the same file against a stock desktop
  peanut-gb build (`ext/peanut-gb/examples/`).
- `read_gb_emulator_state` overwrites the **whole** `struct gb_s`. The four callback pointers are
  now re-installed after the restore (`gb_rom_read` is the first member, so a state file written by
  a different build installs stale addresses and hangs on the first ROM read). Everything else in
  the struct is still restored blind, including `direct.frame_skip`.
- SRAM is nearly full: text 119832 + bss 362636 = 482468 of 520KB, ~50KB free. A second 184KB frame
  buffer (to overlap transfer with emulation, worth ~50 fps unskipped) does **not** fit.
