#include "bios.h"

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
}

void update_cursor() {
    __asm__ __volatile__ (
        "int $0x10"
        :
        : "a" (0x0200), "b" (0x0000), "d" ((cursor_y << 8) | cursor_x)
        : "cc"
    );
}

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

char get_char() {
    unsigned short ax;
    __asm__ __volatile__ (
        "int $0x16"
        : "=a" (ax)
        : "a" (0x0000)
        : "cc"
    );
    return (char)(ax & 0xFF);
}

// Non-blocking: returns 1 if Esc or Ctrl+C is waiting in the keyboard
// buffer. Any keys typed while a program runs are consumed, so a BREAK
// press can't get stuck behind them.
int check_break() {
    while (1) {
        unsigned char no_key;
        __asm__ __volatile__ (
            "int $0x16\n\t"
            "setz %0"
            : "=qm" (no_key)
            : "a" (0x0100)
            : "cc"
        );
        if (no_key) return 0;
        char c = get_char();
        if (c == 27 || c == 3) return 1;   // Esc, Ctrl+C
    }
}

void beep() {
    __asm__ __volatile__ (
        "mov $0xB6, %%al \n"
        "out %%al, $0x43 \n"
        "mov $0xA9, %%al \n"
        "out %%al, $0x42 \n"
        "mov $0x04, %%al \n"
        "out %%al, $0x42 \n"
        "in $0x61, %%al \n"
        "or $0x03, %%al \n"
        "out %%al, $0x61 \n"
        "mov $0x5FFFF, %%ecx \n"
        "1: loop 1b \n"
        "in $0x61, %%al \n"
        "and $0xFC, %%al \n"
        "out %%al, $0x61 \n"
        : : : "eax", "ecx"
    );
}

void init_video() {
    // Real BIOSes may leave a graphics or vendor-logo mode active, so
    // explicitly select 80x25 colour text mode (this also clears it).
    __asm__ __volatile__ ("int $0x10" : : "a" (0x0003) : "cc", "memory");

    // Attribute bit 7 means "blink" by default on real VGA hardware, which
    // would make bright paper colours (e.g. white, 15) flash. Switch it to
    // mean "bright background" so all 16 paper colours are usable.
    __asm__ __volatile__ ("int $0x10" : : "a" (0x1003), "b" (0x0000) : "cc");

    // Spectrum-style full block cursor: CH=0 (top line), CL=15 (bottom).
    __asm__ __volatile__ ("int $0x10" : : "a" (0x0100), "c" (0x000F) : "cc");
}
