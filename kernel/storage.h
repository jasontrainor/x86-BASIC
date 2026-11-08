#ifndef STORAGE_H
#define STORAGE_H

/*
 * Where saved programs live on the disk image (in 512-byte sectors).
 * boot/boot.asm and tools/mkimage.py use the same layout.
 */
#define STORE_LBA           128   // directory; slots follow
#define STORE_SLOTS         15
#define STORE_SLOT_SECTORS  64    // 32KB: the whole program store
#define STORE_NAME_LEN      15

// A saved file is a program (param: the line to RUN after loading, or
// 0) or bytes (param: the address they were saved from).
enum { FILE_PROGRAM, FILE_BYTES };

// Each returns 0 on success or an error message. A load with name ""
// takes the first file of that type.
const char *storage_save(const char *name, const unsigned char *data, int len, int param,
                         int type);
const char *storage_load(const char *name, int type, unsigned char *data, int cap, int *len,
                         int *param);
const char *storage_erase(const char *name);
const char *storage_list(void (*each)(const char *name, int len, int param, int type),
                         int *free_slots);

#endif
