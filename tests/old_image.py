#!/usr/bin/env python3
"""Make a disk image with saved files in the old layout (a directory at
sector 256), for testing tools/savefile.py convert.

    tests/old_image.py out.img FILE.SAV...   (the same files, the old way)
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import savefile as sf

img = bytearray(2880 * 512)
d = bytearray(sf.OLD_DIR_SECTORS * 512)
d[:16] = sf.OLD_MAGIC
start = 0
for i, path in enumerate(sys.argv[2:]):
    name, data, param, kind = sf.unpack(open(path, 'rb').read())
    e = name.encode('latin-1').ljust(16, b'\0') + struct.pack('<IHIBBH', len(data), param,
                                                              sf.checksum(data), kind, 0, start)
    d[32 + i * 32:32 + i * 32 + len(e)] = e
    off = (sf.OLD_LBA + sf.OLD_DIR_SECTORS + start) * 512
    img[off:off + len(data)] = data
    start += (len(data) + 511) // 512
img[sf.OLD_LBA * 512:sf.OLD_LBA * 512 + len(d)] = d
open(sys.argv[1], 'wb').write(img)
