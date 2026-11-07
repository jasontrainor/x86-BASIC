/*
 * The Spectrum-style screen and the keyboard queue, shared by every
 * platform.
 *
 * The picture is kept here as a 1-bit-per-pixel bitmap plus one colour
 * attribute per 8x8 cell, just as the Spectrum stores it; the platform
 * layer only has to draw cells (plat_draw_cell). Text is drawn with the
 * font in font.c.
 */
#include "screen.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

// The host test build records what is printed (see tests/host_stub.c).
#ifdef HOST_TEST
void host_echo(char c);
void host_trace(const char *fmt, int a, int b);
#define ECHO(c) host_echo(c)
#define TRACE(fmt, a, b) host_trace(fmt, a, b)
#else
#define ECHO(c) ((void)0)
#define TRACE(fmt, a, b) ((void)0)
#endif

HIGH_MEM static unsigned char bitmap[SCREEN_HEIGHT][SCREEN_COLS];
HIGH_MEM static unsigned char attrs[SCREEN_ROWS][SCREEN_COLS];  // see attr()
static unsigned char dirty[SCREEN_ROWS][SCREEN_COLS];   // changed, not yet drawn

static int cur_col, cur_row;      // cur_col == SCREEN_COLS: wrap before the next character
static int cursor_on;
static struct colours colour = { 0, 7, 0, 0, 0 };

// An attribute byte: ink in bits 0-2, paper in bits 3-5, bright in bit 6.
static unsigned char attr(void) {
    return colour.ink | (colour.paper << 3) | (colour.bright << 6);
}

static void draw(int c, int r) {
    unsigned char bits[8];
    for (int y = 0; y < 8; y++) bits[y] = bitmap[r * 8 + y][c];
    unsigned char a = attrs[r][c];
    int bright = (a & 0x40) ? 8 : 0;
    int ink = (a & 7) + bright, paper = ((a >> 3) & 7) + bright;
    if (cursor_on && c == cur_col && r == cur_row) {
        int t = ink; ink = paper; paper = t;          // the cursor: inverted
    }
    plat_draw_cell(c, r, bits, ink, paper);
    dirty[r][c] = 0;
}

static void draw_dirty(void) {
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            if (dirty[r][c]) draw(c, r);
}

static void draw_all(void) {
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            draw(c, r);
}

/* ------------------------------------------------------------------------
 * Text
 * --------------------------------------------------------------------- */

// The 8 rows of character code ch. 128-143 are the Spectrum's block
// graphics: 2x2 quarter-cell blocks, bit 0 top right, bit 1 top left,
// bit 2 bottom right, bit 3 bottom left.
static void glyph(unsigned char ch, unsigned char *rows) {
    if (ch >= 32 && ch <= 127) {
        memcpy(rows, font[ch - 32], 8);
    } else if (ch >= 128 && ch <= 143) {
        int n = ch - 128;
        unsigned char top = ((n & 2) ? 0xF0 : 0) | ((n & 1) ? 0x0F : 0);
        unsigned char bottom = ((n & 8) ? 0xF0 : 0) | ((n & 4) ? 0x0F : 0);
        for (int y = 0; y < 8; y++) rows[y] = y < 4 ? top : bottom;
    } else {
        memcpy(rows, font['?' - 32], 8);
    }
}

void put_char_at(int c, int r, char ch) {
    unsigned char rows[8];
    glyph((unsigned char)ch, rows);
    for (int y = 0; y < 8; y++) {
        unsigned char b = rows[y];
        if (colour.inverse) b = ~b;
        if (colour.over) b ^= bitmap[r * 8 + y][c];
        bitmap[r * 8 + y][c] = b;
    }
    attrs[r][c] = attr();
    draw(c, r);
}

static void scroll(void) {
    memmove(bitmap[0], bitmap[8], (SCREEN_HEIGHT - 8) * SCREEN_COLS);
    memset(bitmap[SCREEN_HEIGHT - 8], 0, 8 * SCREEN_COLS);
    memmove(attrs[0], attrs[1], (SCREEN_ROWS - 1) * SCREEN_COLS);
    memset(attrs[SCREEN_ROWS - 1], attr(), SCREEN_COLS);
    draw_all();
}

static void newline(void) {
    cur_col = 0;
    if (++cur_row == SCREEN_ROWS) {
        scroll();
        cur_row = SCREEN_ROWS - 1;
    }
}

void print_char(char ch) {
    ECHO(ch);
    if (ch == '\n') {
        newline();
    } else if (ch == '\r') {
        cur_col = 0;
    } else if (ch == 8) {                 // backspace: just move left
        if (cur_col > 0) {
            cur_col--;
        } else if (cur_row > 0) {
            cur_row--;
            cur_col = SCREEN_COLS - 1;
        }
    } else {
        // Wrap only when the next character arrives, so that a line of
        // exactly 32 characters followed by \n doesn't leave a blank line.
        if (cur_col == SCREEN_COLS) newline();
        put_char_at(cur_col, cur_row, ch);
        cur_col++;
    }
}

void print_string(const char *s) {
    while (*s) print_char(*s++);
}

void clear_screen(void) {
    TRACE("[CLS]", 0, 0);
    memset(bitmap, 0, sizeof(bitmap));
    memset(attrs, attr(), sizeof(attrs));
    cur_col = cur_row = 0;
    draw_all();
}

void set_cursor(int c, int r) {
    TRACE("[AT %d,%d]", r, c);
    cur_col = c;
    cur_row = r;
}

int cursor_col(void)  { return cur_col; }
int cursor_row(void)  { return cur_row; }
int screen_cols(void) { return SCREEN_COLS; }
int screen_rows(void) { return SCREEN_ROWS; }

void show_cursor(int on) {
    if (on && cur_col == SCREEN_COLS) newline();
    cursor_on = on;
    if (cur_col < SCREEN_COLS) draw(cur_col, cur_row);
}

/* ------------------------------------------------------------------------
 * Colours
 * --------------------------------------------------------------------- */

void set_ink(int c)       { TRACE("[INK %d]", c, 0); colour.ink = c; }
void set_paper(int c)     { TRACE("[PAPER %d]", c, 0); colour.paper = c; }
void set_bright(int on)   { TRACE("[BRIGHT %d]", on, 0); colour.bright = on; }
void set_inverse(int on)  { TRACE("[INVERSE %d]", on, 0); colour.inverse = on; }
void set_over(int on)     { TRACE("[OVER %d]", on, 0); colour.over = on; }
void get_colours(struct colours *c)       { *c = colour; }
void set_colours(const struct colours *c) { colour = *c; }

void set_border(int c) {
    TRACE("[BORDER %d]", c, 0);
    plat_border(c);
}

/* ------------------------------------------------------------------------
 * Graphics
 * --------------------------------------------------------------------- */

// Set a pixel (in ink; INVERSE 1: in paper; OVER 1: flip it) and give
// its cell the current colours. Drawn on screen by draw_dirty().
static void set_pixel(int x, int y) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= PLOT_HEIGHT) return;
    int py = PLOT_HEIGHT - 1 - y;
    unsigned char *b = &bitmap[py][x >> 3];
    unsigned char m = 0x80 >> (x & 7);
    if (colour.over) *b ^= m;
    else if (colour.inverse) *b &= ~m;
    else *b |= m;
    attrs[py >> 3][x >> 3] = attr();
    dirty[py >> 3][x >> 3] = 1;
}

void plot(int x, int y) {
    set_pixel(x, y);
    draw_dirty();
}

int point(int x, int y) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= PLOT_HEIGHT) return 0;
    return (bitmap[PLOT_HEIGHT - 1 - y][x >> 3] >> (7 - (x & 7))) & 1;
}

// A straight line, not including the starting point (which was already
// plotted: with OVER 1, plotting it again would rub it out).
void draw_line(int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x1 > x0 ? 1 : -1, sy = y1 > y0 ? 1 : -1;
    int err = dx - dy;
    while (x0 != x1 || y0 != y1) {
        int e2 = 2 * err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 < dx)  { err += dx; y0 += sy; }
        set_pixel(x0, y0);
    }
    draw_dirty();
}

void circle(int cx, int cy, int r) {
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        // The eight symmetric points, each plotted once (they coincide
        // where x == y or y == 0, which matters with OVER 1).
        int px[8] = { x, -x, x, -x, y, -y, y, -y };
        int py[8] = { y, y, -y, -y, x, x, -x, -x };
        for (int i = 0; i < 8; i++) {
            int dup = 0;
            for (int j = 0; j < i; j++)
                if (px[j] == px[i] && py[j] == py[i]) dup = 1;
            if (!dup) set_pixel(cx + px[i], cy + py[i]);
        }
        y++;
        if (err < 0) {
            err += 2 * y + 1;
        } else {
            x--;
            err += 2 * (y - x) + 1;
        }
    }
    draw_dirty();
}

/* ------------------------------------------------------------------------
 * Keyboard
 *
 * Keys go into a queue as they are noticed, so that watching for Esc
 * (BREAK) while a program runs doesn't throw other keys away.
 * --------------------------------------------------------------------- */

#define KEYQ_SIZE 256
static int keyq[KEYQ_SIZE];
static int keyq_head, keyq_len;
static int break_pending;

static void collect_keys(void) {
    int k;
    while ((k = plat_read_key()) >= 0) {
        if (k == 27 || k == 3) {          // Esc, Ctrl+C: BREAK
            break_pending = 1;
            keyq_len = 0;
        } else if (keyq_len < KEYQ_SIZE) {
            keyq[(keyq_head + keyq_len++) % KEYQ_SIZE] = k;
        }
    }
}

int poll_key(void) {
    collect_keys();
    if (break_pending) {
        break_pending = 0;
        return 27;
    }
    if (!keyq_len) return -1;
    int k = keyq[keyq_head];
    keyq_head = (keyq_head + 1) % KEYQ_SIZE;
    keyq_len--;
    return k;
}

void unget_key(int k) {
    if (keyq_len == KEYQ_SIZE) return;
    keyq_head = (keyq_head + KEYQ_SIZE - 1) % KEYQ_SIZE;
    keyq[keyq_head] = k;
    keyq_len++;
}

int key_waiting(void) {
    collect_keys();
    return break_pending || keyq_len > 0;
}

int check_break(void) {
    collect_keys();
    if (break_pending) {
        break_pending = 0;
        return 1;
    }
    return 0;
}

// Wait for a key, with a flashing cursor.
int get_key(void) {
    int k, on = 1;
    unsigned long last = timer_ms();
    show_cursor(1);
    while ((k = poll_key()) < 0) {
        unsigned long now = timer_ms();
        if (now - last >= 320) {
            on = !on;
            show_cursor(on);
            last = now;
        }
        plat_idle();
    }
    show_cursor(0);
    return k;
}

char get_char(void) {
    for (;;) {
        int k = get_key();
        if (k < 0x100) return (char)k;    // ignore cursor keys etc.
    }
}

/* ------------------------------------------------------------------------ */

unsigned long timer_ms(void) { return plat_timer_ms(); }
void tone(int freq, int ms)  { plat_tone(freq, ms); }

void screen_init(void) {
    struct colours spectrum = { 0, 7, 0, 0, 0 };   // black ink on white paper
    colour = spectrum;
    set_border(7);
    clear_screen();
}
