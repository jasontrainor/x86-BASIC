/*
 * Saving programs on the boot disk (SAVE, LOAD, CAT, ERASE).
 *
 * Programs are kept in a reserved area of the disk image, as raw sectors
 * (see the layout in boot/boot.asm): a directory sector, then one fixed
 * slot per program, big enough for the largest program. That's simple,
 * needs no filesystem driver, and the BIOS and UEFI kernels share it.
 *
 * Directory sector:
 *   0-15   magic "X86BASIC-SAVES2" (the 2: tokenized program format 2,
 *          with 2-byte line lengths)
 *   32-..  STORE_SLOTS entries of 32 bytes:
 *            0-15  name, NUL-padded (empty: the slot is free)
 *            16-17 program length in bytes
 *            18-19 line to RUN after loading (SAVE ... LINE n), or 0
 *            20-23 checksum of the program bytes
 */
#include "storage.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

#define DIR_LBA       STORE_LBA
#define SLOT_LBA(i)   (STORE_LBA + 1 + (i) * STORE_SLOT_SECTORS)
#define MAGIC         "X86BASIC-SAVES2"
#define ENTRY(i)      (dir + 32 + (i) * 32)

HIGH_MEM static unsigned char dir[512];

static unsigned int get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void put16(unsigned char *p, unsigned int v) { p[0] = v; p[1] = v >> 8; }
static unsigned int get32(const unsigned char *p) { return get16(p) | (get16(p + 2) << 16); }
static void put32(unsigned char *p, unsigned int v) { put16(p, v); put16(p + 2, v >> 16); }

static unsigned int checksum(const unsigned char *data, int len) {
    unsigned int h = 2166136261u;          // FNV-1a
    for (int i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

// Read the directory; a disk that has never been saved to has none yet,
// which is the same as an empty one.
static const char *read_dir(void) {
    if (plat_disk_read(DIR_LBA, dir) < 0) return "R Disk error";
    if (memcmp(dir, MAGIC, 16) != 0) {
        memset(dir, 0, sizeof(dir));
        memcpy(dir, MAGIC, 16);
    }
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
    for (int i = 0; i < STORE_SLOTS; i++) {
        if (ENTRY(i)[0] && same_name(ENTRY(i), name)) return i;
    }
    return -1;
}

static const char *check_name(const char *name) {
    int n = strlen(name);
    if (n == 0 || n > STORE_NAME_LEN) return "F Invalid file name";
    return 0;
}

const char *storage_save(const char *name, const unsigned char *data, int len, int autorun) {
    const char *err = check_name(name);
    if (!err) err = read_dir();
    if (err) return err;
    int slot = find(name);
    if (slot < 0) {
        for (slot = 0; slot < STORE_SLOTS && ENTRY(slot)[0]; slot++) {}
        if (slot == STORE_SLOTS) return "R Disk full";
    }
    unsigned char sector[512];
    for (int i = 0; i * 512 < len; i++) {
        int n = len - i * 512 < 512 ? len - i * 512 : 512;
        memset(sector, 0, sizeof(sector));
        memcpy(sector, data + i * 512, n);
        if (plat_disk_write(SLOT_LBA(slot) + i, sector) < 0) return "R Disk error";
    }
    unsigned char *e = ENTRY(slot);
    memset(e, 0, 32);
    memcpy(e, name, strlen(name));
    put16(e + 16, len);
    put16(e + 18, autorun);
    put32(e + 20, checksum(data, len));
    if (plat_disk_write(DIR_LBA, dir) < 0) return "R Disk error";
    return 0;
}

const char *storage_load(const char *name, unsigned char *data, int cap, int *len, int *autorun) {
    const char *err = check_name(name);
    if (!err) err = read_dir();
    if (err) return err;
    int slot = find(name);
    if (slot < 0) return "R Program not found";
    unsigned char *e = ENTRY(slot);
    int n = get16(e + 16);
    if (n > cap || n > STORE_SLOT_SECTORS * 512) return "R Bad program file";
    unsigned char sector[512];
    for (int i = 0; i * 512 < n; i++) {
        if (plat_disk_read(SLOT_LBA(slot) + i, sector) < 0) return "R Disk error";
        memcpy(data + i * 512, sector, n - i * 512 < 512 ? n - i * 512 : 512);
    }
    if (checksum(data, n) != get32(e + 20)) return "R Bad program file";
    *len = n;
    *autorun = get16(e + 18);
    return 0;
}

const char *storage_erase(const char *name) {
    const char *err = check_name(name);
    if (!err) err = read_dir();
    if (err) return err;
    int slot = find(name);
    if (slot < 0) return "R Program not found";
    memset(ENTRY(slot), 0, 32);
    if (plat_disk_write(DIR_LBA, dir) < 0) return "R Disk error";
    return 0;
}

const char *storage_list(void (*each)(const char *name, int len, int autorun), int *free_slots) {
    const char *err = read_dir();
    if (err) return err;
    *free_slots = 0;
    for (int i = 0; i < STORE_SLOTS; i++) {
        unsigned char *e = ENTRY(i);
        if (!e[0]) {
            (*free_slots)++;
            continue;
        }
        char name[STORE_NAME_LEN + 1];
        memcpy(name, e, STORE_NAME_LEN);
        name[STORE_NAME_LEN] = '\0';
        each(name, get16(e + 16), get16(e + 18));
    }
    return 0;
}
