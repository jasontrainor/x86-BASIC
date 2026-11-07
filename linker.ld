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
