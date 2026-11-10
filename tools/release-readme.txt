x86-BASIC @VERSION@
==================

Home-computer BASICs for PCs: boot a PC from a USB stick and it starts
straight into BASIC. https://github.com/jasontrainor/x86-BASIC

The disk images
---------------

    x86-basic.img   every machine, with a menu to choose one
    spectrum.img    Spectrum BASIC only (ZX Spectrum listings)
    a1basic.img     Integer BASIC (1976; Apple I listings)
    a2int.img       Integer BASIC II (1977; Apple II listings)
    a2fp.img        FP BASIC (1978-83; Applesoft listings)

Each image boots on both BIOS and UEFI PCs, from a USB stick, hard
disk or floppy. SHA256SUMS has their checksums.

Writing one to a USB stick
--------------------------

Everything already on the stick is erased.

  Windows, macOS, Linux: a raw image writer such as balenaEtcher, or
  Rufus on Windows (in "DD image" mode).

  Linux: find the stick's device with lsblk (e.g. /dev/sdb; check the
  size and model carefully), then write to the whole device:

      sudo dd if=x86-basic.img of=/dev/sdX bs=4M conv=fsync status=progress

Booting
-------

Open the PC's boot menu (commonly F12, F11, F8 or Esc at power-on) and
pick the USB stick. On UEFI PCs, Secure Boot must be turned off in the
firmware setup, as the kernel isn't signed. Hold Shift while the PC
starts (BIOS), or press F10 twice in any machine, to get to the menu.

Trying it without a USB stick
-----------------------------

With QEMU installed:

    qemu-system-x86_64 -drive file=x86-basic.img,format=raw,if=floppy -boot a

The full manual is README.md, alongside this file.
