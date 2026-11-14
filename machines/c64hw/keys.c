/*
 * The keyboard and the clock.
 *
 * The PC's keys are the machine's: letters are its unshifted letters (A,
 * shown in capitals) and Shift with a letter its shifted one (a graphics
 * character, or a capital in the lower case set), as with its own Shift
 * Lock: Caps Lock types graphics. And:
 *
 *   Esc            RUN/STOP          Home, Shift+Home   HOME, CLR
 *   Backspace      DEL               Shift+Backspace, End   INST
 *   the arrows     the cursor keys   Shift+Return       Shift+RETURN
 *   right Alt+1-8  CTRL+1-8 (the colours black ... yellow; Ctrl+1-8
 *                  too, where the PC says so: UEFI)
 *   left Alt+1-8   C=+1-8 (orange ... light grey)
 *   Alt+9, Alt+0   RVS ON, RVS OFF (CTRL+9, CTRL+0)
 *   \ ` ^ ~        the pound sign, the left arrow, the up arrow, pi
 *
 * Typed keys go into the key buffer (631-640, the count at 198) as the
 * firmware's keyboard scan puts them there; programs empty it (POKE
 * 198,0) and type into it (POKE 631,13:POKE 198,1) as there.
 *
 * The clock at 160-162 counts 60ths of a second (TI), as the firmware's
 * interrupt counts them.
 */
#include "c64.h"
#include "platform.h"

static unsigned long last_ms;             // (the clock: the time last counted)
static int stop_flag;

static const unsigned char ctrl_colours[10] = { 146, 144, 5, 28, 159, 156, 30, 31, 158, 18 };
static const unsigned char cbm_colours[10] = { 0, 129, 149, 150, 151, 152, 153, 154, 155, 0 };

// A PC key as the machine's (PETSCII), or -1.
static int petscii_key(int k) {
    int shift = plat_buttons() & 4;
    if (k >= 'a' && k <= 'z') return k - 32;
    if (k >= 'A' && k <= 'Z') return k + 128;
    if (k >= KEY_ALT_DIGIT && k < KEY_ALT_DIGIT + 10) {
        int d = k - KEY_ALT_DIGIT;
        return (plat_buttons() & 2) ? ctrl_colours[d] : cbm_colours[d] ? cbm_colours[d] : -1;
    }
    if (k >= KEY_CTRL_DIGIT && k < KEY_CTRL_DIGIT + 10) return ctrl_colours[k - KEY_CTRL_DIGIT];
    switch (k) {
    case 13: case 10: return shift ? 141 : 13;
    case 8: return shift ? 148 : 20;
    case KEY_DELETE: return 20;
    case KEY_END: return 148;
    case KEY_HOME: return shift ? 147 : 19;
    case KEY_UP: return 145;
    case KEY_DOWN: return 17;
    case KEY_LEFT: return 157;
    case KEY_RIGHT: return 29;
    case '\\': case 163: return 92;
    case '`': case '_': return 95;
    case '^': return 94;
    case '~': return 255;
    case '{': return 219;
    case '|': return 221;
    case '}': return 220;
    }
    if (k >= 32 && k < 96) return k;
    return -1;
}

static void put_key(int c) {
    int n = c64_ram(C64_NDX), max = c64_ram(C64_XMAX);
    if (max > 10) max = 10;
    if (n >= max) return;
    c64_set_ram(C64_KEYD + n, c);
    c64_set_ram(C64_NDX, n + 1);
}

// The STOP key works unless its vector (808) has been changed.
static int stop_enabled(void) { return c64_ram(0x0328) == 0xED; }

static void count_time(void) {
    unsigned long now = plat_timer_ms();
    unsigned long ticks = (now - last_ms) * 60 / 1000;
    if (!ticks) return;
    last_ms += ticks * 1000 / 60;
    unsigned long t = (unsigned long)c64_ram(C64_JIFFY) << 16 |
                      c64_ram(C64_JIFFY + 1) << 8 | c64_ram(C64_JIFFY + 2);
    t = (t + ticks) % 5184000UL;          // (24 hours)
    c64_set_ram(C64_JIFFY, t >> 16 & 0xFF);
    c64_set_ram(C64_JIFFY + 1, t >> 8 & 0xFF);
    c64_set_ram(C64_JIFFY + 2, t & 0xFF);
}

void c64_tick(void) {
    int k;
    count_time();
    while ((k = plat_read_key()) >= 0) {
        if (k == 27 || k == 3) {
            if (stop_enabled()) stop_flag = 1;
            else put_key(3);
            continue;
        }
        int c = petscii_key(k);
        if (c >= 0) put_key(c);
    }
    vic_update(0);
}

int c64_get_key(void) {
    c64_tick();
    int n = c64_ram(C64_NDX);
    if (n == 0) return -1;
    if (n > 10) n = 10;
    int c = c64_ram(C64_KEYD);
    for (int i = 1; i < n; i++) c64_set_ram(C64_KEYD + i - 1, c64_ram(C64_KEYD + i));
    c64_set_ram(C64_NDX, n - 1);
    return c;
}

int c64_wait_key(void) {
    int c;
    while ((c = c64_get_key()) < 0) plat_idle();
    return c;
}

int c64_stop_pressed(void) {
    c64_tick();
    if (!stop_flag) return 0;
    stop_flag = 0;
    return 1;
}

unsigned long c64_jiffies(void) {
    count_time();
    return (unsigned long)c64_ram(C64_JIFFY) << 16 | c64_ram(C64_JIFFY + 1) << 8 |
           c64_ram(C64_JIFFY + 2);
}

void keys_init(void) {
    last_ms = plat_timer_ms();
    stop_flag = 0;
}
