/*
 * The VIC-II's picture, drawn from memory as the chip draws it: 320x200
 * dots in the middle of a border, 40x25 characters of 8x8.
 *
 * The VIC sees one 16KB bank of RAM (CIA 2 chooses which), with the
 * character ROM in place of RAM at $1000-$1FFF of banks 0 and 2. In it,
 * $D018 says where the screen (its 1000 bytes) is, and the characters or
 * the bitmap. The modes ($D011 bits 5-6, $D016 bit 4):
 *
 *   text             a character's dots in its colour (colour RAM) on
 *                    the background ($D021)
 *   multicolour text characters whose colour has bit 3 set are 4x8 dots
 *                    of two bits: background, $D022, $D023, colour & 7
 *   extended colour  bits 6-7 of a character's code choose its background
 *                    ($D021-$D024), of characters 0-63
 *   bitmap           320x200 dots, 8 bytes a character cell; the screen
 *                    gives each cell its colours (high nybble for 1, low
 *                    for 0)
 *   multicolour bitmap  160x200: background, the screen's high and low
 *                    nybbles, colour RAM
 *
 * and $D011 and $D016 also scroll the picture (0-7 dots down and right),
 * cover a row or a column at its edges with the border (24 rows, 38
 * columns) and blank it ($D011 bit 4). The raster line ($D012, and bit 7
 * of $D011) moves on each time it's read, so that programs waiting for
 * a line find it.
 *
 * Eight rows of dots are drawn again when memory they show changes (and
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
// What the VIC sees, worked out when the registers change: the bank,
// and the absolute addresses of the screen, the characters and the
// bitmap within it.
static unsigned int bank, screen_at, chars_at, bitmap_at;

HIGH_MEM static unsigned char row_px[320 * 8];
HIGH_MEM static unsigned char line_px[336];   // (a line, before scrolling)
HIGH_MEM static unsigned char line_fg[336];   // ... and which dots are foreground
__attribute__((noinline, noclone)) static unsigned char *row_pixels(void) { return row_px; }
__attribute__((noinline, noclone)) static unsigned char *line_pixels(void) { return line_px; }
__attribute__((noinline, noclone)) static unsigned char *line_fore(void) { return line_fg; }

static void all_dirty(void) {
    memset(dirty, 1, sizeof dirty);
}

static void find_memory(void) {
    bank = c64_vic_bank();
    screen_at = bank + (regs[0x18] >> 4) * 0x400;
    chars_at = bank + (regs[0x18] >> 1 & 7) * 0x800;
    bitmap_at = bank + (regs[0x18] & 8) * 0x400;
    all_dirty();
}

// A byte as the VIC sees it at addr (in its bank): RAM, or in banks 0
// and 2 the character ROM at $1000-$1FFF.
static int vic_byte(unsigned int addr) {
    if ((addr & 0x7000) == 0x1000) return c64_char_rom(addr & 0x0FFF);
    return c64_ram(addr);
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
    case 0x11:                            // (the raster line: time moves on as it's read)
    case 0x12: {
        int r = raster;
        raster = (raster + 1) % 312;
        return reg == 0x12 ? r & 0xFF : (regs[0x11] & 0x7F) | (r > 255 ? 0x80 : 0);
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
    else if (reg == 0x20 && (regs[0x11] & 0x18) != 0x18) all_dirty();   // (border in the picture)
    else if (reg == 0x20 && !(regs[0x16] & 8)) all_dirty();
}

// Rows of characters that change: those dots drawn again (and the
// rows next to them, if the picture is scrolled up or down).
static void row_changed(int row) {
    dirty[row] = 1;
    if ((regs[0x11] & 7) != 3) {
        if (row > 0) dirty[row - 1] = 1;
        if (row < 24) dirty[row + 1] = 1;
    }
}

void vic_mem_written(unsigned int addr) {
    if (addr - screen_at < 1000) row_changed((addr - screen_at) / 40);
    else if (regs[0x11] & 0x20) {         // a bitmap
        if (addr - bitmap_at < 8000) row_changed((addr - bitmap_at) / 320);
    } else if (addr - chars_at < 2048 && (chars_at & 0x7000) != 0x1000) {
        all_dirty();                      // (characters in RAM)
    }
}

void vic_colour_written(int offset) {
    if (offset < 1000) row_changed(offset / 40);
}

void vic_cursor(int off) {
    if (off == cursor_off) return;
    if (cursor_off >= 0 && cursor_off < 1000) row_changed(cursor_off / 40);
    cursor_off = off;
    cursor_on = 1;
    cursor_time = plat_timer_ms();
    if (off >= 0 && off < 1000) row_changed(off / 40);
}

/*
 * One line of the picture's 40 cells (y: 0-199 of the unscrolled
 * picture), into px from x, and whether each dot is foreground (for
 * sprites' priority and collisions) into fg.
 */
static void draw_cells(int y, unsigned char *px, unsigned char *fg) {
    int row = y >> 3, line = y & 7;
    int ecm = regs[0x11] & 0x40, bmm = regs[0x11] & 0x20, mcm = regs[0x16] & 0x10;
    int bg0 = regs[0x21] & 15;
    if (ecm && (bmm || mcm)) {            // (no such mode: black)
        memset(px, 0, 320);
        memset(fg, 0, 320);
        return;
    }
    for (int col = 0; col < 40; col++, px += 8, fg += 8) {
        int off = row * 40 + col;
        int code = c64_ram(screen_at + off);
        int colour = c64_colour(off);
        int bits, c[4];
        if (bmm) {
            bits = vic_byte(bitmap_at + off * 8 + line);
            if (mcm) {
                c[0] = bg0, c[1] = code >> 4, c[2] = code & 15, c[3] = colour;
            } else {
                for (int x = 0; x < 8; x++) {
                    int on = bits & (0x80 >> x);
                    px[x] = on ? code >> 4 : code & 15;
                    fg[x] = on != 0;
                }
                continue;
            }
        } else {
            int bg = bg0;
            if (ecm) {
                bg = regs[0x21 + (code >> 6)] & 15;
                code &= 63;
            }
            bits = vic_byte(chars_at + code * 8 + line);
            if (off == cursor_off && cursor_on) bits ^= 0xFF;
            if (!mcm || !(colour & 8)) {
                int ink = mcm ? colour & 7 : colour;
                for (int x = 0; x < 8; x++) {
                    int on = bits & (0x80 >> x);
                    px[x] = on ? ink : bg;
                    fg[x] = on != 0;
                }
                continue;
            }
            c[0] = bg0, c[1] = regs[0x22] & 15, c[2] = regs[0x23] & 15, c[3] = colour & 7;
        }
        for (int x = 0; x < 8; x += 2) {  // (multicolour: two dots a pair of bits)
            int pair = bits >> (6 - x) & 3;
            px[x] = px[x + 1] = c[pair];
            fg[x] = fg[x + 1] = pair >= 2;
        }
    }
}

// Line y (0-199) of the picture as shown: scrolled, with the border's
// edges and blanking.
static void draw_line(int y, unsigned char *out) {
    int border = regs[0x20] & 15;
    if (!(regs[0x11] & 0x10) || (!(regs[0x11] & 8) && (y < 4 || y >= 196))) {
        memset(out, border, 320);
        return;
    }
    unsigned char *px = line_pixels(), *fg = line_fore();
    int xs = regs[0x16] & 7, sy = y - ((regs[0x11] & 7) - 3);
    if (sy < 0 || sy >= 200) {            // (scrolled away: background)
        memset(px + xs, regs[0x21] & 15, 320);
        memset(fg + xs, 0, 320);
    } else {
        draw_cells(sy, px + xs, fg + xs);
    }
    memset(px, regs[0x21] & 15, xs);
    memcpy(out, px, 320);
    if (!(regs[0x16] & 8)) {              // 38 columns
        memset(out, border, 7);
        memset(out + 311, border, 9);
    }
}

static void draw_row(int row) {
    unsigned char *px = row_pixels();
    for (int y = 0; y < 8; y++) draw_line(row * 8 + y, px + y * 320);
    plat_put_block(0, row * 8, 320, 8, px);
}

void vic_update(int now) {
    unsigned long t = plat_timer_ms();
    if (!now && t - last_frame < 20) return;
    last_frame = t;
    if (cursor_off >= 0 && t - cursor_time >= 330) {   // the cursor flashes
        cursor_on = !cursor_on;
        cursor_time = t;
        if (cursor_off < 1000) row_changed(cursor_off / 40);
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
