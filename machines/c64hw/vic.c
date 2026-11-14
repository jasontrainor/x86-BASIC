/*
 * The VIC-II's picture, drawn from memory as the chip draws it: 320x200
 * dots in the middle of a border, 40x25 characters of 8x8.
 *
 * The VIC sees one 16KB bank of RAM (CIA 2 chooses which), with the
 * character ROM in place of RAM at $1000-$1FFF of banks 0 and 2. In it,
 * $D018 says where the screen (its 1000 character codes) and the
 * characters are. The colour of each character is in colour RAM; the
 * background is $D021 and the border $D020.
 *
 * A row of characters is drawn again when memory it shows changes (and
 * everything when a register that changes the picture is written), at
 * most 50 times a second.
 *
 * The colours are this project's choice, for each of the sixteen as they
 * are named; no measured or published values were used.
 */
#include "c64.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

int c64_vic_bank(void);                   // memory.c

static const unsigned char palette[16][3] = {
    { 0x00, 0x00, 0x00 },   //  0 black
    { 0xFF, 0xFF, 0xFF },   //  1 white
    { 0x9A, 0x3A, 0x2E },   //  2 red
    { 0x6E, 0xC8, 0xCC },   //  3 cyan
    { 0x92, 0x40, 0xA4 },   //  4 purple
    { 0x58, 0xAC, 0x48 },   //  5 green
    { 0x3C, 0x30, 0xA0 },   //  6 blue
    { 0xC8, 0xD2, 0x70 },   //  7 yellow
    { 0x98, 0x58, 0x22 },   //  8 orange
    { 0x60, 0x46, 0x00 },   //  9 brown
    { 0xC8, 0x70, 0x66 },   // 10 light red
    { 0x48, 0x48, 0x48 },   // 11 dark grey
    { 0x7A, 0x7A, 0x7A },   // 12 grey
    { 0x9E, 0xE6, 0x8C },   // 13 light green
    { 0x78, 0x6E, 0xD8 },   // 14 light blue
    { 0xAA, 0xAA, 0xAA },   // 15 light grey
};

static unsigned char regs[64];
static unsigned char dirty[25];
static int raster;
static int border_shown = -1;
static unsigned long last_frame;
static int cursor_off = -1, cursor_on;
static unsigned long cursor_time;
// Where the screen and characters are now (absolute addresses), and
// whether the characters are the ROM's.
static unsigned int screen_at, chars_at;
static int chars_rom;

HIGH_MEM static unsigned char row_px[320 * 8];
__attribute__((noinline, noclone)) static unsigned char *row_pixels(void) { return row_px; }

static void all_dirty(void) {
    memset(dirty, 1, sizeof dirty);
}

static void find_memory(void) {
    unsigned int bank = c64_vic_bank();
    screen_at = bank + (regs[0x18] >> 4) * 0x400;
    unsigned int c = (regs[0x18] >> 1 & 7) * 0x800;
    chars_rom = (bank == 0 || bank == 0x8000) && c >= 0x1000 && c < 0x2000;
    chars_at = chars_rom ? c - 0x1000 : bank + c;   // (ROM: the offset in it)
    all_dirty();
}

void vic_init(void) {
    memset(regs, 0, sizeof regs);
    regs[0x11] = 0x1B;                    // screen on, 25 rows, scrolled 3
    regs[0x16] = 0xC8;                    // 40 columns
    regs[0x18] = 0x15;                    // screen $0400, characters $1000 (ROM)
    regs[0x20] = 14;                      // light blue border
    regs[0x21] = 6;                       // on blue
    struct video v = { 320, 200, 1, 384, 272, { { 0 } } };
    memcpy(v.palette, palette, sizeof palette);
    plat_video(&v);
    border_shown = -1;
    find_memory();
}

void vic_bank_changed(void) { find_memory(); }

int vic_read(int reg) {
    reg &= 63;
    switch (reg) {
    case 0x11:
        return (regs[0x11] & 0x7F) | (raster > 255 ? 0x80 : 0);
    case 0x12: {                          // the raster line: time moves on as it's read
        int r = raster & 0xFF;
        raster = (raster + 1) % 312;
        return r;
    }
    case 0x16: return regs[reg] | 0xC0;
    case 0x18: return regs[reg] | 0x01;
    case 0x19: return regs[reg] | 0x70;
    case 0x1A: return regs[reg] | 0xF0;
    }
    if (reg >= 0x20 && reg <= 0x2E) return regs[reg] | 0xF0;
    if (reg > 0x2E) return 0xFF;
    return regs[reg];
}

void vic_write(int reg, int value) {
    reg &= 63;
    if (reg > 0x2E) return;
    int old = regs[reg];
    regs[reg] = value;
    if (old == value) return;
    if (reg == 0x18) find_memory();
    else if (reg == 0x11 || reg == 0x16 || (reg >= 0x21 && reg <= 0x24)) all_dirty();
}

void vic_mem_written(unsigned int addr) {
    if (addr - screen_at < 1000) dirty[(addr - screen_at) / 40] = 1;
    else if (!chars_rom && addr - chars_at < 2048) all_dirty();
}

void vic_colour_written(int offset) {
    if (offset < 1000) dirty[offset / 40] = 1;
}

void vic_cursor(int off) {
    if (off == cursor_off) return;
    if (cursor_off >= 0 && cursor_off < 1000) dirty[cursor_off / 40] = 1;
    cursor_off = off;
    cursor_on = 1;
    cursor_time = plat_timer_ms();
    if (off >= 0 && off < 1000) dirty[off / 40] = 1;
}

// A byte of a character's shape: line y of character code.
static int char_line(int code, int y) {
    unsigned int off = code * 8 + y;
    return chars_rom ? c64_char_rom(chars_at + off) : c64_ram(chars_at + off);
}

static void draw_row(int row) {
    unsigned char *px = row_pixels();
    int bg = regs[0x21] & 15;
    if (!(regs[0x11] & 0x10)) {           // the screen blanked: all border
        memset(px, regs[0x20] & 15, 320 * 8);
        plat_put_block(0, row * 8, 320, 8, px);
        return;
    }
    for (int col = 0; col < 40; col++) {
        int off = row * 40 + col;
        int code = c64_ram(screen_at + off);
        int fg = c64_colour(off);
        int inv = off == cursor_off && cursor_on;
        for (int y = 0; y < 8; y++) {
            int bits = char_line(code, y);
            if (inv) bits ^= 0xFF;
            unsigned char *p = px + y * 320 + col * 8;
            for (int x = 0; x < 8; x++) p[x] = bits & (0x80 >> x) ? fg : bg;
        }
    }
    plat_put_block(0, row * 8, 320, 8, px);
}

void vic_update(int now) {
    unsigned long t = plat_timer_ms();
    if (!now && t - last_frame < 20) return;
    last_frame = t;
    if (cursor_off >= 0 && t - cursor_time >= 330) {   // the cursor flashes
        cursor_on = !cursor_on;
        cursor_time = t;
        if (cursor_off < 1000) dirty[cursor_off / 40] = 1;
    }
    if (border_shown != (regs[0x20] & 15)) {
        border_shown = regs[0x20] & 15;
        plat_border(border_shown);
    }
    for (int row = 0; row < 25; row++)
        if (dirty[row]) {
            dirty[row] = 0;
            draw_row(row);
        }
}
