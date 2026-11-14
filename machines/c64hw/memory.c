/*
 * The BASIC V2 machine's memory, for PEEK and POKE and for the picture,
 * which is drawn from it (vic.c):
 *
 *   $0000-$0001  the processor port: bits 0-2 of 1 bank the ROMs and I/O
 *                in and out (LORAM, HIRAM, CHAREN); $37 at power on
 *   $0002-$9FFF  RAM: zero page and the firmware's variables, the screen
 *                at $0400, BASIC's program from $0801
 *   $A000-$BFFF  BASIC "ROM" (LORAM and HIRAM): reads as 0, there being
 *                no ROM here; RAM underneath, which writes always reach
 *   $C000-$CFFF  RAM
 *   $D000-$DFFF  I/O (CHAREN set): the VIC-II $D000, the SID $D400,
 *                colour RAM $D800 (4 bits a byte), CIA 1 $DC00, CIA 2
 *                $DD00; or (CHAREN clear) the character ROM, which is
 *                this project's characters; or RAM (LORAM and HIRAM clear)
 *   $E000-$FFFF  KERNAL "ROM" (HIRAM): reads as 0; RAM underneath
 */
#include "c64.h"
#include "string.h"
#include "mem.h"

HIGH_MEM static unsigned char ram_mem[65536];
HIGH_MEM static unsigned char colour_mem[1024];

// (Through functions: see core/mem.h.)
__attribute__((noinline, noclone)) static unsigned char *ram(void) { return ram_mem; }
__attribute__((noinline, noclone)) static unsigned char *colours(void) { return colour_mem; }


int c64_ram(unsigned int addr) { return ram()[addr & 0xFFFF]; }

void c64_set_ram(unsigned int addr, int value) {
    addr &= 0xFFFF;
    ram()[addr] = value;
    vic_mem_written(addr);
}

int c64_colour(int offset) { return colours()[offset & 1023] & 15; }

void c64_set_colour(int offset, int value) {
    colours()[offset & 1023] = value & 15;
    vic_colour_written(offset & 1023);
}

int c64_char_rom(int offset) {
    int set = offset >> 11 & 1, code = offset >> 3 & 255;
    int b = c64_chars[set][code & 127][offset & 7];
    return code & 128 ? b ^ 0xFF : b;
}

// What's at $A000-$FFFF for the processor: 'R' RAM, 'O' ROM, 'I' I/O,
// 'C' the character ROM.
static int mapped(unsigned int addr) {
    int port = ram()[C64_PORT] | ~ram()[C64_PORT_DDR];   // (inputs read as 1)
    int lo = port & 1, hi = port & 2, charen = port & 4;
    if (addr >= 0xA000 && addr < 0xC000) return lo && hi ? 'O' : 'R';
    if (addr >= 0xE000) return hi ? 'O' : 'R';
    if (addr >= 0xD000 && addr < 0xE000) {
        if (!lo && !hi) return 'R';
        return charen ? 'I' : 'C';
    }
    return 'R';
}

static int io_read(unsigned int addr) {
    if (addr < 0xD400) return vic_read(addr & 63);
    if (addr < 0xD800) return sid_read(addr & 31);
    if (addr < 0xDC00) return c64_colour(addr - 0xD800);
    if (addr < 0xDE00) return cia_read(addr >> 8 & 1, addr & 15);
    return 0;
}

static void io_write(unsigned int addr, int value) {
    if (addr < 0xD400) vic_write(addr & 63, value);
    else if (addr < 0xD800) sid_write(addr & 31, value);
    else if (addr < 0xDC00) c64_set_colour(addr - 0xD800, value);
    else if (addr < 0xDE00) cia_write(addr >> 8 & 1, addr & 15, value);
}

int c64_peek(unsigned int addr) {
    addr &= 0xFFFF;
    if (addr == C64_LSTX || addr == C64_SHFLAG) cia_tick();   // (the keys down now)
    if (addr >= 0xA000)
        switch (mapped(addr)) {
        case 'O': return 0;
        case 'I': return io_read(addr);
        case 'C': return c64_char_rom(addr - 0xD000);
        }
    return ram()[addr];
}

void c64_poke(unsigned int addr, int value) {
    addr &= 0xFFFF;
    value &= 0xFF;
    if (addr >= 0xD000 && addr < 0xE000 && mapped(addr) == 'I') io_write(addr, value);
    else c64_set_ram(addr, value);
}

// The 16KB bank the VIC sees: CIA 2's port A, bits 0-1, inverted.
int c64_vic_bank(void) {
    return (3 - (cia_port_a2() & 3)) * 0x4000;
}

void c64_warm_start(void) {
    unsigned char *m = ram();
    m[C64_PORT_DDR] = 0x2F;
    m[C64_PORT] = 0x37;
    m[C64_HIBASE] = 0x04;
    m[C64_COLOR] = 14;
    m[C64_RVS] = 0;
    sid_init();
    cia_init();
    vic_init();
    screen_init();
}

void c64_init(void) {
    memset(ram(), 0, 65536);
    memset(colours(), 14, 1024);
    sid_init();
    cia_init();
    cia_config();
    unsigned char *m = ram();
    m[C64_PORT_DDR] = 0x2F;
    m[C64_PORT] = 0x37;
    m[C64_TXTTAB] = 0x01;                 // BASIC from $0801 to $A000
    m[C64_TXTTAB + 1] = 0x08;
    m[C64_MEMSIZ] = 0x00;
    m[C64_MEMSIZ + 1] = 0xA0;
    m[C64_HIBASE] = 0x04;
    m[C64_XMAX] = 10;
    m[C64_COLOR] = 14;                    // light blue
    m[C64_LSTX] = 64;
    m[0x0328] = 0xED;                     // the STOP key's vector ($F6ED)
    m[0x0329] = 0xF6;
    m[0x0318] = 0x47;                     // the RESTORE key's ($FE47)
    m[0x0319] = 0xFE;
    vic_init();
    screen_init();
    keys_init();
}
