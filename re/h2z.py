#!/usr/bin/env python3
# h2z reader. "H2Z\0" u32 ? u32 size, "HIZ\0" u32 count u32 ?, then per entry
# u16 flag, u32 size, u32 size2, u16 namelen, name, payload. entry headers xor "bio4",
# key index runs over header bytes only. flag 8 = raw deflate (mobile)
import struct, sys, zlib

KEY = b'bio4'

def entries(d):
    assert d[:4] == b'H2Z\0' and d[12:16] == b'HIZ\0', 'not h2z'
    n, = struct.unpack_from('<I', d, 16)
    p, k = 0x18, 0
    def take(m):
        nonlocal p, k
        b = bytes(c ^ KEY[(k + i) & 3] for i, c in enumerate(d[p:p + m]))
        p += m; k += m
        return b
    for _ in range(n):
        flag, = struct.unpack('<H', take(2))
        size, = struct.unpack('<I', take(4))
        size2, = struct.unpack('<I', take(4))
        nl, = struct.unpack('<H', take(2))
        name = take(nl).decode('latin1')
        yield name, flag, size, size2, p
        p += size

def get(d, want):
    for name, flag, size, size2, off in entries(d):
        if name == want:
            b = d[off:off + size]
            return zlib.decompress(b, -15) if flag & 8 else b
    raise KeyError(want)

if __name__ == '__main__':
    for f in sys.argv[1:]:
        d = open(f, 'rb').read()
        for name, flag, size, size2, off in entries(d):
            print(f'{flag:04x} {size:8d} {size2:8d} {off:#9x} {name}  {d[off:off+8].hex()}')
