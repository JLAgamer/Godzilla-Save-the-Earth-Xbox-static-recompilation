import struct,sys
# Mean absolute difference (0-255) between two 640x480 BMP frames, sampled on a grid.
def load(p):
    d=open(p,'rb').read(); off=struct.unpack_from('<I',d,10)[0]; w,h=struct.unpack_from('<ii',d,18); bp=struct.unpack_from('<H',d,28)[0]//8
    st=(w*bp+3)&~3; H=abs(h)
    return d,off,w,H,bp,st,h
a=load(sys.argv[1]); b=load(sys.argv[2]); tot=0; n=0
for y in range(0,a[3],8):
    for x in range(0,a[2],8):
        for (d,off,w,H,bp,st,h),acc in ((a,0),):
            pass
        sy=a[3]-1-y if a[6]>0 else y; oa=a[1]+sy*a[5]+x*a[4]
        sy2=b[3]-1-y if b[6]>0 else y; ob=b[1]+sy2*b[5]+x*b[4]
        tot+=sum(abs(a[0][oa+i]-b[0][ob+i]) for i in range(3)); n+=3
print(int(tot/n))
