/*
 * The Apple II memory map, for PEEK and POKE and for the screen, which
 * is drawn from it (video.c):
 *
 *   $0000-$BFFF  RAM: zero page, the stack, the text pages ($0400 and
 *                $0800), the high-resolution pages ($2000 and $4000) ...
 *   $C000-$C0FF  I/O: the keyboard, the speaker and the soft switches
 *   $C100-$FFFF  "ROM": reads as 0 (there is none: nothing of any Apple
 *                ROM is here), except the bytes programs read to tell
 *                the models apart ($FBB3, $FBC0); writes are ignored
 *
 * The IIe has another 64KB, auxiliary memory, used for 80 columns and
 * double resolution: its switches choose which 64KB the processor sees
 * (RAMRD and RAMWRT for $0200-$BFFF, ALTZP for $0000-$01FF, and with
 * 80STORE on, PAGE2 for the text page and, with HIRES, the first
 * high-resolution page).
 *
 * Soft switches ($C0xx), as on the real machines (any access flips
 * those marked *; the IIe's are written at $C000-$C00F and read back at
 * $C011-$C01F):
 *
 *   $C000 read   the keyboard: the last key, bit 7 set until $C010
 *   $C000/1      80STORE off/on         $C010*  clear the keyboard strobe
 *   $C002/3      RAMRD off/on           $C030*  click the speaker
 *   $C004/5      RAMWRT off/on          $C050/1* graphics/text
 *   $C008/9      ALTZP off/on           $C052/3* full screen/mixed
 *   $C00C/D      80COL off/on           $C054/5* page 1/page 2
 *   $C00E/F      ALTCHARSET off/on      $C056/7* low/high resolution
 *   $C05E/F*     double resolution on/off (with 80COL on)
 *   $C061-$C063  buttons 0-2: bit 7 set while pressed
 *
 * The 1976 model has no soft switches, just the terminal's: $D010 the
 * last key (bit 7 set), $D011 bit 7 set while a key is waiting (reading
 * $D010 takes it), $D012 written to show a character. All else below
 * $D000 is RAM.
 */
#include "apple.h"
#include "string.h"
#include "mem.h"

int apple_model;
struct apple_switches apple_sw;

HIGH_MEM static unsigned char ram_mem[2][65536];

// (Through a function: see core/mem.h.)
__attribute__((noinline, noclone)) static unsigned char *ram(int aux) {
    return ram_mem[aux];
}

int text_latch(void);                     // text.c: the keyboard, for $C000
void text_strobe(void);                   // $C010
int text_any_key(void);
int apple_key(void);
void plat_click(void);

void apple_init(int model) {
    apple_model = model;
    memset(ram(0), 0, 65536);
    memset(ram(1), 0, 65536);
    memset(&apple_sw, 0, sizeof(apple_sw));
    apple_sw.text = 1;
    video_init();
    text_init();
}

int apple_ram(int aux, unsigned int addr) {
    return ram(aux & 1)[addr & 0xFFFF];
}

void apple_set_ram(int aux, unsigned int addr, int value) {
    addr &= 0xFFFF;
    ram(aux & 1)[addr] = value;
    video_changed(addr, aux & 1);
}

// Which 64KB the processor reads (write = 0) or writes (1) at addr.
static int bank(unsigned int addr, int write) {
    if (apple_model != APPLE_IIE) return 0;
    if (addr < 0x200) return apple_sw.altzp;
    if (apple_sw.store80) {
        if (addr >= 0x400 && addr < 0x800) return apple_sw.page2;
        if (apple_sw.hires && addr >= 0x2000 && addr < 0x4000) return apple_sw.page2;
    }
    return write ? apple_sw.ramwrt : apple_sw.ramrd;
}

// A switch that any access flips (or that a IIe write at $C00x sets).
static void flip(unsigned char *sw, int value) {
    if (*sw != value) {
        *sw = value;
        video_mode_changed();
    }
}

static int io(unsigned int addr, int write) {
    int a = addr & 0xFF, iie = apple_model == APPLE_IIE;
    if (a < 0x10) {
        if (!write || !iie) return text_latch();
        switch (a) {
        case 0x0: case 0x1: flip(&apple_sw.store80, a & 1); break;
        case 0x2: case 0x3: apple_sw.ramrd = a & 1; break;
        case 0x4: case 0x5: apple_sw.ramwrt = a & 1; break;
        case 0x8: case 0x9: apple_sw.altzp = a & 1; break;
        case 0xC: case 0xD: flip(&apple_sw.col80, a & 1); break;
        case 0xE: case 0xF: flip(&apple_sw.altchar, a & 1); break;
        }
        return 0;
    }
    if (a == 0x10) {
        int any = text_any_key();
        text_strobe();
        return iie && any ? 0x80 : 0;
    }
    if (a < 0x20) {                       // the IIe's switches, read back
        if (!iie) return text_latch();
        int on = 0;
        switch (a) {
        case 0x13: on = apple_sw.ramrd; break;
        case 0x14: on = apple_sw.ramwrt; break;
        case 0x16: on = apple_sw.altzp; break;
        case 0x18: on = apple_sw.store80; break;
        case 0x1A: on = apple_sw.text; break;
        case 0x1B: on = apple_sw.mixed; break;
        case 0x1C: on = apple_sw.page2; break;
        case 0x1D: on = apple_sw.hires; break;
        case 0x1E: on = apple_sw.altchar; break;
        case 0x1F: on = apple_sw.col80; break;
        }
        return (on ? 0x80 : 0) | (text_latch() & 0x7F);
    }
    if (a >= 0x61 && a <= 0x63) return apple_button(a - 0x61) ? 0x80 : 0;
    if (a >= 0x30 && a < 0x40) {
        plat_click();
        return 0;
    }
    if (a >= 0x50 && a < 0x58) {
        unsigned char *sw[] = { &apple_sw.text, &apple_sw.mixed, &apple_sw.page2,
                                &apple_sw.hires };
        flip(sw[(a - 0x50) / 2], a & 1);  // (odd addresses turn them on)
        return 0;
    }
    if (a == 0x5E || a == 0x5F) {         // AN3: off ($C05E) is double resolution
        if (iie) flip(&apple_sw.dhires, a == 0x5E);
        return 0;
    }
    return 0;                             // buttons, paddles, slots: nothing there
}

// The 1976 terminal's keyboard and display.
static int last_key;
static int terminal(unsigned int addr, int write, int value) {
    switch (addr & 3) {
    case 0:                               // $D010: the key
        if (!write && apple_key_waiting()) last_key = apple_key() | 0x80;
        return last_key;
    case 1: return apple_key_waiting() ? 0x80 : 0;
    case 2:
        if (write) apple_cout(value & 0x7F);
        return 0;                         // (always ready for another)
    }
    return 0;
}

int apple_peek(unsigned int addr) {
    addr &= 0xFFFF;
    if (apple_model == APPLE_I) {
        if (addr < 0xD000) return ram(0)[addr];
        return (addr & 0xFFF0) == 0xD010 ? terminal(addr, 0, 0) : 0;
    }
    if (addr < 0xC000) return ram(bank(addr, 0))[addr];
    if (addr < 0xC100) return io(addr, 0);
    // The bytes programs read to tell which model they're on.
    if (addr == 0xFBB3) return apple_model == APPLE_II ? 0x38 :
                               apple_model == APPLE_IIPLUS ? 0xEA : 0x06;
    if (addr == 0xFBC0 && apple_model == APPLE_IIE) return 0xE0;
    return 0;
}

void apple_poke(unsigned int addr, int value) {
    addr &= 0xFFFF;
    if (apple_model == APPLE_I) {
        if (addr < 0xD000) ram(0)[addr] = value;
        else if ((addr & 0xFFF0) == 0xD010) terminal(addr, 1, value);
        return;
    }
    if (addr < 0xC000) {
        int b = bank(addr, 1);
        ram(b)[addr] = value;
        video_changed(addr, b);
    } else if (addr < 0xC100) {
        io(addr, 1);
    }
}
