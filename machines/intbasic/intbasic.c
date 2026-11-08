/*
 * Integer BASIC, as on the 1976 terminal computer and the 1977 colour
 * one (intbasic.h). Written for this project from how it behaves; none
 * of it comes from the original's code.
 *
 * Numbers are 16-bit integers, -32767 to 32767; going outside that is
 * an error (*** >32767 ERR). Variable names are a letter then letters
 * and digits, all of them significant; string names end in $. Strings
 * are DIMmed with the longest they may be (DIM A$(20)), and parts of
 * them are A$(I) (from I on) and A$(I,J); there's no + for strings:
 * A$(LEN(A$)+1)="MORE" adds to the end. Arrays have one subscript,
 * from 0.
 *
 * Lines are checked as they're typed (*** SYNTAX ERR, and the line
 * isn't kept), and kept "crunched": the keywords as single bytes, the
 * spaces taken out. LIST puts spaces back in its own way.
 *
 * Some things programs depend on:
 *   - IF ... THEN statement: when the condition is false only that one
 *     statement is skipped; statements after it on the line (after a :)
 *     still run.
 *   - # and <> both mean "not equal". A comparison is 1 or 0.
 *   - A program has to END: running off the end is *** NO END ERR.
 *   - GOSUBs and FOR loops nest 8 deep at most.
 *   - INPUT of a number takes an expression (typing A gives A's
 *     value); a string INPUT shows no "?".
 *
 * Errors are "*** NAME ERR", then "STOPPED AT n" if a program was
 * running. Ctrl+C stops a program (STOPPED AT n); CON carries on.
 */
#include "intbasic.h"
#include "apple.h"
#include "dos.h"
#include "string.h"
#include "mem.h"

static int setting;                       // INT_1976, INT_1977

/* ------------------------------------------------------------------------
 * Keywords
 * --------------------------------------------------------------------- */

enum { K_STMT = 1, K_FUNC = 2, K_OP = 4, K_1977 = 8 };   // (K_1977: not on the terminal)

static const struct { const char *name; unsigned char kind; } keywords[] = {
    { "LET", K_STMT }, { "PRINT", K_STMT }, { "INPUT", K_STMT }, { "IF", K_STMT },
    { "THEN", K_OP }, { "GOTO", K_STMT }, { "GOSUB", K_STMT }, { "RETURN", K_STMT },
    { "FOR", K_STMT }, { "TO", K_OP }, { "STEP", K_OP }, { "NEXT", K_STMT },
    { "END", K_STMT }, { "REM", K_STMT }, { "DIM", K_STMT }, { "POKE", K_STMT },
    { "CALL", K_STMT }, { "TAB", K_STMT }, { "VTAB", K_STMT | K_1977 },
    { "GR", K_STMT | K_1977 }, { "COLOR=", K_STMT | K_1977 }, { "PLOT", K_STMT | K_1977 },
    { "HLIN", K_STMT | K_1977 }, { "VLIN", K_STMT | K_1977 }, { "AT", K_OP | K_1977 },
    { "TEXT", K_STMT | K_1977 }, { "POP", K_STMT | K_1977 }, { "DSP", K_STMT | K_1977 },
    { "TRACE", K_STMT | K_1977 }, { "NOTRACE", K_STMT | K_1977 }, { "CLR", K_STMT },
    { "NEW", K_STMT }, { "RUN", K_STMT }, { "LIST", K_STMT }, { "DEL", K_STMT },
    { "AUTO", K_STMT }, { "MAN", K_STMT }, { "CON", K_STMT }, { "HIMEM:", K_STMT },
    { "LOMEM:", K_STMT }, { "LOAD", K_STMT }, { "SAVE", K_STMT }, { "PR#", K_STMT },
    { "IN#", K_STMT }, { "AND", K_OP }, { "OR", K_OP }, { "NOT", K_OP }, { "MOD", K_OP },
    { "ABS", K_FUNC }, { "SGN", K_FUNC }, { "RND", K_FUNC }, { "PEEK", K_FUNC },
    { "PDL", K_FUNC | K_1977 }, { "LEN", K_FUNC }, { "ASC", K_FUNC | K_1977 },
    { "SCRN", K_FUNC | K_1977 },
};
#define N_KEYWORDS ((int)(sizeof(keywords) / sizeof(keywords[0])))

enum {
    T_LET = 0x80, T_PRINT, T_INPUT, T_IF, T_THEN, T_GOTO, T_GOSUB, T_RETURN, T_FOR, T_TO,
    T_STEP, T_NEXT, T_END, T_REM, T_DIM, T_POKE, T_CALL, T_TAB, T_VTAB, T_GR, T_COLOR,
    T_PLOT, T_HLIN, T_VLIN, T_AT, T_TEXT, T_POP, T_DSP, T_TRACE, T_NOTRACE, T_CLR, T_NEW,
    T_RUN, T_LIST, T_DEL, T_AUTO, T_MAN, T_CON, T_HIMEM, T_LOMEM, T_LOAD, T_SAVE, T_PR, T_IN,
    T_AND, T_OR, T_NOT, T_MOD, T_ABS, T_SGN, T_RND, T_PEEK, T_PDL, T_LEN, T_ASC, T_SCRN,
};

static const char *kw_name(int t) { return keywords[t - 0x80].name; }
static int kw_kind(int t) { return keywords[t - 0x80].kind; }

/* ------------------------------------------------------------------------
 * Errors
 * --------------------------------------------------------------------- */

static const char *err;                   // the error, once there is one
#define E_SYNTAX   "SYNTAX"
#define E_32767    ">32767"
#define E_255      ">255"
#define E_RANGE    "RANGE"
#define E_DIM      "DIM"
#define E_STR      "STR OVFL"
#define E_NO_END   "NO END"
#define E_BRANCH   "BAD BRANCH"
#define E_GOSUBS   ">8 GOSUBS"
#define E_FORS     ">8 FORS"
#define E_RETURN   "BAD RETURN"
#define E_NEXT     "BAD NEXT"
#define E_MEM      "MEM FULL"
#define E_LONG     "TOO LONG"

static void error(const char *e) {
    if (!err) err = e;
}

static void dos_done(int result);
static int dos_err;                       // a disk error (its DOS number), or 0
#define E_DOS "DOS"

// What programs print: disk commands (Ctrl+D) and text files go to dos.c.
static void out(int c) {
    if (dos_print(c)) {
        int r = dos_result();
        if (r != DOS_NONE) dos_done(r);
        return;
    }
    apple_cout(c);
}
static void outs(const char *s) {
    while (*s) out(*s++);
}
static void out_num(int n) {
    char b[8];
    itoa(n, b);
    outs(b);
}
static void fresh_line(void) {
    if (apple_peek(ZP_CH) != 0) out(13);
}

/* ------------------------------------------------------------------------
 * The program: lines in order, each the line number (2 bytes), the
 * length of what follows (1 byte), and the crunched text, ending in 0.
 * --------------------------------------------------------------------- */

#define PROG_SIZE 32768
#define MAX_TOKENS 250
HIGH_MEM unsigned char ib_prog[PROG_SIZE];
static int prog_len;

static int line_num(int off) { return ib_prog[off] | (ib_prog[off + 1] << 8); }
static int line_next(int off) { return off + 3 + ib_prog[off + 2]; }
static unsigned char *line_text(int off) { return ib_prog + off + 3; }

static int line_find(int num) {           // the first line >= num
    int off = 0;
    while (off < prog_len && line_num(off) < num) off = line_next(off);
    return off;
}

static void line_delete(int num) {
    int off = line_find(num);
    if (off < prog_len && line_num(off) == num) {
        int next = line_next(off);
        memmove(ib_prog + off, ib_prog + next, prog_len - next);
        prog_len -= next - off;
    }
}

static int line_insert(int num, const unsigned char *text, int len) {
    line_delete(num);
    if (prog_len + len + 3 > PROG_SIZE) return 0;
    int off = line_find(num);
    memmove(ib_prog + off + len + 3, ib_prog + off, prog_len - off);
    ib_prog[off] = num & 0xFF;
    ib_prog[off + 1] = num >> 8;
    ib_prog[off + 2] = len;
    memcpy(ib_prog + off + 3, text, len);
    prog_len += len + 3;
    return 1;
}

/* ------------------------------------------------------------------------
 * Crunching a typed line: keywords to bytes, spaces out.
 *
 * Whether letters are a keyword or a name depends on where they are:
 * where a statement starts, a statement's keyword; after a value (a
 * name, number, string or ")") an operator's (AND, TO, THEN ...);
 * elsewhere a function's (followed by "(") or NOT. So SCORE and TOTAL
 * are names. A name followed by = or ( where a statement starts is
 * being assigned to (PRINTER=1), unless the keyword includes the = (as
 * COLOR= does). If the line doesn't make sense that way (IFA=BTHEN10),
 * it's crunched again with operators' keywords taken out of the middle
 * of names too.
 * --------------------------------------------------------------------- */

static int is_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
static int is_alnum(int c) { return is_alpha(c) || (c >= '0' && c <= '9'); }
static int is_digit(int c) { return c >= '0' && c <= '9'; }

// The keyword of one of the kinds at s, or 0; *end is just after it.
static int match_keyword(const char *s, int kinds, const char **end) {
    int best = 0, best_len = 0;
    for (int i = 0; i < N_KEYWORDS; i++) {
        int k = keywords[i].kind;
        if (!(k & kinds) || ((k & K_1977) && setting == INT_1976)) continue;
        const char *p = s, *w = keywords[i].name;
        while (*w) {
            while (*p == ' ' && w != keywords[i].name) p++;
            if (toupper(*p) != *w) break;
            p++, w++;
        }
        if (*w) continue;
        if (k & K_FUNC) {                 // (a function only with its bracket)
            const char *q = p;
            while (*q == ' ') q++;
            if (*q != '(') continue;
        }
        int len = w - keywords[i].name;
        if (len > best_len) {
            best = 0x80 + i;
            best_len = len;
            *end = p;
        }
    }
    return best;
}

// Crunch src into out (at most cap bytes, then a 0); its length, or -1.
static int crunch(const char *src, unsigned char *out_buf, int cap, int strict) {
    const char *s = src;
    int n = 0, start = 1, value = 0;      // value: the last thing was a value
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
            start = 0;
            value = 1;
            continue;
        }
        const char *e;
        int t = 0;
        if (is_alpha(*s)) {
            if (start) {
                t = match_keyword(s, K_STMT, &e);
                if (t && !strict) {
                    const char *r = s;    // a name being assigned to?
                    while (is_alnum(*r)) r++;
                    const char *q = r;
                    if (*q == '$') q++;
                    while (*q == ' ') q++;
                    if ((*q == '=' || *q == '(') && e <= r) t = 0;
                }
            } else if (value) {
                t = match_keyword(s, K_OP, &e);
                if (t == T_NOT) t = 0;
            } else {
                t = match_keyword(s, K_FUNC | K_OP, &e);
                if (t && (kw_kind(t) & K_OP) && t != T_NOT) t = 0;
            }
        }
        if (t) {
            PUT(t);
            s = e;
            start = t == T_THEN;
            value = 0;
            if (t == T_REM) {             // the rest as it is
                while (*s) PUT(*s++);
                break;
            }
            continue;
        }
        start = 0;
        if (is_alpha(*s)) {               // a name
            const char *first = s;
            while (is_alnum(*s)) {
                if (strict && s != first && match_keyword(s, K_OP, &e) &&
                    match_keyword(s, K_OP, &e) != T_NOT)
                    break;
                PUT(toupper(*s));
                s++;
            }
            if (*s == '$') PUT(*s++);
            value = 1;
        } else if (is_digit(*s)) {
            while (is_digit(*s)) PUT(*s++);
            value = 1;
        } else {
            int c = *s++;
            PUT(c);
            value = c == ')';
            if (c == ':') start = 1;
        }
    }
    PUT(0);
    return n - 1;
#undef PUT
}

/* ------------------------------------------------------------------------
 * Variables, in a heap that only grows (until CLR, RUN or NEW): each a
 * record of its kind, its name, and its value.
 *
 *   number:  kind 1, name, value (2 bytes)
 *   string:  kind 2, name, the longest it may be, its length, the text
 *   array:   kind 3, name, the number of elements (2 bytes), the values
 * --------------------------------------------------------------------- */

enum { V_NUM = 1, V_STR = 2, V_ARRAY = 3 };
#define HEAP_SIZE 49152
HIGH_MEM unsigned char ib_heap[HEAP_SIZE];
static int heap_len;

static int get16(int at) { return (short)(ib_heap[at] | (ib_heap[at + 1] << 8)); }
static void set16(int at, int v) {
    ib_heap[at] = v & 0xFF;
    ib_heap[at + 1] = (v >> 8) & 0xFF;
}

static int record_size(int at) {
    int data = at + 2 + ib_heap[at + 1];
    switch (ib_heap[at]) {
    case V_NUM: return data + 2 - at;
    case V_STR: return data + 2 + ib_heap[data] - at;
    default:    return data + 2 + 2 * (get16(data) & 0xFFFF) - at;
    }
}

// The record for the name (its data: after the name), or -1.
static int var_find(int kind, const unsigned char *name, int len) {
    for (int at = 0; at < heap_len; at += record_size(at))
        if (ib_heap[at] == kind && ib_heap[at + 1] == len && !memcmp(ib_heap + at + 2, name, len))
            return at + 2 + len;
    return -1;
}

static int var_new(int kind, const unsigned char *name, int len, int size) {
    int need = 2 + len + size;
    if (heap_len + need > HEAP_SIZE) {
        error(E_MEM);
        return -1;
    }
    int at = heap_len;
    ib_heap[at] = kind;
    ib_heap[at + 1] = len;
    memcpy(ib_heap + at + 2, name, len);
    memset(ib_heap + at + 2 + len, 0, size);
    heap_len += need;
    return at + 2 + len;
}

static int new_string(const unsigned char *name, int len, int cap) {
    int d = var_new(V_STR, name, len, 2 + cap);
    if (d >= 0) ib_heap[d] = cap;
    return d;
}

static int new_array(const unsigned char *name, int len, int count) {
    if (count * 2 + heap_len + 8 > HEAP_SIZE) {
        error(E_MEM);
        return -1;
    }
    int d = var_new(V_ARRAY, name, len, 2 + 2 * count);
    if (d >= 0) set16(d, count);
    return d;
}

/* ------------------------------------------------------------------------
 * Running
 * --------------------------------------------------------------------- */

static const unsigned char *ip;           // where we are in the crunched text
static int cur_line = -1;                 // its line's offset, or -1: a typed line
static int checking;                      // just checking the syntax, not running
static int jumped;                        // the statement moved ip (GOTO ...)
static int ended;                         // END, STOP, Ctrl+C, NEW ...
static int tracing;
HIGH_MEM unsigned char ib_typed[MAX_TOKENS + 2];

static struct { int line; const unsigned char *ip; } gosubs[8];
static int n_gosubs;
static struct { int var, limit, step, line; const unsigned char *ip; } fors[8];
static int n_fors;
static struct { int line; const unsigned char *ip; int ok; } cont;   // CON

#define MAX_DSP 8
static int dsp_vars[MAX_DSP], n_dsp;

static int auto_on, auto_line, auto_step;
static unsigned int rnd_state = 12345;

static void clear_vars(void) {
    heap_len = 0;
    n_gosubs = n_fors = n_dsp = 0;
    cont.ok = 0;
}

static int at_end(void) { return *ip == 0 || *ip == ':'; }

static int expect(int c) {
    if (*ip != c) {
        error(E_SYNTAX);
        return 0;
    }
    ip++;
    return 1;
}

// A name at ip: its start and length (0 if there isn't one).
static int read_name(const unsigned char **name) {
    *name = ip;
    if (!is_alpha(*ip)) return 0;
    while (is_alnum(*ip)) ip++;
    return ip - *name;
}

// Does a string start at p (a "string" or a name ending in $)?
static int string_at(const unsigned char *p) {
    if (*p == '"') return 1;
    if (!is_alpha(*p)) return 0;
    while (is_alnum(*p)) p++;
    return *p == '$';
}

static int check_range(long v) {
    if (v > 32767 || v < -32767) {
        error(E_32767);
        return 0;
    }
    return (int)v;
}

static int expr(void);

static void seed_rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; }

// A string: "text", A$, A$(I) or A$(I,J). *s and *len: its text.
static void string_expr(const unsigned char **s, int *len) {
    static const unsigned char empty[1] = { 0 };
    *s = empty;
    *len = 0;
    if (*ip == '"') {
        const unsigned char *p = ++ip;
        while (*ip && *ip != '"') ip++;
        *s = p;
        *len = ip - p;
        if (*ip == '"') ip++;
        return;
    }
    const unsigned char *name;
    int nlen = read_name(&name);
    if (!nlen || !expect('$')) {
        error(E_SYNTAX);
        return;
    }
    int from = 1, to = -1;
    if (*ip == '(') {
        ip++;
        from = expr();
        if (*ip == ',') {
            ip++;
            to = expr();
        }
        expect(')');
    }
    if (err || checking) return;
    int d = var_find(V_STR, name, nlen);
    int slen = d >= 0 ? ib_heap[d + 1] : 0;
    if (to < 0) to = slen;
    if (from < 1 || to > slen || from > to + 1) {
        error(E_RANGE);
        return;
    }
    if (d >= 0) *s = ib_heap + d + 2 + from - 1;
    *len = to - from + 1;
}

static int compare(int op, int c) {
    switch (op) {
    case '=': return c == 0;
    case '#': return c != 0;
    case '<': return c < 0;
    case '>': return c > 0;
    case 'l': return c <= 0;
    case 'g': return c >= 0;
    }
    return 0;
}

// A comparison operator at ip (= # <> < > <= >=), as a character
// ('l' for <=, 'g' for >=), or 0.
static int rel_op(void) {
    int c = *ip;
    if (c == '=' || c == '#') {
        ip++;
        return c;
    }
    if (c == '<' || c == '>') {
        ip++;
        if (c == '<' && *ip == '>') return ip++, '#';
        if (*ip == '=') return ip++, c == '<' ? 'l' : 'g';
        if (c == '>' && *ip == '<') return ip++, '#';
        return c;
    }
    return 0;
}

static int primary(void);

static int factor(void) {                 // NOT, unary - and +
    if (*ip == '-') {
        ip++;
        return -factor();
    }
    if (*ip == '+') {
        ip++;
        return factor();
    }
    if (*ip == T_NOT) {
        ip++;
        return !factor();
    }
    return primary();
}

static int power(void) {
    int a = factor();
    while (*ip == '^' && !err) {
        ip++;
        int b = factor();
        if (checking || err) continue;
        if (b < 0) {
            error(E_RANGE);
            return 0;
        }
        long r = 1;
        while (b-- > 0 && !err) r = check_range(r * a);
        a = (int)r;
    }
    return a;
}

static int term(void) {
    int a = power();
    for (;;) {
        int op = *ip;
        if (op != '*' && op != '/' && op != T_MOD) return a;
        ip++;
        int b = power();
        if (checking || err) continue;
        if (op == '*') {
            a = check_range((long)a * b);
        } else if (b == 0) {
            error(E_32767);
            return 0;
        } else {
            a = op == '/' ? a / b : a % b;
        }
    }
}

static int sum(void) {
    int a = term();
    for (;;) {
        int op = *ip;
        if (op != '+' && op != '-') return a;
        ip++;
        int b = term();
        if (checking || err) continue;
        a = check_range(op == '+' ? (long)a + b : (long)a - b);
    }
}

static int relation(void) {
    if (string_at(ip)) {                  // strings: compared, 1 or 0
        const unsigned char *s1, *s2;
        int l1, l2;
        string_expr(&s1, &l1);
        int op = rel_op();
        if (!op) {
            error(E_SYNTAX);
            return 0;
        }
        string_expr(&s2, &l2);
        if (checking || err) return 0;
        int n = l1 < l2 ? l1 : l2, c = memcmp(s1, s2, n);
        if (c == 0) c = l1 - l2;
        return compare(op, c);
    }
    int a = sum();
    for (;;) {
        const unsigned char *at = ip;
        int op = rel_op();
        if (!op) return a;
        if (string_at(ip)) {
            ip = at;
            error(E_SYNTAX);
            return 0;
        }
        int b = sum();
        a = compare(op, a < b ? -1 : a > b);
    }
}

static int conjunction(void) {
    int a = relation();
    while (*ip == T_AND) {
        ip++;
        int b = relation();
        a = a && b;
    }
    return a;
}

static int expr(void) {
    int a = conjunction();
    while (*ip == T_OR) {
        ip++;
        int b = conjunction();
        a = a || b;
    }
    return err ? 0 : a;
}

// The record of a numeric array element, for reading or assigning: the
// heap offset of its value, or -1 (checking, or an error).
static int element(const unsigned char *name, int nlen, int sub) {
    if (checking || err) return -1;
    int d = var_find(V_ARRAY, name, nlen);
    if (d < 0) d = new_array(name, nlen, 11);   // (not DIMmed: 0-10)
    if (d < 0) return -1;
    if (sub < 0 || sub >= get16(d)) {
        error(E_RANGE);
        return -1;
    }
    return d + 2 + 2 * sub;
}

static int bracketed(void) {
    expect('(');
    int v = expr();
    expect(')');
    return v;
}

static int primary(void) {
    if (err) return 0;
    int c = *ip;
    if (is_digit(c)) {
        long v = 0;
        while (is_digit(*ip)) {
            v = v * 10 + (*ip++ - '0');
            if (v > 32767) v = 99999;
        }
        return check_range(v);
    }
    if (c == '(') return bracketed();
    if (is_alpha(c)) {
        const unsigned char *name;
        int nlen = read_name(&name);
        if (*ip == '$') {                 // (a string where a number should be)
            error(E_SYNTAX);
            return 0;
        }
        if (*ip == '(') {
            int sub = bracketed();
            int at = element(name, nlen, sub);
            return at < 0 ? 0 : get16(at);
        }
        if (checking) return 0;
        int d = var_find(V_NUM, name, nlen);
        return d < 0 ? 0 : get16(d);
    }
    ip++;
    int v, w;
    const unsigned char *s;
    switch (c) {
    case T_ABS:
        v = bracketed();
        return v < 0 ? -v : v;
    case T_SGN:
        v = bracketed();
        return v > 0 ? 1 : v < 0 ? -1 : 0;
    case T_RND:
        v = bracketed();
        if (checking || err) return 0;
        if (v == 0) {
            error(E_RANGE);
            return 0;
        }
        seed_rnd();
        w = (rnd_state >> 16) % (v < 0 ? -v : v);
        return v < 0 ? -w : w;
    case T_PEEK:
        v = bracketed();
        return checking || err ? 0 : apple_peek(v & 0xFFFF);
    case T_PDL:
        v = bracketed();
        if (!checking && !err && (v < 0 || v > 3)) error(E_RANGE);
        return checking || err ? 0 : apple_paddle(v);
    case T_LEN:
    case T_ASC:
        expect('(');
        string_expr(&s, &v);
        expect(')');
        if (c == T_LEN) return v;
        return checking || err ? 0 : v ? s[0] | 0x80 : 0;
    case T_SCRN:
        expect('(');
        v = expr();
        expect(',');
        w = expr();
        expect(')');
        if (checking || err) return 0;
        if (v < 0 || v > 39 || w < 0 || w > 47) {
            error(E_RANGE);
            return 0;
        }
        return apple_scrn(v, w);
    }
    error(E_SYNTAX);
    return 0;
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

static void goto_line(int num) {
    if (checking || err) return;
    int off = line_find(num);
    if (off >= prog_len || line_num(off) != num) {
        error(E_BRANCH);
        return;
    }
    cur_line = off;
    ip = line_text(off);
    jumped = 1;
    if (tracing) {
        out('#');
        out_num(num);
        out(' ');
    }
}

static void show_dsp(int d, const unsigned char *name, int nlen) {
    for (int i = 0; i < n_dsp; i++)
        if (dsp_vars[i] == d) {
            fresh_line();
            out('#');
            out_num(cur_line >= 0 ? line_num(cur_line) : 0);
            out(' ');
            for (int k = 0; k < nlen; k++) out(name[k]);
            out('=');
            out_num(get16(d));
            out(13);
        }
}

static void set_num(const unsigned char *name, int nlen, int v) {
    int d = var_find(V_NUM, name, nlen);
    if (d < 0) d = var_new(V_NUM, name, nlen, 2);
    if (d < 0) return;
    set16(d, v);
    show_dsp(d, name, nlen);
}

// A$ = s, or A$(from) = s (replacing from there to the end).
static void set_string(const unsigned char *name, int nlen, int from, const unsigned char *s,
                       int len) {
    int d = var_find(V_STR, name, nlen);
    if (d < 0) d = new_string(name, nlen, 255);   // (not DIMmed: up to 255)
    if (d < 0) return;
    int cap = ib_heap[d], old = ib_heap[d + 1];
    if (from < 1 || from > old + 1) {
        error(E_RANGE);
        return;
    }
    if (from - 1 + len > cap) {
        error(E_STR);
        return;
    }
    memmove(ib_heap + d + 2 + from - 1, s, len);
    ib_heap[d + 1] = from - 1 + len;
}

static void let(void) {
    const unsigned char *name;
    int nlen = read_name(&name);
    if (!nlen) {
        error(E_SYNTAX);
        return;
    }
    if (*ip == '$') {
        ip++;
        int from = 1;
        if (*ip == '(') from = bracketed();
        if (!expect('=')) return;
        if (!string_at(ip)) {
            error(E_SYNTAX);
            return;
        }
        const unsigned char *s;
        int len;
        string_expr(&s, &len);
        if (!checking && !err) set_string(name, nlen, from, s, len);
        return;
    }
    int at = -1, sub = 0, is_elem = *ip == '(';
    if (is_elem) sub = bracketed();
    if (!expect('=')) return;
    if (string_at(ip)) {
        error(E_SYNTAX);
        return;
    }
    int v = expr();
    if (checking || err) return;
    if (is_elem) {
        at = element(name, nlen, sub);
        if (at >= 0) set16(at, v);
    } else {
        set_num(name, nlen, v);
    }
}

static int column(void) { return apple_peek(ZP_CH); }

static void print(void) {
    int newline = 1;
    while (!at_end() && !err) {
        if (*ip == ';') {
            ip++;
            newline = 0;
            continue;
        }
        if (*ip == ',') {                 // the next of the 8-column stops
            ip++;
            newline = 0;
            if (!checking) {
                if (column() >= 32) out(13);
                else do out(' '); while (column() % 8);
            }
            continue;
        }
        if (string_at(ip)) {
            const unsigned char *s;
            int len;
            string_expr(&s, &len);
            if (!checking && !err)
                for (int i = 0; i < len; i++) out(s[i]);
        } else {
            int v = expr();
            if (!checking && !err) out_num(v);
        }
        newline = 1;
        if (!at_end() && *ip != ';' && *ip != ',') error(E_SYNTAX);
    }
    if (newline && !checking && !err) out(13);
}

static void stop_at(void);
static void poll(void);

// The values typed for INPUT's numbers, evaluated as expressions.
HIGH_MEM char ib_input[260];
HIGH_MEM unsigned char ib_input_tokens[MAX_TOKENS + 2];

// A line for INPUT: typed, or from the text file being read (READ).
static int input_line(void) {
    if (dos_reading()) {
        int n = dos_read_line(ib_input, 256);
        if (n < 0) {
            dos_err = DOS_END_OF_DATA;
            error(E_DOS);
            return 0;
        }
        return n;
    }
    return apple_getln(0, ib_input, 256);
}

static void input(void) {
    const unsigned char *prompt = 0;
    int plen = 0;
    if (*ip == '"') {
        string_expr(&prompt, &plen);
        if (*ip != ',' && *ip != ';') {
            error(E_SYNTAX);
            return;
        }
        ip++;
    }
    const unsigned char *vars = ip;
    // Check the variables, then ask.
    for (;;) {
        const unsigned char *name;
        if (!read_name(&name)) {
            error(E_SYNTAX);
            return;
        }
        if (*ip == '$') ip++;
        else if (*ip == '(') {
            int save = checking;
            checking = 1;
            bracketed();
            checking = save;
        }
        if (*ip != ',') break;
        ip++;
    }
    if (checking || err) return;
    const unsigned char *after = ip;
again:
    ip = vars;
    for (int i = 0; i < plen; i++) out(prompt[i]);
    const unsigned char *saved = 0;       // the typed values, where we're up to
    int first = 1;
    for (;;) {
        const unsigned char *name;
        int nlen = read_name(&name);
        if (*ip == '$') {                 // a string: the whole of a line
            ip++;
            int from = 1;
            if (*ip == '(') from = bracketed();
            int n = input_line();
            if (err) return;
            set_string(name, nlen, from, (unsigned char *)ib_input, n);
            if (err) return;
        } else {
            int sub = 0, is_elem = *ip == '(';
            if (is_elem) sub = bracketed();
            if (err) return;
            if (!saved || !*saved) {
                if (!dos_reading()) {
                    if (!first || !plen) out('?');
                    if (!first) out('?');
                }
                input_line();
                if (err) return;
                int n = crunch(ib_input, ib_input_tokens, MAX_TOKENS, 0);
                if (n <= 0) n = crunch(ib_input, ib_input_tokens, MAX_TOKENS, 1);
                if (n < 0) {
                    outs("RETYPE LINE");
                    out(13);
                    goto again;
                }
                saved = ib_input_tokens;
            }
            const unsigned char *here = ip;
            int here_line = cur_line;
            ip = saved;
            int v = expr();
            int bad = err || (*ip && *ip != ',');
            if (*ip == ',') ip++;
            saved = ip;
            ip = here;
            cur_line = here_line;
            if (bad) {
                err = 0;
                outs("RETYPE LINE");
                out(13);
                goto again;
            }
            if (is_elem) {
                int at = element(name, nlen, sub);
                if (at >= 0) set16(at, v);
            } else {
                set_num(name, nlen, v);
            }
            if (err) return;
        }
        first = 0;
        if (*ip != ',') break;
        ip++;
    }
    ip = after;
}

static void skip_statement(void) {
    if (*ip == T_REM) {
        while (*ip) ip++;
        return;
    }
    int in_str = 0;
    while (*ip && (in_str || *ip != ':')) {
        if (*ip == '"') in_str = !in_str;
        ip++;
    }
}

static void statement(void);

static void if_then(void) {
    int cond = expr();
    if (!expect(T_THEN)) return;
    if (is_digit(*ip)) {
        int n = expr();
        if (cond && !checking) goto_line(n);
        return;
    }
    if (checking || cond) statement();
    else skip_statement();                // (just this statement: see the top)
}

static void for_loop(void) {
    const unsigned char *name;
    int nlen = read_name(&name);
    if (!nlen || *ip == '$' || *ip == '(') {
        error(E_SYNTAX);
        return;
    }
    if (!expect('=')) return;
    int from = expr();
    if (!expect(T_TO)) return;
    int limit = expr(), step = 1;
    if (*ip == T_STEP) {
        ip++;
        step = expr();
    }
    if (checking || err) return;
    set_num(name, nlen, from);
    int d = var_find(V_NUM, name, nlen);
    if (d < 0) return;
    for (int i = 0; i < n_fors; i++)      // (a loop with the same variable ends)
        if (fors[i].var == d) {
            n_fors = i;
            break;
        }
    if (n_fors == 8) {
        error(E_FORS);
        return;
    }
    fors[n_fors].var = d;
    fors[n_fors].limit = limit;
    fors[n_fors].step = step;
    fors[n_fors].line = cur_line;
    fors[n_fors].ip = ip;
    n_fors++;
}

static void next(void) {
    for (;;) {
        const unsigned char *name;
        int nlen = read_name(&name);
        if (!nlen) {
            if (!checking && n_fors == 0) error(E_NEXT);
        }
        if (!checking && !err) {
            int i = n_fors - 1;
            if (nlen) {
                int d = var_find(V_NUM, name, nlen);
                while (i >= 0 && fors[i].var != d) i--;
            }
            if (i < 0) {
                error(E_NEXT);
                return;
            }
            n_fors = i + 1;
            int d = fors[i].var;
            long v = (long)get16(d) + fors[i].step;
            int done = v > 32767 || v < -32767 ||
                       (fors[i].step >= 0 ? v > fors[i].limit : v < fors[i].limit);
            if (v <= 32767 && v >= -32767) {
                set16(d, (int)v);
                if (nlen) show_dsp(d, ib_heap + d - nlen, nlen);
            }
            if (!done) {
                cur_line = fors[i].line;
                ip = fors[i].ip;
                jumped = 1;
                return;
            }
            n_fors = i;
        }
        if (*ip != ',' || err) return;
        ip++;
    }
}

static void dim(void) {
    for (;;) {
        const unsigned char *name;
        int nlen = read_name(&name);
        if (!nlen) {
            error(E_SYNTAX);
            return;
        }
        int is_str = *ip == '$';
        if (is_str) ip++;
        int n = bracketed();
        if (err) return;
        if (!checking) {
            if (var_find(is_str ? V_STR : V_ARRAY, name, nlen) >= 0) error(E_DIM);
            else if (n < 0 || (is_str && (n < 1 || n > 255))) error(E_RANGE);
            else if (is_str) new_string(name, nlen, n);
            else new_array(name, nlen, n + 1);
            if (err) return;
        }
        if (*ip != ',') return;
        ip++;
    }
}

static int number_in(int lo, int hi, const char *e) {
    int v = expr();
    if (!checking && !err && (v < lo || v > hi)) error(e);
    return v;
}

static void list_program(int from, int to);
static void enter_line(const char *text);

// Arguments "a" or "a,b" for LIST, DEL and AUTO; how many there were.
static int range_args(int *a, int *b) {
    if (at_end()) return 0;
    *a = expr();
    if (*ip != ',') return 1;
    ip++;
    *b = expr();
    return 2;
}

static void run_from(int num) {
    clear_vars();
    if (prog_len == 0) {
        ended = 1;
        return;
    }
    if (num < 0) {
        cur_line = 0;
        ip = line_text(0);
        jumped = 1;
        if (tracing) {
            out('#');
            out_num(line_num(0));
            out(' ');
        }
    } else {
        goto_line(num);
    }
}

static void statement(void) {
    if (err) return;
    int t = *ip, a = 0, b = 0, n;
    if (is_alpha(t)) {
        let();
        return;
    }
    if (t < 0x80 || !(kw_kind(t) & K_STMT)) {
        error(E_SYNTAX);
        return;
    }
    ip++;
    switch (t) {
    case T_LET: let(); break;
    case T_PRINT: print(); break;
    case T_INPUT: input(); break;
    case T_IF: if_then(); break;
    case T_GOTO:
        n = expr();
        goto_line(n);
        break;
    case T_GOSUB:
        n = expr();
        if (checking || err) break;
        if (n_gosubs == 8) {
            error(E_GOSUBS);
            break;
        }
        gosubs[n_gosubs].line = cur_line;
        gosubs[n_gosubs].ip = ip;
        n_gosubs++;
        goto_line(n);
        if (err) n_gosubs--;
        break;
    case T_RETURN:
    case T_POP:
        if (checking) break;
        if (n_gosubs == 0) {
            error(E_RETURN);
            break;
        }
        n_gosubs--;
        if (t == T_RETURN) {
            cur_line = gosubs[n_gosubs].line;
            ip = gosubs[n_gosubs].ip;
            jumped = 1;
        }
        break;
    case T_FOR: for_loop(); break;
    case T_NEXT: next(); break;
    case T_END:
        if (!checking) ended = 1;
        break;
    case T_REM:
        while (*ip) ip++;
        break;
    case T_DIM: dim(); break;
    case T_POKE:
        a = expr();
        expect(',');
        b = number_in(0, 255, E_255);
        if (!checking && !err) apple_poke(a & 0xFFFF, b);
        break;
    case T_CALL:
        a = expr();
        if (!checking && !err) apple_call(a & 0xFFFF);   // (machine code: not run)
        break;
    case T_TAB:
        a = number_in(1, 255, E_RANGE);
        if (checking || err) break;
        if (setting == INT_1976) {
            while (column() < a - 1) out(' ');
        } else {
            int w = apple_peek(ZP_WNDWDTH);
            apple_htab(a - 1 < w ? a - 1 : w - 1);
        }
        break;
    case T_VTAB:
        a = number_in(1, 24, E_RANGE);
        if (!checking && !err) apple_vtab(a - 1);
        break;
    case T_GR:
        if (!checking) apple_gr();
        break;
    case T_TEXT:
        if (!checking) apple_text();
        break;
    case T_COLOR:
        a = number_in(0, 255, E_255);
        if (!checking && !err) apple_color(a);
        break;
    case T_PLOT:
        a = number_in(0, 39, E_RANGE);
        expect(',');
        b = number_in(0, 47, E_RANGE);
        if (!checking && !err) apple_plot(a, b);
        break;
    case T_HLIN:
    case T_VLIN: {
        int top = t == T_HLIN ? 39 : 47;
        a = number_in(0, top, E_RANGE);
        expect(',');
        b = number_in(0, top, E_RANGE);
        expect(T_AT);
        n = number_in(0, 86 - top, E_RANGE);
        if (checking || err) break;
        if (t == T_HLIN) apple_hlin(a, b, n);
        else apple_vlin(a, b, n);
        break;
    }
    case T_DSP: {
        const unsigned char *name;
        int nlen = read_name(&name);
        if (!nlen || *ip == '$' || *ip == '(') {
            error(E_SYNTAX);
            break;
        }
        if (checking) break;
        int d = var_find(V_NUM, name, nlen);
        if (d < 0) d = var_new(V_NUM, name, nlen, 2);
        if (d >= 0 && n_dsp < MAX_DSP) dsp_vars[n_dsp++] = d;
        break;
    }
    case T_TRACE:
        if (!checking) tracing = 1;
        break;
    case T_NOTRACE:
        if (!checking) tracing = 0;
        break;
    case T_CLR:
        if (!checking) clear_vars();
        break;
    case T_NEW:
        if (checking) break;
        prog_len = 0;
        clear_vars();
        auto_on = 0;
        ended = 1;
        break;
    case T_RUN:
        n = at_end() ? -1 : expr();
        if (!checking && !err) run_from(n);
        break;
    case T_LIST:
        a = 0, b = 32767;
        n = range_args(&a, &b);
        if (n == 1) b = a;
        if (!checking && !err) list_program(a, b);
        break;
    case T_DEL:
        n = range_args(&a, &b);
        if (n == 0) error(E_SYNTAX);
        if (n == 1) b = a;
        if (checking || err) break;
        for (int off = line_find(a); off < prog_len && line_num(off) <= b;)
            line_delete(line_num(off));
        ended = 1;
        break;
    case T_AUTO:
        a = 10, b = 10;
        n = range_args(&a, &b);
        if (n == 0) error(E_SYNTAX);
        if (checking || err) break;
        auto_on = 1;
        auto_line = a;
        auto_step = b > 0 ? b : 10;
        break;
    case T_MAN:
        if (!checking) auto_on = 0;
        break;
    case T_CON:
        if (checking) break;
        if (cont.ok) {
            cur_line = cont.line;
            ip = cont.ip;
            jumped = 1;
            cont.ok = 0;
        }
        break;
    case T_HIMEM:
    case T_LOMEM:
        a = expr();
        if (checking || err) break;
        apple_poke(t == T_HIMEM ? 76 : 74, a & 0xFF);
        apple_poke(t == T_HIMEM ? 77 : 75, (a >> 8) & 0xFF);
        clear_vars();
        break;
    case T_LOAD:
    case T_SAVE:
        break;                            // (from tape: see the DOS commands)
    case T_PR:
    case T_IN:
        number_in(0, 7, E_RANGE);
        break;
    default:
        error(E_SYNTAX);
    }
}

/* ------------------------------------------------------------------------
 * Running lines
 * --------------------------------------------------------------------- */

static int poll_count;

// Every few statements: the screen, and Ctrl+C.
static void poll(void) {
    if (++poll_count < 16) return;
    poll_count = 0;
    if (apple_key_peek() == 3) {
        apple_key();
        stop_at();
    }
}

static void stop_at(void) {
    if (cur_line < 0) {
        ended = 1;
        return;
    }
    cont.line = cur_line;
    cont.ip = ip;
    cont.ok = 1;
    fresh_line();
    outs("STOPPED AT ");
    out_num(line_num(cur_line));
    out(13);
    ended = 1;
}

static void report(void) {
    fresh_line();
    apple_bell();
    if (dos_err) {
        outs(dos_message(dos_err));
        out(13);
        return;
    }
    outs("*** ");
    outs(err);
    outs(" ERR");
    out(13);
    if (cur_line >= 0 && strcmp(err, E_NO_END)) {
        outs("STOPPED AT ");
        out_num(line_num(cur_line));
        out(13);
    }
}

// Run from ip (a typed line, or a program line) until done.
static void execute(void) {
    ended = 0;
    while (!err && !ended) {
        if (*ip == ':') {
            ip++;
            continue;
        }
        if (*ip == 0) {
            if (cur_line < 0) break;
            int next_off = line_next(cur_line);
            if (next_off >= prog_len) {
                error(E_NO_END);
                break;
            }
            cur_line = next_off;
            ip = line_text(next_off);
            if (tracing) {
                out('#');
                out_num(line_num(cur_line));
                out(' ');
            }
            continue;
        }
        jumped = 0;
        statement();
        if (err || ended) break;
        if (!jumped && !at_end()) error(E_SYNTAX);
        poll();
    }
    if (err) report();
    err = 0;
    dos_err = 0;
    dos_program_ended();
    cur_line = -1;
}

static void dos_done(int r) {
    if (r == DOS_RUN) {
        run_from(-1);
    } else if (r != DOS_OK) {
        dos_err = r;
        error(E_DOS);
    }
}

// Check a crunched line's syntax (and its numbers' sizes).
static const char *check(const unsigned char *tokens) {
    checking = 1;
    err = 0;
    ip = tokens;
    while (!err) {
        while (*ip == ':') ip++;
        if (!*ip) break;
        statement();
        if (!err && !at_end()) error(E_SYNTAX);
    }
    checking = 0;
    const char *e = err;
    err = 0;
    return e;
}

// Crunch a line and check it, as described above. Its length, or -1
// with the error in *e.
static int crunch_line(const char *text, unsigned char *buf, const char **e) {
    int n = crunch(text, buf, MAX_TOKENS, 0);
    *e = n < 0 ? E_LONG : check(buf);
    if (*e) {
        int m = crunch(text, buf, MAX_TOKENS, 1);
        const char *e2 = m < 0 ? E_LONG : check(buf);
        if (!e2) {
            *e = 0;
            return m;
        }
        crunch(text, buf, MAX_TOKENS, 0);
    }
    return *e ? -1 : n;
}

static void line_error(const char *e) {
    fresh_line();
    apple_bell();
    outs("*** ");
    outs(e);
    outs(" ERR");
    out(13);
}

// A line typed (or read from a listing): a program line, or run now.
static void enter_line(const char *text) {
    while (*text == ' ') text++;
    if (is_digit(*text)) {
        long num = 0;
        while (is_digit(*text)) {
            num = num * 10 + (*text++ - '0');
            if (num > 99999) num = 99999;
        }
        if (num > 32767) {
            line_error(E_32767);
            return;
        }
        const char *p = text;
        while (*p == ' ') p++;
        if (!*p) {
            line_delete(num);
            return;
        }
        const char *e;
        int n = crunch_line(text, ib_typed, &e);
        if (n < 0) {
            line_error(e);
            return;
        }
        clear_vars();                     // (as the original does when a line changes)
        if (!line_insert(num, ib_typed, n + 1)) line_error(E_MEM);
        return;
    }
    const char *e;
    if (crunch_line(text, ib_typed, &e) < 0) {
        line_error(e);
        return;
    }
    cur_line = -1;
    ip = ib_typed;
    execute();
}

/* ------------------------------------------------------------------------
 * LIST
 * --------------------------------------------------------------------- */

// The line at off as LIST shows it, into buf; its length.
static int detokenize(int off, char *buf) {
    char *p = buf;
    char num[8];
    itoa(line_num(off), num);
    for (int i = strlen(num); i < 5; i++) *p++ = ' ';
    for (char *q = num; *q; q++) *p++ = *q;
    *p++ = ' ';
    const unsigned char *t = line_text(off);
    int in_str = 0;
    for (; *t; t++) {
        int c = *t;
        if (in_str || c < 0x80) {
            if (c == '"') in_str = !in_str;
            *p++ = c;
            continue;
        }
        const char *w = kw_name(c);
        int func = kw_kind(c) & K_FUNC;
        if (!func && p[-1] != ' ') *p++ = ' ';
        while (*w) *p++ = *w++;
        int last = p[-1];
        if (c == T_REM) {
            while (t[1]) *p++ = *++t;
            break;
        }
        if (!func && last != '=' && last != ':' && last != '#') *p++ = ' ';
    }
    while (p > buf && p[-1] == ' ') p--;
    *p = '\0';
    return p - buf;
}

HIGH_MEM char ib_list[MAX_TOKENS * 8];

static void list_program(int from, int to) {
    for (int off = line_find(from); off < prog_len && line_num(off) <= to; off = line_next(off)) {
        if (apple_key_peek() == 3) {
            apple_key();
            break;
        }
        detokenize(off, ib_list);
        fresh_line();
        outs(ib_list);
        out(13);
    }
}

/* ------------------------------------------------------------------------
 * The disk commands' side (dos.h)
 * --------------------------------------------------------------------- */

static int dos_listing(char *buf, int cap) {
    int n = 0;
    for (int off = 0; off < prog_len; off = line_next(off)) {
        int len = detokenize(off, ib_list);
        if (n + len + 1 > cap) return -1;
        memcpy(buf + n, ib_list, len);
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

static const struct dos_basic dos_side = { 'I', dos_listing, dos_enter };

/* ------------------------------------------------------------------------ */

HIGH_MEM char ib_line[260];

void intbasic_run(int s) {
    setting = s;
    apple_init(s == INT_1976 ? APPLE_I : APPLE_II);
    dos_init(&dos_side, s == INT_1976 ? "A1BASIC" : "A2INT");
    // Where programs find the memory limits (zero page, as the original).
    int himem = s == INT_1976 ? 0x1000 : 0x9600, lomem = 0x0800;
    apple_poke(76, himem & 0xFF);
    apple_poke(77, himem >> 8);
    apple_poke(74, lomem & 0xFF);
    apple_poke(75, lomem >> 8);
    if (s == INT_1977) apple_vtab(23);
    for (;;) {
        fresh_line();
        if (auto_on) {
            out_num(auto_line);
            out(' ');
            apple_getln(0, ib_line, 250);
            const char *p = ib_line;
            while (*p == ' ') p++;
            if (!*p) {                    // (an empty line ends AUTO)
                auto_on = 0;
                continue;
            }
            char numbered[262];
            itoa(auto_line, numbered);
            int k = strlen(numbered);
            numbered[k++] = ' ';
            strncpy_safe(numbered + k, ib_line, sizeof(numbered) - k);
            auto_line += auto_step;
            if (auto_line > 32767) auto_on = 0;
            enter_line(numbered);
            continue;
        }
        if (dos_exec_line(ib_line, 250) < 0) apple_getln('>', ib_line, 250);
        int d = dos_command(ib_line, 1);
        if (d == DOS_NONE) {
            enter_line(ib_line);
        } else if (d == DOS_RUN) {
            ended = 0;
            run_from(-1);
            if (!ended && !err) execute();
            err = 0;
            cur_line = -1;
        } else if (d != DOS_OK) {
            fresh_line();
            apple_bell();
            outs(dos_message(d));
            out(13);
        }
    }
}
