#!/usr/bin/env python3
"""Seeds and a dictionary for one machine's fuzzer (tests/fuzz.sh).

    tests/fuzz/seeds.py MACHINE CORPUS_DIR DICT_FILE

An input is a file, a 0 byte, then the keys typed (see tests/host_stub.c,
FUZZ): the seeds are this project's listings, typed in and also as files
loaded and run, and the dictionary is the machine's keywords (taken from
its source, so that it keeps up) and the commands that read the file.
"""
import glob
import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Commands that read the fuzzed file (FUZZ.BAS, FUZZ.SAV).
LOADS = {
    'spectrum': ['LOAD "fuzz.bas"\n', 'MERGE "fuzz.bas"\n', 'LOAD ""\n', 'LOAD "" CODE\n',
                 'LOAD "" SCREEN$\n', 'LOAD "" DATA a()\n', 'LOAD "" DATA a$()\n',
                 'VERIFY "fuzz.bas"\n', 'CAT\n', 'RUN\n', 'LIST\n', 'EDIT\n'],
    'c64': ['load"fuzz.bas",8\n', 'load"fuzz.sav",8\n', 'load"$",8\n', 'run\n', 'list\n'],
    'apple': ['RUN FUZZ.BAS\n', 'LOAD FUZZ.BAS\n', 'EXEC FUZZ.BAS\n', 'BLOAD FUZZ.BAS\n',
              'CATALOG\n', 'RUN\n', 'LIST\n', '\x04OPEN FUZZ.BAS\n', 'PRINT CHR$(4);"',
              'READ FUZZ.BAS\n', 'CLOSE\n', 'POSITION', ',R', ',B', ',L', ',A'],
}
MAGIC = ['X86BSAV1', 'X86BSAV2']


def keywords(machine):
    def strings(path, start, end):
        s = open(os.path.join(ROOT, path)).read()
        a = s.index(start)
        return re.findall(r'"((?:[^"\\]|\\.)+)"', s[a:s.index(end, a)])
    if machine == 'spectrum':
        return strings('machines/spectrum/basic.c', 'token_names[] = {', '};')
    if machine == 'a2fp':
        return strings('machines/a2fp/fpbasic.c', 'keywords[] = {', '};')
    if machine == 'c64':          # (typed in lower case: upper case is shifted)
        return [k.lower() for k in strings('machines/c64/basic.c', 'keywords[] = {', '};')]
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import dsk
    return sorted({w for w in dsk.INT_TOKENS if w})


def listings(machine):
    if machine == 'spectrum':
        paths = glob.glob(os.path.join(ROOT, 'tests/listings/demo.bas'))
    elif machine == 'c64':
        paths = glob.glob(os.path.join(ROOT, 'tests/listings/c64_*.bas'))
    elif machine == 'a2fp':
        paths = [os.path.join(ROOT, 'tests/listings/dsk_test.bas')] + \
            glob.glob(os.path.join(ROOT, 'docs/screenshots/HIRES.BAS'))
    else:
        paths = [os.path.join(ROOT, 'tests/listings/dsk_int.bas')] + \
            glob.glob(os.path.join(ROOT, 'docs/screenshots/LORES.BAS')) + \
            glob.glob(os.path.join(ROOT, 'docs/screenshots/TERMINAL.BAS'))
    return [open(p, 'rb').read() for p in paths if os.path.exists(p)]


def main():
    machine, corpus, dict_file = sys.argv[1:4]
    kind = machine if machine in ('spectrum', 'c64') else 'apple'
    seeds = []
    for text in listings(machine):
        text = text.replace(b'\r', b'')
        run = b'run\n' if machine == 'c64' else b'RUN\n'   # (c64: upper case is shifted)
        seeds.append(b'\0' + text + run)                         # typed in
        for load in LOADS[kind][:3]:                              # loaded from a file
            seeds.append(text + b'\0' + load.encode() + run + run.replace(b'run', b'list')
                         .replace(b'RUN', b'LIST'))
    for s in seeds:
        name = hashlib.sha1(s).hexdigest()
        with open(os.path.join(corpus, name), 'wb') as f:
            f.write(s)
    entries = set(keywords(machine)) | set(LOADS[kind]) | set(MAGIC) | {'\n', '"', ':', '('}
    with open(dict_file, 'w') as f:
        for e in sorted(entries):
            f.write('"%s"\n' % ''.join(c if 32 <= ord(c) < 127 and c not in '"\\' else
                                       '\\x%02x' % ord(c) for c in e))


if __name__ == '__main__':
    main()
