"""Overwrite same-size files in the rootfs of a COPY of the card image, in place.

Only the file's data block is rewritten; inode, size and checksums are untouched,
so the replacement must be exactly the same length as the original.
"""
import sys, os, hashlib
sys.path.insert(0, os.path.dirname(__file__))
from ext4read import Ext4, PARTS

img = sys.argv[1]
assert "backup" not in os.path.basename(img), "refusing to modify the backup image"
pairs = list(zip(sys.argv[2::2], sys.argv[3::2]))  # (path in rootfs, local replacement file)

fs = Ext4(img, PARTS["rootfs"])
with open(img, "r+b") as out:
    for path, local in pairs:
        new = open(local, "rb").read()
        ino = fs.lookup(path)
        nfo = fs.info(ino)
        old = fs.read(ino)
        assert len(new) == nfo["size"] == len(old), (path, len(new), nfo["size"])
        assert nfo["flags"] & 0x80000, "expected extent-mapped file"
        ext = []
        fs._extent_blocks(nfo["raw"][40:100], ext)
        assert len(ext) == 1 and ext[0][0] == 0 and not ext[0][3], ext
        pos = PARTS["rootfs"] + ext[0][1] * fs.bs
        out.seek(pos)
        assert out.read(len(old)) == old, "on-disk bytes don't match original"
        out.seek(pos)
        out.write(new)
        print("patched %s at byte offset %d (%d bytes)" % (path, pos, len(new)))

fs = Ext4(img, PARTS["rootfs"])
for path, local in pairs:
    assert fs.read(fs.lookup(path)) == open(local, "rb").read()
    print("verified", path)
