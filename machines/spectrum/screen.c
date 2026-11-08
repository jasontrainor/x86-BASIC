/*
 * The Spectrum-style screen and the keyboard queue, shared by every
 * platform.
 *
 * The picture is kept here as a 1-bit-per-pixel bitmap plus one colour
 * attribute per 8x8 cell, just as the Spectrum stores it, and drawn a
 * cell at a time (plat_put_block). Text is drawn with the font in font.c.
 */
#include "screen.h"
#include "platform.h"
#include "string.h"
#include "mem.h"
#include "memory.h"

// The host test build records what is printed (see tests/host_stub.c).
#ifdef HOST_TEST
void host_echo(char c);
void host_trace(const char *fmt, int a, int b);
extern int host_scroll_prompt;
static int echo = 1;
#define ECHO(c) do { if (echo) host_echo(c); } while (0)
#define TRACE(fmt, a, b) do { if (echo) host_trace(fmt, a, b); } while (0)
#else
#define ECHO(c) ((void)0)
#define TRACE(fmt, a, b) ((void)0)
#endif

HIGH_MEM static unsigned char bitmap[SCREEN_HEIGHT][SCREEN_COLS];
HIGH_MEM static unsigned char attrs[SCREEN_ROWS][SCREEN_COLS];
HIGH_MEM static unsigned char dirty[SCREEN_ROWS][SCREEN_COLS];   // changed, not yet drawn

/*
 * The two parts of the screen, each with a print position and colours.
 * col == SCREEN_COLS means "wrap before the next character". The lower
 * screen's row counts from its top line.
 */
struct part {
    int col, row;
    struct colours colour;
};
static struct part upper, lower;
static struct part *cur = &upper;
static int lower_size = 2;        // rows in the lower screen (DF_SZ)
static int border = 7;
static int cursor_on;
static int flash_on;              // FLASH cells show swapped colours
static unsigned long flash_time;
static int scr_ct = 1;            // scrolls left before "scroll?"

// How long the printing and drawing done would have taken a Spectrum,
// in microseconds (basic.c holds programs back to match).
unsigned long screen_cost_us;
static int scroll_refused;

static int top_of_lower(void) { return SCREEN_ROWS - lower_size; }

// The screen row of a part's row.
static int abs_row(struct part *pt, int row) {
    return pt == &lower ? top_of_lower() + row : row;
}

// The lower screen's colours: border-coloured paper, and ink that shows
// up on it (white on dark colours, black on light ones), as BORDCR.
static struct colours lower_colours(void) {
    struct colours c = { 0, 0, 0, 0, 0, 0 };
    c.attr = (border << 3) | (border < 4 ? 7 : 0);
    return c;
}

// The attribute a cell gets when drawn on in colours c (old: its
// attribute before).
static unsigned char cell_attr(const struct colours *c, unsigned char old) {
    unsigned char a = (c->attr & ~c->mask) | (old & c->mask);
    if (c->ink9) a = (a & ~0x07) | (((a >> 3) & 7) >= 4 ? 0 : 7);
    if (c->paper9) a = (a & ~0x38) | (((a & 7) >= 4 ? 0 : 7) << 3);
    return a;
}

static void draw(int c, int r) {
    unsigned char bits[8];
    for (int y = 0; y < 8; y++) bits[y] = bitmap[r * 8 + y][c];
    unsigned char a = attrs[r][c];
    int bright = (a & 0x40) ? 8 : 0;
    int ink = (a & 7) + bright, paper = ((a >> 3) & 7) + bright;
    int swap = (a & 0x80) && flash_on;
    if (cursor_on && c == cur->col && r == abs_row(cur, cur->row)) swap = !swap;
    if (swap) { int t = ink; ink = paper; paper = t; }
    unsigned char px[64];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) px[y * 8 + x] = (bits[y] & (0x80 >> x)) ? ink : paper;
    plat_put_block(c * 8, r * 8, 8, 8, px);
    dirty[r][c] = 0;
}

static void draw_dirty(void) {
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            if (dirty[r][c]) draw(c, r);
}

static void draw_rows(int from, int to) {
    for (int r = from; r < to; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            draw(c, r);
}

// FLASH: swap the colours of flashing cells every 16 frames (0.32s).
// Called whenever the keyboard is looked at, which is often.
static void flash_tick(void) {
    unsigned long now = plat_timer_ms();
    if (now - flash_time < 320) return;
    flash_time = now;
    flash_on = !flash_on;
    for (int r = 0; r < SCREEN_ROWS; r++)
        for (int c = 0; c < SCREEN_COLS; c++)
            if (attrs[r][c] & 0x80) draw(c, r);
}

/* ------------------------------------------------------------------------
 * Text
 * --------------------------------------------------------------------- */

// The 8 rows of character code ch, as on the Spectrum: 32-127 from the
// character set CHARS points to (normally the font, in the "ROM"), 144-164
// the user-defined graphics UDG points to, and 128-143 block graphics:
// 2x2 quarter-cell blocks, bit 0 top right, bit 1 top left, bit 2 bottom
// right, bit 3 bottom left.
static void glyph(unsigned char ch, unsigned char *rows) {
    if (ch >= 32 && ch <= 127) {
        unsigned int a = mem_peek16(SYS_CHARS) + ch * 8;
        for (int y = 0; y < 8; y++) rows[y] = mem_peek(a + y);
    } else if (ch >= 144 && ch <= 164) {
        unsigned int a = mem_peek16(SYS_UDG) + (ch - 144) * 8;
        for (int y = 0; y < 8; y++) rows[y] = mem_peek(a + y);
    } else if (ch >= 128 && ch <= 143) {
        int n = ch - 128;
        unsigned char top = ((n & 2) ? 0xF0 : 0) | ((n & 1) ? 0x0F : 0);
        unsigned char bottom = ((n & 8) ? 0xF0 : 0) | ((n & 4) ? 0x0F : 0);
        for (int y = 0; y < 8; y++) rows[y] = y < 4 ? top : bottom;
    } else {
        memcpy(rows, font['?' - 32], 8);
    }
}

// Draw character ch in cell (c, r) of the screen, in colours col.
static void put_cell(int c, int r, char ch, const struct colours *col) {
    unsigned char rows[8];
    glyph((unsigned char)ch, rows);
    for (int y = 0; y < 8; y++) {
        unsigned char b = rows[y];
        if (col->inverse) b = ~b;
        if (col->over) b ^= bitmap[r * 8 + y][c];
        bitmap[r * 8 + y][c] = b;
    }
    attrs[r][c] = cell_attr(col, attrs[r][c]);
    draw(c, r);
    screen_cost_us += 300;
}

void lower_put(int col, int row, char ch) {
    put_cell(col, top_of_lower() + row, ch, &lower.colour);
}

// Scroll screen rows [from, to) up by one, blanking the last in attr a.
static void scroll_rows(int from, int to, unsigned char a) {
    screen_cost_us += 700 * (to - from);
    if (to - from > 1) {
        memmove(bitmap[from * 8], bitmap[(from + 1) * 8], (to - from - 1) * 8 * SCREEN_COLS);
        memmove(attrs[from], attrs[from + 1], (to - from - 1) * SCREEN_COLS);
    }
    memset(bitmap[(to - 1) * 8], 0, 8 * SCREEN_COLS);
    memset(attrs[to - 1], a, SCREEN_COLS);
    draw_rows(from, to);
}

// "scroll?": 1 to carry on, 0 if the answer was N, space or BREAK.
static int ask_scroll(void) {
    struct part *was = cur;
    struct part saved = lower;
    int saved_size = lower_size;
    cur = &lower;
    lower.col = lower.row = 0;
    lower.colour = lower_colours();
    for (const char *s = "scroll?"; *s; s++) lower_put(lower.col++, 0, *s);
    transcript("scroll?\n");
    int k = wait_key();
    for (int i = 0; i < 7; i++) lower_put(i, 0, ' ');
    lower = saved;
    lower_size = saved_size;
    cur = was;
    return !(k == 'n' || k == 'N' || k == ' ' || k == 27);
}

static void scroll_upper(void) {
#ifdef HOST_TEST
    if (!host_scroll_prompt) scr_ct = 2;  // most tests don't want to answer
#endif
    if (--scr_ct <= 0) {
        if (!ask_scroll()) {
            scroll_refused = 1;
            scr_ct = 1;
            return;
        }
        scr_ct = upper_rows();
    }
    scroll_rows(0, upper_rows(), upper.colour.attr);
}

static void newline(void) {
    cur->col = 0;
    if (cur == &upper) {
        if (++upper.row >= upper_rows()) {
            upper.row = upper_rows() - 1;
            scroll_upper();
        }
    } else if (++lower.row >= lower_size) {
        lower_need_rows(lower.row + 1);
        if (lower.row >= lower_size) {    // the whole screen already
            scroll_rows(0, SCREEN_ROWS, cell_attr(&lower.colour, 0));
            lower.row = lower_size - 1;
        }
    }
}

// Grow the lower screen to n rows: each new row pushes it up a line.
void lower_need_rows(int n) {
    if (n > SCREEN_ROWS) n = SCREEN_ROWS;
    while (lower_size < n) {
        int top = top_of_lower();
        // If the upper print position is on the row about to be taken,
        // move the upper screen's contents up out of the way.
        if (upper.row >= top - 1 && top > 1) {
            scroll_rows(0, top, upper.colour.attr);
            if (upper.row > 0) upper.row--;
        }
        scroll_rows(top - 1, SCREEN_ROWS, cell_attr(&lower.colour, 0));
        lower_size++;
    }
}

void print_char(char ch) {
    if (scroll_refused) return;
    ECHO(ch);
    if (ch == '\n') {
        newline();
    } else if (ch == '\r') {
        cur->col = 0;
    } else if (ch == 8) {                 // backspace: just move left
        if (cur->col > 0) {
            cur->col--;
        } else if (cur->row > 0) {
            cur->row--;
            cur->col = SCREEN_COLS - 1;
        }
    } else {
        // Wrap only when the next character arrives, so that a line of
        // exactly 32 characters followed by \n doesn't leave a blank line.
        if (cur->col == SCREEN_COLS) newline();
        if (scroll_refused) return;
        put_cell(cur->col, abs_row(cur, cur->row), ch, &cur->colour);
        cur->col++;
    }
}

void print_string(const char *s) {
    while (*s) print_char(*s++);
}

void clear_lower(void) {
    unsigned char a;
    lower.colour = lower_colours();
    a = cell_attr(&lower.colour, 0);
    int top = top_of_lower();
    memset(bitmap[top * 8], 0, (SCREEN_ROWS - top) * 8 * SCREEN_COLS);
    memset(attrs[top], a, (SCREEN_ROWS - top) * SCREEN_COLS);
    draw_rows(top, SCREEN_ROWS);
    lower_size = 2;
    lower.col = lower.row = 0;
}

void clear_screen(void) {
    TRACE("[CLS]\n", 0, 0);
    memset(bitmap, 0, sizeof(bitmap));
    memset(attrs, upper.colour.attr, sizeof(attrs));
    screen_cost_us += 15000;
    lower_size = 2;
    clear_lower();
    draw_rows(0, SCREEN_ROWS);
    upper.col = upper.row = 0;
    scr_ct = 1;
}

void use_lower(int on) { cur = on ? &lower : &upper; }
int using_lower(void)  { return cur == &lower; }

int set_cursor(int c, int r) {
    if (r < 0 || c < 0 || c >= SCREEN_COLS) return 0;
    if (cur == &upper) {
        if (r >= upper_rows()) return 0;
        TRACE("[AT %d,%d]", r, c);
    } else {
        if (r >= SCREEN_ROWS) return 0;
        lower_need_rows(r + 1);
    }
    cur->col = c;
    cur->row = r;
    return 1;
}

void transcript(const char *s) {
    for (; *s; s++) ECHO(*s);         // (ECHO may expand to nothing)
}

void screen_echo(int on) {
#ifdef HOST_TEST
    echo = on;
#else
    (void)on;
#endif
}

int cursor_col(void) { return cur->col; }
int cursor_row(void) { return cur->row; }
int upper_rows(void) { return SCREEN_ROWS - lower_size; }
int lower_rows(void) { return lower_size; }

int scroll_stopped(void) {
    int r = scroll_refused;
    scroll_refused = 0;
    return r;
}

void set_scroll_count(int n) { scr_ct = n; }
int scroll_count(void)       { return scr_ct; }

void show_cursor(int on) {
    if (on && cur->col == SCREEN_COLS) newline();
    cursor_on = on;
    if (cur->col < SCREEN_COLS) draw(cur->col, abs_row(cur, cur->row));
}

/* ------------------------------------------------------------------------
 * The screen's memory, for PEEK and POKE
 * --------------------------------------------------------------------- */

int screen_byte(int y, int col) { return bitmap[y][col]; }
int screen_attr(int r, int c)   { return attrs[r][c]; }

void screen_set_byte(int y, int col, int v) {
    bitmap[y][col] = v;
    draw(col, y / 8);
}

void screen_set_attr(int r, int c, int v) {
    attrs[r][c] = v;
    draw(c, r);
}

struct colours *permanent_colours(void) { return &upper.colour; }

// SCREEN$: the character (32-127) whose shape is in cell (c, r), drawn
// normally or inverted; -1 if none.
int screen_char(int r, int c) {
    for (int ch = 32; ch <= 127; ch++) {
        unsigned char rows[8];
        glyph(ch, rows);
        int same = 1, inverse = 1;
        for (int y = 0; y < 8; y++) {
            unsigned char b = bitmap[r * 8 + y][c];
            if (b != rows[y]) same = 0;
            if (b != (unsigned char)~rows[y]) inverse = 0;
        }
        if (same || inverse) return ch;
    }
    return -1;
}

/* ------------------------------------------------------------------------
 * Colours
 * --------------------------------------------------------------------- */

// Set INK, PAPER etc. in c to n, as the Spectrum allows: INK and PAPER
// 0-9 (8: leave as it is, 9: contrast), FLASH and BRIGHT 0, 1 or 8,
// INVERSE and OVER 0 or 1. Returns 0 if n isn't allowed.
int colour_set(struct colours *c, int which, int n) {
    static const unsigned char bits[] = { 0x07, 0x38, 0x80, 0x40 };
    static const unsigned char shift[] = { 0, 3, 7, 6 };
    if (which == C_INVERSE || which == C_OVER) {
        if (n > 1) return 0;
        if (which == C_INVERSE) c->inverse = n;
        else c->over = n;
        return 1;
    }
    int max = which <= C_PAPER ? 9 : 1;
    if (n < 0 || (n > max && n != 8)) return 0;
    unsigned char m = bits[which];
    if (which == C_INK) c->ink9 = n == 9;
    if (which == C_PAPER) c->paper9 = n == 9;
    if (n == 8) {
        c->mask |= m;
    } else {
        c->mask &= ~m;
        if (n != 9) c->attr = (c->attr & ~m) | (n << shift[which]);
    }
    return 1;
}

void get_colours(struct colours *c)       { *c = cur->colour; }
void set_colours(const struct colours *c) { cur->colour = *c; }

void set_border(int c) {
    TRACE("[BORDER %d]", c, 0);
    border = c;
    plat_border(c);
}

int border_colour(void) { return border; }

/* ------------------------------------------------------------------------
 * Graphics
 * --------------------------------------------------------------------- */

// Set a pixel (in ink; INVERSE 1: in paper; OVER 1: flip it) and give
// its cell the upper screen's colours. Drawn on screen by draw_dirty().
static void set_pixel(int x, int y) {
    if (x < 0 || x >= SCREEN_WIDTH || y < 0 || y >= PLOT_HEIGHT) return;
    int py = PLOT_HEIGHT - 1 - y;
    unsigned char *b = &bitmap[py][x >> 3];
    unsigned char m = 0x80 >> (x & 7);
    if (upper.colour.over) *b ^= m;
    else if (upper.colour.inverse) *b &= ~m;
    else *b |= m;
    unsigned char *a = &attrs[py >> 3][x >> 3];
    *a = cell_attr(&upper.colour, *a);
    dirty[py >> 3][x >> 3] = 1;
    screen_cost_us += 100;
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
HIGH_MEM static int keyq[KEYQ_SIZE];
static int keyq_head, keyq_len;

// Esc and Ctrl+C (BREAK) go into the queue as 27, in order, so that keys
// typed before them aren't lost.
static void collect_keys(void) {
    int k;
    flash_tick();
    while ((k = plat_read_key()) >= 0) {
        if (k == 3) k = 27;
        if (keyq_len < KEYQ_SIZE) keyq[(keyq_head + keyq_len++) % KEYQ_SIZE] = k;
    }
}

static int last_k;
static unsigned long last_k_time;

int poll_key(void) {
    collect_keys();
    if (!keyq_len) return -1;
    int k = keyq[keyq_head];
    keyq_head = (keyq_head + 1) % KEYQ_SIZE;
    keyq_len--;
    if (k < 0x100) {
        last_k = k == '\n' ? 13 : k;
        last_k_time = timer_ms();
    }
    return k;
}

int last_key(void) { return last_k; }

// For IN: a PC keyboard only says when a key goes down (and repeats), not
// whether it's still held, so a key counts as held for a moment after.
int held_key(void) {
    collect_keys();
    while (keyq_len && keyq[keyq_head] != 27) poll_key();   // (leave BREAK be)
    return last_k && timer_ms() - last_k_time < 150 ? last_k : -1;
}

void unget_key(int k) {
    if (keyq_len == KEYQ_SIZE) return;
    keyq_head = (keyq_head + KEYQ_SIZE - 1) % KEYQ_SIZE;
    keyq[keyq_head] = k;
    keyq_len++;
}

int key_waiting(void) {
    collect_keys();
    return keyq_len > 0;
}

// BREAK: if Esc has been pressed, drop it and the keys before it.
int check_break(void) {
    collect_keys();
    for (int i = 0; i < keyq_len; i++) {
        if (keyq[(keyq_head + i) % KEYQ_SIZE] == 27) {
            keyq_head = (keyq_head + i + 1) % KEYQ_SIZE;
            keyq_len -= i + 1;
            return 1;
        }
    }
    return 0;
}

int wait_key(void) {
    int k;
    while ((k = poll_key()) < 0) plat_idle();
    return k;
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

// The picture: 256x192, with a border of at least 32 pixels each side
// (the BIOS kernel has less above and below: VGA mode 13h is 200 high).
// Colours 0-7 are the Spectrum's: black, blue, red, magenta, green,
// cyan, yellow, white - bit 0 blue, bit 1 red, bit 2 green; 8-15 are
// their BRIGHT versions.
static void video_init(void) {
    struct video v = { SCREEN_WIDTH, SCREEN_HEIGHT, 1, 320, 240, { { 0 } } };
    for (int i = 0; i < 16; i++) {
        int level = (i & 8) ? 0xFF : 0xD0;
        v.palette[i][0] = (i & 2) ? level : 0;
        v.palette[i][1] = (i & 4) ? level : 0;
        v.palette[i][2] = (i & 1) ? level : 0;
    }
    plat_video(&v);
}

void screen_init(void) {
    video_init();
    struct colours spectrum = { 0x38, 0, 0, 0, 0, 0 };   // black ink on white paper
    upper.colour = spectrum;
    cur = &upper;
    set_border(7);
    clear_screen();
}
