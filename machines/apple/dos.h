#ifndef DOS_H
#define DOS_H

/*
 * Disk commands in the style of the Apple II's DOS 3.3 (dos.c), for both
 * Apple BASICs: typed at the prompt (CATALOG, SAVE NAME ...) or, in a
 * program, printed after a Ctrl+D (PRINT CHR$(4);"CATALOG"). Files are
 * files in the machine's folder on the FAT partition: programs and
 * memory saved here are saved files (core/storage.h), kept as their
 * DOS names say; a listing copied there from a PC (GAME.BAS) can be
 * LOADed or RUN by its DOS 8.3 name.
 *
 * Only how the commands behave is copied; there's no DOS code here.
 */

// The BASIC using the commands.
struct dos_basic {
    char type;                                // its programs: 'I' Integer, 'A' FP
    // The program as text (as LIST shows it); its length, or -1 if it
    // doesn't fit.
    int (*listing)(char *buf, int cap);
    // NEW, then enter the lines of a listing; 0, or a DOS error.
    int (*enter)(const char *text, int len);
};

// DOS's errors, by the numbers programs see (FP BASIC's ONERR).
enum {
    DOS_OK = 0,
    DOS_LANGUAGE = 1, DOS_RANGE = 2, DOS_WRITE_PROTECTED = 4, DOS_END_OF_DATA = 5,
    DOS_NOT_FOUND = 6, DOS_IO = 8, DOS_DISK_FULL = 9, DOS_LOCKED = 10, DOS_SYNTAX = 11,
    DOS_NO_BUFFERS = 12, DOS_TYPE_MISMATCH = 13, DOS_TOO_LARGE = 14, DOS_NOT_DIRECT = 15,
    DOS_RUN = 100,                            // (not an error: RUN the program now)
    DOS_NONE = -1,                            // (not a DOS command)
};

void dos_init(const struct dos_basic *basic, const char *folder);

// Do a command line (without the Ctrl+D). direct: typed at the prompt.
// Returns DOS_NONE if it isn't a DOS command, else DOS_OK, DOS_RUN or
// an error number (not printed: dos_message() says what to print).
int dos_command(const char *line, int direct);
const char *dos_message(int error);

#endif
