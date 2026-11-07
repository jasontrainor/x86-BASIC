/*
 * The platform layer (kernel/platform.h) for running the BASIC
 * interpreter on Linux, with sanitizers, for tests. The shared screen
 * code (kernel/screen.c) runs as on a PC; this just doesn't draw.
 *
 * Keyboard input comes from argv[1]. When the interpreter waits for a
 * key and the input has run out, the process exits.
 *
 * What is printed is written to stdout as plain text (screen.c calls
 * host_echo), with [TAG] markers for screen and sound commands.
 */
#include <stdio.h>
#include <stdlib.h>
#include "platform.h"

void basic_run(void);

static const char *input;
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
void plat_draw_cell(int col, int row, const unsigned char *bits, int ink, int paper) {}
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

int main(int argc, char **argv) {
    input = argc > 1 ? argv[1] : "";
    basic_run();
    return 0;
}
