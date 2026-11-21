"""V2b: the halftrack's MG42 gunner - his aim poses: standing behind the gun, the butt in his shoulder, his cheek on the
stock, wherever it points (owner 2026-11-21, with period photos: "they stand a good bit above the gun ... the butt of the
gun should be on his shoulder" - the first version held it out in his hands like a man afraid of it).

    python3 mg_gunner_compose.py <ride pack dir> <CoD2 pack dir> <cod2_vehicle_tags.txt> <DoD decompile dir> <out dir> [pack .qc]
        writes ww2_a_halftrack_mg_<yaw>_<pitch>.smd (13 x 4 one-frame poses) and mg_gunner.txt (where he stands and how well
        each pose holds the gun); with the pack's .qc it also adds (once) $poseparameter "mg_yaw" / "mg_pitch" and the
        sequence "ww2_halftrack_mg" blending the 52 poses - the sequence the halftrack's gunner seat names - and
        "ww2_halftrack_mg_death" (13 columns, blended by mg_yaw: see DEATH below), the seat's "death".

THE HOLD: DoD's own standing deployed MG42 (dod_player_shared aimmatrix_deployed_idle_MG_neutral - Valve's animators'
shouldered MG42, on the very skeleton the German soldier uses): upright, legs straight, the right hand on the grip, the
left under the stock, the head down on it. The gun is mg42_model.py's (gun_facts: one owner of its size and points), at
its REAL size - CoD2's is 1.21x, and its butt then sat out of any standing man's reach.

FOR EACH AIM (mg_yaw columns -30..30, mg_pitch rows): the gun turns about its pivot (CoD2's tag_turret) and
  * his whole body turns with it about the pivot (the butt stays in his shoulder, so he walks round the mount: his feet
    are planted per column),
  * his torso leans LEAN of the pitch (about Spine1), his head the rest (he keeps looking down the sights),
  * the body is moved so his shoulder POCKET (a point fixed to his chest, just in front of and inside the right shoulder
    joint) sits on CoD2's tag_butt; his legs (two-bone IK, feet planted) bend for it - a muzzle up drops the butt and he
    sinks; a muzzle down lifts it, he straightens and rises on his toes (HEEL_MAX), and what's left is the butt's GAP
    off his shoulder: the lowest row is the steepest muzzle-down that keeps the gap within GAP_MAX,
  * each hand is put back on the gun where DoD's hold has it (the right palm on CoD2's tag_weapon, the left where DoD's
    left hand sits from the right), turned with the gun; the arms solved by two-bone IK.
Checked: hands on the gun in every pose (read back), arm reach, everything below the bay walls' top inside them.

DEATH (one per column, blended by mg_yaw like the aim): CoD2's death_stand_dropinplace (he crumples where he stands),
moved so its first frame's pelvis stands on his; each column's copy starts turned with the column (where his body is)
and turns back over DEATH_UNSTEP frames while he drops (a death that kept the turn would fall diagonally into the bay
wall). Its first DEATH_SETTLE frames go from his aim pose onto the death's (no foot slide); his arms let go of the gun in
the 0.2 s blend. Checked: every bone clear of the walls and the floor in every frame of every column.

Frames: the pack's SMDs face -y; everything here is worked in the GAME frame (x forward, y left: Gm below) and written
back. mg_pitch > 0 = muzzle UP (the server's convention); mg_yaw > 0 = left.
"""
import os, sys, re, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.dirname(HERE)); sys.path.insert(0, HERE)
from smd import SMD
from xanim_to_smd import rotm, euler
from mg42_model import gun_facts

VB = 'ValveBiped.Bip01_'
YAWS = tuple(range(-30, 31, 5))                     # columns (mg_yaw, right..left); 5-degree steps (the riders-aboard
                                                    # limit, +-20, is a column; the engine's blend between columns keeps
                                                    # the hands near the grips - mg_preview.py measures it)
TOP = 15                                            # the highest row (mg_pitch, + = muzzle up); the lowest is searched
NROWS = 4                                           # rows, evenly spaced (a blend needs that)
MAX_SEQ_ANIMS = 64                                  # studiomdl's ParseSequence keeps a sequence's animations in
                                                    # animations[64] and never checks: one more writes past it
DOD_HOLD = 'dod_player_shared_anim_a_aimmatrix_deployed_idle_MG_neutral.smd'
LEAN = 0.5                                          # share of the gun's pitch his torso takes; his head takes the rest
POCKET = (2.5, 2.0, 0.5)                            # where the butt sits on him: from his right shoulder joint, forward /
                                                    # in toward his chest / up (his chest's frame at the hold)
HEEL_MAX = 3.0                                      # he rises on his toes at most this far
SHRUG_MAX = 2.5                                     # and lifts his right shoulder (the clavicle) at most this far
ROLL_MAX = 35.0                                     # he leans his upper body out over the wall at most this far
HIP_SHARE = 0.5                                     # share of the gun's yaw his hips and feet take round the pivot; his spine
                                                    # twists the rest (Spine, Spine1, Spine2) - a full turn walks his hips
                                                    # into the bay wall at +-30
GAP_MAX = 2.0                                       # the butt may leave his shoulder this far (the lowest row's limit)
LEG_REACH = 0.995                                   # a leg is never stretched past this share of its length
REACH = 0.97                                        # nor an arm
WALL = 30.0                                         # the bay's inner walls (|y|, vehicle_rig.py OPEN_TOP 'halftrack' walls)
WALL_TOP = 77.0                                     # their top (measured on the mesh: 77 along the bay) - a bone whose
                                                    # middle is above it may lean out over the wall
# each bone's thickness kept off the wall (bone points are its middle) - troop_preview.py's body radii
RADIUS = (('Head', 4.5), ('Neck', 2.5), ('Spine', 5.5), ('Pelvis', 5.5), ('Thigh', 3.5), ('Calf', 2.8), ('Foot', 2.2),
          ('Toe', 1.8), ('Clavicle', 2.2), ('UpperArm', 2.3), ('Forearm', 2.0), ('Hand', 1.8))
DEATH = 'halftrack_mg_deathdrop'                    # the pack's name of the converted death (see DEATH above)
DEATH_UNSTEP = 15                                   # frames (0.5 s) over which a column's death turns back to the middle
FLOOR = 30.0                                        # the bay floor (vehicle_rig.py OPEN_TOP 'halftrack' floor)
FLOOR_SLACK = 4.0                                   # a bone's thickness may reach this far into the floor (soles +-4, as
                                                    # seat_death_preview.py's sitting check)
DEATH_LIFT = 1.0                                    # CoD2's crumple raised this much (it ends a little in the floor)
DEATH_SETTLE = 8                                    # frames over which his whole pose goes from the aim onto the death's
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

def two_bone(S, E, W, T, a, b, hint=None):
    """new elbow (knee) for a shoulder (hip) S, upper arm a, forearm b reaching T; the elbow stays on the side of the S-T
    line it was (E, W = old elbow / wrist) - or bends toward hint where the limb was straight"""
    d = T - S; L = np.linalg.norm(d); u = d / L; L = min(L, (a + b) * 0.999)
    x = (a * a - b * b + L * L) / (2 * L); h = np.sqrt(max(a * a - x * x, 0.0))
    pole = E - S; pole = pole - u * (pole @ u)
    if hint is not None and np.linalg.norm(pole) < 0.5: pole = hint - u * (hint @ u)
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

def hold_pose(pack, dod):
    """DoD's shouldered MG42 on the pack's skeleton: its bones by name, the pack's own extra bones (gear, twist) as the
    halftrack idle has them"""
    idle = SMD(os.path.join(pack, 'ww2_a_halftrack_mggunner_idle.smd'))
    d = SMD(os.path.join(dod, DOD_HOLD)); dn = {n.lower(): i for i, (n, p) in enumerate(d.nodes)}
    fr = dict(idle.frames[0][1])
    for i, (n, p) in enumerate(idle.nodes):
        if n.lower() in dn: fr[i] = d.frames[0][1][dn[n.lower()]]
    s = SMD(); s.nodes = list(idle.nodes); s.frames = [(0, fr)]
    return s

def main(pack, cod2, tags_file, dod, out, qc=None):
    os.makedirs(out, exist_ok=True)
    F = gun_facts(cod2)
    base = hold_pose(pack, dod); WB = world(base)
    ix = lambda n: base.index(VB + n)
    mount = tag(tags_file, 'halftrack', 'tag_turret')                       # the gun's pivot on the vehicle (CoD2)
    floor = tag(tags_file, 'halftrack', 'tag_mgguy')[2]                     # CoD2's gunner stands here (his feet)
    # the pocket, fixed to his right collarbone (a shrug lifts it): DoD's hold faces +x, so forward = +x, in toward his
    # chest = +y (right shoulder)
    s4 = ix('R_Clavicle'); sh = WB[ix('R_UpperArm')][1]
    pocket_l = WB[s4][0].T @ (sh + np.array(POCKET) - WB[s4][1])
    pocket = lambda W: W[s4][0] @ pocket_l + W[s4][1]
    # place the hold at the gun level and ahead: the pocket on the butt, his soles on the floor (z: the legs say below)
    butt0 = mount + F['butt']
    shift = butt0 - pocket(WB)
    origin = np.array([shift[0], shift[1], floor])                          # his origin (feet) in the vehicle frame
    # everything below in HIS frame (origin at his feet, facing +x); the pivot there:
    piv = mount - origin
    W0 = {i: (R, t + np.array([0, 0, shift[2] - floor])) for i, (R, t) in WB.items()}   # pocket on the butt, feet as they fall
    # the hands' hold on the gun (gun frame, level and ahead): the right palm on CoD2's tag_weapon, DoD's hand turn; the
    # left where DoD has it from the right
    rh, lh = ix('R_Hand'), ix('L_Hand')
    palm_l = 0.6 * (WB[rh][0].T @ (WB[ix('R_Finger2')][1] - WB[rh][1]))
    hold_R = [WB[rh][0], WB[lh][0]]
    r_hand = F['grip'] - hold_R[0] @ palm_l                                 # the right hand bone, rel. the pivot
    hold_t = [r_hand, r_hand + (WB[lh][1] - WB[rh][1])]
    legs = {sd: (np.linalg.norm(WB[ix(sd + '_Calf')][1] - WB[ix(sd + '_Thigh')][1]),
                 np.linalg.norm(WB[ix(sd + '_Foot')][1] - WB[ix(sd + '_Calf')][1])) for sd in 'RL'}
    arms = {sd: (np.linalg.norm(WB[ix(sd + '_Forearm')][1] - WB[ix(sd + '_UpperArm')][1]),
                 np.linalg.norm(WB[ix(sd + '_Hand')][1] - WB[ix(sd + '_Forearm')][1])) for sd in 'RL'}
    lines = ['gunner origin %.1f %.1f %.1f (vehicle frame: the crew seat\'s "origin"; gun pivot %.1f %.1f %.1f = tag_turret)' % (*origin, *mount),
             'gun at its real size: x %.3f of CoD2\'s; butt %.1f behind the pivot, %.1f under it; grip %.1f behind' % (
                 F['scale'], -F['butt'][0], -F['butt'][2], -F['grip'][0]),
             'his feet planted on the floor; at the gun level the hold stands him %.1f units %s DoD\'s (legs straight in DoD\'s)' % (
                 abs(shift[2] - floor), 'above' if shift[2] > floor else 'below')]

    def turn(W, root, M, about):
        rotate_subtree(base, W, root, M, about)

    spine = subtree(base, ix('Spine'))
    def twisted(y, roll):
        """the level pose turned about the pivot, his hips and legs turned back by (1 - HIP_SHARE) of it about his spine
        (the spine twists the difference, a third at each of three joints), his upper body rolled `roll` degrees toward
        the middle of the bay (about his forward axis at the spine's root), then moved across so the pocket is over the butt"""
        M = Rz(y)
        W = {i: (M @ R, M @ (t - piv) + piv) for i, (R, t) in W0.items()}
        back = (1 - HIP_SHARE) * y; c = W[ix('Spine')][1].copy()
        Mb = Rz(-back)
        for i in W:
            if i not in spine: R_, t_ = W[i]; W[i] = (Mb @ R_, Mb @ (t_ - c) + c)
        turn(W, ix('Spine'), Rz(-back), c)
        for bn in ('Spine', 'Spine1', 'Spine2'):
            j = ix(bn); turn(W, j, Rz(back / 3.0), W[j][1].copy())
        if roll:
            turn(W, ix('Spine'), axis_rot(Rz(y) @ np.array([1.0, 0, 0]), roll), c)
        d = piv + Rz(y) @ F['butt'] - pocket(W); d[2] = 0.0      # across only: his feet stay on the floor (pose() sets
        return {i: (R_, t_ + d) for i, (R_, t_) in W.items()}       # his height with the legs)

    def wall_out(W, names):
        return max(abs(W[i][1][1] + origin[1]) + radius(base.nodes[i][0]) for i in names) - WALL

    rolls = {}
    hips = [ix(n) for n in ('Pelvis', 'L_Thigh', 'R_Thigh', 'Spine', 'Spine1')]   # what's below the walls' top (knees: the leg IK bends them in; feet: moved in)
    def column(y):
        """his stance for the column (feet planted from here): twisted(), with the least roll that keeps his hips inside
        the walls (a man at the gun's side leans out over the wall, his hips stay in), and each foot moved in from a
        wall it would stand in"""
        for roll in np.arange(0, ROLL_MAX + 0.01, 0.5):
            for sg in (1, -1):
                W = twisted(y, sg * roll)
                if wall_out(W, hips) <= -1.0: break
            else: continue
            break
        else:
            W = twisted(y, 0)
            raise SystemExit('column %+d: no roll up to %g degrees keeps his hips inside the bay walls (no roll: %s)' % (y, ROLL_MAX,
                ', '.join('%s %.1f' % (base.nodes[i][0][17:], wall_out(W, [i])) for i in hips)))
        for sd in 'RL':
            fo = subtree(base, ix(sd + '_Foot'))
            out = wall_out(W, fo)
            if out > -1.5:                                  # margin: on his toes the foot turns a little
                sg = 1.0 if W[ix(sd + '_Foot')][1][1] + origin[1] < 0 else -1.0
                for i in fo: R_, t_ = W[i]; W[i] = (R_, t_ + np.array([0, sg * (out + 1.5), 0]))
        rolls[y] = sg * roll
        return W

    def on_toes(fe, y, h):
        """the planted feet with the heels up h: each foot turned about its toe joint (across the foot)"""
        out = dict(fe)
        for sd in 'RL':
            fo, to = ix(sd + '_Foot'), ix(sd + '_Toe0')
            T0, A = fe[to][1], fe[fo][1]
            axis = np.cross(np.array([0, 0, 1.0]), T0 - A); axis /= np.linalg.norm(axis)
            for deg in np.arange(0, 60, 0.25):
                M = axis_rot(axis, deg)
                if (M @ (A - T0))[2] - (A - T0)[2] >= h or deg == 59.75: break
                M2 = axis_rot(axis, -deg)
                if (M2 @ (A - T0))[2] - (A - T0)[2] >= h: M = M2; break
            for i in subtree(base, fo): out[i] = (M @ fe[i][0], M @ (fe[i][1] - T0) + T0)
        return out

    def pose(y, p, feet):
        """the pose for the gun at yaw y / pitch p, feet planted as in feet (the column's stance): (world transforms,
        worst arm reach, worst leg reach, butt gap, heel lift)"""
        W = {i: (R.copy(), t.copy()) for i, (R, t) in stances[y].items()}
        base_feet = feet
        left = Rz(y) @ np.array([0, 1.0, 0])
        turn(W, ix('Spine1'), axis_rot(left, -LEAN * p), W[ix('Spine1')][1].copy())    # same way as the gun: muzzle up, chest back
        turn(W, ix('Neck1'), axis_rot(left, -(1 - LEAN) * p), W[ix('Neck1')][1].copy())
        G = gun_rot(y, p)
        want = piv + G @ F['butt']
        d = want - pocket(W)
        # his legs: how high can his hips go with the feet planted (+ his toes)?
        def leg_need(dz):
            worst = 0.0
            for sd in 'RL':
                hip = W[ix(sd + '_Thigh')][1] + np.array([d[0], d[1], dz]); a, b = legs[sd]
                worst = max(worst, np.linalg.norm(feet[ix(sd + '_Foot')][1] - hip) / (a + b))
            return worst
        dz, heel = d[2], 0.0
        while leg_need(dz) > LEG_REACH and heel < HEEL_MAX:
            heel = min(HEEL_MAX, heel + 0.25); feet = on_toes(base_feet, y, heel)
        while leg_need(dz) > LEG_REACH:
            dz -= 0.05
        for i in W:
            R_, t_ = W[i]; W[i] = (R_, t_ + np.array([d[0], d[1], dz]))
        # what the legs couldn't give, the shoulder shrugs (the collarbone turned up about its root)
        gap = want[2] - pocket(W)[2]
        if gap > 0.01:
            cl = ix('R_Clavicle'); c0 = W[cl][1].copy(); out = Rz(y) @ np.array([0, -1.0, 0])
            ax = np.cross(out, [0, 0, 1.0])                        # turning about this lifts the outer end (checked)
            for deg in np.arange(0.25, 40, 0.25):
                Wt = dict(W); rotate_subtree(base, Wt, cl, axis_rot(ax, deg), c0)
                if pocket(Wt)[2] - pocket(W)[2] >= min(gap, SHRUG_MAX): break
            if pocket(Wt)[2] < pocket(W)[2]: raise SystemExit('the shrug turns his shoulder down')
            W = Wt
        gap = max(0.0, want[2] - pocket(W)[2])
        # legs: hips moved with the body, feet (and toes) planted, knees bent forward
        fwd = Rz(y) @ np.array([1.0, 0, 0]); lr = 0.0
        for sd in 'RL':
            th, ca, fo = ix(sd + '_Thigh'), ix(sd + '_Calf'), ix(sd + '_Foot')
            S_, E, Fo, T = W[th][1].copy(), W[ca][1].copy(), W[fo][1].copy(), feet[fo][1]
            a, b = legs[sd]; lr = max(lr, np.linalg.norm(T - S_) / (a + b))
            inward = np.array([0, -np.sign(E[1] + origin[1]), 0])   # a knee near a wall bends in from it, not into it
            sideways = min(1.0, max(0.0, (abs(E[1] + origin[1]) - 15.0) / 10.0))
            En = two_bone(S_, E, Fo, T, a, b, hint=fwd + sideways * inward)
            if sideways > 0:                                       # re-aim the knee's bend at the hint (the old bend side
                u = (T - S_) / np.linalg.norm(T - S_)              # would win in two_bone otherwise)
                h_ = fwd + sideways * inward; h_ = h_ - u * (h_ @ u); h_ /= np.linalg.norm(h_)
                L_ = min(np.linalg.norm(T - S_), (a + b) * 0.999); x_ = (a * a - b * b + L_ * L_) / (2 * L_)
                En = S_ + u * x_ + h_ * np.sqrt(max(a * a - x_ * x_, 0.0))
            turn(W, th, aim_bone(np.eye(3), S_, E, En), S_)            # the thigh (and all below) so the knee is at En
            turn(W, ca, aim_bone(np.eye(3), En, W[fo][1], T), En)     # the calf so the ankle is on the planted foot
            for i in subtree(base, fo): W[i] = (feet[i][0].copy(), feet[i][1].copy())
        # arms: each hand on the gun, turned with it
        rr = 0.0
        for k, side in enumerate('RL'):
            Tw = G @ hold_t[k] + piv
            u, f, h = ix(side + '_UpperArm'), ix(side + '_Forearm'), ix(side + '_Hand')
            S_, E, Wr = W[u][1], W[f][1], W[h][1]
            a, b = arms[side]
            rr = max(rr, np.linalg.norm(Tw - S_) / (a + b))
            En = two_bone(S_, E, Wr, Tw, a, b)
            W[u] = (aim_bone(W[u][0], S_, E, En), S_)
            for i in subtree(base, f):
                R_, t_ = W[i]; W[i] = (R_, t_ - E + En)
            W[f] = (aim_bone(W[f][0], En, W[h][1], Tw), En)
            M = (G @ hold_R[k]) @ W[h][0].T; old = W[h][1]
            for i in subtree(base, h):
                R_, t_ = W[i]; W[i] = (M @ R_, M @ (t_ - old) + Tw)
        return W, rr, lr, gap, heel

    stances = {y: column(y) for y in YAWS}
    feet_of = lambda y: {i: stances[y][i] for sd in 'RL' for i in subtree(base, ix(sd + '_Foot'))}
    # the steepest muzzle-down that keeps the butt in his shoulder (within GAP_MAX)
    # (rows on whole degrees: TOP - p_min a multiple of NROWS - 1)
    for p_min in range(-15, 1):
        if (TOP - p_min) % (NROWS - 1) == 0 and all(pose(y, p_min, feet_of(y))[3] <= GAP_MAX for y in YAWS): break
    rows = tuple(int(round(v)) for v in np.linspace(p_min, TOP, NROWS))
    if len(YAWS) * len(rows) > MAX_SEQ_ANIMS: raise SystemExit('%d x %d poses: studiomdl takes at most %d animations in one sequence' % (len(YAWS), len(rows), MAX_SEQ_ANIMS))
    worst = reach_worst = leg_worst = gap_worst = heel_worst = wall_worst = 0.0; poses = {}
    for p in rows:
        for y in YAWS:
            W, rr, lr, gap, heel = pose(y, p, feet_of(y))
            reach_worst, leg_worst, gap_worst, heel_worst = max(reach_worst, rr), max(leg_worst, lr), max(gap_worst, gap), max(heel_worst, heel)
            for i, (n, par) in enumerate(base.nodes):
                if i in W and n.startswith(VB) and 'Finger' not in n and W[i][1][2] + origin[2] < WALL_TOP:   # its middle below the top (leaning on the wall's edge is fine)
                    wall_worst = max(wall_worst, abs(W[i][1][1] + origin[1]) + radius(n))
            s = SMD(); s.nodes = list(base.nodes); s.frames = [(0, to_local(base, W))]
            name = 'halftrack_mg_%s_%d' % (col_name(y), rows.index(p))
            s.write(os.path.join(out, 'ww2_a_%s.smd' % name)); poses[(y, p)] = name
            Wb = world(SMD(os.path.join(out, 'ww2_a_%s.smd' % name)))      # read back: the written pose holds the gun
            for k, side in enumerate('RL'):
                worst = max(worst, np.linalg.norm(Wb[ix(side + '_Hand')][1] - (gun_rot(y, p) @ hold_t[k] + piv)))
    lines.append('rows (mg_pitch, + = muzzle up): %s - the steepest muzzle-down with the butt in his shoulder (gap <= %.1f) is %d' % (
        ', '.join('%g' % r for r in rows), GAP_MAX, rows[0]))
    lines.append('hands on the gun, worst over the %d poses (read back): %.3f units; arm reach %.2f (limit %.2f), legs %.3f (limit %.3f)' % (
        len(YAWS) * len(rows), worst, reach_worst, REACH, leg_worst, LEG_REACH))
    lines.append('butt off his shoulder at most %.1f units; on his toes at most %.1f' % (gap_worst, heel_worst))
    lines.append('leans out over the wall (deg) per column: ' + ', '.join('%+d: %.1f' % (y, abs(rolls[y])) for y in YAWS))
    lines.append('nearest the side walls (below their top, %.0f): %.1f units out of %.1f (bone + its thickness)' % (WALL_TOP, wall_worst, WALL))
    if wall_worst > WALL: raise SystemExit('\n'.join(lines) + '\nHE STEPS INTO THE BAY WALL - narrow YAWS')
    if worst > 0.01 or reach_worst > REACH + 1e-6: raise SystemExit('\n'.join(lines) + '\nA HAND IS OFF THE GUN OR AN ARM OVERSTRETCHED')
    # the deaths, one per column: CoD2's crumple, its first frame's pelvis moved under his (the level aim's) pelvis
    idle = base; pivot_his = piv
    level = min(rows, key=abs)
    ds = SMD(os.path.join(pack, 'ww2_a_%s.smd' % DEATH)); deaths = {}
    if [n for n, p in ds.nodes] != [n for n, p in base.nodes]: raise SystemExit('%s has another skeleton than the pack' % DEATH)
    pel = ix('Pelvis'); D0 = world(ds, 0)
    limbs = j0 = d_wall = d_floor = 0.0; d_x = [1e9, -1e9]
    for y in YAWS:
        A = pose(y, level, feet_of(y))[0]
        dmove = Rz(-y) @ (A[pel][1] - piv) + piv - D0[pel][1]; dmove[2] = DEATH_LIFT
        limbs = max(limbs, max(np.linalg.norm(Rz(y) @ (D0[i][1] + dmove - piv) + piv - A[i][1]) for i in A if i in D0 and 'Finger' not in base.nodes[i][0]))
        frames = []
        for f in range(len(ds.frames)):
            M_ = Rz(y * (1 - smooth(f / float(DEATH_UNSTEP))))
            W = {i: (M_ @ R_, M_ @ (t_ + dmove - pivot_his) + pivot_his) for i, (R_, t_) in world(ds, f).items()}
            if f < DEATH_SETTLE:                              # his legs and body come off the aim's (no foot slide)
                w = smooth(f / float(DEATH_SETTLE)); W = {i: (blend_R(A[i][0], W[i][0], w), A[i][1] + (W[i][1] - A[i][1]) * w) for i in W}
            for i, (n, par) in enumerate(idle.nodes):
                if i in W and n.startswith(VB) and 'Finger' not in n:
                    q = W[i][1] + origin; r = radius(n)
                    d_floor = max(d_floor, FLOOR - (q[2] - r))
                    if q[2] < WALL_TOP: d_wall = max(d_wall, abs(q[1]) + r)
                    d_x = [min(d_x[0], q[0] - r), max(d_x[1], q[0] + r)]
            if f == 0: j0 = max(j0, max(np.linalg.norm(W[i][1] - A[i][1]) for i in A if i in W))
            frames.append((f, to_local(idle, W)))
        s = SMD(); s.nodes = list(idle.nodes); s.frames = frames
        name = 'halftrack_mg_death_%s' % col_name(y); s.write(os.path.join(out, 'ww2_a_%s.smd' % name)); deaths[y] = name
    lines.append('death (%s, %d frames, one per column): first frame vs his level aim (mg_pitch %g) %.3f units (CoD2\'s own frame 0 has limbs up to %.0f off); '
                 'nearest the walls %.1f of %.1f; into the floor %.1f (slack %.1f); spans x %.0f..%.0f' % (DEATH, len(ds.frames), level, j0, limbs,
                 d_wall, WALL, max(d_floor, 0), FLOOR_SLACK, *d_x))
    if j0 > 0.01: raise SystemExit('\n'.join(lines) + '\nTHE DEATH DOES NOT START ON HIS POSE')
    if (d_wall > WALL or d_floor > FLOOR_SLACK): raise SystemExit('\n'.join(lines) + '\nTHE DEATH GOES INTO THE BAY')
    open(os.path.join(out, 'mg_gunner.txt'), 'w').write('\n'.join(lines) + '\n'); print('\n'.join(lines))
    if qc:
        text = open(qc).read()
        if '"ww2_halftrack_mg"' not in text:
            add = '\n$poseparameter "mg_yaw" %d %d\n$poseparameter "mg_pitch" %g %g\n' % (YAWS[0], YAWS[-1], rows[0], rows[-1])
            for (y, p), n in poses.items():
                add += '$animation "a_%s" "ww2_a_%s.smd" {\n\tfps 30\n}\n' % (n, n)
            add += '$sequence "ww2_halftrack_mg" {\n' + ''.join('\t"a_%s"\n' % poses[(y, p)] for p in rows for y in YAWS)
            add += '\tblend "mg_yaw" %d %d\n\tblend "mg_pitch" %g %g\n\tblendwidth %d\n\tfadein 0.2\n\tfadeout 0.2\n}\n' % (
                YAWS[0], YAWS[-1], rows[0], rows[-1], len(YAWS))
            for y in YAWS:
                add += '$animation "a_%s" "ww2_a_%s.smd" {\n\tfps 30\n}\n' % (deaths[y], deaths[y])
            add += '$sequence "ww2_halftrack_mg_death" {\n' + ''.join('\t"a_%s"\n' % deaths[y] for y in YAWS)
            add += '\tblend "mg_yaw" %d %d\n\tblendwidth %d\n\tfadein 0.2\n\tfadeout 0.2\n}\n' % (YAWS[0], YAWS[-1], len(YAWS))
            open(qc, 'a').write(add); print('added ww2_halftrack_mg to', qc)
    return dict(stand=origin, mount=mount, origin=origin, rows=rows, rolls=rolls, hold_t=hold_t, poses=poses, deaths=deaths)

if __name__ == '__main__':
    main(*sys.argv[1:7])
