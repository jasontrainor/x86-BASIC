/*
 * The platform layer (platform.h) for PCs booted through the BIOS:
 * VGA graphics, BIOS keyboard and timer services, and the PC speaker.
 */
#include "platform.h"
#include "string.h"

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
 * Display. A picture that fits is shown in VGA mode 13h: 320x200, one
 * byte per pixel at 0xA0000 (the Spectrum's 256x192 leaves a border of
 * 32 pixels left and right and 4 above and below). A bigger one uses
 * mode 12h: 640x480 in 16 colours, as four bit planes at 0xA0000, one
 * bit per pixel each, chosen for writing through the sequencer's map
 * mask. Every VGA has both. (Unreal mode lets the C code address
 * 0xA0000 directly.)
 *
 * Writing to the planes is slow (four writes for 8 pixels, each one a
 * trip out to the emulator in a virtual machine), so for a picture
 * shown twice as tall the VGA shows each line of memory twice itself
 * ("double scanning", as 200-line modes do): 640x240 in memory. And a
 * line that is all black and white (as text is) goes into all four
 * planes at once.
 * --------------------------------------------------------------------- */

#define VGA_MEM   ((volatile unsigned char *)0xA0000)

static int planar;                        // mode 12h
static int dscan;                         // ... double-scanned: 240 lines
static int vga_w, vga_h;                  // the mode's size
static int pic_w, pic_h, tall;            // the picture, as plat_video() gave it
static int win_x, win_y;                  // where it is (win_x a multiple of 8)

static void set_palette(const unsigned char (*rgb)[3]) {
    if (planar) {
        // Mode 12h's attribute controller maps colours 0-15 to DAC
        // registers 0-5, 20, 7, 56-63; make that 0-15. Reading 0x3DA
        // resets its index/data flip-flop.
        inb(0x3DA);
        for (int i = 0; i < 16; i++) {
            outb(0x3C0, i);
            outb(0x3C0, i);
        }
        outb(0x3C0, 0x20);                // and turn the picture back on
    }
    outb(0x3C8, 0);                       // DAC from colour 0: levels 0-63
    for (int i = 0; i < 16; i++)
        for (int k = 0; k < 3; k++) outb(0x3C9, rgb[i][k] >> 2);
}

void plat_video(const struct video *v) {
    planar = v->width > 320 || v->height * v->tall > 200;
    vga_w = planar ? 640 : 320;
    vga_h = planar ? 480 : 200;
    // (This also clears the screen, and gets rid of any text mode or
    // vendor logo the BIOS left.)
    __asm__ __volatile__ ("int $0x10" : : "a" (planar ? 0x0012 : 0x0013) : "cc", "memory");
    enter_unreal();
    pic_w = v->width;
    pic_h = v->height;
    tall = v->tall;
    dscan = planar && tall == 2;
    if (dscan) {                          // CRTC 9: two scan lines a line
        outb(0x3D4, 0x09);
        outb(0x3D5, (inb(0x3D5) & 0xE0) | 1);
        vga_h = 240;
        tall = 1;
    }
    win_x = (vga_w - pic_w) / 2 & ~7;
    win_y = (vga_h - pic_h * tall) / 2;
    set_palette(v->palette);
    if (planar) {
        outb(0x3CE, 0x05); outb(0x3CF, 0x00);   // graphics controller: write mode 0
        outb(0x3CE, 0x03); outb(0x3CF, 0x00);   // no rotate, no logical operation
        outb(0x3CE, 0x01); outb(0x3CF, 0x00);   // no set/reset
        outb(0x3CE, 0x08); outb(0x3CF, 0xFF);   // all 8 bits of each byte
    }
}

static void map_mask(int planes) {
    outb(0x3C4, 0x02);
    outb(0x3C5, planes);
}

void plat_put_block(int x, int y, int w, int h, const unsigned char *pixels) {
    for (int j = 0; j < h; j++, pixels += w) {
        for (int t = 0; t < tall; t++) {
            int row = win_y + (y + j) * tall + t;
            if (!planar) {
                volatile unsigned char *p = VGA_MEM + row * 320 + win_x + x;
                // Four pixels at a time: fewer, faster writes.
                for (int i = 0; i < w; i += 4)
                    *(volatile unsigned int *)(p + i) = pixels[i] | pixels[i + 1] << 8 |
                        pixels[i + 2] << 16 | (unsigned int)pixels[i + 3] << 24;
                continue;
            }
            volatile unsigned char *p = VGA_MEM + row * 80 + (win_x + x) / 8;
            int mono = 1;
            for (int i = 0; i < w && mono; i++) mono = pixels[i] == 0 || pixels[i] == 15;
            for (int plane = 0; plane < (mono ? 1 : 4); plane++) {
                map_mask(mono ? 0x0F : 1 << plane);
                for (int i = 0; i < w; i += 8) {
                    unsigned char b = 0;
                    for (int k = 0; k < 8; k++) b |= ((pixels[i + k] >> plane) & 1) << (7 - k);
                    p[i / 8] = b;
                }
            }
        }
    }
    if (planar) map_mask(0x0F);
}

// Fill a rectangle of the whole screen (x and w multiples of 8 in mode 12h).
static void fill(int x, int y, int w, int h, int colour) {
    for (int j = y; j < y + h; j++) {
        if (!planar) {
            unsigned int four = colour * 0x01010101u;
            volatile unsigned int *p = (volatile unsigned int *)(VGA_MEM + j * 320 + x);
            for (int i = 0; i < w / 4; i++) p[i] = four;
            continue;
        }
        volatile unsigned char *p = VGA_MEM + j * 80 + x / 8;
        for (int plane = 0; plane < 4; plane++) {
            map_mask(1 << plane);
            unsigned char b = (colour >> plane) & 1 ? 0xFF : 0;
            for (int i = 0; i < w / 8; i++) p[i] = b;
        }
    }
    if (planar) map_mask(0x0F);
}

void plat_border(int colour) {
    int win_w = pic_w, win_h = pic_h * tall;
    fill(0, 0, vga_w, win_y, colour);
    fill(0, win_y + win_h, vga_w, vga_h - win_y - win_h, colour);
    fill(0, win_y, win_x, win_h, colour);
    fill(win_x + win_w, win_y, vga_w - win_x - win_w, win_h, colour);
}

void plat_init(void) {
    // Put the timer (PIT channel 0) in mode 2, counting down once per
    // tick, at the same rate as the BIOS set (divisor 65536, 18.2Hz), so
    // that plat_timer_ms() can read how far through a tick it is. (The
    // BIOS's mode 3 counts down twice per tick, which can't be read.)
    __asm__ __volatile__ ("cli");
    outb(0x43, 0x34);                     // channel 0, low then high byte, mode 2
    outb(0x40, 0);
    outb(0x40, 0);
    __asm__ __volatile__ ("sti");
}

/* ------------------------------------------------------------------------
 * Keyboard: BIOS INT 16h
 * --------------------------------------------------------------------- */

static int f10_pressed(void);

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
        if (ascii == 0 && scan == 0x44) {      // F10
            if (f10_pressed()) plat_restart_menu();
            continue;
        }
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

// Milliseconds: whole ticks (54.925ms each) plus how far the PIT has
// counted through the current one (1193182 counts a second).
unsigned long plat_timer_ms(void) {
    unsigned int ticks, count;
    do {
        ticks = bios_ticks();
        __asm__ __volatile__ ("cli");
        outb(0x43, 0x00);                 // latch channel 0's count
        count = inb(0x40);
        count |= inb(0x40) << 8;
        __asm__ __volatile__ ("sti");
    } while (bios_ticks() != ticks);      // a tick went by: try again
    unsigned long into = (65536UL - (count ? count : 65536UL)) * 1000UL / 1193182UL;
    return ticks * 54UL + ticks * 925UL / 1000UL + into;
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

/* ------------------------------------------------------------------------
 * Back to the chooser: F10 twice
 *
 * The chooser is started by the boot sector, so the way back is to run
 * the boot sector again, after leaving a note in the BIOS data area's
 * "intra-applications communication area" (0x4F0-0x4FF, which the BIOS
 * leaves alone) that the menu is wanted.
 * --------------------------------------------------------------------- */

void restart_boot(void);                  // kernel_entry.asm
#define MENU_NOTE 0x554E454DU            // "MENU"

static unsigned long f10_time;
static int f10_once;

// F10 was pressed: is it the second time running?
static int f10_pressed(void) {
    unsigned long now = plat_timer_ms();
    if (f10_once && now - f10_time < 2000) return 1;
    f10_once = 1;
    f10_time = now;
    plat_tone(1200, 50);
    return 0;
}

void plat_restart_menu(void) {
    unsigned int note = MENU_NOTE;
    __asm__ __volatile__ ("movl %0, 0x04F0" : : "r" (note) : "memory");
    if (plat_disk_read(0, (void *)0x7C00) < 0) return;
    restart_boot();
}

int plat_buttons(void) {
    unsigned char shift, flags, extra;
    __asm__ __volatile__ ("movb 0x0417, %0" : "=q" (shift) : : "memory");
    __asm__ __volatile__ ("movb 0x0418, %0" : "=q" (flags) : : "memory");
    __asm__ __volatile__ ("movb 0x0496, %0" : "=q" (extra) : : "memory");
    // 0x418 bit 1: left Alt; 0x496 bit 3: right Alt (on an enhanced keyboard)
    return ((flags >> 1) & 1) | ((extra >> 2) & 2) | (shift & 3 ? 4 : 0);
}

int plat_menu_wanted(void) {
    unsigned int note;
    unsigned char shift;
    __asm__ __volatile__ ("movl 0x04F0, %0" : "=r" (note) : : "memory");
    __asm__ __volatile__ ("movb 0x0417, %0" : "=q" (shift) : : "memory");
    __asm__ __volatile__ ("movl $0, 0x04F0" : : : "memory");
    int wanted = note == MENU_NOTE || (shift & 3);   // (bits 0, 1: right, left Shift)
    while (plat_read_key() >= 0) wanted = 1;          // a key pressed while starting
    return wanted;
}

/* ------------------------------------------------------------------------
 * Disk: BIOS INT 13h, on the drive we booted from
 *
 * The BIOS can only transfer to memory it can address in real mode, so
 * every sector goes through a buffer in low memory (not crossing a 64KB
 * boundary, which floppy DMA can't). Like the boot sector, this uses LBA
 * (the INT 13h extensions) when the drive has them, and CHS otherwise.
 * --------------------------------------------------------------------- */

extern unsigned char boot_drive;          // kernel_entry.asm

static unsigned char sector_buf[512] __attribute__((aligned(512)));
static struct {
    unsigned char size, zero;
    unsigned short count, offset, segment;
    unsigned int lba_low, lba_high;
} __attribute__((packed)) dap;

static int disk_mode;                     // 0: not checked yet, 1: LBA, 2: CHS
static unsigned int sectors_per_track = 18, heads = 2;

static void check_disk(void) {
    unsigned short bx = 0x55AA;
    unsigned char cf;
    unsigned short cx;
    __asm__ __volatile__ (
        "int $0x13\n\t"
        "setc %0"
        : "=qm" (cf), "+b" (bx), "=c" (cx)
        : "a" (0x4100), "d" (boot_drive)
        : "cc", "memory"
    );
    enter_unreal();
    if (!cf && bx == 0xAA55 && (cx & 1)) {
        disk_mode = 1;
        return;
    }
    disk_mode = 2;
    unsigned short dx = boot_drive;
    __asm__ __volatile__ (
        "pushw %%es\n\t"
        "xor %%di, %%di\n\t"              // ES:DI = 0 works around buggy BIOSes
        "mov %%di, %%es\n\t"
        "int $0x13\n\t"
        "setc %0\n\t"
        "popw %%es"
        : "=qm" (cf), "=c" (cx), "+d" (dx)
        : "a" (0x0800)
        : "bx", "di", "cc", "memory"
    );
    enter_unreal();
    if (!cf && (cx & 0x3F)) {
        sectors_per_track = cx & 0x3F;
        heads = (dx >> 8) + 1;
    }
}

static int bios_disk(int write, unsigned int lba) {
    if (!disk_mode) check_disk();
    for (int tries = 0; tries < 3; tries++) {
        unsigned char cf;
        if (disk_mode == 1) {
            dap.size = 16;
            dap.zero = 0;
            dap.count = 1;
            dap.offset = (unsigned int)sector_buf;    // in segment 0
            dap.segment = 0;
            dap.lba_low = lba;
            dap.lba_high = 0;
            __asm__ __volatile__ (
                "int $0x13\n\t"
                "setc %0"
                : "=qm" (cf)
                : "a" (write ? 0x4300 : 0x4200), "d" (boot_drive), "S" (&dap)
                : "cc", "memory"
            );
        } else {
            unsigned int cyl = lba / sectors_per_track / heads;
            unsigned int head = lba / sectors_per_track % heads;
            unsigned int sec = lba % sectors_per_track + 1;
            __asm__ __volatile__ (
                "int $0x13\n\t"
                "setc %0"
                : "=qm" (cf)
                : "a" ((write ? 0x0300 : 0x0200) | 1),
                  "b" ((unsigned int)sector_buf),      // ES:BX, ES = 0
                  "c" (((cyl & 0xFF) << 8) | ((cyl >> 2) & 0xC0) | sec),
                  "d" ((head << 8) | boot_drive)
                : "cc", "memory"
            );
        }
        enter_unreal();
        if (!cf) return 0;
        __asm__ __volatile__ ("int $0x13" : : "a" (0), "d" (boot_drive) : "cc");  // reset
        enter_unreal();
    }
    return -1;
}

int plat_disk_read(unsigned int lba, void *buf) {
    if (bios_disk(0, lba) < 0) return -1;
    memcpy(buf, sector_buf, 512);
    return 0;
}

int plat_disk_write(unsigned int lba, const void *buf) {
    memcpy(sector_buf, buf, 512);
    return bios_disk(1, lba);
}

// The speaker's data bit (port 0x61 bit 1), with the timer's gate to it
// (bit 0) off, moves the cone directly.
void plat_click(void) {
    unsigned char v = inb(0x61);
    outb(0x61, (v & ~0x01) ^ 0x02);
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
