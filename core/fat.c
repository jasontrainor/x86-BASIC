/*
 * Files on the FAT partition: the one part of the disk that other
 * computers can read and write. It's found from the partition table
 * (the EFI system partition, or any FAT12/FAT16 partition), and may be
 * any size.
 *
 * Little is kept in memory: a few sectors of the FAT, one sector of a
 * directory and one of data, read when needed and written back (to
 * every copy of the FAT) at the end of each operation. The partition is
 * looked at afresh for each operation, so a disk changed elsewhere is
 * never written with stale information.
 *
 * A file being replaced is only freed once the new one is written, if
 * there's room for both.
 */
#include "fat.h"
#include "platform.h"
#include "string.h"
#include "mem.h"

#define FAT_CACHE 4                       // sectors of the FAT held at once
#define DIR_BUF   FAT_CACHE               // then the directory sector
#define DATA_BUF  (FAT_CACHE + 1)         // and a data sector

HIGH_MEM static unsigned char buffers[(FAT_CACHE + 2) * 512];

// One of the 512-byte buffers. (Through a function: see mem.h.)
__attribute__((noinline, noclone)) static unsigned char *buffer(int i) {
    return buffers + i * 512;
}

static unsigned int part_lba, per_cluster, fat_start, fat_sectors, fats, root_start;
static unsigned int root_entries, data_start, clusters;
static int fat16;

static int cache_sector[FAT_CACHE];       // which FAT sector each holds, or -1
static unsigned char cache_dirty[FAT_CACHE];
static int cache_next;
static unsigned int dir_lba;              // the sector in the directory buffer, or 0
static int dir_dirty;

static char folder[9];
static unsigned int mounts;               // how many times mount() has run

static unsigned int get16(const unsigned char *p) { return p[0] | (p[1] << 8); }
static void put16(unsigned char *p, unsigned int v) { p[0] = v; p[1] = v >> 8; }
static unsigned int get32(const unsigned char *p) { return get16(p) | (get16(p + 2) << 16); }
static void put32(unsigned char *p, unsigned int v) { put16(p, v); put16(p + 2, v >> 16); }

static const char *disk_error = "R Disk error";
static const char *no_fat = "R No FAT partition";
static int failed;                        // a read or write went wrong

void fat_folder(const char *name) {
    int n = 0;
    while (name[n] && n < 8) {
        folder[n] = name[n];
        n++;
    }
    folder[n] = '\0';
}

static int is_fat_type(int t) {
    return t == 0x01 || t == 0x04 || t == 0x06 || t == 0x0E || t == 0xEF;
}

// Find the partition and read its boot sector.
static const char *mount(void) {
    unsigned char *b = buffer(DATA_BUF);
    for (int i = 0; i < FAT_CACHE; i++) cache_sector[i] = -1, cache_dirty[i] = 0;
    dir_lba = 0;
    dir_dirty = 0;
    failed = 0;
    mounts++;
    if (plat_disk_read(0, b) < 0) return disk_error;
    part_lba = 0;
    if (b[510] == 0x55 && b[511] == 0xAA)
        for (int i = 0; i < 4 && !part_lba; i++)
            if (is_fat_type(b[446 + i * 16 + 4])) part_lba = get32(b + 446 + i * 16 + 8);
    if (!part_lba) return no_fat;
    if (plat_disk_read(part_lba, b) < 0) return disk_error;
    per_cluster = b[13];
    unsigned int reserved = get16(b + 14);
    fats = b[16];
    root_entries = get16(b + 17);
    unsigned int total = get16(b + 19) ? get16(b + 19) : get32(b + 32);
    fat_sectors = get16(b + 22);
    unsigned int root_sectors = (root_entries * 32 + 511) / 512;
    if (b[510] != 0x55 || b[511] != 0xAA || get16(b + 11) != 512 || per_cluster == 0 ||
        (per_cluster & (per_cluster - 1)) || reserved < 1 || fats < 1 || fats > 2 ||
        fat_sectors < 1 || root_sectors < 1)
        return no_fat;
    fat_start = part_lba + reserved;
    root_start = fat_start + fats * fat_sectors;
    data_start = root_start + root_sectors;
    unsigned int meta = data_start - part_lba;
    if (total <= meta) return no_fat;
    clusters = (total - meta) / per_cluster;
    fat16 = clusters >= 4085;
    // The FAT must have an entry for every cluster (2 to clusters + 1).
    unsigned int need = fat16 ? (clusters + 2) * 2 : ((clusters + 2) * 3 + 1) / 2;
    if (clusters >= 65525 || need > fat_sectors * 512) return no_fat;
    return 0;
}

/* ------------------------------------------------------------------------
 * The FAT, through a small cache of its sectors
 * --------------------------------------------------------------------- */

static void write_fat_sector(int i) {
    for (unsigned int f = 0; f < fats; f++)
        if (plat_disk_write(fat_start + f * fat_sectors + cache_sector[i], buffer(i)) < 0)
            failed = 1;
    cache_dirty[i] = 0;
}

// The byte at offset off in the FAT, in the cache. dirty: about to change it.
static unsigned char *fat_byte(unsigned int off, int dirty) {
    int s = off / 512, i;
    for (i = 0; i < FAT_CACHE && cache_sector[i] != s; i++) {}
    if (i == FAT_CACHE) {
        i = cache_next;
        cache_next = (cache_next + 1) % FAT_CACHE;
        if (cache_dirty[i]) write_fat_sector(i);
        cache_sector[i] = s;
        if (plat_disk_read(fat_start + s, buffer(i)) < 0) {
            failed = 1;
            memset(buffer(i), 0xFF, 512);     // (looks used: nothing is put there)
        }
    }
    if (dirty) cache_dirty[i] = 1;
    return buffer(i) + off % 512;
}

static unsigned int get_fat(unsigned int c) {
    if (fat16) return *fat_byte(c * 2, 0) | (*fat_byte(c * 2 + 1, 0) << 8);
    unsigned int off = c * 3 / 2;
    unsigned int w = *fat_byte(off, 0) | (*fat_byte(off + 1, 0) << 8);
    return c & 1 ? w >> 4 : w & 0xFFF;
}

static void set_fat(unsigned int c, unsigned int v) {
    if (fat16) {
        *fat_byte(c * 2, 1) = v;
        *fat_byte(c * 2 + 1, 1) = v >> 8;
        return;
    }
    unsigned int off = c * 3 / 2;
    unsigned char *q = fat_byte(off, 1);
    if (c & 1) *q = (*q & 0x0F) | ((v & 0x0F) << 4);
    else *q = v;
    q = fat_byte(off + 1, 1);
    if (c & 1) *q = v >> 4;
    else *q = (*q & 0xF0) | ((v >> 8) & 0x0F);
}

static unsigned int end_mark(void) { return fat16 ? 0xFFFF : 0xFFF; }
static int is_cluster(unsigned int c) { return c >= 2 && c < clusters + 2; }
static unsigned int cluster_lba(unsigned int c) { return data_start + (c - 2) * per_cluster; }

static void free_chain(unsigned int c) {
    for (unsigned int n = 0; is_cluster(c) && n < clusters; n++) {
        unsigned int next = get_fat(c);
        set_fat(c, 0);
        c = next;
    }
}

static unsigned int free_clusters(void) {
    unsigned int n = 0;
    for (unsigned int c = 2; c < clusters + 2; c++) n += get_fat(c) == 0;
    return n;
}

// A free cluster, marked as the end of a chain (0 if there's none).
static unsigned int take_cluster(void) {
    for (unsigned int c = 2; c < clusters + 2; c++)
        if (get_fat(c) == 0) {
            set_fat(c, end_mark());
            return c;
        }
    return 0;
}

static const char *zero_cluster(unsigned int c) {
    unsigned char *b = buffer(DATA_BUF);
    memset(b, 0, 512);
    for (unsigned int s = 0; s < per_cluster; s++)
        if (plat_disk_write(cluster_lba(c) + s, b) < 0) return disk_error;
    return 0;
}

/* ------------------------------------------------------------------------
 * Directories: the root (dir 0), or a chain of clusters (dir = the first)
 * --------------------------------------------------------------------- */

static void write_dir_sector(void) {
    if (dir_dirty && plat_disk_write(dir_lba, buffer(DIR_BUF)) < 0) failed = 1;
    dir_dirty = 0;
}

// Write back everything changed. Returns an error if anything failed.
static const char *flush(void) {
    for (int i = 0; i < FAT_CACHE; i++)
        if (cache_dirty[i]) write_fat_sector(i);
    write_dir_sector();
    return failed ? disk_error : 0;
}

// The sector holding entry i of directory dir, or 0 past its end.
static unsigned int entry_lba(unsigned int dir, unsigned int i) {
    unsigned int s = i / 16;
    if (dir == 0) return i < root_entries ? root_start + s : 0;
    unsigned int c = dir;
    for (unsigned int k = s / per_cluster; k > 0; k--) {
        c = get_fat(c);
        if (!is_cluster(c)) return 0;
    }
    return is_cluster(c) ? cluster_lba(c) + s % per_cluster : 0;
}

// Entry i of directory dir (32 bytes, in the directory buffer, until the
// next call), or 0 past its end. dirty: about to change it.
static unsigned char *entry(unsigned int dir, unsigned int i, int dirty) {
    unsigned int lba = entry_lba(dir, i);
    if (!lba) return 0;
    if (lba != dir_lba) {
        write_dir_sector();
        dir_lba = lba;
        if (plat_disk_read(lba, buffer(DIR_BUF)) < 0) {
            failed = 1;
            dir_lba = 0;
            return 0;
        }
    }
    if (dirty) dir_dirty = 1;
    return buffer(DIR_BUF) + i % 16 * 32;
}

static int is_file(const unsigned char *e) {      // (not a folder, label or long name)
    return e[0] != 0 && e[0] != 0xE5 && !(e[11] & 0x18);
}

static int is_folder(const unsigned char *e) {
    return e[0] != 0 && e[0] != 0xE5 && e[0] != '.' && e[11] != 0x0F && (e[11] & 0x10);
}

// Characters allowed in DOS names, besides letters and digits.
static int name_char(unsigned char c) {
    const char *ok = "!#$%&'()-@^_`{}~";
    for (; *ok; ok++)
        if (c == (unsigned char)*ok) return 1;
    return 0;
}

// The 8.3 name at *s ("game.bas" -> "GAME    BAS"), up to a "/" or the
// end; 0 if it isn't a valid 8.3 name.
static int short_name(const char **s, unsigned char *out) {
    const char *name = *s;
    memset(out, ' ', 11);
    int i = 0, n = 0;
    for (; name[i] && name[i] != '.' && name[i] != '/'; i++) {
        if (n == 8) return 0;
        out[n++] = toupper(name[i]);
    }
    if (n == 0) return 0;
    if (name[i] == '.') {
        i++;
        for (n = 8; name[i] && name[i] != '/'; i++) {
            if (n == 11) return 0;
            out[n++] = toupper(name[i]);
        }
    }
    for (int k = 0; k < 11; k++) {
        unsigned char c = out[k];
        if (c == ' ') continue;
        if (!isalnum(c) && !name_char(c)) return 0;
    }
    *s = name + i;
    return 1;
}

// The entry in dir called sname, of a file (folder 0) or a folder (1); -1 if none.
static int find(unsigned int dir, const unsigned char *sname, int want_folder) {
    unsigned char *e;
    for (unsigned int i = 0; (e = entry(dir, i, 0)) && e[0]; i++)
        if ((want_folder ? is_folder(e) : is_file(e)) && memcmp(e, sname, 11) == 0) return i;
    return -1;
}

// The cluster of directory dir's entry i (a file's first, a folder's).
static unsigned int entry_cluster(unsigned int dir, unsigned int i) {
    unsigned char *e = entry(dir, i, 0);
    return e ? get16(e + 26) : 0;
}

// A free entry in dir, making the folder longer if it's full; -1 if
// there's no room.
static int free_entry(unsigned int dir) {
    unsigned char *e;
    unsigned int i;
    for (i = 0; (e = entry(dir, i, 0)); i++)
        if (e[0] == 0 || e[0] == 0xE5) return i;
    if (dir == 0) return -1;                // (the root has a fixed size)
    unsigned int last = dir;
    while (is_cluster(get_fat(last))) last = get_fat(last);
    unsigned int c = take_cluster();
    if (!c || zero_cluster(c)) return -1;
    set_fat(last, c);
    return i;
}

static void set_entry(unsigned char *e, const unsigned char *sname, int attr, unsigned int cluster,
                      unsigned int size) {
    unsigned int date = (1 << 5) | 1;        // (no clock: 1 January 1980)
    memset(e, 0, 32);
    memcpy(e, sname, 11);
    e[11] = attr;
    put16(e + 16, date);                     // created
    put16(e + 18, date);                     // accessed
    put16(e + 24, date);                     // modified
    put16(e + 26, cluster);
    put32(e + 28, size);
}

// Make a folder called sname in dir; returns its cluster, or 0.
static unsigned int make_folder(unsigned int dir, const unsigned char *sname) {
    int i = free_entry(dir);
    unsigned int c = i < 0 ? 0 : take_cluster();
    if (!c || zero_cluster(c)) return 0;
    unsigned char dots[11];
    memset(dots, ' ', 11);
    dots[0] = '.';
    set_entry(entry(c, 0, 1), dots, 0x10, c, 0);
    dots[1] = '.';
    set_entry(entry(c, 1, 1), dots, 0x10, dir, 0);
    set_entry(entry(dir, i, 1), sname, 0x10, c, 0);
    return c;
}

// Mount the partition and find the folder a name is in, and its last
// part (sname). create: make folders that aren't there.
static const char *open_path(const char *name, unsigned int *dir, unsigned char *sname,
                             int create) {
    const char *err = mount();
    if (err) return err;
    *dir = 0;
    const char *p = name;
    int absolute = 0;
    for (const char *q = name; *q; q++) absolute |= *q == '/';
    if (!absolute && folder[0]) {
        const char *f = folder;
        if (!short_name(&f, sname)) return "F Invalid file name";
        int i = find(0, sname, 1);
        if (i >= 0) *dir = entry_cluster(0, i);
        else if (!create) return "R Program not found";
        else if (!(*dir = make_folder(0, sname))) return failed ? disk_error : "R Disk full";
    }
    for (;;) {
        if (!short_name(&p, sname)) return "F Invalid file name";
        if (*p != '/') break;
        p++;
        int i = find(*dir, sname, 1);
        if (i >= 0) *dir = entry_cluster(*dir, i);
        else if (!create) return "R Program not found";
        else if (!(*dir = make_folder(*dir, sname))) return failed ? disk_error : "R Disk full";
    }
    if (*p) return "F Invalid file name";
    return failed ? disk_error : 0;
}

/* ------------------------------------------------------------------------ */

static const char *read_file(const char *name, unsigned char *buf, int cap, int *len,
                             int partial) {
    unsigned char sname[11];
    unsigned int dir;
    const char *err = open_path(name, &dir, sname, 0);
    if (err) return err;
    int i = find(dir, sname, 0);
    if (i < 0) return failed ? disk_error : "R Program not found";
    unsigned char *e = entry(dir, i, 0);
    unsigned int size = get32(e + 28), c = get16(e + 26), done = 0;
    *len = size;
    if (size > (unsigned int)cap) {
        if (!partial) return "4 Out of memory";
        size = cap;
    }
    unsigned char *sector = buffer(DATA_BUF);
    while (done < size) {
        if (!is_cluster(c)) return "R Bad program file";
        for (unsigned int s = 0; s < per_cluster && done < size; s++) {
            if (plat_disk_read(cluster_lba(c) + s, sector) < 0) return disk_error;
            unsigned int n = size - done < 512 ? size - done : 512;
            memcpy(buf + done, sector, n);
            done += n;
        }
        c = get_fat(c);
    }
    return failed ? disk_error : 0;
}

const char *fat_read(const char *name, unsigned char *buf, int cap, int *len) {
    return read_file(name, buf, cap, len, 0);
}

const char *fat_read_start(const char *name, unsigned char *buf, int n, int *len) {
    return read_file(name, buf, n, len, 1);
}

const char *fat_write(const char *name, const unsigned char *data, int len) {
    unsigned char sname[11];
    unsigned int dir;
    const char *err = open_path(name, &dir, sname, 1);
    if (err) {
        flush();                              // (a folder it made)
        return err;
    }
    int i = find(dir, sname, 0);
    unsigned int old = i >= 0 ? entry_cluster(dir, i) : 0;
    if (i < 0 && (i = free_entry(dir)) < 0) {
        flush();
        return failed ? disk_error : "R Disk full";
    }
    unsigned int bytes = per_cluster * 512;
    unsigned int need = (len + bytes - 1) / bytes;
    if (free_clusters() < need) {             // only room if the old file goes first
        free_chain(old);
        old = 0;
        if (free_clusters() < need) {
            mount();                          // (forget the change: nothing is written)
            return "R Disk full";
        }
    }
    // Take free clusters in order and write the data into them.
    unsigned int first = 0, prev = 0;
    unsigned char *sector = buffer(DATA_BUF);
    for (unsigned int k = 0; k < need; k++) {
        unsigned int c = take_cluster();
        if (prev) set_fat(prev, c);
        else first = c;
        prev = c;
        for (unsigned int s = 0; s < per_cluster; s++) {
            int off = k * bytes + s * 512;
            memset(sector, 0, 512);
            if (off < len) memcpy(sector, data + off, len - off < 512 ? len - off : 512);
            if (plat_disk_write(cluster_lba(c) + s, sector) < 0) return disk_error;
        }
    }
    if (old) free_chain(old);
    unsigned char *e = entry(dir, i, 1);
    if (!e) return disk_error;
    set_entry(e, sname, 0x20, first, len);    // (archive)
    return flush();
}

const char *fat_erase(const char *name) {
    unsigned char sname[11];
    unsigned int dir;
    const char *err = open_path(name, &dir, sname, 0);
    if (err) return err;
    int i = find(dir, sname, 0);
    if (i < 0) return failed ? disk_error : "R Program not found";
    free_chain(entry_cluster(dir, i));
    entry(dir, i, 1)[0] = 0xE5;
    unsigned char *e;
    for (int k = i - 1; k >= 0 && (e = entry(dir, k, 0)) && e[11] == 0x0F && e[0] != 0xE5; k--)
        entry(dir, k, 1)[0] = 0xE5;           // its long name, if it has one
    return flush();
}

const char *fat_list(void (*each)(const char *name, int len)) {
    unsigned char sname[11];
    unsigned int dir;
    const char *err = open_path("X", &dir, sname, 0);   // (just the folder)
    if (err) return err;
    unsigned char *e;
    for (unsigned int i = 0; (e = entry(dir, i, 0)) && e[0]; i++) {
        if (!is_file(e)) continue;
        char name[13];
        int n = 0, size = get32(e + 28);
        for (int k = 0; k < 8 && e[k] != ' '; k++) name[n++] = e[k];
        if (e[8] != ' ') {
            name[n++] = '.';
            for (int k = 8; k < 11 && e[k] != ' '; k++) name[n++] = e[k];
        }
        name[n] = '\0';
        unsigned int before = mounts;
        each(name, size);
        // (each() may have read other files: look at this folder afresh.)
        if (mounts != before && (err = open_path("X", &dir, sname, 0))) return err;
    }
    return failed ? disk_error : 0;
}

const char *fat_free_kb(int *kb) {
    const char *err = mount();
    if (err) return err;
    *kb = free_clusters() * per_cluster / 2;
    return failed ? disk_error : 0;
}
