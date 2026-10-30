#!/usr/bin/env python3
# SPDX-License-Identifier: MIT -- Copyright (c) 2026 DezPanda
"""Makes the public system partition's contents from an archive of the live one.

    python3 build_public_rootfs.py rootfs-live.tar OUTDIR manifest.txt --repo-url=https://...

rootfs-live.tar: `tar -cpf` of the handheld's system partition, taken through a bind mount of /
(so it holds only what's on the partition); it's only read to find what to change. The partition is
built on the handheld with `mke2fs -d` from the same bind mount, after OUTDIR/apply.sh has taken
the personal and leftover files out of the live system and put the starter files in (it backs
up everything it touches first; OUTDIR/restore.sh puts back the saved volume and time).
manifest.txt lists everything removed, replaced or changed.
"""
import io
import os
import re
import sys
import tarfile

HERE = os.path.dirname(os.path.abspath(__file__))
TEMPLATE = os.path.join(HERE, "..", "patch", "template")

# removed outright (exact paths, or a regex on the path)
REMOVE_EXACT = {
    "etc/g36-clock":                       "the saved time (the clock then starts at its 1 Jan 2026 floor)",
    "etc/init.d/.S05growdata.old":         "an old boot script",
    "growdata.log":                        "a log of this card's partition sizes",
    "usr/bin/retroarch":                   "the old factory RetroArch (replaced by retroarch-1.22)",
    "usr/bin/retroarch_r.stock":           "the old factory RetroArch (same file)",
    "usr/bin/g36d.prev":                   "an earlier build of g36d",
    "usr/bin/g36menu.prev":                "an earlier build of the menu",
    "oem/release/config/usrrun.sh.prev":   "an earlier startup script",
    "oem/release/config/usrrun.sh.pre-nofallback": "an earlier startup script",
    # the vendor's closed-source programs and the build of FFmpeg that can't be redistributed
    "oem/release/bin/vol_adj":             "the vendor's volume helper (closed source; g36d does its job now)",
    "oem/release/bin/libawtk.so":          "the vendor's UI library, only the volume helper used it",
    "oem/release/config/ledcontrol.sh":    "the vendor's LED script (calls the volume helper; the handheld has no LEDs)",
    "usr/bin/game_play":                   "the vendor's game launcher (closed source, unused)",
    "usr/bin/mediaplay":                   "the vendor's video player (closed source, unused)",
    "usr/bin/ffmpeg":                      "FFmpeg command line from the vendor's GPL + non-free build (not redistributable)",
    "usr/bin/ffplay":                      "FFmpeg player from the vendor's GPL + non-free build (not redistributable)",
    "usr/bin/ffprobe":                     "FFmpeg probe from the vendor's GPL + non-free build (not redistributable)",
}
REMOVE_RE = [
    # libraries only the vendor's FFmpeg build used (the menu's own LGPL build replaces the six it needs)
    (r"^usr/lib/lib(avdevice|avresample|postproc|x264|x265|dav1d)\.so", "a library of the vendor's GPL + non-free FFmpeg build (not needed by the LGPL one)"),
    (r"\.stock$",       "a backup copy of a factory file"),
    (r"\.pre-[\w-]+$",  "a backup copy from before one of our changes"),
    (r"\.prev$",        "an earlier version"),
]

# replaced or added in the starter template (the first boot copies it onto the games drive)
TEMPLATE_FILES = ["README.txt", "CREDITS.txt", "SOURCE.txt", "LICENSE.txt", os.path.join("g36", "settings.cfg")]
T = "usr/share/g36/template/"

# permissions set right (they were world-writable)
CHMODS = {"usr/bin/g36-key": 0o755}

# the saved volume and brightness (the file's quirks are kept, vol_adj and g36d edit it by hand)
GAMEBOX_JSON = "oem/release/config/gamebox.json"

# the storage script is taken whole from the repo (patch/S05g36storage), not patched into the
# live one: it holds the starter-model copy (once, atomic) and the guard on formatting the games drive
STORAGE = "etc/init.d/S05g36storage"
STORAGE_SRC = os.path.join(HERE, "..", "patch", "S05g36storage")

# the menu is the public build (compiled with -DG36_PUBLIC), made by build/g36/menu/build.sh and copied here
MENU = "usr/bin/g36menu"
MENU_SRC = os.path.join(HERE, "g36menu-public")


REPO_URL = ""


def clean(n):
    return n[2:] if n.startswith("./") else n


def main():
    """Writes apply.sh, restore.sh and repl/ into OUTDIR. apply.sh makes the changes on the live
    handheld (after copying every file it touches to live-backup/ on the card); restore.sh puts
    back the two personal files (the saved volume and the saved time)."""
    global REPO_URL
    args = [a for a in sys.argv[1:] if not a.startswith("--repo-url=")]
    for a in sys.argv[1:]:
        if a.startswith("--repo-url="):
            REPO_URL = a.split("=", 1)[1].rstrip("/")
    src, outdir, man = args[0:3]
    os.makedirs(os.path.join(outdir, "repl"), exist_ok=True)
    t_in = tarfile.open(src)
    removed, installs, lines = [], [], []

    def repl(path, data, mode):
        with open(os.path.join(outdir, "repl", path.replace("/", "__")), "wb") as f:
            f.write(data)
        installs.append((path, mode))

    for m in t_in.getmembers():
        n = clean(m.name)
        if n in ("", "."):
            continue
        why = REMOVE_EXACT.get(n)
        if not why:
            for rx, w in REMOVE_RE:
                if re.search(rx, n):
                    why = w
                    break
        if why:
            lines.append("REMOVED   %-60s %9d  %s" % (n, m.size, why))
            removed.append(n)
        elif n == GAMEBOX_JSON:
            text = t_in.extractfile(m).read().decode()
            new = re.sub(r'("volume" *: *)\d+', r'\g<1>50', text)
            new = re.sub(r'("back_light" *: *)\d+', r'\g<1>50', new)
            repl(n, new.encode(), m.mode & 0o7777)
            lines.append("CHANGED   %-60s %9d  volume and brightness back to 50" % (n, len(new)))
        elif n == MENU:
            new = open(MENU_SRC, "rb").read()
            repl(n, new, m.mode & 0o7777)
            lines.append("REPLACED  %-60s %9d  the public build of the menu" % (n, len(new)))
        elif n == STORAGE:
            new = open(STORAGE_SRC, "rb").read()
            assert b".starter-copied" in new and b"blank()" in new, "patch/S05g36storage is the old one"
            repl(n, new, m.mode & 0o7777)
            lines.append("REPLACED  %-60s %9d  the repo's storage script: starter models copied once, "
                         "and the games drive is formatted only when new or empty" % (n, len(new)))
    for n, mode in CHMODS.items():
        lines.append("CHMOD     %-60s %9s  permissions %o (were world-writable)" % (n, "", mode))
    for rel in TEMPLATE_FILES:
        data = open(os.path.join(TEMPLATE, rel), "rb").read()
        if b"@REPO_URL@" in data:
            if not REPO_URL:
                sys.exit("%s names @REPO_URL@: pass --repo-url=https://... (where the source is published)" % rel)
            data = data.replace(b"@REPO_URL@", REPO_URL.encode())
        repl(T + rel.replace(os.sep, "/"), data, 0o644)
        lines.append("REPLACED  %-60s %9d  the current starter file" % (T + rel.replace(os.sep, "/"), len(data)))

    W = "/mnt/sdcard/release-work"
    touched = removed + [p for p, _ in installs] + ["etc/g36-clock"]
    a = ["#!/bin/sh", "# Makes the live handheld match the public image (backups first). Generated.", "set -e",
         "B=%s/live-backup" % W, "mkdir -p $B"]
    for p in sorted(set(touched)):
        a.append('[ -e "/%s" ] && { mkdir -p "$B/$(dirname "%s")"; cp "/%s" "$B/%s"; }' % (p, p, p, p))
    a.append("sync")
    for p in removed:
        a.append('rm -f "/%s"' % p)
    a.append("rm -f /etc/g36-clock")
    for p, mode in installs:
        a.append('cp "%s/repl/%s" "/%s" && chmod %o "/%s" && chown 0:0 "/%s"' % (W, p.replace("/", "__"), p, mode, p, p))
    for n, mode in CHMODS.items():
        a.append('chmod %o "/%s"' % (mode, n))
    a.append("sync")
    open(os.path.join(outdir, "apply.sh"), "w").write("\n".join(a) + "\n")
    r = ["#!/bin/sh", "# Puts back the personal files apply.sh set aside. Generated.",
         "B=%s/live-backup" % W,
         'cp "$B/%s" "/%s"' % (GAMEBOX_JSON, GAMEBOX_JSON),
         '[ -e "$B/etc/g36-clock" ] && cp "$B/etc/g36-clock" /etc/g36-clock', "sync"]
    open(os.path.join(outdir, "restore.sh"), "w").write("\n".join(r) + "\n")
    with open(man, "w") as f:
        f.write("\n".join(sorted(lines)) + "\n")
    print("\n".join(sorted(lines)))
    print("%d entries listed" % len(lines))


if __name__ == "__main__":
    main()
