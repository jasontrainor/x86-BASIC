[bits 16]
; The BIOS loads this sector at 0x7C00, but the first thing it does is
; copy itself down to 0x0600, so that the kernel - the chooser, which
; starts the machines (machines/chooser/) - can be loaded from 0x1000 up
; without overwriting the code doing the loading.
[org 0x0600]

RELOCATED     equ 0x0600
KERNEL_OFFSET equ 0x1000 ; The memory offset to which we will load our kernel
MAX_KERNEL_SECTORS equ (0x20000 - KERNEL_OFFSET) / 512   ; as core/bios.ld

; The Makefile passes the chooser's size in sectors (-DKERNEL_SECTORS=n).
%ifndef KERNEL_SECTORS
  %error "KERNEL_SECTORS not defined; build with make"
%endif
%if KERNEL_SECTORS > MAX_KERNEL_SECTORS
  %error "kernel.bin is too big to load below 0x20000"
%endif

; ---------------------------------------------------------------------------
; BIOS Parameter Block
;
; When booting a USB stick, firmware decides how to present it by looking
; at this sector. In floppy-emulation ("USB-FDD") mode, many BIOSes expect
; a FAT-style BPB here and overwrite some of its fields with the drive
; geometry, and some refuse to boot without one. So the first bytes are a
; short jump over a BPB rather than code that could be clobbered.
;
; The values describe a 1.44MB floppy. The number of FATs is deliberately
; 0: there is no FAT filesystem on this disk, and an invalid FAT count
; stops operating systems from trying to mount (and write to) it.
; ---------------------------------------------------------------------------
    jmp short start
    nop
    db "X86BASIC"          ; OEM name
    dw 512                 ; bytes per sector
    db 1                   ; sectors per cluster
    dw 1                   ; reserved sectors
    db 0                   ; number of FATs (0: not a FAT volume, see above)
    dw 224                 ; root directory entries
    dw 2880                ; total sectors (1.44MB)
    db 0xF0                ; media descriptor (1.44MB floppy)
    dw 9                   ; sectors per FAT
    dw 18                  ; sectors per track
    dw 2                   ; heads
    dd 0                   ; hidden sectors
    dd 0                   ; total sectors (32-bit)
    db 0                   ; drive number (often patched by the BIOS)
    db 0                   ; reserved
    db 0x29                ; extended boot signature
    dd 0x20261108          ; volume serial number
    db "X86 BASIC  "       ; volume label (11 bytes)
    db "        "          ; filesystem type (8 bytes, blank: none)

start:
    ; Some BIOSes enter at 07C0:0000 rather than 0000:7C00, so don't rely
    ; on CS until the far jump below. Copying from linear 0x7C00 works
    ; either way.
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    ; Set SS and SP together with interrupts off, so an interrupt can't
    ; arrive while the stack pointer is half set up. The stack sits in
    ; 0x0800-0x0FFF, between the relocated boot sector and the kernel.
    mov ss, ax
    mov sp, KERNEL_OFFSET
    cld                    ; string instructions count upwards
    mov si, 0x7C00
    mov di, RELOCATED
    mov cx, 256
    rep movsw
    jmp 0x0000:relocated   ; continue in the copy, with CS = 0
relocated:
    sti

    ; BIOS sets boot drive in 'dl'; store it
    mov [BOOT_DRIVE], dl

    mov bx, MSG_REAL_MODE
    call print_string

    call load_kernel

    ; Jump to the kernel at 0000:1000, telling it the boot drive in DL
    ; (it needs it to save and load programs).
    mov dl, [BOOT_DRIVE]
    jmp 0x0000:KERNEL_OFFSET

%include "boot/print.asm"
%include "boot/disk_load.asm"

[bits 16]
load_kernel:
    mov bx, MSG_LOAD_KERNEL
    call print_string
    call disk_load         ; KERNEL_SECTORS sectors to KERNEL_OFFSET and up
    ret

BOOT_DRIVE      db 0
MSG_REAL_MODE   db "Started in 16-bit Real Mode", 0x0D, 0x0A, 0
MSG_LOAD_KERNEL db "Loading x86 BASIC...", 0x0D, 0x0A, 0

; ---------------------------------------------------------------------------
; MBR partition table: written by tools/mkimage.py, which decides the
; layout. Partition 1 (active, type 0xDA "non-filesystem data", from LBA
; 1) holds the chooser, so that BIOSes that only boot a disk with an
; active partition boot this one; partition 2 is the FAT partition (type
; 0xEF, the EFI system partition) with everything else.
; ---------------------------------------------------------------------------
times 0x1B8-($-$$) db 0    ; (assembly fails here if the code is too big)
    dd 0x42534958          ; disk signature
    dw 0
    times 4 * 16 db 0      ; the four partitions

dw 0xaa55
