#ifndef STORAGE_H
#define STORAGE_H

/*
 * Saved files (SAVE, LOAD, CAT, ERASE ...): each a file in the machine's
 * folder on the FAT partition (core/storage.c), with a header saying
 * what it is.
 */
#define STORE_NAME_LEN 15

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
// Calls each() for every saved file; *free_kb says what room is left.
const char *storage_list(void (*each)(const char *name, int len, int param, int type),
                         int *free_kb);
// Is this the name of one of storage's files on the disk (NAME.SAV)?
int storage_file(const char *disk_name);

#endif
