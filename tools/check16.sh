#!/bin/sh
# Fail if a 16-bit kernel object file contains an instruction that uses
# 16-bit addressing (SI, DI, BX or BP as an address).
#
# The kernel's data may live above 64KB ("unreal mode", see core/bios.ld),
# which only 32-bit addresses can reach. GCC's -m16 code always uses
# them, except when it emits a string instruction (movs, stos, ...): the
# assembler leaves those with 16-bit addressing. Such code works in
# QEMU's emulator, which doesn't check segment limits, but faults on a
# real CPU, so catch it at build time.
#
#   tools/check16.sh file.o...
status=0
for f in "$@"; do
    bad=$(objdump -d -M addr16,data16 "$f" |
          grep -E '\((%bx|%si|%di|%bp)|(movs|stos|lods|scas|cmps|ins|outs)[bwl]? ' |
          grep -vE '\s(lea|nop[wl]?)\s|movs[bw][wl]\s|cmovs')
    if [ -n "$bad" ]; then
        echo "$f: instructions with 16-bit addressing (see tools/check16.sh):" >&2
        echo "$bad" >&2
        status=1
    fi
done
exit $status
