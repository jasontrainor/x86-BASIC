/*
 * The platform layer (core/platform.h) for running a machine's BASIC
 * interpreter on Linux, with sanitizers, for tests. The machine's screen
 * code runs as on a PC; this just doesn't draw.
 *
 * Keyboard input comes from argv[1]. When the interpreter waits for a
 * key and the input has run out, the process exits.
 *
 * What is printed is written to stdout as plain text (screen.c calls
 * host_echo), with [TAG] markers for screen and sound commands.
 *
 * Built with -DFUZZ (tests/fuzz.sh), it's a libFuzzer target instead:
 * see the end of this file.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "keymap.h"
#include "fat.h"
#include "machine.h"

#ifdef FUZZ
#include <setjmp.h>
static jmp_buf fuzz_done;           /* where a run ends: the input is used up */
static long fuzz_polls;             /* key polls since the input ran out */
#define printf(...) ((void)0)
#define puts(s) ((void)0)
#define putchar(c) ((void)0)
#define exit(n) longjmp(fuzz_done, 1)
#endif

static const char *input;
int host_scroll_prompt;             /* ask "scroll?" (env SCROLL_PROMPT) */
static long output_chars;

void host_echo(char c) {
    if (c == '\r') return;          /* cursor-only; keep output as plain \n */
    if (c == 8) c = '~';            /* make backspaces visible */
    putchar(c);
#ifdef FUZZ
    if (output_chars > 20000) exit(0);
#endif
    if (++output_chars > 100000) {  /* guard against runaway programs */
        puts("\n[OUTPUT LIMIT]");
        exit(0);
    }
}

void host_trace(const char *fmt, int a, int b) { printf(fmt, a, b); }

/* A machine can leave something for the tests when the run ends (the
   C64's screen memory as text, say): it sets host_exit_hook, which
   writes with host_write() to the file an environment variable names. */
void (*host_exit_hook)(void);
static void run_exit_hook(void) {
    if (host_exit_hook) host_exit_hook();
}
void host_write(const char *env, const void *data, int len) {
    const char *path = getenv(env);
    FILE *f = path ? fopen(path, "wb") : 0;
    if (!f) return;
    fwrite(data, 1, len, f);
    fclose(f);
}

/* The picture is kept, and written out at the end as a PPM file if
   SCREEN_OUT names one (each pixel tall times, as it's shown). */
static struct video video;
static unsigned char *picture;
void plat_init(void) {}
void plat_video(const struct video *v) {
    video = *v;
    free(picture);
    picture = calloc(v->width * v->height, 1);
}
void plat_put_block(int x, int y, int w, int h, const unsigned char *pixels) {
    if (x % 8 || w % 8 || x < 0 || y < 0 || x + w > video.width || y + h > video.height) {
        printf("[BAD BLOCK %d,%d %dx%d]", x, y, w, h);
        return;
    }
    for (int j = 0; j < h; j++)
        memcpy(picture + (y + j) * video.width + x, pixels + j * w, w);
}
void plat_border(int colour) {}
static void save_screen(void) {
    FILE *f = fopen(getenv("SCREEN_OUT"), "wb");
    if (!f || !picture) return;
    fprintf(f, "P6\n%d %d\n255\n", video.width, video.height * video.tall);
    for (int y = 0; y < video.height * video.tall; y++)
        for (int x = 0; x < video.width; x++)
            fwrite(video.palette[picture[(y / video.tall) * video.width + x] & 15], 1, 3, f);
    fclose(f);
}

/*
 * The input is "typed ahead": a key becomes available on every other
 * call, so that a running program sees keys arrive one at a time (an
 * Esc reaches check_break() only when it's next) rather than finding
 * all of the rest of the input already waiting.
 */
int plat_read_key(void) {
    static int toggle;
    int c;
#ifdef FUZZ
    /* A program still running when the input has run out (10 GOTO 10)
       ends the run after a while. */
    if (!*input && ++fuzz_polls > 2000) exit(0);
#endif
    if (!*input || (toggle = !toggle) == 0) return -1;
    switch (*input++) {             /* control characters for the editing keys */
    case 0x02: return KEY_LEFT;     /* (as Ctrl+B, F, P, N, A, E in Emacs) */
    case 0x06: return KEY_RIGHT;
    case 0x10: return KEY_UP;
    case 0x0e: return KEY_DOWN;
    case 0x01: return KEY_HOME;
    case 0x05: return KEY_END;
    case 0x7f: return KEY_DELETE;
    case 0x12: return KEY_PAGE_UP;  /* (Ctrl+R) */
    case 0x1c:                      /* Alt with the next key (a digit or letter) */
        if (!*input) return -1;
        c = (unsigned char)*input++;
        if (c >= '0' && c <= '9') return KEY_ALT_DIGIT + c - '0';
        if (c >= 'a' && c <= 'z') return KEY_ALT_LETTER + c - 'a';
        return -1;
    case 0x1d:                      /* AltGr with the next key */
        if (!*input) return -1;
        c = keymap_key((unsigned char)*input++, 0, 1);
        return c == 163 || (c >= 32 && c < 127) ? c : -1;
    }
    c = (unsigned char)input[-1];
    if (c < 32 || c >= 127) return c;
    c = keymap_key(c, 0, 0);        /* (as the platforms do) */
    return c == 163 || (c >= 32 && c < 127) ? c : -1;
}

/* HOST_BUTTONS: the modifier keys held down (plat_buttons()). */
int plat_buttons(void) {
    const char *b = getenv("HOST_BUTTONS");
    return b ? atoi(b) : 0;
}

/* HOST_HELD: the keys held down (plat_key_down()), as hex scan codes:
   "1E,2A" is A and Shift. Unset, the platform can't tell (-1), as on UEFI. */
int plat_key_down(int code) {
    const char *h = getenv("HOST_HELD");
    if (!h) return -1;
    while (*h) {
        char *end;
        long c = strtol(h, &end, 16);
        if (end == h) break;
        if (c == code) return 1;
        h = *end ? end + 1 : end;
    }
    return 0;
}

void plat_idle(void) {
    if (!*input) {                  /* waiting for a key that will never come */
        fflush(stdout);
        exit(0);
    }
}

/* Time moves on 20ms every time something looks at the clock. */
unsigned long plat_timer_ms(void) { static unsigned long t; return t += 20; }

void plat_tone(int f, int ms) { printf("[TONE %d %d]", f, ms); }
static long host_clicks;
void plat_click(void) { host_clicks++; }
static void report_clicks(void) {
    if (host_clicks) printf("[CLICKS %ld]", host_clicks);
}

/* A blank 1.44MB disk in memory (or DISK_IMAGE, of any size). Its last
   sector always fails, for tests. */
static unsigned char (*disk)[512];
static unsigned int disk_sectors = 2880;
int plat_disk_read(unsigned int lba, void *buf) {
    if (lba >= disk_sectors - 1) return -1;
    memcpy(buf, disk[lba], 512);
    return 0;
}
#ifdef FUZZ
static unsigned char *dirty;        /* sectors a run has written */
#endif
int plat_disk_write(unsigned int lba, const void *buf) {
    if (lba >= disk_sectors - 1) return -1;
    memcpy(disk[lba], buf, 512);
#ifdef FUZZ
    dirty[lba] = 1;
#endif
    return 0;
}

/* DISK_IMAGE: start with this disk image instead of a blank disk;
   DISK_OUT: write the disk out here at the end. */
static void save_disk(void) {
    FILE *f = fopen(getenv("DISK_OUT"), "wb");
    if (f) {
        fwrite(disk, 512, disk_sectors, f);
        fclose(f);
    }
}

#ifndef FUZZ
int main(int argc, char **argv) {
    input = argc > 1 ? argv[1] : "";
    const char *image = getenv("DISK_IMAGE");
    if (image && *image) {
        FILE *f = fopen(image, "rb");
        if (!f || fseek(f, 0, SEEK_END) != 0) {
            perror("DISK_IMAGE");
            return 1;
        }
        disk_sectors = ftell(f) / 512;
        rewind(f);
        disk = calloc(disk_sectors, 512);
        if (disk_sectors < 2 || fread(disk, 512, disk_sectors, f) != disk_sectors) {
            perror("DISK_IMAGE");
            return 1;
        }
        fclose(f);
    } else {
        disk = calloc(disk_sectors, 512);
    }
    if (getenv("DISK_OUT")) atexit(save_disk);
    if (getenv("SCREEN_OUT")) atexit(save_screen);
    atexit(run_exit_hook);
    atexit(report_clicks);
    host_scroll_prompt = getenv("SCROLL_PROMPT") != NULL;
    keymap_init();                  /* as kernel_start() does: BOOT.CFG, */
    if (getenv("HOST_KEYBOARD")) keymap_set(getenv("HOST_KEYBOARD"));   /* or this */
    machine_start();
    return 0;
}
#endif

#ifdef FUZZ
/*
 * The fuzz target: an input is a file, then a 0 byte, then what's typed.
 * The file is put on a blank disk as FUZZ.BAS and FUZZ.SAV in the
 * machine's folder (FUZZ_FOLDER), for LOAD, MERGE, RUN, EXEC and the
 * like to read, and the machine starts afresh each time; the run ends
 * when it waits for a key and there are none left, or one has been
 * waited for too long. The disk starts as FUZZ_DISK, made by
 * tests/make_disk.py (a FAT partition and nothing else).
 */
static unsigned char (*blank)[512];
static char typed[65536];

int LLVMFuzzerInitialize(int *argc, char ***argv) {
    (void)argc, (void)argv;
    const char *image = getenv("FUZZ_DISK");
    FILE *f = image ? fopen(image, "rb") : 0;
    if (!f) {
        fprintf(stderr, "FUZZ_DISK: a disk image made by tests/make_disk.py\n");
        abort();
    }
    blank = calloc(disk_sectors, 512);
    disk = calloc(disk_sectors, 512);
    dirty = calloc(disk_sectors, 1);
    if (fread(blank, 512, disk_sectors, f) != disk_sectors) abort();
    fclose(f);
    for (unsigned int i = 0; i < disk_sectors; i++) dirty[i] = 1;
    return 0;
}

int LLVMFuzzerTestOneInput(const unsigned char *data, size_t size) {
    const unsigned char *zero = 0;  /* (core/string.h, not the C library's, is in use) */
    for (size_t i = 0; i < size && !zero; i++)
        if (!data[i]) zero = data + i;
    size_t file_len = zero ? (size_t)(zero - data) : 0;
    size_t typed_len = zero ? size - file_len - 1 : size;
    if (typed_len >= sizeof typed) typed_len = sizeof typed - 1;
    memcpy(typed, zero ? zero + 1 : data, typed_len);
    typed[typed_len] = '\0';
    for (size_t i = 0; i < typed_len; i++)
        if (!typed[i]) typed[i] = ' ';
    /* The disk as it started: only the sectors the last run wrote (this
       memcpy is core/string.c's, a byte at a time, as are the
       sanitizers'). */
    for (unsigned int i = 0; i < disk_sectors; i++)
        if (dirty[i]) {
            memcpy(disk[i], blank[i], 512);
            dirty[i] = 0;
        }
    if (file_len) {
        fat_folder(getenv("FUZZ_FOLDER") ? getenv("FUZZ_FOLDER") : "SPECTRUM");
        fat_write("FUZZ.BAS", data, (int)file_len);
        fat_write("FUZZ.SAV", data, (int)file_len);
    }
    input = typed;
    output_chars = 0;
    fuzz_polls = 0;
    keymap_set("US");
    if (!setjmp(fuzz_done)) machine_start();
    return 0;
}
#endif
