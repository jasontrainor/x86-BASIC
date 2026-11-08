#!/usr/bin/env python3
"""
Build os.img: a 1.44MB disk image that boots on both kinds of PC.

    tools/mkimage.py boot.bin kernel.bin BOOTX64.EFI os.img [listing...]

Any listings (text files with DOS 8.3 names, such as game.bas) go in the
root directory of the EFI system partition, where LOAD "GAME.BAS" finds
them.

Layout (512-byte sectors):

    0           boot sector: BIOS boot code, BPB, MBR partition table
    1-255       the 16-bit kernel, loaded by the boot sector     \\ partition 1
    256-2047    saved files (core/storage.h)                    / (type 0xDA)
    2048-2879   EFI system partition: FAT12 with                  partition 2
                EFI/BOOT/BOOTX64.EFI, the 64-bit UEFI kernel      (type 0xEF)

A BIOS (or UEFI firmware with legacy "CSM" boot) runs the boot sector;
UEFI firmware finds the EFI system partition and runs BOOTX64.EFI. The
FAT filesystem is written here directly, so no extra tools are needed.
"""
import struct
import sys

SECTOR = 512
IMAGE_SECTORS = 2880             # 1.44MB, so it also works as a floppy image
KERNEL_MAX = 255                 # sectors 1-255
ESP_START = 2048                 # must match boot/boot.asm
ESP_SECTORS = IMAGE_SECTORS - ESP_START


def fat_image(files, sectors, hidden):
    """A FAT filesystem of the given size holding files, a dict of
    'DIR/SUB/NAME.EXT' -> bytes. Directories are created as needed.
    FAT12 (with one sector a cluster) if it's small enough, otherwise
    FAT16 with clusters as small as allow it."""
    reserved, fats, root_entries = 1, 2, 224 if sectors < 8192 else 512
    root_sectors = root_entries * 32 // SECTOR
    for fat16, per_cluster in [(False, 1)] + [(True, 1 << k) for k in range(8)]:
        fat_sectors = 1
        while True:              # FAT size depends on the cluster count
            clusters = (sectors - reserved - fats * fat_sectors - root_sectors) // per_cluster
            need = (((clusters + 2) * 2 if fat16 else (clusters + 2) * 3 // 2)
                    + SECTOR - 1) // SECTOR
            if need <= fat_sectors:
                break
            fat_sectors = need
        if (clusters < 4085) != fat16 and clusters < 65525:
            break
    else:
        raise ValueError('no FAT12/FAT16 layout for %d sectors' % sectors)
    data_start = reserved + fats * fat_sectors + root_sectors
    eoc = 0xFFFF if fat16 else 0xFFF

    img = bytearray(sectors * SECTOR)
    fat = {0: 0xFFF8 if fat16 else 0xFF8, 1: eoc}
    next_cluster = [2]

    def alloc(data):
        """Store data in a chain of clusters; returns the first (0 if empty)."""
        size = per_cluster * SECTOR
        n = max(1, (len(data) + size - 1) // size)
        first = next_cluster[0]
        assert first + n - 2 <= clusters, 'filesystem full'
        for i in range(n):
            c = first + i
            fat[c] = c + 1 if i < n - 1 else eoc
            off = (data_start + (c - 2) * per_cluster) * SECTOR
            img[off:off + size] = data[i * size:(i + 1) * size].ljust(size, b'\0')
        next_cluster[0] += n
        return first

    def entry(name, attr, cluster, size):
        if name in ('.', '..'):
            short = name.ljust(11)
        else:
            base, _, ext = name.partition('.')
            short = base.upper().ljust(8)[:8] + ext.upper().ljust(3)[:3]
        # Fixed timestamp (2026-01-01 00:00) so builds are reproducible.
        date = ((2026 - 1980) << 9) | (1 << 5) | 1
        return struct.pack('<11sBBBHHHHHHHI', short.encode(), attr, 0, 0, 0,
                           date, date, 0, 0, date, cluster, size)

    # Build the tree bottom-up: each directory is written once its
    # contents (and so their clusters) are known.
    tree = {}
    for path, data in files.items():
        node = tree
        parts = path.split('/')
        for d in parts[:-1]:
            node = node.setdefault(d, {})
        node[parts[-1]] = data

    def write_dir(node, parent_cluster):
        # Reserve this directory's cluster first, so "." can refer to it.
        size = (len(node) + 2) * 32
        me = alloc(bytes(size))
        entries = [entry('.', 0x10, me, 0), entry('..', 0x10, parent_cluster, 0)]
        for name, child in node.items():
            if isinstance(child, dict):
                entries.append(entry(name, 0x10, write_dir(child, me), 0))
            else:
                entries.append(entry(name, 0x20, alloc(child), len(child)))
        raw = b''.join(entries)
        off = (data_start + (me - 2) * per_cluster) * SECTOR
        img[off:off + len(raw)] = raw
        return me

    root = [b'X86 BASIC  ' + bytes([0x08]) + bytes(20)]   # volume label
    for name, child in tree.items():
        if isinstance(child, dict):
            root.append(entry(name, 0x10, write_dir(child, 0), 0))
        else:
            root.append(entry(name, 0x20, alloc(child), len(child)))
    raw = b''.join(root)
    off = (reserved + fats * fat_sectors) * SECTOR
    img[off:off + len(raw)] = raw

    # File allocation tables: 16-bit entries, or 12-bit ones, two per
    # three bytes.
    table = bytearray(fat_sectors * SECTOR)
    for c, v in fat.items():
        i = c * 3 // 2
        if fat16:
            struct.pack_into('<H', table, c * 2, v)
        elif c % 2 == 0:
            table[i] = v & 0xFF
            table[i + 1] = (table[i + 1] & 0xF0) | (v >> 8)
        else:
            table[i] = (table[i] & 0x0F) | ((v & 0x0F) << 4)
            table[i + 1] = v >> 4
    for f in range(fats):
        off = (reserved + f * fat_sectors) * SECTOR
        img[off:off + len(table)] = table

    # Boot sector with the BIOS parameter block.
    small = sectors < 65536
    bpb = struct.pack('<3s8sHBHBHHBHHHII', b'\xEB\x3C\x90', b'X86BASIC', SECTOR,
                      per_cluster, reserved, fats, root_entries, sectors if small else 0, 0xF8,
                      fat_sectors, 63, 255, hidden, 0 if small else sectors)
    bpb += struct.pack('<BBBI11s8s', 0x80, 0, 0x29, 0x20261108, b'X86 BASIC  ',
                       b'FAT16   ' if fat16 else b'FAT12   ')
    img[:len(bpb)] = bpb
    img[0x3E:0x40] = b'\xEB\xFE'                 # (not bootable itself: loop)
    img[510:512] = b'\x55\xAA'
    return img


def dos_name(path):
    """The 8.3 name for a listing file, or exit with an error."""
    import os
    name = os.path.basename(path).upper()
    base, dot, ext = name.partition('.')
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')
    if not (1 <= len(base) <= 8 and len(ext) <= 3 and '.' not in ext and
            set(base + ext) <= ok):
        sys.exit('%s: listings need DOS 8.3 names, such as GAME.BAS' % path)
    return name


def main():
    boot_path, kernel_path, efi_path, out_path = sys.argv[1:5]
    listings = {'SPECTRUM/' + dos_name(p): open(p, 'rb').read() for p in sys.argv[5:]}
    boot = open(boot_path, 'rb').read()
    kernel = open(kernel_path, 'rb').read()
    efi = open(efi_path, 'rb').read()
    assert len(boot) == SECTOR, 'boot sector must be 512 bytes'
    assert len(kernel) <= KERNEL_MAX * SECTOR, 'kernel too big for sectors 1-255'

    img = bytearray(IMAGE_SECTORS * SECTOR)
    img[:SECTOR] = boot
    img[SECTOR:SECTOR + len(kernel)] = kernel
    files = {'EFI/BOOT/BOOTX64.EFI': efi}
    files.update(listings)
    esp = fat_image(files, ESP_SECTORS, ESP_START)
    img[ESP_START * SECTOR:] = esp
    with open(out_path, 'wb') as f:
        f.write(img)


if __name__ == '__main__':
    main()
