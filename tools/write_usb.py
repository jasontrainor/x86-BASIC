#!/usr/bin/env python3
"""Write a disk image to a USB stick (Linux).

    tools/write_usb.py os.img /dev/sdX [--yes]

Everything on the stick is replaced. To guard against writing over the
wrong disk, it refuses a device that isn't a whole, removable disk (USB
sticks and card readers are; internal disks aren't; USB disks that don't
call themselves removable, such as SSDs in USB cases, are allowed), one
with anything mounted, or one smaller than the image; and it asks before writing,
showing the disk's make and size, unless --yes is given. Needs
permission to write the device (run with sudo, or as a member of the
"disk" group).
"""
import argparse
import os
import stat
import sys


def sysfs(dev, *path):
    try:
        return open(os.path.join('/sys/block', dev, *path)).read().strip()
    except OSError:
        return ''


def mounted(dev):
    """Mount points of the device or any of its partitions."""
    out = []
    with open('/proc/mounts') as f:
        for line in f:
            src, point = line.split()[:2]
            name = os.path.basename(os.path.realpath(src)) if src.startswith('/dev/') else ''
            if name == dev or (name.startswith(dev) and
                               os.path.exists(os.path.join('/sys/block', dev, name))):
                out.append(point)
    return out


def main():
    ap = argparse.ArgumentParser(description='Write a disk image to a USB stick.')
    ap.add_argument('image')
    ap.add_argument('device')
    ap.add_argument('--yes', action='store_true', help="don't ask first")
    a = ap.parse_args()

    image = open(a.image, 'rb').read()
    path = os.path.realpath(a.device)
    try:
        mode = os.stat(path).st_mode
    except OSError as e:
        sys.exit('%s: %s' % (a.device, e.strerror))
    if not stat.S_ISBLK(mode):
        sys.exit('%s: not a disk device' % a.device)
    dev = os.path.basename(path)
    if not os.path.isdir(os.path.join('/sys/block', dev)):
        sys.exit('%s: a partition, not a whole disk (use /dev/%s, say)' %
                 (a.device, dev.rstrip('0123456789').rstrip('p') or dev))
    on_usb = '/usb' in os.path.realpath(os.path.join('/sys/block', dev))
    if sysfs(dev, 'removable') != '1' and not on_usb:
        sys.exit('%s: not a removable or USB disk; refusing to write to it' % a.device)
    points = mounted(dev)
    if points:
        sys.exit('%s: in use (mounted on %s); unmount it first' % (a.device, ', '.join(points)))
    size = int(sysfs(dev, 'size') or 0) * 512
    if size < len(image):
        sys.exit('%s: too small (%d bytes, the image is %d)' % (a.device, size, len(image)))

    model = ' '.join(x for x in (sysfs(dev, 'device', 'vendor'), sysfs(dev, 'device', 'model'))
                     if x) or 'disk'
    print('%s: %s, %.1f GB' % (path, model, size / 1e9))
    if not a.yes:
        answer = input('Everything on it will be replaced by %s. Type the device name '
                       '(%s) to go ahead: ' % (a.image, dev))
        if answer.strip() not in (dev, path):
            sys.exit('Not written.')
    try:
        with open(path, 'r+b', buffering=0) as f:
            for off in range(0, len(image), 1 << 20):
                f.write(image[off:off + (1 << 20)])
            os.fsync(f.fileno())
    except PermissionError:
        sys.exit('%s: no permission to write it (try sudo)' % path)
    print('Written: %d bytes. It can be unplugged.' % len(image))


if __name__ == '__main__':
    main()
