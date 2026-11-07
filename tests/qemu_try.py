#!/usr/bin/env python3
"""Debug helper: boot os.img, type the given text, print the screen.

    tests/qemu_try.py 'PRINT 1+1\n' [--mode hdd]
"""
import argparse
import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_smoke as q


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('keys')
    ap.add_argument('--mode', default='hdd')
    ap.add_argument('--image', default=os.path.join(q.ROOT, 'os.img'))
    ap.add_argument('--wait', type=float, default=1.5)
    a = ap.parse_args()
    keys = a.keys.encode().decode('unicode_escape')
    with tempfile.TemporaryDirectory() as tmp:
        mon_path = os.path.join(tmp, 'mon.sock')
        proc = subprocess.Popen(q.qemu_args(a.mode, a.image, mon_path),
                                stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        try:
            mon = q.Monitor(mon_path)
            deadline = time.time() + 20
            while not q.ready(mon, tmp) and time.time() < deadline:
                time.sleep(0.2)
            for ch in keys:
                mon.cmd('sendkey ' + q.key_name(ch))
                time.sleep(0.02)
            time.sleep(a.wait)
            print(q.screen_text(mon, tmp)[0])
        finally:
            proc.kill()
            proc.wait()


if __name__ == '__main__':
    main()
