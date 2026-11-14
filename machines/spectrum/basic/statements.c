/*
 * Part of the Spectrum's BASIC (included by ../basic.c, not built on its
 * own): the statements: PRINT and the printer, FOR, GO SUB, INPUT, DIM,
 * READ, PAUSE, BEEP and PLAY.
 */

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
#define PRINTER_FILE "PRINTOUT.TXT"       // (in the machine's folder)
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
