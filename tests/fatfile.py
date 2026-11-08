#!/usr/bin/env python3
"""Read a file from the FAT partition of a disk image, for the tests.

    tests/fatfile.py disk.img NAME     print the file NAME (8.3) to stdout
    tests/fatfile.py disk.img --fsck   check the filesystem with fsck.fat
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import mkimage

data = open(sys.argv[1], 'rb').read()
part = data[mkimage.ESP_START * 512:]

if sys.argv[2] == '--fsck':
    if not shutil.which('fsck.fat'):
        print('(no fsck.fat: not checked)')
        sys.exit(0)
    with tempfile.NamedTemporaryFile(suffix='.img') as f:
        f.write(part)
        f.flush()
        r = subprocess.run(['fsck.fat', '-n', f.name], capture_output=True, text=True)
        print(r.stdout + r.stderr)
        sys.exit(r.returncode)

per_cluster, reserved, fats, root_entries = part[13], struct.unpack_from('<H', part, 14)[0], \
    part[16], struct.unpack_from('<H', part, 17)[0]
fat_sectors = struct.unpack_from('<H', part, 22)[0]
fat = part[reserved * 512:(reserved + fat_sectors) * 512]
root = (reserved + fats * fat_sectors) * 512
data_start = root + root_entries * 32


def next_cluster(c):
    w = struct.unpack_from('<H', fat, c * 3 // 2)[0]
    return w >> 4 if c & 1 else w & 0xFFF


base, _, ext = sys.argv[2].upper().partition('.')
want = (base.ljust(8) + ext.ljust(3)).encode()
for i in range(root_entries):
    e = part[root + i * 32:root + i * 32 + 32]
    if e[0] == 0:
        break
    if e[:11] == want and e[0] != 0xE5 and not e[11] & 0x18:
        c, size = struct.unpack_from('<HI', e, 26)
        out = b''
        while len(out) < size:
            off = data_start + (c - 2) * per_cluster * 512
            out += part[off:off + per_cluster * 512]
            c = next_cluster(c)
        sys.stdout.buffer.write(out[:size])
        sys.exit(0)
sys.exit('not found: ' + sys.argv[2])
