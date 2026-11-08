/*
 * Starting a machine's kernel, for the chooser (plat_start_machine() in
 * core/platform.h).
 */
#include "platform.h"
#include "fat.h"
#include "string.h"
#include "mem.h"

#ifdef BIOS_KERNEL
/*
 * BIOS: read X86BASIC/ID.SYS into memory above the chooser's data, then
 * copy it to 0x1000 and jump to it, just as the boot sector would have
 * loaded it (start_image, in core/kernel_entry.asm).
 */
#define MAX_KERNEL (0x20000 - 0x1000)     // as core/bios.ld allows

void start_image(const unsigned char *src, unsigned int len);   // doesn't return

HIGH_MEM static unsigned char image_buf[MAX_KERNEL];

__attribute__((noinline, noclone)) static unsigned char *image(void) { return image_buf; }

const char *plat_start_machine(const char *id) {
    char path[24];
    strcpy(path, "X86BASIC/");
    strcpy(path + 9, id);
    strcpy(path + strlen(path), ".SYS");
    int len;
    fat_folder("");
    const char *err = fat_read(path, image(), MAX_KERNEL, &len);
    if (err) return strcmp(err, "4 Out of memory") == 0 ? "That kernel is too big" : err;
    if (len < 16) return "That kernel is damaged";
    start_image(image(), len);
    return 0;
}
#endif

#ifdef UEFI_KERNEL
/*
 * UEFI: have the firmware load X86BASIC\ID.EFI from the partition we
 * were loaded from (its device path, then a file path node), and run it.
 * StartImage() returns when the machine goes back to the menu (Exit()).
 */
#include "uefi/efi.h"

extern EFI_HANDLE uefi_image;
extern EFI_SYSTEM_TABLE *uefi_st;

static unsigned int node_length(EFI_DEVICE_PATH_PROTOCOL *n) {
    return n->Length[0] | (n->Length[1] << 8);
}

const char *plat_start_machine(const char *id) {
    EFI_BOOT_SERVICES *bs = uefi_st->BootServices;
    EFI_GUID loaded_guid = EFI_LOADED_IMAGE_PROTOCOL_GUID;
    EFI_GUID path_guid = EFI_DEVICE_PATH_PROTOCOL_GUID;
    EFI_LOADED_IMAGE_PROTOCOL *loaded;
    EFI_DEVICE_PATH_PROTOCOL *dev;
    if (bs->HandleProtocol(uefi_image, &loaded_guid, (void **)&loaded) != EFI_SUCCESS ||
        bs->HandleProtocol(loaded->DeviceHandle, &path_guid, (void **)&dev) != EFI_SUCCESS)
        return "Can't find the disk";

    static UINT8 path[1024] __attribute__((aligned(8)));
    unsigned int used = 0;
    for (EFI_DEVICE_PATH_PROTOCOL *n = dev; n->Type != DEVICE_PATH_END_TYPE;
         n = (EFI_DEVICE_PATH_PROTOCOL *)((UINT8 *)n + node_length(n))) {
        unsigned int len = node_length(n);
        if (len < 4 || used + len + 128 > sizeof(path)) return "Can't find the disk";
        memcpy(path + used, n, len);
        used += len;
    }
    // The file: \X86BASIC\ID.EFI, in UCS-2.
    char name[24];
    strcpy(name, "\\X86BASIC\\");
    strcpy(name + 10, id);
    strcpy(name + strlen(name), ".EFI");
    EFI_DEVICE_PATH_PROTOCOL *file = (EFI_DEVICE_PATH_PROTOCOL *)(path + used);
    unsigned int file_len = 4 + (strlen(name) + 1) * 2;
    file->Type = MEDIA_DEVICE_PATH;
    file->SubType = MEDIA_FILEPATH_DP;
    file->Length[0] = file_len;
    file->Length[1] = 0;
    CHAR16 *p = (CHAR16 *)(path + used + 4);
    for (int i = 0; name[i]; i++) p[i] = (unsigned char)name[i];
    p[strlen(name)] = 0;
    used += file_len;
    EFI_DEVICE_PATH_PROTOCOL *end = (EFI_DEVICE_PATH_PROTOCOL *)(path + used);
    end->Type = DEVICE_PATH_END_TYPE;
    end->SubType = DEVICE_PATH_END_SUBTYPE;
    end->Length[0] = 4;
    end->Length[1] = 0;

    EFI_HANDLE child;
    if (bs->LoadImage(0, uefi_image, path, 0, 0, &child) != EFI_SUCCESS)
        return "Can't load that kernel";
    bs->StartImage(child, 0, 0);
    return 0;
}
#endif
