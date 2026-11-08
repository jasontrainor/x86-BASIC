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
    -o "$BIN" tests/host_stub.c kernel/basic.c kernel/screen.c kernel/font.c kernel/editor.c kernel/storage.c kernel/string.c kernel/num.c kernel/memory.c || exit 1

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
    out=$(timeout 10 "$BIN" "$in" 2>&1; echo "<EOF>")
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
ESC=$'\x1b'
# Editing keys, as the test platform reads them (see tests/host_stub.c)
L=$'\x02'; R=$'\x06'; UP=$'\x10'; DN=$'\x0e'; HOME=$'\x01'; END=$'\x05'; DEL=$'\x7f'; BS=$'\b'

# --- Basics -------------------------------------------------------------
check "let and print"    "LET A=6*7${NL}PRINT A${NL}" "${NL}42${NL}"
check "implicit let"     "B=2+3*4${NL}PRINT B${NL}" "${NL}14${NL}"
check "string variable"  "A\$=\"HI\"${NL}PRINT A\$;A\$${NL}" "HIHI"
check "list sorts lines" "20 PRINT 2${NL}10 PRINT 1${NL}LIST${NL}" "LIST${NL}  10>PRINT 1${NL}  20 PRINT 2${NL}"
check "delete line"      "10 PRINT 1${NL}20 PRINT 2${NL}10${NL}LIST${NL}" "LIST${NL}  20 PRINT 2${NL}"

# --- Keywords are dispatched (not swallowed as assignments) --------------
check "for/next loop"    "10 FOR I=1 TO 3${NL}20 PRINT I*11${NL}30 NEXT I${NL}RUN${NL}" "${NL}11${NL}22${NL}33${NL}"
check "for step"         "10 FOR I=10 TO 1 STEP 0-3${NL}20 PRINT I*11${NL}30 NEXT I${NL}RUN${NL}" "${NL}110${NL}77${NL}44${NL}11${NL}"
check "goto"             "10 GOTO 30${NL}20 PRINT 1+1${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}3${NL}" "!${NL}2${NL}"
check "if then true"     "IF 1=1 THEN PRINT 2*21${NL}" "${NL}42${NL}"
check "if then false"    "IF 1=2 THEN PRINT 2*21${NL}" "!42"
check "end stops"        "10 PRINT 1+1${NL}20 END${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}2${NL}" "!${NL}3${NL}"
check "cls"              "CLS${NL}" "[CLS]"
check "colours"          "INK 2${NL}PAPER 5${NL}BORDER 1${NL}" "[BORDER 1]" "![ERR]"
check "beep"             "BEEP${NL}BEEP 1,12${NL}BEEP .5,0${NL}" "[TONE 1000 200]" "[TONE 523 1000]" "[TONE 261 500]"

# --- Errors are reported, and stop the program --------------------------
check "unknown statement" "10 FROB${NL}RUN${NL}" "C Nonsense in BASIC, 10:"
check "trailing junk"     "PRINT 1 2${NL}" "C Nonsense in BASIC, 0:"
check "error stops run"   "10 PRINT 1+1${NL}20 PRINT Q${NL}30 PRINT 1+2${NL}RUN${NL}" "2 Variable not found, 20:" "!${NL}3${NL}"
check "if without then"   "IF 1=1 PRINT 5${NL}" "C Nonsense in BASIC, 0:"
check "division by zero"  "PRINT 1/0${NL}" "6 Number too big, 0:"
check "missing paren"     "PRINT (1+2${NL}" "C Nonsense in BASIC, 0:"
check "bad expression"    "PRINT 1+*2${NL}" "C Nonsense in BASIC, 0:"
check "for without to"    "FOR I=1${NL}" "C Nonsense in BASIC, 0:"
check "next without for"  "10 NEXT I${NL}RUN${NL}" "1 NEXT without FOR, 10:"
check "let without ="     "LET A 5${NL}" "C Nonsense in BASIC, 0:"
check "unary minus"       "A=5${NL}PRINT -A*2${NL}PRINT -(2+3)${NL}PRINT 3--2${NL}" "${NL}-10${NL}" "${NL}-5${NL}" "${NL}5${NL}"
check "many for loops"     "10 FOR A=1 TO 2${NL}11 FOR B=1 TO 2${NL}12 FOR C=1 TO 2${NL}13 FOR D=1 TO 2${NL}14 FOR E=1 TO 2${NL}15 FOR F=1 TO 2${NL}16 FOR G=1 TO 2${NL}17 FOR H=1 TO 2${NL}18 FOR I=1 TO 2${NL}19 FOR J=1 TO 2${NL}20 FOR K=1 TO 2${NL}21 FOR L=1 TO 2${NL}22 FOR M=1 TO 2${NL}23 FOR N=1 TO 2${NL}24 FOR O=1 TO 2${NL}25 FOR P=1 TO 2${NL}26 FOR Q=1 TO 2${NL}RUN${NL}" "![ERR]"
check "goto into for"     "10 FOR I=1 TO 2${NL}20 PRINT 7*7${NL}30 GOTO 10${NL}RUN${NL}" "[OUTPUT LIMIT]" "!TOO MANY"
check "next skips inner"  "10 FOR I=1 TO 2${NL}20 FOR J=1 TO 5${NL}30 NEXT I${NL}40 PRINT I*100${NL}RUN${NL}" "${NL}300${NL}"

# --- Input bounds ---------------------------------------------------------
A60=AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA
A200=${A60}${A60}${A60}${A60:0:20}
check "long line"         "10 PRINT \"${A200}\"${NL}LIST${NL}" "LIST${NL}  10>PRINT \"${A200}\"${NL}" "![ERR]"
check "long strings"      "A\$=\"${A200}\"${NL}A\$=A\$+A\$+A\$+A\$: PRINT LEN A\$${NL}" "${NL}800${NL}" "![ERR]"
check "line number range" "0 PRINT 1${NL}99999999999 PRINT 1${NL}" "C Nonsense in BASIC"

FULL=""; for i in $(seq 1 200); do FULL+="$i REM ${A200}${NL}"; done
check "program full"      "${FULL}PRINT 6*7${NL}" "G No room for line, 0:" "${NL}42${NL}"

# --- Variable names are validated ------------------------------------------
check "let digit var"     "LET 5=3${NL}" "C Nonsense in BASIC, 0:"
check "let string digit"  "LET 5\$=\"X\"${NL}" "C Nonsense in BASIC, 0:"
check "for no var"        "FOR =1 TO 5${NL}" "C Nonsense in BASIC, 0:"
check "next no var"       "10 FOR I=1 TO 2${NL}20 NEXT${NL}RUN${NL}" "C Nonsense in BASIC, 20:"
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
check "keyword prefix"    "10 PRINT 3*3${NL}RUNX${NL}" "C Nonsense in BASIC, 0:" "!${NL}9${NL}"
check "no space needed"   "10 GOTO30${NL}20 PRINT 1+1${NL}30 PRINT 1+2${NL}RUN${NL}" "${NL}3${NL}" "!${NL}2${NL}"
check "long names"        "SCORE=5${NL}TOTAL2=SCORE*2${NL}PRINT TOTAL2+1${NL}" "${NL}11${NL}"
check "keyword in name"   "FORM=3${NL}TOTAL=4${NL}PRINT FORM*TOTAL${NL}" "${NL}12${NL}"
check "name too long"     "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEFG=1${NL}" "C Nonsense in BASIC, 0:"

# --- Numbers ---------------------------------------------------------------
check "decimals"          "PRINT 7/2${NL}PRINT 1/3${NL}PRINT 2/3${NL}PRINT .5+.25${NL}" "${NL}3.5${NL}" "${NL}0.33333333${NL}" "${NL}0.66666667${NL}" "${NL}0.75${NL}"
check "negative decimal"  "PRINT -1/4${NL}PRINT 0-0.001${NL}" "${NL}-0.25${NL}" "${NL}-.001${NL}"
check "big whole numbers" "PRINT 2147483647+1${NL}PRINT 65536*65536${NL}" "${NL}2.1474836E+9${NL}" "${NL}4.2949673E+9${NL}"
check "exponent output"   "PRINT 1E15${NL}PRINT 12345678912345${NL}PRINT 1/1E7${NL}" "${NL}1E+15${NL}" "${NL}1.2345679E+13${NL}" "${NL}1E-7${NL}"
check "exponent input"    "PRINT 1.5E3${NL}PRINT 25E-2${NL}" "${NL}1500${NL}" "${NL}0.25${NL}"
check "spectrum formats"  "PRINT 12345678;\" \";123456789;\" \";1E8${NL}PRINT .00001;\" \";.000001;\" \";0.5;\" \";2/3;\" \";PI${NL}" "${NL}12345678 1.2345679E+8 1E+8${NL}" "${NL}.00001 1E-6 0.5 0.66666667 3.1415927${NL}"
check "small fixed"       "PRINT 0.00012${NL}" "${NL}.00012${NL}"
check "rounding"          "PRINT 0.1+0.2${NL}PRINT 1/3*3${NL}" "${NL}0.3${NL}" "${NL}1${NL}"
check "literal too big"   "PRINT 1E400${NL}" "6 Number too big, 0:"
check "multiply overflow" "PRINT 1E200*1E200${NL}" "6 Number too big, 0:"
check "power"             "PRINT 2^10${NL}PRINT -2^2${NL}PRINT 2^-1${NL}PRINT (-2)^3${NL}PRINT 2^0.5${NL}" "${NL}1024${NL}" "${NL}-4${NL}" "${NL}0.5${NL}" "${NL}-8${NL}" "${NL}1.4142136${NL}"
check "power errors"      "PRINT (-8)^(1/3)${NL}PRINT 0^-1${NL}" "A Invalid argument, 0:"
check "functions"         "PRINT SQR 16${NL}PRINT ABS -3${NL}PRINT INT 2.7${NL}PRINT INT -2.5${NL}PRINT SGN -9${NL}" "${NL}4${NL}" "${NL}3${NL}" "${NL}2${NL}" "${NL}-3${NL}" "${NL}-1${NL}"
check "trig"              "PRINT SIN (PI/2)${NL}PRINT COS 0${NL}PRINT INT (ATN 1*4*1000)${NL}PRINT TAN (PI/4)${NL}" "${NL}1${NL}" "${NL}1${NL}" "${NL}3141${NL}"
check "asn acs"           "PRINT ASN 1*2${NL}PRINT ACS 1${NL}" "${NL}3.1415927${NL}" "${NL}0${NL}"
check "ln exp"            "PRINT LN EXP 2${NL}PRINT EXP 1${NL}PRINT EXP 0${NL}" "${NL}2${NL}" "${NL}2.7182818${NL}" "${NL}1${NL}"
check "function binding"  "PRINT SQR 4*3${NL}PRINT SQR (4*4)${NL}" "${NL}6${NL}" "${NL}4${NL}"
check "invalid argument"  "PRINT SQR -1${NL}" "A Invalid argument, 0:"
check "ln of zero"        "PRINT LN 0${NL}" "A Invalid argument, 0:"
check "decimal for step"  "10 FOR I=0 TO 1 STEP 0.25${NL}20 PRINT I${NL}30 NEXT I${NL}RUN${NL}" "${NL}0${NL}0.25${NL}0.5${NL}0.75${NL}1${NL}"
check "integer range"     "INK 10${NL}" "K Invalid colour, 0:"
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
check "for no next"       "10 FOR I=5 TO 1${NL}RUN${NL}" "I FOR without NEXT, 10:"
check "string concat"     "A\$=\"AB\"${NL}B\$=A\$+\"CD\"+A\$${NL}PRINT B\$${NL}" "${NL}ABCDAB${NL}"
check "string compare"    "IF \"ABC\"<\"ABD\" THEN PRINT 1+1${NL}IF \"A\"=\"A\" THEN PRINT 2+2${NL}IF \"AB\">\"A\" THEN PRINT 3+3${NL}" "${NL}2${NL}" "${NL}4${NL}" "${NL}6${NL}"
check "type mismatch"     "PRINT \"A\"+1${NL}" "C Nonsense in BASIC, 0:"
check "type mismatch 2"   "A=\"X\"${NL}" "C Nonsense in BASIC, 0:"
check "variable not found" "PRINT Q+1${NL}" "2 Variable not found, 0:"
check "run clears vars"   "A=5${NL}10 PRINT A${NL}RUN${NL}" "2 Variable not found, 10:"
check "print ; at end"    "PRINT 1+1;: PRINT 3+3${NL}" "${NL}26${NL}"
check "list detokenizes"  "10 print a:goto 10${NL}20 if a<=b then print \"if\"${NL}LIST${NL}" "  10 PRINT a: GO TO 10${NL}  20>IF a<=b THEN PRINT \"if\"${NL}"
check "list rem keeps text" "10 rem Hello print${NL}LIST${NL}" "  10>REM Hello print${NL}"

# --- More statements ---------------------------------------------------------
check "go to with space"  "10 GO TO 30${NL}20 PRINT 1+1${NL}30 GO SUB 50: PRINT 1+2: STOP${NL}50 PRINT 4*4: RETURN${NL}RUN${NL}" "${NL}16${NL}3${NL}" "!${NL}2${NL}" "![ERR]"
check "go to missing line" "10 GO TO 15${NL}12 PRINT 1+1${NL}20 PRINT 1+2${NL}RUN${NL}" "${NL}3${NL}" "!${NL}2${NL}" "![ERR]"
check "go to past end"    "10 PRINT 1+2: GO TO 100${NL}20 PRINT 7*7${NL}RUN${NL}" "${NL}3${NL}" "!49" "![ERR]"
check "run from line"     "10 PRINT 1+1${NL}20 PRINT 1+2${NL}RUN 20${NL}" "${NL}3${NL}" "!${NL}2${NL}"
check "names with spaces" "LET hi score=5: PRINT HiScore+1${NL}" "${NL}6${NL}"
check "print apostrophe"  "PRINT 1'2;' '3${NL}" "${NL}1${NL}2${NL}${NL}3${NL}"
check "def fn"            "10 DEF FN f(x)=x*x+1${NL}20 DEF FN j\$(a\$,n)=a\$(n TO)+a\$( TO n-1)${NL}30 LET x=100: PRINT FN f(3);\" \";x;\" \";FN j\$(\"ABCD\",3);\" \";FN f(FN f(1))${NL}RUN${NL}" "${NL}10 100 CDAB 5${NL}" "![ERR]"
check "def fn no params"  "10 DEF FN p()=PI*2${NL}20 PRINT FN p()${NL}RUN${NL}LIST 10${NL}" "${NL}6.2831853${NL}" "10 DEF FN p()=PI*2${NL}"
check "fn without def"    "PRINT FN q(1)${NL}" "P FN without DEF"
check "fn bad parameters" "10 DEF FN f(x)=x${NL}20 PRINT FN f(1,2)${NL}RUN${NL}" "Q Parameter error, 20:"
check "data skips strings" "10 PRINT \"x\": DATA 5${NL}20 READ a: PRINT a${NL}RUN${NL}" "${NL}5${NL}" "![ERR]"
# --- The Spectrum's screen: reports, INPUT, scroll? ----------------------
check "report ok"         "10 PRINT 1: PRINT 2${NL}RUN${NL}" "0 OK, 10:2"
check "report statement"  "PRINT 1: PRINT 2: PRINT X${NL}" "2 Variable not found, 0:3"
check "report after then" "IF 1 THEN PRINT 1: PRINT X${NL}" "2 Variable not found, 0:3"
check "stop report"       "10 STOP${NL}RUN${NL}" "9 STOP statement, 10:1"
check "clear clears screen" "CLEAR${NL}" "[CLS]"
check "at off screen"     "PRINT AT 22,0;1${NL}" "5 Out of screen, 0:1"
check "input prompt"      "10 INPUT \"NAME? \";N\$: PRINT N\$${NL}RUN${NL}FRED${NL}" "NAME? \"FRED\"${NL}" "${NL}FRED${NL}" "![ERR]"
check "input string expr" "10 LET B\$=\"BB\": INPUT A\$: PRINT A\$${NL}RUN${NL}${DEL}${BS}${BS}\"A\"+B\$${NL}" "${NL}ABB${NL}" "![ERR]"
check "input line"        "10 INPUT LINE A\$: PRINT A\$${NL}RUN${NL}HI \"THERE\"${NL}" "${NL}HI \"THERE\"${NL}" "![ERR]"
check "input prompt value" "10 LET X=5: INPUT (\"X IS \";X;\" \");Y: PRINT Y${NL}RUN${NL}7${NL}" "X IS 5 7${NL}" "${NL}7${NL}"
check "input stop"        "10 INPUT A${NL}RUN${NL}STOP${NL}" "H STOP in INPUT, 10:1"
check "long input prompt" "10 INPUT \"${A200}\";A: PRINT A*2${NL}RUN${NL}21${NL}" "${NL}42${NL}" "![ERR]"
check "print to lower"    "PRINT #1;\"LOW\": PRINT \"UP\"${NL}" "LOW${NL}UP${NL}" "![ERR]"
check "bad stream"        "PRINT #5;1${NL}" "O Invalid stream, 0:1"
check "lprint ignored"    "LPRINT \"PAPER\": LLIST: COPY: PRINT 3${NL}" "${NL}3${NL}" "!PAPER${NL}" "![ERR]"
SCROLL_PROMPT=1 check "scroll prompt no"  "10 FOR I=1 TO 30: PRINT I: NEXT I${NL}RUN${NL}n" "scroll?" "D BREAK - CONT repeats, 10:2" "!${NL}25${NL}"
SCROLL_PROMPT=1 check "scroll prompt yes" "10 FOR I=1 TO 30: PRINT I: NEXT I${NL}RUN${NL}y" "scroll?" "${NL}30${NL}" "0 OK, 10:3"
# --- The Spectrum's memory: PEEK, POKE, UDGs, system variables ----------
check "peek poke ram"     "POKE 30000,77: POKE 30001,-1: PRINT PEEK 30000;\" \";PEEK 30001;\" \";PEEK 100${NL}" "${NL}77 255 0${NL}"
check "poke range"        "POKE 70000,1${NL}POKE 30000,256${NL}" "B Integer out of range, 0:1" "B Integer out of range, 0:1"
check "chars sysvar"      "PRINT PEEK 23606+256*PEEK 23607;\" \";PEEK 23675+256*PEEK 23676${NL}" "${NL}15360 65368${NL}"
check "rom font"          "PRINT PEEK 15616;PEEK (15616+33*8+1)>0${NL}" "${NL}01${NL}"
check "display memory"    "CLS: PLOT 0,175: PLOT 255,0: PRINT AT 5,0;PEEK 16384;\" \";PEEK 22463${NL}" "]128 1${NL}"
check "poke display"      "CLS: POKE 16384,255: PRINT AT 5,0;POINT (0,175);POINT (7,175);POINT (8,175)${NL}" "]110${NL}"
check "attributes memory" "PRINT ATTR (0,0): POKE 22529,79: PRINT ATTR (0,1);\" \";PEEK 22529${NL}" "${NL}56${NL}79 79${NL}"
check "udg"               "PRINT USR \"a\";\" \";USR \"U\"${NL}FOR i=0 TO 7: POKE USR \"b\"+i,BIN 11110000: NEXT i: CLS: PRINT \"\\b\": PRINT PEEK 16384${NL}" "${NL}65368 65528${NL}" "${NL}240${NL}"
check "udg starts as letter" "CLS: PRINT \"\\c\": PRINT SCREEN\$ (0,0)${NL}" "${NL}C${NL}"
check "bin"               "PRINT BIN 1010;\" \";BIN;\" \";BIN 11111111${NL}" "${NL}10 0 255${NL}"
check "val string"        "LET b\$=\"X\": PRINT VAL\$ \"b\$+\"\"Y\"\"\"${NL}" "${NL}XY${NL}"
check "screen string"     "CLS: PRINT \"Q\";: INVERSE 1: PRINT \"R\": INVERSE 0: PRINT SCREEN\$ (0,0);SCREEN\$ (0,1);\"[\";SCREEN\$ (0,2);\"]\"${NL}" "${NL}QR[ ]${NL}"
check "usr machine code"  "RANDOMIZE USR 32768${NL}" "A Machine code not supported, 0:1"
check "usr 0 resets"      "10 PRINT 1${NL}PRINT USR 0${NL}LIST${NL}" "x86 BASIC for PCs${NL}LIST${NL}0 OK"
check "seed sysvar"       "RANDOMIZE 300: PRINT PEEK 23670+256*PEEK 23671${NL}" "${NL}300${NL}"
check "frames"            "LET a=PEEK 23672: PAUSE 10: PRINT PEEK 23672<>a${NL}" "${NL}1${NL}"
check "coords"            "PLOT 10,20: PRINT PEEK 23677;\" \";PEEK 23678${NL}" "${NL}10 20${NL}"
check "keyword chars"     "PRINT CHR\$ 245;\"X\";CHR\$ 199${NL}" "${NL}PRINT X<=${NL}"
check "custom charset"    "CLS: POKE 23606,8: PRINT \"A\";: POKE 23606,0: PRINT SCREEN\$ (0,0)${NL}" "${NL}AB${NL}"
check "caps lock"         "10 POKE 23658,8: INPUT a\$: POKE 23658,0: PRINT a\$${NL}RUN${NL}abc${NL}" "${NL}ABC${NL}"
check "out border"        "OUT 254,2: PRINT IN 65278${NL}" "[BORDER 2]255${NL}"
check "quotes in strings"  "10 PRINT \"say \"\"hi\"\"\";LEN \"\"\"\"${NL}RUN${NL}LIST${NL}" "${NL}say \"hi\"1${NL}" "10>PRINT \"say \"\"hi\"\"\"; LEN \"\"\"\"${NL}"
check "graphics escapes"  "PRINT CODE \"\\a\";\" \";CODE \"\\u\";\" \";CODE \"\\::\";\" \";CODE \"\\  \";\" \";CODE \"\\'.\";\" \";CODE \"\\*\";\" \";CODE \"\\{65}\";\" \";CODE \"\\\\\";LEN \"\\x\"${NL}" "${NL}144 164 143 128 134 127 65 922${NL}"
check "pound sign"        "PRINT CODE \"\`\"${NL}" "${NL}96${NL}"
check "deep nesting"      "10 DEF FN r(n)=FN r(n)${NL}20 PRINT FN r(1)${NL}RUN${NL}PRINT ((((((((((2))))))))))${NL}" "4 Out of memory, 20:1" "${NL}2${NL}"
check "gosub return"      "10 GOSUB 100: PRINT \"BACK\"${NL}20 END${NL}100 PRINT \"SUB\"${NL}110 RETURN${NL}RUN${NL}" "${NL}SUB${NL}BACK${NL}"
check "nested gosub"      "10 GOSUB 100${NL}20 PRINT 3*3: END${NL}100 GOSUB 200: PRINT 2*2: RETURN${NL}200 PRINT 1*1: RETURN${NL}RUN${NL}" "${NL}1${NL}4${NL}9${NL}"
check "return no gosub"   "RETURN${NL}" "7 RETURN without GO SUB, 0:"
check "gosub too deep"    "10 GOSUB 10${NL}RUN${NL}" "4 Out of memory, 10:"
check "input number"      "10 INPUT \"N? \";N${NL}20 PRINT N*2${NL}RUN${NL}21${NL}" "N? 21${NL}42${NL}"
check "input expression"  "10 INPUT N${NL}20 PRINT N${NL}RUN${NL}6*7${NL}" "${NL}42${NL}"
check "input retry"       "10 INPUT N${NL}20 PRINT N+1${NL}RUN${NL}ABC${NL}${BS}${BS}${BS}41${NL}" "${NL}42${NL}"
check "input string"      "10 INPUT \"NAME\";A\$${NL}20 PRINT \"HI \";A\$${NL}RUN${NL}Bob Smith${NL}" "HI Bob Smith${NL}"
check "input two vars"    "10 INPUT A,B${NL}20 PRINT A*B${NL}RUN${NL}6${NL}7${NL}" "${NL}42${NL}"
check "input break"       "10 INPUT A${NL}RUN${NL}XXXX${ESC}" "H STOP in INPUT, 10:"
check "stop continue"     "10 PRINT 1+1: STOP: PRINT 3+3${NL}20 PRINT 5+5${NL}RUN${NL}CONTINUE${NL}" "9 STOP statement, 10:" "${NL}6${NL}10${NL}"
check "continue after error" "10 PRINT 100/D${NL}RUN${NL}D=4${NL}CONTINUE${NL}" "2 Variable not found, 10:" "${NL}25${NL}"
check "continue after break" "10 GOTO 10${NL}RUN${NL}${ESC}CONTINUE${NL}${ESC}" "CONTINUE${NL}[ERR]L BREAK into program, 10:"
check "can't continue"    "CONTINUE${NL}" "N Statement lost, 0:"
check "and or not"        "PRINT 1 AND 1;0 AND 1;1 OR 0;0 OR 0;NOT 0;NOT 5${NL}" "${NL}101010${NL}"
check "logic precedence"  "A=1: B=2${NL}IF A=1 AND B=2 THEN PRINT 6*7${NL}IF NOT A=2 THEN PRINT 6*8${NL}IF A=2 OR B=2 THEN PRINT 6*9${NL}" "${NL}42${NL}" "${NL}48${NL}" "${NL}54${NL}"
check "string and"        "PRINT \"YES\" AND 1;\"NO\" AND 0;\"!\"${NL}" "${NL}YES!${NL}"
check "len code chr"      "PRINT LEN \"HELLO\";\" \";CODE \"A\";\" \";CHR\$ 66;CHR\$ (67)${NL}" "${NL}5 65 BC${NL}"
check "str val"           "A\$=STR\$ 3.5${NL}PRINT A\$+\"!\"${NL}PRINT VAL \"2*21\"${NL}PRINT VAL \"1E3\"+1${NL}" "${NL}3.5!${NL}" "${NL}42${NL}" "${NL}1001${NL}"
check "val bad"           "PRINT VAL \"1+\"${NL}" "C Nonsense in BASIC, 0:"
check "slicing"           "A\$=\"HELLO\"${NL}PRINT A\$(2 TO 4);\"/\";A\$(2);\"/\";A\$(TO 2);\"/\";A\$(4 TO)${NL}" "${NL}ELL/E/HE/LO${NL}"
check "slice literal"     "PRINT \"ABCDEF\"(3 TO 4)${NL}" "${NL}CD${NL}"
check "slice empty"       "A\$=\"AB\"${NL}PRINT \"[\";A\$(2 TO 1);\"]\"${NL}" "${NL}[]${NL}"
check "slice range"       "A\$=\"AB\"${NL}PRINT A\$(1 TO 3)${NL}" "3 Subscript wrong, 0:"
check "self slice"        "A\$=\"HELLO\"${NL}A\$=A\$(2 TO)${NL}PRINT A\$${NL}" "${NL}ELLO${NL}"
check "dim numbers"       "DIM A(3): A(1)=5: A(3)=A(1)*2${NL}PRINT A(1);A(2);A(3)${NL}" "${NL}5010${NL}"
check "dim 2d"            "DIM B(2,3): B(2,3)=7: PRINT B(2,3);B(1,1)${NL}" "${NL}70${NL}"
check "dim strings"       "DIM N\$(2): N\$(1)=\"AL\": N\$(2)=\"BO\": PRINT N\$(2)+N\$(1)${NL}" "${NL}BA${NL}"
check "string array"      "DIM B\$(3,4): B\$(2)=\"AB\": PRINT \"[\";B\$(2);\"]\";LEN B\$(2);B\$(2,1);B\$(2,2 TO 3);\"]\"${NL}" "${NL}[AB  ]4AB ]${NL}"
check "fixed length"      "A\$=\"X\": DIM A\$(3): PRINT \"[\";A\$;\"]\": LET A\$=\"TOOLONG\": PRINT A\$;LEN A\$${NL}" "${NL}[   ]${NL}TOO3${NL}"
check "slice assignment"  "A\$=\"HELLO\": A\$(2 TO 3)=\"XYZ\": PRINT A\$: A\$(1)=\"JJ\": A\$(5 TO)=\"\": PRINT A\$;\"]\"${NL}" "${NL}HXYLO${NL}JXYL ]${NL}"
check "slice bad"         "A\$=\"AB\": A\$(3)=\"X\"${NL}" "3 Subscript wrong"
check "heap compaction"   "5 B\$=\"\"${NL}10 FOR I=1 TO 60: DIM Z(3000): B\$=B\$+\"XY\": C\$=B\$: NEXT I: PRINT LEN C\$${NL}RUN${NL}" "${NL}120${NL}" "![ERR]"
check "for in subroutine" "10 FOR I=1 TO 50: GO SUB 100: NEXT I: PRINT \"OK\";I: STOP${NL}100 FOR J=1 TO 5: IF J=2 THEN RETURN${NL}110 NEXT J${NL}RUN${NL}" "${NL}OK51${NL}"
check "next plain var"    "LET X=1: NEXT X${NL}" "1 NEXT without FOR"
check "continue after edit" "10 PRINT 1: STOP: PRINT 2${NL}RUN${NL}20 PRINT 3${NL}CONTINUE${NL}" "${NL}1${NL}" "${NL}2${NL}3${NL}"
check "array and var"     "DIM A(2): A=9: A(1)=3: PRINT A+A(1)${NL}" "${NL}12${NL}"
check "subscript range"   "DIM A(3): PRINT A(4)${NL}" "3 Subscript wrong, 0:"
check "subscript count"   "DIM A(3): PRINT A(1,1)${NL}" "3 Subscript wrong, 0:"
check "no array"          "PRINT Z(1)${NL}" "2 Variable not found, 0:"
check "dim too big"       "DIM A(65535,65535)${NL}" "4 Out of memory, 0:"
check "for over array"    "DIM S(5): FOR I=1 TO 5: S(I)=I*I: NEXT I: PRINT S(5)${NL}" "${NL}25${NL}"
check "read data"         "10 READ A,B\$,C${NL}20 PRINT A;B\$;C${NL}30 DATA 1,\"TWO\"${NL}40 DATA 2+1${NL}RUN${NL}" "${NL}1TWO3${NL}"
check "out of data"       "10 READ A,B${NL}20 DATA 1${NL}RUN${NL}" "E Out of DATA, 10:"
check "restore"           "10 READ A: RESTORE: READ B${NL}20 PRINT A+B${NL}30 DATA 21${NL}RUN${NL}" "${NL}42${NL}"
check "restore line"      "10 RESTORE 40: READ A: PRINT A${NL}30 DATA 1${NL}40 DATA 2${NL}RUN${NL}" "${NL}2${NL}"
check "data skipped"      "10 DATA 1,\"A:B\",3: PRINT 4*4${NL}RUN${NL}" "${NL}16${NL}"
check "rnd"               "RANDOMIZE 1: A=RND: RANDOMIZE 1: B=RND${NL}PRINT A=B;A>=0;A<1${NL}" "${NL}111${NL}"
check "rnd from power on" "PRINT RND${NL}RANDOMIZE 1: PRINT RND${NL}" "${NL}.0011291504${NL}" "${NL}.0022735596${NL}"
check "rnd varies"        "RANDOMIZE 5: PRINT RND=RND${NL}" "${NL}0${NL}"
check "inkey"             "10 K\$=INKEY\$: IF K\$=\"\" THEN GOTO 10${NL}20 PRINT \"KEY \";K\$${NL}RUN${NL}X" "KEY X${NL}"
check "pause"             "PAUSE 5: PRINT 6*7${NL}" "${NL}42${NL}"
check "print at"          "PRINT AT 3,5;\"X\"${NL}" "[AT 3,5]X"
check "print at range"    "PRINT AT 30,0;\"X\"${NL}PRINT AT 0,32;\"X\"${NL}" "5 Out of screen, 0:" "B Integer out of range, 0:"
check "print tab"         "PRINT \"AB\";TAB 6;\"C\"${NL}" "${NL}AB    C${NL}"
check "print comma"       "PRINT 1,2${NL}" "${NL}1               2${NL}"
check "list from"         "10 PRINT 1${NL}20 PRINT 2${NL}30 PRINT 3${NL}LIST 20${NL}" "LIST 20${NL}  20>PRINT 2${NL}  30 PRINT 3${NL}"
check "beep range"        "BEEP 1,100${NL}" "B Integer out of range, 0:"

# --- Graphics ----------------------------------------------------------------
check "plot point"        "PLOT 10,10: PRINT POINT (10,10);POINT (11,10)${NL}" "${NL}10${NL}"
check "plot range"        "PLOT 256,0${NL}" "B Integer out of range, 0:"
check "plot y range"      "PLOT 0,176${NL}" "B Integer out of range, 0:"
check "draw line"         "PLOT 0,0: DRAW 100,50: PRINT POINT (50,25);POINT (100,50);POINT (50,30)${NL}" "${NL}110${NL}"
check "draw relative"     "PLOT 10,10: DRAW 10,0: DRAW 0,10: PRINT POINT (20,15);POINT (15,10)${NL}" "${NL}11${NL}"
check "draw off screen"   "PLOT 250,0: DRAW 10,0${NL}" "B Integer out of range, 0:"
check "draw arc"          "PLOT 100,100: DRAW 50,0,PI: PRINT POINT (150,100);POINT (125,75);POINT (125,125)${NL}" "${NL}110${NL}"
check "circle"            "CIRCLE 100,80,10: PRINT POINT (110,80);POINT (100,90);POINT (100,80)${NL}" "${NL}110${NL}"
check "circle clipped"    "CIRCLE 0,0,50: PRINT POINT (50,0)${NL}" "${NL}1${NL}"
check "over"              "PLOT 5,5: OVER 1: PLOT 5,5: PRINT POINT (5,5)${NL}" "${NL}0${NL}"
check "inverse plot"      "PLOT 5,5: INVERSE 1: PLOT 5,5: INVERSE 0: PRINT POINT (5,5)${NL}" "${NL}0${NL}"
check "cls clears"        "PLOT 5,5: CLS: PRINT POINT (5,5)${NL}" "[CLS]${NL}0${NL}"
check "attributes"        "BRIGHT 1: INVERSE 1: OVER 1: FLASH 1: INK 9: PAPER 8: BRIGHT 8${NL}" "0 OK, 0:7"
check "attribute range"   "BRIGHT 2${NL}INVERSE 8${NL}" "K Invalid colour, 0:1" "K Invalid colour, 0:1"
check "print colours"     "PRINT INK 2;\"R\";PAPER 6;\"Y\"${NL}PRINT \"X\"${NL}" "${NL}RY${NL}0 OK, 0:1${NL}PRINT \"X\"${NL}X${NL}"
check "block graphics"    "PRINT CHR\$ 128;CHR\$ 143;CHR\$ 127${NL}" "![ERR]"
check "scrolling"         "FOR I=1 TO 40: PRINT I: NEXT I${NL}PRINT 6*7${NL}" "${NL}39${NL}40${NL}"

# --- Line editor ---------------------------------------------------------------
check "edit insert"       "PRINT 12${L}*${NL}" "PRINT 1*2${NL}2${NL}"
check "edit backspace"    "PRINT 99${BS}8${NL}" "PRINT 98${NL}98${NL}"
check "edit home"         "RINT 5${HOME}P${NL}" "PRINT 5${NL}5${NL}"
check "edit delete"       "PRINT 1X2${L}${L}${DEL}${NL}" "PRINT 12${NL}12${NL}"
check "edit end"          "PRINT 1${HOME}${END}+1${NL}" "PRINT 1+1${NL}2${NL}"
check "edit right"        "PRINT 15${HOME}${R}${R}${R}${R}${R}${R}${R}0${NL}" "PRINT 105${NL}105${NL}"
check "history up"        "PRINT 6*7${NL}${UP}${NL}" "42${NL}0 OK, 0:1${NL}PRINT 6*7${NL}42${NL}"
check "history twice"     "PRINT 1${NL}PRINT 2${NL}${UP}${UP}${NL}" "PRINT 2${NL}2${NL}0 OK, 0:1${NL}PRINT 1${NL}1${NL}"
check "history down"      "PRINT 1${NL}PRINT 3${UP}${DN}${NL}" "PRINT 3${NL}3${NL}"
check "edit line"         "10 PRINT 5${NL}EDIT 10${NL}${BS}6${NL}LIST${NL}" "LIST${NL}  10>PRINT 6${NL}"
check "edit last line"    "10 PRINT 5${NL}20 PRINT 7${NL}EDIT${NL}${HOME}${R}${DEL}5${NL}LIST${NL}" "LIST${NL}  10 PRINT 5${NL}  20 PRINT 7${NL}  25>PRINT 7${NL}" "![ERR]"
check "edit missing"      "EDIT 50${NL}" "N Statement lost, 0:"
check "esc forgets line"  "PRINT 5*5${ESC}PRINT 6*6${NL}" "!25" "${NL}36${NL}"
check "input not history" "10 INPUT A\$${NL}RUN${NL}SECRET${NL}${UP}${NL}" "\"SECRET\"${NL}0 OK, 10:1${NL}RUN${NL}"

# --- Saving programs -----------------------------------------------------------
check "save load"         "10 PRINT 6*7${NL}SAVE \"answer\"${NL}NEW${NL}LOAD \"ANSWER\"${NL}LIST${NL}RUN${NL}" "LIST${NL}  10 PRINT 6*7${NL}" "${NL}42${NL}"
check "cat"               "10 PRINT 1${NL}SAVE \"ONE\"${NL}20 PRINT 2${NL}SAVE \"TWO\" LINE 20${NL}CAT${NL}" "ONE             6${NL}TWO             12 LINE 20${NL}13 FREE${NL}"
check "save replaces"     "10 PRINT 1${NL}SAVE \"P\"${NL}10 PRINT 2${NL}SAVE \"P\"${NL}NEW${NL}LOAD \"P\"${NL}LIST${NL}CAT${NL}" "LIST${NL}  10 PRINT 2${NL}" "14 FREE"
check "load autorun"      "10 PRINT 6*7${NL}SAVE \"GO\" LINE 10${NL}NEW${NL}LOAD \"GO\"${NL}" "LOAD \"GO\"${NL}42${NL}"
check "load missing"      "LOAD \"NOPE\"${NL}" "R Program not found, 0:"
check "erase"             "10 PRINT 1${NL}SAVE \"X\"${NL}ERASE \"X\"${NL}LOAD \"X\"${NL}" "R Program not found, 0:"
check "bad name"          "SAVE \"\"${NL}SAVE \"ABCDEFGHIJKLMNOP\"${NL}SAVE 5${NL}" "F Invalid file name, 0:" "C Nonsense in BASIC, 0:"
check "save empty"        "SAVE \"E\"${NL}" "F Invalid file name, 0:"
check "load clears vars"  "10 PRINT 1${NL}SAVE \"V\"${NL}A=5${NL}LOAD \"V\"${NL}PRINT A${NL}" "2 Variable not found, 0:"
FULL15=""; for i in $(seq 1 15); do FULL15+="SAVE \"P$i\"${NL}"; done
check "disk full"         "10 PRINT 1${NL}${FULL15}SAVE \"P16\"${NL}CAT${NL}" "R Disk full, 0:" "0 FREE"
BIG=""; for i in $(seq 1 150); do BIG+="$i REM ${A200}${NL}"; done
check "big program"       "${BIG}SAVE \"BIG\"${NL}NEW${NL}LOAD \"BIG\"${NL}LIST 97${NL}" "LIST 97${NL}  97>REM" "![ERR]"
check "save in program"   "10 SAVE \"SELF\"${NL}20 PRINT 6*7${NL}RUN${NL}CAT${NL}" "${NL}42${NL}" "SELF "

# --- BREAK --------------------------------------------------------------------
check "break stops loop"  "10 GOTO 10${NL}RUN${NL}${ESC}PRINT 5*5${NL}" "L BREAK into program, 10:" "${NL}25${NL}"

echo "$pass passed, $fail failed"
((fail == 0))
