# Pull (short name, full title) pairs out of an FBNeo libretro core (ELF32 ARM).
# BurnDriver starts: szShortName, szParent, szBoardROM, szSampleName, szDate, szFullName, ...
# The .so is position-independent: R_ARM_RELATIVE pointers hold link-time addresses in place.
import struct, sys, re
data = open(sys.argv[1], 'rb').read()
e_shoff, = struct.unpack_from('<I', data, 0x20)
e_shentsize, e_shnum = struct.unpack_from('<HH', data, 0x2e)
secs = []
for i in range(e_shnum):
    name, typ, flags, addr, off, size = struct.unpack_from('<IIIIII', data, e_shoff + i * e_shentsize)
    if typ != 8:                          # skip SHT_NOBITS (.bss)
        secs.append((addr, off, size, flags))
def v2o(a):
    for addr, off, size, flags in secs:
        if addr and addr <= a < addr + size and flags & 2:   # SHF_ALLOC
            return off + (a - addr)
    return None
def cstr(a, maxlen=200):
    o = v2o(a)
    if o is None: return None
    end = data.find(b'\0', o, o + maxlen)
    if end < 0: return None
    try: return data[o:end].decode('latin-1')
    except: return None
short_re = re.compile(r'^[a-z0-9_]{2,16}$'); year_re = re.compile(r'^(19|20)[0-9?]{2}')
out = {}
for addr, off, size, flags in secs:
    if not (flags & 1): continue          # SHF_WRITE: .data / .data.rel.ro
    for p in range(off, off + size - 40, 4):
        w = struct.unpack_from('<10I', data, p)
        s = cstr(w[0], 20)
        if not s or not short_re.match(s): continue
        y = cstr(w[4], 12)
        if not y or not year_re.match(y): continue
        full = cstr(w[5])
        if not full or len(full) < 2: continue
        out.setdefault(s, full)
for k in sorted(out): print(f"{k}\t{out[k]}")
