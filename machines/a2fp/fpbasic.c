/*
 * FP BASIC: the floating-point BASIC of the 1978-83 colour machines,
 * as on the later model (the IIe-style one: lower case, 80 columns).
 * Written for this project from how it behaves; none of it comes from
 * the original's code.
 *
 * What programs depend on, and is kept:
 *   - Numbers have 32 significant bits (about 9.6 digits), from about
 *     2.9E-39 to 1.7E+38, and print with up to 9 digits: 1/3 is
 *     .333333333, 1E9 is 1E+09, .001 is 1E-03.
 *   - Only the first two characters of a name count (SCORE is SC), and
 *     keywords are found anywhere in a line, even inside names, when it's
 *     typed in (SCORE is SC OR E; A TO B, typed ATOB, is A TO B, as AT is
 *     never followed by O). Spaces are taken out, except in strings, REM
 *     and DATA.
 *   - Lines aren't checked as they're typed: mistakes show when they're
 *     run (?SYNTAX ERROR IN 10).
 *   - A% names are integers (-32767 to 32767), A$ strings (up to 255
 *     characters). Arrays not DIMmed have 11 elements (0-10) a subscript.
 *   - IF ... THEN: when false, the rest of the line is skipped.
 *   - ONERR GOTO catches errors: PEEK(222) is the error's number,
 *     PEEK(218)+256*PEEK(219) its line; RESUME goes back to the statement.
 *     POKE 216,0 turns it off.
 *   - PRINT CHR$(4);"command" gives a disk command (dos.h).
 */
#include "fpbasic.h"
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

static const char *const keywords[] = {
    "END", "FOR", "NEXT", "DATA", "INPUT", "DEL", "DIM", "READ", "GR", "TEXT", "PR#", "IN#",
    "CALL", "PLOT", "HLIN", "VLIN", "HGR2", "HGR", "HCOLOR=", "HPLOT", "DRAW", "XDRAW",
    "HTAB", "HOME", "ROT=", "SCALE=", "SHLOAD", "TRACE", "NOTRACE", "NORMAL", "INVERSE",
    "FLASH", "COLOR=", "POP", "VTAB", "HIMEM:", "LOMEM:", "ONERR", "RESUME", "RECALL",
    "STORE", "SPEED=", "LET", "GOTO", "RUN", "IF", "RESTORE", "&", "GOSUB", "RETURN", "REM",
    "STOP", "ON", "WAIT", "LOAD", "SAVE", "DEF", "POKE", "PRINT", "CONT", "LIST", "CLEAR",
    "GET", "NEW", "TAB(", "TO", "FN", "SPC(", "THEN", "AT", "NOT", "STEP", "AND", "OR",
    "SGN", "INT", "ABS", "USR", "FRE", "SCRN(", "PDL", "POS", "SQR", "RND", "LOG", "EXP",
    "COS", "SIN", "TAN", "ATN", "PEEK", "LEN", "STR$", "VAL", "ASC", "CHR$", "LEFT$",
    "RIGHT$", "MID$",
};
#define N_KEYWORDS ((int)(sizeof(keywords) / sizeof(keywords[0])))

enum {
    T_END = 0x80, T_FOR, T_NEXT, T_DATA, T_INPUT, T_DEL, T_DIM, T_READ, T_GR, T_TEXT, T_PR,
    T_IN, T_CALL, T_PLOT, T_HLIN, T_VLIN, T_HGR2, T_HGR, T_HCOLOR, T_HPLOT, T_DRAW, T_XDRAW,
    T_HTAB, T_HOME, T_ROT, T_SCALE, T_SHLOAD, T_TRACE, T_NOTRACE, T_NORMAL, T_INVERSE,
    T_FLASH, T_COLOR, T_POP, T_VTAB, T_HIMEM, T_LOMEM, T_ONERR, T_RESUME, T_RECALL,
    T_STORE, T_SPEED, T_LET, T_GOTO, T_RUN, T_IF, T_RESTORE, T_AMPER, T_GOSUB, T_RETURN,
    T_REM, T_STOP, T_ON, T_WAIT, T_LOAD, T_SAVE, T_DEF, T_POKE, T_PRINT, T_CONT, T_LIST,
    T_CLEAR, T_GET, T_NEW, T_TAB, T_TO, T_FN, T_SPC, T_THEN, T_AT, T_NOT, T_STEP, T_AND,
    T_OR, T_SGN, T_INT, T_ABS, T_USR, T_FRE, T_SCRN, T_PDL, T_POS, T_SQR, T_RND, T_LOG,
    T_EXP, T_COS, T_SIN, T_TAN, T_ATN, T_PEEK, T_LEN, T_STR, T_VAL, T_ASC, T_CHR, T_LEFT,
    T_RIGHT, T_MID,
};

/* ------------------------------------------------------------------------
 * Errors: by the numbers ONERR's programs see at PEEK(222)
 * --------------------------------------------------------------------- */

enum {
    E_NONE = -1, E_NEXT = 0, E_SYNTAX = 16, E_RETURN = 22, E_DATA = 42, E_QUANTITY = 53,
    E_OVERFLOW = 69, E_MEMORY = 77, E_UNDEF = 90, E_SUBSCRIPT = 107, E_REDIM = 120,
    E_DIV0 = 133, E_DIRECT = 149, E_TYPE = 163, E_LONG = 176, E_COMPLEX = 191,
    E_CONT = 214, E_FN = 224, E_REENTER = 254, E_BREAK = 255,
};

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

static int err = E_NONE;                  // the error, once there is one
static int dos_err;                       // (a disk error: its DOS number)

static void error(int e) {
    if (err == E_NONE) err = e;
}

/* ------------------------------------------------------------------------
 * Output (with disk commands: a line printed starting with Ctrl+D)
 * --------------------------------------------------------------------- */

static int at_line_start = 1;             // nothing printed since a Return
static int capturing;                     // after a Ctrl+D: a disk command
static char command[256];
static int command_len;
static void disk_command(void);

static void speed_delay(void) {
    int speed = 256 - apple_peek(241);    // (kept as 256 - SPEED, as the original)
    if (speed >= 255 || speed < 0) return;
    unsigned long until = plat_timer_ms() + (255 - speed) / 8;
    while (plat_timer_ms() < until) apple_key_peek();
}

static void out(int c) {
    c &= 0x7F;
    if (capturing) {
        if (c == 13) {
            capturing = 0;
            command[command_len] = '\0';
            at_line_start = 1;
            disk_command();
        } else if (command_len < 255) {
            command[command_len++] = c;
        }
        return;
    }
    if (c == 4 && at_line_start) {
        capturing = 1;
        command_len = 0;
        return;
    }
    if (dos_write_char(c)) {                 // (a text file being written: dos.c)
        at_line_start = c == 13;
        return;
    }
    speed_delay();
    apple_cout(c);
    at_line_start = c == 13;
}

static void outs(const char *s) {
    while (*s) out(*s++);
}

static int column(void) { return apple_peek(ZP_CH); }

static void fresh_line(void) {
    if (column() != 0) out(13);
}

/* ------------------------------------------------------------------------
 * Numbers
 * --------------------------------------------------------------------- */

// Rounded to what the original's numbers hold; an error if too big.
static double fit(double x) {
    if (!num_spectrum(&x)) {
        error(E_OVERFLOW);
        return 0;
    }
    return x;
}

// x as PRINT shows it: up to 9 significant digits.
static void format(double x, char *buf) {
    char *p = buf;
    if (x == 0) {
        strcpy(buf, "0");
        return;
    }
    if (x < 0) {
        *p++ = '-';
        x = -x;
    }
    int e = (int)num_floor(num_log10(x));
    double m = num_round(x * num_pow10(8 - e));
    if (m >= 999999999.5) {
        e++;
        m = num_round(x * num_pow10(8 - e));
    } else if (m < 99999999.5) {
        e--;
        m = num_round(x * num_pow10(8 - e));
    }
    char d[10];
    long v = (long)m;
    for (int i = 8; i >= 0; i--) {
        d[i] = '0' + v % 10;
        v /= 10;
    }
    int n = 9;
    while (n > 1 && d[n - 1] == '0') n--;
    if (e < -2 || e > 8) {                // 1.5E+09, 1E-03
        *p++ = d[0];
        if (n > 1) {
            *p++ = '.';
            for (int i = 1; i < n; i++) *p++ = d[i];
        }
        *p++ = 'E';
        *p++ = e < 0 ? '-' : '+';
        int a = e < 0 ? -e : e;
        *p++ = '0' + a / 10;
        *p++ = '0' + a % 10;
    } else if (e >= 0) {
        for (int i = 0; i <= e; i++) *p++ = i < n ? d[i] : '0';
        if (n > e + 1) {
            *p++ = '.';
            for (int i = e + 1; i < n; i++) *p++ = d[i];
        }
    } else {
        *p++ = '.';
        for (int i = 0; i < -e - 1; i++) *p++ = '0';
        for (int i = 0; i < n; i++) *p++ = d[i];
    }
    *p = '\0';
}

static void out_num(double x) {
    char b[24];
    format(x, b);
    outs(b);
}

static void out_int(int n) {
    char b[12];
    itoa(n, b);
    outs(b);
}

/* ------------------------------------------------------------------------
 * The program: lines in order, each the line number (2 bytes), the
 * length of what follows (2 bytes) and the crunched text, ending in 0.
 * --------------------------------------------------------------------- */

#define PROG_SIZE 40960
#define MAX_LINE 255                      // (crunched)
HIGH_MEM unsigned char fp_prog[PROG_SIZE];
static int prog_len;

static int line_num(int off) { return fp_prog[off] | (fp_prog[off + 1] << 8); }
static int line_next(int off) { return off + 4 + (fp_prog[off + 2] | (fp_prog[off + 3] << 8)); }
static unsigned char *line_text(int off) { return fp_prog + off + 4; }

static int line_find(int num) {
    int off = 0;
    while (off < prog_len && line_num(off) < num) off = line_next(off);
    return off;
}

static void line_delete(int num) {
    int off = line_find(num);
    if (off < prog_len && line_num(off) == num) {
        int next = line_next(off);
        memmove(fp_prog + off, fp_prog + next, prog_len - next);
        prog_len -= next - off;
    }
}

static int line_insert(int num, const unsigned char *text, int len) {
    line_delete(num);
    if (prog_len + len + 4 > PROG_SIZE) return 0;
    int off = line_find(num);
    memmove(fp_prog + off + len + 4, fp_prog + off, prog_len - off);
    fp_prog[off] = num & 0xFF;
    fp_prog[off + 1] = num >> 8;
    fp_prog[off + 2] = len & 0xFF;
    fp_prog[off + 3] = len >> 8;
    memcpy(fp_prog + off + 4, text, len);
    prog_len += len + 4;
    return 1;
}

/* ------------------------------------------------------------------------
 * Crunching a typed line
 * --------------------------------------------------------------------- */

static int is_alpha(int c) { return c >= 'A' && c <= 'Z'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

// The keyword at s (spaces inside it skipped), or 0; *end just after it.
static int match_keyword(const char *s, const char **end) {
    for (int i = 0; i < N_KEYWORDS; i++) {
        const char *p = s, *w = keywords[i];
        while (*w) {
            if (w != keywords[i])
                while (*p == ' ') p++;
            if (upper(*p) != *w) break;
            p++, w++;
        }
        if (*w) continue;
        if (0x80 + i == T_AT) {           // AT, ATN, and A TO
            const char *q = p;
            while (*q == ' ') q++;
            if (upper(*q) == 'N') continue;   // (ATN, later on)
            if (upper(*q) == 'O') return 0;   // A TO: the A's a name
        }
        *end = p;
        return 0x80 + i;
    }
    return 0;
}

static int crunch(const char *s, unsigned char *out_buf, int cap) {
    int n = 0;
#define PUT(c) do { if (n >= cap) return -1; out_buf[n++] = (c); } while (0)
    while (*s) {
        if (*s == ' ') {
            s++;
            continue;
        }
        if (*s == '"') {
            PUT(*s++);
            while (*s && *s != '"') PUT(*s++);
            if (*s) PUT(*s++);
            continue;
        }
        if (*s == '?') {                  // (PRINT, for short)
            PUT(T_PRINT);
            s++;
            continue;
        }
        const char *e;
        int t = match_keyword(s, &e);
        if (t) {
            PUT(t);
            s = e;
            if (t == T_REM) {
                while (*s) PUT(*s++);
                break;
            }
            if (t == T_DATA) {            // as typed, to the end of the statement
                int q = 0;
                while (*s && (q || *s != ':')) {
                    if (*s == '"') q = !q;
                    PUT(*s++);
                }
            }
            continue;
        }
        PUT(upper(*s));
        s++;
    }
    PUT(0);
    return n - 1;
#undef PUT
}

/* ------------------------------------------------------------------------
 * Variables
 *
 * Simple variables are a table; arrays and strings live in pools that
 * CLEAR empties. A string's text is in the string pool, which is
 * compacted when it fills; strings being worked on within a statement
 * are in a scratch area that's emptied before each statement.
 * --------------------------------------------------------------------- */

enum { REAL, INT, STR };

struct strref { unsigned short off; unsigned char len; unsigned char pad; };

struct var {
    char name[2];
    char type;
    double n;
    struct strref s;
};

#define MAX_VARS 400
HIGH_MEM struct var fp_vars[MAX_VARS];
static int n_vars;

#define MAX_ARRAYS 64
#define MAX_DIMS 8
struct array {
    char name[2];
    char type;
    char ndims;
    int dims[MAX_DIMS];                   // the largest subscript + 1
    long count;
    long off;                             // in the array pool
};
HIGH_MEM struct array fp_arrays[MAX_ARRAYS];
static int n_arrays;

#define ARRAY_POOL (64 * 1024L)
HIGH_MEM unsigned char fp_array_pool[ARRAY_POOL];
static long array_used;

#define STR_POOL 32768
HIGH_MEM unsigned char fp_strings[STR_POOL];
HIGH_MEM unsigned char fp_strings2[STR_POOL];
static int str_used;

#define SCRATCH 16384
HIGH_MEM unsigned char fp_scratch[SCRATCH];
static int scratch_used;

struct fn {
    char name[2];
    char param[2];
    const unsigned char *body;            // in the program
};
#define MAX_FNS 32
static struct fn fns[MAX_FNS];
static int n_fns;

static unsigned char *scratch(int len) {
    if (scratch_used + len > SCRATCH) {
        error(E_COMPLEX);
        return 0;
    }
    unsigned char *p = fp_scratch + scratch_used;
    scratch_used += len;
    return p;
}

static int elem_size(int type) { return type == REAL ? 8 : type == INT ? 2 : 4; }

static void compact_strings(void) {
    int top = 0;
    for (int i = 0; i < n_vars; i++) {
        struct strref *r = &fp_vars[i].s;
        if (fp_vars[i].type != STR || !r->len) continue;
        memcpy(fp_strings2 + top, fp_strings + r->off, r->len);
        r->off = top;
        top += r->len;
    }
    for (int i = 0; i < n_arrays; i++) {
        if (fp_arrays[i].type != STR) continue;
        for (long k = 0; k < fp_arrays[i].count; k++) {
            struct strref r;
            unsigned char *at = fp_array_pool + fp_arrays[i].off + k * 4;
            memcpy(&r, at, sizeof(r));
            if (!r.len) continue;
            memcpy(fp_strings2 + top, fp_strings + r.off, r.len);
            r.off = top;
            top += r.len;
            memcpy(at, &r, sizeof(r));
        }
    }
    memcpy(fp_strings, fp_strings2, top);
    str_used = top;
}

static void str_store(struct strref *r, const unsigned char *s, int len) {
    r->len = 0;
    if (len == 0) return;
    if (str_used + len > STR_POOL) {
        if (s >= fp_strings && s < fp_strings + STR_POOL) {   // (it'll move)
            unsigned char *t = scratch(len);
            if (!t) return;
            memcpy(t, s, len);
            s = t;
        }
        compact_strings();
        if (str_used + len > STR_POOL) {
            error(E_MEMORY);
            return;
        }
    }
    memcpy(fp_strings + str_used, s, len);
    r->off = str_used;
    r->len = len;
    str_used += len;
}

static void clear_vars(void);

/* ------------------------------------------------------------------------
 * Running
 * --------------------------------------------------------------------- */

static const unsigned char *ip;
static int cur_line = -1;                 // offset of the line running, -1: typed
static const unsigned char *stmt_start;   // the statement running (RESUME, errors)
static int jumped, ended;
static const unsigned char *break_ip;     // where CONT goes on from (after BREAK)
static int tracing;

#define STACK 64
struct frame {
    char gosub;                           // a GOSUB (else a FOR)
    int var;                              // FOR: the variable
    double limit, step;
    int line;                             // where to go back to
    const unsigned char *ip;
};
static struct frame stack[STACK];
static int sp;

static int data_line, data_started;       // READ: the line DATA is read from
static const unsigned char *data_ip;      // and where in it (0: find the next DATA)

static struct { int ok, line; const unsigned char *ip; } cont;
static int onerr_line = -1;               // ONERR GOTO's line number, -1 if none
static struct { int line; const unsigned char *ip; } resume;
static unsigned long rnd_seed = 0x1234567;
static double rnd_last;

static void clear_vars(void) {
    n_vars = n_arrays = n_fns = 0;
    array_used = 0;
    str_used = 0;
    sp = 0;
    data_line = 0;
    data_ip = 0;
    data_started = 0;
    onerr_line = -1;
    cont.ok = 0;
}

static int expect(int c) {
    if (*ip != c) {
        error(E_SYNTAX);
        return 0;
    }
    ip++;
    return 1;
}

struct val {
    char str;
    double n;
    const unsigned char *s;
    int len;
};

static void expr(struct val *v);

static double num_expr(void) {
    struct val v;
    expr(&v);
    if (v.str) error(E_TYPE);
    return err != E_NONE ? 0 : v.n;
}

// A number that must be 0-max: ILLEGAL QUANTITY if not.
static int quantity(long max) {
    double x = num_expr();
    if (err != E_NONE) return 0;
    x = num_floor(x);
    if (x < 0 || x > max) {
        error(E_QUANTITY);
        return 0;
    }
    return (int)x;
}

// An address: -65535 to 65535 (negative counts back from 65536).
static unsigned int address(void) {
    double x = num_floor(num_expr());
    if (err != E_NONE) return 0;
    if (x < -65535 || x > 65535) {
        error(E_QUANTITY);
        return 0;
    }
    long a = (long)x;
    return (unsigned int)(a < 0 ? a + 65536 : a) & 0xFFFF;
}

// A name: its two characters that count, and its type ($, % or none).
static int read_name(char name[2], int *type) {
    if (!is_alpha(*ip)) {
        error(E_SYNTAX);
        return 0;
    }
    name[0] = *ip++;
    name[1] = is_alnum(*ip) ? *ip : 0;
    while (is_alnum(*ip)) ip++;
    *type = REAL;
    if (*ip == '$') {
        *type = STR;
        ip++;
    } else if (*ip == '%') {
        *type = INT;
        ip++;
    }
    return 1;
}

static struct var *find_var(const char name[2], int type) {
    for (int i = 0; i < n_vars; i++)
        if (fp_vars[i].name[0] == name[0] && fp_vars[i].name[1] == name[1] &&
            fp_vars[i].type == type)
            return &fp_vars[i];
    if (n_vars == MAX_VARS) {
        error(E_MEMORY);
        return 0;
    }
    struct var *v = &fp_vars[n_vars++];
    v->name[0] = name[0];
    v->name[1] = name[1];
    v->type = type;
    v->n = 0;
    v->s.len = 0;
    return v;
}

static struct array *find_array(const char name[2], int type) {
    for (int i = 0; i < n_arrays; i++)
        if (fp_arrays[i].name[0] == name[0] && fp_arrays[i].name[1] == name[1] &&
            fp_arrays[i].type == type)
            return &fp_arrays[i];
    return 0;
}

static struct array *make_array(const char name[2], int type, int ndims, const int *dims) {
    if (n_arrays == MAX_ARRAYS) {
        error(E_MEMORY);
        return 0;
    }
    long count = 1;
    for (int i = 0; i < ndims; i++) {
        count *= dims[i];
        if (count > ARRAY_POOL) break;
    }
    long size = (count * elem_size(type) + 7) & ~7L;
    if (count > ARRAY_POOL || array_used + size > ARRAY_POOL) {
        error(E_MEMORY);
        return 0;
    }
    struct array *a = &fp_arrays[n_arrays++];
    a->name[0] = name[0];
    a->name[1] = name[1];
    a->type = type;
    a->ndims = ndims;
    for (int i = 0; i < ndims; i++) a->dims[i] = dims[i];
    a->count = count;
    a->off = array_used;
    memset(fp_array_pool + array_used, 0, size);
    array_used += size;
    return a;
}

// The subscripts in brackets (ip at the "("): the element's address.
static unsigned char *element(const char name[2], int type) {
    int subs[MAX_DIMS], n = 0;
    ip++;
    for (;;) {
        double x = num_expr();
        if (err != E_NONE) return 0;
        if (x < 0 || x >= 32768) {
            error(E_QUANTITY);
            return 0;
        }
        if (n == MAX_DIMS) {
            error(E_SUBSCRIPT);
            return 0;
        }
        subs[n++] = (int)x;
        if (*ip == ',') {
            ip++;
            continue;
        }
        if (!expect(')')) return 0;
        break;
    }
    struct array *a = find_array(name, type);
    if (!a) {                             // not DIMmed: 0-10 for each subscript
        int dims[MAX_DIMS];
        for (int i = 0; i < n; i++) dims[i] = 11;
        a = make_array(name, type, n, dims);
        if (!a) return 0;
    }
    if (a->ndims != n) {
        error(E_SUBSCRIPT);
        return 0;
    }
    long index = 0;
    for (int i = 0; i < n; i++) {
        if (subs[i] >= a->dims[i]) {
            error(E_SUBSCRIPT);
            return 0;
        }
        index = index * a->dims[i] + subs[i];
    }
    return fp_array_pool + a->off + index * elem_size(type);
}

// A variable or array element, to read or assign: where its value is.
struct place {
    int type;
    struct var *var;                      // a simple variable, or
    unsigned char *elem;                  // an array element
};

static int place(struct place *p) {
    char name[2];
    int type;
    if (!read_name(name, &type)) return 0;
    p->type = type;
    p->var = 0;
    p->elem = 0;
    if (*ip == '(') {
        p->elem = element(name, type);
        return p->elem != 0;
    }
    p->var = find_var(name, type);
    return p->var != 0;
}

static void get_place(const struct place *p, struct val *v) {
    v->str = p->type == STR;
    if (p->type == STR) {
        struct strref r;
        if (p->var) r = p->var->s;
        else memcpy(&r, p->elem, sizeof(r));
        v->s = fp_strings + r.off;
        v->len = r.len;
    } else if (p->var) {
        v->n = p->var->n;
    } else if (p->type == INT) {
        short i;
        memcpy(&i, p->elem, 2);
        v->n = i;
    } else {
        memcpy(&v->n, p->elem, 8);
    }
}

static void set_place(const struct place *p, const struct val *v) {
    if ((p->type == STR) != v->str) {
        error(E_TYPE);
        return;
    }
    if (p->type == STR) {
        struct strref r;
        str_store(&r, v->s, v->len);
        if (err != E_NONE) return;
        if (p->var) p->var->s = r;
        else memcpy(p->elem, &r, sizeof(r));
        return;
    }
    double x = v->n;
    if (p->type == INT) {
        x = num_floor(x);
        if (x < -32767 || x > 32767) {
            error(E_QUANTITY);
            return;
        }
        if (p->var) {
            p->var->n = x;
        } else {
            short i = (short)x;
            memcpy(p->elem, &i, 2);
        }
        return;
    }
    if (p->var) p->var->n = x;
    else memcpy(p->elem, &x, 8);
}

/* ------------------------------------------------------------------------
 * Expressions
 * --------------------------------------------------------------------- */

static int depth;                         // (FORMULA TOO COMPLEX)

static void make_str(struct val *v, const unsigned char *s, int len) {
    v->str = 1;
    v->s = s;
    v->len = len;
}

static void make_num(struct val *v, double n) {
    v->str = 0;
    v->n = n;
}

static void primary(struct val *v);

static void bracketed(struct val *v) {
    if (!expect('(')) return;
    expr(v);
    expect(')');
}

static double num_arg(void) {
    struct val v;
    bracketed(&v);
    if (v.str) error(E_TYPE);
    return v.n;
}

static void str_arg(struct val *v) {
    expr(v);
    if (!v->str) error(E_TYPE);
}

// A number at s (as VAL and READ read them); how many characters.
static int parse_number(const unsigned char *s, int len, double *x) {
    char buf[64];
    int n = 0;
    for (int i = 0; i < len && n < 63; i++)
        if (s[i] != ' ') buf[n++] = s[i];
    buf[n] = '\0';
    const char *p = buf;
    int neg = 0;
    if (*p == '-' || *p == '+') neg = *p++ == '-';
    if (!num_parse(&p, x)) {
        *x = 0;
        return 0;
    }
    if (neg) *x = -*x;
    return p - buf;
}

static double rnd(double x) {
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

static void call_fn(struct val *v) {
    char name[2];
    int type;
    if (!read_name(name, &type)) return;
    if (type != REAL) {
        error(E_SYNTAX);
        return;
    }
    double arg = num_arg();
    if (err != E_NONE) return;
    struct fn *f = 0;
    for (int i = 0; i < n_fns; i++)
        if (fns[i].name[0] == name[0] && fns[i].name[1] == name[1]) f = &fns[i];
    if (!f) {
        error(E_FN);
        return;
    }
    struct var *p = find_var(f->param, REAL);
    if (!p) return;
    double saved = p->n;
    p->n = arg;
    const unsigned char *back = ip;
    ip = f->body;
    expr(v);
    if (v->str) error(E_TYPE);
    ip = back;
    p = find_var(f->param, REAL);
    if (p) p->n = saved;
}

static void function(int t, struct val *v) {
    double x, y;
    struct val a;
    int n, m;
    switch (t) {
    case T_SGN: x = num_arg(); make_num(v, x > 0 ? 1 : x < 0 ? -1 : 0); return;
    case T_INT: make_num(v, fit(num_floor(num_arg()))); return;
    case T_ABS: make_num(v, num_abs(num_arg())); return;
    case T_FRE:
        bracketed(&a);
        compact_strings();
        make_num(v, (double)(STR_POOL - str_used) + (ARRAY_POOL - array_used) / 4);
        return;
    case T_SCRN:                          // (SCRN( has its bracket)
        x = num_expr();
        expect(',');
        y = num_expr();
        expect(')');
        if (err != E_NONE) return;
        if (x < 0 || x > 39 || y < 0 || y > 47) {
            error(E_QUANTITY);
            return;
        }
        make_num(v, apple_scrn((int)x, (int)y));
        return;
    case T_PDL:
        x = num_arg();
        if (x < 0 || x > 3) error(E_QUANTITY);
        make_num(v, err == E_NONE ? apple_paddle((int)x) : 0);
        return;
    case T_POS: bracketed(&a); make_num(v, column()); return;
    case T_SQR:
        x = num_arg();
        if (x < 0) error(E_QUANTITY);
        make_num(v, err == E_NONE ? fit(num_sqrt(x)) : 0);
        return;
    case T_RND: make_num(v, rnd(num_arg())); return;
    case T_LOG:
        x = num_arg();
        if (x <= 0) error(E_QUANTITY);
        make_num(v, err == E_NONE ? fit(num_ln(x)) : 0);
        return;
    case T_EXP:
        x = num_arg();
        if (x > 88.03) error(E_OVERFLOW);
        make_num(v, err == E_NONE ? fit(num_exp(x)) : 0);
        return;
    case T_COS: make_num(v, fit(num_cos(num_arg()))); return;
    case T_SIN: make_num(v, fit(num_sin(num_arg()))); return;
    case T_TAN:
        x = num_arg();
        if (err == E_NONE && num_cos(x) == 0) error(E_DIV0);
        make_num(v, err == E_NONE ? fit(num_tan(x)) : 0);
        return;
    case T_ATN: make_num(v, fit(num_atan2(num_arg(), 1))); return;
    case T_PEEK: {
        if (!expect('(')) return;
        unsigned int addr = address();
        expect(')');
        make_num(v, err == E_NONE ? apple_peek(addr) : 0);
        return;
    }
    case T_LEN:
        expect('(');
        str_arg(&a);
        expect(')');
        make_num(v, a.len);
        return;
    case T_STR: {
        x = num_arg();
        unsigned char *s = scratch(24);
        if (!s) return;
        format(x, (char *)s);
        make_str(v, s, strlen((char *)s));
        return;
    }
    case T_VAL:
        expect('(');
        str_arg(&a);
        expect(')');
        if (err != E_NONE) return;
        parse_number(a.s, a.len, &x);
        make_num(v, fit(x));
        return;
    case T_ASC:
        expect('(');
        str_arg(&a);
        expect(')');
        if (err == E_NONE && a.len == 0) error(E_QUANTITY);
        make_num(v, err == E_NONE ? a.s[0] : 0);
        return;
    case T_CHR: {
        expect('(');
        n = quantity(255);
        expect(')');
        unsigned char *s = scratch(1);
        if (!s) return;
        s[0] = n;
        make_str(v, s, 1);
        return;
    }
    case T_LEFT:
    case T_RIGHT:
        expect('(');
        str_arg(&a);
        expect(',');
        n = quantity(255);
        expect(')');
        if (err != E_NONE) return;
        if (n > a.len) n = a.len;
        make_str(v, t == T_LEFT ? a.s : a.s + a.len - n, n);
        return;
    case T_MID:
        expect('(');
        str_arg(&a);
        expect(',');
        n = quantity(255);
        m = 255;
        if (*ip == ',') {
            ip++;
            m = quantity(255);
        }
        expect(')');
        if (err != E_NONE) return;
        if (n == 0) {
            error(E_QUANTITY);
            return;
        }
        if (n > a.len) {
            make_str(v, a.s, 0);
            return;
        }
        if (m > a.len - n + 1) m = a.len - n + 1;
        make_str(v, a.s + n - 1, m);
        return;
    case T_FN: call_fn(v); return;
    case T_USR:
        num_arg();
        error(E_QUANTITY);
        return;
    }
    error(E_SYNTAX);
}

static void primary(struct val *v) {
    make_num(v, 0);
    if (err != E_NONE) return;
    int c = *ip;
    if (is_digit(c) || c == '.') {
        const char *p = (const char *)ip;
        double x;
        if (!num_parse(&p, &x)) {
            error(E_SYNTAX);
            return;
        }
        ip = (const unsigned char *)p;
        make_num(v, fit(x));
        return;
    }
    if (c == '"') {
        const unsigned char *s = ++ip;
        while (*ip && *ip != '"') ip++;
        make_str(v, s, ip - s);
        if (*ip == '"') ip++;
        return;
    }
    if (c == '(') {
        bracketed(v);
        return;
    }
    if (is_alpha(c)) {
        struct place p;
        if (!place(&p)) return;
        get_place(&p, v);
        return;
    }
    if (c == '-') {                       // (below ^, as in the original: -2^2 is -4)
        ip++;
        primary(v);
        if (v->str) error(E_TYPE);
        v->n = -v->n;
        return;
    }
    if (c == '+') {
        ip++;
        primary(v);
        return;
    }
    if (c >= 0x80) {
        ip++;
        function(c, v);
        return;
    }
    error(E_SYNTAX);
}

static double power(double a, double b) {
    if (b == num_floor(b) && b >= -64 && b <= 64) {
        int n = (int)(b < 0 ? -b : b);
        double r = 1;
        while (n-- > 0 && err == E_NONE) r = fit(r * a);
        if (b < 0) {
            if (r == 0) {
                error(E_DIV0);
                return 0;
            }
            r = fit(1 / r);
        }
        return r;
    }
    if (a < 0) {
        error(E_QUANTITY);
        return 0;
    }
    if (a == 0) return 0;
    double l = b * num_ln(a);
    if (l > 88.03) {
        error(E_OVERFLOW);
        return 0;
    }
    return fit(num_exp(l));
}

static void pow_level(struct val *v) {
    primary(v);
    while (*ip == '^' && err == E_NONE) {
        ip++;
        struct val b;
        if (*ip == '-') {                 // 2^-1
            ip++;
            primary(&b);
            b.n = -b.n;
        } else {
            primary(&b);
        }
        if (v->str || b.str) {
            error(E_TYPE);
            return;
        }
        v->n = power(v->n, b.n);
    }
}

static void unary(struct val *v) {
    if (*ip == '-') {
        ip++;
        unary(v);
        if (v->str) error(E_TYPE);
        v->n = -v->n;
        return;
    }
    if (*ip == '+') {
        ip++;
        unary(v);
        return;
    }
    pow_level(v);
}

static void mul_level(struct val *v) {
    unary(v);
    while ((*ip == '*' || *ip == '/') && err == E_NONE) {
        int op = *ip++;
        struct val b;
        unary(&b);
        if (v->str || b.str) {
            error(E_TYPE);
            return;
        }
        if (op == '*') {
            v->n = fit(v->n * b.n);
        } else if (b.n == 0) {
            error(E_DIV0);
            return;
        } else {
            v->n = fit(v->n / b.n);
        }
    }
}

static void add_level(struct val *v) {
    mul_level(v);
    while ((*ip == '+' || *ip == '-') && err == E_NONE) {
        int op = *ip++;
        struct val b;
        mul_level(&b);
        if (v->str != b.str || (v->str && op == '-')) {
            error(E_TYPE);
            return;
        }
        if (v->str) {
            if (v->len + b.len > 255) {
                error(E_LONG);
                return;
            }
            unsigned char *s = scratch(v->len + b.len);
            if (!s) return;
            memcpy(s, v->s, v->len);
            memcpy(s + v->len, b.s, b.len);
            make_str(v, s, v->len + b.len);
        } else {
            v->n = fit(op == '+' ? v->n + b.n : v->n - b.n);
        }
    }
}

// A comparison: bits 1 <, 2 =, 4 >; or 0.
static int rel_op(void) {
    int bits = 0;
    for (;;) {
        int c = *ip;
        if (c == '<' && !(bits & 1)) bits |= 1;
        else if (c == '=' && !(bits & 2)) bits |= 2;
        else if (c == '>' && !(bits & 4)) bits |= 4;
        else return bits;
        ip++;
    }
}

static void rel_level(struct val *v) {
    add_level(v);
    for (;;) {
        if (err != E_NONE) return;
        int c = *ip;
        if (c != '<' && c != '=' && c != '>') return;
        int bits = rel_op();
        struct val b;
        add_level(&b);
        if (v->str != b.str) {
            error(E_TYPE);
            return;
        }
        int cmp;
        if (v->str) {
            int n = v->len < b.len ? v->len : b.len;
            cmp = memcmp(v->s, b.s, n);
            if (cmp == 0) cmp = v->len - b.len;
        } else {
            cmp = v->n < b.n ? -1 : v->n > b.n;
        }
        int r = (cmp < 0 && (bits & 1)) || (cmp == 0 && (bits & 2)) || (cmp > 0 && (bits & 4));
        make_num(v, r);
    }
}

static void not_level(struct val *v) {
    if (*ip == T_NOT) {
        ip++;
        not_level(v);
        if (v->str) error(E_TYPE);
        make_num(v, v->n == 0);
        return;
    }
    rel_level(v);
}

static void and_level(struct val *v) {
    not_level(v);
    while (*ip == T_AND && err == E_NONE) {
        ip++;
        struct val b;
        not_level(&b);
        if (v->str || b.str) error(E_TYPE);
        make_num(v, v->n != 0 && b.n != 0);
    }
}

static void expr(struct val *v) {
    if (++depth > 40) {
        error(E_COMPLEX);
        make_num(v, 0);
        depth--;
        return;
    }
    and_level(v);
    while (*ip == T_OR && err == E_NONE) {
        ip++;
        struct val b;
        and_level(&b);
        if (v->str || b.str) error(E_TYPE);
        make_num(v, v->n != 0 || b.n != 0);
    }
    depth--;
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

static int at_end(void) { return *ip == 0 || *ip == ':'; }

static void trace_line(void) {
    if (tracing && cur_line >= 0) {
        out('#');
        out_int(line_num(cur_line));
        out(' ');
    }
}

// A line number written in the program (GOTO 100): -1 if none.
static long line_number(void) {
    if (!is_digit(*ip)) return -1;
    long n = 0;
    while (is_digit(*ip)) {
        n = n * 10 + (*ip++ - '0');
        if (n > 999999) n = 999999;
    }
    return n;
}

static void goto_line(long num) {
    int off = line_find((int)(num > 65535 ? 65535 : num));
    if (num < 0 || num > 63999 || off >= prog_len || line_num(off) != num) {
        error(E_UNDEF);
        return;
    }
    cur_line = off;
    ip = line_text(off);
    jumped = 1;
    trace_line();
}

static void skip_statement(void) {
    int q = 0;
    while (*ip && (q || *ip != ':')) {
        if (*ip == '"') q = !q;
        if (*ip == T_REM && !q) {
            while (*ip) ip++;
            return;
        }
        ip++;
    }
}

static void skip_line(void) {
    while (*ip) ip++;
}

static void let(void) {
    struct place p;
    if (!place(&p)) return;
    if (!expect('=')) return;
    struct val v;
    expr(&v);
    if (err == E_NONE) set_place(&p, &v);
}

static void push_gosub(void) {
    if (sp == STACK) {
        error(E_MEMORY);
        return;
    }
    stack[sp].gosub = 1;
    stack[sp].line = cur_line;
    stack[sp].ip = ip;
    sp++;
}

static void print(void) {
    int newline = 1;
    while (!at_end() && err == E_NONE) {
        int c = *ip;
        if (c == ';') {
            ip++;
            newline = 0;
            continue;
        }
        if (c == ',') {                   // the next of the 16-column fields
            ip++;
            newline = 0;
            int w = apple_peek(ZP_WNDWDTH);
            if (column() >= w - 16) out(13);
            else do out(' '); while (column() % 16);
            continue;
        }
        if (c == T_TAB || c == T_SPC) {
            ip++;
            int n = quantity(255);
            expect(')');
            if (err != E_NONE) return;
            if (c == T_SPC) while (n-- > 0) out(' ');
            else while (column() < n - 1) out(' ');
            newline = 0;
            continue;
        }
        struct val v;
        expr(&v);
        if (err != E_NONE) return;
        if (v.str) for (int i = 0; i < v.len; i++) out(v.s[i]);
        else out_num(v.n);
        newline = 1;
    }
    if (newline && err == E_NONE) out(13);
}

/* INPUT, GET, READ */

HIGH_MEM char fp_input[260];

// (Through a function: see core/mem.h.)
__attribute__((noinline, noclone)) static int first_char(const char *s) { return s[0]; }
static int break_now(void);

// One item of typed input or DATA at *s: a number or string for a
// variable of the type. Returns 0 if it isn't one (?REENTER), and
// moves *s past it (and its comma, if any): *more says there was one.
static int read_item(const char **s, int type, struct val *v, int is_data) {
    const char *p = *s;
    while (*p == ' ') p++;
    const char *start = p, *end;
    if (*p == '"') {
        start = ++p;
        while (*p && *p != '"') p++;
        end = p;
        if (*p == '"') p++;
        while (*p == ' ') p++;
    } else {
        while (*p && *p != ',' && (is_data || *p != ':')) p++;
        end = p;
        if (type == STR && is_data)       // (DATA strings lose their trailing spaces)
            while (end > start && end[-1] == ' ') end--;
    }
    if (*p && *p != ',' && !(!is_data && *p == ':')) return 0;
    if (type == STR) {
        unsigned char *t = scratch(end - start + 1);
        if (!t) return 0;
        memcpy(t, start, end - start);
        make_str(v, t, end - start);
    } else {
        double x;
        int len = end - start, used = parse_number((const unsigned char *)start, len, &x);
        int blank = 1;
        for (const char *q = start; q < end; q++) if (*q != ' ') blank = 0;
        if (!blank && used == 0) return 0;
        // (anything after the number but spaces: not a number)
        int n = 0;
        for (const char *q = start; q < end; q++) if (*q != ' ') n++;
        if (used != n) return 0;
        make_num(v, x);
    }
    *s = *p == ',' ? p + 1 : p;
    return 1;
}

static void input(void) {
    if (cur_line < 0) {
        error(E_DIRECT);
        return;
    }
    const unsigned char *prompt = 0;
    int plen = 0;
    if (*ip == '"') {
        struct val v;
        primary(&v);
        prompt = v.s;
        plen = v.len;
        if (!expect(';')) return;
    }
    const unsigned char *vars = ip;
again:
    ip = vars;
    for (int i = 0; i < plen; i++) out(prompt[i]);
    apple_getln(plen ? 0 : '?', fp_input, 240);
    at_line_start = 1;
    if (first_char(fp_input) == 3) {
        error(E_BREAK);
        return;
    }
    const char *s = fp_input;
    for (;;) {
        struct place p;
        if (!place(&p)) return;
        if (!*s) {
            if (s != fp_input) {          // more needed: ??
                out('?');
                apple_getln('?', fp_input, 240);
                if (first_char(fp_input) == 3) {
                    error(E_BREAK);
                    return;
                }
                s = fp_input;
            }
        }
        struct val v;
        if (!read_item(&s, p.type, &v, 0) || (p.type != STR && !first_char(fp_input))) {
            if (onerr_line >= 0) {
                error(E_REENTER);
                return;
            }
            outs("?REENTER");
            out(13);
            goto again;
        }
        set_place(&p, &v);
        if (err != E_NONE) return;
        if (*ip != ',') break;
        ip++;
    }
    while (*s == ' ') s++;
    if (*s) {
        outs("?EXTRA IGNORED");
        out(13);
    }
}

static void get(void) {
    if (cur_line < 0) {
        error(E_DIRECT);
        return;
    }
    struct place p;
    if (!place(&p)) return;
    int k = apple_wait_key();
    struct val v;
    if (p.type == STR) {
        unsigned char *s = scratch(1);
        if (!s) return;
        s[0] = k;
        make_str(&v, s, 1);
    } else {
        if (k < '0' || k > '9') {
            error(E_SYNTAX);
            return;
        }
        make_num(&v, k - '0');
    }
    set_place(&p, &v);
}

// The next DATA item's text (in data_ip), finding the next DATA statement.
static int next_data(void) {
    for (;;) {
        if (data_ip && *data_ip && *data_ip != ':') return 1;
        // Look on from here for a DATA token, outside strings.
        const unsigned char *q = data_ip;
        if (!data_started) {
            data_started = 1;
            data_line = 0;
            q = 0;
        }
        for (;;) {
            if (data_line >= prog_len) return 0;
            if (!q) q = line_text(data_line);
            int in_str = 0;
            while (*q) {
                if (*q == '"') in_str = !in_str;
                else if (!in_str && *q == T_REM) {
                    while (*q) q++;
                    break;
                } else if (!in_str && *q == T_DATA) {
                    data_ip = q + 1;
                    if (*data_ip == 0 || *data_ip == ':') {   // an empty DATA: ""
                        return 1;
                    }
                    return 1;
                }
                q++;
            }
            data_line = line_next(data_line);
            q = 0;
        }
    }
}

static void read_data(void) {
    for (;;) {
        struct place p;
        if (!place(&p)) return;
        if (!next_data()) {
            error(E_DATA);
            return;
        }
        // (the item, as text: up to the next comma or the statement's end)
        const char *s = (const char *)data_ip;
        char item[256];
        int n = 0, q = 0;
        while (*s && (q || (*s != ':' && *s != ',')) && n < 255) {
            if (*s == '"') q = !q;
            item[n++] = *s++;
        }
        item[n] = '\0';
        data_ip = (const unsigned char *)(*s == ',' ? s + 1 : s);
        if (*s == ',' && (!*data_ip || *data_ip == ':')) {
            // (a trailing comma: one more, empty item; leave it)
        }
        const char *t = item;
        struct val v;
        if (!read_item(&t, p.type, &v, 1)) {
            cur_line = data_line;         // (the error is in the DATA line)
            error(E_SYNTAX);
            return;
        }
        set_place(&p, &v);
        if (err != E_NONE || *ip != ',') return;
        ip++;
    }
}

static void for_loop(void) {
    char name[2];
    int type;
    if (!read_name(name, &type)) return;
    if (type != REAL || *ip == '(') {
        error(E_SYNTAX);
        return;
    }
    if (!expect('=')) return;
    double from = num_expr();
    if (!expect(T_TO)) return;
    double limit = num_expr(), step = 1;
    if (*ip == T_STEP) {
        ip++;
        step = num_expr();
    }
    if (err != E_NONE) return;
    struct var *v = find_var(name, REAL);
    if (!v) return;
    v->n = from;
    int index = v - fp_vars;
    for (int i = sp - 1; i >= 0; i--)     // (a loop with the same variable ends)
        if (!stack[i].gosub && stack[i].var == index) {
            sp = i;
            break;
        }
    if (sp == STACK) {
        error(E_MEMORY);
        return;
    }
    stack[sp].gosub = 0;
    stack[sp].var = index;
    stack[sp].limit = limit;
    stack[sp].step = step;
    stack[sp].line = cur_line;
    stack[sp].ip = ip;
    sp++;
}

static void next(void) {
    for (;;) {
        int i = sp - 1;
        if (is_alpha(*ip)) {
            char name[2];
            int type;
            read_name(name, &type);
            struct var *v = find_var(name, REAL);
            if (!v) return;
            int index = v - fp_vars;
            while (i >= 0 && !stack[i].gosub && stack[i].var != index) i--;
            if (i >= 0 && stack[i].gosub) i = -1;
        } else if (i >= 0 && stack[i].gosub) {
            i = -1;
        }
        if (i < 0) {
            error(E_NEXT);
            return;
        }
        sp = i + 1;
        struct var *v = &fp_vars[stack[i].var];
        v->n = fit(v->n + stack[i].step);
        if (err != E_NONE) return;
        int done = stack[i].step >= 0 ? v->n > stack[i].limit : v->n < stack[i].limit;
        if (!done) {
            cur_line = stack[i].line;
            ip = stack[i].ip;
            jumped = 1;
            return;
        }
        sp = i;
        if (*ip != ',') return;
        ip++;
    }
}

static void dim(void) {
    for (;;) {
        char name[2];
        int type;
        if (!read_name(name, &type)) return;
        if (!expect('(')) return;
        int dims[MAX_DIMS], n = 0;
        for (;;) {
            double x = num_expr();
            if (err != E_NONE) return;
            if (x < 0 || x >= 32767) {
                error(E_QUANTITY);
                return;
            }
            if (n == MAX_DIMS) {
                error(E_SUBSCRIPT);
                return;
            }
            dims[n++] = (int)x + 1;
            if (*ip != ',') break;
            ip++;
        }
        if (!expect(')')) return;
        if (find_array(name, type)) {
            error(E_REDIM);
            return;
        }
        if (!make_array(name, type, n, dims)) return;
        if (*ip != ',') return;
        ip++;
    }
}

static void def_fn(void) {
    if (cur_line < 0) {
        error(E_DIRECT);
        return;
    }
    if (!expect(T_FN)) return;
    char name[2], param[2];
    int type, ptype;
    if (!read_name(name, &type) || type != REAL) {
        error(E_SYNTAX);
        return;
    }
    if (!expect('(')) return;
    if (!read_name(param, &ptype) || ptype != REAL) {
        error(E_SYNTAX);
        return;
    }
    if (!expect(')') || !expect('=')) return;
    struct fn *f = 0;
    for (int i = 0; i < n_fns; i++)
        if (fns[i].name[0] == name[0] && fns[i].name[1] == name[1]) f = &fns[i];
    if (!f) {
        if (n_fns == MAX_FNS) {
            error(E_MEMORY);
            return;
        }
        f = &fns[n_fns++];
    }
    f->name[0] = name[0];
    f->name[1] = name[1];
    f->param[0] = param[0];
    f->param[1] = param[1];
    f->body = ip;
    skip_statement();
}

static void list_program(int from, int to);

static void list_args(void) {
    int from = 0, to = 63999;
    long n = line_number();
    if (n >= 0) from = to = (int)n;
    if (*ip == '-' || *ip == ',') {
        ip++;
        to = 63999;
        n = line_number();
        if (n >= 0) to = (int)n;
    }
    if (!at_end()) {
        error(E_SYNTAX);
        return;
    }
    list_program(from, to);
}

static void run_from(long num) {
    clear_vars();
    apple_poke(216, 0);
    if (prog_len == 0) {
        ended = 1;
        return;
    }
    if (num < 0) {
        cur_line = 0;
        ip = line_text(0);
        jumped = 1;
        trace_line();
    } else {
        goto_line(num);
    }
}

static void plot_args(int *x, int *y, int xmax, int ymax) {
    *x = quantity(xmax);
    expect(',');
    *y = quantity(ymax);
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
    int n = quantity(255), x = -1, y = -1;
    if (*ip == T_AT) {
        ip++;
        plot_args(&x, &y, 279, 191);
    }
    if (err != E_NONE) return;
    if (!apple_draw(n, x, y, xor)) error(E_QUANTITY);
}

static void on_goto(void) {
    int n = quantity(255);
    if (err != E_NONE) return;
    int t = *ip;
    if (t != T_GOTO && t != T_GOSUB) {
        error(E_SYNTAX);
        return;
    }
    ip++;
    for (int i = 1;; i++) {
        long num = line_number();
        if (num < 0) {
            error(E_SYNTAX);
            return;
        }
        if (i == n) {
            skip_statement();             // (where RETURN comes back to)
            if (t == T_GOSUB) push_gosub();
            if (err == E_NONE) goto_line(num);
            return;
        }
        if (*ip != ',') return;           // (n is 0, or beyond the list: on)
        ip++;
    }
}

static void wait_statement(void) {
    unsigned int a = address();
    expect(',');
    int mask = quantity(255), flip = 0;
    if (*ip == ',') {
        ip++;
        flip = quantity(255);
    }
    if (err != E_NONE) return;
    while (!((apple_peek(a) ^ flip) & mask)) {
        if (break_now()) return;
        plat_idle();
    }
}

static void statement(void) {
    if (err != E_NONE) return;
    int t = *ip, n, x, y;
    long num;
    if (is_alpha(t)) {
        let();
        return;
    }
    if (t < 0x80) {
        error(E_SYNTAX);
        return;
    }
    ip++;
    switch (t) {
    case T_LET: let(); return;
    case T_PRINT: print(); return;
    case T_INPUT: input(); return;
    case T_GET: get(); return;
    case T_READ: read_data(); return;
    case T_DATA: skip_statement(); return;
    case T_RESTORE:
        data_started = 0;
        data_ip = 0;
        return;
    case T_IF: {
        struct val v;
        expr(&v);
        if (err != E_NONE) return;
        int truth = v.str ? v.len != 0 : v.n != 0;
        if (*ip == T_THEN) ip++;
        else if (*ip != T_GOTO) {
            error(E_SYNTAX);
            return;
        }
        if (!truth) {
            skip_line();
            return;
        }
        if (is_digit(*ip)) goto_line(line_number());
        else jumped = 1;                  // (go on with the statement after THEN)
        return;
    }
    case T_GOTO:
        num = line_number();
        if (num < 0) error(E_SYNTAX);
        else goto_line(num);
        return;
    case T_GOSUB:
        num = line_number();
        if (num < 0) {
            error(E_SYNTAX);
            return;
        }
        push_gosub();
        if (err == E_NONE) goto_line(num);
        if (err != E_NONE && sp) sp--;
        return;
    case T_RETURN:
    case T_POP:
        while (sp > 0 && !stack[sp - 1].gosub) sp--;
        if (sp == 0) {
            error(E_RETURN);
            return;
        }
        sp--;
        if (t == T_RETURN) {
            cur_line = stack[sp].line;
            ip = stack[sp].ip;
            jumped = 1;
        }
        return;
    case T_ON: on_goto(); return;
    case T_ONERR:
        if (!expect(T_GOTO)) return;
        num = line_number();
        if (num < 0) {
            error(E_SYNTAX);
            return;
        }
        onerr_line = (int)num;
        apple_poke(216, 0x80);
        skip_line();                      // (the rest of the line: not run)
        return;
    case T_RESUME:
        if (!resume.ip) {
            error(E_SYNTAX);
            return;
        }
        cur_line = resume.line;
        ip = resume.ip;
        jumped = 1;
        return;
    case T_FOR: for_loop(); return;
    case T_NEXT: next(); return;
    case T_END: ended = 1; return;
    case T_STOP:
        break_ip = ip;
        error(E_BREAK);
        return;
    case T_REM: skip_line(); return;
    case T_DIM: dim(); return;
    case T_DEF: def_fn(); return;
    case T_POKE: {
        unsigned int a = address();
        expect(',');
        n = quantity(255);
        if (err == E_NONE) apple_poke(a, n);
        return;
    }
    case T_CALL: {
        unsigned int a = address();
        if (err != E_NONE) return;
        if (a == 0xF3F2) apple_hclear(0);         // 62450: clear the hi-res page
        else if (a == 0xF3F6) apple_hclear(1);    // 62454: fill it with HCOLOR
        else if (a == 0xF3D8) apple_hgr(1);       // 62424: HGR2
        else if (a == 0xF3E2) apple_hgr(0);       // 62434: HGR
        else apple_call(a);                       // (machine code: not run)
        return;
    }
    case T_WAIT: wait_statement(); return;
    case T_HOME: apple_home(); return;
    case T_NORMAL: apple_poke(ZP_INVFLG, 0xFF); return;
    case T_INVERSE: apple_poke(ZP_INVFLG, 0x3F); return;
    case T_FLASH: apple_poke(ZP_INVFLG, 0x7F); return;
    case T_HTAB: {
        n = quantity(255);
        if (err != E_NONE) return;
        if (n == 0) {
            error(E_QUANTITY);
            return;
        }
        int w = apple_peek(ZP_WNDWDTH);
        apple_htab((n - 1) % (w ? w : 40));
        return;
    }
    case T_VTAB:
        n = quantity(24);
        if (err == E_NONE && n == 0) error(E_QUANTITY);
        if (err == E_NONE) apple_vtab(n - 1);
        return;
    case T_SPEED:
        n = quantity(255);
        if (err == E_NONE) apple_poke(241, (256 - n) & 0xFF);
        return;
    case T_GR: apple_gr(); return;
    case T_TEXT: apple_text(); return;
    case T_COLOR:
        n = quantity(255);
        if (err == E_NONE) apple_color(n);
        return;
    case T_PLOT:
        plot_args(&x, &y, 39, 47);
        if (err == E_NONE) apple_plot(x, y);
        return;
    case T_HLIN:
    case T_VLIN: {
        int top = t == T_HLIN ? 39 : 47;
        int a = quantity(top);
        expect(',');
        int b = quantity(top);
        expect(T_AT);
        n = quantity(86 - top);
        if (err != E_NONE) return;
        if (t == T_HLIN) apple_hlin(a, b, n);
        else apple_vlin(a, b, n);
        return;
    }
    case T_HGR: apple_hgr(0); return;
    case T_HGR2: apple_hgr(1); return;
    case T_HCOLOR:
        n = quantity(7);
        if (err == E_NONE) apple_hcolor(n);
        return;
    case T_HPLOT: hplot(); return;
    case T_DRAW: draw(0); return;
    case T_XDRAW: draw(1); return;
    case T_ROT:
        n = quantity(255);
        if (err == E_NONE) apple_rot(n);
        return;
    case T_SCALE:
        n = quantity(255);
        if (err == E_NONE) apple_scale(n);
        return;
    case T_TRACE: tracing = 1; return;
    case T_NOTRACE: tracing = 0; return;
    case T_HIMEM:
    case T_LOMEM: {
        unsigned int a = address();
        if (err != E_NONE) return;
        apple_poke(t == T_HIMEM ? 115 : 105, a & 0xFF);
        apple_poke(t == T_HIMEM ? 116 : 106, a >> 8);
        clear_vars();
        return;
    }
    case T_PR:
        n = quantity(7);
        if (err != E_NONE) return;
        if (n == 3) apple_80col(1);
        return;
    case T_IN:
        quantity(7);
        return;
    case T_CLEAR: clear_vars(); return;
    case T_NEW:
        prog_len = 0;
        clear_vars();
        ended = 1;
        return;
    case T_RUN:
        num = line_number();
        if (!at_end() && num < 0) {
            error(E_SYNTAX);
            return;
        }
        run_from(num);
        return;
    case T_LIST: list_args(); return;
    case T_DEL: {
        num = line_number();
        long to = -1;
        if (*ip == ',') {
            ip++;
            to = line_number();
        }
        if (num < 0 || to < 0) {
            error(E_SYNTAX);
            return;
        }
        for (int off = line_find((int)num); off < prog_len && line_num(off) <= to;)
            line_delete(line_num(off));
        ended = 1;
        return;
    }
    case T_CONT:
        if (!cont.ok) {
            error(E_CONT);
            return;
        }
        cur_line = cont.line;
        ip = cont.ip;
        jumped = 1;
        cont.ok = 0;
        return;
    case T_LOAD:
    case T_SAVE:
    case T_SHLOAD:
    case T_STORE:
    case T_RECALL:
        skip_statement();                 // (cassette tape: see the disk commands)
        return;
    }
    error(E_SYNTAX);
}

/* ------------------------------------------------------------------------
 * Running lines, errors, BREAK
 * --------------------------------------------------------------------- */

static int poll_count;

static int break_now(void) {
    if (apple_key_peek() == 3) {
        apple_key();
        break_ip = ip;
        error(E_BREAK);
        return 1;
    }
    return 0;
}

static void report(void) {
    int line = cur_line >= 0 ? line_num(cur_line) : -1;
    fresh_line();
    if (err == E_BREAK) {
        outs("BREAK");
        if (line >= 0) {
            cont.ok = 1;
            cont.line = cur_line;
            cont.ip = break_ip ? break_ip : stmt_start;
        }
    } else {
        apple_bell();
        if (dos_err) outs(dos_message(dos_err));
        else {
            out('?');
            outs(error_text(err));
            outs(" ERROR");
        }
    }
    if (line >= 0 && !dos_err) {
        outs(" IN ");
        out_int(line);
    }
    out(13);
}

// An error while running: to ONERR's line, if there is one (returns 1).
static int catch_error(void) {
    if (onerr_line < 0 || !(apple_peek(216) & 0x80) || cur_line < 0) return 0;
    int code = dos_err ? dos_err : err;
    apple_poke(222, code);
    int line = line_num(cur_line);
    apple_poke(218, line & 0xFF);
    apple_poke(219, line >> 8);
    resume.line = cur_line;
    resume.ip = stmt_start;
    err = E_NONE;
    dos_err = 0;
    goto_line(onerr_line);
    return err == E_NONE;
}

static void execute(void) {
    ended = 0;
    while (!ended) {
        if (err != E_NONE) {
            if (catch_error()) continue;
            break;
        }
        if (*ip == ':') {
            ip++;
            continue;
        }
        if (*ip == 0) {
            if (cur_line < 0) break;
            int next_off = line_next(cur_line);
            if (next_off >= prog_len) break;   // (the end: no error)
            cur_line = next_off;
            ip = line_text(next_off);
            trace_line();
            continue;
        }
        scratch_used = 0;
        depth = 0;
        jumped = 0;
        stmt_start = ip;
        break_ip = 0;
        statement();
        if (err != E_NONE) continue;
        if (ended) break;
        if (!jumped && !at_end()) {
            error(E_SYNTAX);
            continue;
        }
        if (++poll_count >= 16) {
            poll_count = 0;
            break_now();
        }
    }
    if (err != E_NONE) report();
    if (capturing) capturing = 0;
    err = E_NONE;
    dos_err = 0;
    cur_line = -1;
}

/* ------------------------------------------------------------------------
 * Disk commands from programs (PRINT CHR$(4);"...")
 * --------------------------------------------------------------------- */

static void run_loaded(void);

static void disk_command(void) {
    int e = dos_command(command, cur_line < 0);
    if (e == DOS_NONE) e = DOS_SYNTAX;
    if (e == DOS_RUN) {
        run_loaded();
        return;
    }
    if (e != DOS_OK) {
        dos_err = e;
        error(E_SYNTAX);
    }
}

/* ------------------------------------------------------------------------
 * Entering lines, LIST
 * --------------------------------------------------------------------- */

HIGH_MEM unsigned char fp_typed[MAX_LINE + 2];

static void enter_line(const char *text) {
    while (*text == ' ') text++;
    if (is_digit(*text)) {
        long num = 0;
        while (is_digit(*text)) {
            num = num * 10 + (*text++ - '0');
            if (num > 999999) num = 999999;
        }
        if (num > 63999) {
            fresh_line();
            apple_bell();
            outs("?SYNTAX ERROR");
            out(13);
            return;
        }
        clear_vars();
        int n = crunch(text, fp_typed, MAX_LINE);
        if (n < 0) {
            fresh_line();
            outs("?STRING TOO LONG ERROR");   // (hmm: the line is too long)
            out(13);
            return;
        }
        if (n == 0) {
            line_delete((int)num);
            return;
        }
        if (!line_insert((int)num, fp_typed, n + 1)) {
            fresh_line();
            apple_bell();
            outs("?OUT OF MEMORY ERROR");
            out(13);
        }
        return;
    }
    if (crunch(text, fp_typed, MAX_LINE) < 0) return;
    while (sp > 0 && stack[sp - 1].line < 0) sp--;   // (loops on the typed line before)
    cur_line = -1;
    ip = fp_typed;
    execute();
}

static int is_operator(int c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '<' || c == '=' ||
           c == '>';
}

// The line as LIST shows it: the number, then a space before and after
// each keyword and operator.
static int detokenize(int off, char *buf) {
    char *p = buf;
    char num[8];
    itoa(line_num(off), num);
    for (char *q = num; *q; q++) *p++ = *q;
    *p++ = ' ';
    const unsigned char *t = line_text(off);
    int in_str = 0, raw = 0;
    for (; *t; t++) {
        int c = *t;
        if (c == '"') in_str = !in_str;
        if (raw == 1 || in_str || c == '"') {
            *p++ = c;
            continue;
        }
        if (raw == 2) {                   // DATA: as typed, to the end of the statement
            if (c == ':') raw = 0;
            else {
                *p++ = c;
                continue;
            }
        }
        if (c >= 0x80) {
            *p++ = ' ';
            for (const char *w = keywords[c - 0x80]; *w; w++) *p++ = *w;
            *p++ = ' ';
            if (c == T_REM) raw = 1;
            if (c == T_DATA) raw = 2;
        } else if (is_operator(c)) {
            *p++ = ' ';
            *p++ = c;
            *p++ = ' ';
        } else {
            *p++ = c;
        }
    }
    *p = '\0';
    return p - buf;
}

HIGH_MEM char fp_list[MAX_LINE * 8];

static void list_program(int from, int to) {
    for (int off = line_find(from); off < prog_len && line_num(off) <= to; off = line_next(off)) {
        if (apple_key_peek() == 3) {
            apple_key();
            break;
        }
        detokenize(off, fp_list);
        fresh_line();
        outs(fp_list);
        out(13);
    }
}

/* ------------------------------------------------------------------------
 * The disk commands' side (dos.h)
 * --------------------------------------------------------------------- */

static int dos_listing(char *buf, int cap) {
    int n = 0;
    for (int off = 0; off < prog_len; off = line_next(off)) {
        int len = detokenize(off, fp_list);
        if (n + len + 1 > cap) return -1;
        memcpy(buf + n, fp_list, len);
        n += len;
        buf[n++] = '\n';
    }
    return n;
}

static int dos_enter(const char *text, int len) {
    prog_len = 0;
    clear_vars();
    char line[256];
    int i = 0;
    while (i < len) {
        int n = 0;
        while (i < len && text[i] != '\n' && text[i] != '\r') {
            if (n < 255) line[n++] = text[i];
            i++;
        }
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        line[n] = '\0';
        const char *p = line;
        while (*p == ' ') p++;
        if (is_digit(*p)) enter_line(line);
    }
    return DOS_OK;
}

static const struct dos_basic dos_side = { 'A', dos_listing, dos_enter };

// RUN name, from the prompt or a program: the program it loaded.
static void run_loaded(void) {
    err = E_NONE;
    run_from(-1);
    jumped = 1;
}

/* ------------------------------------------------------------------------ */

HIGH_MEM char fp_line[260];

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
        apple_getln(']', fp_line, 240);
        at_line_start = 1;
        if (first_char(fp_line) == 3) continue;
        int d = dos_command(fp_line, 1);
        if (d == DOS_NONE) {
            enter_line(fp_line);
        } else if (d == DOS_RUN) {
            run_from(-1);
            if (!ended && err == E_NONE) execute();
            err = E_NONE;
            cur_line = -1;
        } else if (d != DOS_OK) {
            fresh_line();
            apple_bell();
            outs(dos_message(d));
            out(13);
        }
    }
}
