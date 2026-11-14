/*
 * The language core of the 6502 BASIC family (see msbasic.h): the
 * program, variables, expressions and the statements all its dialects
 * share. Written for this project from how they behave; none of it
 * comes from their code.
 */
#include "msbasic.h"
#include "num.h"
#include "string.h"
#include "mem.h"

#define D ms_dialect

int ms_err = E_NONE;
int ms_dos_err;
int ms_input_quiet;
unsigned long ms_cost_us;
#define COST(what) (ms_cost_us += D.what)

void ms_error(int e) {
    if (ms_err == E_NONE) ms_err = e;
}

static void outs(const char *s) {
    while (*s) ms_out(*s++);
}

static void out_int(int n) {
    char b[12];
    itoa(n, b);
    outs(b);
}

/* ------------------------------------------------------------------------
 * Numbers
 * --------------------------------------------------------------------- */

// Rounded to what the original's numbers hold; an error if too big.
double ms_fit(double x) {
    if (!num_spectrum(&x)) {
        ms_error(E_OVERFLOW);
        return 0;
    }
    return x;
}

// x as PRINT shows it: up to 9 significant digits.
void ms_format(double x, char *buf) {
    char *p = buf;
    if (x < 0) {
        *p++ = '-';
        x = -x;
    } else if (D.sign_space) {
        *p++ = ' ';
    }
    if (x == 0) {
        strcpy(p, "0");
        return;
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

/* ------------------------------------------------------------------------
 * The program
 * --------------------------------------------------------------------- */

#define PROG_SIZE MS_PROG_SIZE
#define MAX_LINE 255                      // (crunched)
HIGH_MEM unsigned char ms_prog[PROG_SIZE];
int ms_prog_len;

int ms_line_num(int off) { return ms_prog[off] | (ms_prog[off + 1] << 8); }
int ms_line_next(int off) { return off + 4 + (ms_prog[off + 2] | (ms_prog[off + 3] << 8)); }
static unsigned char *line_text(int off) { return ms_prog + off + 4; }

int ms_line_find(int num) {
    int off = 0;
    while (off < ms_prog_len && ms_line_num(off) < num) off = ms_line_next(off);
    return off;
}

void ms_line_delete(int num) {
    int off = ms_line_find(num);
    if (off < ms_prog_len && ms_line_num(off) == num) {
        int next = ms_line_next(off);
        memmove(ms_prog + off, ms_prog + next, ms_prog_len - next);
        ms_prog_len -= next - off;
    }
}

static int line_insert(int num, const unsigned char *text, int len) {
    ms_line_delete(num);
    if (ms_prog_len + len + 4 > PROG_SIZE) return 0;
    int off = ms_line_find(num);
    memmove(ms_prog + off + len + 4, ms_prog + off, ms_prog_len - off);
    ms_prog[off] = num & 0xFF;
    ms_prog[off + 1] = num >> 8;
    ms_prog[off + 2] = len & 0xFF;
    ms_prog[off + 3] = len >> 8;
    memcpy(ms_prog + off + 4, text, len);
    ms_prog_len += len + 4;
    return 1;
}

/* ------------------------------------------------------------------------
 * Crunching a typed line
 * --------------------------------------------------------------------- */

static int is_alpha(int c) { return c >= 'A' && c <= 'Z'; }
static int is_digit(int c) { return c >= '0' && c <= '9'; }
static int is_alnum(int c) { return is_alpha(c) || is_digit(c); }
static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

const char *ms_keyword_name(int token) {
    for (int i = 0; i < D.n_keywords; i++)
        if (D.keywords[i].token == token && token >= 0x80) return D.keywords[i].name;
    return 0;
}

// The keyword at s, or 0; *end just after it. (Spaces inside it are
// skipped, unless the dialect keeps them.)
static int match_keyword(const char *s, const char **end) {
    for (int i = 0; i < D.n_keywords; i++) {
        const char *p = s, *w = D.keywords[i].name;
        while (*w) {
            if (w != D.keywords[i].name && !D.keep_spaces)
                while (*p == ' ') p++;
            if (upper(*p) != *w) {
                if (D.abbrev && w != D.keywords[i].name && (*p & 0xFF) == (*w | 0x80)) {
                    p++;                  // (pO: POKE)
                    w = "";
                }
                break;
            }
            p++, w++;
        }
        if (*w) continue;
        int t = D.keywords[i].token;
        if (t == D.at_token) {            // AT, ATN, and A TO
            const char *q = p;
            while (*q == ' ') q++;
            if (upper(*q) == 'N') continue;   // (ATN, later on)
            if (upper(*q) == 'O') return 0;   // A TO: the A's a name
        }
        *end = p;
        return t;
    }
    return 0;
}

static int crunch(const char *s, unsigned char *out_buf, int cap) {
    int n = 0;
#define PUT(c, q) do { if (n >= cap) return -1; out_buf[n++] = ms_line_char(c, q); } while (0)
#define PUT_TOKEN(t) do { if (n >= cap) return -1; out_buf[n++] = (t); } while (0)
    while (*s) {
        if (*s == ' ' && !D.keep_spaces) {
            s++;
            continue;
        }
        if (*s == '"') {
            PUT(*s++, 1);
            while (*s && *s != '"') PUT(*s++, 1);
            if (*s) PUT(*s++, 1);
            continue;
        }
        if (*s == '?') {                  // (PRINT, for short)
            PUT_TOKEN(T_PRINT);
            s++;
            continue;
        }
        const char *e;
        int t = match_keyword(s, &e);
        if (t) {
            PUT_TOKEN(t);
            s = e;
            if (t == T_REM) {
                while (*s) PUT(*s++, 1);
                break;
            }
            if (t == T_DATA) {            // as typed, to the end of the statement
                int q = 0;
                while (*s && (q || *s != ':')) {
                    if (*s == '"') q = !q;
                    PUT(*s++, 1);
                }
            }
            continue;
        }
        PUT(*s, 0);
        s++;
    }
    PUT_TOKEN(0);
    return n - 1;
#undef PUT
#undef PUT_TOKEN
}

/* ------------------------------------------------------------------------
 * Variables
 *
 * Simple variables are a table; arrays and strings live in pools that
 * CLEAR empties. A string's text is in the string pool, which is
 * compacted when it fills; strings being worked on within a statement
 * are in a scratch area that's emptied before each statement.
 * --------------------------------------------------------------------- */

struct strref { unsigned short off; unsigned char len; unsigned char pad; };

struct ms_var {
    char name[2];
    char type;
    double n;
    struct strref s;
};

#define MAX_VARS 400
HIGH_MEM struct ms_var ms_vars[MAX_VARS];
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
HIGH_MEM struct array ms_arrays[MAX_ARRAYS];
static int n_arrays;

#define ARRAY_POOL (64 * 1024L)
HIGH_MEM unsigned char ms_array_pool[ARRAY_POOL];
static long array_used;

#define STR_POOL 32768
HIGH_MEM unsigned char ms_strings[STR_POOL];
HIGH_MEM unsigned char ms_strings2[STR_POOL];
static int str_used;

#define SCRATCH 16384
HIGH_MEM unsigned char ms_scratch_buf[SCRATCH];
static int scratch_used;

struct fn {
    char name[2];
    char param[2];
    const unsigned char *body;            // in the program
};
#define MAX_FNS 32
static struct fn fns[MAX_FNS];
static int n_fns;

unsigned char *ms_scratch(int len) {
    if (scratch_used + len > SCRATCH) {
        ms_error(E_COMPLEX);
        return 0;
    }
    unsigned char *p = ms_scratch_buf + scratch_used;
    scratch_used += len;
    return p;
}

static int elem_size(int type) { return type == MS_REAL ? 8 : type == MS_INT ? 2 : 4; }

static void compact_strings(void) {
    int top = 0;
    for (int i = 0; i < n_vars; i++) {
        struct strref *r = &ms_vars[i].s;
        if (ms_vars[i].type != MS_STR || !r->len) continue;
        memcpy(ms_strings2 + top, ms_strings + r->off, r->len);
        r->off = top;
        top += r->len;
    }
    for (int i = 0; i < n_arrays; i++) {
        if (ms_arrays[i].type != MS_STR) continue;
        for (long k = 0; k < ms_arrays[i].count; k++) {
            struct strref r;
            unsigned char *at = ms_array_pool + ms_arrays[i].off + k * 4;
            memcpy(&r, at, sizeof(r));
            if (!r.len) continue;
            memcpy(ms_strings2 + top, ms_strings + r.off, r.len);
            r.off = top;
            top += r.len;
            memcpy(at, &r, sizeof(r));
        }
    }
    memcpy(ms_strings, ms_strings2, top);
    str_used = top;
}

static void str_store(struct strref *r, const unsigned char *s, int len) {
    r->len = 0;
    if (len == 0) return;
    if (str_used + len > STR_POOL) {
        if (s >= ms_strings && s < ms_strings + STR_POOL) {   // (it'll move)
            unsigned char *t = ms_scratch(len);
            if (!t) return;
            memcpy(t, s, len);
            s = t;
        }
        compact_strings();
        if (str_used + len > STR_POOL) {
            ms_error(E_MEMORY);
            return;
        }
    }
    memcpy(ms_strings + str_used, s, len);
    r->off = str_used;
    r->len = len;
    str_used += len;
}

/*
 * The memory the program and its variables would take in the original,
 * laid out as it lays them out (this one's aren't), for FRE: the program
 * (each line as it's kept here, as there: 4 bytes and the crunched text,
 * then 2 bytes at the end), 7 bytes a simple variable or DEF FN, arrays
 * (5 bytes, 2 a subscript, and 5 an element, 2 for integers, 3 for
 * strings), and the strings' text. (The garbage is collected first, as
 * there.)
 */
long ms_memory_used(void) {
    static const int size[] = { 5, 2, 3 };    // MS_REAL, MS_INT, MS_STR
    compact_strings();
    long used = ms_prog_len + 2 + 7L * (n_vars + n_fns) + str_used;
    for (int i = 0; i < n_arrays; i++)
        used += 5 + 2 * ms_arrays[i].ndims + ms_arrays[i].count * size[(int)ms_arrays[i].type];
    return used;
}

/* ------------------------------------------------------------------------
 * Running
 * --------------------------------------------------------------------- */

const unsigned char *ms_ip;
#define ip ms_ip
int ms_cur_line = -1;                     // offset of the line running, -1: typed
#define cur_line ms_cur_line
static const unsigned char *stmt_start;   // the statement running (RESUME, errors)
int ms_jumped, ms_ended;
#define jumped ms_jumped
#define ended ms_ended
static const unsigned char *break_ip;     // where CONT goes on from (after BREAK)
int ms_tracing;

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

void ms_clear_vars(void) {
    n_vars = n_arrays = n_fns = 0;
    array_used = 0;
    str_used = 0;
    sp = 0;
    data_line = 0;
    data_ip = 0;
    data_started = 0;
    onerr_line = -1;
    cont.ok = 0;
    ms_cleared();
}

int ms_cur(void) {
    while (*ip == ' ') ip++;
    return *ip;
}

int ms_expect(int c) {
    if (ms_cur() != c) {
        ms_error(E_SYNTAX);
        return 0;
    }
    ip++;
    return 1;
}

#define expect ms_expect
#define cur ms_cur
#define error ms_error
#define err ms_err

double ms_num_expr(void) {
    struct ms_val v;
    ms_expr(&v);
    if (v.str) error(E_TYPE);
    return err != E_NONE ? 0 : v.n;
}

// A number that must be 0-max: ILLEGAL QUANTITY if not.
int ms_quantity(long max) {
    double x = ms_num_expr();
    if (err != E_NONE) return 0;
    x = num_floor(x);
    if (x < 0 || x > max) {
        error(E_QUANTITY);
        return 0;
    }
    return (int)x;
}

// An address: -65535 to 65535 (negative counts back from 65536).
unsigned int ms_address(void) {
    double x = num_floor(ms_num_expr());
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
    if (!is_alpha(cur())) {
        error(E_SYNTAX);
        return 0;
    }
    name[0] = *ip++;
    name[1] = is_alnum(cur()) ? *ip : 0;
    while (is_alnum(cur())) ip++;
    *type = MS_REAL;
    if (*ip == '$') {
        *type = MS_STR;
        ip++;
    } else if (*ip == '%') {
        *type = MS_INT;
        ip++;
    }
    return 1;
}

static struct ms_var *find_var(const char name[2], int type) {
    for (int i = 0; i < n_vars; i++)
        if (ms_vars[i].name[0] == name[0] && ms_vars[i].name[1] == name[1] &&
            ms_vars[i].type == type)
            return &ms_vars[i];
    if (n_vars == MAX_VARS) {
        error(E_MEMORY);
        return 0;
    }
    struct ms_var *v = &ms_vars[n_vars++];
    v->name[0] = name[0];
    v->name[1] = name[1];
    v->type = type;
    v->n = 0;
    v->s.len = 0;
    return v;
}

static struct array *find_array(const char name[2], int type) {
    for (int i = 0; i < n_arrays; i++)
        if (ms_arrays[i].name[0] == name[0] && ms_arrays[i].name[1] == name[1] &&
            ms_arrays[i].type == type)
            return &ms_arrays[i];
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
    struct array *a = &ms_arrays[n_arrays++];
    a->name[0] = name[0];
    a->name[1] = name[1];
    a->type = type;
    a->ndims = ndims;
    for (int i = 0; i < ndims; i++) a->dims[i] = dims[i];
    a->count = count;
    a->off = array_used;
    memset(ms_array_pool + array_used, 0, size);
    array_used += size;
    return a;
}

// The subscripts in brackets (ip at the "("): the element's address.
static unsigned char *element(const char name[2], int type) {
    int subs[MAX_DIMS], n = 0;
    ip++;
    for (;;) {
        double x = ms_num_expr();
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
        if (cur() == ',') {
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
    return ms_array_pool + a->off + index * elem_size(type);
}

int ms_place(struct ms_place *p) {
    char name[2];
    int type;
    if (!read_name(name, &type)) return 0;
    p->type = type;
    p->var = 0;
    p->elem = 0;
    p->special = 0;
    if (cur() == '(') {
        p->elem = element(name, type);
        return p->elem != 0;
    }
    p->special = ms_special(name, type);
    if (p->special) return 1;
    p->var = find_var(name, type);
    return p->var != 0;
}

void ms_get_place(const struct ms_place *p, struct ms_val *v) {
    v->str = p->type == MS_STR;
    if (p->special) {
        ms_special_get(p->special, v);
        return;
    }
    if (p->type == MS_STR) {
        struct strref r;
        if (p->var) r = p->var->s;
        else memcpy(&r, p->elem, sizeof(r));
        v->s = ms_strings + r.off;
        v->len = r.len;
    } else if (p->var) {
        v->n = p->var->n;
    } else if (p->type == MS_INT) {
        short i;
        memcpy(&i, p->elem, 2);
        v->n = i;
    } else {
        memcpy(&v->n, p->elem, 8);
    }
}

void ms_set_place(const struct ms_place *p, const struct ms_val *v) {
    if ((p->type == MS_STR) != v->str) {
        error(E_TYPE);
        return;
    }
    if (p->special) {
        ms_special_set(p->special, v);
        return;
    }
    if (p->type == MS_STR) {
        struct strref r;
        str_store(&r, v->s, v->len);
        if (err != E_NONE) return;
        if (p->var) p->var->s = r;
        else memcpy(p->elem, &r, sizeof(r));
        return;
    }
    double x = v->n;
    if (p->type == MS_INT) {
        x = num_floor(x);
        if (x < D.int_min || x > 32767) {
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

void ms_make_str(struct ms_val *v, const unsigned char *s, int len) {
    v->str = 1;
    v->s = s;
    v->len = len;
}

void ms_make_num(struct ms_val *v, double n) {
    v->str = 0;
    v->n = n;
}

static void primary(struct ms_val *v);

static void bracketed(struct ms_val *v) {
    if (!expect('(')) return;
    ms_expr(v);
    expect(')');
}

double ms_num_arg(void) {
    struct ms_val v;
    bracketed(&v);
    if (v.str) error(E_TYPE);
    return v.n;
}

void ms_str_arg(struct ms_val *v) {
    ms_expr(v);
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

static void call_fn(struct ms_val *v) {
    char name[2];
    int type;
    if (!read_name(name, &type)) return;
    if (type != MS_REAL) {
        error(E_SYNTAX);
        return;
    }
    double arg = ms_num_arg();
    if (err != E_NONE) return;
    struct fn *f = 0;
    for (int i = 0; i < n_fns; i++)
        if (fns[i].name[0] == name[0] && fns[i].name[1] == name[1]) f = &fns[i];
    if (!f) {
        error(E_FN);
        return;
    }
    struct ms_var *p = find_var(f->param, MS_REAL);
    if (!p) return;
    double saved = p->n;
    p->n = arg;
    const unsigned char *back = ip;
    ip = f->body;
    ms_expr(v);
    if (v->str) error(E_TYPE);
    ip = back;
    p = find_var(f->param, MS_REAL);
    if (p) p->n = saved;
}

#define num_arg ms_num_arg
#define make_num ms_make_num
#define make_str ms_make_str
#define quantity ms_quantity

static void function(int t, struct ms_val *v) {
    double x;
    struct ms_val a;
    int n, m;
    COST(cost_fn);
    if (t == T_SQR || t == T_LOG || t == T_EXP || (t >= T_COS && t <= T_ATN)) COST(cost_maths);
    switch (t) {
    case T_SGN: x = num_arg(); make_num(v, x > 0 ? 1 : x < 0 ? -1 : 0); return;
    case T_INT: make_num(v, ms_fit(num_floor(num_arg()))); return;
    case T_ABS: make_num(v, num_abs(num_arg())); return;
    case T_FRE:
        bracketed(&a);
        make_num(v, ms_fre());
        return;
    case T_POS: bracketed(&a); make_num(v, ms_column()); return;
    case T_SQR:
        x = num_arg();
        if (x < 0) error(E_QUANTITY);
        make_num(v, err == E_NONE ? ms_fit(num_sqrt(x)) : 0);
        return;
    case T_RND:
        x = num_arg();
        make_num(v, err == E_NONE ? ms_rnd(x) : 0);
        return;
    case T_LOG:
        x = num_arg();
        if (x <= 0) error(E_QUANTITY);
        make_num(v, err == E_NONE ? ms_fit(num_ln(x)) : 0);
        return;
    case T_EXP:
        x = num_arg();
        if (x > 88.03) error(E_OVERFLOW);
        make_num(v, err == E_NONE ? ms_fit(num_exp(x)) : 0);
        return;
    case T_COS: make_num(v, ms_fit(num_cos(num_arg()))); return;
    case T_SIN: make_num(v, ms_fit(num_sin(num_arg()))); return;
    case T_TAN:
        x = num_arg();
        if (err == E_NONE && num_cos(x) == 0) error(E_DIV0);
        make_num(v, err == E_NONE ? ms_fit(num_tan(x)) : 0);
        return;
    case T_ATN: make_num(v, ms_fit(num_atan2(num_arg(), 1))); return;
    case T_PEEK: {
        if (!expect('(')) return;
        unsigned int addr = ms_address();
        expect(')');
        make_num(v, err == E_NONE ? ms_peek(addr) : 0);
        return;
    }
    case T_LEN:
        expect('(');
        ms_str_arg(&a);
        expect(')');
        make_num(v, a.len);
        return;
    case T_STR: {
        x = num_arg();
        unsigned char *s = ms_scratch(24);
        if (!s) return;
        ms_format(x, (char *)s);
        make_str(v, s, strlen((char *)s));
        return;
    }
    case T_VAL:
        expect('(');
        ms_str_arg(&a);
        expect(')');
        if (err != E_NONE) return;
        parse_number(a.s, a.len, &x);
        make_num(v, ms_fit(x));
        return;
    case T_ASC:
        expect('(');
        ms_str_arg(&a);
        expect(')');
        if (err == E_NONE && a.len == 0) error(E_QUANTITY);
        make_num(v, err == E_NONE ? a.s[0] : 0);
        return;
    case T_CHR: {
        expect('(');
        n = quantity(255);
        expect(')');
        unsigned char *s = ms_scratch(1);
        if (!s) return;
        s[0] = n;
        make_str(v, s, 1);
        return;
    }
    case T_LEFT:
    case T_RIGHT:
        expect('(');
        ms_str_arg(&a);
        expect(',');
        n = quantity(255);
        expect(')');
        if (err != E_NONE) return;
        if (n > a.len) n = a.len;
        make_str(v, t == T_LEFT ? a.s : a.s + a.len - n, n);
        return;
    case T_MID:
        expect('(');
        ms_str_arg(&a);
        expect(',');
        n = quantity(255);
        m = 255;
        if (cur() == ',') {
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
    if (!ms_function(t, v)) error(E_SYNTAX);
}

static void primary(struct ms_val *v) {
    make_num(v, 0);
    if (err != E_NONE) return;
    int c = cur();
    if (is_digit(c) || c == '.') {
        const char *p = (const char *)ip;
        double x;
        if (!num_parse(&p, &x)) {
            error(E_SYNTAX);
            return;
        }
        ip = (const unsigned char *)p;
        make_num(v, ms_fit(x));
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
        struct ms_place p;
        if (!ms_place(&p)) return;
        ms_get_place(&p, v);
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
        while (n-- > 0 && err == E_NONE) r = ms_fit(r * a);
        if (b < 0) {
            if (r == 0) {
                error(E_DIV0);
                return 0;
            }
            r = ms_fit(1 / r);
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
    return ms_fit(num_exp(l));
}

static void pow_level(struct ms_val *v) {
    primary(v);
    while (cur() == '^' && err == E_NONE) {
        ip++;
        struct ms_val b;
        if (cur() == '-') {               // 2^-1
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
        COST(cost_maths);
        v->n = power(v->n, b.n);
    }
}

static void unary(struct ms_val *v) {
    if (cur() == '-') {
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

static void mul_level(struct ms_val *v) {
    unary(v);
    while ((cur() == '*' || *ip == '/') && err == E_NONE) {
        int op = *ip++;
        struct ms_val b;
        unary(&b);
        if (v->str || b.str) {
            error(E_TYPE);
            return;
        }
        COST(cost_mul);
        if (op == '*') {
            v->n = ms_fit(v->n * b.n);
        } else if (b.n == 0) {
            error(E_DIV0);
            return;
        } else {
            v->n = ms_fit(v->n / b.n);
        }
    }
}

static void add_level(struct ms_val *v) {
    mul_level(v);
    while ((cur() == '+' || *ip == '-') && err == E_NONE) {
        int op = *ip++;
        struct ms_val b;
        mul_level(&b);
        COST(cost_op);
        if (v->str != b.str || (v->str && op == '-')) {
            error(E_TYPE);
            return;
        }
        if (v->str) {
            if (v->len + b.len > 255) {
                error(E_LONG);
                return;
            }
            unsigned char *s = ms_scratch(v->len + b.len);
            if (!s) return;
            memcpy(s, v->s, v->len);
            memcpy(s + v->len, b.s, b.len);
            make_str(v, s, v->len + b.len);
        } else {
            v->n = ms_fit(op == '+' ? v->n + b.n : v->n - b.n);
        }
    }
}

// A comparison: bits 1 <, 2 =, 4 >; or 0.
static int rel_op(void) {
    int bits = 0;
    for (;;) {
        int c = cur();
        if (c == '<' && !(bits & 1)) bits |= 1;
        else if (c == '=' && !(bits & 2)) bits |= 2;
        else if (c == '>' && !(bits & 4)) bits |= 4;
        else return bits;
        ip++;
    }
}

static void rel_level(struct ms_val *v) {
    add_level(v);
    for (;;) {
        if (err != E_NONE) return;
        int c = cur();
        if (c != '<' && c != '=' && c != '>') return;
        int bits = rel_op();
        struct ms_val b;
        add_level(&b);
        COST(cost_op);
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
        make_num(v, r && D.bitwise ? -1 : r);
    }
}

// A number as a 16-bit integer, for AND, OR and NOT (bitwise).
static int int16(double x) {
    x = num_floor(x);
    if (x < -32768 || x > 32767) {
        error(E_QUANTITY);
        return 0;
    }
    return (int)x;
}

static void not_level(struct ms_val *v) {
    if (cur() == T_NOT) {
        ip++;
        not_level(v);
        if (v->str) error(E_TYPE);
        make_num(v, D.bitwise ? ~int16(v->n) : v->n == 0);
        return;
    }
    rel_level(v);
}

static void and_level(struct ms_val *v) {
    not_level(v);
    while (cur() == T_AND && err == E_NONE) {
        ip++;
        struct ms_val b;
        not_level(&b);
        if (v->str || b.str) error(E_TYPE);
        make_num(v, D.bitwise ? (short)(int16(v->n) & int16(b.n)) : v->n != 0 && b.n != 0);
    }
}

void ms_expr(struct ms_val *v) {
    if (++depth > 40) {
        error(E_COMPLEX);
        make_num(v, 0);
        depth--;
        return;
    }
    and_level(v);
    while (cur() == T_OR && err == E_NONE) {
        ip++;
        struct ms_val b;
        and_level(&b);
        if (v->str || b.str) error(E_TYPE);
        make_num(v, D.bitwise ? (short)(int16(v->n) | int16(b.n)) : v->n != 0 || b.n != 0);
    }
    depth--;
}

/* ------------------------------------------------------------------------
 * Statements
 * --------------------------------------------------------------------- */

int ms_at_end(void) { return cur() == 0 || *ip == ':'; }
#define at_end ms_at_end

static void trace_line(void) {
    if (ms_tracing && cur_line >= 0) {
        ms_out('#');
        out_int(ms_line_num(cur_line));
        ms_out(' ');
    }
}

// A line number written in the program (GOTO 100): -1 if none.
long ms_line_number(void) {
    if (!is_digit(cur())) return -1;
    long n = 0;
    while (is_digit(*ip)) {
        n = n * 10 + (*ip++ - '0');
        if (n > 999999) n = 999999;
    }
    return n;
}
#define line_number ms_line_number

void ms_goto_line(long num) {
    int off = ms_line_find((int)(num > 65535 ? 65535 : num));
    if (num < 0 || num > 63999 || off >= ms_prog_len || ms_line_num(off) != num) {
        error(E_UNDEF);
        return;
    }
    cur_line = off;
    ip = line_text(off);
    jumped = 1;
    trace_line();
}
#define goto_line ms_goto_line

void ms_skip_statement(void) {
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

void ms_skip_line(void) {
    while (*ip) ip++;
}

static void let(void) {
    struct ms_place p;
    if (!ms_place(&p)) return;
    if (!expect('=')) return;
    struct ms_val v;
    ms_expr(&v);
    if (err == E_NONE) ms_set_place(&p, &v);
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

void ms_print_items(void);
#define print ms_print_items

void ms_print_items(void) {
    int newline = 1;
    while (!at_end() && err == E_NONE) {
        int c = *ip;
        if (c == ';') {
            ip++;
            newline = 0;
            continue;
        }
        if (c == ',') {                   // the next field
            ip++;
            newline = 0;
            ms_print_comma();
            continue;
        }
        if (c == T_TAB || c == T_SPC) {
            ip++;
            int n = quantity(255);
            expect(')');
            if (err != E_NONE) return;
            ms_print_tab(n, c == T_SPC);
            newline = 0;
            continue;
        }
        struct ms_val v;
        ms_expr(&v);
        if (err != E_NONE) return;
        if (v.str) for (int i = 0; i < v.len; i++) ms_out(v.s[i]);
        else ms_print_number(v.n);
        newline = 1;
    }
    if (newline && err == E_NONE) ms_out(13);
}

/* INPUT, GET, READ */

HIGH_MEM char ms_inbuf[260];

// (Through a function: see core/mem.h.)
__attribute__((noinline, noclone)) static int first_char(const char *s) { return s[0]; }
static int break_now(void);

// One item of typed input or DATA at *s: a number or string for a
// variable of the type. Returns 0 if it isn't one (?REENTER), and
// moves *s past it (and its comma, if any).
static int read_item(const char **s, int type, struct ms_val *v, int is_data) {
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
        if (type == MS_STR && is_data)    // (DATA strings lose their trailing spaces)
            while (end > start && end[-1] == ' ') end--;
    }
    if (*p && *p != ',' && !(!is_data && *p == ':')) return 0;
    if (type == MS_STR) {
        unsigned char *t = ms_scratch(end - start + 1);
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

static int read_input(const unsigned char *prompt, int plen, int more) {
    if (!ms_input_line(prompt, plen, more, ms_inbuf, 240)) return 0;
    if (first_char(ms_inbuf) == 3) {
        error(E_BREAK);
        return 0;
    }
    return 1;
}

static void input_from(const unsigned char *prompt, int plen) {
    const unsigned char *vars = ip;
again:
    ip = vars;
    if (!read_input(prompt, plen, 0)) return;
    if (D.empty_input_ends && !first_char(ms_inbuf)) {
        ms_skip_statement();
        return;
    }
    const char *s = ms_inbuf;
    for (;;) {
        struct ms_place p;
        if (!ms_place(&p)) return;
        if (!*s && s != ms_inbuf) {       // more needed: ??
            if (!read_input(0, 0, 1)) return;
            s = ms_inbuf;
        }
        struct ms_val v;
        if (!read_item(&s, p.type, &v, 0) || (p.type != MS_STR && !first_char(ms_inbuf))) {
            if (onerr_line >= 0 || ms_input_quiet) {
                error(ms_input_quiet ? ms_input_quiet : E_REENTER);
                return;
            }
            outs(D.redo);
            ms_out(13);
            goto again;
        }
        ms_set_place(&p, &v);
        if (err != E_NONE) return;
        if (cur() != ',') break;
        ip++;
    }
    while (*s == ' ') s++;
    if (*s && !ms_input_quiet) {
        outs("?EXTRA IGNORED");
        ms_out(13);
    }
}

void ms_input_vars(void) { input_from(0, 0); }

static void input(void) {
    if (cur_line < 0) {
        error(E_DIRECT);
        return;
    }
    const unsigned char *prompt = 0;
    int plen = 0;
    if (cur() == '"') {
        struct ms_val v;
        primary(&v);
        prompt = v.s;
        plen = v.len;
        if (!expect(';')) return;
    }
    input_from(prompt, plen);
}

static void get(void) {
    if (cur_line < 0) {
        error(E_DIRECT);
        return;
    }
    ms_get_vars();
}

void ms_get_vars(void) {
    for (;;) {
        struct ms_place p;
        if (!ms_place(&p)) return;
        int k = ms_get_key();
        if (k == -2) return;
        struct ms_val v;
        if (p.type == MS_STR) {
            unsigned char *s = ms_scratch(1);
            if (!s) return;
            s[0] = k;
            make_str(&v, s, k >= 0);
        } else if (k < 0) {
            make_num(&v, 0);
        } else {
            if (k < '0' || k > '9') {
                error(E_SYNTAX);
                return;
            }
            make_num(&v, k - '0');
        }
        ms_set_place(&p, &v);
        if (err != E_NONE || cur() != ',') return;
        ip++;
    }
}

// The next DATA item's text (in data_ip), finding the next DATA statement.
static int next_data(void) {
    if (data_ip && *data_ip && *data_ip != ':') return 1;
    // Look on from here for a DATA token, outside strings.
    const unsigned char *q = data_ip;
    if (!data_started) {
        data_started = 1;
        data_line = 0;
        q = 0;
    }
    for (;;) {
        if (data_line >= ms_prog_len) return 0;
        if (!q) q = line_text(data_line);
        int in_str = 0;
        while (*q) {
            if (*q == '"') in_str = !in_str;
            else if (!in_str && *q == T_REM) {
                while (*q) q++;
                break;
            } else if (!in_str && *q == T_DATA) {
                data_ip = q + 1;
                return 1;
            }
            q++;
        }
        data_line = ms_line_next(data_line);
        q = 0;
    }
}

static void read_data(void) {
    for (;;) {
        struct ms_place p;
        if (!ms_place(&p)) return;
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
        const char *t = item;
        struct ms_val v;
        if (!read_item(&t, p.type, &v, 1)) {
            cur_line = data_line;         // (the error is in the DATA line)
            error(E_SYNTAX);
            return;
        }
        ms_set_place(&p, &v);
        if (err != E_NONE || cur() != ',') return;
        ip++;
    }
}

static void for_loop(void) {
    char name[2];
    int type;
    if (!read_name(name, &type)) return;
    if (type != MS_REAL || cur() == '(') {
        error(E_SYNTAX);
        return;
    }
    if (!expect('=')) return;
    double from = ms_num_expr();
    if (!expect(T_TO)) return;
    double limit = ms_num_expr(), step = 1;
    if (cur() == T_STEP) {
        ip++;
        step = ms_num_expr();
    }
    if (err != E_NONE) return;
    struct ms_var *v = find_var(name, MS_REAL);
    if (!v) return;
    v->n = from;
    int index = v - ms_vars;
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
        if (is_alpha(cur())) {
            char name[2];
            int type;
            read_name(name, &type);
            struct ms_var *v = find_var(name, MS_REAL);
            if (!v) return;
            int index = v - ms_vars;
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
        struct ms_var *v = &ms_vars[stack[i].var];
        v->n = ms_fit(v->n + stack[i].step);
        if (err != E_NONE) return;
        int done = stack[i].step >= 0 ? v->n > stack[i].limit : v->n < stack[i].limit;
        if (!done) {
            cur_line = stack[i].line;
            ip = stack[i].ip;
            jumped = 1;
            return;
        }
        sp = i;
        if (cur() != ',') return;
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
            double x = ms_num_expr();
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
            if (cur() != ',') break;
            ip++;
        }
        if (!expect(')')) return;
        if (find_array(name, type)) {
            error(E_REDIM);
            return;
        }
        if (!make_array(name, type, n, dims)) return;
        if (cur() != ',') return;
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
    if (!read_name(name, &type) || type != MS_REAL) {
        error(E_SYNTAX);
        return;
    }
    if (!expect('(')) return;
    if (!read_name(param, &ptype) || ptype != MS_REAL) {
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
    ms_skip_statement();
}

HIGH_MEM char ms_listbuf[MAX_LINE * 8];

static void list_program(int from, int to) {
    for (int off = ms_line_find(from); off < ms_prog_len && ms_line_num(off) <= to;
         off = ms_line_next(off)) {
        if (ms_break()) break;
        ms_detokenize(off, ms_listbuf);
        ms_list_line(ms_listbuf);
    }
}

static void list_args(void) {
    int from = 0, to = 63999;
    long n = line_number();
    if (n >= 0) from = to = (int)n;
    if (cur() == '-' || *ip == ',') {
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

void ms_run_from(long num) {
    ms_clear_vars();
    if (D.onerr) ms_poke(216, 0);         // (ONERR off)
    if (ms_prog_len == 0) {
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

static void on_goto(void) {
    int n = quantity(255);
    if (err != E_NONE) return;
    int t = cur();
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
            ms_skip_statement();          // (where RETURN comes back to)
            if (t == T_GOSUB) push_gosub();
            if (err == E_NONE) goto_line(num);
            return;
        }
        if (cur() != ',') return;         // (n is 0, or beyond the list: on)
        ip++;
    }
}

static void wait_statement(void) {
    unsigned int a = ms_address();
    expect(',');
    int mask = quantity(255), flip = 0;
    if (cur() == ',') {
        ip++;
        flip = quantity(255);
    }
    if (err != E_NONE) return;
    while (!((ms_peek(a) ^ flip) & mask)) {
        if (break_now()) return;
        ms_wait_idle();
    }
}

static void statement(void) {
    if (err != E_NONE) return;
    int t = cur(), n;
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
    case T_DATA: ms_skip_statement(); return;
    case T_RESTORE:
        data_started = 0;
        data_ip = 0;
        return;
    case T_IF: {
        struct ms_val v;
        ms_expr(&v);
        if (err != E_NONE) return;
        int truth = v.str ? v.len != 0 : v.n != 0;
        if (cur() == T_THEN) ip++;
        else if (*ip != T_GOTO) {
            error(E_SYNTAX);
            return;
        }
        if (!truth) {
            ms_skip_line();
            return;
        }
        if (is_digit(cur())) goto_line(line_number());
        else jumped = 1;                  // (go on with the statement after THEN)
        return;
    }
    case T_GO:                            // GO TO
        if (!expect(T_TO)) return;
        /* fall through */
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
        ms_poke(216, 0x80);
        ms_skip_line();                   // (the rest of the line: not run)
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
    case T_REM: ms_skip_line(); return;
    case T_DIM: dim(); return;
    case T_DEF: def_fn(); return;
    case T_POKE: {
        unsigned int a = ms_address();
        expect(',');
        n = quantity(255);
        if (err == E_NONE) ms_poke(a, n);
        return;
    }
    case T_WAIT: wait_statement(); return;
    case T_TRACE: ms_tracing = 1; return;
    case T_NOTRACE: ms_tracing = 0; return;
    case T_CLEAR: ms_clear_vars(); return;
    case T_NEW:
        ms_prog_len = 0;
        ms_clear_vars();
        ended = 1;
        return;
    case T_RUN:
        num = line_number();
        if (!at_end() && num < 0) {
            error(E_SYNTAX);
            return;
        }
        ms_run_from(num);
        return;
    case T_LIST: list_args(); return;
    case T_DEL: {
        num = line_number();
        long to = -1;
        if (cur() == ',') {
            ip++;
            to = line_number();
        }
        if (num < 0 || to < 0) {
            error(E_SYNTAX);
            return;
        }
        for (int off = ms_line_find((int)num); off < ms_prog_len && ms_line_num(off) <= to;)
            ms_line_delete(ms_line_num(off));
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
    }
    if (!ms_statement(t)) error(E_SYNTAX);
}

/* ------------------------------------------------------------------------
 * Running lines, errors, BREAK
 * --------------------------------------------------------------------- */

static int poll_count;

static int break_now(void) {
    if (ms_break()) {
        break_ip = ip;
        error(E_BREAK);
        return 1;
    }
    return 0;
}

static void report(void) {
    int line = cur_line >= 0 ? ms_line_num(cur_line) : -1;
    if (err == E_BREAK && line >= 0) {
        cont.ok = 1;
        cont.line = cur_line;
        cont.ip = break_ip ? break_ip : stmt_start;
    }
    ms_report(line);
}

// An error while running: to ONERR's line, if there is one (returns 1).
static int catch_error(void) {
    if (onerr_line < 0 || !(ms_peek(216) & 0x80) || cur_line < 0) return 0;
    int code = ms_dos_err ? ms_dos_err : err;
    ms_poke(222, code);
    int line = ms_line_num(cur_line);
    ms_poke(218, line & 0xFF);
    ms_poke(219, line >> 8);
    resume.line = cur_line;
    resume.ip = stmt_start;
    err = E_NONE;
    ms_dos_err = 0;
    goto_line(onerr_line);
    return err == E_NONE;
}

void ms_execute(void) {
    ended = 0;
    while (!ended) {
        if (err != E_NONE) {
            if (catch_error()) continue;
            break;
        }
        if (cur() == ':') {
            ip++;
            continue;
        }
        if (*ip == 0) {
            if (cur_line < 0) break;
            int next_off = ms_line_next(cur_line);
            if (next_off >= ms_prog_len) break;   // (the end: no error)
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
        COST(cost_statement);
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
    ms_program_ended();
    err = E_NONE;
    ms_dos_err = 0;
    cur_line = -1;
}

/* ------------------------------------------------------------------------
 * Entering lines, LIST
 * --------------------------------------------------------------------- */

HIGH_MEM unsigned char ms_typed[MAX_LINE + 2];

static void line_error(int e) {
    err = e;
    ms_report(-1);
    err = E_NONE;
}

void ms_enter_line(const char *text) {
    while (*text == ' ') text++;
    if (is_digit(*text)) {
        long num = 0;
        while (is_digit(*text)) {
            num = num * 10 + (*text++ - '0');
            if (num > 999999) num = 999999;
        }
        if (num > 63999) {
            line_error(E_SYNTAX);
            return;
        }
        ms_clear_vars();
        while (*text == ' ') text++;
        int n = crunch(text, ms_typed, MAX_LINE);
        if (n < 0) {
            line_error(E_LONG);           // (hmm: the line is too long)
            return;
        }
        int blank = 1;
        for (int i = 0; i < n; i++) if (ms_typed[i] != ' ') blank = 0;
        if (blank) {
            ms_line_delete((int)num);
            return;
        }
        if (!line_insert((int)num, ms_typed, n + 1)) line_error(E_MEMORY);
        return;
    }
    if (crunch(text, ms_typed, MAX_LINE) < 0) return;
    while (sp > 0 && stack[sp - 1].line < 0) sp--;   // (loops on the typed line before)
    cur_line = -1;
    ip = ms_typed;
    ms_execute();
}

static int is_operator(int c) {
    return c == '+' || c == '-' || c == '*' || c == '/' || c == '^' || c == '<' || c == '=' ||
           c == '>';
}

// The line as LIST shows it: the number, then (in a dialect that takes
// spaces out) a space before and after each keyword and operator.
int ms_detokenize(int off, char *buf) {
    char *p = buf;
    char num[8];
    itoa(ms_line_num(off), num);
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
        const char *w = c >= 0x80 ? ms_keyword_name(c) : 0;
        if (w) {
            if (D.list_spaces) *p++ = ' ';
            for (; *w; w++) *p++ = *w;
            if (D.list_spaces) *p++ = ' ';
            if (c == T_REM) raw = 1;
            if (c == T_DATA) raw = 2;
        } else if (is_operator(c) && D.list_spaces) {
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

int ms_store_line(int num, const unsigned char *text, int len) {
    if (len < 1 || len > MAX_LINE + 1) return 0;
    return line_insert(num, text, len);
}

void ms_chain(void) {
    sp = 0;
    data_started = 0;
    data_ip = 0;
    if (ms_prog_len == 0) {
        ended = 1;
        return;
    }
    cur_line = 0;
    ip = line_text(0);
    jumped = 1;
}

void ms_enter_text(const char *text, int len) {
    ms_prog_len = 0;
    ms_clear_vars();
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
        if (is_digit(*p)) ms_enter_line(line);
    }
}
