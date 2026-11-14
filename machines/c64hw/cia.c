/*
 * The two CIAs ($DC00 and $DD00), as programs read them, and the keys
 * held down.
 *
 * CIA 1's ports are the keyboard's matrix and the joysticks: a program
 * writes the columns to look at to $DC00 and reads the rows down from
 * $DC01 (0: a key down), and the joysticks are its other bits, active
 * low (1 up, 2 down, 4 left, 8 right, 16 fire): port 2 at $DC00 (idle,
 * 127), port 1 at $DC01 (255). The PC's arrow keys and right Ctrl are a
 * joystick, in port 2 unless BOOT.CFG says JOYSTICK=1.
 *
 * Which keys are down: the BIOS says (plat_key_down). Where the platform
 * can't (UEFI), a key typed in the last 150ms counts as down (a key held
 * repeats, and so stays down), and so does an arrow as the joystick's
 * direction (its fire button can't be told there).
 *
 * The firmware's keyboard scan puts the key down (its matrix code, 64 if
 * none) at 197 and the shift keys (1 Shift, 2 C=, 4 CTRL) at 653; this
 * does that as time passes (cia_tick).
 *
 * The timers ($DC04-$DC07, $DD04-$DD07) count down as time passes (from
 * their latches, 1 MHz), and the time-of-day clocks ($DC08-$DC0B: tenths,
 * seconds, minutes, hours in BCD) run from when they're set. CIA 2's
 * port A ($DD00) chooses the VIC's bank.
 */
#include "c64.h"
#include "platform.h"
#include "keymap.h"
#include "string.h"

static unsigned char regs[2][16];
static unsigned short latch[2][2];
static unsigned long timer_start[2][2];   // (ms when each timer last started)
static unsigned long tod_ms[2];           // the time of day, set at tod_set
static unsigned long tod_set[2];
static unsigned char tod_latched[2][4];
static int tod_latching[2];
static int joy_port = 2;
static int typed_matrix = 64;             // (UEFI: the last key typed, and when)
static unsigned long typed_at;
static int typed_joy;                     // (... and the last arrow, as a joystick)

// Each key of the matrix (code = column x 8 + row): the PC key in its
// place (a scan code, 0x80 + for E0 keys), and the character it types.
static const struct { unsigned char scan, ch; } matrix[64] = {
    { 0x0E, 20 }, { 0x1C, 13 }, { 0xCD, 29 }, { 0x41, 0 }, { 0x3B, 0 }, { 0x3D, 0 },
    { 0x3F, 0 }, { 0xD0, 17 },
    { 0x04, '3' }, { 0x11, 'W' }, { 0x1E, 'A' }, { 0x05, '4' }, { 0x2C, 'Z' },
    { 0x1F, 'S' }, { 0x12, 'E' }, { 0x2A, 0 },
    { 0x06, '5' }, { 0x13, 'R' }, { 0x20, 'D' }, { 0x07, '6' }, { 0x2E, 'C' },
    { 0x21, 'F' }, { 0x14, 'T' }, { 0x2D, 'X' },
    { 0x08, '7' }, { 0x15, 'Y' }, { 0x22, 'G' }, { 0x09, '8' }, { 0x30, 'B' },
    { 0x23, 'H' }, { 0x16, 'U' }, { 0x2F, 'V' },
    { 0x0A, '9' }, { 0x17, 'I' }, { 0x24, 'J' }, { 0x0B, '0' }, { 0x32, 'M' },
    { 0x25, 'K' }, { 0x18, 'O' }, { 0x31, 'N' },
    { 0x0C, '+' }, { 0x19, 'P' }, { 0x26, 'L' }, { 0x0D, '-' }, { 0x34, '.' },
    { 0x27, ':' }, { 0x1A, '@' }, { 0x33, ',' },
    { 0x2B, 92 }, { 0x1B, '*' }, { 0x28, ';' }, { 0xC7, 19 }, { 0x36, 0 },
    { 0, '=' }, { 0, 94 }, { 0x35, '/' },
    { 0x02, '1' }, { 0x29, 95 }, { 0x1D, 0 }, { 0x03, '2' }, { 0x39, ' ' },
    { 0x38, 0 }, { 0x10, 'Q' }, { 0x01, 0 },
};

// A key typed (PETSCII): remembered as down for a moment, where the
// platform can't say which keys are down.
void cia_typed(int c) {
    typed_joy = c == 145 ? 1 : c == 17 ? 2 : c == 157 ? 4 : c == 29 ? 8 : 0;
    if (c >= 193 && c <= 218) c -= 128;   // (shifted letters: the same keys)
    if (c == 157) c = 29;                 // (left: Shift and right)
    if (c == 145) c = 17;
    if (c == 147) c = 19;
    if (c == 148) c = 20;
    for (int m = 0; m < 64; m++)
        if (matrix[m].ch && matrix[m].ch == c) {
            typed_matrix = m;
            typed_at = plat_timer_ms();
            return;
        }
}

static int key_down(int m) {
    int d = matrix[m].scan ? plat_key_down(matrix[m].scan) : 0;
    if (d >= 0) return d;
    return m == typed_matrix && plat_timer_ms() - typed_at < 150;
}

static int can_tell(void) { return plat_key_down(SC_LSHIFT) >= 0; }

// The joystick: its bits as the port reads them (active low, 0x1F idle).
static int joystick(void) {
    int bits = 0;
    if (can_tell()) {
        if (plat_key_down(SC_UP) > 0) bits |= 1;
        if (plat_key_down(SC_DOWN) > 0) bits |= 2;
        if (plat_key_down(SC_LEFT) > 0) bits |= 4;
        if (plat_key_down(SC_RIGHT) > 0) bits |= 8;
        if (plat_key_down(SC_RCTRL) > 0) bits |= 16;
    } else if (plat_timer_ms() - typed_at < 150) {
        bits = typed_joy;                 // (UEFI: an arrow just typed; no fire)
    }
    return ~bits & 0x1F;
}

static unsigned long now_us(void) { return plat_timer_ms() * 1000UL; }

static int timer_value(int cia, int t) {
    int ctrl = regs[cia][14 + t];
    unsigned int l = latch[cia][t] ? latch[cia][t] : 65536U;
    if (!(ctrl & 1)) return regs[cia][4 + t * 2] | regs[cia][5 + t * 2] << 8;   // stopped
    static unsigned long reads;           // (each read a little later: it counts on)
    unsigned long gone = now_us() - timer_start[cia][t] + (reads += 113);
    return (int)(l - 1 - gone % l);
}

static int bcd(int n) { return (n / 10) << 4 | n % 10; }

static void tod_now(int cia, unsigned char out[4]) {
    unsigned long ms = tod_ms[cia] + (plat_timer_ms() - tod_set[cia]);
    unsigned long tenths = ms / 100 % 864000UL;
    int h = (int)(tenths / 36000);
    out[0] = tenths % 10;
    out[1] = bcd((int)(tenths / 10 % 60));
    out[2] = bcd((int)(tenths / 600 % 60));
    out[3] = (h >= 12 ? 0x80 : 0) | bcd(h % 12 == 0 ? 12 : h % 12);
}

int cia_read(int cia, int reg) {
    reg &= 15;
    if (cia == 0 && reg <= 1) {
        int cols = regs[0][0], rows = 0;   // (the columns selected: bits written 0)
        for (int m = 0; m < 64; m++)
            if (!(cols >> (m >> 3) & 1) && key_down(m)) rows |= 1 << (m & 7);
        if (reg == 1) return ~rows & 0xFF & (joy_port == 1 ? joystick() | 0xE0 : 0xFF);
        return regs[0][0] & (joy_port == 2 ? joystick() | 0xE0 : 0xFF);
    }
    if (reg >= 4 && reg <= 7) {
        int v = timer_value(cia, (reg - 4) >> 1);
        return reg & 1 ? v >> 8 & 0xFF : v & 0xFF;
    }
    if (reg >= 8 && reg <= 11) {          // reading the hours latches the rest
        int i = reg - 8;
        if (i == 3 || !tod_latching[cia]) {
            tod_now(cia, tod_latched[cia]);
            tod_latching[cia] = i == 3;
        }
        if (i == 0) tod_latching[cia] = 0;
        return tod_latched[cia][i];
    }
    if (reg == 13) return 0;              // (no interrupts here)
    return regs[cia][reg];
}

void cia_write(int cia, int reg, int value) {
    reg &= 15;
    value &= 0xFF;
    if (reg >= 4 && reg <= 7) {           // a timer's latch
        int t = (reg - 4) >> 1;
        latch[cia][t] = reg & 1 ? (latch[cia][t] & 0xFF) | value << 8 :
                                  (latch[cia][t] & 0xFF00) | value;
    } else if (reg >= 8 && reg <= 11) {   // the time of day, set
        unsigned char t[4];
        tod_now(cia, t);
        t[reg - 8] = value;
        int h = (t[3] >> 4 & 1) * 10 + (t[3] & 15);
        if (h == 12) h = 0;
        if (t[3] & 0x80) h += 12;
        tod_ms[cia] = (((h * 60UL + (t[2] >> 4) * 10 + (t[2] & 15)) * 60 +
                        (t[1] >> 4) * 10 + (t[1] & 15)) * 10 + (t[0] & 15)) * 100;
        tod_set[cia] = plat_timer_ms();
    } else if (reg == 14 || reg == 15) {  // a timer started (or loaded)
        int t = reg - 14;
        if (((value & 1) && !(regs[cia][reg] & 1)) || (value & 0x10))
            timer_start[cia][t] = now_us();
        if (!(value & 1)) {               // (stopped: holds its count)
            int v = timer_value(cia, t);
            regs[cia][4 + t * 2] = v & 0xFF;
            regs[cia][5 + t * 2] = v >> 8;
        }
    }
    regs[cia][reg] = value;
    if (cia == 1 && reg == 0) vic_bank_changed();
}

int cia_port_a2(void) { return regs[1][0]; }

// The firmware's keyboard scan, as time passes: 197 and 653.
void cia_tick(void) {
    int lstx = 64;
    for (int m = 0; m < 64 && lstx == 64; m++)
        if (m != 15 && m != 52 && m != 58 && m != 61 && key_down(m)) lstx = m;
    c64_set_ram(C64_LSTX, lstx);
    int b = plat_buttons(), sh = 0;
    if (b & 4) sh |= 1;
    if (b & 1) sh |= 2;
    if (plat_key_down(SC_LCTRL) > 0) sh |= 4;
    c64_set_ram(C64_SHFLAG, sh);
}

void cia_init(void) {
    memset(regs, 0, sizeof regs);
    memset(latch, 0, sizeof latch);
    regs[0][0] = 0x7F;                    // CIA 1: all columns
    regs[0][2] = 0xFF;
    regs[0][14] = 0x01;                   // timer A running (the firmware's interrupt)
    latch[0][0] = 0x4025;
    regs[1][0] = 0x97;                    // CIA 2: bank 0
    regs[1][2] = 0x3F;
    unsigned long t = now_us();
    timer_start[0][0] = t;
    tod_ms[0] = tod_ms[1] = 0;
    tod_set[0] = tod_set[1] = plat_timer_ms();
}

void cia_config(void) {                   // (JOYSTICK=1 in BOOT.CFG)
    char v[4];
    joy_port = boot_cfg("JOYSTICK", v, sizeof v) && v[0] == '1' ? 1 : 2;
}
