#ifndef MEM_H
#define MEM_H

/*
 * HIGH_MEM puts a (zero-initialised) buffer above 64KB in the BIOS
 * kernel, where there is plenty of room; see core/bios.ld. Elsewhere (the
 * host test build) it does nothing.
 *
 * GCC's 16-bit code reaches a fixed address (buf[0], a constant offset
 * into a HIGH_MEM buffer) with a 16-bit address, which can't get there:
 * the link fails with "relocation truncated to fit: R_386_16". So such
 * a buffer is used through a pointer the compiler can't see through: a
 * parameter of a noinline, noclone function, or a non-static global.
 */
#ifdef BIOS_KERNEL
#define HIGH_MEM __attribute__((section(".bss.high")))
#else
#define HIGH_MEM
#endif

#endif
