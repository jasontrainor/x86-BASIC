#include "platform.h"
#include "screen.h"
#include "basic.h"

extern char __bss_start, __bss_end;     // see linker.ld
extern char __high_start, __high_end;

static void zero(char *from, char *to) {
    while (from < to) *from++ = 0;
}

void main() {
    zero(&__bss_start, &__bss_end);
    zero(&__high_start, &__high_end);   // above 64KB: needs unreal mode

    plat_init();
    screen_init();   // Spectrum defaults: black ink on white paper, white border

    // Spectrum-style boot screen: blank, with a banner at the bottom left.
    // (Our own name rather than a real company's copyright notice.)
    // The first key clears it.
    set_cursor(0, SCREEN_ROWS - 1);
    print_string("x86 BASIC for PCs");
    set_cursor(0, 0);
    unget_key(get_key());
    clear_screen();

    basic_run();
}
