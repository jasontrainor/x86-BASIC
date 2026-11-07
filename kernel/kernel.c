#include "bios.h"
#include "basic.h"

extern char __bss_start, __bss_end;     // see linker.ld
extern char __high_start, __high_end;

static void zero(char *from, char *to) {
    while (from < to) *from++ = 0;
}

void main() {
    zero(&__bss_start, &__bss_end);
    zero(&__high_start, &__high_end);   // above 64KB: needs unreal mode

    init_video();
    
    // Spectrum defaults: White paper (7), Black ink (0), White border (7)
    set_paper(7);
    set_ink(0);
    set_border(7);
    clear_screen();
    
    // Spectrum-style boot screen: blank, with a banner at the bottom left.
    // (Our own name rather than a real company's copyright notice.)
    for (int i = 0; i < 23; i++) {
        print_string("\n");
    }
    print_string("x86 BASIC - Spectrum-style BASIC for PCs\n");
    
    basic_run();
}
