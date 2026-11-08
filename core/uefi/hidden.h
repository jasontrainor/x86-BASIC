/*
 * Included at the top of every file in the UEFI build (gcc -include).
 *
 * In position-independent code, GCC takes the address of a symbol that
 * may be defined in another file (a function pointer to set_ink, say) by
 * loading it from a global offset table, which an ELF linker builds or
 * optimises away. Linking to a UEFI (PE) image, GNU ld does neither, so
 * mark every symbol hidden: GCC then computes addresses directly,
 * relative to the instruction pointer. The Makefile checks that no GOT
 * references are left.
 */
#pragma GCC visibility push(hidden)
