ASM = nasm
CC = gcc
LD = ld

# --- The 16-bit kernel, booted by the BIOS --------------------------------
#
# -O2 rather than -Os: at -Os GCC turns copy loops into movsb and similar
# string instructions, which only reach the first 64KB in 16-bit code
# (see tools/check16.sh). Floating point uses the x87 FPU only: SSE
# instructions fault until an OS enables them, and this kernel never does.
BIOS_CFLAGS = -m16 -march=i686 -mtune=generic -mfpmath=387 -mno-mmx -mno-sse -mno-sse2 \
              -ffreestanding -fno-PIE -fno-pic -fno-stack-protector -nostdlib \
              -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
              -fno-tree-loop-distribute-patterns -mstringop-strategy=loop \
              -O2 -falign-functions=1 -falign-jumps=1 -falign-loops=1 -falign-labels=1 \
              -Wall -Ikernel -DBIOS_KERNEL

BIOS_C_OBJS = $(patsubst %,build/bios/%.o,kernel bios screen font string num basic)
BIOS_OBJS = build/bios/kernel_entry.o build/bios/bios_string.o $(BIOS_C_OBJS)
HEADERS = $(wildcard kernel/*.h)

.PHONY: all test boot-test clean

all: os.img

build/bios:
	mkdir -p $@

build/bios/%.o: kernel/%.c $(HEADERS) Makefile | build/bios
	$(CC) $(BIOS_CFLAGS) -c $< -o $@

build/bios/%.o: kernel/%.asm Makefile | build/bios
	$(ASM) -f elf32 $< -o $@

build/kernel.bin: $(BIOS_OBJS) linker.ld tools/check16.sh
	sh tools/check16.sh $(BIOS_C_OBJS)
	$(LD) -m elf_i386 -T linker.ld --oformat binary $(BIOS_OBJS) -o $@

# The boot sector loads exactly as many sectors as the kernel occupies,
# so it is rebuilt whenever the kernel changes size.
build/boot.bin: boot/boot.asm boot/print.asm boot/disk_load.asm build/kernel.bin
	$(ASM) -f bin -DKERNEL_SECTORS=$$(( ($$(wc -c < build/kernel.bin) + 511) / 512 )) \
		boot/boot.asm -o $@

os.img: build/boot.bin build/kernel.bin
	cat build/boot.bin build/kernel.bin > os.img
	# Pad to 1.44MB floppy size
	truncate -s 1474560 os.img

test:
	tests/run_tests.sh

# Boot os.img headless in QEMU as floppy, hard disk and USB stick, and
# check a short BASIC session on screen.
boot-test: os.img
	tests/qemu_smoke.py --mode all

clean:
	rm -rf build tests/build os.img
