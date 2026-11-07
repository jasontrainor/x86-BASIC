#include "bios.h"

// kernel_entry.asm. Called after every BIOS call: a BIOS that switches to
// protected mode internally may leave the segment limits at 64KB.
void enter_unreal(void);

int cursor_x = 0;
int cursor_y = 0;
unsigned char current_color = 0x70; // Spectrum default: White paper (7<<4), Black ink (0)

/*
 * Text-mode video memory lives at physical 0xB8000. This code runs in
 * 16-bit real mode, where an address is segment*16 + offset and the
 * offset must stay below 64KB, so 0xB8000 can't be reached through the
 * zero-based DS that the C code uses. Instead, load FS (which GCC never
 * uses) with segment 0xB800 and access cells as small FS-relative offsets.
 * QEMU doesn't enforce the 64KB limit, but real CPUs fault on it.
 */
#define VGA_SEGMENT 0xB800
#define VGA_COLS 80
#define VGA_ROWS 25

static void vga_put(int cell, unsigned short value) {
    __asm__ __volatile__ (
        "mov %w2, %%fs\n\t"
        "movw %w0, %%fs:(%1)"
        :
        : "r" (value), "r" (cell * 2), "r" (VGA_SEGMENT)
        : "memory"
    );
}

static unsigned short vga_get(int cell) {
    unsigned short value;
    __asm__ __volatile__ (
        "mov %w2, %%fs\n\t"
        "movw %%fs:(%1), %w0"
        : "=r" (value)
        : "r" (cell * 2), "r" (VGA_SEGMENT)
        : "memory"
    );
    return value;
}

static unsigned short vga_cell(char c) {
    return (unsigned char)c | (current_color << 8);
}

// Map Spectrum colors (0-7) to VGA text colors
unsigned char spec_to_vga(int color) {
    switch (color) {
        case 0: return 0; // Black
        case 1: return 1; // Blue
        case 2: return 4; // Red
        case 3: return 5; // Magenta
        case 4: return 2; // Green
        case 5: return 3; // Cyan
        case 6: return 14; // Yellow
        case 7: return 15; // White
    }
    return 0;
}

void set_ink(int c) {
    current_color = (current_color & 0xF0) | (spec_to_vga(c) & 0x0F);
}

void set_paper(int c) {
    current_color = (current_color & 0x0F) | ((spec_to_vga(c) & 0x0F) << 4);
}

void set_border(int c) {
    int vga_col = spec_to_vga(c);
    __asm__ __volatile__ (
        "int $0x10"
        :
        : "a" (0x0B00), "b" (vga_col)
        : "cc"
    );
    enter_unreal();
}

void update_cursor() {
    __asm__ __volatile__ (
        "int $0x10"
        :
        : "a" (0x0200), "b" (0x0000), "d" ((cursor_y << 8) | cursor_x)
        : "cc"
    );
    enter_unreal();
}

void set_cursor(int col, int row) {
    cursor_x = col;
    cursor_y = row;
    update_cursor();
}

int cursor_col() { return cursor_x; }
int screen_cols() { return VGA_COLS; }
int screen_rows() { return VGA_ROWS; }

void clear_screen() {
    for (int i = 0; i < VGA_COLS * VGA_ROWS; i++) {
        vga_put(i, vga_cell(' '));
    }
    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

void scroll() {
    for (int i = 0; i < VGA_COLS * (VGA_ROWS - 1); i++) {
        vga_put(i, vga_get(i + VGA_COLS));
    }
    for (int i = VGA_COLS * (VGA_ROWS - 1); i < VGA_COLS * VGA_ROWS; i++) {
        vga_put(i, vga_cell(' '));
    }
    cursor_y = VGA_ROWS - 1;
}

void print_char(char c) {
    if (c == '\n') {
        cursor_x = 0;
        cursor_y++;
    } else if (c == '\r') {
        cursor_x = 0;
    } else if (c == 8) {
        if (cursor_x > 0) {
            cursor_x--;
            vga_put(cursor_y * VGA_COLS + cursor_x, vga_cell(' '));
        }
    } else {
        vga_put(cursor_y * VGA_COLS + cursor_x, vga_cell(c));
        cursor_x++;
        if (cursor_x >= VGA_COLS) {
            cursor_x = 0;
            cursor_y++;
        }
    }
    if (cursor_y >= VGA_ROWS) {
        scroll();
    }
    update_cursor();
}

void print_string(const char* str) {
    while (*str) {
        if (*str == '\n') {
            print_char('\r');
        }
        print_char(*str);
        str++;
    }
}

// ---------------------------------------------------------------------------
// Keyboard
//
// Keys are moved from the BIOS buffer into our own small queue, so that
// check_break() can look for Esc among keys typed while a program runs
// without throwing the others away (INKEY$ and INPUT still get them).
// ---------------------------------------------------------------------------

#define KEYQ_SIZE 32
static char keyq[KEYQ_SIZE];
static int keyq_head, keyq_len;
static int break_pending;

// Move any keys waiting in the BIOS buffer into the queue (non-blocking).
static void poll_bios_keys() {
    while (1) {
        unsigned char no_key;
        __asm__ __volatile__ (
            "int $0x16\n\t"
            "setz %0"
            : "=qm" (no_key)
            : "a" (0x0100)
            : "cc"
        );
        enter_unreal();
        if (no_key) return;
        unsigned short ax;
        __asm__ __volatile__ ("int $0x16" : "=a" (ax) : "a" (0x0000) : "cc");
        enter_unreal();
        char c = (char)(ax & 0xFF);
        if (c == 27 || c == 3) {           // Esc, Ctrl+C: BREAK
            break_pending = 1;
            keyq_len = 0;
        } else if (keyq_len < KEYQ_SIZE) {
            keyq[(keyq_head + keyq_len++) % KEYQ_SIZE] = c;
        }
    }
}

// Next key, or -1 if none is waiting. Esc comes back as 27.
int poll_key() {
    poll_bios_keys();
    if (break_pending) {
        break_pending = 0;
        return 27;
    }
    if (!keyq_len) return -1;
    char c = keyq[keyq_head];
    keyq_head = (keyq_head + 1) % KEYQ_SIZE;
    keyq_len--;
    return (unsigned char)c;
}

int key_waiting() {
    poll_bios_keys();
    return break_pending || keyq_len > 0;
}

char get_char() {
    int c;
    while ((c = poll_key()) < 0) {
        __asm__ __volatile__ ("hlt");      // sleep until the next interrupt
    }
    return (char)c;
}

// Non-blocking: returns 1 if Esc or Ctrl+C has been pressed.
int check_break() {
    poll_bios_keys();
    if (break_pending) {
        break_pending = 0;
        return 1;
    }
    return 0;
}

static void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__ ("outb %0, %1" : : "a" (val), "Nd" (port));
}

static unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ __volatile__ ("inb %1, %0" : "=a" (val) : "Nd" (port));
    return val;
}

// BIOS timer tick count at 0040:006C, incremented ~18.2 times a second
// by the BIOS timer interrupt. Lies inside segment 0, so DS can reach it.
// (Read via asm: GCC warns about dereferencing constant addresses < 4KB.)
static unsigned int bios_ticks() {
    unsigned int t;
    __asm__ __volatile__ ("movl 0x046C, %0" : "=r" (t) : : "memory");
    return t;
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

unsigned long timer_ms() {
    return bios_ticks() * 55UL;
}

// Play a tone of freq Hz on the PC speaker for about ms milliseconds
// (to the nearest timer tick, ~55ms). freq 0 is a rest.
void tone(int freq, int ms) {
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

void init_video() {
    // Real BIOSes may leave a graphics or vendor-logo mode active, so
    // explicitly select 80x25 colour text mode (this also clears it).
    __asm__ __volatile__ ("int $0x10" : : "a" (0x0003) : "cc", "memory");
    enter_unreal();

    // Attribute bit 7 means "blink" by default on real VGA hardware, which
    // would make bright paper colours (e.g. white, 15) flash. Switch it to
    // mean "bright background" so all 16 paper colours are usable.
    __asm__ __volatile__ ("int $0x10" : : "a" (0x1003), "b" (0x0000) : "cc");
    enter_unreal();

    // Spectrum-style full block cursor: CH=0 (top line), CL=15 (bottom).
    __asm__ __volatile__ ("int $0x10" : : "a" (0x0100), "c" (0x000F) : "cc");
    enter_unreal();
}
