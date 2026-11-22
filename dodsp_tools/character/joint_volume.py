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
JV_DEEP_BEND = 90.0     # past this bend the whole inside half is squeezed against the other bone (a crouched knee's crease) - left out too
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
    def __init__(self, tris, nb, dedup=True):
        """dedup=False keeps every triangle corner in order (two meshes of the same triangles then line up)"""
        P, B, W = [], [], []
        seen = {}
        for m, *corners in tris:
            for p, n, uv, w in corners:
                key = (tuple(np.round(p, 4)), tuple(sorted((b, round(x, 4)) for b, x in w)))
                if dedup and key in seen: continue
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
        mine |= {i for i, (n, p) in enumerate(skel.nodes) if n.startswith('dodsp_%s_' % side) and (p in mine or n.startswith('dodsp_%s_%s' % (side, joint)))}
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
            rv = X - foot; inner = (rv @ b) > (JV_INNER_DOT if bend <= JV_DEEP_BEND else 0.0) * np.linalg.norm(rv, axis=1)
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

# ---------------------------------------------------------------- the helpers (one design table, one target rule)
# Per joint: the blend band (units each side of the joint across which the skin goes parent -> helpers -> child), the
# helpers' shares of the child's turn, and roll helpers along the PARENT bone. Chosen by measurement (2026-11-22):
#  - knee: two helpers (1/3, 2/3) - a crouched knee folds 146 deg; one half-way helper leaves 73 deg between
#    neighbours (cos 36 = 0.81 of the leg kept between them, a pointed bulb in game); two leave 49 deg (0.91). Band 8:
#    deep-crouch knees median 0.33 / 0.46 (R / L) vs 0.21 / 0.29 with one helper at 6, no other pose thinner.
#    BUT the calf side of that band SWELLED the knee (joints2 in game, owner 2026-11-22: "nothing changed", a bulb under
#    the kneeling knee): calf skin turned by the helpers' 49/97 deg instead of 146 sticks out below the joint. Scored
#    both ways (thinner AND fatter: 95th percentile radius ratio) over the held poses through the ENGINE's triggers,
#    the band is 8 on the thigh, 5 on the calf: swell p90 (L / R) 1.37 / 1.33 at 8 -> 1.17 / 1.18 (original 1.06 /
#    1.09), thinning p10 kept (0.77 / 0.21). Calf 4 thins the left knee again (0.31), calf 6 the kneeling right
#    (0.07). The prone crawl's left knee ends 0.33 -> 0.15 at one frame (listed by the guard; looked at: no visible
#    pinch). Tried and NOT used: a
#    1-2 unit band on the inside of the bend only ('inside_band', place_folds) - it pinches the back of the knee at
#    moderate bends (0.81 -> 0.19).
#  - elbow: two helpers, band 4 - held poses' worst 10% 0.83 / 0.90 (L / R) -> 0.89 / 0.95 against one at 5; a few
#    extreme held poses already collapsed before (prone Panzerschreck / pistol, MG crouch-walk: 0.39-0.49) end 0.1-0.16
#    lower with any design tried - cod2_package lists them (JV_POSE_DROP_MAX bounds them).
#  - wrist: the hand ROLLS about the forearm - the K98 support hand -100 deg in the aims, -150 aimed down crouched (the
#    owner's screenshot: forearm wrung flat). A roll can't be taken at the wrist: four roll helpers along the forearm's
#    far half (0.2 / 0.4 / 0.6 / 0.8 of the roll at 0.45 / 0.6 / 0.75 / 0.9 of its length - CoD's own wrist twist bone
#    takes 0.84 of it at 0.76) and the wrist helper takes the WHOLE roll and half the bend ('swing'). Measured over
#    the files + the aim-layer poses (layered_poses.py): support wrist worst 10% 0.83 -> 0.90, stance poses 0.78 -> 0.89;
#    three evenly spread (0.25 .. 0.75) gave 0.86.
JOINT_DESIGN = {
    'shoulder': dict(band=6.0, fracs=(0.5,)),
    'elbow': dict(band=4.0, fracs=(1.0 / 3, 2.0 / 3)),
    'wrist': dict(band=1.5, fracs=(0.5,), roll=((0.45, 0.2), (0.6, 0.4), (0.75, 0.6), (0.9, 0.8))),
    'hip': dict(band=6.0, fracs=(0.5,)),
    'knee': dict(band=(8.0, 5.0), fracs=(1.0 / 3, 2.0 / 3)),
    'ankle': dict(band=3.0, fracs=(0.5,)),
}
JV_FOLD_FULL = 0.6      # a corner this far toward the fold direction (cos) takes the inside band in full
JV_LIMB_REACH = 1.15    # a joint's band takes skin out to this x the limb's own radius (split_weights) - never the torso / gear beside it
JV_ROLL_CLEAR = 20.0    # degrees: no held pose may roll this close to a roll helper's branch cut (place_cuts)

def half_target(q_child, q_bind, k=0.5):
    """k of the child's turn away from its bind, then the bind (parent space)"""
    rel = Rot.from_quat(q_child) * Rot.from_quat(q_bind).inv()
    return (Rot.from_rotvec(rel.as_rotvec() * k) * Rot.from_quat(q_bind)).as_quat()

def roll_angle(q_child, q_bind, axis):
    """the child's roll about `axis` (parent space, unit) away from its bind, degrees in (-180, 180]"""
    v = (Rot.from_quat(q_child) * Rot.from_quat(q_bind).inv()).as_quat()
    p = (v[:3] @ axis) * axis; tw = np.array([*p, v[3]])
    if np.linalg.norm(tw) < 1e-9: return 180.0
    a = np.degrees(Rot.from_quat(tw / np.linalg.norm(tw)).as_rotvec() @ axis)
    return a if a > -180 else a + 360

def target(sp, q_child):
    """THE helper rule: (local rotation, local position) of helper `sp` for its driver's local rotation.
    turn: frac of the whole turn. swing: the whole roll about the parent bone and frac of the rest (the bend).
    roll: frac of the roll, unwrapped at the helper's branch cut sp['cut'] (a half-roll has two answers; the cut is
    put where no pose goes)."""
    qb, kind, k = sp['qb'], sp['kind'], sp['frac']
    if kind == 'turn': return half_target(q_child, qb, k), sp['pos']
    ax = sp['axis']; a = roll_angle(q_child, qb, ax)
    if kind == 'swing':
        rel = Rot.from_quat(q_child) * Rot.from_quat(qb).inv(); tw = Rot.from_rotvec(np.radians(a) * ax)
        sw = rel * tw.inv()
        return (Rot.from_rotvec(sw.as_rotvec() * k) * tw * Rot.from_quat(qb)).as_quat(), sp['pos']
    if a > sp['cut']: a -= 360.0
    return (Rot.from_rotvec(np.radians(k * a) * ax) * Rot.from_quat(qb)).as_quat(), sp['pos']

def design_helpers(nodes, local, design=None, sides='LR'):
    """the helper specs (no triggers yet): per side and joint, its turn / swing helpers (under the joint's parent, at
    the joint) and roll helpers (under the parent, along it). Order = node order once added."""
    design = design or JOINT_DESIGN; idx = {n: i for i, (n, p) in enumerate(nodes)}; out = []
    for s in sides:
        for j, dz in design.items():
            Pn, Cn, En = JOINTS[j]
            p, c, e = (idx[side_name(s, x)] for x in (Pn, Cn, En))
            pos = np.array(local[c][:3], float); qb = qrel(rotm(local[c][3:]))
            axis = pos / max(np.linalg.norm(pos), 1e-9)
            fr = dz['fracs']; kind = 'swing' if dz.get('roll') else 'turn'
            for i, k in enumerate(fr):
                name = HELPER_NAME % (s, j) if len(fr) == 1 else 'dodsp_%s_%s_%d' % (s, j, i + 1)
                ub = np.array(local[e][:3], float); ub /= max(np.linalg.norm(ub), 1e-9)
                out.append(dict(side=s, joint=j, name=name, kind=kind, frac=k, level=(i + 1) / (len(fr) + 1), parent=p,
                                child=c, end=e, band=dz['band'], pos=pos, qb=qb, axis=axis, cut=180.0, u_child=ub,
                                inside_band=dz.get('inside_band')))
            for i, r in enumerate(dz.get('roll', ())):
                t, k = r if isinstance(r, tuple) else (r, r)          # (place along the bone, share of the roll)
                out.append(dict(side=s, joint=j, name='dodsp_%s_%s_roll%d' % (s, j, i + 1), kind='roll', frac=k, at=t, **{'from': dz.get('roll_from', 0.0)},
                                parent=p, grand=nodes[p][1], child=c, end=e, band=dz['band'], pos=pos * t, qb=qb, axis=axis, cut=180.0))
    return out

def place_cuts(specs, samples_of, stance=None):
    """each roll helper's branch cut (a half-roll has two answers; poses either side of the cut get the far one, a
    motion across it pops): where the fewest poses come within JV_ROLL_CLEAR of it, stance poses (held poses: the
    stances and aim extremes, `stance` = a mask over the samples) counting a thousand times - and none may be that
    close. Measured 2026-11-22: the K98 support hand's roll leaves no empty 40-degree gap over every animation (deaths
    and prone transitions are scattered round the circle), but a cut at ~+127 has no held pose within 20 degrees."""
    for sp in specs:
        if sp['kind'] != 'roll': continue
        a = np.array([roll_angle(q, sp['qb'], sp['axis']) for q in samples_of(sp)])
        wt = np.ones(len(a)) if stance is None else np.where(stance, 1000.0, 1.0)
        best = None
        for c in np.arange(-180.0, 180.0, 1.0):
            near = np.abs((a - c + 180) % 360 - 180) < JV_ROLL_CLEAR
            score = float(wt[near].sum())
            if best is None or score < best[0]: best = (score, c, int(near.sum()))
        sp['cut'] = best[1]; sp['near_cut'] = best[2]
        if best[0] >= 1000:
            raise SystemExit('joint_volume: %s - every place for the roll\'s branch cut has a held pose within %d deg'
                             % (sp['name'], JV_ROLL_CLEAR))
    return specs

def place_folds(specs, samples_of):
    """hinge joints (design key 'inside_band'): the direction the child folds toward, in the PARENT's frame - the mean
    way the child's far end moves over every pose bent past 45 degrees. The skin on that side of the joint is the crease."""
    for sp in specs:
        if sp['kind'] == 'roll' or sp.get('inside_band') is None: continue
        qb = Rot.from_quat(sp['qb']); u = qb.apply(sp['u_child']); acc = np.zeros(3); n = 0
        for q in samples_of(sp):
            v = Rot.from_quat(q).apply(sp['u_child'])
            if np.degrees(np.arccos(np.clip(v @ u, -1, 1))) < 45: continue
            dv = v - u; dv -= (dv @ u) * u; acc += dv / max(np.linalg.norm(dv), 1e-9); n += 1
        if n:
            acc -= (acc @ u) * u
            if np.linalg.norm(acc) > 1e-6: sp['fold'] = acc / np.linalg.norm(acc)
    return specs

def helper_skeleton(nodes, local, specs, engine=False):
    """the skeleton with the helpers: exact (target) or as the ENGINE runs them (sp['triggers'] through
    DoQuatInterpBone)"""
    import twist_bones as TW
    sk = Skeleton(nodes, local)
    for sp in specs:
        c = sp['child']
        if engine:
            def f(loc, c=c, tr=sp['triggers']):
                q, pp = TW.engine_quat_interp(qrel(loc[c][1]), tr); return (np.array(pp, float), Rot.from_quat(q).as_matrix())
        else:
            def f(loc, c=c, sp=sp):
                q, pp = target(sp, qrel(loc[c][1])); return (np.array(pp, float), Rot.from_quat(q).as_matrix())
        sp['helper'] = sk.add_helper(sp['name'], sp['parent'], sp['pos'], Rot.from_quat(sp['qb']).as_matrix(), f)
    return sk

def joint_sides(A, J, E, band, joint):
    """the band each side of the joint: never past half the bone it lies on (the shoulder's parent, the clavicle, is
    6 units - a 6-unit band would reach the chest), except the hip's parent side (the pelvis: buttock and belly follow
    the thigh part way, as in any hip rig)"""
    bp_, bc_ = band if isinstance(band, tuple) else (band, band)        # (parent side, child side) or one for both
    bp = bp_ if joint == 'hip' else min(bp_, 0.5 * np.linalg.norm(J - A))
    return bp, min(bc_, 0.5 * np.linalg.norm(E - J))

def toward_child(sv, bp, bc):
    """0 at the parent end of the band, 1/2 on the joint, 1 at the child end (smoothstep each side)"""
    x = np.where(sv < 0, 0.5 + sv / (2 * bp), 0.5 + sv / (2 * bc)); x = np.clip(x, 0, 1)
    return x * x * (3 - 2 * x)

def split_levels(w, bones, at, x):
    """a corner's weight on the chain `bones` (first and last are real bones, those between helpers at `at`, all in
    0..1) shared out by its place x on the chain: between the two levels around it, linearly; <= 3 bones kept"""
    t = sum(v for b, v in w if b in (bones[0], bones[-1]))
    if t <= 1e-4: return w
    rest = [(b, v) for b, v in w if b not in (bones[0], bones[-1])]
    i = int(np.clip(np.searchsorted(at, x, side='right') - 1, 0, len(at) - 2))
    f = float(np.clip((x - at[i]) / max(at[i + 1] - at[i], 1e-9), 0, 1))
    w2 = sorted([(b, v) for b, v in rest + [(bones[i], (1 - f) * t), (bones[i + 1], f * t)] if v > 1e-4], key=lambda bv: -bv[1])[:3]
    sm = sum(v for _, v in w2); return [(b, v / sm) for b, v in w2]

def split_weights(P, W, specs, wpos, wrot=None):
    """THE re-weighting: P corners (bind, model space), W [[(bone, weight)]] (changed in place), wpos(bone) -> its bind
    position. Joint by joint: corners within the band go parent -> turn/swing helpers -> child (toward_child); then
    along a parent bone with roll helpers, what is still on it goes bone -> roll helpers -> the joint's swing helper
    by the corner's place along the bone."""
    groups = {}
    for sp in specs: groups.setdefault((sp['side'], sp['joint']), []).append(sp)
    # every joint's band first, then the roll passes (a roll pass takes what the bands left on the bone)
    for (s, j), sps in groups.items():
        sp0 = sps[0]; p, c, e = sp0['parent'], sp0['child'], sp0['end']
        J, E = wpos(c), wpos(e); A = wpos(p) if j != 'hip' else J + (J - E)
        rad, sv = JointProbe.radius(P, A, J, E); bp, bc = joint_sides(A, J, E, sp0['band'], j); cc = toward_child(sv, bp, bc)
        turn = [sp for sp in sps if sp['kind'] != 'roll']
        chain = [p] + [sp['helper'] for sp in turn] + [c]; at = [0.0] + [sp['level'] for sp in turn] + [1.0]
        inband = (sv > -bp) & (sv < bc)
        # only the LIMB's skin: a corner joins the split if it already moves with the child, or lies within the limb's
        # own surface (JV_LIMB_REACH x the 98th percentile radius of the child-weighted skin in the band). The band is cut
        # along the bone, so without this every corner level with the joint took part - 148 gear / chest corners on the
        # left clavicle moved onto the upper arm, and the coat's cuff lip beyond the wrist swung with the hand (owner
        # 2026-11-22: "a triangle around the thumb sticking up")
        onc = np.array([any(b == c and x > 0.05 for b, x in w) for w in W])
        limb_r = np.percentile(rad[inband & onc], 98) if (inband & onc).any() else 0.0
        sp0['limb_radius'] = float(limb_r)
        fold = sp0.get('fold'); ib = sp0.get('inside_band')
        if fold is not None and ib is not None:
            # a hinge: the skin on the INSIDE of the bend is the crease - it splits at the joint over ib units and folds
            # in, the outside blends over the whole band (round, not swollen). The fold direction is in the parent's frame.
            PR = wrot(p) if j != 'hip' else wrot(c)
            fw = PR @ fold
            seg = np.where((sv < 0)[:, None], A + np.clip(((P - A) @ (J - A)) / max((J - A) @ (J - A), 1e-9), 0, 1)[:, None] * (J - A),
                           J + np.clip(((P - J) @ (E - J)) / max((E - J) @ (E - J), 1e-9), 0, 1)[:, None] * (E - J))
            rv = P - seg; rn = np.linalg.norm(rv, axis=1); ins = (rv @ fw) / np.maximum(rn, 1e-9)
            k = np.clip(ins / JV_FOLD_FULL, 0, 1); k = k * k * (3 - 2 * k)
            bpv = bp * (1 - k) + min(ib, bp) * k; bcv = bc * (1 - k) + min(ib, bc) * k
            x = np.where(sv < 0, 0.5 + sv / (2 * bpv), 0.5 + sv / (2 * bcv)); x = np.clip(x, 0, 1); cc = x * x * (3 - 2 * x)
        # ...and never onto a bone CoD kept it off: a corner with no weight on the child (the coat's skirt beside the
        # thigh, a strap over the shoulder) blends parent -> helpers up to the joint and back to the parent past it
        # (mirrored, so its neighbours stay continuous). Without this the tunic skirt took 0.6 of the thigh and moved
        # 8.8 units in the crouched aim, a gear strap took 0.4 of the left upper arm (2026-11-22). NOT the same rule the
        # other way round (no parent weight -> no parent share): the heel of the hand is hand-only on the forearm's side
        # of the wrist and needs the forearm's share - mirrored, the MG crouch-walk support wrist went 0.56 -> 0.30.
        cc = np.where(~onc, toward_child(-np.abs(sv), bp, bc), cc)
        for v in np.where(inband & (onc | (rad <= JV_LIMB_REACH * limb_r)))[0]:
            W[v] = split_levels(W[v], chain, at, cc[v])
    for (s, j), sps in groups.items():
        roll = [sp for sp in sps if sp['kind'] == 'roll']
        if not roll: continue
        turn = [sp for sp in sps if sp['kind'] != 'roll']; p, c = roll[0]['parent'], roll[0]['child']
        # the place along the bone, measured on the bone LINE grandparent -> bone -> joint (the probe's rule): skin on the
        # grandparent's side of the bone's root is 0 - a straight projection onto the bone gave upper-arm skin a share
        # of the forearm's roll when the bind elbow is bent (the MG idle's upper arm 0.96 -> 0.60, 2026-11-22)
        A2, J = wpos(p), wpos(c); L = np.linalg.norm(J - A2)
        _, sv = JointProbe.radius(P, wpos(roll[0]['grand']), A2, J); x = np.clip(sv / max(L, 1e-9), 0, 1)
        chain = [p, p] + [sp['helper'] for sp in roll] + [turn[0]['helper']]
        at = [0.0, roll[0]['from']] + [sp['at'] for sp in roll] + [1.0]
        for v in range(len(P)):
            f = sum(x_ for b, x_ in W[v] if b == p)
            if f <= 1e-4: continue
            part = [(b, x_ * f) for b, x_ in split_levels([(p, 1.0)], chain, at, x[v])]     # only what is still on the bone
            # moves (split_levels returns shares of 1 - scaled back to f: unscaled, 0.05 of forearm on upper-arm skin
            # became half its weight, the MG idle's upper arm 0.96 -> 0.60, 2026-11-22)
            acc = {}
            for b, x_ in [(b, x_) for b, x_ in W[v] if b != p] + part: acc[b] = acc.get(b, 0.0) + x_
            w2 = sorted(acc.items(), key=lambda bv: -bv[1])[:3]; sm = sum(x_ for _, x_ in w2)
            W[v] = [(b, x_ / sm) for b, x_ in w2]
    return W

def reweight(mesh, specs, skel):
    """the measuring copy: a Mesh re-weighted by split_weights"""
    Wb = skel.world(skel.bind)
    W = [[(b, x) for b, x in zip(bb, ww) if x > 0] for bb, ww in zip(mesh.B, mesh.W)]
    W = split_weights(mesh.P, W, specs, lambda i: Wb[i][1], lambda i: Wb[i][0])
    m2 = Mesh.__new__(Mesh); m2.P = mesh.P
    m2.B = np.array([[b for b, _ in w] + [0] * (3 - len(w)) for w in W], int)
    m2.W = np.array([[x for _, x in w] + [0.0] * (3 - len(w)) for w in W])
    return m2

# ---------------------------------------------------------------- the engine's version: triggers
JV_FIT_SAMPLES = 2500   # poses the trigger fit and its error are judged on: every held pose + a seeded sample of the rest
JV_TRIGGERS = 32        # the engine's DoQuatInterpBone keeps weight[32] - never more
JV_TOLS = (30.0, 40.0, 50.0, 60.0, 75.0, 90.0)   # trigger tolerances tried (degrees); the best 95th-percentile error wins

def ang(a, b):
    return np.degrees(2 * np.arccos(np.clip(np.abs(np.sum(a * b, -1)), 0, 1)))

def fit_triggers(samples, sp, n=JV_TRIGGERS, seed=1, keep=0):
    """samples: the driver's local quaternions over every pose. Triggers = the bind first, then farthest-point picks
    refined by k-means (angle distance), each with target(sp, it); the tolerance that gives the smallest 95th
    percentile error under the ENGINE's blend (twist_bones.engine_quat_interp) is kept.
    -> (triggers [(inv_tol, q_driver, q_target, pos)], tol, (median, 95%, worst) error in degrees)"""
    import twist_bones as TW
    qb = sp['qb']
    X = np.array(samples, float); X *= np.sign(X @ qb)[:, None]
    rng = np.random.default_rng(seed)
    if len(X) > max(JV_FIT_SAMPLES, keep):      # the first `keep` samples (the held poses) always stay in
        X = np.concatenate([X[:keep], X[keep:][rng.choice(len(X) - keep, max(JV_FIT_SAMPLES - keep, 0), replace=False)]]) if keep < len(X) else X
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
    T = [target(sp, c) for c in C]
    want = np.array([target(sp, x)[0] for x in X])
    best = None
    for tol in JV_TOLS:
        tr = [(1.0 / np.radians(tol), c, q, np.array(pp, float)) for c, (q, pp) in zip(C, T)]
        got = np.array([TW.engine_quat_interp(x, tr)[0] for x in X])
        e = ang(got, want); score = (float(np.median(e)), float(np.percentile(e, 95)), float(e.max()))
        if best is None or score[1] < best[2][1]: best = (tr, tol, score)
    return best

# ---------------------------------------------------------------- into the package (cod2_package calls these)
def add_to_rig(rig, tris, poses, design=None, stance=None):
    """cod2_char's rig + mesh get the helpers of `design` (JOINT_DESIGN): nodes appended, triggers fitted to the poses,
    corners re-weighted (split_weights). -> (tris, spec [dict]) - the rig is changed in place"""
    loc = rig.local(); sk = Skeleton(rig.nodes, loc)
    keep = [(l, fr) for l, fr in poses if pose_valid(sk, fr)]
    if stance is not None: keep.sort(key=lambda lf: not stance(lf[0]))      # held poses first: the fit keeps them all
    held = sum(1 for l, _ in keep if stance is not None and stance(l)); valid = [fr for _, fr in keep]
    specs = design_helpers(rig.nodes, loc, design)
    drv = {}
    def samples_of(sp):
        c = sp['child']
        if c not in drv: drv[c] = [qrel(sk.pose(fr)[c][1]) for fr in valid]
        return drv[c]
    place_cuts(specs, samples_of, None if stance is None else np.array([stance(l) for l, _ in keep]))
    place_folds(specs, samples_of)
    rig.nodes = list(rig.nodes); rig.idx = dict(rig.idx)
    for sp in specs:
        tr, tol, err = fit_triggers(samples_of(sp), sp, keep=held)
        h = len(rig.nodes); rig.nodes.append((sp['name'], sp['parent'])); rig.idx[sp['name']] = h; sp['helper'] = h
        PR, Pt = rig.W[sp['parent']]
        rig.W[h] = (PR @ Rot.from_quat(sp['qb']).as_matrix(), PR @ sp['pos'] + Pt)
        sp.update(triggers=tr, tol=tol, error=[round(x, 2) for x in err], samples=len(valid))
    flat = [(ti, k) for ti, (m, *cs) in enumerate(tris) for k in range(3)]
    P = np.array([tris[ti][1 + k][0] for ti, k in flat])
    W = split_weights(P, [list(tris[ti][1 + k][3]) for ti, k in flat], specs, lambda i: rig.W[i][1], lambda i: rig.W[i][0])
    new = [list(t) for t in tris]
    for (ti, k), w in zip(flat, W):
        pp, nn, uv, _ = new[ti][1 + k]; new[ti][1 + k] = (pp, nn, uv, w)
    return [tuple(t) for t in new], specs


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
    loc = rig.local()
    trig = {vrd_name(h[0]): h[3] for h in TW.read_vrd(vrd)} if vrd else {}
    sps = [dict(sp, triggers=trig.get(vrd_name(sp['name']), sp['triggers'])) for sp in spec]
    return helper_skeleton(rig.nodes[:n0], {i: loc[i] for i in range(n0)}, sps, engine=True)
