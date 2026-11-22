"""Procedural wrist-twist bones for a converted character - designed from CoD's own rig, previewed with the ENGINE's
own procedural-bone code before anything is compiled.

Why: DoD's poses roll the hand 70-130 deg about the forearm (K98 aim: left 91-104). Our skeleton has no twist bone, so
the whole roll happens in the ring of skin at the wrist (owner 2026-11-21: wrung, pinched wrists). CoD's rig has
j_wristtwist 76% down the forearm taking 84-91% of the hand's roll (measured over 500 CoD2 animations, twist only).
Source does the same with a procedural helper ($proceduralbones .vrd): a bone under the forearm whose rotation is
blended from "triggers" - (hand rotation, helper rotation) pairs - by bone_setup.cpp DoQuatInterpBone:
    w_i = max(0, 1 - 2 acos|q_hand . trigger_i| / tolerance_i), normalised; helper = normalised sum of w_i target_i
(all weights 0 -> trigger 0's target). This file copies that function exactly (engine_quat_interp).

Design (one owner each, used by the preview AND the .vrd writer later):
  TWIST_AT     where the helper sits along the forearm (CoD: 7.30 / 9.65)
  TWIST_SHARE  how much of the hand's roll it takes (CoD: 0.84 left / 0.91 right, symmetric here)
  triggers     the hand rolled TRIGGER_STEP apart round the forearm axis (bind pose otherwise), each with the helper
               rolled TWIST_SHARE as far; tolerance TRIGGER_TOL wide enough that a bent wrist still finds its roll
  weights      each vertex's FOREARM weight moves onto the helper by how far down the forearm it lies:
               0 at the elbow, all of it at the helper (linear) - the hand's weights don't change

    python3 -I twist_preview.py <body.smd> <textures dir> <out.png> <anim.smd>[:frame] ...
prints, per pose and side: the hand's roll, the roll the engine code gives the helper, and the wanted share."""
import os, sys
import numpy as np
from PIL import Image
from scipy.spatial.transform import Rotation as Rot
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), 'vehicle_preview'), os.path.dirname(HERE)]
import char_preview as CP
from render_pose import world
from render3d import Camera
from xanim_to_smd import euler, rotm

VB = 'ValveBiped.Bip01_'
TWIST_AT = 7.30 / 9.65
TWIST_SHARE = 0.87
TRIGGER_STEP = 30.0           # degrees of hand roll between triggers (-180..150: 12 triggers, the engine allows 32)
TRIGGER_TOL = 90.0            # degrees: the "angle of influence" of every trigger
TWIST_NAME = 'dodsp_%s_ForeTwist'   # ours - nothing of ours shares a name with Valve's

def qmul(a, b): return (Rot.from_quat(a) * Rot.from_quat(b)).as_quat()

def engine_quat_interp(src, triggers):
    """bone_setup.cpp DoQuatInterpBone, line for line: src = the driver's quaternion in its parent's space;
    triggers = [(inv_tolerance, trigger q, target q, target pos)] -> (q, pos) of the helper in its parent's space"""
    w = []
    for inv_tol, trig, q, p in triggers:
        dot = min(abs(float(np.dot(trig, src))), 1.0)
        w.append(max(0.0, 1 - (2 * np.arccos(dot) * inv_tol)))
    scale = sum(w)
    if scale <= 0.001:
        return triggers[0][2], triggers[0][3]
    quat = np.zeros(4); pos = np.zeros(3)
    for wi, (inv_tol, trig, q, p) in zip(w, triggers):
        if wi:
            s = wi / scale
            if np.dot(q, quat) < 0: q = -q                  # QuaternionAlign
            quat = quat + s * q; pos = pos + s * p
    return quat / np.linalg.norm(quat), pos

def twist_angle(q, axis):
    """the roll part of rotation q about axis (degrees, -180..180)"""
    return float(np.degrees(2 * np.arctan2(np.dot(q[:3], axis), q[3])))

def rig_twist(ref):
    """the helpers on our skeleton: (side, forearm index, hand index, axis in forearm space, hand bind local q,
    helper base position, triggers)"""
    idx = {n: i for i, (n, _) in enumerate(ref.nodes)}; fr0 = ref.frames[0][1]; out = []
    for side in 'LR':
        f, h = idx[VB + side + '_Forearm'], idx[VB + side + '_Hand']
        hp = np.array(fr0[h][:3]); axis = hp / np.linalg.norm(hp)
        q0 = Rot.from_matrix(rotm(fr0[h][3:])).as_quat()
        trig = []
        for a in np.arange(-180.0, 180.0, TRIGGER_STEP):
            roll = Rot.from_rotvec(np.radians(a) * axis).as_quat()
            trig.append((1.0 / np.radians(TRIGGER_TOL), qmul(roll, q0),
                         Rot.from_rotvec(np.radians(a * TWIST_SHARE) * axis).as_quat(), np.zeros(3)))
        out.append((side, f, h, axis, q0, hp * TWIST_AT, trig))
    return out

def add_helpers(ref, P, N, Wb, rig):
    """the skeleton with one helper per arm (aligned with its forearm) and every forearm weight split onto it"""
    nodes = list(ref.nodes); fr = dict(ref.frames[0][1]); B = world(ref, 0)
    newW = [list(w) for w in Wb]; flat = P.reshape(-1, 3)
    for side, f, h, axis, q0, base, trig in rig:
        t = len(nodes); nodes.append((TWIST_NAME % side, f)); fr[t] = (*base, 0.0, 0.0, 0.0)
        Rf, tf = B[f]; L = np.linalg.norm(base) / TWIST_AT
        for k, w in enumerate(newW):
            fw = sum(x for b, x in w if b == f)
            if not fw: continue
            along = float(np.dot(Rf.T @ (flat[k] - tf), axis)) / L       # 0 at the elbow, 1 at the wrist
            share = float(np.clip(along / TWIST_AT, 0.0, 1.0))
            if share <= 0: continue
            w2 = [(b, x * (1 - share) if b == f else x) for b, x in w] + [(t, fw * share)]
            newW[k] = sorted([(b, x) for b, x in w2 if x > 1e-4], key=lambda bx: -bx[1])[:3]
            s = sum(x for _, x in newW[k]); newW[k] = [(b, x / s) for b, x in newW[k]]
    r2 = type(ref)(); r2.nodes = nodes; r2.frames = [(0, fr)]
    return r2, newW

def posed(ref2, ref, anim, frame, rig, procedural=True):
    """a pose of the animation on the skeleton with helpers; the helpers placed by the engine's code"""
    p = CP.pose_from(ref, anim, frame); fr = dict(p.frames[0][1]); rows = []
    fr0 = ref2.frames[0][1]
    for side, f, h, axis, q0, base, trig in rig:
        t = [i for i, (n, _) in enumerate(ref2.nodes) if n == TWIST_NAME % side][0]
        src = Rot.from_matrix(rotm(fr[h][3:])).as_quat()
        q, pos = engine_quat_interp(src, trig) if procedural else (np.array([0, 0, 0, 1.0]), np.zeros(3))
        fr[t] = (*(base + pos), *euler(Rot.from_quat(q).as_matrix()))
        hand_roll = twist_angle(qmul(src, Rot.from_quat(q0).inv().as_quat()), axis)
        rows.append((side, hand_roll, twist_angle(q, axis)))
    p2 = type(ref2)(); p2.nodes = ref2.nodes; p2.frames = [(0, fr)]
    return p2, rows

def closeups(ref, pose, P, N, UV, mats, tex, Wb):
    Pp, Np = CP.skin(ref, pose, P, N, Wb); W = world(pose, 0); idx = {n: i for i, (n, _) in enumerate(ref.nodes)}
    out = []
    for side in 'LR':
        c = W[idx[VB + side + '_Hand']][1] * 0.6 + W[idx[VB + side + '_Forearm']][1] * 0.4
        for az in (0, 90, 180, 270):
            cam = Camera(az, 15, c, 20, 220, 220); Z = np.full((220, 220), -1e9); C = np.zeros((220, 220, 3)); C[:] = (228, 230, 226)
            CP.render(cam, Pp, Np, UV, mats, tex, Z, C); out.append(C.astype(np.uint8))
    return np.concatenate(out, 1)

if __name__ == '__main__':
    body, texdir, out = sys.argv[1:4]
    ref, mats, P, N, UV, Wb = CP.read_mesh(body)
    tex = {m: np.array(Image.open(os.path.join(texdir, m + '.png')).convert('RGB')) for m in set(mats)}
    rig = rig_twist(ref); ref2, Wb2 = add_helpers(ref, P, N, Wb, rig)
    rows = []
    for spec in sys.argv[4:]:
        path, fr = (spec.rsplit(':', 1) + ['0'])[:2]
        before = closeups(ref, CP.pose_from(ref, path, int(fr)), P, N, UV, mats, tex, Wb)
        p2, info = posed(ref2, ref, path, int(fr), rig)
        after = closeups(ref2, p2, P, N, UV, mats, tex, Wb2)
        for side, hr, tr in info:
            print('%-40s %s hand roll %7.1f  helper (engine code) %7.1f  wanted %7.1f' % (os.path.basename(path)[:40], side, hr, tr, hr * TWIST_SHARE))
        rows += [before, after]
    Image.fromarray(np.concatenate(rows, 0)).save(out); print('wrote', out)

def arm_metrics(ref, pose, P, Wb, side):
    """the forearm skin's worst squeeze: per vertex near the wrist, its distance from the forearm axis posed (in the
    forearm's frame, after removing the hand's bend at the wrist ring) / at rest -> (min ratio, 5th percentile)"""
    idx = {n: i for i, (n, _) in enumerate(ref.nodes)}; f = idx[VB + side + '_Forearm']; h = idx[VB + side + '_Hand']
    B = world(ref, 0); A = world(pose, 0)
    flat = P.reshape(-1, 3); axis = B[h][1] - B[f][1]; L = np.linalg.norm(axis); axis /= L
    M = {i: (A[i][0] @ B[i][0].T, A[i][1] - A[i][0] @ B[i][0].T @ B[i][1]) for i in B if i in A}
    ratios = []
    for k, (p, w) in enumerate(zip(flat, Wb)):
        along = float(np.dot(p - B[f][1], axis)) / L
        if not (0.55 <= along <= 1.02) or not any(b in (f, h) or ref.nodes[b][0].startswith('dodsp_' + side) for b, _ in w): continue
        if sum(x for b, x in w if b == f or b == h or ref.nodes[b][0].startswith('dodsp_' + side)) < 0.9: continue
        r0 = np.linalg.norm((p - B[f][1]) - np.dot(p - B[f][1], axis) * axis)
        q = sum(x * (M[b][0] @ p + M[b][1]) for b, x in w)
        # in the posed forearm frame
        ax2 = A[f][0] @ (B[f][0].T @ axis); d = q - A[f][1]; r1 = np.linalg.norm(d - np.dot(d, ax2) * ax2)
        if r0 > 0.8: ratios.append(r1 / r0)
    r = np.array(ratios); return float(r.min()), float(np.percentile(r, 5)), len(r)

def arm_views(ref, pose, P, N, UV, mats, tex, Wb, side):
    """only this side's forearm + hand, seen square-on from four sides round the forearm"""
    idx = {n: i for i, (n, _) in enumerate(ref.nodes)}; f = idx[VB + side + '_Forearm']; h = idx[VB + side + '_Hand']
    keep_b = {f, h} | {i for i, (n, _) in enumerate(ref.nodes) if n.startswith(VB + side + '_Finger') or n.startswith('dodsp_' + side)}
    T = len(mats); sel = []
    for t in range(T):
        ok = True
        for c in range(3):
            w = Wb[3 * t + c]
            if sum(x for b, x in w if b in keep_b) < 0.5: ok = False
        if ok: sel.append(t)
    sel = np.array(sel)
    Pp, Np = CP.skin(ref, pose, P, N, Wb); W = world(pose, 0)
    c = (W[f][1] + W[h][1]) / 2; ax = W[h][1] - W[f][1]; ax /= np.linalg.norm(ax)
    az0 = np.degrees(np.arctan2(ax[1], ax[0]))
    out = []
    for k, (az, el) in enumerate(((az0 + 90, 0), (az0 - 90, 0), (az0 + 90, 70), (az0 + 90, -70))):
        cam = Camera(az, el, c, 16, 260, 200); Z = np.full((200, 260), -1e9); C = np.zeros((200, 260, 3)); C[:] = (228, 230, 226)
        CP.render(cam, Pp[sel], Np[sel], UV[sel], [mats[i] for i in sel], tex, Z, C); out.append(C.astype(np.uint8))
    return np.concatenate(out, 1)

def report(body, texdir, out, specs):
    ref, mats, P, N, UV, Wb = CP.read_mesh(body)
    tex = {m: np.array(Image.open(os.path.join(texdir, m + '.png')).convert('RGB')) for m in set(mats)}
    rig = rig_twist(ref); ref2, Wb2 = add_helpers(ref, P, N, Wb, rig)
    Wflat = [w for w in Wb]; rows = []
    for spec in specs:
        path, fr = (spec.rsplit(':', 1) + ['0'])[:2]
        p1 = CP.pose_from(ref, path, int(fr)); p2, info = posed(ref2, ref, path, int(fr), rig)
        for side, hr, tr in info:
            m1 = arm_metrics(ref, p1, P, Wflat, side); m2 = arm_metrics(ref2, p2, P, Wb2, side)
            print('%-28s %s hand roll %6.1f | wrist kept: now min %.2f (5%% %.2f)  with twist bone min %.2f (5%% %.2f)'
                  % (os.path.basename(path).replace('dod_player_shared_anim_', '')[:28], side, hr, m1[0], m1[1], m2[0], m2[1]))
            if abs(hr) > 30:
                rows.append(np.concatenate([arm_views(ref, p1, P, N, UV, mats, tex, Wflat, side),
                                            arm_views(ref2, p2, P, N, UV, mats, tex, Wb2, side)], 0))
    Image.fromarray(np.concatenate(rows, 0)).save(out); print('wrote', out)
