"""Textured preview of a converted character (cod2_char.py's reference.smd) - in its reference pose and posed by
any of our animations (an SMD on the same skeleton), skinned the way Source does it (linear blend, <= 3 bones).

    python3 -I char_preview.py <reference.smd> <textures dir (material.png)> <out.png> [<anim.smd>[:frame] ...]

Each column: one pose seen from the front, the side and the back. Orthographic, a ground grid every 12 units."""
import os, sys, re
import numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), 'vehicle_preview'), os.path.dirname(HERE)]
from smd import SMD
from render_pose import world
from render3d import Camera

def read_mesh(path):
    s = SMD(path); lines = s.tail.splitlines()[1:]
    mats, P, N, UV, Wb = [], [], [], [], []
    i = 0
    while i < len(lines) and lines[i].strip() != 'end':
        mats.append(lines[i].strip())
        for k in range(3):
            v = lines[i + 1 + k].split()
            P.append([float(x) for x in v[1:4]]); N.append([float(x) for x in v[4:7]]); UV.append([float(v[7]), float(v[8])])
            n = int(v[9]); Wb.append([(int(v[10 + 2 * j]), float(v[11 + 2 * j])) for j in range(n)])
        i += 4
    return s, mats, np.array(P).reshape(-1, 3, 3), np.array(N).reshape(-1, 3, 3), np.array(UV).reshape(-1, 3, 2), Wb

def skin(ref, pose, P, N, Wb):
    """linear blend: each corner by sum w * (pose world x bind world^-1)"""
    B = world(ref, 0); A = world(pose, 0)
    M = {}
    for i in B:
        if i in A:
            R = A[i][0] @ B[i][0].T; M[i] = (R, A[i][1] - R @ B[i][1])
    flat = P.reshape(-1, 3); nf = N.reshape(-1, 3); outP = np.zeros_like(flat); outN = np.zeros_like(nf)
    for k, (p, n, w) in enumerate(zip(flat, nf, Wb)):
        for b, ww in w:
            R, t = M.get(b, (np.eye(3), np.zeros(3)))
            outP[k] += ww * (R @ p + t); outN[k] += ww * (R @ n)
    outN /= np.linalg.norm(outN, axis=1, keepdims=True) + 1e-9
    return outP.reshape(P.shape), outN.reshape(N.shape)

def pose_from(ref, anim_path, frame):
    a = SMD(anim_path); f = a.frames[min(frame, len(a.frames) - 1)][1]
    p = SMD(); p.nodes = ref.nodes; fr = dict(ref.frames[0][1])
    for i, (n, _) in enumerate(ref.nodes):
        j = a.index(n)
        if j >= 0 and j in f: fr[i] = f[j]
    p.frames = [(0, fr)]
    return p

def render(cam, P, N, UV, mats, tex, Z, C):
    S = cam.project(P.reshape(-1, 3)).reshape(-1, 3, 3)
    light = np.array([0.4, -0.5, 0.75]); light /= np.linalg.norm(light)
    # back faces culled the way the engine culls an SMD's: a front face's corners turn right-hand about the outward
    # normal (a two-sided preview hid an inside-out soldier, 2026-11-21)
    front = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]) @ cam.fwd > 0
    for tri, nn, uv, m, fr in zip(S, N, UV, mats, front):
        if not fr: continue
        x0, y0 = np.floor(tri[:, :2].min(0)).astype(int); x1, y1 = np.ceil(tri[:, :2].max(0)).astype(int)
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, cam.w - 1), min(y1, cam.h - 1)
        if x0 > x1 or y0 > y1: continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
        a, b, c = tri
        den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(den) < 1e-9: continue
        l1 = ((b[1] - c[1]) * (gx - c[0]) + (c[0] - b[0]) * (gy - c[1])) / den
        l2 = ((c[1] - a[1]) * (gx - c[0]) + (a[0] - c[0]) * (gy - c[1])) / den
        l3 = 1 - l1 - l2
        ins = (l1 >= -1e-4) & (l2 >= -1e-4) & (l3 >= -1e-4)
        d = l1 * a[2] + l2 * b[2] + l3 * c[2]
        zs = Z[y0:y1 + 1, x0:x1 + 1]; msk = ins & (d > zs)
        if not msk.any(): continue
        L = np.stack([l1, l2, l3], -1)[msk]
        u = L @ uv[:, 0]; v = 1.0 - (L @ uv[:, 1])          # SMD v is up; images are top-down
        t = tex[m]; th, tw = t.shape[:2]
        col = t[(np.mod(v, 1) * th).astype(int).clip(0, th - 1), (np.mod(u, 1) * tw).astype(int).clip(0, tw - 1)].astype(float)
        n = L @ nn; n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
        k = 0.35 + 0.75 * np.clip(n @ light, 0, 1)
        zs[msk] = d[msk]; C[y0:y1 + 1, x0:x1 + 1][msk] = (col * k[:, None]).clip(0, 255)

def panel(P, N, UV, mats, tex, az, w=300, h=560):
    c = P.reshape(-1, 3).mean(0)          # some animations carry the root off the origin
    cam = Camera(az, 5, (c[0], c[1], 36), 6.8, w, h)
    Z = np.full((h, w), -1e9); C = np.zeros((h, w, 3)); C[:] = (228, 230, 226)
    # ground grid lines every 12 units
    gx0, gy0 = np.round(c[:2] / 12) * 12
    for g in np.arange(-36, 37, 12):
        for a, b in (((gx0 + g, gy0 - 36, 0), (gx0 + g, gy0 + 36, 0)), ((gx0 - 36, gy0 + g, 0), (gx0 + 36, gy0 + g, 0))):
            q = cam.project(np.linspace(a, b, 200))
            for x, y, _ in q.astype(int):
                if 0 <= x < w and 0 <= y < h: C[y, x] = (170, 176, 168)
    render(cam, P, N, UV, mats, tex, Z, C)
    return C.astype(np.uint8)

if __name__ == '__main__':
    ref_path, texdir, out = sys.argv[1:4]; poses = sys.argv[4:]
    ref, mats, P, N, UV, Wb = read_mesh(ref_path)
    tex = {m: np.array(Image.open(os.path.join(texdir, m + '.png')).convert('RGB')) for m in set(mats)}
    cols = [('reference', P, N)]
    for spec in poses:
        path, _, fr = spec.partition(':')
        pp, nn = skin(ref, pose_from(ref, path, int(fr or 0)), P, N, Wb)
        cols.append((os.path.basename(path)[:-4], pp, nn))
    rows = []
    for name, pp, nn in cols:
        rows.append(np.concatenate([panel(pp, nn, UV, mats, tex, az) for az in (0, 90, 180)], 1))   # front (+x), side, back
    img = np.concatenate(rows, 0) if len(rows) > 1 else rows[0]
    Image.fromarray(img).save(out)
    print('wrote', out, img.shape)
