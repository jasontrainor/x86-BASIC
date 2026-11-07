#!/bin/bash
# Build os.img (if needed) and boot it in QEMU.
#   ./run.sh          boot as a floppy disk
#   ./run.sh usb      boot as a USB stick, as on real hardware
set -e
cd "$(dirname "$0")"
make --no-print-directory

case "${1:-floppy}" in
    floppy)
        exec qemu-system-x86_64 -drive file=os.img,format=raw,if=floppy -boot a ;;
    usb)
        exec qemu-system-x86_64 -drive file=os.img,format=raw,if=none,id=stick \
            -device qemu-xhci -device usb-storage,drive=stick,bootindex=0 ;;
    *)
        echo "usage: $0 [floppy|usb]" >&2
        exit 2 ;;
esac
