#!/usr/bin/env python3
# shrink a .sf2 for the 3ds: resample every sample above --rate down to it.
# loop lengths stay integer by nudging each sample's stored rate, so pitch stays exact.
# --gm keeps only bank 0 melodic + the standard drum kit and drops whatever that leaves unused.
# usage: sf2shrink.py in.sf2 out.sf2 [--rate 32000] [--gm]
import struct, sys
import numpy as np

def chunks(d, o, end):
    while o + 8 <= end:
        cid, sz = d[o:o + 4], struct.unpack_from('<I', d, o + 4)[0]
        yield cid, o + 8, sz
        o += 8 + sz + (sz & 1)

def resample(x, n):
    # band limited via fft, fine for one-shot conversion
    if n == len(x) or len(x) < 2:
        return x.astype(np.float64)
    X = np.fft.rfft(x)
    m = n // 2 + 1
    Y = np.zeros(m, complex)
    k = min(m, len(X))
    Y[:k] = X[:k]
    return np.fft.irfft(Y, n) * (n / len(x))

def recs(b, size):
    return [bytes(b[i:i + size]) for i in range(0, len(b), size)]

def gm_subset(p):
    phdr, pbag, pgen, pmod = recs(p[b'phdr'], 38), recs(p[b'pbag'], 4), recs(p[b'pgen'], 4), recs(p[b'pmod'], 10)
    inst, ibag, igen, imod = recs(p[b'inst'], 22), recs(p[b'ibag'], 4), recs(p[b'igen'], 4), recs(p[b'imod'], 10)
    shdr = recs(p[b'shdr'], 46)

    def zones(hdrs, bags, idx_off):
        # (bag range) per header, last header is the terminal record
        out = []
        for i in range(len(hdrs) - 1):
            b0 = struct.unpack_from('<H', hdrs[i], idx_off)[0]
            b1 = struct.unpack_from('<H', hdrs[i + 1], idx_off)[0]
            out.append((b0, b1))
        return out

    keep_p = []
    for i, (b0, b1) in enumerate(zones(phdr, pbag, 24)):
        prog, bank = struct.unpack_from('<HH', phdr[i], 20)
        if bank == 0 or (bank == 128 and prog == 0):
            keep_p.append(i)

    # rebuild preset side, collect instruments
    used_i = []
    n_phdr, n_pbag, n_pgen, n_pmod = [], [], [], []
    pz = zones(phdr, pbag, 24)
    for i in keep_p:
        b0, b1 = pz[i]
        n_phdr.append(phdr[i][:24] + struct.pack('<H', len(n_pbag)) + phdr[i][26:])
        for b in range(b0, b1):
            g0, m0 = struct.unpack_from('<HH', pbag[b])
            g1, m1 = struct.unpack_from('<HH', pbag[b + 1])
            n_pbag.append(struct.pack('<HH', len(n_pgen), len(n_pmod)))
            n_pmod.extend(pmod[m0:m1])
            for g in range(g0, g1):
                op, amt = struct.unpack_from('<HH', pgen[g])
                if op == 41:
                    if amt not in used_i: used_i.append(amt)
                    amt = used_i.index(amt)
                n_pgen.append(struct.pack('<HH', op, amt))
    n_phdr.append(b'EOP'.ljust(20, b'\0') + b'\0' * 4 + struct.pack('<H', len(n_pbag)) + b'\0' * 12)
    n_pbag.append(struct.pack('<HH', len(n_pgen), len(n_pmod)))
    n_pgen.append(b'\0' * 4)
    n_pmod.append(b'\0' * 10)

    used_s = []
    n_inst, n_ibag, n_igen, n_imod = [], [], [], []
    iz = zones(inst, ibag, 20)
    for i in used_i:
        b0, b1 = iz[i]
        n_inst.append(inst[i][:20] + struct.pack('<H', len(n_ibag)))
        for b in range(b0, b1):
            g0, m0 = struct.unpack_from('<HH', ibag[b])
            g1, m1 = struct.unpack_from('<HH', ibag[b + 1])
            n_ibag.append(struct.pack('<HH', len(n_igen), len(n_imod)))
            n_imod.extend(imod[m0:m1])
            for g in range(g0, g1):
                op, amt = struct.unpack_from('<HH', igen[g])
                if op == 53:
                    if amt not in used_s: used_s.append(amt)
                    amt = used_s.index(amt)
                n_igen.append(struct.pack('<HH', op, amt))
    n_inst.append(b'EOI'.ljust(20, b'\0') + struct.pack('<H', len(n_ibag)))
    n_ibag.append(struct.pack('<HH', len(n_igen), len(n_imod)))
    n_igen.append(b'\0' * 4)
    n_imod.append(b'\0' * 10)

    # stereo partners have to come along, then remap the links
    k = 0
    while k < len(used_s):
        link = struct.unpack_from('<H', shdr[used_s[k]], 42)[0]
        typ = struct.unpack_from('<H', shdr[used_s[k]], 44)[0]
        if typ & 6 and link < len(shdr) - 1 and link not in used_s: used_s.append(link)
        k += 1
    n_shdr = []
    for sidx in used_s:
        r = bytearray(shdr[sidx])
        link = struct.unpack_from('<H', r, 42)[0]
        struct.pack_into('<H', r, 42, used_s.index(link) if link in used_s else 0)
        n_shdr.append(bytes(r))
    n_shdr.append(shdr[-1])

    for k, v in ((b'phdr', n_phdr), (b'pbag', n_pbag), (b'pgen', n_pgen), (b'pmod', n_pmod),
                 (b'inst', n_inst), (b'ibag', n_ibag), (b'igen', n_igen), (b'imod', n_imod), (b'shdr', n_shdr)):
        p[k] = bytearray(b''.join(v))
    print(f'gm subset: {len(keep_p)} presets, {len(used_i)} instruments, {len(used_s)} samples')

def main():
    src, dst = sys.argv[1], sys.argv[2]
    rate = int(sys.argv[sys.argv.index('--rate') + 1]) if '--rate' in sys.argv else 32000
    d = open(src, 'rb').read()
    assert d[:4] == b'RIFF' and d[8:12] == b'sfbk'
    parts = {}
    for cid, o, sz in chunks(d, 12, len(d)):
        if cid == b'LIST':
            kind = d[o:o + 4]
            parts[kind] = (o + 4, o + sz)
    so, se = parts[b'sdta']
    smpl = None
    for cid, o, sz in chunks(d, so, se):
        if cid == b'smpl': smpl = np.frombuffer(d[o:o + sz], '<i2')
    po, pe = parts[b'pdta']
    pdta = {cid: bytearray(d[o:o + sz]) for cid, o, sz in chunks(d, po, pe)}

    if '--gm' in sys.argv:
        gm_subset(pdta)

    shdr = pdta[b'shdr']
    n = len(shdr) // 46
    out = []
    pos = 0
    ratio = {}
    newhdr = bytearray()
    for i in range(n):
        rec = shdr[i * 46:(i + 1) * 46]
        name = rec[:20]
        start, end, ls, le, sr, opitch, pcorr, link, typ = struct.unpack_from('<IIIIIBbHH', rec, 20)
        if i == n - 1 or typ & 0x8000 or end <= start:
            # terminal record and rom samples stay as they are, relative to our new data
            newhdr += name + struct.pack('<IIIIIBbHH', 0, 0, 0, 0, sr, opitch, pcorr, link, typ)
            ratio[i] = 1.0
            continue
        x = smpl[start:end].astype(np.float64)
        r = min(1.0, rate / sr) if sr else 1.0
        loop = le - ls
        if r < 1.0 and loop > 0:
            nl = max(1, round(loop * r))
            r = nl / loop
        nlen = max(1, round(len(x) * r))
        y = resample(x, nlen) if r < 1.0 else x
        y = np.clip(np.round(y), -32768, 32767).astype('<i2')
        nsr = round(sr * r)
        nls = pos + round((ls - start) * r)
        nle = nls + (round(loop * r) if loop > 0 else 0)
        newhdr += name + struct.pack('<IIIIIBbHH', pos, pos + nlen, nls, nle, nsr, opitch, pcorr, link, typ)
        ratio[i] = r
        out.append(y)
        out.append(np.zeros(46, '<i2'))
        pos += nlen + 46
    pdta[b'shdr'] = newhdr

    # sample offset generators live in instrument zones, scale them with that zone's sample
    ibag, igen = pdta[b'ibag'], pdta[b'igen']
    nbag = len(ibag) // 4
    offs = {0: 1, 1: 1, 2: 1, 3: 1, 4: 32768, 12: 32768, 45: 32768, 50: 32768}
    for b in range(nbag - 1):
        g0 = struct.unpack_from('<H', ibag, b * 4)[0]
        g1 = struct.unpack_from('<H', ibag, (b + 1) * 4)[0]
        sid = None
        for g in range(g0, g1):
            op, amt = struct.unpack_from('<HH', igen, g * 4)
            if op == 53: sid = amt
        if sid is None or ratio.get(sid, 1.0) == 1.0: continue
        r = ratio[sid]
        # fold coarse into fine where possible would change semantics, just scale each
        for g in range(g0, g1):
            op, amt = struct.unpack_from('<Hh', igen, g * 4)
            if op in offs and amt:
                struct.pack_into('<Hh', igen, g * 4, op, int(round(amt * r)))

    smpl_new = np.concatenate(out).tobytes() if out else b''
    def chunk(cid, body):
        return cid + struct.pack('<I', len(body)) + body + (b'\0' if len(body) & 1 else b'')
    def lst(kind, body):
        return chunk(b'LIST', kind + body)
    io, ie = parts[b'INFO']
    info = d[io - 4:ie]
    sdta = lst(b'sdta', chunk(b'smpl', smpl_new))
    order = [b'phdr', b'pbag', b'pmod', b'pgen', b'inst', b'ibag', b'imod', b'igen', b'shdr']
    pd = lst(b'pdta', b''.join(chunk(c, bytes(pdta[c])) for c in order))
    body = b'sfbk' + chunk(b'LIST', info) + sdta + pd
    open(dst, 'wb').write(b'RIFF' + struct.pack('<I', len(body)) + body)
    print(f'{len(smpl) * 2 / 1e6:.1f} MB -> {len(smpl_new) / 1e6:.1f} MB, {n - 1} samples, cap {rate} Hz')

main()
