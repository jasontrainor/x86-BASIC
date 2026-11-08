#!/usr/bin/env python3
"""
Build a disk image (os.img) that boots on both kinds of PC, with one or
more machines on it.

    tools/mkimage.py OUT.img [--machines spectrum,...] [--default NAME]
                     [--menu auto|always|never] [--timeout SECONDS]
                     [--sectors N] [--build build] [listing ...]

Each machine NAME is taken from build/NAME/ (kernel.bin and BOOTX64.EFI)
and called what machines/NAME/title.txt says. The first is the default
unless --default says otherwise. Listings (text files or .SAV files with
DOS 8.3 names, such as game.bas) go in the first machine's folder, where
LOAD "GAME.BAS" finds them.

Layout (512-byte sectors), with F = 256 for a 1.44MB image (so it also
works as a floppy) and 2048 for a bigger one:

    0           boot sector: BIOS boot code, BPB, MBR partition table
    1-(F-1)     the BIOS chooser, loaded by the boot sector     partition 1
                                                                (type 0xDA)
    F-end       FAT partition (the EFI system partition):       partition 2
                  EFI/BOOT/BOOTX64.EFI   the UEFI chooser        (type 0xEF)
                  X86BASIC/NAME.SYS      each machine's kernels:
                  X86BASIC/NAME.EFI        BIOS and UEFI
                  BOOT.CFG               which to start (see
                                         machines/chooser/chooser.c)
                  NAME/                  each machine's files

A BIOS (or UEFI firmware with legacy "CSM" boot) runs the boot sector;
UEFI firmware finds the EFI system partition and runs BOOTX64.EFI. Both
start the chooser, which starts a machine. The FAT filesystem is written
here directly, so no extra tools are needed. The size is 1.44MB if
everything fits with room to spare, else 16MB, or --sectors.
"""
import os
import struct
import sys

SECTOR = 512
IMAGE_SECTORS = 2880             # 1.44MB, so it also works as a floppy image
BIG_SECTORS = 32768              # 16MB, when that's too small
KERNEL_MAX = 0x1F000             # a kernel is loaded at 0x1000, below 0x20000
SPARE = 384 * 1024               # room to leave for saved files (else: bigger)
ESP_START = 2048                 # (where tests/make_disk.py puts the partition)
ESP_SECTORS = IMAGE_SECTORS - ESP_START
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')



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
    name = os.path.basename(path).upper()
    base, dot, ext = name.partition('.')
    ok = set('ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789!#$%&\'()-@^_`{}~')
    if not (1 <= len(base) <= 8 and len(ext) <= 3 and '.' not in ext and
            set(base + ext) <= ok):
        sys.exit('%s: listings need DOS 8.3 names, such as GAME.BAS' % path)
    return name


def chs(lba):
    """CHS address of an LBA, with the usual 255-head, 63-sector translation
    (the largest there is, for LBAs beyond it)."""
    cyl, rest = divmod(lba, 255 * 63)
    if cyl > 1023:
        return bytes([254, 0xFF, 0xFF])
    head, sec = divmod(rest, 63)
    return bytes([head, (sec + 1) | ((cyl >> 2) & 0xC0), cyl & 0xFF])


def partition(active, ptype, start, count):
    return bytes([0x80 if active else 0]) + chs(start) + bytes([ptype]) + \
        chs(start + count - 1) + struct.pack('<II', start, count)


def boot_cfg(machines, titles, default, menu, timeout):
    lines = ['# x86 BASIC: which machine to start (see the README).',
             'DEFAULT=%s' % default.upper(),
             'MENU=%s' % menu.upper(),
             'TIMEOUT=%d' % timeout,
             '# The menu\'s names for X86BASIC/NAME.SYS and NAME.EFI:']
    lines += ['%s=%s' % (m.upper(), titles[m]) for m in machines]
    return ('\r\n'.join(lines) + '\r\n').encode()


def main():
    import argparse
    ap = argparse.ArgumentParser(description='Build a disk image.')
    ap.add_argument('out')
    ap.add_argument('listings', nargs='*')
    ap.add_argument('--machines', default='spectrum')
    ap.add_argument('--default')
    ap.add_argument('--menu', default='auto', choices=['auto', 'always', 'never'])
    ap.add_argument('--timeout', type=int, default=5)
    ap.add_argument('--sectors', type=int)
    ap.add_argument('--build', default=os.path.join(ROOT, 'build'))
    a = ap.parse_args()
    machines = [m for m in a.machines.split(',') if m]
    if not machines:
        sys.exit('no machines')
    default = a.default or machines[0]
    if default not in machines:
        sys.exit('--default %s: not one of the machines' % default)

    def read(path, limit=None):
        data = open(path, 'rb').read()
        if limit is not None and len(data) > limit:
            sys.exit('%s: too big (%d bytes, at most %d)' % (path, len(data), limit))
        return data

    boot = read(os.path.join(a.build, 'boot.bin'))
    assert len(boot) == SECTOR, 'boot sector must be 512 bytes'
    chooser = read(os.path.join(a.build, 'chooser', 'kernel.bin'), KERNEL_MAX)
    files = {'EFI/BOOT/BOOTX64.EFI': read(os.path.join(a.build, 'chooser', 'BOOTX64.EFI'))}
    titles = {}
    for m in machines:
        if len(m) > 8 or dos_name(m + '.SYS') != m.upper() + '.SYS':
            sys.exit('%s: machine names must be DOS names of up to 8 characters' % m)
        files['X86BASIC/%s.SYS' % m.upper()] = read(os.path.join(a.build, m, 'kernel.bin'),
                                                    KERNEL_MAX)
        files['X86BASIC/%s.EFI' % m.upper()] = read(os.path.join(a.build, m, 'BOOTX64.EFI'))
        title_file = os.path.join(ROOT, 'machines', m, 'title.txt')
        titles[m] = (open(title_file).read().strip() if os.path.exists(title_file)
                     else m.upper())[:26]
    files['BOOT.CFG'] = boot_cfg(machines, titles, default, a.menu, a.timeout)
    for p in a.listings:
        files['%s/%s' % (machines[0].upper(), dos_name(p))] = read(p)

    used = sum(len(d) for d in files.values()) + SPARE
    sectors = a.sectors or (IMAGE_SECTORS if used <= (IMAGE_SECTORS - 256 - 40) * SECTOR
                            else BIG_SECTORS)
    fat_start = 256 if sectors <= IMAGE_SECTORS else 2048
    if (len(chooser) + SECTOR - 1) // SECTOR > fat_start - 1:
        sys.exit('the chooser is too big for sectors 1-%d' % (fat_start - 1))

    img = bytearray(sectors * SECTOR)
    img[:SECTOR] = boot
    img[446:510] = (partition(True, 0xDA, 1, fat_start - 1) +
                    partition(False, 0xEF, fat_start, sectors - fat_start) + bytes(32))
    img[SECTOR:SECTOR + len(chooser)] = chooser
    img[fat_start * SECTOR:] = fat_image(files, sectors - fat_start, fat_start)
    with open(a.out, 'wb') as f:
        f.write(img)


if __name__ == '__main__':
    main()
