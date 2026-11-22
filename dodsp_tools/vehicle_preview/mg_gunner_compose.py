"""V2b: the halftrack's MG42 gunner - his aim poses, made to hold the gun wherever it points.

    python3 mg_gunner_compose.py <ride pack dir> <CoD2 pack dir> <cod2_vehicle_tags.txt> <out dir> [pack .qc]
        writes ww2_a_halftrack_mg_<yaw>_<pitch>.smd (13 x 4 one-frame poses) and mg_gunner.txt (where he stands and how well
        each pose holds the gun); with the pack's .qc it also adds (once) $poseparameter "mg_yaw" / "mg_pitch" and the
        sequence "ww2_halftrack_mg" blending the 52 poses - the sequence the halftrack's gunner seat names - and
        "ww2_halftrack_mg_death" (13 columns, blended by mg_yaw: see DEATH below), the seat's "death".

WHY NOT CoD2's OWN GRID: CoD2's standing MG42 gunner (pb_standmg42gunner_aim_*, in the officer port as cod2_mg42_aim) is
made for a HIP-HIGH gun on the ground: its pivot is 42 units over his feet and he walks round it as it turns (the
animation turns with the gun's yaw; his hands stay put on the grips and his feet sweep +-30 units). On the halftrack the
pivot (CoD2's tag_turret, between the pintle's fork arms) is 58 units over the bay floor, and the bay is 56 wide with men
on both benches - so that grid would float him 16+ units and walk him into the riders. CoD2's halftrack gunner idle
(halftrack_mggunner_idle, made for this vehicle) stands right: hands on the grips, feet on the floor.

SO: his body is the halftrack idle (feet planted, never moves), and for each aim (yaw -45..45, pitch -15..15, CoD2's
grid) the gun turns about its pivot, his spine twists / bends a share of the way (TWIST / BEND), his head looks along the
gun, and each arm is solved (two-bone IK, the elbow kept on the side it was) so the wrist lands on the GRIP of the turned
gun with the hand turned with it. The grips and the hands' hold on them are the halftrack idle's own (made for this gun);
CoD2's ground grid is fitted only as a cross-check (printed). Where an arm can't reach, his whole body steps round the
pivot (per column, the smallest step that keeps every arm within REACH at every row).

DEATH (one per column, blended by mg_yaw like the aim): CoD2's death_stand_dropinplace (he crumples where he stands -
converted with --start-at the halftrack idle: its root re-based so his pelvis starts on the idle's). Each column's copy starts turned by that
column's step round the pivot (so it starts where his body is)
and turns back to the idle's place over DEATH_UNSTEP frames while he drops (a death that kept the step would fall
diagonally into the bay wall). Its first DEATH_SETTLE frames go from the idle's whole pose onto the death's (CoD2's frame 0 stands with other feet:
they'd slide in the blend), so it starts exactly on the idle; his arms let go of the grips in the 0.2 s blend. Checked: every bone clear of the walls and the floor in every frame of every column.

Frames: the pack's SMDs face -y; everything here is worked in the GAME frame (x forward, y left: G below) and written
back. mg_pitch > 0 = muzzle UP (the server's convention); mg_yaw > 0 = left.
"""
import os, sys, re, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.dirname(HERE)); sys.path.insert(0, HERE)
from smd import SMD
from xanim_to_smd import rotm, euler
from scipy.optimize import least_squares

VB = 'ValveBiped.Bip01_'
YAWS = tuple(range(-30, 31, 5))                     # columns (mg_yaw, right..left); 5-degree steps (the riders-aboard
                                                    # limit, +-20, is a column; the engine's blend between columns keeps
                                                    # the hands within ~0.5 of the grips - mg_preview.py measures it). The bay's walls stop him past +-30 (checked)
TOP = 15                                            # the highest row (mg_pitch, + = muzzle up); the lowest is searched
NROWS = 4                                           # rows, evenly spaced (a blend needs that)
MAX_SEQ_ANIMS = 64                                  # studiomdl's ParseSequence keeps a sequence's animations in
                                                    # animations[64] and never checks: one more writes past it
TWIST = (('Spine', 0.15), ('Spine1', 0.15), ('Spine2', 0.2), ('Spine4', 0.15))   # shares of the gun's yaw
BEND = (('Spine1', 0.25), ('Spine2', 0.25))         # shares of the gun's pitch, as a lean (muzzle up = grips down = lean in)
LOOK = (('Neck1', 0.3), ('Head1', 0.4))             # the head follows the rest of the yaw
GRID_NAME = {-45: '45right', -30: '30right', -15: '15right', 0: 'forward', 15: '15left', 30: '30left', 45: '45left'}
# CoD2's grid names its rows by the turret's pitch, Quake style (+ = muzzle down): "15up" is the muzzle 15 DOWN - the fit
# below checks this (the grips rise in "15up", so the muzzle, ahead of the pivot, dips)
GRID_ROW = {-15: '15up', 0: 'level', 15: '15down'}
PORT_SCALE = 1.0284
REACH = 0.95                                        # an arm is never stretched past this share of its length
WALL = 30.0                                         # the bay's inner walls (|y|, vehicle_rig.py OPEN_TOP 'halftrack' walls)
# each bone's thickness kept off the wall (bone points are its middle) - troop_preview.py's body radii
RADIUS = (('Head', 4.5), ('Neck', 2.5), ('Spine', 5.5), ('Pelvis', 5.5), ('Thigh', 3.5), ('Calf', 2.8), ('Foot', 2.2),
          ('Toe', 1.8), ('Clavicle', 2.2), ('UpperArm', 2.3), ('Forearm', 2.0), ('Hand', 1.8))

DEATH = 'halftrack_mg_deathdrop'                    # the pack's name of the converted death (see DEATH above)
DEATH_UNSTEP = 15                                   # frames (0.5 s) over which a column's death turns back to the idle's place
FLOOR = 30.0                                        # the bay floor (vehicle_rig.py OPEN_TOP 'halftrack' floor)
FLOOR_SLACK = 4.0                                   # a bone's thickness may reach this far into the floor (soles +-4, as
                                                    # seat_death_preview.py's sitting check)
DEATH_SETTLE = 8                                    # frames over which his whole pose goes from the idle onto the death's
def blend_R(A, B, w):
    from scipy.spatial.transform import Rotation, Slerp
    return Slerp([0, 1], Rotation.from_matrix([A, B]))(w).as_matrix()
def smooth(w): w = min(max(w, 0.0), 1.0); return w * w * (3 - 2 * w)
def col_name(y): return ('l%d' % y) if y > 0 else ('r%d' % -y) if y < 0 else 'fwd'
def radius(n): return next((r for k, r in RADIUS if k in n), 1.5)

Gm = np.array([[0, -1, 0], [1, 0, 0], [0, 0, 1]], float)     # SMD (faces -y) -> game (faces +x)
def Rz(a): a = np.radians(a); c, s = np.cos(a), np.sin(a); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
def Ry(a): a = np.radians(a); c, s = np.cos(a), np.sin(a); return np.array([[c, 0, s], [0, 1, 0], [-s, 0, c]])
def gun_rot(yaw, pitch): return Rz(yaw) @ Ry(-pitch)    # muzzle up for pitch > 0
def axis_rot(axis, deg):
    axis = axis / np.linalg.norm(axis); a = np.radians(deg); K = np.array([[0, -axis[2], axis[1]], [axis[2], 0, -axis[0]], [-axis[1], axis[0], 0]])
    return np.eye(3) + np.sin(a) * K + (1 - np.cos(a)) * K @ K

def world(s, f=0):
    """{bone index: (R, t)} game frame"""
    fr = s.frames[f][1]; W = {}
    for i, (n, p) in enumerate(s.nodes):
        if i not in fr: continue
        R = rotm(fr[i][3:]); t = np.array(fr[i][:3])
        W[i] = (R, t) if p < 0 or p not in W else (W[p][0] @ R, W[p][0] @ t + W[p][1])
    return {i: (Gm @ R, Gm @ t) for i, (R, t) in W.items()}

def to_local(s, Wg):
    """game-frame world transforms -> the SMD's local (pos, euler) per bone"""
    out = {}
    for i, (n, p) in enumerate(s.nodes):
        if i not in Wg: continue
        R, t = Gm.T @ Wg[i][0], Gm.T @ Wg[i][1]
        if p >= 0 and p in Wg:
            PR, Pt = Gm.T @ Wg[p][0], Gm.T @ Wg[p][1]; R, t = PR.T @ R, PR.T @ (t - Pt)
        out[i] = tuple(t) + tuple(euler(R))
    return out

def grip_fit(pack):
    """CoD2's grid, retargeted: the pivot (in the animation's frame, which turns with the gun's yaw) and the two
    hands' hold (position + orientation) in the gun's frame"""
    H = {}
    for y, yn in GRID_NAME.items():
        for p, pn in GRID_ROW.items():
            s = SMD(os.path.join(pack, 'ww2_a_standmg42gunner_aim_%s_%s.smd' % (yn, pn))); W = world(s)
            H[(y, p)] = [W[s.index(VB + 'R_Hand')], W[s.index(VB + 'L_Hand')]]
    # the pitch axis runs across the gun, so the pivot can slide along it without changing the fit: it is pinned on
    # the gun's centre line, midway between the hands (the grips sit on the centre line)
    py = float(np.mean([t[1] for hands in H.values() for R, t in hands]))
    def res(v):
        out = []
        for (y, p), hands in H.items():
            for k, (R, t) in enumerate(hands):
                out += list(t - (np.r_[v[0], py, v[1]] + gun_rot(0, p) @ v[2 + 3 * k: 5 + 3 * k]))
        return out
    r = least_squares(res, np.r_[30.0, 40, -30, -2, 0, -30, 2, 0])
    e = np.array(r.fun).reshape(-1, 3); rms = float(np.sqrt((e ** 2).sum(1).mean()))
    pivot, hold_t = np.r_[r.x[0], py, r.x[1]], [r.x[2:5], r.x[5:8]]
    # the hands' orientation in the gun frame: averaged over the grid (gun frame = anim frame turned by the pitch)
    hold_R = []
    for k in range(2):
        M = sum(gun_rot(0, p).T @ H[(y, p)][k][0] for (y, p) in H)
        U, _, Vt = np.linalg.svd(M); hold_R.append(U @ np.diag([1, 1, np.sign(np.linalg.det(U @ Vt))]) @ Vt)
    return pivot, hold_t, hold_R, rms

def two_bone(S, E, W, T, a, b):
    """new elbow for a shoulder S, upper arm a, forearm b reaching T; the elbow stays on the side of the S-T line it was
    (E, W = old elbow / wrist)"""
    d = T - S; L = np.linalg.norm(d); u = d / L; L = min(L, (a + b) * 0.999)
    x = (a * a - b * b + L * L) / (2 * L); h = np.sqrt(max(a * a - x * x, 0.0))
    pole = E - S; pole = pole - u * (pole @ u)
    if np.linalg.norm(pole) < 1e-6: pole = np.array([0, 0, -1.0]) - u * (-u[2])
    return S + u * x + pole / np.linalg.norm(pole) * h

def aim_bone(R, t, child_old, child_new):
    """turn a bone (R at t) so its child moves from child_old to child_new"""
    a, b = child_old - t, child_new - t
    a, b = a / np.linalg.norm(a), b / np.linalg.norm(b); v = np.cross(a, b); c = a @ b
    if np.linalg.norm(v) < 1e-9: return R
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return (np.eye(3) + K + K @ K / (1 + c)) @ R

def subtree(s, root):
    kids = {root}
    for i, (n, p) in enumerate(s.nodes):
        if p in kids or i == root: kids.add(i)
    changed = True
    while changed:
        changed = False
        for i, (n, p) in enumerate(s.nodes):
            if p in kids and i not in kids: kids.add(i); changed = True
    return kids

def rotate_subtree(s, W, root, M, about):
    for i in subtree(s, root):
        if i in W: R, t = W[i]; W[i] = (M @ R, M @ (t - about) + about)

def tag(tags_file, vehicle, name):
    block = False
    for l in open(tags_file):
        if l and not l.startswith(' '): block = l.startswith(vehicle + ' ')
        elif block and l.split()[0] == name: return np.array([float(v) for v in l.split()[3:6]])
    raise SystemExit('no %s on %s in %s' % (name, vehicle, tags_file))

def main(pack, cod2, tags_file, out, qc=None):
    os.makedirs(out, exist_ok=True)
    pivot_a, hold_t, hold_R, rms = grip_fit(pack)
    idle = SMD(os.path.join(pack, 'ww2_a_halftrack_mggunner_idle.smd')); W0 = world(idle)
    ix = lambda n: idle.index(VB + n)
    # stand him so his hands (the idle's) are as near CoD2's hold as they get with the gun level and ahead
    hands0 = [W0[ix('R_Hand')][1], W0[ix('L_Hand')][1]]
    mount = tag(tags_file, 'halftrack', 'tag_turret')                       # the gun's pivot on the vehicle (CoD2)
    stand = tag(tags_file, 'halftrack', 'tag_mgguy')                        # where CoD2 stands its gunner (feet on the floor)
    origin = stand - mount                                                  # his origin rel. the pivot
    # THE HOLD = CoD2's halftrack gunner idle (made for this gun on this vehicle): standing on tag_mgguy with the gun level
    # and ahead, his hands ARE on the grips. CoD2's ground-gun grid (fitted above) holds the gun differently (its left
    # hand further along the butt) - printed below as a cross-check only.
    grid_t = hold_t
    hold_t = [hands0[k] + origin for k in range(2)]
    hold_R = [W0[ix('R_Hand')][0], W0[ix('L_Hand')][0]]
    feet = min(W0[ix(n)][1][2] for n in ('L_Toe0', 'R_Toe0', 'L_Foot', 'R_Foot'))
    lines = ['gunner origin %.1f %.1f %.1f (vehicle frame; gun pivot %.1f %.1f %.1f = tag_turret)' % (*stand, *mount),
             'CoD2 grid hold fit: rms %.2f units; grid pivot %.1f ahead, %.1f up of its origin' % (rms, pivot_a[0], pivot_a[2]),
             'grips (from the halftrack idle) vs the ground grid\'s hold: R %.1f  L %.1f units; grips %.1f / %.1f behind the pivot' % (
                 *tuple(np.linalg.norm(hold_t[k] - grid_t[k]) for k in range(2)), -hold_t[0][0], -hold_t[1][0]),
             'lowest foot bone %.1f over his origin -> %.1f over the vehicle origin' % (feet, stand[2] + feet)]
    a_len = {}
    for side in 'RL':
        a_len[side] = (np.linalg.norm(W0[ix(side + '_Forearm')][1] - W0[ix(side + '_UpperArm')][1]),
                       np.linalg.norm(W0[ix(side + '_Hand')][1] - W0[ix(side + '_Forearm')][1]))
    pivot_his = -origin                                                     # the pivot in his frame

    def pose(y, p, o):
        """the pose for the gun at yaw y / pitch p, his body stepped o degrees round the pivot: (world transforms, the
        worst arm reach as a share of the arm)"""
        W = {i: (R.copy(), t.copy()) for i, (R, t) in W0.items()}
        if o:
            M_ = Rz(o)
            for i in W: R_, t_ = W[i]; W[i] = (M_ @ R_, M_ @ (t_ - pivot_his) + pivot_his)
        # spine: twist (about up, through each bone) and lean (about the gunner's left axis), the yaw not stepped
        for (bn, share) in TWIST:
            i = ix(bn); rotate_subtree(idle, W, i, Rz((y - o) * share), W[i][1].copy())
        for (bn, share) in BEND:
            i = ix(bn); rotate_subtree(idle, W, i, axis_rot(Rz(y) @ np.array([0, 1.0, 0]), p * share), W[i][1].copy())
        for (bn, share) in LOOK:
            i = ix(bn); rotate_subtree(idle, W, i, Rz((y - o) * share), W[i][1].copy())
        G = gun_rot(y, p); rr = 0.0
        for k, side in enumerate('RL'):
            Tw = G @ hold_t[k] - origin                        # the grip, in his frame
            u, f, h = ix(side + '_UpperArm'), ix(side + '_Forearm'), ix(side + '_Hand')
            S, E, Wr = W[u][1], W[f][1], W[h][1]
            a, b = a_len[side]
            rr = max(rr, np.linalg.norm(Tw - S) / (a + b))
            En = two_bone(S, E, Wr, Tw, a, b)
            W[u] = (aim_bone(W[u][0], S, E, En), S)
            for i in subtree(idle, f):                         # the forearm and below follow the new elbow
                R_, t_ = W[i]; W[i] = (R_, t_ - E + En)
            W[f] = (aim_bone(W[f][0], En, W[h][1], Tw), En)
            # the hand: at the grip, turned with the gun (CoD2's hold), fingers with it
            M = (G @ hold_R[k]) @ W[h][0].T; old = W[h][1]
            for i in subtree(idle, h):
                R_, t_ = W[i]; W[i] = (M @ R_, M @ (t_ - old) + Tw)
        return W, rr

    def plan(p_min):
        """rows p_min .. TOP (evenly spaced, as a blend needs) and per column the smallest step round the pivot that keeps
        every arm within REACH at every row; None if some column can't"""
        rows = tuple(float(v) for v in np.linspace(p_min, TOP, NROWS)); steps = {}
        for y in YAWS:
            o = 0
            while max(pose(y, p, o)[1] for p in rows) > REACH:
                if abs(o) >= abs(y): return None
                o += 1 if y > 0 else -1
            steps[y] = o
        return rows, steps

    # the steepest muzzle-down angle he can hold (a muzzle down lifts the grips, 37 units behind the pivot)
    for p_min in range(-15, 1):
        got = plan(p_min)
        if got: break
    else:
        raise SystemExit('\n'.join(lines) + '\nno muzzle-down angle keeps his arms within reach')
    rows, steps = got
    if len(YAWS) * len(rows) > MAX_SEQ_ANIMS: raise SystemExit('%d x %d poses: studiomdl takes at most %d animations in one sequence' % (len(YAWS), len(rows), MAX_SEQ_ANIMS))
    worst = 0.0; poses = {}; reach_worst = 0.0; wall_worst = 0.0
    for p in rows:
        for y in YAWS:
            W, rr = pose(y, p, steps[y]); reach_worst = max(reach_worst, rr)
            # inside the bay: every bone of his (body and limbs, not the hands on the gun) clear of the side walls
            for i, (n, par) in enumerate(idle.nodes):
                if i in W and n.startswith(VB) and 'Finger' not in n and 'Hand' not in n:
                    wall_worst = max(wall_worst, abs(W[i][1][1] + stand[1]) + next((r for k, r in RADIUS if k in n), 1.5))
            s = SMD(); s.nodes = list(idle.nodes); s.frames = [(0, to_local(idle, W))]
            name = 'halftrack_mg_%s_%d' % (col_name(y), rows.index(p))
            s.write(os.path.join(out, 'ww2_a_%s.smd' % name)); poses[(y, p)] = name
            Wb = world(SMD(os.path.join(out, 'ww2_a_%s.smd' % name)))      # read back: the written pose holds the grips
            for k, side in enumerate('RL'):
                worst = max(worst, np.linalg.norm(Wb[ix(side + '_Hand')][1] - (gun_rot(y, p) @ hold_t[k] - origin)))
    lines.append('rows (mg_pitch, + = muzzle up): %s - the steepest muzzle-down his arms reach is %d' % (', '.join('%g' % r for r in rows), rows[0]))
    lines.append('hands on the grips, worst over the %d poses' % (len(YAWS) * len(rows)) + ' (read back): %.3f units; furthest reach %.2f of the arm (limit %.2f)' % (worst, reach_worst, REACH))
    lines.append('steps round the pivot per column (deg): ' + ', '.join('%+d: %+d' % (y, steps[y]) for y in YAWS))
    lines.append('nearest the side walls: %.1f units out of %.1f (bone + its thickness)' % (wall_worst, WALL))
    if wall_worst > WALL: raise SystemExit('\n'.join(lines) + '\nHE STEPS INTO THE BAY WALL - narrow YAWS')
    if worst > 0.01 or reach_worst > REACH + 1e-6: raise SystemExit('\n'.join(lines) + '\nA HAND IS OFF ITS GRIP OR AN ARM OVERSTRETCHED')
    # the deaths, one per column (see DEATH)
    ds = SMD(os.path.join(pack, 'ww2_a_%s.smd' % DEATH)); deaths = {}
    if ds.nodes != idle.nodes: raise SystemExit('%s has another skeleton than the idle' % DEATH)
    D0 = world(ds, 0); pel = ix('Pelvis')
    limbs = max(np.linalg.norm(D0[i][1] - W0[i][1]) for i in W0 if i in D0 and 'Finger' not in idle.nodes[i][0])
    d_wall = d_floor = 0.0; d_x = [1e9, -1e9]
    for y in YAWS:
        frames = []
        for f in range(len(ds.frames)):
            W = world(ds, f)
            if f < DEATH_SETTLE:                              # his legs and body come off the idle's (no foot slide)
                w = smooth(f / float(DEATH_SETTLE)); W = {i: (blend_R(W0[i][0], W[i][0], w), W0[i][1] + (W[i][1] - W0[i][1]) * w) for i in W}
            M_ = Rz(steps[y] * (1 - smooth(f / float(DEATH_UNSTEP))))
            W = {i: (M_ @ R_, M_ @ (t_ - pivot_his) + pivot_his) for i, (R_, t_) in W.items()}
            for i, (n, par) in enumerate(idle.nodes):
                if i in W and n.startswith(VB) and 'Finger' not in n:
                    q = W[i][1] + stand; r = radius(n)
                    d_wall = max(d_wall, abs(q[1]) + r); d_floor = max(d_floor, FLOOR - (q[2] - r))
                    d_x = [min(d_x[0], q[0] - r), max(d_x[1], q[0] + r)]
            if f == 0: j0 = max(np.linalg.norm(W[i][1] - M_ @ (W0[i][1] - pivot_his) - pivot_his) for i in W0 if i in W)
            frames.append((f, to_local(idle, W)))
        s = SMD(); s.nodes = list(idle.nodes); s.frames = frames
        name = 'halftrack_mg_death_%s' % col_name(y); s.write(os.path.join(out, 'ww2_a_%s.smd' % name)); deaths[y] = name
    lines.append('death (%s, %d frames, one per column): first frame vs the idle (stepped) %.3f units (CoD2\'s own frame 0 has limbs up to %.0f off); nearest the walls %.1f of %.1f; '
                 'into the floor %.1f (slack %.1f); spans x %.0f..%.0f' % (DEATH, len(ds.frames), j0, limbs, d_wall, WALL,
                 max(d_floor, 0), FLOOR_SLACK, *d_x))
    if j0 > 0.01: raise SystemExit('\n'.join(lines) + '\nTHE DEATH DOES NOT START ON THE IDLE')
    if d_wall > WALL or d_floor > FLOOR_SLACK: raise SystemExit('\n'.join(lines) + '\nTHE DEATH GOES INTO THE BAY')
    open(os.path.join(out, 'mg_gunner.txt'), 'w').write('\n'.join(lines) + '\n'); print('\n'.join(lines))
    if qc:
        text = open(qc).read()
        if '"ww2_halftrack_mg"' not in text:
            # like every animation in the pack: the hands are never pulled onto a weapon (ikrule release, whole length)
            ik = lambda n: '\tikrule "rhand" release range 0 0 %d %d\n\tikrule "lhand" release range 0 0 %d %d\n' % ((n - 1,) * 4)
            add = '\n$poseparameter "mg_yaw" %d %d\n$poseparameter "mg_pitch" %g %g\n' % (YAWS[0], YAWS[-1], rows[0], rows[-1])
            for (y, p), n in poses.items():
                add += '$animation "a_%s" "ww2_a_%s.smd" {\n\tfps 30\n\tloop\n%s}\n' % (n, n, ik(1))
            add += '$sequence "ww2_halftrack_mg" {\n' + ''.join('\t"a_%s"\n' % poses[(y, p)] for p in rows for y in YAWS)
            add += '\tblend "mg_yaw" %d %d\n\tblend "mg_pitch" %g %g\n\tblendwidth %d\n\tloop\n\tfadein 0.2\n\tfadeout 0.2\n}\n' % (
                YAWS[0], YAWS[-1], rows[0], rows[-1], len(YAWS))
            for y in YAWS:
                add += '$animation "a_%s" "ww2_a_%s.smd" {\n\tfps 30\n%s}\n' % (deaths[y], deaths[y], ik(len(ds.frames)))
            add += '$sequence "ww2_halftrack_mg_death" {\n' + ''.join('\t"a_%s"\n' % deaths[y] for y in YAWS)
            add += '\tblend "mg_yaw" %d %d\n\tblendwidth %d\n\tfadein 0.2\n\tfadeout 0.2\n}\n' % (YAWS[0], YAWS[-1], len(YAWS))
            open(qc, 'a').write(add); print('added ww2_halftrack_mg to', qc)
    return dict(stand=stand, mount=mount, origin=origin, rows=rows, steps=steps, hold_t=hold_t, poses=poses, deaths=deaths)

if __name__ == '__main__':
    main(*sys.argv[1:6])
