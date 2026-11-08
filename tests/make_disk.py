#!/usr/bin/env python3
"""Make a disk image for the host tests: blank, except for the FAT
partition (as tools/mkimage.py makes it), holding the given files (in
the SPECTRUM folder, unless NAME has a folder in it; /NAME is at the top).

    tests/make_disk.py out.img [--sectors N] [NAME=file ...]

--sectors: the size of the whole disk (normally 2880, 1.44MB). Big
enough, and the partition is FAT16.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import mkimage

files = {'EFI/BOOT/BOOTX64.EFI': b'not really an EFI program' * 100}
args = sys.argv[2:]
sectors = mkimage.IMAGE_SECTORS
if args[:1] == ['--sectors']:
    sectors = int(args[1])
    args = args[2:]
for arg in args:
    name, _, path = arg.partition('=')
    files[name.lstrip('/') if '/' in name else 'SPECTRUM/' + name] = open(path, 'rb').read()
img = bytearray(mkimage.ESP_START * mkimage.SECTOR)
img += mkimage.fat_image(files, sectors - mkimage.ESP_START, mkimage.ESP_START)
# The partition table, as boot/boot.asm has it (just the FAT partition).
img[446 + 16:446 + 32] = bytes([0, 0, 0, 0, 0xEF, 0, 0, 0]) + \
    mkimage.struct.pack('<II', mkimage.ESP_START, sectors - mkimage.ESP_START)
img[510:512] = b'\x55\xAA'
open(sys.argv[1], 'wb').write(img)
