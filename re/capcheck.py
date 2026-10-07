#!/usr/bin/env python3
# usage: capcheck.py bgm_capture.raw reference.raw   (reference from re/midiport.c)
import numpy as np, sys
# capture vs single-instance reference: per-second correlation after alignment. a clean single
# instance tracks the reference closely; a second copy or replayed chunks drag it down
cap = np.fromfile(sys.argv[1], np.int16).reshape(-1, 2).mean(1)
ref = np.fromfile(sys.argv[2], np.int16).reshape(-1, 2).mean(1)
R = 22050
probe = cap[3 * R:5 * R]
cs = np.correlate(ref, probe, 'valid') if len(ref) < 4_000_000 else None
norms = np.sqrt(np.convolve(ref ** 2, np.ones(len(probe)), 'valid'))
c = cs / (norms * np.linalg.norm(probe) + 1e-9)
off = int(np.argmax(c)) - 3 * R
print('probe corr %.3f, capture starts %.2fs into the song' % (c.max(), off / R))
for s in range(0, len(cap) // R):
    a = cap[s * R:(s + 1) * R]
    if off + (s + 1) * R > len(ref) or off + s * R < 0: break
    b = ref[off + s * R:off + (s + 1) * R]
    cc = np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b) + 1e-9)
    print('%2ds corr %.3f' % (s, cc), end='   ' if (s + 1) % 6 else '\n')
print()
