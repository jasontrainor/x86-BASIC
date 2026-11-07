# x86-BASIC

## Build and run

Needs `nasm`, `gcc`, GNU `ld`, `make` and `qemu-system-x86_64`.

    ./run.sh          # build, then boot in QEMU as a floppy
    ./run.sh usb      # build, then boot as a USB stick

or build with `make` and run QEMU yourself:

    qemu-system-x86_64 -drive file=os.img,format=raw,if=floppy -boot a

## Using BASIC

Type a line with a number to store it in the program, or without one to
run it immediately. Keywords can be typed in any case.

| Command | Meaning |
|---|---|
| `RUN`, `LIST`, `NEW` | run, list or erase the program |
| `10` (number alone) | delete line 10 |
| `PRINT "TEXT"; A; A$` | print strings, numbers and string variables |
| `LET A = 2*(B+1)` (`LET` optional) | integer variables `A`-`Z` |
| `LET A$ = "TEXT"` | string variables `A$`-`Z$` (up to 63 characters) |
| `IF A <> B THEN statement` | comparisons `=` `<>` `<` `<=` `>` `>=` |
| `GOTO 100` | jump to a line |
| `FOR I = 1 TO 10 STEP 2` ... `NEXT I` | loops, up to 16 nested |
| `INK n`, `PAPER n`, `BORDER n` | colours 0-7, as on the ZX Spectrum |
| `CLS`, `BEEP`, `END` | clear screen, beep, stop the program |

Press **Esc** or **Ctrl+C** to stop a running program. Program lines are
numbered 1-9999, hold up to 63 characters, and a program can have up to
256 lines.

## Tests

    make test        # interpreter tests on the host, with ASan/UBSan
    make boot-test   # boot the image in QEMU (floppy, hard disk, USB)

## Booting real hardware from a USB stick

`os.img` can be written directly to a USB stick and booted on a PC.

**Requirements**

- The PC must be able to boot in **legacy BIOS mode** (often labelled
  "CSM", "Legacy boot" or "Legacy support" in the firmware setup). This
  kernel uses BIOS services for the screen, keyboard and disk, so it does
  not boot on UEFI-only machines (many PCs from about 2020 on). Secure
  Boot must be off for legacy boot to be offered.
- Everything else on the stick will be erased.

**Write the image (Linux)**

1. Plug the stick in and find its device name with `lsblk`, e.g.
   `/dev/sdb`. Check the size and model carefully: writing to the wrong
   device destroys its contents.
2. Write the image to the whole device (not a partition such as
   `/dev/sdb1`):

       sudo dd if=os.img of=/dev/sdX bs=4M conv=fsync status=progress

On Windows, a raw image writer such as Rufus (in "DD image" mode) or
balenaEtcher does the same job.

**Boot**

Open the firmware's boot menu (commonly F12, F11, F8 or Esc at power-on)
and pick the USB stick, choosing the non-UEFI entry if it is listed twice.

The boot sector works whether the firmware presents the stick as a hard
disk ("USB-HDD", the usual default) or as a floppy ("USB-FDD"). If one
doesn't boot, try switching the USB emulation mode in firmware setup.

**Notes on modern hardware**

- USB keyboards work through the firmware's legacy USB keyboard support,
  which is normally enabled whenever legacy boot is.
- `BORDER` sets the VGA overscan colour, which many modern graphics
  cards and flat panels don't display.
- `BEEP` drives the PC speaker. Many modern PCs have no speaker, in
  which case it is silent.

You can try the USB boot path without a stick:

    make boot-test

boots the image in QEMU as a floppy, as a hard disk, and as a USB stick on
an xHCI controller.
