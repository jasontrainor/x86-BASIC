#!/usr/bin/env python3
"""
Boot os.img in QEMU (headless), type a BASIC session, and check the screen.

The screen is read straight out of VGA text memory (0xB8000) through the
QEMU monitor, so the check sees exactly what a user would.

    tests/qemu_smoke.py [--mode floppy|hdd|usb|all] [--image os.img] [--show]

  floppy  boot as drive A: (BIOS drive 0x00)
  hdd     boot as the first hard disk (drive 0x80) - how most BIOSes
          present a USB stick in "USB-HDD" mode
  usb     boot from an emulated USB mass-storage stick on an xHCI controller
"""
import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

KEYS = {
    ' ': 'spc', '\n': 'ret', '=': 'equal', '+': 'shift-equal', '-': 'minus',
    '*': 'shift-8', '/': 'slash', '(': 'shift-9', ')': 'shift-0',
    '"': 'shift-apostrophe', '$': 'shift-4', ';': 'semicolon', ',': 'comma',
    '<': 'shift-comma', '>': 'shift-dot', ':': 'shift-semicolon',
    '\x1b': 'esc',
}

# (keystrokes, text that must then be on screen)
SESSION = [
    ('10 FOR I=1 TO 3\n20 PRINT "LOOP ";I*11\n30 NEXT I\n40 PRINT "DONE"\nRUN\n',
     ['LOOP 11', 'LOOP 22', 'LOOP 33', 'DONE']),
    ('IF 1<>2 THEN PRINT "NOT EQUAL"\n', ['NOT EQUAL']),
    ('PRINT 1/0\n', ['ERROR: DIVISION BY ZERO']),
    ('NEW\n10 GOTO 10\nRUN\n\x1b', ['BREAK IN LINE 10']),
]

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


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


def screen_text(mon, tmpdir):
    dump = os.path.join(tmpdir, 'vga.bin')
    if os.path.exists(dump):
        os.unlink(dump)
    mon.qmp('pmemsave', val=0xB8000, size=4000, filename=dump)
    for _ in range(50):
        if os.path.exists(dump) and os.path.getsize(dump) == 4000:
            break
        time.sleep(0.05)
    data = open(dump, 'rb').read()
    rows = []
    for r in range(25):
        row = data[r * 160:(r + 1) * 160:2]
        rows.append(row.decode('ascii', 'replace').rstrip())
    return '\n'.join(rows), data


def qemu_args(mode, image, monitor_path):
    drive = f'file={image},format=raw'
    args = ['qemu-system-x86_64', '-display', 'none', '-no-reboot',
            '-qmp', f'unix:{monitor_path},server,nowait']
    if mode == 'floppy':
        args += ['-drive', drive + ',if=floppy', '-boot', 'a']
    elif mode == 'hdd':
        args += ['-drive', drive + ',if=ide', '-boot', 'c']
    elif mode == 'usb':
        args += ['-drive', drive + ',if=none,id=stick',
                 '-device', 'qemu-xhci',
                 '-device', 'usb-storage,drive=stick,bootindex=0']
    return args


def run(mode, image, show):
    with tempfile.TemporaryDirectory() as tmp:
        mon_path = os.path.join(tmp, 'mon.sock')
        proc = subprocess.Popen(qemu_args(mode, image, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            mon = Monitor(mon_path)
            # Wait for the kernel's banner rather than a fixed delay.
            deadline = time.time() + 20
            while True:
                text, _ = screen_text(mon, tmp)
                if 'x86 BASIC' in text or 'Sinclair' in text:
                    break
                if time.time() > deadline:
                    print(f'[{mode}] FAIL: kernel never started. Screen:\n{text}')
                    return False
                time.sleep(0.2)

            ok = True
            for keys, expected in SESSION:
                for ch in keys:
                    mon.cmd('sendkey ' + key_name(ch))
                    time.sleep(0.02)
                # Key events are queued, so poll until the output shows up.
                deadline = time.time() + 15
                while True:
                    text, raw = screen_text(mon, tmp)
                    missing = [w for w in expected if w not in text]
                    if not missing or time.time() > deadline:
                        break
                    time.sleep(0.2)
                for want in missing:
                    print(f'[{mode}] FAIL: expected {want!r} on screen')
                    ok = False
            if show or not ok:
                print(text)
            # Cell 0's attribute shows the paper/ink actually in effect
            # (0xf0 = bright white paper, black ink).
            print(f'[{mode}] {"PASS" if ok else "FAIL"} (attribute of cell 0: 0x{raw[1]:02x})')
            return ok
        finally:
            proc.kill()
            proc.wait()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='all', choices=['floppy', 'hdd', 'usb', 'all'])
    ap.add_argument('--image', default=os.path.join(ROOT, 'os.img'))
    ap.add_argument('--show', action='store_true', help='print the final screen')
    a = ap.parse_args()
    modes = ['floppy', 'hdd', 'usb'] if a.mode == 'all' else [a.mode]
    results = [run(m, a.image, a.show) for m in modes]
    sys.exit(0 if all(results) else 1)


if __name__ == '__main__':
    main()
