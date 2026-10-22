"""Put a new boot splash into a copy of the G36 boot partition (p2).

    python3 splash_install.py <boot-p2.img> <splash.bmp> <out.img>

The splash lives twice in the FIT's resource image (RSCE): logo.bmp (shown by U-Boot)
and logo_kernel.bmp (shown while the kernel starts). Both are replaced. Each resource
entry carries its own SHA-1, and the FIT header the resource's SHA-256; U-Boot checks
both, so both are updated. The new BMP must be the same size as the old one (480x854,
24-bit), so nothing moves.
"""
import hashlib, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import fitcheck
from fitcheck import parse_fdt

src, bmp_path, out = sys.argv[1:4]
assert os.path.abspath(src) != os.path.abspath(out), "write to a new file"
part = bytearray(open(src, "rb").read())
bmp = open(bmp_path, "rb").read()
assert bmp[:2] == b"BM"

fit, total = parse_fdt(bytes(part[:65536]))
imgs = fit["children"]["images"]["children"]
res_pos = struct.unpack(">I", imgs["resource"]["props"]["data-position"])[0]
res_size = struct.unpack(">I", imgs["resource"]["props"]["data-size"])[0]
res = bytearray(part[res_pos:res_pos + res_size])
old_res_hash = imgs["resource"]["children"]["hash"]["props"]["value"]
assert hashlib.sha256(res).digest() == old_res_hash, "resource already changed?"
assert res[:4] == b"RSCE"

tbl, eb, n = res[8], res[9], struct.unpack_from("<I", res, 12)[0]
done = []
for k in range(n):
    e = (tbl + k * eb) * 512
    name = res[e + 4:e + 224].split(b"\0")[0].decode()
    hsz, off, size = struct.unpack_from("<III", res, e + 256)
    if name not in ("logo.bmp", "logo_kernel.bmp"):
        continue
    assert size == len(bmp), "%s is %d bytes, the new splash %d" % (name, size, len(bmp))
    assert hsz == 20 and res[e + 224:e + 244] == hashlib.sha1(res[off * 512:off * 512 + size]).digest()
    res[off * 512:off * 512 + size] = bmp
    res[e + 224:e + 244] = hashlib.sha1(bmp).digest()
    done.append(name)
assert sorted(done) == ["logo.bmp", "logo_kernel.bmp"], done

# the FIT header: the resource's new SHA-256 (it appears once)
new_hash = hashlib.sha256(res).digest()
hdr = part[:total]
assert hdr.count(old_res_hash) == 1
i = hdr.index(old_res_hash)
part[i:i + 32] = new_hash
part[res_pos:res_pos + res_size] = res
open(out, "wb").write(part)
print("replaced", ", ".join(done))

fitcheck.IMG = out
fitcheck.check(0, "boot")
