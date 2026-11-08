#!/usr/bin/env python3
"""Make a disk image for the host tests: blank, except for the FAT
partition (as tools/mkimage.py makes it), holding the given files.

    tests/make_disk.py out.img [NAME=file ...]
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import mkimage

files = {'EFI/BOOT/BOOTX64.EFI': b'not really an EFI program' * 100}
for arg in sys.argv[2:]:
    name, _, path = arg.partition('=')
    files[name] = open(path, 'rb').read()
img = bytearray(mkimage.ESP_START * mkimage.SECTOR)
img += mkimage.fat12_image(files, mkimage.ESP_SECTORS, mkimage.ESP_START)
open(sys.argv[1], 'wb').write(img)
