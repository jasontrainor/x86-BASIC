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
    # timeout: programs that loop forever without printing are cut off.
    # The sentinel keeps trailing newlines that $(...) would strip.
    out=$(timeout 2 "$BIN" "$in" 2>&1; echo "<EOF>")
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

# --- Errors are reported, and stop the program --------------------------
check "unknown statement" "10 FROB${NL}RUN${NL}" "ERROR: SYNTAX ERROR IN LINE 10"
check "trailing junk"     "PRINT 1 2${NL}" "ERROR: SYNTAX ERROR"
check "error stops run"   "10 PRINT 1+1${NL}20 GOTO 99${NL}30 PRINT 1+2${NL}RUN${NL}" "ERROR: LINE NOT FOUND IN LINE 20" "!${NL}3${NL}"
check "if without then"   "IF 1=1 PRINT 5${NL}" "ERROR: THEN EXPECTED"
check "division by zero"  "PRINT 1/0${NL}" "ERROR: DIVISION BY ZERO"
check "missing paren"     "PRINT (1+2${NL}" "ERROR: MISSING )"
check "bad expression"    "PRINT 1+*2${NL}" "ERROR: SYNTAX ERROR"
check "for without to"    "FOR I=1${NL}" "ERROR: TO EXPECTED"
check "next without for"  "10 NEXT I${NL}RUN${NL}" "ERROR: NEXT WITHOUT FOR IN LINE 10"
check "let without ="     "LET A 5${NL}" "ERROR: SYNTAX ERROR"
check "unary minus"       "A=5${NL}PRINT -A*2${NL}PRINT -(2+3)${NL}" "${NL}-10${NL}" "${NL}-5${NL}"
check "for stack full"    "10 FOR A=1 TO 2${NL}11 FOR B=1 TO 2${NL}12 FOR C=1 TO 2${NL}13 FOR D=1 TO 2${NL}14 FOR E=1 TO 2${NL}15 FOR F=1 TO 2${NL}16 FOR G=1 TO 2${NL}17 FOR H=1 TO 2${NL}18 FOR I=1 TO 2${NL}19 FOR J=1 TO 2${NL}20 FOR K=1 TO 2${NL}21 FOR L=1 TO 2${NL}22 FOR M=1 TO 2${NL}23 FOR N=1 TO 2${NL}24 FOR O=1 TO 2${NL}25 FOR P=1 TO 2${NL}26 FOR Q=1 TO 2${NL}RUN${NL}" "TOO MANY NESTED FOR LOOPS IN LINE 26"
check "goto into for"     "10 FOR I=1 TO 2${NL}20 PRINT 7*7${NL}30 GOTO 10${NL}RUN${NL}" "[OUTPUT LIMIT]" "!TOO MANY"
check "next skips inner"  "10 FOR I=1 TO 2${NL}20 FOR J=1 TO 5${NL}30 NEXT I${NL}40 PRINT I*100${NL}RUN${NL}" "${NL}300${NL}"

# --- Input bounds ---------------------------------------------------------
A60=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
check "line too long"     "10 PRINT \"${A60}\"${NL}20 PRINT 2*2${NL}LIST${NL}" "ERROR: LINE TOO LONG IN LINE 10" "LIST${NL}20 PRINT 2*2${NL}"
check "line max length"   "10 PRINT \"${A60:0:54}\"${NL}LIST${NL}" "!ERROR"
check "string too long"   "A\$=\"${A60}${A60}\"${NL}" "ERROR: STRING TOO LONG"
check "line number range" "0 PRINT 1${NL}99999999999 PRINT 1${NL}" "LINE NUMBER MUST BE 1-9999"

FULL=""; for i in $(seq 1 256); do FULL+="$i PRINT 1${NL}"; done
check "program full"      "${FULL}257 PRINT 1${NL}" "ERROR: PROGRAM FULL IN LINE 257"
check "full: replace ok"  "${FULL}256 PRINT 2${NL}" "!PROGRAM FULL"

# --- Variable names are validated ------------------------------------------
check "let digit var"     "LET 5=3${NL}" "ERROR: BAD VARIABLE NAME"
check "let string digit"  "LET 5\$=\"X\"${NL}" "ERROR: BAD VARIABLE NAME"
check "for no var"        "FOR =1 TO 5${NL}" "ERROR: BAD VARIABLE NAME"
check "next no var"       "10 FOR I=1 TO 2${NL}20 NEXT${NL}RUN${NL}" "ERROR: BAD VARIABLE NAME IN LINE 20"
check "lowercase var"     "let z=9*9${NL}PRINT Z${NL}" "${NL}81${NL}"

# --- Comparisons ----------------------------------------------------------
# Each line prints 1+1 when the condition holds, right after its echo.
T=" THEN PRINT 1+1${NL}"
check "true comparisons"  "IF 1=1${T}IF 1<>2${T}IF 1<2${T}IF 2<=2${T}IF 3>2${T}IF 2>=2${T}" \
    "1=1${T}2${NL}" "1<>2${T}2${NL}" "1<2${T}2${NL}" "2<=2${T}2${NL}" "3>2${T}2${NL}" "2>=2${T}2${NL}"
check "false comparisons" "IF 1=2 THEN PRINT 1+1${NL}IF 2<>2 THEN PRINT 1+1${NL}IF 2<1 THEN PRINT 1+1${NL}IF 3<=2 THEN PRINT 1+1${NL}IF 2>3 THEN PRINT 1+1${NL}IF 1>=2 THEN PRINT 1+1${NL}" "!${NL}2${NL}"
check "no comparison"     "IF 1${NL}" "ERROR: COMPARISON EXPECTED"

# --- Keyword matching -------------------------------------------------------
check "mixed case"        "Print 3*3${NL}pRiNt 4*4${NL}" "${NL}9${NL}" "${NL}16${NL}"
check "mixed case for"    "10 For I=1 To 2${NL}20 Print I*50${NL}30 Next I${NL}Run${NL}" "${NL}50${NL}100${NL}"
check "keyword prefix"    "10 PRINT 3*3${NL}RUNX${NL}" "ERROR: SYNTAX ERROR" "!${NL}9${NL}"
check "no space needed"   "10 GOTO30${NL}20 PRINT 1+1${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}3${NL}" "!${NL}2${NL}"
check "two-letter var"    "AB=5${NL}" "ERROR: SYNTAX ERROR"

# --- Integer limits -----------------------------------------------------------
check "most negative int" "A=0-2147483647-1${NL}PRINT A${NL}" "${NL}-2147483648${NL}"
check "literal too big"   "PRINT 99999999999${NL}" "ERROR: NUMBER TOO BIG"
check "multiply overflow" "PRINT 65536*65536${NL}" "ERROR: NUMBER TOO BIG"
check "add overflow"      "PRINT 2147483647+1${NL}" "ERROR: NUMBER TOO BIG"
check "min / -1"          "A=0-2147483647-1${NL}PRINT A/(0-1)${NL}" "ERROR: NUMBER TOO BIG"
check "for at int max"    "10 FOR I=2147483646 TO 2147483647${NL}20 PRINT 1+1${NL}30 NEXT I${NL}40 PRINT 3+3${NL}RUN${NL}" "${NL}2${NL}2${NL}6${NL}"

echo "$pass passed, $fail failed"
((fail == 0))
