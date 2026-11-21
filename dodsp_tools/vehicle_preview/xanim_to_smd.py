"""CoD2 xanim -> SMD in the CoD2 officer port's ValveBiped skeleton (the format build_ww2_pack.py retargets from).

    python3 xanim_to_smd.py learn <cod2 pack dir> <officer decompile dir> <out .npz>
    python3 xanim_to_smd.py convert <out .npz> <cod2 pack dir> <officer decompile dir> <xanim name> <out .smd>

The officer port's animations were made by its porter from these same xanims onto ValveBiped. Rather than guess how,
the mapping is LEARNED from the 434 animations that exist both as an xanim and in the owner's decompile:
  - one global frame (scale, rotation, offset) from CoD's world to the port's;
  - per ValveBiped bone: the CoD bone it follows (by name - see MAP) and the constant turn between their world
    orientations (averaged over every sampled frame; its spread is printed, small = the mapping is right);
  - bones the porter kept still (attachments, 'forward') keep the port's own local transform;
  - every local translation but the pelvis's is the port's own (constant: bone lengths); the pelvis follows
    j_mainroot through the global frame plus learned constants (in its own frame and in the port's world).
    The porter placed some families differently (the MG gunners' roots sit 30-40 higher, runs keep less travel), so
    pelvis POSITIONS are a few units off for most animations and more for those; ROTATIONS (what build_ww2_pack takes
    from these files, with every length but the pelvis's replaced by the DoD German's) match within a degree.
'learn' holds out every 15th animation and reports how well those are rebuilt (local rotation degrees, joint units).
"""
import os, sys, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.dirname(HERE))
from smd import SMD
from render_pose import world as smd_world
from cod2_xmodel import parts
import cod2_xanim as X

VB = 'ValveBiped.Bip01_'
MAP = {'Pelvis': 'j_mainroot', 'Spine': 'j_spine1', 'Spine1': 'j_spine2', 'Spine2': 'j_spine3', 'Spine4': 'j_spine4',
       'Neck1': 'j_neck', 'Head1': 'j_head'}
for side, s in (('L', 'le'), ('R', 'ri')):
    MAP.update({side + '_Clavicle': 'j_clavicle_' + s, side + '_UpperArm': 'j_shoulder_' + s, side + '_Forearm': 'j_elbow_' + s,
                side + '_Hand': 'j_wrist_' + s, side + '_Thigh': 'j_hip_' + s, side + '_Calf': 'j_knee_' + s,
                side + '_Foot': 'j_ankle_' + s, side + '_Toe0': 'j_ball_' + s})
    for k, f in enumerate(('thumb', 'index', 'mid', 'ring', 'pinky')):
        MAP[side + '_Finger%d' % k] = 'j_%s_%s_1' % (f, s)
        MAP[side + '_Finger%d1' % k] = 'j_%s_%s_2' % (f, s)
        MAP[side + '_Finger%d2' % k] = 'j_%s_%s_3' % (f, s)
SKELETON = 'xmodelparts/german_officer_body_4k1'
PREFIX = 'german_normandy_officer_anim_a_'

def cod_bone(vb_name):
    if vb_name.startswith(VB):
        return MAP.get(vb_name[len(VB):])
    if vb_name.startswith('j_'):
        return vb_name                                    # the port kept some CoD helper bones under their own names
    return None

def avg_rot(Ms):
    """the rotation closest to the mean of several (SVD projection)"""
    U, _, Vt = np.linalg.svd(np.sum(Ms, axis=0)); D = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))]); return U @ D @ Vt

def ang(A, B):
    return np.degrees(np.arccos(np.clip((np.trace(A.T @ B) - 1) / 2, -1, 1)))

def shared(pack, decomp):
    out = []
    for f in sorted(os.listdir(decomp)):
        if f.startswith(PREFIX) and f.endswith('.smd'):
            n = f[len(PREFIX):-4]
            if os.path.exists(os.path.join(pack, 'xanim', n)): out.append(n)
    return out

def learn(pack, decomp, out):
    skel = parts(os.path.join(pack, SKELETON))
    names = shared(pack, decomp)
    train = [n for i, n in enumerate(names) if i % 15]; test = [n for i, n in enumerate(names) if not i % 15]
    samples = []           # (cod world dict, smd world dict, smd nodes)
    nodes = None
    for n in train:
        a = X.read(os.path.join(pack, 'xanim', n)); s = SMD(os.path.join(decomp, PREFIX + n + '.smd'))
        nodes = nodes or s.nodes
        for f in range(0, min(a['frames'], len(s.frames)), max(1, a['frames'] // 6)):
            samples.append((X.world(a, skel, f), smd_world(s, f)))
    idx = {nm: i for i, (nm, p) in enumerate(nodes)}
    # 1. the global frame, from the main joints' positions
    A, B = [], []
    for W, Ws in samples:
        for v, c in MAP.items():
            if VB + v in idx and idx[VB + v] in Ws and c in W:
                A.append(W[c][1]); B.append(Ws[idx[VB + v]][1])
    A, B = np.array(A), np.array(B)
    ca, cb = A.mean(0), B.mean(0); U, sv, Vt = np.linalg.svd((B - cb).T @ (A - ca)); D = np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))])
    Rg = U @ D @ Vt; sg = (sv * np.diag(D)).sum() / ((A - ca) ** 2).sum(); tg = cb - sg * Rg @ ca
    res = np.linalg.norm((sg * (Rg @ A.T)).T + tg - B, axis=1)
    print('global frame: scale %.4f, joint miss median %.2f, 95%% %.2f units' % (sg, np.median(res), np.percentile(res, 95)))
    # 2. per bone: the turn between the port's bone and its CoD bone, and how steady it is
    off = {}; spread = {}
    for vn, p in nodes:
        c = cod_bone(vn)
        if not c: continue
        Ms = [(Rg @ W[c][0]).T @ Ws[idx[vn]][0] for W, Ws in samples if c in W and idx[vn] in Ws]
        if not Ms: continue
        O = avg_rot(Ms); off[vn] = O
        spread[vn] = np.median([ang(O, M) for M in Ms])
    bad = {k: v for k, v in spread.items() if v > 3.0}
    print('bones mapped %d of %d; turn spread median %.2f deg; over 3 deg: %s' % (len(off), len(nodes), np.median(list(spread.values())),
          ', '.join('%s %.1f' % (k.replace(VB, ''), v) for k, v in sorted(bad.items(), key=lambda x: -x[1])) or 'none'))
    # 3. the pelvis's place: through the global frame + a constant in its own frame
    pv = idx[VB + 'Pelvis']; L, r = [], []
    for W, Ws in samples:
        Rp = Rg @ W['j_mainroot'][0] @ off[VB + 'Pelvis']
        L.append(Rp); r.append(Ws[pv][1] - (sg * Rg @ W['j_mainroot'][1] + tg))
    # unknowns: c (in the pelvis's frame) and k (in the port's world) - the porter's root sits a constant ~3 units off
    Mx = np.concatenate([np.hstack([Rp, np.eye(3)]) for Rp in L])
    sol = np.linalg.lstsq(Mx, np.concatenate(r), rcond=None)[0]; cpel, kpel = sol[:3], sol[3:]
    # 4. the port's own local transform per bone (first training SMD's first frame): lengths, and the still bones
    s0 = SMD(os.path.join(decomp, PREFIX + train[0] + '.smd'))
    rest = {nm: np.array(s0.frames[0][1][i], float) for i, (nm, p) in enumerate(s0.nodes) if i in s0.frames[0][1]}
    np.savez(out, Rg=Rg, sg=sg, tg=tg + kpel, cpel=cpel, nodes=np.array([n for n, p in nodes]), parents=np.array([p for n, p in nodes]),
             off_names=np.array(list(off)), off=np.array([off[k] for k in off]),
             rest_names=np.array(list(rest)), rest=np.array([rest[k] for k in rest]))
    print('learned from %d animations (%d frames); testing on %d held out' % (len(train), len(samples), len(test)))
    model = load(out)
    worst_rot, worst_joint = [], []
    for n in test:
        a = X.read(os.path.join(pack, 'xanim', n)); s = SMD(os.path.join(decomp, PREFIX + n + '.smd'))
        frames = convert_frames(model, a, skel)
        for f in range(0, min(len(frames), len(s.frames)), max(1, a['frames'] // 5)):
            mine = {i: v for i, v in enumerate(frames[f])}
            Wm = smd_world_from(model, mine); Ws = smd_world(s, f)
            # local rotation error on the mapped bones, joint position error on the main joints
            for vn in off:
                i = idx[vn]
                if i in s.frames[f][1]:
                    worst_rot.append(ang(rotm(mine[i][3:]), rotm(s.frames[f][1][i][3:])))
            for v in MAP:
                if VB + v in idx and idx[VB + v] in Ws:
                    worst_joint.append(np.linalg.norm(Wm[idx[VB + v]][1] - Ws[idx[VB + v]][1]))
    print('held-out: local rotation error median %.2f deg, 95%% %.2f, max %.1f; joint position median %.2f, 95%% %.2f, max %.1f units' % (
        np.median(worst_rot), np.percentile(worst_rot, 95), max(worst_rot), np.median(worst_joint), np.percentile(worst_joint, 95), max(worst_joint)))

def rotm(e):
    x, y, z = e; cx, sx, cy, sy, cz, sz = np.cos(x), np.sin(x), np.cos(y), np.sin(y), np.cos(z), np.sin(z)
    return np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]) @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]])

def euler(R):
    y = np.arcsin(np.clip(-R[2, 0], -1, 1)); return (np.arctan2(R[2, 1], R[2, 2]), y, np.arctan2(R[1, 0], R[0, 0]))

def load(path):
    z = np.load(path, allow_pickle=False)
    return dict(Rg=z['Rg'], sg=float(z['sg']), tg=z['tg'], cpel=z['cpel'], nodes=list(z['nodes']), parents=list(z['parents']),
                off=dict(zip(z['off_names'], z['off'])), rest=dict(zip(z['rest_names'], z['rest'])))

def smd_world_from(model, frame):
    W = {}
    for i, (n, p) in enumerate(zip(model['nodes'], model['parents'])):
        v = frame[i]; R = rotm(v[3:]); t = np.array(v[:3])
        W[i] = (R, t) if p < 0 else (W[p][0] @ R, W[p][0] @ t + W[p][1])
    return W

def convert_frames(model, a, skel):
    """one list per frame: [x y z rx ry rz] per port bone, local - like the port's own SMDs"""
    out = []
    nodes, parents = model['nodes'], model['parents']
    for f in range(a['frames'] + (1 if a['looping'] else 0)):
        W = X.world(a, skel, f)
        world = {}
        frame = []
        for i, (n, p) in enumerate(zip(nodes, parents)):
            rest = model['rest'].get(n)
            c = cod_bone(n)
            if n in model['off'] and c in W:
                Rw = model['Rg'] @ W[c][0] @ model['off'][n]
            else:
                Rl = rotm(rest[3:]) if rest is not None else np.eye(3)
                Rw = world[p][0] @ Rl if p >= 0 else Rl
            if p < 0:
                tw = model['sg'] * model['Rg'] @ W['j_mainroot'][1] + model['tg'] + Rw @ model['cpel']
                world[i] = (Rw, tw); frame.append(list(tw) + list(euler(Rw)))
            else:
                PR, Pt = world[p]
                tl = rest[:3] if rest is not None else np.zeros(3)
                world[i] = (Rw, PR @ tl + Pt)
                frame.append(list(tl) + list(euler(PR.T @ Rw)))
        out.append(frame)
    return out

def convert(model_path, pack, decomp, name, out):
    model = load(model_path)
    skel = parts(os.path.join(pack, SKELETON))
    a = X.read(os.path.join(pack, 'xanim', name))
    frames = convert_frames(model, a, skel)
    lines = ['version 1', 'nodes']
    lines += ['%d "%s" %d' % (i, n, p) for i, (n, p) in enumerate(zip(model['nodes'], model['parents']))]
    lines += ['end', 'skeleton']
    for k, fr in enumerate(frames):
        lines.append('time %d' % k)
        lines += ['%d %.6f %.6f %.6f %.6f %.6f %.6f' % ((i,) + tuple(v)) for i, v in enumerate(fr)]
    lines.append('end')
    open(out, 'w', newline='\n').write('\n'.join(lines) + '\n')
    return a

if __name__ == '__main__':
    if sys.argv[1] == 'learn':
        learn(*sys.argv[2:5])
    else:
        a = convert(*sys.argv[2:7]); print('wrote %s: %d frames at %d fps' % (sys.argv[6], a['frames'], a['fps']))
