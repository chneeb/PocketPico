/**
 * Copyright (C) 2022 by Mahyar Koshkouei <mk@deltabeard.com>
 * Copyright (C) 2024 by Vlastimil Slintak <slintak@uart.cz>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH
 * REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
 * AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
 * LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */

// Peanut-GB emulator settings
#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define ENABLE_SDCARD 1
#define PEANUT_GB_HIGH_LCD_ACCURACY 1
#define PEANUT_GB_USE_BIOS 0
#define PEANUT_FULL_GBC_SUPPORT 0
#define SYS_CLK_FREQ 300 * MHZ

#define ENABLE_DEBUG 1
#define FULL_FRAME_LCD_STAGING 1
/* Periodic APU/PWM/frame-budget diagnostics over USB serial. Set to 1 to get
 * per-interval `AUD` and `FPS: ... lcd/blk/pac/emu` lines; costs a little
 * printf time on both cores, so keep it off normally. */
#define AUDIO_DEBUG 0
/* Pace audio playback at the rate the emulator actually produces samples,
 * rather than the nominal AUDIO_SAMPLE_RATE (which assumes 59.7 fps). */
#define ADAPTIVE_AUDIO_RATE 1

// Display selection
#define USE_ILI9225 0
#define USE_ILI9488 1

/**
 * Reducing VSYNC calculation to lower multiple.
 * When setting a clock IRQ to DMG_CLOCK_FREQ_REDUCED, count to
 * SCREEN_REFRESH_CYCLES_REDUCED to obtain the time required each VSYNC.
 * DMG_CLOCK_FREQ_REDUCED = 2^18, and SCREEN_REFRESH_CYCLES_REDUCED = 4389.
 * Currently unused.
 */
#define VSYNC_REDUCTION_FACTOR 16u
#define SCREEN_REFRESH_CYCLES_REDUCED (SCREEN_REFRESH_CYCLES / VSYNC_REDUCTION_FACTOR)
#define DMG_CLOCK_FREQ_REDUCED (DMG_CLOCK_FREQ / VSYNC_REDUCTION_FACTOR)

/* C Headers */
#include <stdlib.h>
#include <string.h>

/* RP2040 Headers */
#include <hardware/pio.h>
#include <hardware/clocks.h>
#include <hardware/dma.h>
#include <hardware/spi.h>
#include <hardware/sync.h>
#include <hardware/flash.h>
#include <hardware/xip_cache.h>
#include <pico/flash.h>
#include <hardware/timer.h>
#include <hardware/vreg.h>
#include <pico/bootrom.h>
#include <pico/stdlib.h>
#include <pico/multicore.h>
#include <sys/unistd.h>
#include <hardware/irq.h>

/* Project headers */
// #include "pwm_audio.h"
#include "debug.h"
#include "hedley.h"
#include "minigb_apu.h"
#include "sdcard.h"

// #include "i2s.h"
#include "gbcolors.h"

#include "../ext/ili9488_p/mono8x16.h"

/*
#include "ili9488_lcd.h"
#include "ili9488_font.h"
#define SCREEN_WIDTH ILI9488_SCREEN_WIDTH
#define SCREEN_HEIGHT ILI9488_SCREEN_HEIGHT
*/
#include "i2ckbd.h"
#include "picocalc.h"
#define FRAME_BUFF_WIDTH 240
#define FRAME_BUFF_STRIDE (FRAME_BUFF_WIDTH * 2)
#define FRAME_BUFF_HEIGHT 240
#define GAME_FRAME_WIDTH (LCD_WIDTH * 2)
#define GAME_FRAME_HEIGHT (LCD_HEIGHT * 2)
#define GAME_FRAME_STRIDE (GAME_FRAME_WIDTH * 2)

#if ENABLE_SOUND

typedef enum
{
    AUDIO_CMD_IDLE = 0,
    AUDIO_CMD_PLAYBACK,
    AUDIO_CMD_VOLUME_UP,
    AUDIO_CMD_VOLUME_DOWN,
    AUDIO_CMD_INVALID
} audio_commands_e;

#define audio_read(a) audio_read(&apu_ctx, (a))
#define audio_write(a, v) audio_write(&apu_ctx, (a), (v));

/**
 * Global variables for audio task
 * stream contains N=AUDIO_SAMPLES samples
 * each sample is 32 bits
 * 16 bits for the left channel + 16 bits for the right channel in stereo interleaved format)
 * This is intended to be played at AUDIO_SAMPLE_RATE Hz
 */
int16_t *stream;
struct minigb_apu_ctx apu_ctx = {0};

// PWM audio driver
#define AUDIO_DATA_PIN 26
#define AUDIO_CLOCK_PIN 27
#define AUDIO_PWM_PIN 26
#define PIN_SPEAKER 26
#define SPK_LATENCY 256
#define SPK_PWM_FREQ 22050

#include "audio.h"
#include "peanut_gb.h"
#undef audio_read
#undef audio_write
#else
#include "peanut_gb.h"
#endif

/** Definition of ROM data
 * We're going to erase and reprogram a region 1Mb from the start of the flash
 * Once done, we can access this at XIP_BASE + 1Mb.
 * Game Boy DMG ROM size ranges from 32768 bytes (e.g. Tetris) to 1,048,576 bytes (e.g. Pokemod Red)
 */
// #define FLASH_TARGET_OFFSET ((1024 * 1024) + (256 * 1024))
#define FLASH_TARGET_OFFSET (1024 * 1024)
/*
 * Read the ROM through the NOTRANSLATE alias. Measured on hardware after
 * writing a ROM whose header byte 0x100 is 0x00:
 *
 *   cached  (0x10000000) = 2C   nocache (0x14000000) = 2C
 *   notrans (0x1c000000) = 00   direct-SPI probe     = 00
 *
 * So RP2350's QMI address translation was remapping our reads — this was never
 * the XIP cache going stale (the long-standing explanation in this file's
 * history) and never a failed write: erase reads FF, program reads back 00, and
 * a raw 0x03-command probe finds correct data at both 0x100 and 0x10100.
 *
 * Only the NOTRANSLATE alias bypasses translation, and it is also non-cached,
 * so nothing can go stale either. Addresses below 64KB never took this path —
 * they come from rom_bank0, filled straight from the SD buffer — which is why
 * ROMs up to 64KB always worked and anything banked above it executed garbage.
 */
const uint8_t *rom = (const uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + FLASH_TARGET_OFFSET);
static unsigned char rom_bank0[65536];

static uint8_t ram[32768];
static int lcd_line_busy = 0;
static palette_t palette; // Colour palette
static uint8_t manual_palette_selected = 0;

static struct
{
    unsigned a : 1;
    unsigned b : 1;
    unsigned select : 1;
    unsigned start : 1;
    unsigned right : 1;
    unsigned left : 1;
    unsigned up : 1;
    unsigned down : 1;
} prev_joypad_bits;

/* Pixel data for full-frame UI/menu drawing. */
static uint8_t pixels_buffer[FRAME_BUFF_STRIDE * 240 * 2];
_Static_assert(sizeof(pixels_buffer) >= (GAME_FRAME_STRIDE * GAME_FRAME_HEIGHT),
               "pixels_buffer must fit one 2x-scaled game frame");

#if AUDIO_DEBUG
/* Temporary: microseconds spent blocked in the full-frame LCD transfer,
 * accumulated per reporting interval by the draw callback (core0 only). */
static uint32_t lcd_xfer_us = 0;
/* Temporary: per-interval microseconds core0 spent blocked pushing audio, and
 * waiting in the frame pacer. Separate from the adaptive controller's own
 * accumulator, which uses a different window length. */
static uint32_t block_dbg_us = 0;
static uint32_t pacer_dbg_us = 0;
#endif

#if ADAPTIVE_AUDIO_RATE
/* Playback rate in Hz that core0 has measured and wants core1 to run at.
 * 0 until the first window completes. */
static volatile uint32_t audio_target_rate = 0;
/* Microseconds core0 spent blocked pushing audio during the current
 * measurement window. */
static uint32_t audio_block_us = 0;
#endif

/* Ping-pong scanline buffers used by the Game Boy LCD callback while DMA is active. */
#define SCANLINE_BUF_SIZE GAME_FRAME_STRIDE
static uint8_t pixels_buf_a[SCANLINE_BUF_SIZE];
static uint8_t pixels_buf_b[SCANLINE_BUF_SIZE];
static uint8_t *scanline_buf;

/**
 * Returns a byte from the ROM file at the given address.
 */
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    if (addr < sizeof(rom_bank0))
        return rom_bank0[addr];

    return rom[addr];
}

/**
 * Returns a byte from the cartridge RAM at the given address.
 */
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr)
{
    (void)gb;
    return ram[addr];
}

/**
 * Writes a given byte to the cartridge RAM at the given address.
 */
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr,
                       const uint8_t val)
{
    ram[addr] = val;
}

/**
 * Ignore all errors.
 */
void gb_error(struct gb_s *gb, const enum gb_error_e gb_err, const uint16_t addr)
{
#if 1
    const char *gb_err_str[4] = {
        "UNKNOWN",
        "INVALID OPCODE",
        "INVALID READ",
        "INVALID WRITE"};
    DBG_INFO("Error %d occurred: %s at %04X\n.\n", gb_err, gb_err_str[gb_err], addr);
//  abort();
#endif
}

#if ENABLE_LCD
void draw_string(int x, int y, const char *str)
{
    draw_string_rgb565(
        pixels_buffer, FRAME_BUFF_STRIDE, FRAME_BUFF_WIDTH, FRAME_BUFF_HEIGHT,
        x, y, str, 0xffff);
}

__attribute__((optimize("-Og")))
void clear_frame_buff()
{
    for (int i = 0; i < FRAME_BUFF_STRIDE * FRAME_BUFF_HEIGHT * 2; i++)
    {
        pixels_buffer[i] = 0;
    }
}

__attribute__((optimize("-Og")))
void clear_screen_buff()
{
    for (int i = 0; i < (WIDTH)*HEIGHT * 2; i++)
    {
        pixels_buffer[i] = 0;
    }
}

void update_lcd()
{
    start_write_data((WIDTH - FRAME_BUFF_WIDTH) / 2, (HEIGHT - FRAME_BUFF_HEIGHT) / 2,
                     FRAME_BUFF_WIDTH, FRAME_BUFF_HEIGHT, pixels_buffer);
    finish_write_data(true);
}

void update_full_screen()
{
    start_write_data(0, 0, WIDTH, HEIGHT, pixels_buffer);
    finish_write_data(true);
}

void lcd_draw_line(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH],
                   const uint_fast8_t line)
{
    if (line == 0)
        scanline_buf = pixels_buf_a;
    else
        scanline_buf = (scanline_buf == pixels_buf_a) ? pixels_buf_b : pixels_buf_a;

#if PEANUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode)
    {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t color555 = gb->cgb.fixPalette[pixels[x]];
            uint16_t r = (color555 >> 10) & 0x1F;
            uint16_t g = (color555 >> 5) & 0x1F;
            uint16_t b = color555 & 0x1F;
            uint16_t color565 = (r << 11) | ((g << 1) << 5) | b;
            scanline_buf[x * 2] = (uint8_t)(color565 >> 8);
            scanline_buf[x * 2 + 1] = (uint8_t)(color565 & 0xFF);
        }
    }
    else
    {
#endif
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t color = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
            scanline_buf[x * 2] = (uint8_t)(color >> 8);
            scanline_buf[x * 2 + 1] = (uint8_t)(color & 0xFF);
        }
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif

    if (line == 0)
    {
        start_window((WIDTH - LCD_WIDTH) / 2, ((HEIGHT - LCD_HEIGHT) / 2), LCD_WIDTH, LCD_HEIGHT);

        write_data(scanline_buf, LCD_WIDTH);
        finish_write_data(false);
    }
    else if (line == LCD_HEIGHT - 1)
    {
        /* Last scanline: send pixel data and close window. */
        write_data(scanline_buf, LCD_WIDTH);
        finish_write_data(true);
    }
    else
    {
        write_data(scanline_buf, LCD_WIDTH);
        finish_write_data(false);
    }
}

void lcd_draw_line_bis(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH],
                       const uint_fast8_t line)
{
#if FULL_FRAME_LCD_STAGING
    uint8_t *row = pixels_buffer + (line * 2 * GAME_FRAME_STRIDE);
#else
    if (line == 0)
        scanline_buf = pixels_buf_a;
    else
        scanline_buf = (scanline_buf == pixels_buf_a) ? pixels_buf_b : pixels_buf_a;

    uint8_t *row = scanline_buf;
#endif

#if PEANUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode)
    {
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t color555 = gb->cgb.fixPalette[pixels[x]];
            uint16_t r = (color555 >> 10) & 0x1F;
            uint16_t g = (color555 >> 5) & 0x1F;
            uint16_t b = color555 & 0x1F;
            uint16_t pixel = (r << 11) | ((g << 1) << 5) | b;
            row[x * 4] = (uint8_t)(pixel >> 8);
            row[x * 4 + 1] = (uint8_t)(pixel & 0xFF);
            row[x * 4 + 2] = (uint8_t)(pixel >> 8);
            row[x * 4 + 3] = (uint8_t)(pixel & 0xFF);
        }
    }
    else
    {
#endif
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t pixel = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
            row[x * 4] = (uint8_t)(pixel >> 8);
            row[x * 4 + 1] = (uint8_t)(pixel & 0xFF);
            row[x * 4 + 2] = (uint8_t)(pixel >> 8);
            row[x * 4 + 3] = (uint8_t)(pixel & 0xFF);
        }
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif

#if FULL_FRAME_LCD_STAGING
    memcpy(row + GAME_FRAME_STRIDE, row, GAME_FRAME_STRIDE);

    if (line == LCD_HEIGHT - 1)
    {
#if AUDIO_DEBUG
        uint64_t xfer_t0 = time_us_64();
#endif
        start_write_data((WIDTH - GAME_FRAME_WIDTH) / 2,
                         (HEIGHT - GAME_FRAME_HEIGHT) / 2,
                         GAME_FRAME_WIDTH, GAME_FRAME_HEIGHT,
                         pixels_buffer);
        finish_write_data(true);
#if AUDIO_DEBUG
        lcd_xfer_us += (uint32_t)(time_us_64() - xfer_t0);
#endif
    }
#else
    if (line == 0)
    {
        start_window((WIDTH - (LCD_WIDTH * 2)) / 2, ((HEIGHT - (LCD_HEIGHT * 2)) / 2), LCD_WIDTH * 2, LCD_HEIGHT * 2);

        write_data(scanline_buf, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(scanline_buf, LCD_WIDTH * 2);
        finish_write_data(false);
    }
    else if (line == LCD_HEIGHT - 1)
    {
        write_data(scanline_buf, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(scanline_buf, LCD_WIDTH * 2);
        finish_write_data(true);
    }
    else
    {
        write_data(scanline_buf, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(scanline_buf, LCD_WIDTH * 2);
        finish_write_data(false);
    }
#endif
}
#endif

#if ENABLE_SDCARD

/* Subdirectory searched for rom files; the card root is used as a fallback */
#define ROM_DIR "gb"

/**
 * Directory holding save files and emulator states, as a prefix ending in '/'
 * ("" means the card root). The rom selector points this at the directory it
 * listed roms from, so saves sit next to the rom they belong to. The
 * resume-from-flash path (Start) keeps whatever that scan chose.
 */
static const char *save_dir_prefix = "";

/**
 * Build the save file path for the currently loaded rom. The name comes from
 * the cartridge header title, so it is independent of the rom's file name.
 */
static void gb_save_path(struct gb_s *gb, const char *suffix, char *out, size_t out_size)
{
    char title[17]; /* header title is 16 bytes (0x134..0x143) plus terminator */

    gb_get_rom_name(gb, title);
    snprintf(out, out_size, "%s%s%s", save_dir_prefix, title, suffix);
}

/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s *gb)
{
    char filename[48];
    uint_fast32_t save_size;
    UINT br;

    gb_save_path(gb, "", filename, sizeof filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0)
    {
        sd_card_t *pSD = sd_get_by_num(0);
        FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
        if (FR_OK != fr)
        {
            DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }

        FIL fil;
        fr = f_open(&fil, filename, FA_READ);
        if (fr == FR_OK)
        {
            f_read(&fil, ram, f_size(&fil), &br);
        }
        else
        {
            DBG_INFO("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK)
        {
            DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }
        f_unmount(pSD->pcName);
        DBG_INFO("I read_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, save_size);
    }
    else
    {
        DBG_INFO("I read_cart_ram_file(%s) SKIPPED\n", filename);
    }
}

/**
 * Write a save file to the SD card
 */
void write_cart_ram_file(struct gb_s *gb)
{
    char filename[48];
    uint_fast32_t save_size;
    UINT bw = 0; /* reported below even when save_size == 0 skips the write */

    gb_save_path(gb, "", filename, sizeof filename);
    save_size = gb_get_save_size(gb);
    if (save_size > 0)
    {
        sd_card_t *sd = sd_get_by_num(0);
        FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);
        if (FR_OK != fr)
        {
            DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
            return;
        }

        FIL fil;
        fr = f_open(&fil, filename, FA_CREATE_ALWAYS | FA_WRITE);
        if (fr == FR_OK)
        {
            f_write(&fil, ram, save_size, &bw);
        }
        else
        {
            DBG_INFO("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr);
        }

        fr = f_close(&fil);
        if (fr != FR_OK)
        {
            DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
        }

        f_unmount(sd->pcName);
    }

    DBG_INFO("I write_cart_ram_file(%s) COMPLETE (%lu bytes)\n", filename, bw);
}

/**
 * Read a save file with internal GB enumalor state from the SD card.
 * This state will allow to resume game from the last run.
 */
void read_gb_emulator_state(struct gb_s *gb)
{
    char filename_state[48];
    UINT br = 0;
    FIL fil;

    sd_card_t *sd = sd_get_by_num(0);
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);

    gb_save_path(gb, "_state.bin", filename_state, sizeof filename_state);
    fr = f_open(&fil, filename_state, FA_READ);

    if (fr == FR_OK)
    {
        f_read(&fil, (uint8_t *)gb, sizeof(struct gb_s), &br);
    }
    else
    {
        DBG_INFO("W read_gb_emulator_state(%s): SKIPPED (no previous state)\n", filename_state);
        /* f_open failed, so there is nothing to close — falling through to
         * finish: would call f_close on an invalid FIL (FR_INVALID_OBJECT). */
        f_unmount(sd->pcName);
        return;
    }

    DBG_INFO("I read_gb_emulator_state(%s) COMPLETED (%lu bytes)\n", filename_state, br);

finish:
    fr = f_close(&fil);
    if (fr != FR_OK)
    {
        DBG_INFO("W f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }

    f_unmount(sd->pcName);
}

/**
 * Write a save file with internal GB enumalor state to the SD card.
 * When loaded, this state will allow to resume game from the last run.
 */
void write_gb_emulator_state(struct gb_s *gb)
{
    char filename_state[48];
    UINT bw;
    FIL fil;

    sd_card_t *sd = sd_get_by_num(0);
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);

    gb_save_path(gb, "_state.bin", filename_state, sizeof filename_state);
    fr = f_open(&fil, filename_state, FA_CREATE_ALWAYS | FA_WRITE);

    if (fr == FR_OK)
    {
        f_write(&fil, (uint8_t *)gb, sizeof(struct gb_s), &bw);
    }
    else
    {
        DBG_INFO("E write_gb_emulator_state(%s) FAILED (%s)\n", filename_state, FRESULT_str(fr));
        goto finish;
    }

    DBG_INFO("I write_gb_emulator_state(%s) COMPLETED (%lu bytes)\n", filename_state, bw);

finish:
    fr = f_close(&fil);
    if (fr != FR_OK)
    {
        DBG_INFO("E f_close error: %s (%d)\n", FRESULT_str(fr), fr);
    }
    f_unmount(sd->pcName);
}

/**
 * Load a .gb rom file in flash from the SD card
 */
/* Saved during load for post-load diagnostics */
static uint8_t rom_sd_hdr_byte    = 0xFF;
static uint8_t rom_sd_stored_ck   = 0xFF;
static uint8_t rom_sd_computed_ck = 0xFF;
static uint8_t rom_buf_hdr_byte   = 0xFF; /* buffer[0x100] from SD card — never overwritten */
static int     last_fse_ret       = 999;  /* 999 = never called */
static uint8_t pre_write_hdr      = 0xFF; /* flash byte 0x100 before write */
static uint8_t post_erase_raw     = 0xFF; /* raw direct read after erase (should be 0xFF) */
static uint8_t post_prog_raw      = 0xFF; /* raw direct read after program (should be 0x00) */
static bool    rom_bank0_ready    = false; /* true when load_cart_rom_file filled rom_bank0 from SD */

typedef struct { uint32_t offset; const uint8_t *data; } flash_sector_args_t;

/* Direct SPI read of one flash byte, bypassing XIP entirely. */
static void __no_inline_not_in_flash_func(flash_raw_read_byte)(uint32_t addr, uint8_t *out)
{
    uint8_t tx[5] = {0x03,
                     (uint8_t)(addr >> 16),
                     (uint8_t)(addr >>  8),
                     (uint8_t)(addr),
                     0x00};
    uint8_t rx[5] = {0, 0, 0, 0, 0};
    flash_do_cmd(tx, rx, 5);
    *out = rx[4];
}

#if AUDIO_DEBUG
/* Ground truth: read a byte straight off the chip with a 0x03 command,
 * bypassing XIP, its cache, and any QMI translation. This is what proved the
 * QMI translation bug — see the alias table in CLAUDE.md. */
static uint32_t   raw_probe_addr = 0;
static uint8_t    raw_probe_val  = 0xFF;
static void __no_inline_not_in_flash_func(do_flash_raw_probe)(void *arg)
{
    (void)arg;
    flash_raw_read_byte(raw_probe_addr, &raw_probe_val);
}

static uint8_t flash_raw_probe(uint32_t addr)
{
    raw_probe_addr = addr;
    raw_probe_val = 0xFF;
    flash_safe_execute(do_flash_raw_probe, NULL, 5000);
    return raw_probe_val;
}
#endif

/*
 * Re-initialise the XIP read path after programming.
 *
 * flash_range_erase()/flash_range_program() call flash_exit_xip() on entry but
 * only flash_flush_cache() on exit — they never re-enter XIP (pico-sdk
 * hardware_flash/flash.c). A flash-resident binary gets away with it; this one
 * is copy_to_ram, so no code is fetched from flash and a broken XIP read path
 * fails silently. Every read then returns pre-write data — through the cached
 * *and* non-cached aliases alike — which is why "Flash verify ... MISMATCH" has
 * always been logged while direct-SPI probes show the chip holding correct
 * data. Invisible for ROMs under 64KB (served from rom_bank0), fatal above it.
 *
 * flash_start_xip() does the full connect / exit / flush / enter_cmd_xip
 * sequence. It leaves XIP in the generic 03h serial read mode, which is slower
 * than the boot-time configuration but correct, and only bank-switched reads
 * above 64KB take that path.
 */
static void __no_inline_not_in_flash_func(do_flash_restart_xip)(void *arg)
{
    (void)arg;
    flash_start_xip();
}

static void __no_inline_not_in_flash_func(do_flash_sector)(void *arg)
{
    const flash_sector_args_t *a = (const flash_sector_args_t *)arg;
    flash_range_erase(a->offset, FLASH_SECTOR_SIZE);
    /* Raw reads only for sector 0 — this is what WR hdr/XIP also reads */
    if (a->offset == FLASH_TARGET_OFFSET) {
        flash_raw_read_byte(FLASH_TARGET_OFFSET + 0x100, &post_erase_raw);
    }
    flash_range_program(a->offset, a->data, FLASH_SECTOR_SIZE);
    if (a->offset == FLASH_TARGET_OFFSET) {
        flash_raw_read_byte(FLASH_TARGET_OFFSET + 0x100, &post_prog_raw);
    }
}

bool load_cart_rom_file(char *filename)
{
    UINT br;
    static uint8_t buffer[FLASH_SECTOR_SIZE];
    sd_card_t *pSD = sd_get_by_num(0);
    last_fse_ret = 999;
    DBG_INFO("LC1: mount\n"); stdio_flush();
    FRESULT fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fr)
    {
        DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr); stdio_flush();
        return false;
    }
    DBG_INFO("LC2: open %s\n", filename); stdio_flush();
    FIL fil;
    bool success = false;
    uint32_t sector_num = 0;
    fr = f_open(&fil, filename, FA_READ);
    if (fr == FR_OK)
    {
        uint32_t flash_target_offset = FLASH_TARGET_OFFSET;
        for (;;)
        {
            FRESULT frd = f_read(&fil, buffer, sizeof buffer, &br);
            if (frd != FR_OK || br == 0)
                break;

            /* On the first sector, capture header bytes and record pre-write flash state */
            if (sector_num == 0 && br >= 0x14E)
            {
                rom_buf_hdr_byte = buffer[0x100]; /* SD card value, never overwritten */
                rom_sd_hdr_byte = buffer[0x100];
                rom_sd_stored_ck = buffer[0x14D];
                uint8_t ck = 0;
                for (int i = 0x134; i <= 0x14C; i++)
                    ck = ck - buffer[i] - 1;
                rom_sd_computed_ck = ck;
                DBG_INFO("SD s0: hdr[100]=%02X ck computed=%02X stored=%02X %s\n",
                         rom_sd_hdr_byte, rom_sd_computed_ck, rom_sd_stored_ck,
                         (rom_sd_computed_ck == rom_sd_stored_ck) ? "OK" : "MISMATCH");
                /* snapshot flash before any write */
                flash_flush_cache();
                pre_write_hdr = rom[0x100];
                DBG_INFO("Pre-write flash[100]=%02X\n", pre_write_hdr);
                stdio_flush();
            }

            DBG_INFO("LC3/4: flash s%lu\n", sector_num); stdio_flush();
            flash_sector_args_t args = { flash_target_offset, buffer };
            int fse_ret = flash_safe_execute(do_flash_sector, &args, 5000);
            last_fse_ret = fse_ret;
            DBG_INFO("FSE s%lu ret=%d\n", sector_num, fse_ret); stdio_flush();
            if (fse_ret != PICO_OK)
            {
                DBG_INFO("E flash_safe_execute failed: %d — aborting\n", fse_ret); stdio_flush();
                break;
            }
            /* Fill rom_bank0 directly from SD card data — bypasses XIP cache entirely */
            {
                uint32_t offset_in_rom = flash_target_offset - FLASH_TARGET_OFFSET;
                if (offset_in_rom < sizeof(rom_bank0)) {
                    uint32_t copy_len = br;
                    if (offset_in_rom + copy_len > (uint32_t)sizeof(rom_bank0))
                        copy_len = (uint32_t)sizeof(rom_bank0) - offset_in_rom;
                    memcpy((uint8_t *)rom_bank0 + offset_in_rom, buffer, copy_len);
                }
            }
            flash_target_offset += FLASH_SECTOR_SIZE;
            sector_num++;
        }
        success = (sector_num > 0 && last_fse_ret == PICO_OK);
        if (success) rom_bank0_ready = true;
        /* Verify header bytes in flash match what was read from SD */
        if (sector_num > 0) {
            /* Restore the XIP read path the flash writes tore down, then drop
             * anything the cache retained from before it worked. */
            flash_safe_execute(do_flash_restart_xip, NULL, 5000);
            xip_cache_invalidate_all();

            const uint8_t *xip_hdr = rom;
            rom_sd_hdr_byte      = xip_hdr[0x100]; /* reuse globals: now holds post-write XIP values */
            rom_sd_stored_ck     = xip_hdr[0x14D];
            uint8_t ck = 0;
            for (int i = 0x134; i <= 0x14C; i++)
                ck = ck - xip_hdr[i] - 1;
            rom_sd_computed_ck = ck;
            DBG_INFO("Flash verify: hdr[100]=%02X ck=%02X/%02X %s\n",
                     xip_hdr[0x100], ck, xip_hdr[0x14D],
                     (ck == xip_hdr[0x14D]) ? "OK" : "MISMATCH"); stdio_flush();

            /* Compare the XIP view against the chip itself. s0 raw values are
             * captured inside do_flash_sector immediately after erase (expect
             * FF) and after program (expect the SD byte). low/high are fresh
             * probes at the start of the ROM and inside bank 4 (>64KB, the
             * region rom_bank0 does not cover and banked games actually run
             * from). Disagreement between raw and XIP means the read path is at
             * fault; agreement on the pre-write value means the write is. */
#if AUDIO_DEBUG
            DBG_INFO("Raw: er=%02X pr=%02X low=%02X high=%02X | SD=%02X XIP=%02X\n",
                     post_erase_raw, post_prog_raw,
                     flash_raw_probe(FLASH_TARGET_OFFSET + 0x100),
                     flash_raw_probe(FLASH_TARGET_OFFSET + 0x10100),
                     rom_buf_hdr_byte, xip_hdr[0x100]); stdio_flush();

            /*
             * Same byte through every XIP alias, against the chip itself.
             * volatile so none of these can be folded into a single load.
             *   cached  0x10000000 — normal view
             *   nocache 0x14000000 — bypasses the XIP cache
             *   notrans 0x1c000000 — also bypasses QMI address translation
             * If notrans alone matches raw, translation is remapping our reads.
             * If none match raw, the fault is below all of them.
             */
            {
                const volatile uint8_t *a_cached =
                    (const volatile uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
                const volatile uint8_t *a_nocache =
                    (const volatile uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + FLASH_TARGET_OFFSET);
                const volatile uint8_t *a_notrans =
                    (const volatile uint8_t *)(XIP_NOCACHE_NOALLOC_NOTRANSLATE_BASE + FLASH_TARGET_OFFSET);

                DBG_INFO("Alias: cached=%02X nocache=%02X notrans=%02X | raw=%02X SD=%02X\n",
                         a_cached[0x100], a_nocache[0x100], a_notrans[0x100],
                         flash_raw_probe(FLASH_TARGET_OFFSET + 0x100),
                         rom_buf_hdr_byte); stdio_flush();
            }
#endif
        }
        DBG_INFO("I load_cart_rom_file(%s) %s (%lu sectors, fse=%d)\n",
                 filename, success ? "OK" : "FAIL", sector_num, last_fse_ret); stdio_flush();

        /* Full byte-for-byte check of flash against the source file. The header
         * spot-check only proves sector 0; a banked game reads all of it. */
        if (success && f_lseek(&fil, 0) == FR_OK)
        {
            uint32_t off = 0, bad = 0;
            uint32_t first_bad = 0xFFFFFFFF;

            for (;;)
            {
                if (f_read(&fil, buffer, sizeof buffer, &br) != FR_OK || br == 0)
                    break;

                for (UINT i = 0; i < br; i++)
                {
                    /* Check both sources gb_rom_read() uses: rom_bank0 (RAM,
                     * filled from the SD buffer) below 64KB, flash above it. */
                    uint8_t got = (off + i) < sizeof(rom_bank0)
                                      ? rom_bank0[off + i]
                                      : rom[off + i];

                    if (got != buffer[i])
                    {
                        if (first_bad == 0xFFFFFFFF)
                            first_bad = off + i;
                        bad++;
                    }
                }
                off += br;
            }

            if (bad == 0)
                DBG_INFO("ROM verify: %lu bytes OK\n", off);
            else
                DBG_INFO("ROM verify: %lu of %lu bytes differ, first at 0x%lX (flash=%02X file=%02X)\n",
                         bad, off, first_bad, rom[first_bad],
                         (unsigned)(first_bad < sizeof(rom_bank0) ? rom_bank0[first_bad] : 0));
            stdio_flush();
        }

        f_close(&fil);
    }
    else
    {
        DBG_INFO("E f_open(%s) error: %s (%d)\n", filename, FRESULT_str(fr), fr); stdio_flush();
    }

    f_unmount(pSD->pcName);
    return success;
}

/**
 * Start a rom search in ROM_DIR, falling back to the card root when that
 * directory is missing or holds no rom files. On return *prefix is the string
 * to prepend to fno->fname to obtain a path usable with f_open().
 */
static FRESULT rom_dir_findfirst(DIR *dj, FILINFO *fno, const char **prefix)
{
    FRESULT fr = f_findfirst(dj, fno, ROM_DIR, "?*.gb");
    if (fr == FR_OK && fno->fname[0])
    {
        *prefix = ROM_DIR "/";
        return fr;
    }

    if (fr == FR_OK)
        f_closedir(dj); /* directory exists but contains no roms */

    DBG_INFO("Selector: no roms in %s/ (fr=%d), using card root\n", ROM_DIR, fr);
    *prefix = "";
    return f_findfirst(dj, fno, ".", "?*.gb");
}

/**
 * Return the file name part of a rom path stored by the selector
 */
static const char *rom_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/**
 * Function used by the rom file selector to display one page of .gb rom files
 */
uint16_t rom_file_selector_display_page(char filename[22][256], uint16_t num_page)
{
    sd_card_t *pSD = sd_get_by_num(0);
    DIR dj;
    FILINFO fno;
    FRESULT fr;

    fr = f_mount(&pSD->fatfs, pSD->pcName, 1);
    if (FR_OK != fr)
    {
        DBG_INFO("E f_mount error: %s (%d)\n", FRESULT_str(fr), fr);
        return 0;
    }

    /* clear the filenames array */
    for (uint8_t ifile = 0; ifile < 22; ifile++)
    {
        strcpy(filename[ifile], "");
    }

    /* search *.gb files in ROM_DIR, falling back to the card root */
    uint16_t num_file = 0;
    const char *prefix = "";
    fr = rom_dir_findfirst(&dj, &fno, &prefix);

    /* keep saves and states in the directory the roms were listed from */
    save_dir_prefix = prefix;

    /* skip the first N pages */
    if (num_page > 0)
    {
        while (num_file < num_page * 22 && fr == FR_OK && fno.fname[0])
        {
            num_file++;
            fr = f_findnext(&dj, &fno);
        }
    }

    /* store the filenames of this page */
    num_file = 0;
    while (num_file < 22 && fr == FR_OK && fno.fname[0])
    {
        if (fno.fname[0] != '.')
        {
            /* Skip any file starting with dot. These are hidden files. */
            snprintf(filename[num_file], 256, "%s%s", prefix, fno.fname);
            num_file++;
        }

        fr = f_findnext(&dj, &fno);
    }
    f_closedir(&dj);
    f_unmount(pSD->pcName);

    /* display *.gb rom files on screen */
    clear_frame_buff();
    for (uint8_t ifile = 0; ifile < num_file; ifile++)
    {
        DBG_INFO("Game: %s\n", filename[ifile]);
        draw_string(20, ifile * 20, rom_basename(filename[ifile]));
    }
    DBG_INFO("DP: update_lcd\n"); stdio_flush();
    update_lcd();
    DBG_INFO("DP: done\n"); stdio_flush();
    return num_file;
}

/**
 * The ROM selector displays pages of up to 22 rom files
 * allowing the user to select which rom file to start
 * Copy your *.gb rom files to the "gb" directory of the SD card
 * (the root directory is used when that directory holds no roms)
 */
void rom_file_selector()
{
    DBG_INFO("ROM File Selector: Starting...\n");
    uint16_t num_page = 0;
    static char filename[22][256];
    uint16_t num_file;
    char buf[32];
    bool break_outer = false;

    /* display the first page with up to 22 rom files */
    num_file = rom_file_selector_display_page(filename, num_page);
    DBG_INFO("ROM File Selector: Found %d files on first page\n", num_file);

    /* select the first rom */
    uint8_t selected = 0;
    DBG_INFO("ROM File Selector: Waiting 5 seconds before highlighting first ROM\n");

    DBG_INFO("ROM File Selector: Highlighting first ROM: %s\n", filename[selected]);
    sprintf(buf, "%02d", selected + 1);
    draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
    draw_string(0, (selected % 22) * 20, "=>");
    update_lcd();

    /* get user's input */
    bool up = true, down = true, left = true, right = true, a = true, b = true, select = true, start = true;
    while (true)
    {
        switch (wait_key())
        {
        case KEY_A:
        case KEY_B:
        case KEY_START:
            rom_file_selector_display_page(filename, num_page);
            snprintf(buf, sizeof(buf), "Loading %s", rom_basename(filename[selected]));
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            DBG_INFO("Load: update_lcd\n"); stdio_flush();
            update_lcd();
            DBG_INFO("Load: calling load_cart_rom_file\n"); stdio_flush();
            if (load_cart_rom_file(filename[selected]))
            {
                DBG_INFO("Load: load_cart_rom_file OK\n"); stdio_flush();
                break_outer = true;
            }
            else
            {
                DBG_INFO("Load: load_cart_rom_file FAILED\n"); stdio_flush();
                draw_string(0, FRAME_BUFF_HEIGHT - 20, "Load FAILED");
                update_lcd();
                sleep_ms(2000);
            }
            break;

        case KEY_UP:
            DBG_INFO("ROM File Selector: Up button - selecting previous ROM\n");
            rom_file_selector_display_page(filename, num_page);
            draw_string(0, (selected % 22) * 20, "");
            if (selected == 0)
            {
                selected = num_file - 1;
            }
            else
            {
                selected--;
            }
            DBG_INFO("ROM File Selector: Selected ROM: %s\n", filename[selected]);
            sprintf(buf, "%02d", selected + 1);
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            draw_string(0, (selected % 22) * 20, "=>");
            update_lcd();
            sleep_ms(150);
            break;

        case KEY_DOWN:

            DBG_INFO("ROM File Selector: Down button - selecting next ROM\n");
            rom_file_selector_display_page(filename, num_page);
            selected++;
            if (selected >= num_file)
                selected = 0;
            DBG_INFO("DW:1 sel=%d\n", selected); stdio_flush();
            sprintf(buf, "%02d", selected + 1);
            DBG_INFO("DW:2\n"); stdio_flush();
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            DBG_INFO("DW:3\n"); stdio_flush();
            draw_string(0, (selected % 22) * 20, "=>");
            DBG_INFO("DW:4\n"); stdio_flush();
            update_lcd();
            DBG_INFO("DW:5\n"); stdio_flush();
            sleep_ms(150);
            break;
        }

        if (break_outer)
            break;
    }

    DBG_INFO("ROM File Selector: Exiting selector\n");
}

#endif

#if ENABLE_SOUND

void core1_audio(void)
{
    /*
     * Do NOT call flash_safe_execute_core_init() here. It installs
     * multicore_lockout_handler() as an exclusive handler on this core's SIO
     * FIFO IRQ, and that handler drains and discards every FIFO word that is
     * not LOCKOUT_MAGIC_START — including our AUDIO_CMD_* commands, which then
     * never reach the multicore_fifo_pop_blocking_inline() below (silence, with
     * no other visible symptom). This core is instead declared safe via
     * PICO_FLASH_ASSUME_CORE1_SAFE=1: copy_to_ram puts all code and rodata in
     * SRAM, so core1 touches no XIP while core0 erases/programs flash.
     */

    /* Allocate memory for the stream buffer */
    stream = malloc(AUDIO_SAMPLES_TOTAL * sizeof(int16_t));
    assert(stream != NULL);
    memset(stream, 0, AUDIO_SAMPLES_TOTAL * sizeof(int16_t));

    /* Initialize the PWM/I2S sound driver (see AUDIO_PWM_PIN note in audio.c) */
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 4);
    i2s_init(&i2s_config);

    /* Initialize audio emulation. */
    audio_init(&apu_ctx);

    /*
     * Work around a minigb_apu bug: audio_init() replays its register defaults
     * in ascending address order, but audio_write() discards every write while
     * the APU is powered off (NR52 == 0) — and NR52 is the *last* register it
     * writes. So NR50 (master volume) and NR51 (channel routing) never take
     * effect, ctx->vol_l/vol_r stay 0, and every channel multiplies out to
     * silence no matter how loudly it triggers. A game booting from scratch
     * sets these itself during its audio init, but we always resume from a save
     * state well past that code, and apu_ctx is not part of the saved state —
     * so nothing ever restores them. NR52 is set by now, so these stick.
     */
    audio_write(&apu_ctx, 0xFF24, 0x77); /* NR50: master volume, both channels */
    audio_write(&apu_ctx, 0xFF25, 0xF3); /* NR51: default channel routing */

    DBG_INFO("I Audio ready on core1. samples=%u dma_ch=%u vol=%u nr50=%02X\n",
             (unsigned)AUDIO_SAMPLES, i2s_config.dma_channel, i2s_config.volume,
             audio_read(&apu_ctx, 0xFF24));

    while (1)
    {
        audio_commands_e cmd = multicore_fifo_pop_blocking_inline();
        switch (cmd)
        {
        case AUDIO_CMD_PLAYBACK:
#if ADAPTIVE_AUDIO_RATE
            /*
             * Adopt the rate core0 measured. The measurement cannot be done
             * here: once the drain rate falls below production this loop
             * becomes the bottleneck, so its period reflects the drain rather
             * than production, agrees with whatever rate is already set, and
             * can never climb back out. Core0 measures it instead, excluding
             * the time it spends back-pressured by us.
             *
             * Ignore drift under 2% so the rate does not thrash; that threshold
             * is deliberately larger than core0's 1% safety shade, so the shade
             * cannot ratchet the rate down window after window.
             */
            {
                uint32_t want = audio_target_rate;
                uint32_t cur = i2s_config.sample_freq;

                if (want != 0 &&
                    (want > cur ? want - cur : cur - want) > cur / 50u)
                {
                    i2s_set_sample_freq(&i2s_config, want);
                    DBG_INFO("AUD rate %lu -> %lu Hz\n", cur, want);
                }
            }
#endif
            audio_callback(&apu_ctx, stream);
#if AUDIO_DEBUG
            /* Every ~2s report whether the APU is producing anything and what
             * actually reaches the PWM compare register. peak==0 means the APU
             * is silent; peak>0 with a flat pwm value means the output stage
             * is at fault. */
            {
                static uint32_t cb_count = 0;
                if ((cb_count++ % 120) == 0)
                {
                    int16_t peak = 0;
                    for (unsigned i = 0; i < AUDIO_SAMPLES_TOTAL; i++)
                    {
                        int16_t v = stream[i] < 0 ? (int16_t)-stream[i] : stream[i];
                        if (v > peak)
                            peak = v;
                    }
                    DBG_INFO("AUD n=%lu peak=%d nr52=%02X nr50=%02X pwm=%u\n",
                             cb_count, peak,
                             audio_read(&apu_ctx, 0xFF26),
                             audio_read(&apu_ctx, 0xFF24),
                             i2s_config.dma_buf ? i2s_config.dma_buf[0] : 0);
                }
            }
#endif
            i2s_dma_write(&i2s_config, stream);
            break;

        case AUDIO_CMD_VOLUME_UP:
            i2s_increase_volume(&i2s_config);
            break;

        case AUDIO_CMD_VOLUME_DOWN:
            i2s_decrease_volume(&i2s_config);
            break;

        default:
            break;
        }
    }

    HEDLEY_UNREACHABLE();
}
#endif

/* Real Game Boy frame period: SCREEN_REFRESH_CYCLES / DMG_CLOCK_FREQ, i.e.
 * 70224 / 4194304 Hz = 59.727 fps = 16742 us. */
/* Integer, so the pacer does no floating-point work per frame — the source
 * constants are doubles (peanut_gb.h:164). Truncating 16742.7 to 16742 costs
 * 0.004% in speed, which is nothing next to the 1% audio safety shade. */
#define GB_FRAME_PERIOD_US ((uint64_t)(1000000.0 * SCREEN_REFRESH_CYCLES / DMG_CLOCK_FREQ))

int main(void)
{
    static struct gb_s gb;
    enum gb_init_error_e ret;

    /* Overclock to 300 MHZ. */
    vreg_set_voltage(VREG_VOLTAGE_1_30);
    sleep_ms(100);
    set_sys_clock_khz(SYS_CLK_FREQ / 1000, true);

    DBG_INIT();
    DBG_INFO("INIT: ");

#if ENABLE_SOUND
    multicore_launch_core1(core1_audio);
#endif

#if ENABLE_LCD
    init(SYS_CLK_FREQ);
    start_game();
    clear_screen_buff();
    update_full_screen();
#endif

    init_i2c_kbd(); // Init keyboard
    device_init();  // Init device

    while (true)
    {

#if ENABLE_SDCARD
        /* ROM File selector */
        rom_file_selector();
#endif

#if ENABLE_LCD
        /*
         * The argument is the LCD bit rate: the PIO program spends 2 cycles per
         * bit and setup_pio() sets div = sys_clk / 2 / speed, so speed is what
         * actually reaches the panel. The full-frame transfer is 320*288*2
         * bytes, and at SYS_CLK_FREQ/4 (75 Mbit/s) it blocked ~19.9 ms of every
         * 24.9 ms frame — 80% of the budget, capping the emulator at 40 fps.
         *
         * Measured on hardware (ILI9488 over the PicoCalc ribbon):
         *   SYS_CLK_FREQ/2 (150 Mbit/s) — solid garbage, panel cannot take it.
         *   SYS_CLK_FREQ/3 (100 Mbit/s) — ~14.9 ms, 51 fps, but intermittent
         *                                 distortion; too close to the cliff.
         *   SYS_CLK_FREQ/4  (75 Mbit/s) — ~19.9 ms, 40 fps, stable.
         * Back at /4 until the safe ceiling between 75 and 100 is bisected.
         */
        set_spi_speed(SYS_CLK_FREQ / 4);
        clear_frame_buff();
        update_lcd();
#endif
        /* Initialise GB context. */
        xip_cache_invalidate_all();
        if (!rom_bank0_ready) {
            /* resume path: fill rom_bank0 from XIP (cache freshly invalidated above) */
            memcpy(rom_bank0, rom, sizeof(rom_bank0));
        }
        rom_bank0_ready = false;
        ret = gb_init(&gb, &gb_rom_read, &gb_cart_ram_read,
                      &gb_cart_ram_write, &gb_error, NULL);
        DBG_INFO("GB ");

#if AUDIO_DEBUG
        /* Cartridge hardware as declared by the header vs. what gb_init made of
         * it. A wrong num_rom_banks_mask silently truncates bank switches, which
         * lands the CPU in the wrong bank — the classic "game resets itself"
         * symptom, with no invalid opcode because the wrong bank is still valid
         * code. header: 0x147 type, 0x148 rom size, 0x149 ram size. */
        DBG_INFO("Cart: type=%02X romsz=%02X ramsz=%02X | mbc=%d cart_ram=%d "
                 "rom_mask=%04X ram_banks=%d save=%lu\n",
                 rom_bank0[0x147], rom_bank0[0x148], rom_bank0[0x149],
                 (int)gb.mbc, (int)gb.cart_ram,
                 (unsigned)gb.num_rom_banks_mask, (int)gb.num_ram_banks,
                 (unsigned long)gb_get_save_size(&gb));
        stdio_flush();
#endif

        if (ret != GB_INIT_NO_ERROR)
        {
            /* Compute what gb_init saw so user can diagnose without USB */
            uint8_t computed_ck = 0;
            for (int i = 0x134; i <= 0x14C; i++)
                computed_ck = computed_ck - rom_bank0[i] - 1;
            uint8_t stored_ck = rom_bank0[0x14D];
            DBG_INFO("GB init error: %d  hdr[0x100]=%02X ck computed=%02X stored=%02X\n",
                     ret, rom_bank0[0x100], computed_ck, stored_ck);
            DBG_INFO("SD values: hdr=%02X ck=%02X/%02X\n",
                     rom_sd_hdr_byte, rom_sd_computed_ck, rom_sd_stored_ck);

            char errbuf[40];
            /* Line 1: FSE result, SD buf byte, pre-write flash byte */
            snprintf(errbuf, sizeof(errbuf), "FSE=%d buf=%02X pre=%02X",
                     last_fse_ret, rom_buf_hdr_byte, pre_write_hdr);
            draw_string(0, FRAME_BUFF_HEIGHT / 2 - 80, errbuf);
            /* Line 2: raw direct-SPI reads (bypass XIP): after erase and after program */
            snprintf(errbuf, sizeof(errbuf), "er=%02X pr=%02X (raw)",
                     post_erase_raw, post_prog_raw);
            draw_string(0, FRAME_BUFF_HEIGHT / 2 - 60, errbuf);
            /* Line 3: post-write XIP readback */
            snprintf(errbuf, sizeof(errbuf), "WR hdr=%02X ck=%02X/%02X%s",
                     rom_sd_hdr_byte, rom_sd_computed_ck, rom_sd_stored_ck,
                     (rom_sd_computed_ck == rom_sd_stored_ck) ? " OK" : " BAD");
            draw_string(0, FRAME_BUFF_HEIGHT / 2 - 40, errbuf);
            /* Line 4: what gb_init read from rom_bank0 */
            snprintf(errbuf, sizeof(errbuf), "RD hdr=%02X ck=%02X/%02X%s",
                     rom_bank0[0x100], computed_ck, stored_ck,
                     (computed_ck == stored_ck) ? " OK" : " BAD");
            draw_string(0, FRAME_BUFF_HEIGHT / 2 - 20, errbuf);
            /* Line 5: do WR and RD agree? */
            bool wr_rd_match = (rom_sd_hdr_byte == rom_bank0[0x100]) &&
                               (rom_sd_stored_ck == stored_ck);
            snprintf(errbuf, sizeof(errbuf), "WR==RD: %s", wr_rd_match ? "YES" : "NO");
            draw_string(0, FRAME_BUFF_HEIGHT / 2, errbuf);
            if (ret == GB_INIT_INVALID_CHECKSUM)
                draw_string(0, FRAME_BUFF_HEIGHT / 2 + 20, "Bad checksum");
            else if (ret == GB_INIT_CARTRIDGE_UNSUPPORTED)
                draw_string(0, FRAME_BUFF_HEIGHT / 2 + 20, "Unsupported cart");
            update_lcd();
            sleep_ms(5000);
            goto out;
        }

#if ENABLE_SDCARD
        /* Try to load last saved emulator state for this game. */
        read_gb_emulator_state(&gb);

        /*
         * Re-install the callbacks. read_gb_emulator_state() overwrites the
         * whole struct gb_s, and the function pointers are its first members —
         * so a state file restores addresses captured under whatever firmware
         * build saved it. Any rebuild moves those addresses, and calling
         * through a stale gb_rom_read hangs on the first ROM access, which
         * looks like a freeze at startup. gb_init_lcd() below happens to repair
         * display.lcd_draw_line; nothing repaired these four.
         */
        gb.gb_rom_read = &gb_rom_read;
        gb.gb_cart_ram_read = &gb_cart_ram_read;
        gb.gb_cart_ram_write = &gb_cart_ram_write;
        gb.gb_error = &gb_error;
#endif

        /* Automatically assign a colour palette to the game */
        char rom_title[17]; /* header title is 16 bytes (0x134..0x143) plus terminator */
        auto_assign_palette(palette, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));

#if ENABLE_LCD
        gb_init_lcd(&gb, &lcd_draw_line_bis);
        DBG_INFO("LCD ");
#endif

        /*
         * Render every other frame. The LCD transfer is ~19.9 ms of a 24.9 ms
         * frame, which caps full rendering at ~40 fps — 67% of real speed, and
         * the reason both the game and its audio ran slow. Halving the render
         * rate leaves headroom over 59.727 Hz, which the pacer spends on
         * running at true speed instead. Display refresh drops to ~30 Hz;
         * select + A toggles this at runtime.
         *
         * Must come after gb_init_lcd(), which resets frame_skip to 0
         * (peanut_gb.h:4168) — and after the state restore, which would
         * otherwise carry over whatever the flag was when the state was saved.
         */
        gb.direct.frame_skip = 1;

#if ENABLE_SDCARD
        /* Load Save File. */
        read_cart_ram_file(&gb);
#endif

        DBG_INFO("\n> ");
        uint_fast32_t frames = 0;
        uint64_t start_time = time_us_64();
        uint64_t next_frame_at = time_us_64();
        while (1)
        {
            int input;

            /*
             * Throttle to the real Game Boy frame rate. This only ever limits:
             * when the emulator cannot keep up (frame_skip off) `now` is always
             * past the deadline and it never waits, so the fallback is simply
             * the old behaviour.
             */
            {
                uint64_t now;

                /*
                 * Advance the deadline by exactly one period every frame, so a
                 * frame that overruns is paid back by the next short one. This
                 * matters with frame_skip, where work alternates ~22.7 ms
                 * (rendered) and ~2.8 ms (skipped): only the *pair* fits in two
                 * periods. Resyncing on every overrun instead — as this did
                 * originally — discards the deficit, so the short frame waits a
                 * full fresh period and the pair costs 22.7 + 16.7 ms rather
                 * than 2 x 16.742, capping the emulator near 50 fps.
                 */
                next_frame_at += GB_FRAME_PERIOD_US;
                now = time_us_64();

                if (now < next_frame_at)
                {
#if AUDIO_DEBUG
                    pacer_dbg_us += (uint32_t)(next_frame_at - now);
#endif
                    busy_wait_until(from_us_since_boot(next_frame_at));
                }
                else if (now - next_frame_at > 4u * GB_FRAME_PERIOD_US)
                {
                    /* Far behind (a stall, or simply not fast enough): drop the
                     * accumulated deficit rather than running a catch-up burst
                     * to repay time that can never be made up. */
                    next_frame_at = now;
                }
            }

            /* Execute CPU cycles until the screen has to be redrawn. */
            gb_run_frame(&gb);

            frames++;
#if AUDIO_DEBUG
            /* Temporary: report the emulator's actual frame rate every ~5s.
             * Audio is produced one batch per frame, so this is what decides
             * whether the PWM output starves between batches. Uses its own
             * counters so the 'b' serial benchmark still works independently. */
            {
                static uint32_t fps_frames = 0;
                static uint64_t fps_t0 = 0;

                if (fps_t0 == 0)
                    fps_t0 = time_us_64();

                if (++fps_frames >= 300)
                {
                    uint64_t now = time_us_64();
                    uint32_t diff = (uint32_t)(now - fps_t0);
                    uint32_t accounted = lcd_xfer_us + block_dbg_us + pacer_dbg_us;

                    DBG_INFO("FPS: %lu  frame=%lu  lcd=%lu  blk=%lu  pac=%lu  emu=%lu us\n",
                             (uint32_t)(((uint64_t)fps_frames * 1000000u) / diff),
                             diff / fps_frames,
                             lcd_xfer_us / fps_frames,
                             block_dbg_us / fps_frames,
                             pacer_dbg_us / fps_frames,
                             (diff > accounted ? diff - accounted : 0) / fps_frames);
                    lcd_xfer_us = 0;
                    block_dbg_us = 0;
                    pacer_dbg_us = 0;
                    fps_frames = 0;
                    fps_t0 = now;
                }
            }
#endif
#if ENABLE_SOUND
            /*
             * Push every emulated frame. The APU advances with emulation, not
             * with rendering, so frame_skip (which only halves how often the
             * draw callback runs) must not gate audio — doing so is what made
             * fast-forward silent.
             *
             * This blocks while core1 is behind, which is what keeps producer
             * and consumer in step. The blocked time is accumulated so the rate
             * estimate below can subtract it.
             */
            {
#if ADAPTIVE_AUDIO_RATE
                uint64_t push_t0 = time_us_64();
#endif
                multicore_fifo_push_blocking_inline(AUDIO_CMD_PLAYBACK);
#if ADAPTIVE_AUDIO_RATE
                uint32_t blocked = (uint32_t)(time_us_64() - push_t0);
                audio_block_us += blocked;
#if AUDIO_DEBUG
                block_dbg_us += blocked;
#endif
#endif
            }
#endif

#if ADAPTIVE_AUDIO_RATE
            /*
             * Measure how fast frames are actually produced and tell core1 to
             * play back at that rate. AUDIO_SAMPLES samples are generated per
             * emulated frame, so the true sample rate is AUDIO_SAMPLES times
             * the real frame rate — not the nominal AUDIO_SAMPLE_RATE, which
             * assumes 59.727 fps.
             *
             * Subtracting audio_block_us is what makes this converge. Measured
             * wall-clock time includes any stall on the push above, so a
             * too-low playback rate would slow core0, which would then measure
             * a low production rate and confirm the too-low playback rate
             * forever. Excluding that stall measures the rate core0 *could*
             * sustain, letting the estimate climb back out. Pacer waits are
             * deliberately not excluded — being throttled to real time is
             * genuine frame spacing, not a measurement artefact.
             */
            {
                static uint32_t rate_frames = 0;
                static uint64_t rate_t0 = 0;
                uint64_t now = time_us_64();

                if (rate_t0 == 0)
                {
                    rate_t0 = now;
                }
                else if (++rate_frames >= 256)
                {
                    uint32_t window_us = (uint32_t)(now - rate_t0);

                    if (window_us > audio_block_us)
                        window_us -= audio_block_us;

                    if (window_us > 0)
                    {
                        uint32_t rate = (uint32_t)(((uint64_t)rate_frames * AUDIO_SAMPLES *
                                                    1000000u) / window_us);

                        /* Shade 1% low so the pipeline settles into mild
                         * back-pressure (inaudible) rather than mild
                         * starvation (audible gaps). */
                        rate -= rate / 100u;

                        if (rate >= 8000u && rate <= 48000u)
                            audio_target_rate = rate;
                    }

                    rate_frames = 0;
                    rate_t0 = now;
                    audio_block_us = 0;
                }
            }
#endif
            /* Update buttons state */
            prev_joypad_bits.up = gb.direct.joypad_bits.up;
            prev_joypad_bits.down = gb.direct.joypad_bits.down;
            prev_joypad_bits.left = gb.direct.joypad_bits.left;
            prev_joypad_bits.right = gb.direct.joypad_bits.right;
            prev_joypad_bits.a = gb.direct.joypad_bits.a;
            prev_joypad_bits.b = gb.direct.joypad_bits.b;
            prev_joypad_bits.select = gb.direct.joypad_bits.select;
            prev_joypad_bits.start = gb.direct.joypad_bits.start;
            gb.direct.joypad_bits.up = input_pins[KEY_UP] == 0 ? 1 : 0;
            gb.direct.joypad_bits.down = input_pins[KEY_DOWN] == 0 ? 1 : 0;
            gb.direct.joypad_bits.left = input_pins[KEY_LEFT] == 0 ? 1 : 0;
            gb.direct.joypad_bits.right = input_pins[KEY_RIGHT] == 0 ? 1 : 0;
            gb.direct.joypad_bits.a = input_pins[KEY_A] == 0 ? 1 : 0;
            gb.direct.joypad_bits.b = input_pins[KEY_B] == 0 ? 1 : 0;
            gb.direct.joypad_bits.select = input_pins[KEY_SELECT] == 0 ? 1 : 0;
            gb.direct.joypad_bits.start = input_pins[KEY_START] == 0 ? 1 : 0;

            /* hotkeys (select + * combo)*/
            if (!gb.direct.joypad_bits.select)
            {
#if ENABLE_SOUND
                if (!gb.direct.joypad_bits.up && prev_joypad_bits.up)
                {
                    /* select + up: increase sound volume */
                    multicore_fifo_push_blocking_inline(AUDIO_CMD_VOLUME_UP);
                }
                if (!gb.direct.joypad_bits.down && prev_joypad_bits.down)
                {
                    /* select + down: decrease sound volume */
                    multicore_fifo_push_blocking_inline(AUDIO_CMD_VOLUME_DOWN);
                }
#endif
                if (!gb.direct.joypad_bits.right && prev_joypad_bits.right)
                {
                    /* select + right: select the next manual color palette */
                    if (manual_palette_selected < 12)
                    {
                        manual_palette_selected++;
                        manual_assign_palette(palette, manual_palette_selected);
                    }
                }
                if (!gb.direct.joypad_bits.left && prev_joypad_bits.left)
                {
                    /* select + left: select the previous manual color palette */
                    if (manual_palette_selected > 0)
                    {
                        manual_palette_selected--;
                        manual_assign_palette(palette, manual_palette_selected);
                    }
                }
                if (!gb.direct.joypad_bits.start && prev_joypad_bits.start)
                {
                    /* select + start: save ram and resets to the game selection menu */
#if ENABLE_SDCARD
                    write_cart_ram_file(&gb);
                    /* Try to save the emulator state for this game. */
                    write_gb_emulator_state(&gb);
#endif
                    goto out;
                }
                if (!gb.direct.joypad_bits.a && prev_joypad_bits.a)
                {
                    /* select + A: enable/disable frame-skip => fast-forward */
                    gb.direct.frame_skip = !gb.direct.frame_skip;
                    DBG_INFO("I gb.direct.frame_skip = %d\n", gb.direct.frame_skip);
                }
            }

#if ENABLE_DEBUG
            /* Serial monitor commands */
            input = getchar_timeout_us(0);
            if (input == PICO_ERROR_TIMEOUT)
                continue;

            switch (input)
            {
#if 0
        static bool invert = false;
        static bool sleep = false;
        static uint8_t freq = 1;
        static ili9225_color_mode_e colour = ILI9225_COLOR_MODE_FULL;

        case 'i':
            invert = !invert;
            ili9225_display_control(invert, colour);
            break;

        case 'f':
            freq++;
            freq &= 0x0F;
            ili9225_set_drive_freq(freq);
            DBG_INFO("Freq %u\n", freq);
            break;
#endif
            case 'i':
                gb.direct.interlace = !gb.direct.interlace;
                break;

            case 'f':
                gb.direct.frame_skip = !gb.direct.frame_skip;
                break;

            case 'b':
            {
                uint64_t end_time;
                uint32_t diff;
                uint32_t fps;

                end_time = time_us_64();
                diff = end_time - start_time;
                fps = ((uint64_t)frames * 1000 * 1000) / diff;
                DBG_INFO("Frames: %u\n"
                         "Time: %lu us\n"
                         "FPS: %lu\n",
                         frames, diff, fps);
                stdio_flush();
                frames = 0;
                start_time = time_us_64();
                break;
            }

            case '\n':
            case '\r':
            {
                gb.direct.joypad_bits.start = 0;
                break;
            }

            case '\b':
            {
                gb.direct.joypad_bits.select = 0;
                break;
            }

            case '8':
            {
                gb.direct.joypad_bits.up = 0;
                break;
            }

            case '2':
            {
                gb.direct.joypad_bits.down = 0;
                break;
            }

            case '4':
            {
                gb.direct.joypad_bits.left = 0;
                break;
            }

            case '6':
            {
                gb.direct.joypad_bits.right = 0;
                break;
            }

            case 'z':
            case 'w':
            {
                gb.direct.joypad_bits.a = 0;
                break;
            }

            case 'x':
            {
                gb.direct.joypad_bits.b = 0;
                break;
            }

            case 'q':
                goto out;

            default:
                break;
            }
#endif /* ENABLE_DEBUG */
        }

    out:
        DBG_INFO("\nEmulation Ended");
    }
}
