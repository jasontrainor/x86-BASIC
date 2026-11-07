#ifndef SCREEN_H
#define SCREEN_H

/*
 * The Spectrum-style screen (kernel/screen.c), the keyboard queue and
 * other services BASIC uses, built on the platform layer (platform.h).
 *
 * The screen is 32 columns by 24 rows of 8x8 characters, a 256x192
 * pixel picture. Each character cell has one ink and one paper colour,
 * as on the Spectrum: drawing in a cell sets its colours.
 */

#define SCREEN_COLS 32
#define SCREEN_ROWS 24
#define SCREEN_WIDTH 256      // pixels
#define SCREEN_HEIGHT 192
#define PLOT_HEIGHT 176       // PLOT y runs 0-175 (bottom two rows: text)

extern const unsigned char font[96][8];   // font.c: characters 32-127

void screen_init(void);

// Text
void print_char(char c);
void print_string(const char *s);
void clear_screen(void);              // CLS: also homes the cursor
void set_cursor(int col, int row);
int cursor_col(void);
int cursor_row(void);
int screen_cols(void);
int screen_rows(void);
void put_char_at(int col, int row, char c);   // no cursor movement
void show_cursor(int on);             // draw/remove the cursor block
void place_cursor(int col, int row);  // like set_cursor, for the editor
void scroll_up(void);                 // scroll the screen up a row
void transcript(const char *s);       // tests: record s as if printed

// Colours and attributes
void set_ink(int c);                  // 0-7
void set_paper(int c);                // 0-7
void set_border(int c);               // 0-7
void set_bright(int on);
void set_inverse(int on);
void set_over(int on);                // drawing XORs instead of replacing

struct colours { int ink, paper, bright, inverse, over; };
void get_colours(struct colours *c);
void set_colours(const struct colours *c);

// Graphics: x 0-255 across, y 0-175 up from the bottom (as on the
// Spectrum). Points off the screen are ignored.
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

// Time and sound
unsigned long timer_ms(void);
void tone(int freq, int ms);

#endif
