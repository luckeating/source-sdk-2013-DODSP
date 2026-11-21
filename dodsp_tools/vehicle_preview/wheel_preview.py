"""H1 preview: a rigged vehicle's wheels turned and steered exactly as client.dll does it, from the rig's own output.

    python3 wheel_preview.py <rig out dir> <variant> <out .gif>

Reads <variant>_body.smd (bones + triangles) and the "dodsp_wheels" $keyvalues of <variant>.qc - the same facts the
game reads - and applies c_dodsp_vehicle.cpp's rule: each wheel turns about the axle (the model's left) through its
own centre by distance / its radius; a steered wheel then turns about the up axis by the steering angle (+ = left).
The wheels are painted in alternating sectors so turning shows. Three views per frame: side (spin), top (steer),
three-quarter (both). Also checks every wheel's triangles stay within its radius of its centre (turning about the
right point) and prints the largest steer each front wheel can take before it passes into the hull.
"""
import os, re, sys, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render3d import Camera, background

def m2g(p): return np.array([-p[1], p[0], p[2]])       # rig mesh frame -> game frame (x forward, y left)

def read_smd(path):
    txt = open(path).read().split('\n')
    i = txt.index('nodes') + 1; names = {}
    while txt[i] != 'end':
        k, n, _ = re.match(r'(\d+) "([^"]+)" (-?\d+)', txt[i]).groups(); names[int(k)] = n; i += 1
    i = txt.index('skeleton') + 2; pivot = {}
    while txt[i] != 'end':
        f = txt[i].split(); pivot[names[int(f[0])]] = m2g(np.array(list(map(float, f[1:4])))); i += 1
    i = txt.index('triangles') + 1; tris = []
    while txt[i] != 'end':
        mat = txt[i]; vs = [txt[i + 1 + k].split() for k in range(3)]
        tris.append((mat, names[int(vs[0][0])], [m2g(np.array(list(map(float, v[1:4])))) for v in vs])); i += 4
    return names, pivot, tris

def read_wheels(qc):
    s = open(qc).read(); m = re.search(r'"dodsp_wheels"\s*\{(.*?)\}', s, re.S)
    out, wb = {}, 0.0
    for k, v in re.findall(r'"([^"]+)"\s*"([^"]+)"', m.group(1)):
        if k == 'wheelbase': wb = float(v); continue
        f = v.split(); d = dict(zip(f[0::2], f[1::2]))
        out[k] = (float(d['radius']), d.get('steer', '0') == '1')
    return out, wb

def rot(axis, deg):
    a = np.radians(deg); axis = np.asarray(axis, float) / np.linalg.norm(axis)
    K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + np.sin(a) * K + (1 - np.cos(a)) * K @ K

def pose(tris, pivot, wheels, dist, steer):
    """client.dll's BuildTransformations, on the rest mesh"""
    T, colour = [], []
    axle, up = np.array([0.0, 1.0, 0.0]), np.array([0.0, 0.0, 1.0])
    for mat, bone, P in tris:
        P = np.array(P)
        if bone in wheels:
            r, st = wheels[bone]; c = pivot[bone]
            R = rot(axle, np.degrees(dist / r))
            if st: R = rot(up, steer) @ R
            # sector colour from the REST angle about the axle, so the sectors turn with the wheel
            mid = P.mean(0) - c; ang = np.degrees(np.arctan2(mid[2], mid[0])) % 360
            P = (P - c) @ R.T + c
            colour.append((205, 72, 58) if int(ang // 45) % 2 else (240, 214, 120))
        else:
            colour.append((150, 156, 142) if 'tread' not in mat else (96, 92, 86))
        T.append(P)
    return np.array(T), colour

def render(T, colour, cam):
    Z, C = background(cam, T, colour=(150, 150, 150))
    # background() shades with one colour: redo the vehicle with per-triangle colours
    from render3d import _raster
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    light = np.array([0.35, 0.55, 0.76]); light /= np.linalg.norm(light)
    k = 0.45 + 0.55 * np.abs(n @ light)
    Z2 = np.full_like(Z, -1e9); C2 = C.copy()
    _raster(cam, T, (np.array(colour, float) * k[:, None]).clip(0, 255), Z2, C2)
    return np.where((Z2 > -1e8)[..., None], C2, C).clip(0, 255).astype(np.uint8)

def main(rig, var, out):
    names, pivot, tris = read_smd(os.path.join(rig, var + '_body.smd'))
    wheels, wb = read_wheels(os.path.join(rig, var + '.qc'))
    # turning about the right point: every wheel triangle within its radius (+ a little) of its centre
    worst = 0.0
    for mat, bone, P in tris:
        if bone in wheels:
            d = np.linalg.norm(np.array(P) - pivot[bone], axis=1).max() / wheels[bone][0]; worst = max(worst, d)
    print('%d wheels (%d steer), wheelbase %.1f; farthest wheel vertex %.2f radii from its centre (1.0 = on the rim; '
          'the axle hub sticks out sideways)' % (len(wheels), sum(s for r, s in wheels.values()), wb, worst))
    # how far the front wheels can steer before their triangles enter the hull's (root) box around them
    hull = np.array([p for mat, bone, P in tris if bone == 'root' and 'tread' not in mat for p in P])
    for name, (r, st) in wheels.items():
        if not st: continue
        c = pivot[name]; Wv = np.array([p for mat, bone, P in tris if bone == name for p in P])
        near = hull[np.linalg.norm(hull - c, axis=1) < r * 2.2]
        # hull vertices within the turned wheel's own cylinder (radius about the turned axle, half-width across it),
        # counted against the straight-ahead count: the hub and axle parts are in it from the start
        def inside(s):
            R = rot([0, 0, 1], s if c[1] > 0 else -s); ax = R @ np.array([0.0, 1.0, 0.0])
            Q = near - c; along = Q @ ax; radial = np.linalg.norm(Q - np.outer(along, ax), axis=1)
            half = np.abs((Wv - c) @ np.array([0.0, 1.0, 0.0])).max()
            return int(((np.abs(along) < half) & (radial < r * 0.98)).sum())
        rest = inside(0); free = 0
        for s in range(0, 46):
            if inside(s) > rest + 2: break
            free = s
        print('  %s: radius %.1f, steers freely to about %d degrees (outward turn) before it meets the hull' % (name, r, free))
    lock = np.degrees(np.arctan(wb / 300.0))
    print('  full lock in game (min_turn_radius 300): %.1f degrees' % lock)
    W, H = 360, 250
    cams = [Camera(90, 0, (0, 0, 30), 1.1, W, H), Camera(0.01, 89.9, (10, 0, 30), 1.1, W, H), Camera(40, 22, (0, 0, 35), 1.0, W, H)]
    frames = []
    N = 36
    for f in range(N):
        t = f / N
        dist = t * 2 * np.pi * 17.5            # one turn of the front tyre
        steer = lock * np.sin(2 * np.pi * t)
        T, col = pose(tris, pivot, wheels, dist, steer)
        row = np.concatenate([render(T, col, c) for c in cams], axis=1)
        img = Image.fromarray(row)
        frames.append(img)
    frames[0].save(out, save_all=True, append_images=frames[1:], duration=90, loop=0)
    frames[N // 4].save(out.replace('.gif', '_left.png')); frames[3 * N // 4].save(out.replace('.gif', '_right.png'))
    print('wrote', out)

if __name__ == '__main__':
    main(*sys.argv[1:4])
