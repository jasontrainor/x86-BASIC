#!/usr/bin/env python3
"""Programs and files for the BASIC V2 machine (c64), on a PC.

    tools/c64.py list FILE                .d64 or .t64: what's on it;
                                          .prg or .p00: the program, as text
    tools/c64.py extract IMAGE DIR        .d64 or .t64: each file into DIR, as
                                          the machine's C64 folder keeps them
                                          (NAME.PRG, NAME.SEQ)
    tools/c64.py pack OUT.d64 FILE ...    files onto a new disk image: .prg,
                                          .seq, .p00, and listings (.bas,
                                          .txt), which become programs
    tools/c64.py prg LISTING OUT.prg      a listing as a program file
    tools/c64.py text FILE.prg            a program file as a listing

A program file (.PRG) is its load address and then its bytes; a BASIC
program's are its lines as the machine keeps them, linked from $0801,
with its keywords as tokens. A listing is text, with the characters a
keyboard hasn't written as names in braces ({CLR}, {RVS ON}, {CBM-A},
{3 DOWN}, {147}), as printed listings and tools write them; in one with
lower case letters, capitals are shifted letters (see
machines/c64/listing.c, which LOAD uses).

The disk images are those of the 1541 drive (.d64: 35 tracks, the
directory on track 18) and tape archives (.t64), as emulators keep
them. The formats are documented facts about the machine and these
files; nothing here comes from any ROM.
"""
import os
import re
import struct
import sys

KEYWORDS = ['END', 'FOR', 'NEXT', 'DATA', 'INPUT#', 'INPUT', 'DIM', 'READ', 'LET', 'GOTO',
            'RUN', 'IF', 'RESTORE', 'GOSUB', 'RETURN', 'REM', 'STOP', 'ON', 'WAIT', 'LOAD',
            'SAVE', 'VERIFY', 'DEF', 'POKE', 'PRINT#', 'PRINT', 'CONT', 'LIST', 'CLR', 'CMD',
            'SYS', 'OPEN', 'CLOSE', 'GET', 'NEW', 'TAB(', 'TO', 'FN', 'SPC(', 'THEN', 'NOT',
            'STEP', '+', '-', '*', '/', '^', 'AND', 'OR', '>', '=', '<', 'SGN', 'INT', 'ABS',
            'USR', 'FRE', 'POS', 'SQR', 'RND', 'LOG', 'EXP', 'COS', 'SIN', 'TAN', 'ATN', 'PEEK',
            'LEN', 'STR$', 'VAL', 'ASC', 'CHR$', 'LEFT$', 'RIGHT$', 'MID$', 'GO']
TOKEN_REM, TOKEN_DATA, TOKEN_PRINT = 0x8F, 0x83, 0x99

# Escapes: names for codes (the first for each code is the one written).
NAMES = [
    ('CLR', 147), ('HOME', 19), ('DOWN', 17), ('UP', 145), ('LEFT', 157), ('RIGHT', 29),
    ('RVS ON', 18), ('RVS OFF', 146), ('BLK', 144), ('WHT', 5), ('RED', 28), ('CYN', 159),
    ('PUR', 156), ('GRN', 30), ('BLU', 31), ('YEL', 158), ('ORNG', 129), ('BRN', 149),
    ('LRED', 150), ('GRY1', 151), ('GRY2', 152), ('LGRN', 153), ('LBLU', 154), ('GRY3', 155),
    ('INST', 148), ('DEL', 20), ('F1', 133), ('F3', 134), ('F5', 135), ('F7', 136),
    ('F2', 137), ('F4', 138), ('F6', 139), ('F8', 140), ('PI', 255), ('SHIFT-SPACE', 160),
    ('RETURN', 13), ('LOWER', 14), ('UPPER', 142), ('CBM-*', 223), ('CBM-+', 166),
    ('CBM--', 220), ('CBM-@', 164), ('CBM-POUND', 168), ('SHIFT-*', 192), ('SHIFT-+', 219),
    ('SHIFT--', 221), ('SHIFT-@', 186), ('SHIFT-POUND', 169), ('SHIFT-^', 222),
    # (other spellings, read but not written)
    ('CLEAR', 147), ('RVSON', 18), ('RVON', 18), ('REVERSE ON', 18), ('RVSOFF', 146),
    ('RVOF', 146), ('REVERSE OFF', 146), ('BLACK', 144), ('WHITE', 5), ('CYAN', 159),
    ('PURPLE', 156), ('GREEN', 30), ('BLUE', 31), ('YELLOW', 158), ('ORANGE', 129),
    ('BROWN', 149), ('LT RED', 150), ('LIGHT RED', 150), ('PINK', 150), ('GREY1', 151),
    ('DGRY', 151), ('DARK GREY', 151), ('DARK GRAY', 151), ('GREY2', 152), ('GRY', 152),
    ('GREY', 152), ('GRAY', 152), ('MGRY', 152), ('LT GREEN', 153), ('LIGHT GREEN', 153),
    ('LT BLUE', 154), ('LIGHT BLUE', 154), ('GREY3', 155), ('LGRY', 155),
    ('LIGHT GREY', 155), ('LIGHT GRAY', 155), ('INSERT', 148), ('DELETE', 20),
    ('POUND', 92), ('SPACE', 32), ('SPACES', 32), ('SWLC', 14), ('SWUC', 142),
    ('CBM-^', 222),
]
BY_NAME = {}
for _n, _c in NAMES:
    BY_NAME.setdefault(_n, _c)
NAME_OF = {}
for _n, _c in NAMES:
    NAME_OF.setdefault(_c, _n)
CBM_LETTERS = [176, 191, 188, 172, 177, 187, 165, 180, 162, 181, 161, 182, 167,
               170, 185, 175, 171, 178, 174, 163, 184, 190, 179, 189, 183, 173]
for _i, _c in enumerate(CBM_LETTERS):
    NAME_OF.setdefault(_c, 'CBM-' + chr(65 + _i))


def escape_code(name):
    """The code an escape's name (in capitals) stands for, or None."""
    if re.fullmatch(r'\d+', name):
        return int(name) if int(name) < 256 else None
    if re.fullmatch(r'\$[0-9A-F]{1,2}', name):
        return int(name[1:], 16)
    m = re.fullmatch(r'(?:CBM-|CBM |C=-?|C= )([A-Z])', name)
    if m:
        return CBM_LETTERS[ord(m.group(1)) - 65]
    m = re.fullmatch(r'(?:SHIFT-|SHIFT |S-)([A-Z])', name)
    if m:
        return ord(m.group(1)) + 128
    return BY_NAME.get(name)


def has_lower(text):
    return re.search(r'[a-z]', re.sub(r'\{[^}]*\}', '', text)) is not None


def to_petscii(line, lower, unknown=None):
    """A line of a listing as PETSCII bytes (as machines/c64/listing.c reads
    it). Escapes it doesn't know are kept as text, and listed in unknown."""
    out = bytearray()
    i = 0
    while i < len(line):
        c = line[i]
        if c == '{':
            j = line.find('}', i)
            if j > 0:
                name, count = line[i + 1:j].upper(), 1
                m = re.fullmatch(r'([1-9]\d*) (.+)', name)
                if m and int(m.group(1)) <= 255:
                    count, name = int(m.group(1)), m.group(2)
                code = escape_code(name)
                if code is None and count > 1 and name.endswith('S'):
                    code = escape_code(name[:-1])
                if code is not None:
                    out += bytes([code]) * count
                    i = j + 1
                    continue
                if unknown is not None:
                    unknown.append(line[i:j + 1])
        o = ord(c)
        if 'a' <= c <= 'z':
            o -= 32
        elif 'A' <= c <= 'Z' and lower:
            o += 128
        elif c in '\\£':
            o = 92
        elif c in '`_':
            o = 95
        elif c == '~':
            o = 255
        elif c == '\t':
            o = 32
        elif o < 32 or o > 126:
            i += 1
            continue
        out.append(o)
        i += 1
    return bytes(out)


def crunch(text):
    """A line's PETSCII text (after its number) in tokens, as BASIC V2
    crunches what's typed."""
    out = bytearray()
    i = 0
    while i < len(text):
        c = text[i]
        if c == 0x22:                     # a string: as it is
            j = text.find(b'"', i + 1)
            j = len(text) if j < 0 else j + 1
            out += text[i:j]
            i = j
            continue
        if c == ord('?'):
            out.append(TOKEN_PRINT)
            i += 1
            continue
        if c >= 0x80:
            out.append(c)
            i += 1
            continue
        for k, word in enumerate(KEYWORDS):
            if text[i:i + len(word)] == word.encode():
                tok = 0x80 + k
                out.append(tok)
                i += len(word)
                if tok == TOKEN_REM:
                    out += text[i:]
                    i = len(text)
                elif tok == TOKEN_DATA:
                    quote = False
                    while i < len(text) and (quote or text[i] != ord(':')):
                        if text[i] == 0x22:
                            quote = not quote
                        out.append(text[i])
                        i += 1
                break
        else:
            out.append(c)
            i += 1
    return bytes(out)


def listing_to_prg(text):
    """A listing (str) as a program file's bytes."""
    lower = has_lower(text)
    lines = {}
    for raw in text.replace('\r\n', '\n').replace('\r', '\n').split('\n'):
        m = re.match(r'\s*(\d+) ?(.*)', raw)
        if not m or not raw.strip():
            continue
        num = int(m.group(1))
        if num > 63999:
            raise ValueError('line number over 63999: %s' % raw.strip())
        lines[num] = crunch(to_petscii(m.group(2), lower).lstrip(b' '))
    out, addr = bytearray([0x01, 0x08]), 0x0801
    for num in sorted(lines):
        body = struct.pack('<H', num) + lines[num] + b'\0'
        addr += 2 + len(body)
        out += struct.pack('<H', addr) + body
    return bytes(out + b'\0\0')


def char_text(c, lower):
    """A PETSCII character as a listing writes it."""
    if 65 <= c <= 90:
        return chr(c + 32) if lower else chr(c)
    if 193 <= c <= 218:
        return chr(c - 128) if lower else '{SHIFT-%s}' % chr(c - 128)
    if c == 92:
        return '\\'
    if c == 94:
        return '^'
    if c == 95:
        return '_'
    if c == 255:
        return '~'
    if 32 <= c < 92 or c == 93 or c == 91:
        return chr(c)
    name = NAME_OF.get(c)
    if name is None and 96 <= c <= 127:
        return char_text(c + 96, lower)    # (the same characters as 192-223)
    return '{%s}' % (name or str(c))


def prg_to_listing(data):
    """A program file's bytes (load address and all) as a listing."""
    body = data[2:]
    lines, i = [], 0
    while i + 4 <= len(body) and (body[i] or body[i + 1]):
        num = body[i + 2] | body[i + 3] << 8
        j = body.find(b'\0', i + 4)
        if j < 0:
            j = len(body)
        lines.append((num, body[i + 4:j]))
        i = j + 1
    def shifted(t):                       # (shifted letters in strings or REMs)
        quote = raw = False
        for c in t:
            if c == 0x22:
                quote = not quote
            elif (quote or raw) and 193 <= c <= 218:
                return True
            elif c == TOKEN_REM and not quote:
                raw = True
        return False
    lower = any(shifted(t) for _, t in lines)
    out = []
    for num, t in lines:
        s, quote, raw = [str(num), ' '], False, False
        for c in t:
            if c == 0x22:
                quote = not quote
            if c >= 0x80 and not quote and not raw and c - 0x80 < len(KEYWORDS):
                word = KEYWORDS[c - 0x80]
                s.append(word.lower() if lower else word)
                raw = c == TOKEN_REM
            else:
                s.append(char_text(c, lower))
        out.append(''.join(s))
    return '\n'.join(out) + '\n'


def dos_name(name, ext):
    """The DOS name the machine keeps a file called name under (as
    machines/c64/disk.c makes them)."""
    clean = ''.join(c for c in name.upper() if c.isalnum() and c.isascii() or c in '-_')
    if not clean:
        clean = 'NONAME'
    if len(clean) > 8:
        clean = clean[:6] + clean[-2:]
    return clean + '.' + ext


def petscii_name(raw):
    """A file's name (bytes, padded with $A0) as text."""
    return ''.join(chr(c) if 32 <= c < 127 else chr(c - 128) if 193 <= c <= 218 else '?'
                   for c in raw.rstrip(b'\xa0'))


# --- .d64: the 1541's disk image ------------------------------------------

TRACKS = 35
TYPES = {0: 'DEL', 1: 'SEQ', 2: 'PRG', 3: 'USR', 4: 'REL'}


def sectors_in(track):
    return 21 if track <= 17 else 19 if track <= 24 else 18 if track <= 30 else 17


def offset(track, sector):
    return (sum(sectors_in(t) for t in range(1, track)) + sector) * 256


class D64:
    def __init__(self, data):
        if len(data) < offset(TRACKS + 1, 0):
            raise ValueError('not a .d64 disk image (too short)')
        self.data = data

    def sector(self, t, s):
        o = offset(t, s)
        return self.data[o:o + 256]

    def name(self):
        bam = self.sector(18, 0)
        return petscii_name(bam[144:160]), petscii_name(bam[162:164]) + ' ' + \
            petscii_name(bam[165:167])

    def files(self):
        """(name, type, blocks, bytes) for each file in the directory."""
        t, s, seen, out = 18, 1, set(), []
        while t and (t, s) not in seen and t <= TRACKS:
            seen.add((t, s))
            sec = self.sector(t, s)
            for e in range(8):
                ent = sec[e * 32:e * 32 + 32]
                kind = ent[2]
                if not kind & 0x80 or kind & 7 == 4:   # (deleted or REL: left out)
                    continue
                name = petscii_name(ent[5:21])
                out.append((name, TYPES.get(kind & 7, '?'), ent[30] | ent[31] << 8,
                            self.read_chain(ent[3], ent[4])))
            t, s = sec[0], sec[1]
        return out

    def read_chain(self, t, s):
        data, seen = bytearray(), set()
        while t and (t, s) not in seen and t <= TRACKS:
            seen.add((t, s))
            sec = self.sector(t, s)
            if sec[0]:
                data += sec[2:]
            else:
                data += sec[2:sec[1] + 1]
            t, s = sec[0], sec[1]
        return bytes(data)


def make_d64(files, disk_name='X86 BASIC', disk_id='64'):
    """A new .d64 with files [(name, type 'PRG'|'SEQ'|'USR', bytes)]."""
    img = bytearray(offset(TRACKS + 1, 0))
    used = {t: set() for t in range(1, TRACKS + 1)}
    used[18].update({0, 1})

    def put(t, s, data):
        o = offset(t, s)
        img[o:o + len(data)] = data

    def free_sector():
        for t in list(range(17, 0, -1)) + list(range(19, TRACKS + 1)):
            for s in range(sectors_in(t)):
                if s not in used[t]:
                    used[t].add(s)
                    return t, s
        raise ValueError('the disk is full')

    entries = []
    for name, kind, data in files:
        chunks = [data[i:i + 254] for i in range(0, len(data), 254)] or [b'']
        places = [free_sector() for _ in chunks]
        for k, (chunk, (t, s)) in enumerate(zip(chunks, places)):
            if k + 1 < len(places):
                put(t, s, bytes(places[k + 1]) + chunk)
            else:
                put(t, s, bytes([0, len(chunk) + 1]) + chunk)
        entries.append((name, kind, places[0], len(chunks)))
    # The directory, from 18/1 (eight entries a sector)
    dir_sectors = [1] + [s for s in range(2, sectors_in(18))][:max(0, (len(entries) - 1) // 8)]
    if len(entries) > 8 * 18:
        raise ValueError('too many files for the directory')
    used[18].update(dir_sectors)
    for k, s in enumerate(dir_sectors):
        sec = bytearray(256)
        if k + 1 < len(dir_sectors):
            sec[0], sec[1] = 18, dir_sectors[k + 1]
        else:
            sec[1] = 0xFF
        for e, (name, kind, (t, ss), blocks) in enumerate(entries[k * 8:k * 8 + 8]):
            ent = bytearray(32)
            ent[2] = 0x80 | {'SEQ': 1, 'PRG': 2, 'USR': 3}[kind]
            ent[3], ent[4] = t, ss
            ent[5:21] = name.upper().encode('ascii', 'replace')[:16].ljust(16, b'\xa0')
            ent[30], ent[31] = blocks & 0xFF, blocks >> 8
            sec[e * 32:e * 32 + 32] = ent
        put(18, s, sec)
    # The BAM
    bam = bytearray(256)
    bam[0], bam[1], bam[2] = 18, 1, 0x41
    for t in range(1, TRACKS + 1):
        bits = sum(1 << s for s in range(sectors_in(t)) if s not in used[t])
        bam[4 * t:4 * t + 4] = bytes([sectors_in(t) - len(used[t]), bits & 0xFF,
                                      bits >> 8 & 0xFF, bits >> 16 & 0xFF])
    bam[144:160] = disk_name.upper().encode()[:16].ljust(16, b'\xa0')
    bam[160:162] = b'\xa0\xa0'
    bam[162:164] = disk_id.upper().encode()[:2].ljust(2, b'0')
    bam[164] = 0xA0
    bam[165:167] = b'2A'
    bam[167:171] = b'\xa0' * 4
    put(18, 0, bam)
    return bytes(img)


# --- .t64: tape archives ----------------------------------------------------

def t64_files(data):
    if not data.startswith(b'C64'):
        raise ValueError('not a .t64 tape archive')
    count = struct.unpack_from('<H', data, 36)[0] or 1
    out = []
    for k in range(count):
        e = data[64 + 32 * k:96 + 32 * k]
        if len(e) < 32 or e[0] == 0:
            continue
        start, end, off = struct.unpack_from('<HHxxI', e, 2)
        body = data[off:off + (end - start)]
        if end <= start or len(body) < end - start:   # (often wrong: to the file's end)
            body = data[off:]
        out.append((petscii_name(e[16:32].rstrip(b' ')), 'PRG', (len(body) + 255) // 254,
                    struct.pack('<H', start) + body))
    return out


# --- the commands -----------------------------------------------------------

def image_files(path):
    data = open(path, 'rb').read()
    if path.lower().endswith('.t64') or data.startswith(b'C64S') or data.startswith(b'C64 '):
        return t64_files(data)
    return D64(data).files()


def program_bytes(path):
    """A file given to pack or text: (its name, type, bytes)."""
    base, ext = os.path.splitext(os.path.basename(path))
    ext = ext.lower()
    data = open(path, 'rb').read()
    if ext in ('.bas', '.txt'):
        return base, 'PRG', listing_to_prg(data.decode('latin-1'))
    if ext == '.p00' or data.startswith(b'C64File'):
        return petscii_name(data[8:24].rstrip(b'\0')), 'PRG', data[26:]
    if ext == '.seq':
        return base, 'SEQ', data
    return base, 'USR' if ext == '.usr' else 'PRG', data


def main():
    args = sys.argv[1:]
    if not args or args[0] not in ('list', 'extract', 'pack', 'prg', 'text'):
        sys.exit(__doc__)
    cmd = args[0]
    try:
        if cmd == 'list' and len(args) == 2:
            low = args[1].lower()
            if low.endswith('.prg') or low.endswith('.p00'):
                sys.stdout.write(prg_to_listing(program_bytes(args[1])[2]))
                return
            if low.endswith('.d64'):
                name, did = D64(open(args[1], 'rb').read()).name()
                print('0 "%-16s" %s' % (name, did))
            for name, kind, blocks, _ in image_files(args[1]):
                print('%-5d "%s"%s %s' % (blocks, name, ' ' * (16 - len(name)), kind))
        elif cmd == 'extract' and len(args) == 3:
            os.makedirs(args[2], exist_ok=True)
            for name, kind, _, data in image_files(args[1]):
                out = dos_name(name, 'SEQ' if kind == 'SEQ' else 'PRG')
                with open(os.path.join(args[2], out), 'wb') as f:
                    f.write(data)
                print('%s -> %s' % (name, out))
        elif cmd == 'pack' and len(args) >= 3:
            files = [program_bytes(p) for p in args[2:]]
            with open(args[1], 'wb') as f:
                f.write(make_d64(files))
            for name, kind, data in files:
                print('%-5d "%s"%s %s' % ((len(data) + 253) // 254, name.upper(),
                                          ' ' * (16 - len(name)), kind))
        elif cmd == 'prg' and len(args) == 3:
            with open(args[2], 'wb') as f:
                f.write(listing_to_prg(open(args[1], 'rb').read().decode('latin-1')))
        elif cmd == 'text' and len(args) == 2:
            sys.stdout.write(prg_to_listing(program_bytes(args[1])[2]))
        else:
            sys.exit(__doc__)
    except (ValueError, OSError) as e:
        sys.exit('c64.py: %s' % e)


if __name__ == '__main__':
    main()
