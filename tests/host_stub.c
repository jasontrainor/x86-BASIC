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
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "machine.h"

static const char *input;
int host_scroll_prompt;             /* ask "scroll?" (env SCROLL_PROMPT) */
static long output_chars;

void host_echo(char c) {
    if (c == '\r') return;          /* cursor-only; keep output as plain \n */
    if (c == 8) c = '~';            /* make backspaces visible */
    putchar(c);
    if (++output_chars > 100000) {  /* guard against runaway programs */
        puts("\n[OUTPUT LIMIT]");
        exit(0);
    }
}

void host_trace(const char *fmt, int a, int b) { printf(fmt, a, b); }

void plat_init(void) {}
void plat_video(const struct video *v) {}
void plat_put_block(int x, int y, int w, int h, const unsigned char *pixels) {}
void plat_border(int colour) {}

/*
 * The input is "typed ahead": a key becomes available on every other
 * call, so that a running program sees keys arrive one at a time (an
 * Esc reaches check_break() only when it's next) rather than finding
 * all of the rest of the input already waiting.
 */
int plat_read_key(void) {
    static int toggle;
    if (!*input || (toggle = !toggle) == 0) return -1;
    switch (*input++) {             /* control characters for the editing keys */
    case 0x02: return KEY_LEFT;     /* (as Ctrl+B, F, P, N, A, E in Emacs) */
    case 0x06: return KEY_RIGHT;
    case 0x10: return KEY_UP;
    case 0x0e: return KEY_DOWN;
    case 0x01: return KEY_HOME;
    case 0x05: return KEY_END;
    case 0x7f: return KEY_DELETE;
    }
    return (unsigned char)input[-1];
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

/* A blank 1.44MB disk in memory (or DISK_IMAGE, of any size). Its last
   sector always fails, for tests. */
static unsigned char (*disk)[512];
static unsigned int disk_sectors = 2880;
int plat_disk_read(unsigned int lba, void *buf) {
    if (lba >= disk_sectors - 1) return -1;
    memcpy(buf, disk[lba], 512);
    return 0;
}
int plat_disk_write(unsigned int lba, const void *buf) {
    if (lba >= disk_sectors - 1) return -1;
    memcpy(disk[lba], buf, 512);
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

int main(int argc, char **argv) {
    input = argc > 1 ? argv[1] : "";
    if (getenv("DISK_IMAGE")) {
        FILE *f = fopen(getenv("DISK_IMAGE"), "rb");
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
    host_scroll_prompt = getenv("SCROLL_PROMPT") != NULL;
    machine_start();                /* as kernel_start() does */
    return 0;
}
