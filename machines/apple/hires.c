/*
 * High-resolution graphics as FP BASIC's HGR, HCOLOR=, HPLOT, DRAW and
 * XDRAW do them, on the page zero page says ($E6: $20 page 1, $40
 * page 2). Written for this project from how the machines behave.
 *
 * A dot is a bit of a byte, seven dots to a byte; bit 7 of the byte is
 * which pair of colours its dots show. The eight HCOLORs are patterns:
 * 0 black, 1 green, 2 violet, 3 white, 4 black, 5 orange, 6 blue,
 * 7 white (4-7 with bit 7 set). Plotting a dot sets it if the pattern
 * has it at that place, else clears it, and gives the byte the
 * pattern's bit 7: so green on an even column comes out black, as it
 * does on the real machine.
 *
 * Shapes (DRAW, XDRAW) are read from a shape table in memory, whose
 * address is at $E8-$E9: a count, then each shape's offset (2 bytes),
 * then the shapes. A shape is bytes of up to three moves each (bits
 * 0-2, 3-5, 6-7), each a direction (0 up, 1 right, 2 down, 3 left) and
 * (bit 2, not in the last) whether to plot before moving; a 0 byte ends
 * it. ROT= turns it (in 64ths of a turn: here to the nearest quarter),
 * SCALE= makes each move that many dots.
 */
#include "apple.h"

#define ZP_HPAG  0xE6                     // the page drawn on: $20 or $40
#define ZP_SHAPE 0xE8                     // the shape table's address
#define ZP_COLLISIONS 0xEA

static const unsigned char patterns[8] = { 0x00, 0x2A, 0x55, 0x7F, 0x80, 0xAA, 0xD5, 0xFF };
static int hcolor;
static int last_x, last_y;
static int rot, scale = 1;

static unsigned int line_addr(int y) {
    unsigned int base = (apple_peek(ZP_HPAG) & 0x60) << 8;
    if (base != 0x2000 && base != 0x4000) base = 0x2000;
    return base + (y & 7) * 1024 + ((y >> 3) & 7) * 128 + (y >> 6) * 40;
}

static int pattern(int c, int byte) {
    int p = patterns[c & 7];
    if ((byte & 1) && (c & 3) != 0 && (c & 3) != 3) p ^= 0x7F;   // (colours alternate)
    return p;
}

void apple_hgr(int page2) {
    if (apple_model == APPLE_I) return;
    apple_poke(0xC050, 0);                // graphics
    apple_poke(page2 ? 0xC052 : 0xC053, 0);   // HGR2: the whole screen
    apple_poke(page2 ? 0xC055 : 0xC054, 0);
    apple_poke(0xC057, 0);                // high resolution
    apple_poke(ZP_HPAG, page2 ? 0x40 : 0x20);
    apple_hclear(0);
}

void apple_hclear(int fill) {
    int c = fill ? hcolor : 0;
    for (int y = 0; y < 192; y++) {
        unsigned int a = line_addr(y);
        for (int b = 0; b < 40; b++) apple_poke(a + b, pattern(c, b));
    }
}

void apple_hcolor(int c) {
    hcolor = c & 7;
    apple_poke(0xE4, patterns[hcolor]);
}

static void dot(int x, int y, int xor) {
    if (x < 0 || x > 279 || y < 0 || y > 191) return;
    int b = x / 7, bit = 1 << (x % 7);
    unsigned int a = line_addr(y) + b;
    int old = apple_peek(a), v;
    if (xor) {
        if (old & bit) apple_poke(ZP_COLLISIONS, apple_peek(ZP_COLLISIONS) + 1);
        v = old ^ bit;
    } else {
        int p = pattern(hcolor, b);
        v = (old & ~bit & 0x7F) | (p & bit) | (p & 0x80);
    }
    apple_poke(a, v);
}

void apple_hplot(int x, int y) {
    dot(x, y, 0);
    last_x = x;
    last_y = y;
}

void apple_hplot_to(int x, int y) {
    int x0 = last_x, y0 = last_y;
    int dx = x > x0 ? x - x0 : x0 - x, sx = x > x0 ? 1 : -1;
    int dy = y > y0 ? y0 - y : y - y0, sy = y > y0 ? 1 : -1;
    int e = dx + dy;
    for (;;) {
        dot(x0, y0, 0);
        if (x0 == x && y0 == y) break;
        int e2 = 2 * e;
        if (e2 >= dy) {
            e += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            e += dx;
            y0 += sy;
        }
    }
    last_x = x;
    last_y = y;
}

void apple_rot(int r) { rot = r & 63; }
void apple_scale(int s) { scale = s & 255 ? s & 255 : 256; }

// Shape n (from 1) of the table; returns 0 if there's no such shape.
int apple_draw(int n, int x, int y, int xor) {
    unsigned int table = apple_peek(ZP_SHAPE) | (apple_peek(ZP_SHAPE + 1) << 8);
    if (n < 1 || n > apple_peek(table)) return 0;
    unsigned int a = table + (apple_peek(table + 2 * n) | (apple_peek(table + 2 * n + 1) << 8));
    if (x < 0) {                          // (DRAW without AT: where the last ended)
        x = last_x;
        y = last_y;
    }
    int turn = ((rot + 8) / 16) & 3;
    static const int dx[4] = { 0, 1, 0, -1 }, dy[4] = { -1, 0, 1, 0 };
    if (xor) apple_poke(ZP_COLLISIONS, 0);
    for (int guard = 0; guard < 4096; guard++, a++) {
        int b = apple_peek(a);
        if (b == 0) break;
        int moves[3] = { b & 7, (b >> 3) & 7, (b >> 6) & 3 };
        int count = moves[2] ? 3 : moves[1] ? 2 : 1;
        for (int m = 0; m < count; m++) {
            int dir = (moves[m] + turn) & 3, plot = moves[m] & 4 && m < 2;
            for (int s = 0; s < scale; s++) {
                if (plot) dot(x, y, xor);
                x += dx[dir];
                y += dy[dir];
            }
        }
    }
    last_x = x;
    last_y = y;
    return 1;
}
