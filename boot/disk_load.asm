disk_load:
    push dx

    mov ah, 0x02 ; BIOS read sector function
    mov al, dh   ; Read DH sectors
    mov ch, 0x00 ; Cylinder 0
    mov dh, 0x00 ; Head 0
    mov cl, 0x02 ; Start reading from second sector (i.e. after the boot sector)

    int 0x13
    jc disk_error

    pop dx
    cmp dh, al   ; if AL (sectors read) != DH (sectors expected)
    jne disk_error
    ret

disk_error:
    mov bx, DISK_ERROR_MSG
    call print_string
    jmp $

DISK_ERROR_MSG db "Disk read error!", 0x0D, 0x0A, 0
