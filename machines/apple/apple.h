#ifndef APPLE_H
#define APPLE_H

/*
 * The Apple II-style hardware shared by the Apple machines (built with
 * the code here: their "uses" file says "apple"). It is a model of what
 * BASIC programs see - the memory map, the soft switches, the screen
 * and the keyboard - not an emulation of the 6502 or of any ROM.
 *
 *   memory.c   64KB of memory (and the IIe's 64KB of auxiliary memory),
 *              with the I/O soft switches at $C000-$C0FF
 *   video.c    the picture, drawn from memory as the video hardware does:
 *              text (40 or 80 columns), low-resolution, high-resolution,
 *              double low- and high-resolution (IIe), mixed modes, page 2
 *   text.c     the screen as the firmware writes it (the text window,
 *              cursor, scrolling), the keyboard, and line input
 *
 * Addresses are 0-65535; BASIC's negative ones (-16384) are the same
 * modulo 65536.
 */

enum {
    APPLE_II,              // 1977: upper case only
    APPLE_IIPLUS,          // 1979: the same hardware, as far as BASIC sees it
    APPLE_IIE,             // 1983: lower case, 80 columns, auxiliary memory,
                           // double resolution, MouseText (enhanced IIe)
};

extern int apple_model;

// Start: the model, memory cleared, text mode, page 1, the screen blank.
void apple_init(int model);

int apple_peek(unsigned int addr);
void apple_poke(unsigned int addr, int value);

// Main and auxiliary memory directly (no soft switches): aux = 1 for aux.
int apple_ram(int aux, unsigned int addr);
void apple_set_ram(int aux, unsigned int addr, int value);

// The soft switches the video hardware looks at.
struct apple_switches {
    unsigned char text, mixed, page2, hires;
    unsigned char col80, store80, altchar, dhires;   // (IIe)
    unsigned char ramrd, ramwrt, altzp;              // (IIe)
};
extern struct apple_switches apple_sw;

/* ------------------------------------------------------------------------
 * The picture (video.c)
 * --------------------------------------------------------------------- */

void video_init(void);
// Bring the picture up to date with memory (only what changed is drawn),
// and flash flashing characters. Call often: the keyboard functions do.
void video_update(void);
void video_changed(unsigned int addr, int aux);  // memory.c: a byte changed
void video_mode_changed(void);                   // memory.c: a switch changed
void video_cursor(int col, int row, int on);     // the input cursor

/* ------------------------------------------------------------------------
 * The firmware's screen and keyboard (text.c)
 *
 * The text window and cursor are kept in zero page, where programs PEEK
 * and POKE them: WNDLFT 32, WNDWDTH 33, WNDTOP 34, WNDBTM 35, CH 36,
 * CV 37, INVFLG 50 (255 normal, 63 inverse, 127 flashing), PROMPT 51.
 * --------------------------------------------------------------------- */

#define ZP_WNDLFT  32
#define ZP_WNDWDTH 33
#define ZP_WNDTOP  34
#define ZP_WNDBTM  35
#define ZP_CH      36
#define ZP_CV      37
#define ZP_INVFLG  50
#define ZP_PROMPT  51

void text_init(void);
void apple_cout(int c);              // print a character (ASCII; controls act)
void apple_print(const char *s);
void apple_home(void);               // clear the window, cursor to its top left
void apple_clreol(void);             // clear to the end of the line
void apple_clreop(void);             // clear to the end of the window
void apple_vtab(int row);            // cursor to row 0-23 (column unchanged)
void apple_htab(int col);            // cursor to column 0-39 (79), in the window
void apple_bell(void);
void apple_80col(int on);            // the IIe's 80-column firmware (PR#3)
int  apple_cursor_col(void);         // absolute column, 0-39 (79)
int  apple_cursor_row(void);

// Keyboard: the next key (ASCII, as the Apple keyboard would give it:
// upper case only before the IIe; arrows are 8, 21, 11, 10; Esc 27;
// Return 13; Delete 127), or -1 if none. Typed keys queue up.
int apple_key(void);
int apple_wait_key(void);            // with the cursor showing
int apple_key_waiting(void);
// Read a line, as the monitor's GETLN does: the prompt character, then
// editing with the arrows, Esc moves and Ctrl+X. Returns its length.
int apple_getln(int prompt, char *buf, int cap);

#endif
