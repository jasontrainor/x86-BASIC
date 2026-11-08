/*
 * The screen and keyboard as the Apple II's firmware handles them: text
 * output into the text page (within the text window, scrolling it when
 * the cursor goes off the bottom), the keyboard, and reading a line. All
 * written for this project from how the machines behave.
 *
 * The window and cursor live in zero page (apple.h), so programs that
 * POKE them (POKE 32,10 to move the window's left edge, say) get the
 * same effect as on the real machines; CH counts from the window's left
 * edge, CV from the top of the screen.
 *
 * Keyboard: the Apple has a one-key latch, read at $C000 (bit 7 set
 * while a key is waiting) and cleared at $C010. Keys typed faster than
 * that is read wait in a queue behind it, rather than being lost.
 *
 * The 1976 terminal (APPLE_I) is simpler: characters go on at the
 * bottom, the screen scrolls up, and nothing else - no window, no
 * cursor moves, no inverse, no lower case. Its cursor is a flashing @,
 * and it shows 60 characters a second. Its screen is kept in the
 * auxiliary 64KB, which that model's programs can't reach.
 */
#include "apple.h"
#include "platform.h"
#include "string.h"

#define BLANK 0xA0                        // a space, in normal text

static int col80;                         // the IIe's 80-column firmware is on
int apple_slow = 1;

#ifdef HOST_TEST
void host_echo(char c);                   // (the tests read what's printed)
#define ECHO(c) host_echo(c)
#else
#define ECHO(c) ((void)0)
#endif

int video_flash_phase(void);

static int zp(int a) { return apple_ram(0, a); }
static void set_zp(int a, int v) { apple_set_ram(0, a, v); }

static int width(void) { return col80 ? 80 : 40; }

// Window: left, width, top, bottom, kept sensible (as the firmware does
// not, but nothing here may write outside the text page).
static void window(int *l, int *w, int *t, int *b) {
    *l = zp(ZP_WNDLFT);
    *w = zp(ZP_WNDWDTH);
    *t = zp(ZP_WNDTOP);
    *b = zp(ZP_WNDBTM);
    if (*l >= width()) *l = 0;
    if (*w < 1 || *l + *w > width()) *w = width() - *l;
    if (*b > 24 || *b < 1) *b = 24;
    if (*t >= *b) *t = 0;
}

// The text page byte for (col, row) on page 1.
static void cell_addr(int col, int row, int *aux, unsigned int *addr) {
    unsigned int base = 0x400 + (row & 7) * 128 + (row >> 3) * 40;
    if (col80) {
        *aux = !(col & 1);
        *addr = base + col / 2;
    } else {
        *aux = apple_model == APPLE_I;
        *addr = base + col;
    }
}

static int get_cell(int col, int row) {
    int aux;
    unsigned int a;
    cell_addr(col, row, &aux, &a);
    return apple_ram(aux, a);
}

static void put_cell(int col, int row, int code) {
    int aux;
    unsigned int a;
    cell_addr(col, row, &aux, &a);
    apple_set_ram(aux, a, code);
}

int apple_cursor_col(void) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    int ch = zp(ZP_CH);
    return l + (ch < w ? ch : w - 1);
}

int apple_cursor_row(void) {
    int cv = zp(ZP_CV);
    return cv < 24 ? cv : 23;
}

/* ------------------------------------------------------------------------
 * Output
 * --------------------------------------------------------------------- */

static void clear_line(int row, int from_col) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    for (int c = from_col; c < l + w; c++) put_cell(c, row, BLANK);
}

void apple_clreol(void) {
    clear_line(apple_cursor_row(), apple_cursor_col());
}

void apple_clreop(void) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    apple_clreol();
    for (int r = apple_cursor_row() + 1; r < b; r++) clear_line(r, l);
}

void apple_home(void) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    set_zp(ZP_CH, 0);
    set_zp(ZP_CV, t);
    apple_clreop();
}

static void scroll(int up) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    if (up) {
        for (int r = t; r < b - 1; r++)
            for (int c = l; c < l + w; c++) put_cell(c, r, get_cell(c, r + 1));
        clear_line(b - 1, l);
    } else {
        for (int r = b - 1; r > t; r--)
            for (int c = l; c < l + w; c++) put_cell(c, r, get_cell(c, r - 1));
        clear_line(t, l);
    }
}

static void line_feed(void) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    int cv = zp(ZP_CV) + 1;
    if (cv >= b) {
        scroll(1);
        cv = b - 1;
    }
    set_zp(ZP_CV, cv);
}

void apple_vtab(int row) {
    set_zp(ZP_CV, row < 0 ? 0 : row > 23 ? 23 : row);
}

void apple_htab(int col) {
    set_zp(ZP_CH, col < 0 ? 0 : col);
}

void apple_bell(void) {
    plat_tone(1000, 100);
}

// The screen code for character c in the current mode (INVFLG).
static int screen_code(int c) {
    int inv = zp(ZP_INVFLG);
    if (apple_model != APPLE_IIE && c >= 0x60) c &= 0x5F;   // (no lower case)
    if (apple_model == APPLE_IIE && apple_sw.altchar && inv != 0xFF && c >= 0x60)
        return c;                         // inverse lower case
    return (c | 0x80) & inv;
}

// The 1976 terminal: 60 characters a second (due counts thirds of a ms).
static unsigned long due;
static void collect(void);
static void pace(void) {
    if (!apple_slow) return;
    unsigned long now = plat_timer_ms() * 3;
    if (due < now || due > now + 100) due = now;
    while (plat_timer_ms() * 3 < due) collect();
    due += 50;
}

static void terminal_out(int c) {
    if (c == 13) {
        pace();
        ECHO('\n');
        set_zp(ZP_CH, 0);
        line_feed();
    } else if (c >= 32 && c < 127) {
        pace();
        if (c >= 0x60) c -= 0x20;         // (upper case only)
        ECHO(c);
        put_cell(apple_cursor_col(), apple_cursor_row(), c | 0x80);
        int ch = zp(ZP_CH) + 1;
        if (ch >= 40) {
            set_zp(ZP_CH, 0);
            line_feed();
        } else {
            set_zp(ZP_CH, ch);
        }
    }                                     // (it ignores other controls)
}

void apple_cout(int c) {
    int l, w, t, b;
    c &= 0x7F;
    if (apple_model == APPLE_I) {
        terminal_out(c);
        return;
    }
    if (c == 13) ECHO('\n');
    else if (c >= 32 && c < 127) ECHO(apple_model == APPLE_IIE || c < 0x60 ? c : c - 0x20);
    else if (c == 8) ECHO(8);
    if (c >= 32 && c < 127) {
        window(&l, &w, &t, &b);
        put_cell(apple_cursor_col(), apple_cursor_row(), screen_code(c));
        int ch = zp(ZP_CH) + 1;
        if (ch >= w) {
            set_zp(ZP_CH, 0);
            line_feed();
        } else {
            set_zp(ZP_CH, ch);
        }
        return;
    }
    switch (c) {
    case 13:                              // Return: the start of the next line
        set_zp(ZP_CH, 0);
        line_feed();
        return;
    case 10:
        line_feed();
        return;
    case 8:                               // backspace: left, or the end of the line above
        window(&l, &w, &t, &b);
        if (zp(ZP_CH) > 0) {
            set_zp(ZP_CH, zp(ZP_CH) - 1);
        } else if (zp(ZP_CV) > t) {
            set_zp(ZP_CH, w - 1);
            set_zp(ZP_CV, zp(ZP_CV) - 1);
        }
        return;
    case 7:
        apple_bell();
        return;
    }
    if (!col80) return;                   // (the 80-column firmware's controls:)
    window(&l, &w, &t, &b);
    switch (c) {
    case 11: apple_clreop(); break;                       // Ctrl+K
    case 12: apple_home(); break;                         // Ctrl+L
    case 14: set_zp(ZP_INVFLG, 0xFF); break;              // Ctrl+N: normal
    case 15: set_zp(ZP_INVFLG, 0x3F); break;              // Ctrl+O: inverse
    case 17: apple_80col(0); break;                       // Ctrl+Q: 40 columns
    case 21: apple_80col(0); break;                       // Ctrl+U: off
    case 22: scroll(0); break;                            // Ctrl+V: scroll down
    case 23: scroll(1); break;                            // Ctrl+W: scroll up
    case 25:                                              // Ctrl+Y: home
        set_zp(ZP_CH, 0);
        set_zp(ZP_CV, t);
        break;
    case 28:                                              // Ctrl+\: right
        if (zp(ZP_CH) < w - 1) set_zp(ZP_CH, zp(ZP_CH) + 1);
        break;
    case 29: apple_clreol(); break;                       // Ctrl+]
    case 31:                                              // Ctrl+_: up
        if (zp(ZP_CV) > t) set_zp(ZP_CV, zp(ZP_CV) - 1);
        break;
    }
}

void apple_print(const char *s) {
    while (*s) apple_cout(*s++);
}

void apple_text(void) {
    if (apple_model == APPLE_I) return;
    apple_poke(0xC051, 0);                // text
    apple_poke(0xC054, 0);                // page 1
    set_zp(ZP_WNDLFT, 0);
    set_zp(ZP_WNDWDTH, width());
    set_zp(ZP_WNDTOP, 0);
    set_zp(ZP_WNDBTM, 24);
    set_zp(ZP_CV, 23);                    // (the firmware's TEXT: VTAB 24)
}

void apple_setinv(int on) {
    set_zp(ZP_INVFLG, on ? 0x3F : 0xFF);
}

void apple_80col(int on) {
    if (apple_model != APPLE_IIE) return;
    col80 = on;
    apple_poke(on ? 0xC00D : 0xC00C, 0);  // 80COL
    apple_poke(on ? 0xC001 : 0xC000, 0);  // 80STORE
    apple_poke(on ? 0xC00F : 0xC00E, 0);  // ALTCHARSET (MouseText, inverse lower case)
    set_zp(ZP_WNDLFT, 0);
    set_zp(ZP_WNDWDTH, on ? 80 : 40);
    apple_home();
}

void text_init(void) {
    col80 = 0;
    due = 0;
    set_zp(ZP_WNDLFT, 0);
    set_zp(ZP_WNDWDTH, 40);
    set_zp(ZP_WNDTOP, 0);
    set_zp(ZP_WNDBTM, 24);
    set_zp(ZP_INVFLG, 0xFF);
    set_zp(ZP_PROMPT, ']');
    apple_home();
}

/* ------------------------------------------------------------------------
 * Keyboard
 * --------------------------------------------------------------------- */

#define QUEUE 64
static unsigned char queue[QUEUE];
static int q_head, q_len;
static int latch;                         // $C000: the key, bit 7 while waiting

static int paddle[4] = { 127, 127, 127, 127 };

int apple_paddle(int n) {
    return paddle[n & 3];
}

int apple_button(int n) {
    return n >= 0 && n < 3 && (plat_buttons() >> n) & 1;
}

static void move_paddle(int n, int by) {
    int v = paddle[n] + by;
    paddle[n] = v < 0 ? 0 : v > 255 ? 255 : v;
}

// A PC key as the Apple keyboard would type it, or -1.
static int translate(int k) {
    int iie = apple_model == APPLE_IIE;
    switch (k) {                          // (the arrows also move the paddles)
    case KEY_LEFT:  move_paddle(0, -16); break;
    case KEY_RIGHT: move_paddle(0, 16); break;
    case KEY_UP:    move_paddle(1, -16); break;
    case KEY_DOWN:  move_paddle(1, 16); break;
    }
    if (apple_model == APPLE_I) {
        if (k == 9) {                     // Tab: 60 characters a second, or fast
            apple_slow = !apple_slow;
            return -1;
        }
        if (k == 8 || k == KEY_LEFT) return '_';   // its rub-out
        if (k >= 0x100) return -1;
        if (k == '\n') return 13;
        if (k >= 'a' && k <= 'z') return k - 32;
        return k & 0x7F;
    }
    switch (k) {
    case KEY_LEFT:   return 8;
    case KEY_RIGHT:  return 21;
    case KEY_UP:     return iie ? 11 : -1;
    case KEY_DOWN:   return iie ? 10 : -1;
    case KEY_DELETE: return iie ? 127 : -1;
    }
    if (k >= 0x100) return -1;
    if (k == '\n') return 13;
    if (!iie && k >= 'a' && k <= 'z') return k - 32;   // upper case only
    return k & 0x7F;
}

static void collect(void) {
    int k;
    video_update();
    while ((k = plat_read_key()) >= 0) {
        k = translate(k);
        if (k >= 0 && q_len < QUEUE) queue[(q_head + q_len++) % QUEUE] = k;
    }
    if (!(latch & 0x80) && q_len) {       // the next key into the latch
        latch = queue[q_head] | 0x80;
        q_head = (q_head + 1) % QUEUE;
        q_len--;
    }
}

int text_latch(void) {
    collect();
    return latch;
}

void text_strobe(void) {
    latch &= 0x7F;
}

int text_any_key(void) {
    return (latch & 0x80) || q_len;
}

int apple_key_waiting(void) {
    collect();
    return (latch & 0x80) != 0;
}

int apple_key_peek(void) {
    collect();
    return latch & 0x80 ? latch & 0x7F : -1;
}

int apple_key(void) {
    collect();
    if (!(latch & 0x80)) return -1;
    latch &= 0x7F;
    return latch;
}

int apple_wait_key(void) {
    int k, shown = -1;
    while ((k = apple_key()) < 0) {
        int on = video_flash_phase();
        if (on != shown) {
            video_cursor(apple_cursor_col(), apple_cursor_row(), on);
            shown = on;
            video_update();
        }
        plat_idle();
    }
    video_cursor(apple_cursor_col(), apple_cursor_row(), 0);
    video_update();
    return k;
}

/* ------------------------------------------------------------------------
 * Reading a line
 * --------------------------------------------------------------------- */

// The character shown at the cursor (for the right arrow, which copies
// what's on the screen into the line).
static int char_at_cursor(void) {
    int c = get_cell(apple_cursor_col(), apple_cursor_row());
    if (c < 0x40) c &= 0x3F;
    else if (c < 0x80) c = apple_sw.altchar && c >= 0x60 ? c : c - 0x40;
    else c &= 0x7F;
    if (c < 0x20) c += 0x40;
    return c;
}

static void move_cursor(int dcol, int drow) {
    int l, w, t, b;
    window(&l, &w, &t, &b);
    int ch = zp(ZP_CH) + dcol, cv = zp(ZP_CV) + drow;
    if (ch >= 0 && ch < w) set_zp(ZP_CH, ch);
    if (cv >= t && cv < b) set_zp(ZP_CV, cv);
}

// Esc: move the cursor without typing. I J K M (or the arrows) move and
// stay in this mode; A B C D move once; @ clears the window, E the rest
// of the line, F the rest of the window. Any other key ends it, and is
// then typed as usual. Returns that key.
static int escape(void) {
    for (;;) {
        int k = apple_wait_key();
        int u = k >= 'a' && k <= 'z' ? k - 32 : k;
        switch (u) {
        case 'I': case 11: move_cursor(0, -1); continue;
        case 'J': case 8:  move_cursor(-1, 0); continue;
        case 'K': case 21: move_cursor(1, 0); continue;
        case 'M': case 10: move_cursor(0, 1); continue;
        case 'A': move_cursor(1, 0); return -1;
        case 'B': move_cursor(-1, 0); return -1;
        case 'C': move_cursor(0, 1); return -1;
        case 'D': move_cursor(0, -1); return -1;
        case '@': apple_home(); return -1;
        case 'E': apple_clreol(); return -1;
        case 'F': apple_clreop(); return -1;
        }
        return k;
    }
}

// The 1976 terminal's line input: _ (or Backspace) rubs out the last
// character, showing a _; Esc or Ctrl+X cancels the line.
static int terminal_getln(int prompt, char *buf, int cap) {
restart:
    if (prompt) apple_cout(prompt);
    int n = 0;
    for (;;) {
        int k = apple_wait_key();
        if (k == '_') {
            if (n > 0) n--;
            apple_cout('_');
        } else if (k == 27 || k == 24) {
            apple_cout('\\');
            apple_cout(13);
            goto restart;
        } else if (k == 13) {
            apple_cout(13);
            buf[n] = '\0';
            return n;
        } else if (k >= 32 && k < 127) {
            buf[n++] = k;
            apple_cout(k);
        }
        if (n >= cap - 1) {
            apple_cout('\\');
            apple_cout(13);
            goto restart;
        }
    }
}

int apple_getln(int prompt, char *buf, int cap) {
    if (cap > 256) cap = 256;
    if (apple_model == APPLE_I) return terminal_getln(prompt, buf, cap);
restart:
    if (prompt) apple_cout(prompt);
    int n = 0;
    for (;;) {
        int k = apple_wait_key();
        if (k == 27 && (k = escape()) < 0) continue;
        if (k == 8) {                     // left arrow: back a character
            if (n > 0) {
                n--;
                apple_cout(8);
            }
        } else if (k == 21) {             // right arrow: copy from the screen
            int c = char_at_cursor();
            buf[n++] = c;
            apple_cout(c);
        } else if (k == 24) {             // Ctrl+X: cancel the line
            apple_cout('\\');
            apple_cout(13);
            goto restart;
        } else if (k == 13) {
            apple_clreol();
            apple_cout(13);
            buf[n] = '\0';
            return n;
        } else if (k >= 32 && k < 127) {
            buf[n++] = k;
            apple_cout(k);
            if (n == cap - 8) apple_bell();   // nearly full
        } else if (k == 3) {              // Ctrl+C: kept (FP BASIC's INPUT stops on it)
            buf[n++] = k;
        }
        if (n >= cap - 1) {               // full: as the Apple does, start again
            apple_cout('\\');
            apple_cout(13);
            goto restart;
        }
    }
}
