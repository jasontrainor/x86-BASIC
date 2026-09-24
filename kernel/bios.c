#include "bios.h"

int cursor_x = 0;
int cursor_y = 0;
unsigned char current_color = 0x70; // Spectrum default: White paper (7<<4), Black ink (0)

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
    );
}

void update_cursor() {
    __asm__ __volatile__ (
        "int $0x10"
        :
        : "a" (0x0200), "b" (0x0000), "d" ((cursor_y << 8) | cursor_x)
    );
}

void clear_screen() {
    unsigned char* vga = (unsigned char*)0xB8000;
    for (int i = 0; i < 80 * 25; i++) {
        vga[i * 2] = ' ';
        vga[i * 2 + 1] = current_color;
    }
    cursor_x = 0;
    cursor_y = 0;
    update_cursor();
}

void scroll() {
    unsigned char* vga = (unsigned char*)0xB8000;
    for (int i = 0; i < 80 * 24; i++) {
        vga[i * 2] = vga[(i + 80) * 2];
        vga[i * 2 + 1] = vga[(i + 80) * 2 + 1];
    }
    for (int i = 80 * 24; i < 80 * 25; i++) {
        vga[i * 2] = ' ';
        vga[i * 2 + 1] = current_color;
    }
    cursor_y = 24;
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
            unsigned char* vga = (unsigned char*)0xB8000;
            int offset = (cursor_y * 80 + cursor_x) * 2;
            vga[offset] = ' ';
            vga[offset + 1] = current_color;
        }
    } else {
        unsigned char* vga = (unsigned char*)0xB8000;
        int offset = (cursor_y * 80 + cursor_x) * 2;
        vga[offset] = c;
        vga[offset + 1] = current_color;
        cursor_x++;
        if (cursor_x >= 80) {
            cursor_x = 0;
            cursor_y++;
        }
    }
    if (cursor_y >= 25) {
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
    );
    return (char)(ax & 0xFF);
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
    // Hide cursor using standard INT 10h trick (set start line > end line)
    // Actually, Spectrum has a block cursor. Let's make it a block cursor!
    __asm__ __volatile__ (
        "int $0x10"
        :
        : "a" (0x0100), "c" (0x000F) // CH=0 (top), CL=15 (bottom) - full block
    );
}
