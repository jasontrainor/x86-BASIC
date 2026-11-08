/*
 * The platform layer (platform.h) for PCs that boot with UEFI, where
 * there is no BIOS: the screen is drawn through the Graphics Output
 * Protocol, keys come from the console input, and saved programs are
 * read and written through Block I/O on the disk we were loaded from.
 *
 * This file is built, with the shared kernel code, into a 64-bit UEFI
 * application, EFI/BOOT/BOOTX64.EFI on the disk image's EFI system
 * partition. The firmware's boot services stay in use throughout.
 */
#include "efi.h"
#include "platform.h"
#include "string.h"

void kernel_start(void);                  // kernel.c

static EFI_SYSTEM_TABLE *st;
static EFI_BOOT_SERVICES *bs;

static void print(const char *s) {
    CHAR16 buf[128];
    int n = 0;
    while (*s && n < 126) {
        if (*s == '\n') buf[n++] = '\r';
        buf[n++] = (unsigned char)*s++;
    }
    buf[n] = 0;
    st->ConOut->OutputString(st->ConOut, buf);
}

static void halt(const char *why) {
    print(why);
    for (;;) bs->Stall(1000000);
}

/* ------------------------------------------------------------------------
 * Display. The picture is drawn at the largest whole-number scale at
 * which its frame (the picture and the least border the machine wants)
 * fits on the screen - the same rule tests/qemu_smoke.py uses to find
 * it - centred, with the border colour filling the rest.
 * --------------------------------------------------------------------- */

static EFI_GRAPHICS_OUTPUT_PROTOCOL *gop;
static unsigned int screen_w, screen_h, scale_x, scale_y, win_x, win_y, win_w, win_h;
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL palette[16];

#define MAX_SCALE 8
#define BLOCK_PIXELS (128 * 1024)
static EFI_GRAPHICS_OUTPUT_BLT_PIXEL block_px[BLOCK_PIXELS];

void plat_init(void) {
    EFI_GUID gop_guid = EFI_GRAPHICS_OUTPUT_PROTOCOL_GUID;
    if (bs->LocateProtocol(&gop_guid, 0, (void **)&gop) != EFI_SUCCESS || !gop)
        halt("x86 BASIC: no graphics output available\n");
    screen_w = gop->Mode->Info->HorizontalResolution;
    screen_h = gop->Mode->Info->VerticalResolution;
}

void plat_video(const struct video *v) {
    unsigned int sx = screen_w / v->frame_width, sy = screen_h / v->frame_height;
    unsigned int scale = sx < sy ? sx : sy;
    if (scale < 1) scale = 1;
    if (scale > MAX_SCALE) scale = MAX_SCALE;
    scale_x = scale;
    scale_y = scale * v->tall;
    win_w = v->width * scale_x;
    win_h = v->height * scale_y;
    if (screen_w < win_w || screen_h < win_h || win_w * scale_y > BLOCK_PIXELS)
        halt("x86 BASIC: the screen is too small\n");
    win_x = (screen_w - win_w) / 2;
    win_y = (screen_h - win_h) / 2;
    for (int i = 0; i < 16; i++) {
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL px = {
            v->palette[i][2], v->palette[i][1], v->palette[i][0], 0
        };
        palette[i] = px;
    }
}

void plat_put_block(int x, int y, int w, int h, const unsigned char *pixels) {
    unsigned int row_w = w * scale_x;
    // As many whole (scaled) rows of the block at a time as fit in block_px.
    int rows = BLOCK_PIXELS / (row_w * scale_y);
    for (int j0 = 0; j0 < h; j0 += rows) {
        int n = h - j0 < rows ? h - j0 : rows;
        EFI_GRAPHICS_OUTPUT_BLT_PIXEL *q = block_px;
        for (int j = j0; j < j0 + n; j++) {
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *first = q;
            for (int i = 0; i < w; i++) {
                EFI_GRAPHICS_OUTPUT_BLT_PIXEL px = palette[pixels[j * w + i] & 15];
                for (unsigned int k = 0; k < scale_x; k++) *q++ = px;
            }
            for (unsigned int k = 1; k < scale_y; k++, q += row_w)
                memcpy(q, first, row_w * sizeof(*q));
        }
        gop->Blt(gop, block_px, EfiBltBufferToVideo, 0, 0, win_x + x * scale_x,
                 win_y + (y + j0) * scale_y, row_w, n * scale_y, 0);
    }
}

static void fill(unsigned int x, unsigned int y, unsigned int w, unsigned int h,
                 EFI_GRAPHICS_OUTPUT_BLT_PIXEL px) {
    if (w && h) gop->Blt(gop, &px, EfiBltVideoFill, 0, 0, x, y, w, h, 0);
}

void plat_border(int c) {
    EFI_GRAPHICS_OUTPUT_BLT_PIXEL px = palette[c & 15];
    fill(0, 0, screen_w, win_y, px);
    fill(0, win_y + win_h, screen_w, screen_h - win_y - win_h, px);
    fill(0, win_y, win_x, win_h, px);
    fill(win_x + win_w, win_y, screen_w - win_x - win_w, win_h, px);
}

/* ------------------------------------------------------------------------
 * Keyboard
 * --------------------------------------------------------------------- */

static int f10_pressed(void);

int plat_read_key(void) {
    EFI_INPUT_KEY key;
    for (;;) {
        if (st->ConIn->ReadKeyStroke(st->ConIn, &key) != EFI_SUCCESS) return -1;
        if (key.ScanCode == SCAN_F10) {
            if (f10_pressed()) plat_restart_menu();
            continue;
        }
        switch (key.ScanCode) {
        case SCAN_UP:     return KEY_UP;
        case SCAN_DOWN:   return KEY_DOWN;
        case SCAN_LEFT:   return KEY_LEFT;
        case SCAN_RIGHT:  return KEY_RIGHT;
        case SCAN_HOME:   return KEY_HOME;
        case SCAN_END:    return KEY_END;
        case SCAN_DELETE: return KEY_DELETE;
        case SCAN_ESC:    return 27;
        }
        CHAR16 c = key.UnicodeChar;
        if (c == '\r' || c == '\n') return 13;
        if (c == 8 || c == 3 || (c >= 32 && c < 127)) return c;
        // Anything else (function keys, accented letters): ignore it.
    }
}

void plat_idle(void) {
    bs->Stall(1000);
}

/* ------------------------------------------------------------------------
 * Time: the CPU's time-stamp counter, calibrated against the firmware's
 * Stall() at startup.
 * --------------------------------------------------------------------- */

static UINT64 tsc_per_ms = 1;

static UINT64 rdtsc(void) {
    UINT32 lo, hi;
    __asm__ __volatile__ ("rdtsc" : "=a" (lo), "=d" (hi));
    return ((UINT64)hi << 32) | lo;
}

static void calibrate_timer(void) {
    UINT64 start = rdtsc();
    bs->Stall(20000);                     // 20ms
    UINT64 per_ms = (rdtsc() - start) / 20;
    tsc_per_ms = per_ms ? per_ms : 1;
}

unsigned long plat_timer_ms(void) {
    return rdtsc() / tsc_per_ms;
}

/* ------------------------------------------------------------------------
 * Sound: PC-compatible machines still decode the PC speaker ports, so
 * this is the same as in the BIOS kernel (silent where there is none).
 * --------------------------------------------------------------------- */

static void outb(UINT16 port, UINT8 val) {
    __asm__ __volatile__ ("outb %0, %1" : : "a" (val), "Nd" (port));
}

static UINT8 inb(UINT16 port) {
    UINT8 val;
    __asm__ __volatile__ ("inb %1, %0" : "=a" (val) : "Nd" (port));
    return val;
}

void plat_tone(int freq, int ms) {
    if (freq >= 20 && freq <= 20000) {
        unsigned int div = 1193182 / freq;
        outb(0x43, 0xB6);
        outb(0x42, div & 0xFF);
        outb(0x42, div >> 8);
        outb(0x61, inb(0x61) | 0x03);
        bs->Stall((UINTN)ms * 1000);
        outb(0x61, inb(0x61) & ~0x03);
    } else {
        bs->Stall((UINTN)ms * 1000);
    }
}

/* ------------------------------------------------------------------------
 * Disk. We are loaded from a partition (the EFI system partition), but
 * the shared code reads the whole disk, partition table and all, as the
 * BIOS kernel does (core/fat.c). So find the disk the partition is on:
 * take the partition's device path, drop its last (hard drive partition)
 * node, and ask for the Block I/O protocol of the device that path names.
 * --------------------------------------------------------------------- */

static EFI_BLOCK_IO_PROTOCOL *disk;
static UINT8 sector[512] __attribute__((aligned(4096)));   // meets any IoAlign

static unsigned int node_length(EFI_DEVICE_PATH_PROTOCOL *n) {
    return n->Length[0] | (n->Length[1] << 8);
}

static void find_disk(EFI_HANDLE image) {
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_GUID block_guid = EFI_BLOCK_IO_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_DEVICE_PATH_PROTOCOL *path;
    if (bs->HandleProtocol(image, &loaded_guid, (void **)&loaded) != EFI_SUCCESS) return;
    if (bs->HandleProtocol(loaded->DeviceHandle, &path_guid, (void **)&path) != EFI_SUCCESS) return;

    // Copy the path, ending it before a final partition node.
    static UINT8 copy[1024];
    unsigned int used = 0;
    EFI_DEVICE_PATH_PROTOCOL *n = path, *last = 0;
    while (n->Type != DEVICE_PATH_END_TYPE) {
        unsigned int len = node_length(n);
        if (len < 4 || used + len + 4 > sizeof(copy)) return;
        last = (EFI_DEVICE_PATH_PROTOCOL *)(copy + used);
        memcpy(copy + used, n, len);
        used += len;
        n = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)n + len);
    }
    if (last && last->Type == MEDIA_DEVICE_PATH && last->SubType == MEDIA_HARDDRIVE_DP)
        used = (UINT8 *)last - copy;
    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(copy + used);
    end->Type = DEVICE_PATH_END_TYPE;
    end->SubType = DEVICE_PATH_END_SUBTYPE;
    end->Length[0] = 4;
    end->Length[1] = 0;

    void *remaining = copy;
    EFI_HANDLE handle;
    EFI_BLOCK_IO_PROTOCOL *bio;
    if (bs->LocateDevicePath(&block_guid, &remaining, &handle) != EFI_SUCCESS) return;
    if (((EFI_DEVICE_PATH_PROTOCOL *)remaining)->Type != DEVICE_PATH_END_TYPE) return;
    if (bs->HandleProtocol(handle, &block_guid, (void **)&bio) != EFI_SUCCESS) return;
    if (bio->Media->LogicalPartition || bio->Media->BlockSize != 512) return;
    disk = bio;
}

int plat_disk_read(unsigned int lba, void *buf) {
    if (!disk || disk->ReadBlocks(disk, disk->Media->MediaId, lba, 512, sector) != EFI_SUCCESS)
        return -1;
    memcpy(buf, sector, 512);
    return 0;
}

int plat_disk_write(unsigned int lba, const void *buf) {
    if (!disk || disk->Media->ReadOnly) return -1;
    memcpy(sector, buf, 512);
    if (disk->WriteBlocks(disk, disk->Media->MediaId, lba, 512, sector) != EFI_SUCCESS)
        return -1;
    disk->FlushBlocks(disk);
    return 0;
}

/* ------------------------------------------------------------------------
 * Back to the chooser: F10 twice. The chooser started this kernel with
 * StartImage(), so leaving with Exit() returns to it (and if the firmware
 * started it, to the firmware's boot menu).
 * --------------------------------------------------------------------- */

EFI_HANDLE uefi_image;
EFI_SYSTEM_TABLE *uefi_st;
static unsigned long f10_time;
static int f10_once;

static int f10_pressed(void) {
    unsigned long now = plat_timer_ms();
    if (f10_once && now - f10_time < 2000) return 1;
    f10_once = 1;
    f10_time = now;
    plat_tone(1200, 50);
    return 0;
}

void plat_restart_menu(void) {
    bs->Exit(uefi_image, EFI_SUCCESS, 0, 0);
}

int plat_menu_wanted(void) {
    int wanted = 0;
    while (plat_read_key() >= 0) wanted = 1;  // a key pressed while starting
    return wanted;
}

/* ------------------------------------------------------------------------ */

EFI_STATUS EFIAPI efi_main(EFI_HANDLE image, EFI_SYSTEM_TABLE *system_table) {
    st = uefi_st = system_table;
    bs = st->BootServices;
    uefi_image = image;
    // The firmware resets the machine after 5 minutes in a boot
    // application unless told otherwise.
    bs->SetWatchdogTimer(0, 0, 0, 0);
    st->ConOut->EnableCursor(st->ConOut, 0);
    calibrate_timer();
    find_disk(image);
    kernel_start();
    return EFI_SUCCESS;
}
