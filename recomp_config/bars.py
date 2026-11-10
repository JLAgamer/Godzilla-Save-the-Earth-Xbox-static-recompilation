import struct,sys
# Measure P1 and COM health bars (bright green/blue run lengths) in a 640x480 BMP frame.
def load(p):
    d=open(p,'rb').read(); off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); bp=struct.unpack_from('<H',d,28)[0]//8
    st=(w*bp+3)&~3; H=abs(h)
    def px(x,y):
        sy=H-1-y if h>0 else y; o=off+sy*st+x*bp; return d[o+2],d[o+1],d[o]
    return w,H,px
for p in sys.argv[1:]:
    w,H,px=load(p)
    res=[]
    for (x0,x1) in ((110,300),(360,530)):   # P1 bar region, COM bar region
        best=0
        for y in range(44,60):
            run=0
            for x in range(x0,x1):
                r,g,b=px(x,y)
                if g>120 and r<120: run+=1      # green fill
            best=max(best,run)
        res.append(best)
    print(p.split('/')[-1], 'P1 green', res[0], 'COM green', res[1])
