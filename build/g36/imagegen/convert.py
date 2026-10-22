#!/usr/bin/env python3
"""Pulls the image generator's weights out of the downloaded files into simple f32
files for g36-imagegen to pack (plain Python: no PyTorch or NumPy needed).

    python3 -I convert.py DOWNLOADS_DIR OUT_DIR

DOWNLOADS_DIR holds supra2-img/, flan-t5-base/ and taesd/ as downloaded. Writes
dit.f32, t5.f32 and taesd.f32 to OUT_DIR. File layout (little endian):
    "G36W" u32 version(1) u32 count, then per tensor:
    u16 name length, name, u8 type (0 = f32), u8 dims, u32 ne[4] (ggml order:
    fastest first), u64 bytes, zero padding to a multiple of 32, the data.
"""
import collections
import json
import os
import pickle
import struct
import sys
import zipfile


class Storage:
    def __init__(self, key, dtype):
        self.key, self.dtype = key, dtype


class Tensor:
    def __init__(self, storage, offset, size, stride):
        self.storage, self.offset, self.size, self.stride = storage, offset, tuple(size), tuple(stride)


def _rebuild(storage, offset, size, stride, *rest):
    return Tensor(storage, offset, size, stride)


class _Unpickler(pickle.Unpickler):
    def find_class(self, mod, name):
        if name == "_rebuild_tensor_v2":
            return _rebuild
        if mod == "collections" and name == "OrderedDict":
            return collections.OrderedDict
        if mod.startswith("torch"):
            return name          # storage types arrive as their names
        return super().find_class(mod, name)

    def persistent_load(self, pid):
        return Storage(pid[2], pid[1])


ELEM = {"FloatStorage": 4, "BFloat16Storage": 2, "LongStorage": 8}


def pt_tensor_bytes(zf, prefix, t):
    """A tensor's data as f32 bytes (contiguous only)."""
    n = 1
    for s in t.size:
        n *= s
    exp, run = [], 1
    for s in reversed(t.size):
        exp.insert(0, run)
        run *= s
    if tuple(exp) != t.stride and n > 1:
        raise SystemExit("not contiguous: %s %s" % (t.size, t.stride))
    es = ELEM[t.storage.dtype]
    raw = zf.read("%s/data/%s" % (prefix, t.storage.key))[t.offset * es:(t.offset + n) * es]
    if t.storage.dtype == "FloatStorage":
        return raw
    if t.storage.dtype == "BFloat16Storage":
        out = bytearray(n * 4)
        out[2::4] = raw[0::2]
        out[3::4] = raw[1::2]
        return bytes(out)
    if t.storage.dtype == "LongStorage":
        vals = struct.unpack("<%dq" % n, raw)
        return struct.pack("<%df" % n, *[float(v) for v in vals])
    raise SystemExit("type " + t.storage.dtype)


def write(path, tensors):
    """tensors: [(name, torch shape, f32 bytes)]"""
    with open(path + ".tmp", "wb") as f:
        f.write(b"G36W" + struct.pack("<II", 1, len(tensors)))
        for name, shape, data in tensors:
            ne = list(reversed(shape)) + [1] * (4 - len(shape))
            nb = name.encode()
            f.write(struct.pack("<H", len(nb)) + nb + struct.pack("<BB4IQ", 0, len(shape), *ne, len(data)))
            f.write(b"\0" * (-f.tell() % 32))
            f.write(data)
    os.replace(path + ".tmp", path)
    print("%s: %d tensors, %.0f MB" % (path, len(tensors), os.path.getsize(path) / 1e6))


def safetensors(path, keep):
    with open(path, "rb") as f:
        n = struct.unpack("<Q", f.read(8))[0]
        head = json.loads(f.read(n))
        base = 8 + n
        out = []
        for name in sorted(k for k in head if k != "__metadata__" and keep(k)):
            h = head[name]
            if h["dtype"] != "F32":
                raise SystemExit(name + " is " + h["dtype"])
            a, b = h["data_offsets"]
            f.seek(base + a)
            out.append((name, h["shape"], f.read(b - a)))
        return out


def main():
    src, dst = sys.argv[1], sys.argv[2]
    os.makedirs(dst, exist_ok=True)

    # the DiT, and the stored "no prompt" text for guidance
    zf = zipfile.ZipFile(os.path.join(src, "supra2-img", "model_final_ema.pt"))
    prefix = zf.namelist()[0].split("/")[0]
    state = _Unpickler(zf.open(prefix + "/data.pkl")).load()
    weights = state.get("ema", state.get("model"))
    tensors = [(k, list(t.size), pt_tensor_bytes(zf, prefix, t)) for k, t in weights.items()]
    cfg = state.get("config", {})
    mask = struct.unpack("<%df" % cfg["uncond_mask"].size[0], pt_tensor_bytes(zf, prefix, cfg["uncond_mask"]))
    n = int(sum(mask))
    un = cfg["uncond_text"]
    data = pt_tensor_bytes(zf, prefix, un)[: n * un.size[1] * 4]   # only its real tokens
    tensors.append(("uncond_text", [n, un.size[1]], data))
    write(os.path.join(dst, "dit.f32"), tensors)

    # Flan-T5-Base: only the encoder (and the shared word table) is used
    write(os.path.join(dst, "t5.f32"), safetensors(
        os.path.join(src, "flan-t5-base", "model.safetensors"),
        lambda k: k.startswith("encoder.") or k == "shared.weight"))

    write(os.path.join(dst, "taesd.f32"), safetensors(
        os.path.join(src, "taesd", "taesd_decoder.safetensors"), lambda k: True))


if __name__ == "__main__":
    main()
