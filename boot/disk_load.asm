; Load KERNEL_SECTORS sectors, starting at the sector after the boot
; sector (LBA 1), into memory at 0000:KERNEL_OFFSET.
;
; Reads one sector at a time. That's slower than one big read, but it
; works on every BIOS: multi-sector reads that cross a track (or a 64KB
; DMA boundary) are rejected by many real BIOSes, and the geometry of a
; USB stick or hard disk is not that of a floppy.
;
; Uses the INT 13h extensions (LBA addressing) when the drive supports
; them, as USB sticks and hard disks do. Otherwise (floppies) converts
; to cylinder/head/sector using the geometry the BIOS reports. Each read
; is retried a few times with a disk reset in between, since floppy
; reads commonly fail once while the motor spins up.

disk_load:
    ; --- Are INT 13h extensions available? (AH=41h) ---
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .chs_geometry
    cmp bx, 0xAA55
    jne .chs_geometry
    test cl, 1                  ; bit 0: extended disk access functions
    jz .chs_geometry
    mov byte [use_lba], 1
    jmp .read_all

.chs_geometry:
    ; --- Ask the BIOS for the drive geometry (AH=08h) ---
    ; Keeps the 1.44MB floppy defaults (18 sectors, 2 heads) on failure.
    mov ah, 0x08
    mov dl, [BOOT_DRIVE]
    xor di, di                  ; ES:DI = 0 works around buggy BIOSes
    push es
    int 0x13
    pop es
    jc .read_all
    and cx, 0x3F                ; CL bits 0-5: sectors per track
    jz .read_all
    mov [spt], cx
    movzx dx, dh                ; DH: highest head number
    inc dx
    mov [heads], dx

.read_all:
    mov cx, KERNEL_SECTORS
.next_sector:
    push cx
    call read_sector
    pop cx
    add word [dest], 512
    inc word [lba]
    loop .next_sector
    ret

; Read the sector at [lba] into 0000:[dest].
read_sector:
    mov byte [tries], 4
.retry:
    cmp byte [use_lba], 0
    je .chs

    ; LBA read (AH=42h) via the disk address packet below.
    mov ax, [dest]
    mov [dap_offset], ax
    mov ax, [lba]
    mov [dap_lba], ax
    mov si, dap
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    int 0x13
    jnc .done
    jmp .failed

.chs:
    ; LBA -> CHS:  sector = lba % spt + 1
    ;              head   = (lba / spt) % heads
    ;              cyl    = (lba / spt) / heads
    mov ax, [lba]
    xor dx, dx
    div word [spt]
    mov cl, dl
    inc cl                      ; CL bits 0-5 = sector (1-based)
    xor dx, dx
    div word [heads]            ; AX = cylinder, DX = head
    mov ch, al                  ; CH = cylinder bits 0-7
    shl ah, 6
    or cl, ah                   ; CL bits 6-7 = cylinder bits 8-9
    mov dh, dl                  ; DH = head
    mov dl, [BOOT_DRIVE]
    mov bx, [dest]              ; ES:BX = destination (ES = 0)
    mov ax, 0x0201              ; AH=02h read, AL=1 sector
    int 0x13
    jnc .done

.failed:
    dec byte [tries]
    jz disk_error
    xor ah, ah                  ; AH=00h: reset the disk system
    mov dl, [BOOT_DRIVE]
    int 0x13
    jmp .retry
.done:
    ret

disk_error:
    mov bx, DISK_ERROR_MSG
    call print_string
    jmp $

DISK_ERROR_MSG db "Disk read error!", 0x0D, 0x0A, 0

use_lba db 0
tries   db 0
lba     dw 1                    ; next sector to read (0 = boot sector)
dest    dw KERNEL_OFFSET        ; where it goes
spt     dw 18                   ; sectors per track (CHS only)
heads   dw 2                    ; number of heads (CHS only)

; Disk address packet for INT 13h AH=42h
dap:
    db 0x10                     ; packet size
    db 0
    dw 1                        ; sectors to read
dap_offset:
    dw 0                        ; destination offset
    dw 0                        ; destination segment
dap_lba:
    dq 0                        ; starting LBA (we only use the low word)
