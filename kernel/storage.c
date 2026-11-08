/*
 * Saving programs and bytes on the boot disk (SAVE, LOAD, CAT, ERASE...).
 *
 * Files are kept in a reserved area of the disk image, as raw sectors
 * (see kernel/storage.h): a directory, then the files' data, each file
 * in consecutive sectors wherever there is room for it. That's simple,
 * needs no filesystem driver, and the BIOS and UEFI kernels share it.
 *
 * Directory (STORE_DIR_SECTORS sectors):
 *   0-15   magic "X86BASIC-SAVES3"
 *   32-..  STORE_FILES entries of 32 bytes:
 *            0-15  name, NUL-padded (empty: the entry is free)
 *            16-19 length in bytes
 *            20-21 programs: the line to RUN after loading (SAVE ...
 *                  LINE n), or 0; bytes: the address they came from;
 *                  arrays: the array's name
 *            22-25 checksum of the bytes
 *            26    the kind of file (FILE_PROGRAM ...)
 *            28-29 the first sector, counted from the end of the directory
 */
#include "storage.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

#define DATA_LBA      (STORE_LBA + STORE_DIR_SECTORS)
#define DATA_SECTORS  (STORE_END - DATA_LBA)
#define MAGIC         "X86BASIC-SAVES3"
#define ENTRY(i)      (dir + 32 + (i) * 32)

HIGH_MEM static unsigned char dir[STORE_DIR_SECTORS * 512];

static unsigned int get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void put16(unsigned char *p, unsigned int v) { p[0] = v; p[1] = v >> 8; }
static unsigned int get32(const unsigned char *p) { return get16(p) | (get16(p + 2) << 16); }
static void put32(unsigned char *p, unsigned int v) { put16(p, v); put16(p + 2, v >> 16); }

static unsigned int checksum(const unsigned char *data, int len) {
    unsigned int h = 2166136261u;          // FNV-1a
    for (int i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

static int sectors_for(unsigned int len) { return (len + 511) / 512; }
static int entry_start(const unsigned char *e) { return get16(e + 28); }
static int entry_sectors(const unsigned char *e) { return sectors_for(get32(e + 16)); }

// Read the directory; a disk that has never been saved to has none yet,
// which is the same as an empty one.
static const char *read_dir(void) {
    for (int i = 0; i < STORE_DIR_SECTORS; i++)
        if (plat_disk_read(STORE_LBA + i, dir + i * 512) < 0) return "R Disk error";
    if (memcmp(dir, MAGIC, 16) != 0) {
        memset(dir, 0, sizeof(dir));
        memcpy(dir, MAGIC, 16);
    }
    return 0;
}

// Write back the directory sector holding entry i (and the first, with
// the magic, in case this is a new directory).
static const char *write_dir(int i) {
    int s = (32 + i * 32) / 512;
    if (plat_disk_write(STORE_LBA, dir) < 0) return "R Disk error";
    if (s && plat_disk_write(STORE_LBA + s, dir + s * 512) < 0) return "R Disk error";
    return 0;
}

static int same_name(const unsigned char *entry, const char *name) {
    for (int i = 0; i < STORE_NAME_LEN; i++) {
        if (toupper(entry[i]) != toupper(name[i])) return 0;
        if (!name[i]) return 1;
    }
    return 1;
}

static int find(const char *name) {
    for (int i = 0; i < STORE_FILES; i++) {
        if (ENTRY(i)[0] && same_name(ENTRY(i), name)) return i;
    }
    return -1;
}

// The first run of n free sectors (ignoring entry skip's), or -1.
static int find_room(int n, int skip) {
    int start = 0;
    for (;;) {
        if (start + n > DATA_SECTORS) return -1;
        int clash = 0;
        for (int i = 0; i < STORE_FILES; i++) {
            unsigned char *e = ENTRY(i);
            if (i == skip || !e[0]) continue;
            int s = entry_start(e), m = entry_sectors(e);
            if (m && s < start + n && start < s + m) {
                start = s + m;
                clash = 1;
                break;
            }
        }
        if (!clash) return start;
    }
}

static const char *check_name(const char *name) {
    int n = strlen(name);
    if (n == 0 || n > STORE_NAME_LEN) return "F Invalid file name";
    return 0;
}

const char *storage_save(const char *name, const unsigned char *data, int len, int param,
                         int type) {
    const char *err = check_name(name);
    if (!err) err = read_dir();
    if (err) return err;
    int slot = find(name);
    if (slot < 0) {
        for (slot = 0; slot < STORE_FILES && ENTRY(slot)[0]; slot++) {}
        if (slot == STORE_FILES) return "R Disk full";
    }
    // Somewhere new if there's room, so the old file survives a failed
    // save; otherwise over the old one.
    int start = find_room(sectors_for(len), -1);
    if (start < 0 && ENTRY(slot)[0]) start = find_room(sectors_for(len), slot);
    if (start < 0) return "R Disk full";
    unsigned char sector[512];
    for (int i = 0; i * 512 < len; i++) {
        int n = len - i * 512 < 512 ? len - i * 512 : 512;
        memset(sector, 0, sizeof(sector));
        memcpy(sector, data + i * 512, n);
        if (plat_disk_write(DATA_LBA + start + i, sector) < 0) return "R Disk error";
    }
    unsigned char *e = ENTRY(slot);
    memset(e, 0, 32);
    memcpy(e, name, strlen(name));
    put32(e + 16, len);
    put16(e + 20, param);
    put32(e + 22, checksum(data, len));
    e[26] = type;
    put16(e + 28, start);
    return write_dir(slot);
}

const char *storage_load(const char *name, int type, unsigned char *data, int cap, int *len,
                         int *param) {
    const char *err = name[0] ? check_name(name) : 0;
    if (!err) err = read_dir();
    if (err) return err;
    int slot = -1;
    if (name[0]) {
        slot = find(name);
    } else {                              // LOAD "": the first of its kind
        for (int i = 0; i < STORE_FILES && slot < 0; i++)
            if (ENTRY(i)[0] && ENTRY(i)[26] == type) slot = i;
    }
    if (slot < 0) return "R Program not found";
    unsigned char *e = ENTRY(slot);
    if (e[26] != type) return "R Wrong kind of file";
    unsigned int n = get32(e + 16);
    if (n > (unsigned int)cap) return "4 Out of memory";
    if (entry_start(e) + sectors_for(n) > DATA_SECTORS) return "R Bad program file";
    unsigned char sector[512];
    for (unsigned int i = 0; i * 512 < n; i++) {
        if (plat_disk_read(DATA_LBA + entry_start(e) + i, sector) < 0) return "R Disk error";
        memcpy(data + i * 512, sector, n - i * 512 < 512 ? n - i * 512 : 512);
    }
    if (checksum(data, n) != get32(e + 22)) return "R Bad program file";
    *len = n;
    *param = get16(e + 20);
    return 0;
}

const char *storage_erase(const char *name) {
    const char *err = check_name(name);
    if (!err) err = read_dir();
    if (err) return err;
    int slot = find(name);
    if (slot < 0) return "R Program not found";
    memset(ENTRY(slot), 0, 32);
    return write_dir(slot);
}

const char *storage_list(void (*each)(const char *name, int len, int param, int type),
                         int *free_files, int *free_kb) {
    const char *err = read_dir();
    if (err) return err;
    int used = 0;
    *free_files = 0;
    for (int i = 0; i < STORE_FILES; i++) {
        unsigned char *e = ENTRY(i);
        if (!e[0]) {
            (*free_files)++;
            continue;
        }
        used += entry_sectors(e);
        char name[STORE_NAME_LEN + 1];
        memcpy(name, e, STORE_NAME_LEN);
        name[STORE_NAME_LEN] = '\0';
        each(name, get32(e + 16), get16(e + 20), e[26]);
    }
    *free_kb = (DATA_SECTORS - used) / 2;
    return 0;
}
