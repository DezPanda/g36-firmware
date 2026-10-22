"""Walk an ext4 (jbd2, csum v3) journal the way the kernel's recovery scan does and verify every checksum."""
import sys, os, struct
sys.path.insert(0, os.path.dirname(__file__))
from ext4read import Ext4, PARTS, IMG

_T = []
for i in range(256):
    c = i
    for _ in range(8):
        c = (c >> 1) ^ 0x82F63B78 if c & 1 else c >> 1
    _T.append(c)


def crc32c(seed, data):
    c = seed
    for b in data:
        c = _T[(c ^ b) & 0xFF] ^ (c >> 8)
    return c


MAGIC = 0xC03B3998
DESC, COMMIT, SB1, SB2, REVOKE = 1, 2, 3, 4, 5


def scan(fs, verbose=True):
    j_ino, = struct.unpack_from("<I", fs.sb, 0xE0)
    jdata = fs.read(j_ino)
    bs = 4096
    jsb = jdata[:1024]
    maxlen, first, sequence, start = struct.unpack_from(">IIII", jsb, 16)
    incompat, = struct.unpack_from(">I", jsb, 0x28)
    has64 = bool(incompat & 0x2)
    uuid = jsb[0x30:0x40]
    seed = crc32c(0xFFFFFFFF, uuid)
    sb_ck = jsb[:0xFC] + b"\0\0\0\0" + jsb[0x100:1024]
    print("journal sb checksum:", "OK" if crc32c(0xFFFFFFFF, sb_ck) == struct.unpack_from(">I", jsb, 0xFC)[0] else "BAD")
    if start == 0:
        print("journal is empty (start=0); nothing to replay")
        return []
    blk, seq = start, sequence
    txns, cur = [], None
    problems = []

    def nxt(b):
        b += 1
        return first if b >= maxlen else b

    def block(b):
        return jdata[b * bs:(b + 1) * bs]

    steps = 0
    while steps < maxlen:
        steps += 1
        b = block(blk)
        magic, btype, bseq = struct.unpack_from(">III", b, 0)
        if magic != MAGIC or bseq != seq:
            print("end of log at journal block %d (magic=%x type=%d seq=%d, expected seq %d)" % (blk, magic, btype, bseq, seq))
            break
        if cur is None:
            cur = {"seq": seq, "blocks": [], "revokes": 0, "bad_data": [], "start": blk}
        if btype == DESC:
            tail_ok = crc32c(seed, b[:-4] + b"\0\0\0\0") == struct.unpack_from(">I", b, bs - 4)[0]
            if not tail_ok:
                problems.append("txn %d: descriptor block %d checksum BAD" % (seq, blk))
            p = 12
            while p + 16 <= bs - 4:
                lo, flags, hi, tck = struct.unpack_from(">IIII", b, p)
                p += 16
                if not flags & 2:
                    p += 16
                blk = nxt(blk)
                data = block(blk)
                if flags & 1:  # escaped: first 4 bytes were the magic
                    data = struct.pack(">I", MAGIC) + data[4:]
                want = crc32c(crc32c(seed, struct.pack(">I", seq)), data)
                target = lo | ((hi << 32) if has64 else 0)
                cur["blocks"].append(target)
                cur.setdefault("tags", []).append((target, blk, bool(flags & 1), want == tck))
                if want != tck:
                    cur["bad_data"].append((blk, target))
                if flags & 8:
                    break
            blk = nxt(blk)
        elif btype == REVOKE:
            ok = crc32c(seed, b[:-4] + b"\0\0\0\0") == struct.unpack_from(">I", b, bs - 4)[0]
            if not ok:
                problems.append("txn %d: revoke block %d checksum BAD" % (seq, blk))
            cur["revokes"] += (struct.unpack_from(">I", b, 12)[0] - 16) // (8 if has64 else 4)
            blk = nxt(blk)
        elif btype == COMMIT:
            stored, = struct.unpack_from(">I", b, 16)
            calc = crc32c(seed, b[:16] + b"\0\0\0\0" + b[20:])
            cur["commit_ok"] = stored == calc
            cur["commit_blk"] = blk
            txns.append(cur)
            if verbose:
                print("txn %5d: %3d blocks, %d revokes, commit %s, bad data blocks: %d" % (
                    seq, len(cur["blocks"]), cur["revokes"], "OK" if cur["commit_ok"] else "BAD", len(cur["bad_data"])))
            cur = None
            seq += 1
            blk = nxt(blk)
        else:
            problems.append("txn %d: unexpected block type %d at %d" % (seq, btype, blk))
            break
    if cur is not None:
        print("incomplete transaction %d (no commit) - kernel ignores it" % cur["seq"])
    for t in txns:
        for jb, target in t["bad_data"]:
            problems.append("txn %d: data block (journal blk %d -> fs block %d) checksum BAD" % (t["seq"], jb, target))
        if not t["commit_ok"]:
            problems.append("txn %d: commit block checksum BAD" % t["seq"])
    print("transactions to replay: %d; problems: %d" % (len(txns), len(problems)))
    for p in problems[:50]:
        print("  ", p)
    scan.info = dict(jdata=jdata, j_ino=j_ino, maxlen=maxlen, first=first, last_seq=txns[-1]["seq"] if txns else None)
    return txns


if __name__ == "__main__":
    img = sys.argv[2] if len(sys.argv) > 2 else IMG
    scan(Ext4(img, PARTS[sys.argv[1]]))
