"""Rebuild a flattened device tree blob after editing its parsed tree (see fitcheck.parse_fdt)."""
import struct


def encode(root, orig_blob):
    _, _, _, off_strings, off_rsv, ver, last_comp, boot_cpu, size_strings, _ = struct.unpack_from(">10I", orig_blob, 0)
    strings = bytearray(orig_blob[off_strings:off_strings + size_strings])

    def name_off(name):
        key = name.encode() + b"\0"
        i = strings.find(key)  # dtc shares string tails, so a match may start mid-string
        if i >= 0:
            return i
        strings.extend(key)
        return len(strings) - len(key)

    out = bytearray()

    def node(name, n):
        out.extend(struct.pack(">I", 1))
        nm = name.encode() + b"\0"
        out.extend(nm + b"\0" * (-len(nm) % 4))
        for k, v in n["props"].items():
            out.extend(struct.pack(">III", 3, len(v), name_off(k)))
            out.extend(v + b"\0" * (-len(v) % 4))
        for k, v in n["children"].items():
            node(k, v)
        out.extend(struct.pack(">I", 2))

    node("", root)
    out.extend(struct.pack(">I", 9))
    rsv = struct.pack(">QQ", 0, 0)
    off_rsv = 40
    off_struct = off_rsv + len(rsv)
    off_str = off_struct + len(out)
    total = off_str + len(strings)
    hdr = struct.pack(">10I", 0xD00DFEED, total, off_struct, off_str, off_rsv, ver, last_comp, boot_cpu,
                      len(strings), len(out))
    return hdr + rsv + bytes(out) + bytes(strings)
