/*
 * The BASIC interpreter.
 *
 * Program lines are tokenized as they are entered: each keyword is
 * stored as a single byte (0x80 and up), names are upper-cased, and
 * everything else is kept as typed. The program is a packed, sorted list
 * of lines in one buffer:
 *
 *     [line number: 2 bytes] [length: 2 bytes] [text] [0]   (little-endian)
 *
 * where the length counts the text and its terminating 0.
 *
 * Statements are executed straight from the tokenized text: p points at
 * the next character to read.
 *
 * This file has the tokens, the program, errors and the parser, graphics,
 * listing and the command loop; basic/expr.c the values and expressions,
 * basic/statements.c the statements, basic/files.c the disk ones.
 */
#include "basic.h"
#include "screen.h"
#include "string.h"
#include "mem.h"
#include "num.h"
#include "editor.h"
#include "storage.h"
#include "fat.h"
#include "memory.h"
#include "platform.h"

/* ------------------------------------------------------------------------
 * Tokens
 * --------------------------------------------------------------------- */

// The order matters: it defines the byte stored in programs.
enum {
    T_PRINT = 0x80, T_LET, T_GOTO, T_GOSUB, T_RETURN, T_IF, T_THEN, T_FOR,
    T_TO, T_STEP, T_NEXT, T_REM, T_INPUT, T_END, T_STOP, T_CONTINUE, T_RUN,
    T_LIST, T_NEW, T_CLEAR, T_CLS, T_BEEP, T_INK, T_PAPER, T_BORDER,
    T_BRIGHT, T_INVERSE, T_OVER, T_PLOT, T_DRAW, T_CIRCLE, T_PAUSE,
    T_RANDOMIZE, T_DIM, T_READ, T_DATA, T_RESTORE, T_SAVE, T_LOAD, T_CAT,
    T_ERASE, T_EDIT, T_AT, T_TAB, T_LINE, T_AND, T_OR, T_NOT, T_PI, T_ABS,
    T_INT, T_SGN, T_SQR, T_SIN, T_COS, T_TAN, T_ATN, T_ASN, T_ACS, T_LN,
    T_EXP, T_RND, T_LEN, T_VAL, T_CODE, T_CHRS, T_STRS, T_INKEYS, T_POINT,
    T_DEFFN, T_FN, T_FLASH, T_LPRINT, T_LLIST, T_COPY,
    T_PEEK, T_POKE, T_USR, T_BIN, T_VALS, T_ATTR, T_SCREENS, T_IN, T_OUT,
    T_MERGE, T_VERIFY, T_PLAY,
    T_LE, T_GE, T_NE,
    T_END_OF_TOKENS
};

static const char *const token_names[] = {
    "PRINT", "LET", "GO TO", "GO SUB", "RETURN", "IF", "THEN", "FOR",
    "TO", "STEP", "NEXT", "REM", "INPUT", "END", "STOP", "CONTINUE", "RUN",
    "LIST", "NEW", "CLEAR", "CLS", "BEEP", "INK", "PAPER", "BORDER",
    "BRIGHT", "INVERSE", "OVER", "PLOT", "DRAW", "CIRCLE", "PAUSE",
    "RANDOMIZE", "DIM", "READ", "DATA", "RESTORE", "SAVE", "LOAD", "CAT",
    "ERASE", "EDIT", "AT", "TAB", "LINE", "AND", "OR", "NOT", "PI", "ABS",
    "INT", "SGN", "SQR", "SIN", "COS", "TAN", "ATN", "ASN", "ACS", "LN",
    "EXP", "RND", "LEN", "VAL", "CODE", "CHR$", "STR$", "INKEY$", "POINT",
    "DEF FN", "FN", "FLASH", "LPRINT", "LLIST", "COPY",
    "PEEK", "POKE", "USR", "BIN", "VAL$", "ATTR", "SCREEN$", "IN", "OUT",
    "MERGE", "VERIFY", "PLAY",
    "<=", ">=", "<>",
};
_Static_assert(sizeof(token_names) / sizeof(token_names[0]) == T_END_OF_TOKENS - T_PRINT,
               "token_names doesn't match the token enum");

#define IS_TOKEN(c) ((unsigned char)(c) >= T_PRINT && (unsigned char)(c) < T_END_OF_TOKENS)

static const char *token_name(unsigned char t) {
    return token_names[t - T_PRINT];
}

// Look up a keyword (upper case, NUL-terminated); 0 if it isn't one.
// GO TO and GO SUB may also be typed without the space.
static int find_token(const char *word) {
    for (int t = T_PRINT; t < T_LE; t++) {
        if (strcmp(token_name(t), word) == 0) return t;
    }
    if (strcmp(word, "GOTO") == 0) return T_GOTO;
    if (strcmp(word, "GOSUB") == 0) return T_GOSUB;
    if (strcmp(word, "DEFFN") == 0) return T_DEFFN;
    return 0;
}

// The length of the run of letters at s.
static int letters(const char *s) {
    int n = 0;
    while (isalpha(s[n])) n++;
    return n;
}

// Does the run of letters at s spell word (upper case), in any case?
static int is_word(const char *s, const char *word) {
    int n = letters(s);
    if (n != (int)strlen(word)) return 0;
    for (int i = 0; i < n; i++)
        if (toupper(s[i]) != word[i]) return 0;
    return 1;
}

/*
 * Characters that can't be typed on a PC keyboard are written in strings
 * as escapes, as the zmakebas tool for Spectrum listings has them:
 *
 *   \a to \u   the user-defined graphics (CHR$ 144-164)
 *   \ followed by two of space ' . :   a block graphic (CHR$ 128-143): the
 *              left and right halves of the cell, ' the top quarter, .
 *              the bottom one, : both
 *   \*         the copyright sign (CHR$ 127)
 *   \{n}       character n
 *   \\         a backslash
 *
 * (The pound sign is CHR$ 96, the ` key, as on the Spectrum.) If s starts
 * with one, returns its character and moves s past it; otherwise -1.
 */
static int block_half(char c, int top, int bottom) {
    return c == ' ' ? 0 : c == '\'' ? top : c == '.' ? bottom : c == ':' ? top | bottom : -1;
}

static int escape(const char **sp) {
    const char *s = *sp;
    if (s[0] != '\\') return -1;
    char c = s[1];
    if (c >= 'a' && c <= 'u') { *sp += 2; return 144 + c - 'a'; }
    if (c == '\\') { *sp += 2; return '\\'; }
    if (c == '*') { *sp += 2; return 127; }
    if (c == '{' && isdigit(s[2])) {
        int n = 0, i = 2;
        while (isdigit(s[i]) && n < 1000) n = n * 10 + s[i++] - '0';
        if (s[i] != '}' || n > 255 || n == 0) return -1;
        *sp += i + 1;
        return n;
    }
    int left = block_half(c, 2, 8), right = c ? block_half(s[2], 1, 4) : -1;
    if (left >= 0 && right >= 0) { *sp += 3; return 128 + left + right; }
    return -1;
}

/*
 * Tokenize the typed line src into dst (at most cap bytes, including the
 * terminating 0). Returns the length including the 0, or -1 if it
 * doesn't fit.
 *
 * A keyword is a whole run of letters (plus a trailing $ for CHR$ etc.)
 * that matches one, in any case, so names may contain keywords
 * (TOTAL, FORM) and keywords may be followed directly by digits
 * (GOTO10). Spaces around keywords are dropped; LIST puts them back.
 * Escapes (see escape()) are taken in strings and REM text.
 * The two-word keywords GO TO, GO SUB and DEF FN are recognised with or
 * without the space. Names are kept as typed (LIST shows them that way) but
 * compared in any case. The text of strings and of REM comments is kept
 * exactly.
 */
static int tokenize(const char *src, unsigned char *dst, int cap) {
    int n = 0;
    int rest_raw = 0;    // after REM: copy the rest of the line as is
#define PUT(c) do { if (n >= cap - 1) return -1; dst[n++] = (c); } while (0)
    while (*src) {
        char c = *src;
        if (rest_raw) {              // (escapes work in REM text too)
            int e = escape(&src);
            if (e >= 0) PUT(e);
            else PUT(*src++);
            continue;
        }
        if (c == '"') {              // a string: copy up to the closing quote
            PUT(*src++);             //   ("" inside it is a quote)
            while (*src && (*src != '"' || src[1] == '"')) {
                if (*src == '"') {
                    PUT(*src++);
                    PUT(*src++);
                    continue;
                }
                int e = escape(&src);
                if (e >= 0) PUT(e);
                else PUT(*src++);
            }
            if (*src) PUT(*src++);
            continue;
        }
        if (isalpha(c)) {
            char word[12];
            int len = 0;
            while (isalpha(src[len])) {
                if (len < 10) word[len] = toupper(src[len]);
                len++;
            }
            int t = 0;
            if (len <= 10) {
                word[len] = '\0';
                if (src[len] == '$') {      // CHR$, STR$, INKEY$, VAL$, SCREEN$
                    word[len] = '$';
                    word[len + 1] = '\0';
                    t = find_token(word);
                    if (t) len++;
                    word[len] = '\0';
                }
                if (!t) t = find_token(word);
            }
            if (len == 2 && toupper(src[0]) == 'G' && toupper(src[1]) == 'O') {
                const char *q = src + 2;     // GO TO, GO SUB
                while (*q == ' ') q++;
                if (is_word(q, "TO"))  { t = T_GOTO; len = q + 2 - src; }
                if (is_word(q, "SUB")) { t = T_GOSUB; len = q + 3 - src; }
            }
            if (len == 3 && is_word(src, "DEF")) {
                const char *q = src + 3;     // DEF FN
                while (*q == ' ') q++;
                if (is_word(q, "FN")) { t = T_DEFFN; len = q + 2 - src; }
            }
            if (t) {
                while (n > 0 && dst[n - 1] == ' ') n--;
                PUT(t);
                src += len;
                while (*src == ' ') src++;
                if (t == T_REM) rest_raw = 1;
            } else {
                for (int i = 0; i < len; i++) PUT(*src++);
            }
            continue;
        }
        if (c == '<' && src[1] == '=')      { PUT(T_LE); src += 2; }
        else if (c == '>' && src[1] == '=') { PUT(T_GE); src += 2; }
        else if (c == '<' && src[1] == '>') { PUT(T_NE); src += 2; }
        else { PUT(c); src++; }
    }
    dst[n++] = '\0';
    return n;
#undef PUT
}

// The reverse, for LIST: tokens back to words, with spaces around them.
// out must hold at least cap bytes. If kw isn't 0, kw[i] is set to 1
// where out[i] is part of a keyword (for colouring).
static void detokenize(const unsigned char *src, char *out, char *kw, int cap) {
    int n = 0;
    int in_str = 0;
    int is_kw = 0;
#define OUT(c) do { if (n < cap - 1) { if (kw) kw[n] = is_kw; out[n++] = (c); } } while (0)
    int rem = 0;          // after REM, the rest is text (any character)
    for (; *src; src++) {
        unsigned char c = *src;
        if (c == '"' && !rem) in_str = !in_str;
        if (in_str || rem || !IS_TOKEN(c)) { OUT(c); continue; }
        if (c == T_REM) rem = 1;
        // Spaced as the Spectrum lists them: a space after each keyword
        // (but not RND, INKEY$ or PI, which take nothing after them), and
        // one before each, except functions (LET A=INT (RND*6)).
        const char *name = token_name(c);
        int word = isalpha(name[0]);
        int function = (c >= T_PI && c <= T_POINT) || c == T_FN || c == T_AT || c == T_TAB ||
                       c == T_NOT || (c >= T_PEEK && c <= T_IN && c != T_POKE);
        if (word && !function && n > 0 && out[n - 1] != ' ') OUT(' ');
        is_kw = 1;
        while (*name) OUT(*name++);
        is_kw = 0;
        if (word && c != T_RND && c != T_INKEYS && c != T_PI && src[1] != ' ') OUT(' ');
    }
    out[n] = '\0';
#undef OUT
}

/* ------------------------------------------------------------------------
 * The program
 * --------------------------------------------------------------------- */

#define SPECTRUM_NAME_LEN 15             // (storage allows longer, for DOS 3.3)
#define PROG_SIZE 32768
// The longest line: what fits in the lower screen, which can grow to
// the whole screen (with the line number and the cursor).
#define MAX_TEXT 760
#define LINE_HEAD 4                       // number and length

HIGH_MEM unsigned char prog[PROG_SIZE];

// For text files, arrays being saved or loaded, and the printer's file.
#define TEXT_SIZE (64 * 1024)
#define DATA_SIZE TEXT_SIZE
HIGH_MEM unsigned char text_buf[TEXT_SIZE];  // (text files and arrays)
int prog_len = 0;

static int line_num(int off) { return prog[off] | (prog[off + 1] << 8); }
static int line_len(int off) { return prog[off + 2] | (prog[off + 3] << 8); }
static int line_next(int off) { return off + LINE_HEAD + line_len(off); }
static unsigned char *line_text(int off) { return prog + off + LINE_HEAD; }

// The first token t at or after q in a line, outside strings and REM
// comments (which can hold any character); 0 if there isn't one.
static unsigned char *find_token_in(unsigned char *q, int t) {
    int in_str = 0;
    for (; *q; q++) {
        if (*q == '"') in_str = !in_str;
        else if (!in_str && *q == T_REM) return 0;
        else if (!in_str && *q == t) return q;
    }
    return 0;
}

// Offset of the first line numbered >= num (prog_len if none).
static int line_find(int num) {
    int off = 0;
    while (off < prog_len && line_num(off) < num) off = line_next(off);
    return off;
}

static void line_delete(int num) {
    int off = line_find(num);
    if (off < prog_len && line_num(off) == num) {
        int next = line_next(off);
        memmove(prog + off, prog + next, prog_len - next);
        prog_len -= next - off;
    }
}

// Store (or replace) a tokenized line. Returns an error message or 0.
static const char *line_store(int num, const unsigned char *text, int len) {
    int off = line_find(num);
    int old = (off < prog_len && line_num(off) == num) ? line_next(off) - off : 0;
    if (prog_len - old + LINE_HEAD + len > PROG_SIZE) return "G No room for line";
    line_delete(num);
    memmove(prog + off + LINE_HEAD + len, prog + off, prog_len - off);
    prog[off] = num & 0xFF;
    prog[off + 1] = num >> 8;
    prog[off + 2] = len & 0xFF;
    prog[off + 3] = len >> 8;
    memcpy(prog + off + LINE_HEAD, text, len);
    prog_len += LINE_HEAD + len;
    return 0;
}

/* ------------------------------------------------------------------------
 * Errors and the parser's position
 * --------------------------------------------------------------------- */

char *p;                  // next character of the statement being run
int cur_line = -1;        // offset in prog of the running line; -1: typed
int jumped;               // p is already at the start of the next statement
int running;              // still going (cleared by END, STOP, errors)

// First error raised while executing the current statement, or 0.
// Once set, the parser stops consuming input and the statement is abandoned.
const char *error_msg = 0;

// The report shown when the program or command finishes (if there
// wasn't an error), and whether to show one at all.
const char *report_msg;
int no_report;

/*
 * The syntax check. As on the Spectrum, a line is checked when it's
 * entered, and refused (with a ? where it goes wrong) unless it makes
 * sense; only then is it stored or run. The check is the interpreter run
 * with checking set: statements and expressions are parsed as usual, but
 * nothing is done, variables aren't looked up (their names say whether
 * they're numbers or strings), and only syntax errors (C Nonsense in
 * BASIC) count. error_pos is where the first one was found.
 */
int checking;
char *error_pos;

static void error(const char *msg) {
    if (checking && msg[0] != 'C') return;   // (numbers out of range etc.: not syntax)
    if (!error_msg) {
        error_msg = msg;
        error_pos = p;
    }
}

static void skip_space() {
    while (*p == ' ') p++;
}

// If the next item is the token t, consume it and return 1.
static int accept(int t) {
    skip_space();
    if ((unsigned char)*p == t) { p++; return 1; }
    return 0;
}

static int accept_char(char c) {
    skip_space();
    if (*p == c) { p++; return 1; }
    return 0;
}

static void expect_char(char c, const char *msg) {
    if (!accept_char(c)) error(msg);
}

static int at_statement_end() {
    skip_space();
    return *p == '\0' || *p == ':';
}

/*
 * The parts in basic/ (one file, as far as the compiler is concerned, so
 * that what they share can stay static: the compiler then inlines and
 * leaves out what it can, which the BIOS kernel's 64KB of code needs).
 */
#include "basic/expr.c"
#include "basic/statements.c"
#include "basic/files.c"

/*
 * Graphics, in Spectrum coordinates: x 0-255 left to right, y 0-175
 * bottom to top. DRAW is relative to the last point plotted.
 */
int plot_x, plot_y;

static void st_plot() {
    struct colours saved;
    graphics_colours(&saved);
    int x = expr_int(0, SCREEN_WIDTH - 1);
    expect_char(',', "C Nonsense in BASIC");
    int y = expr_int(0, PLOT_HEIGHT - 1);
    if (!error_msg && !checking) {
        plot(x, y);
        plot_x = x;
        plot_y = y;
    }
    set_colours(&saved);
}

// DRAW dx,dy [,angle]: a line, or with an angle (in radians), an arc
// that turns through that angle (anticlockwise if positive).
static void draw_to(double dx, double dy, double a);

static void st_draw() {
    struct colours saved;
    graphics_colours(&saved);
    double dx = expr_num();
    expect_char(',', "C Nonsense in BASIC");
    double dy = expr_num();
    double a = 0;
    if (accept_char(',')) a = expr_num();
    if (!error_msg && !checking) draw_to(dx, dy, a);
    set_colours(&saved);
}

static void draw_to(double dx, double dy, double a) {
    int x1 = to_int(plot_x + dx, 0, SCREEN_WIDTH - 1);
    int y1 = to_int(plot_y + dy, 0, PLOT_HEIGHT - 1);
    if (error_msg) return;
    double t = num_tan(a / 2);
    double sx = plot_x, sy = plot_y, ddx = x1 - sx, ddy = y1 - sy;
    double len = num_sqrt(ddx * ddx + ddy * ddy);
    if (num_abs(t) < 1e-9 || len == 0) {
        draw_line(plot_x, plot_y, x1, y1);
    } else {
        // The arc's centre is on the perpendicular bisector of the chord,
        // to its left for a positive (anticlockwise) angle.
        double h = (len / 2) / t;
        double cx = sx + ddx / 2 - ddy / len * h;
        double cy = sy + ddy / 2 + ddx / len * h;
        double r = num_sqrt((sx - cx) * (sx - cx) + (sy - cy) * (sy - cy));
        double start = num_atan2(sy - cy, sx - cx);
        int n = (int)(num_abs(a) * r / 4) + 4;     // segments of ~4 pixels
        if (n > 1000) n = 1000;
        int px = plot_x, py = plot_y;
        for (int i = 1; i <= n; i++) {
            int qx = x1, qy = y1;
            if (i < n) {
                double th = start + a * i / n;
                qx = (int)num_round(cx + r * num_cos(th));
                qy = (int)num_round(cy + r * num_sin(th));
            }
            draw_line(px, py, qx, qy);     // parts off the screen are skipped
            px = qx;
            py = qy;
        }
    }
    plot_x = x1;
    plot_y = y1;
}

static void st_circle() {
    struct colours saved;
    graphics_colours(&saved);
    int x = expr_int(0, SCREEN_WIDTH - 1);
    expect_char(',', "C Nonsense in BASIC");
    int y = expr_int(0, PLOT_HEIGHT - 1);
    expect_char(',', "C Nonsense in BASIC");
    int r = expr_int(0, 1000);
    if (!error_msg && !checking) circle(x, y, r);       // parts off the screen are skipped
    set_colours(&saved);
}

// A program line as text: "10 PRINT X". kw: see detokenize.
static void line_to_text(int off, char *out, char *kw, int cap) {
    itoa(line_num(off), out);
    int n = strlen(out);
    out[n] = ' ';
    if (kw) memset(kw, 0, n + 1);
    detokenize(line_text(off), out + n + 1, kw ? kw + n + 1 : 0, cap - n - 1);
}

// LIST shows keywords in a colour of their own: blue, or cyan on dark paper.
/*
 * Listing, as on the Spectrum: line numbers right-aligned in four
 * columns, and a > after the number of the current line (the last one
 * entered or edited). Keywords are shown in a colour of their own (blue,
 * or cyan on dark paper).
 */
int e_ppc;                // the current line


static int listed_length(int off, char *buf, char *kw, int cap) {
    char num[8];
    itoa(line_num(off), num);
    int n = 0;
    for (int i = strlen(num); i < 4; i++) buf[n++] = ' ';
    for (int i = 0; num[i]; i++) buf[n++] = num[i];
    buf[n++] = line_num(off) == e_ppc ? '>' : ' ';
    memset(kw, 0, n);
    detokenize(line_text(off), buf + n, kw + n, cap - n);
    return strlen(buf);
}

static void list_line(int off) {
    char *buf = list_buf, *kw = list_kw;
    struct colours normal, keyword;
    listed_length(off, buf, kw, sizeof(list_buf));
    get_colours(&normal);
    keyword = normal;
    int paper = (normal.attr >> 3) & 7, ink = normal.attr & 7;
    int kink = paper <= 3 ? 5 : 1;
    if (kink == paper) kink = ink;
    colour_set(&keyword, C_INK, kink);
    for (int i = 0; buf[i]; i++) {
        set_colours(kw[i] ? &keyword : &normal);
        print_char(buf[i]);
    }
    set_colours(&normal);
}

// Rows a line takes when listed.
static int listed_rows(int off) {
    int n = listed_length(off, list_buf, list_kw, sizeof(list_buf));
    return n ? (n + SCREEN_COLS - 1) / SCREEN_COLS : 1;
}

// LLIST: the listing, on the printer.
static void llist(int from) {
    to_printer = 1;
    for (int off = line_find(from); off < prog_len; off = line_next(off)) {
        listed_length(off, list_buf, list_kw, sizeof(list_buf));
        out_string(list_buf);
        out_char('\n');
    }
    to_printer = 0;
}

// COPY: the upper screen's text, on the printer. (Graphics that aren't
// characters come out as spaces.)
static void copy_screen() {
    to_printer = 1;
    for (int r = 0; r < SCREEN_ROWS - 2; r++) {
        int end = SCREEN_COLS;
        while (end > 0 && screen_char(r, end - 1) <= ' ') end--;
        for (int c = 0; c < end; c++) {
            int ch = screen_char(r, c);
            out_char(ch < 0 ? ' ' : ch);
        }
        out_char('\n');
    }
    to_printer = 0;
}

static void list_program(int from) {
    for (int off = line_find(from); off < prog_len && !check_break(); off = line_next(off)) {
        list_line(off);
        print_string("\n");
        if (scroll_stopped()) {
            error("D BREAK - CONT repeats");
            return;
        }
    }
}

/*
 * The automatic listing shown after a line is entered: a screenful,
 * from the line at the top last time (s_top), moved on if need be so
 * that the current line is on it.
 */
int s_top;

static void auto_list() {
    screen_echo(0);                   // (tests don't want to see it)
    clear_screen();
    if (prog_len == 0) {
        screen_echo(1);
        return;
    }
    int cur = line_find(e_ppc);
    if (cur >= prog_len) {            // after the last line: show the last
        cur = 0;
        while (line_next(cur) < prog_len) cur = line_next(cur);
    }
    int top = line_find(s_top);
    if (top > cur) top = cur;
    for (;;) {
        int rows = 0;
        for (int off = top; off <= cur; off = line_next(off)) rows += listed_rows(off);
        if (rows <= upper_rows() || top == cur) break;
        top = line_next(top);
    }
    s_top = line_num(top);
    int rows = 0;
    for (int off = top; off < prog_len; off = line_next(off)) {
        int n = listed_rows(off);
        if (rows + n > upper_rows()) break;
        if (rows) print_string("\n");
        list_line(off);
        rows += n;
        key_waiting();                // (keep keys typed meanwhile: the
                                      // BIOS only holds 15)
    }
    screen_echo(1);
}
static void clear_all();
static void show_banner();
static void reset_colours();

// NEW, and USR 0: start again as if just switched on.
static void new_program() {
    cont_ok = 0;
    prog_len = 0;
    e_ppc = s_top = 0;
    clear_variables();
    reset_run_state();
    mem_reset();
    reset_colours();
    set_border(7);
    clear_screen();
    plot_x = plot_y = 0;
    show_banner();
    no_report = 1;
    running = 0;
}

static void execute_statement() {
    skip_space();
    unsigned char t = *p;
    if (IS_TOKEN(t)) p++;
    switch (t) {
    case T_PRINT:  st_print(2); break;
    case T_LPRINT: st_print(3); break;
    case T_LLIST: {
        int from = at_statement_end() ? 0 : expr_int(0, 9999);
        if (!error_msg && !checking) llist(from);
        break;
    }
    case T_COPY:
        if (!checking) copy_screen();
        break;
    case T_LET:    st_let(); break;
    case T_GOTO:   go_to_line(expr_int(0, 65535)); break;
    case T_GOSUB:  st_gosub(); break;
    case T_RETURN: st_return(); break;
    case T_INPUT:  st_input(); break;
    case T_DIM:    st_dim(); break;
    case T_READ:   st_read(); break;
    case T_DATA:   st_data(T_DATA); break;
    case T_DEFFN:  st_data(T_DEFFN); break;   // only used by FN
    case T_RESTORE: {
        int n = at_statement_end() ? 0 : expr_int(1, 9999);
        if (!checking) restore(n);
        break;
    }
    case T_RANDOMIZE: {
        int n = at_statement_end() ? 0 : expr_int(0, 65535);
        if (!error_msg && !checking) randomize(n);
        break;
    }
    case T_PAUSE: {
        int n = expr_int(0, 65535);
        if (!error_msg && !checking) pause(n);
        break;
    }
    case T_STOP:
        if (checking) break;
        cont_ok = cur_line >= 0;
        cont_line = current_line_number();
        cont_stmt = statement_number(stmt_pos);
        cont_next = 1;
        report_msg = "9 STOP statement";
        running = 0;
        break;
    case T_CONTINUE: st_continue(); break;
    case T_SAVE:   st_save(); break;
    case T_LOAD:   st_load(T_LOAD); break;
    case T_VERIFY: st_load(T_VERIFY); break;
    case T_MERGE:  st_load(T_MERGE); break;
    case T_ERASE:  st_erase(); break;
    case T_CAT:    st_cat(); break;
    case T_EDIT: {
        // Put the line in the editor for the next command line.
        int num = at_statement_end() ? last_line : expr_int(1, 9999);
        if (error_msg || checking) break;
        int off = line_find(num);
        if (num <= 0 || off >= prog_len || line_num(off) != num) {
            error("N Statement lost");
            break;
        }
        line_to_text(off, list_buf, list_kw, sizeof(list_buf));
        strncpy_safe(edit_next, list_buf, sizeof(edit_next));
        e_ppc = num;
        no_report = 1;
        running = 0;
        break;
    }
    case T_IF:     st_if(); break;
    case T_FOR:    st_for(); break;
    case T_NEXT:   st_next(); break;
    case T_REM:    skip_line(); break;
    case T_CLS:
        if (checking) break;
        clear_screen();
        plot_x = plot_y = 0;
        break;
    case T_INK:     st_colour(C_INK); break;
    case T_PAPER:   st_colour(C_PAPER); break;
    case T_FLASH:   st_colour(C_FLASH); break;
    case T_BRIGHT:  st_colour(C_BRIGHT); break;
    case T_INVERSE: st_colour(C_INVERSE); break;
    case T_OVER:    st_colour(C_OVER); break;
    case T_BORDER: {
        int n = expr_int(0, 255);
        if (error_msg || checking) break;
        if (n > 7) error("K Invalid colour");
        else set_border(n);
        break;
    }
    case T_PLOT:    st_plot(); break;
    case T_DRAW:    st_draw(); break;
    case T_CIRCLE:  st_circle(); break;
    case T_END:    if (!checking) running = 0; break;
    case T_NEW:                            // as if switched on again
        if (!checking) new_program();
        break;
    case T_CLEAR:                          // CLEAR [n]: n (RAMTOP) is ignored
        if (!at_statement_end()) expr_num();
        if (error_msg || checking) break;
        clear_all();
        break;
    case T_LIST: {
        int from = at_statement_end() ? 0 : expr_int(0, 9999);
        if (error_msg || checking) break;
        if (from) e_ppc = from;
        list_program(from);
        break;
    }
    case T_RUN: {                          // RUN [line]
        int num = at_statement_end() ? 0 : expr_int(0, 65535);
        if (error_msg || checking) break;
        clear_all();
        cont_ok = 0;
        go_to_line(num);
        break;
    }
    case T_BEEP:   st_beep(); break;
    case T_PLAY:   st_play(); break;
    case T_POKE: {
        int a = expr_int(0, 65535);
        expect_char(',', "C Nonsense in BASIC");
        int v = expr_int(-255, 255);
        if (!error_msg && !checking) mem_poke(a, v);
        break;
    }
    case T_OUT: {                          // OUT 254,n sets the border
        int port = expr_int(0, 65535);
        expect_char(',', "C Nonsense in BASIC");
        int v = expr_int(0, 255);
        if (!error_msg && !checking && !(port & 1)) set_border(v & 7);
        break;
    }
    default:
        if (isalpha(t)) {
            st_let();                      // LET is optional
        } else if (is_name_token(t)) {
            p--;                           // (to=1: a keyword as a name)
            st_let();
        } else {
            error("C Nonsense in BASIC");
        }
    }
}

/*
 * CLEAR (and RUN): as on the Spectrum, it also clears the screen, RESTOREs
 * and sends PLOT back to the bottom left.
 */
static void clear_all() {
    clear_variables();
    reset_run_state();
    clear_screen();
    plot_x = plot_y = 0;
}

// Print "ERROR: <msg>", plus " IN LINE <n>" when num >= 0.
int report_shown;         // a report is in the lower screen

/*
 * Show a report in the lower screen, as the Spectrum does:
 * "2 Variable not found, 10:1" (line 10, statement 1; line 0 is a typed
 * command). It stays until a key is pressed.
 */
static void print_report(const char *msg, int line, int stmt) {
    char buf[8];
    int was_lower = using_lower();
    use_lower(1);
    clear_lower();
    if (msg[0] != '0' && msg[0] != '9') transcript("[ERR]");   // for tests
    print_string(msg);
    print_string(", ");
    itoa(line, buf);
    print_string(buf);
    print_string(":");
    itoa(stmt, buf);
    print_string(buf);
    transcript("\n");
    use_lower(was_lower);
    report_shown = 1;
}

// The message shown when switched on (and after NEW), where the Spectrum
// has its copyright line: our own name.
static void show_banner() {
    use_lower(1);
    clear_lower();
    print_string("x86 BASIC for PCs");
    transcript("\n");
    use_lower(0);
    report_shown = 1;
}

static void reset_colours() {
    struct colours c = { 0x38, 0, 0, 0, 0, 0 };   // black ink on white paper
    use_lower(0);
    set_colours(&c);
}
/*
 * Move p from the end of a statement to the start of the next one.
 * Returns 0 at the end of the program (or of a typed line).
 */
static int next_statement() {
    skip_space();
    if (*p == ':') {
        p++;
        return 1;
    }
    if (*p == '\0') {
        if (cur_line < 0) return 0;
        cur_line = line_next(cur_line);
        if (cur_line >= prog_len) return 0;
        p = (char *)line_text(cur_line);
        return 1;
    }
    error("C Nonsense in BASIC");
    return 0;
}

/*
 * Check the (tokenized) line text: returns -1 if it makes sense, or the
 * offset in it where it stops making sense.
 */
static int syntax_check(char *text) {
    unsigned long saved_cost = cost_us;
    char *saved_p = p;
    int saved_jumped = jumped, saved_then = then_part, saved_pool = pool_used;
    checking = 1;
    error_msg = 0;
    pool_used = 0;
    fn_depth = 0;
    p = text;
    for (;;) {
        jumped = 0;
        execute_statement();
        if (error_msg) break;
        skip_space();
        if (jumped) continue;             // IF ... THEN: the next statement follows
        if (*p == '\0') break;
        if (*p != ':') {
            error("C Nonsense in BASIC");
            break;
        }
        p++;
    }
    checking = 0;
    then_part = saved_then;
    jumped = saved_jumped;
    pool_used = saved_pool;
    p = saved_p;
    cost_us = saved_cost;
    int bad = error_msg ? (int)(error_pos - text) : -1;
    error_msg = 0;
    return bad;
}

/*
 * Run statements from p until the end of the program (or of a typed
 * line, when cur_line is -1), STOP, an error or BREAK; then show the
 * report. Errors and BREAK leave CONTINUE set to run the statement again.
 */
static void run() {
    int rep_line = 0, rep_stmt = 1;
    int first = 1;            // (an Esc typed ahead stops it after the first statement)
    running = 1;
    pace_start();
    report_msg = "0 OK";
    no_report = 0;
    use_lower(0);
    while (running) {
        if (break_requested || (!first && check_break())) {
            break_requested = 0;
            rep_line = current_line_number();
            rep_stmt = statement_number(p);
            cont_ok = cur_line >= 0;
            cont_line = rep_line;
            cont_stmt = rep_stmt;
            cont_next = 0;
            report_msg = "L BREAK into program";
            break;
        }
        error_msg = 0;
        pool_used = 0;
        fn_depth = 0;
        jumped = 0;
        stmt_line = cur_line;
        stmt_pos = p;
        rep_line = current_line_number();
        rep_stmt = statement_number(p);
        int in_then = then_part;
        then_part = 0;
        execute_statement();
        if (!in_then) cost_us += STATEMENT_US;   // (THEN's part is IF's)
        pace();
        first = 0;
        if (reset_requested) {
            reset_requested = 0;
            new_program();
            break;
        }
        use_lower(0);
        if (!error_msg && scroll_stopped()) error("D BREAK - CONT repeats");
        if (!error_msg && !jumped && running && !next_statement()) running = 0;
        if (error_msg) {
            cont_ok = stmt_line >= 0;
            if (cont_ok) {
                cur_line = stmt_line;
                cont_line = line_num(stmt_line);
                cont_stmt = statement_number(stmt_pos);
            }
            cont_next = 0;
            report_msg = error_msg;
            break;
        }
    }
    scroll_stopped();
    printer_flush();
    if (!no_report) print_report(report_msg, rep_line < 0 ? 0 : rep_line, rep_stmt);
}
/* ------------------------------------------------------------------------
 * The command line
 * --------------------------------------------------------------------- */

HIGH_MEM char input_buf[MAX_TEXT + 1];
int last_line;            // the last line number entered, for EDIT
HIGH_MEM unsigned char typed_line[MAX_TEXT + 1];   // tokenized; run from here

// Read a line into input_buf with the line editor, starting with
// whatever is in edit_next (put there by EDIT). Returns -1 for Esc.
HIGH_MEM char edit_next[MAX_TEXT + 1];

static int edit_cursor = -1;      // where the cursor starts in edit_next (-1: the end)

static int edit_input(int use_history) {
    strcpy(input_buf, edit_next);
    memset(edit_next, 0, 1);          // (not edit_next[0] = 0: see memory.c's ram_p)
    int cursor = edit_cursor;
    edit_cursor = -1;
    return edit_line(input_buf, sizeof(input_buf), use_history, cursor);
}

// Where, in the typed text s, offset k of its tokenized form (tokens)
// comes from: the shortest start of s that tokenizes to the same first k
// bytes.
static int source_offset(const char *s, int k, const unsigned char *tokens) {
    int n = strlen(s);
    for (int len = 0; len < n; len++) {
        memcpy(list_buf, s, len);
        memset(list_buf + len, 0, 1);
        int m = tokenize(list_buf, (unsigned char *)list_kw, sizeof(list_kw));
        if (m - 1 >= k && memcmp(list_kw, tokens, k) == 0) {
            while (s[len] == ' ') len++;  // (the ? goes on what follows)
            return len;
        }
    }
    return n;
}

// A line that doesn't make sense goes back to the editor, with a
// flashing ? where it stops making sense (and a rasp), as on the Spectrum.
static void refuse_line(const char *s, int bad) {
    int mark = (s - input_buf) + source_offset(s, bad, typed_line);
    strcpy(edit_next, input_buf);
    edit_cursor = mark;
    edit_show_error();
    transcript("[ERR]");              // (for tests: the line, with the ?)
    memcpy(list_buf, input_buf, mark);
    memcpy(list_buf + mark, "?", 2);
    transcript(list_buf);
    transcript(input_buf + mark);
    transcript("\n");
    tone(110, 100);
}

// For INPUT: edit what's in input_buf, with the cursor at cursor.
static int read_line(int cursor) {
    return edit_line(input_buf, sizeof(input_buf), 0, cursor);
}

/*
 * The command loop. As on the Spectrum: lines are typed in the lower
 * screen; a line with a number goes into the program and the program is
 * listed (the automatic listing); anything else is run straight away,
 * and its report shown until a key is pressed.
 */
void basic_run() {
    stack_base = __builtin_frame_address(0);
    int want_list = 0;        // show the automatic listing
    int listing_shown = 0;    // it's on the screen
    mem_reset();
    show_banner();
    for (;;) {
        if (want_list) {
            auto_list();
            want_list = 0;
            listing_shown = 1;
        }
        use_lower(1);
        if (report_shown) {               // the first key clears the report
            int k = wait_key();
            clear_lower();
            if (k != 27) unget_key(k);
            report_shown = 0;
        }
        set_scroll_count(upper_rows());
        int r = edit_input(1);
        clear_lower();
        use_lower(0);
        if (r < 0) continue;              // Esc: forget the line
        char *s = input_buf;
        while (*s == ' ') s++;
        if (*s == '\0') {                // Enter on its own: list
            want_list = 1;
            continue;
        }

        int num = -1;
        if (isdigit(*s)) {
            num = 0;
            while (isdigit(*s) && num <= 9999) num = num * 10 + (*s++ - '0');
            if (num < 1 || num > 9999) {
                print_report("C Nonsense in BASIC", 0, 1);
                continue;
            }
            while (*s == ' ') s++;
        }
        int len = tokenize(s, typed_line, sizeof(typed_line));
        if (len < 0) {
            print_report("G No room for line", 0, 1);
            continue;
        }
        if (len > 1 || num < 0) {
            int bad = syntax_check((char *)typed_line);
            if (bad >= 0) {
                refuse_line(s, bad);
                continue;
            }
        }
        if (num > 0) {
            last_line = e_ppc = num;
            // FOR loops, GO SUB and CONTINUE survive editing, as on the
            // Spectrum (they refer to line numbers).
            data_mark();
            if (len == 1) {
                line_delete(num);         // just a number: delete the line
            } else {
                const char *err = line_store(num, typed_line, len);
                if (err) print_report(err, 0, 1);
            }
            data_unmark();
            want_list = 1;
        } else {
            if (listing_shown) {          // (tests don't want to see this)
                screen_echo(0);
                clear_screen();
                screen_echo(1);
            }
            listing_shown = 0;
            cur_line = -1;
            p = (char *)typed_line;
            run();
        }
    }
}
