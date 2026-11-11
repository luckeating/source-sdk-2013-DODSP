"""Minimal SMD (Source studiomdl) reader/writer for animation work."""
import re
class SMD:
    def __init__(self, path=None):
        self.nodes=[]   # list of (name, parent)
        self.frames=[]  # list of (time, {idx:(px,py,pz,rx,ry,rz)})
        self.tail=''    # anything after skeleton (triangles) kept raw
        if path: self.read(path)
    def read(self, path):
        lines=open(path,encoding='latin1').read().splitlines()
        i=0; mode=None; cur=None; tail=[]
        while i<len(lines):
            l=lines[i].strip(); i+=1
            if mode is None:
                if l=='nodes': mode='nodes'
                elif l=='skeleton': mode='skel'
                elif l=='triangles' or l=='vertexanimation':
                    tail=[lines[i-1]]+lines[i:]; break
                continue
            if l=='end': mode=None; continue
            if mode=='nodes':
                m=re.match(r'(-?\d+)\s+"([^"]*)"\s+(-?\d+)',l); self.nodes.append((m.group(2),int(m.group(3))))
            elif mode=='skel':
                if l.startswith('time'):
                    cur={}; self.frames.append((int(l.split()[1]),cur))
                elif l:
                    p=l.split(); cur[int(p[0])]=tuple(float(x) for x in p[1:7])
        self.tail='\n'.join(tail)
    def index(self, name):
        n=name.lower()
        for i,(nm,_) in enumerate(self.nodes):
            if nm.lower()==n: return i
        return -1
    def write(self, path):
        out=['version 1','nodes']
        for i,(n,p) in enumerate(self.nodes): out.append(f'{i} "{n}" {p}')
        out+=['end','skeleton']
        for t,f in self.frames:
            out.append(f'time {t}')
            for i in sorted(f):
                v=f[i]; out.append(f'{i} '+' '.join(f'{x:.6f}' for x in v))
        out.append('end')
        if self.tail: out.append(self.tail)
        open(path,'w',newline='\n').write('\n'.join(out)+'\n')
