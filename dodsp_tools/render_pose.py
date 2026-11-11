import sys, os, math
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from smd import SMD
def rot(x,y,z):
    cx,sx,cy,sy,cz,sz=math.cos(x),math.sin(x),math.cos(y),math.sin(y),math.cos(z),math.sin(z)
    Rx=np.array([[1,0,0],[0,cx,-sx],[0,sx,cx]]); Ry=np.array([[cy,0,sy],[0,1,0],[-sy,0,cy]]); Rz=np.array([[cz,-sz,0],[sz,cz,0],[0,0,1]])
    return Rz@Ry@Rx
def world(s, fi):
    f=s.frames[fi][1]; W={}
    for i,(n,p) in enumerate(s.nodes):
        if i not in f: continue
        v=f[i]; R=rot(*v[3:]); t=np.array(v[:3])
        if p<0 or p not in W: W[i]=(R,t)
        else:
            PR,Pt=W[p]; W[i]=(PR@R, PR@t+Pt)
    return W
def draw(ax, s, fi, title, guns):
    W=world(s,fi)
    for i,(n,p) in enumerate(s.nodes):
        if i in W and p in W and n.startswith('ValveBiped.Bip01'):
            a=W[p][1]; b=W[i][1]; ax.plot([a[0],b[0]],[a[2],b[2]],'k-',lw=1)
    for bone,muz,col in guns:
        i=s.index(bone)
        if i in W:
            R,t=W[i]; m=R@np.array(muz)+t; ax.plot([t[0],m[0]],[t[2],m[2]],'-',color=col,lw=3)
    ax.set_title(title,fontsize=8); ax.set_aspect('equal'); ax.set_xlim(-40,40); ax.set_ylim(-5,75); ax.axis('off')
if __name__=='__main__':
  pass
GUNS=[('ValveBiped.weapon_bone',(0.8,4.5,26.5),'tab:blue'),('ValveBiped.weapon_bone_left',(1.3,4.3,35.5),'tab:red')]
if __name__=='__main__':
    items=[(p,t) for p,t in zip(sys.argv[2::2],sys.argv[3::2])]
    fig,axs=plt.subplots(1,len(items),figsize=(3*len(items),3.4))
    for ax,(p,t) in zip(axs,items):
        s=SMD(p); draw(ax,s,0,t,GUNS)
    fig.suptitle('side view (X forward/right, Z up). blue = MP40 barrel, red = K98 barrel',fontsize=8)
    plt.tight_layout(); plt.savefig(sys.argv[1],dpi=110)
