ASM = nasm
CC = gcc
LD = ld

CFLAGS = -m16 -ffreestanding -fno-PIE -fno-pic -fno-stack-protector -nostdlib -Os -Wall -Ikernel \
         -fno-builtin -fno-asynchronous-unwind-tables -fno-unwind-tables \
         -mgeneral-regs-only

all: os.img

# The boot sector loads exactly as many sectors as the kernel occupies,
# so it is rebuilt whenever the kernel changes size.
boot.bin: boot/boot.asm boot/print.asm boot/disk_load.asm kernel.bin
	$(ASM) -f bin -DKERNEL_SECTORS=$$(( ($$(wc -c < kernel.bin) + 511) / 512 )) \
		boot/boot.asm -o boot.bin

kernel_entry.o: kernel/kernel_entry.asm
	$(ASM) -f elf32 kernel/kernel_entry.asm -o kernel_entry.o

kernel.o: kernel/kernel.c kernel/bios.h kernel/basic.h kernel/string.h
	$(CC) $(CFLAGS) -c kernel/kernel.c -o kernel.o

bios.o: kernel/bios.c kernel/bios.h
	$(CC) $(CFLAGS) -c kernel/bios.c -o bios.o

string.o: kernel/string.c kernel/string.h
	$(CC) $(CFLAGS) -c kernel/string.c -o string.o

basic.o: kernel/basic.c kernel/basic.h kernel/bios.h kernel/string.h
	$(CC) $(CFLAGS) -c kernel/basic.c -o basic.o

kernel.bin: kernel_entry.o kernel.o bios.o string.o basic.o linker.ld
	$(LD) -m elf_i386 -T linker.ld --oformat binary kernel_entry.o kernel.o bios.o string.o basic.o -o kernel.bin

os.img: boot.bin kernel.bin
	cat boot.bin kernel.bin > os.img
	# Pad to 1.44MB floppy size
	dd if=/dev/zero of=os.img bs=512 seek=2880 count=0

test:
	tests/run_tests.sh

# Boot os.img headless in QEMU as floppy, hard disk and USB stick, and
# check a short BASIC session on screen.
boot-test: os.img
	tests/qemu_smoke.py --mode all

clean:
	rm -f *.bin *.o kernel/*.o boot/*.bin os.img
	rm -rf tests/build
