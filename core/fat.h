#ifndef FAT_H
#define FAT_H

/*
 * Files on the disk's FAT partition (FAT12 or FAT16), which any PC can
 * open: listings, saved programs, the printer's output, and the kernels
 * themselves. Names are DOS 8.3 names, in any case.
 *
 * Each machine keeps its files in a folder of its own (fat_folder()); a
 * name with a "/" in it ("X86BASIC/SPECTRUM.SYS") is from the top of the
 * disk instead. A folder is created when it's first written to.
 *
 * Each returns 0 on success or an error message.
 */
void fat_folder(const char *name);         // "SPECTRUM"; "" for the top
const char *fat_read(const char *name, unsigned char *buf, int cap, int *len);
// The first n bytes of a file (fewer if it's shorter), and its length.
const char *fat_read_start(const char *name, unsigned char *buf, int n, int *len);
// The file's bytes after the first skip; *len is how many.
const char *fat_read_from(const char *name, int skip, unsigned char *buf, int cap, int *len);
const char *fat_write(const char *name, const unsigned char *data, int len);
// Write a file made of head_len bytes of head, then the data.
const char *fat_write_header(const char *name, const unsigned char *head, int head_len,
                             const unsigned char *data, int data_len);
const char *fat_erase(const char *name);
// Calls each() for every file in the folder, in the order they are on
// the disk; returns an error if there's no FAT partition.
const char *fat_list(void (*each)(const char *name, int len));
// The room left, in KB.
const char *fat_free_kb(int *kb);

#endif
