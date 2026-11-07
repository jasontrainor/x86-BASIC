/*
 * Memory map (real mode; all segment registers are 0):
 *
 *   0x00000-0x004FF  interrupt vectors and BIOS data area
 *   0x00600-0x007FF  boot sector (moved here from 0x7C00 by itself)
 *   0x00800-0x00FFF  boot sector's stack
 *   0x01000-...      kernel code and data, loaded by the boot sector
 *   ...              .bss (small variables), zeroed at startup
 *   ...-0x0FFF0      stack, growing down from __stack_top
 *   0x10000-...      .bss.high: big buffers (program, variables, screen)
 *
 * Code and stack must stay below 64KB: CS and SS are real-mode segments
 * at 0. The data segments have a 4GB limit ("unreal mode", set up in
 * kernel_entry.asm), so data above 64KB can be reached with 32-bit
 * addresses; C code marks such buffers HIGH_MEM (see kernel/mem.h).
 *
 * The ASSERTs below fail the build if anything doesn't fit, instead of
 * letting code, data and stack silently overlap.
 */
__stack_top  = 0xFFF0;
STACK_RESERVE = 12K;
HIGH_START = 0x10000;
HIGH_END   = 0x80000;   /* conventional memory is safe to well past this */

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

    /* Not stored in kernel.bin; kernel.c zeroes it at startup. Listed
       before .bss so that the .bss.* pattern there doesn't claim it. */
    .bss.high HIGH_START (NOLOAD) : ALIGN(16) {
        __high_start = .;
        *(.bss.high .bss.high.*)
        __high_end = .;
    }

    .bss __image_end (NOLOAD) : ALIGN(4) {
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

/* boot/boot.asm loads at most up to 0xF000. */
ASSERT(__image_end <= 0xF000, "kernel.bin too big for the boot sector to load")
ASSERT(__bss_end <= __stack_top - STACK_RESERVE,
       "kernel code and data too big: less than STACK_RESERVE left for the stack")
ASSERT(__high_end <= HIGH_END, "HIGH_MEM buffers too big")
