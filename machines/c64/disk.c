/*
 * The disk drive (devices 8-11) and the other devices BASIC V2 talks to:
 * written for this project from how they behave; none of it comes from
 * the original's code.
 *
 * The disk (and the tape, device 1) is the machine's folder on the FAT
 * partition, C64. A program is saved as NAME.PRG: its load address, then
 * its bytes as the machine keeps them (tokens and all), the format
 * emulators and tools use; names are made DOS names (A-Z, 0-9, - and _,
 * eight at most: the first six and the last two of a longer one). LOAD
 * also reads .P00 files, listings as text (.BAS, .TXT: see listing.c),
 * and any file by its DOS name (LOAD "GAME.BAS",8).
 *
 *   LOAD "NAME",8      a program, as BASIC's (from 2049, whatever its
 *                      address); in a program, the new one runs from its
 *                      start, keeping the variables
 *   LOAD "NAME",8,1    bytes into memory at their own address (a program,
 *                      if that's 2049); in a program, it goes on from its
 *                      first line again, as there
 *   LOAD "$",8         the directory, as a program to LIST
 *   LOAD "*",8 / "G*"  the first file / the first whose name matches
 *   SAVE "NAME",8      SAVE "@0:NAME",8 too (it replaces either way)
 *   VERIFY "NAME",8    the program (or memory) compared with the file
 *
 * Files, by logical file number: OPEN lfn,device,channel,"name,type,mode"
 * and CLOSE lfn; PRINT#, INPUT# and GET# lfn; CMD lfn to send PRINT
 * there until PRINT# or CLOSE. Sequential files are NAME.SEQ (type S,
 * mode R, W or A). Channel 15 is the drive's commands (S: scratch, R:
 * rename, I, V, UJ) and its error channel: INPUT#15,E,M$,T,S reads the
 * last error ("62,FILE NOT FOUND,00,00"). ST (144) is 64 after a file's
 * last byte is read, 66 when there's no more.
 *
 * Device 0 is the keyboard (INPUT# from it has no prompt), 3 the screen,
 * 4 the printer: what's printed to it goes into PRINTOUT.TXT.
 *
 * As there, opening a file that isn't there gives no error in BASIC:
 * the drive's error channel says so, and reading it gives nothing. CLR,
 * RUN and entering a line close the files (those being written are
 * kept).
 */
#include "c64basic.h"
#include "c64listing.h"
#include "c64.h"
#include "fat.h"
#include "string.h"
#include "mem.h"

#define ip ms_ip
#define FOLDER "C64"

enum {
    E_TOO_MANY = 300, E_FILE_OPEN, E_NOT_OPEN, E_NOT_FOUND, E_NO_DEVICE, E_NOT_INPUT,
    E_NOT_OUTPUT, E_NO_NAME, E_BAD_DEVICE, E_VERIFY, E_FILE_DATA,
};

/* ------------------------------------------------------------------------
 * Buffers: one for LOAD and SAVE (up to 64KB and the address), and one
 * for each of the drive's data channels.
 * --------------------------------------------------------------------- */

#define IO_SIZE 65540
#define CHANNELS 4
#define CHAN_SIZE 16384

HIGH_MEM static unsigned char io_mem[IO_SIZE];
HIGH_MEM static unsigned char chan_mem[CHANNELS][CHAN_SIZE];
HIGH_MEM static char line_mem[260];
__attribute__((noinline, noclone)) static unsigned char *io(void) { return io_mem; }
__attribute__((noinline, noclone)) static unsigned char *chan_buf(int c) { return chan_mem[c]; }
__attribute__((noinline, noclone)) static char *line_buf(void) { return line_mem; }

struct channel {
    char used, write, printer;
    char dos[13];                         // its DOS name
    int len, pos;
};
static struct channel chans[CHANNELS];

struct lfile {
    unsigned char lfn, dev, sa;
    signed char chan;                     // a data channel, or -1
    int column;                           // (for PRINT#'s commas)
};
#define MAX_FILES 10
static struct lfile files[MAX_FILES];
static int n_files;
static int cmd_lfn, out_lfn, in_lfn;      // CMD's file; PRINT#'s, INPUT#'s just now

static int drive_err = 73;                // the error channel: the last error,
static int drive_count;                   // and its number (FILES SCRATCHED)
static char cmd_line[48];                 // a command being sent to channel 15
static int cmd_len;

static void set_st(int v) { c64_poke(C64_STATUS, v); }

/* ------------------------------------------------------------------------
 * Names
 * --------------------------------------------------------------------- */

static int is_dos_char(int c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
}

static int upper(int c) {
    c &= 0xFF;
    if (c >= 'a' && c <= 'z') return c - 32;
    if (c >= 193 && c <= 218) return c - 128;   // (shifted letters: capitals too)
    return c;
}

// The DOS base name a name is kept under (into out: 9 bytes).
static void dos_base(const char *name, char *out) {
    char clean[17];
    int n = 0;
    for (; *name && n < 16; name++) {
        int c = upper(*name);
        if (is_dos_char(c)) clean[n++] = c;
    }
    if (n == 0) {
        strcpy(out, "NONAME");
        return;
    }
    if (n > 8) {                          // the first six and the last two
        clean[6] = clean[n - 2];
        clean[7] = clean[n - 1];
        n = 8;
    }
    memcpy(out, clean, n);
    out[n] = '\0';
}

// A name given as a DOS name (GAME.BAS): itself, in capitals; or 0.
static int dos_given(const char *name, char *out) {
    const char *dot = 0;
    int n = 0;
    for (const char *p = name; *p; p++, n++)
        if (*p == '.') dot = p;
    if (!dot || dot == name || n - (dot - name) - 1 < 1 || n - (dot - name) - 1 > 3 ||
        dot - name > 8)
        return 0;
    int i = 0;
    for (const char *p = name; *p; p++) out[i++] = upper(*p);
    out[i] = '\0';
    return 1;
}

static int matches(const char *pattern, const char *base) {
    for (;; pattern++, base++) {
        if (*pattern == '*') return 1;
        if (!*pattern || !*base) return !*pattern && !*base;
        if (*pattern != '?' && upper(*pattern) != *base) return 0;
    }
}

// Finding a file: the first whose base name matches the pattern and
// whose extension is one of exts ("PRG P00 BAS"), into found.
static const char *find_pattern, *find_exts;
static char found[13];

static void find_each(const char *name, int len) {
    (void)len;
    if (found[0]) return;
    char base[13];
    int i = 0;
    while (name[i] && name[i] != '.' && i < 12) base[i] = name[i], i++;
    base[i] = '\0';
    const char *ext = name[i] == '.' ? name + i + 1 : "";
    for (const char *e = find_exts; *e; e += 4) {
        if (strncmp(e, ext, 3) || strlen(ext) != 3) continue;
        if (matches(find_pattern, base)) strcpy(found, name);
        return;
    }
}

// The DOS name of the file a name means (with one of exts), into out;
// 0 if there's none.
static int find_file(const char *name, const char *exts, char *out) {
    fat_folder(FOLDER);
    if (dos_given(name, out)) {
        int len;
        unsigned char b;
        return !fat_read_start(out, &b, 1, &len);
    }
    int wild = 0;
    for (const char *p = name; *p; p++)
        if (*p == '*' || *p == '?') wild = 1;
    char base[9];
    if (!wild) dos_base(name, base);
    find_pattern = wild ? name : base;
    find_exts = exts;
    found[0] = '\0';
    fat_folder(FOLDER);
    if (fat_list(find_each) || !found[0]) return 0;
    strcpy(out, found);
    return 1;
}

static int has_ext(const char *dos, const char *ext) {
    const char *dot = dos;
    while (*dot && *dot != '.') dot++;
    return *dot && !strcmp(dot + 1, ext);
}

/* ------------------------------------------------------------------------
 * Programs as the machine keeps them (PRG: linked lines from $0801)
 * --------------------------------------------------------------------- */

// The program into io() as a PRG; its length.
static int program_bytes(void) {
    unsigned char *b = io();
    int n = 2, addr = 0x0801;
    b[0] = 0x01;
    b[1] = 0x08;
    for (int off = 0; off < ms_prog_len; off = ms_line_next(off)) {
        const unsigned char *t = ms_prog + off + 4;
        int start = n, quote = 0, raw = 0;
        if (n + 4 + 260 > IO_SIZE) break;
        n += 2;
        b[n++] = ms_line_num(off) & 0xFF;
        b[n++] = ms_line_num(off) >> 8;
        for (; *t; t++) {
            int c = *t;
            if (c == '"') quote = !quote;
            else if (!quote && raw != 1) {
                if (raw == 2 && c == ':') raw = 0;
                else if (!raw && (c >= 0x80 || c == '+' || c == '-' || c == '*' || c == '/' ||
                                  c == '^' || c == '>' || c == '=' || c == '<')) {
                    if (c == T_REM) raw = 1;
                    if (c == T_DATA) raw = 2;
                    c = c64_original_token(c);
                }
            }
            b[n++] = c;
        }
        b[n++] = 0;
        addr += n - start;
        b[start] = addr & 0xFF;
        b[start + 1] = addr >> 8;
    }
    b[n++] = 0;
    b[n++] = 0;
    return n;
}

// A PRG's lines (data, len: after its address) as the program.
static void enter_program(const unsigned char *data, int len) {
    ms_prog_len = 0;
    unsigned char text[256];
    int i = 0;
    while (i + 4 <= len && (data[i] | data[i + 1])) {
        int num = data[i + 2] | data[i + 3] << 8, n = 0, quote = 0, raw = 0;
        i += 4;
        while (i < len && data[i] && n < 255) {
            int c = data[i++];
            if (c == '"') quote = !quote;
            else if (!quote && raw != 1) {
                if (raw == 2 && c == ':') raw = 0;
                else if (!raw && c >= 0x80) {
                    c = c64_token(c);
                    if (c == T_REM) raw = 1;
                    if (c == T_DATA) raw = 2;
                }
            }
            text[n++] = c;
        }
        while (i < len && data[i]) i++;   // (a line too long: the rest left out)
        i++;
        text[n++] = 0;
        if (num > 63999 || !ms_store_line(num, text, n)) break;
    }
}

// A listing as text (a .BAS or .TXT) as the program.
static void enter_listing(const char *text, int len) {
    int lower = c64_listing_lower(text, len);
    ms_prog_len = 0;
    char *line = line_buf();
    int i = 0;
    while (i < len) {
        int start = i;
        while (i < len && text[i] != '\n' && text[i] != '\r') i++;
        int n = c64_listing_line(text + start, i - start, line, 256, lower);
        while (i < len && (text[i] == '\n' || text[i] == '\r')) i++;
        int k = 0;
        while (k < n && line[k] == ' ') k++;
        if (k < n && line[k] >= '0' && line[k] <= '9') ms_enter_line(line);
    }
}


/* ------------------------------------------------------------------------
 * The directory, as a program: a line for the disk's name, one for each
 * file (numbered with its size in blocks of 254 bytes, in the order the
 * files are on the disk), and BLOCKS FREE.
 * --------------------------------------------------------------------- */

static int dir_at;                        // (where the next line goes in the program)

static void dir_add(int num, const char *text) {
    int len = strlen(text) + 1;
    if (num > 63999) num = 63999;
    if (dir_at + 4 + len > MS_PROG_SIZE) return;
    unsigned char *p = ms_prog + dir_at;
    p[0] = num & 0xFF;
    p[1] = num >> 8;
    p[2] = len & 0xFF;
    p[3] = len >> 8;
    memcpy(p + 4, text, len);
    dir_at += 4 + len;
}

static void dir_each(const char *name, int len) {
    char text[48], *p = text;
    int blocks = (len + 253) / 254;
    int prg = has_ext(name, "PRG"), seq = has_ext(name, "SEQ"), n = 0;
    for (int pad = blocks < 10 ? 3 : blocks < 100 ? 2 : 1; pad > 0; pad--) *p++ = ' ';
    *p++ = '"';
    for (; name[n] && !((prg || seq) && name[n] == '.'); n++) *p++ = name[n];
    *p++ = '"';
    for (; n < 17; n++) *p++ = ' ';
    strcpy(p, seq ? "SEQ" : "PRG");
    dir_add(blocks, text);
}

static void load_directory(void) {
    dir_at = 0;
    dir_add(0, "\x12\"X86 BASIC       \" 64 2A");
    fat_folder(FOLDER);
    fat_list(dir_each);
    int kb = 0;
    long free_blocks = fat_free_kb(&kb) ? 0 : kb * 1024L / 254;
    dir_add(free_blocks > 63999 ? 63999 : (int)free_blocks, "BLOCKS FREE.");
    ms_prog_len = dir_at;
}

/* ------------------------------------------------------------------------
 * LOAD, SAVE and VERIFY
 * --------------------------------------------------------------------- */

// The arguments: a name, a device and a secondary address (each may be
// left out). The name into name (17 bytes). 0 if there's an error.
static int file_args(char *name, int *dev, int *sa) {
    name[0] = '\0';
    *dev = 1;
    *sa = 0;
    if (ms_at_end()) return 1;
    struct ms_val v;
    ms_str_arg(&v);
    if (ms_err != E_NONE) return 0;
    int n = v.len < 16 ? v.len : 16;
    memcpy(name, v.s, n);
    name[n] = '\0';
    if (ms_cur() == ',') {
        ip++;
        *dev = ms_quantity(255);
        if (ms_cur() == ',') {
            ip++;
            *sa = ms_quantity(255);
        }
    }
    return ms_err == E_NONE;
}

// Is device dev one that holds files (the tape or a disk)? An error if
// it isn't.
static int file_device(int dev) {
    if (dev == 1 || (dev >= 8 && dev <= 11)) return 1;
    ms_error(dev == 0 || dev == 3 ? E_BAD_DEVICE : E_NO_DEVICE);
    return 0;
}

// The name without a drive ("0:") or the replace sign ("@0:").
static const char *plain_name(const char *name) {
    if (*name == '@') name++;
    if ((name[0] >= '0' && name[0] <= '9' && name[1] == ':')) name += 2;
    else if (name[0] == ':') name++;
    return name;
}

static void message(const char *what, const char *name) {
    if (ms_cur_line >= 0) return;         // (only in direct mode)
    ms_out(13);
    for (; *what; what++) ms_out(*what);
    for (; name && *name; name++) ms_out(*name);
}

static void load(int verify) {
    char name[17], dos[13];
    int dev, sa, len;
    if (!file_args(name, &dev, &sa) || !file_device(dev)) return;
    const char *plain = plain_name(name);
    if (dev != 1 && !*plain) {
        ms_error(E_NO_NAME);
        return;
    }
    message("SEARCHING FOR ", plain);
    if (!strcmp(plain, "$")) {
        if (verify) return;
        message("LOADING", 0);
        load_directory();
        ms_clear_vars();
        set_st(0);
        ms_ended = 1;
        return;
    }
    if (!find_file(*plain ? plain : "*", "PRG\0P00\0BAS\0TXT\0", dos) ||
        fat_read(dos, io(), IO_SIZE, &len)) {
        ms_error(E_NOT_FOUND);
        return;
    }
    message(verify ? "VERIFYING" : "LOADING", 0);
    set_st(0);
    unsigned char *b = io();
    int in_program = ms_cur_line >= 0;
    if (has_ext(dos, "BAS") || has_ext(dos, "TXT")) {   // a listing
        if (verify) return;
        enter_listing((const char *)b, len);
    } else {
        int skip = has_ext(dos, "P00") ? 26 : 0;
        if (len < skip + 2) {
            ms_error(E_NOT_FOUND);
            return;
        }
        unsigned int addr = b[skip] | b[skip + 1] << 8;
        const unsigned char *data = b + skip + 2;
        int n = len - skip - 2;
        if (sa == 0 || addr == 0x0801) {  // a program
            if (verify) {                 // (compared as the program would be saved)
                int saved = program_bytes(), again;
                unsigned char *b2 = io() + saved;
                if (fat_read_from(dos, skip + 2, b2, IO_SIZE - saved, &again) ||
                    again != saved - 2 || memcmp(io() + 2, b2, again))
                    ms_error(E_VERIFY);
                return;
            }
            enter_program(data, n);
        } else {                          // bytes into memory
            for (int i = 0; i < n; i++) {
                if (verify) {
                    if (c64_peek(addr + i) != data[i]) {
                        ms_error(E_VERIFY);
                        return;
                    }
                } else {
                    c64_poke(addr + i, data[i]);
                }
            }
            if (verify) return;
            c64_poke(174, (addr + n) & 0xFF);   // (where the load ended)
            c64_poke(175, (addr + n) >> 8 & 0xFF);
            if (in_program) ms_chain();   // (and on from the start: as there)
            return;
        }
    }
    if (in_program) {
        ms_chain();                       // the new program runs, with the variables
    } else {
        ms_clear_vars();
        ms_ended = 1;                     // (the rest of the line isn't run, as there)
    }
}

static void verify(void) {
    load(1);
    if (ms_err == E_NONE) message("OK", 0);
}

static void save(void) {
    char name[17], dos[13];
    int dev, sa;
    if (!file_args(name, &dev, &sa) || !file_device(dev)) return;
    const char *plain = plain_name(name);
    if (!*plain) {
        if (dev != 1) {
            ms_error(E_NO_NAME);
            return;
        }
        plain = "NONAME";
    }
    message("SAVING ", plain);
    if (!dos_given(plain, dos)) {
        dos_base(plain, dos);
        strcpy(dos + strlen(dos), ".PRG");
    }
    int n = program_bytes();
    fat_folder(FOLDER);
    const char *e = fat_write(dos, io(), n);
    drive_err = e ? 72 : 0;               // (DISK FULL: BASIC says nothing, as there)
    set_st(0);
}

/* ------------------------------------------------------------------------
 * Files
 * --------------------------------------------------------------------- */

static struct lfile *find_lfn(int lfn) {
    for (int i = 0; i < n_files; i++)
        if (files[i].lfn == lfn) return &files[i];
    return 0;
}

static const char *const drive_messages[] = {
    "00, OK", "01, FILES SCRATCHED", "30,SYNTAX ERROR", "31,SYNTAX ERROR",
    "60,WRITE FILE OPEN", "61,FILE NOT OPEN", "62,FILE NOT FOUND", "63,FILE EXISTS",
    "70,NO CHANNEL", "72,DISK FULL", "73,X86 DOS V2.6 1541",
};

// The error channel's line: "62,FILE NOT FOUND,00,00".
static int status_line(char *out) {
    const char *m = drive_messages[0];
    for (unsigned i = 0; i < sizeof drive_messages / sizeof drive_messages[0]; i++)
        if ((drive_messages[i][0] - '0') * 10 + drive_messages[i][1] - '0' == drive_err)
            m = drive_messages[i];
    int n = 0;
    while (*m) out[n++] = *m++;
    out[n++] = ',';
    out[n++] = '0' + drive_count / 10 % 10;
    out[n++] = '0' + drive_count % 10;
    out[n++] = ',';
    out[n++] = '0';
    out[n++] = '0';
    out[n] = '\0';
    return n;
}

static int scratched;

static void scratch_each(const char *name, int len) {
    (void)len;
    char base[13];
    int i = 0;
    while (name[i] && name[i] != '.' && i < 12) base[i] = name[i], i++;
    base[i] = '\0';
    if (matches(find_pattern, base) && !fat_erase(name)) scratched++;
}

// A command sent to channel 15.
static void drive_command(const char *c) {
    drive_count = 0;
    drive_err = 0;
    fat_folder(FOLDER);
    char a = upper(c[0]);
    const char *arg = c;
    while (*arg && *arg != ':') arg++;
    if (*arg) arg++;
    if (a == 'S') {                       // S0:NAME[,NAME ...]: scratch
        char pat[17];
        while (*arg) {
            int n = 0;
            while (*arg && *arg != ',' && n < 16) pat[n++] = *arg++;
            pat[n] = '\0';
            if (*arg == ',') arg++;
            char base[9];
            int wild = 0;
            for (int i = 0; pat[i]; i++) wild |= pat[i] == '*' || pat[i] == '?';
            if (!wild) dos_base(pat, base);
            find_pattern = wild ? pat : base;
            do {
                scratched = 0;
                fat_list(scratch_each);   // (erasing changes the list: again till none)
                drive_count += scratched;
            } while (scratched);
        }
        drive_err = 1;
    } else if (a == 'R') {                // R0:NEW=OLD: rename
        char new_name[17], old_name[17], from[13], to[13];
        int n = 0, len;
        while (*arg && *arg != '=' && n < 16) new_name[n++] = *arg++;
        new_name[n] = '\0';
        if (*arg++ != '=') {
            drive_err = 30;
            return;
        }
        n = 0;
        while (*arg && n < 16) old_name[n++] = *arg++;
        old_name[n] = '\0';
        if (!find_file(plain_name(old_name), "PRG\0SEQ\0USR\0P00\0", from)) {
            drive_err = 62;
            return;
        }
        dos_base(plain_name(new_name), to);
        const char *dot = from;
        while (*dot && *dot != '.') dot++;
        strcpy(to + strlen(to), dot);
        if (find_file(plain_name(new_name), dot + 1, to) && strcmp(to, from)) {
            drive_err = 63;
            return;
        }
        dos_base(plain_name(new_name), to);
        strcpy(to + strlen(to), dot);
        if (fat_read(from, io(), IO_SIZE, &len) || fat_write(to, io(), len)) drive_err = 72;
        else if (strcmp(to, from)) fat_erase(from);
    } else if (a == 'U' && (upper(c[1]) == 'J' || upper(c[1]) == 'I' || c[1] == ':')) {
        drive_err = 73;
    } else if (a != 'I' && a != 'V' && a) {
        drive_err = 31;
    }
}

static int free_channel(void) {
    for (int c = 0; c < CHANNELS; c++)
        if (!chans[c].used) return c;
    return -1;
}

// OPEN a file on a disk (or the tape): name ("NAME,S,W"), channel sa.
static int open_data(const char *spec, int sa) {
    char name[17], type = 0, mode = 0;
    int n = 0;
    const char *p = plain_name(spec);
    while (*p && *p != ',' && n < 16) name[n++] = *p++;
    name[n] = '\0';
    for (int f = 0; *p == ','; f++) {
        p++;
        char c = upper(*p);
        if (f == 0) type = c;
        else mode = c;
        while (*p && *p != ',') p++;
    }
    if (sa == 0) mode = 'R', type = type ? type : 'P';   // (as LOAD reads)
    if (sa == 1) mode = 'W', type = 'P';                 // (as SAVE writes)
    if (!mode) mode = type == 'W' || type == 'A' ? type : 'R';
    if (type == 'W' || type == 'A' || type == 'R') type = 0;
    const char *ext = type == 'P' ? "PRG" : type == 'U' ? "USR" : "SEQ";
    int c = free_channel();
    if (c < 0) {
        drive_err = 70;
        return -1;
    }
    struct channel *ch = &chans[c];
    memset(ch, 0, sizeof *ch);
    ch->used = 1;
    ch->write = mode != 'R';
    fat_folder(FOLDER);
    if (mode == 'R') {
        int len;
        if (!find_file(name, type ? (type == 'P' ? "PRG\0P00\0" : type == 'U' ? "USR\0" : "SEQ\0")
                                  : "SEQ\0PRG\0USR\0TXT\0", ch->dos) ||
            fat_read(ch->dos, chan_buf(c), CHAN_SIZE, &len)) {
            drive_err = 62;
            ch->len = 0;
            return c;                     // (open, with nothing to read)
        }
        ch->len = len;
        if (has_ext(ch->dos, "TXT")) {    // (a PC's text: its lines end in Return)
            unsigned char *b = chan_buf(c);
            int k = 0;
            for (int i = 0; i < len; i++)
                if (b[i] == '\n') b[k++] = 13;
                else if (b[i] != '\r') b[k++] = b[i];
            ch->len = k;
        }
    } else {
        if (!dos_given(name, ch->dos)) {
            dos_base(name, ch->dos);
            strcpy(ch->dos + strlen(ch->dos), ".");
            strcpy(ch->dos + strlen(ch->dos), ext);
        }
        if (mode == 'A') {
            int len;
            if (fat_read(ch->dos, chan_buf(c), CHAN_SIZE, &len)) {
                drive_err = 62;
                ch->used = 0;
                return -1;
            }
            ch->len = len;
        }
    }
    drive_err = 0;
    return c;
}

static void close_channel(int c) {
    struct channel *ch = &chans[c];
    if (!ch->used) return;
    if (ch->write) {
        fat_folder(FOLDER);
        if (ch->printer) {                // (added to what's printed before)
            int old = 0;
            unsigned char *b = io();
            if (fat_read("PRINTOUT.TXT", b, IO_SIZE - CHAN_SIZE, &old)) old = 0;
            memcpy(b + old, chan_buf(c), ch->len);
            fat_write("PRINTOUT.TXT", b, old + ch->len);
        } else if (fat_write(ch->dos, chan_buf(c), ch->len)) {
            drive_err = 72;
        }
    }
    ch->used = 0;
}

static void close_lfn(int lfn) {
    struct lfile *f = find_lfn(lfn);
    if (!f) return;
    if (f->chan >= 0) close_channel(f->chan);
    if (f->dev >= 8 && f->sa == 15) {     // (closing the command channel closes all)
        for (int c = 0; c < CHANNELS; c++) close_channel(c);
        cmd_len = 0;
    }
    if (cmd_lfn == lfn) cmd_lfn = 0;
    *f = files[--n_files];
}

void disk_close_all(void) {
    while (n_files) close_lfn(files[0].lfn);
    cmd_lfn = out_lfn = in_lfn = 0;
}

void disk_screen(void) { cmd_lfn = out_lfn = in_lfn = 0; }

static void open_statement(void) {
    int lfn = ms_quantity(255), dev = 1, sa = 0;
    char name[41];
    name[0] = '\0';
    if (ms_cur() == ',') {
        ip++;
        dev = ms_quantity(255);
        if (ms_cur() == ',') {
            ip++;
            sa = ms_quantity(255);
            if (ms_cur() == ',') {
                ip++;
                struct ms_val v;
                ms_str_arg(&v);
                int n = v.len < 40 ? v.len : 40;
                if (ms_err == E_NONE) memcpy(name, v.s, n);
                name[n] = '\0';
            }
        }
    }
    if (ms_err != E_NONE) return;
    if (lfn == 0) {
        ms_error(E_QUANTITY);
        return;
    }
    if (find_lfn(lfn)) {
        ms_error(E_FILE_OPEN);
        return;
    }
    if (n_files == MAX_FILES) {
        ms_error(E_TOO_MANY);
        return;
    }
    if (dev == 2 || (dev >= 5 && dev <= 7) || dev > 11) {
        ms_error(E_NO_DEVICE);
        return;
    }
    struct lfile *f = &files[n_files];
    f->lfn = lfn;
    f->dev = dev;
    f->sa = sa;
    f->chan = -1;
    f->column = 0;
    if (dev == 4) {                       // the printer
        int c = free_channel();
        if (c < 0) {
            ms_error(E_TOO_MANY);
            return;
        }
        memset(&chans[c], 0, sizeof chans[c]);
        chans[c].used = chans[c].write = chans[c].printer = 1;
        f->chan = c;
    } else if ((dev == 1 || dev >= 8) && !(dev >= 8 && sa == 15)) {
        if (dev == 1 && !name[0]) {
            if (sa) strcpy(name, "NONAME");
            else strcpy(name, "*");
        }
        if (dev == 1) {                   // the tape: a data file, SEQ
            const char *mode = sa ? ",S,W" : ",S,R";
            strcpy(name + strlen(name), mode);
            f->chan = open_data(name, 2);
            if (f->chan < 0 || (!sa && !chans[f->chan].len && drive_err == 62)) {
                if (f->chan >= 0) chans[f->chan].used = 0;
                ms_error(E_NOT_FOUND);
                return;
            }
        } else {
            f->chan = open_data(name, sa);
        }
    } else if (dev >= 8 && sa == 15 && name[0]) {
        drive_command(name);
    }
    n_files++;
}

static void close_statement(void) {
    int lfn = ms_quantity(255);
    if (ms_err == E_NONE) close_lfn(lfn);
}

// The file after PRINT#, INPUT# ... (ip at its number): checked for the
// direction, the "," after it skipped.
static struct lfile *channel_arg(int output) {
    int lfn = ms_quantity(255);
    if (ms_err != E_NONE) return 0;
    struct lfile *f = find_lfn(lfn);
    if (!f) {
        ms_error(E_NOT_OPEN);
        return 0;
    }
    int writes = f->dev == 3 || f->dev == 4 || (f->dev >= 8 && f->sa == 15) ||
                 (f->chan >= 0 && chans[f->chan].write);
    int reads = f->dev == 0 || f->dev == 3 || (f->dev >= 8 && f->sa == 15) ||
                (f->chan >= 0 && !chans[f->chan].write);
    if (output ? !writes : !reads) {
        ms_error(output ? E_NOT_OUTPUT : E_NOT_INPUT);
        return 0;
    }
    return f;
}

/* ------------------------------------------------------------------------
 * Characters to and from files
 * --------------------------------------------------------------------- */

static void file_put(struct lfile *f, int c) {
    if (f->dev == 3) {
        c64_chrout(c);
        return;
    }
    f->column = c == 13 ? 0 : f->column + 1;
    if (f->dev >= 8 && f->sa == 15) {     // a command: at the Return
        if (c == 13) {
            cmd_line[cmd_len] = '\0';
            drive_command(cmd_line);
            cmd_len = 0;
        } else if (cmd_len < (int)sizeof cmd_line - 1) {
            cmd_line[cmd_len++] = c;
        }
        return;
    }
    if (f->chan < 0) return;
    struct channel *ch = &chans[f->chan];
    if (ch->printer) {                    // as text, in the printer's case
        int lower = f->sa == 7;
        if (c >= 65 && c <= 90) c += lower ? 32 : 0;
        else if (c >= 193 && c <= 218) c -= lower ? 128 : 96;
        else if (c == 13) c = '\n';
        else if (c < 32 || c >= 127) return;
    }
    if (ch->len < CHAN_SIZE) chan_buf(f->chan)[ch->len++] = c;
}

// The next character from a file: 0-255, or -1 at the end (ST says).
static int file_get(struct lfile *f) {
    if (f->dev >= 8 && f->sa == 15) {     // the error channel
        static char line[48];
        static int pos = -1, n;
        if (pos < 0) {
            n = status_line(line);
            line[n++] = 13;
            pos = 0;
        }
        int c = line[pos++];
        if (pos == n) {
            pos = -1;
            drive_err = 0;
            drive_count = 0;
            set_st(64);
        } else {
            set_st(0);
        }
        return c;
    }
    if (f->chan < 0) return -1;
    struct channel *ch = &chans[f->chan];
    if (ch->pos >= ch->len) {
        set_st(66);
        return -1;
    }
    int c = chan_buf(f->chan)[ch->pos++];
    set_st(ch->pos >= ch->len ? 64 : 0);
    return c;
}

int disk_to_file(void) {
    int lfn = out_lfn ? out_lfn : cmd_lfn;
    struct lfile *f = lfn ? find_lfn(lfn) : 0;
    return f && f->dev != 3;
}

int disk_out(int c) {
    int lfn = out_lfn ? out_lfn : cmd_lfn;
    if (!lfn) return 0;
    struct lfile *f = find_lfn(lfn);
    if (!f) {
        cmd_lfn = 0;
        return 0;
    }
    file_put(f, c);
    return 1;
}

int disk_input_line(char *buf, int cap, int *done) {
    *done = 0;
    if (!in_lfn) return 0;
    struct lfile *f = find_lfn(in_lfn);
    if (!f) return 0;
    *done = 1;
    if (f->dev == 0) return c64_input_line(buf, cap) >= 0;   // (no prompt)
    int n = 0, c;
    while ((c = file_get(f)) >= 0 && c != 13)
        if (n < cap - 1 && c) buf[n++] = c;
    buf[n] = '\0';
    return 1;
}

int disk_get(int *done) {
    *done = 0;
    if (!in_lfn) return -1;
    struct lfile *f = find_lfn(in_lfn);
    if (!f) return -1;
    *done = 1;
    if (f->dev == 0) return c64_get_key();
    int c = file_get(f);
    return c == 0 ? -1 : c;               // (a 0 byte reads as "", as there)
}

/* ------------------------------------------------------------------------
 * The statements
 * --------------------------------------------------------------------- */

int disk_statement(int t) {
    struct lfile *f;
    switch (t) {
    case T_LOAD: load(0); return 1;
    case T_VERIFY: verify(); return 1;
    case T_SAVE: save(); return 1;
    case T_OPEN: open_statement(); return 1;
    case T_CLOSE: close_statement(); return 1;
    case T_PRINTN:
    case T_CMD:
        f = channel_arg(1);
        if (!f) return 1;
        if (t == T_CMD) cmd_lfn = f->lfn;
        out_lfn = f->lfn;
        if (ms_cur() == ',') {
            ip++;
            ms_print_items();
        } else if (t == T_PRINTN || ms_at_end()) {
            if (t == T_PRINTN) ms_out(13);
        } else {
            ms_print_items();
        }
        if (t == T_PRINTN && cmd_lfn == f->lfn) cmd_lfn = 0;   // (PRINT# ends CMD)
        out_lfn = 0;
        return 1;
    case T_INPUTN:
    case T_GET64:                         // (GET#: GET with a #)
        if (t == T_GET64) ip++;
        f = channel_arg(0);
        if (!f) return 1;
        if (!ms_expect(',')) return 1;
        in_lfn = f->lfn;
        ms_input_quiet = f->dev ? E_FILE_DATA : 0;
        if (t == T_INPUTN) ms_input_vars();
        else ms_get_vars();
        ms_input_quiet = 0;
        in_lfn = 0;
        return 1;
    }
    return 0;
}

void disk_init(void) {
    disk_close_all();
    memset(chans, 0, sizeof chans);
    drive_err = 73;
    drive_count = 0;
    cmd_len = 0;
}

const char *disk_error_text(int e) {
    switch (e) {
    case E_TOO_MANY:   return "TOO MANY FILES";
    case E_FILE_OPEN:  return "FILE OPEN";
    case E_NOT_OPEN:   return "FILE NOT OPEN";
    case E_NOT_FOUND:  return "FILE NOT FOUND";
    case E_NO_DEVICE:  return "DEVICE NOT PRESENT";
    case E_NOT_INPUT:  return "NOT INPUT FILE";
    case E_NOT_OUTPUT: return "NOT OUTPUT FILE";
    case E_NO_NAME:    return "MISSING FILE NAME";
    case E_BAD_DEVICE: return "ILLEGAL DEVICE NUMBER";
    case E_VERIFY:     return "VERIFY";
    case E_FILE_DATA:  return "FILE DATA";
    }
    return 0;
}
