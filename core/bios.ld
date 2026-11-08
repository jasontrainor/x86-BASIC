/*
 * Memory map (real mode):
 *
 *   0x00000-0x004FF  interrupt vectors and BIOS data area
 *   0x00600-0x007FF  boot sector (moved here from 0x7C00 by itself)
 *   0x00800-0x00FFF  boot sector's stack
 *   0x01000-...      start-up code, constants and initialised data
 *   ...              .bss (small variables), zeroed at startup
 *   ...-0x0FFF0      stack, growing down from __stack_top
 *   0x10000-0x7FFFF  .bss.high: big buffers (program, variables, screen)
 *   0x80000-0x8FFFF  the kernel's code (CS = 0x8000)
 *
 * The data segment registers are 0 and have a 4GB limit ("unreal mode",
 * set up in kernel_entry.asm), so data anywhere in memory can be reached
 * with 32-bit addresses. But GCC addresses a variable it can name
 * directly with a 16-bit address, so the small variables must stay below
 * 64KB; C code marks the big buffers HIGH_MEM (see core/mem.h) to put
 * them above it. The stack is in the first 64KB too (SS = 0, SP).
 *
 * The code has a real-mode segment of its own, CS = 0x8000, so it can be
 * up to 64KB without crowding out the data and stack. It is linked at
 * offset 0 of that segment, so code addresses (return addresses,
 * function pointers, switch tables) are offsets within it, while data
 * addresses are linear. Nothing reads the code as data.
 *
 * The boot sector (for the chooser) or the chooser (for a machine: see
 * start_image in kernel_entry.asm) loads kernel.bin, start-up code and
 * data followed by the code, to 0x1000; kernel_entry.asm moves the code
 * up to 0x80000.
 * The .bss then reuses the space the code was loaded into.
 *
 * The ASSERTs below fail the build if anything doesn't fit, instead of
 * letting code, data and stack silently overlap.
 */
__stack_top  = 0xFFF0;
STACK_RESERVE = 40K;
HIGH_START = 0x10000;
HIGH_END   = 0x80000;
CODE_BASE  = 0x80000;   /* kernel_entry.asm: CODE_SEGMENT */
CODE_MAX   = 0x10000;

ENTRY(_start)
SECTIONS
{
    /* The boot sector loads the kernel image here (see boot/boot.asm). */
    . = 0x1000;

    .entry : {
        /* _start must be the first byte: the boot sector jumps to 0x1000. */
        KEEP(*(.text.entry))
    }

    .rodata : ALIGN(4) {
        *(.rodata .rodata.*)
    }

    .data : ALIGN(4) {
        *(.data .data.*)
    }

    /* The code, loaded here (a paragraph boundary, so kernel_entry.asm
       can copy it by segments) and run at CODE_BASE. */
    . = ALIGN(16);
    __code_load = .;
    .text 0 : AT(__code_load) {
        /* _code_start first: offset 0 is never a function pointer. */
        KEEP(*(.text.code_entry))
        *(.text.startup .text.startup.*)   /* GCC puts main() here */
        *(.text .text.*)
    }
    __code_size = SIZEOF(.text);
    __image_end = __code_load + __code_size;   /* end of what kernel.bin contains */

    /* Not stored in kernel.bin; kernel.c zeroes it at startup. Listed
       before .bss so that the .bss.* pattern there doesn't claim it. */
    .bss.high HIGH_START (NOLOAD) : ALIGN(16) {
        __high_start = .;
        *(.bss.high .bss.high.*)
        __high_end = .;
    }

    .bss __code_load (NOLOAD) : ALIGN(4) {
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

ASSERT(__code_size <= CODE_MAX, "kernel code bigger than its 64KB segment")
ASSERT(__code_load <= 0xF000, "kernel constants and data too big")
/* boot/boot.asm loads the image to 0x1000 and up, at most to 0x20000 */
ASSERT(__image_end <= 0x20000, "kernel.bin too big for the boot sector to load")
ASSERT(__bss_end <= __stack_top - STACK_RESERVE,
       "kernel data too big: less than STACK_RESERVE left for the stack")
ASSERT(__high_end <= HIGH_END, "HIGH_MEM buffers too big")
