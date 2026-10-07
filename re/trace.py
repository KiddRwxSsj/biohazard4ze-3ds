#!/usr/bin/env python3
# runs bio4_brew.mod under unicorn with stub brew objects, logs every api hit.
# dev tool only, the real shim lives in src/
import json, os, struct, sys, time
from unicorn import *
from unicorn.arm_const import *

GAME = os.environ.get('GAME', '../private/game/mod/276675')   # zeebo dump, not in the repo
WORK = os.environ.get('WORK', '../private/re-work')
MODF = os.path.join(GAME, 'bio4_brew.mod')
APP_CLSID = 0x0108AF6C
MOD = 0x00100000
HEAP, HEAPSZ = 0x10000000, 48 << 20
STACK, STACKSZ = 0x20000000, 1 << 20
OBJ, OBJSZ = 0x30000000, 4 << 20
TRAP, TRAPSZ = 0x40000000, 1 << 20
RET = TRAP  # first trap word = return magic

SLOTS = json.load(open('slots.json'))
VERBOSE = '-v' in sys.argv

uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
uc.ctl_set_cpu_model(UC_CPU_ARM_1136)

data = open(MODF, 'rb').read()
img_end = max(len(data), 0x200 + 0x74fc0 + 0x350 + 0x2880)
uc.mem_map(MOD - 0x1000, (img_end + 0x1fff) & ~0xfff)
uc.mem_write(MOD, data)
for b, s in ((HEAP, HEAPSZ), (STACK, STACKSZ), (OBJ, OBJSZ), (TRAP, TRAPSZ)):
    uc.mem_map(b, s)
uc.mem_write(TRAP, struct.pack('<I', 0xe12fff1e) * (TRAPSZ // 4))  # bx lr everywhere

R = lambda r: uc.reg_read(r)
def rd(a): return struct.unpack('<I', uc.mem_read(a, 4))[0]
def wr(a, v): uc.mem_write(a, struct.pack('<I', v & 0xffffffff))
def cstr(a, n=4096):
    if not a: return None
    out = bytearray()
    while len(out) < n:
        try: chunk = bytes(uc.mem_read(a + len(out), 64 - ((a + len(out)) & 63)))
        except UcError: break
        z = chunk.find(0)
        if z >= 0: out += chunk[:z]; break
        out += chunk
    return out.decode('latin1')
ABASE = [0]
def arg(i):
    i += ABASE[0]
    if i < 4: return R(UC_ARM_REG_R0 + i)
    return rd(R(UC_ARM_REG_SP) + 4 * (i - 4))

traps = {}
ntrap = [TRAP + 4]
def mktrap(name, fn):
    a = ntrap[0]; ntrap[0] += 4
    traps[a] = (name, fn)
    return a

objtop = [OBJ]
def oalloc(n):
    a = objtop[0]; objtop[0] += (n + 7) & ~7
    return a

# heap: dumb bump, free is a no-op. enough to see the boot path
heaptop = [HEAP]
sizes = {}
def malloc(n):
    a = heaptop[0]; heaptop[0] += (n + 15) & ~15
    if heaptop[0] > HEAP + HEAPSZ: raise RuntimeError('heap out')
    uc.mem_write(a, b'\0' * n); sizes[a] = n
    return a

hits = {}
def note(k):
    hits[k] = hits.get(k, 0) + 1

def mkobj(iface, impl=None, extra=0x40, tag=None):
    names = SLOTS[iface]
    vt = oalloc(4 * len(names))
    for i, n in enumerate(names):
        f = (impl or {}).get(n)
        if f is not None and iface == 'GLES' and getattr(f, '__name__', '') == 'h' and n not in ('GenTextures',):
            pass
        wr(vt + 4 * i, mktrap(f'{tag or iface}.{n}', f))
    o = oalloc(4 + extra)
    wr(o, vt)
    return o

# --- printf for sprintf/dbgprintf. va is a callable returning next word
def fmt(f, nxt):
    out, i = [], 0
    while i < len(f):
        c = f[i]
        if c != '%': out.append(c); i += 1; continue
        j = i + 1
        while j < len(f) and f[j] in '-+ #0123456789.lh': j += 1
        spec, conv = f[i:j], f[j] if j < len(f) else ''
        i = j + 1
        if conv == '%': out.append('%'); continue
        if '*' in spec: spec = spec.replace('*', str(nxt()))
        spec = spec.replace('l', '').replace('h', '')
        if conv == 's': out.append((spec + 's') % (cstr(nxt()) or '(null)'))
        elif conv in 'di': v = nxt(); out.append((spec + 'd') % (v - (1 << 32) if v & 0x80000000 else v))
        elif conv in 'uxXoc': out.append((spec + conv) % nxt())
        elif conv == 'p': out.append('%x' % nxt())
        elif conv in 'fge': out.append('<flt>'); nxt(); nxt()
        else: out.append(spec + conv)
    return ''.join(out)

def va_regs(first):
    st = {'i': first}
    def nxt():
        v = arg(st['i']); st['i'] += 1; return v
    return nxt
def va_ptr(p):
    st = {'p': p}
    def nxt():
        v = rd(st['p']); st['p'] += 4; return v
    return nxt

def h_sprintf():
    s = fmt(cstr(arg(1)), va_regs(2)).encode('latin1') + b'\0'
    uc.mem_write(arg(0), s); return len(s) - 1
def h_snprintf():
    n = arg(1); s = fmt(cstr(arg(2)), va_regs(3)).encode('latin1')
    if n: uc.mem_write(arg(0), s[:n - 1] + b'\0')
    return len(s)
def h_vsnprintf():
    n = arg(1); s = fmt(cstr(arg(2)), va_ptr(rd(arg(3)))).encode('latin1')
    if n: uc.mem_write(arg(0), s[:n - 1] + b'\0')
    return len(s)
def h_vsprintf():
    s = fmt(cstr(arg(1)), va_ptr(rd(arg(2)))).encode('latin1') + b'\0'
    uc.mem_write(arg(0), s); return len(s) - 1
def h_dbgprintf():
    print('  dbg:', fmt(cstr(arg(0)), va_regs(1))); return 0

def h_memmove():
    d, s, n = arg(0), arg(1), arg(2)
    if n: uc.mem_write(d, bytes(uc.mem_read(s, n)))
    return d
def h_memset():
    d, c, n = arg(0), arg(1), arg(2)
    if n: uc.mem_write(d, bytes([c & 0xff]) * n)
    return d
def h_strlen(): return len(cstr(arg(0), 4096))
def h_strcpy():
    s = cstr(arg(1), 4096).encode('latin1') + b'\0'; uc.mem_write(arg(0), s); return arg(0)
def h_strcat():
    a = arg(0); h = cstr(a, 4096)
    uc.mem_write(a + len(h), cstr(arg(1), 4096).encode('latin1') + b'\0'); return a
def h_strcmp():
    a, b = cstr(arg(0), 4096), cstr(arg(1), 4096); return (a > b) - (a < b)
def h_memcmp():
    a, b = bytes(uc.mem_read(arg(0), arg(2))), bytes(uc.mem_read(arg(1), arg(2))); return (a > b) - (a < b)
live = [0, 0]
def h_malloc():
    a = malloc(arg(0)); live[0] += arg(0); live[1] = max(live[1], live[0]); return a
def h_free():
    live[0] -= sizes.pop(arg(0), 0); return 0
def h_realloc():
    p, n = arg(0), arg(1)
    q = malloc(n)
    if p: uc.mem_write(q, bytes(uc.mem_read(p, min(n, sizes.get(p, n)))))
    return q

def wstr(a, n=2048):
    out = []
    while a and len(out) < n:
        c = struct.unpack('<H', uc.mem_read(a + 2 * len(out), 2))[0]
        if not c: break
        out.append(chr(c))
    return ''.join(out)
def put(a, b): uc.mem_write(a, b); return a
def h_strncmp():
    a, b = cstr(arg(0))[:arg(2)], cstr(arg(1))[:arg(2)]; return (a > b) - (a < b)
def h_stricmp():
    a, b = cstr(arg(0)).lower(), cstr(arg(1)).lower(); return (a > b) - (a < b)
def h_strnicmp():
    a, b = cstr(arg(0)).lower()[:arg(2)], cstr(arg(1)).lower()[:arg(2)]; return (a > b) - (a < b)
def h_strncpy():
    s_ = cstr(arg(1)).encode('latin1')[:arg(2)]; put(arg(0), s_ + b'\0' * (arg(2) - len(s_))); return arg(0)
def h_strchr():
    s_ = cstr(arg(0)); i = s_.find(chr(arg(1) & 0xff)) if arg(1) & 0xff else len(s_); return arg(0) + i if i >= 0 else 0
def h_strrchr():
    s_ = cstr(arg(0)); i = s_.rfind(chr(arg(1) & 0xff)); return arg(0) + i if i >= 0 else 0
def h_strstr():
    i = cstr(arg(0)).find(cstr(arg(1))); return arg(0) + i if i >= 0 else 0
def h_memchr():
    b = bytes(uc.mem_read(arg(0), arg(2))); i = b.find(arg(1) & 0xff); return arg(0) + i if i >= 0 else 0
def h_atoi():
    import re as r_; m = r_.match(r'\s*[-+]?\d+', cstr(arg(0))); return int(m.group()) if m else 0
def h_strexpand():
    # strexpand(const byte *src, int n, AECHAR *dst, int dstbytes)
    b = bytes(uc.mem_read(arg(0), arg(1))); m = arg(3) // 2 - 1
    put(arg(2), b''.join(struct.pack('<H', c) for c in b[:m]) + b'\0\0'); return arg(2)
def h_utf8towstr():
    # utf8towstr(const byte *src, int n, AECHAR *dst, int dstbytes)
    t = bytes(uc.mem_read(arg(0), arg(1))).decode('utf8', 'replace'); m = arg(3) // 2 - 1
    put(arg(2), t[:m].encode('utf-16-le') + b'\0\0'); return 1
def h_wstrtostr():
    t = wstr(arg(0)).encode('latin1', 'replace')[:arg(2) - 1]; put(arg(1), t + b'\0'); return arg(1)
def h_strtowstr():
    t = cstr(arg(0))[:arg(2) // 2 - 1]; put(arg(1), t.encode('utf-16-le') + b'\0\0'); return arg(1)
def h_wstrlen(): return len(wstr(arg(0)))
def h_wstrcpy(): put(arg(0), wstr(arg(1)).encode('utf-16-le') + b'\0\0'); return arg(0)
def h_getrand(): put(arg(0), os.urandom(arg(1))); return 0
def h_strdup():
    b = cstr(arg(0)).encode('latin1') + b'\0'; return put(malloc(len(b)), b)
T0 = time.monotonic()
ms = lambda: int((time.monotonic() - T0) * 1000)

HELP_IMPL = {
    'memmove': h_memmove, 'memset': h_memset, 'strcpy': h_strcpy, 'strcat': h_strcat,
    'strcmp': h_strcmp, 'strlen': h_strlen, 'sprintf': h_sprintf, 'malloc': h_malloc,
    'free': h_free, 'realloc': h_realloc, 'memcmp': h_memcmp, 'dbgprintf': h_dbgprintf,
    'snprintf': h_snprintf, 'vsnprintf': h_vsnprintf, 'vsprintf': h_vsprintf,
    'aee_GetTimeMS': ms, 'aee_GetUpTimeMS': ms, 'aee_GetSeconds': lambda: int(time.time()) - 315964800,
    'GetAEEVersion': lambda: 0,
    'strncmp': h_strncmp, 'stricmp': h_stricmp, 'strnicmp': h_strnicmp, 'strncpy': h_strncpy,
    'strchr': h_strchr, 'strrchr': h_strrchr, 'strstr': h_strstr, 'memchr': h_memchr, 'atoi': h_atoi,
    'strexpand': h_strexpand, 'utf8towstr': h_utf8towstr, 'wstrtostr': h_wstrtostr,
    'strtowstr': h_strtowstr, 'wstrlen': h_wstrlen, 'wstrcpy': h_wstrcpy, 'aee_GetRand': h_getrand,
    'strdup': h_strdup,
    'GetAppInstance': lambda: cur_app[0] or rd(app_out),
}
cur_app = [0]
import re as _re
HELPERS = _re.findall(r'"(\w+)"', open('../tools/zeebx-emu/src/brew/aee_helpers.rs').read())
helptab = oalloc(4 * len(HELPERS))
for i, n in enumerate(HELPERS):
    wr(helptab + 4 * i, mktrap('helper.' + n, HELP_IMPL.get(n)))
wr(MOD - 8, 1)
wr(MOD - 4, helptab)

# --- shell
timers = []      # (due_ms, pfn, user)
resumes = []     # AEECallback*
def sh_create():
    cls, out = arg(1), arg(2)
    iface = CLASSES.get(cls)
    print(f'  CreateInstance {cls:#010x} -> {iface and iface[0]}')
    if not iface:
        if out: wr(out, 0)
        return 0x0E  # ECLASSNOTSUPPORT
    o = mkobj(iface[0], iface[1], tag=f'{iface[0]}@{cls:08x}')
    wr(out, o); return 0
def sh_settimer():
    pfn, u = arg(2), arg(3)
    if pfn and pfn == u:  # SetTimerEx passes AEECallback* twice
        pfn, u = rd(u + 16), rd(u + 20)
    timers.append((ms() + arg(1), pfn, u)); return 0
def sh_canceltimer():
    pfn, u = arg(1), arg(2)
    timers[:] = [t for t in timers if not (t[1] == pfn and (u == 0 or t[2] == u))]; return 0
def sh_resume():
    cb = arg(1)
    if cb not in resumes: resumes.append(cb)
    wr(cb + 8, cancel_trap)  # pfnCancel
    wr(cb + 12, cb)
    return 0
def cb_cancel():
    cb = arg(0)
    if cb in resumes: resumes.remove(cb)
    wr(cb + 8, 0); return 0
cancel_trap = mktrap('cb.cancel', cb_cancel)

def sh_devinfo():
    p = arg(1)
    # AEEDeviceInfo: cxScreen, cyScreen, cxAltScreen, cyAltScreen, scrollbar, wEncoding...
    # just the size bits, game logs the rest
    # 8 x uint16 head of AEEDeviceInfo, then dwRAM at +24
    uc.mem_write(p, b'\0' * 44)
    uc.mem_write(p, struct.pack('<8H', 640, 480, 0, 0, 8, 0, 0, 16))
    wr(p + 24, HEAPSZ)
    return 0
SHELL_IMPL = {'CreateInstance': sh_create, 'SetTimer': sh_settimer, 'CancelTimer': sh_canceltimer,
              'Resume': sh_resume, 'GetDeviceInfo': sh_devinfo, 'AddRef': lambda: 1, 'Release': lambda: 0}
signals = {}
def sig_create():
    ctl = mkobj('SIGNALCTL', {'Set': sig_set, 'Enable': lambda: 0}, tag='SIGNALCTL')
    signals[ctl] = (arg(1), arg(2))
    if arg(3): wr(arg(3), ctl)
    if arg(4): wr(arg(4), ctl)
    print(f'  signal {ctl:#x} -> {arg(1):#x}({arg(2):#x})')
    return 0
pending_sig = []
def sig_set():
    pending_sig.append(signals[arg(0)]); return 0
JOY = 0x0106c3fd
def hid_conn():
    # GetConnectedDevices(this, type, handles*, len, *lenReq)
    n = 1 if arg(1) == JOY else 0
    if arg(4): wr(arg(4), n)
    if n and arg(2) and arg(3): wr(arg(2), 1)
    print(f'  hid devices type={arg(1):#x} -> {n}')
    return 0
def hid_info():
    uc.mem_write(arg(2), struct.pack('<IHHI', JOY, 0, 0, 0)); return 0
def hid_create():
    d = mkobj('HIDDEVICE', HIDDEV_IMPL, tag='HIDDEV'); wr(arg(2), d); return 0
BTN = [0x0106c40b, 0x0106c40a, 0x0106c40c, 0x0106c40d, 0x0106c408, 0x0106c407, 0x0106c406,
       0x0106c409, 0x0106c405, 0x0106c403, 0x0106c404, 0x0106c402, 0x0106c3fe, 0x0106c3ff,
       0x0106c400, 0x0106c401]
padq = []
hidsig = {}
axes = [128, 128, 128, 128]
def hd_reg(kind):
    def h():
        hidsig[kind] = arg(1)
        if kind == 'pos': pending_sig.append(signals[arg(1)])  # hw reports first position right away
        return 0
    return h
def hd_next():
    if not padq:
        uc.mem_write(arg(1), b'\0' * 20); return 1  # EFAILED
    i, down = padq.pop(0)
    uc.mem_write(arg(1), struct.pack('<IIIII', i, down, BTN[i], 0, 1))
    if arg(2): wr(arg(2), ms())
    if arg(3): wr(arg(3), 0)
    print(f'  pad event {i} {down}')
    return 0
def hd_pos():
    w = [128] * 25; w[0] = 0
    for slot, v in zip((1, 2, 3, 6), axes): w[slot] = v
    uc.mem_write(arg(1), struct.pack('<25I', *w)); return 0
def hd_range(v):
    def h():
        w = [v] * 25; w[0] = 0; uc.mem_write(arg(1), struct.pack('<25I', *w)); return 0
    return h
def hd_axes():
    w = [0] * 25
    for slot, u in zip((1, 2, 3, 6), (0x0106c4d0, 0x0106c4d1, 0x0106c4ce, 0x0106c4cf)): w[slot] = u
    uc.mem_write(arg(1), struct.pack('<25I', *w)); return 0
def hlog(n):
    def h():
        print(f'  hiddev.{n} {[hex(arg(i)) for i in range(1, 5)]}'); return 0
    return h
HIDDEV_IMPL = {n: hlog(n) for n in SLOTS['HIDDEVICE']}
HIDDEV_IMPL.update({'AddRef': lambda: 1, 'Release': lambda: 0,
    'RegisterForButtonEvent': hd_reg('btn'), 'RegisterForPositionChange': hd_reg('pos'),
    'GetNextButtonEvent': hd_next, 'GetPositionState': hd_pos, 'GetMinPositionInfo': hd_range(0),
    'GetMaxPositionInfo': hd_range(255), 'GetAxesInfo': hd_axes})
import os
ROOT = GAME
SAVE = os.path.join(WORK, 'save')
os.makedirs(SAVE, exist_ok=True)
files = {}
def gpath(a):
    g = cstr(a)
    n = g.replace('fs:/~/', '').replace('\\', '/').lstrip('/')
    for base in (SAVE, ROOT):
        if os.path.exists(os.path.join(base, n)): return g, os.path.join(base, n)
    return g, os.path.join(SAVE, n)
def fm_open():
    g, hp = gpath(arg(1)); mode = arg(2)
    try:
        if mode & 4: os.makedirs(os.path.dirname(hp) or '.', exist_ok=True); f = open(hp, 'w+b')
        elif mode & 2: f = open(hp, 'r+b')
        else: f = open(hp, 'rb')
    except OSError:
        print(f'  open {g!r} mode {mode} FAIL'); return 0
    print(f'  open {g!r} mode {mode} -> {hp}')
    o = mkobj('FILE', FILE_IMPL, tag='FILE')
    files[o] = f; return o
def fm_test():
    g, hp = gpath(arg(1)); ok = os.path.exists(hp)
    print(f'  test {g!r} {ok}'); return 0 if ok else 1
def f_read():
    f = files[arg(0)]; b = f.read(arg(2))
    if b: uc.mem_write(arg(1), b)
    return len(b)
def f_write():
    files[arg(0)].write(bytes(uc.mem_read(arg(1), arg(2)))); return arg(2)
def f_seek():
    f = files[arg(0)]; how, pos = arg(1), arg(2)
    pos = pos - (1 << 32) if pos & 0x80000000 else pos
    if how == 2 and pos == 0: return f.tell()
    f.seek(pos, {0: 0, 1: 2, 2: 1}[how]); return 0
def f_info():
    f = files[arg(0)]; size = os.fstat(f.fileno()).st_size
    wr(arg(1) + 8, size); return 0
def f_release():
    f = files.pop(arg(0), None)
    if f: f.close()
    return 0
FILE_IMPL = {'Read': f_read, 'Write': f_write, 'Seek': f_seek, 'GetInfo': f_info, 'Release': f_release}
swaps = [0]
# legacy IEGL/IGL slots: plain egl/gl signatures, no this
def egl_choose():
    if arg(2): wr(arg(2), 1)
    if arg(4): wr(arg(4), 1)
    return 1
def egl_init():
    if arg(1): wr(arg(1), 1)
    if arg(2): wr(arg(2), 0)
    return 1
def egl_query():
    v = {0x3057: 640, 0x3056: 480}.get(arg(2), 0)
    if arg(3): wr(arg(3), v)
    return 1
DUMP_EVERY = int(os.environ.get('DUMP_EVERY', 60))
FRAMES = os.path.join(WORK, 'frames')
os.makedirs(FRAMES, exist_ok=True)
icount = [0, None, 0]   # instr since last swap, hook handle, frames measured
fstats = []
def blk(uc_, addr, size, _):
    if addr < TRAP: icount[0] += size >> 2
MIDI = os.path.join(WORK, 'midi')
os.makedirs(MIDI, exist_ok=True)
midis = {}
def scan_midi():
    mem = bytes(uc.mem_read(HEAP, heaptop[0] - HEAP))
    i = mem.find(b'MThd')
    while i >= 0:
        o = i + 14
        ntr = struct.unpack('>H', mem[i + 10:i + 12])[0]
        ok = True
        for _ in range(ntr):
            if mem[o:o + 4] != b'MTrk': ok = False; break
            o += 8 + struct.unpack('>I', mem[o + 4:o + 8])[0]
        if ok:
            blob = mem[i:o]
            import hashlib
            h = hashlib.md5(blob).hexdigest()[:10]
            if h not in midis:
                midis[h] = len(blob)
                open(f'{MIDI}/{h}.mid', 'wb').write(blob)
                print(f'  midi {h} {len(blob)} bytes')
        i = mem.find(b'MThd', i + 4)
def egl_swap():
    swaps[0] += 1
    if swaps[0] % 150 == 0: scan_midi()
    if icount[1] is not None:
        fstats.append(icount[0]); icount[0] = 0
        if len(fstats) % 30 == 0:
            import statistics
            print(f'  instr/frame last30: mean {statistics.mean(fstats[-30:])/1e6:.2f}M max {max(fstats[-30:])/1e6:.2f}M (swap {swaps[0]})')
    if G.capture:
        Image.fromarray(G.render()).save(f'{FRAMES}/f{swaps[0]:05d}.png'); G.capture = False
        print(f'  dumped frame {swaps[0]} ({len(G.draws)} draws)')
        if '-d' in sys.argv:
            import numpy as np
            np.set_printoptions(precision=3, suppress=True, linewidth=150)
            for d in G.draws[:6]:
                if d[0] != 'tri': print('   ', d); continue
                _, clip, tc, col, color, tex, env, en, blend, alphaf, vp, tris = d
                print('    clip', clip[:4].tolist(), 'tc', None if tc is None else tc[:4].tolist(), 'tex', None if tex is None else tex.shape,
                      'color', color, 'en', sorted(hex(e) for e in en), 'vp', vp, 'env', hex(env))
    G.draws = []
    if swaps[0] % 60 == 0:
        print(f'  frame {swaps[0]} bytes {G.fbytes} verts {G.fverts} regions {sorted(hex(x << 20) for x in G.ptrs)}')
    G.fbytes = 0; G.fverts = 0; G.ptrs = set()
    if (swaps[0] + 1) % DUMP_EVERY == 0: G.capture = True
    if swaps[0] % 30 == 0: print(f'  swap {swaps[0]} t={ms()}')
    return 1
EGL_IMPL = {'eglGetDisplay': lambda: 1, 'eglInitialize': egl_init, 'eglChooseConfig': egl_choose,
            'eglGetConfigAttrib': lambda: 1, 'eglCreateWindowSurface': lambda: 2,
            'eglCreateContext': lambda: 3, 'eglMakeCurrent': lambda: 1, 'eglSwapBuffers': egl_swap,
            'eglQuerySurface': egl_query, 'eglGetError': lambda: 0x3000, 'eglSwapIntervalOES': lambda: 1,
            'AddRef': lambda: 1, 'Release': lambda: 0}
texn = [0]
def gl_gen():
    for i in range(arg(0)):
        texn[0] += 1; wr(arg(1) + 4 * i, texn[0])
    return 0
glstr = [0]
def gl_getstring():
    if not glstr[0]: glstr[0] = put(malloc(32), b'OpenGL ES-CM 1.0\0')
    return glstr[0]
def gl_getint():
    v = {0x0D33: 1024, 0x84E2: 2, 0x0D32: 8, 0x86A2: 0}.get(arg(0), 0)
    print(f'  glGetIntegerv {arg(0):#x} -> {v}'); wr(arg(1), v); return 0
GL_IMPL = {n: (lambda: 0) for n in SLOTS['GL_LEGACY']}
GL_IMPL.update({'glGenTextures': gl_gen, 'glGenBuffers': gl_gen, 'glGetString': gl_getstring,
                'glGetIntegerv': gl_getint, 'AddRef': lambda: 1})
fb = [0]
def disp_devbmp():
    b = mkobj('BITMAP', {'AddRef': lambda: 1, 'Release': lambda: 0, 'GetInfo': bmp_info,
                         'QueryInterface': bmp_qi}, tag='BITMAP')
    # IDIB view of the same object: pBmp, cx, cy, pitch, depth, 565 scheme
    if not fb[0]: fb[0] = malloc(640 * 480 * 2)
    wr(b + 8, fb[0])
    uc.mem_write(b + 20, struct.pack('<HHhHBB', 640, 480, 1280, 0, 16, 16))
    wr(arg(1), b); return 0
def bmp_qi():
    print(f'  bitmap QI {arg(1):#x}')
    if arg(1) in (0x01001045, 0x0100102c, 0x01001021): wr(arg(2), arg(0)); return 0
    wr(arg(2), 0); return 0x0E
def bmp_info():
    # AEEBitmapInfo { uint32 cx, cy, nDepth }
    uc.mem_write(arg(1), struct.pack('<III', 640, 480, 16)); return 0
DISPLAY_IMPL = {'GetDeviceBitmap': disp_devbmp, 'AddRef': lambda: 1, 'Release': lambda: 0}
for n in ('SetColor', 'SetFont', 'DrawText', 'DrawRect', 'Update'): DISPLAY_IMPL[n] = lambda: 0
# QEGL: same args as egl*, this in r0, result through trailing out ptr, returns aee err
QARGC = {'GetError': 0, 'GetDisplay': 1, 'Initialize': 3, 'Terminate': 1, 'ChooseConfig': 5,
         'GetConfigs': 4, 'GetConfigAttrib': 4, 'CreateWindowSurface': 4, 'CreateContext': 4,
         'MakeCurrent': 4, 'SwapBuffers': 2, 'QuerySurface': 4, 'DestroyContext': 2,
         'DestroySurface': 2, 'SwapInterval': 2, 'QueryString': 2, 'GetCurrentContext': 0,
         'GetCurrentSurface': 1, 'GetCurrentDisplay': 0, 'WaitGL': 0, 'WaitNative': 1}
def qegl(name):
    f = EGL_IMPL.get('egl' + name)
    def h():
        ABASE[0] = 1
        try:
            v = f() if f else 1
            if name not in QARGC: print(f'  ?? QEGL.{name} no argc'); return 0
            out = arg(QARGC[name])
            if out: wr(out, v)
        finally:
            ABASE[0] = 0
        return 0
    return h
QEGL_IMPL = {n: qegl(n) for n in SLOTS['EGL'][3:]}
gles_obj = [0]
def shifted(f, outidx=None):
    def h():
        ABASE[0] = 1
        try:
            v = f()
            if outidx is not None and arg(outidx): wr(arg(outidx), v)
        finally: ABASE[0] = 0
        return 0
    return h
glcount = {}
def gles_any(n):
    def h():
        glcount[n] = glcount.get(n, 0) + 1; return 0
    return h
GLES_IMPL = {n: gles_any(n) for n in SLOTS['GLES']}
GLES_IMPL.update({'AddRef': lambda: 1, 'Release': lambda: 0,
                  'GenTextures': shifted(gl_gen), 'GenBuffers': shifted(gl_gen),
                  'GetIntegerv': shifted(gl_getint), 'GetString': shifted(gl_getstring, 1),
                  'GetError': lambda: wr(arg(1), 0) or 0})
texfmts = {}
def gl_ctex():
    ABASE[0] = 1
    try:
        t, lvl, fmt_, w, h, b, sz, d = (arg(i) for i in range(8))
    finally: ABASE[0] = 0
    k = (hex(fmt_), w, h, lvl)
    texfmts[k] = texfmts.get(k, 0) + 1
    if texfmts[k] == 1: print(f'  ctex fmt={fmt_:#x} {w}x{h} lvl={lvl} size={sz} head={bytes(uc.mem_read(d, 16)).hex()}')
    glcount['CompressedTexImage2D'] = glcount.get('CompressedTexImage2D', 0) + 1
    return 0
import glsw
from PIL import Image
G = glsw.GL(lambda a, n: bytes(uc.mem_read(a, n)))
G.lr = lambda: R(UC_ARM_REG_LR)
NARGS = {'Translatex': 3, 'Scalex': 3, 'Orthox': 6, 'Frustumx': 6, 'Viewport': 4, 'VertexPointer': 4,
         'TexCoordPointer': 4, 'ColorPointer': 4, 'NormalPointer': 3, 'Color4x': 4, 'Color4ub': 4,
         'ClearColorx': 4, 'BlendFunc': 2, 'AlphaFuncx': 2, 'BindTexture': 2, 'TexEnvx': 3,
         'CompressedTexImage2D': 8, 'TexImage2D': 9, 'DrawArrays': 3, 'DrawElements': 4,
         'MatrixMode': 1, 'LoadMatrixx': 1, 'MultMatrixx': 1, 'ActiveTexture': 1, 'ClientActiveTexture': 1,
         'Enable': 1, 'Disable': 1, 'EnableClientState': 1, 'DisableClientState': 1, 'Clear': 1,
         'LoadIdentity': 0, 'PushMatrix': 0, 'PopMatrix': 0}
def glroute(n, f):
    k = NARGS[n]
    def h():
        glcount[n] = glcount.get(n, 0) + 1
        ABASE[0] = 1
        try: a = [arg(i) for i in range(k)]
        finally: ABASE[0] = 0
        f(*a); return 0
    return h
for n in NARGS:
    GLES_IMPL[n] = glroute(n, getattr(G, n))
def egl_qi():
    iid, out = arg(1), arg(2)
    if iid in (0x0103d8dd, 0x0103d8ea):
        if not gles_obj[0]: gles_obj[0] = mkobj('GLES', GLES_IMPL, tag='GLES')
        wr(out, gles_obj[0]); return 0
    if iid in (0x0103d8ed, 0x0103d8ee): wr(out, arg(0)); return 0
    print(f'  QEGL QI {iid:#x} declined'); wr(out, 0); return 0x0E
QEGL_IMPL.update({'AddRef': lambda: 1, 'Release': lambda: 0, 'QueryInterface': egl_qi})
def media_log(n):
    def h():
        a = [arg(i) for i in range(6)]
        if n in ('SetMediaParm', 'GetMediaParm', 'RegisterNotify', 'Play', 'Stop'):
            extra = ''
            if n == 'SetMediaParm' and a[1] == 1:  # MM_PARM_MEDIA_DATA: AEEMediaData*
                cls_, pdata, sz = rd(a[2]), rd(a[2] + 4), rd(a[2] + 8)
                extra = f' data cls={cls_:#x} p={pdata:#x} size={sz}'
                if sz and pdata: extra += ' head=' + bytes(uc.mem_read(pdata, 32)).hex()
            print(f'  media{a[0]:#x}.{n} {[hex(x) for x in a[1:]]}{extra}')
        return 0
    return h
MEDIA_IMPL = {n: media_log(n) for n in SLOTS['MEDIA']}
mnotify = {}
mdone = []
def m_reg():
    mnotify[arg(0)] = (arg(1), arg(2)); return 0
def m_play():
    o = arg(0)
    print(f'  PLAY {o:#x} at {ms()}')
    mdone.append((ms() + 5000, o))
    return 0
def fire_done():
    now = ms()
    for t, o in list(mdone):
        if t <= now and o in mnotify:
            mdone.remove((t, o))
            blk_ = malloc(28)
            uc.mem_write(blk_, struct.pack('<7I', 0, o, 4, 0, 2, 0, 0))
            print(f'  DONE -> {o:#x}')
            call(mnotify[o][0], mnotify[o][1], blk_)
MEDIA_IMPL['RegisterNotify'] = m_reg
MEDIA_IMPL['Play'] = m_play
CLASSES = {
    0x0100550a: ('MEDIA', MEDIA_IMPL), 0x01005511: ('MEDIA', MEDIA_IMPL), 0x01005501: ('MEDIA', MEDIA_IMPL),
    0x01005502: ('MEDIA', MEDIA_IMPL),
    0x0103d8ec: ('EGL', QEGL_IMPL),
    0x01001001: ('DISPLAY', DISPLAY_IMPL), 0x010127d4: ('DISPLAY', DISPLAY_IMPL),
    0x01001003: ('FILEMGR', {'OpenFile': fm_open, 'Test': fm_test, 'Release': lambda: 0}), 0x0106c411: ('HID', {'GetConnectedDevices': hid_conn, 'RegisterForConnectEvents': lambda: 0,
                              'GetDeviceInfo': hid_info, 'CreateDevice': hid_create}),
    0x01014bc4: ('EGL_LEGACY', EGL_IMPL), 0x01014bc3: ('GL_LEGACY', GL_IMPL),
    0x01001056: ('SOUND', {}), 0x01005500: ('MEDIA', {}),
    0x01041207: ('SIGNALCBFACTORY', {'CreateSignal': sig_create}), 0x01001002: ('HEAP', {'CheckAvail': lambda: 1, 'Release': lambda: 0}),
}
shell = mkobj('SHELL', SHELL_IMPL)

def hook(uc_, addr, size, _):
    if addr == RET:
        uc.emu_stop(); return
    t = traps.get(addr)
    if not t: return
    name, fn = t
    note(name)
    if fn is None:
        print(f'  ?? {name}({arg(0):#x}, {arg(1):#x}, {arg(2):#x}, {arg(3):#x}) lr={R(UC_ARM_REG_LR):#x}')
        uc.reg_write(UC_ARM_REG_R0, 0)
        return
    if VERBOSE: print('  ', name)
    try:
        r = fn()
    except Exception as e:
        print(f'!! host {name}: {e!r} args {[hex(arg(i)) for i in range(6)]}'); uc.emu_stop(); raise SystemExit(1)
    uc.reg_write(UC_ARM_REG_R0, (r or 0) & 0xffffffff)
uc.hook_add(UC_HOOK_CODE, hook, begin=TRAP, end=TRAP + TRAPSZ - 1)

def bad(uc_, access, addr, size, value, _):
    print(f'!! bad mem {access} @ {addr:#x} pc={R(UC_ARM_REG_PC):#x} lr={R(UC_ARM_REG_LR):#x}')
    return False
uc.hook_add(UC_HOOK_MEM_INVALID, bad)

def call(fn, *a, budget=200_000_000):
    regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
    sp = STACK + STACKSZ - 0x100
    extra = list(a[4:])
    sp -= 4 * len(extra)
    for i, v in enumerate(extra): wr(sp + 4 * i, v)
    for r, v in zip(regs, a): uc.reg_write(r, v & 0xffffffff)
    uc.reg_write(UC_ARM_REG_SP, sp)
    uc.reg_write(UC_ARM_REG_LR, RET)
    thumb = fn & 1
    try:
        uc.emu_start(fn | thumb, RET, count=budget)
    except UcError as e:
        print(f'!! {e} pc={R(UC_ARM_REG_PC):#x} lr={R(UC_ARM_REG_LR):#x}')
        raise SystemExit(1)
    return R(UC_ARM_REG_R0)

pmod_out = oalloc(8)
app_out = oalloc(8)
print('AEEMod_Load ->', call(MOD + 0x40, shell, helptab, pmod_out))
pmod = rd(pmod_out)
print(f'IModule {pmod:#x}')
print('IModule.CreateInstance ->', call(rd(rd(pmod) + 8), pmod, shell, APP_CLSID, app_out))
app = rd(app_out)
he = rd(rd(app) + 8)
print(f'IApplet {app:#x} HandleEvent {he:#x}')
cur_app[0] = app
disp = mkobj('DISPLAY', {}, tag='DISPLAY@start')
st = malloc(24)
wr(st + 4, APP_CLSID); wr(st + 8, disp)
uc.mem_write(st + 12, struct.pack('<hhhh', 0, 0, 640, 480))
print(f'pfnHandleEvent {rd(app + 24):#x}')
print('EVT_APP_START ->', call(he, app, 0, 0, st))

last_press = [0]
WATCH = int(os.environ.get('WATCH', '0'), 16)
if WATCH:
    seen = {}
    def wh(uc_, acc, addr, size, val, _):
        pc = R(UC_ARM_REG_PC)
        k = (pc, addr - WATCH)
        if k not in seen:
            seen[k] = 1; print(f'  watch {"W" if acc == UC_MEM_WRITE else "R"} +{addr - WATCH:#x} pc={pc:#x} val={val:#x}')
    uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, wh, begin=WATCH, end=WATCH + 0x40)
# SCRIPT: list of (ms, button, down). default walks every button
SCRIPT = []
for k, b in enumerate([0] * int(os.environ.get('PRESSES', 12))):
    t0 = 7000 + k * 1200
    SCRIPT += [(t0, b, 1), (t0 + 300, b, 0)]
def script_tick():
    now = ms()
    while SCRIPT and SCRIPT[0][0] <= now:
        _, b, d = SCRIPT.pop(0)
        padq.append((b, d))
        print(f'  [{now}] press {b} {d} (swap {swaps[0]})')
        if 'btn' in hidsig:
            pfn, cx = signals[hidsig['btn']]
            call(pfn, cx)
frames = int(next((a for a in sys.argv[1:] if a.isdigit()), '50'))
for f in range(frames):
    now = ms()
    due = [t for t in timers if t[0] <= now]
    timers[:] = [t for t in timers if t[0] > now]
    for _, pfn, u in due:
        call(pfn, u)
    for cb in list(resumes):
        resumes.remove(cb)
        wr(cb + 8, 0)
        call(rd(cb + 16), rd(cb + 20))
    while pending_sig:
        pfn, cx = pending_sig.pop(0); call(pfn, cx)
    script_tick()
    fire_done()
    if icount[1] is None and swaps[0] >= int(os.environ.get('COUNT_FROM', 10**9)):
        icount[1] = uc.hook_add(UC_HOOK_BLOCK, blk); icount[0] = 0
    if not due and not timers and not resumes:
        print('idle, nothing scheduled'); break
    time.sleep(0.001)

print(f'\nheap live now {live[0]} peak {live[1]} bump {heaptop[0] - HEAP}')
print('\nglstats:')
for k, v in sorted(G.stats.items(), key=lambda x: -x[1]): print(f'  {v:7d} {k}')
print('\ngl:')
for k, v in sorted(glcount.items(), key=lambda x: -x[1]): print(f'  {v:7d} {k}')
print('\nhits:')
for k, v in sorted(hits.items()): print(f'  {v:6d} {k}')
