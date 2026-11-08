"""Read files from the FAT partition of a disk image (FAT12 or FAT16,
with folders), for the tests and tools.

    import fatread
    disk = fatread.Disk(open('os.img', 'rb').read())
    disk.read('SPECTRUM/GAME.BAS')        # bytes, or None
    disk.listdir('SPECTRUM')              # [(name, size, is_folder)]
"""
import struct

FAT_TYPES = (0x01, 0x04, 0x06, 0x0E, 0xEF)


class Disk:
    def __init__(self, data):
        self.data = data
        start = 0
        for i in range(4):
            e = data[446 + i * 16:462 + i * 16]
            if e[4] in FAT_TYPES:
                start = struct.unpack_from('<I', e, 8)[0]
                break
        else:
            raise ValueError('no FAT partition')
        self.start = start
        p = data[start * 512:]
        self.part = p
        self.per_cluster = p[13]
        reserved = struct.unpack_from('<H', p, 14)[0]
        self.fats = p[16]
        self.root_entries = struct.unpack_from('<H', p, 17)[0]
        total = struct.unpack_from('<H', p, 19)[0] or struct.unpack_from('<I', p, 32)[0]
        self.fat_sectors = struct.unpack_from('<H', p, 22)[0]
        self.fat = p[reserved * 512:(reserved + self.fat_sectors) * 512]
        self.root = (reserved + self.fats * self.fat_sectors) * 512
        self.data_start = self.root + (self.root_entries * 32 + 511) // 512 * 512
        self.clusters = (total - self.data_start // 512) // self.per_cluster
        self.fat16 = self.clusters >= 4085
        self.size = total * 512

    def next_cluster(self, c):
        if self.fat16:
            return struct.unpack_from('<H', self.fat, c * 2)[0]
        w = struct.unpack_from('<H', self.fat, c * 3 // 2)[0]
        return w >> 4 if c & 1 else w & 0xFFF

    def chain(self, c):
        end = 0xFFF8 if self.fat16 else 0xFF8
        seen = 0
        while 2 <= c < end and seen <= self.clusters:
            yield c
            c = self.next_cluster(c)
            seen += 1

    def cluster_bytes(self, c):
        off = self.data_start + (c - 2) * self.per_cluster * 512
        return self.part[off:off + self.per_cluster * 512]

    def dir_bytes(self, cluster):
        if cluster == 0:
            return self.part[self.root:self.root + self.root_entries * 32]
        return b''.join(self.cluster_bytes(c) for c in self.chain(cluster))

    def entries(self, cluster):
        raw = self.dir_bytes(cluster)
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08:
                continue
            base, ext = e[:8].decode('latin-1').rstrip(), e[8:11].decode('latin-1').rstrip()
            name = base + ('.' + ext if ext else '')
            c, size = struct.unpack_from('<HI', e, 26)
            yield name, c, size, bool(e[11] & 0x10)

    def find(self, path):
        """(cluster, size, is_folder) of a path, or None."""
        cluster, found = 0, (0, 0, True)
        for part in [p for p in path.upper().split('/') if p]:
            if not found[2]:
                return None
            for name, c, size, folder in self.entries(cluster):
                if name == part:
                    found = (c, size, folder)
                    cluster = c
                    break
            else:
                return None
        return found

    def read(self, path):
        f = self.find(path)
        if not f or f[2]:
            return None
        c, size, _ = f
        return b''.join(self.cluster_bytes(x) for x in self.chain(c))[:size]

    def listdir(self, path=''):
        f = self.find(path)
        if not f or not f[2]:
            return []
        return [(n, s, d) for n, c, s, d in self.entries(f[0]) if n not in ('.', '..')]
