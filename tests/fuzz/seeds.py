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
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import dsk
    return sorted({w for w in dsk.INT_TOKENS if w})


def listings(machine):
    if machine == 'spectrum':
        paths = glob.glob(os.path.join(ROOT, 'tests/listings/demo.bas'))
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
    kind = 'spectrum' if machine == 'spectrum' else 'apple'
    seeds = []
    for text in listings(machine):
        text = text.replace(b'\r', b'')
        seeds.append(b'\0' + text + b'RUN\n')                     # typed in
        for load in LOADS[kind][:3]:                              # loaded from a file
            seeds.append(text + b'\0' + load.encode() + b'RUN\nLIST\n')
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
