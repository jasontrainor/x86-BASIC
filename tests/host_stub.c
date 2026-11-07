/*
 * Host-side stand-ins for the BIOS layer (kernel/bios.c), so the BASIC
 * interpreter can be compiled and tested on Linux with sanitizers.
 *
 * Keyboard input comes from argv[1]; when it runs out the process exits.
 * Screen output goes to stdout. Hardware calls print a [TAG] marker.
 */
#include <stdio.h>
#include <stdlib.h>

void basic_run(void);

static const char *input;
static long output_chars;

void print_char(char c) {
    if (c == '\r') return;          /* cursor-only; keep output as plain \n */
    if (c == 8) c = '~';            /* make backspaces visible */
    putchar(c);
    if (++output_chars > 100000) {   /* guard against runaway programs */
        puts("\n[OUTPUT LIMIT]");
        exit(0);
    }
}

void print_string(const char *s) { while (*s) print_char(*s++); }

char get_char(void) {
    if (!*input) { fflush(stdout); exit(0); }
    return *input++;
}

/* An Esc (0x1b) at the front of the input acts as a BREAK press. */
int check_break(void) {
    if (*input == 27) { input++; return 1; }
    return 0;
}

void clear_screen(void)  { fputs("[CLS]", stdout); }
void set_ink(int c)      { printf("[INK %d]", c); }
void set_paper(int c)    { printf("[PAPER %d]", c); }
void set_border(int c)   { printf("[BORDER %d]", c); }
void beep(void)          { fputs("[BEEP]", stdout); }
void update_cursor(void) {}
void init_video(void)    {}

int main(int argc, char **argv) {
    input = argc > 1 ? argv[1] : "";
    basic_run();
    return 0;
}
