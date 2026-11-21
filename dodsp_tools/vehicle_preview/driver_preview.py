"""H2 driver preview: the driver's animations, retargeted exactly as the game gets them (the ride pack built by
build_ww2_pack.py), seated at CoD2's own tag_driver on the rigged halftrack mesh.

    python3 driver_preview.py <rig out dir> <variant> <ride pack dir> <out .png|.gif> <seat x y z> <anim[:frame]> ...

Each anim gets a panel (two views: high from behind looking down into the cab, and from the left side with the near hull cut away); ':frame' picks a frame, or
'name:a-b' plays frames a..b into a GIF (one row per anim). The seat is his origin (the animation's root sits on it).
Prints for each panel where his hands are and how high his pelvis sits over the seat.
"""
import os, sys, numpy as np
from PIL import Image
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from smd import SMD
from render_pose import world
from render3d import Camera, background, splat_figures
from wheel_preview import read_smd

def G(p): return np.array([-p[1], p[0], p[2]])           # pack SMD frame -> his frame (+x = his forward)
RADIUS = (('Head', 4.5), ('Neck', 2.5), ('Spine', 5.5), ('Pelvis', 5.5), ('Thigh', 3.5), ('Calf', 2.8), ('Foot', 2.2),
          ('Toe', 1.8), ('Clavicle', 2.2), ('UpperArm', 2.3), ('Forearm', 2.0), ('Hand', 1.8))
_cache = {}
def frames_of(pack, name):
    if name not in _cache:
        s = SMD(os.path.join(pack, 'ww2_a_' + name + '.smd'))
        _cache[name] = (s, [{i: G(t) for i, (R, t) in world(s, k).items()} for k in range(len(s.frames))])
    return _cache[name]

def figure(pack, name, f, seat):
    s, F = frames_of(pack, name); fr = F[min(f, len(F) - 1)]
    segs = []
    for i, (n, p) in enumerate(s.nodes):
        if not n.startswith('ValveBiped.Bip01') or p < 0 or 'Finger' in n or i not in fr or p not in fr: continue
        r = next((v for k, v in RADIUS if k in n), 1.5)
        segs.append((fr[p] + seat, fr[i] + seat, r))
    hands = [fr[s.index('ValveBiped.Bip01_%s_Hand' % h)] + seat for h in 'LR']
    pel = fr[s.index('ValveBiped.Bip01_Pelvis')] + seat
    return segs, hands, pel

def main(rig, var, pack, out, sx, sy, sz, *anims):
    seat = np.array([float(sx), float(sy), float(sz)])
    names, pivot, tris = read_smd(os.path.join(rig, var + '_body.smd'))
    T = np.array([P for mat, bone, P in tris])
    # the cab is armoured: a high view down into its open top, and a side view with the near (left) hull cut away
    Tcut = np.array([P for P in T if np.mean(np.array(P)[:, 1]) < seat[1] - 2])
    W, H = 380, 300
    cams = [Camera(200, 55, seat + [15, 0, 20], 4.2, W, H), Camera(90, 4, seat + [15, 0, 18], 3.6, W, H)]
    bgs = [background(cams[0], T, colour=(176, 172, 160)), background(cams[1], Tcut, colour=(176, 172, 160))]
    rows, gif = [], False
    for a in anims:
        name, _, fr = a.partition(':')
        if '-' in fr:
            gif = True; f0, f1 = map(int, fr.split('-')); seq = range(f0, f1 + 1)
        else:
            seq = [int(fr) if fr else 0]
        row = []
        for f in seq:
            segs, hands, pel = figure(pack, name, f, seat)
            row.append(np.concatenate([splat_figures(c, Z, C, [(segs, (52, 66, 96))]) for c, (Z, C) in zip(cams, bgs)], axis=1))
        rows.append(row)
        print('%-40s frame %-4s hands L %s R %s, pelvis %.1f over the seat' % (name, fr or '0', hands[0].round(1), hands[1].round(1), pel[2] - seat[2]))
    if gif:
        n = max(len(r) for r in rows)
        imgs = [Image.fromarray(np.concatenate([r[min(k, len(r) - 1)] for r in rows], axis=0)) for k in range(n)]
        imgs[0].save(out, save_all=True, append_images=imgs[1:], duration=33, loop=0)
    else:
        Image.fromarray(np.concatenate([r[0] for r in rows], axis=0)).save(out)
    print('wrote', out)

if __name__ == '__main__':
    main(*sys.argv[1:])
