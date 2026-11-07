#!/usr/bin/env python3
"""
Boot os.img in QEMU (headless), type a BASIC session, and check the screen.

The screen is a graphics mode, so the text is read back by taking a
screenshot and matching each 8x8 character cell against the font
(tools/mkfont.py): the check sees exactly what a user would.

    tests/qemu_smoke.py [--mode MODE|all] [--image os.img] [--show]

BIOS (SeaBIOS) modes:
  floppy     boot as drive A: (BIOS drive 0x00)
  hdd        boot as the first hard disk (drive 0x80) - how most BIOSes
             present a USB stick in "USB-HDD" mode
  usb        boot from an emulated USB stick on an xHCI controller
UEFI (OVMF) modes - skipped if OVMF isn't installed:
  uefi       boot the UEFI kernel from a USB stick on an xHCI controller
  uefi-hdd   the same, from a SATA/IDE hard disk
"""
import argparse
import json
import shutil
import os
import socket
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
from mkfont import font_table  # noqa: E402

KEYS = {
    ' ': 'spc', '\n': 'ret', '=': 'equal', '+': 'shift-equal', '-': 'minus',
    '*': 'shift-8', '/': 'slash', '(': 'shift-9', ')': 'shift-0',
    '"': 'shift-apostrophe', '$': 'shift-4', ';': 'semicolon', ',': 'comma',
    '<': 'shift-comma', '>': 'shift-dot', ':': 'shift-semicolon',
    '.': 'dot', '^': 'shift-6', '!': 'shift-1', '@': 'shift-2', '#': 'shift-3',
    '%': 'shift-5', '&': 'shift-7', '?': 'shift-slash', '_': 'shift-minus',
    "'": 'apostrophe', '`': 'grave_accent', '~': 'shift-grave_accent',
    '[': 'bracket_left', ']': 'bracket_right', '{': 'shift-bracket_left',
    '}': 'shift-bracket_right', '\\': 'backslash', '|': 'shift-backslash',
    '\x1b': 'esc', '\b': 'backspace',
    # Control characters stand for the cursor keys in a session.
    '\x02': 'left', '\x06': 'right', '\x10': 'up', '\x0e': 'down',
    '\x01': 'home', '\x05': 'end', '\x7f': 'delete',
}

# (keystrokes, text that must then be on screen); RESET reboots the PC.
RESET = None
SESSION = [
    ('10 FOR I=1 TO 3\n20 PRINT "LOOP ";I*11\n30 NEXT I\n40 PRINT "DONE"\nRUN\n',
     ['LOOP 11', 'LOOP 22', 'LOOP 33', 'DONE']),
    ('IF 1<>2 THEN PRINT "NOT EQUAL"\n', ['NOT EQUAL']),
    ('TOTAL=0: FOR N=1 TO 4: TOTAL=TOTAL+N: NEXT N: PRINT "SUM ";TOTAL\n', ['SUM 10']),
    ('PRINT 1/0\n', ['ERROR: DIVISION BY ZERO']),
    ('PRINT 1/3;" ";SQR 2\n', ['0.33333333 1.4142136']),
    ('BEEP\nPRINT 6*7\n', ['\n42']),   # BEEP must return (timer-based)
    ('INK 1: BORDER 2: PRINT 6*9: INK 0: BORDER 7\n', ['\n54']),
    ('A$="HELLO": DIM N(3): N(2)=7: PRINT A$(2 TO 4);LEN A$;N(2)\n', ['ELL57']),
    ('NEW\n10 INPUT "AGE";A\n20 GOSUB 100: STOP\n100 PRINT "DOUBLE ";A*2: RETURN\nRUN\n21\n',
     ['DOUBLE 42', 'STOPPED IN LINE 20']),
    ('CLS: PLOT 0,0: DRAW 50,50: CIRCLE 100,100,20: PRINT POINT (25,25);POINT (120,100)\n',
     ['11']),
    ('CLS: PRINT "lower case: abc xyz"\n', ['lower case: abc xyz']),
    # Line editor: cursor keys, Home, Delete, history.
    ('PRINT 1X2\x02\x02\x7f\x01\x06\x06\x06\x06\x06\x06"=";\x05\n', ['PRINT "=";12\n=12']),
    ('PRINT 6*8\n\x10\n', ['48\nPRINT 6*8\n48']),
    ('NEW\n10 PRINT "SAVED ";6*7\nSAVE "keep"\nCAT\n', ['keep            ']),
    (RESET, []),                      # reboot: the program must still be there
    ('LOAD "KEEP"\nRUN\n', ['SAVED 42']),
    ('NEW\n10 GOTO 10\nRUN\n\x1b', ['BREAK IN LINE 10']),
]

# Use KVM when available. Besides being faster, it runs the code on the
# real CPU, which enforces real-mode segment limits; QEMU's emulator
# doesn't, so it can't catch code that only works because of that.
# Set NOKVM=1 to force emulation.
KVM = (['-enable-kvm'] if os.access('/dev/kvm', os.R_OK | os.W_OK)
       and not os.environ.get('NOKVM') else [])

# Glyph rows -> character, for reading the screen.
GLYPH_CHARS = {tuple(rows): chr(code) for code, rows in font_table().items()
               if code < 127}


def key_name(ch):
    if ch in KEYS:
        return KEYS[ch]
    if ch.isupper():
        return 'shift-' + ch.lower()
    return ch


class Monitor:
    """Minimal QMP client; human-monitor commands are sent through it."""

    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        for _ in range(100):
            try:
                self.sock.connect(path)
                break
            except OSError:
                time.sleep(0.05)
        else:
            raise RuntimeError('QEMU monitor did not come up')
        self.sock.settimeout(10)
        self.buf = b''
        self._reply()                       # greeting
        self._send({'execute': 'qmp_capabilities'})
        self._reply()

    def _send(self, obj):
        self.sock.sendall(json.dumps(obj).encode() + b'\n')

    def _reply(self):
        while True:
            while b'\n' not in self.buf:
                chunk = self.sock.recv(65536)
                if not chunk:
                    raise RuntimeError('QEMU monitor closed')
                self.buf += chunk
            line, self.buf = self.buf.split(b'\n', 1)
            msg = json.loads(line)
            if 'event' in msg:
                continue                    # asynchronous; not our reply
            if 'error' in msg:
                raise RuntimeError(msg['error'])
            return msg

    def cmd(self, line):
        self._send({'execute': 'human-monitor-command',
                    'arguments': {'command-line': line}})
        return self._reply().get('return', '')

    def qmp(self, command, **arguments):
        self._send({'execute': command, 'arguments': arguments})
        return self._reply().get('return')


def read_ppm(path):
    data = open(path, 'rb').read()
    fields, pos = [], 0
    while len(fields) < 4:                  # P6 width height maxval
        while data[pos:pos + 1].isspace():
            pos += 1
        start = pos
        while not data[pos:pos + 1].isspace():
            pos += 1
        fields.append(data[start:pos])
    assert fields[0] == b'P6', fields[0]
    return int(fields[1]), int(fields[2]), data[pos + 1:]


def layout(width, height):
    """Where the 256x192 screen is: (scale, left, top), as the kernel
    places it. VGA mode 13h (320x200) is shown double-scanned (640x400)."""
    double = 2 if (width, height) == (640, 400) else 1
    w, h = width // double, height // double
    scale = max(1, min(w // 320, h // 240))
    return (scale * double, (w - 256 * scale) // 2 * double,
            (h - 192 * scale) // 2 * double)


def screen_text(mon, tmpdir):
    """The text on screen, read off a screenshot: 24 lines of 32."""
    dump = os.path.join(tmpdir, 'screen.ppm')
    if os.path.exists(dump):
        os.unlink(dump)
    mon.qmp('screendump', filename=dump)
    for _ in range(50):
        if os.path.exists(dump) and os.path.getsize(dump) > 0:
            break
        time.sleep(0.05)
    width, height, pix = read_ppm(dump)
    if width < 256 or height < 192:         # not in graphics mode yet
        return ''
    scale, left, top = layout(width, height)

    def pixel(x, y):
        px, py = left + x * scale + scale // 2, top + y * scale + scale // 2
        i = (py * width + px) * 3
        return pix[i:i + 3]

    lines = []
    for r in range(24):
        line = ''
        for c in range(32):
            # Every glyph's top-left pixel is background (or, under the
            # inverted cursor, foreground), so compare against that.
            bg = pixel(c * 8, r * 8)
            rows = tuple(sum(0x80 >> x for x in range(8)
                             if pixel(c * 8 + x, r * 8 + y) != bg)
                         for y in range(8))
            line += GLYPH_CHARS.get(rows, '?')
        lines.append(line.rstrip())
    return '\n'.join(lines)


def ready(mon, tmpdir):
    """True once the kernel's banner is on screen."""
    return 'x86 BASIC' in screen_text(mon, tmpdir)


MODES = ['floppy', 'hdd', 'usb', 'uefi', 'uefi-hdd']

# UEFI firmware for QEMU (the edk2 "OVMF" build), wherever the
# distribution puts it. A single combined image works with -bios.
OVMF_PATHS = ['/usr/share/ovmf/OVMF.fd', '/usr/share/qemu/OVMF.fd',
              '/usr/share/OVMF/OVMF.fd', '/usr/share/edk2/ovmf/OVMF.fd',
              '/usr/share/edk2-ovmf/x64/OVMF.fd']


def ovmf():
    return next((p for p in OVMF_PATHS if os.path.exists(p)), None)


def qemu_args(mode, image, monitor_path):
    drive = f'file={image},format=raw'
    args = ['qemu-system-x86_64', '-display', 'none', '-no-reboot'] + KVM + [
            '-qmp', f'unix:{monitor_path},server,nowait']
    if mode == 'floppy':
        args += ['-drive', drive + ',if=floppy', '-boot', 'a']
    elif mode == 'hdd':
        args += ['-drive', drive + ',if=ide', '-boot', 'c']
    elif mode in ('usb', 'uefi'):
        args += ['-drive', drive + ',if=none,id=stick',
                 '-device', 'qemu-xhci',
                 '-device', 'usb-storage,drive=stick,bootindex=0']
    elif mode == 'uefi-hdd':
        args += ['-drive', drive + ',if=ide']
    if mode.startswith('uefi'):
        args += ['-bios', ovmf()]
    return args


def type_keys(mon, keys):
    for ch in keys:
        mon.cmd('sendkey ' + key_name(ch))
        time.sleep(0.02)


def run(mode, image, show):
    accel = 'kvm' if KVM else 'emulated'
    if mode.startswith('uefi') and not ovmf():
        print(f'[{mode}] SKIPPED: no OVMF firmware found (install the ovmf package)')
        return True
    with tempfile.TemporaryDirectory() as tmp:
        mon_path = os.path.join(tmp, 'mon.sock')
        # Boot a copy: the session SAVEs programs, and a copy can't clash
        # with another QEMU using the image.
        copy = os.path.join(tmp, 'os.img')
        shutil.copy(image, copy)
        proc = subprocess.Popen(qemu_args(mode, copy, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            try:
                mon = Monitor(mon_path)
            except RuntimeError:
                proc.kill()
                print(f'[{mode}] FAIL: QEMU did not start: '
                      f'{proc.communicate()[1].decode().strip()}')
                return False
            # Wait for the kernel's banner rather than a fixed delay.
            deadline = time.time() + 60    # (UEFI firmware takes a while)
            while not ready(mon, tmp):
                if time.time() > deadline:
                    print(f'[{mode}] FAIL: kernel never started. Screen:\n'
                          f'{screen_text(mon, tmp)}')
                    return False
                time.sleep(0.2)

            ok = True
            text = ''
            for keys, expected in SESSION:
                if keys is RESET:
                    # Power off and boot again from the same disk.
                    mon.sock.close()
                    proc.kill()
                    proc.wait()
                    os.unlink(mon_path)
                    proc = subprocess.Popen(qemu_args(mode, copy, mon_path),
                                            stdout=subprocess.DEVNULL,
                                            stderr=subprocess.PIPE)
                    mon = Monitor(mon_path)
                    deadline = time.time() + 60
                    while not ready(mon, tmp) and time.time() < deadline:
                        time.sleep(0.2)
                    continue
                type_keys(mon, keys)
                # Key events are queued, so poll until the output shows up.
                deadline = time.time() + 20
                while True:
                    text = screen_text(mon, tmp)
                    missing = [w for w in expected if w not in text]
                    if not missing or time.time() > deadline:
                        break
                    time.sleep(0.2)
                for want in missing:
                    print(f'[{mode}] FAIL: expected {want!r} on screen')
                    ok = False
            if show or not ok:
                print(text)
            print(f'[{mode}, {accel}] {"PASS" if ok else "FAIL"}')
            return ok
        finally:
            proc.kill()
            proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='all', choices=MODES + ['all'])
    ap.add_argument('--image', default=os.path.join(ROOT, 'os.img'))
    ap.add_argument('--show', action='store_true', help='print the final screen')
    a = ap.parse_args()
    modes = MODES if a.mode == 'all' else [a.mode]
    results = [run(m, a.image, a.show) for m in modes]
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
