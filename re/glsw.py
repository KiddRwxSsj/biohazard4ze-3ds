# gles1 state tracker + slow numpy rasterizer, only for dumping frames out of trace.py
# also the reference for the atc/palette decoders before they go to C
import struct
import numpy as np

FX = 1.0 / 65536

def atc_decode(data, w, h, alpha):
    out = np.zeros((h, w, 4), np.uint8)
    bs = 16 if alpha else 8
    bw, bh = (w + 3) // 4, (h + 3) // 4
    p = 0
    for by in range(bh):
        for bx in range(bw):
            blk = data[p:p + bs]; p += bs
            a = None
            if alpha:
                a = []
                for byte in blk[:8]:
                    a += [(byte & 15) * 17, (byte >> 4) * 17]
                blk = blk[8:]
            c0, c1, idx = struct.unpack('<HHI', blk)
            A = [((c0 >> 10) & 31) * 255 // 31, ((c0 >> 5) & 31) * 255 // 31, (c0 & 31) * 255 // 31]
            B = [(c1 >> 11) * 255 // 31, ((c1 >> 5) & 63) * 255 // 63, (c1 & 31) * 255 // 31]
            if c0 & 0x8000:
                pal = [[0, 0, 0], [max(0, x - y // 4) for x, y in zip(A, B)], A, B]
            else:
                pal = [A, [(5 * x + 3 * y) // 8 for x, y in zip(A, B)], [(3 * x + 5 * y) // 8 for x, y in zip(A, B)], B]
            for i in range(16):
                y, x = by * 4 + i // 4, bx * 4 + i % 4
                if y < h and x < w:
                    out[y, x, :3] = pal[(idx >> (2 * i)) & 3]
                    out[y, x, 3] = a[i] if a else 255
    return out

def palette_decode(fmt, data, w, h):
    # only the ones seen so far, rest grey
    n8 = fmt >= 0x8B95
    entry = {0x8B90: 3, 0x8B91: 4, 0x8B92: 2, 0x8B93: 2, 0x8B94: 2,
             0x8B95: 3, 0x8B96: 4, 0x8B97: 2, 0x8B98: 2, 0x8B99: 2}[fmt]
    base = fmt - (0x8B95 if n8 else 0x8B90)
    cnt = 256 if n8 else 16
    praw = data[:cnt * entry]
    pal = np.zeros((cnt, 4), np.uint8)
    for i in range(cnt):
        e = praw[i * entry:(i + 1) * entry]
        if base == 0: pal[i] = (e[0], e[1], e[2], 255)
        elif base == 1: pal[i] = tuple(e)
        elif base == 2:
            v = e[0] | e[1] << 8; pal[i] = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255)
        elif base == 3:
            v = e[0] | e[1] << 8; pal[i] = ((v >> 12) * 17, ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17)
        else:
            v = e[0] | e[1] << 8; pal[i] = ((v >> 11) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, (v & 1) * 255)
    idx = np.frombuffer(data[cnt * entry:], np.uint8)
    if not n8:
        idx = np.stack([idx >> 4, idx & 15], 1).reshape(-1)
    return pal[idx[:w * h]].reshape(h, w, 4)

TYPES = {0x1400: ('b', 1, 1.0), 0x1401: ('B', 1, 1.0), 0x1402: ('h', 2, 1.0), 0x1403: ('H', 2, 1.0),
         0x1406: ('f', 4, 1.0), 0x140C: ('i', 4, FX)}

class GL:
    def __init__(self, rd_bytes, w=640, h=480):
        self.mem = rd_bytes
        self.W, self.H = w, h
        self.stack = {0x1700: [np.eye(4)], 0x1701: [np.eye(4)], 0x1702: [np.eye(4)]}
        self.mode = 0x1700
        self.arr = {}
        self.cen = set()
        self.en = set()
        self.unit = 0; self.cunit = 0
        self.tex = {}
        self.bound = [0, 0]
        self.texen = [set(), set()]
        self.color = [1.0, 1.0, 1.0, 1.0]
        self.clear = [0.0, 0.0, 0.0, 1.0]
        self.blend = (1, 0)
        self.alphaf = (0x0207, 0.0)
        self.vp = (0, 0, w, h)
        self.capture = False
        self.draws = []
        self.stats = {}
        self.envmode = [0x2100, 0x2100]
        self.fbytes = 0; self.fverts = 0; self.ptrs = set()

    def st(self, k):
        self.stats[k] = self.stats.get(k, 0) + 1

    # --- calls, already without this
    def MatrixMode(self, m): self.mode = m
    def cur(self): return self.stack[self.mode]
    def LoadIdentity(self): self.cur()[-1] = np.eye(4)
    def LoadMatrixx(self, p):
        v = struct.unpack('<16i', self.mem(p, 64))
        self.cur()[-1] = np.array(v, float).reshape(4, 4).T * FX
    def MultMatrixx(self, p):
        v = struct.unpack('<16i', self.mem(p, 64))
        self.cur()[-1] = self.cur()[-1] @ (np.array(v, float).reshape(4, 4).T * FX)
    def PushMatrix(self): self.cur().append(self.cur()[-1].copy())
    def PopMatrix(self):
        if len(self.cur()) > 1: self.cur().pop()
    def Translatex(self, x, y, z):
        m = np.eye(4); m[:3, 3] = [sx(x) * FX, sx(y) * FX, sx(z) * FX]; self.cur()[-1] = self.cur()[-1] @ m
    def Scalex(self, x, y, z):
        self.cur()[-1] = self.cur()[-1] @ np.diag([sx(x) * FX, sx(y) * FX, sx(z) * FX, 1])
    def Orthox(self, l, r, b, t, n, f):
        print('    glOrthox', [sx(v) / 65536 for v in (l, r, b, t, n, f)])
        l, r, b, t, n, f = (sx(v) * FX for v in (l, r, b, t, n, f))
        m = np.eye(4)
        m[0, 0] = 2 / (r - l); m[1, 1] = 2 / (t - b); m[2, 2] = -2 / (f - n)
        m[0, 3] = -(r + l) / (r - l); m[1, 3] = -(t + b) / (t - b); m[2, 3] = -(f + n) / (f - n)
        self.cur()[-1] = self.cur()[-1] @ m
    def Frustumx(self, l, r, b, t, n, f):
        l, r, b, t, n, f = (sx(v) * FX for v in (l, r, b, t, n, f))
        m = np.zeros((4, 4))
        m[0, 0] = 2 * n / (r - l); m[1, 1] = 2 * n / (t - b)
        m[0, 2] = (r + l) / (r - l); m[1, 2] = (t + b) / (t - b)
        m[2, 2] = -(f + n) / (f - n); m[2, 3] = -2 * f * n / (f - n); m[3, 2] = -1
        self.cur()[-1] = self.cur()[-1] @ m
    def Viewport(self, x, y, w, h):
        print('    glViewport', x, y, w, h, hex(self.lr())); self.vp = (sx(x), sx(y), w, h)
    def ActiveTexture(self, u): self.unit = u - 0x84C0
    def ClientActiveTexture(self, u): self.cunit = u - 0x84C0
    def Enable(self, c):
        if c == 0x0DE1: self.texen[self.unit].add(c)
        else: self.en.add(c)
        self.st(f'en {c:#x}')
    def Disable(self, c):
        if c == 0x0DE1: self.texen[self.unit].discard(c)
        else: self.en.discard(c)
    def EnableClientState(self, a): self.cen.add((a, self.cunit) if a == 0x8078 else a)
    def DisableClientState(self, a): self.cen.discard((a, self.cunit) if a == 0x8078 else a)
    def _ptr(self, key, size, typ, stride, p):
        self.arr[key] = (size, typ, stride, p)
        self.st(f'{key} {size}x{TYPES.get(typ, ("?",))[0]} stride {stride}')
    def VertexPointer(self, s, t, st, p): self._ptr('v', s, t, st, p)
    def TexCoordPointer(self, s, t, st, p): self._ptr(('t', self.cunit), s, t, st, p)
    def ColorPointer(self, s, t, st, p): self._ptr('c', s, t, st, p)
    def NormalPointer(self, t, st, p): self._ptr('n', 3, t, st, p)
    def Color4x(self, r, g, b, a): self.color = [sx(v) * FX for v in (r, g, b, a)]
    def Color4ub(self, r, g, b, a): self.color = [v / 255 for v in (r, g, b, a)]
    def ClearColorx(self, r, g, b, a): self.clear = [sx(v) * FX for v in (r, g, b, a)]
    def BlendFunc(self, s, d): self.blend = (s, d); self.st(f'blend {s:#x} {d:#x}')
    def AlphaFuncx(self, f, r): self.alphaf = (f, sx(r) * FX)
    def BindTexture(self, t, n): self.bound[self.unit] = n
    def TexEnvx(self, t, p, v):
        if p == 0x2200: self.envmode[self.unit] = v; self.st(f'env u{self.unit} {v:#x}')
        else: self.st(f'texenv {p:#x}={v:#x}')
    def CompressedTexImage2D(self, t, lvl, fmt, w, h, b, sz, p):
        self.st(f'ctex {fmt:#x}')
        if lvl: return
        d = self.mem(p, sz)
        if fmt in (0x8C92, 0x8C93):
            img = atc_decode(d, w, h, fmt == 0x8C93)
        elif 0x8B90 <= fmt <= 0x8B99:
            img = palette_decode(fmt, d, w, h)
        else:
            img = np.full((h, w, 4), 128, np.uint8)
        self.tex[self.bound[self.unit]] = img
    def TexImage2D(self, t, lvl, ifmt, w, h, b, fmt, typ, p):
        self.st(f'tex {fmt:#x}/{typ:#x}')
        if lvl or not p: return
        if typ == 0x1401 and fmt in (0x1908, 0x1907):
            c = 4 if fmt == 0x1908 else 3
            a = np.frombuffer(self.mem(p, w * h * c), np.uint8).reshape(h, w, c)
            if c == 3: a = np.concatenate([a, np.full((h, w, 1), 255, np.uint8)], 2)
            self.tex[self.bound[self.unit]] = a
    def Clear(self, m):
        if self.capture: self.draws.append(('clear', m, list(self.clear)))

    def _fetch(self, key, idx):
        size, typ, stride, p = self.arr[key]
        f, n, sc = TYPES[typ]
        stride = stride or size * n
        lo, hi = int(idx.min()), int(idx.max())
        raw = self.mem(p + lo * stride, (hi - lo) * stride + size * n)
        out = np.zeros((hi - lo + 1, size))
        for i in range(hi - lo + 1):
            out[i] = struct.unpack_from('<%d%s' % (size, f), raw, i * stride)
        out *= sc
        return out[idx - lo]

    def _draw(self, mode, idx):
        self.st(f'draw mode {mode}')
        lo, hi = int(idx.min()), int(idx.max())
        tot = 0
        for key in ('v', ('t', 0), 'c'):
            en = {'v': 0x8074, 'c': 0x8076}.get(key, (0x8078, 0))
            if en in self.cen and key in self.arr:
                size, typ, stride, p = self.arr[key]
                stride = stride or size * TYPES[typ][1]
                tot += (hi - lo + 1) * stride
                self.ptrs.add(p >> 20)
        self.fbytes += tot
        self.fverts += len(idx)
        if not self.capture or 0x8074 not in self.cen: return
        v = self._fetch('v', idx)
        if v.shape[1] == 2: v = np.c_[v, np.zeros(len(v))]
        tc = None
        if ('t', 0x8078 if False else 0) and (0x8078, 0) in self.cen and self.texen[0] and ('t', 0) in self.arr:
            tc = self._fetch(('t', 0), idx)[:, :2]
            tm = self.stack[0x1702][-1]
            if not np.allclose(tm, np.eye(4)):
                tc = (np.c_[tc, np.zeros(len(tc)), np.ones(len(tc))] @ tm.T)[:, :2]
        col = None
        if 0x8076 in self.cen and 'c' in self.arr:
            col = self._fetch('c', idx)
            if self.arr['c'][1] == 0x1401: col = col / 255
        mvp = self.stack[0x1701][-1] @ self.stack[0x1700][-1]
        clip = np.c_[v, np.ones(len(v))] @ mvp.T
        if mode == 5:
            tris = [(i, i + 1, i + 2) if i % 2 == 0 else (i + 1, i, i + 2) for i in range(len(idx) - 2)]
        elif mode == 6:
            tris = [(0, i, i + 1) for i in range(1, len(idx) - 1)]
        elif mode == 4:
            tris = [(i, i + 1, i + 2) for i in range(0, len(idx) - 2, 3)]
        else:
            return
        self.draws.append(('tri', clip, tc, col, list(self.color), self.tex.get(self.bound[0]) if self.texen[0] else None,
                           self.envmode[0], set(self.en), self.blend, self.alphaf, self.vp, tris))

    def DrawArrays(self, mode, first, count):
        self._draw(mode, np.arange(first, first + count))
    def DrawElements(self, mode, count, typ, p):
        fmt = {0x1401: 'B', 0x1403: 'H'}[typ]
        n = 1 if fmt == 'B' else 2
        self._draw(mode, np.array(struct.unpack('<%d%s' % (count, fmt), self.mem(p, count * n))))
        self.st(f'elements {fmt}')

    # --- raster, slow on purpose, frame dumps only
    def render(self):
        W, H = self.W, self.H
        fb = np.zeros((H, W, 3)); zb = np.full((H, W), np.inf)
        ys, xs = np.mgrid[0:H, 0:W]
        for d in self.draws:
            if d[0] == 'clear':
                if d[1] & 0x4000: fb[:] = d[2][:3]
                if d[1] & 0x100: zb[:] = np.inf
                continue
            _, clip, tc, col, color, tex, env, en, blend, alphaf, vp, tris = d
            for t in tris:
                c = clip[list(t)]
                if (c[:, 3] <= 1e-5).any(): continue
                ndc = c[:, :3] / c[:, 3:4]
                sxs = vp[0] + (ndc[:, 0] + 1) * vp[2] / 2
                sys_ = H - (vp[1] + (ndc[:, 1] + 1) * vp[3] / 2)
                x0, x1 = int(max(0, np.floor(sxs.min()))), int(min(W - 1, np.ceil(sxs.max())))
                y0, y1 = int(max(0, np.floor(sys_.min()))), int(min(H - 1, np.ceil(sys_.max())))
                if x0 > x1 or y0 > y1: continue
                px, py = xs[y0:y1 + 1, x0:x1 + 1] + .5, ys[y0:y1 + 1, x0:x1 + 1] + .5
                (ax, bx_, cx), (ay, by, cy) = sxs, sys_
                area = (bx_ - ax) * (cy - ay) - (cx - ax) * (by - ay)
                if abs(area) < 1e-9: continue
                w0 = ((bx_ - px) * (cy - py) - (cx - px) * (by - py)) / area
                w1 = ((cx - px) * (ay - py) - (ax - px) * (cy - py)) / area
                w2 = 1 - w0 - w1
                m = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
                if not m.any(): continue
                iw = 1 / c[:, 3]
                pw = w0 * iw[0] + w1 * iw[1] + w2 * iw[2]
                z = w0 * ndc[0, 2] + w1 * ndc[1, 2] + w2 * ndc[2, 2]
                zr = zb[y0:y1 + 1, x0:x1 + 1]
                if 0x0B71 in en: m &= z <= zr
                rgba = np.ones(px.shape + (4,)) * color
                if col is not None:
                    cc = col[list(t)]
                    for k in range(min(4, cc.shape[1])):
                        rgba[..., k] = (w0 * cc[0, k] * iw[0] + w1 * cc[1, k] * iw[1] + w2 * cc[2, k] * iw[2]) / pw
                if tex is not None and tc is not None:
                    uv = tc[list(t)]
                    u = (w0 * uv[0, 0] * iw[0] + w1 * uv[1, 0] * iw[1] + w2 * uv[2, 0] * iw[2]) / pw
                    v = (w0 * uv[0, 1] * iw[0] + w1 * uv[1, 1] * iw[1] + w2 * uv[2, 1] * iw[2]) / pw
                    th, tw = tex.shape[:2]
                    tx = (np.floor(u * tw).astype(int)) % tw
                    ty = (np.floor(v * th).astype(int)) % th
                    s = tex[ty, tx] / 255.0
                    if env == 0x1E01: rgba = s  # replace
                    else: rgba = rgba * s
                if 0x0BC0 in en:
                    f, ref = alphaf
                    a = rgba[..., 3]
                    m &= {0x0200: a < -1, 0x0201: a < ref, 0x0202: a == ref, 0x0203: a <= ref,
                          0x0204: a > ref, 0x0205: a != ref, 0x0206: a >= ref}.get(f, a >= -1)
                dst = fb[y0:y1 + 1, x0:x1 + 1]
                src = rgba[..., :3]
                if 0x0BE2 in en:
                    sa = rgba[..., 3:4]
                    fac = {0: 0, 1: 1, 0x0302: sa, 0x0303: 1 - sa}
                    src = src * fac.get(blend[0], 1) + dst * fac.get(blend[1], 0)
                dst[m] = np.clip(src[m], 0, 1)
                if 0x0B71 in en and (0x0B72 not in en): zr[m] = z[m]
        return (fb * 255).astype(np.uint8)

def sx(v):
    return v - (1 << 32) if v & 0x80000000 else v
