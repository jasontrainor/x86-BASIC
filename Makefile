ASM = nasm
CC = gcc
LD = ld

# Each machine (a kind of computer: machines/NAME/) is built into its own
# pair of kernels, on top of the shared code in core/: build/NAME/kernel.bin
# for the BIOS and build/NAME/BOOTX64.EFI for UEFI. The chooser
# (machines/chooser/) is built the same way.
MACHINES = spectrum a2int a1basic
# (make run-listing's)
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
CORE = kernel fat storage string num font
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

.PHONY: all test boot-test run-listing write clean FORCE

all: os.img

# $(call machine,NAME): the rules for one machine's two kernels. A
# machine can also be built with the code in other directories under
# machines/ that its file "uses" names (machines/apple/, say).
define machine
$(1)_DIRS := machines/$(1) $$(addprefix machines/,$$(shell cat machines/$(1)/uses 2>/dev/null))
$(1)_SRC := $$(basename $$(notdir $$(wildcard $$(addsuffix /*.c,$$($(1)_DIRS)))))
$(1)_HEADERS := $$(wildcard $$(addsuffix /*.h,$$($(1)_DIRS)))
$(1)_INC := $$(addprefix -I,$$($(1)_DIRS))
$(1)_BIOS_C := $$(patsubst %,build/$(1)/bios/%.o,bios $$(CORE) $$($(1)_SRC))
$(1)_BIOS := build/$(1)/bios/kernel_entry.o build/$(1)/bios/bios_string.o $$($(1)_BIOS_C)
$(1)_UEFI := $$(patsubst %,build/$(1)/uefi/%.o,uefi $$(CORE) $$($(1)_SRC))

build/$(1)/bios build/$(1)/uefi:
	mkdir -p $$@

build/$(1)/bios/%.o: core/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/bios
	$$(CC) $$(BIOS_CFLAGS) $$($(1)_INC) -c $$< -o $$@

$$(foreach d,$$($(1)_DIRS),$$(eval $$(call machine_dir,$(1),$$(d))))

build/$(1)/bios/%.o: core/%.asm Makefile | build/$(1)/bios
	$$(ASM) -f elf32 $$< -o $$@

build/$(1)/uefi/%.o: core/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/uefi
	$$(CC) $$(UEFI_CFLAGS) $$($(1)_INC) -c $$< -o $$@

build/$(1)/uefi/%.o: core/uefi/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/uefi
	$$(CC) $$(UEFI_CFLAGS) $$($(1)_INC) -c $$< -o $$@


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

# $(call machine_dir,NAME,DIR): compiling machine NAME's code in DIR.
define machine_dir
build/$(1)/bios/%.o: $(2)/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/bios
	$$(CC) $$(BIOS_CFLAGS) $$($(1)_INC) -c $$< -o $$@

build/$(1)/uefi/%.o: $(2)/%.c $$(HEADERS) $$($(1)_HEADERS) Makefile | build/$(1)/uefi
	$$(CC) $$(UEFI_CFLAGS) $$($(1)_INC) -c $$< -o $$@
endef

# Machines that are built but not put on images unless asked for
# (IMAGE_MACHINES): a2video tests the Apple screen and keyboard.
TEST_MACHINES = a2video

$(foreach m,$(MACHINES) $(TEST_MACHINES) chooser,$(eval $(call machine,$(m))))

# --- Disk images -----------------------------------------------------------
#
# The boot sector (BIOS) and BOOTX64.EFI (UEFI) start the chooser
# (machines/chooser/), which starts a machine: X86BASIC/NAME.SYS or
# NAME.EFI on the FAT partition. With one machine on the image it starts
# straight away; with more, there's a menu (see tools/mkimage.py).
#
#   make                  os.img, with IMAGE_MACHINES (all of them)
#   make image-NAME       build/images/NAME.img, with just that machine
#   make IMAGE_MACHINES="spectrum" DEFAULT=spectrum MENU=always
#
# make LISTINGS="game.bas other.bas" also puts those files (listings, or
# .SAV files, with DOS 8.3 names) in the first machine's folder, for
# LOAD "GAME.BAS".
IMAGE_MACHINES ?= $(MACHINES)
LISTINGS ?=
DEFAULT ?=
MENU ?= auto
TIMEOUT ?= 5
IMAGE_OPTIONS = --menu $(MENU) --timeout $(TIMEOUT) $(if $(DEFAULT),--default $(DEFAULT))

CHOOSER = build/chooser/kernel.bin build/chooser/BOOTX64.EFI
kernels = $(foreach m,$(1),build/$(m)/kernel.bin build/$(m)/BOOTX64.EFI machines/$(m)/title.txt)

# The boot sector loads exactly as many sectors as the chooser occupies,
# so it is rebuilt whenever the chooser changes size.
build/boot.bin: boot/boot.asm boot/print.asm boot/disk_load.asm build/chooser/kernel.bin
	$(ASM) -f bin -DKERNEL_SECTORS=$$(( ($$(wc -c < build/chooser/kernel.bin) + 511) / 512 )) \
		boot/boot.asm -o $@

# (Remembers what os.img is made of, so that changing it rebuilds it.)
build/image-options: FORCE
	@mkdir -p build
	@echo '$(IMAGE_MACHINES) $(IMAGE_OPTIONS) $(LISTINGS)' | cmp -s - $@ || \
		echo '$(IMAGE_MACHINES) $(IMAGE_OPTIONS) $(LISTINGS)' > $@

os.img: build/boot.bin $(CHOOSER) $(call kernels,$(IMAGE_MACHINES)) tools/mkimage.py \
		build/image-options $(LISTINGS)
	python3 tools/mkimage.py $@ --machines $(subst $() ,$(comma),$(strip $(IMAGE_MACHINES))) \
		$(IMAGE_OPTIONS) $(LISTINGS)

comma = ,

build/images/%.img: build/boot.bin $(CHOOSER) $(call kernels,%) tools/mkimage.py
	@mkdir -p build/images
	python3 tools/mkimage.py $@ --machines $*

.PRECIOUS: build/images/%.img
image-%: build/images/%.img
	@echo "$<"

# make write DEVICE=/dev/sdX [IMAGE=os.img]: write an image to a USB stick
# (it asks first, and refuses disks that aren't removable or are in use).
IMAGE ?= os.img
write: $(IMAGE)
	@test -n "$(DEVICE)" || { echo "usage: make write DEVICE=/dev/sdX [IMAGE=os.img]" >&2; exit 2; }
	tools/write_usb.py $(IMAGE) $(DEVICE)

test:
	tests/run_tests.sh

# Boot os.img headless in QEMU as floppy, hard disk and USB stick, and
# check a short BASIC session on screen; then the chooser's menu, and the
# Apple screen (with the a2video test machine).
boot-test: os.img build/boot.bin $(CHOOSER) $(call kernels,$(MACHINES) $(TEST_MACHINES))
	tests/qemu_smoke.py --mode all
	tests/qemu_menu.py
	tests/qemu_apple.py

# make run-listing LISTING=game.bas [MODE=uefi] [WAIT=20]: boot the image
# with that listing on it, LOAD and RUN it, and save the screen as game.png.
MODE ?= hdd
WAIT ?= 10
run-listing: build/boot.bin $(CHOOSER) $(call kernels,$(MACHINE))
	@test -n "$(LISTING)" || { echo "usage: make run-listing LISTING=game.bas" >&2; exit 2; }
	tools/run_listing.py $(LISTING) --mode $(MODE) --wait $(WAIT) --machine $(MACHINE)

clean:
	rm -rf build tests/build os.img
