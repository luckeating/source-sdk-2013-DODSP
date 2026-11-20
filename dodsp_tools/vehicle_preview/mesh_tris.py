import struct, numpy as np
def tris(mdl):
    d = open(mdl,'rb').read(); I = lambda o: struct.unpack_from('<i', d, o)[0]
    v = open(mdl[:-4]+'.vvd','rb').read(); Iv = lambda o: struct.unpack_from('<i', v, o)[0]
    nfix, fixs, vstart, nv = Iv(48), Iv(52), Iv(56), Iv(16)
    if nfix:
        verts = b''
        for k in range(nfix):
            lod, src, cnt = struct.unpack_from('<iii', v, fixs+12*k)
            if lod >= 0: verts += v[vstart+src*48: vstart+(src+cnt)*48]
    else: verts = v[vstart: vstart+nv*48]
    P = np.frombuffer(np.frombuffer(verts,np.uint8).reshape(-1,48)[:,16:28].tobytes(),np.float32).reshape(-1,3)
    x = open(mdl[:-4]+'.dx90.vtx','rb').read(); X = lambda o: struct.unpack_from('<i', x, o)[0]
    out = []
    nbp, bpo = X(28), X(32); mnbp, mbpi = I(232), I(236)
    for b in range(nbp):
        bo = 32 + bpo + 8*b if False else bpo + 8*b
        nm, mo = X(bo), X(bo+4)
        mb = mbpi + 16*b; mmi = I(mb+12)
        for m in range(nm):
            mho = bo + mo + 8*m; nlod, lo = X(mho), X(mho+4)
            mdlm = mb + mmi + 148*m; meshi, vidx = I(mdlm+76), I(mdlm+84)
            lod0 = mho + lo; nmesh, meo = X(lod0), X(lod0+4)
            for k in range(nmesh):
                mh = lod0 + meo + 9*k; nsg, sgo = X(mh), X(mh+4)
                me = mdlm + meshi + 116*k; voff = I(me+12)
                base = vidx//48 + voff
                for g in range(nsg):
                    sg = mh + sgo + 25*g
                    nvv, vo, ni, io = X(sg), X(sg+4), X(sg+8), X(sg+12)
                    ids = [struct.unpack_from('<H', x, sg+vo+9*i+4)[0] for i in range(nvv)]
                    idx = struct.unpack_from('<%dH'%ni, x, sg+io)
                    for t in range(0, ni, 3):
                        out.append([P[base+ids[idx[t+j]]] for j in range(3)])
    T = np.array(out)  # n,3,3 mesh frame
    return np.stack([-T[...,1], T[...,0], T[...,2]], -1)   # game frame: x fwd
def heightmap(T, c=1.0):
    lo = T.reshape(-1,3).min(0); hi = T.reshape(-1,3).max(0)
    nx, ny = int((hi[0]-lo[0])/c)+2, int((hi[1]-lo[1])/c)+2
    H = np.full((ny,nx), -1e9)
    for tri in T:
        a,b,cc = tri
        x0,x1 = int((min(a[0],b[0],cc[0])-lo[0])/c), int((max(a[0],b[0],cc[0])-lo[0])/c)+1
        y0,y1 = int((min(a[1],b[1],cc[1])-lo[1])/c), int((max(a[1],b[1],cc[1])-lo[1])/c)+1
        gx, gy = np.meshgrid(lo[0]+(np.arange(x0,x1+1)+.5)*c, lo[1]+(np.arange(y0,y1+1)+.5)*c)
        v0, v1 = b-a, cc-a; den = v0[0]*v1[1]-v1[0]*v0[1]
        if abs(den) < 1e-9: continue
        px, py = gx-a[0], gy-a[1]
        u = (px*v1[1]-v1[0]*py)/den; w = (v0[0]*py-px*v0[1])/den
        ins = (u>=-1e-3)&(w>=-1e-3)&(u+w<=1+1e-3)
        z = a[2]+u*v0[2]+w*v1[2]
        sub = H[y0:y1+1, x0:x1+1]
        zz = np.where(ins, z, -1e9)[:sub.shape[0], :sub.shape[1]]
        np.maximum(sub, zz, out=sub)
    H[H<-1e8] = np.nan
    return H, lo, hi
