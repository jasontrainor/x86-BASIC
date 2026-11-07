#include "bios.h"
#include "basic.h"

extern char __bss_start;
extern char __bss_end;

void main() {
    char *bss = &__bss_start;
    while (bss < &__bss_end) {
        *bss++ = 0;
    }

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
