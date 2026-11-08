#ifndef PLATFORM_H
#define PLATFORM_H

/*
 * What each platform (the BIOS kernel, the UEFI kernel, the host test
 * stub) provides. Everything else - the screen, keyboard queue, BASIC -
 * is shared code built on these.
 */

// Get ready: timers, input and anything else the platform needs. The
// machine then sets up the display with plat_video().
void plat_init(void);

/*
 * The display: a picture of width x height pixels (width a multiple of
 * 8) in up to 16 colours, centred in a border. Each pixel is `tall`
 * times as tall as it is wide (1, or 2 for a picture like 560x192 that
 * a TV shows twice as tall). frame_width x frame_height (in the same
 * units as width and height*tall) is the picture plus the least border
 * the machine wants; the platform scales the picture by the largest
 * whole number that fits the frame on the screen.
 *
 * The BIOS kernel uses VGA mode 13h (320x200) for pictures that fit in
 * it, and mode 12h (640x480, 16 colours) for bigger ones.
 */
struct video {
    int width, height, tall;
    int frame_width, frame_height;
    unsigned char palette[16][3];         // red, green, blue: 0-255
};
void plat_video(const struct video *v);

// Draw a w x h block of the picture at (x, y), from w*h colours (0-15),
// row by row. x and w must be multiples of 8.
void plat_put_block(int x, int y, int w, int h, const unsigned char *pixels);

// Fill the border with colour (0-15).
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
