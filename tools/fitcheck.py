"""Parse a FIT (flattened image tree) image inside the disk image and verify each component's hash."""
import struct, hashlib, sys, os, zlib

IMG = os.path.join(os.path.dirname(__file__), "..", "g36-original-BROKEN-backup.img")


def parse_fdt(blob):
    magic, totalsize, off_struct, off_strings, _, ver, _, _, _, size_struct = struct.unpack_from(">10I", blob, 0)
    assert magic == 0xD00DFEED
    strings = blob[off_strings:]
    p = off_struct
    root = {"props": {}, "children": {}}
    stack = []
    node = None
    while True:
        tok, = struct.unpack_from(">I", blob, p); p += 4
        if tok == 1:  # BEGIN_NODE
            end = blob.index(b"\0", p)
            name = blob[p:end].decode()
            p = (end + 4) & ~3
            new = {"props": {}, "children": {}}
            if node is None:
                node = root
            else:
                node["children"][name] = new
                stack.append(node)
                node = new
        elif tok == 2:  # END_NODE
            if not stack:
                break
            node = stack.pop()
        elif tok == 3:  # PROP
            ln, nameoff = struct.unpack_from(">II", blob, p); p += 8
            pname = strings[nameoff:strings.index(b"\0", nameoff)].decode()
            node["props"][pname] = blob[p:p + ln]
            p = (p + ln + 3) & ~3
        elif tok == 4:
            continue
        elif tok == 9:
            break
    return root, totalsize


def s(v):
    return v.rstrip(b"\0").decode(errors="replace")


def u32(v):
    return struct.unpack(">I", v[:4])[0]


def check(lba, label):
    f = open(IMG, "rb")
    base = lba * 512
    f.seek(base)
    hdr = f.read(64)
    totalsize = struct.unpack_from(">I", hdr, 4)[0]
    f.seek(base)
    blob = f.read(totalsize)
    root, _ = parse_fdt(blob)
    print("=====", label, "FIT header size", totalsize)
    for k, v in root["props"].items():
        if k in ("description", "timestamp"):
            print(" ", k, s(v) if k != "timestamp" else u32(v))
    images = root["children"].get("images", {"children": {}})["children"]
    ok_all = True
    for name, img in images.items():
        pr = img["props"]
        if "data" in pr:
            data = pr["data"]
        elif "data-position" in pr:
            f.seek(base + u32(pr["data-position"])); data = f.read(u32(pr["data-size"]))
        elif "data-offset" in pr:
            off = (totalsize + 3) & ~3
            f.seek(base + off + u32(pr["data-offset"])); data = f.read(u32(pr["data-size"]))
        else:
            data = b""
        desc = s(pr.get("description", b""))
        line = "  %-12s %-10s %-10s %8d bytes  %s" % (name, s(pr.get("type", b"")), s(pr.get("compression", b"")), len(data), desc)
        results = []
        for hn, h in img["children"].items():
            if not hn.startswith("hash"):
                continue
            algo = s(h["props"].get("algo", b""))
            want = h["props"].get("value", b"")
            if algo.startswith("sha256"):
                got = hashlib.sha256(data).digest()
            elif algo.startswith("sha1"):
                got = hashlib.sha1(data).digest()
            elif algo == "crc32":
                got = struct.pack(">I", zlib.crc32(data) & 0xFFFFFFFF)
            elif algo == "md5":
                got = hashlib.md5(data).digest()
            else:
                results.append(algo + ":?"); continue
            good = got == want
            ok_all &= good
            results.append("%s:%s" % (algo, "OK" if good else "MISMATCH"))
        print(line, " ".join(results) or "(no hash)")
        if len(sys.argv) > 1 and sys.argv[1] == "extract":
            out = os.path.join(os.path.dirname(__file__), "..", "extracted", "%s_%s.bin" % (label, name))
            open(out, "wb").write(data)
        if s(pr.get("type", b"")) == "multi" and data[:4] == b"RSCE":
            tbl, eb, n = data[8], data[9], struct.unpack_from("<I", data, 12)[0]
            for k in range(n):
                e = (tbl + k * eb) * 512
                rname = data[e + 4:e + 224].split(b"\0")[0].decode()
                hsz, roff, rsize = struct.unpack_from("<III", data, e + 256)
                good = data[e + 224:e + 224 + hsz] == hashlib.sha1(data[roff * 512:roff * 512 + rsize]).digest()
                ok_all &= good
                print("      resource entry %-16s sha1:%s" % (rname, "OK" if good else "MISMATCH"))
    print("  ALL HASHES OK" if ok_all else "  *** HASH FAILURES ***")
    return root


if __name__ == "__main__":
    check(8192, "uboot")
    check(16384, "boot")
