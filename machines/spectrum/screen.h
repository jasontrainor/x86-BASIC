#ifndef SCREEN_H
#define SCREEN_H

/*
 * The Spectrum-style screen (machines/spectrum/screen.c), the keyboard queue and
 * other services BASIC uses, built on the platform layer (platform.h).
 *
 * The screen is 32 columns by 24 rows of 8x8 characters, a 256x192
 * pixel picture. Each character cell has one ink and one paper colour,
 * as on the Spectrum: drawing in a cell sets its colours.
 *
 * As on the Spectrum, it's split in two. The upper screen is where
 * programs print (stream 2); it scrolls, asking "scroll?" each time a
 * screenful has gone by. The lower screen, normally the bottom two rows,
 * is where lines are typed and edited, INPUT asks its questions and
 * reports appear (streams 0 and 1). It grows upwards as needed, over the
 * upper screen, and goes back to two rows when it's cleared. Its paper
 * is the border colour.
 */

#define SCREEN_COLS 32
#define SCREEN_ROWS 24
#define SCREEN_WIDTH 256      // pixels
#define SCREEN_HEIGHT 192
#define PLOT_HEIGHT 176       // PLOT y runs 0-175 (bottom two rows: text)

#include "font.h"                         // the character set

void screen_init(void);

// Which part of the screen print_char() etc. use.
void use_lower(int on);               // 1: the lower screen, 0: the upper
int using_lower(void);

// Text
void print_char(char c);
void print_string(const char *s);
void clear_screen(void);              // CLS: the whole screen
void clear_lower(void);               // the lower screen: blank, two rows
int set_cursor(int col, int row);     // AT; 0 if the row is off the screen
int cursor_col(void);
int cursor_row(void);
int upper_rows(void);                 // rows in the upper screen
int lower_rows(void);                 // rows in the lower screen
void lower_need_rows(int n);          // grow the lower screen to n rows
void lower_put(int col, int row, char c);     // editor: no cursor movement
void show_cursor(int on);             // draw/remove the cursor block
void transcript(const char *s);       // tests: record s as if printed
void screen_echo(int on);             // tests: record printing (default on)
int wait_key(void);                   // wait for a key, without a cursor
int scroll_stopped(void);             // 1 (once) if "scroll?" was refused
void set_scroll_count(int n);         // scrolls before "scroll?" (SCR_CT)
int scroll_count(void);

// Colours, kept as the Spectrum does: an attribute byte (flash<<7 |
// bright<<6 | paper<<3 | ink), a mask of the bits to leave as they are
// on the screen (INK 8 etc.), and INK 9/PAPER 9 (contrast), INVERSE and
// OVER (drawing XORs instead of replacing).
struct colours {
    unsigned char attr, mask;
    unsigned char ink9, paper9, inverse, over;
};
enum { C_INK, C_PAPER, C_FLASH, C_BRIGHT, C_INVERSE, C_OVER };
int colour_set(struct colours *c, int which, int n);   // 0 if n is invalid
void get_colours(struct colours *c);  // of the part of the screen in use
void set_colours(const struct colours *c);
void set_border(int c);               // 0-7; also the lower screen's paper
int border_colour(void);

// The screen as the Spectrum's memory holds it (for PEEK and POKE):
// pixel row y (0-191, from the top), byte col (0-31); attributes.
int screen_byte(int y, int col);
void screen_set_byte(int y, int col, int v);
int screen_attr(int row, int col);
void screen_set_attr(int row, int col, int v);
struct colours *permanent_colours(void);   // the upper screen's
int screen_char(int row, int col);    // SCREEN$: the character there, or -1

// Graphics: x 0-255 across, y 0-175 up from the bottom (as on the
// Spectrum), in the upper screen's colours. Points off the screen are
// ignored.
void plot(int x, int y);
int point(int x, int y);
void draw_line(int x0, int y0, int x1, int y1);
void circle(int cx, int cy, int r);

// Keyboard
char get_char(void);          // waits for a key, showing the cursor
int get_key(void);            // the same, but also returns KEY_ codes
int poll_key(void);           // next key, or -1 if none (doesn't wait)
int key_waiting(void);        // 1 if a key has been pressed (doesn't take it)
int check_break(void);        // 1 if Esc or Ctrl+C has been pressed
void unget_key(int k);        // put a key back, to be read next
int last_key(void);           // the last key read (LAST_K)
int held_key(void);           // a key pressed just now, or -1 (for IN)

// Time and sound
unsigned long timer_ms(void);
void tone(int freq, int ms);

#endif
