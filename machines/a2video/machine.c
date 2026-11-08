/*
 * A test machine for the Apple layer (machines/apple/): no BASIC, just
 * the Apple screen, keyboard and line input, and commands to try them.
 * Not put on images unless asked for (make IMAGE_MACHINES=a2video, or
 * make image-a2video); tests/run_tests.sh and tests/qemu_apple.py use it.
 *
 *   TEXT GR HGR       the modes (GR and HGR as Applesoft does them: mixed,
 *   HGR2 MIXED FULL   page 1; HGR2: full screen, page 2)
 *   PAGE1 PAGE2
 *   80 40             the IIe's 80 columns on and off (PR#3)
 *   DGR DHGR          double low and high resolution (IIe)
 *   BARS              fill the graphics page shown with colour bars
 *   CHARS             all 256 character codes, from the top left
 *   NORMAL INVERSE FLASH
 *   HOME
 *   POKE a,v  PEEK a  (decimal; negative addresses too)
 *   FLAG a            1 if bit 7 of PEEK a is set, else 0
 *   WINDOW l,w,t,b    the text window
 *   MODEL II|II+|IIE  start again as that model
 */
#include "machine.h"
#include "apple.h"
#include "string.h"

static int is(const char *line, const char *word) {
    int n = strlen(word);
    for (int i = 0; i < n; i++)
        if (toupper(line[i]) != word[i]) return 0;
    return line[n] == '\0' || line[n] == ' ';
}

// Numbers after the command word, separated by commas or spaces.
static int numbers(const char *s, int *out, int max) {
    int n = 0;
    while (*s && *s != ' ') s++;
    while (*s && n < max) {
        while (*s == ' ' || *s == ',') s++;
        if (!*s) break;
        int neg = *s == '-', v = 0;
        if (neg) s++;
        if (!isdigit(*s)) return -1;
        while (isdigit(*s)) v = v * 10 + *s++ - '0';
        out[n++] = neg ? -v : v;
    }
    return n;
}

static void print_number(int v) {
    char buf[12];
    itoa(v, buf);
    apple_print(buf);
}

static unsigned int page(void) {
    return apple_sw.page2 && !apple_sw.store80 ? 1 : 0;
}

static void bars(void) {
    if (!apple_sw.hires) {                // low resolution: 16 colours across
        unsigned int base = page() ? 0x800 : 0x400;
        int dbl = apple_sw.col80 && apple_sw.dhires;
        for (int r = 0; r < 24; r++)
            for (int c = 0; c < 40; c++) {
                unsigned int a = base + (r & 7) * 128 + (r >> 3) * 40 + c;
                int colour = dbl ? c * 2 * 16 / 80 : c * 16 / 40;
                if (dbl) {
                    int aux = colour;     // (stored rotated right: shown rotated left)
                    aux = ((aux >> 1) | (aux << 3)) & 15;
                    apple_set_ram(1, a, aux * 17);
                    colour = (c * 2 + 1) * 16 / 80;
                }
                apple_set_ram(0, a, colour * 17);
            }
        return;
    }
    unsigned int base = page() ? 0x4000 : 0x2000;
    for (int y = 0; y < 192; y++) {
        unsigned int a = base + (y & 7) * 1024 + ((y >> 3) & 7) * 128 + (y >> 6) * 40;
        for (int c = 0; c < 40; c++) {
            if (apple_sw.col80 && apple_sw.dhires) {
                // 16 colours, 4 bits a dot: 140 dots, 35 to each 10 columns.
                int colour = c * 16 / 40, bits = 0;
                for (int d = 0; d < 14; d++)
                    bits |= ((colour >> ((c * 14 + d) & 3)) & 1) << d;
                apple_set_ram(1, a + c, bits & 0x7F);
                apple_set_ram(0, a + c, bits >> 7);
                continue;
            }
            // violet, green, blue, orange, white, black: 8 columns each... then
            static const unsigned char even[5] = { 0x55, 0x2A, 0xD5, 0xAA, 0x7F };
            static const unsigned char odd[5] = { 0x2A, 0x55, 0xAA, 0xD5, 0x7F };
            int g = c / 8;
            apple_set_ram(0, a + c, c & 1 ? odd[g] : even[g]);
        }
    }
}

static void chars(void) {
    for (int code = 0; code < 256; code++) {
        int row = code / 32, col = code % 32;
        unsigned int a = 0x400 + (row & 7) * 128 + (row >> 3) * 40 + col;
        apple_set_ram(0, a, code);
    }
}

static void command(char *line) {
    int n[4], k;
    if (!line[0]) return;
    if (is(line, "TEXT")) apple_poke(0xC051, 0);
    else if (is(line, "GR") || is(line, "HGR")) {
        apple_poke(0xC050, 0);
        apple_poke(0xC053, 0);
        apple_poke(0xC054, 0);
        apple_poke(is(line, "HGR") ? 0xC057 : 0xC056, 0);
    } else if (is(line, "HGR2")) {
        apple_poke(0xC050, 0);
        apple_poke(0xC052, 0);
        apple_poke(0xC055, 0);
        apple_poke(0xC057, 0);
    } else if (is(line, "MIXED")) apple_poke(0xC053, 0);
    else if (is(line, "FULL")) apple_poke(0xC052, 0);
    else if (is(line, "PAGE1")) apple_poke(0xC054, 0);
    else if (is(line, "PAGE2")) apple_poke(0xC055, 0);
    else if (is(line, "80")) apple_80col(1);
    else if (is(line, "40")) apple_80col(0);
    else if (is(line, "DGR") || is(line, "DHGR")) {
        apple_poke(0xC00D, 0);            // 80COL
        apple_poke(0xC05E, 0);            // double resolution
        apple_poke(0xC050, 0);
        apple_poke(0xC053, 0);
        apple_poke(is(line, "DHGR") ? 0xC057 : 0xC056, 0);
    } else if (is(line, "BARS")) bars();
    else if (is(line, "CHARS")) chars();
    else if (is(line, "NORMAL")) apple_poke(ZP_INVFLG, 0xFF);
    else if (is(line, "INVERSE")) apple_poke(ZP_INVFLG, 0x3F);
    else if (is(line, "FLASH")) apple_poke(ZP_INVFLG, 0x7F);
    else if (is(line, "HOME")) apple_home();
    else if (is(line, "POKE") && numbers(line, n, 2) == 2) apple_poke(n[0], n[1]);
    else if (is(line, "PEEK") && numbers(line, n, 1) == 1) {
        print_number(apple_peek(n[0]));
        apple_cout(13);
    } else if (is(line, "FLAG") && numbers(line, n, 1) == 1) {
        print_number(apple_peek(n[0]) >> 7);
        apple_cout(13);
    } else if (is(line, "WINDOW") && numbers(line, n, 4) == 4) {
        apple_poke(ZP_WNDLFT, n[0]);
        apple_poke(ZP_WNDWDTH, n[1]);
        apple_poke(ZP_WNDTOP, n[2]);
        apple_poke(ZP_WNDBTM, n[3]);
    } else if (is(line, "MODEL")) {
        char *m = line + 5;
        while (*m == ' ') m++;
        k = is(m, "IIE") ? APPLE_IIE : is(m, "II+") ? APPLE_IIPLUS : APPLE_II;
        apple_init(k);
        apple_print("MODEL ");
        print_number(k);
        apple_cout(13);
    } else {
        apple_print("?");
        apple_print(line);
        apple_cout(13);
    }
}

void machine_start(void) {
    apple_init(APPLE_IIE);
    apple_print("VIDEO TEST\r");
    char line[256];
    for (;;) {
        apple_getln(']', line, sizeof(line));
        command(line);
    }
}
