/*
 * DOS 3.3-style disk commands (see dos.h), on the FAT partition.
 *
 *   CATALOG               the files: type (I, A, T, B), size in 256-byte
 *                         sectors, name
 *   SAVE, LOAD, RUN name  programs (saved as listings); LOAD and RUN
 *                         also take a listing from a PC (GAME.BAS)
 *   DELETE name, RENAME old,new, LOCK and UNLOCK name (*: locked)
 *   BSAVE name,A addr,L len   memory; BLOAD name[,A addr]; BRUN loads
 *                         it, but machine code isn't run
 *   OPEN name[,L len]     a text file (made if it isn't there)
 *   READ name[,R n][,B n] INPUT and GET read from it from now on
 *   WRITE name[,R n][,B n] PRINT writes to it from now on
 *   APPEND name           open it, at its end (then WRITE)
 *   POSITION name,R n     on n lines
 *   CLOSE [name]          written to the disk (all files, without a name)
 *   EXEC name             its lines, as if typed
 *   MON, NOMON, MAXFILES, VERIFY, INIT, PR#, IN#, INT, FP: accepted
 *
 * Numbers after A, L, R and B can be decimal or $hex. Other options
 * (",S6,D1", ",V254") are ignored: there's one disk. In a text file, a
 * line ends with a Return (on the disk: a newline; either is read).
 * READ and WRITE last until the next command. OPEN, READ, WRITE,
 * APPEND and POSITION can't be typed at the prompt (NOT DIRECT COMMAND).
 */
#include "dos.h"
#include "apple.h"
#include "storage.h"
#include "fat.h"
#include "string.h"
#include "mem.h"

#define BUF_SIZE (40 * 1024)
HIGH_MEM char dos_buf[BUF_SIZE];

#define FILES 3
#define FILE_CAP (16 * 1024)
HIGH_MEM unsigned char dos_files[FILES][FILE_CAP];

// (Through a function: see core/mem.h.)
__attribute__((noinline, noclone)) static unsigned char *file_data(int i) {
    return dos_files[i];
}

static const struct dos_basic *basic;

static struct {
    char name[32];
    int open, len, pos, reclen, dirty;
} files[FILES];

enum { IDLE, READING, WRITING };
static int mode, current;                 // READ or WRITE, and which file
static int exec_file = -1, exec_pos;      // EXEC: 0 while there is one, and where
static int result = DOS_NONE;
static int capturing;
static char command[256];
static int command_len;

void dos_init(const struct dos_basic *b, const char *folder) {
    basic = b;
    fat_folder(folder);
    for (int i = 0; i < FILES; i++) files[i].open = 0;
    mode = IDLE;
    exec_file = -1;
}

const char *dos_message(int e) {
    switch (e) {
    case DOS_LANGUAGE:        return "LANGUAGE NOT AVAILABLE";
    case DOS_RANGE:           return "RANGE ERROR";
    case DOS_WRITE_PROTECTED: return "WRITE PROTECTED";
    case DOS_END_OF_DATA:     return "END OF DATA";
    case DOS_NOT_FOUND:       return "FILE NOT FOUND";
    case DOS_DISK_FULL:       return "DISK FULL";
    case DOS_LOCKED:          return "FILE LOCKED";
    case DOS_SYNTAX:          return "SYNTAX ERROR";
    case DOS_NO_BUFFERS:      return "NO BUFFERS AVAILABLE";
    case DOS_TYPE_MISMATCH:   return "FILE TYPE MISMATCH";
    case DOS_TOO_LARGE:       return "PROGRAM TOO LARGE";
    case DOS_NOT_DIRECT:      return "NOT DIRECT COMMAND";
    }
    return "I/O ERROR";
}

// A storage or FAT error message as DOS's error.
static int dos_error(const char *msg) {
    if (!msg) return DOS_OK;
    for (const char *s = msg; *s; s++) {
        if (!strncmp(s, "not found", 9)) return DOS_NOT_FOUND;
        if (!strncmp(s, "Disk full", 9)) return DOS_DISK_FULL;
        if (!strncmp(s, "Wrong kind", 10)) return DOS_TYPE_MISMATCH;
        if (!strncmp(s, "Out of memory", 13)) return DOS_TOO_LARGE;
        if (!strncmp(s, "Invalid file name", 17)) return DOS_SYNTAX;
    }
    return DOS_IO;
}

/* ------------------------------------------------------------------------
 * Parsing: the command word, a name, and options
 * --------------------------------------------------------------------- */

static const char *word(const char *line, const char *w) {
    while (*line == ' ') line++;
    while (*w) {
        if (toupper(*line) != *w) return 0;
        line++, w++;
    }
    return line;
}

struct options { long a, l, r, b; char has_a, has_l, has_r, has_b; };

// The name (up to a comma), then options; *rest after the name.
static int parse(const char *s, char *name, struct options *o, const char **rest) {
    memset(o, 0, sizeof(*o));
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != ',' && n < 31) name[n++] = *s++;
    while (n > 0 && name[n - 1] == ' ') n--;
    name[n] = '\0';
    if (rest) *rest = s;
    if (n == 0 || (*s && *s != ',')) return DOS_SYNTAX;
    if (n > STORE_NAME_LEN) return DOS_SYNTAX;
    while (*s == ',') {
        s++;
        while (*s == ' ') s++;
        int letter = toupper(*s++);
        while (*s == ' ') s++;
        long v = 0;
        int digits = 0;
        if (*s == '$') {
            s++;
            for (;; s++) {
                int c = toupper(*s), d = c >= '0' && c <= '9' ? c - '0' :
                                         c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
                if (d < 0) break;
                v = v * 16 + d;
                digits++;
            }
        } else {
            while (isdigit(*s)) v = v * 10 + (*s++ - '0'), digits++;
        }
        if (!digits && letter != 'S' && letter != 'D' && letter != 'V') return DOS_SYNTAX;
        while (*s == ' ') s++;
        switch (letter) {
        case 'A': o->a = v; o->has_a = 1; break;
        case 'L': o->l = v; o->has_l = 1; break;
        case 'R': o->r = v; o->has_r = 1; break;
        case 'B': o->b = v; o->has_b = 1; break;
        case 'S': case 'D': case 'V': break;
        default: return DOS_SYNTAX;
        }
        if (*s && *s != ',') return DOS_SYNTAX;
    }
    return DOS_OK;
}

static int ends_with(const char *s, const char *end) {
    int n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}

/* ------------------------------------------------------------------------
 * CATALOG and programs
 * --------------------------------------------------------------------- */

static void number3(int n) {
    char b[4] = { '0' + n / 100 % 10, '0' + n / 10 % 10, '0' + n % 10, 0 };
    apple_print(b);
}

static void catalog_entry(int type, int len, const char *name) {
    apple_cout(' ');
    apple_cout(type);
    apple_cout(' ');
    number3((len + 255) / 256 + 1);
    apple_cout(' ');
    apple_print(name);
    apple_cout(13);
}

static void catalog_saved(const char *name, int len, int param, int type) {
    catalog_entry(type == FILE_PROGRAM ? (param == 'I' ? 'I' : 'A') :
                  type == FILE_BYTES ? 'B' : 'T', len, name);
}

static void catalog_plain(const char *name, int len) {
    if (storage_file(name)) return;        // (listed above, by its own name)
    catalog_entry(ends_with(name, ".BAS") ? basic->type : 'T', len, name);
}

static int catalog(void) {
    int kb;
    apple_cout(13);
    apple_print("DISK VOLUME 254");
    apple_cout(13);
    apple_cout(13);
    const char *e = storage_list(catalog_saved, &kb);
    if (!e) e = fat_list(catalog_plain);
    int r = dos_error(e);
    return r == DOS_NOT_FOUND ? DOS_OK : r;   // (no folder yet: no files)
}

// A saved file of the type, or a plain file by that name (a PC's).
static int read_file(const char *name, int type, unsigned char *buf, int cap, int *len,
                     int *param) {
    const char *e = storage_load(name, type, buf, cap, len, param);
    if (e && dos_error(e) == DOS_NOT_FOUND) {
        e = fat_read(name, buf, cap, len);
        *param = type == FILE_PROGRAM ? basic->type : 0;
        if (e) return dos_error(e) == DOS_SYNTAX ? DOS_NOT_FOUND : dos_error(e);
        return DOS_OK;
    }
    return dos_error(e);
}

static int save(const char *name) {
    int len = basic->listing(dos_buf, BUF_SIZE);
    if (len < 0) return DOS_TOO_LARGE;
    return dos_error(storage_save(name, (unsigned char *)dos_buf, len, basic->type, FILE_PROGRAM));
}

static int load(const char *name) {
    int len, param;
    int e = read_file(name, FILE_PROGRAM, (unsigned char *)dos_buf, BUF_SIZE - 1, &len, &param);
    if (e) return e;
    if (param != basic->type) return DOS_LANGUAGE;
    dos_buf[len] = '\0';
    return basic->enter(dos_buf, len);
}

static int erase(const char *name) {
    int e = dos_error(storage_erase(name));
    if (e == DOS_NOT_FOUND) e = dos_error(fat_erase(name));
    return e == DOS_SYNTAX ? DOS_NOT_FOUND : e;
}

static int rename_file(const char *old, const char *rest) {
    char name[32];
    struct options o;
    if (*rest != ',') return DOS_SYNTAX;
    int e = parse(rest + 1, name, &o, 0);
    if (e) return e;
    static const int types[] = { FILE_PROGRAM, FILE_TEXT, FILE_BYTES };
    for (int i = 0; i < 3; i++) {
        int len, param;
        const char *m = storage_load(old, types[i], (unsigned char *)dos_buf, BUF_SIZE, &len,
                                     &param);
        if (m && dos_error(m) == DOS_TYPE_MISMATCH) continue;
        if (m) return dos_error(m);
        e = dos_error(storage_save(name, (unsigned char *)dos_buf, len, param, types[i]));
        return e ? e : dos_error(storage_erase(old));
    }
    return DOS_NOT_FOUND;
}

/* ------------------------------------------------------------------------
 * Memory: BSAVE, BLOAD
 * --------------------------------------------------------------------- */

static int bsave(const char *name, const struct options *o) {
    if (!o->has_a || !o->has_l) return DOS_SYNTAX;
    if (o->a > 65535 || o->l < 1 || o->l > 32767 || o->l > BUF_SIZE) return DOS_RANGE;
    for (long i = 0; i < o->l; i++) dos_buf[i] = apple_peek((unsigned int)(o->a + i));
    return dos_error(storage_save(name, (unsigned char *)dos_buf, (int)o->l, (int)o->a,
                                  FILE_BYTES));
}

static int bload(const char *name, const struct options *o) {
    int len, param;
    int e = read_file(name, FILE_BYTES, (unsigned char *)dos_buf, BUF_SIZE, &len, &param);
    if (e) return e;
    unsigned int at = o->has_a ? (unsigned int)o->a : (unsigned int)param & 0xFFFF;
    for (int i = 0; i < len; i++) apple_poke((at + i) & 0xFFFF, (unsigned char)dos_buf[i]);
    return DOS_OK;
}

/* ------------------------------------------------------------------------
 * Text files
 * --------------------------------------------------------------------- */

static int find_open(const char *name) {
    for (int i = 0; i < FILES; i++)
        if (files[i].open && !strcmp(files[i].name, name)) return i;
    return -1;
}

static int flush(int i) {
    if (!files[i].dirty) return DOS_OK;
    files[i].dirty = 0;
    return dos_error(storage_save(files[i].name, file_data(i), files[i].len, 0, FILE_TEXT));
}

static int open_file(const char *name, const struct options *o) {
    int i = find_open(name);
    if (i < 0)
        for (i = 0; i < FILES && files[i].open; i++) {}
    if (i == FILES) return DOS_NO_BUFFERS;
    strcpy(files[i].name, name);
    files[i].pos = 0;
    files[i].reclen = o->has_l ? (int)o->l : 0;
    files[i].dirty = 0;
    int len, param;
    int e = read_file(name, FILE_TEXT, file_data(i), FILE_CAP, &len, &param);
    if (e == DOS_NOT_FOUND) {             // (made, empty)
        len = 0;
        files[i].dirty = 1;
    } else if (e) {
        return e;
    }
    files[i].len = len;
    files[i].open = 1;
    if (files[i].dirty) return flush(i);
    return DOS_OK;
}

static int close_files(const char *name) {
    int e = DOS_OK;
    for (int i = 0; i < FILES; i++) {
        if (!files[i].open || (name && strcmp(files[i].name, name))) continue;
        int f = flush(i);
        if (!e) e = f;
        files[i].open = 0;
        if (mode != IDLE && current == i) mode = IDLE;
    }
    return e;
}

static int position(int i, const struct options *o) {
    if (o->has_r || o->has_b) {
        long p = (o->has_r ? o->r * files[i].reclen : 0) + (o->has_b ? o->b : 0);
        if (p > FILE_CAP) return DOS_RANGE;
        files[i].pos = (int)p;
    }
    return DOS_OK;
}

static int is_return(int c) { return c == '\r' || c == '\n'; }

static int skip_lines(int i, long n) {
    unsigned char *d = file_data(i);
    while (n-- > 0) {
        while (files[i].pos < files[i].len && !is_return(d[files[i].pos])) files[i].pos++;
        if (files[i].pos >= files[i].len) return DOS_END_OF_DATA;
        files[i].pos++;
        if (files[i].pos < files[i].len && d[files[i].pos - 1] == '\r' &&
            d[files[i].pos] == '\n')
            files[i].pos++;
    }
    return DOS_OK;
}

int dos_reading(void) {
    return mode == READING;
}

int dos_read_char(void) {
    if (mode != READING) return -1;
    int i = current;
    if (files[i].pos >= files[i].len) return -1;
    int c = file_data(i)[files[i].pos++];
    if (c == '\n') return 13;
    if (c == '\r') {
        if (files[i].pos < files[i].len && file_data(i)[files[i].pos] == '\n') files[i].pos++;
        return 13;
    }
    return c;
}

int dos_read_line(char *buf, int cap) {
    if (mode != READING) return -1;
    int i = current;
    if (files[i].pos >= files[i].len) return -1;
    int n = 0, c;
    while ((c = dos_read_char()) >= 0 && c != 13)
        if (n < cap - 1) buf[n++] = c;
    buf[n] = '\0';
    return n;
}

static void write_char(int c) {
    int i = current;
    if (files[i].pos >= FILE_CAP) return;
    file_data(i)[files[i].pos++] = c == 13 ? '\n' : c;
    if (files[i].pos > files[i].len) files[i].len = files[i].pos;
    files[i].dirty = 1;
}

/* ------------------------------------------------------------------------
 * EXEC: the file, in a buffer of its own
 * --------------------------------------------------------------------- */

HIGH_MEM char dos_exec[FILE_CAP];
static int exec_len;

__attribute__((noinline, noclone)) static char *exec_data(void) { return dos_exec; }

static int exec(const char *name) {
    int len, param;
    int e = read_file(name, FILE_TEXT, (unsigned char *)exec_data(), FILE_CAP, &len, &param);
    if (e == DOS_TYPE_MISMATCH)
        e = read_file(name, FILE_PROGRAM, (unsigned char *)exec_data(), FILE_CAP, &len, &param);
    if (e) return e;
    exec_len = len;
    exec_pos = 0;
    exec_file = 0;
    return DOS_OK;
}

int dos_exec_line(char *buf, int cap) {
    if (exec_file < 0) return -1;
    char *d = exec_data();
    if (exec_pos >= exec_len) {
        exec_file = -1;
        return -1;
    }
    int n = 0;
    while (exec_pos < exec_len && !is_return(d[exec_pos])) {
        if (n < cap - 1) buf[n++] = d[exec_pos];
        exec_pos++;
    }
    if (exec_pos < exec_len && d[exec_pos] == '\r') exec_pos++;
    if (exec_pos < exec_len && d[exec_pos] == '\n') exec_pos++;
    buf[n] = '\0';
    return n;
}

/* ------------------------------------------------------------------------
 * The commands
 * --------------------------------------------------------------------- */

static int file_command(const char *r, int direct, int which) {
    char name[32];
    struct options o;
    if (direct) return DOS_NOT_DIRECT;
    int e = parse(r, name, &o, 0);
    if (e) return e;
    int i;
    switch (which) {
    case 'O': return open_file(name, &o);
    case 'A':
        if ((e = open_file(name, &o))) return e;
        i = find_open(name);
        files[i].pos = files[i].len;
        return DOS_OK;
    }
    i = find_open(name);
    if (i < 0) {                          // (READ and WRITE open it, if need be)
        if ((e = open_file(name, &o))) return e;
        i = find_open(name);
    }
    if ((e = position(i, &o))) return e;
    current = i;
    switch (which) {
    case 'R': mode = READING; return DOS_OK;
    case 'W': mode = WRITING; return DOS_OK;
    case 'P': return skip_lines(i, o.has_r ? o.r : 0);
    }
    return DOS_SYNTAX;
}

int dos_command(const char *line, int direct) {
    const char *r, *rest;
    char name[32];
    struct options o;
    int e;
    mode = IDLE;                          // (any command ends READ and WRITE)
    while (*line == ' ') line++;
    if (!*line) return direct ? DOS_NONE : DOS_OK;
    if ((r = word(line, "CATALOG"))) return catalog();
    if ((r = word(line, "SAVE")) && *r) return (e = parse(r, name, &o, 0)) ? e : save(name);
    if ((r = word(line, "LOAD")) && *r) return (e = parse(r, name, &o, 0)) ? e : load(name);
    if ((r = word(line, "RUN")) && *r) {
        const char *p = r;
        while (*p == ' ') p++;
        if (isdigit(*p) || !*p) return DOS_NONE;   // (BASIC's RUN 100)
        if ((e = parse(r, name, &o, 0))) return e;
        e = load(name);
        return e ? e : DOS_RUN;
    }
    if ((r = word(line, "DELETE"))) return (e = parse(r, name, &o, 0)) ? e : erase(name);
    if ((r = word(line, "RENAME"))) {
        if ((e = parse(r, name, &o, &rest)) && e != DOS_SYNTAX) return e;
        if (!*name) return DOS_SYNTAX;
        return rename_file(name, rest);
    }
    if ((r = word(line, "BSAVE"))) return (e = parse(r, name, &o, 0)) ? e : bsave(name, &o);
    if ((r = word(line, "BLOAD")) || (r = word(line, "BRUN")))
        return (e = parse(r, name, &o, 0)) ? e : bload(name, &o);
    if ((r = word(line, "OPEN"))) return file_command(r, direct, 'O');
    if ((r = word(line, "APPEND"))) return file_command(r, direct, 'A');
    if ((r = word(line, "READ"))) return file_command(r, direct, 'R');
    if ((r = word(line, "WRITE"))) return file_command(r, direct, 'W');
    if ((r = word(line, "POSITION"))) return file_command(r, direct, 'P');
    if ((r = word(line, "CLOSE"))) {
        while (*r == ' ') r++;
        if (!*r) return close_files(0);
        if ((e = parse(r, name, &o, 0))) return e;
        return close_files(name);
    }
    if ((r = word(line, "EXEC"))) return (e = parse(r, name, &o, 0)) ? e : exec(name);
    if ((r = word(line, "LOCK")) || (r = word(line, "UNLOCK")) || (r = word(line, "VERIFY")))
        return parse(r, name, &o, 0);
    if (word(line, "MAXFILES") || word(line, "NOMON") || word(line, "MON") ||
        word(line, "INIT"))
        return DOS_OK;
    if ((r = word(line, "FP")) && !*r) return basic->type == 'A' ? DOS_OK : DOS_LANGUAGE;
    if ((r = word(line, "INT")) && !*r) return basic->type == 'I' ? DOS_OK : DOS_LANGUAGE;
    if (!direct && (word(line, "PR#") || word(line, "IN#"))) return DOS_OK;
    return DOS_NONE;
}

/* ------------------------------------------------------------------------
 * What programs print
 * --------------------------------------------------------------------- */

int dos_print(int c) {
    c &= 0x7F;
    if (capturing) {
        if (c == 13) {
            capturing = 0;
            command[command_len] = '\0';
            result = dos_command(command, 0);
            if (result == DOS_NONE) result = DOS_SYNTAX;
        } else if (command_len < 255) {
            command[command_len++] = c;
        }
        return 1;
    }
    if (c == 4) {
        capturing = 1;
        command_len = 0;
        mode = IDLE;
        return 1;
    }
    if (mode == WRITING) {
        write_char(c);
        return 1;
    }
    return 0;
}

int dos_result(void) {
    int r = result;
    result = DOS_NONE;
    return r;
}

void dos_program_ended(void) {
    mode = IDLE;
    capturing = 0;
}
