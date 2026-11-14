/*
 * BASIC V2: the 1982 64KB home computer's BASIC, as a dialect of the
 * family's language core (machines/msbasic/), on its hardware
 * (machines/c64hw/). Written for this project from how it behaves; none
 * of it comes from the original's code.
 *
 * BASIC V2 has no statements for graphics, sound or the cursor: programs
 * POKE the chips and print control characters for those. What's its own:
 *   - Lines keep their spaces as typed, and are listed as they're kept.
 *     Keywords can be typed short: their first letters, then the next
 *     shifted (pO is POKE).
 *   - True is -1, and AND, OR and NOT work on the bits of 16-bit integers.
 *   - Numbers print with a space (or -) for the sign, and a cursor right
 *     after them; STR$ keeps the space. Integers are -32768 to 32767.
 *   - Errors: ?SYNTAX  ERROR IN 10, then READY.
 *   - TI is the clock in 60ths of a second, TI$ the time as HHMMSS (TI$ =
 *     "000000" sets it), ST the status of the last input or output.
 *   - GET doesn't wait: "" (or 0) if no key has been pressed. An empty
 *     line for INPUT leaves its variables as they were.
 *   - RND(-n) starts a sequence again, RND(0) takes a number from the
 *     clock.
 *   - FRE(0) counts from the program's start (2049) to the top of BASIC's
 *     memory (40960), and is negative above 32767, as there.
 *   - It runs at about the original's speed (see pace()).
 */
#include "c64basic.h"
#include "c64listing.h"
#include "msbasic.h"
#include "c64.h"
#include "num.h"
#include "string.h"
#include "mem.h"
#include "platform.h"

/* ------------------------------------------------------------------------
 * Keywords, in the original's order, which is also its token numbers
 * (128 on): its files keep them so (operators among them).
 * --------------------------------------------------------------------- */

static const struct ms_keyword keywords[] = {
    { "END", T_END }, { "FOR", T_FOR }, { "NEXT", T_NEXT }, { "DATA", T_DATA },
    { "INPUT#", T_INPUTN }, { "INPUT", T_INPUT }, { "DIM", T_DIM }, { "READ", T_READ },
    { "LET", T_LET }, { "GOTO", T_GOTO }, { "RUN", T_RUN }, { "IF", T_IF },
    { "RESTORE", T_RESTORE }, { "GOSUB", T_GOSUB }, { "RETURN", T_RETURN }, { "REM", T_REM },
    { "STOP", T_STOP }, { "ON", T_ON }, { "WAIT", T_WAIT }, { "LOAD", T_LOAD },
    { "SAVE", T_SAVE }, { "VERIFY", T_VERIFY }, { "DEF", T_DEF }, { "POKE", T_POKE },
    { "PRINT#", T_PRINTN }, { "PRINT", T_PRINT }, { "CONT", T_CONT }, { "LIST", T_LIST },
    { "CLR", T_CLEAR }, { "CMD", T_CMD }, { "SYS", T_SYS }, { "OPEN", T_OPEN },
    { "CLOSE", T_CLOSE }, { "GET", T_GET64 }, { "NEW", T_NEW }, { "TAB(", T_TAB },
    { "TO", T_TO }, { "FN", T_FN }, { "SPC(", T_SPC }, { "THEN", T_THEN }, { "NOT", T_NOT },
    { "STEP", T_STEP }, { "+", '+' }, { "-", '-' }, { "*", '*' }, { "/", '/' }, { "^", '^' },
    { "AND", T_AND }, { "OR", T_OR }, { ">", '>' }, { "=", '=' }, { "<", '<' },
    { "SGN", T_SGN }, { "INT", T_INT }, { "ABS", T_ABS }, { "USR", T_USR }, { "FRE", T_FRE },
    { "POS", T_POS }, { "SQR", T_SQR }, { "RND", T_RND }, { "LOG", T_LOG }, { "EXP", T_EXP },
    { "COS", T_COS }, { "SIN", T_SIN }, { "TAN", T_TAN }, { "ATN", T_ATN }, { "PEEK", T_PEEK },
    { "LEN", T_LEN }, { "STR$", T_STR }, { "VAL", T_VAL }, { "ASC", T_ASC }, { "CHR$", T_CHR },
    { "LEFT$", T_LEFT }, { "RIGHT$", T_RIGHT }, { "MID$", T_MID }, { "GO", T_GO },
};
#define N_KEYWORDS ((int)(sizeof(keywords) / sizeof(keywords[0])))

const struct ms_dialect ms_dialect = {
    .keywords = keywords,
    .n_keywords = N_KEYWORDS,
    .keep_spaces = 1,
    .sign_space = 1,
    .int_min = -32768,
    .abbrev = 1,
    .empty_input_ends = 1,
    .bitwise = 1,
    .redo = "?REDO FROM START",
    // (benchmark timings of the original: an empty FOR loop's round
    // 1.1ms, an assignment with an addition about 1.5ms, a SQR 25ms)
    .cost_statement = 1100,
    .cost_op = 400,
    .cost_mul = 900,
    .cost_fn = 1200,
    .cost_maths = 9000,
};

#define ip ms_ip
#define err ms_err

// The token the original gives byte c (128-255) of a line, as this one
// keeps it: 128 on are its keywords, in order; pi is 255.
int c64_token(int c) {
    if (c >= 0x80 && c - 0x80 < N_KEYWORDS) return keywords[c - 0x80].token;
    return c;
}

// ... and back: the original's byte for one of this one's tokens.
int c64_original_token(int t) {
    for (int i = 0; i < N_KEYWORDS; i++)
        if (keywords[i].token == t) return 0x80 + i;
    return t;
}

int ms_line_char(int c, int in_string) {
    c &= 0xFF;
    // (Outside strings, a byte of 128 or more is a keyword's, as there:
    // a shifted letter typed on its own is one.)
    return in_string ? c : c64_token(c);
}

/* ------------------------------------------------------------------------
 * Errors
 * --------------------------------------------------------------------- */

const char *disk_error_text(int e);

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
    const char *t = disk_error_text(e);
    return t ? t : "SYNTAX";
}

/* ------------------------------------------------------------------------
 * Output
 * --------------------------------------------------------------------- */

void ms_out(int c) {
    if (disk_out(c)) return;
    c64_chrout(c);
    ms_cost_us += 350;                    // (the screen editor's time)
}

static void outs(const char *s) {
    while (*s) ms_out(*s++);
}

int ms_column(void) { return c64_column(); }

// Cursor rights on the screen; spaces in a file.
static void cursor_rights(int n) {
    int c = disk_to_file() ? ' ' : 29;
    while (n-- > 0) ms_out(c);
}

void ms_print_number(double x) {
    char b[24];
    ms_format(x, b);
    outs(b);
    cursor_rights(1);                     // (on the screen a cursor right, not a space)
}

void ms_print_comma(void) {               // to the next 10-column field
    cursor_rights(10 - ms_column() % 10);
}

void ms_print_tab(int n, int spc) {
    cursor_rights(spc ? n : n - ms_column());
}

void ms_list_line(const char *text) {
    ms_out(13);
    outs(text);
}

int ms_peek(unsigned int addr) { return c64_peek(addr); }
void ms_poke(unsigned int addr, int value) { c64_poke(addr, value); }

static int restored;                      // RUN/STOP+RESTORE stopped the program

/*
 * Speed. A PC runs BASIC thousands of times faster than the original,
 * which would make delay loops and games far too fast, so programs are
 * held back to about its speed: what each statement would take there is
 * added up (ms_cost_us: from the statement, the arithmetic and functions
 * in it, and the characters it prints) and the program waits to match.
 * POKE 2,1 runs at full speed (2 is a byte the firmware leaves unused);
 * POKE 2,0 goes back.
 */
#define FULL_SPEED 2
static long ahead_us;                     // how far the original's time is ahead of ours
static unsigned long pace_ms;

static void pace(void) {
    unsigned long now = plat_timer_ms();
    ahead_us += (long)ms_cost_us - (long)(now - pace_ms) * 1000;
    ms_cost_us = 0;
    pace_ms = now;
    if (c64_peek(FULL_SPEED) & 1) {
        ahead_us = 0;
        return;
    }
    // If the PC fell behind (waiting for a key, say), don't make it up
    // later by running fast; a little slack evens out the timer's steps.
    if (ahead_us < -60000) ahead_us = -60000;
    while (ahead_us > 0 && !c64_stop_waiting()) {
#ifndef HOST_TEST                         // (the tests' clock moves on by itself)
        plat_idle();
#endif
        c64_tick();
        now = plat_timer_ms();
        ahead_us -= (long)(now - pace_ms) * 1000;
        pace_ms = now;
    }
}

int ms_break(void) {
    pace();
    if (c64_restore_pressed()) restored = 1;
    return restored || c64_stop_pressed();
}

void ms_wait_idle(void) {
    static int n;
    c64_tick();
    if (++n % 1024 == 0) plat_idle();
}

/* ------------------------------------------------------------------------
 * INPUT and GET
 * --------------------------------------------------------------------- */

int ms_input_line(const unsigned char *prompt, int plen, int more, char *buf, int cap) {
    int done, ok = disk_input_line(buf, cap > 89 ? 89 : cap, &done);
    if (done) return ok;                  // (INPUT#)
    if (more) ms_out('?');
    for (int i = 0; i < plen; i++) ms_out(prompt[i]);
    outs("? ");
    if (c64_input_line(buf, cap > 89 ? 89 : cap) < 0) {
        restored = 1;
        ms_error(E_BREAK);
        return 0;
    }
    return 1;
}

int ms_get_key(void) {
    int done, k = disk_get(&done);
    if (done) return k;                   // (GET#)
    k = c64_get_key();
    c64_show_cursor(k < 0 && c64_peek(C64_BLNSW) == 0);   // (POKE 204,0: it flashes)
    return k;
}

/* ------------------------------------------------------------------------
 * TI, TI$ and ST
 * --------------------------------------------------------------------- */

enum { V_TI = 1, V_TIS, V_ST };

int ms_special(const char name[2], int type) {
    if (name[0] == 'T' && name[1] == 'I') return type == MS_STR ? V_TIS : type == MS_REAL ? V_TI : 0;
    if (name[0] == 'S' && name[1] == 'T' && type == MS_REAL) return V_ST;
    return 0;
}

void ms_special_get(int id, struct ms_val *v) {
    unsigned long j = c64_jiffies();
    if (id == V_TIS) {
        unsigned char *s = ms_scratch(6);
        if (!s) return;
        unsigned long t[3] = { j / 216000, j / 3600 % 60, j / 60 % 60 };
        for (int i = 0; i < 3; i++) {
            s[i * 2] = '0' + t[i] / 10 % 10;
            s[i * 2 + 1] = '0' + t[i] % 10;
        }
        ms_make_str(v, s, 6);
    } else {
        ms_make_num(v, id == V_TI ? (double)j : c64_peek(C64_STATUS));
    }
}

void ms_special_set(int id, const struct ms_val *v) {
    if (id != V_TIS) {                    // (TI and ST can't be set)
        ms_error(E_SYNTAX);
        return;
    }
    unsigned long t = 0;
    for (int i = 0; i < 6; i++) {
        int c = i < v->len ? v->s[i] : -1;
        if (v->len != 6 || c < '0' || c > '9') {
            ms_error(E_QUANTITY);
            return;
        }
        t = t * 10 + c - '0';
    }
    unsigned long j = (t / 10000 * 3600 + t / 100 % 100 * 60 + t % 100) * 60;
    if (j >= 5184000UL) {
        ms_error(E_QUANTITY);
        return;
    }
    c64_poke(C64_JIFFY, j >> 16);
    c64_poke(C64_JIFFY + 1, j >> 8 & 0xFF);
    c64_poke(C64_JIFFY + 2, j & 0xFF);
}

/* ------------------------------------------------------------------------
 * Functions
 * --------------------------------------------------------------------- */

static unsigned long rnd_seed = 0x2B5A1C3;

double ms_rnd(double x) {
    if (x < 0) {
        double s = -x;
        rnd_seed = (unsigned long)(s * 1000003.0) ^ 0x5DEECE6UL;
    } else if (x == 0) {                  // from the clock
        rnd_seed ^= plat_timer_ms() * 2654435761UL ^ c64_jiffies() << 9;
    }
    rnd_seed = rnd_seed * 1103515245UL + 12345UL;
    rnd_seed &= 0xFFFFFFFFUL;
    return (double)(rnd_seed >> 8) / 16777216.0;
}

double ms_fre(void) {
    long start = c64_peek(C64_TXTTAB) | c64_peek(C64_TXTTAB + 1) << 8;
    long top = c64_peek(C64_MEMSIZ) | c64_peek(C64_MEMSIZ + 1) << 8;
    long room = top - start - ms_memory_used();
    if (room < 0) room = 0;
    return room > 32767 ? room - 65536 : room;
}

int ms_function(int t, struct ms_val *v) {
    if (t == T_PI) {
        ms_make_num(v, 3.14159265358979);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

static int reset;                         // SYS 64738: start again

int ms_statement(int t) {
    switch (t) {
    case T_GET64:
        if (ms_cur_line < 0) {
            ms_error(E_DIRECT);
            return 1;
        }
        if (ms_cur() == '#') return disk_statement(t);
        ms_get_vars();
        return 1;
    case T_SYS: {
        unsigned int a = ms_address();
        if (ms_err != E_NONE) return 1;
        if (c64_sys(a) == 2) {            // the reset
            reset = 1;
            ms_ended = 1;
        }
        return 1;                         // (machine code: not run)
    }
    }
    return disk_statement(t);
}

/* ------------------------------------------------------------------------
 * Errors, and the prompt
 * --------------------------------------------------------------------- */

void ms_report(int line) {
    disk_screen();                        // (output back to the screen)
    if (restored) {                       // (no message: just READY.)
        restored = 0;
        c64_warm_start();
        return;
    }
    ms_out(13);
    if (err == E_BREAK) {
        outs("BREAK");
    } else {
        ms_out('?');
        outs(error_text(err));
        outs("  ERROR");
    }
    if (line >= 0) {
        char b[12];
        itoa(line, b);
        outs(" IN ");
        outs(b);
    }
}

void ms_program_ended(void) {}
void ms_cleared(void) { disk_close_all(); }

#ifdef HOST_TEST
void screen_host_hook(void);
#endif

HIGH_MEM char c64_line[96];

// (Through a function: see core/mem.h.)
__attribute__((noinline, noclone)) static int first_char(const char *s) {
    while (*s == ' ') s++;
    return *s;
}

void c64basic_run(void) {
    c64_init();
    disk_init();
#ifdef HOST_TEST
    screen_host_hook();
#endif
start:
    c64_print("\r    **** X86 BASIC V2 ****\r\r 64K RAM SYSTEM  38911 BASIC BYTES FREE\r");
    for (;;) {
        c64_print("\rREADY.\r");
        for (;;) {
            if (c64_input_line(c64_line, 89) < 0) {   // RUN/STOP+RESTORE
                c64_warm_start();
                break;
            }
            int c = first_char(c64_line);
            if (!c) continue;
            ms_enter_line(c64_line);
            if (reset) {
                reset = 0;
                ms_prog_len = 0;
                ms_clear_vars();
                c64_init();
                disk_init();
                goto start;
            }
            if (restored) {               // (RESTORE stopped LIST, say)
                restored = 0;
                c64_warm_start();
            }
            if (c < '0' || c > '9') break;   // (a command: READY. again)
        }
    }
}
