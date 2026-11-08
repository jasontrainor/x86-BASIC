/*
 * The Spectrum's memory map, for PEEK and POKE: see memory.h.
 *
 * Addresses of the system variables are the Spectrum's (they're facts
 * about the machine that BASIC programs rely on); the values behind the
 * ones that matter come from the interpreter and the screen.
 */
#include "memory.h"
#include "screen.h"
#include "string.h"
#include "mem.h"

HIGH_MEM static unsigned char ram[65536 - 16384];   // 16384-65535
// Used through a pointer: in the 16-bit BIOS kernel, GCC would reach
// ram[constant] with a 16-bit address, which can't get above 64KB.
unsigned char *ram_p = ram;              // (not static: GCC mustn't fold it away)
#define RAM(a) ram_p[(a) - 16384]

// The interpreter's state behind some system variables (basic.c).
extern unsigned int rnd_seed;
extern int plot_x, plot_y;

static unsigned long frames_base;     // FRAMES = timer / 20ms - frames_base

#define FONT_START 15616              // characters 32-127 in the "ROM"
#define DISPLAY 16384
#define ATTRS 22528
#define ATTRS_END 23296

// System variables
enum {
    LAST_K = 23560, REPDEL = 23561, REPPER = 23562, BORDCR = 23624,
    PROG = 23635, VARS = 23627, E_LINE = 23641, DF_SZ = 23659,
    SEED = 23670, FRAMES = 23672, COORDS = 23677, S_POSN = 23688,
    SCR_CT = 23692, ATTR_P = 23693, MASK_P = 23694, ATTR_T = 23695,
    MASK_T = 23696, P_FLAG = 23697, RAMTOP = 23730, P_RAMT = 23732,
};

static void set16(unsigned int a, unsigned int v) {
    RAM(a) = v & 0xFF;
    RAM(a + 1) = (v >> 8) & 0xFF;
}

// A pixel row byte: y 0-191 from the top, as the Spectrum's addresses
// interleave them: 010y7y6 y2y1y0 y5y4y3 x4..x0.
static void display_pos(unsigned int a, int *y, int *col) {
    unsigned int o = a - DISPLAY;
    *y = ((o >> 5) & 0xC0) | ((o >> 8) & 0x07) | ((o >> 2) & 0x38);
    *col = o & 31;
}

void mem_reset(void) {
    memset(ram_p, 0, sizeof(ram));
    set16(SYS_CHARS, FONT_START - 256);   // (CHARS points 256 below char 32)
    set16(SYS_UDG, UDG_START);
    set16(PROG, 23755);
    set16(VARS, 23755);
    set16(E_LINE, 23756);
    set16(RAMTOP, UDG_START - 1);
    set16(P_RAMT, 65535);
    RAM(REPDEL) = 35;
    RAM(REPPER) = 5;
    // The user-defined graphics start as copies of the letters A-U.
    for (int i = 0; i < 21 * 8; i++)
        RAM(UDG_START + i) = font['A' - 32 + i / 8][i % 8];
    frames_base = timer_ms() / 20;
}

int mem_peek(unsigned int a) {
    a &= 0xFFFF;
    if (a < DISPLAY) {
        if (a >= FONT_START) return font[(a - FONT_START) / 8][(a - FONT_START) % 8];
        return 0;
    }
    if (a < ATTRS) {
        int y, col;
        display_pos(a, &y, &col);
        return screen_byte(y, col);
    }
    if (a < ATTRS_END) return screen_attr((a - ATTRS) / 32, (a - ATTRS) % 32);
    struct colours *pc = permanent_colours();
    unsigned long frames = timer_ms() / 20 - frames_base;
    switch (a) {
    case LAST_K:     return last_key() & 0xFF;
    case BORDCR: {
        int b = border_colour();
        return (b << 3) | (b < 4 ? 7 : 0);
    }
    case DF_SZ:      return lower_rows();
    case SEED:       return rnd_seed & 0xFF;
    case SEED + 1:   return (rnd_seed >> 8) & 0xFF;
    case FRAMES:     return frames & 0xFF;
    case FRAMES + 1: return (frames >> 8) & 0xFF;
    case FRAMES + 2: return (frames >> 16) & 0xFF;
    case COORDS:     return plot_x & 0xFF;
    case COORDS + 1: return plot_y & 0xFF;
    case S_POSN:     return using_lower() ? 33 : 33 - cursor_col();
    case S_POSN + 1: return using_lower() ? 24 : 24 - cursor_row();
    case SCR_CT:     return scroll_count() & 0xFF;
    case ATTR_P:
    case ATTR_T:     return pc->attr;
    case MASK_P:
    case MASK_T:     return pc->mask;
    case P_FLAG: {
        int f = (pc->over ? 1 : 0) | (pc->inverse ? 4 : 0) |
                (pc->ink9 ? 16 : 0) | (pc->paper9 ? 64 : 0);
        return f | (f << 1);          // temporary and permanent bits
    }
    }
    return RAM(a);
}

int mem_peek16(unsigned int a) {
    return mem_peek(a) | (mem_peek(a + 1) << 8);
}

void mem_poke(unsigned int a, int v) {
    a &= 0xFFFF;
    v &= 0xFF;
    if (a < DISPLAY) return;                  // ROM
    if (a < ATTRS) {
        int y, col;
        display_pos(a, &y, &col);
        screen_set_byte(y, col, v);
        return;
    }
    if (a < ATTRS_END) {
        screen_set_attr((a - ATTRS) / 32, (a - ATTRS) % 32, v);
        return;
    }
    struct colours *pc = permanent_colours();
    switch (a) {
    case SEED:       rnd_seed = (rnd_seed & 0xFF00) | v; return;
    case SEED + 1:   rnd_seed = (rnd_seed & 0x00FF) | (v << 8); return;
    case FRAMES:
    case FRAMES + 1:
    case FRAMES + 2: {
        unsigned long frames = timer_ms() / 20 - frames_base;
        int shift = (a - FRAMES) * 8;
        frames = (frames & ~(0xFFUL << shift)) | ((unsigned long)v << shift);
        frames_base = timer_ms() / 20 - (frames & 0xFFFFFF);
        return;
    }
    case COORDS:     plot_x = v; return;
    case COORDS + 1: plot_y = v > 175 ? 175 : v; return;
    case SCR_CT:     set_scroll_count(v); return;
    case ATTR_P:
    case ATTR_T:     pc->attr = v; return;
    case MASK_P:
    case MASK_T:     pc->mask = v; return;
    case P_FLAG:
        pc->over = (v & 3) != 0;
        pc->inverse = (v & 12) != 0;
        pc->ink9 = (v & 48) != 0;
        pc->paper9 = (v & 192) != 0;
        return;
    case DF_SZ:                           // (the screen's layout is fixed)
    case LAST_K:
    case BORDCR:
    case S_POSN:
    case S_POSN + 1:
        return;
    }
    RAM(a) = v;
}
