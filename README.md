# x86-BASIC

A ZX Spectrum-style BASIC computer for PCs. Boot it from a USB stick and
the PC starts straight into BASIC, like a home computer from the 1980s.
There's no operating system underneath: just a boot loader, a small
kernel and the interpreter.

- A Spectrum-like screen: 32×24 characters on a 256×192 picture inside a
  coloured border, eight colours with BRIGHT, and a character set
  drawn for this project
- Spectrum-flavoured BASIC with decimal numbers, strings, arrays,
  graphics (`PLOT`, `DRAW`, `CIRCLE`) and sound (`BEEP`)
- A line editor with history, and `SAVE`/`LOAD` to the boot disk
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

## Using BASIC

Type a line with a number to add it to the program (or replace that
line), or without one to run it straight away. A number on its own
deletes that line. Keywords can be typed in any case.

Press **Esc** (or **Ctrl+C**) to stop a running program; `CONTINUE`
carries on from where it stopped.

### Editing

| Key | |
|---|---|
| ← → Home End | move along the line |
| Backspace, Delete | delete before / at the cursor |
| ↑ ↓ | recall earlier lines |
| Enter | done; Esc abandons the line |

`EDIT 20` brings line 20 back to change it (`EDIT` alone: the last line
you entered). `LIST` shows keywords in colour.

### The language

| | |
|---|---|
| `RUN [line]`, `LIST [line]`, `NEW`, `CLEAR` | run, list or erase the program; `CLEAR` erases variables |
| `PRINT a; b, c` | `;` prints items together, `,` moves to the next half of the line, a trailing `;` stays on the line |
| `PRINT AT row,col; ...`, `TAB col` | position the text (row 0-23, column 0-31) |
| `PRINT INK 2; "RED"` | colours inside `PRINT` last for that `PRINT` only |
| `LET X = 2` (`LET` optional) | numeric variables: any name of letters and digits (`SCORE`, `X2`) |
| `LET A$ = "TEXT"` | string variables, up to 255 characters |
| `DIM A(10)`, `DIM B(3,4)`, `DIM N$(5)` | arrays of up to 3 dimensions, subscripts from 1 |
| `INPUT "Name? "; N$` | read numbers or strings from the keyboard |
| `IF ... THEN statements`, `IF ... THEN 100` | conditional; the rest of the line runs only if true |
| `FOR I = 1 TO 10 STEP 2` ... `NEXT I` | loops (a loop that wouldn't run at all is skipped) |
| `GOTO n`, `GOSUB n`, `RETURN` | jumps and subroutines |
| `READ`, `DATA`, `RESTORE [line]` | data in the program |
| `STOP`, `CONTINUE`, `END` | stop, resume, end |
| `REM ...` | comment |
| `:` | separates statements on one line |
| `RANDOMIZE [n]`, `PAUSE n` | seed `RND`; wait n/50 seconds (0: until a key) |
| `CLS`, `INK n`, `PAPER n`, `BORDER n` | colours 0-7: black, blue, red, magenta, green, cyan, yellow, white |
| `BRIGHT 0/1`, `INVERSE 0/1`, `OVER 0/1` | bright colours, swap ink and paper, draw by flipping pixels |
| `PLOT x,y`, `DRAW dx,dy[,angle]`, `CIRCLE x,y,r` | graphics: x 0-255, y 0-175 from the bottom; `DRAW` with an angle draws an arc |
| `BEEP [seconds, semitones]` | a note: pitch in semitones above middle C |
| `SAVE "name" [LINE n]`, `LOAD "name"`, `CAT`, `ERASE "name"` | programs on the boot disk (see below) |

**Expressions:** `+ - * / ^`, comparisons `= <> < <= > >=` (giving 1 or
0), `AND`, `OR`, `NOT`, and brackets. Strings join with `+` and compare
alphabetically. `A$(2 TO 4)`, `A$(3)`, `A$(TO 2)` and `A$(2 TO)` take
part of a string.

**Functions:** `ABS INT SGN SQR SIN COS TAN ATN ASN ACS LN EXP PI RND`,
`LEN VAL CODE CHR$ STR$`, `INKEY$` (the key being pressed, or `""`), and
`POINT (x,y)` (1 if that pixel is ink). As on the Spectrum, a function
applies to the next item, so `SQR 4*3` is 6; use brackets for more.
`VAL` works out a whole expression: `VAL "2*3"` is 6.

**Numbers** are printed with up to 8 significant digits, and in
exponent form when very large or small (`1.5E+12`). `CHR$ 128` to
`CHR$ 143` are block graphics.

**Limits:** program lines 1-9999 of up to 250 characters, a 32KB program
area, 128 variables, 192KB for arrays, 16 nested `FOR` loops, 32 nested
`GOSUB`s.

### Differences from Spectrum BASIC

It isn't a copy of Sinclair BASIC: keywords are typed in full rather
than with single keys, there's no `DEF FN`, `PEEK`/`POKE`, `ATTR`,
`SCREEN$` or `FLASH`, and string arrays hold separate strings of any
length (not fixed-length character arrays). Names of variables and
string variables can be longer than one letter.

## Saving programs

`SAVE "name"` writes the program to the disk the computer booted from;
`LOAD "name"` reads it back, `CAT` lists what's saved and `ERASE "name"`
deletes one. `SAVE "name" LINE 10` makes `LOAD` start the program at
line 10. There's room for 15 programs, with names of up to 15
characters.

Programs are kept in a reserved area of the disk image, so they persist
on a USB stick, and the BIOS and UEFI versions share them. Rebuilding
`os.img` with `make` starts with an empty save area.

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

`make boot-test` boots the image five ways: BIOS floppy, BIOS hard disk,
BIOS USB stick, UEFI USB stick and UEFI hard disk. It reads the text
back from screenshots. It uses KVM when available, because QEMU's
emulator doesn't enforce some CPU rules that real PCs do. The same tests
run on GitHub Actions for every push (`.github/workflows/ci.yml`).

## How it works

    boot/boot.asm          boot sector: loads the 16-bit kernel; also the
                           partition table and floppy BPB
    kernel/kernel_entry.asm  16-bit start-up: sets up "unreal mode" so the
                           real-mode kernel can use memory above 64KB
    kernel/bios.c          BIOS platform: VGA mode 13h, keyboard, timer,
                           speaker, disk (INT 13h)
    kernel/uefi/uefi.c     UEFI platform: graphics output, keyboard,
                           timer, Block I/O
    kernel/screen.c        the Spectrum-style screen, cursor and keys
    kernel/editor.c        the line editor
    kernel/basic.c         the interpreter (tokenizer, expressions,
                           statements)
    kernel/num.c           numbers: x87 maths, formatting, parsing
    kernel/storage.c       SAVE/LOAD directory on the disk
    kernel/font.c          the character set (generated by tools/mkfont.py)
    tools/mkimage.py       builds os.img, including its FAT EFI partition

Everything above the platform layer (`kernel/platform.h`) is shared by
the BIOS and UEFI kernels and the host tests.
