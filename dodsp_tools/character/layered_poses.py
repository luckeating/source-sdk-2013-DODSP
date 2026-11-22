"""Poses as the ENGINE draws them, not as the animation files hold them: a DoD stance sequence plays with an aim layer
on top (`addlayer "aimmatrix_*"`, a delta blended by body_pitch / body_yaw - the soldier aims with those, server
npc_german_soldier.cpp) and, in most rifle stances, the left hand's IK rule `ikrule "lhand" touch "...R_Hand"`
(the hand kept where the sequence holds it relative to the right hand, the arm re-solved).

Why (2026-11-22, owner's crouched screenshots): the joint helpers were fitted and checked on the files' own poses. In
@CrouchAim_BOLT the support hand is turned 114 degrees from the mesh's bind; aimed DOWN at the player the aim layer
moves the arms and the touch rule turns the hand with the gun - 135..179 degrees, almost all of it ROLL about the
forearm. None of those poses were in the set, the measure never saw them, and in game the forearm wrung flat.

    layered(qc, smd_dir, skel) -> [(label, {bone: (pos + euler)})]

One pose per (stance sequence, aim layer animation) at full weight - the extremes the blend reaches; the blend
between them stays inside. Delta layers: local rotation = base x delta (the delta in the bone's own frame), position
base + delta; the weightlist zeroes bones it lists at 0 (the legs). CHECKED on the files: base x delta keeps the left
hand on the right (the gun) within 0.7 units / 2 degrees before any IK at every aim extreme; delta x base misses by up
to 20 units / 95 degrees - and an earlier try of this file used that order and reported 180-degree wrist rolls that
don't exist. check_order() repeats the test.
The decompile writes each delta animation raw with a `subtract` corrective; a bone the corrective doesn't list is
identity there. Touch IK: the hand's matrix relative to the touched bone as in the base frame, the arm two-bone
solved with the elbow kept in its plane (Studio_SolveIK keeps the knee direction the same way)."""
import os, re
import numpy as np
from scipy.spatial.transform import Rotation as Rot
from smd import SMD
from xanim_to_smd import rotm, euler

VB = 'ValveBiped.Bip01_'

def parse_qc(qc):
    """{sequences: name -> (files, addlayers, lhand touch bone, delta)}, {animations: name -> (file, subtract, weightlist)},
    {weightlists: name -> {bone: w}}"""
    txt = open(qc, encoding='latin-1').read()
    seqs, anims, wls = {}, {}, {}
    for m in re.finditer(r'^\$(sequence|animation|weightlist)\s+"([^"]+)"(?:\s+"([^"]+)")?\s*\{(.*?)^\}', txt, re.M | re.S):
        kind, name, file, body = m.groups()
        if kind == 'weightlist':
            wls[name] = {b: float(w) for b, w in re.findall(r'"([^"]+)"\s+([-\d.]+)', body)}
        elif kind == 'animation':
            sub = re.search(r'subtract\s+"([^"]+)"', body); wl = re.search(r'weightlist\s+"([^"]+)"', body)
            anims[name] = (file, sub.group(1) if sub else None, wl.group(1) if wl else None)
        else:
            lines = [l.strip() for l in body.splitlines()]
            files = [l.strip('"') for l in lines if re.fullmatch(r'"[^"]+"', l)]
            layers = re.findall(r'addlayer\s+"([^"]+)"', body)
            touch = re.search(r'ikrule\s+"lhand"\s+touch\s+"([^"]+)"', body)
            seqs[name] = (files, layers, touch.group(1) if touch else None, bool(re.search(r'^\s*delta\s*$', body, re.M)))
    return seqs, anims, wls

def _frame(path, k=0):
    a = SMD(path); f = a.frames[min(k, len(a.frames) - 1)][1]
    return {a.nodes[i][0]: v for i, v in f.items()}

def _file(smd_dir, f):
    f = f if f.lower().endswith('.smd') else f + '.smd'
    return os.path.join(smd_dir, f)

def add_delta(base, delta, corr, weights):
    out = dict(base)
    for n, v in base.items():
        w = weights.get(n, 0.0) if weights is not None else 1.0
        if n not in delta or w <= 0: continue
        cr = corr.get(n, (0, 0, 0, 0, 0, 0))
        dq = Rot.from_matrix(rotm(delta[n][3:])) * Rot.from_matrix(rotm(cr[3:])).inv()
        dq = Rot.from_rotvec(dq.as_rotvec() * w)
        q = Rot.from_matrix(rotm(v[3:])) * dq
        p = np.array(v[:3], float) + w * (np.array(delta[n][:3], float) - np.array(cr[:3], float))
        out[n] = tuple(p) + tuple(euler(q.as_matrix()))
    return out

def _turn(u, v):
    u = u / np.linalg.norm(u); v = v / np.linalg.norm(v); ax = np.cross(u, v); s = np.linalg.norm(ax)
    if s < 1e-9: return np.eye(3)
    return Rot.from_rotvec(ax / s * np.arctan2(s, u @ v)).as_matrix()

def touch_ik(skel, base, posed, hand='L', touched=VB + 'R_Hand'):
    """the hand's world matrix relative to `touched` as in `base`; upper arm + forearm re-solved, elbow in its plane"""
    i = skel.idx; U, F, H, T0 = [i[VB + hand + '_' + x] for x in ('UpperArm', 'Forearm', 'Hand')] + [i[touched]]
    Wb = skel.world(skel.pose(base)); loc = skel.pose(posed); Wp = skel.world(loc)
    rel_R = Wb[T0][0].T @ Wb[H][0]; rel_t = Wb[T0][0].T @ (Wb[H][1] - Wb[T0][1])
    Rt = Wp[T0][0] @ rel_R; T = Wp[T0][1] + Wp[T0][0] @ rel_t
    S, E, Hc = Wp[U][1], Wp[F][1], Wp[H][1]
    a, b = np.linalg.norm(E - S), np.linalg.norm(Hc - E)
    c = np.clip(np.linalg.norm(T - S), abs(a - b) + 1e-3, a + b - 1e-3)
    d = (T - S) / max(np.linalg.norm(T - S), 1e-9); pole = (E - S) - d * ((E - S) @ d)
    if np.linalg.norm(pole) < 1e-6: return posed
    pole /= np.linalg.norm(pole); cs = (a * a + c * c - b * b) / (2 * a * c)
    E2 = S + d * a * cs + pole * a * np.sqrt(max(0.0, 1 - cs * cs)); H2 = S + d * c
    r1 = _turn(E - S, E2 - S); RU = r1 @ Wp[U][0]
    Hmid = S + r1 @ (Hc - S); RF = _turn(Hmid - E2, H2 - E2) @ r1 @ Wp[F][0]
    PU = Wp[skel.nodes[U][1]][0]
    fr = dict(posed)
    for k, R in ((U, PU.T @ RU), (F, RU.T @ RF), (H, RF.T @ Rt)):
        fr[skel.nodes[k][0]] = tuple(loc[k][0]) + tuple(euler(R))
    return fr

def check_order(qc, smd_dir, skel, seq='StandAim_BOLT', tol=(2.0, 5.0)):
    """the composition rule tested on one stance: with no IK the left hand must already sit on the right as in the
    base pose (the aim deltas were authored that way) -> worst (units, degrees); SystemExit past tol"""
    seqs, anims, wls = parse_qc(qc)
    files, layers, touch, _ = seqs[seq]; bf = anims[files[0]][0] if files[0] in anims else files[0]
    base = _frame(_file(smd_dir, bf)); W0 = skel.world(skel.pose(base)); i = skel.idx
    H, RH = i[VB + 'L_Hand'], i[VB + 'R_Hand']
    relR = W0[RH][0].T @ W0[H][0]; relt = W0[RH][0].T @ (W0[H][1] - W0[RH][1]); worst = (0.0, 0.0)
    for an in sorted(set(seqs[layers[0]][0])):
        f, sub, wl = anims[an]; cf = (anims[sub][0] if sub in anims else sub) if sub else None
        corr = _frame(_file(smd_dir, cf)) if cf else {}
        W = skel.world(skel.pose(add_delta(base, _frame(_file(smd_dir, f)), corr, wls.get(wl) if wl else None)))
        miss = np.linalg.norm(W[H][1] - (W[RH][1] + W[RH][0] @ relt))
        deg = np.degrees(Rot.from_matrix(W[H][0].T @ W[RH][0] @ relR).magnitude())
        worst = (max(worst[0], miss), max(worst[1], deg))
    if worst[0] > tol[0] or worst[1] > tol[1]:
        raise SystemExit('layered_poses: aim layers composed wrong - the left hand leaves the gun by %.1f units / %.0f deg' % worst)
    return worst

def layered(qc, smd_dir, skel, only=None):
    """every stance sequence with an aim layer, at each of the layer's animations (full weight), touch IK applied"""
    seqs, anims, wls = parse_qc(qc)
    out, cache = [], {}
    def frame(f):
        if f not in cache: cache[f] = _frame(_file(smd_dir, f))
        return cache[f]
    for name, (files, layers, touch, delta) in sorted(seqs.items()):
        if delta or not files or not layers or (only and not any(o in name for o in only)): continue
        bf = files[0]
        bf = anims[bf][0] if bf in anims else bf
        if not os.path.exists(_file(smd_dir, bf)): continue
        base = frame(bf)
        for lay in layers:
            if lay not in seqs: continue
            for an in sorted(set(seqs[lay][0])):
                if an not in anims: continue
                f, sub, wl = anims[an]
                if not os.path.exists(_file(smd_dir, f)): continue
                cf = (anims[sub][0] if sub in anims else sub) if sub else None
                corr = frame(cf) if cf and os.path.exists(_file(smd_dir, cf)) else {}
                p = add_delta(base, frame(f), corr, wls.get(wl) if wl else None)
                if touch and touch.endswith('R_Hand'): p = touch_ik(skel, base, p, 'L', touch)
                out.append(('%s+%s' % (name, an.replace('a_aimmatrix_', '')), p))
    return out
