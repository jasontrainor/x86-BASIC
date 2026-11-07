#!/bin/bash
# Build os.img (if needed) and boot it in QEMU.
#   ./run.sh          boot as a floppy disk (BIOS)
#   ./run.sh usb      boot as a USB stick, as on a real PC with legacy BIOS boot
#   ./run.sh uefi     boot as a USB stick on a UEFI PC (needs OVMF firmware)
set -e
cd "$(dirname "$0")"
make --no-print-directory

usb_stick=(-drive file=os.img,format=raw,if=none,id=stick
           -device qemu-xhci -device usb-storage,drive=stick,bootindex=0)

case "${1:-floppy}" in
    floppy)
        exec qemu-system-x86_64 -drive file=os.img,format=raw,if=floppy -boot a ;;
    usb)
        exec qemu-system-x86_64 "${usb_stick[@]}" ;;
    uefi)
        for f in /usr/share/ovmf/OVMF.fd /usr/share/qemu/OVMF.fd \
                 /usr/share/OVMF/OVMF.fd /usr/share/edk2/ovmf/OVMF.fd \
                 /usr/share/edk2-ovmf/x64/OVMF.fd; do
            [ -f "$f" ] && exec qemu-system-x86_64 -bios "$f" "${usb_stick[@]}"
        done
        echo "No OVMF UEFI firmware found; install the ovmf package." >&2
        exit 1 ;;
    *)
        echo "usage: $0 [floppy|usb|uefi]" >&2
        exit 2 ;;
esac
