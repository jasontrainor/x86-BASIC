ASM = nasm
CC = gcc
LD = ld

# --- The 16-bit kernel, booted by the BIOS --------------------------------
#
# -Os: the code has a 64KB segment of its own (see linker.ld). GCC likes to turn copy loops into movsb and similar string
# instructions, which only reach the first 64KB in 16-bit code; the
# stringop and loop-pattern options stop that, and tools/check16.sh
# checks that none slipped through. Floating point uses the x87 FPU only: SSE
# instructions fault until an OS enables them, and this kernel never does.
BIOS_CFLAGS = -m16 -march=i686 -mtune=generic -mfpmath=387 -mno-mmx -mno-sse -mno-sse2 \
              -ffreestanding -fno-PIE -fno-pic -fno-stack-protector -nostdlib \
              -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fno-tree-loop-distribute-patterns -mstringop-strategy=loop \
              -Os -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 \
              -Wall -Ikernel -DBIOS_KERNEL

SHARED = kernel screen font editor storage fat string num memory basic
BIOS_C_OBJS = $(patsubst %,build/bios/%.o,bios $(SHARED))
BIOS_OBJS = build/bios/kernel_entry.o build/bios/bios_string.o $(BIOS_C_OBJS)
HEADERS = $(wildcard kernel/*.h kernel/uefi/*.h)

# --- The 64-bit kernel, a UEFI application ------------------------------
#
# Compiled as ordinary x86-64 ELF objects (position-independent, so the
# firmware can load it anywhere) and linked straight into a PE image by
# GNU ld's i386pep emulation. Only calls into the firmware use the
# Microsoft calling convention (EFIAPI in kernel/uefi/efi.h).
UEFI_CFLAGS = -ffreestanding -fpie -fno-stack-protector -fno-stack-check -mno-red-zone \
              -fshort-wchar -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fcf-protection=none -fno-ident -fno-tree-loop-distribute-patterns \
              -include kernel/uefi/hidden.h -O2 -Wall -Ikernel -DUEFI_KERNEL
UEFI_OBJS = $(patsubst %,build/uefi/%.o,uefi $(SHARED))

.PHONY: all test boot-test clean FORCE

all: os.img

build/bios:
	mkdir -p $@

build/bios/%.o: kernel/%.c $(HEADERS) Makefile | build/bios
	$(CC) $(BIOS_CFLAGS) -c $< -o $@

build/bios/%.o: kernel/%.asm Makefile | build/bios
	$(ASM) -f elf32 $< -o $@

build/uefi:
	mkdir -p $@

build/uefi/%.o: kernel/%.c $(HEADERS) Makefile | build/uefi
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

build/uefi/uefi.o: kernel/uefi/uefi.c $(HEADERS) Makefile | build/uefi
	$(CC) $(UEFI_CFLAGS) -c $< -o $@

# Subsystem 10: EFI application.
build/BOOTX64.EFI: $(UEFI_OBJS)
	@if objdump -r $(UEFI_OBJS) | grep -q GOTPC; then \
		echo "GOT references in the UEFI build (see kernel/uefi/hidden.h)" >&2; exit 1; fi
	$(LD) -m i386pep --subsystem 10 -e efi_main -nostdlib --no-insert-timestamp \
		$(UEFI_OBJS) -o $@

build/kernel.bin: $(BIOS_OBJS) linker.ld tools/check16.sh
	sh tools/check16.sh $(BIOS_C_OBJS)
	@# --no-check-sections: the code is linked at offset 0 of its own
	@# segment, so its addresses overlap the data's (see linker.ld).
	$(LD) -m elf_i386 -T linker.ld --no-check-sections --oformat binary $(BIOS_OBJS) -o $@

# The boot sector loads exactly as many sectors as the kernel occupies,
# so it is rebuilt whenever the kernel changes size.
build/boot.bin: boot/boot.asm boot/print.asm boot/disk_load.asm build/kernel.bin
	$(ASM) -f bin -DKERNEL_SECTORS=$$(( ($$(wc -c < build/kernel.bin) + 511) / 512 )) \
		boot/boot.asm -o $@

# One image for both: BIOS boot code and kernel, plus an EFI system
# partition with the UEFI kernel (see tools/mkimage.py).
#
# make LISTINGS="game.bas other.bas" also puts those files (text listings,
# with DOS 8.3 names) on the FAT partition, for LOAD "GAME.BAS".
LISTINGS ?=

# (Remembers LISTINGS, so that changing it rebuilds os.img.)
build/listings: FORCE
	@mkdir -p build
	@echo '$(LISTINGS)' | cmp -s - $@ || echo '$(LISTINGS)' > $@

os.img: build/boot.bin build/kernel.bin build/BOOTX64.EFI tools/mkimage.py build/listings $(LISTINGS)
	python3 tools/mkimage.py build/boot.bin build/kernel.bin build/BOOTX64.EFI $@ $(LISTINGS)

test:
	tests/run_tests.sh

# Boot os.img headless in QEMU as floppy, hard disk and USB stick, and
# check a short BASIC session on screen.
boot-test: os.img
	tests/qemu_smoke.py --mode all

clean:
	rm -rf build tests/build os.img
