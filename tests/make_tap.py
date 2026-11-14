#!/usr/bin/env python3
"""Make a small Spectrum tape file for the tests of tools/tap.py: a
program "demo" (autostart line 10), as the Spectrum stores it, with
keywords, numbers and their hidden values, a UDG, the pound sign, a block
graphic, an INK control code and a REM; then bytes "udg" (CODE 65368,8).

    tests/make_tap.py OUT.tap|OUT.tzx
"""
import struct
import sys


def number(text):
    """A number as typed, then 14 and its value (here 5 zero bytes: the
    value isn't read back)."""
    return text.encode() + b'\x0e' + bytes(5)


PRINT, GOTO, REM, AT, INK, LINE_END = 0xF5, 0xEC, 0xEA, 0xAC, 0xD9, 0x0D
lines = {
    10: bytes([PRINT]) + b'"hi\x90\x60";' + bytes([AT]) + number('0') + b',' + number('1') +
        b';"\x8f\x10\x02x": ' + bytes([GOTO]) + number('10'),
    20: bytes([REM]) + b' keep \x7f ' + bytes([PRINT]),
}
prog = b''
for num, text in sorted(lines.items()):
    text += bytes([LINE_END])
    prog += struct.pack('>H', num) + struct.pack('<H', len(text)) + text
variables = b'\x61' + bytes(5)            # (a variable, after the program: not a line)


def block(flag, body):
    x = flag
    for c in body:
        x ^= c
    return bytes([flag]) + body + bytes([x])


def header(kind, name, length, p1, p2):
    return block(0, bytes([kind]) + name.ljust(10).encode() + struct.pack('<HHH', length, p1, p2))


blocks = [
    header(0, 'demo', len(prog + variables), 10, len(prog)), block(0xFF, prog + variables),
    header(3, 'udg', 8, 65368, 32768), block(0xFF, bytes([0x3C, 0x42, 0x81, 0, 0, 0x81, 0x42, 0x3C])),
]
out = sys.argv[1]
if out.endswith('.tzx'):
    data = b'ZXTape!\x1a\x01\x14'
    data += b'\x30' + bytes([5]) + b'tests'      # a text description, to skip
    for b in blocks:
        data += b'\x10' + struct.pack('<HH', 1000, len(b)) + b
else:
    data = b''.join(struct.pack('<H', len(b)) + b for b in blocks)
open(out, 'wb').write(data)
