/*
 * Keyboard layouts: see keymap.h.
 *
 * A key is known by its scan code (set 1), which says where it is on the
 * keyboard, whatever its keycap says. The BIOS gives it; UEFI only gives
 * the character a US keyboard would type, so it's looked up in the US
 * layout (us_plain, us_shift) to find the key. The one key that can't be
 * told that way is the extra one beside the left Shift on European
 * keyboards, which types the same as the one beside Enter (\ and |) on
 * a US layout: on UEFI, each layout says which of the two to take it as
 * (for German and French, the one with < and >, which BASIC needs).
 *
 * Each layout lists the keys that differ from the US one: what each
 * types on its own, with Shift and with AltGr (0: nothing). Latin-1 for
 * the characters beyond ASCII.
 */
#include "keymap.h"
#include "fat.h"
#include "string.h"

#define KEYS 0x57

// What a US keyboard types, by scan code: on its own and with Shift.
static const char us_plain[KEYS + 1] =
    "\0\0" "1234567890-=" "\0\0" "qwertyuiop[]" "\0\0" "asdfghjkl;'`" "\0" "\\zxcvbnm,./"
    "\0\0\0 " "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" "\\";
static const char us_shift[KEYS + 1] =
    "\0\0" "!@#$%^&*()_+" "\0\0" "QWERTYUIOP{}" "\0\0" "ASDFGHJKL:\"~" "\0" "|ZXCVBNM<>?"
    "\0\0\0 " "\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0\0" "|";

struct key { unsigned char scan, plain, shift, altgr; };

static const struct key uk[] = {
    {0x03, '2', '"', 0}, {0x04, '3', 0xA3, 0}, {0x28, '\'', '@', 0},
    {0x29, '`', 0xAC, 0xA6}, {0x2B, '#', '~', '\\'}, {0x56, '\\', '|', 0}, {0},
};

static const struct key de[] = {
    {0x03, '2', '"', 0xB2}, {0x04, '3', 0xA7, 0xB3}, {0x07, '6', '&', 0},
    {0x08, '7', '/', '{'}, {0x09, '8', '(', '['}, {0x0A, '9', ')', ']'},
    {0x0B, '0', '=', '}'}, {0x0C, 0xDF, '?', '\\'}, {0x0D, 0xB4, '`', 0},
    {0x10, 'q', 'Q', '@'}, {0x15, 'z', 'Z', 0}, {0x1A, 0xFC, 0xDC, 0},
    {0x1B, '+', '*', '~'}, {0x27, 0xF6, 0xD6, 0}, {0x28, 0xE4, 0xC4, 0},
    {0x29, '^', 0xB0, 0}, {0x2B, '#', '\'', 0}, {0x2C, 'y', 'Y', 0},
    {0x32, 'm', 'M', 0xB5}, {0x33, ',', ';', 0}, {0x34, '.', ':', 0},
    {0x35, '-', '_', 0}, {0x56, '<', '>', '|'}, {0},
};

static const struct key fr[] = {
    {0x02, '&', '1', 0}, {0x03, 0xE9, '2', '~'}, {0x04, '"', '3', '#'},
    {0x05, '\'', '4', '{'}, {0x06, '(', '5', '['}, {0x07, '-', '6', '|'},
    {0x08, 0xE8, '7', '`'}, {0x09, '_', '8', '\\'}, {0x0A, 0xE7, '9', '^'},
    {0x0B, 0xE0, '0', '@'}, {0x0C, ')', 0xB0, ']'}, {0x0D, '=', '+', '}'},
    {0x10, 'a', 'A', 0}, {0x11, 'z', 'Z', 0}, {0x1A, '^', 0xA8, 0},
    {0x1B, '$', 0xA3, 0xA4}, {0x1E, 'q', 'Q', 0}, {0x27, 'm', 'M', 0},
    {0x28, 0xF9, '%', 0}, {0x29, 0xB2, 0, 0}, {0x2B, '*', 0xB5, 0},
    {0x2C, 'w', 'W', 0}, {0x32, ',', '?', 0}, {0x33, ';', '.', 0},
    {0x34, ':', '/', 0}, {0x35, '!', 0xA7, 0}, {0x56, '<', '>', 0}, {0},
};

static const struct {
    const char *name;
    const struct key *keys;
    int uefi_key;                         // \ or | from UEFI: this key
} layouts[] = {
    {"US", 0, 0x2B}, {"UK", uk, 0x2B}, {"GB", uk, 0x2B}, {"DE", de, 0x56}, {"FR", fr, 0x56},
};

static const struct key *layout;          // 0: US
static int uefi_key = 0x2B;

int keymap_set(const char *name) {
    for (unsigned i = 0; i < sizeof layouts / sizeof *layouts; i++) {
        const char *a = layouts[i].name, *b = name;
        while (*a && toupper(*b) == *a) a++, b++;
        if (!*a && !*b) {
            layout = layouts[i].keys;
            uefi_key = layouts[i].uefi_key;
            return 1;
        }
    }
    return 0;
}

int keymap_active(void) {
    return layout != 0;
}

int keymap_scan_char(int scan) {
    if (scan < 2 || scan > KEYS || !us_plain[scan]) return 0;
    int c = keymap_key(us_plain[scan], scan, 0);
    return c > 0 ? c : 0;
}

int keymap_key(int us, int scan, int altgr) {
    if (!layout) return altgr && !us ? -1 : us;
    // Which key, and whether it's shifted (for a letter, whether it's
    // upper case, which counts Caps Lock)
    if (scan < 2 || scan > KEYS || (!us_plain[scan] && !us_shift[scan])) {
        scan = 0;
        if (us == '\\' || us == '|') scan = uefi_key;
        for (int k = 2; k <= KEYS && us && !scan; k++)
            if (us_plain[k] == us || us_shift[k] == us) scan = k;
        if (!scan) return altgr ? -1 : us;   // (Enter, Ctrl+letter ...: as they are)
    }
    int shifted = us ? us == us_shift[scan] : 0;
    if (us && us != us_plain[scan] && us != us_shift[scan]) return us;   // (Ctrl+key)
    for (const struct key *k = layout; k->scan; k++) {
        if (k->scan != scan) continue;
        int c = altgr ? k->altgr : shifted ? k->shift : k->plain;
        // A letter keeps the case the US one has (Caps Lock)
        if (!altgr && us && c < 128 && isalpha(us) && isalpha(c))
            c = us >= 'a' ? c | 0x20 : c & ~0x20;
        return c ? c : -1;
    }
    if (altgr) return -1;
    return us ? us : shifted ? us_shift[scan] : us_plain[scan];
}

// KEYBOARD=UK in BOOT.CFG, at the top of the FAT partition.
void keymap_init(void) {
    static unsigned char text[4096];
    int len;
    fat_folder("");
    if (fat_read("BOOT.CFG", text, sizeof text - 1, &len)) return;
    text[len] = '\0';
    for (char *p = (char *)text; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        const char *key = "KEYBOARD";
        int i = 0;
        while (key[i] && toupper(p[i]) == key[i]) i++;
        if (!key[i]) {
            char *v = p + i;
            while (*v == ' ' || *v == '\t') v++;
            if (*v == '=') {
                v++;
                while (*v == ' ' || *v == '\t') v++;
                char name[8];
                int n = 0;
                while (n < 7 && isalpha(v[n])) name[n] = v[n], n++;
                name[n] = '\0';
                keymap_set(name);
            }
        }
        while (*p && *p != '\n') p++;
    }
}
