#!/usr/bin/env python3
"""Check a listing before it goes on the stick: each line is typed into the
machine's BASIC, run on this PC, and the lines it won't take are listed.

    tools/check.py [--machine spectrum|a2int|a1basic|a2fp|c64] [--run [SECONDS]] FILE ...

--machine is the BASIC the listing is for (spectrum unless the file is in
a folder named after another, such as A2FP/). The Spectrum's BASIC and
Integer BASIC check each line as it's typed, as on the originals, so this
finds mistyped lines; FP BASIC only finds mistakes when a line runs, so
for it (or for any), --run also runs the program, for up to SECONDS
(default 10) of this PC's time and as long as it doesn't wait for a key,
and prints what it shows. Each file is read as LOAD reads one: blank lines
are skipped, and for the Spectrum so are lines starting with #, and a \\ at
the end of a line joins the next one on.

BASIC V2 (c64) doesn't check lines as they're typed either; for it, the
lines with escapes in braces it doesn't know ({CLR} and the like: see
tools/c64.py) are listed, and those longer than the 80 characters its
editor takes (LOAD takes them). --run shows its screen when the program
stops.

It needs gcc, as make test does: the BASICs are built for this PC (into
tests/build/check/) the first time, and again when their source changes.
Exits with 1 if any line was refused.
"""
import argparse
import glob
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MACHINES = ['spectrum', 'a2int', 'a1basic', 'a2fp', 'c64']
CORE = ['storage', 'fat', 'string', 'num', 'font', 'keymap']


def host_binary(machine):
    """The machine's BASIC built for this PC (tests/host_stub.c), up to date."""
    dirs = ['machines/' + machine]
    uses = os.path.join(ROOT, 'machines', machine, 'uses')
    if os.path.exists(uses):
        dirs += ['machines/' + u for u in open(uses).read().split()]
    src = ['tests/host_stub.c'] + ['core/%s.c' % c for c in CORE]
    for d in dirs:
        src += sorted(os.path.relpath(p, ROOT) for p in glob.glob(os.path.join(ROOT, d, '*.c')))
    deps = src + [os.path.relpath(p, ROOT) for d in ['core'] + dirs
                  for p in glob.glob(os.path.join(ROOT, d, '*.h')) +
                  glob.glob(os.path.join(ROOT, d, '*', '*.c'))]   # (parts a file includes)
    out = os.path.join(ROOT, 'tests', 'build', 'check', machine)
    if not os.path.exists(out) or any(os.path.getmtime(os.path.join(ROOT, p)) >
                                      os.path.getmtime(out) for p in deps):
        os.makedirs(os.path.dirname(out), exist_ok=True)
        cmd = ['gcc', '-O1', '-fno-builtin', '-w', '-Icore'] + ['-I' + d for d in dirs] + \
            ['-DHOST_TEST', '-o', out] + src
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode:
            sys.exit('building %s for this PC failed:\n%s' % (machine, r.stderr))
    return out


def lines_of(path, machine):
    """(line number in the file, text) for each line, as LOAD reads it."""
    raw = open(path, 'rb').read().decode('latin-1').replace('\r\n', '\n').replace('\r', '\n')
    out, pending, start = [], '', 0
    for n, line in enumerate(raw.split('\n'), 1):
        if machine == 'spectrum':
            if not pending and line.lstrip().startswith('#'):
                continue
            if line.endswith('\\') and not line.endswith('\\\\'):
                if not pending:
                    start = n
                pending += line[:-1]
                continue
        text, at = pending + line, start if pending else n
        pending = ''
        if text.strip():
            out.append((at, text))
    if pending.strip():
        out.append((start, pending))
    return out


# What each BASIC prints when it won't take a line.
def refused(machine, out):
    if machine == 'spectrum':
        m = re.search(r'\[ERR\](.*)', out)
        return m.group(1) if m else None
    m = re.search(r'\*\*\* ?([A-Z0-9 >]*ERR)', out)
    return m.group(1) if m else None


SHOWN = 40                                # lines of what a run printed
TAGS = re.compile(r'\[(?:CLS|BORDER [0-9]+|TONE [0-9 ]+|AT [0-9,]+|CLICKS [0-9]+|ERR|'
                  r'SPK [0-9]+ @[0-9]+)\]')


def c64_problem(text, folder):
    """What's wrong with a line of a BASIC V2 listing, or None."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import c64
    m = re.match(r'\s*(\d+) ?(.*)', text)
    if int(m.group(1)) > 63999:
        return 'line number over 63999'
    unknown = []
    body = c64.to_petscii(m.group(2), folder['lower'], unknown)
    if unknown:
        return 'not known: %s' % ' '.join(unknown)
    if len(m.group(1)) + 1 + len(body) > 80:
        return 'longer than 80 characters (LOAD takes it; it can\'t be typed)'
    return None


def run(binary, machine, path, seconds):
    """Run the program from a disk with the file on it; what it printed."""
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    name = os.path.basename(path)
    base, ext = os.path.splitext(name)
    disk_name = (re.sub(r'[^A-Za-z0-9]', '_', base)[:8] + (ext if ext.upper() in
                 ('.BAS', '.TXT') else '.BAS')).upper()
    with tempfile.TemporaryDirectory() as tmp:
        img = os.path.join(tmp, 'disk.img')
        subprocess.run([sys.executable, os.path.join(ROOT, 'tests', 'make_disk.py'), img,
                        '/%s/%s=%s' % (machine.upper(), disk_name, path)], check=True)
        keys = ('LOAD "%s"\nRUN\n' % disk_name if machine == 'spectrum' else
                'load "%s",8\nrun\n' % disk_name.lower() if machine == 'c64'
                else 'RUN %s\n' % disk_name)
        screen = os.path.join(tmp, 'screen.txt')
        try:
            r = subprocess.run([binary, keys], capture_output=True, timeout=seconds,
                               env=dict(os.environ, DISK_IMAGE=img, C64_SCREEN=screen))
            out = r.stdout.decode('latin-1')
        except subprocess.TimeoutExpired as e:
            out = (e.stdout or b'').decode('latin-1') + '\n(still running after %gs)' % seconds
        if machine == 'c64' and os.path.exists(screen):   # (its screen, not its {RIGHT}s)
            rows = open(screen, encoding='latin-1').read().split('\n\n')[0].split('\n')
            out = '\n'.join(r.rstrip() for r in rows).rstrip('\n')
    return TAGS.sub('', out)


def main():
    ap = argparse.ArgumentParser(description='Check listings for the BASICs.')
    ap.add_argument('files', nargs='+')
    ap.add_argument('--machine', choices=MACHINES)
    ap.add_argument('--run', nargs='?', type=float, const=10.0, metavar='SECONDS')
    a = ap.parse_args()
    bad = 0
    for path in a.files:
        machine = a.machine
        if not machine:
            folder = os.path.basename(os.path.dirname(os.path.abspath(path))).lower()
            machine = folder if folder in MACHINES else 'spectrum'
        binary = host_binary(machine)
        lines = lines_of(path, machine)
        problems = 0
        if machine == 'c64':
            sys.path.insert(0, os.path.join(ROOT, 'tools'))
            import c64
            folder = {'lower': c64.has_lower('\n'.join(t for _, t in lines))}
        for at, text in lines:
            if not text.lstrip()[:1].isdigit():
                print('%s:%d: not a program line (no line number): %s' % (path, at, text))
                problems += 1
                continue
            if machine == 'c64':
                why = c64_problem(text, folder)
                if why is not None:
                    print('%s:%d: %s  (%s)' % (path, at, text.strip(), why))
                    problems += 1
                continue
            out = subprocess.run([binary, text + '\n'], capture_output=True,
                                 timeout=10).stdout.decode('latin-1')
            why = refused(machine, out)
            if why is not None:
                print('%s:%d: %s' % (path, at, why if machine == 'spectrum' else
                                     '%s  (%s)' % (text.strip(), why)))
                problems += 1
        print('%s: %d line%s, %s (%s)' % (path, len(lines), '' if len(lines) == 1 else 's',
                                          '%d refused' % problems if problems else 'all taken',
                                          machine))
        bad += problems
        if a.run is not None:
            print('--- running it:')
            shown = run(binary, machine, path, a.run).strip('\n').split('\n')
            print('\n'.join(shown[:SHOWN]))
            if len(shown) > SHOWN:
                print('... (%d lines more)' % (len(shown) - SHOWN))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
