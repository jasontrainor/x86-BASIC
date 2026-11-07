[bits 16]
[extern main]

STACK_TOP equ 0x9000

; Placed first in the image by linker.ld, so this is at 0x1000.
section .text.entry progbits alloc exec nowrite align=1

global _start
_start:
    ; GCC's -m16 code addresses the stack through the full 32-bit ESP and
    ; EBP, but the boot sector (and BIOS) only set the low 16 bits. Load
    ; clean 32-bit values so stale upper bits can't produce bad addresses.
    cli
    xor eax, eax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov esp, STACK_TOP
    xor ebp, ebp
    sti
    cld                    ; GCC assumes the direction flag is clear

    call main
    jmp $
