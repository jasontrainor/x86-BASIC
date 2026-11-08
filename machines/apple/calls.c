/*
 * CALL: the firmware routines that BASIC programs call for the screen
 * and the speaker, by address (as BASIC writes them: -936 is $FC58).
 * Each is done here as the routine behaves; there's no 6502 and no ROM,
 * so a CALL anywhere else (machine code a program POKEd in, say) can't
 * be run, and the BASIC says so.
 */
#include "apple.h"
#include "platform.h"

int apple_call(int addr) {
    if (apple_model == APPLE_I) return 0;
    switch (addr & 0xFFFF) {
    case 0xFC58: apple_home(); return 1;                  // -936  HOME
    case 0xFC42: apple_clreop(); return 1;                // -958  clear to end of window
    case 0xFC9C: apple_clreol(); return 1;                // -868  clear to end of line
    case 0xFC66: apple_cout(10); return 1;                // -922  line feed
    case 0xFC70: {                                        // -912  scroll up a line
        int ch = apple_peek(ZP_CH), cv = apple_peek(ZP_CV);
        apple_vtab(apple_peek(ZP_WNDBTM) - 1);
        apple_cout(10);
        apple_poke(ZP_CH, ch);
        apple_poke(ZP_CV, cv);
        return 1;
    }
    case 0xFF3A: case 0xFBDD: case 0xFBE4:                // -198, -1059, -1052  bell
        apple_bell();
        return 1;
    case 0xFC10: apple_cout(8); return 1;                 // -1008 back a character
    case 0xFC1A:                                          // -998  up a line
        if (apple_peek(ZP_CV) > apple_peek(ZP_WNDTOP)) apple_poke(ZP_CV, apple_peek(ZP_CV) - 1);
        return 1;
    case 0xFBF4: {                                        // -1036 forward a character
        int ch = apple_peek(ZP_CH) + 1;
        if (ch >= apple_peek(ZP_WNDWDTH)) {
            apple_poke(ZP_CH, 0);
            apple_cout(10);
        } else {
            apple_poke(ZP_CH, ch);
        }
        return 1;
    }
    case 0xFE80: apple_setinv(1); return 1;               // -384  inverse
    case 0xFE84: apple_setinv(0); return 1;               // -380  normal
    case 0xF832: case 0xF836: {                           // -1998, -1994  clear lores
        int c = apple_get_color();                        // (all 48 rows, or the top 40)
        apple_color(0);
        for (int y = 0; y < ((addr & 0xFFFF) == 0xF832 ? 48 : 40); y++) apple_hlin(0, 39, y);
        apple_color(c);
        return 1;
    }
    case 0xFB40: apple_gr(); return 1;                    // -1216 graphics, mixed
    case 0xFB39: case 0xFB2F: apple_text(); return 1;     // -1223 text; -1233 (and INIT)
    case 0xFCA8: plat_tone(0, 10); return 1;              // -856  wait a little
    case 0xFD0C: case 0xFD1B: apple_wait_key(); return 1; // -756, -741  wait for a key
    }
    return 0;
}
