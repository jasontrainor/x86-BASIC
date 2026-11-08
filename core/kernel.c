#include "platform.h"
#include "machine.h"

// Common start-up, once the platform is ready to go.
void kernel_start(void) {
    plat_init();
    machine_start();
}

#ifdef BIOS_KERNEL
extern char __bss_start, __bss_end;     // see core/bios.ld
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
