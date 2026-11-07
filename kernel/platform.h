#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * What each platform (the BIOS kernel, the UEFI kernel, the host test
 * stub) provides. Everything else - the screen, keyboard queue, BASIC -
 * is shared code built on these.
 */

// Set up the display. The screen is a 256x192 picture (32x24 cells of
// 8x8 pixels) inside a border, like the ZX Spectrum's.
void plat_init(void);

// Draw one 8x8 character cell. bits: 8 rows, top first, bit 7 = left
// pixel; set bits in colour ink, clear ones in paper. Colours are 0-15:
// the Spectrum's 8 colours, plus 8 for their BRIGHT versions.
void plat_draw_cell(int col, int row, const unsigned char *bits, int ink, int paper);

// Colour the border (0-7).
void plat_border(int colour);

// The next key typed, or -1 if there is none (doesn't wait). Printable
// characters are ASCII; also 8 (backspace), 13 (Enter), 27 (Esc), 3
// (Ctrl+C) and the KEY_ codes below.
int plat_read_key(void);

#define KEY_LEFT   0x101
#define KEY_RIGHT  0x102
#define KEY_UP     0x103
#define KEY_DOWN   0x104
#define KEY_HOME   0x105
#define KEY_END    0x106
#define KEY_DELETE 0x107

// Wait a little (until the next interrupt, say) when there is nothing
// to do but wait for a key.
void plat_idle(void);

// Milliseconds since some fixed point; may only change every ~55ms.
unsigned long plat_timer_ms(void);

// Sound freq Hz on the speaker for about ms milliseconds, then stop.
// freq 0 means silence (just wait).
void plat_tone(int freq, int ms);

// Read or write one 512-byte sector of the disk the system booted from
// (lba counts from the start of the disk). Return 0, or -1 on failure.
int plat_disk_read(unsigned int lba, void *buf);
int plat_disk_write(unsigned int lba, const void *buf);

#endif
