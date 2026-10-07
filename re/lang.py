#!/usr/bin/env python3
# zeebo pt text tables + the english patch over them
#
#   lang.py dump  <mod dir> <out.txt>           every string, for translating (stays local, it's Capcom's)
#   lang.py check <mod dir> <en.txt>            fit, width and coverage report
#   lang.py build <mod dir> <en.txt> <out.h>    C patch table for source/lang.c
#   lang.py apply <mod dir> <en.txt> <out dir>  patched archives, reference for testing lang.c
#
# keys: archive:entry:offset + fnv of the original bytes, other dumps get left alone.
# known layouts get rebuilt with new lengths as long as the table/section keeps its size.
# item_file_info goes in place, english must fit and gets space padded.
#
# 2 encodings. data tables: bytes, ascii + 0x82 xx accents, \r\n breaks.
# Bio4_StringLayout.bin (menus/shop/options): u16 glyphs, 0x100 + index into 2d_fnt.hfi,
# 0x20 space, 0x0a break.
#
# en.txt: one block per string, "|" = break, {1}..{4} {H} = pad buttons (BUTTONS),
# {^} {v} {<} {>} = arrow glyphs. key with no text keeps the original.
#
#   @common.h2z:item_base_info.bin:0x1f
#   Combat Knife
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import h2z

ARCHIVES = ('common.h2z', '3d_base.h2z')   # the only ones with text, both cached in ram by file.c
TEXT = re.compile(r'item_base_info|item_file_info|stgnfo_mission_m[ms]|mis_data_[ms]\d+|sys_param|Bio4_StringLayout')
HFS = 'Bio4_StringLayout.bin'

# zeebo button numbers in the text -> 3ds names via hid.c. status is "button 3" in the text
# and X on hw, 2/4 follow from that. 2 and 4 not checked on hw yet
BUTTONS = {'1': 'A', '2': 'B', '3': 'X', '4': 'Y', 'H': 'START'}

# accented letters: 0x82 xx in the data tables, glyph code 0x82xx in the font
ACC = {0xa0: 'Á', 0xa2: 'É', 0xaf: 'Ã', 0xb9: 'á', 0xbb: 'é', 0xbd: 'í', 0xbf: 'ó', 0xc2: 'ú',
       0xc4: 'â', 0xc6: 'ê', 0xc8: 'ô', 0xc9: 'ã', 0xca: 'õ', 0xcb: 'à', 0xcd: 'ç', 0xd6: 'º'}
RACC = {v: k for k, v in ACC.items()}
FACC = {0x65: 'Á', 0x6d: 'Ê', 0x6f: 'Ã', 0x71: 'Õ', 0x74: 'Ç', 0x75: 'á', 0x76: 'é', 0x78: 'í',
        0x81: 'ó', 0x85: 'ê', 0x87: 'ô', 0x88: 'ã', 0x89: 'õ', 0x8b: 'à', 0x8e: 'ç',
        0xe0: '>', 0xe1: '<', 0xe2: '^', 0xe3: 'v'}
ARROWS = {'^': 0xe2, 'v': 0xe3, '>': 0xe0, '<': 0xe1}

def fnv(b):
    h = 2166136261
    for c in b: h = ((h ^ c) * 16777619) & 0xffffffff
    return h

FONT = []   # glyph index -> code, from 2d_fnt.hfi
def load_font(moddir):
    d = open(os.path.join(moddir, 'common.h2z'), 'rb').read()
    h = h2z.get(d, '2d_fnt.hfi')
    _, _, ns, tot = struct.unpack_from('<4H', h, 3)
    FONT[:] = list(h[11:11 + ns]) + list(struct.unpack_from(f'<{tot - ns}H', h, 11 + ns))

def decode(name, b):
    out = []
    if name == HFS:
        for v, in struct.iter_unpack('<H', b):
            if v == 0x20: out.append(' ')
            elif v == 0x0a: out.append('|')
            elif v >= 0x100 and v - 0x100 < len(FONT):
                c = FONT[v - 0x100]
                out.append(chr(c) if c < 0x80 else FACC.get(c & 0xff, f'<{c:04x}>') if c >> 8 == 0x82 else f'<{c:04x}>')
            else: out.append(f'[{v:x}]')
        return ''.join(out)
    i = 0
    while i < len(b):
        if b[i] == 0x82 and i + 1 < len(b) and b[i + 1] in ACC: out.append(ACC[b[i + 1]]); i += 2
        elif b[i:i + 2] == b'\r\n': out.append('|'); i += 2
        else: out.append(chr(b[i])); i += 1
    return ''.join(out)

def encode(name, s):
    s = re.sub(r'\{([1-4H])\}', lambda m: BUTTONS[m.group(1)], s)
    if name == HFS:
        out = []
        for m in re.finditer(r'\{([\^v<>])\}|.', s):
            ch = m.group(0)
            if m.group(1): out.append(0x100 + FONT.index(0x8200 | ARROWS[m.group(1)]))
            elif ch == ' ': out.append(0x20)
            elif ch == '|': out.append(0x0a)
            elif 0x21 <= ord(ch) < 0x7f: out.append(0x100 + FONT.index(ord(ch)))
            else: raise ValueError(f'no glyph for {ch!r}')
        return struct.pack(f'<{len(out)}H', *out)
    out = bytearray()
    for ch in s:
        if ch == '|': out += b'\r\n'
        elif ch in RACC: out += bytes((0x82, RACC[ch]))
        elif 0x20 <= ord(ch) < 0x7f: out.append(ord(ch))
        else: raise ValueError(f'no glyph for {ch!r}')
    return bytes(out)

def width(name, b):
    return max(len(l) for l in decode(name, b).split('|'))

# ---- string finders

# data tables: printable run (ascii, \r\n, 0x82 pairs) ending in \0, preceded by a non-text
# byte, 3+ letters. TMP and the SCB magic aren't text
STR = re.compile(rb'(?:[\x20-\x7e]|\r\n|\x82[\xa0-\xd6])+(?=\x00)')
def strings(name, b):
    if name == HFS:
        yield from hfs_strings(b)
        return
    for m in STR.finditer(b):
        s, o = m.group(), m.start()
        if o and (0x20 <= b[o - 1] < 0x7f or b[o - 1] in (0x0a, 0x82)): continue
        if len(re.findall(rb'[A-Za-z]|\x82', s)) < 3 or s == b'TMP' or s.startswith(b'SCB'): continue
        yield o, s

def hfs_pool(b):
    n, units = struct.unpack_from('<HH', b, 3)
    return n, units, 7 + 20 * n

def hfs_strings(b):
    # every string a record points at that has at least two letters
    n, units, pool = hfs_pool(b)
    for o in sorted({struct.unpack_from('<H', b, 7 + 20 * i)[0] for i in range(n)}):
        p = pool + 2 * o
        e = p
        while struct.unpack_from('<H', b, e)[0]: e += 2
        s = b[p:e]
        if sum(1 for v, in struct.iter_unpack('<H', s) if v >= 0x100) >= 2: yield p, s

def cstr(b, p):
    return b[p:b.index(b'\0', p)]

# ---- rebuilders. new = {orig string offset: bytes}. return table, None if it won't fit

def items_rebuild(b, new):
    # "SCB" u8 n u8; n x { u16 strbytes, 24 bytes (u16 namelen at +13), name\0, desc\0 }
    out, p = bytearray(b[:5]), 5
    for _ in range(b[3]):
        L, = struct.unpack_from('<H', b, p)
        q = p + 26
        name = cstr(b, q); desc = cstr(b, q + len(name) + 1)
        name, desc = new.get(q, name), new.get(q + len(name) + 1, desc)
        fx = bytearray(b[p + 2:p + 26])
        struct.pack_into('<H', fx, 13, len(name) + 1)
        out += struct.pack('<H', len(name) + len(desc) + 2) + fx + name + b'\0' + desc + b'\0'
        p += 26 + L
    return out

def missions_rebuild(b, new):
    # "SCB" u8 n u8; n x { u16 strbytes, 5 bytes, u16 end of title, u16 end of place,
    # title\0 place\0 text\0 }
    out, p = bytearray(b[:5]), 5
    for _ in range(b[3]):
        L, = struct.unpack_from('<H', b, p)
        q, ss = p + 11, []
        for _ in range(3):
            s = cstr(b, q); ss.append(new.get(q, s)); q += len(s) + 1
        t, pl, tx = ss
        a = len(t) + 1; c = a + len(pl) + 1
        out += struct.pack('<H', c + len(tx) + 1) + b[p + 2:p + 7] + struct.pack('<HH', a, c)
        out += t + b'\0' + pl + b'\0' + tx + b'\0'
        p += 11 + L
    return out

def section(b, s, e, new):
    # u32 count, u32 strbytes, count x { u16 off, u16 len (with \0) }, strings.
    # rebuilt in place, zero fill to e
    cnt, = struct.unpack_from('<I', b, s)
    st = s + 8 + 4 * cnt
    hdr, body = bytearray(struct.pack('<II', cnt, 0)), bytearray()
    for k in range(cnt):
        o, l = struct.unpack_from('<HH', b, s + 8 + 4 * k)
        t = new.get(st + o, b[st + o:st + o + l - 1])
        hdr += struct.pack('<HH', len(body), len(t) + 1); body += t + b'\0'
    struct.pack_into('<I', hdr, 4, len(body))
    if len(hdr) + len(body) > e - s: return False
    b[s:e] = hdr + body + bytes(e - s - len(hdr) - len(body))
    return True

def sysparam_rebuild(b, new):
    # u32 1, u32 base, u32 end; at base u32 n, u32 off[n + 1] from base, n text sections
    out = bytearray(b)
    base, = struct.unpack_from('<I', b, 4)
    n, = struct.unpack_from('<I', b, base)
    offs = struct.unpack_from(f'<{n + 1}I', b, base + 4)
    for i in range(n):
        if not section(out, base + offs[i], base + offs[i + 1], new): return None
    return out

def misdata_rebuild(b, new):
    # u32 6, u32 off[7]; subtable 5 is the text section
    out = bytearray(b)
    n, = struct.unpack_from('<I', b, 0)
    offs = struct.unpack_from(f'<{n + 1}I', b, 4)
    return out if section(out, offs[5], offs[6], new) else None

def hfs_rebuild(b, new):
    # "HFS" u16 n, u16 pool units; n x 20 byte records { u16 string (units into the pool),
    # s16 x, s16 y, ... }; pool of u16 strings, 0 terminated. relay in order, repoint records,
    # unit count stays, tail zeroed
    n, units, pool = hfs_pool(b)
    starts, o = [], 0
    while o < units:
        starts.append(o)
        while struct.unpack_from('<H', b, pool + 2 * o)[0]: o += 1
        o += 1
    moved, body = {}, bytearray()
    for o in starts:
        p = pool + 2 * o
        e = p
        while struct.unpack_from('<H', b, e)[0]: e += 2
        moved[o] = len(body) // 2
        body += new.get(p, b[p:e]) + b'\0\0'
    if len(body) > 2 * units: return None
    out = bytearray(b)
    for i in range(n):
        o, = struct.unpack_from('<H', b, 7 + 20 * i)
        if o not in moved: return None
        struct.pack_into('<H', out, 7 + 20 * i, moved[o])
    out[pool:pool + 2 * units] = body + bytes(2 * units - len(body))
    return out

def inplace(b, new):
    out = bytearray(b)
    for o, t in new.items():
        n = len(cstr(b, o))
        if len(t) > n: return None
        out[o:o + n] = t + b' ' * (n - len(t))
    return out

def kind(name):
    if name == 'item_base_info.bin': return items_rebuild
    if name.startswith('stgnfo_mission'): return missions_rebuild
    if name == 'sys_param.bin': return sysparam_rebuild
    if name.startswith('mis_data'): return misdata_rebuild
    if name == HFS: return hfs_rebuild
    return inplace

def rebuild(name, b, new):
    out = kind(name)(b, new)
    if out is None or len(out) > len(b): return None
    return bytes(out) + bytes(len(b) - len(out))

# ---- widths

# widest line the original already uses per box type. item names: own limit, inventory list
# is narrower than the desc box. menu labels sit at fixed spots, only grow a bit
WIDTH = {'item_base_info': 30, 'item_file_info': 29, 'stgnfo_mission_mm': 29,
         'stgnfo_mission_ms': 28, 'mis_data': 30, 'sys_param': 26}
NAME_WIDTH = 20

def limit(name, b, o, s):
    if name == HFS: return max(width(name, s) + 3, 8)
    if name == 'item_base_info.bin':
        p = 5
        for _ in range(b[3]):
            if o == p + 26: return NAME_WIDTH
            p += 26 + struct.unpack_from('<H', b, p)[0]
    return WIDTH[re.sub(r'(_[ms]\d+)?\.bin$', '', name)]

# ----

def entries(moddir):
    for a in ARCHIVES:
        d = open(os.path.join(moddir, a), 'rb').read()
        for name, flag, size, _, off in h2z.entries(d):
            if name.endswith('.bin') and TEXT.fullmatch(name[:-4]):
                yield a, name, off, d[off:off + size]

def read_en(path):
    out, key, lines = {}, None, []
    def flush():
        if key: out[key] = '|'.join(lines).strip('|')
    for l in open(path, encoding='utf-8'):
        l = l.rstrip('\n')
        if l.startswith('#'): continue
        if l.startswith('@'):
            flush(); key, lines = l[1:].strip(), []
        elif key is not None and l.strip():
            lines.append(l)
    flush()
    return out

def plan(moddir, en):
    """[(archive, entry, entry offset, table, {off: (orig, new)})], problems"""
    out, bad, seen = [], [], set()
    for a, n, eo, b in entries(moddir):
        new = {}
        for o, s in strings(n, b):
            k = f'{a}:{n}:{o:#x}'; seen.add(k)
            if not en.get(k): continue
            t = encode(n, en[k])
            lim = limit(n, b, o, s)
            if width(n, t) > lim: bad.append(f'too wide {width(n, t)} > {lim}: {k}  {en[k]}')
            if kind(n) is inplace and len(t) > len(s): bad.append(f'too long {len(t)} > {len(s)}: {k}  {en[k]}')
            new[o] = (s, t)
        if new and kind(n) is not inplace and rebuild(n, b, {o: t for o, (s, t) in new.items()}) is None:
            bad.append(f'table outgrows its space: {a}:{n}')
        out.append((a, n, eo, b, new))
    bad += [f'unknown key {k}' for k in en if k not in seen]
    return out, bad

def c_str(b):
    return ''.join(chr(c) if 0x20 <= c < 0x7f and c not in (0x22, 0x5c, 0x3f) else f'\\{c:03o}' for c in b)

def main():
    cmd, moddir = sys.argv[1], sys.argv[2]
    load_font(moddir)
    if cmd == 'dump':
        n = 0
        with open(sys.argv[3], 'w', encoding='utf-8') as f:
            for a, name, _, b in entries(moddir):
                for o, s in strings(name, b):
                    f.write(f'@{a}:{name}:{o:#x}\n# {len(s)} {decode(name, s)}\n\n'); n += 1
        print(n, 'strings')
        return
    en = read_en(sys.argv[3])
    p, bad = plan(moddir, en)
    for x in bad: print(x)
    total = sum(1 for _, n, _, b, _ in p for _ in strings(n, b))
    done = sum(len(new) for *_, new in p)
    print(f'{done}/{total} translated, {len(bad)} problems')
    if bad and cmd != 'check': sys.exit(1)
    if cmd == 'build':
        with open(sys.argv[4], 'w') as f:
            f.write('// generated by re/lang.py from re/lang/en.txt, do not edit\n')
            for a, n, _, b, new in p:
                for o, (s, t) in sorted(new.items()):
                    f.write(f'{{ "{a}", "{n}", {o:#x}, {len(s)}, {fnv(s):#010x}u, {len(t)}, "{c_str(t)}" }},\n')
        print('wrote', sys.argv[4], done, 'patches')
    elif cmd == 'apply':
        os.makedirs(sys.argv[4], exist_ok=True)
        for a in ARCHIVES:
            d = bytearray(open(os.path.join(moddir, a), 'rb').read())
            for aa, n, eo, b, new in p:
                if aa == a and new:
                    d[eo:eo + len(b)] = rebuild(n, b, {o: t for o, (s, t) in new.items()})
            open(os.path.join(sys.argv[4], a), 'wb').write(d)
        print('wrote', sys.argv[4])

if __name__ == '__main__':
    main()
