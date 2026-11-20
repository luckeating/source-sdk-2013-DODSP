"""CoD2 (version 20) xmodelparts + xmodelsurfs reader."""
import struct, numpy as np
def parts(path):
    d=open(path,'rb').read()
    ver,nrel,nroot=struct.unpack_from('<HHH',d,0); assert ver==20, ver
    o=6; rel=[]
    for i in range(nrel):
        par=d[o]; pos=struct.unpack_from('<3f',d,o+1); q=struct.unpack_from('<3h',d,o+13); o+=19
        rel.append((par,np.array(pos),[x/32768 for x in q]))
    names=[n.decode() for n in d[o:].split(b'\0')[:nrel+nroot]]
    def qm(x,y,z):
        w=max(0.0,1-x*x-y*y-z*z)**.5
        return np.array([[1-2*(y*y+z*z),2*(x*y-z*w),2*(x*z+y*w)],[2*(x*y+z*w),1-2*(x*x+z*z),2*(y*z-x*w)],[2*(x*z-y*w),2*(y*z+x*w),1-2*(x*x+y*y)]])
    bones=[dict(name=names[0],parent=-1,R=np.eye(3),t=np.zeros(3),lR=np.eye(3),lt=np.zeros(3))]
    for i,(par,pos,q) in enumerate(rel):
        P=bones[par]; lR=qm(*q)
        bones.append(dict(name=names[i+nroot],parent=par,lR=lR,lt=pos,R=P['R']@lR,t=P['R']@pos+P['t']))
    return bones
def surfs(path):
    d=open(path,'rb').read()
    ver,ns=struct.unpack_from('<HH',d,0); assert ver==20, ver
    o=4; out=[]
    for s in range(ns):
        tile=d[o]; nv,nt,dbone=struct.unpack_from('<HHh',d,o+1); o+=7
        if dbone == -1: o += 2          # weighted surfaces carry 2 more header bytes (a count; not needed to read)
        V=[]
        for v in range(nv):
            n=struct.unpack_from('<3f',d,o); col=d[o+12:o+16]; uv=struct.unpack_from('<2f',d,o+16)
            bi=struct.unpack_from('<3f',d,o+24); ta=struct.unpack_from('<3f',d,o+36); o+=48
            if dbone==-1:
                nw=d[o]; b0=struct.unpack_from('<H',d,o+1)[0]; o+=3
            else:
                nw=0; b0=dbone
            pos=struct.unpack_from('<3f',d,o); o+=12
            w=[]
            if nw:
                # each further weight: u8 (the first bone's share, /255), u16 bone, vec3 position in THAT bone, u16 share
                for k in range(nw):
                    b=struct.unpack_from('<H',d,o+1)[0]; p2=struct.unpack_from('<3f',d,o+3); inf=struct.unpack_from('<H',d,o+15)[0]
                    o+=17; w.append((b,inf/65536.0,np.array(p2)))
            V.append(dict(n=np.array(n),uv=uv,bone=b0,pos=np.array(pos),w=w))
        T=[struct.unpack_from('<3H',d,o+6*k) for k in range(nt)]; o+=6*nt
        out.append(dict(tile=tile,dbone=dbone,verts=V,tris=T))
    assert o==len(d), (o,len(d))
    return out
