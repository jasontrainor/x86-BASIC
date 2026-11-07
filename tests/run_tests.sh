#!/bin/bash
# Host-side tests for the BASIC interpreter.
# Builds kernel/basic.c + kernel/string.c against tests/host_stub.c with
# AddressSanitizer/UBSan, feeds keystrokes in, and checks the output.
set -u
cd "$(dirname "$0")/.."
mkdir -p tests/build
BIN=tests/build/basic_host
gcc -g -O1 -fno-builtin -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Wall -Ikernel \
    -o "$BIN" tests/host_stub.c kernel/basic.c kernel/string.c || exit 1

pass=0; fail=0
# check NAME INPUT EXPECTED [EXPECTED...]
#   Each EXPECTED is a fixed string that must appear in the output.
#   A string starting with '!' must NOT appear.
#   Typed input is echoed to the output, so expectations should be strings
#   that only the interpreter's response can produce.
check() {
    local name=$1 in=$2; shift 2
    local out ok=1
    out=$("$BIN" "$in" 2>&1; echo "<EOF>")   # sentinel keeps trailing newlines
    if [[ $out == *AddressSanitizer* || $out == *"runtime error"* ]]; then ok=0; fi
    for want in "$@"; do
        if [[ $want == !* ]]; then
            [[ $out == *"${want:1}"* ]] && ok=0
        else
            [[ $out == *"$want"* ]] || ok=0
        fi
    done
    if ((ok)); then pass=$((pass+1)); echo "PASS $name"
    else fail=$((fail+1)); echo "FAIL $name"; sed 's/^/    | /' <<<"$out" | head -20; fi
}
NL=$'\n'

# --- Basics -------------------------------------------------------------
check "let and print"    "LET A=6*7${NL}PRINT A${NL}" "${NL}42${NL}"
check "implicit let"     "B=2+3*4${NL}PRINT B${NL}" "${NL}14${NL}"
check "string variable"  "A\$=\"HI\"${NL}PRINT A\$;A\$${NL}" "HIHI"
check "list sorts lines" "20 PRINT 2${NL}10 PRINT 1${NL}LIST${NL}" "LIST${NL}10 PRINT 1${NL}20 PRINT 2${NL}"
check "delete line"      "10 PRINT 1${NL}20 PRINT 2${NL}10${NL}LIST${NL}" "LIST${NL}20 PRINT 2${NL}"

# --- Keywords are dispatched (not swallowed as assignments) --------------
check "for/next loop"    "10 FOR I=1 TO 3${NL}20 PRINT I*11${NL}30 NEXT I${NL}RUN${NL}" "${NL}11${NL}22${NL}33${NL}"
check "for step"         "10 FOR I=10 TO 1 STEP 0-3${NL}20 PRINT I*11${NL}30 NEXT I${NL}RUN${NL}" "${NL}110${NL}77${NL}44${NL}11${NL}"
check "goto"             "10 GOTO 30${NL}20 PRINT 1+1${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}3${NL}" "!${NL}2${NL}"
check "if then true"     "IF 1=1 THEN PRINT 2*21${NL}" "${NL}42${NL}"
check "if then false"    "IF 1=2 THEN PRINT 2*21${NL}" "!42"
check "end stops"        "10 PRINT 1+1${NL}20 END${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}2${NL}" "!${NL}3${NL}"
check "cls"              "CLS${NL}" "[CLS]"
check "colours"          "INK 2${NL}PAPER 5${NL}BORDER 1${NL}" "[INK 2]" "[PAPER 5]" "[BORDER 1]"
check "beep"             "BEEP${NL}" "[BEEP]"

echo "$pass passed, $fail failed"
((fail == 0))
