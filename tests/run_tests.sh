#!/bin/bash
# Host-side tests for the BASIC interpreter.
# Builds kernel/basic.c + kernel/string.c against tests/host_stub.c with
# AddressSanitizer/UBSan, feeds keystrokes in, and checks the output.
set -u
cd "$(dirname "$0")/.."
mkdir -p tests/build
BIN=tests/build/basic_host
gcc -g -O1 -fno-builtin -fsanitize=address,undefined -fno-sanitize-recover=all \
    -Wall -Ikernel -DHOST_TEST \
    -o "$BIN" tests/host_stub.c kernel/basic.c kernel/string.c kernel/num.c || exit 1

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
check "unary minus"       "A=5${NL}PRINT -A*2${NL}PRINT -(2+3)${NL}PRINT 3--2${NL}" "${NL}-10${NL}" "${NL}-5${NL}" "${NL}5${NL}"
check "for stack full"    "10 FOR A=1 TO 2${NL}11 FOR B=1 TO 2${NL}12 FOR C=1 TO 2${NL}13 FOR D=1 TO 2${NL}14 FOR E=1 TO 2${NL}15 FOR F=1 TO 2${NL}16 FOR G=1 TO 2${NL}17 FOR H=1 TO 2${NL}18 FOR I=1 TO 2${NL}19 FOR J=1 TO 2${NL}20 FOR K=1 TO 2${NL}21 FOR L=1 TO 2${NL}22 FOR M=1 TO 2${NL}23 FOR N=1 TO 2${NL}24 FOR O=1 TO 2${NL}25 FOR P=1 TO 2${NL}26 FOR Q=1 TO 2${NL}RUN${NL}" "TOO MANY NESTED FOR LOOPS IN LINE 26"
check "goto into for"     "10 FOR I=1 TO 2${NL}20 PRINT 7*7${NL}30 GOTO 10${NL}RUN${NL}" "[OUTPUT LIMIT]" "!TOO MANY"
check "next skips inner"  "10 FOR I=1 TO 2${NL}20 FOR J=1 TO 5${NL}30 NEXT I${NL}40 PRINT I*100${NL}RUN${NL}" "${NL}300${NL}"

# --- Input bounds ---------------------------------------------------------
A60=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
A200=${A60}${A60}${A60}${A60:0:20}
check "long line"         "10 PRINT \"${A200}\"${NL}LIST${NL}" "LIST${NL}10 PRINT \"${A200}\"${NL}" "!ERROR"
check "string too long"   "A\$=\"${A200}\"${NL}A\$=A\$+A\$${NL}" "ERROR: STRING TOO LONG"
check "line number range" "0 PRINT 1${NL}99999999999 PRINT 1${NL}" "LINE NUMBER MUST BE 1-9999"

FULL=""; for i in $(seq 1 200); do FULL+="$i REM ${A200}${NL}"; done
check "program full"      "${FULL}PRINT 6*7${NL}" "ERROR: PROGRAM FULL IN LINE 160" "${NL}42${NL}"

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
check "number as condition" "IF 2 THEN PRINT 6*7${NL}IF 0 THEN PRINT 6*8${NL}" "${NL}42${NL}" "!48"

# --- Keyword matching -------------------------------------------------------
check "mixed case"        "Print 3*3${NL}pRiNt 4*4${NL}" "${NL}9${NL}" "${NL}16${NL}"
check "mixed case for"    "10 For I=1 To 2${NL}20 Print I*50${NL}30 Next I${NL}Run${NL}" "${NL}50${NL}100${NL}"
check "keyword prefix"    "10 PRINT 3*3${NL}RUNX${NL}" "ERROR: SYNTAX ERROR" "!${NL}9${NL}"
check "no space needed"   "10 GOTO30${NL}20 PRINT 1+1${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}3${NL}" "!${NL}2${NL}"
check "long names"        "SCORE=5${NL}TOTAL2=SCORE*2${NL}PRINT TOTAL2+1${NL}" "${NL}11${NL}"
check "keyword in name"   "FORM=3${NL}TOTAL=4${NL}PRINT FORM*TOTAL${NL}" "${NL}12${NL}"
check "name too long"     "ABCDEFGHIJKLMNOPQ=1${NL}" "ERROR: NAME TOO LONG"

# --- Numbers ---------------------------------------------------------------
check "decimals"          "PRINT 7/2${NL}PRINT 1/3${NL}PRINT 2/3${NL}PRINT .5+.25${NL}" "${NL}3.5${NL}" "${NL}0.33333333${NL}" "${NL}0.66666667${NL}" "${NL}0.75${NL}"
check "negative decimal"  "PRINT -1/4${NL}PRINT 0-0.001${NL}" "${NL}-0.25${NL}" "${NL}-0.001${NL}"
check "big whole numbers" "PRINT 2147483647+1${NL}PRINT 65536*65536${NL}" "${NL}2147483648${NL}" "${NL}4294967296${NL}"
check "exponent output"   "PRINT 1E15${NL}PRINT 12345678912345${NL}PRINT 1/1E7${NL}" "${NL}1E+15${NL}" "${NL}1.2345679E+13${NL}" "${NL}1E-7${NL}"
check "exponent input"    "PRINT 1.5E3${NL}PRINT 25E-2${NL}" "${NL}1500${NL}" "${NL}0.25${NL}"
check "small fixed"       "PRINT 0.00012${NL}" "${NL}0.00012${NL}"
check "rounding"          "PRINT 0.1+0.2${NL}PRINT 1/3*3${NL}" "${NL}0.3${NL}" "${NL}1${NL}"
check "literal too big"   "PRINT 1E400${NL}" "ERROR: NUMBER TOO BIG"
check "multiply overflow" "PRINT 1E200*1E200${NL}" "ERROR: NUMBER TOO BIG"
check "power"             "PRINT 2^10${NL}PRINT -2^2${NL}PRINT 2^-1${NL}PRINT (-2)^3${NL}PRINT 2^0.5${NL}" "${NL}1024${NL}" "${NL}-4${NL}" "${NL}0.5${NL}" "${NL}-8${NL}" "${NL}1.4142136${NL}"
check "power errors"      "PRINT (-8)^(1/3)${NL}PRINT 0^-1${NL}" "ERROR: INVALID ARGUMENT"
check "functions"         "PRINT SQR 16${NL}PRINT ABS -3${NL}PRINT INT 2.7${NL}PRINT INT -2.5${NL}PRINT SGN -9${NL}" "${NL}4${NL}" "${NL}3${NL}" "${NL}2${NL}" "${NL}-3${NL}" "${NL}-1${NL}"
check "trig"              "PRINT SIN (PI/2)${NL}PRINT COS 0${NL}PRINT INT (ATN 1*4*1000)${NL}PRINT TAN (PI/4)${NL}" "${NL}1${NL}" "${NL}1${NL}" "${NL}3141${NL}"
check "asn acs"           "PRINT ASN 1*2${NL}PRINT ACS 1${NL}" "${NL}3.1415927${NL}" "${NL}0${NL}"
check "ln exp"            "PRINT LN EXP 2${NL}PRINT EXP 1${NL}PRINT EXP 0${NL}" "${NL}2${NL}" "${NL}2.7182818${NL}" "${NL}1${NL}"
check "function binding"  "PRINT SQR 4*3${NL}PRINT SQR (4*4)${NL}" "${NL}6${NL}" "${NL}4${NL}"
check "invalid argument"  "PRINT SQR -1${NL}" "ERROR: INVALID ARGUMENT"
check "ln of zero"        "PRINT LN 0${NL}" "ERROR: INVALID ARGUMENT"
check "decimal for step"  "10 FOR I=0 TO 1 STEP 0.25${NL}20 PRINT I${NL}30 NEXT I${NL}RUN${NL}" "${NL}0${NL}0.25${NL}0.5${NL}0.75${NL}1${NL}"
check "integer range"     "INK 9${NL}" "ERROR: INTEGER OUT OF RANGE"
check "goto rounds"       "10 GOTO 19.6${NL}20 PRINT 4*5${NL}RUN${NL}" "${NL}20${NL}"

# --- Statements per line, comments, strings ---------------------------------
check "colon"             "PRINT 1+1: PRINT 2+2${NL}" "${NL}2${NL}4${NL}"
check "colon in program"  "10 A=1: B=2: PRINT A+B${NL}RUN${NL}" "${NL}3${NL}"
check "for on one line"   "FOR I=1 TO 3: PRINT I*7;: NEXT I${NL}" "71421"
check "if skips rest"     "IF 0 THEN PRINT 1+1: PRINT 3+3${NL}PRINT 5+5${NL}" "!${NL}6${NL}" "${NL}10${NL}"
check "if runs rest"      "IF 1 THEN PRINT 1+1: PRINT 3+3${NL}" "${NL}2${NL}6${NL}"
check "if then line"      "10 IF 1 THEN 30${NL}20 PRINT 1+1${NL}30 PRINT 3+3${NL}RUN${NL}" "${NL}6${NL}" "!${NL}2${NL}"
check "rem"               "10 REM PRINT 1+1: PRINT 9${NL}20 PRINT 3+3${NL}RUN${NL}" "${NL}6${NL}" "!${NL}2${NL}" "!${NL}9${NL}"
check "for skipped"       "10 FOR I=5 TO 1${NL}20 PRINT 1+1${NL}30 NEXT I${NL}40 PRINT I*10${NL}RUN${NL}" "${NL}50${NL}" "!${NL}2${NL}"
check "for no next"       "10 FOR I=5 TO 1${NL}RUN${NL}" "ERROR: FOR WITHOUT NEXT IN LINE 10"
check "string concat"     "A\$=\"AB\"${NL}B\$=A\$+\"CD\"+A\$${NL}PRINT B\$${NL}" "${NL}ABCDAB${NL}"
check "string compare"    "IF \"ABC\"<\"ABD\" THEN PRINT 1+1${NL}IF \"A\"=\"A\" THEN PRINT 2+2${NL}IF \"AB\">\"A\" THEN PRINT 3+3${NL}" "${NL}2${NL}" "${NL}4${NL}" "${NL}6${NL}"
check "type mismatch"     "PRINT \"A\"+1${NL}" "ERROR: STRING EXPECTED"
check "type mismatch 2"   "A=\"X\"${NL}" "ERROR: NUMBER EXPECTED"
check "variable not found" "PRINT Q+1${NL}" "ERROR: VARIABLE NOT FOUND"
check "run clears vars"   "A=5${NL}10 PRINT A${NL}RUN${NL}" "ERROR: VARIABLE NOT FOUND IN LINE 10"
check "print ; at end"    "PRINT 1+1;: PRINT 3+3${NL}" "${NL}26${NL}"
check "list detokenizes"  "10 print a:goto 10${NL}20 if a<=b then print \"if\"${NL}LIST${NL}" "10 PRINT A: GOTO 10${NL}20 IF A<=B THEN PRINT \"if\"${NL}"
check "list rem keeps text" "10 rem Hello print${NL}LIST${NL}" "10 REM Hello print${NL}"

# --- BREAK --------------------------------------------------------------------
ESC=$'\x1b'
check "break stops loop"  "10 GOTO 10${NL}RUN${NL}${ESC}PRINT 5*5${NL}" "BREAK IN LINE 10" "${NL}25${NL}"

echo "$pass passed, $fail failed"
((fail == 0))
