/*
 * Saved files: each one a file in the machine's folder on the FAT
 * partition (see core/fat.h), so they can be copied to and from other
 * computers, backed up, and moved between sticks.
 *
 * A saved file is the 48-byte header below, then the data:
 *   0-7    "X86BSAV1"
 *   8-23   the name it was saved with (up to 15 characters, NUL-padded)
 *   24-27  the length of the data
 *   28-29  param (see storage.h)
 *   30     the kind of file (FILE_PROGRAM ...)
 *   32-35  a checksum of the data (FNV-1a)
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

#define HEADER    48
#define MAGIC     "X86BSAV1"
#define CANDIDATES 36                       // NAME.SAV, NAME~1.SAV ... NAME~Z.SAV

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

static int same_name(const unsigned char *saved, const char *name) {
    for (int i = 0; i < STORE_NAME_LEN; i++) {
        if (toupper(saved[i]) != toupper(name[i])) return 0;
        if (!name[i]) return 1;
    }
    return 1;
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
    if (fat_read_start(file, look.head, HEADER, &n) || n < HEADER ||
        memcmp(look.head, MAGIC, 8) != 0)
        return;
    if (look.name[0] ? same_name(look.head + 8, look.name) : look.head[30] == look.type) {
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
    memset(h, 0, HEADER);
    memcpy(h, MAGIC, 8);
    memcpy(h + 8, name, strlen(name));
    put32(h + 24, len);
    put16(h + 28, param);
    h[30] = type;
    put32(h + 32, checksum(data, len));
    return fat_write_header(look.file, h, HEADER, data, len);
}

const char *storage_load(const char *name, int type, unsigned char *data, int cap, int *len,
                         int *param) {
    const char *err = name[0] ? check_name(name) : 0;
    if (!err) err = find(name, type);
    if (err) return err;
    if (!look.found) return "R Program not found";
    if (look.head[30] != type) return "R Wrong kind of file";
    unsigned int n = get32(look.head + 24);
    if (n > (unsigned int)cap) return "4 Out of memory";
    int got;
    err = fat_read_from(look.file, HEADER, data, cap, &got);
    if (err) return err;
    if ((unsigned int)got != n || checksum(data, n) != get32(look.head + 32))
        return "R Bad program file";
    *len = n;
    *param = get16(look.head + 28);
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
    unsigned char h[HEADER];
    int n;
    if (!ends_with_sav(file) || fat_read_start(file, h, HEADER, &n) || n < HEADER ||
        memcmp(h, MAGIC, 8) != 0)
        return;
    char name[STORE_NAME_LEN + 1];
    memcpy(name, h + 8, STORE_NAME_LEN);
    name[STORE_NAME_LEN] = '\0';
    list_each(name, get32(h + 24), get16(h + 28), h[30]);
}

const char *storage_list(void (*each)(const char *name, int len, int param, int type),
                         int *free_kb) {
    list_each = each;
    const char *err = fat_list(list_one);
    if (err && strcmp(err, "R Program not found") != 0) return err;
    return fat_free_kb(free_kb);
}
