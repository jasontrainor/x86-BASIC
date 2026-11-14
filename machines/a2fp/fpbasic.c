/*
 * FP BASIC: the floating-point BASIC of the 1978-83 colour machines,
 * as on the later model (the IIe-style one: lower case, 80 columns).
 * Written for this project from how it behaves; none of it comes from
 * the original's code.
 *
 * The language is the family's (machines/msbasic/); this is the dialect:
 * its keywords, its graphics and screen statements, the disk commands,
 * and the hooks to the Apple layer (machines/apple/). What's its own:
 *   - Spaces are taken out of lines, except in strings, REM and DATA,
 *     and keywords are found even with spaces in them (A TO B, typed
 *     ATOB, is A TO B, as AT is never followed by O). LIST puts spaces
 *     around keywords and operators.
 *   - Integers are -32767 to 32767.
 *   - ONERR GOTO catches errors: PEEK(222) is the error's number,
 *     PEEK(218)+256*PEEK(219) its line; RESUME goes back to the statement.
 *     POKE 216,0 turns it off.
 *   - PRINT CHR$(4);"command" gives a disk command (dos.h).
 */
#include "fpbasic.h"
#include "msbasic.h"
#include "apple.h"
#include "dos.h"
#include "num.h"
#include "string.h"
#include "mem.h"
#include "platform.h"

/* ------------------------------------------------------------------------
 * Keywords, in the original's order (which matters: the first that
 * matches is the one used, so ONERR is found before ON)
 * --------------------------------------------------------------------- */

enum {
    T_GR = T_DIALECT, T_TEXT, T_PR, T_IN, T_CALL, T_PLOT, T_HLIN, T_VLIN, T_HGR2, T_HGR,
    T_HCOLOR, T_HPLOT, T_DRAW, T_XDRAW, T_HTAB, T_HOME, T_ROT, T_SCALE, T_SHLOAD, T_NORMAL,
    T_INVERSE, T_FLASH, T_COLOR, T_VTAB, T_HIMEM, T_LOMEM, T_RECALL, T_STORE, T_SPEED,
    T_AMPER, T_AT, T_SCRN, T_PDL,
};

static const struct ms_keyword keywords[] = {
    { "END", T_END }, { "FOR", T_FOR }, { "NEXT", T_NEXT }, { "DATA", T_DATA },
    { "INPUT", T_INPUT }, { "DEL", T_DEL }, { "DIM", T_DIM }, { "READ", T_READ },
    { "GR", T_GR }, { "TEXT", T_TEXT }, { "PR#", T_PR }, { "IN#", T_IN }, { "CALL", T_CALL },
    { "PLOT", T_PLOT }, { "HLIN", T_HLIN }, { "VLIN", T_VLIN }, { "HGR2", T_HGR2 },
    { "HGR", T_HGR }, { "HCOLOR=", T_HCOLOR }, { "HPLOT", T_HPLOT }, { "DRAW", T_DRAW },
    { "XDRAW", T_XDRAW }, { "HTAB", T_HTAB }, { "HOME", T_HOME }, { "ROT=", T_ROT },
    { "SCALE=", T_SCALE }, { "SHLOAD", T_SHLOAD }, { "TRACE", T_TRACE },
    { "NOTRACE", T_NOTRACE }, { "NORMAL", T_NORMAL }, { "INVERSE", T_INVERSE },
    { "FLASH", T_FLASH }, { "COLOR=", T_COLOR }, { "POP", T_POP }, { "VTAB", T_VTAB },
    { "HIMEM:", T_HIMEM }, { "LOMEM:", T_LOMEM }, { "ONERR", T_ONERR }, { "RESUME", T_RESUME },
    { "RECALL", T_RECALL }, { "STORE", T_STORE }, { "SPEED=", T_SPEED }, { "LET", T_LET },
    { "GOTO", T_GOTO }, { "RUN", T_RUN }, { "IF", T_IF }, { "RESTORE", T_RESTORE },
    { "&", T_AMPER }, { "GOSUB", T_GOSUB }, { "RETURN", T_RETURN }, { "REM", T_REM },
    { "STOP", T_STOP }, { "ON", T_ON }, { "WAIT", T_WAIT }, { "LOAD", T_LOAD },
    { "SAVE", T_SAVE }, { "DEF", T_DEF }, { "POKE", T_POKE }, { "PRINT", T_PRINT },
    { "CONT", T_CONT }, { "LIST", T_LIST }, { "CLEAR", T_CLEAR }, { "GET", T_GET },
    { "NEW", T_NEW }, { "TAB(", T_TAB }, { "TO", T_TO }, { "FN", T_FN }, { "SPC(", T_SPC },
    { "THEN", T_THEN }, { "AT", T_AT }, { "NOT", T_NOT }, { "STEP", T_STEP }, { "AND", T_AND },
    { "OR", T_OR }, { "SGN", T_SGN }, { "INT", T_INT }, { "ABS", T_ABS }, { "USR", T_USR },
    { "FRE", T_FRE }, { "SCRN(", T_SCRN }, { "PDL", T_PDL }, { "POS", T_POS },
    { "SQR", T_SQR }, { "RND", T_RND }, { "LOG", T_LOG }, { "EXP", T_EXP }, { "COS", T_COS },
    { "SIN", T_SIN }, { "TAN", T_TAN }, { "ATN", T_ATN }, { "PEEK", T_PEEK }, { "LEN", T_LEN },
    { "STR$", T_STR }, { "VAL", T_VAL }, { "ASC", T_ASC }, { "CHR$", T_CHR },
    { "LEFT$", T_LEFT }, { "RIGHT$", T_RIGHT }, { "MID$", T_MID },
};

const struct ms_dialect ms_dialect = {
    .keywords = keywords,
    .n_keywords = sizeof(keywords) / sizeof(keywords[0]),
    .list_spaces = 1,
    .get_waits = 1,
    .int_min = -32767,
    .at_token = T_AT,
    .onerr = 1,
    .redo = "?REENTER",
};

#define ip ms_ip
#define err ms_err

static const char *error_text(int e) {
    switch (e) {
    case E_NEXT:      return "NEXT WITHOUT FOR";
    case E_SYNTAX:    return "SYNTAX";
    case E_RETURN:    return "RETURN WITHOUT GOSUB";
    case E_DATA:      return "OUT OF DATA";
    case E_QUANTITY:  return "ILLEGAL QUANTITY";
    case E_OVERFLOW:  return "OVERFLOW";
    case E_MEMORY:    return "OUT OF MEMORY";
    case E_UNDEF:     return "UNDEF'D STATEMENT";
    case E_SUBSCRIPT: return "BAD SUBSCRIPT";
    case E_REDIM:     return "REDIM'D ARRAY";
    case E_DIV0:      return "DIVISION BY ZERO";
    case E_DIRECT:    return "ILLEGAL DIRECT";
    case E_TYPE:      return "TYPE MISMATCH";
    case E_LONG:      return "STRING TOO LONG";
    case E_COMPLEX:   return "FORMULA TOO COMPLEX";
    case E_CONT:      return "CAN'T CONTINUE";
    case E_FN:        return "UNDEF'D FUNCTION";
    }
    return "SYNTAX";
}

/* ------------------------------------------------------------------------
 * Output (with disk commands: Ctrl+D, then the command; see dos.h)
 * --------------------------------------------------------------------- */

static void dos_done(int result);

static void speed_delay(void) {
    int speed = 256 - apple_peek(241);    // (kept as 256 - SPEED, as the original)
    if (speed >= 255 || speed < 0) return;
    unsigned long until = plat_timer_ms() + (255 - speed) / 8;
    while (plat_timer_ms() < until) apple_key_peek();
}

void ms_out(int c) {
    c &= 0x7F;
    if (dos_print(c)) {                   // (a command, or a text file being written)
        int r = dos_result();
        if (r != DOS_NONE) dos_done(r);
        return;
    }
    speed_delay();
    apple_cout(c);
}

static void outs(const char *s) {
    while (*s) ms_out(*s++);
}

int ms_column(void) { return apple_peek(ZP_CH); }

static void fresh_line(void) {
    if (ms_column() != 0) ms_out(13);
}

void ms_print_number(double x) {
    char b[24];
    ms_format(x, b);
    outs(b);
}

void ms_print_comma(void) {               // the next of the 16-column fields
    int w = apple_peek(ZP_WNDWDTH);
    if (ms_column() >= w - 16) ms_out(13);
    else do ms_out(' '); while (ms_column() % 16);
}

void ms_print_tab(int n, int spc) {
    if (spc) while (n-- > 0) ms_out(' ');
    else while (ms_column() < n - 1) ms_out(' ');
}

void ms_list_line(const char *text) {
    fresh_line();
    outs(text);
    ms_out(13);
}

int ms_peek(unsigned int addr) { return apple_peek(addr); }
void ms_poke(unsigned int addr, int value) { apple_poke(addr, value); }

int ms_break(void) {
    if (apple_key_peek() == 3) {
        apple_key();
        return 1;
    }
    return 0;
}

void ms_wait_idle(void) { plat_idle(); }

// A character of a line as the original keeps it: upper case outside
// strings; in 7 bits, since its keyboard has no more (codes 128 and up
// are its keywords, here as there), and a 0 (the end of a line) as a
// space.
int ms_line_char(int c, int in_string) {
    c &= 0xFF;
    if (!in_string && c >= 'a' && c <= 'z') c -= 32;
    c &= 0x7F;
    return c ? c : ' ';
}

/* ------------------------------------------------------------------------
 * INPUT and GET: from the keyboard, or the text file being read
 * --------------------------------------------------------------------- */

int ms_input_line(const unsigned char *prompt, int plen, int more, char *buf, int cap) {
    if (dos_reading()) {
        if (dos_read_line(buf, cap) < 0) {
            ms_dos_err = DOS_END_OF_DATA;
            ms_error(E_SYNTAX);
            return 0;
        }
        return 1;
    }
    if (more) {
        ms_out('?');
        apple_getln('?', buf, cap);
        return 1;
    }
    for (int i = 0; i < plen; i++) ms_out(prompt[i]);
    apple_getln(plen ? 0 : '?', buf, cap);
    return 1;
}

int ms_get_key(void) {
    int k = dos_reading() ? dos_read_char() : apple_wait_key();
    if (k < 0) {
        ms_dos_err = DOS_END_OF_DATA;
        ms_error(E_SYNTAX);
        return -2;
    }
    return k;
}

/* ------------------------------------------------------------------------
 * Functions
 * --------------------------------------------------------------------- */

static unsigned long rnd_seed = 0x1234567;
static double rnd_last;

double ms_rnd(double x) {
    if (x < 0) {
        double s = -x;
        rnd_seed = (unsigned long)(s * 1000003.0) ^ 0x5DEECE6UL;
    }
    if (x == 0) return rnd_last;
    rnd_seed = rnd_seed * 1103515245UL + 12345UL;
    rnd_seed &= 0xFFFFFFFFUL;
    rnd_last = (double)(rnd_seed >> 8) / 16777216.0;
    return rnd_last;
}

/*
 * FRE: the room the original would have left, worked out as its memory
 * would be laid out (this one's isn't), so that programs that check it
 * see what they would there: the program from $801, then its variables
 * (ms_memory_used), up to HIMEM. As there, more than 32767 comes out
 * less 65536 (programs add it back).
 */
double ms_fre(void) {
    long used = 0x801 + ms_memory_used();
    long room = (apple_peek(115) | apple_peek(116) << 8) - used;
    if (room < 0) room = 0;
    return room > 32767 ? room - 65536 : room;
}

int ms_function(int t, struct ms_val *v) {
    double x, y;
    switch (t) {
    case T_SCRN:                          // (SCRN( has its bracket)
        x = ms_num_expr();
        ms_expect(',');
        y = ms_num_expr();
        ms_expect(')');
        if (err != E_NONE) return 1;
        if (x < 0 || x > 39 || y < 0 || y > 47) {
            ms_error(E_QUANTITY);
            return 1;
        }
        ms_make_num(v, apple_scrn((int)x, (int)y));
        return 1;
    case T_PDL:
        x = ms_num_arg();
        if (x < 0 || x > 3) ms_error(E_QUANTITY);
        ms_make_num(v, err == E_NONE ? apple_paddle((int)x) : 0);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

static void plot_args(int *x, int *y, int xmax, int ymax) {
    *x = ms_quantity(xmax);
    ms_expect(',');
    *y = ms_quantity(ymax);
}

static void hplot(void) {
    int x, y;
    if (*ip != T_TO) {
        plot_args(&x, &y, 279, 191);
        if (err != E_NONE) return;
        apple_hplot(x, y);
    }
    while (*ip == T_TO && err == E_NONE) {
        ip++;
        plot_args(&x, &y, 279, 191);
        if (err != E_NONE) return;
        apple_hplot_to(x, y);
    }
}

static void draw(int xor) {
    int n = ms_quantity(255), x = -1, y = -1;
    if (*ip == T_AT) {
        ip++;
        plot_args(&x, &y, 279, 191);
    }
    if (err != E_NONE) return;
    if (!apple_draw(n, x, y, xor)) ms_error(E_QUANTITY);
}

int ms_statement(int t) {
    int n, x, y;
    switch (t) {
    case T_CALL: {
        unsigned int a = ms_address();
        if (err != E_NONE) return 1;
        if (a == 0xF3F2) apple_hclear(0);         // 62450: clear the hi-res page
        else if (a == 0xF3F6) apple_hclear(1);    // 62454: fill it with HCOLOR
        else if (a == 0xF3D8) apple_hgr(1);       // 62424: HGR2
        else if (a == 0xF3E2) apple_hgr(0);       // 62434: HGR
        else apple_call(a);                       // (machine code: not run)
        return 1;
    }
    case T_HOME: apple_home(); return 1;
    case T_NORMAL: apple_poke(ZP_INVFLG, 0xFF); return 1;
    case T_INVERSE: apple_poke(ZP_INVFLG, 0x3F); return 1;
    case T_FLASH: apple_poke(ZP_INVFLG, 0x7F); return 1;
    case T_HTAB: {
        n = ms_quantity(255);
        if (err != E_NONE) return 1;
        if (n == 0) {
            ms_error(E_QUANTITY);
            return 1;
        }
        int w = apple_peek(ZP_WNDWDTH);
        apple_htab((n - 1) % (w ? w : 40));
        return 1;
    }
    case T_VTAB:
        n = ms_quantity(24);
        if (err == E_NONE && n == 0) ms_error(E_QUANTITY);
        if (err == E_NONE) apple_vtab(n - 1);
        return 1;
    case T_SPEED:
        n = ms_quantity(255);
        if (err == E_NONE) apple_poke(241, (256 - n) & 0xFF);
        return 1;
    case T_GR: apple_gr(); return 1;
    case T_TEXT: apple_text(); return 1;
    case T_COLOR:
        n = ms_quantity(255);
        if (err == E_NONE) apple_color(n);
        return 1;
    case T_PLOT:
        plot_args(&x, &y, 39, 47);
        if (err == E_NONE) apple_plot(x, y);
        return 1;
    case T_HLIN:
    case T_VLIN: {
        int top = t == T_HLIN ? 39 : 47;
        int a = ms_quantity(top);
        ms_expect(',');
        int b = ms_quantity(top);
        ms_expect(T_AT);
        n = ms_quantity(86 - top);
        if (err != E_NONE) return 1;
        if (t == T_HLIN) apple_hlin(a, b, n);
        else apple_vlin(a, b, n);
        return 1;
    }
    case T_HGR: apple_hgr(0); return 1;
    case T_HGR2: apple_hgr(1); return 1;
    case T_HCOLOR:
        n = ms_quantity(7);
        if (err == E_NONE) apple_hcolor(n);
        return 1;
    case T_HPLOT: hplot(); return 1;
    case T_DRAW: draw(0); return 1;
    case T_XDRAW: draw(1); return 1;
    case T_ROT:
        n = ms_quantity(255);
        if (err == E_NONE) apple_rot(n);
        return 1;
    case T_SCALE:
        n = ms_quantity(255);
        if (err == E_NONE) apple_scale(n);
        return 1;
    case T_HIMEM:
    case T_LOMEM: {
        unsigned int a = ms_address();
        if (err != E_NONE) return 1;
        apple_poke(t == T_HIMEM ? 115 : 105, a & 0xFF);
        apple_poke(t == T_HIMEM ? 116 : 106, a >> 8);
        ms_clear_vars();
        return 1;
    }
    case T_PR:
        n = ms_quantity(7);
        if (err != E_NONE) return 1;
        if (n == 3) apple_80col(1);
        return 1;
    case T_IN:
        ms_quantity(7);
        return 1;
    case T_LOAD:
    case T_SAVE:
    case T_SHLOAD:
    case T_STORE:
    case T_RECALL:
        ms_skip_statement();              // (cassette tape: see the disk commands)
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Errors
 * --------------------------------------------------------------------- */

void ms_report(int line) {
    fresh_line();
    if (err == E_BREAK) {
        outs("BREAK");
    } else {
        apple_bell();
        if (ms_dos_err) outs(dos_message(ms_dos_err));
        else {
            ms_out('?');
            outs(error_text(err));
            outs(" ERROR");
        }
    }
    if (line >= 0 && !ms_dos_err) {
        char b[12];
        itoa(line, b);
        outs(" IN ");
        outs(b);
    }
    ms_out(13);
}

void ms_program_ended(void) { dos_program_ended(); }
void ms_cleared(void) {}

int ms_special(const char name[2], int type) {   // (none)
    (void)name, (void)type;
    return 0;
}
void ms_special_get(int id, struct ms_val *v) { (void)id, (void)v; }
void ms_special_set(int id, const struct ms_val *v) { (void)id, (void)v; }

/* ------------------------------------------------------------------------
 * Disk commands from programs (PRINT CHR$(4);"..."), and the disk
 * commands' side (dos.h)
 * --------------------------------------------------------------------- */

static void dos_done(int e) {
    if (e == DOS_RUN) {                   // RUN name, from a program: the program it loaded
        err = E_NONE;
        ms_run_from(-1);
        ms_jumped = 1;
        return;
    }
    if (e != DOS_OK) {
        ms_dos_err = e;
        ms_error(E_SYNTAX);
    }
}

HIGH_MEM char fp_list[255 * 8];

static int dos_listing(char *buf, int cap) {
    int n = 0;
    for (int off = 0; off < ms_prog_len; off = ms_line_next(off)) {
        int len = ms_detokenize(off, fp_list);
        if (n + len + 1 > cap) return -1;
        memcpy(buf + n, fp_list, len);
        n += len;
        buf[n++] = '\n';
    }
    return n;
}

static int dos_enter(const char *text, int len) {
    ms_enter_text(text, len);
    return DOS_OK;
}

static const struct dos_basic dos_side = { 'A', dos_listing, dos_enter };

/* ------------------------------------------------------------------------ */

HIGH_MEM char fp_line[260];

// (Through a function: see core/mem.h.)
__attribute__((noinline, noclone)) static int first_char(const char *s) { return s[0]; }

void fpbasic_run(void) {
    apple_init(APPLE_IIE);
    dos_init(&dos_side, "A2FP");
    apple_poke(115, 0x00);                // HIMEM: $9600 (with the disk system)
    apple_poke(116, 0x96);
    apple_poke(103, 0x01);                // the program starts at $0801
    apple_poke(104, 0x08);
    apple_poke(241, 0);                   // SPEED=255
    apple_vtab(23);
    for (;;) {
        fresh_line();
        if (dos_exec_line(fp_line, 240) < 0) apple_getln(']', fp_line, 240);
        if (first_char(fp_line) == 3) continue;
        int d = dos_command(fp_line, 1);
        if (d == DOS_NONE) {
            ms_enter_line(fp_line);
        } else if (d == DOS_RUN) {
            ms_ended = 0;
            ms_run_from(-1);
            if (!ms_ended && err == E_NONE) ms_execute();
            err = E_NONE;
            ms_cur_line = -1;
        } else if (d != DOS_OK) {
            fresh_line();
            apple_bell();
            outs(dos_message(d));
            ms_out(13);
        }
    }
}
