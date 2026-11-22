"""THE owner of the procedural wrist-twist bones (design, preview, the .vrd, the read-back check) for a converted character - designed from CoD's own rig, previewed with the ENGINE's
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

    python3 -I twist_bones.py <body.smd> <textures dir> <out.png> <anim.smd>[:frame] ...   (the preview)
cod2_package calls add_to_rig / write_vrd; check_compiled reads the helpers back from the owner's compiled .mdl.
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

def trigger_rolls():
    """the hand rolls the triggers sit at: the REST pose first (VDC VRD: the reference trigger comes first, and the
    engine falls back to trigger 0 when no trigger is in reach), then +-TRIGGER_STEP outward to 180"""
    out = [0.0]; k = 1
    while k * TRIGGER_STEP <= 180.0:
        out.append(k * TRIGGER_STEP)
        if k * TRIGGER_STEP < 180.0: out.append(-k * TRIGGER_STEP)
        k += 1
    return out

def vrd_name(n):
    """a bone name as a .vrd must spell it: studiomdl misreads a period, so everything up to and including it goes
    (VDC VRD; HL2's own .vrd files say Bip01_L_Hand for ValveBiped.Bip01_L_Hand). First compile with full names,
    2026-11-22: 'Missing control bone "dodsp_L_ForeTwist" for procedural bone "ValveBiped.Bip01_L_Hand"'."""
    return n.split('.')[-1]

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
    a = float(np.degrees(2 * np.arctan2(np.dot(q[:3], axis), q[3])))
    return (a + 180.0) % 360.0 - 180.0

def rig_twist(ref):
    """the helpers on our skeleton: (side, forearm index, hand index, axis in forearm space, hand bind local q,
    helper base position, triggers)"""
    idx = {n: i for i, (n, _) in enumerate(ref.nodes)}; fr0 = ref.frames[0][1]; out = []
    for side in 'LR':
        f, h = idx[VB + side + '_Forearm'], idx[VB + side + '_Hand']
        hp = np.array(fr0[h][:3]); axis = hp / np.linalg.norm(hp)
        q0 = Rot.from_matrix(rotm(fr0[h][3:])).as_quat()
        trig = []
        for a in trigger_rolls():
            roll = Rot.from_rotvec(np.radians(a) * axis).as_quat()
            trig.append((1.0 / np.radians(TRIGGER_TOL), qmul(roll, q0),
                         Rot.from_rotvec(np.radians(a * TWIST_SHARE) * axis).as_quat(), hp * TWIST_AT))
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
        q, pos = engine_quat_interp(src, trig) if procedural else (np.array([0, 0, 0, 1.0]), base)
        fr[t] = (*pos, *euler(Rot.from_quat(q).as_matrix()))
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

if __name__ == '__main__' and len(sys.argv) > 1 and not sys.argv[1].endswith('.mdl'):
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


def rig_triggers(L, f, h):
    """one side's triggers from a skeleton's local frames L (node -> pos + euler): (axis, q0, base, triggers)"""
    hp = np.array(L[h][:3]); axis = hp / np.linalg.norm(hp); q0 = Rot.from_matrix(rotm(L[h][3:])).as_quat()
    trig = []
    for a in trigger_rolls():
        roll = Rot.from_rotvec(np.radians(a) * axis).as_quat()
        trig.append((1.0 / np.radians(TRIGGER_TOL), qmul(roll, q0),
                     Rot.from_rotvec(np.radians(a * TWIST_SHARE) * axis).as_quat(), hp * TWIST_AT))
    return axis, q0, hp * TWIST_AT, trig

def add_to_rig(rig, tris):
    """cod2_char's rig + mesh get one helper per arm (a child of the forearm, aligned with it, at TWIST_AT) and every
    corner's forearm weight moved onto it by how far down the forearm the corner lies. -> (tris, [(side, helper
    index, forearm index, hand index, triggers)]) - the rig is changed in place"""
    VB_ = VB; L = rig.local(); out = []
    rig.nodes = list(rig.nodes); rig.idx = dict(rig.idx)
    for side in 'LR':
        f, h = rig.idx[VB_ + side + '_Forearm'], rig.idx[VB_ + side + '_Hand']
        axis, q0, base, trig = rig_triggers(L, f, h)
        t = len(rig.nodes); rig.nodes.append((TWIST_NAME % side, f)); rig.idx[TWIST_NAME % side] = t
        Rf, tf = rig.W[f]; rig.W[t] = (Rf, tf + Rf @ base)
        out.append((side, t, f, h, axis, trig))
    new = []
    for m, *corners in tris:
        cs = []
        for p, n, uv, w in corners:
            for side, t, f, h, axis, trig in out:
                fw = sum(x for b, x in w if b == f)
                if not fw: continue
                Rf, tf = rig.W[f]; L_h = np.linalg.norm(trig[0][3]) / TWIST_AT
                share = float(np.clip(np.dot(Rf.T @ (p - tf), axis) / L_h / TWIST_AT, 0.0, 1.0))
                if share <= 0: continue
                w2 = [(b, x * (1 - share) if b == f else x) for b, x in w] + [(t, fw * share)]
                w2 = sorted([(b, x) for b, x in w2 if x > 1e-4], key=lambda bx: -bx[1])[:3]
                s_ = sum(x for _, x in w2); w = [(b, x / s_) for b, x in w2]
            cs.append((p, n, uv, w))
        new.append((m, *cs))
    return new, out

def _deg_euler(q):
    return tuple(np.degrees(euler(Rot.from_quat(q).as_matrix())))

def write_vrd(path, rig, helpers):
    """$proceduralbones file (VDC $proceduralbones): <helper> bone parent driverParent driver, <basepos> (local pos in
    the parent), <trigger> tolerance(deg) driver-local XYZ(deg) helper-local XYZ(deg) helper pos offset from basepos.
    Angles in the SMD's own euler order (studiomdl reads both with the same RadianEuler)."""
    out = ['// generated by dodsp_tools/character/twist_bones.py - wrist-twist helpers (CoD rig: %.2f down the forearm,'
           ' %.2f of the hand roll)' % (TWIST_AT, TWIST_SHARE)]
    for side, t, f, h, axis, trig in helpers:
        name = rig.nodes[t][0]; base = trig[0][3]
        out += ['', '<helper> %s %s %s %s' % tuple(vrd_name(x) for x in (name, rig.nodes[f][0], rig.nodes[f][0], rig.nodes[h][0])),
                '<basepos> %.6f %.6f %.6f' % tuple(base)]
        for inv_tol, tq, q, pos in trig:
            out.append('<trigger> %.3f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f' % (
                np.degrees(1.0 / inv_tol), *_deg_euler(tq), *_deg_euler(q), *(pos - base)))
    open(path, 'w').write('\n'.join(out) + '\n')

def check_compiled(mdl_path, helpers_expected=None, tol_deg=0.5):
    """read the helpers back from a COMPILED .mdl (studio.h: mstudiobone_t proctype 2 = STUDIO_PROC_QUATINTERP;
    mstudioquatinterpbone_t {int control, numtriggers, triggerindex}; mstudioquatinterpinfo_t {float inv_tolerance;
    Quaternion trigger; Vector pos; Quaternion quat} = 48 bytes) and compare them with the design: names, control =
    the hand, trigger count, each trigger's hand / helper rotations and position. -> report lines; raises on a mismatch"""
    import struct
    d = open(mdl_path, 'rb').read(); nb, bi = struct.unpack_from('<ii', d, 156)
    cstr = lambda o: d[o:d.index(b'\0', o)].decode()
    names = [cstr(bi + i * 216 + struct.unpack_from('<i', d, bi + i * 216)[0]) for i in range(nb)]
    lines = []; found = {}
    for i in range(nb):
        o = bi + i * 216; parent = struct.unpack_from('<i', d, o + 4)[0]
        flags, pt, pi = struct.unpack_from('<iii', d, o + 160)
        if pt != 2: continue
        q = o + pi; ctrl, ntr, ti = struct.unpack_from('<iii', d, q)
        trig = []
        for k in range(ntr):
            t = q + ti + k * 48
            inv, = struct.unpack_from('<f', d, t); tq = struct.unpack_from('<4f', d, t + 4)
            pos = struct.unpack_from('<3f', d, t + 20); qq = struct.unpack_from('<4f', d, t + 32)
            trig.append((inv, np.array(tq), np.array(qq), np.array(pos)))
        found[names[i]] = (names[parent], names[ctrl], trig)
        lines.append('%s (parent %s) driven by %s, %d triggers' % (names[i], names[parent], names[ctrl], ntr))
    if helpers_expected is not None:
        for name, parent, ctrl, trig in helpers_expected:
            if name not in found: raise SystemExit('compiled model has no procedural %s - the .vrd did not take' % name)
            fp, fc, ft = found[name]
            if (vrd_name(fp), vrd_name(fc)) != (vrd_name(parent), vrd_name(ctrl)): raise SystemExit('%s: parent/driver %s/%s, designed %s/%s' % (name, fp, fc, parent, ctrl))
            if len(ft) != len(trig): raise SystemExit('%s: %d triggers, designed %d' % (name, len(ft), len(trig)))
            worst = 0.0
            for (i1, tq1, q1, p1), (i2, tq2, q2, p2) in zip(ft, trig):
                for a, b in ((tq1, tq2), (q1, q2)):
                    worst = max(worst, np.degrees(2 * np.arccos(min(1.0, abs(float(np.dot(a, b)))))))
                if abs(i1 - i2) > 1e-3 * i2 or np.linalg.norm(p1 - p2) > 0.01:
                    raise SystemExit('%s: tolerance / position differ (%s %s vs %s %s)' % (name, i1, p1, i2, p2))
            if worst > tol_deg: raise SystemExit('%s: a trigger rotation is %.2f deg off the design' % (name, worst))
            lines.append('%s matches the design (worst rotation %.3f deg)' % (name, worst))
    return lines

def read_vrd(path):
    """a .vrd read back the way studiomdl reads it: [(helper, parent, driver, triggers as (inv_tol, q, q, pos))]"""
    out = []; cur = None
    for line in open(path):
        t = line.split()
        if not t or t[0].startswith('//'): continue
        if t[0] == '<helper>': cur = [t[1], t[2], t[4], [], None]; out.append(cur)
        elif t[0] == '<basepos>': cur[4] = np.array(t[1:4], float)
        elif t[0] == '<trigger>':
            v = list(map(float, t[1:]))
            cur[3].append((1 / np.radians(v[0]), Rot.from_matrix(rotm(np.radians(v[1:4]))).as_quat(),
                           Rot.from_matrix(rotm(np.radians(v[4:7]))).as_quat(), cur[4] + np.array(v[7:10])))
    return [(a, b, c, d) for a, b, c, d, _ in out]

def check_vrd(path, rig, helpers, tol_deg=0.05):
    """the written .vrd read back must give the designed helpers (names, driver, every trigger) - the same comparison
    check_compiled makes on the owner's compile"""
    got = {h[0]: h for h in read_vrd(path)}
    for side, t, f, h, axis, trig in helpers:
        n = vrd_name(rig.nodes[t][0])
        if n not in got: raise SystemExit('%s: %s missing' % (path, n))
        _, parent, driver, tr = got[n]
        if any('.' in x for x in got[n][:3]): raise SystemExit('%s: a bone name with a period (studiomdl misreads it)' % path)
        if (parent, driver) != (vrd_name(rig.nodes[f][0]), vrd_name(rig.nodes[h][0])) or len(tr) != len(trig):
            raise SystemExit('%s: %s reads back wrong' % (path, n))
        for (i1, a1, b1, p1), (i2, a2, b2, p2) in zip(tr, trig):
            for a, b in ((a1, a2), (b1, b2)):
                if np.degrees(2 * np.arccos(min(1.0, abs(float(np.dot(a, b)))))) > tol_deg or np.linalg.norm(p1 - p2) > 1e-3:
                    raise SystemExit('%s: %s trigger reads back off the design' % (path, n))
    return len(helpers)
