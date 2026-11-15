# XBF reader for analysis. Port of LibEmperor (IceReaper/ebfd-re, MIT License, Copyright (c) IceReaper and Contributors).
import struct, numpy as np
class R:
    def __init__(s,b): s.b=b; s.p=0
    def i32(s): v=struct.unpack_from('<i',s.b,s.p)[0]; s.p+=4; return v
    def u16(s): v=struct.unpack_from('<H',s.b,s.p)[0]; s.p+=2; return v
    def i16(s): v=struct.unpack_from('<h',s.b,s.p)[0]; s.p+=2; return v
    def f32(s,n=1): v=struct.unpack_from('<%df'%n,s.b,s.p); s.p+=4*n; return v
    def f64(s,n=1): v=struct.unpack_from('<%dd'%n,s.b,s.p); s.p+=8*n; return v
    def raw(s,n): v=s.b[s.p:s.p+n]; s.p+=n; return v
class Obj: pass
def read_obj(r):
    o=Obj(); o.start=r.p
    nv=r.i32(); o.flags=r.i32(); nt=r.i32(); nc=r.i32()
    o.transform=np.array(r.f64(16)).reshape(4,4)
    o.name=r.raw(r.i32()).split(b'\0')[0].decode('latin1')
    o.children=[read_obj(r) for _ in range(nc)]
    vs=np.frombuffer(r.raw(24*nv),dtype='<f4').reshape(nv,6); o.pos=vs[:,:3].copy(); o.nrm=vs[:,3:].copy()
    tri=r.raw(44*nt); o.tri=np.frombuffer(tri,dtype=np.dtype([('v','<i4',3),('tex','<i4'),('sg','<i4'),('uv','<f4',6)])).copy()
    if o.flags&1: r.raw(3*nv)
    if o.flags&2: r.raw(4*nt)
    o.vanim=None; o.oanim=None
    if o.flags&4:
        L=r.i32(); en=r.i32(); used=r.i32(); fids=[r.i32() for _ in range(used)]
        info={'len':L,'used':used}
        if en<0:
            unk=r.i16(); fl=r.u16(); entries=r.i32()
            nvert=entries//used
            data=np.frombuffer(r.raw(8*entries),dtype='i1').reshape(used,nvert,8)
            info.update(unk=unk,fl=fl,nvert=nvert,data=data)
            if fl: r.raw(4*L)
        o.vanim=info
    if o.flags&8:
        L=r.i32()+1; used=r.i32(); info={'len':L,'used':used}
        if used==-1: r.raw(64*L)
        elif used==-2: r.raw(48*L)
        elif used==-3:
            nm=r.i32(); r.raw(2*L); r.raw(48*nm)
        else:
            for _ in range(used):
                fid=r.i16(); fl=r.i16()
                if (fl>>12)&1: r.raw(16)
                if (fl>>12)&2: r.raw(12)
                if (fl>>12)&4: r.raw(12)
        o.oanim=info
    o.end=r.p
    return o
def read_xbf(b):
    r=R(b); assert r.i32()==1; n=r.i32(); fx=r.raw(n)
    tex=[t for t in r.raw(r.i32()).replace(b'\x02',b'').split(b'\0') if t]
    objs=[]
    while True:
        if struct.unpack_from('<i',b,r.p)[0]==-1: r.p+=4; break
        objs.append(read_obj(r))
    assert r.p==len(b), (r.p,len(b))
    return [t.decode('latin1') for t in tex], objs
