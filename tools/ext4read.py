"""Minimal read-only ext2/3/4 reader for disk images. Never writes to the image."""
import struct, sys, os, time, stat as st

IMG = os.path.join(os.path.dirname(__file__), "..", "g36-original-BROKEN-backup.img")
PARTS = {"rootfs": 65536 * 512, "data": 1114112 * 512}


class Ext4:
    def __init__(self, path, offset):
        self.f = open(path, "rb")
        self.off = offset
        sb = self.raw(1024, 1024)
        self.sb = sb
        (self.inodes_count, blocks_lo, _, free_blocks_lo, self.free_inodes,
         self.first_data_block, log_bs) = struct.unpack_from("<IIIIIII", sb, 0)
        self.bs = 1024 << log_bs
        self.blocks_per_group, = struct.unpack_from("<I", sb, 32)
        self.inodes_per_group, = struct.unpack_from("<I", sb, 40)
        self.mtime, self.wtime, self.mnt_count, self.max_mnt, magic, self.state, self.errors = \
            struct.unpack_from("<IIHhHHH", sb, 44)
        assert magic == 0xEF53, "not ext"
        self.rev, = struct.unpack_from("<I", sb, 76)
        self.inode_size = struct.unpack_from("<H", sb, 88)[0] if self.rev else 128
        self.feat_compat, self.feat_incompat, self.feat_ro = struct.unpack_from("<III", sb, 92)
        self.label = sb[120:136].rstrip(b"\0").decode(errors="replace")
        self.last_mounted = sb[136:200].rstrip(b"\0").decode(errors="replace")
        self.is64 = bool(self.feat_incompat & 0x80)
        self.desc_size = struct.unpack_from("<H", sb, 254)[0] if self.is64 else 32
        if self.desc_size == 0:
            self.desc_size = 32
        blocks_hi = struct.unpack_from("<I", sb, 0x150)[0] if self.is64 else 0
        free_hi = struct.unpack_from("<I", sb, 0x158)[0] if self.is64 else 0
        self.blocks = blocks_lo | (blocks_hi << 32)
        self.free_blocks = free_blocks_lo | (free_hi << 32)
        self.error_count, self.first_error_time = struct.unpack_from("<II", sb, 0x194)
        self.first_error_func = sb[0x1A0:0x1C0].rstrip(b"\0").decode(errors="replace")
        self.first_error_ino, self.first_error_block = struct.unpack_from("<II", sb, 0x198)
        self.last_error_time, self.last_error_ino = struct.unpack_from("<II", sb, 0x1CC)
        self.last_error_func = sb[0x1E0:0x200].rstrip(b"\0").decode(errors="replace")
        self.gdt_block = self.first_data_block + 1

    def raw(self, pos, n):
        self.f.seek(self.off + pos)
        return self.f.read(n)

    def block(self, b, n=1):
        return self.raw(b * self.bs, self.bs * n)

    def inode(self, ino):
        g, idx = divmod(ino - 1, self.inodes_per_group)
        d = self.raw(self.gdt_block * self.bs + g * self.desc_size, self.desc_size)
        tbl = struct.unpack_from("<I", d, 8)[0]
        if self.desc_size >= 64:
            tbl |= struct.unpack_from("<I", d, 0x28)[0] << 32
        return self.raw(tbl * self.bs + idx * self.inode_size, self.inode_size)

    def info(self, ino):
        i = self.inode(ino)
        mode, uid, size_lo, atime, ctime, mtime, dtime, gid, links = struct.unpack_from("<HHIIIIIHH", i, 0)
        flags, = struct.unpack_from("<I", i, 32)
        size_hi, = struct.unpack_from("<I", i, 108)
        return dict(mode=mode, size=size_lo | (size_hi << 32), mtime=mtime, flags=flags,
                    links=links, dtime=dtime, raw=i)

    def _extent_blocks(self, node, out):
        magic, entries, _, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            raise IOError("bad extent header")
        for k in range(entries):
            e = node[12 + 12 * k: 24 + 12 * k]
            if depth == 0:
                lblk, ln, hi, lo = struct.unpack("<IHHI", e)
                uninit = ln > 32768
                if uninit:
                    ln -= 32768
                out.append((lblk, (hi << 32) | lo, ln, uninit))
            else:
                lblk, lo, hi, _ = struct.unpack("<IIHH", e)
                self._extent_blocks(self.block((hi << 32) | lo), out)

    def _indirect(self, ptr, level, out):
        if ptr == 0:
            out.extend([0] * ((self.bs // 4) ** level if level else 1))
            return
        if level == 0:
            out.append(ptr)
            return
        for p in struct.unpack("<%dI" % (self.bs // 4), self.block(ptr)):
            self._indirect(p, level - 1, out)

    def read(self, ino, limit=None):
        nfo = self.info(ino)
        size = nfo["size"] if limit is None else min(limit, nfo["size"])
        i = nfo["raw"]
        if nfo["flags"] & 0x10000000:  # inline data
            return i[40:100][:size]
        if st.S_ISLNK(nfo["mode"]) and size < 60 and not (nfo["flags"] & 0x80000):
            return i[40:40 + size]
        buf = bytearray()
        if nfo["flags"] & 0x80000:  # extents
            ext = []
            self._extent_blocks(i[40:100], ext)
            ext.sort()
            for lblk, pblk, ln, uninit in ext:
                want = lblk * self.bs
                if want >= size:
                    break
                if len(buf) < want:
                    buf.extend(b"\0" * (want - len(buf)))
                n = min(ln, (size - want + self.bs - 1) // self.bs)
                buf.extend(b"\0" * (n * self.bs) if uninit else self.block(pblk, n))
        else:
            ptrs = struct.unpack_from("<15I", i, 40)
            blocks = list(ptrs[:12])
            for lvl, p in ((1, ptrs[12]), (2, ptrs[13]), (3, ptrs[14])):
                if len(blocks) * self.bs >= size:
                    break
                self._indirect(p, lvl, blocks)
            for b in blocks:
                if len(buf) >= size:
                    break
                buf.extend(self.block(b) if b else b"\0" * self.bs)
        if len(buf) < size:
            buf.extend(b"\0" * (size - len(buf)))
        return bytes(buf[:size])

    def listdir(self, ino):
        data = self.read(ino)
        out, p = [], 0
        while p + 8 <= len(data):
            child, rec_len, name_len, ftype = struct.unpack_from("<IHBB", data, p)
            if rec_len < 8:
                break
            if child and name_len:
                name = data[p + 8:p + 8 + name_len].decode(errors="replace")
                if name not in (".", ".."):
                    out.append((name, child, ftype))
            p += rec_len
        return out

    def lookup(self, path, follow=True, depth=0):
        ino = 2
        parts = [x for x in path.split("/") if x]
        for n, name in enumerate(parts):
            hit = [c for nm, c, _ in self.listdir(ino) if nm == name]
            if not hit:
                raise FileNotFoundError(path)
            ino = hit[0]
            if st.S_ISLNK(self.info(ino)["mode"]) and (follow or n < len(parts) - 1) and depth < 20:
                tgt = self.read(ino).decode()
                base = "/" + "/".join(parts[:n]) if not tgt.startswith("/") else ""
                rest = "/".join(parts[n + 1:])
                return self.lookup(base + "/" + tgt + ("/" + rest if rest else ""), follow, depth + 1)
        return ino

    def walk(self, ino=2, prefix=""):
        for name, child, ftype in sorted(self.listdir(ino)):
            path = prefix + "/" + name
            yield path, child
            if ftype == 2:
                yield from self.walk(child, path)


def fmt_time(t):
    return time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t)) if t else "-"


def open_part(name):
    return Ext4(IMG, PARTS[name])


if __name__ == "__main__":
    cmd, part = sys.argv[1], sys.argv[2]
    fs = open_part(part)
    if cmd == "sb":
        for k in ("label", "last_mounted", "blocks", "free_blocks", "free_inodes", "inodes_count", "bs",
                  "state", "errors", "mnt_count", "error_count", "first_error_func", "first_error_ino",
                  "first_error_block", "last_error_func", "last_error_ino"):
            print(k, getattr(fs, k))
        print("mtime", fmt_time(fs.mtime), "wtime", fmt_time(fs.wtime))
        print("first_error_time", fmt_time(fs.first_error_time), "last_error_time", fmt_time(fs.last_error_time))
        print("features compat=%x incompat=%x ro=%x" % (fs.feat_compat, fs.feat_incompat, fs.feat_ro))
    elif cmd == "ls":
        ino = fs.lookup(sys.argv[3] if len(sys.argv) > 3 else "/", follow=True)
        for name, child, ftype in sorted(fs.listdir(ino)):
            nfo = fs.info(child)
            extra = ""
            if st.S_ISLNK(nfo["mode"]):
                extra = " -> " + fs.read(child).decode(errors="replace")
            print("%s %10d %s %s%s" % (st.filemode(nfo["mode"]), nfo["size"], fmt_time(nfo["mtime"]), name, extra))
    elif cmd == "cat":
        sys.stdout.buffer.write(fs.read(fs.lookup(sys.argv[3])))
    elif cmd == "get":
        open(sys.argv[4], "wb").write(fs.read(fs.lookup(sys.argv[3])))
    elif cmd == "find":
        for path, child in fs.walk():
            nfo = fs.info(child)
            print("%s %10d %s %s" % (st.filemode(nfo["mode"]), nfo["size"], fmt_time(nfo["mtime"]), path))
