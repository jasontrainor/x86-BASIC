# x86-BASIC

A ZX Spectrum-style BASIC computer for PCs. Boot it from a USB stick and
the PC starts straight into BASIC, like a home computer from the 1980s.
There's no operating system underneath: just a boot loader, a small
kernel and the interpreter.

The aim is that a Spectrum BASIC listing — from a magazine, a book or the
Spectrum manual — can be typed in and gives the same result as on a real
Spectrum. It isn't an emulator: it doesn't run Z80 machine code, tapes or
snapshots, and it contains nothing of the Spectrum's ROM. The BASIC is a
new implementation, and the character set was drawn for this project.

- The Spectrum's screen: 32×24 characters on a 256×192 picture inside a
  coloured border, eight colours with BRIGHT and FLASH, an upper screen
  for programs and a lower screen for typing, INPUT and reports
- Spectrum BASIC: its numbers, strings and slicing, arrays, DEF FN,
  READ/DATA, graphics, BEEP, PEEK and POKE, user-defined graphics, error
  reports — and it runs at about the Spectrum's speed
- A line editor with cursor keys and history, and SAVE/LOAD to the boot
  disk
- One disk image that boots on both old (BIOS) and new (UEFI) PCs, from
  a USB stick, hard disk or floppy

## Build and run

You need `nasm`, `gcc`, GNU `ld` and `make` (the GNU toolchain of any
Linux distribution), `python3`, and `qemu-system-x86_64` to try it out.
For UEFI, also the OVMF firmware (the `ovmf` package).

    make              # builds os.img
    ./run.sh          # boot it in QEMU as a floppy disk (BIOS)
    ./run.sh usb      # boot it as a USB stick (BIOS)
    ./run.sh uefi     # boot it as a USB stick on a UEFI PC

On Debian or Ubuntu: `sudo apt install build-essential nasm python3
qemu-system-x86 ovmf`.

## Using it

As on a Spectrum, lines are typed at the bottom of the screen (the lower
screen). A line with a number goes into the program, which is then listed
above, with `>` marking the line just entered. A line without a number
runs straight away. Either way, when it's done a report appears at the
bottom — `0 OK, 0:1`, or `2 Variable not found, 30:2` (line 30,
statement 2) — and stays until a key is pressed. A number on its own
deletes that line, and Enter on its own lists the program again.

Each line is checked when Enter is pressed, as on the Spectrum. If it
doesn't make sense, it isn't stored or run: it stays in the editor with
a flashing `?` where it goes wrong, to be corrected (or Esc to abandon
it). The check is for the form of the line: whether variables exist,
numbers are in range and so on is only known when it runs.

Keywords are typed in full, in any case: `print`, `GO TO`, `goto`. Press
**Esc** (or **Ctrl+C**) for BREAK; `CONTINUE` carries on.

### Editing

| Key | |
|---|---|
| ← → Home End | move along the line |
| Backspace, Delete | delete before / at the cursor |
| ↑ ↓ | recall earlier lines |
| Enter | done; Esc abandons the line |

`EDIT 20` brings line 20 back to change it (`EDIT` alone: the last line
entered). `LIST` shows keywords in colour. Lines can be up to 760
characters long; the lower screen grows upwards to hold them.

### Typing Spectrum characters

The Spectrum's keyboard could type characters a PC's can't. In strings
(and `REM` text), they are written as escapes, the same ones the `zmakebas` tool uses for
Spectrum listings:

| | |
|---|---|
| `"\a"` … `"\u"` | the user-defined graphics A–U (`CHR$ 144`–`164`) |
| `"\::"`, `"\'."`, `"\  "` … | block graphics (`CHR$ 128`–`143`): two characters for the left and right halves of the cell, each `'` (top), `.` (bottom), `:` (both) or a space |
| `"\*"` | © (`CHR$ 127`) |
| `"\{n}"` | character *n* |
| `"\\"` | a backslash |
| `` ` `` | £ (`CHR$ 96`, as on the Spectrum) |

A quote inside a string is written `""`, as on the Spectrum.

### The language

It's the Spectrum's BASIC; a quick summary:

| | |
|---|---|
| `PRINT`, `LPRINT` | items separated by `;` (together), `,` (next half-line) or `'` (new line); `AT row,col`, `TAB col`; colours (`PRINT INK 2;"RED"`) last for that `PRINT`. `PRINT #1;` prints in the lower screen. `LPRINT`, `LLIST` and `COPY` do nothing (no printer). |
| `LET` | numbers and strings; numeric names can be long and contain spaces (`LET hi score=0`) |
| `DIM a(10,10)`, `DIM a$(5,10)` | arrays from 1; a string array holds fixed-length strings, as on the Spectrum: `DIM a$(10)` is one string of 10 characters |
| `a$(2 TO 4)`, `LET a$(3)="X"` | slicing, and assigning to a slice (padded or cut to fit) |
| `INPUT` | in the lower screen, with prompts, `AT`, colours and `LINE`; numbers can be typed as expressions, strings between quotes |
| `IF ... THEN`, `FOR ... TO ... STEP`, `NEXT`, `GO TO`, `GO SUB`, `RETURN`, `STOP`, `CONTINUE` | as on the Spectrum (`GO TO` a missing line goes to the next one) |
| `READ`, `DATA`, `RESTORE`, `DEF FN`, `FN`, `RANDOMIZE`, `PAUSE`, `BEEP`, `REM` | |
| `CLS`, `INK`, `PAPER`, `BORDER`, `BRIGHT`, `FLASH`, `INVERSE`, `OVER` | colours 0–7, 8 (leave as it is) and 9 (contrast) |
| `PLOT`, `DRAW x,y[,angle]`, `CIRCLE`, `POINT` | x 0–255, y 0–175 from the bottom |
| `PEEK`, `POKE`, `USR "a"`, `IN`, `OUT` | see below |
| `RUN`, `LIST`, `NEW`, `CLEAR`, `SAVE`, `LOAD`, `VERIFY`, `MERGE` | `RUN` and `CLEAR` also clear the screen |

Functions: `ABS INT SGN SQR SIN COS TAN ATN ASN ACS LN EXP PI RND`, `LEN
VAL VAL$ CODE CHR$ STR$ BIN INKEY$ ATTR SCREEN$ POINT PEEK IN USR`. As on
the Spectrum, a function applies to the next item (`SQR 4*3` is 6), `^`
is power, comparisons give 1 or 0, and `a$ AND x` is `a$` if `x` is
true, otherwise `""`.

Numbers are printed as the Spectrum prints them (8 significant digits,
`1.2345679E+8`, `0.5` but `.05`), and `RND` uses the Spectrum's formula:
from power-on it gives the same sequence a Spectrum would.

Beyond the Spectrum: `END`, `EDIT`, `CAT`, `ERASE`, `IF ... THEN 100`,
long string variable names, and strings of up to 65535 characters.

### Reports

The Spectrum's reports and codes: `0 OK`, `1 NEXT without FOR`, `2
Variable not found`, `3 Subscript wrong`, `4 Out of memory`, `5 Out of
screen`, `6 Number too big`, `7 RETURN without GO SUB`, `9 STOP
statement`, `A Invalid argument`, `B Integer out of range`, `C Nonsense
in BASIC`, `D BREAK - CONT repeats`, `E Out of DATA`, `F Invalid file
name`, `G No room for line`, `H STOP in INPUT`, `I FOR without NEXT`, `K
Invalid colour`, `L BREAK into program`, `N Statement lost`, `O Invalid
stream`, `P FN without DEF`, `Q Parameter error`, and `R` for problems
with the disk.

When the upper screen fills, it asks `scroll?` (N, Space or Esc stop),
as the Spectrum does; `POKE 23692,255` holds that off for 255 lines.

### Memory, PEEK and POKE

`PEEK` and `POKE` see a 64KB memory laid out as the Spectrum's:

- the screen's pixels at 16384–22527 and its colours at 22528–23295, in
  the Spectrum's order: they *are* the screen
- the system variables programs use, at their Spectrum addresses: `CHARS`
  (23606, a character set of your own), `UDG` (23675), `SEED`, `FRAMES`
  (23672, counts 50 a second), `COORDS`, `SCR_CT`, `ATTR_P`, `LAST_K`,
  `FLAGS2` (`POKE 23658,8` for capitals) and others
- the character set at 15616–16383, where the Spectrum's ROM keeps one
  (this project's own font); the rest of the "ROM" reads 0
- the user-defined graphics at 65368 (`USR "a"`), starting as copies of
  the letters A–U
- ordinary memory for the rest

BASIC keeps its program and variables elsewhere, so PEEKing the program
area doesn't find them. `USR` with a number can't run machine code (`USR
0` starts afresh, as it does on a Spectrum). `IN` reads the keyboard
ports as the Spectrum's do, and `OUT 254,n` sets the border.

### Speed

A PC runs BASIC thousands of times faster than a Spectrum, which would
make delay loops and games far too fast, so programs are held back to
about the Spectrum's speed: a `FOR` loop of 1000 takes about 4.4 seconds,
as it does on a Spectrum. `POKE 23681,1` (a system variable the Spectrum
doesn't use) runs at full speed; `POKE 23681,0` goes back.

### Differences from a Spectrum

- Numbers have the Spectrum's precision (32 bits) and range (up to about
  1.7E38, and down to about 2.9E-39, below which they're 0), so rounding
  drift and overflow happen where they would there. But the arithmetic
  and functions are the PC's, rounded to that precision, not the
  Spectrum ROM's own routines, so a last digit can still occasionally
  differ.
- A keyword can't be used as a variable name (`LET to=1`, `LET line=1`),
  since keywords are typed as words.
- No machine code: `USR` addresses, `RANDOMIZE USR`, and `LOAD "" CODE`
  followed by a call don't run anything.
- A PC keyboard only reports a key going down, not that it's still held,
  so `IN` counts a key as held for a moment after each press (or repeat),
  and `INKEY$` returns each press (and repeat) once.
- No printer, microdrives or other Interface 1 commands; `OPEN #`,
  `CLOSE #`, `FORMAT`, `MOVE` and `SAVE ... DATA` aren't there.
- Speed is matched on average, statement by statement, not exactly.

## Saving programs

The boot disk stands in for a tape: `SAVE "name"` writes the program to
the disk the computer booted from and `LOAD "name"` reads it back (`LOAD
""` loads the first program). `SAVE "name" LINE 10` makes `LOAD` run it
from line 10. `SAVE "name" CODE start,length` and `SAVE "name" SCREEN$`
save memory or the screen, loaded back with `LOAD "name" CODE [start]`
and `LOAD "name" SCREEN$`. `VERIFY` checks a save, `MERGE` adds a saved
program's lines to the one in memory, `CAT` lists what's saved, and how much
room is left, and `ERASE "name"` deletes one. There's room for 127
files, about 890KB in all, with names of up to 15 characters.

Files are kept in a reserved area of the disk image, so they persist on a
USB stick, and the BIOS and UEFI versions share them. Rebuilding
`os.img` with `make` starts with an empty save area.

### Listings as text files

A name ending `.BAS` or `.TXT` is a text file on the stick's FAT
partition, the one any PC shows when the stick is plugged in. So a
listing can be typed or fixed up in a PC's text editor, copied onto the
stick, and loaded: `LOAD "game.bas"` (or `MERGE`). `SAVE "game.bas"`
writes the program out as text, to take back to a PC, and `VERIFY`,
`ERASE` and `CAT` work too. Names are DOS 8.3 names, in the partition's
top folder.

The text is as `LIST` shows it, a line per line, with the characters a
PC can't show written as the escapes above (zmakebas's). Reading one,
blank lines and lines starting with `#` are skipped, and a `\` at the
end of a line joins the next one on. Each line is checked as if typed;
the first that doesn't make sense is reported and put in the editor,
with its `?`, to be put right.

`make LISTINGS="game.bas other.bas"` puts listings on the image when it's
built.

## Booting a real PC from a USB stick

**Write the image (Linux):**

1. Plug the stick in and find its device name with `lsblk`, e.g.
   `/dev/sdb`. Check the size and model carefully: writing to the wrong
   device destroys its contents.
2. Write the image to the whole device (not a partition such as
   `/dev/sdb1`). Everything else on the stick is erased.

       sudo dd if=os.img of=/dev/sdX bs=4M conv=fsync status=progress

On Windows, a raw image writer such as Rufus (in "DD image" mode) or
balenaEtcher does the same job.

**Boot:** open the firmware's boot menu (commonly F12, F11, F8 or Esc at
power-on) and pick the USB stick.

- **UEFI PCs** (most made since about 2012, and all recent ones) start
  the 64-bit UEFI version. **Secure Boot must be turned off** in the
  firmware setup, as the kernel isn't signed.
- **Older PCs**, or UEFI PCs with legacy/"CSM" boot enabled, start the
  16-bit BIOS version. The boot sector works whether the firmware treats
  the stick as a hard disk ("USB-HDD") or a floppy ("USB-FDD").

The two versions look and behave the same.

**Notes on modern hardware:**

- USB keyboards work through the firmware (UEFI, or the BIOS's legacy
  USB keyboard support).
- `BEEP` drives the PC speaker. Many modern PCs have none, in which case
  it's silent.
- The display mode is the firmware's: on UEFI the screen is scaled up
  by a whole number to fit, with the border colour filling the rest.

## Tests

    make test         # interpreter tests on the host, with ASan/UBSan
    make boot-test    # boot os.img in QEMU and type a BASIC session

`make test` runs about 280 tests of the interpreter, including a program
written the way Spectrum listings are (`tests/listings/demo.bas`).
`make boot-test` boots the image five ways: BIOS floppy, BIOS hard disk,
BIOS USB stick, UEFI USB stick and UEFI hard disk. It reads the text back
from screenshots. It uses KVM when available, because QEMU's emulator
doesn't enforce some CPU rules that real PCs do. The same tests run on
GitHub Actions for every push (`.github/workflows/ci.yml`).

`tests/qemu_try.py 'PRINT 1+1\n'` boots the image, types something and
prints what's on the screen, for trying things out.

## How it works

    boot/boot.asm          boot sector: loads the 16-bit kernel; also the
                           partition table and floppy BPB
    kernel/kernel_entry.asm  16-bit start-up: moves the code to its segment
                           and sets up "unreal mode" so the real-mode
                           kernel can use memory above 64KB
    kernel/bios.c          BIOS platform: VGA mode 13h, keyboard, timer,
                           speaker, disk (INT 13h)
    kernel/uefi/uefi.c     UEFI platform: graphics output, keyboard,
                           timer, Block I/O
    kernel/screen.c        the Spectrum's screen (upper and lower), the
                           cursor and keys
    kernel/editor.c        the line editor
    kernel/basic.c         the interpreter (tokenizer, expressions,
                           statements, reports, speed)
    kernel/memory.c        the Spectrum memory map for PEEK and POKE
    kernel/num.c           numbers: x87 maths, Spectrum-style printing
    kernel/storage.c       SAVE/LOAD files on the disk
    kernel/fat.c           text files on the FAT partition
    kernel/font.c          the character set (generated by tools/mkfont.py)
    tools/mkimage.py       builds os.img, including its FAT EFI partition

Everything above the platform layer (`kernel/platform.h`) is shared by
the BIOS and UEFI kernels and the host tests.

The BIOS kernel runs in 16-bit real mode. Its code has a 64KB segment
of its own (at 0x80000, so the PC needs 576KB of memory); its small data
and stack fit in the first 64KB, and larger data lives above that (see
`linker.ld`). It's built with `-Os`.
