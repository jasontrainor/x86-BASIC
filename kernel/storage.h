#ifndef STORAGE_H
#define STORAGE_H

/*
 * Where saved files live on the disk image (in 512-byte sectors), after
 * the BIOS kernel and before the EFI system partition. boot/boot.asm and
 * tools/mkimage.py use the same layout.
 */
#define STORE_LBA           256   // the directory; file data follows
#define STORE_DIR_SECTORS   8
#define STORE_END           2048  // the EFI system partition starts here
#define STORE_FILES         127
#define STORE_NAME_LEN      15

// A saved file is a program (param: the line to RUN after loading, or
// 0), bytes (param: the address they were saved from), an array (SAVE
// ... DATA; param: its name) or text (a listing).
enum { FILE_PROGRAM, FILE_BYTES, FILE_NUMBERS, FILE_CHARS, FILE_TEXT };

// Each returns 0 on success or an error message. A load with name ""
// takes the first file of that type.
const char *storage_save(const char *name, const unsigned char *data, int len, int param,
                         int type);
const char *storage_load(const char *name, int type, unsigned char *data, int cap, int *len,
                         int *param);
const char *storage_erase(const char *name);
// Calls each() for every file; *free_files and *free_kb say what room is left.
const char *storage_list(void (*each)(const char *name, int len, int param, int type),
                         int *free_files, int *free_kb);

#endif
