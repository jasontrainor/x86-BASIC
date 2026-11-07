/*
 * Memory map (real mode, everything in segment 0):
 *
 *   0x00000-0x004FF  interrupt vectors and BIOS data area
 *   0x01000-...      kernel code and data, loaded by the boot sector
 *   ...              .bss (program store, variables), zeroed at startup
 *   0x07C00-0x07DFF  boot sector (only needed until the kernel starts)
 *   ...-0x0FFF0      stack, growing down from __stack_top
 *
 * Everything must stay below 64KB: the C code addresses memory as
 * offsets from segment 0. The ASSERTs below fail the build if it doesn't
 * fit, instead of letting data and stack silently overlap.
 */
__stack_top  = 0xFFF0;
STACK_RESERVE = 16K;

ENTRY(_start)
SECTIONS
{
    /* The boot sector loads the kernel image here (see boot/boot.asm). */
    . = 0x1000;

    .text : ALIGN(4) {
        /* _start must be the first byte: the boot sector jumps to 0x1000. */
        KEEP(*(.text.entry))
        *(.text.startup .text.startup.*)   /* GCC puts main() here */
        *(.text .text.*)
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4) {
        *(.data .data.*)
    }
    __image_end = .;   /* end of what kernel.bin contains */

    /* Not stored in kernel.bin; kernel.c zeroes it at startup. */
    .bss : ALIGN(4) {
        __bss_start = .;
        *(.bss .bss.*)
        *(COMMON)
        __bss_end = .;
    }

    /* Unwind tables, compiler notes etc. are useless in a flat binary. */
    /DISCARD/ : {
        *(.eh_frame .eh_frame_hdr .comment .note .note.*)
    }
}

/* The boot sector is still running at 0x7C00 while it loads the kernel. */
ASSERT(__image_end <= 0x7C00,
       "kernel.bin too big: loading it would overwrite the boot sector")
ASSERT(__bss_end <= __stack_top - STACK_RESERVE,
       "kernel data too big: less than STACK_RESERVE left for the stack")
