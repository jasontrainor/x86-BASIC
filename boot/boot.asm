[bits 16]
[org 0x7c00]

KERNEL_OFFSET equ 0x1000 ; The memory offset to which we will load our kernel

start:
    ; Some BIOSes enter at 07C0:0000 rather than 0000:7C00. Far-jump so
    ; CS is 0 and matches the org above.
    cli
    jmp 0x0000:.set_segments
.set_segments:
    xor ax, ax
    mov ds, ax
    mov es, ax
    ; Set SS and SP together with interrupts off, so an interrupt can't
    ; arrive while the stack pointer is half set up.
    mov ss, ax
    mov sp, 0x9000
    mov bp, sp
    sti
    cld                    ; string instructions count upwards

    ; BIOS sets boot drive in 'dl'; store it
    mov [BOOT_DRIVE], dl

    mov bx, MSG_REAL_MODE
    call print_string

    call load_kernel

    ; After kernel is loaded, jump to it
    ; Ensure segment is 0, offset is 0x1000
    jmp 0x0000:KERNEL_OFFSET

    jmp $ ; infinite loop if kernel somehow returns

%include "boot/print.asm"
%include "boot/disk_load.asm"

[bits 16]
load_kernel:
    mov bx, MSG_LOAD_KERNEL
    call print_string

    mov bx, KERNEL_OFFSET  ; Set ES:BX = 0x0000:0x1000
    mov dh, 30             ; Load 30 sectors (15KB, enough for our BASIC)
    mov dl, [BOOT_DRIVE]
    call disk_load

    ret

BOOT_DRIVE      db 0
MSG_REAL_MODE   db "Started in 16-bit Real Mode", 0x0D, 0x0A, 0
MSG_LOAD_KERNEL db "Loading BASIC kernel...", 0x0D, 0x0A, 0

; Bootsector padding
times 510-($-$$) db 0
dw 0xaa55
