#!/usr/bin/env python3
# mct texture decoder (.mcts / .mct), both builds
# bank: u32 n, u32 off[n + 1], then n "MCT_" blocks. Per texture at +0x20: u16 0, u16 w, u16 h,
# u16 0, u16 fmt. Zeebo 0x15/0x16 = ATC RGB / ATC explicit alpha, data at +0x3e.
# Mobile 6/7 = 256 x RGB / RGBA palette at +0x2a, then u8 indices.
import struct, sys
import numpy as np

def bank(b):
    n, = struct.unpack_from('<I', b, 0)
    offs = struct.unpack_from(f'<{n + 1}I', b, 4)
    return [b[offs[i]:offs[i + 1]] for i in range(n)]

def info(t):
    assert t[:4] == b'MCT_'
    _, w, h, _, fmt = struct.unpack_from('<5H', t, 0x20)
    return w, h, fmt

def _atc(src, w, h, alpha):
    bs = 16 if alpha else 8
    nb = (w // 4) * (h // 4)
    blk = np.frombuffer(src[:nb * bs], np.uint8).reshape(nb, bs)
    a = None
    if alpha:
        ab = blk[:, :8]; blk = blk[:, 8:]
        a = np.stack([(ab[:, i >> 1] >> ((i & 1) * 4)) & 15 for i in range(16)], 1).astype(np.int32) * 17
    c0 = blk[:, 0].astype(np.int32) | blk[:, 1].astype(np.int32) << 8
    c1 = blk[:, 2].astype(np.int32) | blk[:, 3].astype(np.int32) << 8
    idx = (blk[:, 4].astype(np.uint32) | blk[:, 5].astype(np.uint32) << 8 |
           blk[:, 6].astype(np.uint32) << 16 | blk[:, 7].astype(np.uint32) << 24)
    A = np.stack([(c0 >> 10 & 31) * 255 // 31, (c0 >> 5 & 31) * 255 // 31, (c0 & 31) * 255 // 31], 1)
    B = np.stack([(c1 >> 11) * 255 // 31, (c1 >> 5 & 63) * 255 // 63, (c1 & 31) * 255 // 31], 1)
    m = (c0 & 0x8000)[:, None] != 0
    p0 = np.where(m, 0, A)
    p1 = np.where(m, np.clip(A - B // 4, 0, 255), (5 * A + 3 * B) // 8)
    p2 = np.where(m, A, (3 * A + 5 * B) // 8)
    pal = np.stack([p0, p1, p2, B], 1)                      # nb,4,3
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], 1).astype(np.int64)  # nb,16
    rgb = np.take_along_axis(pal, sel[:, :, None].repeat(3, 2), 1)  # nb,16,3
    out = np.empty((nb, 16, 4), np.uint8)
    out[:, :, :3] = rgb
    out[:, :, 3] = a if alpha else 255
    out = out.reshape(h // 4, w // 4, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(h, w, 4)
    return out

def decode(t):
    """RGBA uint8 (h, w, 4), row 0 = gl row 0."""
    w, h, fmt = info(t)
    if fmt in (0x15, 0x16):
        return _atc(t[0x3e:], w, h, fmt == 0x16)
    if fmt in (6, 7):
        pn = 4 if fmt == 7 else 3
        p = 0x2a
        pal = np.frombuffer(t[p:p + 256 * pn], np.uint8).reshape(256, pn)
        p += 256 * pn
        ix = np.frombuffer(t[p:p + w * h], np.uint8).reshape(h, w)
        out = np.full((h, w, 4), 255, np.uint8)
        out[:, :, :pn] = pal[ix]
        return out
    raise ValueError(f'fmt {fmt:#x}')

if __name__ == '__main__':
    from PIL import Image
    for f in sys.argv[1:]:
        for i, t in enumerate(bank(open(f, 'rb').read())):
            w, h, fmt = info(t)
            print(f, i, w, h, hex(fmt))
            Image.fromarray(decode(t)).save(f'{f}.{i}.png')
