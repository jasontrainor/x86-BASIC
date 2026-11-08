/*
 * Low-resolution graphics, as both BASICs' GR, COLOR=, PLOT, HLIN, VLIN
 * and SCRN do them: 40x48 blocks on text page 1, two to a byte (the
 * upper block in the low four bits). Written for this project from how
 * the machines behave.
 */
#include "apple.h"

#define ZP_COLOR 0x30                     // the colour, in both halves of a byte

static unsigned int addr(int x, int y) {
    int row = y >> 1;
    return 0x400 + (row & 7) * 128 + (row >> 3) * 40 + x;
}

void apple_gr(void) {
    if (apple_model == APPLE_I) return;
    apple_poke(0xC050, 0);                // graphics
    apple_poke(0xC053, 0);                // mixed: four lines of text below
    apple_poke(0xC054, 0);                // page 1
    apple_poke(0xC056, 0);                // low resolution
    apple_color(0);
    for (int y = 0; y < 40; y += 2) apple_hlin(0, 39, y), apple_hlin(0, 39, y + 1);
    apple_poke(ZP_WNDTOP, 20);
    apple_poke(ZP_WNDBTM, 24);
    apple_vtab(23);
}

void apple_color(int c) {
    apple_poke(ZP_COLOR, (c & 15) * 17);
}

int apple_get_color(void) {
    return apple_peek(ZP_COLOR) & 15;
}

void apple_plot(int x, int y) {
    unsigned int a = addr(x, y);
    int old = apple_ram(0, a), c = apple_get_color();
    apple_set_ram(0, a, y & 1 ? (old & 0x0F) | (c << 4) : (old & 0xF0) | c);
}

void apple_hlin(int x1, int x2, int y) {
    do apple_plot(x1, y); while (++x1 <= x2);   // (x1 > x2: just x1)
}

void apple_vlin(int y1, int y2, int x) {
    do apple_plot(x, y1); while (++y1 <= y2);
}

int apple_scrn(int x, int y) {
    int b = apple_ram(0, addr(x, y));
    return y & 1 ? b >> 4 : b & 15;
}
