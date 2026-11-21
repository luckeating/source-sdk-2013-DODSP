"""Tracks preview: a rigged vehicle (vehicle_rig.py output) driven over rough ground with the client's suspension maths.

    python3 track_preview.py <rig out dir> <variant> <out .gif> [frames]

Reads <variant>_body.smd (bones, rest positions, weighted triangles) and the "dodsp_wheels" $keyvalues of <variant>.qc -
the same facts client.dll reads - and does what the game does, frame by frame:
  * the hull: the server's BuildAngles (ground under four points at 3/4 of the half-length / half-width: pitch, roll,
    height) - the tanks' lurch spring is left out, it rides on top of this;
  * every suspension unit (susp_*): the ground straight under its contact point, as a height along the hull's up axis,
    clamped to its measured up / down travel, followed at SUSP_RATE units a second; the unit and every bone under it
    (its road wheels, its share of the belt, the top run where it rests on them) move by that;
  * every wheel spins about its own (moved) centre by distance / its radius;
  * every vertex = its bones' moves blended by its weights (the belt bends between units).
The ground has a log across both tracks, a rock under the left track only and a dip under the right one only, on a
gentle roll. Two views: the left side, and a low three-quarter from the front right. Each wheel has two red marks so
its turning shows; the belt is dark.
Checks printed: per unit the most it rose and dropped (must stay within its travel), and the worst belt stretch (the
longest any belt edge gets / its rest length - a tearing belt would show here)."""
import os, re, sys, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render3d import Camera, _raster
from vehicle_rig import tread_materials

SUSP_RATE = 160.0                 # units/s: how fast a unit follows the ground (c_dodsp_vehicle.cpp)
SPEED = 110.0                     # units/s
FPS = 30

def m2g(p): return np.array([-p[1], p[0], p[2]])

def read_rig(rig, var):
    L = open(os.path.join(rig, var + '_body.smd')).read().split('\n')
    i = L.index('nodes') + 1; names, parent = [], []
    while L[i] != 'end':
        k, n, p = re.match(r'(\d+) "([^"]+)" (-?\d+)', L[i]).groups(); names.append(n); parent.append(int(p)); i += 1
    i = L.index('skeleton') + 2; local = {}
    while L[i] != 'end':
        f = L[i].split(); local[int(f[0])] = np.array(list(map(float, f[1:4]))); i += 1
    rest = []
    for b in range(len(names)):
        p = local[b].copy(); q = parent[b]
        while q >= 0: p = p + local[q]; q = parent[q]
        rest.append(m2g(p))
    i = L.index('triangles') + 1; tris = []
    while L[i] != 'end':
        mat = L[i]; vs = []
        for k in range(3):
            f = L[i + 1 + k].split(); n = int(f[9])
            vs.append((m2g(np.array(list(map(float, f[1:4])))), [(int(f[10 + 2 * j]), float(f[11 + 2 * j])) for j in range(n)]))
        tris.append((mat, vs)); i += 4
    qc = open(os.path.join(rig, var + '.qc')).read()
    m = re.search(r'"dodsp_wheels"\s*\{(.*?)\}', qc, re.S)
    susp, wheels = {}, {}
    for k, v in re.findall(r'"([^"]+)"\s*"([^"]+)"', m.group(1)):
        f = v.split(); d = dict(zip(f[0::2], f[1::2]))
        if k.startswith('susp_'): susp[names.index(k)] = (float(d['up']), float(d['down']))
        elif k.startswith('wheel_'): wheels[names.index(k)] = float(d['radius'])
    return names, parent, np.array(rest), tris, susp, wheels

def ground(x, y):
    """rough ground, game units: a roll, a log across at x 300, a rock under the LEFT track (y > 0) at x 520, a dip
    under the RIGHT track at x 700"""
    z = 4.0 * np.sin(x / 90.0) + 2.0 * np.sin(x / 37.0 + 1.0)
    z += 7.0 * np.exp(-((x - 300.0) / 9.0) ** 2)
    z += 10.0 * np.exp(-((x - 520.0) / 22.0) ** 2) * (1.0 / (1.0 + np.exp(-(y - 20.0) / 4.0)))
    z -= 7.0 * np.exp(-((x - 700.0) / 30.0) ** 2) * (1.0 / (1.0 + np.exp((y + 20.0) / 4.0)))
    return z

def hull_pose(x, hl, hw):
    """the server's BuildAngles: ground at front/back/left/right (3/4 out), height = mean of the higher pairs"""
    L, W = hl * 0.75, hw * 0.75
    zF, zB, zL, zR = ground(x + L, 0), ground(x - L, 0), ground(x, W), ground(x, -W)
    gz = 0.5 * (max(zF, zB) + max(zL, zR))
    F = np.array([2 * L, 0, zF - zB]); F /= np.linalg.norm(F)
    R = np.array([0, -2 * W, zR - zL]); R /= np.linalg.norm(R)
    U = np.cross(R, F); U /= np.linalg.norm(U)
    R = np.cross(F, U); F = np.cross(U, R)
    M = np.stack([F, -R, U], axis=1)            # model x = forward, y = left (= -right), z = up
    return np.array([x, 0.0, gz]), M

def rot_y(a):
    c, s = np.cos(a), np.sin(a)
    return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])

def main(rig, var, out, frames=None):
    names, parent, rest, tris, susp, wheels = read_rig(rig, var)
    nb = len(names)
    owner = []                                   # each bone's suspension unit (or -1)
    for b in range(nb):
        q = b
        while q >= 0 and q not in susp: q = parent[q]
        owner.append(q)
    P0 = np.array([[p for p, w in vs] for mat, vs in tris])
    hl, hw = np.ptp(P0[:, :, 0]) / 2, np.ptp(P0[:, :, 1]) / 2
    treads = tread_materials()
    tread = np.array([mat.lower() in treads for mat, vs in tris])
    colour = []
    for (mat, vs), tr in zip(tris, tread):
        b = max(vs[0][1], key=lambda bw: bw[1])[0]
        if tr: colour.append((70, 66, 62))
        elif b in wheels:
            c = rest[b]; mid = np.mean([p for p, w in vs], axis=0) - c
            ang = np.degrees(np.arctan2(mid[2], mid[0])) % 360
            colour.append((190, 60, 48) if int(ang // 45) % 4 == 0 else (128, 132, 122))
        else: colour.append((156, 160, 146))
    colour = np.array(colour, float)
    # belt edges for the stretch check
    edges = []
    for k in np.where(tread)[0]:
        for a, b in ((0, 1), (1, 2), (2, 0)):
            edges.append((k, a, b))
    rest_len = np.array([np.linalg.norm(P0[k, a] - P0[k, b]) for k, a, b in edges])
    n = int(frames or 150)
    lift = {s: 0.0 for s in susp}; seen = {s: [0.0, 0.0] for s in susp}
    W, H = 460, 250
    imgs, worst, worst_abs = [], 1.0, 0.0
    x0 = 150.0
    for f in range(n):
        dt = 1.0 / FPS; x = x0 + SPEED * f * dt; dist = SPEED * f * dt
        O, M = hull_pose(x, hl, hw)
        # suspension: ground under each unit's contact, along the hull's up
        for s, (up, down) in susp.items():
            c = O + M @ rest[s]
            want = (ground(c[0], c[1]) - c[2]) / max(M[2, 2], 0.5)
            want = float(np.clip(want, -down, up))
            step = SUSP_RATE * dt
            lift[s] += float(np.clip(want - lift[s], -step, step))
            seen[s][0] = max(seen[s][0], lift[s]); seen[s][1] = min(seen[s][1], lift[s])
        # bone moves (model space): translation by its unit's lift, wheels spin about their own centre
        Rb, Tb = [], []
        for b in range(nb):
            t = np.array([0, 0, lift[owner[b]]]) if owner[b] >= 0 else np.zeros(3)
            if b in wheels:
                R = rot_y(dist / wheels[b]); c = rest[b]
                Rb.append(R); Tb.append(c + t - R @ c)
            else:
                Rb.append(np.eye(3)); Tb.append(t)
        P = np.zeros_like(P0)
        for k, (mat, vs) in enumerate(tris):
            for j, (p, w) in enumerate(vs):
                P[k, j] = sum(x_ * (Rb[b] @ p + Tb[b]) for b, x_ in w)
        if len(edges):
            L_ = np.array([np.linalg.norm(P[k, a] - P[k, b]) for k, a, b in edges])
            worst = max(worst, float((L_ / np.maximum(rest_len, 1e-3)).max()))
            worst_abs = max(worst_abs, float((L_ - rest_len).max()))
        Pw = P @ M.T + O
        # ground patch around the vehicle
        gx = np.arange(x - hl - 60, x + hl + 60, 8.0); gy = np.arange(-hw - 40, hw + 41, 8.0)
        G = []
        for i in range(len(gx) - 1):
            for j in range(len(gy) - 1):
                q = [np.array([a, b, ground(a, b)]) for a, b in ((gx[i], gy[j]), (gx[i + 1], gy[j]), (gx[i + 1], gy[j + 1]), (gx[i], gy[j + 1]))]
                G += [[q[0], q[1], q[2]], [q[0], q[2], q[3]]]
        G = np.array(G)
        gcol = np.array([(150, 128, 96) if ((int(t[:, 0].mean() // 32) + int(t[:, 1].mean() // 32)) % 2) else (138, 118, 88) for t in G], float)
        cams = [Camera(90, 3, (x, 0, 28), 1.5, W, H), Camera(-40, 10, (x + 20, 0, 25), 1.35, W, H)]
        row = []
        for cam in cams:
            Z = np.full((H, W), -1e9); C = np.zeros((H, W, 3)); C[:] = (232, 236, 240)
            nrm = np.cross(G[:, 1] - G[:, 0], G[:, 2] - G[:, 0]); nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
            light = np.array([0.35, 0.55, 0.76]); light /= np.linalg.norm(light)
            _raster(cam, G, gcol * (0.55 + 0.45 * np.abs(nrm @ light))[:, None], Z, C)
            nrm = np.cross(Pw[:, 1] - Pw[:, 0], Pw[:, 2] - Pw[:, 0]); nrm /= np.linalg.norm(nrm, axis=1, keepdims=True) + 1e-9
            _raster(cam, Pw, (colour * (0.45 + 0.55 * np.abs(nrm @ light))[:, None]).clip(0, 255), Z, C)
            row.append(C.clip(0, 255).astype(np.uint8))
        imgs.append(Image.fromarray(np.concatenate(row, axis=1)))
    imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=int(1000 / FPS), loop=0)
    print('%s: %d frames, %.0f units driven' % (var, n, SPEED * n / FPS))
    for s, (up, down) in sorted(susp.items(), key=lambda kv: names[kv[0]]):
        print('   %-18s travel +%.1f / -%.1f   rose %.1f, dropped %.1f%s' % (names[s], up, down, seen[s][0], -seen[s][1],
              '  OUT OF TRAVEL' if seen[s][0] > up + 0.01 or -seen[s][1] > down + 0.01 else ''))
    print('   worst belt edge stretch x%.3f, longest any belt edge grew %.2f units' % (worst, worst_abs))
    return imgs

if __name__ == '__main__':
    main(*sys.argv[1:4], *(sys.argv[4:5] or [None]))
