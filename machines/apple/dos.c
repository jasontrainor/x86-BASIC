/*
 * DOS 3.3-style disk commands (see dos.h), on the FAT partition.
 *
 *   CATALOG          the files: type (I, A, T, B), size in 256-byte
 *                    sectors, name
 *   SAVE name        the program, as a listing in a saved file
 *   LOAD name        a program saved here, or a listing (GAME.BAS)
 *   RUN name         LOAD it, then RUN it
 *   DELETE name
 *   LOCK, UNLOCK, MON, NOMON, MAXFILES, INT, FP, PR#, IN#: accepted
 *
 * Options after a name (",S6,D1", ",V254") are ignored: there's one disk.
 */
#include "dos.h"
#include "apple.h"
#include "storage.h"
#include "fat.h"
#include "string.h"
#include "mem.h"

#define BUF_SIZE (60 * 1024)
HIGH_MEM char dos_buf[BUF_SIZE];

static const struct dos_basic *basic;

void dos_init(const struct dos_basic *b, const char *folder) {
    basic = b;
    fat_folder(folder);
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
    const char *s = msg;
    for (; *s; s++) {
        if (!strncmp(s, "not found", 9)) return DOS_NOT_FOUND;
        if (!strncmp(s, "Disk full", 9)) return DOS_DISK_FULL;
        if (!strncmp(s, "Wrong kind", 10)) return DOS_TYPE_MISMATCH;
        if (!strncmp(s, "Out of memory", 13)) return DOS_TOO_LARGE;
        if (!strncmp(s, "Invalid file name", 17)) return DOS_SYNTAX;
        if (!strncmp(s, "No FAT", 6)) return DOS_IO;
    }
    return DOS_IO;
}

/* ------------------------------------------------------------------------
 * Parsing
 * --------------------------------------------------------------------- */

// If line starts with the word (spaces before it allowed), the rest.
static const char *word(const char *line, const char *w) {
    while (*line == ' ') line++;
    while (*w) {
        if (toupper(*line) != *w) return 0;
        line++, w++;
    }
    return line;
}

// The file name in s (up to a comma: options follow), in name.
static int file_name(const char *s, char *name) {
    while (*s == ' ') s++;
    int n = 0;
    while (*s && *s != ',' && n < 30) name[n++] = *s++;
    while (n > 0 && name[n - 1] == ' ') n--;
    name[n] = '\0';
    if (*s && *s != ',') return DOS_SYNTAX;
    if (n == 0) return DOS_SYNTAX;
    if (n > STORE_NAME_LEN) return DOS_SYNTAX;
    return DOS_OK;
}

/* ------------------------------------------------------------------------
 * The commands
 * --------------------------------------------------------------------- */

static int ends_with(const char *s, const char *end) {
    int n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}

static void number3(int n) {
    char b[4] = { '0' + n / 100 % 10, '0' + n / 10 % 10, '0' + n % 10, 0 };
    apple_print(b);
}

static void catalog_saved(const char *name, int len, int param, int type) {
    int t = type == FILE_PROGRAM ? (param == 'I' ? 'I' : 'A') : type == FILE_BYTES ? 'B' : 'T';
    apple_print(" ");
    apple_cout(t);
    apple_cout(' ');
    number3((len + 255) / 256 + 1);
    apple_cout(' ');
    apple_print(name);
    apple_cout(13);
}

static void catalog_plain(const char *name, int len) {
    if (storage_file(name)) return;        // (listed above, by its own name)
    apple_print(" ");
    apple_cout(ends_with(name, ".BAS") ? basic->type : 'T');
    apple_cout(' ');
    number3((len + 255) / 256 + 1);
    apple_cout(' ');
    apple_print(name);
    apple_cout(13);
}

static int catalog(void) {
    int kb;
    apple_cout(13);
    apple_print("DISK VOLUME 254");
    apple_cout(13);
    apple_cout(13);
    const char *e = storage_list(catalog_saved, &kb);
    if (!e) e = fat_list(catalog_plain);
    return dos_error(e);
}

static int save(const char *name) {
    int len = basic->listing(dos_buf, BUF_SIZE);
    if (len < 0) return DOS_TOO_LARGE;
    return dos_error(storage_save(name, (unsigned char *)dos_buf, len, basic->type, FILE_PROGRAM));
}

static int load(const char *name) {
    int len, param;
    const char *e = storage_load(name, FILE_PROGRAM, (unsigned char *)dos_buf, BUF_SIZE - 1,
                                 &len, &param);
    if (e && dos_error(e) == DOS_NOT_FOUND) {
        // A listing from a PC, by its file name.
        e = fat_read(name, (unsigned char *)dos_buf, BUF_SIZE - 1, &len);
        if (e) return dos_error(e);
        param = basic->type;
    } else if (e) {
        return dos_error(e);
    }
    if (param != basic->type) return DOS_LANGUAGE;
    dos_buf[len] = '\0';
    return basic->enter(dos_buf, len);
}

int dos_command(const char *line, int direct) {
    const char *r;
    char name[32];
    int e;
    (void)direct;
    if ((r = word(line, "CATALOG"))) return catalog();
    if ((r = word(line, "SAVE")) && *r) {
        if ((e = file_name(r, name))) return e;
        return save(name);
    }
    if ((r = word(line, "LOAD")) && *r) {
        if ((e = file_name(r, name))) return e;
        return load(name);
    }
    if ((r = word(line, "RUN")) && *r && !isdigit(*r)) {
        const char *p = r;
        while (*p == ' ') p++;
        if (isdigit(*p)) return DOS_NONE;  // (BASIC's RUN 100)
        if ((e = file_name(r, name))) return e;
        e = load(name);
        return e ? e : DOS_RUN;
    }
    if ((r = word(line, "DELETE"))) {
        if ((e = file_name(r, name))) return e;
        e = dos_error(storage_erase(name));
        if (e == DOS_NOT_FOUND) e = dos_error(fat_erase(name));
        return e;
    }
    if ((r = word(line, "LOCK")) || (r = word(line, "UNLOCK")) || (r = word(line, "VERIFY"))) {
        if ((e = file_name(r, name))) return e;
        return DOS_OK;
    }
    if (word(line, "MAXFILES") || word(line, "MON") || word(line, "NOMON")) return DOS_OK;
    return DOS_NONE;
}
