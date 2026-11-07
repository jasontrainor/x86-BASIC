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

#define COLS 32
#define ROWS 24

static const char *input;
static long output_chars;
static int col;

void print_char(char c) {
    if (c == '\r') return;          /* cursor-only; keep output as plain \n */
    if (c == '\n') col = 0;
    else if (c == 8) { if (col) col--; c = '~'; }   /* make backspaces visible */
    else if (++col == COLS) col = 0;
    putchar(c);
    if (++output_chars > 100000) {  /* guard against runaway programs */
        puts("\n[OUTPUT LIMIT]");
        exit(0);
    }
}

void print_string(const char *s) { while (*s) print_char(*s++); }

char get_char(void) {
    if (!*input) { fflush(stdout); exit(0); }
    return *input++;
}

/* The rest of the input is "typed ahead": INKEY$ and PAUSE see it. */
int poll_key(void)    { return *input ? (unsigned char)*input++ : -1; }
int key_waiting(void) { return *input != 0; }

/* An Esc (0x1b) at the front of the input acts as a BREAK press. */
int check_break(void) {
    if (*input == 27) { input++; return 1; }
    return 0;
}

/* Time moves on 20ms every time something looks at the clock. */
unsigned long timer_ms(void) { static unsigned long t; return t += 20; }

void clear_screen(void)  { fputs("[CLS]", stdout); col = 0; }
void set_ink(int c)      { printf("[INK %d]", c); }
void set_paper(int c)    { printf("[PAPER %d]", c); }
void set_border(int c)   { printf("[BORDER %d]", c); }
void tone(int f, int ms) { printf("[TONE %d %d]", f, ms); }
void set_cursor(int c, int r) { printf("[AT %d,%d]", r, c); col = c; }
int cursor_col(void)     { return col; }
int screen_cols(void)    { return COLS; }
int screen_rows(void)    { return ROWS; }
void update_cursor(void) {}
void init_video(void)    {}

int main(int argc, char **argv) {
    input = argc > 1 ? argv[1] : "";
    basic_run();
    return 0;
}
