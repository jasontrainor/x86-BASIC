/*
 * The Apple II picture, drawn from memory as the video hardware does.
 *
 * The picture is 560x192 (the IIe's double-resolution dots; a 40-column
 * character or a high-resolution dot is two of them wide), shown twice
 * as tall as wide, as a TV shows it. Each of the 24 text rows is 8
 * scanlines; a row is drawn again when memory it shows or a switch
 * changes.
 *
 *   text      40x24 (or 80x24) characters from the text page: $00-$3F
 *             inverse, $40-$7F flashing, $80-$FF normal; the IIe's
 *             alternate set has MouseText at $40-$5F and inverse lower
 *             case at $60-$7F instead of flashing
 *   lores     40x48 blocks in 16 colours, two to a text-page byte
 *   hires     280x192 dots, 7 to a byte (bit 0 leftmost); bit 7 shifts
 *             the byte's dots half a dot right
 *   double    (IIe, 80COL on and AN3 off) 80x48 blocks or 560x192 dots,
 *             auxiliary and main memory bytes taking turns
 *
 * Colour comes from how a TV decodes the dot pattern. The colours here
 * are approximations worked out for this project. In high resolution a
 * dot next to another is white; a dot on its own is violet or green
 * (blue or orange with bit 7 set), by whether its column is even or odd;
 * a gap between two dots of the same colour is filled in. Double high
 * resolution is read as 140 coloured dots a line, each four bits.
 * Text is white on black, as a colour monitor shows it once the colour
 * is turned off for text.
 *
 * The characters are this project's own font (core/font.c), with
 * MouseText symbols drawn here.
 */
#include "apple.h"
#include "platform.h"
#include "font.h"
#include "string.h"
#include "mem.h"

#define WIDTH 560
#define ROWS 24

enum { BLACK = 0, VIOLET = 3, GREEN = 12, BLUE = 6, ORANGE = 9, WHITE = 15 };

static const unsigned char palette[16][3] = {
    { 0x00, 0x00, 0x00 },   //  0 black
    { 0x8A, 0x15, 0x47 },   //  1 deep red
    { 0x3E, 0x31, 0xA8 },   //  2 dark blue
    { 0xD2, 0x46, 0xE8 },   //  3 purple
    { 0x00, 0x6A, 0x3E },   //  4 dark green
    { 0x82, 0x82, 0x82 },   //  5 grey
    { 0x2D, 0x92, 0xE6 },   //  6 medium blue
    { 0xBE, 0xAE, 0xFF },   //  7 light blue
    { 0x42, 0x55, 0x00 },   //  8 brown
    { 0xD4, 0x6C, 0x1D },   //  9 orange
    { 0x82, 0x82, 0x82 },   // 10 grey
    { 0xFF, 0x99, 0xC1 },   // 11 pink
    { 0x2C, 0xC0, 0x1A },   // 12 light green
    { 0xC0, 0xD5, 0x5C },   // 13 yellow
    { 0x6C, 0xEA, 0xC4 },   // 14 aqua
    { 0xFF, 0xFF, 0xFF },   // 15 white
};

static unsigned char dirty[ROWS];
static unsigned char has_flash[ROWS];
static int flash_on;
static unsigned long flash_time;
static int cursor_col = -1, cursor_row, cursor_shown;

HIGH_MEM static unsigned char row_px[WIDTH * 8];

__attribute__((noinline, noclone)) static unsigned char *row_pixels(void) { return row_px; }

/* ------------------------------------------------------------------------
 * Characters
 * --------------------------------------------------------------------- */

// MouseText ($40-$5F in the IIe's alternate set): 7 dots wide, 8 high.
static const char *const mousetext[32] = {
    "...#.../..###../.#####./#######/.#####./..###../...#.../.......",   // filled diamond
    "...#.../..#.#../.#...#./#.....#/.#...#./..#.#../...#.../.......",   // hollow diamond
    ".#...../.##..../.###.../.####../.#####./.##..../.#.#.../.......",   // pointer
    "#######/.#...#./..#.#../...#.../..#.#../.#...#./#######/.......",   // hourglass
    "......./......#/.....#./#...#../.#.#.../..#..../......./.......",   // check mark
    "#######/######./#####.#/.###.##/#.#.###/##.####/#######/#######",   // inverse check
    "......./..###../.#####./.#####./.#####./..###../......./.......",   // dot
    "......./..###../.#...#./.#...#./.#...#./..###../......./.......",   // ring
    "......./...#.../..#..../.######/..#..../...#.../......./.......",   // left arrow
    "......./......./......./......./......./#..#..#/......./.......",   // ellipsis
    "...#.../...#.../...#.../...#.../#.###.#/.#####./..###../...#...",   // down arrow
    "...#.../..###../.#####./#.###.#/...#.../...#.../...#.../...#...",   // up arrow
    "#######/......./......./......./......./......./......./.......",   // top line
    "......#/......#/..#...#/.#....#/#######/.#...../..#..../.......",   // return
    "#######/#######/#######/#######/#######/#######/#######/#######",   // block
    "...#..#/..##..#/.###..#/####..#/.###..#/..##..#/...#..#/.......",   // scroll left
    "#..#.../#..##../#..###./#..####/#..###./#..##../#..#.../.......",   // scroll right
    "......./#######/.#####./..###../...#.../......./#######/.......",   // scroll down
    "#######/......./...#.../..###../.#####./#######/......./.......",   // scroll up
    "......./......./......./#######/......./......./......./.......",   // middle line
    "#....../#....../#....../#....../#....../#....../#....../#######",   // corner
    "......./...#.../....#../######./....#../...#.../......./.......",   // right arrow
    "#.#.#.#/.#.#.#./#.#.#.#/.#.#.#./#.#.#.#/.#.#.#./#.#.#.#/.#.#.#.",   // shade
    ".#.#.#./#.#.#.#/.#.#.#./#.#.#.#/.#.#.#./#.#.#.#/.#.#.#./#.#.#.#",   // shade
    "......./.####../#....##/#......#/#......#/#......#/########/.......",  // folder
    "......./......./######./.....#./.....#./.....#./######./.......",   // folder end
    "......#/......#/......#/......#/......#/......#/......#/......#",   // right line
    "......./...#.../..#.#../.#...#./..#.#../...#.../......./.......",   // small diamond
    "#######/......./......./......./......./......./......./#######",   // top and bottom
    "......./...#.../...#.../.#####./...#.../...#.../......./.......",   // plus
    "#######/#.....#/#.....#/#..#..#/#.....#/#.....#/#######/.......",   // box with dot
    "#....../#....../#....../#....../#....../#....../#....../#......",   // left line
};

static unsigned char mouse_bits[32][8];
static unsigned char extra_bits[2][8];      // ` and DEL, which the font has otherwise

static void art(const char *s, unsigned char *out) {
    for (int y = 0; y < 8; y++) {
        unsigned char b = 0;
        for (int x = 0; x < 7 && *s && *s != '/'; x++, s++)
            if (*s == '#') b |= 0x80 >> x;
        out[y] = b;
        while (*s && *s != '/') s++;
        if (*s) s++;
    }
}

// The 8 rows of the glyph for character code c on screen, bit 7 the
// leftmost of its 7 dots; *inverse and *flash say how it shows.
static const unsigned char *glyph(int c, int *inverse, int *flash) {
    int iie = apple_model == APPLE_IIE;
    *inverse = *flash = 0;
    int ascii;
    if (c < 0x40) {
        *inverse = 1;
        ascii = c;
    } else if (c < 0x80) {
        if (iie && apple_sw.altchar) {
            if (c < 0x60) return mouse_bits[c - 0x40];
            *inverse = 1;
            ascii = c;                    // inverse lower case
            goto lower;
        }
        *flash = 1;
        ascii = c - 0x40;
    } else {
        ascii = c & 0x7F;
        if (ascii >= 0x60) goto lower;
    }
    // $00-$3F of the character set: @A-Z[\]^_ then space to ?
    ascii &= 0x3F;
    ascii = ascii < 0x20 ? ascii + 0x40 : ascii;
    return font[ascii - 32];
lower:
    ascii = c & 0x7F;
    if (!iie) return font[(ascii & 0x3F) - 32];   // (no lower case: the symbols)
    if (ascii == 0x60) return extra_bits[0];
    if (ascii == 0x7F) return extra_bits[1];
    return font[ascii - 32];
}

/* ------------------------------------------------------------------------
 * Drawing a row
 * --------------------------------------------------------------------- */

static unsigned int text_base(void) {
    return apple_sw.page2 && !apple_sw.store80 ? 0x800 : 0x400;
}

static unsigned int row_offset(int r) {
    return (r & 7) * 128 + (r >> 3) * 40;
}

static int double_res(void) {
    return apple_model == APPLE_IIE && apple_sw.col80 && apple_sw.dhires;
}

// A character cell w dots wide (14: 40 columns, 7: 80) at pixel x.
static void draw_char(unsigned char *px, int x, int w, int code, int cursor, int r) {
    int inverse, flash;
    const unsigned char *g = glyph(code, &inverse, &flash);
    if (flash) has_flash[r] = 1;
    int inv = inverse ^ (flash && flash_on) ^ cursor;
    for (int y = 0; y < 8; y++) {
        unsigned char b = g[y];
        unsigned char *p = px + y * WIDTH + x;
        for (int d = 0; d < 7; d++) {
            int on = ((b >> (7 - d)) & 1) ^ inv;
            if (w == 14) p[2 * d] = p[2 * d + 1] = on ? WHITE : BLACK;
            else p[d] = on ? WHITE : BLACK;
        }
    }
}

static void draw_text_row(unsigned char *px, int r) {
    unsigned int a = text_base() + row_offset(r);
    int cur = cursor_shown && cursor_row == r;
    if (apple_model == APPLE_IIE && apple_sw.col80) {
        for (int c = 0; c < 80; c++)
            draw_char(px, c * 7, 7, apple_ram(!(c & 1), a + c / 2), cur && cursor_col == c, r);
    } else {
        for (int c = 0; c < 40; c++)
            draw_char(px, c * 14, 14, apple_ram(0, a + c), cur && cursor_col == c, r);
    }
}

static int rotate_left(int c) { return ((c << 1) | (c >> 3)) & 15; }

static void draw_lores_row(unsigned char *px, int r) {
    unsigned int a = text_base() + row_offset(r);
    for (int y = 0; y < 8; y++) {
        unsigned char *p = px + y * WIDTH;
        int shift = y < 4 ? 0 : 4;
        if (double_res()) {
            for (int c = 0; c < 80; c++) {
                int colour = (apple_ram(!(c & 1), a + c / 2) >> shift) & 15;
                if (!(c & 1)) colour = rotate_left(colour);   // (aux colours are rotated)
                for (int d = 0; d < 7; d++) p[c * 7 + d] = colour;
            }
        } else {
            for (int c = 0; c < 40; c++) {
                int colour = (apple_ram(0, a + c) >> shift) & 15;
                for (int d = 0; d < 14; d++) p[c * 14 + d] = colour;
            }
        }
    }
}

static unsigned int hires_line(int y) {
    unsigned int base = apple_sw.page2 && !apple_sw.store80 ? 0x4000 : 0x2000;
    return base + (y & 7) * 1024 + ((y >> 3) & 7) * 128 + (y >> 6) * 40;
}

static int hires_colour(int x, int high) {
    return x & 1 ? (high ? ORANGE : GREEN) : (high ? BLUE : VIOLET);
}

static void draw_hires_line(unsigned char *p, int y) {
    unsigned int a = hires_line(y);
    if (double_res()) {
        // 560 dots, 14 to a column (aux then main), read 4 at a time.
        unsigned char dots[WIDTH];
        for (int c = 0; c < 40; c++) {
            int aux = apple_ram(1, a + c), main = apple_ram(0, a + c);
            for (int d = 0; d < 7; d++) {
                dots[c * 14 + d] = (aux >> d) & 1;
                dots[c * 14 + 7 + d] = (main >> d) & 1;
            }
        }
        for (int x = 0; x < WIDTH; x += 4) {
            int colour = dots[x] | dots[x + 1] << 1 | dots[x + 2] << 2 | dots[x + 3] << 3;
            p[x] = p[x + 1] = p[x + 2] = p[x + 3] = colour;
        }
        return;
    }
    // dot[2 + x], x 0-279, with two blank dots each side.
    unsigned char dot[284], high[284];
    memset(dot, 0, sizeof(dot));
    memset(high, 0, sizeof(high));
    for (int c = 0; c < 40; c++) {
        int b = apple_ram(0, a + c);
        for (int d = 0; d < 7; d++) {
            dot[2 + c * 7 + d] = (b >> d) & 1;
            high[2 + c * 7 + d] = b >> 7;
        }
    }
    memset(p, BLACK, WIDTH);
    for (int x = 0; x < 280; x++) {
        int i = x + 2, colour;
        if (dot[i]) {
            colour = dot[i - 1] || dot[i + 1] ? WHITE : hires_colour(x, high[i]);
        } else if (dot[i - 1] && dot[i + 1]) {
            // A gap between two coloured dots of the same colour is filled.
            int left = dot[i - 2] ? WHITE : hires_colour(x - 1, high[i - 1]);
            int right = dot[i + 2] ? WHITE : hires_colour(x + 1, high[i + 1]);
            if (left != right || left == WHITE) continue;
            colour = left;
        } else {
            continue;
        }
        int px = 2 * x + high[i];
        p[px] = colour;
        if (px + 1 < WIDTH) p[px + 1] = colour;
    }
}

static void draw_row(int r) {
    unsigned char *px = row_pixels();
    has_flash[r] = 0;
    int graphics = !apple_sw.text && !(apple_sw.mixed && r >= 20);
    if (!graphics) draw_text_row(px, r);
    else if (!apple_sw.hires) draw_lores_row(px, r);
    else for (int y = 0; y < 8; y++) draw_hires_line(px + y * WIDTH, r * 8 + y);
    plat_put_block(0, r * 8, WIDTH, 8, px);
    dirty[r] = 0;
}

/* ------------------------------------------------------------------------ */

void video_init(void) {
    struct video v = { WIDTH, 192, 2, 600, 400, { { 0 } } };   // (a 20-pixel border at least)
    memcpy(v.palette, palette, sizeof(palette));
    plat_video(&v);
    plat_border(BLACK);
    for (int i = 0; i < 32; i++) art(mousetext[i], mouse_bits[i]);
    art(".#...../..#..../...#.../......./......./......./......./.......", extra_bits[0]);
    art("#.#.#.#/.#.#.#./#.#.#.#/.#.#.#./#.#.#.#/.#.#.#./#.#.#.#/.......", extra_bits[1]);
    cursor_col = -1;
    cursor_shown = 0;
    video_mode_changed();
}

void video_mode_changed(void) {
    memset(dirty, 1, sizeof(dirty));
}

void video_changed(unsigned int addr, int aux) {
    (void)aux;
    if (addr >= 0x400 && addr < 0xC00) {
        unsigned int off = addr & 0x3FF, col = off & 0x7F;
        if (col < 120) dirty[(off >> 7) + 8 * (col / 40)] = 1;
    } else if (addr >= 0x2000 && addr < 0x6000) {
        unsigned int off = addr & 0x1FFF, col = off & 0x7F;
        if (col < 120) dirty[((off >> 7) & 7) + 8 * (col / 40)] = 1;
    }
}

void video_cursor(int col, int row, int on) {
    if (cursor_shown && cursor_row >= 0 && cursor_row < ROWS) dirty[cursor_row] = 1;
    cursor_col = col;
    cursor_row = row;
    cursor_shown = on;
    if (on && row >= 0 && row < ROWS) dirty[row] = 1;
}

void video_update(void) {
    unsigned long now = plat_timer_ms();
    if (now - flash_time >= 267) {        // about twice a second, as on the Apple
        flash_time = now;
        flash_on = !flash_on;
        for (int r = 0; r < ROWS; r++)
            if (has_flash[r]) dirty[r] = 1;
    }
    for (int r = 0; r < ROWS; r++)
        if (dirty[r]) draw_row(r);
}

// The cursor blinks with flashing characters.
int video_flash_phase(void) { return flash_on; }
