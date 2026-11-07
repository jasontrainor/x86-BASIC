[bits 16]
[extern main]

; Placed first in the image by linker.ld, so this is at 0x1000.
section .text.entry progbits alloc exec nowrite align=1

global _start
_start:
    call main
    jmp $
