ASM = nasm
CC = gcc
LD = ld

# Each machine (a kind of computer: machines/NAME/) is built into its own
# pair of kernels, on top of the shared code in core/: build/NAME/kernel.bin
# for the BIOS and build/NAME/BOOTX64.EFI for UEFI.
MACHINES = spectrum
MACHINE ?= spectrum

# --- The 16-bit kernel, booted by the BIOS --------------------------------
#
# -Os: the code has a 64KB segment of its own (see core/bios.ld). GCC
# likes to turn copy loops into movsb and similar string instructions,
# which only reach the first 64KB in 16-bit code; the stringop and
# loop-pattern options stop that, and tools/check16.sh checks that none
# slipped through. Floating point uses the x87 FPU only: SSE
# instructions fault until an OS enables them, and this kernel never does.
BIOS_CFLAGS = -m16 -march=i686 -mtune=generic -mfpmath=387 -mno-mmx -mno-sse -mno-sse2 \
              -ffreestanding -fno-PIE -fno-pic -fno-stack-protector -nostdlib \
              -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fno-tree-loop-distribute-patterns -mstringop-strategy=loop \
              -Os -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 \
              -Wall -Icore -DBIOS_KERNEL

# The shared code every machine is built with.
CORE = kernel fat storage string num
HEADERS = $(wildcard core/*.h core/uefi/*.h)

# --- The 64-bit kernel, a UEFI application ------------------------------
#
# Compiled as ordinary x86-64 ELF objects (position-independent, so the
# firmware can load it anywhere) and linked straight into a PE image by
# GNU ld's i386pep emulation. Only calls into the firmware use the
# Microsoft calling convention (EFIAPI in core/uefi/efi.h).
UEFI_CFLAGS = -ffreestanding -fpie -fno-stack-protector -fno-stack-check -mno-red-zone \
              -fshort-wchar -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fcf-protection=none -fno-ident -fno-tree-loop-distribute-patterns \
              -include core/uefi/hidden.h -O2 -Wall -Icore -DUEFI_KERNEL

.PHONY: all test boot-test run-listing clean FORCE

all: os.img

# $(call machine,NAME): the rules for one machine's two kernels.
define machine
$(1)_SRC := $$(basename $$(notdir $$(wildcard machines/$(1)/*.c)))
$(1)_HEADERS := $$(wildcard machines/$(1)/*.h)
$(1)_BIOS_C := $$(patsubst %,build/$(1)/bios/%.o,bios $$(CORE) $$($(1)_SRC))
$(1)_BIOS := build/$(1)/bios/kernel_entry.o build/$(1)/bios/bios_string.o $$($(1)_BIOS_C)
$(1)_UEFI := $$(patsubst %,build/$(1)/uefi/%.o,uefi $$(CORE) $$($(1)_SRC))

build/$(1)/bios build/$(1)/uefi:
	mkdir -p $$@

build/$(1)/bios/%.o: core/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/bios
	$$(CC) $$(BIOS_CFLAGS) -Imachines/$(1) -c $$< -o $$@

build/$(1)/bios/%.o: machines/$(1)/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/bios
	$$(CC) $$(BIOS_CFLAGS) -Imachines/$(1) -c $$< -o $$@

build/$(1)/bios/%.o: core/%.asm Makefile | build/$(1)/bios
	$$(ASM) -f elf32 $$< -o $$@

build/$(1)/uefi/%.o: core/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/uefi
	$$(CC) $$(UEFI_CFLAGS) -Imachines/$(1) -c $$< -o $$@

build/$(1)/uefi/%.o: core/uefi/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/uefi
	$$(CC) $$(UEFI_CFLAGS) -Imachines/$(1) -c $$< -o $$@

build/$(1)/uefi/%.o: machines/$(1)/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/uefi
	$$(CC) $$(UEFI_CFLAGS) -Imachines/$(1) -c $$< -o $$@

build/$(1)/kernel.bin: $$($(1)_BIOS) core/bios.ld tools/check16.sh
	sh tools/check16.sh $$($(1)_BIOS_C)
	@# --no-check-sections: the code is linked at offset 0 of its own
	@# segment, so its addresses overlap the data's (see core/bios.ld).
	$$(LD) -m elf_i386 -T core/bios.ld --no-check-sections --oformat binary $$($(1)_BIOS) -o $$@

# Subsystem 10: EFI application.
build/$(1)/BOOTX64.EFI: $$($(1)_UEFI)
	@if objdump -r $$($(1)_UEFI) | grep -q GOTPC; then \
		echo "GOT references in the UEFI build (see core/uefi/hidden.h)" >&2; exit 1; fi
	$$(LD) -m i386pep --subsystem 10 -e efi_main -nostdlib --no-insert-timestamp \
		$$($(1)_UEFI) -o $$@
endef

$(foreach m,$(MACHINES),$(eval $(call machine,$(m))))

KERNEL = build/$(MACHINE)/kernel.bin
EFI = build/$(MACHINE)/BOOTX64.EFI

# The boot sector loads exactly as many sectors as the kernel occupies,
# so it is rebuilt whenever the kernel changes size.
build/boot.bin: boot/boot.asm boot/print.asm boot/disk_load.asm $(KERNEL)
	$(ASM) -f bin -DKERNEL_SECTORS=$$(( ($$(wc -c < $(KERNEL)) + 511) / 512 )) \
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

os.img: build/boot.bin $(KERNEL) $(EFI) tools/mkimage.py build/listings $(LISTINGS)
	python3 tools/mkimage.py build/boot.bin $(KERNEL) $(EFI) $@ $(LISTINGS)

test:
	tests/run_tests.sh

# Boot os.img headless in QEMU as floppy, hard disk and USB stick, and
# check a short BASIC session on screen.
boot-test: os.img
	tests/qemu_smoke.py --mode all

# make run-listing LISTING=game.bas [MODE=uefi] [WAIT=20]: boot the image
# with that listing on it, LOAD and RUN it, and save the screen as game.png.
MODE ?= hdd
WAIT ?= 10
run-listing: build/boot.bin $(KERNEL) $(EFI)
	@test -n "$(LISTING)" || { echo "usage: make run-listing LISTING=game.bas" >&2; exit 2; }
	tools/run_listing.py $(LISTING) --mode $(MODE) --wait $(WAIT) --machine $(MACHINE)

clean:
	rm -rf build tests/build os.img
