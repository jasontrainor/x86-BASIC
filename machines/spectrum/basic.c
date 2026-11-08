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

/* ------------------------------------------------------------------------
 * Expression values
 *
 * Strings produced while evaluating (by +, CHR$ etc.) live in a scratch
 * pool that is emptied at the start of every statement.
 * --------------------------------------------------------------------- */

#define MAX_STR 65535     // the longest string

struct value {
    int is_str;
    double num;
    const char *str;      // not NUL-terminated
    int len;
};

#define POOL_SIZE (64 * 1024)
HIGH_MEM char str_pool[POOL_SIZE];
int pool_used;

static char *pool_alloc(int len) {
    static char dummy[16];
    if (pool_used + len > POOL_SIZE) {
        error("4 Out of memory");
        return len <= (int)sizeof(dummy) ? dummy : 0;
    }
    char *s = str_pool + pool_used;
    pool_used += len;
    return s;
}

// Every number is held as the Spectrum would hold it (see num_spectrum).
static struct value num_value(double n) {
    if (!num_spectrum(&n)) error("6 Number too big");
    struct value v = { 0, n, 0, 0 };
    return v;
}

static struct value str_value(const char *s, int len) {
    struct value v = { 1, 0, s, len };
    return v;
}

static struct value expr();

// Results too big for the Spectrum's numbers (including infinity and
// NaN) are reported; the rest are rounded to its precision.
static double check(double v) {
    if (!num_spectrum(&v)) error("6 Number too big");
    return v;
}

static double expr_num() {
    struct value v = expr();
    if (v.is_str) error("C Nonsense in BASIC");
    return v.num;
}

// Convert to a whole number in [lo, hi], rounding to the nearest.
static int to_int(double v, int lo, int hi) {
    v = num_round(v);
    if (!(v >= lo && v <= hi)) {     // (also catches NaN)
        error("B Integer out of range");
        return lo;
    }
    return (int)v;
}

static int expr_int(int lo, int hi) {
    double v = expr_num();
    return error_msg ? lo : to_int(v, lo, hi);
}

/* ------------------------------------------------------------------------
 * Variables
 *
 * Simple variables are kept in a table. The text of strings, and the
 * elements of arrays, live in a heap of blocks. Each block records which
 * variable or array owns it; a block its owner no longer points to is
 * garbage, and when the heap fills up the live blocks are slid down
 * over it (compact()).
 * --------------------------------------------------------------------- */

#define NAME_LEN 32       // including the terminating 0
#define MAX_VARS 256

struct var {
    char name[NAME_LEN];  // "" for a free slot
    char is_str;
    char is_for;          // numbers: the control variable of a FOR loop
    double num;           // numbers
    char *str;            // strings: the text, in the heap (cap bytes)
    int len, cap;
    double limit, step;   // FOR: the loop's limit and step,
    int loop_line;        //   the line number it's on (-1: typed line)
    int loop_stmt;        //   and the statement number of the FOR
};
HIGH_MEM struct var vars[MAX_VARS];
int num_vars = 0;

/*
 * Arrays (DIM). Subscripts run from 1, as on the Spectrum. Numeric array
 * names are separate from simple variable names, so A and A() can both
 * exist. A string array is the Spectrum's: DIM A$(5,10) makes five
 * strings of exactly ten characters (the last subscript is the length),
 * and it replaces any simple string A$.
 */
#define MAX_ARRAYS 64
#define MAX_DIMS 8

struct array {
    char name[NAME_LEN];  // "" for a free slot
    char is_str;
    int ndims;
    int dims[MAX_DIMS];
    int count;            // elements (characters, for strings)
    char *data;           // in the heap
};
HIGH_MEM struct array arrays[MAX_ARRAYS];
int num_arrays = 0;

#define HEAP_SIZE (96 * 1024)
HIGH_MEM __attribute__((aligned(8))) unsigned char heap[HEAP_SIZE];
int heap_used = 0;

struct block {
    unsigned int size;              // bytes of data that follow (a multiple of 8)
    unsigned short owner_is_array;
    unsigned short owner;           // index in vars[] or arrays[]
};

static char **block_owner(struct block *b) {
    return b->owner_is_array ? &arrays[b->owner].data : &vars[b->owner].str;
}

static void compact() {
    unsigned int src = 0, dst = 0;
    while (src < (unsigned int)heap_used) {
        struct block *b = (struct block *)(heap + src);
        unsigned int total = sizeof(struct block) + b->size;
        char **owner = block_owner(b);
        if (*owner == (char *)(b + 1)) {          // still in use: keep it
            if (dst != src) memmove(heap + dst, heap + src, total);
            *owner = (char *)(heap + dst + sizeof(struct block));
            dst += total;
        }
        src += total;
    }
    heap_used = dst;
}

static int in_heap(const char *s) {
    return (const unsigned char *)s >= heap && (const unsigned char *)s < heap + HEAP_SIZE;
}

// A block of at least size bytes for vars[owner] or arrays[owner];
// 0 (with an error) if there's no room. Moves other blocks.
static char *heap_alloc(unsigned int size, int is_array, int owner) {
    size = (size + 7) & ~7u;
    unsigned int need = sizeof(struct block) + size;
    if (heap_used + need > HEAP_SIZE) compact();
    if (heap_used + need > HEAP_SIZE) {
        error("4 Out of memory");
        return 0;
    }
    struct block *b = (struct block *)(heap + heap_used);
    b->size = size;
    b->owner_is_array = is_array;
    b->owner = owner;
    heap_used += need;
    return (char *)(b + 1);
}

// Read a name at p: a letter, then letters and digits, then an optional
// $ for a string. As on the Spectrum, spaces within a name are ignored
// (LET hi score=0 sets HISCORE) and case doesn't matter. Returns 0 (with
// an error) if there isn't one.
/*
 * A keyword that can be used as a variable name: a single word that
 * can't start a value (not a function or operator), so that where a
 * value or a name is expected it can only be a name: LET to=1, FOR
 * step=1 TO 3, PRINT line. (On the Spectrum, names typed letter by
 * letter are never keywords; here keywords are typed as words.)
 */
static int is_name_token(unsigned char t) {
    if (!IS_TOKEN(t) || t >= T_LE) return 0;
    if ((t >= T_AND && t <= T_POINT) || t == T_FN || t == T_PEEK ||
        (t >= T_USR && t <= T_IN) || t == T_GOTO || t == T_GOSUB || t == T_DEFFN)
        return 0;
    return 1;
}

static int read_name(char *name, int *is_str) {
    skip_space();
    int n = 0;
    if (is_name_token(*p)) {              // a keyword as a name (see above)
        for (const char *k = token_name(*p++); *k; k++) name[n++] = *k;
    } else if (!isalpha(*p)) {
        error("C Nonsense in BASIC");
        return 0;
    }
    for (;;) {
        while (isalnum(*p)) {
            if (n == NAME_LEN - 1) {
                error("C Nonsense in BASIC");
                return 0;
            }
            name[n++] = toupper(*p++);
        }
        char *q = p;
        while (*q == ' ') q++;
        if (q == p || !isalnum(*q)) break;
        p = q;
    }
    name[n] = '\0';
    *is_str = (*p == '$');
    if (*is_str) p++;
    return 1;
}

static struct var *lookup_var(const char *name, int is_str) {
    for (int i = 0; i < num_vars; i++) {
        if (vars[i].is_str == is_str && vars[i].name[0] == name[0] &&
            strcmp(vars[i].name, name) == 0)
            return &vars[i];
    }
    return 0;
}

static struct var *find_var(const char *name, int is_str, int create) {
    struct var *found = lookup_var(name, is_str);
    if (found) return found;
    if (!create) {
        error("2 Variable not found");
        return 0;
    }
    int i = 0;
    while (i < num_vars && vars[i].name[0]) i++;   // reuse a free slot
    if (i == MAX_VARS) {
        error("4 Out of memory");
        return 0;
    }
    if (i == num_vars) num_vars++;
    struct var *v = &vars[i];
    memset(v, 0, sizeof(*v));
    strcpy(v->name, name);
    v->is_str = is_str;
    return v;
}

static void delete_var(struct var *v) {
    v->name[0] = '\0';
    v->str = 0;           // (its block is now garbage)
}

static struct array *find_array(const char *name, int is_str) {
    for (int i = 0; i < num_arrays; i++) {
        if (arrays[i].is_str == is_str && arrays[i].name[0] == name[0] &&
            strcmp(arrays[i].name, name) == 0)
            return &arrays[i];
    }
    return 0;
}

static void clear_variables() {
    num_vars = 0;
    num_arrays = 0;
    heap_used = 0;
}

// Set a simple string variable.
static void set_string(struct var *v, struct value val) {
    if (val.len > v->cap) {
        // Making room may move the heap, and the value could be in it.
        if (in_heap(val.str)) {
            char *copy = pool_alloc(val.len);
            if (error_msg) return;
            memcpy(copy, val.str, val.len);
            val.str = copy;
        }
        char *s = heap_alloc(val.len, 0, v - vars);
        if (!s) return;
        v->str = s;
        v->cap = (val.len + 7) & ~7;
    }
    memmove(v->str, val.str, val.len);
    v->len = val.len;
}

/*
 * Slicing, Spectrum style: the part inside the brackets of S$(3),
 * S$(2 TO 4), S$(TO 3), S$(2 TO) or S$() for a string of length len.
 * Sets *from and *to (1-based); from > to means an empty slice, which is
 * allowed even out of range. Stops at the ")" (not consumed).
 */
static int slice_spec(int len, int *from, int *to) {
    *from = 1;
    *to = len;
    skip_space();
    if (*p == ')') return 1;
    if (accept(T_TO)) {                   // (TO n), (TO)
        skip_space();
        if (*p != ')') *to = expr_int(0, 65535);
    } else {
        *from = expr_int(0, 65535);
        if (!accept(T_TO)) *to = *from;   // (n)
        else if (skip_space(), *p != ')') *to = expr_int(0, 65535);   // (n TO m)
    }
    if (error_msg) return 0;
    if (!checking && *from <= *to && (*from < 1 || *to > len)) {
        error("3 Subscript wrong");
        return 0;
    }
    return 1;
}

/*
 * When checking, the subscripts after a name (the "(" consumed): the
 * variable isn't looked at, so anything of the right form is taken: for
 * numbers, (i, j, ...); for strings, the same but each may be a slice
 * (i TO j, TO j, i TO, or nothing).
 */
static void check_subscripts(int is_str) {
    skip_space();
    if (*p != ')' || !is_str) {
        do {
            int to = is_str && accept(T_TO);
            skip_space();
            if (*p == ',' || *p == ')') {
                if (!is_str) error("C Nonsense in BASIC");
                continue;
            }
            expr_num();
            if (!to && is_str && accept(T_TO)) {
                skip_space();
                if (*p != ',' && *p != ')') expr_num();
            }
        } while (!error_msg && accept_char(','));
    }
    expect_char(')', "C Nonsense in BASIC");
}

// Parse the subscripts "i, j, ...)" of an element of numeric array a
// (the "(" already consumed). Returns the element number, or -1 after
// an error.
static int element_index(struct array *a) {
    int idx = 0;
    for (int k = 0; k < a->ndims; k++) {
        if (k > 0 && !accept_char(',')) {
            error("3 Subscript wrong");     // too few subscripts
            return -1;
        }
        double v = expr_num();
        if (error_msg) return -1;
        v = num_round(v);
        if (!(v >= 1 && v <= a->dims[k])) {
            error("3 Subscript wrong");
            return -1;
        }
        idx = idx * a->dims[k] + (int)v - 1;
    }
    if (!accept_char(')')) {
        error("3 Subscript wrong");         // too many subscripts
        return -1;
    }
    return idx;
}

/*
 * The characters of string array a that A$, A$(i, j) or A$(i, j, slice)
 * refers to (p is just after the name): all of them, one of its strings,
 * or part of one. Returns 0 after an error.
 */
static int string_array_part(struct array *a, char **ptr, int *len) {
    *ptr = a->data;
    *len = a->count;
    if (!accept_char('(')) return 1;
    int last = a->ndims - 1;      // the last subscript is within a string
    int idx = 0;
    for (int k = 0; k < last; k++) {
        if (k > 0 && !accept_char(',')) {
            error("3 Subscript wrong");
            return 0;
        }
        int i = expr_int(1, a->dims[k]);
        if (error_msg) return 0;
        idx = idx * a->dims[k] + i - 1;
    }
    *ptr = a->data + idx * a->dims[last];
    *len = a->dims[last];
    if (last > 0 && accept_char(')')) return 1;   // a whole string
    if (last > 0 && !accept_char(',')) {
        error("3 Subscript wrong");
        return 0;
    }
    int from, to;
    if (!slice_spec(*len, &from, &to)) return 0;
    if (!accept_char(')')) {
        error("3 Subscript wrong");
        return 0;
    }
    *ptr += from - 1;
    *len = from > to ? 0 : to - from + 1;
    return 1;
}

/*
 * Something that can be assigned to: a numeric variable or element, a
 * simple string variable, or a fixed-length string: part of a string
 * (LET A$(2 TO 4)="XYZ") or of a string array. As on the Spectrum, a
 * fixed-length string is filled with the value, cut short or padded with
 * spaces. A simple variable that doesn't exist yet is only created when
 * the value is stored, so LET A=A+1 with no A is still an error.
 */
struct lvalue {
    int is_str;
    char name[NAME_LEN];
    double *num;          // numbers: where the value goes (0: a new variable)
    struct var *var;      // simple strings (0: a new variable)
    char *fixed;          // fixed-length strings: where the text goes
    int fixed_len;        //   and its length; -1 if not one
};

static int parse_lvalue(struct lvalue *lv) {
    lv->num = 0;
    lv->var = 0;
    lv->fixed = 0;
    lv->fixed_len = -1;
    if (!read_name(lv->name, &lv->is_str)) return 0;
    skip_space();
    if (checking) {
        if (accept_char('(')) check_subscripts(lv->is_str);
        return !error_msg;
    }
    if (!lv->is_str) {
        if (*p == '(') {
            struct array *a = find_array(lv->name, 0);
            if (!a) {
                error("2 Variable not found");
                return 0;
            }
            p++;
            int i = element_index(a);
            if (i < 0) return 0;
            lv->num = (double *)a->data + i;
        } else {
            struct var *v = lookup_var(lv->name, 0);
            if (v) lv->num = &v->num;
        }
        return 1;
    }
    struct array *a = find_array(lv->name, 1);
    if (a) return string_array_part(a, &lv->fixed, &lv->fixed_len);
    lv->var = lookup_var(lv->name, 1);
    if (*p == '(') {                      // LET A$(2 TO 3)=...
        if (!lv->var) {
            error("2 Variable not found");
            return 0;
        }
        p++;
        int from, to;
        if (!slice_spec(lv->var->len, &from, &to)) return 0;
        if (!accept_char(')')) {
            error("C Nonsense in BASIC");
            return 0;
        }
        lv->fixed = lv->var->str + from - 1;
        lv->fixed_len = from > to ? 0 : to - from + 1;
    }
    return 1;
}

static void store(struct lvalue *lv, struct value val) {
    if (error_msg) return;
    if (lv->is_str != val.is_str) {
        error("C Nonsense in BASIC");
        return;
    }
    if (checking) return;
    if (!lv->is_str) {
        if (!lv->num) {                    // a new simple variable
            struct var *v = find_var(lv->name, 0, 1);
            if (!v) return;
            lv->num = &v->num;
        }
        *lv->num = val.num;
    } else if (lv->fixed_len >= 0) {
        int n = val.len < lv->fixed_len ? val.len : lv->fixed_len;
        // memmove: the value may overlap the target (A$(2 TO)=A$)
        memmove(lv->fixed, val.str, n);
        memset(lv->fixed + n, ' ', lv->fixed_len - n);
    } else {
        struct var *v = lv->var ? lv->var : find_var(lv->name, 1, 1);
        if (v) set_string(v, val);
    }
}

/* ------------------------------------------------------------------------
 * Random numbers, as on the Spectrum: a 16-bit seed that starts at 0 when
 * the computer is switched on, so a program that doesn't use RANDOMIZE
 * gets the same "random" numbers every time, just as it would there.
 * --------------------------------------------------------------------- */

unsigned int rnd_seed = 0;

// RANDOMIZE n; 0 takes the seed from the clock.
static void randomize(unsigned int seed) {
    rnd_seed = seed ? seed : (unsigned int)(timer_ms() / 20) & 0xFFFF;
}

static double rnd() {
    if (checking) return 0;
    rnd_seed = (75 * (rnd_seed + 1)) % 65537 - 1;
    return rnd_seed / 65536.0;            // 0 <= RND < 1
}

int break_requested;      // Esc was seen by INKEY$ or INPUT

/* ------------------------------------------------------------------------
 * Running at the Spectrum's speed
 *
 * A PC runs BASIC thousands of times faster than a Spectrum, which would
 * change what programs do: delay loops, games, animation. So the time
 * each statement would take on a Spectrum is added up (roughly: from
 * the kind of statement and the arithmetic, functions, printing and
 * drawing it does) and the program is held back to match. POKE 23681,1
 * (a system variable the Spectrum leaves unused) runs at full speed.
 *
 * The figures are from benchmark timings of Spectrum BASIC: about 3.5ms
 * a statement, a FOR...NEXT loop round in 4.4ms, multiplying 2.5ms,
 * dividing 3.5ms, SQR 35ms.
 * --------------------------------------------------------------------- */

#define SPEED_SYSVAR 23681
#define STATEMENT_US 3500
unsigned long cost_us;    // Spectrum time used since the last pace()
int then_part;            // set by IF: the next statement is its THEN part
static long ahead_us;     // how far the Spectrum's time is ahead of ours
static unsigned long pace_ms;
extern unsigned long screen_cost_us;   // (screen.c: printing and drawing)

static void pace_start() {
    cost_us = screen_cost_us = 0;
    ahead_us = 0;
    pace_ms = timer_ms();
}

static void pace() {
    unsigned long now = timer_ms();
    ahead_us += (long)(cost_us + screen_cost_us) - (long)(now - pace_ms) * 1000;
    cost_us = screen_cost_us = 0;
    pace_ms = now;
    if (mem_peek(SPEED_SYSVAR) & 1) {     // full speed
        ahead_us = 0;
        return;
    }
    // If the PC fell behind (waiting for a key, say), don't make it up
    // later by running fast; a little slack evens out the timer's steps.
    if (ahead_us < -60000) ahead_us = -60000;
    while (ahead_us > 0) {
        if (key_waiting() && check_break()) {   // (stop at once)
            break_requested = 1;
            ahead_us = 0;
            return;
        }
#ifndef HOST_TEST                         // (the tests' clock moves on by itself)
        plat_idle();
#endif
        now = timer_ms();
        ahead_us -= (long)(now - pace_ms) * 1000;
        pace_ms = now;
    }
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
    error("A Invalid argument");
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
    if (v.is_str) error("C Nonsense in BASIC");
    return v.num;
}

__attribute__((noinline)) static struct value numeric_function(int t) {
    double x = num_arg();
    if (error_msg) return num_value(0);
    cost_us += t == T_SQR ? 35000 : t == T_TAN ? 25000 :
               (t == T_ABS || t == T_INT || t == T_SGN) ? 1000 : 15000;
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

static struct value *fn_param(const char *name, int is_str);
static int eval_text(const char *str, int len, struct value *out);
static struct value copy_to_pool(const char *s, int len);

static struct value variable_value() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return num_value(0);
    skip_space();
    if (checking) {
        if (accept_char('(')) check_subscripts(is_str);
        return is_str ? str_value("", 0) : num_value(0);
    }
    struct value *param = fn_param(name, is_str);
    if (param) return *param;
    if (is_str) {
        struct array *a = find_array(name, 1);
        if (a) {
            char *s;
            int len;
            if (!string_array_part(a, &s, &len)) return str_value("", 0);
            return str_value(s, len);
        }
        // A simple string followed by ( is sliced: see factor().
    } else if (*p == '(') {
        struct array *a = find_array(name, 0);
        if (!a) {
            error("2 Variable not found");
            return num_value(0);
        }
        p++;
        int i = element_index(a);
        if (i < 0) return num_value(0);
        return num_value(((double *)a->data)[i]);
    }
    struct var *v = find_var(name, is_str, 0);
    if (!v) return num_value(0);
    return is_str ? str_value(v->str, v->len) : num_value(v->num);
}

// Slice a string value (the "(" not yet consumed).
__attribute__((noinline)) static struct value slice(struct value s) {
    p++;
    int from, to;
    if (!slice_spec(s.len, &from, &to)) return str_value("", 0);
    expect_char(')', "C Nonsense in BASIC");
    if (error_msg || from > to) return str_value("", 0);
    return str_value(s.str + from - 1, to - from + 1);
}

static struct value str_arg() {
    struct value v = factor();
    if (!error_msg && !v.is_str) error("C Nonsense in BASIC");
    return v;
}

// VAL: evaluate a string as a numeric expression.
/*
 * Work out the text s (as typed, not tokenized) as an expression, for
 * VAL and INPUT. Returns 0 if it isn't a valid expression (leaving
 * error_msg set).
 */
static int eval_text(const char *str, int len, struct value *out) {
    char *text = pool_alloc(len + 1);
    unsigned char *tokens = (unsigned char *)pool_alloc(len + 2);
    if (error_msg) return 0;
    memcpy(text, str, len);
    text[len] = '\0';
    if (tokenize(text, tokens, len + 2) < 0) { error("C Nonsense in BASIC"); return 0; }
    char *saved = p;
    p = (char *)tokens;
    *out = expr();
    skip_space();
    if (*p && !error_msg) error("C Nonsense in BASIC");
    p = saved;
    return !error_msg;
}

// VAL: work out a string as a numeric expression.
static double val(struct value s) {
    struct value v;
    if (!eval_text(s.str, s.len, &v)) return 0;
    if (v.is_str) { error("C Nonsense in BASIC"); return 0; }
    return v.num;
}
static struct value copy_to_pool(const char *s, int len) {
    char *d = pool_alloc(len);
    memcpy(d, s, len);
    return str_value(d, len);
}

__attribute__((noinline)) static struct value string_function(int t) {
    struct value v;
    char buf[24];
    switch (t) {
    case T_LEN:
        v = str_arg();
        return num_value(v.len);
    case T_CODE:
        v = str_arg();
        return num_value(v.len ? (unsigned char)v.str[0] : 0);
    case T_VAL:
        v = str_arg();
        return num_value(error_msg || checking ? 0 : val(v));
    case T_CHRS:
        buf[0] = to_int(num_arg(), 0, 255);
        return copy_to_pool(buf, 1);
    case T_STRS:
        num_format(num_arg(), buf);
        return copy_to_pool(buf, strlen(buf));
    case T_INKEYS: {
        if (checking) return str_value("", 0);
        int k = poll_key();
        if (k == 27) {            // Esc: leave it to stop the program
            break_requested = 1;
            k = -1;
        }
        if (k < 0) return str_value("", 0);
        buf[0] = k;
        return copy_to_pool(buf, 1);
    }
    }
    return num_value(0);
}

/*
 * DEF FN and FN. A call FN F(args) finds the DEF FN F in the program,
 * gives its parameters the argument values and works out its expression.
 * While it does, the parameters hide any variables of the same names.
 */
#define MAX_PARAMS 8
#define MAX_FN_DEPTH 32

struct fn_frame {
    int count;
    char name[MAX_PARAMS][NAME_LEN];
    char is_str[MAX_PARAMS];
    struct value val[MAX_PARAMS];
};
HIGH_MEM struct fn_frame fn_frames[MAX_FN_DEPTH];
int fn_depth;             // frames in use; the innermost one is visible

// A parameter of the function being worked out, or 0.
static struct value *fn_param(const char *name, int is_str) {
    if (!fn_depth) return 0;
    struct fn_frame *f = &fn_frames[fn_depth - 1];
    for (int i = 0; i < f->count; i++)
        if (f->is_str[i] == is_str && strcmp(f->name[i], name) == 0) return &f->val[i];
    return 0;
}

// Find DEF FN name: p is left just after its name. Returns 0 if none.
static int find_def_fn(const char *name, int is_str) {
    for (int off = 0; off < prog_len; off = line_next(off)) {
        unsigned char *q = line_text(off);
        while ((q = find_token_in(q, T_DEFFN)) != 0) {
            p = (char *)q + 1;
            char n[NAME_LEN];
            int s;
            if (read_name(n, &s) && s == is_str && strcmp(n, name) == 0) return 1;
            error_msg = 0;
            q++;
        }
    }
    return 0;
}

__attribute__((noinline)) static struct value fn_call() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return num_value(0);
    if (fn_depth == MAX_FN_DEPTH) {
        error("4 Out of memory");
        return num_value(0);
    }
    struct fn_frame *f = &fn_frames[fn_depth];
    // The arguments, worked out here (before the frame is in use).
    struct value args[MAX_PARAMS];
    int nargs = 0;
    expect_char('(', "C Nonsense in BASIC");
    skip_space();
    if (*p != ')') {
        do {
            if (nargs == MAX_PARAMS) {
                error("Q Parameter error");
                if (!checking) break;
                expr();
                continue;
            }
            args[nargs++] = expr();
        } while (!error_msg && accept_char(','));
    }
    expect_char(')', "C Nonsense in BASIC");
    if (error_msg) return num_value(0);
    if (checking) return is_str ? str_value("", 0) : num_value(0);

    char *saved = p;
    if (!find_def_fn(name, is_str)) {
        p = saved;
        error("P FN without DEF");
        return num_value(0);
    }
    // DEF FN F(A, B$)=expression
    f->count = 0;
    expect_char('(', "C Nonsense in BASIC");
    skip_space();
    if (*p != ')') {
        do {
            int s;
            if (f->count == MAX_PARAMS) { error("Q Parameter error"); break; }
            if (!read_name(f->name[f->count], &s)) break;
            f->is_str[f->count] = s;
            if (f->count >= nargs || args[f->count].is_str != s) {
                error("Q Parameter error");
                break;
            }
            f->val[f->count] = args[f->count];
            f->count++;
        } while (accept_char(','));
    }
    expect_char(')', "C Nonsense in BASIC");
    if (!error_msg && f->count != nargs) error("Q Parameter error");
    expect_char('=', "C Nonsense in BASIC");
    struct value v = num_value(0);
    if (!error_msg) {
        fn_depth++;
        v = expr();
        fn_depth--;
        if (!error_msg && v.is_str != is_str)
            error(is_str ? "C Nonsense in BASIC" : "C Nonsense in BASIC");
    }
    p = saved;
    return v;
}

// IN port: only the keyboard is there, read as the Spectrum's: the high
// byte selects half-rows of keys (a 0 bit each), and a key held down
// reads as a 0 in bits 0-4. Anything else reads 255.
static int in_port(int port) {
    static const char *const half_rows[8] = {
        "\001ZXCV", "ASDFG", "QWERT", "12345", "09876", "POIUY", "\rLKJH", " \002MNB",
    };
    if (port & 1) return 255;
    int k = held_key(), bits = 0x1F;
    if (k >= 0) {
        char c = k == 13 ? '\r' : toupper(k);
        for (int r = 0; r < 8; r++) {
            if (port & (0x100 << r)) continue;
            for (int b = 0; b < 5; b++)
                if (half_rows[r][b] == c) bits &= ~(1 << b);
        }
    }
    return 0xE0 | bits;
}

int reset_requested;      // USR 0: start again, as if switched on

// (row, col) for ATTR and SCREEN$
static int cell_args(int *row, int *col) {
    expect_char('(', "C Nonsense in BASIC");
    *row = expr_int(0, SCREEN_ROWS - 1);
    expect_char(',', "C Nonsense in BASIC");
    *col = expr_int(0, SCREEN_COLS - 1);
    expect_char(')', "C Nonsense in BASIC");
    return !error_msg;
}

__attribute__((noinline)) static struct value spectrum_function(int t) {
    int row, col;
    switch (t) {
    case T_PEEK:
        return num_value(mem_peek(to_int(num_arg(), 0, 65535)));
    case T_IN:
        return num_value(in_port(to_int(num_arg(), 0, 65535)));
    case T_BIN: {                          // BIN 10110: a number in binary
        double n = 0;
        skip_space();
        while (*p == '0' || *p == '1') n = n * 2 + (*p++ - '0');
        return num_value(n);
    }
    case T_USR: {
        struct value v = factor();
        if (error_msg) return num_value(0);
        if (v.is_str) {                    // USR "a": where UDG a is
            int c = v.len == 1 ? (unsigned char)v.str[0] : 0;
            if (c >= 'A' && c <= 'U') c += 'a' - 'A';
            if (c >= 144 && c <= 164) c = c - 144 + 'a';
            if (c < 'a' || c > 'u') { error("A Invalid argument"); return num_value(0); }
            return num_value(mem_peek16(SYS_UDG) + (c - 'a') * 8);
        }
        if (checking) return num_value(0);
        if (to_int(v.num, 0, 65535) == 0 && !error_msg) {
            reset_requested = 1;           // USR 0 resets the Spectrum
            return num_value(0);
        }
        if (!error_msg) error("A Machine code not supported");
        return num_value(0);
    }
    case T_VALS: {                         // VAL$ "expression": a string
        struct value v = str_arg(), r;
        if (error_msg || checking) return str_value("", 0);
        if (!eval_text(v.str, v.len, &r)) return str_value("", 0);
        if (!r.is_str) { error("C Nonsense in BASIC"); return str_value("", 0); }
        return r;
    }
    case T_ATTR:
        if (!cell_args(&row, &col)) return num_value(0);
        return num_value(screen_attr(row, col));
    case T_SCREENS: {
        if (!cell_args(&row, &col)) return str_value("", 0);
        int c = screen_char(row, col);
        if (c < 0) return str_value("", 0);
        char ch = c;
        return copy_to_pool(&ch, 1);
    }
    }
    return num_value(0);
}

static struct value primary();

/*
 * Brackets and functions nest by recursion, and the BIOS kernel's stack
 * is small (see core/bios.ld), so deep nesting is stopped with an error
 * rather than letting the stack overflow.
 */
#if defined(BIOS_KERNEL)
#define STACK_BUDGET (36 * 1024)      // of the 40KB in core/bios.ld
#elif defined(UEFI_KERNEL)
#define STACK_BUDGET (64 * 1024)      // (UEFI promises at least 128KB)
#else
#define STACK_BUDGET (1024 * 1024)    // tests: frames are big with sanitizers
#endif
char *stack_base;         // the stack at the start, set by basic_run()

static int stack_full() {
    char *here = __builtin_frame_address(0);
    if (stack_base - here > STACK_BUDGET) {
        error("4 Out of memory");
        return 1;
    }
    return 0;
}

static struct value factor() {
    if (stack_full()) return num_value(0);
    struct value v = primary();
    while (!error_msg && v.is_str && accept_char('(')) {
        p--;
        v = slice(v);
    }
    return v;
}

static struct value primary() {
    if (error_msg) return num_value(0);
    skip_space();
    unsigned char c = *p;
    double n;
    if (num_parse((const char **)&p, &n)) {
        return num_value(check(n));
    } else if (c == '"') {              // "text", with "" for a quote
        const char *start = ++p;
        int quotes = 0;
        while (*p && (*p != '"' || p[1] == '"')) {
            if (*p == '"') { quotes++; p++; }
            p++;
        }
        struct value v = str_value(start, p - start);
        if (*p == '"') p++;
        if (quotes) {
            char *d = pool_alloc(v.len - quotes);
            if (error_msg) return str_value("", 0);
            int n = 0;
            for (int i = 0; i < v.len; i++) {
                d[n++] = start[i];
                if (start[i] == '"') i++;
            }
            v = str_value(d, n);
        }
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
        expect_char(')', "C Nonsense in BASIC");
        return v;
    } else if (c == T_PI) {
        p++;
        return num_value(3.14159265358979323846);
    } else if (c >= T_ABS && c <= T_EXP) {
        p++;
        return numeric_function(c);
    } else if (c >= T_LEN && c <= T_INKEYS) {
        p++;
        return string_function(c);
    } else if (c == T_RND) {
        p++;
        cost_us += 5000;
        return num_value(rnd());
    } else if (c == T_POINT) {             // POINT (x,y): 1 if ink there
        p++;
        expect_char('(', "C Nonsense in BASIC");
        int x = expr_int(0, SCREEN_WIDTH - 1);
        expect_char(',', "C Nonsense in BASIC");
        int y = expr_int(0, PLOT_HEIGHT - 1);
        expect_char(')', "C Nonsense in BASIC");
        return num_value(error_msg ? 0 : point(x, y));
    } else if (c == T_FN) {
        p++;
        return fn_call();
    } else if (c >= T_PEEK && c <= T_IN && c != T_POKE) {
        p++;
        return spectrum_function(c);
    } else if (isalpha(c) || is_name_token(c)) {
        return variable_value();
    }
    error("C Nonsense in BASIC");
    return num_value(0);
}

// factor ^ factor ^ ...   (left to right, as on the Spectrum)
static struct value power_expr() {
    struct value v = factor();
    while (!error_msg && accept_char('^')) {
        double y = num_arg();
        if (v.is_str) error("C Nonsense in BASIC");
        cost_us += 15000;
        if (!error_msg) v = num_value(power(v.num, y));
    }
    return v;
}

// A leading minus applies after ^, so -2^2 is -4.
static struct value unary() {
    if (accept_char('-')) {
        struct value v = unary();
        if (v.is_str) error("C Nonsense in BASIC");
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
        if (v.is_str || w.is_str) { error("C Nonsense in BASIC"); break; }
        cost_us += op == '*' ? 2500 : 3500;
        if (op == '*') {
            v.num = check(v.num * w.num);
        } else if (w.num == 0) {
            error("6 Number too big");
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
            error(v.is_str ? "C Nonsense in BASIC" : "C Nonsense in BASIC");
            break;
        }
        cost_us += 800;
        if (v.is_str && checking) {
            v = str_value("", 0);          // (only the type matters)
        } else if (v.is_str) {             // concatenation
            if (v.len + w.len > MAX_STR) { error("4 Out of memory"); break; }
            char *s = pool_alloc(v.len + w.len);
            if (error_msg) break;
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
            error(v.is_str ? "C Nonsense in BASIC" : "C Nonsense in BASIC");
            break;
        }
        cost_us += 800;
        int c = compare(v, w);
        int r = op == '=' ? c == 0 : op == '<' ? c < 0 : op == '>' ? c > 0 :
                op == T_LE ? c <= 0 : op == T_GE ? c >= 0 : c != 0;
        v = num_value(r);
    }
    return v;
}

// NOT binds more loosely than comparisons: NOT A=B is NOT (A=B).
static struct value not_expr() {
    if (accept(T_NOT)) {
        struct value v = not_expr();
        if (v.is_str) error("C Nonsense in BASIC");
        return num_value(v.num == 0);
    }
    return comparison();
}

// As on the Spectrum: X AND Y is X if Y is true (non-zero), else 0
// (or "" for a string X); X OR Y is 1 if Y is true, else X.
static struct value and_expr() {
    struct value v = not_expr();
    while (!error_msg && accept(T_AND)) {
        struct value w = not_expr();
        if (w.is_str) { error("C Nonsense in BASIC"); break; }
        if (w.num == 0) v = v.is_str ? str_value("", 0) : num_value(0);
    }
    return v;
}

static struct value expr() {
    struct value v = and_expr();
    while (!error_msg && accept(T_OR)) {
        struct value w = and_expr();
        if (v.is_str || w.is_str) { error("C Nonsense in BASIC"); break; }
        if (w.num != 0) v = num_value(1);
    }
    return v;
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

/*
 * Places in the program (for FOR loops, GO SUB, CONTINUE) are kept as a
 * line number and a statement number, as on the Spectrum, so that they
 * stay valid when the program is edited: STOP, change a line, CONTINUE.
 * Line -1 is the typed command line. Statements are numbered from 1 and
 * separated by ":" and by THEN.
 */
extern unsigned char typed_line[];

// The end of the statement at q: the next ":" or THEN outside strings,
// or the end of the line. (After REM, the rest of the line is a comment.)
static char *statement_end(char *q) {
    int in_str = 0;
    for (; *q; q++) {
        unsigned char c = *q;
        if (c == '"') {
            in_str = !in_str;
        } else if (!in_str) {
            if (c == ':' || c == T_THEN) return q;
            if (c == T_REM) {
                while (*q) q++;
                return q;
            }
        }
    }
    return q;
}

// The text of the running line (or the typed line).
static char *current_text() {
    return cur_line >= 0 ? (char *)line_text(cur_line) : (char *)typed_line;
}

static int current_line_number() {
    return cur_line >= 0 ? line_num(cur_line) : -1;
}

// The number of the statement that pos (in the current line) is in.
static int statement_number(char *pos) {
    int k = 1;
    for (char *q = statement_end(current_text()); *q && q < pos; q = statement_end(q + 1))
        k++;
    return k;
}

/*
 * Carry on at statement k of line num, or just after it if after is 1.
 * As on the Spectrum, it's an error if the line has gone.
 */
static void resume(int num, int k, int after) {
    if (num >= 0) {
        int off = line_find(num);
        if (off >= prog_len || line_num(off) != num) {
            error("N Statement lost");
            return;
        }
        cur_line = off;
    } else {
        cur_line = -1;
    }
    char *q = current_text();
    for (int i = 1; i < k && *q; i++) {
        q = statement_end(q);
        if (*q) q++;
    }
    if (after) {
        p = statement_end(q);   // the run loop moves on to what follows
        jumped = 0;
    } else {
        p = q;
        jumped = 1;
    }
}

struct gosub {
    int line, stmt;       // the GO SUB statement, to RETURN to
};
#define MAX_GOSUB 1024
HIGH_MEM struct gosub gosub_stack[MAX_GOSUB];
int gosub_sp = 0;

// Where READ takes its next item from: in the DATA statement at
// data_pos if in_data, otherwise the next DATA from data_pos on (from
// the start of the line at data_line if data_pos is 0).
int data_line;
char *data_pos;
int in_data;

// Where CONTINUE carries on: statement cont_stmt of line cont_line (or
// the one after it, if cont_next), if cont_ok.
int cont_ok, cont_line, cont_stmt, cont_next;

// The statement being run, for errors and BREAK.
int stmt_line;
char *stmt_pos;

static void restore(int num) {
    data_line = line_find(num);
    data_pos = 0;
    in_data = 0;
}

static void reset_run_state() {
    gosub_sp = 0;
    restore(0);
}

// Editing the program moves lines about: data_mark() notes where READ
// is up to as a line number and offset, and data_unmark() finds it again.
static int data_num, data_off;

static void data_mark() {
    data_num = data_line < prog_len ? line_num(data_line) : -1;
    data_off = data_pos ? data_pos - (char *)line_text(data_line) : -1;
}

static void data_unmark() {
    if (data_num < 0) {
        data_line = prog_len;
        return;
    }
    data_line = line_find(data_num);
    if (data_line < prog_len && line_num(data_line) == data_num && data_off >= 0 &&
        data_off < line_len(data_line)) {
        data_pos = (char *)line_text(data_line) + data_off;
    } else {
        data_pos = 0;
        in_data = 0;
    }
}

// Continue at the start of line number num or, as on the Spectrum, the
// first line after it if there's no such line. Past the last line, the
// program ends.
static void go_to_line(int num) {
    if (error_msg || checking) return;
    int off = line_find(num);
    jumped = 1;
    if (off >= prog_len) {
        running = 0;
        return;
    }
    cur_line = off;
    p = (char *)line_text(off);
    jumped = 1;
}

// [LET] target = expression   (the LET itself already consumed)
static void st_let() {
    struct lvalue lv;
    if (!parse_lvalue(&lv)) return;
    if (!accept_char('=')) { error("C Nonsense in BASIC"); return; }
    struct value val = expr();
    store(&lv, val);
}

/*
 * Characters 165-255 print as the Spectrum's keywords, as they do there
 * (its keywords are single characters with those codes).
 */
static const char *const spectrum_keywords[] = {
    "RND", "INKEY$", "PI", "FN", "POINT", "SCREEN$", "ATTR", "AT", "TAB",
    "VAL$", "CODE", "VAL", "LEN", "SIN", "COS", "TAN", "ASN", "ACS", "ATN",
    "LN", "EXP", "INT", "SQR", "SGN", "ABS", "PEEK", "IN", "USR", "STR$",
    "CHR$", "NOT", "BIN", "OR", "AND", "<=", ">=", "<>", "LINE", "THEN",
    "TO", "STEP", "DEF FN", "CAT", "FORMAT", "MOVE", "ERASE", "OPEN #",
    "CLOSE #", "MERGE", "VERIFY", "BEEP", "CIRCLE", "INK", "PAPER", "FLASH",
    "BRIGHT", "INVERSE", "OVER", "OUT", "LPRINT", "LLIST", "STOP", "READ",
    "DATA", "RESTORE", "NEW", "BORDER", "CONTINUE", "DIM", "REM", "FOR",
    "GO TO", "GO SUB", "INPUT", "LOAD", "LIST", "LET", "PAUSE", "NEXT",
    "POKE", "PRINT", "PLOT", "RUN", "SAVE", "RANDOMIZE", "IF", "CLS", "DRAW",
    "CLEAR", "RETURN", "COPY",
};
_Static_assert(sizeof(spectrum_keywords) / sizeof(spectrum_keywords[0]) == 256 - 165,
               "spectrum_keywords should cover 165-255");

/*
 * The printer (LPRINT, LLIST, COPY, PRINT #3): what's printed goes to
 * the text file PRINTOUT.TXT on the FAT partition, 32 columns wide as on
 * the ZX Printer. It's collected here and added to the end of the file
 * when a command or program finishes (or this fills up). Characters a
 * text file can't hold come out as UTF-8 (the pound and copyright signs
 * and block graphics) or, for UDGs, as the letters they look like until
 * they're redefined.
 */
#define PRINTER_SIZE 4096
#define PRINTER_FILE "PRINTOUT.TXT"
HIGH_MEM char printer_buf[PRINTER_SIZE];
int printer_len, printer_col;
int to_printer;           // PRINT is printing on the printer

static void printer_flush();

static void printer_put(const char *bytes) {
    for (; *bytes; bytes++) {
        if (printer_len == PRINTER_SIZE) printer_flush();
        printer_buf[printer_len++] = *bytes;
    }
}

static void printer_char(unsigned char c) {
    // Unicode quadrant blocks, for CHR$ 128-143 (bits: 1 top right,
    // 2 top left, 4 bottom right, 8 bottom left).
    static const char *const blocks[16] = {
        " ", "\xE2\x96\x9D", "\xE2\x96\x98", "\xE2\x96\x80", "\xE2\x96\x97", "\xE2\x96\x90",
        "\xE2\x96\x9A", "\xE2\x96\x9C", "\xE2\x96\x96", "\xE2\x96\x9E", "\xE2\x96\x8C",
        "\xE2\x96\x9B", "\xE2\x96\x84", "\xE2\x96\x9F", "\xE2\x96\x99", "\xE2\x96\x88",
    };
    char one[2] = { c, 0 };
    if (c == '\n') {
        printer_put("\n");
        printer_col = 0;
        return;
    }
    if (printer_col == SCREEN_COLS) {    // the paper is 32 characters wide
        printer_put("\n");
        printer_col = 0;
    }
    if (c == 96) printer_put("\xC2\xA3");                 // £
    else if (c == 127) printer_put("\xC2\xA9");           // ©
    else if (c >= 128 && c <= 143) printer_put(blocks[c - 128]);
    else if (c >= 144 && c <= 164) { one[0] = 'A' + c - 144; printer_put(one); }
    else if (c < 32 || c > 164) printer_put(" ");
    else printer_put(one);
    printer_col++;
}

static void printer_flush() {
    if (!printer_len) return;
    int len = 0, n = printer_len;
    printer_len = 0;
    if (fat_read(PRINTER_FILE, text_buf, TEXT_SIZE, &len)) len = 0;   // (not there yet)
    if (len + n > TEXT_SIZE) {            // too long: keep the end of it
        int drop = len + n - TEXT_SIZE;
        memmove(text_buf, text_buf + drop, len - drop);
        len -= drop;
    }
    memcpy(text_buf + len, printer_buf, n);
    fat_write(PRINTER_FILE, text_buf, len + n);   // (no FAT partition: it's lost)
}

// Printing, on the screen or the printer.
static void out_char(unsigned char c) {
    if (to_printer) printer_char(c);
    else print_char(c);
}

static void out_string(const char *s) {
    while (*s) out_char(*s++);
}

static int out_col() {
    return to_printer ? printer_col : cursor_col();
}

static void print_value(struct value v) {
    if (v.is_str) {
        for (int i = 0; i < v.len; i++) {
            unsigned char c = v.str[i];
            if (c >= 165) {
                const char *k = spectrum_keywords[c - 165];
                out_string(k);
                if (isalpha(k[0])) out_char(' ');
            } else {
                out_char(c);
            }
        }
    } else {
        char buf[24];
        num_format(v.num, buf);
        out_string(buf);
    }
}

static void print_spaces_to(int col) {
    while (out_col() < col) out_char(' ');
}

// , in PRINT: on to the next half of the line.
static void print_comma() {
    int next = (out_col() / 16 + 1) * 16;
    if (next >= SCREEN_COLS) out_char('\n');
    else print_spaces_to(next);
}

// TAB col: on to that column, on the next line if it's already past.
static void print_tab(int col) {
    col %= SCREEN_COLS;
    if (col < out_col()) out_char('\n');
    print_spaces_to(col);
}

// AT row,col (the AT already consumed): 0 after an error.
static int print_at(int really) {
    int row = expr_int(0, 255);
    expect_char(',', "C Nonsense in BASIC");
    int col = expr_int(0, 255);
    if (error_msg) return 0;
    if (checking) return 1;
    if (col >= SCREEN_COLS) { error("B Integer out of range"); return 0; }
    if (really && to_printer) print_tab(col);     // (the printer only has columns)
    else if (really && !set_cursor(col, row)) { error("5 Out of screen"); return 0; }
    return 1;
}
/*
 * PRINT items separated by ; (nothing between) or , (move to the next
 * half-screen column). AT row,col and TAB col move the cursor. A
 * trailing ; or , leaves the cursor after the text.
 */
static int colour_item(struct colours *c);

/*
 * PRINT [#stream;] items, separated by ; (nothing between), , (on to the
 * next half of the line) or ' (a new line). AT row,col and TAB col move
 * the print position. Colour items (PRINT INK 2;"RED") last for this
 * PRINT only, as on the Spectrum. A trailing ; or , leaves the position
 * after the text.
 *
 * Streams 0 and 1 are the lower screen, 2 the upper screen (the default)
 * and 3 the printer (LPRINT).
 */
static void st_print(int stream) {
    int newline = 1;
    if (accept_char('#')) {
        stream = expr_int(0, 15);
        if (error_msg) return;
        if (stream > 3 && !checking) { error("O Invalid stream"); return; }
        if (!at_statement_end() && !accept_char(';') && !accept_char(',')) {
            error("C Nonsense in BASIC");
            return;
        }
    }
    int printer = checking;               // (printing nothing)
    to_printer = stream == 3;
    int was_lower = using_lower();
    use_lower(stream < 2);
    struct colours saved, temp;
    get_colours(&saved);
    temp = saved;
    while (!at_statement_end() && !error_msg) {
        newline = 1;
        if (colour_item(&temp)) {
            set_colours(&temp);
            continue;
        }
        if (accept_char(';')) {
            newline = 0;
            continue;
        }
        if (accept_char('\'')) {            // ' starts a new line
            newline = 0;
            if (!printer) out_char('\n');
            continue;
        }
        if (accept_char(',')) {
            newline = 0;
            if (!printer) print_comma();
            continue;
        }
        if (accept(T_AT)) {
            if (!print_at(!printer)) break;
            continue;
        }
        if (accept(T_TAB)) {
            int col = expr_int(0, 65535);
            if (error_msg) break;
            if (!printer) print_tab(col);
            continue;
        }
        struct value v = expr();
        if (error_msg) break;
        if (!printer) print_value(v);
        if (!at_statement_end() && *p != ';' && *p != ',' && *p != '\'') {
            error("C Nonsense in BASIC");
            break;
        }
    }
    if (newline && !error_msg && !printer) out_char('\n');
    to_printer = 0;
    set_colours(&saved);
    use_lower(was_lower);
}
// Skip to the end of the current line.
static void skip_line() {
    while (*p) p++;
}

static void st_if() {
    double cond = expr_num();
    if (error_msg) return;
    if (!accept(T_THEN)) { error("C Nonsense in BASIC"); return; }
    if (!cond && !checking) { skip_line(); return; }
    skip_space();
    if (isdigit(*p)) {                    // IF ... THEN 100
        go_to_line(expr_int(0, 65535));
    } else {
        jumped = 1;     // the statements after THEN simply run next
        then_part = 1;
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

// FOR: as on the Spectrum, the loop is kept with its control variable
// (so there's no limit on loops, and jumping out of one is harmless).
static void st_for() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return;
    if (is_str) { error("C Nonsense in BASIC"); return; }
    if (!accept_char('=')) { error("C Nonsense in BASIC"); return; }
    double start = expr_num();
    if (error_msg) return;
    if (!accept(T_TO)) { error("C Nonsense in BASIC"); return; }
    double limit = expr_num();
    double step = 1;
    if (accept(T_STEP)) step = expr_num();
    if (error_msg || checking) return;
    struct var *v = find_var(name, 0, 1);
    if (!v) return;
    v->num = start;
    v->is_for = 1;
    v->limit = limit;
    v->step = step;
    v->loop_line = current_line_number();
    v->loop_stmt = statement_number(stmt_pos);
    // A loop that wouldn't run even once is skipped entirely, as on the
    // Spectrum: carry on after the matching NEXT.
    if ((step >= 0 && start > limit) || (step < 0 && start < limit)) {
        if (!skip_to_next(v)) error("I FOR without NEXT");
    }
}

static void st_next() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return;
    if (checking) {
        if (is_str) error("C Nonsense in BASIC");
        return;
    }
    struct var *v = is_str ? 0 : lookup_var(name, 0);
    if (!v || !v->is_for) {
        error("1 NEXT without FOR");
        return;
    }
    double n = check(v->num + v->step);
    if (error_msg) return;
    cost_us += 900;                       // (the add and the test)
    v->num = n;
    if ((v->step >= 0 && n <= v->limit) || (v->step < 0 && n >= v->limit))
        resume(v->loop_line, v->loop_stmt, 1);    // just after the FOR
}

static void st_gosub() {
    int num = expr_int(0, 65535);
    if (error_msg || checking) return;
    if (gosub_sp == MAX_GOSUB) {
        error("4 Out of memory");
        return;
    }
    gosub_stack[gosub_sp].line = current_line_number();
    gosub_stack[gosub_sp].stmt = statement_number(stmt_pos);
    go_to_line(num);
    if (!error_msg) gosub_sp++;
}

static void st_return() {
    if (checking) return;
    if (gosub_sp == 0) {
        error("7 RETURN without GO SUB");
        return;
    }
    gosub_sp--;
    resume(gosub_stack[gosub_sp].line, gosub_stack[gosub_sp].stmt, 1);
}

static int read_line(int cursor);
extern char input_buf[];
extern char edit_next[MAX_TEXT + 1];
extern int last_line;

// Stop at the current statement as if BREAK had been pressed, so that
// CONTINUE runs it again.
// Is the typed text STOP (which stops the program, in INPUT)?
static int is_stop(const char *s) {
    while (*s == ' ') s++;
    if (!is_word(s, "STOP")) return 0;
    s += 4;
    while (*s == ' ') s++;
    return *s == '\0';
}

// Read a value for lv in the lower screen. A string is typed between
// quotes (which may be removed, to type an expression), except with
// INPUT LINE. If what's typed isn't valid, it's asked for again.
static void input_value(struct lvalue *lv, int line_mode) {
    int quoted = lv->is_str && !line_mode;
    int col = cursor_col(), row = cursor_row();
    strcpy(input_buf, quoted ? "\"\"" : "");
    int cursor = quoted ? 1 : -1;
    for (;;) {
        set_cursor(col, row);
        if (read_line(cursor) < 0 || is_stop(input_buf)) {
            error("H STOP in INPUT");
            return;
        }
        int len = strlen(input_buf);
        struct value v;
        if (line_mode) {
            v = str_value(input_buf, len);
        } else if (!eval_text(input_buf, len, &v) || v.is_str != lv->is_str) {
            error_msg = 0;
            cursor = -1;
            continue;                     // try again
        }
        store(lv, v);
        return;
    }
}
/*
 * INPUT ["prompt";] var [, var]...
 * Each variable is read from its own line. A number may be typed as any
 * expression, as on the Spectrum; if it isn't valid, it's asked for
 * again.
 */
/*
 * INPUT, in the lower screen, as on the Spectrum. Its items are printed as
 * in PRINT (strings, and expressions in brackets, with ; , ' AT TAB and
 * colours), and each variable is typed in where it comes. A number may
 * be typed as any expression. INPUT LINE A$ takes the text as typed.
 * STOP (or Esc) stops the program: H STOP in INPUT.
 */
static void st_input() {
    int quiet = checking;                 // (just checking: print nothing)
    if (!quiet) {
        use_lower(1);
        clear_lower();
        set_scroll_count(upper_rows());   // everything on screen has been seen
    }
    struct colours temp;
    get_colours(&temp);
    while (!at_statement_end() && !error_msg) {
        if (colour_item(&temp)) {
            if (!quiet) set_colours(&temp);
            continue;
        }
        if (accept_char(';')) continue;
        if (accept_char(',')) { if (!quiet) print_comma(); continue; }
        if (accept_char('\'')) { if (!quiet) print_string("\n"); continue; }
        if (accept(T_AT)) { print_at(!quiet); continue; }
        if (accept(T_TAB)) {
            int col = expr_int(0, 65535);
            if (!error_msg && !quiet) print_tab(col);
            continue;
        }
        skip_space();
        if (*p == '"') {                  // a prompt
            struct value v = factor();
            if (!error_msg && !quiet) print_value(v);
            continue;
        }
        if (accept_char('(')) {           // (items): printed, as in PRINT
            while (!error_msg && !accept_char(')')) {
                if (accept_char(';')) continue;
                if (accept_char(',')) { if (!quiet) print_comma(); continue; }
                if (accept_char('\'')) { if (!quiet) print_string("\n"); continue; }
                if (at_statement_end()) { error("C Nonsense in BASIC"); break; }
                struct value v = expr();
                if (!error_msg && !quiet) print_value(v);
            }
            continue;
        }
        int line_mode = accept(T_LINE);
        struct lvalue lv;
        if (!parse_lvalue(&lv)) break;
        if (line_mode && !lv.is_str) { error("C Nonsense in BASIC"); break; }
        if (!quiet) input_value(&lv, line_mode);
    }
    if (!quiet) {
        clear_lower();
        use_lower(0);
    }
}
// Make array name (replacing any old one) with nd dimensions dims and
// count elements in all, cleared (to 0, or spaces). 0 after an error.
static struct array *new_array(const char *name, int is_str, int nd, const int *dims,
                               int count) {
    int esize = is_str ? 1 : (int)sizeof(double);
    struct array *a = find_array(name, is_str);
    if (!a) {
        int i = 0;
        while (i < num_arrays && arrays[i].name[0]) i++;
        if (i == MAX_ARRAYS) { error("4 Out of memory"); return 0; }
        if (i == num_arrays) num_arrays++;
        a = &arrays[i];
        strcpy(a->name, name);
        a->is_str = is_str;
        a->data = 0;
    }
    a->data = 0;                  // (any old elements are now garbage)
    char *data = heap_alloc(count * esize, 1, a - arrays);
    if (!data) { a->name[0] = '\0'; return 0; }
    a->data = data;
    a->ndims = nd;
    a->count = count;
    for (int i = 0; i < nd; i++) a->dims[i] = dims[i];
    memset(a->data, is_str ? ' ' : 0, count * esize);
    if (is_str) {                 // A string array replaces a simple string
        struct var *v = lookup_var(name, 1);
        if (v) delete_var(v);
    }
    return a;
}

static void st_dim() {
    do {
        char name[NAME_LEN];
        int is_str;
        if (!read_name(name, &is_str)) return;
        if (!accept_char('(')) { error("C Nonsense in BASIC"); return; }
        int dims[MAX_DIMS], nd = 0;
        int esize = is_str ? 1 : (int)sizeof(double);
        int room = HEAP_SIZE / esize;     // more elements than could ever fit
        int count = 1;
        do {
            if (nd == MAX_DIMS) {
                error("3 Subscript wrong");
                if (!checking) return;
                nd--;
            }
            int d = expr_int(1, 65535);
            if (error_msg) return;
            dims[nd++] = d;
            count = count > room / d ? room + 1 : count * d;
        } while (accept_char(','));
        expect_char(')', "C Nonsense in BASIC");
        if (error_msg) return;
        if (checking) continue;
        if (count > room) { error("4 Out of memory"); return; }
        if (!new_array(name, is_str, nd, dims, count)) return;
    } while (accept_char(','));
}

// Move data_pos to the next DATA item. Returns 0 if there are no more.
static int find_data() {
    if (in_data) return 1;
    if (data_line >= prog_len) return 0;
    unsigned char *q = data_pos ? (unsigned char *)data_pos : line_text(data_line);
    for (;;) {
        q = find_token_in(q, T_DATA);
        if (q) {
            data_pos = (char *)q + 1;
            in_data = 1;
            return 1;
        }
        data_line = line_next(data_line);
        if (data_line >= prog_len) return 0;
        q = line_text(data_line);
    }
}

static void st_read() {
    do {
        struct lvalue lv;
        if (!parse_lvalue(&lv)) return;
        if (checking) continue;
        if (!find_data()) { error("E Out of DATA"); return; }
        char *saved = p;
        p = data_pos;
        struct value v = expr();
        if (accept_char(',')) in_data = 1;
        else if (at_statement_end()) in_data = 0;
        else error("C Nonsense in BASIC");
        data_pos = p;
        p = saved;
        store(&lv, v);
    } while (!error_msg && accept_char(','));
}

// DATA is only read by READ (and DEF FN by FN); running it does nothing.
// But it's checked: DATA is a list of expressions, DEF FN as below.
static void check_def_fn();

static void st_data(int t) {
    if (checking) {
        if (t == T_DEFFN) {
            check_def_fn();
            return;
        }
        do expr(); while (!error_msg && accept_char(','));
        return;
    }
    while (!at_statement_end()) {
        if (*p++ == '"') {
            while (*p && *p != '"') p++;
            if (*p) p++;
        }
    }
}

// DEF FN F(A, B$)=expression: the name, parameters and expression.
static void check_def_fn() {
    char name[NAME_LEN];
    int is_str;
    if (!read_name(name, &is_str)) return;
    expect_char('(', "C Nonsense in BASIC");
    skip_space();
    if (!error_msg && *p != ')') {
        do {
            int s;
            if (!read_name(name, &s)) return;
        } while (accept_char(','));
    }
    expect_char(')', "C Nonsense in BASIC");
    expect_char('=', "C Nonsense in BASIC");
    if (error_msg) return;
    struct value v = expr();
    if (!error_msg && v.is_str != is_str) error("C Nonsense in BASIC");
}

// Wait for n/50 seconds (n = 0: forever), or until a key is pressed.
static void pause(int n) {
    unsigned long start = timer_ms();
    while (!key_waiting()) {
        if (n && timer_ms() - start >= n * 20UL) break;
    }
}

// BEEP duration, pitch: seconds, and semitones above middle C.
static void st_beep() {
    if (at_statement_end()) {          // plain BEEP: a short click
        if (!checking) tone(1000, 200);
        return;
    }
    double secs = expr_num();
    expect_char(',', "C Nonsense in BASIC");
    double pitch = expr_num();
    if (error_msg || checking) return;
    if (secs < 0 || secs > 10 || pitch < -60 || pitch > 69) {
        error("B Integer out of range");
        return;
    }
    double freq = 261.6255653 * num_exp(pitch / 12 * 0.69314718055994530942);
    tone((int)freq, (int)(secs * 1000));
}

/*
 * PLAY, from the Spectrum 128: music strings. The PC speaker has one
 * voice, so only the first string (channel A) is played; any others
 * (up to 8) are worked out but not heard. In a string:
 *
 *   a-g        a note in the current octave; A-G an octave higher
 *   # $        before a note: sharp, flat
 *   &          a rest
 *   1-12       the length of the notes that follow: 1 semiquaver, 2
 *              dotted semiquaver, 3 quaver, 4 dotted quaver, 5 crotchet
 *              (the start), 6 dotted crotchet, 7 minim, 8 dotted minim,
 *              9 semibreve; 10-12 triplets of semiquavers, quavers and
 *              crotchets
 *   On         octave n, 0-8 (5 at the start: middle C's)
 *   Tn         tempo, 60-240 crotchets a minute (120 at the start)
 *   ( )        repeat what's between once; a ) on its own repeats from
 *              the start
 *   !...!      a comment
 *   _          a tie (the notes run together anyway)
 *   H          stop
 *   V W U X Y Z M N, with a number: volume, envelopes and MIDI, which
 *              the speaker can't do: ignored
 */
static int strchr_in(const char *set, char c) {
    for (; *set; set++)
        if (*set == c) return 1;
    return 0;
}

static int play_number(const char *t, int len, int *i) {
    int n = 0;
    while (*i < len && isdigit(t[*i]) && n < 10000) n = n * 10 + (t[(*i)++] - '0');
    while (*i < len && isdigit(t[*i])) (*i)++;
    return n;
}

static void play_string(const char *t, int len) {
    // Lengths in 24ths of a crotchet.
    static const unsigned char units[12] = { 6, 9, 12, 18, 24, 36, 48, 72, 96, 4, 8, 16 };
    static const signed char semitone[7] = { 9, 11, 0, 2, 4, 5, 7 };   // a-g from c
    int octave = 5, length = 5, tempo = 120, accidental = 0;
    int stack[8], repeated[8], depth = 0, whole_repeated = 0;
    int i = 0;
    while (i < len && !error_msg) {
        if (key_waiting() && check_break()) {
            break_requested = 1;
            return;
        }
        char c = t[i++];
        int ms = units[length - 1] * 2500 / tempo;
        if (isdigit(c)) {
            i--;
            int n = play_number(t, len, &i);
            if (n < 1 || n > 12) error("n Out of range");
            else length = n;
        } else if (c == 'O' || c == 'o') {
            int n = play_number(t, len, &i);
            if (n > 8) error("n Out of range");
            else octave = n;
        } else if (c == 'T' || c == 't') {
            int n = play_number(t, len, &i);
            if (n < 60 || n > 240) error("n Out of range");
            else tempo = n;
        } else if (c == '#' || c == '$') {
            accidental = c == '#' ? 1 : -1;
            if (i >= len || toupper(t[i]) < 'A' || toupper(t[i]) > 'G')
                error("k Invalid note name");
        } else if (toupper(c) >= 'A' && toupper(c) <= 'G') {
            int up = c >= 'A' && c <= 'G';
            double n = semitone[toupper(c) - 'A'] + accidental + 12 * (octave - 5 + up);
            accidental = 0;
            double freq = 261.6255653 * num_exp(n / 12 * 0.69314718055994530942);
            tone((int)(freq + 0.5), ms);
        } else if (c == '&') {
            tone(0, ms);
        } else if (c == '!') {
            while (i < len && t[i] != '!') i++;
            i++;
        } else if (c == '(') {
            if (depth == 8) { error("n Out of range"); break; }
            stack[depth] = i;
            repeated[depth++] = 0;
        } else if (c == ')') {
            if (depth && !repeated[depth - 1]) {
                repeated[depth - 1] = 1;
                i = stack[depth - 1];
            } else if (depth) {
                depth--;
            } else if (!whole_repeated) {
                whole_repeated = 1;
                i = 0;
            }
        } else if (c == 'H' || c == 'h') {
            return;
        } else if (strchr_in("VvWwXxYyZzMmNn", c)) {
            play_number(t, len, &i);
        } else if (c != 'U' && c != 'u' && c != '_' && c != ' ') {
            error("k Invalid note name");
        }
    }
}

static void st_play() {
    struct value first = str_value("", 0);
    int n = 0;
    do {
        struct value v = expr();
        if (error_msg) return;
        if (!v.is_str) { error("C Nonsense in BASIC"); return; }
        if (n++ == 0) first = v;
    } while (accept_char(','));
    if (n > 8) error("n Out of range");
    if (!error_msg && !checking) play_string(first.str, first.len);
}

/*
 * SAVE, LOAD, VERIFY and MERGE, as on the Spectrum but with the boot disk
 * for a tape (see storage.c):
 *
 *   SAVE "name" [LINE n]       the program (LOAD runs it from line n)
 *   SAVE "name" CODE start,len bytes of memory, SAVE "name" SCREEN$ the screen
 *   LOAD "name", LOAD "name" CODE [start[,len]], LOAD "name" SCREEN$
 *   VERIFY: check a SAVE against what's in memory; MERGE: add a saved
 *   program's lines to this one
 *
 * LOAD "" (and VERIFY "", MERGE "") take the first file of the kind.
 */
static int file_name(char *name) {
    struct value v = expr();
    if (error_msg) return 0;
    if (!v.is_str) { error("C Nonsense in BASIC"); return 0; }
    if (checking) {
        name[0] = '\0';
        return 1;
    }
    if (v.len > STORE_NAME_LEN) {
        error("F Invalid file name");
        return 0;
    }
    memcpy(name, v.str, v.len);
    name[v.len] = '\0';
    return 1;
}

// What follows the name: CODE [start[,len]], SCREEN$, DATA a() or DATA
// a$(), or nothing (a program). Returns FILE_BYTES, FILE_NUMBERS,
// FILE_CHARS or FILE_PROGRAM; for bytes, *start and *len are set if given
// (otherwise left as they are); for an array, its name goes in aname.
static int file_kind(unsigned int *start, unsigned int *len, char *aname) {
    if (accept(T_DATA)) {
        int is_str;
        if (!read_name(aname, &is_str)) return FILE_NUMBERS;
        expect_char('(', "C Nonsense in BASIC");
        expect_char(')', "C Nonsense in BASIC");
        return is_str ? FILE_CHARS : FILE_NUMBERS;
    }
    if (accept(T_SCREENS)) {
        *start = 16384;
        *len = 6912;
        return FILE_BYTES;
    }
    if (!accept(T_CODE)) return FILE_PROGRAM;
    if (!at_statement_end()) {
        *start = expr_int(0, 65535);
        if (accept_char(',')) *len = expr_int(0, 65535);
    }
    return FILE_BYTES;
}

HIGH_MEM unsigned char load_buf[PROG_SIZE];

/*
 * SAVE "name" DATA a(): an array, saved as its number of dimensions (a
 * byte), each dimension (2 bytes), then its elements: doubles, or the
 * characters of a string array. The file's param is the array's letter.
 */

__attribute__((noinline, noclone))   // (text_buf is HIGH_MEM: see mem.h)
static int array_to_bytes(struct array *a, unsigned char *out) {
    int n = 0, size = a->count * (a->is_str ? 1 : (int)sizeof(double));
    if (1 + 2 * a->ndims + size > DATA_SIZE) return -1;
    out[n++] = a->ndims;
    for (int i = 0; i < a->ndims; i++) {
        out[n++] = a->dims[i];
        out[n++] = a->dims[i] >> 8;
    }
    memcpy(out + n, a->data, size);
    return n + size;
}

// The dimensions saved in data (len bytes); the number of elements, or
// -1 if it isn't a well-formed array of that kind.
__attribute__((noinline, noclone))   // (text_buf is HIGH_MEM: see mem.h)
static int array_dims(const unsigned char *data, int len, int is_str, int *nd, int *dims) {
    if (len < 1 || data[0] < 1 || data[0] > MAX_DIMS || len < 1 + 2 * data[0]) return -1;
    *nd = data[0];
    long count = 1;
    for (int i = 0; i < *nd; i++) {
        dims[i] = data[1 + 2 * i] | (data[2 + 2 * i] << 8);
        if (dims[i] < 1) return -1;
        count *= dims[i];
        if (count > DATA_SIZE) return -1;
    }
    if (1 + 2 * *nd + count * (is_str ? 1 : (long)sizeof(double)) != len) return -1;
    return count;
}

// LOAD (or VERIFY: t) the array saved in data into array name.
__attribute__((noinline, noclone))   // (text_buf is HIGH_MEM: see mem.h)
static void bytes_to_array(int t, const char *name, int is_str, const unsigned char *data,
                           int len) {
    int nd, dims[MAX_DIMS];
    int count = array_dims(data, len, is_str, &nd, dims);
    if (count < 0) { error("R Bad program file"); return; }
    const unsigned char *elements = data + 1 + 2 * nd;
    int size = count * (is_str ? 1 : (int)sizeof(double));
    if (t == T_VERIFY) {
        struct array *a = find_array(name, is_str);
        if (!a || a->ndims != nd || memcmp(a->dims, dims, nd * sizeof(int)) != 0 ||
            memcmp(a->data, elements, size) != 0)
            error("R Tape loading error");
        return;
    }
    struct array *a = new_array(name, is_str, nd, dims, count);
    if (a) memcpy(a->data, elements, size);
}

/*
 * Listings as text files, on the FAT partition (see fat.c): a name ending
 * .BAS or .TXT is one. The text is as LIST shows it, one line per line,
 * with the characters a PC can't show written as escapes (see escape()),
 * as zmakebas, the tool for making Spectrum tape files from listings,
 * has them. Reading one, blank lines and lines starting with # are
 * skipped, and a \ at the end of a line joins the next one to it. Each
 * line is checked as if typed; one that doesn't make sense is kept as it
 * is, and the first is put in the editor with its ?, to be put right.
 */
// A line as listed, and which of its characters are keywords (for LIST,
// EDIT and text files; kept out of the stack).
HIGH_MEM char list_buf[MAX_TEXT * 4], list_kw[MAX_TEXT * 4];

HIGH_MEM char text_line[MAX_TEXT * 8];
HIGH_MEM unsigned char text_tokens[MAX_TEXT + 1];

static int syntax_check(char *text);
static int source_offset(const char *s, int k, const unsigned char *tokens);
static void line_to_text(int off, char *out, char *kw, int cap);
extern char edit_next[];
static int edit_cursor;

static int is_text_name(const char *name) {
    int n = strlen(name);
    if (n < 4 || name[n - 4] != '.') return 0;
    const char *ext = name + n - 3;
    return (toupper(ext[0]) == 'B' && toupper(ext[1]) == 'A' && toupper(ext[2]) == 'S') ||
           (toupper(ext[0]) == 'T' && toupper(ext[1]) == 'X' && toupper(ext[2]) == 'T');
}

static char block_char(int top, int bottom) {
    return top ? (bottom ? ':' : '\'') : (bottom ? '.' : ' ');
}

// The program line at off as text, with escapes, and a newline. Returns its length.
static int export_line(int off, char *out) {
    char *buf = list_buf, *kw = list_kw;
    line_to_text(off, buf, kw, sizeof(list_buf));
    int n = 0, in_str = 0, rem = 0;
    for (int i = 0; buf[i]; i++) {
        unsigned char c = buf[i];
        if (!rem && kw[i] && i >= 2 && memcmp(buf + i - 2, "REM", 3) == 0 && kw[i - 2] &&
            !kw[i + 1]) {
            rem = 1;                       // the rest of the line is REM text
            if (buf[i + 1] == ' ') out[n++] = c, c = buf[++i];
        } else if (c == '"' && !rem) {
            in_str = !in_str;
        }
        if (!in_str && !rem) {
            out[n++] = c;
        } else if (c == '\\') {
            out[n++] = '\\';
            out[n++] = '\\';
        } else if (c == 127) {
            out[n++] = '\\';
            out[n++] = '*';
        } else if (c >= 128 && c <= 143) {
            out[n++] = '\\';
            out[n++] = block_char(c & 2, c & 8);
            out[n++] = block_char(c & 1, c & 4);
        } else if (c >= 144 && c <= 164) {
            out[n++] = '\\';
            out[n++] = 'a' + c - 144;
        } else if (c < 32 || c >= 165) {
            char num[8];
            itoa(c, num);
            out[n++] = '\\';
            out[n++] = '{';
            for (int k = 0; num[k]; k++) out[n++] = num[k];
            out[n++] = '}';
        } else {
            out[n++] = c;
        }
    }
    while (!rem && n > 0 && out[n - 1] == ' ') n--;   // (LIST's space after STOP etc.)
    out[n++] = '\n';
    return n;
}

// The whole program as text in text_buf; its length, or -1 if too long.
static int program_to_text() {
    int n = 0;
    for (int off = 0; off < prog_len; off = line_next(off)) {
        int m = export_line(off, text_line);
        if (n + m > TEXT_SIZE) return -1;
        memcpy(text_buf + n, text_line, m);
        n += m;
    }
    return n;
}

// The next line of the text in text_buf[*i..len) into text_line, joining
// lines that end with \. Returns 0 at the end; -1 if a line is too long.
static int next_text_line(int *i, int len) {
    int n = 0, too_long = 0;
    for (;;) {
        while (*i < len && text_buf[*i] != '\n' && text_buf[*i] != '\r') {
            if (n < MAX_TEXT) text_line[n++] = text_buf[*i];
            else too_long = 1;
            (*i)++;
        }
        if (*i < len && text_buf[*i] == '\r') (*i)++;
        if (*i < len && text_buf[*i] == '\n') (*i)++;
        if (n > 0 && text_line[n - 1] == '\\' && *i < len) {
            n--;                           // carries on on the next line
            continue;
        }
        break;
    }
    text_line[n] = '\0';
    return too_long ? -1 : 1;
}

// Add the lines of the text in text_buf (len bytes) to the program.
static void text_to_program(int len) {
    int i = 0, bad_seen = 0;
    while (i < len && !error_msg) {
        int r = next_text_line(&i, len);
        if (r < 0) { error("G No room for line"); break; }
        char *t = text_line;
        while (*t == ' ' || *t == '\t') t++;
        if (*t == '\0' || *t == '#') continue;
        int num = 0, bad = -1;
        char *q = t;
        while (isdigit(*q) && num <= 9999) num = num * 10 + (*q++ - '0');
        if (q == t || num < 1 || num > 9999) {
            bad = 0;                       // no line number
        } else {
            while (*q == ' ') q++;
            int n = tokenize(q, text_tokens, sizeof(text_tokens));
            if (n < 0) { error("G No room for line"); break; }
            if (n > 1) bad = syntax_check((char *)text_tokens);
            if (bad >= 0) bad = (q - text_line) + source_offset(q, bad, text_tokens);
            const char *e = n > 1 ? line_store(num, text_tokens, n) : 0;
            if (e) { error(e); break; }
        }
        if (bad >= 0 && !bad_seen) {      // the first bad line, to be put right
            bad_seen = 1;
            strncpy_safe(edit_next, text_line, MAX_TEXT + 1);
            edit_cursor = bad;
            edit_show_error();
        }
    }
    if (bad_seen) error("C Nonsense in BASIC");
}

// SAVE, LOAD, VERIFY, MERGE or ERASE (t) of a text file.
static void text_file(int t, const char *name) {
    const char *err = 0;
    int len = 0;
    if (t == T_SAVE) {
        len = program_to_text();
        err = len < 0 ? "4 Out of memory" : fat_write(name, text_buf, len);
    } else if (t == T_ERASE) {
        err = fat_erase(name);
    } else {
        err = fat_read(name, text_buf, TEXT_SIZE, &len);
    }
    if (err || t == T_SAVE || t == T_ERASE) {
        if (err) error(err);
        return;
    }
    if (t == T_VERIFY) {                   // the same as the program would save as?
        int k = 0;
        for (int off = 0; off < prog_len; off = line_next(off)) {
            int m = export_line(off, text_line);
            for (int j = 0; j < m; j++) {
                if (k < len && text_buf[k] == '\r') k++;
                if (k >= len || text_buf[k++] != (unsigned char)text_line[j]) {
                    error("R Tape loading error");
                    return;
                }
            }
        }
        if (k < len) error("R Tape loading error");
        return;
    }
    if (t == T_LOAD) {
        prog_len = 0;
        clear_variables();
        reset_run_state();
        cont_ok = 0;
        running = 0;
        text_to_program(len);
        return;
    }
    data_mark();                          // MERGE
    text_to_program(len);
    data_unmark();
}

static void st_save() {
    char name[STORE_NAME_LEN + 1];
    if (!file_name(name)) return;
    if (!name[0] && !checking) { error("F Invalid file name"); return; }
    unsigned int start = 0, len = 0;
    char aname[NAME_LEN];
    int kind = file_kind(&start, &len, aname), param = 0;
    if (kind == FILE_PROGRAM && accept(T_LINE)) param = expr_int(1, 9999);
    if (error_msg || checking) return;
    if (is_text_name(name)) {
        if (kind == FILE_PROGRAM) text_file(T_SAVE, name);
        else error("F Invalid file name");
        return;
    }
    const char *err;
    if (kind == FILE_NUMBERS || kind == FILE_CHARS) {
        struct array *a = find_array(aname, kind == FILE_CHARS);
        if (!a) { error("2 Variable not found"); return; }
        int n = array_to_bytes(a, text_buf);
        if (n < 0) { error("4 Out of memory"); return; }
        err = storage_save(name, text_buf, n, aname[0], kind);
    } else if (kind == FILE_BYTES) {
        if (len > PROG_SIZE) { error("4 Out of memory"); return; }
        for (unsigned int i = 0; i < len; i++) load_buf[i] = mem_peek(start + i);
        err = storage_save(name, load_buf, len, start, FILE_BYTES);
    } else {
        err = storage_save(name, prog, prog_len, param, FILE_PROGRAM);
    }
    if (err) error(err);
}

// A loaded program must be well formed: the interpreter trusts it.
static int valid_program(const unsigned char *data, int len) {
    int off = 0, last = 0;
    while (off < len) {
        if (off + LINE_HEAD + 1 > len) return 0;
        int num = data[off] | (data[off + 1] << 8);
        int n = data[off + 2] | (data[off + 3] << 8);
        if (num <= last || num > 9999 || n < 1 || n > MAX_TEXT + 1 ||
            off + LINE_HEAD + n > len) return 0;
        if (data[off + LINE_HEAD + n - 1] != 0) return 0;
        for (int i = 0; i < n - 1; i++)
            if (data[off + LINE_HEAD + i] == 0) return 0;
        last = num;
        off += LINE_HEAD + n;
    }
    return 1;
}

// LOAD, VERIFY and MERGE (t is which).
static void st_load(int t) {
    char name[STORE_NAME_LEN + 1];
    if (!file_name(name)) return;
    unsigned int start = 0, want = 0;
    char aname[NAME_LEN];
    int kind = file_kind(&start, &want, aname);
    if (error_msg) return;
    if (kind != FILE_PROGRAM && t == T_MERGE) { error("C Nonsense in BASIC"); return; }
    if (checking) return;
    if (is_text_name(name)) {
        if (kind == FILE_PROGRAM) text_file(t, name);
        else error("F Invalid file name");
        return;
    }
    int len, param;
    if (kind == FILE_NUMBERS || kind == FILE_CHARS) {
        const char *err = storage_load(name, kind, text_buf, DATA_SIZE, &len, &param);
        if (err) error(err);
        else bytes_to_array(t, aname, kind == FILE_CHARS, text_buf, len);
        return;
    }
    const char *err = storage_load(name, kind, load_buf, PROG_SIZE, &len, &param);
    if (!err && kind == FILE_PROGRAM && !valid_program(load_buf, len)) err = "R Bad program file";
    if (err) { error(err); return; }
    if (kind == FILE_BYTES) {
        if (!start) start = param;        // (CODE alone: where it came from)
        if (want && (int)want < len) {
            if (t == T_LOAD) { error("R Tape loading error"); return; }
            len = want;
        }
        for (int i = 0; i < len; i++) {
            if (t == T_VERIFY) {
                if (mem_peek(start + i) != load_buf[i]) { error("R Tape loading error"); return; }
            } else {
                mem_poke(start + i, load_buf[i]);
            }
        }
        return;
    }
    if (t == T_VERIFY) {
        if (len != prog_len || memcmp(load_buf, prog, len) != 0) error("R Tape loading error");
        return;
    }
    if (t == T_MERGE) {                   // each line goes in, replacing any same-numbered one
        data_mark();
        for (int off = 0; off < len && !error_msg; ) {
            int num = load_buf[off] | (load_buf[off + 1] << 8);
            int n = load_buf[off + 2] | (load_buf[off + 3] << 8);
            const char *e = line_store(num, load_buf + off + LINE_HEAD, n);
            if (e) error(e);
            off += LINE_HEAD + n;
        }
        data_unmark();
        return;
    }
    memcpy(prog, load_buf, len);
    prog_len = len;
    clear_variables();
    reset_run_state();
    cont_ok = 0;
    if (param) {
        go_to_line(param);                // SAVE ... LINE n: run it
    } else {
        running = 0;
    }
}

static void st_erase() {
    char name[STORE_NAME_LEN + 1];
    if (!file_name(name) || checking) return;
    if (is_text_name(name)) {
        text_file(T_ERASE, name);
        return;
    }
    const char *err = storage_erase(name);
    if (err) error(err);
}

static void cat_entry(const char *name, int len, int param, int type) {
    char buf[8];
    print_string(name);
    for (int i = strlen(name); i < 16; i++) print_char(' ');
    if (type == FILE_BYTES && param == 16384 && len == 6912) {
        print_string("SCREEN$");
    } else if (type == FILE_NUMBERS || type == FILE_CHARS) {
        char a[2] = { param >= 'A' && param <= 'Z' ? param + 'a' - 'A' : param, 0 };
        itoa(len, buf);
        print_string(buf);
        print_string(" DATA ");
        print_string(a);
        print_string(type == FILE_CHARS ? "$()" : "()");
    } else {
        itoa(len, buf);
        print_string(buf);
        if (type == FILE_BYTES) {
            itoa(param, buf);
            print_string(" CODE ");
            print_string(buf);
        } else if (param) {
            itoa(param, buf);
            print_string(" LINE ");
            print_string(buf);
        }
    }
    print_string("\n");
}

static void cat_text(const char *name, int len) {
    char buf[12];
    print_string(name);
    for (int i = strlen(name); i < 16; i++) print_char(' ');
    itoa(len, buf);
    print_string(buf);
    print_string(" TEXT\n");
}

static void st_cat() {
    if (checking) return;
    int free_files, free_kb;
    const char *err = storage_list(cat_entry, &free_files, &free_kb);
    if (err) { error(err); return; }
    fat_list(cat_text);                   // (none, if there's no FAT partition)
    char buf[8];
    itoa(free_files ? free_kb : 0, buf);  // (no room in the directory: none)
    print_string(buf);
    print_string("K FREE\n");
}

static int next_statement();

static void st_continue() {
    if (checking) return;
    if (!cont_ok) { error("N Statement lost"); return; }
    resume(cont_line, cont_stmt, cont_next);
}

// INK n, PAPER n etc. as statements: the upper screen's colours.
static void st_colour(int which) {
    int n = expr_int(0, 255);
    if (error_msg || checking) return;
    struct colours c;
    get_colours(&c);
    if (!colour_set(&c, which, n)) error("K Invalid colour");
    else set_colours(&c);
}
// INK n, PAPER n etc. as an item within PRINT: updates *c.
// INK n, PAPER n etc. as an item within PRINT, INPUT, PLOT...: updates *c.
static int colour_item(struct colours *c) {
    skip_space();
    unsigned char t = *p;
    int which = t == T_INK ? C_INK : t == T_PAPER ? C_PAPER : t == T_FLASH ? C_FLASH :
                t == T_BRIGHT ? C_BRIGHT : t == T_INVERSE ? C_INVERSE :
                t == T_OVER ? C_OVER : -1;
    if (which < 0) return 0;
    p++;
    int n = expr_int(0, 255);
    if (!error_msg && !colour_set(c, which, n)) error("K Invalid colour");
    return 1;
}

// Colour items before the numbers of PLOT, DRAW and CIRCLE
// (PLOT INK 2;10,10). They last for the statement: restore *saved after.
static void graphics_colours(struct colours *saved) {
    struct colours temp;
    get_colours(saved);
    temp = *saved;
    while (!error_msg && colour_item(&temp)) {
        if (!accept_char(';') && !accept_char(',')) error("C Nonsense in BASIC");
    }
    set_colours(&temp);
}
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
