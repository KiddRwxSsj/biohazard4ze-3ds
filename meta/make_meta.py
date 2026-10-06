#!/usr/bin/env python3
# menu art -> what bannertool wants
#   icon.png   48x48, home menu grid icon. smdh is rgb565, no alpha: bannertool multiplies by
#              alpha, so transparent pixels go black. any square-ish image, center crop
#   banner.png 256x128 rgba, top screen banner. texture is rgba4444, alpha survives (4 bit).
#              faint torn edges trimmed, art scaled to 214 wide, overflow cut top/bottom
# usage: make_meta.py <icon image> <banner art> [out dir]
import os, sys
from PIL import Image, ImageOps

icon_src, banner_src = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else os.path.dirname(os.path.abspath(__file__))

ic = Image.open(icon_src).convert('RGBA')
bg = Image.new('RGBA', ic.size, (0, 0, 0, 255))
bg.alpha_composite(ic)
ImageOps.fit(bg.convert('RGB'), (48, 48), Image.LANCZOS).save(os.path.join(out, 'icon.png'))

W, H, ART_W = 256, 128, 214
art = Image.open(banner_src).convert('RGBA')
box = art.split()[3].point(lambda v: 255 if v > 96 else 0).getbbox()
if box: art = art.crop(box)
h = round(art.height * ART_W / art.width)
art = art.resize((ART_W, h), Image.LANCZOS)
if h > H: art = art.crop((0, (h - H) // 2, ART_W, (h - H) // 2 + H))
bn = Image.new('RGBA', (W, H), (0, 0, 0, 0))
bn.alpha_composite(art, ((W - art.width) // 2, (H - art.height) // 2))
bn.save(os.path.join(out, 'banner.png'))
print(f'icon 48x48 from {os.path.basename(icon_src)}, banner {art.width}x{art.height} in {W}x{H}')
