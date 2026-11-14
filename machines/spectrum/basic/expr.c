/*
 * Part of the Spectrum's BASIC (included by ../basic.c, not built on its
 * own): values, variables and arrays, slicing, random numbers, running at
 * the Spectrum's speed, expressions, DEF FN and the functions.
 */

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
    if (!IS_TOKEN(t) || (t >= T_LE && t <= T_NE)) return 0;
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
//
// Where the platform can say which keys are held (the BIOS), the PC's
// keys stand where the Spectrum's are: Shift is CAPS SHIFT, Ctrl and
// AltGr SYMBOL SHIFT, and the arrows and Backspace are CAPS SHIFT with
// 5-8 and 0, as on the later Spectrums' keyboards. Otherwise (UEFI) the
// last key typed counts as held for a moment.
#define CAPS_SHIFT 0x100                  // (beyond the PC's scan codes)
#define SYMBOL_SHIFT 0x101
static const unsigned short half_row_keys[8][5] = {  // PC scan codes, and the shifts
    {CAPS_SHIFT, 0x2C, 0x2D, 0x2E, 0x2F},          // CAPS SHIFT Z X C V
    {0x1E, 0x1F, 0x20, 0x21, 0x22},       // A S D F G
    {0x10, 0x11, 0x12, 0x13, 0x14},       // Q W E R T
    {0x02, 0x03, 0x04, 0x05, 0x06},       // 1 2 3 4 5
    {0x0B, 0x0A, 0x09, 0x08, 0x07},       // 0 9 8 7 6
    {0x19, 0x18, 0x17, 0x16, 0x15},       // P O I U Y
    {SC_ENTER, 0x26, 0x25, 0x24, 0x23},   // ENTER L K J H
    {SC_SPACE, SYMBOL_SHIFT, 0x32, 0x31, 0x30},      // SPACE SYMBOL SHIFT M N B
};

static int pc_key_down(int code) {
    return plat_key_down(code) > 0;
}

// Is this Spectrum key (an entry of half_row_keys) held?
static int spectrum_key_down(int code, const int *extra) {
    switch (code) {
    case CAPS_SHIFT:
        return pc_key_down(SC_LSHIFT) | pc_key_down(SC_RSHIFT) | extra[0];
    case SYMBOL_SHIFT:
        return pc_key_down(SC_LCTRL) | pc_key_down(SC_RCTRL) | pc_key_down(SC_RALT);
    case SC_ENTER:
        return pc_key_down(SC_ENTER) | pc_key_down(SC_KPENTER);
    case 0x06: return pc_key_down(code) | extra[1];   // 5: left
    case 0x07: return pc_key_down(code) | extra[2];   // 6: down
    case 0x08: return pc_key_down(code) | extra[3];   // 7: up
    case 0x09: return pc_key_down(code) | extra[4];   // 8: right
    case 0x0B: return pc_key_down(code) | extra[5];   // 0: Backspace (DELETE)
    }
    return pc_key_down(code);
}

static int in_port(int port) {
    static const char *const half_rows[8] = {
        "\001ZXCV", "ASDFG", "QWERT", "12345", "09876", "POIUY", "\rLKJH", " \002MNB",
    };
    if (port & 1) return 255;
    int k = held_key(), bits = 0x1F;      // (and keeps the typed keys in step)
    if (plat_key_down(0) >= 0) {          // (0 is no key: does the platform know?)
        int extra[6];
        extra[1] = pc_key_down(SC_LEFT);
        extra[2] = pc_key_down(SC_DOWN);
        extra[3] = pc_key_down(SC_UP);
        extra[4] = pc_key_down(SC_RIGHT);
        extra[5] = pc_key_down(SC_BACKSPACE);
        extra[0] = extra[1] | extra[2] | extra[3] | extra[4] | extra[5];
        for (int r = 0; r < 8; r++) {
            if (port & (0x100 << r)) continue;
            for (int b = 0; b < 5; b++)
                if (spectrum_key_down(half_row_keys[r][b], extra)) bits &= ~(1 << b);
        }
        return 0xE0 | bits;
    }
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
