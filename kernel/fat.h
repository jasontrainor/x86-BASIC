#ifndef FAT_H
#define FAT_H

/*
 * Text files in the root directory of the disk's FAT partition (the EFI
 * system partition, which any PC can open), for listings: LOAD
 * "GAME.BAS", SAVE "GAME.BAS". Names are DOS 8.3 names, in any case.
 * Each returns 0 on success or an error message.
 */
const char *fat_read(const char *name, unsigned char *buf, int cap, int *len);
const char *fat_write(const char *name, const unsigned char *data, int len);
const char *fat_erase(const char *name);
// Calls each() for every file; returns an error if there's no FAT partition.
const char *fat_list(void (*each)(const char *name, int len));

#endif
