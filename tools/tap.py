#!/usr/bin/env python3
"""Programs and bytes from ZX Spectrum tape files (.tap, .tzx), for the
Spectrum-style machine: no ROM and no machine code needed, just the files.

    tools/tap.py list TAPE.tap           what's on it
    tools/tap.py extract TAPE.tap DIR    its BASIC programs as listings
                                         (NAME.BAS) and its bytes (CODE,
                                         SCREEN$) as saved files (NAME.SAV)

Copy DIR's files into the stick's SPECTRUM folder: LOAD "NAME.BAS" loads
a program (LIST and RUN it as usual; the line it started at is in a #
comment at the top), and LOAD "name" CODE the bytes, by the name they had
on the tape. tools/check.py checks a listing on a PC first.

A program on tape is its lines as the Spectrum keeps them: each line's
number (2 bytes, high byte first), its length (2 bytes), its text and
13. Keywords are single characters (165-255, and 163-164 for the 128's
SPECTRUM and PLAY), each number as typed is followed by 14 and its value
in 5 bytes (dropped here: it's worked out again from the text), and the
Spectrum's own characters are written as the escapes LOAD reads (\\a for
the first UDG, \\:. for a block graphic, \\* for (c), ` for the pound sign,
\\{16} for an INK control code). Arrays (DATA) aren't converted.

.tzx files are read for their standard-speed, turbo and pure data blocks;
blocks that only describe the tape are skipped.
"""
import os
import re
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savefile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FILE_BYTES = 1
KINDS = ['Program', 'Number array', 'Character array', 'Bytes']


def keywords():
    """The Spectrum's keywords by character code, from the interpreter's
    table (machines/spectrum/basic/statements.c), so the two agree."""
    src = open(os.path.join(ROOT, 'machines', 'spectrum', 'basic', 'statements.c')).read()
    a = src.index('spectrum_keywords[] = {')
    words = re.findall(r'"((?:[^"\\]|\\.)*)"', src[a:src.index('};', a)])
    assert len(words) == 256 - 165
    table = {165 + i: w for i, w in enumerate(words)}
    table.update({163: 'SPECTRUM', 164: 'PLAY'})
    return table


KEYWORDS = keywords()


# --- Tape files -------------------------------------------------------------

def tap_blocks(data):
    """The blocks of a .tap file: flag byte, then data (checksum checked)."""
    blocks, pos = [], 0
    while pos + 2 <= len(data):
        n = struct.unpack_from('<H', data, pos)[0]
        blocks.append(data[pos + 2:pos + 2 + n])
        pos += 2 + n
    return blocks


def tzx_blocks(data):
    """The data blocks of a .tzx file (the others are skipped)."""
    if data[:8] != b'ZXTape!\x1a':
        raise ValueError('not a .tzx file')
    blocks, pos = [], 10
    u16 = lambda o: struct.unpack_from('<H', data, o)[0]
    u24 = lambda o: data[o] | data[o + 1] << 8 | data[o + 2] << 16
    u32 = lambda o: struct.unpack_from('<I', data, o)[0]
    while pos < len(data):
        bid, pos = data[pos], pos + 1
        if bid == 0x10:                   # standard speed data
            n = u16(pos + 2)
            blocks.append(data[pos + 4:pos + 4 + n])
            pos += 4 + n
        elif bid == 0x11:                 # turbo speed data
            n = u24(pos + 15)
            blocks.append(data[pos + 18:pos + 18 + n])
            pos += 18 + n
        elif bid == 0x14:                 # pure data
            n = u24(pos + 7)
            blocks.append(data[pos + 10:pos + 10 + n])
            pos += 10 + n
        else:                             # how long the others are
            length = {
                0x12: lambda: 4, 0x13: lambda: 1 + 2 * data[pos],
                0x15: lambda: 8 + u24(pos + 5), 0x18: lambda: 4 + u32(pos),
                0x19: lambda: 4 + u32(pos), 0x20: lambda: 2, 0x21: lambda: 1 + data[pos],
                0x22: lambda: 0, 0x23: lambda: 2, 0x24: lambda: 2, 0x25: lambda: 0,
                0x26: lambda: 2 + 2 * u16(pos), 0x27: lambda: 0, 0x28: lambda: 2 + u16(pos),
                0x2A: lambda: 4, 0x2B: lambda: 5, 0x30: lambda: 1 + data[pos],
                0x31: lambda: 2 + data[pos + 1], 0x32: lambda: 2 + u16(pos),
                0x33: lambda: 1 + 3 * data[pos], 0x35: lambda: 20 + u32(pos + 16),
                0x5A: lambda: 9,
            }.get(bid)
            if length is None:
                print('block type %02Xh: not known, so the rest is skipped' % bid,
                      file=sys.stderr)
                break
            pos += length()
    return blocks


def files(path):
    """[(kind, name, data, param1, param2)] for the files on a tape: kind
    0-3 (KINDS), or None for a block with no header before it."""
    data = open(path, 'rb').read()
    blocks = tzx_blocks(data) if data[:8] == b'ZXTape!\x1a' else tap_blocks(data)
    out, header = [], None
    for b in blocks:
        if len(b) < 2:
            continue
        flag, body, check = b[0], b[1:-1], b[-1]
        x = flag
        for c in body:
            x ^= c
        if x != check:
            print('a block fails its checksum: kept anyway', file=sys.stderr)
        if flag == 0 and len(body) == 17:
            header = body
            continue
        if header is not None:
            kind = header[0]
            name = header[1:11].decode('latin-1').rstrip()
            length, p1, p2 = struct.unpack_from('<HHH', header, 11)
            out.append((kind, name, body[:length], p1, p2))
            header = None
        else:
            out.append((None, '', body, 0, 0))
    return out


# --- Programs ---------------------------------------------------------------

def block_escape(c):
    """A block graphic (128-143) as its escape: the left and right halves."""
    b = c - 128
    half = lambda top, bottom: ' \'.:'[(1 if b & top else 0) | (2 if b & bottom else 0)]
    return '\\' + half(2, 8) + half(1, 4)


def character(c):
    """A character of a line, as a listing writes it."""
    if c == 0x60:
        return '`'                        # the pound sign
    if c == 0x5C:
        return '\\\\'
    if c == 0x7F:
        return '\\*'                      # (c)
    if 0x80 <= c <= 0x8F:
        return block_escape(c)
    if 0x90 <= c <= 0xA4:
        return '\\' + chr(ord('a') + c - 0x90)   # the UDGs (A-U)
    if 32 <= c < 127:
        return chr(c)
    return '\\{%d}' % c


def listing(prog, autostart=None):
    """A program (as SAVE writes it) as text, as LIST shows it."""
    lines = ['# autostart: LINE %d' % autostart] if autostart is not None else []
    pos = 0
    while pos + 4 <= len(prog):
        num = prog[pos] << 8 | prog[pos + 1]
        n = struct.unpack_from('<H', prog, pos + 2)[0]
        if num > 9999:                    # (the variables, after the program)
            break
        text, pos = prog[pos + 4:pos + 4 + n], pos + 4 + n
        out, i, in_str, rem = '', 0, False, False
        while i < len(text) and text[i] != 13:
            c = text[i]
            i += 1
            if c == 14 and not in_str and not rem:    # a number's hidden value
                i += 5
                continue
            if c >= 163 and not in_str:   # (a REM's keywords list as words too)
                word = KEYWORDS[c]
                if word[0].isalpha() and out and out[-1] != ' ':
                    out += ' '
                out += word
                if (word[0].isalpha() or word.endswith('#')) and text[i:i + 1] != b' ':
                    out += ' '
                rem = rem or word == 'REM'
                continue
            if c == 34 and not rem:
                in_str = not in_str
            if 16 <= c <= 21 and not in_str and not rem:   # colour codes in the line
                i += 1
                continue
            out += character(c)
        lines.append('%4d %s' % (num, out.rstrip()))
    return '\n'.join(lines) + '\n'


def disk_name(name, used, ext):
    """A DOS name for a file from the tape (as storage.c makes them)."""
    base = savefile.disk_name(name or 'NONAME', 0)[:-4]
    for k in range(36):
        cand = savefile.disk_name(name or 'NONAME', k)[:-4] + ext
        if cand not in used:
            used.add(cand)
            return cand
    raise SystemExit('too many files named like %s' % base)


def extract(path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    used = set()
    for kind, name, data, p1, p2 in files(path):
        label = '%-10s %s' % (name, KINDS[kind] if kind in (0, 1, 2, 3) else 'headerless')
        if kind == 0:
            disk = disk_name(name, used, '.BAS')
            text = listing(data[:p2] if p2 <= len(data) else data,
                           p1 if p1 < 10000 else None)
            open(os.path.join(out_dir, disk), 'w', encoding='latin-1').write(text)
            print('%-12s %s' % (disk, label))
        elif kind == 3:
            disk = disk_name(name, used, '.SAV')
            save_name = name or 'noname'
            open(os.path.join(out_dir, disk), 'wb').write(
                savefile.pack(save_name[:15], data, p1, FILE_BYTES))
            what = 'SCREEN$' if p1 == 16384 and len(data) == 6912 else 'CODE %d,%d' % (p1, len(data))
            print('%-12s %s: %s' % (disk, label, what))
        else:
            print('%-12s %s: not converted' % ('skipped', label))


def list_tape(path):
    for kind, name, data, p1, p2 in files(path):
        if kind == 0:
            print('Program: %-10s %5d bytes%s' % (name, len(data),
                  ' LINE %d' % p1 if p1 < 10000 else ''))
        elif kind == 3:
            print('Bytes:   %-10s CODE %d,%d' % (name, p1, len(data)))
        elif kind in (1, 2):
            print('%s: %s' % (KINDS[kind], name))
        else:
            print('(headerless: %d bytes)' % len(data))


def main():
    a = sys.argv[1:]
    if len(a) == 2 and a[0] == 'list':
        list_tape(a[1])
    elif len(a) == 3 and a[0] == 'extract':
        extract(a[1], a[2])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
