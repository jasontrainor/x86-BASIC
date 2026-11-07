[bits 16]
[org 0x7c00]

KERNEL_OFFSET equ 0x1000 ; The memory offset to which we will load our kernel

; The Makefile passes the kernel's size in sectors (-DKERNEL_SECTORS=n).
%ifndef KERNEL_SECTORS
  %error "KERNEL_SECTORS not defined; build with make"
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
    call disk_load         ; KERNEL_SECTORS sectors to 0000:KERNEL_OFFSET
    ret

BOOT_DRIVE      db 0
MSG_REAL_MODE   db "Started in 16-bit Real Mode", 0x0D, 0x0A, 0
MSG_LOAD_KERNEL db "Loading BASIC kernel...", 0x0D, 0x0A, 0

; ---------------------------------------------------------------------------
; MBR partition table
;
; In hard-disk emulation ("USB-HDD") mode - the default on most machines
; for USB sticks - many BIOSes only boot a disk whose partition table has
; an active (bootable) entry. Describe one partition covering the rest of
; the image, of type 0xDA ("non-filesystem data") so that operating
; systems leave it alone.
; ---------------------------------------------------------------------------
IMAGE_SECTORS equ 2880

times 0x1B8-($-$$) db 0    ; (assembly fails here if the code is too big)
    dd 0x42534958          ; disk signature
    dw 0
; Partition 1: active, LBA 1 .. IMAGE_SECTORS-1. CHS fields use the
; usual 255-head, 63-sector translation.
    db 0x80                ; status: active
    db 0, 2, 0             ; first sector CHS: head 0, sector 2, cylinder 0
    db 0xDA                ; type: non-filesystem data
    db (IMAGE_SECTORS - 1) / 63 % 255              ; last sector: head
    db (IMAGE_SECTORS - 1) % 63 + 1                ;   sector (cyl < 256)
    db (IMAGE_SECTORS - 1) / (63 * 255)            ;   cylinder
    dd 1                   ; first sector (LBA)
    dd IMAGE_SECTORS - 1   ; number of sectors
    times 3 * 16 db 0      ; partitions 2-4: unused

dw 0xaa55
