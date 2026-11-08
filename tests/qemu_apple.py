#!/usr/bin/env python3
"""
Boot the Apple video test machine (machines/a2video/) in QEMU, BIOS and
UEFI, and check the screen it draws: text, line input, 80 columns, and
the colours of the graphics modes, read back from screenshots with
tests/apple_screen.py. On the BIOS this is VGA mode 12h, double-scanned.

    tests/qemu_apple.py [--mode hdd|uefi|all]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import apple_screen
import qemu_smoke as q

ROOT = q.ROOT


def screen(mon, tmp):
    path = os.path.join(tmp, 'screen.ppm')
    if os.path.exists(path):
        os.unlink(path)
    mon.qmp('screendump', filename=path)
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            break
        time.sleep(0.05)
    return apple_screen.Screen(path)


def wait_text(mon, tmp, want, col80=False, seconds=30):
    deadline = time.time() + seconds
    text = ''
    while time.time() < deadline:
        s = screen(mon, tmp)
        if s.w >= 560:
            text = s.text(col80)
            if want in text:
                return text
        time.sleep(0.3)
    return None


def run(mode):
    if mode.startswith('uefi') and not q.ovmf():
        print(f'[{mode}] SKIPPED: no OVMF firmware found')
        return True
    good = True

    def check(name, ok, detail=''):
        nonlocal good
        good &= ok
        print(f'[{mode}] {"PASS" if ok else "FAIL"} {name}')
        if not ok and detail:
            print(detail)

    with tempfile.TemporaryDirectory() as tmp:
        image = os.path.join(tmp, 'a2video.img')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), image,
                        '--machines', 'a2video'], check=True)
        mon_path = os.path.join(tmp, 'mon.sock')
        proc = subprocess.Popen(q.qemu_args(mode, image, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            mon = q.Monitor(mon_path)
            check('started', wait_text(mon, tmp, 'VIDEO TEST', seconds=60) is not None,
                  screen(mon, tmp).text() if os.path.exists(os.path.join(tmp, 'screen.ppm')) else '')
            q.type_keys(mon, 'HELLO\n')
            text = wait_text(mon, tmp, '?HELLO')
            check('line input', text is not None and ']HELLO\n?HELLO\n]' in text, text or '')
            q.type_keys(mon, 'HGR\nBARS\n')
            time.sleep(3)
            s = screen(mon, tmp)
            got = [s.colour(x, 20) for x in (50, 160, 280, 400, 500)]
            check('hires colours', got == [3, 12, 6, 9, 15], str(got))
            q.type_keys(mon, 'GR\nBARS\n')
            time.sleep(3)
            s = screen(mon, tmp)
            got = [s.colour(x, 20) for x in (7, 45, 77, 553)]
            check('lores colours', got == [0, 1, 2, 15], str(got))
            q.type_keys(mon, 'TEXT\n80\nEIGHTY COLUMNS\n')
            text = wait_text(mon, tmp, '?EIGHTY COLUMNS', col80=True)
            check('80 columns', text is not None and text.startswith(']EIGHTY COLUMNS\n'),
                  text or screen(mon, tmp).text(True))
        finally:
            proc.kill()
            proc.wait()
    return good


def run_basic(mode, machine, prompt, typed, want):
    """Boot an image with just this BASIC machine, type a line, and look
    for what it should print."""
    if mode.startswith('uefi') and not q.ovmf():
        return True
    with tempfile.TemporaryDirectory() as tmp:
        image = os.path.join(tmp, machine + '.img')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), image,
                        '--machines', machine], check=True)
        mon_path = os.path.join(tmp, 'mon.sock')
        proc = subprocess.Popen(q.qemu_args(mode, image, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            mon = q.Monitor(mon_path)
            ok = wait_text(mon, tmp, prompt, seconds=60) is not None
            if ok:
                q.type_keys(mon, typed)
                ok = wait_text(mon, tmp, want) is not None
            print(f'[{mode}] {"PASS" if ok else "FAIL"} {machine}')
            if not ok:
                print(screen(mon, tmp).text())
            return ok
        finally:
            proc.kill()
            proc.wait()


BASICS = [
    ('a2int', '>', 'PRINT 6*7\nGR\nCOLOR=3:PLOT 1,1\n', '42'),
    ('a1basic', '>', 'PRINT 6*7\n', '42'),
    ('a2fp', ']', 'PRINT 1/3\nHGR\nHPLOT 0,0 TO 279,191\n', '.333333333'),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='all', choices=['hdd', 'uefi', 'all'])
    a = ap.parse_args()
    modes = ['hdd', 'uefi'] if a.mode == 'all' else [a.mode]
    ok = all([run(m) for m in modes] +
             [run_basic(m, *b) for m in modes for b in BASICS])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
