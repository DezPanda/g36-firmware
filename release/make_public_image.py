#!/usr/bin/env python3
# SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda
"""Puts the public card image together.

    python3 make_public_image.py BASE.img ROOTFS.img EMU.img OUT.img

BASE.img   a card image up to the end of the emulators partition (the boot parts and the
           partition table come from it: first-stage loader, U-Boot, the boot partition)
ROOTFS.img the new system partition (exactly the size of partition 3)
EMU.img    the emulators partition (exactly the size of partition 4)

The games drive isn't in the image: the first boot creates it in the rest of whatever card the
image is flashed to. So the partition table is cut down to partitions 1 to 4, with a backup table
1 MiB after the end (what g36-clean-adb.img had). Never run on a card; it only writes OUT.img.
"""
import hashlib
import os
import struct
import sys
import zlib

SECT = 512
BUF = 1 << 22


def main():
    base, rootfs, emu, out = sys.argv[1:5]
    assert not os.path.exists(out), "refusing to overwrite " + out
    f = open(base, "rb")
    f.seek(SECT)
    hdr = bytearray(f.read(92))
    assert hdr[:8] == b"EFI PART"
    hdr_size, = struct.unpack_from("<I", hdr, 12)
    ent_lba, n_ent, ent_size = struct.unpack_from("<QII", hdr, 72)
    f.seek(ent_lba * SECT)
    ents = bytearray(f.read(n_ent * ent_size))
    assert zlib.crc32(ents) & 0xFFFFFFFF == struct.unpack_from("<I", hdr, 88)[0], "entry CRC mismatch"

    def part(i):
        e = ents[(i - 1) * ent_size:i * ent_size]
        return struct.unpack_from("<QQ", e, 32)

    p3, p4 = part(3), part(4)
    assert os.path.getsize(rootfs) == (p3[1] - p3[0] + 1) * SECT, "system partition is the wrong size"
    assert os.path.getsize(emu) == (p4[1] - p4[0] + 1) * SECT, "emulators partition is the wrong size"
    end = p4[1] + 1                       # sectors up to the end of partition 4

    # the new partition table: partitions 1-4 only
    ents[4 * ent_size:] = b"\0" * (len(ents) - 4 * ent_size)
    ent_crc = zlib.crc32(bytes(ents)) & 0xFFFFFFFF
    ent_sectors = (n_ent * ent_size + SECT - 1) // SECT
    total = end + 2048                    # 1 MiB for the backup table
    backup_lba = total - 1
    backup_ent_lba = total - 1 - ent_sectors
    last_usable = backup_ent_lba - 1

    def header(current, other, entries_at):
        h = bytearray(hdr)
        struct.pack_into("<QQQQ", h, 24, current, other, struct.unpack_from("<Q", hdr, 40)[0], last_usable)
        struct.pack_into("<Q", h, 72, entries_at)
        struct.pack_into("<I", h, 88, ent_crc)
        struct.pack_into("<I", h, 16, 0)
        struct.pack_into("<I", h, 16, zlib.crc32(bytes(h[:hdr_size])) & 0xFFFFFFFF)
        return bytes(h) + b"\0" * (SECT - len(h))

    with open(out, "wb") as o:
        # everything before the system partition, as it is in the base image
        f.seek(0)
        left = p3[0] * SECT
        while left:
            chunk = f.read(min(BUF, left))
            o.write(chunk)
            left -= len(chunk)
        for src in (rootfs, emu):
            with open(src, "rb") as s:
                while True:
                    chunk = s.read(BUF)
                    if not chunk:
                        break
                    o.write(chunk)
        assert o.tell() == end * SECT, (o.tell(), end * SECT)
        o.truncate(total * SECT)
        o.seek(1 * SECT)
        o.write(header(1, backup_lba, ent_lba))
        o.seek(ent_lba * SECT)
        o.write(bytes(ents))
        o.seek(backup_ent_lba * SECT)
        o.write(bytes(ents))
        o.seek(backup_lba * SECT)
        o.write(header(backup_lba, 1, backup_ent_lba))
    print("%s: %d bytes; partitions 1-4; last usable LBA %d; backup table at %d" % (out, total * SECT, last_usable, backup_lba))


if __name__ == "__main__":
    main()
