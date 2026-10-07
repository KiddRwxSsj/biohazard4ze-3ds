import struct,sys
d=open(sys.argv[1] if len(sys.argv) > 1 else '../private/game/mod/276675/bio4_brew.mod','rb').read()
CODE=0x200; RO=0x74fc0; DATA=CODE+RO; RT=DATA+0x350
p=RT; out=[]
while True:
    w,=struct.unpack_from('<I',d,p); p+=4
    if w==0: break
    t=w&0xff; n=w>>8
    if t&0x3f:
        n<<=2
        if t&0x20: n<<=1
        p+=n; print('skip',hex(t),n); continue
    seg=DATA if t&0x80 else CODE
    for i in range(n):
        o,=struct.unpack_from('<I',d,p); p+=4
        v,=struct.unpack_from('<I',d,seg+o)
        out.append((seg+o,v))
print('table end',hex(p),'fixups',len(out))
import collections
neg=[(hex(a),hex(v)) for a,v in out if v>=0x80000000]
print('neg',neg[:20])
print('max val',hex(max(v for a,v in out if v<0x80000000)))
