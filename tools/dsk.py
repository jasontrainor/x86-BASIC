#!/usr/bin/env python3
"""Files from 140KB DOS 3.3 disk images (.dsk, .do: DOS sector order), for
the Apple-style machines.

    tools/dsk.py list DISK.dsk           what's on it
    tools/dsk.py extract DISK.dsk DIR    its programs (both BASICs'), text
                                         files and binary files, as saved
                                         files (NAME.SAV) in DIR
    tools/dsk.py pack DISK.dsk FILE ...  a disk image of files from the stick,
                                         for an emulator or a real disk:
                                         saved files (NAME.SAV), listings
                                         (.BAS) and text files; FP BASIC
                                         programs, not Integer BASIC ones
    tools/dsk.py make DISK.dsk TYPE:NAME=FILE ...
                                         make an image (for tests): A an FP
                                         BASIC listing, I an Integer BASIC
                                         one (tokenized here), T a text
                                         file, B binary (B@ADDR:NAME=FILE)

Extracted files keep their DOS names (up to 30 characters), so after
copying DIR's files to the stick's A2FP folder, RUN STAR TREK finds the
program and BLOAD and OPEN find the others. Programs are turned back into
listings: FP BASIC ones (type A) for the A2FP folder, Integer BASIC ones
(type I) for A2INT (or A1BASIC).

The disk layout is the one DOS 3.3 documents: the VTOC at track 17,
sector 0, then the catalog sectors, each file's track/sector lists and
its sectors. The token values are FP BASIC's (machines/a2fp/fpbasic.c
lists the keywords in the same order), with its operators among them.

Integer BASIC saves a line as its length, its number and its tokens, then
1. A token below 80h is a keyword or punctuation; INT_TOKENS gives each
one's text (several have the same text: which one is used depends on
where it is, as with the commas of PRINT, of POKE and of arrays).
Characters (names, the insides of strings and REMs) have bit 7 set; a
digit with bit 7 set where a name can't be is a number, its value in the
two bytes that follow.
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

INT_TOKENS = [
    'HIMEM:', None, '_', ':', 'LOAD', 'SAVE', 'CON', 'RUN', 'RUN', 'DEL', ',', 'NEW', 'CLR',
    'AUTO', ',', 'MAN', 'HIMEM:', 'LOMEM:', '+', '-', '*', '/', '=', '#', '>=', '>', '<=',
    '<>', '<', 'AND', 'OR', 'MOD', '^', '+', '(', ',', 'THEN', 'THEN', ',', ',', '"', '"',
    '(', '!', '!', '(', 'PEEK', 'RND', 'SGN', 'ABS', 'PDL', 'RNDX', '(', '+', '-', 'NOT',
    '(', '=', '#', 'LEN(', 'ASC(', 'SCRN(', ',', '(', '$', '$', '(', ',', ',', ';', ';', ';',
    ',', ',', ',', 'TEXT', 'GR', 'CALL', 'DIM', 'DIM', 'TAB', 'END', 'INPUT', 'INPUT', 'INPUT',
    'FOR', '=', 'TO', 'STEP', 'NEXT', ',', 'RETURN', 'GOSUB', 'REM', 'LET', 'GOTO', 'IF',
    'PRINT', 'PRINT', 'PRINT', 'POKE', ',', 'COLOR=', 'PLOT', ',', 'HLIN', ',', 'AT', 'VLIN',
    ',', 'AT', 'VTAB', '=', '=', ')', ')', 'LIST', ',', 'LIST', 'POP', 'NODSP', 'NODSP',
    'NOTRACE', 'DSP', 'DSP', 'TRACE', 'PR#', 'IN#',
]
INT_EOL, INT_QUOTE, INT_UNQUOTE, INT_REM = 0x01, 0x28, 0x29, 0x5D


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


def detokenize_int(prog):
    """An Integer BASIC program (as saved: its length, then its lines) as a
    listing."""
    length = struct.unpack_from('<H', prog)[0]
    body = prog[2:2 + length]
    lines, pos = [], 0
    while pos + 3 < len(body):
        size, num = body[pos], struct.unpack_from('<H', body, pos + 1)[0]
        if size < 4:
            break
        line, pos = body[pos + 3:pos + size], pos + size
        text, i, in_name = '%d ' % num, 0, False
        while i < len(line) and line[i] != INT_EOL:
            c = line[i]
            i += 1
            if c >= 0x80:
                ch = chr(c & 0x7F)
                if not in_name and ch.isdigit():      # a number
                    if i + 2 > len(line):
                        break
                    text += str(struct.unpack_from('<H', line, i)[0])
                    i += 2
                    continue
                in_name = ch.isalnum()
                text += ch
                continue
            in_name = False
            if c == INT_QUOTE:                        # a string, to its end
                text += '"'
                while i < len(line) and line[i] >= 0x80:
                    text += chr(line[i] & 0x7F)
                    i += 1
                if i < len(line) and line[i] == INT_UNQUOTE:
                    i += 1
                text += '"'
            elif c == INT_REM:                        # a REM, to the end
                text += ' REM '
                while i < len(line) and line[i] != INT_EOL:
                    text += chr(line[i] & 0x7F)
                    i += 1
            else:
                word = INT_TOKENS[c] or ''
                text += ' %s ' % word if word[0:1].isalpha() else word
        lines.append(' '.join(text.split(' ')).replace('  ', ' ').rstrip())
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
    return name[:savefile.LONGEST_NAME]


def extract(path, out_dir):
    img = open(path, 'rb').read()
    os.makedirs(out_dir, exist_ok=True)
    used = set()
    for name, kind, locked, data in catalog(img):
        if kind == 'A':
            blob = savefile.pack(short(name), detokenize(data).encode('latin-1'), ord('A'),
                                 FILE_PROGRAM)
        elif kind == 'I':
            blob = savefile.pack(short(name), detokenize_int(data).encode('latin-1'), ord('I'),
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


def tokenize_int(listing):
    """A listing as Integer BASIC saves it. Where several tokens have the
    same text, this takes the first: enough for detokenize_int (and the
    tests), though not always the one Integer BASIC would choose."""
    words = sorted({w for w in INT_TOKENS if w}, key=len, reverse=True)
    body = bytearray()
    for line in listing.splitlines():
        line = line.strip()
        if not line or not line[0].isdigit():
            continue
        i = 0
        while i < len(line) and line[i].isdigit():
            i += 1
        num, s, j, toks, in_name = int(line[:i]), line[i:], 0, bytearray(), False
        while j < len(s):
            c = s[j]
            if c == ' ':
                j, in_name = j + 1, False
                continue
            if c == '"':
                k = s.find('"', j + 1)
                k = len(s) if k < 0 else k
                toks += bytes([INT_QUOTE]) + bytes(ord(x) | 0x80 for x in s[j + 1:k])
                toks.append(INT_UNQUOTE)
                j, in_name = k + 1, False
                continue
            if c.isdigit() and not in_name:
                k = j
                while k < len(s) and s[k].isdigit():
                    k += 1
                toks += bytes([0xB0 + int(c)]) + struct.pack('<H', int(s[j:k]))
                j = k
                continue
            if not in_name:
                for w in words:
                    if s[j:j + len(w)].upper() == w:
                        break
                else:
                    w = None
                if w == 'REM':
                    toks.append(INT_REM)
                    toks += bytes(ord(x) | 0x80 for x in s[j + 3:])
                    j = len(s)
                    continue
                if w:
                    toks.append(INT_TOKENS.index(w))
                    j += len(w)
                    continue
            if c.isalnum():
                toks.append(ord(c.upper()) | 0x80)
                in_name = True
            else:
                toks.append(INT_TOKENS.index(c))
                in_name = False
            j += 1
        toks.append(INT_EOL)
        body += bytes([3 + len(toks)]) + struct.pack('<H', num) + toks
    return struct.pack('<H', len(body)) + bytes(body)


def write_image(path, entries):
    """A DOS 3.3 data disk holding entries [(name, type byte, data)], laid
    out as DOS lays one out: the VTOC at track 17 sector 0 with the map of
    free sectors, a catalog of 15 sectors (7 files each) down track 17,
    and each file's track/sector lists (122 sectors each) and sectors,
    from track 18 out. Tracks 0-2 are left for the DOS image, as on a disk
    DOS has INITed (the disk doesn't start DOS, though: it's for data)."""
    img = bytearray(TRACKS * SECTORS * SECTOR)
    put = lambda t, s, data: img.__setitem__(slice((t * SECTORS + s) * SECTOR,
                                                   (t * SECTORS + s) * SECTOR + len(data)), data)
    order = [t for t in range(18, TRACKS)] + [t for t in range(16, 2, -1)]
    free = [(t, s) for t in order for s in range(SECTORS - 1, -1, -1)]
    used = {(17, s) for s in range(SECTORS)} | {(t, s) for t in range(3) for s in range(SECTORS)}
    if len(entries) > 15 * 7:
        raise SystemExit('%d files: a DOS 3.3 disk holds 105' % len(entries))
    cats = [bytearray(SECTOR) for _ in range(15)]  # sectors 15 ... 1 of track 17
    for k, cat in enumerate(cats):
        if k < 14:
            cat[1], cat[2] = 17, 14 - k
    for n, (name, ftype, data) in enumerate(entries):
        need = (len(data) + SECTOR - 1) // SECTOR or 1
        lists = (need + 121) // 122
        if need + lists > len(free):
            raise SystemExit('%s: the disk is full' % name)
        ts_sectors = [free.pop(0) for _ in range(lists)]
        sectors = [free.pop(0) for _ in range(need)]
        used.update(ts_sectors + sectors)
        for i, (t, sec) in enumerate(sectors):
            put(t, sec, data[i * SECTOR:(i + 1) * SECTOR])
        for j, (t, sec) in enumerate(ts_sectors):
            ts = bytearray(SECTOR)
            if j + 1 < lists:
                ts[1], ts[2] = ts_sectors[j + 1]
            ts[5:7] = struct.pack('<H', j * 122)
            for i, (dt, ds) in enumerate(sectors[j * 122:(j + 1) * 122]):
                ts[0x0C + 2 * i], ts[0x0D + 2 * i] = dt, ds
            put(t, sec, ts)
        cat, e = cats[n // 7], 0x0B + (n % 7) * 35
        cat[e], cat[e + 1], cat[e + 2] = ts_sectors[0][0], ts_sectors[0][1], ftype
        cat[e + 3:e + 33] = bytes(ord(c) | 0x80 for c in name.upper().ljust(30)[:30])
        cat[e + 33:e + 35] = struct.pack('<H', need + lists)
    for k, cat in enumerate(cats):
        put(17, 15 - k, cat)
    vtoc = bytearray(SECTOR)
    vtoc[1], vtoc[2], vtoc[3], vtoc[6] = 17, 15, 3, 254
    vtoc[0x27] = 122
    vtoc[0x30], vtoc[0x31] = 18, 1        # (where DOS goes on allocating, and which way)
    vtoc[0x34], vtoc[0x35], vtoc[0x36], vtoc[0x37] = TRACKS, SECTORS, 0, 1
    for t in range(TRACKS):               # a bit a sector, set if it's free
        bits = sum(1 << sec for sec in range(SECTORS) if (t, sec) not in used)
        vtoc[0x38 + 4 * t] = bits >> 8
        vtoc[0x39 + 4 * t] = bits & 0xFF
    put(17, 0, vtoc)
    open(path, 'wb').write(img)


def text_data(text):
    """A text file as DOS keeps one: characters with bit 7 set, lines ending
    in a return, then a 0."""
    return bytes((13 if c == 10 else c) | 0x80 for c in text.replace(b'\r\n', b'\n')) + b'\0'


def make(path, specs):
    entries = []
    for spec in specs:
        kind_part, rest = spec.split(':', 1)
        name, src = rest.split('=', 1)
        data = open(src, 'rb').read()
        kind = kind_part[0]
        if kind == 'A':
            entries.append((name, 0x02, tokenize(data.decode('latin-1'))))
        elif kind == 'I':
            entries.append((name, 0x01, tokenize_int(data.decode('latin-1'))))
        elif kind == 'T':
            entries.append((name, 0x00, text_data(data)))
        else:
            addr = int(kind_part[2:], 0)
            entries.append((name, 0x04, struct.pack('<HH', addr, len(data)) + data))
    write_image(path, entries)


def pack(path, sources):
    """A disk image of files from the stick (A2FP/ or A2INT/): saved files
    (NAME.SAV) by their saved names, and listings and text files (.BAS,
    .TXT) by their file names, without the .BAS or .TXT. FP BASIC programs are tokenized as FP BASIC
    does; Integer BASIC ones are left out (its tokens depend on its
    grammar, which this doesn't have: type them in, or EXEC them as text)."""
    entries = []
    for src in sources:
        raw = open(src, 'rb').read()
        base = os.path.basename(src)
        saved = savefile.unpack(raw)
        if saved:
            name, data, param, kind = saved
            if kind == FILE_PROGRAM and param == ord('I'):
                print('%s: an Integer BASIC program, left out (see tools/dsk.py)' % name)
                continue
            if kind == FILE_PROGRAM:
                entries.append((name, 0x02, tokenize(data.decode('latin-1'))))
            elif kind == FILE_TEXT:
                entries.append((name, 0x00, text_data(data)))
            elif kind == FILE_BYTES:
                entries.append((name, 0x04, struct.pack('<HH', param, len(data)) + data))
            else:
                print('%s: not a kind of file DOS has, left out' % name)
                continue
        elif base.upper().endswith('.BAS'):           # (named without the .BAS)
            entries.append((base[:-4], 0x02, tokenize(raw.decode('latin-1'))))
        else:
            name = base[:-4] if base.upper().endswith('.TXT') else base
            entries.append((name, 0x00, text_data(raw)))
        print('%s %s' % (TYPES[entries[-1][1]], entries[-1][0]))
    write_image(path, entries)


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'list':
        list_disk(a[1])
    elif len(a) == 3 and a[0] == 'extract':
        extract(a[1], a[2])
    elif len(a) >= 2 and a[0] == 'make':
        make(a[1], a[2:])
    elif len(a) >= 3 and a[0] == 'pack':
        pack(a[1], a[2:])
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
