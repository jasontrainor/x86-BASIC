#ifndef MEMORY_H
#define MEMORY_H

/*
 * The Spectrum's 64KB memory map, for PEEK and POKE (machines/spectrum/memory.c).
 *
 *   0-16383      "ROM": all 0, except the character set at 15616-16383
 *                (this project's own font, where the Spectrum keeps its
 *                own); POKEs are ignored
 *   16384-22527  the screen's pixels and 22528-23295 its attributes, laid
 *                out as on the Spectrum: they are the real screen
 *   23552-23733  the system variables BASIC programs commonly use (see
 *                memory.c), live where it matters: CHARS, UDG, SEED,
 *                FRAMES, SCR_CT, ATTR_P, COORDS, LAST_K ...
 *   the rest     ordinary memory; the user-defined graphics are at 65368
 *
 * BASIC's own program and variables are kept elsewhere, so PEEKing the
 * program area doesn't find them, and machine code can't be run.
 */

#define UDG_START 65368       // where the UDGs are when switched on
#define SYS_CHARS 23606
#define SYS_UDG   23675
#define SYS_FLAGS2 23658

void mem_reset(void);                 // as when switched on
int mem_peek(unsigned int addr);      // addr is taken modulo 65536
int mem_peek16(unsigned int addr);
void mem_poke(unsigned int addr, int v);

#endif
