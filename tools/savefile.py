#!/usr/bin/env python3
"""Saved files (SAVE "name") on a PC: see core/storage.c for the format.

    tools/savefile.py convert old.img DIR   copy the files saved on an old
                                            image (before saves moved into
                                            the FAT partition) into DIR,
                                            as NAME.SAV files
    tools/savefile.py info FILE.SAV ...     what each file is

Converted files go in a machine's folder on the stick (SPECTRUM/ for the
Spectrum), or onto an image with make LISTINGS="DIR/*.SAV".
"""
import os
import struct
import sys

HEADER = 48
MAGIC = b'X86BSAV1'
KINDS = ['program', 'bytes', 'numbers', 'chars', 'text']

# The old layout: a directory at sector 256, its data after it.
OLD_LBA, OLD_DIR_SECTORS, OLD_FILES, OLD_MAGIC = 256, 8, 127, b'X86BASIC-SAVES3\0'


def checksum(data):
    h = 2166136261
    for b in data:
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return h


def disk_name(name, k):
    """Candidate k (0-35) for a saved name's file, as storage.c makes it."""
    base = ''.join(c.upper() if c.isascii() and c.isalnum() else '_' for c in name)[:8]
    if k:
        base = base[:6] + '~' + '0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ'[k]
    return base + '.SAV'


def pack(name, data, param, kind):
    raw = name.encode('latin-1')
    assert 1 <= len(raw) <= 15, 'names are 1-15 characters'
    head = MAGIC + raw.ljust(16, b'\0') + struct.pack('<IHB', len(data), param, kind)
    head += b'\0' + struct.pack('<I', checksum(data))
    return head.ljust(HEADER, b'\0') + data


def unpack(blob):
    """(name, data, param, kind), or None if it isn't a saved file."""
    if len(blob) < HEADER or blob[:8] != MAGIC:
        return None
    name = blob[8:24].split(b'\0')[0].decode('latin-1')
    length, param, kind = struct.unpack_from('<IHB', blob, 24)
    data = blob[HEADER:HEADER + length]
    if len(data) != length or checksum(data) != struct.unpack_from('<I', blob, 32)[0]:
        return None
    return name, data, param, kind


def old_saves(img):
    """The files on an image from before saves moved: (name, data, param, kind)."""
    d = img[OLD_LBA * 512:(OLD_LBA + OLD_DIR_SECTORS) * 512]
    if d[:16] != OLD_MAGIC:
        return []
    out = []
    for i in range(OLD_FILES):
        e = d[32 + i * 32:64 + i * 32]
        if not e[0]:
            continue
        name = e[:16].split(b'\0')[0].decode('latin-1')
        length, param, crc = struct.unpack_from('<IHI', e, 16)
        kind, start = e[26], struct.unpack_from('<H', e, 28)[0]
        off = (OLD_LBA + OLD_DIR_SECTORS + start) * 512
        data = img[off:off + length]
        if len(data) != length or checksum(data) != crc:
            print('%s: damaged, skipped' % name, file=sys.stderr)
            continue
        out.append((name, data, param, kind))
    return out


def convert(img_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    taken = {f.upper() for f in os.listdir(out_dir)}
    files = old_saves(open(img_path, 'rb').read())
    if not files:
        sys.exit('%s: no saved files in the old format' % img_path)
    for name, data, param, kind in files:
        for k in range(36):
            fname = disk_name(name, k)
            if fname not in taken:
                break
        else:
            sys.exit('%s: no free file name' % name)
        taken.add(fname)
        with open(os.path.join(out_dir, fname), 'wb') as f:
            f.write(pack(name, data, param, kind))
        print('%-15s -> %s' % (name, fname))


def main():
    if len(sys.argv) == 4 and sys.argv[1] == 'convert':
        convert(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'info':
        for path in sys.argv[2:]:
            f = unpack(open(path, 'rb').read())
            if not f:
                print('%s: not a saved file (or damaged)' % path)
                continue
            name, data, param, kind = f
            print('%s: "%s", %s, %d bytes, param %d' %
                  (path, name, KINDS[kind] if kind < len(KINDS) else kind, len(data), param))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
