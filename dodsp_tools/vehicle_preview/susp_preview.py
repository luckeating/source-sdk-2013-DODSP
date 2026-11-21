"""H1 suspension preview: a drive (pull away, cruise, turn, brake) through c_dodsp_vehicle.cpp's BodySprings maths, the
body tipped about the model's body_pivot over wheels that stay down - side and front views - plus the clearance check:
how far the wing over each steered tyre can come down before it touches the tyre (the nose dips when braking).

    python3 susp_preview.py <rig out dir> <variant> <out .gif> [accel brake speed radius  susp_accel freq damp roll]
"""
import os, re, sys, numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from render3d import Camera
from wheel_preview import read_smd, read_wheels, pose, render, rot

def read_pivot(qc):
    m = re.search(r'"body_pivot"\s*"([-\d.]+)"', open(qc).read()); return float(m.group(1))

def drive(accel=60.0, brake=160.0, vmax=200.0, radius=300.0, k_pitch=1.0, freq=1.3, damp=0.6, k_roll=1.2, dt=1 / 60):
    """the profile the AI drives, and the client's spring on it (same equations, same 0.08 s low-pass)"""
    t, v, yaw, out = 0.0, 0.0, 0.0, []
    sp = acc = yr = 0.0; p = pv = r = rv = 0.0; w = 2 * np.pi * freq
    while t < 9.0:
        if t < 4.0: v = min(v + accel * dt, vmax)
        elif t < 6.0: pass
        else: v = max(v - brake * dt, 0.0)
        omega = v / radius if 4.0 <= t < 6.0 else 0.0          # a left turn at full lock
        a = min(1.0, dt / 0.08); ps = sp
        sp += (v - sp) * a; acc += ((sp - ps) / dt - acc) * a; yr += (omega - yr) * a
        pt = np.clip(acc / 100 * k_pitch, -6, 6); rt = np.clip(sp * yr / 100 * k_roll, -6, 6)
        for _ in range(2):
            h = dt / 2
            pv += (w * w * (pt - p) - 2 * damp * w * pv) * h; rv += (w * w * (rt - r) - 2 * damp * w * rv) * h
            p += pv * h; r += rv * h
        dist = out[-1][1] + v * dt if out else 0.0
        out.append((t, dist, p, r, v)); t += dt
    return out

def main(rig, var, gif, *args):
    names, pivot, tris = read_smd(os.path.join(rig, var + '_body.smd'))
    wheels, wb = read_wheels(os.path.join(rig, var + '.qc')); bp = read_pivot(os.path.join(rig, var + '.qc'))
    prof = drive(*map(float, args)) if args else drive()
    P = np.array([x[2] for x in prof]); R = np.array([x[3] for x in prof])
    print('pitch: nose up max %.2f deg (pulling away), nose down max %.2f deg (braking); roll max %.2f deg (in the turn)'
          % (P.max(), -P.min(), np.abs(R).max()))
    # clearance: the lowest hull point over each steered tyre's top, and how much the wing drops at the worst nose-down
    hull = np.array([p for mat, bone, Q in tris if bone == 'body' for p in Q])
    for name, (r, st) in wheels.items():
        if not st: continue
        c = pivot[name]; Wv = np.array([p for mat, bone, Q in tris if bone == name for p in Q])
        top = Wv[:, 2].max()
        over = hull[(np.abs(hull[:, 0] - c[0]) < r * 0.8) & (np.abs(hull[:, 1] - c[1]) < (np.ptp(Wv[:, 1]) / 2)) & (hull[:, 2] > top)]
        gap = over[:, 2].min() - top if len(over) else float('inf')
        drop = (c[0] - bp) * np.sin(np.radians(-P.min())) + abs(c[1]) * np.sin(np.radians(np.abs(R).max()))
        print('  %s: wing %.1f units over the tyre top; at the worst nose-down + roll the wing comes down %.1f -> %s'
              % (name, gap, drop, 'clear' if drop < gap else 'TOUCHES'))
    W, H = 420, 260
    cams = [Camera(90, 4, (0, 0, 35), 1.25, W, H), Camera(0.01, 6, (0, 0, 35), 1.6, W, H)]
    frames = []
    for i in range(0, len(prof), 6):
        t, dist, p, r, v = prof[i]
        T, col = pose(tris, pivot, wheels, dist, 22.1 if 4.0 <= t < 6.0 else 0.0)
        # the body (every non-wheel triangle) tipped about the pivot line, exactly like the client
        B = rot([1, 0, 0], r) @ rot([0, 1, 0], -p); piv = np.array([bp, 0.0, 0.0])
        for k, (mat, bone, Q) in enumerate(tris):
            if bone not in wheels and bone != 'root': T[k] = (T[k] - piv) @ B.T + piv
        img = np.concatenate([render(T, col, c) for c in cams], axis=1)
        im = Image.fromarray(img)
        frames.append(im)
    frames[0].save(gif, save_all=True, append_images=frames[1:], duration=100, loop=0)
    # the two extremes as stills
    for tag, idx in (('pullaway', int(np.argmax(P))), ('braking', int(np.argmin(P))), ('turn', int(np.argmax(np.abs(R))))):
        t, dist, p, r, v = prof[idx]
        T, col = pose(tris, pivot, wheels, dist, 22.1 if 4.0 <= t < 6.0 else 0.0)
        B = rot([1, 0, 0], r) @ rot([0, 1, 0], -p); piv = np.array([bp, 0.0, 0.0])
        for k, (mat, bone, Q) in enumerate(tris):
            if bone not in wheels and bone != 'root': T[k] = (T[k] - piv) @ B.T + piv
        Image.fromarray(np.concatenate([render(T, col, c) for c in cams], axis=1)).save(gif.replace('.gif', '_%s.png' % tag))
    print('wrote', gif)

if __name__ == '__main__':
    main(*sys.argv[1:])
