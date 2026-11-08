#!/usr/bin/env python3
"""
Boot images with the chooser's menu in QEMU (headless) and check it:

  - two machines: the menu shows them, 2 starts the second, F10 twice
    goes back to the menu, and Enter starts the first;
  - MENU=never: the default starts with no menu;
  - one machine, Shift held while it starts (BIOS): the menu shows.

The second machine is the Spectrum's kernels again, under another name.
Run with the kernels built (make boot-test does).

    tests/qemu_menu.py [--mode hdd|uefi|all]
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import qemu_smoke as q

ROOT = q.ROOT
BUILD = os.path.join(ROOT, 'build')
BANNER = 'x86 BASIC for PCs'
MENU = 'Choose a computer:'


def make_image(tmp, machines, *options):
    """An image with these machines ('second' is a copy of the Spectrum)."""
    build = os.path.join(tmp, 'build')
    if not os.path.exists(build):
        os.makedirs(build)
        shutil.copy(os.path.join(BUILD, 'boot.bin'), build)
        for d in ('chooser', 'spectrum'):
            shutil.copytree(os.path.join(BUILD, d), os.path.join(build, d),
                            ignore=shutil.ignore_patterns('bios', 'uefi'))
        shutil.copytree(os.path.join(build, 'spectrum'), os.path.join(build, 'second'))
    image = os.path.join(tmp, 'menu.img')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), image,
                    '--build', build, '--machines', ','.join(machines)] + list(options),
                   check=True)
    return image


def wait_for(mon, tmp, want, seconds=60):
    deadline = time.time() + seconds
    while time.time() < deadline:
        text = q.screen_text(mon, tmp)
        if want in text:
            return text
        time.sleep(0.2)
    return None


class Machine:
    def __init__(self, mode, image, tmp):
        self.tmp = tmp
        mon_path = os.path.join(tmp, 'mon.sock')
        if os.path.exists(mon_path):
            os.unlink(mon_path)
        self.proc = subprocess.Popen(q.qemu_args(mode, image, mon_path),
                                     stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        self.mon = q.Monitor(mon_path)

    def close(self):
        self.mon.sock.close()
        self.proc.kill()
        self.proc.wait()


def check(mode, name, ok, screen=''):
    print(f'[{mode}] {"PASS" if ok else "FAIL"} {name}')
    if not ok and screen:
        print(screen)
    return ok


def run(mode):
    if mode.startswith('uefi') and not q.ovmf():
        print(f'[{mode}] SKIPPED: no OVMF firmware found')
        return True
    good = True
    with tempfile.TemporaryDirectory() as tmp:
        # Two machines: the menu, choosing, F10 twice, choosing again.
        image = make_image(tmp, ['spectrum', 'second'], '--timeout', '0')
        m = Machine(mode, image, tmp)
        try:
            text = wait_for(m.mon, tmp, MENU)
            good &= check(mode, 'menu shown', text is not None and '1  Spectrum BASIC' in text
                          and '2  SECOND' in text, text or q.screen_text(m.mon, tmp))
            q.type_keys(m.mon, '2')
            text = wait_for(m.mon, tmp, BANNER, 30)
            good &= check(mode, 'second started', text is not None, q.screen_text(m.mon, tmp))
            q.type_keys(m.mon, 'PRINT 6*7\n')
            good &= check(mode, 'second runs', wait_for(m.mon, tmp, '42', 20) is not None,
                          q.screen_text(m.mon, tmp))
            m.mon.cmd('sendkey f10')
            time.sleep(0.5)
            m.mon.cmd('sendkey f10')
            text = wait_for(m.mon, tmp, MENU, 30)
            good &= check(mode, 'F10 twice: back to the menu', text is not None and
                          'Starting in' not in text, q.screen_text(m.mon, tmp))
            q.type_keys(m.mon, '\n')
            text = wait_for(m.mon, tmp, BANNER, 30)
            good &= check(mode, 'first started', text is not None, q.screen_text(m.mon, tmp))
        finally:
            m.close()

        # MENU=never: straight to the default.
        image = make_image(tmp, ['spectrum', 'second'], '--menu', 'never', '--default', 'second')
        m = Machine(mode, image, tmp)
        try:
            text = wait_for(m.mon, tmp, BANNER)
            good &= check(mode, 'menu never', text is not None, q.screen_text(m.mon, tmp))
        finally:
            m.close()

        # Shift held while starting brings up the menu (one machine). (BIOS
        # only: UEFI firmware has no Shift state to read; a key typed while
        # starting is read by the firmware itself.) Held as a key does on a
        # real keyboard: the key goes down again and again (auto-repeat).
        if not mode.startswith('uefi'):
            image = make_image(tmp, ['spectrum'])
            m = Machine(mode, image, tmp)

            def shift(down):
                m.mon.qmp('input-send-event', events=[{'type': 'key', 'data': {
                    'down': down, 'key': {'type': 'qcode', 'data': 'shift'}}}])
            try:
                # (Fast: SeaBIOS forgets the Shift state just before it boots.)
                deadline = time.time() + 60
                text = ''
                while time.time() < deadline:
                    for _ in range(100):
                        shift(True)
                        time.sleep(0.005)
                    text = q.screen_text(m.mon, tmp)
                    if MENU in text or BANNER in text:
                        break
                shift(False)
                good &= check(mode, 'Shift at start: menu', MENU in text, text)
                q.type_keys(m.mon, '\n')
                good &= check(mode, 'menu then start', wait_for(m.mon, tmp, BANNER, 30)
                              is not None, q.screen_text(m.mon, tmp))
            finally:
                m.close()
    return good


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--mode', default='all', choices=['hdd', 'floppy', 'uefi', 'all'])
    a = ap.parse_args()
    modes = ['hdd', 'uefi'] if a.mode == 'all' else [a.mode]
    ok = all([run(m) for m in modes])
    sys.exit(0 if ok else 1)


if __name__ == '__main__':
    main()
