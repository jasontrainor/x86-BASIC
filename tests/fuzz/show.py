#!/usr/bin/env python3
"""Print a fuzzer input (tests/fuzz.sh) readably: the file it puts on the
disk, then the keys it types.

    tests/fuzz/show.py FILE
"""
import sys

data = open(sys.argv[1], 'rb').read()
file, zero, typed = data.partition(b'\0')
if not zero:
    file, typed = b'', data
print('--- FUZZ.BAS / FUZZ.SAV (%d bytes):' % len(file))
print(repr(file)[2:-1])
print('--- typed (%d bytes):' % len(typed))
print(repr(typed)[2:-1])
