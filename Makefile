ASM = nasm
CC = gcc
LD = ld

CFLAGS = -m16 -ffreestanding -fno-PIE -fno-pic -fno-stack-protector -nostdlib -Os -Wall -Ikernel

all: os.img

boot.bin: boot/boot.asm boot/print.asm boot/disk_load.asm
	$(ASM) -f bin boot/boot.asm -o boot.bin

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

clean:
	rm -f *.bin *.o kernel/*.o boot/*.bin os.img
