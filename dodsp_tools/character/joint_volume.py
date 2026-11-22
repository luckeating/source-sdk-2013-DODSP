"""How much a converted character's limbs keep their thickness at every bending joint, over every animation we play -
and the half-angle joint helpers that keep it (designed, approximated the way the engine runs them, measured).

Why (2026-11-22): the twist bone (twist_bones.py) changed nothing the owner could see. Measured along the WHOLE
forearm, the worst pinch sat just above the hand and was the same with or without it: the wrist BENDING. Source skins
with linear blend skinning, and a vertex weighted between two bones that turn apart by an angle a is pulled toward
the joint by cos(a / 2) - the inside of every bent joint collapses (wrists, inner elbows, the crotch / left leg -
the owner's three complaints are one fault). The standard cure with no new skinning code: a helper bone AT the joint
that turns HALF as far as the child; the skin across the joint blends parent -> helper -> child, so neighbours differ
by a / 2 and keep cos(a / 4).

The measure (one owner for "is a joint keeping its shape"): for each joint (shoulder, elbow, wrist, hip, knee, ankle)
the skin within JV_BAND units of it, mostly weighted to its two bones (+ its helper); each vertex's distance to the
bone line through the joint (parent segment + child segment, posed vs bind). Per pose: the 5th percentile of
posed / bind over the joint's vertices. Over all poses: the median and the worst 10% (JV_BAR applies to that).

    python3 -I joint_volume.py <reference smd> <anim smd or dir> ...      (report only - the rig comes from cod2_package)
"""
import os, sys, glob, random
import numpy as np
from scipy.spatial.transform import Rotation as Rot
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), 'vehicle_preview'), os.path.dirname(HERE)]
from smd import SMD
from xanim_to_smd import rotm, euler

VB = 'ValveBiped.Bip01_'
# joint: (parent bone, child bone, the child's far end) - each side
JOINTS = {'shoulder': ('Clavicle', 'UpperArm', 'Forearm'), 'elbow': ('UpperArm', 'Forearm', 'Hand'),
          'wrist': ('Forearm', 'Hand', 'Finger2'), 'hip': ('Pelvis', 'Thigh', 'Calf'), 'knee': ('Thigh', 'Calf', 'Foot'),
          'ankle': ('Calf', 'Foot', 'Toe0')}
JV_BAND = 6.0           # units each side of the joint that count as "the joint"
JV_MIN_R = 0.8          # vertices closer than this to the bone line at rest are left out (ratio noise)
JV_FRAMES = 3           # frames sampled per animation (first, middle, last)
JV_BAR = 0.80           # the worst 10% of poses must keep at least this much of a joint's thickness
JV_BIN = 1.0           # slice width across the joint (units)
JV_SLICE_MIN = 4       # a slice needs this many vertices to count
JV_INNER_BEND = 25.0    # degrees: past this bend the inside of the joint is left out of the measure
JV_INNER_DOT = 0.5      # a vertex is 'inside' when its direction from the bone line is within 60 deg of the bend's inside
JV_LEN_TOL = 0.2        # a pose whose limb bone lengths are off the skeleton's by more is a delta / layer, not a pose
HELPER_NAME = 'dodsp_%s_%s_half'      # side, joint - ours, nothing of Valve's is called that

def side_name(side, b):
    return VB + b if b == 'Pelvis' else VB + side + '_' + b

def qrel(R): return Rot.from_matrix(R).as_quat()

class Skeleton:
    """nodes [(name, parent)], bind local frames (pos, R); helpers appended as nodes whose local frame is computed
    per pose from their driver"""
    def __init__(self, nodes, local):
        self.nodes = list(nodes); self.idx = {n: i for i, (n, p) in enumerate(self.nodes)}
        self.bind = [(np.array(local[i][:3], float), rotm(local[i][3:])) for i in range(len(self.nodes))]
        self.helpers = []        # (index, func(locals) -> (pos, R))
    def add_helper(self, name, parent, pos, R, func):
        i = len(self.nodes); self.nodes.append((name, parent)); self.idx[name] = i
        self.bind.append((np.array(pos, float), R)); self.helpers.append((i, func)); return i
    def world(self, loc):
        loc = list(loc)
        for i, f in self.helpers: loc[i] = f(loc)
        W = []
        for i, (n, p) in enumerate(self.nodes):
            t, R = loc[i]
            if p < 0: W.append((R, t))
            else: PR, Pt = W[p]; W.append((PR @ R, PR @ t + Pt))
        return W
    def pose(self, frame_by_name):
        """locals for a pose: the animation's bones by name, the bind elsewhere"""
        loc = list(self.bind)
        for i, (n, p) in enumerate(self.nodes):
            v = frame_by_name.get(n)
            if v is not None: loc[i] = (np.array(v[:3], float), rotm(v[3:]))
        return loc

def load_poses(paths, frames=JV_FRAMES, limit=None, seed=1):
    """[(label, {bone name: (pos + euler)})] - full-pose animations only (delta / corrective / upper-layer files are
    not poses)"""
    files = []
    for p in paths:
        files += sorted(glob.glob(os.path.join(p, '*.smd'))) if os.path.isdir(p) else [p]
    skip = ('corrective', '@HS_', 'reference', '/u_', 'ragdoll', 'physics', 'body.smd', 'helmet.smd')
    files = [f for f in files if not any(s in f for s in skip)]
    if limit and len(files) > limit: random.Random(seed).shuffle(files); files = sorted(files[:limit])
    out = []
    for f in files:
        try: a = SMD(f)
        except Exception: continue
        if not a.frames or a.index(VB + 'L_Hand') < 0: continue
        n = len(a.frames); picks = sorted({0, n // 2, n - 1})[:frames]
        names = [nm for nm, _ in a.nodes]
        for k in picks:
            fr = a.frames[k][1]; out.append(('%s:%d' % (os.path.basename(f)[:-4].replace('dod_player_shared_anim_', ''), k),
                                             {names[i]: v for i, v in fr.items()}))
    return out

class Mesh:
    """unique corners of a converted character: position, up to 3 (bone, weight)"""
    def __init__(self, tris, nb):
        P, B, W = [], [], []
        seen = {}
        for m, *corners in tris:
            for p, n, uv, w in corners:
                key = (tuple(np.round(p, 4)), tuple(sorted((b, round(x, 4)) for b, x in w)))
                if key in seen: continue
                seen[key] = len(P); P.append(p)
                w = sorted(w, key=lambda bx: -bx[1])[:3] + [(0, 0.0)] * (3 - len(w[:3]))
                B.append([b for b, _ in w]); W.append([x for _, x in w])
        self.P = np.array(P); self.B = np.array(B, int); self.W = np.array(W)
    def skin(self, Wb, Wp):
        """linear blend skinning (Source): sum w (posed x bind^-1) p"""
        M = np.array([np.hstack([Rp @ Rb.T, (tp - Rp @ Rb.T @ tb)[:, None]]) for (Rb, tb), (Rp, tp) in zip(Wb, Wp)])
        out = np.zeros_like(self.P)
        for k in range(3):
            m = M[self.B[:, k]]; out += self.W[:, k, None] * (np.einsum('nij,nj->ni', m[:, :, :3], self.P) + m[:, :, 3])
        return out

def seg_dist(X, a, b):
    d = b - a; t = np.clip(((X - a) @ d) / max(d @ d, 1e-9), 0, 1)
    return np.linalg.norm(X - (a + t[:, None] * d), axis=1), t

class JointProbe:
    """the skin of one joint and its bone line"""
    def __init__(self, skel, mesh, side, joint):
        Pn, Cn, En = JOINTS[joint]
        self.p, self.c, self.e = (skel.idx[side_name(side, x)] for x in (Pn, Cn, En))
        if joint == 'hip':          # the pelvis' line toward the hip is sideways; use the thigh line carried up
            self.p = None
        Wb = skel.world(skel.bind)
        mine = {self.c} | ({self.p} if self.p is not None else {skel.idx[VB + 'Pelvis']})
        mine |= {i for i, (n, p) in enumerate(skel.nodes) if n.startswith('dodsp_%s_%s' % (side, joint))}
        own = sum(np.where(np.isin(mesh.B[:, k], list(mine)), mesh.W[:, k], 0) for k in range(3))
        J, E = Wb[self.c][1], Wb[self.e][1]
        A = Wb[self.p][1] if self.p is not None else 2 * J - E
        r0, s0 = self.radius(mesh.P, A, J, E)
        self.sel = (own >= 0.9) & (np.abs(s0) <= JV_BAND) & (r0 >= JV_MIN_R)
        self.r0 = r0[self.sel]; self.s = s0[self.sel]
        self.slice_of = np.floor(self.s / JV_BIN).astype(int); self.slices = sorted(set(self.slice_of.tolist()))
    def line(self, W):
        J, E = W[self.c][1], W[self.e][1]
        A = W[self.p][1] if self.p is not None else J + (J - E) * (np.linalg.norm(W[self.c][1] - W[self.e][1]) / max(np.linalg.norm(J - E), 1e-9))
        return A, J, E
    @staticmethod
    def radius(X, A, J, E, inner_out=False):
        """distance to the bone line A-J-E and the signed place along it (- = parent side); with inner_out, also
        which vertices lie on the INSIDE of the bend (flesh there is squeezed by a real bend too - not counted)"""
        d1, t1 = seg_dist(X, A, J); d2, t2 = seg_dist(X, J, E)
        s = np.where(d1 < d2, -(1 - t1) * np.linalg.norm(J - A), t2 * np.linalg.norm(E - J))
        if not inner_out: return np.minimum(d1, d2), s
        u1 = (A - J) / max(np.linalg.norm(A - J), 1e-9); u2 = (E - J) / max(np.linalg.norm(E - J), 1e-9)
        bend = np.degrees(np.arccos(np.clip(-u1 @ u2, -1, 1)))
        inner = np.zeros(len(X), bool)
        if bend > JV_INNER_BEND:
            b = u1 + u2; b /= max(np.linalg.norm(b), 1e-9)
            foot = np.where((d1 < d2)[:, None], A + t1[:, None] * (J - A), J + t2[:, None] * (E - J))
            rv = X - foot; inner = (rv @ b) > JV_INNER_DOT * np.linalg.norm(rv, axis=1)
        return np.minimum(d1, d2), s, inner

def pose_valid(skel, fr):
    """a full pose: the limb bones' local positions are the skeleton's (a delta / layered animation's are ~0)"""
    for n in ('L_Forearm', 'R_Forearm', 'L_Calf', 'R_Calf'):
        v = fr.get(VB + n)
        if v is None: return False
        L0 = np.linalg.norm(skel.bind[skel.idx[VB + n]][0]); L = np.linalg.norm(v[:3])
        if abs(L - L0) > JV_LEN_TOL * L0: return False
    return True

def measure(skel, mesh, poses, joints=None, sides='LR'):
    """{(side, joint): per-pose thickness kept}, the worst poses, vertex counts. Per pose and joint: the skin is cut
    into JV_BIN-unit slices across the joint; a slice's thickness = the 5th percentile of posed / bind distance to the
    bone line over its vertices (the inside of a bend left out); the joint's = its thinnest slice. A pinch is one
    ring of skin - pooling the whole joint hid the wrist's (one unit above the hand, 0.53 in DoD's K98 aim)."""
    joints = joints or list(JOINTS)
    probes = {(s, j): JointProbe(skel, mesh, s, j) for s in sides for j in joints}
    Wb = skel.world(skel.bind); res = {k: [] for k in probes}; worst = {k: [] for k in probes}
    for label, fr in poses:
        if not pose_valid(skel, fr): continue
        Wp = skel.world(skel.pose(fr)); X = mesh.skin(Wb, Wp)
        for k, pr in probes.items():
            A, J, E = pr.line(Wp)
            r, _, inner = JointProbe.radius(X[pr.sel], A, J, E, True)
            q = r / pr.r0; keep = ~inner; worst_slice = 1.0
            for b in pr.slices:
                m = keep & (pr.slice_of == b)
                if m.sum() >= JV_SLICE_MIN: worst_slice = min(worst_slice, float(np.percentile(q[m], 5)))
            res[k].append(worst_slice); worst[k].append((worst_slice, label))
    return {k: np.array(v) for k, v in res.items()}, {k: sorted(v)[:5] for k, v in worst.items()}, {k: int(p.sel.sum()) for k, p in probes.items()}

def summary(res, counts=None):
    lines = []
    for (s, j), v in sorted(res.items(), key=lambda kv: (kv[0][1], kv[0][0])):
        p10 = float(np.percentile(v, 10))
        lines.append('%s %-8s median %.2f  worst-10%% %.2f  worst %.2f %s%s' % (s, j, np.median(v), p10, v.min(),
                     '' if p10 >= JV_BAR else ' < %.2f' % JV_BAR, '  (%d verts)' % counts[(s, j)] if counts else ''))
    return lines

# ---------------------------------------------------------------- half-angle helpers
def half_target(q_child, q_bind, k=0.5):
    """the helper's local rotation (parent space): k of the child's turn away from its bind, then the bind"""
    rel = Rot.from_quat(q_child) * Rot.from_quat(q_bind).inv()
    rv = rel.as_rotvec()
    return (Rot.from_rotvec(rv * k) * Rot.from_quat(q_bind)).as_quat()

def add_half_helpers(skel, joints, sides='LR', exact=True, triggers=None, k=0.5):
    """a helper per (side, joint): child of the joint's parent bone, at the joint, oriented like the child at bind,
    turning k of the child's turn. exact=True computes it directly; else from `triggers[(side, joint)]` through the
    engine's DoQuatInterpBone (twist_bones.engine_quat_interp)"""
    import twist_bones as TW
    out = {}
    for s in sides:
        for j in joints:
            Pn, Cn, _ = JOINTS[j]
            p, c = skel.idx[side_name(s, Pn)], skel.idx[side_name(s, Cn)]
            pos, Rb = skel.bind[c]; qb = qrel(Rb)
            if exact:
                f = (lambda c=c, qb=qb, pos=pos: lambda loc: (pos, Rot.from_quat(half_target(qrel(loc[c][1]), qb, k)).as_matrix()))()
            else:
                tr = triggers[(s, j)]
                def f(loc, c=c, tr=tr, pos=pos):
                    q, pp = TW.engine_quat_interp(qrel(loc[c][1]), tr)
                    return (np.array(pp, float), Rot.from_quat(q).as_matrix())
            out[(s, j)] = (skel.add_helper(HELPER_NAME % (s, j), p, pos, Rb, f), p, c)
    return out

def joint_sides(A, J, E, band, joint):
    """the band each side of the joint: never past half the bone it lies on (the shoulder's parent, the clavicle, is
    6 units - a 6-unit band would reach the chest), except the hip's parent side (the pelvis: buttock and belly follow
    the thigh part way, as in any hip rig)"""
    bp = band if joint == 'hip' else min(band, 0.5 * np.linalg.norm(J - A))
    return bp, min(band, 0.5 * np.linalg.norm(E - J))

def toward_child(sv, bp, bc):
    """0 at the parent end of the band, 1/2 on the joint, 1 at the child end (smoothstep each side)"""
    x = np.where(sv < 0, 0.5 + sv / (2 * bp), 0.5 + sv / (2 * bc)); x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)

def split_weight(w, p, c, h, ci):
    """a corner's parent + child weight shared out parent -> helper -> child by ci (toward_child); <= 3 bones kept"""
    t = sum(x for b, x in w if b in (p, c))
    if t <= 1e-4: return w
    rest = [(b, x) for b, x in w if b not in (p, c)]
    parts = [(p, (1 - 2 * ci) * t), (h, 2 * ci * t)] if ci <= 0.5 else [(h, (2 - 2 * ci) * t), (c, (2 * ci - 1) * t)]
    w2 = sorted([(b, x) for b, x in rest + parts if x > 1e-4], key=lambda bx: -bx[1])[:3]
    sm = sum(x for _, x in w2); return [(b, x / sm) for b, x in w2]

def reweight(mesh, helpers, skel, bands):
    """the measuring copy of add_to_rig's re-weighting (same functions)"""
    B, W = mesh.B.copy(), mesh.W.copy(); Wb = skel.world(skel.bind)
    for (s, j), (h, p, c) in helpers.items():
        pr = JointProbe(skel, mesh, s, j); A, J, E = pr.line(Wb)
        _, sv = JointProbe.radius(mesh.P, A, J, E); bp, bc = joint_sides(A, J, E, bands[j], j); cc = toward_child(sv, bp, bc)
        for v in np.where((sv > -bp) & (sv < bc))[0]:
            w = split_weight([(b, x) for b, x in zip(B[v], W[v]) if x > 0], p, c, h, cc[v]) + [(0, 0.0)] * 3
            B[v] = [b for b, _ in w[:3]]; W[v] = [x for _, x in w[:3]]
    m2 = Mesh.__new__(Mesh); m2.P, m2.B, m2.W = mesh.P, B, W
    return m2

# ---------------------------------------------------------------- the engine's version: triggers
JV_FIT_SAMPLES = 1500   # poses the trigger fit and its error are judged on (a seeded sample of all)
JV_TRIGGERS = 32        # the engine's DoQuatInterpBone keeps weight[32] - never more
JV_TOLS = (30.0, 40.0, 50.0, 60.0, 75.0, 90.0)   # trigger tolerances tried (degrees); the best 95th-percentile error wins

def ang(a, b):
    return np.degrees(2 * np.arccos(np.clip(np.abs(np.sum(a * b, -1)), 0, 1)))

def fit_triggers(samples, qb, pos, k=0.5, n=JV_TRIGGERS, seed=1):
    """samples: the child's local quaternions over every pose. Triggers = the bind first, then farthest-point picks
    refined by k-means (angle distance), each with the exact half target; the tolerance that gives the smallest 95th
    percentile error under the ENGINE's blend (twist_bones.engine_quat_interp) is kept.
    -> (triggers [(inv_tol, q_driver, q_target, pos)], tol, (median, 95%, worst) error in degrees)"""
    import twist_bones as TW
    X = np.array(samples, float); X *= np.sign(X @ qb)[:, None]
    rng = np.random.default_rng(seed)
    if len(X) > JV_FIT_SAMPLES: X = X[rng.choice(len(X), JV_FIT_SAMPLES, replace=False)]
    C = [qb]
    d = ang(X, qb[None])
    while len(C) < n and d.max() > 1.0:
        i = int(d.argmax()); C.append(X[i]); d = np.minimum(d, ang(X, X[i][None]))
    C = np.array(C)
    for _ in range(10):                                  # Lloyd steps, the bind stays put
        lab = np.argmin(np.stack([ang(X, c[None]) for c in C], 1), 1)
        for c in range(1, len(C)):
            m = X[lab == c]
            if len(m): v = (m * np.sign(m @ C[c])[:, None]).mean(0); C[c] = v / np.linalg.norm(v)
    tgt = np.array([half_target(c, qb, k) for c in C]); want = np.array([half_target(x, qb, k) for x in X])
    best = None
    for tol in JV_TOLS:
        tr = [(1.0 / np.radians(tol), c, t, np.array(pos, float)) for c, t in zip(C, tgt)]
        got = np.array([TW.engine_quat_interp(x, tr)[0] for x in X])
        e = ang(got, want); score = (float(np.median(e)), float(np.percentile(e, 95)), float(e.max()))
        if best is None or score[1] < best[2][1]: best = (tr, tol, score)
    return best

# ---------------------------------------------------------------- into the package (cod2_package calls these)
# blend band per joint (units each side of the joint across which the skin goes parent -> helper -> child), chosen by
# measurement (band_search in the log): the band that gives the joint's worst 10% of poses the most thickness
JOINT_BAND = {'shoulder': 6.0, 'elbow': 5.0, 'wrist': 1.5, 'hip': 6.0, 'knee': 6.0, 'ankle': 3.0}

def add_to_rig(rig, tris, poses, bands=None):
    """cod2_char's rig + mesh get a half-angle helper per (side, joint) in `bands`: a node under the joint's parent
    bone, at the joint, oriented like the child at bind; triggers fitted to the poses; corners within the band
    re-weighted parent -> helper -> child. -> (tris, spec [dict]) - the rig is changed in place"""
    bands = bands or JOINT_BAND
    sk = Skeleton(rig.nodes, rig.local())
    valid = [fr for _, fr in poses if pose_valid(sk, fr)]
    rig.nodes = list(rig.nodes); rig.idx = dict(rig.idx); spec = []
    for s in 'LR':
        for j, band in bands.items():
            Pn, Cn, En = JOINTS[j]
            p, c, e = (rig.idx[side_name(s, x)] for x in (Pn, Cn, En))
            pos, Rb = sk.bind[c]; qb = qrel(Rb)
            samples = [qrel(sk.pose(fr)[c][1]) for fr in valid]
            tr, tol, err = fit_triggers(samples, qb, pos)
            h = len(rig.nodes); name = HELPER_NAME % (s, j)
            rig.nodes.append((name, p)); rig.idx[name] = h; rig.W[h] = rig.W[c]
            spec.append(dict(side=s, joint=j, helper=h, parent=p, child=c, end=e, band=band, triggers=tr, tol=tol,
                             error=[round(x, 2) for x in err], samples=len(samples)))
    # re-weight: every corner, joint by joint
    flat = [(ti, k) for ti, (m, *cs) in enumerate(tris) for k in range(3)]
    P = np.array([tris[ti][1 + k][0] for ti, k in flat])
    W = [list(tris[ti][1 + k][3]) for ti, k in flat]
    for sp in spec:
        p, c, h, band = sp['parent'], sp['child'], sp['helper'], sp['band']
        J, E = rig.W[c][1], rig.W[sp['end']][1]
        A = rig.W[p][1] if sp['joint'] != 'hip' else J + (J - E)
        _, sv = JointProbe.radius(P, A, J, E); bp, bc = joint_sides(A, J, E, band, sp['joint']); cc = toward_child(sv, bp, bc)
        sp['band_sides'] = (round(float(bp), 2), round(float(bc), 2))
        for v in np.where((sv > -bp) & (sv < bc))[0]:
            W[v] = split_weight(W[v], p, c, h, cc[v])
    new = [list(t) for t in tris]
    for (ti, k), w in zip(flat, W):
        pp, nn, uv, _ = new[ti][1 + k]; new[ti][1 + k] = (pp, nn, uv, w)
    return [tuple(t) for t in new], spec

def _deg_euler(q):
    return tuple(np.degrees(euler(Rot.from_quat(q).as_matrix())))

def vrd_name(n):
    """a bone as a .vrd spells it: studiomdl misreads a period (VDC VRD; twist2's first compile)"""
    return n.split('.')[-1]

def write_vrd(path, rig, spec):
    """$proceduralbones: <helper> bone parent driverParent driver / <basepos> / <trigger> tolerance driver XYZ helper XYZ
    pos offset (degrees, the SMD's euler order - confirmed by twist2's compiled read-back, 0.05 deg)"""
    out = ['// generated by dodsp_tools/character/joint_volume.py - half-angle joint helpers (one per side and joint)']
    for sp in spec:
        h, p, c = sp['helper'], sp['parent'], sp['child']; base = sp['triggers'][0][3]
        out += ['', '<helper> %s %s %s %s' % tuple(vrd_name(rig.nodes[x][0]) for x in (h, p, p, c)),
                '<basepos> %.6f %.6f %.6f' % tuple(base)]
        for inv_tol, tq, q, pos in sp['triggers']:
            out.append('<trigger> %.3f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f' % (
                np.degrees(1.0 / inv_tol), *_deg_euler(tq), *_deg_euler(q), *(pos - base)))
    open(path, 'w').write('\n'.join(out) + '\n')

def check_vrd(path, rig, spec, tol_deg=0.05):
    """the written .vrd read back the way studiomdl reads it must give the designed helpers"""
    import twist_bones as TW
    got = {h[0]: h for h in TW.read_vrd(path)}
    for sp in spec:
        n = vrd_name(rig.nodes[sp['helper']][0])
        if n not in got: raise SystemExit('%s: %s missing' % (path, n))
        _, parent, driver, tr = got[n]
        if any('.' in x for x in got[n][:3]): raise SystemExit('%s: a bone name with a period' % path)
        if (parent, driver) != (vrd_name(rig.nodes[sp['parent']][0]), vrd_name(rig.nodes[sp['child']][0])) or len(tr) != len(sp['triggers']):
            raise SystemExit('%s: %s reads back wrong' % (path, n))
        if len(tr) > JV_TRIGGERS: raise SystemExit('%s: %s has %d triggers - the engine keeps %d' % (path, n, len(tr), JV_TRIGGERS))
        for (i1, a1, b1, p1), (i2, a2, b2, p2) in zip(tr, sp['triggers']):
            if max(ang(a1, a2), ang(b1, b2)) > tol_deg or np.linalg.norm(p1 - p2) > 1e-3 or abs(i1 - i2) > 1e-3 * i2:
                raise SystemExit('%s: %s trigger reads back off the design' % (path, n))
    return len(spec)

def engine_skeleton(rig, spec, vrd=None):
    """the rig as the ENGINE will pose it: helpers from the triggers (read back from the .vrd when given) through
    DoQuatInterpBone"""
    import twist_bones as TW
    n0 = min(sp['helper'] for sp in spec) if spec else len(rig.nodes)
    loc = rig.local(); sk = Skeleton(rig.nodes[:n0], {i: loc[i] for i in range(n0)})
    trig = {vrd_name(h[0]): h[3] for h in TW.read_vrd(vrd)} if vrd else {}
    for sp in spec:
        name = rig.nodes[sp['helper']][0]; tr = trig.get(vrd_name(name), sp['triggers']); c = sp['child']
        def f(lc, c=c, tr=tr):
            q, pp = TW.engine_quat_interp(qrel(lc[c][1]), tr); return (np.array(pp, float), Rot.from_quat(q).as_matrix())
        pos, Rb = sk.bind[c]; sk.add_helper(name, sp['parent'], pos, Rb, f)
    return sk
