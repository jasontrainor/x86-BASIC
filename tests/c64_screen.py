#!/usr/bin/env python3
"""
Read the BASIC V2 machine's screen (machines/c64hw/) back from a
screenshot (a PPM file): its 25 lines of 40 characters, matched against
the character sets tools/mkc64chars.py draws, and the colour of a dot.

The 320x200 picture is where the kernels put it: on the BIOS, VGA mode
13h fills the screen (QEMU's screenshot of it is 640x400); on UEFI, it is
scaled by the largest whole number that fits a 384x272 frame, centred.

    tests/c64_screen.py SCREEN.ppm        prints the text
    tests/c64_screen.py SCREEN.ppm --colour X,Y ...   the colours of dots
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import mkc64chars  # noqa: E402


def _shapes():
    """Each glyph's rows (as drawn, and reversed) -> the character, as the
    tests write it: the upper case set's letters, digits and punctuation."""
    upper, lower = mkc64chars.char_sets()
    names = {}
    for code in range(128):
        if code == 0:
            ch = '@'
        elif code <= 26:
            ch = chr(64 + code)
        elif code < 32:
            ch = '[\\]^_'[code - 27]
        elif code < 64:
            ch = chr(code)
        else:
            ch = '~'
        rows = tuple(upper[code])
        names.setdefault(rows, ch)
        names.setdefault(tuple(r ^ 0xFF for r in rows), ch)
    for code in range(1, 27):
        names.setdefault(tuple(lower[code]), chr(96 + code))
    return names


SHAPES = _shapes()


class Screen:
    def __init__(self, path):
        data = open(path, 'rb').read()
        fields, pos = [], 0
        while len(fields) < 4:
            while data[pos:pos + 1].isspace():
                pos += 1
            start = pos
            while not data[pos:pos + 1].isspace():
                pos += 1
            fields.append(data[start:pos])
        self.w, self.h = int(fields[1]), int(fields[2])
        self.pix = data[pos + 1:]
        if (self.w, self.h) == (640, 400):        # mode 13h, double-scanned
            self.sx, self.sy, self.left, self.top = 2, 2, 0, 0
        else:
            s = max(1, min(self.w // 384, self.h // 272))
            self.sx = self.sy = s
            self.left, self.top = (self.w - 320 * s) // 2, (self.h - 200 * s) // 2

    def rgb(self, x, y):
        """The colour of dot (x, y) of the 320x200 picture."""
        px = self.left + x * self.sx + self.sx // 2
        py = self.top + y * self.sy + self.sy // 2
        i = (py * self.w + px) * 3
        return tuple(self.pix[i:i + 3])

    def colour(self, x, y):
        """... as the palette index (0-15), or -1."""
        want = self.rgb(x, y)
        for i in range(16):     # (the VGA's DAC keeps 6 bits of each)
            if all(abs(a - b) < 8 for a, b in zip(PALETTE[i], want)):
                return i
        return -1

    def text(self):
        lines = []
        for r in range(25):
            line = ''
            for c in range(40):
                colours = {}
                for y in range(8):
                    for x in range(8):
                        k = self.rgb(c * 8 + x, r * 8 + y)
                        colours[k] = colours.get(k, 0) + 1
                if len(colours) == 1:
                    line += ' '
                    continue
                # Ink is the colour that matches a glyph: try each as ink.
                found = '?'
                for ink in colours:
                    rows = tuple(sum(0x80 >> x for x in range(8)
                                     if self.rgb(c * 8 + x, r * 8 + y) == ink)
                                 for y in range(8))
                    if rows in SHAPES:
                        found = SHAPES[rows]
                        break
                line += found
            lines.append(line.rstrip())
        return '\n'.join(lines)


def _palette():
    src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..',
                            'machines', 'c64hw', 'vic.c')).read()
    import re
    body = src[src.index('palette[16][3] = {'):]
    return [tuple(int(v, 16) for v in m)
            for m in re.findall(r'\{ 0x(..), 0x(..), 0x(..) \}', body)[:16]]


PALETTE = _palette()

if __name__ == '__main__':
    if len(sys.argv) > 3 and sys.argv[2] == '--colour':
        # (the palette index at each x,y given)
        scr = Screen(sys.argv[1])
        print(' '.join(str(scr.colour(*map(int, xy.split(',')))) for xy in sys.argv[3:]))
    else:
        print(Screen(sys.argv[1]).text())
