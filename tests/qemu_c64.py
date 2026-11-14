#!/usr/bin/env python3
"""
Boot the BASIC V2 machine (machines/c64/) in QEMU, BIOS and UEFI, type
at it, and check its screen, read back from screenshots with
tests/c64_screen.py.

    tests/qemu_c64.py [--mode hdd|uefi|all]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import c64_screen
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
    return c64_screen.Screen(path)


def wait_text(mon, tmp, want, seconds=30):
    deadline = time.time() + seconds
    while time.time() < deadline:
        s = screen(mon, tmp)
        if s.w >= 320:
            text = s.text()
            if want in text:
                return text
        time.sleep(0.3)
    return None


# What's typed, and what should then be on the screen.
SESSIONS = [
    ('', 'READY.'),
]


def run(mode):
    if mode.startswith('uefi') and not q.ovmf():
        print(f'[{mode}] SKIPPED: no OVMF firmware found')
        return True
    good = True
    with tempfile.TemporaryDirectory() as tmp:
        image = os.path.join(tmp, 'c64.img')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), image,
                        '--machines', 'c64'], check=True)
        mon_path = os.path.join(tmp, 'mon.sock')
        proc = subprocess.Popen(q.qemu_args(mode, image, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            mon = q.Monitor(mon_path)
            started = wait_text(mon, tmp, '38911 BASIC BYTES FREE', seconds=60) is not None
            print(f'[{mode}] {"PASS" if started else "FAIL"} c64 started')
            good &= started
            for typed, want in SESSIONS if started else []:
                q.type_keys(mon, typed)
                text = wait_text(mon, tmp, want)
                ok = text is not None
                print(f'[{mode}] {"PASS" if ok else "FAIL"} c64 {typed.strip()!r}')
                if not ok:
                    print(screen(mon, tmp).text())
                good &= ok
            s = screen(mon, tmp)
            colours = (s.colour(4, 4), s.colour(1, 41))   # background, READY.'s R
            ok = colours == (6, 14)
            print(f'[{mode}] {"PASS" if ok else "FAIL"} c64 colours {colours}')
            good &= ok
        finally:
            proc.kill()
            proc.wait()
    return good


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='all', choices=['hdd', 'uefi', 'all'])
    a = ap.parse_args()
    modes = ['hdd', 'uefi'] if a.mode == 'all' else [a.mode]
    sys.exit(0 if all([run(m) for m in modes]) else 1)


if __name__ == '__main__':
    main()
