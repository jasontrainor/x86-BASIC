/*
 * The platform layer (platform.h) for PCs booted through the BIOS:
 * VGA graphics, BIOS keyboard and timer services, and the PC speaker.
 */
#include "platform.h"

// kernel_entry.asm. Called after every BIOS call: a BIOS that switches to
// protected mode internally may leave the segment limits at 64KB.
void enter_unreal(void);

static void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__ ("outb %0, %1" : : "a" (val), "Nd" (port));
}

static unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ __volatile__ ("inb %1, %0" : "=a" (val) : "Nd" (port));
    return val;
}

/* ------------------------------------------------------------------------
 * Display: VGA mode 13h, 320x200 pixels with one byte per pixel at
 * 0xA0000. The 256x192 Spectrum screen sits in the middle, leaving a
 * border of 32 pixels left and right and 4 above and below. (Unreal mode
 * lets the C code address 0xA0000 directly.)
 * --------------------------------------------------------------------- */

#define VGA_MEM   ((volatile unsigned char *)0xA0000)
#define VGA_WIDTH 320
#define VGA_HEIGHT 200
#define WIN_X 32
#define WIN_Y 4

// Colours 0-7 are the Spectrum's: black, blue, red, magenta, green,
// cyan, yellow, white - so bit 0 is blue, bit 1 red and bit 2 green.
// 8-15 are their BRIGHT versions. VGA DAC levels are 0-63.
static void set_palette(void) {
    outb(0x3C8, 0);                       // start at colour 0
    for (int i = 0; i < 16; i++) {
        int level = (i & 8) ? 63 : 52;
        outb(0x3C9, (i & 2) ? level : 0); // red
        outb(0x3C9, (i & 4) ? level : 0); // green
        outb(0x3C9, (i & 1) ? level : 0); // blue
    }
}

void plat_init(void) {
    // Real BIOSes may leave a text mode or a vendor-logo mode active;
    // select 320x200 256-colour graphics (this also clears the screen).
    __asm__ __volatile__ ("int $0x10" : : "a" (0x0013) : "cc", "memory");
    enter_unreal();
    set_palette();
}

void plat_draw_cell(int col, int row, const unsigned char *bits, int ink, int paper) {
    volatile unsigned char *p = VGA_MEM + (WIN_Y + row * 8) * VGA_WIDTH + WIN_X + col * 8;
    for (int y = 0; y < 8; y++, p += VGA_WIDTH) {
        // Build the 8 pixels as two 32-bit words: fewer, faster writes.
        unsigned int lo = 0, hi = 0;
        for (int x = 0; x < 4; x++) {
            lo |= (unsigned int)((bits[y] & (0x80 >> x)) ? ink : paper) << (8 * x);
            hi |= (unsigned int)((bits[y] & (0x08 >> x)) ? ink : paper) << (8 * x);
        }
        ((volatile unsigned int *)p)[0] = lo;
        ((volatile unsigned int *)p)[1] = hi;
    }
}

static void fill(int x, int y, int w, int h, int colour) {
    unsigned int four = colour * 0x01010101u;
    for (int j = 0; j < h; j++) {
        volatile unsigned int *p = (volatile unsigned int *)(VGA_MEM + (y + j) * VGA_WIDTH + x);
        for (int i = 0; i < w / 4; i++) p[i] = four;
    }
}

void plat_border(int colour) {
    fill(0, 0, VGA_WIDTH, WIN_Y, colour);
    fill(0, VGA_HEIGHT - WIN_Y, VGA_WIDTH, WIN_Y, colour);
    fill(0, WIN_Y, WIN_X, VGA_HEIGHT - 2 * WIN_Y, colour);
    fill(VGA_WIDTH - WIN_X, WIN_Y, WIN_X, VGA_HEIGHT - 2 * WIN_Y, colour);
}

/* ------------------------------------------------------------------------
 * Keyboard: BIOS INT 16h
 * --------------------------------------------------------------------- */

int plat_read_key(void) {
    for (;;) {
        unsigned char no_key;
        __asm__ __volatile__ (
            "int $0x16\n\t"
            "setz %0"
            : "=qm" (no_key)
            : "a" (0x0100)                // AH=01h: is a key waiting?
            : "cc"
        );
        enter_unreal();
        if (no_key) return -1;
        unsigned short ax;
        __asm__ __volatile__ ("int $0x16" : "=a" (ax) : "a" (0x0000) : "cc");
        enter_unreal();
        unsigned char ascii = ax & 0xFF, scan = ax >> 8;
        if (ascii != 0 && ascii != 0xE0) return ascii;
        switch (scan) {                   // keys without a character
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x53: return KEY_DELETE;
        }
        // Some other key (F1, say): ignore it and look again.
    }
}

void plat_idle(void) {
    __asm__ __volatile__ ("hlt");         // sleep until the next interrupt
}

/* ------------------------------------------------------------------------
 * Time and sound
 * --------------------------------------------------------------------- */

// BIOS timer tick count at 0040:006C, incremented ~18.2 times a second
// by the BIOS timer interrupt. (Read via asm: GCC warns about
// dereferencing constant addresses below 4KB.)
static unsigned int bios_ticks(void) {
    unsigned int t;
    __asm__ __volatile__ ("movl 0x046C, %0" : "=r" (t) : : "memory");
    return t;
}

unsigned long plat_timer_ms(void) {
    return bios_ticks() * 55UL;
}

// Wait for the given number of timer ticks (~55ms each). This is
// independent of CPU speed, unlike a busy loop. If the timer interrupt
// isn't running, the tick count never changes, so give up after a
// bounded number of polls rather than hanging.
static void wait_ticks(int n) {
    unsigned int last = bios_ticks();
    unsigned long polls = 0;
    while (n > 0 && polls < 100000000UL) {
        unsigned int now = bios_ticks();
        if (now != last) {
            last = now;
            n--;
        }
        polls++;
    }
}

// The duration is rounded to whole timer ticks (~55ms).
void plat_tone(int freq, int ms) {
    int ticks = (ms + 27) / 55;
    if (ticks < 1 && ms > 0) ticks = 1;
    if (freq >= 20 && freq <= 20000) {
        // PIT channel 2, square wave, divisor = 1193182 Hz / freq.
        unsigned int div = 1193182 / freq;
        outb(0x43, 0xB6);
        outb(0x42, div & 0xFF);
        outb(0x42, div >> 8);
        outb(0x61, inb(0x61) | 0x03);    // gate the speaker on
        wait_ticks(ticks);
        outb(0x61, inb(0x61) & ~0x03);   // and off
    } else {
        wait_ticks(ticks);
    }
}
