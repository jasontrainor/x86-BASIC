/*
 * SYS: the firmware routines BASIC programs call, done here instead (the
 * machine code of a program isn't run: there is no 6502 here).
 *
 * As there, SYS loads the processor's registers from 780-783 (A, X, Y,
 * the status: bit 0 the carry) before the call, and stores them back
 * after it.
 *
 *   64738 ($FCE2)  reset: the machine starts again (the program is lost)
 *   65520 ($FFF0)  PLOT: with the carry clear, the cursor to row X,
 *                  column Y; set, where it is into X and Y
 *   58640 ($E510)  the cursor to row PEEK(214), column PEEK(211) (what
 *   58732 ($E56C)  PLOT does, after storing X and Y there)
 *   65490 ($FFD2)  CHROUT: print the character A
 *   65508 ($FFE4)  GETIN: the next key into A (0 if none)
 *   65511 ($FFE7)  CLALL, 65484 ($FFCC) CLRCHN: nothing to do here
 *   65517 ($FFED)  SCREEN: 40 columns, 25 rows into X and Y
 *   58692 ($E544)  clear the screen
 *   65439 ($FF9F)  SCNKEY: read the keyboard
 */
#include "c64.h"

int c64_sys(unsigned int addr) {
    int a = c64_peek(780), x = c64_peek(781), y = c64_peek(782), p = c64_peek(783);
    switch (addr) {
    case 64738:
        return 2;
    case 65520:
        if (p & 1) {
            x = c64_peek(C64_TBLX);
            y = c64_peek(C64_PNTR);
            if (y >= 40) y -= 40;
        } else {
            c64_set_cursor(x, y);
        }
        break;
    case 58640:
    case 58732:
        c64_set_cursor(c64_peek(C64_TBLX), c64_peek(C64_PNTR));
        break;
    case 65490: c64_chrout(a); break;
    case 65508: {
        int k = c64_get_key();
        a = k < 0 ? 0 : k;
        break;
    }
    case 65511: case 65484: case 65439: c64_tick(); break;
    case 65517: x = 40, y = 25; break;
    case 58692: c64_chrout(147); break;
    default:
        return 0;
    }
    c64_poke(780, a);
    c64_poke(781, x);
    c64_poke(782, y);
    c64_poke(783, p);
    return 1;
}
