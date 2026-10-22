"""Rewrite the GPT of a truncated card image so it is self-consistent: set one partition's size,
pad the image with room for the backup GPT, and write primary + backup headers with fresh CRCs."""
import sys, os, struct, zlib, uuid

img, part_no, part_sectors, pad_sectors = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4])
assert "backup" not in os.path.basename(img), "refusing to modify the backup image"
SECT = 512

f = open(img, "r+b")
f.seek(SECT)
hdr = bytearray(f.read(92))
assert hdr[:8] == b"EFI PART"
hdr_size, = struct.unpack_from("<I", hdr, 12)
ent_lba, n_ent, ent_size = struct.unpack_from("<QII", hdr, 72)
f.seek(ent_lba * SECT)
ents = bytearray(f.read(n_ent * ent_size))
assert zlib.crc32(ents) & 0xFFFFFFFF == struct.unpack_from("<I", hdr, 88)[0], "entry CRC mismatch before edit"

e = (part_no - 1) * ent_size
first, last = struct.unpack_from("<QQ", ents, e + 32)
new_last = first + part_sectors - 1
print("partition %d: LBA %d-%d -> %d-%d" % (part_no, first, last, first, new_last))
struct.pack_into("<Q", ents, e + 40, new_last)

ent_sectors = (n_ent * ent_size + SECT - 1) // SECT
data_end = new_last + 1
total = data_end + pad_sectors
assert pad_sectors >= ent_sectors + 1
# make sure nothing else in the table extends past the new end
for i in range(n_ent):
    s, l = struct.unpack_from("<QQ", ents, i * ent_size + 32)
    if ents[i * ent_size:i * ent_size + 16] != b"\0" * 16:
        assert l <= total - ent_sectors - 2, "partition %d would extend into the backup GPT" % (i + 1)

f.seek(0, 2)
cur = f.tell() // SECT
assert cur <= total, "image is already larger than the new layout"
f.truncate(total * SECT)

ent_crc = zlib.crc32(ents) & 0xFFFFFFFF
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


f.seek(1 * SECT); f.write(header(1, backup_lba, ent_lba))
f.seek(ent_lba * SECT); f.write(ents)
f.seek(backup_ent_lba * SECT); f.write(ents)
f.seek(backup_lba * SECT); f.write(header(backup_lba, 1, backup_ent_lba))
f.close()
print("image now %d sectors (%d bytes); last usable LBA %d; backup GPT at %d" % (total, total * SECT, last_usable, backup_lba))
