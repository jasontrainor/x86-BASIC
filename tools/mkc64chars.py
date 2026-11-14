#!/usr/bin/env python3
"""
The BASIC V2 machine's two character sets (machines/c64hw/chars.c),
drawn for this project: 128 characters each, by screen code, as the
machine's programs expect them to be laid out (which code is which
character is a fact about the machine; the drawings are this project's).
Codes 128-255 are the same characters reversed, which the machine works
out itself.

    set 0, upper case and graphics: 0 @, 1-26 A-Z, 27 [, 28 pound sign,
        29 ], 30 up arrow, 31 left arrow, 32-63 as ASCII, 64-127 the
        graphics characters (lines, corners, blocks, card suits ...)
    set 1, lower and upper case: 1-26 a-z, 65-90 A-Z, and the same
        graphics otherwise, but for four

Letters, digits and punctuation are the project's own font
(tools/mkfont.py). Lines, corners, blocks and checkerboards are made from
rules below; the rest (card suits, the ball, pi, the arrows ...) are drawn
here, as 8 rows of 8 ('#' ink).

Run this to regenerate machines/c64hw/chars.c.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkfont import GLYPHS, glyph_rows   # noqa: E402

HAND = {
    'spade':   '...##.../..####../.######./########/########/...##.../..####../........',
    'heart':   '.##..##./########/########/########/.######./..####../...##.../........',
    'club':    '...##.../..####../..####../##.##.##/########/##.##.##/...##.../..####..',
    'diamond': '...##.../..####../.######./########/.######./..####../...##.../........',
    'ball':    '..####../.######./########/########/########/########/.######./..####..',
    'circle':  '..####../.##..##./##....##/##....##/##....##/##....##/.##..##./..####..',
    'pi':      '......../......../######../.#..#.../.#..#.../.#..#.../.#..##../........',
    'up':      '...#..../..###.../.#.#.#../...#..../...#..../...#..../...#..../........',
    'left':    '......../...#..../..#...../.######./..#...../...#..../......../........',
    'check':   '......../.......#/......##/#....##./##..##../.####.../..##..../........',
}


def art(s):
    rows = [0] * 8
    for r, line in enumerate(s.split('/')[:8]):
        for c, px in enumerate(line[:8]):
            if px == '#':
                rows[r] |= 0x80 >> c
    return rows


def hline(*rows):
    return [0xFF if r in rows else 0 for r in range(8)]


def vline(*cols):
    b = 0
    for c in cols:
        b |= 0x80 >> c
    return [b] * 8


def block(x0, y0, x1, y1):
    """Ink in columns x0-x1 and rows y0-y1 (inclusive)."""
    b = 0
    for c in range(x0, x1 + 1):
        b |= 0x80 >> c
    return [b if y0 <= r <= y1 else 0 for r in range(8)]


def union(*glyphs):
    rows = [0] * 8
    for g in glyphs:
        rows = [a | b for a, b in zip(rows, g)]
    return rows


def checker(phase=0):
    return [0xAA if (r + phase) % 2 == 0 else 0x55 for r in range(8)]


def masked(glyph, mask):
    return [glyph[r] & mask[r] for r in range(8)]


def triangle(upper_left):
    """The half of the cell above its diagonal: upper left or upper right."""
    rows = []
    for r in range(8):
        n = 8 - r
        rows.append((0xFF << (8 - n)) & 0xFF if upper_left else 0xFF >> (8 - n))
    return rows


def stripes(slope):
    rows = []
    for r in range(8):
        b = 0
        for c in range(8):
            if (c + slope * r) % 4 in (0, 1):
                b |= 0x80 >> c
        rows.append(b)
    return rows


def arc(corner):
    """A quarter circle joining the centre lines (2 dots thick): the corner
    of the cell it bends round, 'tl' (as in the top left of a box) ..."""
    # Drawn for the top left of a box; the others are mirror images.
    tl = art('......../......../......../.....###/....####/...###../...##.../...##...')
    rows = tl
    if corner in ('tr', 'br'):
        rows = [int('{:08b}'.format(b)[::-1], 2) for b in rows]
    if corner in ('bl', 'br'):
        rows = rows[::-1]
    # (mirroring puts the lines one dot off the centre: move them back)
    if corner in ('tr', 'br'):
        rows = [(b << 1) & 0xFF for b in rows]
    if corner in ('bl', 'br'):
        rows = rows[1:] + [rows[0]]
    return rows


def diagonal(down):
    rows = []
    for r in range(8):
        c = r if down else 7 - r
        rows.append(0x80 >> c | (0x80 >> (c + 1) if c < 7 else 0))
    return rows


H = hline(3, 4)                 # the centre lines, which boxes are drawn with
V = vline(3, 4)
UP, DOWN = block(3, 0, 4, 4), block(3, 3, 4, 7)
LEFT, RIGHT = block(0, 3, 4, 4), block(3, 3, 7, 4)


def graphics():
    """Codes 64-127 of the upper case set."""
    g = {
        64: H, 65: art(HAND['spade']), 66: V, 67: hline(3), 68: hline(1), 69: hline(2),
        70: hline(5), 71: vline(2), 72: vline(5), 73: arc('tr'), 74: arc('bl'),
        75: arc('br'), 76: union(hline(7), vline(0)), 77: diagonal(True),
        78: diagonal(False), 79: union(hline(0), vline(0)), 80: union(hline(0), vline(7)),
        81: art(HAND['ball']), 82: hline(6), 83: art(HAND['heart']), 84: vline(1),
        85: arc('tl'), 86: union(diagonal(True), diagonal(False)), 87: art(HAND['circle']),
        88: art(HAND['club']), 89: vline(6), 90: art(HAND['diamond']), 91: union(H, V),
        92: masked(checker(), block(0, 0, 3, 7)), 93: V, 94: art(HAND['pi']),
        95: triangle(False),
        96: [0] * 8, 97: block(0, 0, 3, 7), 98: block(0, 4, 7, 7), 99: hline(0),
        100: hline(7), 101: vline(0), 102: checker(), 103: vline(7),
        104: masked(checker(), block(0, 4, 7, 7)), 105: triangle(True), 106: block(6, 0, 7, 7),
        107: union(V, RIGHT), 108: block(4, 4, 7, 7), 109: union(UP, RIGHT),
        110: union(LEFT, DOWN), 111: block(0, 6, 7, 7), 112: union(RIGHT, DOWN),
        113: union(H, UP), 114: union(H, DOWN), 115: union(V, LEFT), 116: block(0, 0, 1, 7),
        117: block(0, 0, 2, 7), 118: block(5, 0, 7, 7), 119: block(0, 0, 7, 1),
        120: block(0, 0, 7, 2), 121: block(0, 5, 7, 7), 122: union(hline(7), vline(7)),
        123: block(0, 4, 3, 7), 124: block(4, 0, 7, 3), 125: union(UP, LEFT),
        126: block(0, 0, 3, 3), 127: union(block(0, 0, 3, 3), block(4, 4, 7, 7)),
    }
    assert sorted(g) == list(range(64, 128))
    return g


def text_char(code, lower):
    """Screen codes 0-63 (and the letters at 65-90 of the lower case set)."""
    if code == 0:
        return glyph_rows('@')
    if 1 <= code <= 26:
        return glyph_rows(chr((ord('a') if lower else ord('A')) + code - 1))
    special = {27: '[', 28: '`', 29: ']'}          # (the project font's ` is a pound sign)
    if code in special:
        return glyph_rows(special[code])
    if code == 30:
        return art(HAND['up'])
    if code == 31:
        return art(HAND['left'])
    return glyph_rows(chr(code))


def char_sets():
    g = graphics()
    upper = [text_char(c, False) for c in range(64)] + [g[c] for c in range(64, 128)]
    lower = [text_char(c, True) for c in range(64)] + [g[c] for c in range(64, 128)]
    for c in range(65, 91):
        lower[c] = glyph_rows(chr(ord('A') + c - 65))
    lower[94] = checker(1)
    lower[95] = stripes(1)
    lower[105] = stripes(-1)
    lower[122] = art(HAND['check'])
    return upper, lower


def main():
    assert all(len(v.split('/')) == 8 for v in HAND.values())
    sets = char_sets()
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                       'machines', 'c64hw', 'chars.c')
    lines = ['/* Generated by tools/mkc64chars.py - edit the characters there. */',
             '#include "c64.h"', '',
             '// The two character sets: 128 characters each, by screen code, 8 bytes',
             '// a character (top row first, bit 7 = left).',
             'const unsigned char c64_chars[2][128][8] = {']
    for s in sets:
        lines.append('    {')
        for code, rows in enumerate(s):
            lines.append('        { %s },  // %d' % (', '.join('0x%02X' % b for b in rows), code))
        lines.append('    },')
    lines.append('};')
    with open(out, 'w') as f:
        f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
