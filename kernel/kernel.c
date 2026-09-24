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
    
    // Spectrum boot screen: Blank screen, copyright at bottom left
    for (int i = 0; i < 23; i++) {
        print_string("\n");
    }
    print_string("(C) 1982 Sinclair Research Ltd\n");
    
    basic_run();
}
