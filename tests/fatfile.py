#!/usr/bin/env python3
"""Read a file from the FAT partition of a disk image, for the tests.

    tests/fatfile.py disk.img PATH     print the file (SPECTRUM/GAME.BAS)
    tests/fatfile.py disk.img --ls DIR list a folder: name size
    tests/fatfile.py disk.img --fsck   check the filesystem with fsck.fat
"""
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import fatread

disk = fatread.Disk(open(sys.argv[1], 'rb').read())

if sys.argv[2] == '--fsck':
    if not shutil.which('fsck.fat'):
        print('(no fsck.fat: not checked)')
        sys.exit(0)
    with tempfile.NamedTemporaryFile(suffix='.img') as f:
        f.write(disk.data[disk.start * 512:disk.start * 512 + disk.size])
        f.flush()
        r = subprocess.run(['fsck.fat', '-n', f.name], capture_output=True, text=True)
        print(r.stdout + r.stderr)
        sys.exit(r.returncode)

if sys.argv[2] == '--ls':
    for name, size, folder in disk.listdir(sys.argv[3] if len(sys.argv) > 3 else ''):
        print(name + ('/' if folder else ''), size)
    sys.exit(0)

data = disk.read(sys.argv[2])
if data is None:
    sys.exit('not found: ' + sys.argv[2])
sys.stdout.buffer.write(data)
