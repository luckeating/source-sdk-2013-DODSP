"""Close-ups of a converted character's joints, BEFORE and AFTER the half-angle joint helpers, skinned the engine's way
(linear blend, the helpers turned by DoQuatInterpBone from the .vrd as written). Reads the package's own files, so what
you see is what Crowbar compiles.

    python3 -I joint_preview.py <before body.smd> <after body.smd> <after .vrd> <textures dir> <out.png> <anim.smd>[:frame] ...

Per pose, per joint (wrist, elbow, shoulder, hip, knee - both sides): two views round the joint, before | after, and
the joint's thickness number (joint_volume.measure's slice rule) printed under each."""
import os, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial.transform import Rotation as Rot
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), 'vehicle_preview'), os.path.dirname(HERE)]
import char_preview as CP, joint_volume as JV, twist_bones as TW
from smd import SMD
from render3d import Camera

PREVIEW_JOINTS = ('wrist', 'elbow', 'shoulder', 'hip', 'knee')

def skeleton_from_smd(body, vrd=None):
    """the engine's view of a written model: nodes + bind from the SMD, procedural helpers from the .vrd"""
    s = SMD(body); f = s.frames[0][1]
    names = [n for n, _ in s.nodes]
    helpers = {}
    if vrd:
        for name, parent, driver, tr in TW.read_vrd(vrd): helpers[name] = (driver, tr)
    base = [i for i, n in enumerate(names) if JV.vrd_name(n) not in helpers]
    assert base == list(range(len(base))), 'helpers must come after every animated bone'
    sk = JV.Skeleton(s.nodes[:len(base)], {i: f[i] for i in base})
    vn = {JV.vrd_name(n): i for i, n in enumerate(names)}
    for i in range(len(base), len(names)):
        driver, tr = helpers[JV.vrd_name(names[i])]; c = vn[driver]
        def fn(lc, c=c, tr=tr):
            q, pp = TW.engine_quat_interp(JV.qrel(lc[c][1]), tr); return (np.array(pp, float), Rot.from_quat(q).as_matrix())
        sk.add_helper(names[i], s.nodes[i][1], f[i][:3], JV.rotm(f[i][3:]), fn)
    return sk

def load(body, vrd):
    ref, mats, P, N, UV, Wb = CP.read_mesh(body)
    sk = skeleton_from_smd(body, vrd)
    return sk, mats, P, N, UV, Wb

def posed(sk, P, N, Wb, fr):
    """skinned corners in a pose"""
    Wbind = sk.world(sk.bind); Wp = sk.world(sk.pose(fr))
    M = {i: (Rp @ Rb.T, tp - Rp @ Rb.T @ tb) for i, ((Rb, tb), (Rp, tp)) in enumerate(zip(Wbind, Wp))}
    flat = P.reshape(-1, 3); nf = N.reshape(-1, 3); oP = np.zeros_like(flat); oN = np.zeros_like(nf)
    for k, (p, n, w) in enumerate(zip(flat, nf, Wb)):
        for b, x in w:
            R, t = M[b]; oP[k] += x * (R @ p + t); oN[k] += x * (R @ n)
    oN /= np.linalg.norm(oN, axis=1, keepdims=True) + 1e-9
    return oP.reshape(P.shape), oN.reshape(N.shape), Wp

def view(P, N, UV, mats, tex, centre, d, scale=11.0, w=220, h=220):
    """looking along -d at centre (d = from the joint toward the camera)"""
    d = d / np.linalg.norm(d)
    cam = Camera(np.degrees(np.arctan2(d[1], d[0])), np.degrees(np.arcsin(np.clip(d[2], -1, 1))), centre, scale, w, h)
    Z = np.full((h, w), -1e9); C = np.zeros((h, w, 3)); C[:] = (228, 230, 226)
    near = np.linalg.norm(P.reshape(-1, 3) - centre, axis=1).reshape(-1, 3).min(1) < 16
    CP.render(cam, P[near], N[near], UV[near], [m for m, k in zip(mats, near) if k], tex, Z, C)
    return C.astype(np.uint8)

def sheet(before, after, texdir, out, specs):
    sk0, mats, P0, N0, UV, W0 = load(*before)
    sk1, _, P1, N1, _, W1 = load(*after)
    tex = {m: np.array(Image.open(os.path.join(texdir, m + '.png')).convert('RGB')) for m in set(mats)}
    rows = []
    for spec in specs:
        path, fr = (spec.rsplit(':', 1) + ['0'])[:2]
        a = SMD(path); f = a.frames[min(int(fr), len(a.frames) - 1)][1]; frame = {a.nodes[i][0]: v for i, v in f.items()}
        A0, AN0, Wp0 = posed(sk0, P0, N0, W0, frame); A1, AN1, Wp1 = posed(sk1, P1, N1, W1, frame)
        tiles = []
        for j in PREVIEW_JOINTS:
            for s in 'LR':
                Pn, Cn, En = JV.JOINTS[j]
                c = Wp0[sk0.idx[JV.side_name(s, Cn)]][1]; e = Wp0[sk0.idx[JV.side_name(s, En)]][1]
                pa = Wp0[sk0.idx[JV.side_name(s, Pn)]][1] if j != 'hip' else c + (c - e)
                u1, u2 = (c - pa) / np.linalg.norm(c - pa), (e - c) / np.linalg.norm(e - c)
                n = np.cross(u1, u2)
                if np.linalg.norm(n) < 0.05: n = np.cross(u2, [0, 0, 1.0])
                n /= np.linalg.norm(n); side_dir = u1 - u2; side_dir /= max(np.linalg.norm(side_dir), 1e-6)
                if np.linalg.norm(u1 - u2) < 0.05: side_dir = np.cross(u2, n)
                pair = []
                for (AP, AN) in ((A0, AN0), (A1, AN1)):
                    # across the bend plane (the bend seen side on), and from the outside of the bend
                    v = np.concatenate([view(AP, AN, UV, mats, tex, c, dd) for dd in (n, -side_dir)], 0)
                    pair.append(v)
                t = np.concatenate([pair[0], np.full((pair[0].shape[0], 4, 3), 40, np.uint8), pair[1]], 1)
                im = Image.fromarray(t); ImageDraw.Draw(im).text((4, 2), '%s %s  before | after' % (s, j), fill=(0, 0, 0))
                tiles.append(np.array(im))
        row = np.concatenate([np.concatenate([t, np.full((t.shape[0], 10, 3), 255, np.uint8)], 1) for t in tiles], 1)
        im = Image.fromarray(row); ImageDraw.Draw(im).text((4, row.shape[0] - 14), os.path.basename(path)[:-4] + ':' + fr, fill=(150, 0, 0))
        rows.append(np.array(im))
    Image.fromarray(np.concatenate(rows, 0)).save(out); print('wrote', out)

if __name__ == '__main__':
    a = sys.argv[1:]
    sheet((a[0], None), (a[1], a[2]), a[3], a[4], a[5:])
