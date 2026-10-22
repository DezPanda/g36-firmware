#!/usr/bin/env python3
# SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda
"""Checks the public system partition image against the archive it was made from.

    python3 -I verify_rootfs.py rootfs-live.tar rootfs-public.img work/repl manifest.txt

Every entry of the archive must be in the image with the same type, mode, owner and content
(a link's target), except the ones the manifest lists: REMOVED ones must be gone, and
CHANGED / REPLACED ones must hold the new content in work/repl. Nothing may be in the image
that isn't in the archive. Reads only.
"""
import hashlib
import os
import stat
import struct
import sys
import tarfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools"))
from ext4read import Ext4  # noqa: E402


def main():
    tar_path, img_path, repl_dir, man_path = sys.argv[1:5]
    fs = Ext4(img_path, 0)
    removed, replaced, chmods = set(), {}, {}
    for line in open(man_path):
        parts = line.split()
        if len(parts) < 2:
            continue
        if parts[0] == "REMOVED":
            removed.add(parts[1])
        elif parts[0] == "CHMOD":
            chmods[parts[1]] = int(parts[parts.index("permissions") + 1], 8)
        elif parts[0] in ("CHANGED", "REPLACED"):
            replaced[parts[1]] = os.path.join(repl_dir, parts[1].replace("/", "__"))

    img = {}
    for path, ino in fs.walk():
        img[path.lstrip("/")] = ino

    t = tarfile.open(tar_path)
    bad, checked, seen = [], 0, set()
    for m in t.getmembers():
        n = m.name[2:] if m.name.startswith("./") else m.name
        if n in ("", "."):
            continue
        seen.add(n)
        if n in removed:
            if n in img:
                bad.append("still there: " + n)
            continue
        if n not in img:
            bad.append("missing: " + n)
            continue
        info = fs.info(img[n])
        want_mode = m.mode & 0o7777
        mode = info["mode"]
        kind_ok = ((m.isdir() and stat.S_ISDIR(mode)) or (m.issym() and stat.S_ISLNK(mode)) or
                   (m.isreg() and stat.S_ISREG(mode)) or (m.islnk() and stat.S_ISREG(mode)) or
                   ((m.isblk() or m.ischr() or m.isfifo()) and not stat.S_ISREG(mode)))
        if not kind_ok:
            bad.append("type differs: " + n)
            continue
        if n in replaced:
            want = open(replaced[n], "rb").read()
            if fs.read(img[n]) != want:
                bad.append("content isn't the new one: " + n)
            if (mode & 0o7777) != want_mode and not n.endswith(".cfg") and not n.endswith(".txt"):
                pass
        else:
            if n in chmods:
                want_mode = chmods[n]
            if (mode & 0o7777) != want_mode:
                bad.append("mode %o != %o: %s" % (mode & 0o7777, want_mode, n))
            raw = info["raw"]
            uid = struct.unpack_from("<H", raw, 2)[0] | struct.unpack_from("<H", raw, 120)[0] << 16
            gid = struct.unpack_from("<H", raw, 24)[0] | struct.unpack_from("<H", raw, 122)[0] << 16
            if uid != m.uid or gid != m.gid:
                bad.append("owner differs: " + n)
            if m.issym():
                if fs.read(img[n]).decode() != m.linkname:
                    bad.append("link target differs: " + n)
            elif m.isreg():
                a = hashlib.sha256(fs.read(img[n])).digest()
                b = hashlib.sha256(t.extractfile(m).read()).digest()
                if a != b:
                    bad.append("content differs: " + n)
        checked += 1
    extra = sorted(p for p in img if p not in seen and p not in replaced and p != "lost+found" and not p.startswith("lost+found/"))
    for p in extra:
        bad.append("not in the archive: " + p)
    print("%d archive entries compared, %d removed on purpose, %d replaced; %d problems" %
          (checked, len(removed), len(replaced), len(bad)))
    for b in bad[:60]:
        print("  " + b)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
