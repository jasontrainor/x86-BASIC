/*
 * Files in the root directory of the FAT partition: the EFI system
 * partition, which tools/mkimage.py makes FAT12 (FAT16 works too). It's
 * the one part of the disk that other computers can read and write, so
 * listings can be moved to and from a PC on it.
 *
 * The whole FAT and root directory are read in for each operation and
 * written back (all copies of the FAT) after it. A file being replaced
 * is only freed once the new one is written, if there's room for both.
 */
#include "fat.h"
#include "storage.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

#define FAT_LBA          STORE_END        // where the partition starts
#define MAX_FAT_SECTORS  16
#define MAX_ROOT_SECTORS 32

HIGH_MEM static unsigned char fat[MAX_FAT_SECTORS * 512];
HIGH_MEM static unsigned char root[MAX_ROOT_SECTORS * 512];

static int per_cluster, fat_start, fat_sectors, fats, root_start, root_sectors;
static int root_entries, data_start, clusters, fat16;

static unsigned int get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void put16(unsigned char *p, unsigned int v) { p[0] = v; p[1] = v >> 8; }
static unsigned int get32(const unsigned char *p) { return get16(p) | (get16(p + 2) << 16); }
static void put32(unsigned char *p, unsigned int v) { put16(p, v); put16(p + 2, v >> 16); }

static const char *disk_error = "R Disk error";

// Read the boot sector, FAT and root directory.
static const char *mount(void) {
    unsigned char b[512];
    if (plat_disk_read(FAT_LBA, b) < 0) return disk_error;
    per_cluster = b[13];
    int reserved = get16(b + 14);
    fats = b[16];
    root_entries = get16(b + 17);
    unsigned int total = get16(b + 19) ? get16(b + 19) : get32(b + 32);
    fat_sectors = get16(b + 22);
    root_sectors = (root_entries * 32 + 511) / 512;
    if (b[510] != 0x55 || b[511] != 0xAA || get16(b + 11) != 512 || per_cluster == 0 ||
        (per_cluster & (per_cluster - 1)) || reserved < 1 || fats < 1 || fats > 2 ||
        fat_sectors < 1 || fat_sectors > MAX_FAT_SECTORS || root_sectors < 1 ||
        root_sectors > MAX_ROOT_SECTORS)
        return "R No FAT partition";
    fat_start = FAT_LBA + reserved;
    root_start = fat_start + fats * fat_sectors;
    data_start = root_start + root_sectors;
    unsigned int meta = data_start - FAT_LBA;
    if (total <= meta) return "R No FAT partition";
    clusters = (total - meta) / per_cluster;
    fat16 = clusters >= 4085;
    // The FAT must have an entry for every cluster (2 to clusters + 1).
    unsigned int need = fat16 ? (clusters + 2) * 2 : ((clusters + 2) * 3 + 1) / 2;
    if (clusters >= 65525 || need > (unsigned int)fat_sectors * 512) return "R No FAT partition";
    for (int i = 0; i < fat_sectors; i++)
        if (plat_disk_read(fat_start + i, fat + i * 512) < 0) return disk_error;
    for (int i = 0; i < root_sectors; i++)
        if (plat_disk_read(root_start + i, root + i * 512) < 0) return disk_error;
    return 0;
}

static unsigned int get_fat(unsigned int c) {
    if (fat16) return get16(fat + c * 2);
    unsigned int w = get16(fat + c * 3 / 2);
    return c & 1 ? w >> 4 : w & 0xFFF;
}

static void set_fat(unsigned int c, unsigned int v) {
    if (fat16) {
        put16(fat + c * 2, v);
        return;
    }
    unsigned char *q = fat + c * 3 / 2;
    if (c & 1) {
        q[0] = (q[0] & 0x0F) | ((v & 0x0F) << 4);
        q[1] = v >> 4;
    } else {
        q[0] = v;
        q[1] = (q[1] & 0xF0) | ((v >> 8) & 0x0F);
    }
}

static unsigned int end_mark(void) { return fat16 ? 0xFFFF : 0xFFF; }
static int is_cluster(unsigned int c) { return c >= 2 && c < (unsigned int)clusters + 2; }
static int cluster_lba(unsigned int c) { return data_start + (c - 2) * per_cluster; }

// Write the FAT (each copy) and the root directory back.
static const char *flush(void) {
    for (int f = 0; f < fats; f++)
        for (int i = 0; i < fat_sectors; i++)
            if (plat_disk_write(fat_start + f * fat_sectors + i, fat + i * 512) < 0)
                return disk_error;
    for (int i = 0; i < root_sectors; i++)
        if (plat_disk_write(root_start + i, root + i * 512) < 0) return disk_error;
    return 0;
}

static void free_chain(unsigned int c) {
    for (int n = 0; is_cluster(c) && n < clusters; n++) {
        unsigned int next = get_fat(c);
        set_fat(c, 0);
        c = next;
    }
}

// Characters allowed in DOS names, besides letters and digits.
static int name_char(unsigned char c) {
    const char *ok = "!#$%&'()-@^_`{}~";
    for (; *ok; ok++)
        if (c == (unsigned char)*ok) return 1;
    return 0;
}

// "game.bas" -> "GAME    BAS"; 0 if it isn't a valid 8.3 name.
static int short_name(const char *name, unsigned char *out) {
    memset(out, ' ', 11);
    int i = 0, n = 0;
    for (; name[i] && name[i] != '.'; i++) {
        if (n == 8) return 0;
        out[n++] = toupper(name[i]);
    }
    if (n == 0) return 0;
    if (name[i] == '.') {
        i++;
        for (n = 8; name[i]; i++) {
            if (n == 11) return 0;
            out[n++] = toupper(name[i]);
        }
    }
    for (int k = 0; k < 11; k++) {
        unsigned char c = out[k];
        if (c == ' ') continue;
        if (!isalnum(c) && !name_char(c)) return 0;
    }
    return 1;
}

static unsigned char *entry(int i) { return root + i * 32; }

// Is root directory entry i a file (not free, a directory, label or long name)?
static int is_file(int i) {
    unsigned char *e = entry(i);
    return e[0] != 0 && e[0] != 0xE5 && !(e[11] & 0x18);   // (0x0F: long names)
}

static int find(const unsigned char *sname) {
    for (int i = 0; i < root_entries && entry(i)[0]; i++)
        if (is_file(i) && memcmp(entry(i), sname, 11) == 0) return i;
    return -1;
}

static const char *open_file(const char *name, unsigned char *sname, int *index) {
    if (!short_name(name, sname)) return "F Invalid file name";
    const char *err = mount();
    if (err) return err;
    *index = find(sname);
    return 0;
}

const char *fat_read(const char *name, unsigned char *buf, int cap, int *len) {
    unsigned char sname[11];
    int i;
    const char *err = open_file(name, sname, &i);
    if (err) return err;
    if (i < 0) return "R Program not found";
    unsigned char *e = entry(i);
    unsigned int size = get32(e + 28), c = get16(e + 26), done = 0;
    if (size > (unsigned int)cap) return "4 Out of memory";
    unsigned char sector[512];
    while (done < size) {
        if (!is_cluster(c)) return "R Bad program file";
        for (int s = 0; s < per_cluster && done < size; s++) {
            if (plat_disk_read(cluster_lba(c) + s, sector) < 0) return disk_error;
            unsigned int n = size - done < 512 ? size - done : 512;
            memcpy(buf + done, sector, n);
            done += n;
        }
        c = get_fat(c);
    }
    *len = size;
    return 0;
}

const char *fat_write(const char *name, const unsigned char *data, int len) {
    unsigned char sname[11];
    int i;
    const char *err = open_file(name, sname, &i);
    if (err) return err;
    unsigned int old = i >= 0 ? get16(entry(i) + 26) : 0;
    if (i < 0) {                          // a new entry
        for (i = 0; i < root_entries && entry(i)[0] && entry(i)[0] != 0xE5; i++) {}
        if (i == root_entries) return "R Disk full";
    }
    int bytes = per_cluster * 512;
    int need = (len + bytes - 1) / bytes, free = 0;
    for (int c = 2; c < clusters + 2; c++) free += get_fat(c) == 0;
    if (free < need) {                    // only room if the old file goes first
        free_chain(old);
        old = 0;
        free = 0;
        for (int c = 2; c < clusters + 2; c++) free += get_fat(c) == 0;
        if (free < need) {
            mount();                      // (forget the change: nothing is written)
            return "R Disk full";
        }
    }
    // Take free clusters in order and write the data into them.
    unsigned int first = 0, prev = 0, c = 2;
    unsigned char sector[512];
    for (int k = 0; k < need; k++) {
        while (get_fat(c) != 0) c++;
        set_fat(c, end_mark());
        if (prev) set_fat(prev, c);
        else first = c;
        prev = c;
        for (int s = 0; s < per_cluster; s++) {
            int off = k * bytes + s * 512;
            memset(sector, 0, 512);
            if (off < len) memcpy(sector, data + off, len - off < 512 ? len - off : 512);
            if (plat_disk_write(cluster_lba(c) + s, sector) < 0) return disk_error;
        }
    }
    if (old) free_chain(old);
    unsigned char *e = entry(i);
    if (e[0] == 0 || e[0] == 0xE5) {
        memset(e, 0, 32);
        memcpy(e, sname, 11);
        e[11] = 0x20;                     // archive
    }
    unsigned int date = (1 << 5) | 1;     // (no clock: 1 January 1980)
    put16(e + 16, date);                  // created
    put16(e + 18, date);                  // accessed
    put16(e + 22, 0);                     // modified
    put16(e + 24, date);
    put16(e + 26, first);
    put32(e + 28, len);
    return flush();
}

const char *fat_erase(const char *name) {
    unsigned char sname[11];
    int i;
    const char *err = open_file(name, sname, &i);
    if (err) return err;
    if (i < 0) return "R Program not found";
    free_chain(get16(entry(i) + 26));
    entry(i)[0] = 0xE5;
    for (int k = i - 1; k >= 0 && entry(k)[11] == 0x0F && entry(k)[0] != 0xE5; k--)
        entry(k)[0] = 0xE5;               // its long name, if it has one
    return flush();
}

const char *fat_list(void (*each)(const char *name, int len)) {
    const char *err = mount();
    if (err) return err;
    for (int i = 0; i < root_entries && entry(i)[0]; i++) {
        if (!is_file(i)) continue;
        unsigned char *e = entry(i);
        char name[13];
        int n = 0;
        for (int k = 0; k < 8 && e[k] != ' '; k++) name[n++] = e[k];
        if (e[8] != ' ') {
            name[n++] = '.';
            for (int k = 8; k < 11 && e[k] != ' '; k++) name[n++] = e[k];
        }
        name[n] = '\0';
        each(name, get32(e + 28));
    }
    return 0;
}
