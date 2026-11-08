; memcpy, memmove and memset for the 16-bit BIOS kernel.
;
; GCC's -m16 code uses 32-bit addresses throughout, so data can live
; above 64KB (see core/bios.ld). But when GCC compiles a copy loop to a
; string instruction such as movsb, the assembler leaves it using the
; 16-bit SI/DI registers, which only reach the first 64KB. So these are
; written here with the a32 prefix (ESI/EDI/ECX), and the Makefile
; rejects any object file that contains a 16-bit-addressed instruction.
;
; cdecl, called with 32-bit calls (GCC -m16); EBX, ESI, EDI and EBP are
; preserved.

[bits 16]
section .text

global memcpy, memmove, memset

; void *memcpy(void *dest, const void *src, size_t n)
memcpy:
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
    mov eax, edi
    cld
    a32 rep movsb
    pop edi
    pop esi
    retd

; void *memmove(void *dest, const void *src, size_t n)
memmove:
    push esi
    push edi
    mov edi, [esp + 12]
    mov esi, [esp + 16]
    mov ecx, [esp + 20]
    mov eax, edi
    cmp edi, esi
    jbe .forward              ; dest below src: copying upwards is safe
    lea esi, [esi + ecx - 1]  ; otherwise copy downwards from the end
    lea edi, [edi + ecx - 1]
    std
    a32 rep movsb
    cld                       ; GCC assumes the direction flag is clear
    jmp .done
.forward:
    cld
    a32 rep movsb
.done:
    pop edi
    pop esi
    retd

; void *memset(void *dest, int c, size_t n)
memset:
    push edi
    mov edi, [esp + 8]
    mov eax, [esp + 12]
    mov ecx, [esp + 16]
    mov edx, edi
    cld
    a32 rep stosb
    mov eax, edx
    pop edi
    retd
