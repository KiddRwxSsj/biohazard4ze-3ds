#!/usr/bin/env python3
# headless libretro runner for the azahar core: boots the elf, scripted input, png dumps
# usage: lr.py re4z.elf frames [--every N] [--press frame:btn:len ...] [--stick frame:LX|LY|RX|RY:value:len ...]
# stick values are -32768..32767, right stick is the n3ds c-stick (run with N3DS='New 3DS')
import ctypes as C, os, sys, time
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
CORE = os.path.join(HERE, 'dl/azahar_lr/azahar_libretro.so')
RT = os.path.join(HERE, 'lr_rt')
OUT = os.path.join(HERE, 'lr_frames')
os.makedirs(RT, exist_ok=True)
os.makedirs(OUT, exist_ok=True)

OPTS = {
    b'citra_graphics_api': b'Software',
    b'citra_use_cpu_jit': b'enabled',
    b'citra_is_new_3ds': os.environ.get('N3DS', 'Old 3DS').encode(),
    b'citra_layout_option': b'Default Top-Bottom Screen',
    b'citra_use_virtual_sd': b'enabled',
    b'citra_use_libretro_save_path': b'LibRetro Default',
    b'citra_audio_emulation': b'HLE',
    b'citra_use_hw_shader': b'disabled',
    b'citra_resolution_factor': b'1x (Native)',
    b'citra_region_value': b'Auto',
}

class Var(C.Structure): _fields_ = [('key', C.c_char_p), ('value', C.c_char_p)]
class GameInfo(C.Structure): _fields_ = [('path', C.c_char_p), ('data', C.c_void_p), ('size', C.c_size_t), ('meta', C.c_char_p)]

ENV = C.CFUNCTYPE(C.c_bool, C.c_uint, C.c_void_p)
VIDEO = C.CFUNCTYPE(None, C.c_void_p, C.c_uint, C.c_uint, C.c_size_t)
AUDIO = C.CFUNCTYPE(None, C.c_int16, C.c_int16)
AUDIOB = C.CFUNCTYPE(C.c_size_t, C.c_void_p, C.c_size_t)
POLL = C.CFUNCTYPE(None)
STATE = C.CFUNCTYPE(C.c_int16, C.c_uint, C.c_uint, C.c_uint, C.c_uint)
LOGP = C.CFUNCTYPE(None, C.c_int, C.c_char_p)

core = C.CDLL(CORE)
keep = []
rt_c = C.c_char_p(RT.encode())
last = {'img': None}
held = set()
pixfmt = [0]
verbose = '-v' in sys.argv

class LogCb(C.Structure): _fields_ = [('log', C.c_void_p)]

def logfn(level, msg):
    if verbose or level >= 2: sys.stdout.write('core: ' + msg.decode(errors='replace'))
# variadic printf style in C, we just take the format string
LOGFN = C.CFUNCTYPE(None, C.c_int, C.c_char_p)(logfn)
keep.append(LOGFN)

@ENV
def env(cmd, data):
    cmd &= 0xffff
    if cmd in (9, 31):       # system / save dir
        C.cast(data, C.POINTER(C.c_char_p))[0] = rt_c.value; return True
    if cmd == 10:            # pixel format
        pixfmt[0] = C.cast(data, C.POINTER(C.c_int))[0]; return True
    if cmd == 15:            # get variable
        v = C.cast(data, C.POINTER(Var))[0]
        val = OPTS.get(v.key)
        if val is None: return False
        C.cast(data, C.POINTER(Var))[0].value = val; return True
    if cmd == 17:            # variable update
        C.cast(data, C.POINTER(C.c_bool))[0] = False; return True
    if cmd == 52:            # core options version: 0 -> legacy SET_VARIABLES
        C.cast(data, C.POINTER(C.c_uint))[0] = 0; return True
    if cmd == 27:            # log interface
        C.cast(data, C.POINTER(LogCb))[0].log = C.cast(LOGFN, C.c_void_p).value; return True
    if cmd == 14:            # hw render: refuse, we want software
        return False
    if cmd in (16, 11, 35, 39, 53, 54, 55, 56, 64, 65, 66, 67, 69, 70, 6, 8, 18, 36, 40, 44, 51, 13):
        return True if cmd in (16,) else False
    return False

@VIDEO
def video(data, w, h, pitch):
    if not data: return
    buf = C.string_at(data, h * pitch)
    # libretro: 0 = 0rgb1555, 1 = xrgb8888, 2 = rgb565
    if pixfmt[0] == 1:
        img = Image.frombuffer('RGBX', (w, h), buf, 'raw', 'BGRX', pitch, 1).convert('RGB')
    else:
        img = Image.frombuffer('RGB', (w, h), buf, 'raw', 'BGR;16', pitch, 1)
    last['img'] = img

@AUDIO
def audio(l, r): pass

audio_out = []
@AUDIOB
def audiob(d, n):
    audio_out.append(C.string_at(d, n * 4)); return n

@POLL
def poll(): pass

BTN = {'B': 0, 'Y': 1, 'SELECT': 2, 'START': 3, 'UP': 4, 'DOWN': 5, 'LEFT': 6, 'RIGHT': 7,
       'A': 8, 'X': 9, 'L': 10, 'R': 11}

axes = {}

@STATE
def state(port, device, index, id_):
    if port != 0: return 0
    if device == 5: return axes.get((index, id_), 0)
    if device != 1: return 0
    return 1 if id_ in held else 0

for f, cb in (('retro_set_environment', env), ('retro_set_video_refresh', video),
              ('retro_set_audio_sample', audio), ('retro_set_audio_sample_batch', audiob),
              ('retro_set_input_poll', poll), ('retro_set_input_state', state)):
    getattr(core, f)(cb)

def main():
    game = sys.argv[1]
    frames = int(sys.argv[2])
    every = 60
    presses = []
    sticks = []
    a = sys.argv[3:]
    i = 0
    while i < len(a):
        if a[i] == '--every': every = int(a[i + 1]); i += 2; continue
        if a[i] == '--press':
            fr, b, ln = a[i + 1].split(':')
            for bb in b.split('+'): presses.append((int(fr), BTN[bb], int(ln)))
            i += 2; continue
        if a[i] == '--stick':
            fr, ax, val, ln = a[i + 1].split(':')
            sticks.append((int(fr), {'LX': (0, 0), 'LY': (0, 1), 'RX': (1, 0), 'RY': (1, 1)}[ax], int(val), int(ln)))
            i += 2; continue
        i += 1
    core.retro_init()
    gi = GameInfo(game.encode(), None, 0, None)
    core.retro_load_game.restype = C.c_bool
    if not core.retro_load_game(C.byref(gi)):
        print('load failed'); return
    t0 = time.time()
    for f in range(frames):
        held.clear()
        axes.clear()
        for fr, b, ln in presses:
            if fr <= f < fr + ln: held.add(b)
        for fr, ax, val, ln in sticks:
            if fr <= f < fr + ln: axes[ax] = val
        core.retro_run()
        if every and (f + 1) % every == 0 and last['img']:
            last['img'].save(os.path.join(OUT, f'{f + 1:05d}.png'))
    print('pixfmt', pixfmt[0])
    print(f'{frames} frames in {time.time() - t0:.1f}s')
    if last['img']: last['img'].save(os.path.join(OUT, 'last.png'))
    if audio_out:
        import wave
        av = C.cast(C.create_string_buffer(64), C.c_void_p)
        w = wave.open(os.path.join(OUT, 'audio.wav'), 'wb')
        w.setnchannels(2); w.setsampwidth(2); w.setframerate(32728)
        w.writeframes(b''.join(audio_out)); w.close()
        print('audio frames', sum(len(a) for a in audio_out) // 4)
    core.retro_unload_game()
    core.retro_deinit()

main()
