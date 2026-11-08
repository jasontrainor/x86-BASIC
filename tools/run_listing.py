#!/usr/bin/env python3
"""Run a listing in QEMU and take a screenshot of what it shows.

    tools/run_listing.py game.bas [--mode hdd] [--wait 10] [--out game.png]

Builds a copy of the disk image with the listing on its FAT partition
(from the kernels in build/, so run make first: `make run-listing` does),
boots it, types LOAD "GAME.BAS" and RUN (on the Apple-style machines,
--machine a2fp, a2int or a1basic: RUN GAME.BAS), waits, saves the screen
as a PNG and prints the text on it. The listing needs a DOS 8.3 name.
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import qemu_smoke as q
import apple_screen

# The Apple-style machines: the prompt each shows when it's ready.
APPLE_PROMPTS = {'a2fp': ']', 'a2int': '>', 'a1basic': '>'}


def apple_text(mon, tmp):
    path = os.path.join(tmp, 'apple.ppm')
    if os.path.exists(path):
        os.unlink(path)
    mon.qmp('screendump', filename=path)
    for _ in range(50):
        if os.path.exists(path) and os.path.getsize(path) > 0:
            break
        time.sleep(0.05)
    s = apple_screen.Screen(path)
    return s.text() if s.w >= 560 else ''


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('listing')
    ap.add_argument('--mode', default='hdd', choices=q.MODES)
    ap.add_argument('--wait', type=float, default=10, help='seconds to let it run')
    ap.add_argument('--machine', default='spectrum')
    ap.add_argument('--out', help='the PNG to write (default: next to the listing)')
    a = ap.parse_args()
    out = a.out or os.path.splitext(a.listing)[0] + '.png'
    name = os.path.basename(a.listing).upper()

    with tempfile.TemporaryDirectory() as tmp:
        image = os.path.join(tmp, 'listing.img')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), image,
                        '--machines', a.machine, a.listing], check=True)
        mon_path = os.path.join(tmp, 'mon.sock')
        proc = subprocess.Popen(q.qemu_args(a.mode, image, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            mon = q.Monitor(mon_path)
            apple = APPLE_PROMPTS.get(a.machine)
            deadline = time.time() + 30
            while not (apple in apple_text(mon, tmp) if apple else q.ready(mon, tmp)):
                if time.time() > deadline:
                    sys.exit('the image did not start')
                time.sleep(0.2)
            q.type_keys(mon, ('RUN %s\n' if apple else 'LOAD "%s"\nRUN\n') % name)
            time.sleep(a.wait)
            mon.qmp('screendump', filename=os.path.abspath(out), format='png')
            print(apple_text(mon, tmp) if apple else q.screen_text(mon, tmp))
            print('screenshot: ' + out)
        finally:
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    main()
