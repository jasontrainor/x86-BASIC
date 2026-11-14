/*
 * Saved files: each one a file in the machine's folder on the FAT
 * partition (see core/fat.h), so they can be copied to and from other
 * computers, backed up, and moved between sticks.
 *
 * A saved file is a header, then the data. A name of up to 15 characters
 * has the 48-byte header that all files had before longer names:
 *   0-7    "X86BSAV1"
 *   8-23   the name it was saved with (NUL-padded)
 *   24-27  the length of the data
 *   28-29  param (see storage.h)
 *   30     the kind of file (FILE_PROGRAM ...)
 *   32-35  a checksum of the data (FNV-1a)
 * A longer one (up to 30, as DOS 3.3 allows) has a 64-byte header, the
 * same but for a longer name:
 *   0-7    "X86BSAV2"
 *   8-39   the name (NUL-padded)
 *   40-43  the length, 44-45 param, 46 the kind, 48-51 the checksum
 *
 * Its name on the disk comes from the saved name: its first 8 letters and
 * digits (others become _), in capitals, and .SAV: "my game" is
 * MY_GAME.SAV. Names that would be the same file ("Game" and "GAME 2",
 * say) go in NAME~1.SAV to NAME~Z.SAV. Names are matched ignoring case.
 * tools/savefile.py makes and reads these files on a PC.
 */
#include "storage.h"
#include "fat.h"
#include "string.h"

#define HEADER    64                       // (room for either)
#define SHORT_NAME 15                      // the longest name with the old header
#define CANDIDATES 36                       // NAME.SAV, NAME~1.SAV ... NAME~Z.SAV

// A header's fields, for either kind.
struct head {
    int size;                              // 48 or 64
    const unsigned char *name;             // (not NUL-terminated if it fills its field)
    int name_field;                        // 16 or 32
    unsigned int len, sum;
    int param, type;
};

static unsigned int get16(const unsigned char *p);
static unsigned int get32(const unsigned char *p);

// Read a header (n bytes of the file's start): 0 if it isn't one.
static int read_head(const unsigned char *h, int n, struct head *out) {
    if (n >= 48 && memcmp(h, "X86BSAV1", 8) == 0) {
        out->size = 48;
        out->name_field = 16;
    } else if (n >= 64 && memcmp(h, "X86BSAV2", 8) == 0) {
        out->size = 64;
        out->name_field = 32;
    } else {
        return 0;
    }
    const unsigned char *f = h + 8 + out->name_field;
    out->name = h + 8;
    out->len = get32(f);
    out->param = get16(f + 4);
    out->type = f[6];
    out->sum = get32(f + 8);
    return 1;
}
static unsigned int get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void put16(unsigned char *p, unsigned int v) { p[0] = v; p[1] = v >> 8; }
static unsigned int get32(const unsigned char *p) { return get16(p) | (get16(p + 2) << 16); }
static void put32(unsigned char *p, unsigned int v) { put16(p, v); put16(p + 2, v >> 16); }

static unsigned int checksum(const unsigned char *data, int len) {
    unsigned int h = 2166136261u;          // FNV-1a
    for (int i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

static const char *check_name(const char *name) {
    int n = strlen(name);
    if (n == 0 || n > STORE_NAME_LEN) return "F Invalid file name";
    return 0;
}

static int same_name(const struct head *h, const char *name) {
    for (int i = 0; i < h->name_field; i++) {
        if (toupper(h->name[i]) != toupper(name[i])) return 0;
        if (!name[i]) return 1;
    }
    return !name[h->name_field];
}

static int ends_with_sav(const char *s) {
    int n = strlen(s);
    return n > 4 && strcmp(s + n - 4, ".SAV") == 0;
}

int storage_file(const char *disk_name) {
    return ends_with_sav(disk_name);
}

// The disk name of candidate k (0-35) for a saved name.
static void disk_name(const char *name, int k, char *out) {
    int n = 0;
    for (int i = 0; name[i] && n < 8; i++)
        out[n++] = isalnum(name[i]) ? toupper(name[i]) : '_';
    if (k) {
        if (n > 6) n = 6;
        out[n++] = '~';
        out[n++] = k < 10 ? '0' + k : 'A' + k - 10;
    }
    strcpy(out + n, ".SAV");
}

/* ------------------------------------------------------------------------
 * Finding a file: one look through the folder, reading the header of
 * each file that could be it.
 * --------------------------------------------------------------------- */

static struct {
    const char *name;                      // the saved name, or "": the first of type
    int type;
    unsigned long exists;                  // bit k: candidate k is on the disk
    int found;
    char file[13];                         // where it is
    unsigned char head[HEADER];
    struct head h;                         // (its fields)
} look;

static void look_at(const char *file, int len) {
    if (look.found || !ends_with_sav(file)) return;
    if (look.name[0]) {
        int k;
        char cand[13];
        for (k = 0; k < CANDIDATES; k++) {
            disk_name(look.name, k, cand);
            if (strcmp(cand, file) == 0) break;
        }
        if (k == CANDIDATES) return;
        look.exists |= 1UL << k;
    }
    int n;
    if (fat_read_start(file, look.head, HEADER, &n) || !read_head(look.head, n, &look.h))
        return;
    if (look.name[0] ? same_name(&look.h, look.name) : look.h.type == look.type) {
        look.found = 1;
        strcpy(look.file, file);
    }
}

static const char *find(const char *name, int type) {
    look.name = name;
    look.type = type;
    look.exists = 0;
    look.found = 0;
    const char *err = fat_list(look_at);
    // (A folder that isn't there yet: nothing has been saved.)
    if (err && strcmp(err, "R Program not found") != 0) return err;
    return 0;
}

/* ------------------------------------------------------------------------ */

const char *storage_save(const char *name, const unsigned char *data, int len, int param,
                         int type) {
    const char *err = check_name(name);
    if (!err) err = find(name, 0);
    if (err) return err;
    if (!look.found) {
        int k;
        for (k = 0; k < CANDIDATES && (look.exists >> k & 1); k++) {}
        if (k == CANDIDATES) return "R Disk full";
        disk_name(name, k, look.file);
    }
    unsigned char *h = look.head;
    int n = strlen(name), name_field = n > SHORT_NAME ? 32 : 16;
    memset(h, 0, HEADER);
    memcpy(h, n > SHORT_NAME ? "X86BSAV2" : "X86BSAV1", 8);
    memcpy(h + 8, name, n);
    unsigned char *f = h + 8 + name_field;
    put32(f, len);
    put16(f + 4, param);
    f[6] = type;
    put32(f + 8, checksum(data, len));
    return fat_write_header(look.file, h, name_field == 32 ? 64 : 48, data, len);
}

const char *storage_load(const char *name, int type, unsigned char *data, int cap, int *len,
                         int *param) {
    const char *err = name[0] ? check_name(name) : 0;
    if (!err) err = find(name, type);
    if (err) return err;
    if (!look.found) return "R Program not found";
    if (look.h.type != type) return "R Wrong kind of file";
    unsigned int n = look.h.len;
    if (n > (unsigned int)cap) return "4 Out of memory";
    int got;
    err = fat_read_from(look.file, look.h.size, data, cap, &got);
    if (err) return err;
    if ((unsigned int)got != n || checksum(data, n) != look.h.sum)
        return "R Bad program file";
    *len = n;
    *param = look.h.param;
    return 0;
}

const char *storage_erase(const char *name) {
    const char *err = check_name(name);
    if (!err) err = find(name, 0);
    if (err) return err;
    if (!look.found) return "R Program not found";
    return fat_erase(look.file);
}

static void (*list_each)(const char *name, int len, int param, int type);

static void list_one(const char *file, int len) {
    unsigned char raw[HEADER];
    struct head h;
    int n;
    if (!ends_with_sav(file) || fat_read_start(file, raw, HEADER, &n) || !read_head(raw, n, &h))
        return;
    char name[STORE_NAME_LEN + 1];
    int k;
    for (k = 0; k < h.name_field && k < STORE_NAME_LEN && h.name[k]; k++) name[k] = h.name[k];
    name[k] = '\0';
    list_each(name, h.len, h.param, h.type);
}

const char *storage_list(void (*each)(const char *name, int len, int param, int type),
                         int *free_kb) {
    list_each = each;
    const char *err = fat_list(list_one);
    if (err && strcmp(err, "R Program not found") != 0) return err;
    return fat_free_kb(free_kb);
}
