"""Kernel device-tree test changes for SD slot problems, written into the boot partition of a test image.

- sdmmc: ignore the card-detect switch (non-removable), drop high-speed mode, cap clock at 25 MHz
- bootargs: panic=10 so a kernel crash reboots instead of freezing
"""
import sys, os, struct, hashlib
sys.path.insert(0, os.path.dirname(__file__))
import fitcheck
from fitcheck import parse_fdt, s
from dtb_edit import encode

img = sys.argv[1]
opts = set(sys.argv[2:])  # any of: panic, sdslow, nocd, fakeroot
assert "backup" not in os.path.basename(img), "refusing to modify the backup image"
BOOT = 16384 * 512

f = open(img, "r+b")
f.seek(BOOT)
fit_hdr = f.read(1536)
fit, _ = parse_fdt(fit_hdr)
imgs = fit["children"]["images"]["children"]
fdt_pos = struct.unpack(">I", imgs["fdt"]["props"]["data-position"])[0]
fdt_size = struct.unpack(">I", imgs["fdt"]["props"]["data-size"])[0]
kern_pos = struct.unpack(">I", imgs["kernel"]["props"]["data-position"])[0]
res_pos = struct.unpack(">I", imgs["resource"]["props"]["data-position"])[0]
res_size = struct.unpack(">I", imgs["resource"]["props"]["data-size"])[0]

f.seek(BOOT + fdt_pos)
dtb = f.read(fdt_size)
assert hashlib.sha256(dtb).digest() == imgs["fdt"]["children"]["hash"]["props"]["value"], "fdt already modified?"

tree, _ = parse_fdt(dtb)
chosen = tree["children"]["chosen"]["props"]
args = s(chosen["bootargs"])
if "panic" in opts:
    assert "panic=" not in args
    chosen["bootargs"] = (args + " panic=10").encode() + b"\0"
if "fakeroot" in opts:
    # point root at a partition that doesn't exist: a crash before root lookup still reboots, a mount failure can't happen
    cur = s(chosen["bootargs"])
    assert "root=/dev/mmcblk0p3 " in cur
    chosen["bootargs"] = cur.replace("root=/dev/mmcblk0p3 ", "root=/dev/mmcblk0p9 ").encode() + b"\0"
if "sdslow" in opts:
    mmc = tree["children"]["mmc@ff480000"]["props"]
    del mmc["cap-sd-highspeed"]
    mmc["max-frequency"] = struct.pack(">I", 25000000)
if "nocd" in opts:
    mmc = tree["children"]["mmc@ff480000"]["props"]
    del mmc["cd-gpios"]
    mmc["non-removable"] = b""
new_dtb = encode(tree, dtb)
print("dtb %d -> %d bytes; bootargs: %s" % (len(dtb), len(new_dtb), s(chosen["bootargs"])))
assert fdt_pos + len(new_dtb) <= kern_pos

# resource image also carries a copy of the kernel dtb (rk-kernel.dtb); replace it too
f.seek(BOOT + res_pos)
res = bytearray(f.read(res_size))
assert res[:4] == b"RSCE"
tbl_off, ent_blks, n = res[8], res[9], struct.unpack_from("<I", res, 12)[0]
for i in range(n):
    e = (tbl_off + i * ent_blks) * 512
    name = res[e + 4:e + 224].split(b"\0")[0].decode()
    hash_size, off, size = struct.unpack_from("<III", res, e + 256)
    if name == "rk-kernel.dtb":
        assert bytes(res[off * 512:off * 512 + size]) == dtb
        nxt = min(struct.unpack_from("<I", res, (tbl_off + j * ent_blks) * 512 + 260)[0]
                  for j in range(n) if struct.unpack_from("<I", res, (tbl_off + j * ent_blks) * 512 + 260)[0] > off)
        assert off * 512 + len(new_dtb) <= nxt * 512
        res[off * 512:nxt * 512] = new_dtb + b"\0" * (nxt * 512 - off * 512 - len(new_dtb))
        struct.pack_into("<I", res, e + 264, len(new_dtb))
        # each resource entry carries its own SHA-1 that U-Boot checks ("Invalid DTB hash !")
        assert hash_size == 20 and bytes(res[e + 224:e + 244]) == hashlib.sha1(dtb).digest()
        res[e + 224:e + 244] = hashlib.sha1(new_dtb).digest()
        print("resource rk-kernel.dtb replaced")

# update FIT header: fdt size + hashes
new_hdr = bytearray(fit_hdr)
def rep(old_v, new_v):
    assert new_hdr.count(old_v) == 1, "value not unique"
    i = new_hdr.index(old_v); new_hdr[i:i + len(old_v)] = new_v
rep(imgs["fdt"]["children"]["hash"]["props"]["value"], hashlib.sha256(new_dtb).digest())
rep(imgs["resource"]["children"]["hash"]["props"]["value"], hashlib.sha256(bytes(res)).digest())
if len(new_dtb) != fdt_size:
    rep(struct.pack(">I", fdt_size), struct.pack(">I", len(new_dtb)))
new_hdr = bytes(new_hdr)

f.seek(BOOT); f.write(new_hdr)
f.seek(BOOT + fdt_pos); f.write(new_dtb + b"\0" * (kern_pos - fdt_pos - len(new_dtb)))
f.seek(BOOT + res_pos); f.write(bytes(res))
f.close()

fitcheck.IMG = img
fitcheck.check(16384, "boot")
