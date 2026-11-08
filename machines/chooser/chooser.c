/*
 * The chooser: the first thing to start, from the boot sector (BIOS) or
 * as EFI/BOOT/BOOTX64.EFI (UEFI). It picks a machine and starts it.
 *
 * The machines are the kernels in the X86BASIC folder of the FAT
 * partition: X86BASIC/NAME.SYS for the BIOS, X86BASIC/NAME.EFI for UEFI.
 * BOOT.CFG, a text file at the top of the partition, says what they're
 * called and which to start:
 *
 *   # x86 BASIC: which machine to start
 *   DEFAULT=SPECTRUM        the one to start (else the first)
 *   MENU=AUTO               AUTO: a menu if there's more than one machine;
 *                           ALWAYS; or NEVER (just start DEFAULT)
 *   TIMEOUT=5               seconds the menu waits before starting
 *                           DEFAULT (0: until a key is pressed)
 *   SPECTRUM=Spectrum BASIC the menu's name for X86BASIC/SPECTRUM.*
 *
 * Machines are listed in the order BOOT.CFG names them, then any others
 * there are kernels for, by their file names. Holding Shift (BIOS) or
 * pressing a key while it starts shows the menu whatever BOOT.CFG says,
 * and so does pressing F10 twice in a machine.
 */
#include "machine.h"
#include "platform.h"
#include "fat.h"
#include "font.h"
#include "string.h"
#include "mem.h"

#ifdef UEFI_KERNEL
#define KERNEL_EXT ".EFI"
#else
#define KERNEL_EXT ".SYS"
#endif

#define COLS 32
#define ROWS 24
#define MAX_MACHINES 15
#define TITLE_LEN 26

enum { BLACK, BLUE, WHITE, YELLOW, BAR, GREY, RED };

static struct {
    char id[9];                            // NAME, as in X86BASIC/NAME.SYS
    char title[TITLE_LEN + 1];
    int present;                           // there's a kernel for it
} machines[MAX_MACHINES];
static int n_machines;

static char default_id[9];
static int menu_mode;                      // 0 AUTO, 1 ALWAYS, 2 NEVER
static int timeout_s = 5;

/* ------------------------------------------------------------------------
 * The screen: 32x24 characters of 8x8, a 256x192 picture
 * --------------------------------------------------------------------- */

static void video(void) {
    struct video v = { 256, 192, 1, 320, 240, {
        { 0, 0, 0 }, { 0x10, 0x20, 0x60 }, { 0xF0, 0xF0, 0xF0 }, { 0xFF, 0xE0, 0x40 },
        { 0x30, 0x60, 0xC0 }, { 0xA0, 0xA8, 0xB8 }, { 0xFF, 0x60, 0x50 },
    } };
    plat_video(&v);
    plat_border(BLUE);
}

static void put_char(int col, int row, char c, int ink, int paper) {
    unsigned char px[64];
    unsigned char ch = (unsigned char)c;
    const unsigned char *bits = font[(ch >= 32 && ch < 128 ? ch : '?') - 32];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) px[y * 8 + x] = (bits[y] & (0x80 >> x)) ? ink : paper;
    plat_put_block(col * 8, row * 8, 8, 8, px);
}

// Text at (col, row), then blanks to the end of the line.
static void put_line(int col, int row, const char *s, int ink, int paper) {
    for (int c = 0; c < col; c++) put_char(c, row, ' ', ink, paper);
    for (; col < COLS; col++) put_char(col, row, *s ? *s++ : ' ', ink, paper);
}

static void clear(void) {
    for (int r = 0; r < ROWS; r++) put_line(0, r, "", WHITE, BLUE);
}

/* ------------------------------------------------------------------------
 * BOOT.CFG and the kernels
 * --------------------------------------------------------------------- */

HIGH_MEM static unsigned char cfg_buf[4096];

__attribute__((noinline, noclone)) static unsigned char *cfg_text(void) { return cfg_buf; }

static int same(const char *a, const char *b) {
    for (; *a && *b; a++, b++)
        if (toupper(*a) != toupper(*b)) return 0;
    return *a == *b;
}

static int find_machine(const char *id) {
    for (int i = 0; i < n_machines; i++)
        if (same(machines[i].id, id)) return i;
    return -1;
}

static int add_machine(const char *id) {
    int i = find_machine(id);
    if (i >= 0 || n_machines == MAX_MACHINES) return i;
    i = n_machines++;
    int n = 0;
    for (; id[n] && n < 8; n++) machines[i].id[n] = toupper(id[n]);
    machines[i].id[n] = '\0';
    strcpy(machines[i].title, machines[i].id);
    return i;
}

static void cfg_line(char *key, char *value) {
    if (same(key, "DEFAULT")) {
        int n = 0;
        for (; value[n] && n < 8; n++) default_id[n] = toupper(value[n]);
        default_id[n] = '\0';
    } else if (same(key, "MENU")) {
        menu_mode = same(value, "ALWAYS") ? 1 : same(value, "NEVER") ? 2 : 0;
    } else if (same(key, "TIMEOUT")) {
        int t = 0;
        while (*value >= '0' && *value <= '9' && t < 1000) t = t * 10 + *value++ - '0';
        timeout_s = t;
    } else if (key[0] && strlen(key) <= 8) {
        int i = add_machine(key);
        if (i >= 0 && value[0]) {
            int n = 0;
            for (; value[n] && n < TITLE_LEN; n++) machines[i].title[n] = value[n];
            machines[i].title[n] = '\0';
        }
    }
}

static void read_cfg(void) {
    unsigned char *t = cfg_text();
    int len;
    fat_folder("");
    if (fat_read("BOOT.CFG", t, 4095, &len)) return;
    t[len] = '\0';
    char line[80];
    int i = 0;
    while (i < len) {
        int n = 0;
        while (i < len && t[i] != '\n' && t[i] != '\r') {
            if (n < 79) line[n++] = t[i];
            i++;
        }
        while (i < len && (t[i] == '\n' || t[i] == '\r')) i++;
        line[n] = '\0';
        // KEY=VALUE, with spaces around either ignored; # starts a comment.
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || !*p) continue;
        char *eq = p;
        while (*eq && *eq != '=') eq++;
        if (!*eq) continue;
        char *end = eq;
        *eq = '\0';
        while (end > p && (end[-1] == ' ' || end[-1] == '\t')) *--end = '\0';
        char *v = eq + 1;
        while (*v == ' ' || *v == '\t') v++;
        for (char *q = v + strlen(v); q > v && (q[-1] == ' ' || q[-1] == '\t'); ) *--q = '\0';
        cfg_line(p, v);
    }
}

static void found_kernel(const char *name, int len) {
    int n = strlen(name);
    if (n < 5 || strcmp(name + n - 4, KERNEL_EXT) != 0) return;
    char id[9];
    int k = 0;
    for (; k < n - 4 && k < 8; k++) id[k] = name[k];
    id[k] = '\0';
    int i = add_machine(id);
    if (i >= 0) machines[i].present = 1;
}

// The machines there are kernels for, in order.
static void find_machines(void) {
    read_cfg();
    fat_folder("X86BASIC");
    fat_list(found_kernel);
    int n = 0;
    for (int i = 0; i < n_machines; i++)
        if (machines[i].present) machines[n++] = machines[i];
    n_machines = n;
}

/* ------------------------------------------------------------------------
 * The menu
 * --------------------------------------------------------------------- */

static int chosen;
static const char *last_error;

static void draw_menu(void) {
    put_line(1, 1, "x86 BASIC", YELLOW, BLUE);
    put_line(1, 3, "Choose a computer:", WHITE, BLUE);
    for (int i = 0; i < n_machines; i++) {
        char line[COLS + 1];
        line[0] = i < 9 ? '1' + i : ' ';
        line[1] = ' ';
        line[2] = ' ';
        strcpy(line + 3, machines[i].title);
        int sel = i == chosen;
        put_char(0, 5 + i, ' ', WHITE, BLUE);
        put_line(1, 5 + i, line, sel ? WHITE : GREY, sel ? BAR : BLUE);
    }
    put_line(1, 20, "Up, Down and Enter, or 1-9", GREY, BLUE);
    put_line(1, 22, "F10 twice: back to this menu", GREY, BLUE);
    if (last_error) put_line(1, 18, last_error, RED, BLUE);
}

static void show_countdown(int seconds) {
    char line[COLS + 1];
    if (seconds < 0) {
        put_line(1, 21, "", WHITE, BLUE);
        return;
    }
    strcpy(line, "Starting in ");
    char n[8];
    itoa(seconds, n);
    strcpy(line + strlen(line), n);
    put_line(1, 21, line, WHITE, BLUE);
}

static void start(int i) {
    clear();
    char line[COLS + 1];
    strcpy(line, "Starting ");
    strcpy(line + 9, machines[i].title);
    put_line(1, 11, line, WHITE, BLUE);
    last_error = plat_start_machine(machines[i].id);
    // Back: it failed, or (UEFI) the machine went back to the menu.
    video();
    clear();
}

// Show the menu until a machine is chosen, or the time is up (timeout
// in seconds, < 0: none).
static int menu(int timeout) {
    clear();
    draw_menu();
    unsigned long until = plat_timer_ms() + timeout * 1000UL;
    int shown = -2;
    for (;;) {
        if (timeout >= 0) {
            long left = (long)(until - plat_timer_ms());
            int s = left > 0 ? (left + 999) / 1000 : 0;
            if (s != shown) show_countdown(shown = s);
            if (left <= 0) return chosen;
        }
        int k = plat_read_key();
        if (k < 0) {
            plat_idle();
            continue;
        }
        if (timeout >= 0) {                // any key stops the countdown
            timeout = -1;
            show_countdown(-1);
        }
        int old = chosen;
        if (k == KEY_UP && chosen > 0) chosen--;
        else if (k == KEY_DOWN && chosen < n_machines - 1) chosen++;
        else if (k == KEY_HOME) chosen = 0;
        else if (k == KEY_END) chosen = n_machines - 1;
        else if (k == 13) return chosen;
        else if (k >= '1' && k <= '9' && k - '1' < n_machines) return k - '1';
        if (chosen != old) draw_menu();
    }
}

void machine_start(void) {
    video();
    int wanted = plat_menu_wanted();
    find_machines();
    if (n_machines == 0) {
        clear();
        put_line(1, 1, "x86 BASIC", YELLOW, BLUE);
        put_line(1, 3, "No machines found: there are", WHITE, BLUE);
        put_line(1, 4, "no kernels in X86BASIC/", WHITE, BLUE);
        put_line(1, 5, "(NAME" KERNEL_EXT " files).", WHITE, BLUE);
        for (;;) plat_idle();
    }
    int d = find_machine(default_id);
    chosen = d >= 0 ? d : 0;
    if (!wanted && (menu_mode == 2 || (menu_mode == 0 && n_machines == 1)))
        start(chosen);
    // Here with the menu wanted, or after a machine failed to start or
    // (UEFI) went back to the menu.
    int timeout = wanted || last_error || timeout_s == 0 ? -1 : timeout_s;
    for (;;) {
        chosen = menu(timeout);
        start(chosen);
        timeout = -1;
    }
}
