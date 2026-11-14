#!/usr/bin/env python3
"""Make the README's screenshots, in docs/screenshots/.

    tools/screenshots.py            (make screenshots builds first)

Boots os.img in QEMU (headless) for the chooser's menu, then runs the
listings in docs/screenshots/ on their machines (as make run-listing
does) and, for the Spectrum, lists one of them too.
"""
import os
import subprocess
import sys
import tempfile
import time

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
sys.path.insert(0, os.path.join(ROOT, 'tests'))
import qemu_smoke as q

DIR = os.path.normpath(os.path.join(ROOT, 'docs', 'screenshots'))

# (listing, machine, seconds to let it run)
LISTINGS = [
    ('COLOURS.BAS', 'spectrum', 6),
    ('HIRES.BAS', 'a2fp', 10),
    ('LORES.BAS', 'a2int', 8),
    ('TERMINAL.BAS', 'a1basic', 15),
]


def wait_for(mon, tmp, want, seconds=30):
    deadline = time.time() + seconds
    while want not in q.screen_text(mon, tmp):
        if time.time() > deadline:
            sys.exit('timed out waiting for %r' % want)
        time.sleep(0.1)


def boot(image, tmp, shoot):
    mon_path = os.path.join(tmp, 'mon.sock')
    if os.path.exists(mon_path):
        os.unlink(mon_path)
    proc = subprocess.Popen(q.qemu_args('hdd', image, mon_path),
                            stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    try:
        shoot(q.Monitor(mon_path))
    finally:
        proc.kill()
        proc.wait()


def save(mon, name):
    path = os.path.join(DIR, name)
    mon.qmp('screendump', filename=os.path.abspath(path), format='png')
    print(path)


def menu(tmp):
    """The chooser's menu, on an image with every machine."""
    def shoot(mon):
        wait_for(mon, tmp, 'Choose a computer:')
        time.sleep(0.3)
        save(mon, 'menu.png')
    boot(os.path.join(ROOT, 'os.img'), tmp, shoot)


def spectrum_list(tmp):
    """COLOURS.BAS loaded and LISTed, with a line being typed."""
    image = os.path.join(tmp, 'list.img')
    subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'mkimage.py'), image,
                    '--machines', 'spectrum', os.path.join(DIR, 'COLOURS.BAS')], check=True)

    def shoot(mon):
        wait_for(mon, tmp, 'x86 BASIC for PCs')
        q.type_keys(mon, 'LOAD "COLOURS.BAS"\n')
        wait_for(mon, tmp, '0 OK')
        q.type_keys(mon, ' \n')         # clear the report, then Enter lists
        time.sleep(1)
        q.type_keys(mon, '170 PRINT AT 21,0;"Done"')
        time.sleep(0.5)
        save(mon, 'spectrum-list.png')
    boot(image, tmp, shoot)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        menu(tmp)
        spectrum_list(tmp)
    for listing, machine, wait in LISTINGS:
        out = os.path.join(DIR, machine + '.png')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'run_listing.py'),
                        os.path.join(DIR, listing), '--machine', machine,
                        '--wait', str(wait), '--out', out],
                       check=True, stdout=subprocess.DEVNULL)
        print(out)


if __name__ == '__main__':
    main()
