/*
 * The BASIC interpreter.
 *
 * Program lines are tokenized as they are entered: each keyword is
 * stored as a single byte (0x80 and up), names are upper-cased, and
 * everything else is kept as typed. The program is a packed, sorted list
 * of lines in one buffer:
 *
 *     [line number: 2 bytes, little-endian] [length: 1 byte] [text] [0]
 *
 * where the length counts the text and its terminating 0.
 *
 * Statements are executed straight from the tokenized text: p points at
 * the next character to read.
 */
#include "basic.h"
#include "bios.h"
#include "string.h"
#include "mem.h"
#include "num.h"

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
    T_LE, T_GE, T_NE,
    T_END_OF_TOKENS
};

static const char *const token_names[] = {
    "PRINT", "LET", "GOTO", "GOSUB", "RETURN", "IF", "THEN", "FOR",
    "TO", "STEP", "NEXT", "REM", "INPUT", "END", "STOP", "CONTINUE", "RUN",
    "LIST", "NEW", "CLEAR", "CLS", "BEEP", "INK", "PAPER", "BORDER",
    "BRIGHT", "INVERSE", "OVER", "PLOT", "DRAW", "CIRCLE", "PAUSE",
    "RANDOMIZE", "DIM", "READ", "DATA", "RESTORE", "SAVE", "LOAD", "CAT",
    "ERASE", "EDIT", "AT", "TAB", "LINE", "AND", "OR", "NOT", "PI", "ABS",
    "INT", "SGN", "SQR", "SIN", "COS", "TAN", "ATN", "ASN", "ACS", "LN",
    "EXP", "RND", "LEN", "VAL", "CODE", "CHR$", "STR$", "INKEY$", "POINT",
    "<=", ">=", "<>",
};
_Static_assert(sizeof(token_names) / sizeof(token_names[0]) == T_END_OF_TOKENS - T_PRINT,
               "token_names doesn't match the token enum");

#define IS_TOKEN(c) ((unsigned char)(c) >= T_PRINT && (unsigned char)(c) < T_END_OF_TOKENS)

static const char *token_name(unsigned char t) {
    return token_names[t - T_PRINT];
}

// Look up a keyword (upper case, NUL-terminated); 0 if it isn't one.
static int find_token(const char *word) {
    for (int t = T_PRINT; t < T_LE; t++) {
        if (strcmp(token_name(t), word) == 0) return t;
    }
    return 0;
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
 * The text of strings and of REM comments is kept exactly.
 */
static int tokenize(const char *src, unsigned char *dst, int cap) {
    int n = 0;
    int rest_raw = 0;    // after REM: copy the rest of the line as is
    int in_data = 0;     // after DATA: copy as is up to the next ':'
#define PUT(c) do { if (n >= cap - 1) return -1; dst[n++] = (c); } while (0)
    while (*src) {
        char c = *src;
        if (rest_raw) { PUT(c); src++; continue; }
        if (c == '"') {              // a string: copy up to the closing quote
            PUT(*src++);
            while (*src && *src != '"') PUT(*src++);
            if (*src) PUT(*src++);
            continue;
        }
        if (in_data) {
            if (c == ':') in_data = 0;
            PUT(c);
            src++;
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
                if (src[len] == '$') {      // CHR$, STR$, INKEY$
                    word[len] = '$';
                    word[len + 1] = '\0';
                    t = find_token(word);
                    if (t) len++;
                    word[len] = '\0';
                }
                if (!t) t = find_token(word);
            }
            if (t) {
                while (n > 0 && dst[n - 1] == ' ') n--;
                PUT(t);
                src += len;
                while (*src == ' ') src++;
                if (t == T_REM) rest_raw = 1;
                if (t == T_DATA) in_data = 1;
            } else {
                for (int i = 0; i < len; i++) PUT(toupper(*src++));
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
// out must hold at least cap bytes.
static void detokenize(const unsigned char *src, char *out, int cap) {
    int n = 0;
    int in_str = 0;
#define OUT(c) do { if (n < cap - 1) out[n++] = (c); } while (0)
    for (; *src; src++) {
        unsigned char c = *src;
        if (c == '"') in_str = !in_str;
        if (in_str || !IS_TOKEN(c)) { OUT(c); continue; }
        const char *name = token_name(c);
        int word = isalpha(name[0]);
        if (word && n > 0 && out[n - 1] != ' ' && out[n - 1] != '(') OUT(' ');
        while (*name) OUT(*name++);
        unsigned char next = src[1];
        if (word && next && next != ' ' && next != ':' && next != ';' &&
            next != ',' && next != ')')
            OUT(' ');
    }
    out[n] = '\0';
#undef OUT
}

/* ------------------------------------------------------------------------
 * The program
 * --------------------------------------------------------------------- */

#define PROG_SIZE 32768
#define MAX_TEXT 250                      // tokenized length of one line

HIGH_MEM unsigned char prog[PROG_SIZE];
int prog_len = 0;

static int line_num(int off) { return prog[off] | (prog[off + 1] << 8); }
static int line_next(int off) { return off + 3 + prog[off + 2]; }
static unsigned char *line_text(int off) { return prog + off + 3; }

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
    if (prog_len - old + 3 + len > PROG_SIZE) return "PROGRAM FULL";
    line_delete(num);
    memmove(prog + off + 3 + len, prog + off, prog_len - off);
    prog[off] = num & 0xFF;
    prog[off + 1] = num >> 8;
    prog[off + 2] = len;
    memcpy(prog + off + 3, text, len);
    prog_len += 3 + len;
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

static void error(const char *msg) {
    if (!error_msg) error_msg = msg;
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

/* ------------------------------------------------------------------------
 * Variables
 * --------------------------------------------------------------------- */

#define NAME_LEN 16       // including the terminating 0
#define MAX_STR 255
#define MAX_VARS 128

struct var {
    char name[NAME_LEN];
    char is_str;
    unsigned char len;    // strings
    double num;           // numbers
    char str[MAX_STR];
};
HIGH_MEM struct var vars[MAX_VARS];
int num_vars = 0;

// Read a name at p: a letter, then letters and digits, then an optional
// $ for a string. Returns 0 (with an error) if there isn't one.
static int read_name(char *name, int *is_str) {
    skip_space();
    if (!isalpha(*p)) {
        error("BAD VARIABLE NAME");
        return 0;
    }
    int n = 0;
    while (isalnum(*p)) {
        if (n == NAME_LEN - 1) {
            error("NAME TOO LONG");
            return 0;
        }
        name[n++] = toupper(*p++);
    }
    name[n] = '\0';
    *is_str = (*p == '$');
    if (*is_str) p++;
    return 1;
}

static struct var *find_var(const char *name, int is_str, int create) {
    for (int i = 0; i < num_vars; i++) {
        if (vars[i].is_str == is_str && strcmp(vars[i].name, name) == 0)
            return &vars[i];
    }
    if (!create) {
        error("VARIABLE NOT FOUND");
        return 0;
    }
    if (num_vars == MAX_VARS) {
        error("TOO MANY VARIABLES");
        return 0;
    }
    struct var *v = &vars[num_vars++];
    strcpy(v->name, name);
    v->is_str = is_str;
    v->len = 0;
    v->num = 0;
    return v;
}

static void clear_variables() {
    num_vars = 0;
}

/* ------------------------------------------------------------------------
 * Expression values
 *
 * Strings produced while evaluating (by +, CHR$ etc.) live in a scratch
 * pool that is emptied at the start of every statement.
 * --------------------------------------------------------------------- */

struct value {
    int is_str;
    double num;
    const char *str;      // not NUL-terminated
    int len;
};

#define POOL_SIZE 8192
HIGH_MEM char str_pool[POOL_SIZE];
int pool_used;

static char *pool_alloc(int len) {
    static char dummy[MAX_STR + 1];
    if (pool_used + len > POOL_SIZE) {
        error("OUT OF MEMORY");
        return dummy;
    }
    char *s = str_pool + pool_used;
    pool_used += len;
    return s;
}

static struct value num_value(double n) {
    struct value v = { 0, n, 0, 0 };
    return v;
}

static struct value str_value(const char *s, int len) {
    struct value v = { 1, 0, s, len };
    return v;
}

static struct value expr();

// Results that overflow become infinity (or NaN); report them.
static double check(double v) {
    if (!num_is_finite(v)) error("NUMBER TOO BIG");
    return v;
}

static double expr_num() {
    struct value v = expr();
    if (v.is_str) error("NUMBER EXPECTED");
    return v.num;
}

static struct value expr_str() {
    struct value v = expr();
    if (!error_msg && !v.is_str) error("STRING EXPECTED");
    return v;
}

// Convert to a whole number in [lo, hi], rounding to the nearest.
static int to_int(double v, int lo, int hi) {
    v = num_round(v);
    if (!(v >= lo && v <= hi)) {     // (also catches NaN)
        error("INTEGER OUT OF RANGE");
        return lo;
    }
    return (int)v;
}

static int expr_int(int lo, int hi) {
    double v = expr_num();
    return error_msg ? lo : to_int(v, lo, hi);
}

/* ------------------------------------------------------------------------
 * Expressions
 *
 * Precedence, lowest first, as on the Spectrum:
 *   OR, AND, NOT, comparisons, + -, * /, unary -, ^, functions
 * Comparisons give 1 (true) or 0 (false).
 * --------------------------------------------------------------------- */

static struct value factor();

static double invalid_argument() {
    error("INVALID ARGUMENT");
    return 0;
}

static double power(double x, double y) {
    if (y == num_floor(y) && num_abs(y) <= 1024) {
        // Whole exponents by repeated squaring: exact where possible,
        // and negative bases are fine.
        int n = (int)num_abs(y);
        double r = 1, b = x;
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        if (y < 0) {
            if (r == 0) return invalid_argument();
            r = 1 / r;
        }
        return check(r);
    }
    if (x < 0) return invalid_argument();   // e.g. (-8)^(1/3)
    if (x == 0) return y > 0 ? 0 : invalid_argument();
    return check(num_exp(y * num_ln(x)));
}

// Functions take the next factor as their argument, as on the Spectrum:
// SIN X^2 is (SIN X)^2, and brackets are optional: SQR 2.
static double num_arg() {
    struct value v = factor();
    if (v.is_str) error("NUMBER EXPECTED");
    return v.num;
}

static struct value numeric_function(int t) {
    double x = num_arg();
    if (error_msg) return num_value(0);
    switch (t) {
    case T_ABS: return num_value(num_abs(x));
    case T_INT: return num_value(num_floor(x));
    case T_SGN: return num_value(x > 0 ? 1 : x < 0 ? -1 : 0);
    case T_SQR: return num_value(x < 0 ? invalid_argument() : num_sqrt(x));
    case T_SIN: return num_value(num_sin(x));
    case T_COS: return num_value(num_cos(x));
    case T_TAN: return num_value(check(num_tan(x)));
    case T_ATN: return num_value(num_atan2(x, 1));
    case T_ASN:
        if (x < -1 || x > 1) return num_value(invalid_argument());
        return num_value(num_atan2(x, num_sqrt(1 - x * x)));
    case T_ACS:
        if (x < -1 || x > 1) return num_value(invalid_argument());
        return num_value(num_atan2(num_sqrt(1 - x * x), x));
    case T_LN:  return num_value(x <= 0 ? invalid_argument() : num_ln(x));
    case T_EXP: return num_value(check(num_exp(x)));
    }
    return num_value(0);
}

static struct value variable_value() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return num_value(0);
    struct var *v = find_var(name, is_str, 0);
    if (!v) return num_value(0);
    return is_str ? str_value(v->str, v->len) : num_value(v->num);
}

static struct value factor() {
    if (error_msg) return num_value(0);
    skip_space();
    unsigned char c = *p;
    double n;
    if (num_parse((const char **)&p, &n)) {
        return num_value(check(n));
    } else if (c == '"') {
        const char *start = ++p;
        while (*p && *p != '"') p++;
        struct value v = str_value(start, p - start);
        if (*p == '"') p++;
        return v;
    } else if (c == '-') {
        p++;
        return num_value(-num_arg());
    } else if (c == '+') {
        p++;
        return num_value(num_arg());
    } else if (c == '(') {
        p++;
        struct value v = expr();
        expect_char(')', "MISSING )");
        return v;
    } else if (c == T_PI) {
        p++;
        return num_value(3.14159265358979323846);
    } else if (c >= T_ABS && c <= T_EXP) {
        p++;
        return numeric_function(c);
    } else if (isalpha(c)) {
        return variable_value();
    }
    error("SYNTAX ERROR");
    return num_value(0);
}

// factor ^ factor ^ ...   (left to right, as on the Spectrum)
static struct value power_expr() {
    struct value v = factor();
    while (!error_msg && accept_char('^')) {
        double y = num_arg();
        if (v.is_str) error("NUMBER EXPECTED");
        if (!error_msg) v = num_value(power(v.num, y));
    }
    return v;
}

// A leading minus applies after ^, so -2^2 is -4.
static struct value unary() {
    if (accept_char('-')) {
        struct value v = unary();
        if (v.is_str) error("NUMBER EXPECTED");
        return num_value(-v.num);
    }
    if (accept_char('+')) return unary();
    return power_expr();
}

static struct value term() {
    struct value v = unary();
    skip_space();
    while (!error_msg && (*p == '*' || *p == '/')) {
        char op = *p++;
        struct value w = unary();
        if (error_msg) break;
        if (v.is_str || w.is_str) { error("NUMBER EXPECTED"); break; }
        if (op == '*') {
            v.num = check(v.num * w.num);
        } else if (w.num == 0) {
            error("DIVISION BY ZERO");
        } else {
            v.num = check(v.num / w.num);
        }
        skip_space();
    }
    return v;
}

static struct value sum() {
    struct value v = term();
    skip_space();
    while (!error_msg && (*p == '+' || *p == '-')) {
        char op = *p++;
        struct value w = term();
        if (error_msg) break;
        if (v.is_str != w.is_str || (v.is_str && op == '-')) {
            error(v.is_str ? "STRING EXPECTED" : "NUMBER EXPECTED");
            break;
        }
        if (v.is_str) {                    // concatenation
            if (v.len + w.len > MAX_STR) { error("STRING TOO LONG"); break; }
            char *s = pool_alloc(v.len + w.len);
            memcpy(s, v.str, v.len);
            memcpy(s + v.len, w.str, w.len);
            v = str_value(s, v.len + w.len);
        } else {
            v.num = check(op == '+' ? v.num + w.num : v.num - w.num);
        }
        skip_space();
    }
    return v;
}

// Compare two values of the same type: <0, 0 or >0.
static int compare(struct value a, struct value b) {
    if (!a.is_str) return a.num < b.num ? -1 : a.num > b.num;
    int n = a.len < b.len ? a.len : b.len;
    int c = memcmp(a.str, b.str, n);
    if (c) return c;
    return a.len - b.len;
}

static struct value comparison() {
    struct value v = sum();
    while (!error_msg) {
        skip_space();
        unsigned char op = *p;
        if (op != '=' && op != '<' && op != '>' &&
            op != T_LE && op != T_GE && op != T_NE)
            break;
        p++;
        struct value w = sum();
        if (error_msg) break;
        if (v.is_str != w.is_str) {
            error(v.is_str ? "STRING EXPECTED" : "NUMBER EXPECTED");
            break;
        }
        int c = compare(v, w);
        int r = op == '=' ? c == 0 : op == '<' ? c < 0 : op == '>' ? c > 0 :
                op == T_LE ? c <= 0 : op == T_GE ? c >= 0 : c != 0;
        v = num_value(r);
    }
    return v;
}

static struct value expr() {
    return comparison();
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

struct for_loop {
    struct var *var;
    double limit;
    double step;
    int line;             // where the loop body starts: line offset
    char *pos;            //   and position in it
};
#define MAX_FOR 16
struct for_loop for_stack[MAX_FOR];
int for_sp = 0;

static void reset_run_state() {
    for_sp = 0;
}

// Continue at the start of line number num.
static void go_to_line(int num) {
    if (error_msg) return;
    int off = line_find(num);
    if (off >= prog_len || line_num(off) != num) {
        error("LINE NOT FOUND");
        return;
    }
    cur_line = off;
    p = (char *)line_text(off);
    jumped = 1;
}

// Store a value in a variable.
static void assign(struct var *v, struct value val) {
    if (v->is_str) {
        // memmove: val may be part of the variable itself
        memmove(v->str, val.str, val.len);
        v->len = val.len;
    } else {
        v->num = val.num;
    }
}

// [LET] name = expression   (the LET itself already consumed)
static void st_let() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return;
    if (!accept_char('=')) { error("SYNTAX ERROR"); return; }
    struct value val = is_str ? expr_str() : num_value(expr_num());
    if (error_msg) return;
    struct var *v = find_var(name, is_str, 1);
    if (v) assign(v, val);
}

static void print_value(struct value v) {
    if (v.is_str) {
        for (int i = 0; i < v.len; i++) print_char(v.str[i]);
    } else {
        char buf[24];
        num_format(v.num, buf);
        print_string(buf);
    }
}

// PRINT item [; item]... - a trailing ; leaves the cursor after the text.
static void st_print() {
    int newline = 1;
    while (!at_statement_end()) {
        newline = 1;
        if (accept_char(';') || accept_char(',')) {
            newline = 0;
            continue;
        }
        struct value v = expr();
        if (error_msg) return;
        print_value(v);
        if (!at_statement_end() && *p != ';' && *p != ',') {
            error("SYNTAX ERROR");
            return;
        }
    }
    if (newline) print_string("\n");
}

// Skip to the end of the current line.
static void skip_line() {
    while (*p) p++;
}

static void st_if() {
    double cond = expr_num();
    if (error_msg) return;
    if (!accept(T_THEN)) { error("THEN EXPECTED"); return; }
    if (!cond) { skip_line(); return; }
    skip_space();
    if (isdigit(*p)) {                    // IF ... THEN 100
        go_to_line(expr_int(1, 9999));
    } else {
        jumped = 1;     // the statements after THEN simply run next
    }
}

/*
 * Find the NEXT for loop variable v after the current position, for a
 * FOR loop that runs zero times. Moves p (and cur_line) just past it.
 */
static int skip_to_next(struct var *v) {
    for (;;) {
        // Scan the rest of this line, skipping strings and comments.
        int in_str = 0;
        for (; *p; p++) {
            unsigned char c = *p;
            if (c == '"') in_str = !in_str;
            if (in_str) continue;
            if (c == T_REM) { skip_line(); break; }
            if (c == T_NEXT) {
                char *save = ++p;
                char name[NAME_LEN];
                int is_str;
                if (read_name(name, &is_str) && !is_str && strcmp(name, v->name) == 0)
                    return 1;
                error_msg = 0;
                p = save - 1;
            }
        }
        if (cur_line < 0) return 0;
        cur_line = line_next(cur_line);
        if (cur_line >= prog_len) return 0;
        p = (char *)line_text(cur_line);
    }
}

static void st_for() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return;
    if (is_str) { error("BAD VARIABLE NAME"); return; }
    if (!accept_char('=')) { error("SYNTAX ERROR"); return; }
    double start = expr_num();
    if (error_msg) return;
    if (!accept(T_TO)) { error("TO EXPECTED"); return; }
    double limit = expr_num();
    double step = 1;
    if (accept(T_STEP)) step = expr_num();
    if (error_msg) return;
    struct var *v = find_var(name, 0, 1);
    if (!v) return;
    v->num = start;

    // Re-entering a FOR for a variable that is already looping (e.g.
    // via GOTO) restarts that loop instead of stacking a duplicate.
    for (int i = 0; i < for_sp; i++) {
        if (for_stack[i].var == v) { for_sp = i; break; }
    }
    // A loop that wouldn't run even once is skipped entirely, as on the
    // Spectrum: carry on after the matching NEXT.
    if ((step >= 0 && start > limit) || (step < 0 && start < limit)) {
        if (!skip_to_next(v)) error("FOR WITHOUT NEXT");
        return;
    }
    if (for_sp >= MAX_FOR) {
        error("TOO MANY NESTED FOR LOOPS");
        return;
    }
    struct for_loop *f = &for_stack[for_sp++];
    f->var = v;
    f->limit = limit;
    f->step = step;
    f->line = cur_line;
    f->pos = p;
}

static void st_next() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return;
    // Find the matching loop; any inner loops left unfinished are dropped.
    int i = for_sp - 1;
    while (i >= 0 && (is_str || strcmp(for_stack[i].var->name, name) != 0)) i--;
    if (i < 0) {
        error("NEXT WITHOUT FOR");
        return;
    }
    for_sp = i + 1;
    struct for_loop *f = &for_stack[i];
    double v = check(f->var->num + f->step);
    if (error_msg) return;
    f->var->num = v;
    if ((f->step >= 0 && v <= f->limit) || (f->step < 0 && v >= f->limit)) {
        cur_line = f->line;   // back to just after the FOR statement
        p = f->pos;
    } else {
        for_sp--;
    }
}

static void st_colour(void (*set)(int)) {
    int c = expr_int(0, 7);
    if (!error_msg) set(c);
}

static void list_program() {
    char buf[MAX_TEXT * 4];
    for (int off = 0; off < prog_len && !check_break(); off = line_next(off)) {
        char num[8];
        itoa(line_num(off), num);
        print_string(num);
        print_char(' ');
        detokenize(line_text(off), buf, sizeof(buf));
        print_string(buf);
        print_string("\n");
    }
}

static void execute_statement() {
    skip_space();
    unsigned char t = *p;
    if (IS_TOKEN(t)) p++;
    switch (t) {
    case T_PRINT:  st_print(); break;
    case T_LET:    st_let(); break;
    case T_GOTO:   go_to_line(expr_int(1, 9999)); break;
    case T_IF:     st_if(); break;
    case T_FOR:    st_for(); break;
    case T_NEXT:   st_next(); break;
    case T_REM:    skip_line(); break;
    case T_CLS:    clear_screen(); break;
    case T_INK:    st_colour(set_ink); break;
    case T_PAPER:  st_colour(set_paper); break;
    case T_BORDER: st_colour(set_border); break;
    case T_END:    running = 0; break;
    case T_NEW:
        prog_len = 0;
        clear_variables();
        reset_run_state();
        clear_screen();
        running = 0;
        break;
    case T_CLEAR:
        clear_variables();
        reset_run_state();
        break;
    case T_LIST:
        list_program();
        break;
    case T_RUN:
        clear_variables();
        reset_run_state();
        if (prog_len == 0) { running = 0; break; }
        cur_line = 0;
        p = (char *)line_text(0);
        jumped = 1;
        break;
    case T_BEEP:
        beep();
        // Arguments (duration, pitch) are accepted but not used yet.
        while (!at_statement_end()) p++;
        break;
    default:
        if (isalpha(t)) st_let();          // LET is optional
        else error("SYNTAX ERROR");
    }
}

// Print "ERROR: <msg>", plus " IN LINE <n>" when num >= 0.
static void print_error(const char *msg, int num) {
    print_string("ERROR: ");
    print_string(msg);
    if (num >= 0) {
        char buf[8];
        itoa(num, buf);
        print_string(" IN LINE ");
        print_string(buf);
    }
    print_string("\n");
}

/*
 * Run statements from p until the end of the program (or of a typed
 * line, when cur_line is -1), an END, an error or BREAK.
 */
static void run() {
    running = 1;
    while (running) {
        if (cur_line >= 0 && check_break()) {
            char buf[8];
            itoa(line_num(cur_line), buf);
            print_string("BREAK IN LINE ");
            print_string(buf);
            print_string("\n");
            return;
        }
        error_msg = 0;
        pool_used = 0;
        jumped = 0;
        int line = cur_line;
        execute_statement();
        if (!error_msg && !jumped && running) {
            skip_space();
            if (*p == ':') {
                p++;
            } else if (*p == '\0') {
                if (cur_line < 0) return;   // end of a typed line
                cur_line = line_next(cur_line);
                if (cur_line >= prog_len) return;
                p = (char *)line_text(cur_line);
            } else {
                error("SYNTAX ERROR");
            }
        }
        if (error_msg) {
            print_error(error_msg, line >= 0 ? line_num(line) : -1);
            return;
        }
    }
}

/* ------------------------------------------------------------------------
 * The command line
 * --------------------------------------------------------------------- */

char input_buf[MAX_TEXT + 1];
unsigned char typed_line[MAX_TEXT + 1];   // tokenized; run from here

static void read_line() {
    int pos = 0;
    while (1) {
        char c = get_char();
        if (c == '\r' || c == '\n') {
            print_string("\n");
            input_buf[pos] = '\0';
            break;
        } else if (c == 8) {
            if (pos > 0) {
                pos--;
                print_char(8);
                print_char(' ');
                print_char(8);
            }
        } else if (pos < MAX_TEXT && c >= 32 && c < 127) {
            input_buf[pos++] = c;
            print_char(c);
        }
    }
}

void basic_run() {
    while (1) {
        read_line();
        char *s = input_buf;
        while (*s == ' ') s++;
        if (*s == '\0') continue;

        int num = -1;
        if (isdigit(*s)) {
            num = 0;
            while (isdigit(*s) && num <= 9999) num = num * 10 + (*s++ - '0');
            if (num < 1 || num > 9999) {
                print_error("LINE NUMBER MUST BE 1-9999", -1);
                continue;
            }
            while (*s == ' ') s++;
        }
        int len = tokenize(s, typed_line, sizeof(typed_line));
        if (len < 0) {
            print_error("LINE TOO LONG", num);
            continue;
        }
        if (num > 0) {
            // Editing the program invalidates any loops in progress.
            reset_run_state();
            if (len == 1) {
                line_delete(num);         // just a number: delete the line
            } else {
                const char *err = line_store(num, typed_line, len);
                if (err) print_error(err, num);
            }
        } else {
            cur_line = -1;
            p = (char *)typed_line;
            run();
        }
    }
}
