/*
 * Listings as text (a .BAS or .TXT file from a PC) into the machine's
 * characters (PETSCII), for LOAD.
 *
 * Printed listings and the tools that turn programs into text write the
 * characters a keyboard hasn't as names in braces, a convention they
 * share: {CLR}, {HOME}, {DOWN}, {RVS ON}, {WHT}, {F1}, {CBM-A} (C= with
 * A), {SHIFT-A}, {PI}, {POUND}; a count first repeats one ({3 SPACES},
 * {5 DOWN}); and a number is that code ({147}, {$93}).
 *
 * Letters: in a listing with no lower case letters (as magazines print
 * them), capitals are the keyboard's unshifted letters; in one with lower
 * case (as tools write them), lower case is unshifted and capitals are
 * shifted. The pound sign is \ (or £), the up arrow ^, the left arrow _
 * (or `), pi ~.
 */
#include "c64listing.h"
#include "string.h"

static const struct { const char *name; unsigned char code; } names[] = {
    { "CLR", 147 }, { "CLEAR", 147 }, { "HOME", 19 }, { "DOWN", 17 }, { "UP", 145 },
    { "LEFT", 157 }, { "RIGHT", 29 }, { "RVS ON", 18 }, { "RVSON", 18 }, { "RVON", 18 },
    { "REVERSE ON", 18 }, { "RVS OFF", 146 }, { "RVSOFF", 146 }, { "RVOF", 146 },
    { "REVERSE OFF", 146 }, { "BLK", 144 }, { "BLACK", 144 }, { "WHT", 5 }, { "WHITE", 5 },
    { "RED", 28 }, { "CYN", 159 }, { "CYAN", 159 }, { "PUR", 156 }, { "PURPLE", 156 },
    { "GRN", 30 }, { "GREEN", 30 }, { "BLU", 31 }, { "BLUE", 31 }, { "YEL", 158 },
    { "YELLOW", 158 }, { "ORNG", 129 }, { "ORANGE", 129 }, { "BRN", 149 }, { "BROWN", 149 },
    { "LRED", 150 }, { "LT RED", 150 }, { "LIGHT RED", 150 }, { "PINK", 150 },
    { "GRY1", 151 }, { "GREY1", 151 }, { "DGRY", 151 }, { "DARK GREY", 151 },
    { "DARK GRAY", 151 }, { "GRY2", 152 }, { "GREY2", 152 }, { "GRY", 152 }, { "GREY", 152 },
    { "GRAY", 152 }, { "MGRY", 152 }, { "LGRN", 153 }, { "LT GREEN", 153 },
    { "LIGHT GREEN", 153 }, { "LBLU", 154 }, { "LT BLUE", 154 }, { "LIGHT BLUE", 154 },
    { "GRY3", 155 }, { "GREY3", 155 }, { "LGRY", 155 }, { "LIGHT GREY", 155 },
    { "LIGHT GRAY", 155 }, { "INST", 148 }, { "INSERT", 148 }, { "DEL", 20 },
    { "DELETE", 20 }, { "F1", 133 }, { "F3", 134 }, { "F5", 135 }, { "F7", 136 },
    { "F2", 137 }, { "F4", 138 }, { "F6", 139 }, { "F8", 140 }, { "PI", 255 },
    { "POUND", 92 }, { "SPACE", 32 }, { "SPACES", 32 }, { "SHIFT-SPACE", 160 },
    { "RETURN", 13 }, { "LOWER", 14 }, { "SWLC", 14 }, { "UPPER", 142 }, { "SWUC", 142 },
    { "CBM-*", 223 }, { "CBM-+", 166 }, { "CBM--", 220 }, { "CBM-@", 164 },
    { "CBM-POUND", 168 }, { "CBM-^", 222 }, { "SHIFT-*", 192 }, { "SHIFT-+", 219 },
    { "SHIFT--", 221 }, { "SHIFT-@", 186 }, { "SHIFT-POUND", 169 }, { "SHIFT-^", 222 },
};

// C= with A-Z.
static const unsigned char cbm_letters[26] = {
    176, 191, 188, 172, 177, 187, 165, 180, 162, 181, 161, 182, 167,
    170, 185, 175, 171, 178, 174, 163, 184, 190, 179, 189, 183, 173,
};

static int upper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

// The code for an escape's name (in capitals), or -1.
static int escape_code(const char *s) {
    if (*s >= '0' && *s <= '9') {
        int n = 0;
        while (*s >= '0' && *s <= '9') n = n * 10 + *s++ - '0';
        return *s || n > 255 ? -1 : n;
    }
    if (*s == '$') {
        int n = 0, d = 0;
        for (s++; *s; s++, d++) {
            int h = *s >= '0' && *s <= '9' ? *s - '0' : *s >= 'A' && *s <= 'F' ? *s - 'A' + 10 : -1;
            if (h < 0) return -1;
            n = n * 16 + h;
        }
        return d && d <= 2 ? n : -1;
    }
    // C= and Shift with a letter, written several ways: CBM-A, C=A, SHIFT-A, S-A
    const char *rest = 0;
    int shift = 0;
    if (!strncmp(s, "CBM-", 4)) rest = s + 4;
    else if (!strncmp(s, "CBM ", 4)) rest = s + 4;
    else if (!strncmp(s, "C=", 2)) rest = s + 2 + (s[2] == '-' || s[2] == ' ');
    else if (!strncmp(s, "SHIFT-", 6) || !strncmp(s, "SHIFT ", 6)) rest = s + 6, shift = 1;
    else if (!strncmp(s, "S-", 2)) rest = s + 2, shift = 1;
    if (rest && rest[0] >= 'A' && rest[0] <= 'Z' && !rest[1])
        return shift ? rest[0] + 128 : cbm_letters[rest[0] - 'A'];
    for (unsigned i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!strcmp(s, names[i].name)) return names[i].code;
    return -1;
}

int c64_listing_lower(const char *text, int len) {
    int brace = 0;
    for (int i = 0; i < len; i++) {
        if (text[i] == '{') brace = 1;
        else if (text[i] == '}') brace = 0;
        else if (!brace && text[i] >= 'a' && text[i] <= 'z') return 1;
    }
    return 0;
}

int c64_listing_line(const char *s, int len, char *out, int cap, int lower) {
    int n = 0;
    for (int i = 0; i < len; i++) {
        int c = s[i] & 0xFF, count = 1;
        if (c == '{') {                   // an escape: {NAME} or {3 NAME}
            char name[24];
            int k = 0, j = i + 1;
            while (j < len && s[j] != '}' && k < 23) name[k++] = upper(s[j++]);
            name[k] = '\0';
            if (j < len && s[j] == '}') {
                const char *p = name;
                if (*p >= '1' && *p <= '9') {
                    int m = 0;
                    const char *q = p;
                    while (*q >= '0' && *q <= '9') m = m * 10 + *q++ - '0';
                    if (*q == ' ' && m <= 255) {
                        count = m;
                        p = q + 1;
                    }
                }
                int code = escape_code(p);
                if (code < 0 && count > 1) {   // ({3 SPACES} as well as {3 SPACE})
                    k = strlen(p);
                    if (k > 1 && p[k - 1] == 'S') {
                        name[p - name + k - 1] = '\0';
                        code = escape_code(p);
                    }
                }
                if (code >= 0) {
                    while (count-- > 0 && n < cap - 1) out[n++] = code;
                    i = j;
                    continue;
                }
            }
        }
        if (c >= 'a' && c <= 'z') c -= 32;
        else if (c >= 'A' && c <= 'Z' && lower) c += 128;
        else if (c == '\\' || c == 163) c = 92;
        else if (c == '`' || c == '_') c = 95;
        else if (c == '~') c = 255;
        else if (c == '\t') c = ' ';
        else if (c < 32 || c > 126) continue;
        if (n < cap - 1) out[n++] = c;
    }
    out[n] = '\0';
    return n;
}
