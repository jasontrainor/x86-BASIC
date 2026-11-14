/*
 * The screen editor, as the firmware's behaves: written for this project
 * from how it behaves; none of it comes from the original's code.
 *
 * The screen is 25 rows of 40 characters, in screen memory ($0400) and
 * colour RAM ($D800). A logical line - what Return reads - is one row
 * or two (80 characters): printing past the end of a row joins the next
 * row on, opening a blank one there. Each row's entry in the line link
 * table (217-241) says whether it starts a logical line (bit 7).
 *
 * Its state is where the firmware keeps it, for programs to PEEK and
 * POKE: the cursor's logical line (PNT) and its column in it (PNTR, up
 * to 79), its row (TBLX), the colour (646), reverse (199), quote mode
 * (212: an odd number of quotes printed, so cursor and colour codes show
 * as reversed characters instead of acting) and the insert count (216).
 * As there, printing uses PNT and PNTR and Return moves on from TBLX:
 * POKE 214,R:PRINT starts the next line at row R+1, and POKE 211,C moves
 * the column.
 *
 * Printed characters are PETSCII; on the screen they're screen codes
 * (c64_screen_code). Controls:
 *    5 white       17 down       29 right      144 black    145 up
 *   13 Return      18 reverse    30 green      146 reverse off
 *   14 lower case  19 home       31 blue       147 clear     148 insert
 *  141 Shift+Return 20 delete    28 red        157 left
 *  142 upper case   8/9 Shift+C= off/on, and the colours 129, 149-156, 158, 159
 */
#include "c64.h"
#include "platform.h"
#include "string.h"

#ifdef HOST_TEST
void host_echo(char c);                   // (the tests read what's printed)
static void echo(int c);
#define ECHO(c) echo(c)
#else
#define ECHO(c) ((void)0)
#endif

static int rd(int a) { return c64_ram(a); }
static void wr(int a, int v) { c64_set_ram(a, v); }
static int rd16(int a) { return rd(a) | rd(a + 1) << 8; }
static void wr16(int a, int v) {
    wr(a, v & 0xFF);
    wr(a + 1, v >> 8 & 0xFF);
}

static unsigned int screen_base(void) { return rd(C64_HIBASE) << 8; }
static int starts(int row) { return rd(C64_LDTB1 + row) & 0x80; }

static void set_link(int row, int start) {
    wr(C64_LDTB1 + row, ((screen_base() + row * 40) >> 8) | (start ? 0x80 : 0));
}

// The first row of the logical line that row is in.
static int line_start(int row) {
    while (row > 0 && !starts(row)) row--;
    return row;
}

// The cursor at row TBLX, column PNTR (0-39) of that row: PNT, LNMX
// and USER set for its logical line, and PNTR made a column of the line
// (as the firmware's routine at $E56C does).
static void stupt(void) {
    int row = rd(C64_TBLX);
    if (row > 24) row = 24;
    int col = rd(C64_PNTR), start = line_start(row);
    col += (row - start) * 40;
    if (col > 79) col = 79;
    wr(C64_PNTR, col);
    wr(C64_LNMX, start < 24 && !starts(start + 1) ? 79 : 39);
    wr16(C64_PNT, screen_base() + start * 40);
    wr16(C64_USER, C64_COLOUR_RAM + start * 40);
}

// The cursor's offset on the screen (0-999).
static int cursor_offset(void) {
    int off = rd16(C64_PNT) - screen_base() + rd(C64_PNTR);
    return off < 0 ? 0 : off > 999 ? 999 : off;
}

void c64_set_cursor(int row, int col) {
    wr(C64_TBLX, row);
    wr(C64_PNTR, col);
    stupt();
}

int c64_column(void) { return rd(C64_PNTR); }

static void clear_row(int row) {
    unsigned int s = screen_base() + row * 40;
    int colour = rd(C64_COLOR);
    for (int i = 0; i < 40; i++) {
        wr(s + i, 32);
        c64_set_colour(row * 40 + i, colour);
    }
}

static void copy_row(int to, int from) {
    unsigned int b = screen_base();
    for (int i = 0; i < 40; i++) {
        wr(b + to * 40 + i, rd(b + from * 40 + i));
        c64_set_colour(to * 40 + i, c64_colour(from * 40 + i));
    }
    set_link(to, starts(from));
}

static void clear_screen(void) {
    for (int row = 0; row < 25; row++) {
        clear_row(row);
        set_link(row, 1);
    }
    c64_set_cursor(0, 0);
}

// Everything up a row (and again, while the top row is the rest of a
// logical line).
static void scroll_up(void) {
    do {
        for (int row = 0; row < 24; row++) copy_row(row, row + 1);
        clear_row(24);
        set_link(24, 1);
        if (rd(C64_TBLX) > 0) wr(C64_TBLX, rd(C64_TBLX) - 1);
        if (rd(C64_LXSP) > 0) wr(C64_LXSP, rd(C64_LXSP) - 1);
    } while (!starts(0));
}

// A blank row opened at row: the rows from there move down (the bottom
// one is lost).
static void open_row(int row) {
    for (int r = 24; r > row; r--) copy_row(r, r - 1);
    clear_row(row);
}

// The cursor's logical line, one row long, made two: a blank row opened
// below it and joined on.
static void extend_line(void) {
    int start = line_start(rd(C64_TBLX));
    if (start == 24) {
        scroll_up();
        start = 23;
    }
    open_row(start + 1);
    set_link(start + 1, 0);
    wr(C64_TBLX, start);
    stupt();
}

// To the start of the next logical line (scrolling at the bottom).
static void next_line(void) {
    int start = line_start(rd(C64_TBLX));
    wr(C64_TBLX, start + (rd(C64_LNMX) == 79 ? 1 : 0));   // (the line's last row)
    while (rd(C64_TBLX) >= 24) scroll_up();
    c64_set_cursor(rd(C64_TBLX) + 1, 0);
}

static void carriage_return(void) {
    wr(C64_RVS, 0);
    wr(C64_QTSW, 0);
    wr(C64_INSRT, 0);
    next_line();
}

// Move the cursor on a column (to the next row, joining it to the line
// or going on to the next line, as at the end of a row).
static void advance(void) {
    int col = rd(C64_PNTR) + 1;
    int lnmx = rd(C64_LNMX);
    if (col > lnmx) {
        if (lnmx == 39) {
            extend_line();                // (the line now ends at 79)
            wr(C64_PNTR, 40);
            wr(C64_TBLX, line_start(rd(C64_TBLX)) + 1);
        } else {
            next_line();
        }
        return;
    }
    wr(C64_PNTR, col);
    if (col == 40) wr(C64_TBLX, line_start(rd(C64_TBLX)) + 1);
}

static void put_code(int code) {
    int off = cursor_offset();
    wr(screen_base() + off, code);
    c64_set_colour(off, rd(C64_COLOR));
    advance();
}

// The cursor moved to physical row and column (0-39).
static void move_to(int row, int col) {
    if (row > 24) {
        scroll_up();
        row = 24;
    }
    if (row < 0) row = 0;
    c64_set_cursor(row, col);
}

static void cursor_phys(int *row, int *col) {
    int off = cursor_offset();
    *row = off / 40;
    *col = off % 40;
}

static void delete_char(void) {
    int col = rd(C64_PNTR);
    if (col == 0) {                       // at the start: just back a row
        int row, c;
        cursor_phys(&row, &c);
        if (row > 0) move_to(row - 1, 39);
        return;
    }
    unsigned int line = rd16(C64_PNT);
    int last = rd(C64_LNMX), coff = line - screen_base();
    for (int i = col; i <= last; i++) {
        wr(line + i - 1, rd(line + i));
        c64_set_colour(coff + i - 1, c64_colour(coff + i));
    }
    wr(line + last, 32);
    c64_set_colour(coff + last, rd(C64_COLOR));
    wr(C64_PNTR, col - 1);
    if (col - 1 == 39) wr(C64_TBLX, rd(C64_TBLX) - 1);
}

static void insert_char(void) {
    unsigned int line = rd16(C64_PNT);
    int last = rd(C64_LNMX);
    if (rd(line + last) != 32) {          // full: two rows if it's one, else no room
        if (last == 79) return;
        int col = rd(C64_PNTR), row = rd(C64_TBLX);
        extend_line();
        wr(C64_PNTR, col);
        wr(C64_TBLX, row);
        stupt();
        line = rd16(C64_PNT);
        last = 79;
    }
    int col = rd(C64_PNTR), coff = line - screen_base();
    for (int i = last; i > col; i--) {
        wr(line + i, rd(line + i - 1));
        c64_set_colour(coff + i, c64_colour(coff + i - 1));
    }
    wr(line + col, 32);
    c64_set_colour(coff + col, rd(C64_COLOR));
    if (rd(C64_INSRT) < 255) wr(C64_INSRT, rd(C64_INSRT) + 1);
}

int c64_screen_code(int c) {
    c &= 0xFF;
    if (c < 32) return c + 128;           // (a control: shown reversed)
    if (c < 64) return c;
    if (c < 96) return c - 64;
    if (c < 128) return c - 32;
    if (c < 160) return c - 128 + 64 + 128;   // (a control: reversed graphics)
    if (c < 192) return c - 64;
    if (c == 255) return 94;
    return c - 128;
}

int c64_petscii(int sc) {
    sc &= 0x7F;
    if (sc < 32) return sc + 64;
    if (sc < 64) return sc;
    if (sc < 96) return sc + 128;
    return sc + 64;
}

static const unsigned char colour_codes[16] = {
    144, 5, 28, 159, 156, 30, 31, 158, 129, 149, 150, 151, 152, 153, 154, 155,
};

// A control character: what it does (0 if it isn't one that acts).
static int control(int c) {
    int row, col;
    for (int i = 0; i < 16; i++)
        if (colour_codes[i] == c) {
            wr(C64_COLOR, i);
            return 1;
        }
    switch (c) {
    case 13: case 141: carriage_return(); return 1;
    case 14: c64_poke(0xD018, c64_peek(0xD018) | 2); return 1;
    case 142: c64_poke(0xD018, c64_peek(0xD018) & ~2); return 1;
    case 8: wr(C64_MODE, 128); return 1;
    case 9: wr(C64_MODE, 0); return 1;
    case 18: wr(C64_RVS, 1); return 1;
    case 146: wr(C64_RVS, 0); return 1;
    case 19: move_to(0, 0); return 1;
    case 147: clear_screen(); return 1;
    case 20: delete_char(); return 1;
    case 148: insert_char(); return 1;
    case 17: cursor_phys(&row, &col); move_to(row + 1, col); return 1;
    case 145: cursor_phys(&row, &col); move_to(row - 1, col); return 1;
    case 29:
        cursor_phys(&row, &col);
        if (col == 39) move_to(row + 1, 0);
        else move_to(row, col + 1);
        return 1;
    case 157:
        cursor_phys(&row, &col);
        if (col > 0) move_to(row, col - 1);
        else if (row > 0) move_to(row - 1, 39);
        return 1;
    }
    return 0;
}

void c64_chrout(int c) {
    c &= 0xFF;
    ECHO(c);
    int is_control = c < 32 || (c >= 128 && c < 160);
    if (c == 13 || c == 141) {
        carriage_return();
    } else if (is_control) {
        int insrt = rd(C64_INSRT);
        if ((rd(C64_QTSW) && c != 20) || insrt) {   // shown, reversed
            put_code(c64_screen_code(c) | 0x80);
            if (insrt) wr(C64_INSRT, insrt - 1);
        } else {
            control(c);
        }
    } else {
        int insrt = rd(C64_INSRT);
        if (c == '"' && !insrt) wr(C64_QTSW, !rd(C64_QTSW));
        put_code(c64_screen_code(c) | (rd(C64_RVS) ? 0x80 : 0));
        if (insrt) wr(C64_INSRT, insrt - 1);
    }
    vic_update(0);
}

void c64_print(const char *s) {
    while (*s) c64_chrout(*s++);
}

/* ------------------------------------------------------------------------
 * Reading a line
 * --------------------------------------------------------------------- */

int c64_input_line(char *buf, int cap) {
    int start_row = line_start(rd(C64_TBLX));
    wr(C64_LXSP, start_row);
    wr(C64_LXSP + 1, rd(C64_PNTR));
    wr(C64_QTSW, 0);
    int k;
    for (;;) {
        vic_cursor(cursor_offset());
        k = c64_wait_key();
        vic_cursor(-1);
        if (k == 13) break;
        c64_chrout(k);
    }
    // The logical line the cursor is on: from where input started (if
    // that's this line) to its last character that isn't a space.
    unsigned int line = rd16(C64_PNT);
    int last = rd(C64_LNMX);
    while (last >= 0 && rd(line + last) == 32) last--;
    int from = line_start(rd(C64_TBLX)) == rd(C64_LXSP) ? rd(C64_LXSP + 1) : 0;
    int n = 0, quote = 0;
    for (int i = from; i <= last && n < cap - 1; i++) {
        int sc = rd(line + i), c = c64_petscii(sc);
        if (c == '"') quote = !quote;
        if (quote && (sc & 0x80)) {       // a reversed character in quotes: a control
            int v = sc & 0x7F;
            if (v < 32) c = v;
            else if (v >= 64 && v < 96) c = v + 64;
        }
        buf[n++] = c;
    }
    buf[n] = '\0';
    c64_chrout(13);
    return n;
}

void screen_init(void) {
    wr(C64_COLOR, 14);
    clear_screen();
}

#ifdef HOST_TEST
// What's printed, as text: PETSCII as ASCII (the pound sign \, the up
// arrow ^, the left arrow _, shifted letters in lower case), controls
// and graphics as {NAME} or {number}, as listings write them.
static void echo(int c) {
    static const struct { unsigned char code; const char *name; } names[] = {
        { 5, "WHT" }, { 14, "LOWER" }, { 17, "DOWN" }, { 18, "RVS ON" }, { 19, "HOME" },
        { 20, "DEL" }, { 28, "RED" }, { 29, "RIGHT" }, { 30, "GRN" }, { 31, "BLU" },
        { 129, "ORANGE" }, { 142, "UPPER" }, { 144, "BLK" }, { 145, "UP" },
        { 146, "RVS OFF" }, { 147, "CLR" }, { 148, "INST" }, { 149, "BROWN" },
        { 150, "LT RED" }, { 151, "GREY1" }, { 152, "GREY2" }, { 153, "LT GREEN" },
        { 154, "LT BLUE" }, { 155, "GREY3" }, { 156, "PUR" }, { 157, "LEFT" },
        { 158, "YEL" }, { 159, "CYN" },
    };
    const char *s = 0;
    char num[8];
    if (c == 13 || c == 141) {
        host_echo('\n');
        return;
    }
    if (c >= 32 && c < 92) {
        host_echo(c);
        return;
    }
    if (c == 92) c = '\\';
    else if (c == 94) c = '^';
    else if (c == 95) c = '_';
    else if (c >= 193 && c <= 218) c = c - 193 + 'a';
    else {
        for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
            if (names[i].code == c) s = names[i].name;
        if (!s) {
            itoa(c, num);
            s = num;
        }
        host_echo('{');
        while (*s) host_echo(*s++);
        host_echo('}');
        return;
    }
    host_echo(c);
}
#endif

#ifdef HOST_TEST
/*
 * For the tests: when the run ends, the screen as text, in the file
 * C64_SCREEN names. 25 rows of the characters shown (as echo() writes
 * them; graphics as ~, reversed characters in [brackets]), then a blank
 * line and each row's colours (0-9, A-F), then the border, background
 * and character set.
 */
extern void (*host_exit_hook)(void);
void host_write(const char *env, const void *data, int len);
static char dump[25 * 82 * 2 + 26 * 41 + 64];

static int dump_char(int sc, int lower) {
    sc &= 0x7F;
    if (sc == 0) return '@';
    if (sc <= 26) return (lower ? 'a' : 'A') + sc - 1;
    switch (sc) {
    case 27: return '[';
    case 28: return '\\';
    case 29: return ']';
    case 30: return '^';
    case 31: return '_';
    }
    if (sc < 64) return sc;
    if (lower && sc >= 65 && sc <= 90) return sc;
    return '~';
}

static void dump_screen(void) {
    char *p = dump;
    int lower = c64_peek(0xD018) & 2;
    unsigned int base = screen_base();
    for (int row = 0; row < 25; row++) {
        int rev = 0;
        for (int col = 0; col < 40; col++) {
            int sc = rd(base + row * 40 + col);
            if ((sc & 0x80) && !rev) *p++ = '[';
            if (!(sc & 0x80) && rev) *p++ = ']';
            rev = sc & 0x80;
            *p++ = dump_char(sc, lower);
        }
        if (rev) *p++ = ']';
        *p++ = '\n';
    }
    *p++ = '\n';
    for (int row = 0; row < 25; row++) {
        for (int col = 0; col < 40; col++) *p++ = "0123456789ABCDEF"[c64_colour(row * 40 + col)];
        *p++ = '\n';
    }
    const char *s = "border ";
    while (*s) *p++ = *s++;
    *p++ = "0123456789ABCDEF"[c64_peek(0xD020) & 15];
    s = " background ";
    while (*s) *p++ = *s++;
    *p++ = "0123456789ABCDEF"[c64_peek(0xD021) & 15];
    s = lower ? " lower\n" : " upper\n";
    while (*s) *p++ = *s++;
    host_write("C64_SCREEN", dump, p - dump);
}

void screen_host_hook(void) { host_exit_hook = dump_screen; }
#endif
