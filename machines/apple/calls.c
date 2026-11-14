/*
 * CALL: the firmware routines that BASIC programs call for the screen
 * and the speaker, by address (as BASIC writes them: -936 is $FC58).
 * Each is done here as the routine behaves; there's no 6502 and no ROM,
 * so a CALL anywhere else (machine code a program POKEd in, say) can't
 * be run, and the BASIC says so.
 *
 * One piece of machine code is known here, though, since so many
 * listings POKE it in for their sound: the little tone routine (see
 * tone_routine) is recognised by its bytes, wherever it was put, and its
 * tone played.
 */
#include "apple.h"
#include "platform.h"

/*
 * The tone routine listings POKE in (usually at 770, with the pitch at
 * 768 and the length at 769: POKE 768,P: POKE 769,D: CALL 770):
 *
 *   loop  LDA $C030      click the speaker
 *   wait  DEY
 *         BNE count
 *         DEC length     every 256 times round: the end yet?
 *         BEQ done
 *   count DEX
 *         BNE wait
 *         LDX pitch      P times round between clicks
 *         JMP loop
 *   done  RTS
 *
 * Its timing, at the 6502's 1,020,484 cycles a second: 10 cycles a time
 * round, and 10 more a click, so the clicks are 10P+10 cycles apart (two
 * of them a cycle of the tone), and it goes round 256 times for each of
 * the D the length counts down. (0 counts as 256 for both.) Returns 1 if
 * the code at addr is the routine, having played its tone.
 */
static int tone_routine(int addr) {
    static const unsigned char code[21] = {
        0xAD, 0x30, 0xC0, 0x88, 0xD0, 0x05, 0xCE, 0, 0, 0xF0, 0x09,
        0xCA, 0xD0, 0xF5, 0xAE, 0, 0, 0x4C, 0, 0, 0x60,
    };
    for (int i = 0; i < 21; i++) {
        if (i == 7 || i == 8 || i == 15 || i == 16 || i == 18 || i == 19) continue;
        if (apple_peek(addr + i) != code[i]) return 0;
    }
    int jump = apple_peek(addr + 18) | apple_peek(addr + 19) << 8;
    if (jump != addr) return 0;           // (JMP loop: back to its start)
    int length = apple_peek(addr + 7) | apple_peek(addr + 8) << 8;
    int pitch = apple_peek(addr + 15) | apple_peek(addr + 16) << 8;
    long p = apple_peek(pitch), d = apple_peek(length);
    if (!p) p = 256;
    if (!d) d = 256;
    long freq = 1020484L / (2 * (10 * p + 10));
    long cycles = d * (256 * 10 + 7) + d * 256 * 10 / p;
    plat_tone((int)freq, (int)(cycles * 1000 / 1020484L));
    apple_poke(length, 0);                // (counted down to 0, as it leaves it)
    return 1;
}

int apple_call(int addr) {
    if (apple_model == APPLE_I) return 0;
    if (tone_routine(addr & 0xFFFF)) return 1;
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
