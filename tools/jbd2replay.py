"""Replay an ext4 journal into a COPY of the card image, skipping data blocks whose checksum is bad
only when a newer valid copy of the same filesystem block is replayed later (so the end state is identical
to a perfect replay). Then mark the journal empty and clear the needs_recovery flag, like the kernel does."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from ext4read import Ext4, PARTS
from jbd2scan import scan, crc32c, MAGIC

img, part = sys.argv[1], sys.argv[2]
assert "backup" not in os.path.basename(img), "refusing to modify the backup image"
base = PARTS[part]
fs = Ext4(img, base)
txns = scan(fs, verbose=False)
info = scan.info
jdata, bs = info["jdata"], fs.bs

# safety: every bad block must be superseded by a later good copy
for i, t in enumerate(txns):
    assert t["commit_ok"], "commit checksum bad in txn %d" % t["seq"]
    for target, jb, esc, ok in t["tags"]:
        if not ok:
            later_good = any(tg == target and ok2 for t2 in txns[i + 1:] for tg, _, _, ok2 in t2["tags"])
            assert later_good, "bad block %d in txn %d has no newer valid copy" % (target, t["seq"])
    assert t["revokes"] == 0, "revoke handling not implemented"

out = open(img, "r+b")
written = skipped = 0
for t in txns:
    for target, jb, esc, ok in t["tags"]:
        if not ok:
            skipped += 1
            continue
        data = jdata[jb * bs:(jb + 1) * bs]
        if esc:
            data = struct.pack(">I", MAGIC) + data[4:]
        out.seek(base + target * bs)
        out.write(data)
        written += 1
print("replayed %d transactions (%d..%d): %d blocks written, %d superseded bad copies skipped" % (
    len(txns), txns[0]["seq"], txns[-1]["seq"], written, skipped))

# journal superblock (journal block 0): s_sequence = next txn, s_start = 0, fresh checksum
ext = []
fs._extent_blocks(fs.info(info["j_ino"])["raw"][40:100], ext)
ext.sort()
assert ext[0][0] == 0
jsb_pos = base + ext[0][1] * bs
out.seek(jsb_pos)
jsb = bytearray(out.read(1024))
assert struct.unpack_from(">I", jsb, 0)[0] == MAGIC
struct.pack_into(">II", jsb, 0x18, info["last_seq"] + 1, 0)
struct.pack_into(">I", jsb, 0xFC, 0)
struct.pack_into(">I", jsb, 0xFC, crc32c(0xFFFFFFFF, bytes(jsb)))
out.seek(jsb_pos)
out.write(jsb)

# ext4 superblock: clear INCOMPAT_RECOVER, fresh checksum
out.seek(base + 1024)
sb = bytearray(out.read(1024))
inc, = struct.unpack_from("<I", sb, 0x60)
struct.pack_into("<I", sb, 0x60, inc & ~0x4)
struct.pack_into("<I", sb, 0x3FC, crc32c(0xFFFFFFFF, bytes(sb[:0x3FC])))
out.seek(base + 1024)
out.write(sb)
out.close()
print("journal marked empty (next sequence %d), needs_recovery cleared" % (info["last_seq"] + 1))
