/*
 * Part of the Spectrum's BASIC (included by ../basic.c, not built on its
 * own): SAVE, LOAD, VERIFY, MERGE, CAT and ERASE, with the boot disk for
 * a tape (core/storage.c), and listings as text files on the FAT
 * partition.
 */

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
    if (v.len > SPECTRUM_NAME_LEN) {
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
    if (storage_file(name)) return;       // (listed already, by its own name)
    print_string(name);
    for (int i = strlen(name); i < 16; i++) print_char(' ');
    itoa(len, buf);
    print_string(buf);
    print_string(" TEXT\n");
}

static void st_cat() {
    if (checking) return;
    int free_kb;
    const char *err = storage_list(cat_entry, &free_kb);
    if (err) { error(err); return; }
    fat_list(cat_text);
    char buf[8];
    itoa(free_kb, buf);
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
