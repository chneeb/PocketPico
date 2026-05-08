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
const uint8_t *rom = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
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

/* Pixel data is stored in here. */
static uint8_t pixels_buffer[FRAME_BUFF_STRIDE * 240 * 2];

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

void clear_frame_buff()
{
    for (int i = 0; i < FRAME_BUFF_STRIDE * FRAME_BUFF_HEIGHT * 2; i++)
    {
        pixels_buffer[i] = 0;
    }
}

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
    finish_write_data(false);
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
            pixels_buffer[x * 2] = (uint8_t)(color565 >> 8);
            pixels_buffer[x * 2 + 1] = (uint8_t)(color565 & 0xFF);
        }
    }
    else
    {
#endif
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t color = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
            pixels_buffer[x * 2] = (uint8_t)(color >> 8);
            pixels_buffer[x * 2 + 1] = (uint8_t)(color & 0xFF);
        }
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif

    if (line == 0)
    {
        start_window((WIDTH - LCD_WIDTH) / 2, ((HEIGHT - LCD_HEIGHT) / 2), LCD_WIDTH, LCD_HEIGHT);
        write_data(pixels_buffer, LCD_WIDTH);
    }
    else if (line == LCD_HEIGHT - 1)
    {
        write_data(pixels_buffer, LCD_WIDTH);
        finish_write_data(true);
    }
    else
    {
        write_data(pixels_buffer, LCD_WIDTH);
    }
}

void lcd_draw_line_bis(struct gb_s *gb, const uint8_t pixels[LCD_WIDTH],
                       const uint_fast8_t line)
{
    finish_write_data(false);
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
            pixels_buffer[x * 4] = (uint8_t)(pixel >> 8);
            pixels_buffer[x * 4 + 1] = (uint8_t)(pixel & 0xFF);
            pixels_buffer[x * 4 + 2] = (uint8_t)(pixel >> 8);
            pixels_buffer[x * 4 + 3] = (uint8_t)(pixel & 0xFF);
        }
    }
    else
    {
#endif
        for (unsigned int x = 0; x < LCD_WIDTH; x++)
        {
            uint16_t pixel = palette[(pixels[x] & LCD_PALETTE_ALL) >> 4][pixels[x] & 3];
            pixels_buffer[x * 4] = (uint8_t)(pixel >> 8);
            pixels_buffer[x * 4 + 1] = (uint8_t)(pixel & 0xFF);
            pixels_buffer[x * 4 + 2] = (uint8_t)(pixel >> 8);
            pixels_buffer[x * 4 + 3] = (uint8_t)(pixel & 0xFF);
        }
#if PEANUT_FULL_GBC_SUPPORT
    }
#endif

    if (line == 0)
    {
        start_window((WIDTH - (LCD_WIDTH * 2)) / 2, ((HEIGHT - (LCD_HEIGHT * 2)) / 2), LCD_WIDTH * 2, LCD_HEIGHT * 2);
        write_data(pixels_buffer, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(pixels_buffer, LCD_WIDTH * 2);
    }
    else if (line == LCD_HEIGHT - 1)
    {
        write_data(pixels_buffer, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(pixels_buffer, LCD_WIDTH * 2);
        finish_write_data(true);
    }
    else
    {
        write_data(pixels_buffer, LCD_WIDTH * 2);
        finish_write_data(false);
        write_data(pixels_buffer, LCD_WIDTH * 2);
    }
}
#endif

#if ENABLE_SDCARD
/**
 * Load a save file from the SD card
 */
void read_cart_ram_file(struct gb_s *gb)
{
    char filename[16];
    uint_fast32_t save_size;
    UINT br;

    gb_get_rom_name(gb, filename);
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
    char filename[16];
    uint_fast32_t save_size;
    UINT bw;

    gb_get_rom_name(gb, filename);
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
    char filename[16];
    char filename_state[32];
    UINT br = 0;
    FIL fil;

    sd_card_t *sd = sd_get_by_num(0);
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);

    gb_get_rom_name(gb, filename);
    sprintf(filename_state, "%s_state.bin", filename);
    fr = f_open(&fil, filename_state, FA_READ);

    if (fr == FR_OK)
    {
        f_read(&fil, (uint8_t *)gb, sizeof(struct gb_s), &br);
    }
    else
    {
        DBG_INFO("W read_gb_emulator_state(%s): SKIPPED (no previous state)\n", filename_state);
        goto finish;
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
    char filename[16];
    char filename_state[32];
    UINT bw;
    FIL fil;

    sd_card_t *sd = sd_get_by_num(0);
    FRESULT fr = f_mount(&sd->fatfs, sd->pcName, 1);

    gb_get_rom_name(gb, filename);
    sprintf(filename_state, "%s_state.bin", filename);
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

    DBG_INFO("I write_gb_emulator_state(%s) COMPLETED (%lu bytes)\n", filename, bw);

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
                pre_write_hdr = ((const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET))[0x100];
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
            xip_cache_invalidate_all();  /* invalidate XIP cache — flash_flush_cache() is ROM-sequence only on RP2350 */
            const uint8_t *xip_hdr = (const uint8_t *)(XIP_BASE + FLASH_TARGET_OFFSET);
            rom_sd_hdr_byte      = xip_hdr[0x100]; /* reuse globals: now holds post-write XIP values */
            rom_sd_stored_ck     = xip_hdr[0x14D];
            uint8_t ck = 0;
            for (int i = 0x134; i <= 0x14C; i++)
                ck = ck - xip_hdr[i] - 1;
            rom_sd_computed_ck = ck;
            DBG_INFO("Flash verify: hdr[100]=%02X ck=%02X/%02X %s\n",
                     xip_hdr[0x100], ck, xip_hdr[0x14D],
                     (ck == xip_hdr[0x14D]) ? "OK" : "MISMATCH"); stdio_flush();
        }
        DBG_INFO("I load_cart_rom_file(%s) %s (%lu sectors, fse=%d)\n",
                 filename, success ? "OK" : "FAIL", sector_num, last_fse_ret); stdio_flush();

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

    /* search *.gb files */
    uint16_t num_file = 0;
    fr = f_findfirst(&dj, &fno, ".", "?*.gb");

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
            strcpy(filename[num_file], fno.fname);
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
        draw_string(20, ifile * 20, filename[ifile]);
    }
    DBG_INFO("DP: update_lcd\n"); stdio_flush();
    update_lcd();
    DBG_INFO("DP: done\n"); stdio_flush();
    return num_file;
}

/**
 * The ROM selector displays pages of up to 22 rom files
 * allowing the user to select which rom file to start
 * Copy your *.gb rom files to the root directory of the SD card
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
            rom_file_selector_display_page(filename, num_page);
            snprintf(buf, sizeof(buf), "Loading %s", filename[selected]);
            draw_string(0, FRAME_BUFF_HEIGHT - 20, buf);
            DBG_INFO("A: update_lcd\n"); stdio_flush();
            update_lcd();
            DBG_INFO("A: calling load_cart_rom_file\n"); stdio_flush();
            if (load_cart_rom_file(filename[selected]))
            {
                DBG_INFO("A: load_cart_rom_file OK\n"); stdio_flush();
                break_outer = true;
            }
            else
            {
                DBG_INFO("A: load_cart_rom_file FAILED\n"); stdio_flush();
                draw_string(0, FRAME_BUFF_HEIGHT - 20, "Load FAILED");
                update_lcd();
                sleep_ms(2000);
            }
            break;

        case KEY_START:
            DBG_INFO("ROM File Selector: Start button pressed - resuming last game\n");
            break_outer = true;
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
    /* Required on RP2350: lets core0 safely pause this core for flash operations */
    flash_safe_execute_core_init();

    /* Allocate memory for the stream buffer */
    stream = malloc(AUDIO_SAMPLES_TOTAL * sizeof(int16_t));
    assert(stream != NULL);
    memset(stream, 0, AUDIO_SAMPLES_TOTAL * sizeof(int16_t));

    /* Initialize I2S sound driver (using PIO0) */
    i2s_config_t i2s_config = i2s_get_default_config();
    i2s_config.sample_freq = AUDIO_SAMPLE_RATE;
    i2s_config.dma_trans_count = AUDIO_SAMPLES;
    i2s_volume(&i2s_config, 4);
    i2s_init(&i2s_config);

    /* Initialize audio emulation. */
    audio_init(&apu_ctx);

    DBG_INFO("I Audio ready on core1.\n");

    while (1)
    {
        audio_commands_e cmd = multicore_fifo_pop_blocking_inline();
        switch (cmd)
        {
        case AUDIO_CMD_PLAYBACK:
            audio_callback(&apu_ctx, stream);
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
#endif

        /* Automatically assign a colour palette to the game */
        char rom_title[16];
        auto_assign_palette(palette, gb_colour_hash(&gb), gb_get_rom_name(&gb, rom_title));

#if ENABLE_LCD
        gb_init_lcd(&gb, &lcd_draw_line_bis);
        DBG_INFO("LCD ");
#endif

#if ENABLE_SDCARD
        /* Load Save File. */
        read_cart_ram_file(&gb);
#endif

        DBG_INFO("\n> ");
        uint_fast32_t frames = 0;
        uint64_t start_time = time_us_64();
        while (1)
        {
            int input;

            /* Execute CPU cycles until the screen has to be redrawn. */
            gb_run_frame(&gb);

            frames++;
#if ENABLE_SOUND
            if (!gb.direct.frame_skip)
            {
                multicore_fifo_push_blocking_inline(AUDIO_CMD_PLAYBACK);
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
