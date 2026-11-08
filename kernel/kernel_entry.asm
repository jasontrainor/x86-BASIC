[bits 16]
[extern main]
[extern __stack_top]       ; defined in linker.ld, with the memory map

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
    mov esp, __stack_top
    xor ebp, ebp
    mov [boot_drive], dl   ; the boot sector passes the BIOS drive number
    call dword enter_unreal
    fninit                 ; x87 FPU in its default state (BASIC's numbers)
    sti
    cld                    ; GCC assumes the direction flag is clear

    call main
    jmp $

; ---------------------------------------------------------------------------
; enter_unreal: give DS, ES, FS, GS and SS a 4GB limit ("unreal mode").
;
; In real mode a segment is normally limited to 64KB, so with every
; segment register 0 (as the C code needs), only the first 64KB of memory
; can be used. But the CPU keeps each segment's limit in a hidden cache
; that is only reloaded in protected mode. So: switch to protected mode,
; load the segment registers from a descriptor with a 4GB limit, and
; switch straight back. The registers are 0 again, real-mode BIOS calls
; still work, and the C code can now use 32-bit addresses up to 4GB.
;
; SS needs the 4GB limit too, even though the stack itself stays below
; 64KB: GCC uses EBP as an ordinary register, and an address formed with
; EBP uses SS, so a pointer into high memory held in EBP goes through SS.
; The descriptor is 16-bit (B=0), so the stack still uses SP. Code (CS)
; is left alone: it stays below 64KB.
;
; A BIOS routine that switches to protected mode itself could leave the
; limits at 64KB again, so this is called again after each BIOS call.
; Callable from C (GCC -m16 uses 32-bit calls and returns); preserves
; all registers.
; ---------------------------------------------------------------------------
global enter_unreal
enter_unreal:
    pushfd
    push eax
    cli
    o32 lgdt [gdt_descriptor]
    mov eax, cr0
    or al, 1
    mov cr0, eax           ; protected mode
    jmp short .pm          ; (serialise, as the CPU manuals ask)
.pm:
    mov ax, gdt_data - gdt
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    mov eax, cr0
    and al, 0xFE
    mov cr0, eax           ; and back to real mode
    jmp short .rm
.rm:
    xor ax, ax             ; bases back to 0; the 4GB limits stay cached
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    pop eax
    popfd                  ; restores the interrupt flag
    retd                   ; 32-bit return, as GCC -m16 code expects

align 8
gdt:
    dq 0                   ; null descriptor
gdt_data:                  ; base 0, limit 4GB (4KB units), writable data
    dw 0xFFFF              ; limit 15:0
    dw 0                   ; base 15:0
    db 0                   ; base 23:16
    db 0x92                ; present, ring 0, data, writable
    db 0x8F                ; 4KB granularity, 16-bit, limit 19:16 = 0xF
    db 0                   ; base 31:24
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt - 1
    dd gdt

; BIOS drive number the system was booted from (0x00 floppy, 0x80+ disk).
global boot_drive
boot_drive: db 0
