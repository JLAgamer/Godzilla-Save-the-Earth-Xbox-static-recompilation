import struct,zlib,sys
files=sys.argv[2:]; out=sys.argv[1]
def load(p):
    d=open(p,'rb').read(); off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); bp=struct.unpack_from('<H',d,28)[0]//8
    st=(w*bp+3)&~3; H=abs(h); px=[]
    for y in range(H):
        sy=H-1-y if h>0 else y; r=d[off+sy*st:off+sy*st+w*bp]; px.append(r)
    return w,H,bp,px
tw,th=320,240; cols=3; rows=(len(files)+cols-1)//cols
canvas=[bytearray(tw*cols*3) for _ in range(th*rows)]
for i,f in enumerate(files):
    w,H,bp,px=load(f); cx=(i%cols)*tw; cy=(i//cols)*th
    for y in range(th):
        r=px[y*H//th]
        for x in range(tw):
            sx=(x*w//tw)*bp; canvas[cy+y][(cx+x)*3:(cx+x)*3+3]=bytes((r[sx+2],r[sx+1],r[sx]))
raw=b''.join(b'\0'+bytes(r) for r in canvas)
def ch(t,x): return struct.pack('>I',len(x))+t+x+struct.pack('>I',zlib.crc32(t+x))
open(out,'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',tw*cols,th*rows,8,2,0,0,0))+ch(b'IDAT',zlib.compress(raw))+ch(b'IEND',b''))
