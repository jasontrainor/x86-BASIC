#ifndef MEM_H
#define MEM_H

/*
 * HIGH_MEM puts a (zero-initialised) buffer above 64KB in the BIOS
 * kernel, where there is plenty of room; see linker.ld. Elsewhere (the
 * host test build) it does nothing.
 */
#ifdef BIOS_KERNEL
#define HIGH_MEM __attribute__((section(".bss.high")))
#else
#define HIGH_MEM
#endif

#endif
