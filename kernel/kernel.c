#include "platform.h"
#include "screen.h"
#include "basic.h"

// Common start-up, once the platform is ready to go.
void kernel_start(void) {
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

#ifdef BIOS_KERNEL
extern char __bss_start, __bss_end;     // see linker.ld
extern char __high_start, __high_end;

static void zero(char *from, char *to) {
    while (from < to) *from++ = 0;
}

// Called by kernel_entry.asm. (The UEFI kernel starts at efi_main.)
void main() {
    zero(&__bss_start, &__bss_end);
    zero(&__high_start, &__high_end);   // above 64KB: needs unreal mode
    kernel_start();
}
#endif
