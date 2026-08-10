#!/usr/bin/env python3
# Tadpole — read a FAT12/16/32 filesystem image, with nothing installed.
#
#   from fatread import FatFS
#   fs = FatFS("cart.bin")
#   for path, size, mtime, read in fs.walk():
#       data = read()
#
# WHY THIS IS HAND-WRITTEN RATHER THAN SHELLED OUT
# ------------------------------------------------
# The obvious answers all fail on one of the two platforms Tadpole supports:
#
#   mount -o loop      needs root, and Windows has no such thing
#   mtools (mcopy)     not installed on any of the three Linux distros
#                      Tadpole documents, and not on Windows at all
#   7z                 same, plus it is a big dependency for one job
#
# Windows in particular ships a bundled CPython 3.7 with exactly two extra
# packages, chosen so firmware install works with nothing to install first
# (see tools/build-windows.sh). Adding "and also go find mtools" would put the
# same wall back in front of the user that the bundle exists to remove.
#
# So this reads the filesystem directly. FAT is small, completely specified,
# and forty years stable — about three hundred lines, no dependencies, and
# identical behaviour on both platforms because it is all arithmetic on bytes.
#
# WHAT A CARTRIDGE DUMP IS. Users back a cartridge up on the device itself:
#
#     dd if=/dev/mtdblock6 of=/LF/Bulk/cart.bin
#
# then pull it off over FTP. The result is a raw image of the cartridge's
# filesystem — mkdosfs, label "Cartridge", FAT32 on the 64 MB and 128 MB carts.
# It is a perfectly good backup and Tadpole could not read one.
#
# LONG FILENAMES ARE NOT OPTIONAL HERE. LeapFrog's own files are named things
# like "A1_Background_Props_Far.anim", which is not 8.3, so a reader that only
# handles short names produces a package of mangled stubs that installs and
# then fails to load. VFAT stores the real name in extra directory entries
# preceding the short one, and this assembles them.

import os
import struct
import sys
import time


class FatError(Exception):
    pass


ATTR_READ_ONLY = 0x01
ATTR_HIDDEN    = 0x02
ATTR_SYSTEM    = 0x04
ATTR_VOLUME_ID = 0x08
ATTR_DIRECTORY = 0x10
ATTR_LFN       = 0x0F          # a long-name fragment, not a file


def _fat_time(date, tm):
    """FAT's packed date/time -> Unix seconds.

    FAT counts years from 1980 and stores seconds in 2-second units, which is
    why timestamps out of a cartridge are always even. A zero date means "not
    set"; the epoch is a better answer there than 1980.
    """
    if not date:
        return 0
    year  = ((date >> 9) & 0x7F) + 1980
    month = (date >> 5) & 0x0F
    day   = date & 0x1F
    hour  = (tm >> 11) & 0x1F
    minute = (tm >> 5) & 0x3F
    sec   = (tm & 0x1F) * 2
    try:
        return int(time.mktime((year, max(1, month), max(1, day),
                                hour, minute, min(59, sec), 0, 0, -1)))
    except (ValueError, OverflowError):
        return 0


class FatFS(object):
    def __init__(self, path):
        self.f = open(path, "rb")
        self.size = os.fstat(self.f.fileno()).st_size
        self._read_bpb()

    def close(self):
        try:
            self.f.close()
        except OSError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # ---- geometry ------------------------------------------------------
    def _read_bpb(self):
        self.f.seek(0)
        b = self.f.read(512)
        if len(b) < 512:
            raise FatError("file is too small to be a filesystem image")
        if b[510:512] != b"\x55\xaa":
            raise FatError("no boot signature — this is not a FAT image")

        self.bytes_per_sector = struct.unpack_from("<H", b, 11)[0]
        self.sectors_per_cluster = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.num_fats = b[16]
        self.root_entries = struct.unpack_from("<H", b, 17)[0]
        total16 = struct.unpack_from("<H", b, 19)[0]
        fatsz16 = struct.unpack_from("<H", b, 22)[0]
        total32 = struct.unpack_from("<I", b, 32)[0]

        if self.bytes_per_sector not in (512, 1024, 2048, 4096):
            raise FatError("implausible sector size %d" % self.bytes_per_sector)
        if self.sectors_per_cluster == 0 or self.num_fats == 0:
            raise FatError("not a FAT image (zero clusters or FATs)")

        fatsz = fatsz16 or struct.unpack_from("<I", b, 36)[0]
        total = total16 or total32
        if not fatsz or not total:
            raise FatError("not a FAT image (empty FAT or volume)")

        # The root directory is a fixed area on FAT12/16 and an ordinary
        # cluster chain on FAT32; this is what tells the two apart.
        root_sectors = ((self.root_entries * 32) + self.bytes_per_sector - 1) \
            // self.bytes_per_sector
        self.first_data_sector = (self.reserved + self.num_fats * fatsz
                                  + root_sectors)
        data_sectors = total - self.first_data_sector
        self.cluster_count = data_sectors // self.sectors_per_cluster

        # THE OFFICIAL DEFINITION, and the only correct one: the FAT type is
        # decided by the cluster count, never by the "FAT32   " string in the
        # boot sector. That string is a hint that formatters get wrong.
        if self.cluster_count < 4085:
            self.bits = 12
        elif self.cluster_count < 65525:
            self.bits = 16
        else:
            self.bits = 32

        self.root_cluster = struct.unpack_from("<I", b, 44)[0] if self.bits == 32 else 0
        self.root_sectors = root_sectors
        self.cluster_size = self.sectors_per_cluster * self.bytes_per_sector
        self.fat_start = self.reserved * self.bytes_per_sector
        self.fat_bytes = fatsz * self.bytes_per_sector
        self.label = b[71:82].decode("latin-1").strip() if self.bits == 32 \
            else b[43:54].decode("latin-1").strip()
        self._fat = None

    def _load_fat(self):
        if self._fat is None:
            self.f.seek(self.fat_start)
            self._fat = self.f.read(self.fat_bytes)
        return self._fat

    def _next_cluster(self, cl):
        fat = self._load_fat()
        if self.bits == 32:
            off = cl * 4
            if off + 4 > len(fat):
                return 0x0FFFFFFF
            return struct.unpack_from("<I", fat, off)[0] & 0x0FFFFFFF
        if self.bits == 16:
            off = cl * 2
            if off + 2 > len(fat):
                return 0xFFFF
            return struct.unpack_from("<H", fat, off)[0]
        # FAT12: entries are twelve bits, so every other one is split across
        # a byte boundary and which half you want depends on parity.
        off = cl + (cl // 2)
        if off + 2 > len(fat):
            return 0xFFF
        v = struct.unpack_from("<H", fat, off)[0]
        return (v >> 4) if (cl & 1) else (v & 0x0FFF)

    def _is_eoc(self, cl):
        return cl >= {12: 0x0FF8, 16: 0xFFF8, 32: 0x0FFFFFF8}[self.bits]

    def _chain(self, cl):
        """Cluster numbers of a file, in order.

        LOOP-GUARDED. A truncated or corrupt dump can contain a FAT entry that
        points back into its own chain, and without the seen-set this walks
        forever quietly filling the disk with a tar.
        """
        out, seen = [], set()
        while cl >= 2 and not self._is_eoc(cl):
            if cl in seen or len(out) > self.cluster_count + 2:
                break
            seen.add(cl)
            out.append(cl)
            cl = self._next_cluster(cl)
        return out

    def _read_cluster(self, cl):
        sector = self.first_data_sector + (cl - 2) * self.sectors_per_cluster
        self.f.seek(sector * self.bytes_per_sector)
        return self.f.read(self.cluster_size)

    def _read_chain(self, first, size=None):
        parts, got = [], 0
        for cl in self._chain(first):
            b = self._read_cluster(cl)
            parts.append(b)
            got += len(b)
            if size is not None and got >= size:
                break
        data = b"".join(parts)
        return data[:size] if size is not None else data

    # ---- directories ---------------------------------------------------
    def _dir_bytes(self, cluster):
        if cluster == 0 and self.bits != 32:
            # FAT12/16 root: a fixed run of sectors before the data area.
            self.f.seek((self.first_data_sector - self.root_sectors)
                        * self.bytes_per_sector)
            return self.f.read(self.root_sectors * self.bytes_per_sector)
        return self._read_chain(cluster or self.root_cluster)

    def _entries(self, cluster):
        """(name, attr, first_cluster, size, mtime) for one directory."""
        raw = self._dir_bytes(cluster)
        out = []
        lfn = {}
        for off in range(0, len(raw) - 31, 32):
            e = raw[off:off + 32]
            first = e[0]
            if first == 0x00:            # nothing further in this directory
                break
            if first == 0xE5:            # deleted
                lfn = {}
                continue
            attr = e[11]
            if attr == ATTR_LFN:
                seq = first & 0x3F
                chars = e[1:11] + e[14:26] + e[28:32]
                lfn[seq] = chars
                continue
            if attr & ATTR_VOLUME_ID:    # the volume label is not a file
                lfn = {}
                continue

            name = self._long_name(lfn) or self._short_name(e)
            lfn = {}
            if name in (".", ".."):
                continue
            cl = (struct.unpack_from("<H", e, 26)[0]
                  | (struct.unpack_from("<H", e, 20)[0] << 16
                     if self.bits == 32 else 0))
            size = struct.unpack_from("<I", e, 28)[0]
            mtime = _fat_time(struct.unpack_from("<H", e, 24)[0],
                              struct.unpack_from("<H", e, 22)[0])
            out.append((name, attr, cl, size, mtime))
        return out

    @staticmethod
    def _long_name(lfn):
        if not lfn:
            return None
        raw = b"".join(lfn[k] for k in sorted(lfn))
        try:
            s = raw.decode("utf-16-le", "replace")
        except Exception:
            return None
        # Padding is 0xFFFF, and the name ends at the first NUL.
        s = s.split("\x00")[0].replace("￿", "")
        return s or None

    @staticmethod
    def _short_name(e):
        base = e[0:8].decode("latin-1").rstrip()
        ext = e[8:11].decode("latin-1").rstrip()
        # 0x05 stands in for a leading 0xE5 in a real name, because 0xE5 marks
        # a deleted entry and could not be stored literally.
        if base[:1] == "\x05":
            base = "\xe5" + base[1:]
        # VFAT's case flags: WinNT stored "readme.txt" as a short entry with
        # bit 3 set rather than spending a long entry on it, so honouring them
        # is the difference between GameInfo.json and GAMEINFO.JSON.
        nt = e[12]
        if nt & 0x08:
            base = base.lower()
        if nt & 0x10:
            ext = ext.lower()
        return base + ("." + ext if ext else "")

    # ---- the public walk -----------------------------------------------
    def walk(self, cluster=None, prefix=""):
        """Yield (path, is_dir, size, mtime, read) for everything, depth-first.

        `read` is a callable so a caller can skip a file's contents without
        paying to read them, which matters on a 128 MB image.
        """
        if cluster is None:
            cluster = self.root_cluster if self.bits == 32 else 0
        for name, attr, cl, size, mtime in self._entries(cluster):
            path = prefix + name
            if attr & ATTR_DIRECTORY:
                yield path, True, 0, mtime, None
                # A directory whose first cluster is 0 is empty, not the root;
                # recursing into it would list the root again, forever.
                if cl >= 2:
                    for item in self.walk(cl, path + "/"):
                        yield item
            else:
                yield (path, False, size, mtime,
                       (lambda c=cl, s=size: self._read_chain(c, s) if s else b""))


def main(argv):
    if len(argv) < 2:
        print("usage: fatread.py <image> [--list]", file=sys.stderr)
        return 2
    with FatFS(argv[1]) as fs:
        print("FAT%d  label=%r  %d clusters of %d bytes"
              % (fs.bits, fs.label, fs.cluster_count, fs.cluster_size),
              file=sys.stderr)
        n = 0
        for path, is_dir, size, _mtime, _read in fs.walk():
            n += 1
            print("%s%s" % (path, "/" if is_dir else "  (%d)" % size))
        print("%d entries" % n, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
