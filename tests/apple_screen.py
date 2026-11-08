#!/usr/bin/env python3
"""Read an Apple machine's screen (machines/apple/video.c) back from a
picture of it: the 560x384 picture (as tests/host_stub.c writes it with
SCREEN_OUT), or a QEMU screenshot with the picture scaled inside it.

    tests/apple_screen.py screen.ppm [--80] [--box X,Y,SCALE]
        print the text: 24 lines, inverse characters in [brackets]
    tests/apple_screen.py screen.ppm --colour X,Y
        print the colour (palette index) at picture dot (X 0-559, Y 0-191)

Characters are recognised by the font in tools/mkfont.py, as the Apple
screen draws them (7 dots of each 8x8 glyph). MouseText shows as '#'.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools'))
import mkfont

PALETTE = [(0x00, 0x00, 0x00), (0x8A, 0x15, 0x47), (0x3E, 0x31, 0xA8), (0xD2, 0x46, 0xE8),
           (0x00, 0x6A, 0x3E), (0x82, 0x82, 0x82), (0x2D, 0x92, 0xE6), (0xBE, 0xAE, 0xFF),
           (0x42, 0x55, 0x00), (0xD4, 0x6C, 0x1D), (0x82, 0x82, 0x82), (0xFF, 0x99, 0xC1),
           (0x2C, 0xC0, 0x1A), (0xC0, 0xD5, 0x5C), (0x6C, 0xEA, 0xC4), (0xFF, 0xFF, 0xFF)]


def glyph_table():
    table = {}
    for code, rows in mkfont.font_table().items():
        ch = chr(code)
        if code in (96, 127):            # (the Apple screen draws its own ` and DEL)
            continue
        table[tuple(r & 0xFE for r in rows)] = ch
    table[(0x40, 0x20, 0x10, 0, 0, 0, 0, 0)] = '`'
    return table


GLYPHS = glyph_table()


def read_ppm(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    return int(fields[1]), int(fields[2]), data[pos + 1:]


class Screen:
    def __init__(self, path, box=None):
        if path.endswith('.png'):
            from PIL import Image
            im = Image.open(path).convert('RGB')
            self.w, self.h, self.pix = im.width, im.height, im.tobytes()
        else:
            self.w, self.h, self.pix = read_ppm(path)
        if box is None:
            # The picture, centred at the largest scale whose frame (600x400)
            # fits; a bare 560x384 picture is scale 1 at (0, 0).
            if (self.w, self.h) == (560, 384):
                box = (0, 0, 1)
            else:
                s = max(1, min(self.w // 600, self.h // 400))
                box = ((self.w - 560 * s) // 2, (self.h - 384 * s) // 2, s)
        self.x0, self.y0, self.scale = box

    def rgb(self, x, y):
        """The colour at picture dot (x 0-559, y 0-191)."""
        px = self.x0 + x * self.scale + self.scale // 2
        py = self.y0 + y * 2 * self.scale + self.scale
        i = (py * self.w + px) * 3
        return tuple(self.pix[i:i + 3])

    def colour(self, x, y):
        """The palette colour nearest the one at (x, y) (the VGA keeps 6
        bits of each), or -1 if none is near."""
        c = self.rgb(x, y)
        best = min(range(16), key=lambda i: sum(abs(a - b) for a, b in zip(PALETTE[i], c)))
        return best if all(abs(a - b) <= 8 for a, b in zip(PALETTE[best], c)) else -1

    def white(self, x, y):
        return self.colour(x, y) == 15

    def text(self, col80=False):
        cw = 7 if col80 else 14
        lines = []
        for r in range(24):
            line, inv = '', False
            for c in range(80 if col80 else 40):
                rows = []
                for y in range(8):
                    b = 0
                    for d in range(7):
                        x = c * cw + (d if col80 else 2 * d)
                        if self.white(x, r * 8 + y):
                            b |= 0x80 >> d
                    rows.append(b)
                rows = tuple(rows)
                inverse = tuple(r ^ 0xFE for r in rows)
                ch, now = '#', False
                if rows in GLYPHS:
                    ch = GLYPHS[rows]
                elif inverse in GLYPHS:
                    ch, now = GLYPHS[inverse], True
                if now != inv:
                    line += '[' if now else ']'
                    inv = now
                line += ch
            if inv:
                line += ']'
            lines.append(line.rstrip())
        return '\n'.join(lines)


def main():
    args = sys.argv[1:]
    path = args.pop(0)
    box = None
    if '--box' in args:
        i = args.index('--box')
        box = tuple(int(v) for v in args[i + 1].split(','))
        del args[i:i + 2]
    s = Screen(path, box)
    if '--colour' in args:
        x, y = (int(v) for v in args[args.index('--colour') + 1].split(','))
        print(s.colour(x, y))
    else:
        print(s.text('--80' in args))


if __name__ == '__main__':
    main()
