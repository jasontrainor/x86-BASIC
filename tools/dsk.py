#!/usr/bin/env python3
"""Files from 140KB DOS 3.3 disk images (.dsk, .do: DOS sector order), for
the Apple-style machines.

    tools/dsk.py list DISK.dsk           what's on it
    tools/dsk.py extract DISK.dsk DIR    its FP BASIC programs, text files
                                         and binary files, as saved files
                                         (NAME.SAV) in DIR
    tools/dsk.py make DISK.dsk TYPE:NAME=FILE ...
                                         make an image (for tests): A an FP
                                         BASIC listing (tokenized here), T a
                                         text file, B binary (B@ADDR:NAME=FILE)

Extracted files keep their DOS names (up to 15 characters here), so after
copying DIR's files to the stick's A2FP folder, RUN STAR TREK finds the
program and BLOAD and OPEN find the others. FP BASIC programs are turned
back into listings. Integer BASIC programs (type I) aren't converted: they
are listed and skipped.

The disk layout is the one DOS 3.3 documents: the VTOC at track 17,
sector 0, then the catalog sectors, each file's track/sector lists and
its sectors. The token values are FP BASIC's (machines/a2fp/fpbasic.c
lists the keywords in the same order), with its operators among them.
"""
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import savefile

SECTOR, SECTORS, TRACKS = 256, 16, 35
TYPES = {0x00: 'T', 0x01: 'I', 0x02: 'A', 0x04: 'B', 0x08: 'S', 0x10: 'R', 0x20: 'a', 0x40: 'b'}
FILE_PROGRAM, FILE_BYTES, FILE_TEXT = 0, 1, 4

TOKENS = [
    'END', 'FOR', 'NEXT', 'DATA', 'INPUT', 'DEL', 'DIM', 'READ', 'GR', 'TEXT', 'PR#', 'IN#',
    'CALL', 'PLOT', 'HLIN', 'VLIN', 'HGR2', 'HGR', 'HCOLOR=', 'HPLOT', 'DRAW', 'XDRAW',
    'HTAB', 'HOME', 'ROT=', 'SCALE=', 'SHLOAD', 'TRACE', 'NOTRACE', 'NORMAL', 'INVERSE',
    'FLASH', 'COLOR=', 'POP', 'VTAB', 'HIMEM:', 'LOMEM:', 'ONERR', 'RESUME', 'RECALL',
    'STORE', 'SPEED=', 'LET', 'GOTO', 'RUN', 'IF', 'RESTORE', '&', 'GOSUB', 'RETURN', 'REM',
    'STOP', 'ON', 'WAIT', 'LOAD', 'SAVE', 'DEF', 'POKE', 'PRINT', 'CONT', 'LIST', 'CLEAR',
    'GET', 'NEW', 'TAB(', 'TO', 'FN', 'SPC(', 'THEN', 'AT', 'NOT', 'STEP', '+', '-', '*', '/',
    '^', 'AND', 'OR', '>', '=', '<', 'SGN', 'INT', 'ABS', 'USR', 'FRE', 'SCRN(', 'PDL', 'POS',
    'SQR', 'RND', 'LOG', 'EXP', 'COS', 'SIN', 'TAN', 'ATN', 'PEEK', 'LEN', 'STR$', 'VAL',
    'ASC', 'CHR$', 'LEFT$', 'RIGHT$', 'MID$',
]
T_REM, T_DATA = 0x80 + TOKENS.index('REM'), 0x80 + TOKENS.index('DATA')


def sector(img, t, s):
    off = (t * SECTORS + s) * SECTOR
    return img[off:off + SECTOR]


def catalog(img):
    """[(name, type letter, locked, data)] for each file on the disk."""
    vtoc = sector(img, 17, 0)
    t, s = vtoc[1], vtoc[2]
    files, seen = [], set()
    while t and (t, s) not in seen and t < TRACKS:
        seen.add((t, s))
        cat = sector(img, t, s)
        for e in range(7):
            entry = cat[0x0B + e * 35:0x0B + (e + 1) * 35]
            if entry[0] in (0, 0xFF):
                continue
            name = bytes(c & 0x7F for c in entry[3:33]).decode('latin-1').rstrip()
            kind = TYPES.get(entry[2] & 0x7F, '?')
            files.append((name, kind, bool(entry[2] & 0x80), file_data(img, entry[0], entry[1])))
        t, s = cat[1], cat[2]
    return files


def file_data(img, t, s):
    data, seen = bytearray(), set()
    while t and (t, s) not in seen and t < TRACKS:
        seen.add((t, s))
        ts = sector(img, t, s)
        for i in range(0x0C, SECTOR, 2):
            dt, ds = ts[i], ts[i + 1]
            if dt == 0 and ds == 0:       # (the end; sparse files aren't kept)
                return bytes(data)
            data += sector(img, dt, ds)
        t, s = ts[1], ts[2]
    return bytes(data)


def detokenize(prog):
    """An FP BASIC program (as saved: its length, then its lines) as a listing."""
    length = struct.unpack_from('<H', prog)[0]
    body = prog[2:2 + length]
    lines, pos = [], 0
    while pos + 4 <= len(body):
        link, num = struct.unpack_from('<HH', body, pos)
        if link == 0:
            break
        pos += 4
        text, raw = '%d ' % num, None
        while pos < len(body) and body[pos]:
            c = body[pos]
            pos += 1
            if raw is None and c >= 0x80 and c - 0x80 < len(TOKENS):
                word = TOKENS[c - 0x80]
                text += word if not word[0].isalpha() else ' %s ' % word
                if c == T_REM:
                    raw = 'rem'
                elif c == T_DATA:
                    raw = 'data'
            else:
                ch = chr(c & 0x7F)
                if raw == 'data' and ch == ':':
                    raw = None
                text += ch
        pos += 1
        lines.append(text.rstrip())
    return '\n'.join(lines) + '\n'


def text_file(data):
    out = bytearray()
    for c in data:
        if c == 0:
            break
        c &= 0x7F
        out.append(10 if c == 13 else c)
    return bytes(out)


def short(name):
    return name[:15]


def extract(path, out_dir):
    img = open(path, 'rb').read()
    os.makedirs(out_dir, exist_ok=True)
    used = set()
    for name, kind, locked, data in catalog(img):
        if kind == 'A':
            blob = savefile.pack(short(name), detokenize(data).encode('latin-1'), ord('A'),
                                 FILE_PROGRAM)
        elif kind == 'T':
            blob = savefile.pack(short(name), text_file(data), 0, FILE_TEXT)
        elif kind == 'B':
            addr, length = struct.unpack_from('<HH', data)
            blob = savefile.pack(short(name), data[4:4 + length], addr, FILE_BYTES)
        else:
            print('skipped %-30s (type %s: not converted)' % (name, kind))
            continue
        for k in range(36):
            disk = savefile.disk_name(short(name), k)
            if disk not in used:
                break
        used.add(disk)
        with open(os.path.join(out_dir, disk), 'wb') as f:
            f.write(blob)
        if len(name) > 15:
            print('%s: named %s here (15 characters at most)' % (name, short(name)))
        print('%-12s %s %s' % (disk, kind, short(name)))


def list_disk(path):
    img = open(path, 'rb').read()
    for name, kind, locked, data in catalog(img):
        print('%s%s %03d %s' % ('*' if locked else ' ', kind, len(data) // SECTOR + 1, name))


# --- Making an image (for tests) --------------------------------------------

def tokenize(listing):
    """A listing as FP BASIC saves it, crunched as machines/a2fp does."""
    body, addr = bytearray(), 0x801
    for line in listing.splitlines():
        line = line.strip()
        if not line or not line[0].isdigit():
            continue
        i = 0
        while i < len(line) and line[i].isdigit():
            i += 1
        num, s = int(line[:i]), line[i:]
        toks, j, raw = bytearray(), 0, None
        while j < len(s):
            c = s[j]
            if raw == 'rem' or (raw == 'data' and c != ':') or c == '"':
                if c == '"' and raw is None:
                    k = s.find('"', j + 1)
                    k = len(s) if k < 0 else k + 1
                    toks += s[j:k].encode('latin-1')
                    j = k
                    continue
                toks.append(ord(c))
                j += 1
                continue
            raw = None
            if c == ' ':
                j += 1
                continue
            for t, word in enumerate(TOKENS):
                k, w = j, 0
                while w < len(word) and k < len(s):
                    if w and s[k] == ' ':
                        k += 1
                        continue
                    if s[k].upper() != word[w]:
                        break
                    k += 1
                    w += 1
                if w == len(word):
                    if word == 'AT':
                        rest = s[k:].lstrip().upper()
                        if rest.startswith('N'):
                            continue
                        if rest.startswith('O'):
                            break
                    toks.append(0x80 + t)
                    j = k
                    raw = 'rem' if word == 'REM' else 'data' if word == 'DATA' else None
                    break
            else:
                toks.append(ord(c.upper()))
                j += 1
        addr += 4 + len(toks) + 1
        body += struct.pack('<HH', addr, num) + toks + b'\0'
    body += b'\0\0'
    return struct.pack('<H', len(body)) + bytes(body)


def make(path, specs):
    img = bytearray(TRACKS * SECTORS * SECTOR)
    free = [(t, s) for t in range(1, TRACKS) if t != 17 for s in range(SECTORS)]
    vtoc = bytearray(SECTOR)
    vtoc[1], vtoc[2], vtoc[3], vtoc[6] = 17, 15, 3, 254
    vtoc[0x27], vtoc[0x34], vtoc[0x35] = 122, TRACKS, SECTORS
    img[17 * SECTORS * SECTOR:17 * SECTORS * SECTOR + SECTOR] = vtoc
    cat = bytearray(SECTOR)
    for n, spec in enumerate(specs):
        kind_part, rest = spec.split(':', 1)
        name, src = rest.split('=', 1)
        data = open(src, 'rb').read()
        kind = kind_part[0]
        if kind == 'A':
            data, ftype = tokenize(data.decode('latin-1')), 0x02
        elif kind == 'T':
            data, ftype = bytes((13 if c == 10 else c) | 0x80 for c in data) + b'\0', 0x00
        else:
            addr = int(kind_part[2:], 0)
            data, ftype = struct.pack('<HH', addr, len(data)) + data, 0x04
        sectors = [free.pop(0) for _ in range((len(data) + SECTOR - 1) // SECTOR)]
        tslist = free.pop(0)
        ts = bytearray(SECTOR)
        for i, (t, s) in enumerate(sectors):
            ts[0x0C + 2 * i], ts[0x0D + 2 * i] = t, s
            off = (t * SECTORS + s) * SECTOR
            chunk = data[i * SECTOR:(i + 1) * SECTOR]
            img[off:off + len(chunk)] = chunk
        off = (tslist[0] * SECTORS + tslist[1]) * SECTOR
        img[off:off + SECTOR] = ts
        e = 0x0B + n * 35
        cat[e], cat[e + 1], cat[e + 2] = tslist[0], tslist[1], ftype
        cat[e + 3:e + 33] = bytes(ord(c) | 0x80 for c in name.ljust(30)[:30])
        cat[e + 33:e + 35] = struct.pack('<H', len(sectors) + 1)
    off = (17 * SECTORS + 15) * SECTOR
    img[off:off + SECTOR] = cat
    open(path, 'wb').write(img)


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'list':
        list_disk(a[1])
    elif len(a) == 3 and a[0] == 'extract':
        extract(a[1], a[2])
    elif len(a) >= 2 and a[0] == 'make':
        make(a[1], a[2:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
