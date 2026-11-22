"""A compiled model's tangents (.vvd) checked against its own triangles (.dx90.vtx): direction vs dP/du from the
compiled UVs, handedness (w) vs the UVs, length. Source builds the bump frame from these (shader: binormal = cross(N, T) * w,
so the map's green points image-DOWN). python3 -I vvd_tangents.py <model.mdl> <out.npz>
First use 2026-11-21 (CoD3 rifleman, the "black streaks"): median 0.2 deg off, w agrees 99.5% - the tangents were fine."""
import struct, sys, numpy as np
mdl=sys.argv[1]
d=open(mdl,'rb').read(); I=lambda o: struct.unpack_from('<i',d,o)[0]
v=open(mdl[:-4]+'.vvd','rb').read(); Iv=lambda o: struct.unpack_from('<i',v,o)[0]
nv, nfix, fixs, vstart, tstart = Iv(16), Iv(48), Iv(52), Iv(56), Iv(60)
print('vvd: verts', nv, 'fixups', nfix, 'vertex data at', vstart, 'tangents at', tstart)
V=np.frombuffer(v[vstart:vstart+nv*48],np.uint8).reshape(-1,48)
P=np.frombuffer(V[:,16:28].tobytes(),np.float32).reshape(-1,3); N=np.frombuffer(V[:,28:40].tobytes(),np.float32).reshape(-1,3)
UV=np.frombuffer(V[:,40:48].tobytes(),np.float32).reshape(-1,2)
T4=np.frombuffer(v[tstart:tstart+nv*16],np.float32).reshape(-1,4)
x=open(mdl[:-4]+'.dx90.vtx','rb').read(); X=lambda o: struct.unpack_from('<i',x,o)[0]
tri=[]; meshmat=[]
nbp,bpo=X(28),X(32); mbpi=I(236)
for b in range(nbp):
    bo=bpo+8*b; nm,mo=X(bo),X(bo+4); mb=mbpi+16*b; mmi=I(mb+12)
    for m in range(nm):
        mho=bo+mo+8*m; nlod,lo=X(mho),X(mho+4); mdlm=mb+mmi+148*m; meshi,vidx=I(mdlm+76),I(mdlm+84)
        lod0=mho+lo; nmesh,meo=X(lod0),X(lod0+4)
        for k in range(nmesh):
            mh=lod0+meo+9*k; nsg,sgo=X(mh),X(mh+4); me=mdlm+meshi+116*k; mat=I(me); voff=I(me+12); base=vidx//48+voff
            for g in range(nsg):
                sg=mh+sgo+25*g; nvv,vo,ni,io=X(sg),X(sg+4),X(sg+8),X(sg+12)
                ids=[struct.unpack_from('<H',x,sg+vo+9*i+4)[0] for i in range(nvv)]; idx=struct.unpack_from('<%dH'%ni,x,sg+io)
                for t in range(0,ni,3):
                    tri.append([base+ids[idx[t+j]] for j in range(3)]); meshmat.append(mat)
tri=np.array(tri); meshmat=np.array(meshmat)
print('triangles', len(tri), 'materials', sorted(set(meshmat)))
# reference tangent per vertex: area-weighted dP/du, dP/dv from the compiled UVs (u right, v DOWN the image: vvd stores t)
acc=np.zeros((nv,3)); accb=np.zeros((nv,3))
p0,p1,p2=P[tri[:,0]],P[tri[:,1]],P[tri[:,2]]; u0,u1,u2=UV[tri[:,0]],UV[tri[:,1]],UV[tri[:,2]]
e1,e2=p1-p0,p2-p0; d1,d2=u1-u0,u2-u0; r=d1[:,0]*d2[:,1]-d2[:,0]*d1[:,1]; ok=np.abs(r)>1e-12
Tt=(e1*d2[:,1:2]-e2*d1[:,1:2]); Bt=(e2*d1[:,0:1]-e1*d2[:,0:1])
Tt[ok]/=r[ok,None]; Bt[ok]/=r[ok,None]; Tt[~ok]=0; Bt[~ok]=0
for j in range(3): np.add.at(acc,tri[:,j],Tt); np.add.at(accb,tri[:,j],Bt)
def unit(a): return a/(np.linalg.norm(a,axis=1,keepdims=True)+1e-12)
Tref=unit(acc-N*np.einsum('ij,ij->i',acc,N)[:,None]); Bref=unit(accb)
hand=np.sign(np.einsum('ij,ij->i',np.cross(N,Tref),Bref))
Ts=T4[:,:3]; L=np.linalg.norm(Ts,axis=1)
ang=np.degrees(np.arccos(np.clip(np.einsum('ij,ij->i',unit(Ts),Tref),-1,1)))
used=np.zeros(nv,bool); used[tri.ravel()]=True
print('stored tangent length: min %.3f median %.3f'%(L[used].min(),np.median(L[used])))
print('stored tangent vs dP/du: median %.1f deg, 90%% %.1f, over 45 deg: %.1f%% of vertices'%(np.median(ang[used]),np.percentile(ang[used],90),100*np.mean(ang[used]>45)))
print('stored w vs handedness from UVs: agree %.1f%%'%(100*np.mean(np.sign(T4[used,3])==hand[used])))
print('w values', np.unique(np.round(T4[used,3],2), return_counts=True))
print('tangent . normal: median %.3f, 90%% %.3f'%(np.median(np.abs(np.einsum('ij,ij->i',unit(Ts),N))[used]),np.percentile(np.abs(np.einsum('ij,ij->i',unit(Ts),N))[used],90)))
# per material
for m in sorted(set(meshmat)):
    vs=np.unique(tri[meshmat==m]); print(' material %d: %d verts, tangent off median %.1f deg, w agrees %.0f%%, mirrored-UV verts %.0f%%'%(m,len(vs),np.median(ang[vs]),100*np.mean(np.sign(T4[vs,3])==hand[vs]),100*np.mean(hand[vs]<0)))
np.savez(sys.argv[2],P=P,N=N,UV=UV,T4=T4,tri=tri,mat=meshmat)
