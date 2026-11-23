"""THE owner of where the support (left) hand sits on a rifle in the CoD animation packs (N1, 2026-11-22).

The fault (audit "DOD-SP NPC animation audit", 2026-11-22): the packs place the K98 on DoD's grip in the RIGHT hand
(gun_follow.py), but the left arm is CoD's, with DoD's bone lengths - so where the CoD man holds the fore-end, the DoD
arm's hand ends 3-11 units back along the rifle. One class, one fix, here, in the builders: wherever the SOURCE
animation has the left hand on its rifle as the support hand, the left arm is solved onto DoD's K98 grip (two-bone IK,
the elbow kept in the plane the animation put it in, the hand taking DoD's grip orientation), and weapon_bone_left (a
child of the left hand) is written again so the rifle stays exactly where gun_follow put it.
MEASURED WHILE BUILDING IT (correcting the audit): the audit's "median 11.1 units over 332 holding animations" took
every frame of an animation that holds the rifle somewhere - hands on a helmet, a grenade, the ground, a left-
shouldered corner. In frames where the source really holds the rifle two-handed the miss was 3-6 units (worst 11),
mostly inside the client SupportHandIK's full band (8): the bake makes the data right and the blends consistent, but
it was NOT what made K98 men look wrong "in every animation" (dodsp_grip_report, client, reads what's on screen).
TRIED, DROPPED: sliding the rifle so a LEFT-held K98 (gun_follow holder 1) sat on DoD's grip - the right hand then
missed the trigger by 1-2 units more in every CoD2 combat run.

    Grip(dod_bolt_smd)          DoD's left hand in weapon_bone_left's space (StandAim_BOLT) = the client's s_GripLeftHung
    grip.support_weights(src_frames, labels, gun_offset, loop) -> per frame 0..1: how much the left hand is the support
        hand (the gun isn't following the left hand AND the source's left hand is on the rifle - see ON_*; smoothed in time,
        cycles wrap round their seam - a weight that steps would pop the arm)
    grip.solve(nodes, frame, gun_world, w, slide) -> the frame's left upper arm / forearm / hand locals replaced
        (the clavicle too when the grip is out of the arm's reach); returns the new left-hand world matrix (the caller
        writes weapon_bone_left from it), the miss left over (units, deg) and how far out of reach the grip was

The MP40 shares these animations: DoD's MP40 support grip sits 2.3 units from the K98's in the right hand's frame,
inside the client IK's full-strength band (8), so the MP40 men end on their own grip at runtime. A man whose source
hand is OFF the gun (working the bolt, a helmet, a grenade, a fall) keeps CoD's arm: weight 0 there.

INVARIANT: in every frame the source holds the gun two-handed (weight 1), the written left hand is on DoD's grip
(MISS_MAX units / MISS_MAX_DEG). Guard: Report.check() - the builders stop when any frame misses more; the miss is
measured on the WRITTEN frame (worlds rebuilt from the locals as studiomdl will), not on the solver's own numbers."""
import math
import numpy as np
from smd import SMD
from gun_follow import inv, K98_MUZZLE, holder_weights

# Where a source left hand counts as ON the rifle: measured in the SOURCE gun's frame (so it is CoD's own hold, before
# DoD's bone lengths move the hand), from the point DoD's grip maps to there, along the barrel and out from it. Measured
# on the CoD2 port (2026-11-22): hands holding the gun lie 0-5 units off the barrel line, -9..+6 along it from DoD's
# grip; out of that band = a pistol reload, a hand on the chest in a death, a grenade, a helmet.
ON_LAT_FULL, ON_LAT_NONE = 5.0, 7.5          # units off the barrel line
ON_BACK_FULL, ON_BACK_NONE = -10.0, -14.0    # units along it, behind DoD's grip (the bolt / trigger hand at ~-12.5)
ON_FWD_FULL, ON_FWD_NONE = 7.0, 11.0         # ...and ahead of it
TRIGGER_POS_FULL, TRIGGER_POS_NONE = 4.0, 8.0   # the right hand this far from its grip on the source gun...
TRIGGER_DEG_FULL, TRIGGER_DEG_NONE = 30.0, 60.0 # ...and turned this far: not the trigger hand
SLIDE_MIN, SLIDE_MAX = -12.0, 10.0           # the hand stays on the rifle between these (along it, from DoD's grip)
SMOOTH = 3           # frames, half-width of the time smoothing
MISS_MAX = 1.0       # units: a frame held two-handed (weight 1) must end this close to DoD's grip...
MISS_MAX_DEG = 3.0   # ...and turned this close to its orientation
REACH_SPARE = 0.05   # units: the arm is never solved dead straight
REACH_SHORT_MAX = 3.0  # units: a held frame may fall this short of the grip with the arm straight and the clavicle
                       # forward (a pain throwing the shoulder back, a hand arriving); further = the build stops
CLAV_MAX_DEG = 20.0  # the clavicle may come forward this far when the grip is out of the arm's reach

VB = 'ValveBiped.Bip01_'

def e2m(x, y, z):
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return (np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]) @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
            @ np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]]))

def m2e(R):
    y = math.asin(max(-1.0, min(1.0, -R[2, 0])))
    return math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0])

def to4(v):
    M = np.eye(4); M[:3, :3] = e2m(*v[3:]); M[:3, 3] = v[:3]; return M

def worlds(nodes, frame):
    W = {}
    for i, (_, p) in enumerate(nodes):
        if i not in frame: continue
        W[i] = to4(frame[i]) if p < 0 or p not in W else W[p] @ to4(frame[i])
    return W

def _smoothstep(x):
    x = min(1.0, max(0.0, x)); return x * x * (3 - 2 * x)

def _turn(u, v):
    u = u / np.linalg.norm(u); v = v / np.linalg.norm(v); ax = np.cross(u, v); s = np.linalg.norm(ax)
    if s < 1e-9: return np.eye(3)
    ax /= s; a = math.atan2(s, float(u @ v))
    K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
    return np.eye(3) + math.sin(a) * K + (1 - math.cos(a)) * K @ K

def _slerp_R(A, B, t):
    """rotation t of the way from A to B (both 3x3)"""
    D = A.T @ B; c = max(-1.0, min(1.0, (np.trace(D) - 1) / 2)); a = math.acos(c)
    if a < 1e-6 or t <= 0: return A
    if t >= 1: return B
    ax = np.array([D[2, 1] - D[1, 2], D[0, 2] - D[2, 0], D[1, 0] - D[0, 1]]) / (2 * math.sin(a))
    K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]]); b = a * t
    return A @ (np.eye(3) + math.sin(b) * K + (1 - math.cos(b)) * K @ K)

def rot_deg(A, B):
    return math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(A.T @ B) - 1) / 2))))

class Grip:
    def __init__(self, dod_bolt_smd):
        s = SMD(dod_bolt_smd); W = worlds(s.nodes, s.frames[0][1])
        self.grip = inv(W[s.index('ValveBiped.weapon_bone_left')]) @ W[s.index(VB + 'L_Hand')]

        self.barrel = K98_MUZZLE / np.linalg.norm(K98_MUZZLE)  # from the right hand (the bone's origin) to the muzzle

    def support_weights(self, src_frames, labels, gun_offset, loop, right_rel):
        """per frame (w, slide). w 0..1: how much the source's left hand is the SUPPORT hand; slide: where along the
        rifle it holds, units from DoD's grip (+ = toward the muzzle) - CoD's men slide the hand along the fore-end
        (reloads, a swing), and a hold far from DoD's grip is out of the DoD arm's reach at the grip itself (a melee
        swing, picking a man up: 4-8 units short). src_frames = (R, L, G) per frame (the source rig, model space);
        labels = gun_follow's holders (1 = the gun follows the left hand); gun_offset = the builder's O for the K98
        (source gun frame -> weapon_bone_left); right_rel = the source's gun in the right hand in its reference aim pose
        (gun_follow.GunReference.rel_R). Both smoothed in time; cycles wrap round their seam."""
        g = (gun_offset @ np.r_[self.grip[:3, 3], 1.0])[:3]          # DoD's grip, in the source gun's frame
        ax = gun_offset[:3, :3] @ self.barrel
        raw, al = [], []
        # where the gun follows the LEFT hand (gun_follow's holder 1), the hand places the gun, not the other way
        # round: the support weight gives way to that hand's share of the gun's placement, cross-fade included (a
        # full weight inside a hand-over chased a target the blended gun swept out of the arm's reach)
        # and the RIGHT hand must be the trigger hand: CoD1's right-side corners shoulder the rifle LEFT-handed (the
        # right hand 10-16 units and ~100 degrees off its grip, the left hand on the trigger) - the K98's support grip
        # is not that hold
        for (R, L, G), hw in zip(src_frames, holder_weights(labels, loop)):
            p = (inv(G) @ L)[:3, 3] - g; along = float(p @ ax); lat = float(np.linalg.norm(p - along * ax))
            rel = inv(R) @ G
            dr = float(np.linalg.norm(rel[:3, 3] - right_rel[:3, 3])); ar = rot_deg(rel[:3, :3], right_rel[:3, :3])
            w = 1.0 - float(hw[1])
            w *= 1.0 - _smoothstep((dr - TRIGGER_POS_FULL) / (TRIGGER_POS_NONE - TRIGGER_POS_FULL))
            w *= 1.0 - _smoothstep((ar - TRIGGER_DEG_FULL) / (TRIGGER_DEG_NONE - TRIGGER_DEG_FULL))
            w *= 1.0 - _smoothstep((lat - ON_LAT_FULL) / (ON_LAT_NONE - ON_LAT_FULL))
            if along < ON_BACK_FULL: w *= 1.0 - _smoothstep((ON_BACK_FULL - along) / (ON_BACK_FULL - ON_BACK_NONE))
            if along > ON_FWD_FULL: w *= 1.0 - _smoothstep((along - ON_FWD_FULL) / (ON_FWD_NONE - ON_FWD_FULL))
            raw.append(w); al.append(min(max(along, SLIDE_MIN), SLIDE_MAX))
        n = len(raw); out = []
        for i in range(n):
            acc = ws = sa = sw = 0.0
            for k in range(-SMOOTH, SMOOTH + 1):
                j = (i + k) % n if loop else min(n - 1, max(0, i + k))
                kk = SMOOTH + 1 - abs(k)
                acc += kk * raw[j]; ws += kk; sa += kk * raw[j] * al[j]; sw += kk * raw[j]
            w = 1.0 if raw[i] >= 1.0 else acc / ws                # a frame fully on the rifle stays fully on it
            out.append((w, sa / sw if sw > 1e-9 else 0.0))
        return out

    def target(self, gun_world, slide=0.0):
        S = np.eye(4); S[:3, 3] = self.barrel * slide
        return gun_world @ S @ self.grip

    def solve(self, nodes, frame, gun_world, w, slide=0.0):
        """left arm onto DoD's grip on the K98 at gun_world (4x4, model space), weight w. Where DoD's grip is out of the
        arm's reach even with the clavicle forward (a melee swing, a pain throwing the shoulder back), the hand takes
        the nearest point it reaches on the rifle, moving along the barrel toward where the SOURCE hand holds it
        (slide, units from DoD's grip) - never past it."""
        idx = {n.lower(): i for i, (n, _) in enumerate(nodes)}
        U, F, H = (idx[(VB + 'L_' + x).lower()] for x in ('UpperArm', 'Forearm', 'Hand'))
        W = worlds(nodes, frame)
        T = self.target(gun_world)
        short = 0.0
        if w > 1e-4:
            S, E, Hc = W[U][:3, 3], W[F][:3, 3], W[H][:3, 3]
            goal = Hc + (T[:3, 3] - Hc) * w
            a, b = np.linalg.norm(E - S), np.linalg.norm(Hc - E)
            if np.linalg.norm(goal - S) > a + b - REACH_SPARE:
                # out of the arm's reach from where CoD's shoulder is (a man reaching further forward brings his
                # shoulder forward): turn the clavicle toward the grip, as little as reaches, at most CLAV_MAX_DEG
                C = nodes[U][1]; Cp = W[C][:3, 3]; v = S - Cp; to = goal - Cp
                ax = np.cross(v, to); sn = np.linalg.norm(ax)
                if sn > 1e-9:
                    ax /= sn; K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
                    rot = lambda t: np.eye(3) + math.sin(t) * K + (1 - math.cos(t)) * K @ K
                    lo, hi = 0.0, min(math.radians(CLAV_MAX_DEG), math.atan2(sn, float(v @ to)))
                    if np.linalg.norm(goal - (Cp + rot(hi) @ v)) <= a + b - REACH_SPARE:
                        for _ in range(30):
                            mid = (lo + hi) / 2
                            if np.linalg.norm(goal - (Cp + rot(mid) @ v)) <= a + b - REACH_SPARE: hi = mid
                            else: lo = mid
                    PC = W[nodes[C][1]][:3, :3]
                    frame[C] = tuple(frame[C][:3]) + m2e(PC.T @ rot(hi) @ W[C][:3, :3])
                    W = worlds(nodes, frame)
                    S, E, Hc = W[U][:3, 3], W[F][:3, 3], W[H][:3, 3]
            reach = a + b - REACH_SPARE
            if np.linalg.norm(goal - S) > reach and abs(slide) > 1e-3:
                gl = lambda sl: Hc + (self.target(gun_world, sl)[:3, 3] - Hc) * w
                lo, hi = 0.0, 1.0
                if np.linalg.norm(gl(slide) - S) <= reach:
                    for _ in range(30):
                        mid = (lo + hi) / 2
                        if np.linalg.norm(gl(slide * mid) - S) <= reach: hi = mid
                        else: lo = mid
                T = self.target(gun_world, slide * hi); goal = gl(slide * hi)
            d = goal - S; c0 = np.linalg.norm(d); d /= max(c0, 1e-9)
            short = max(0.0, c0 - reach)
            c = min(max(c0, abs(a - b) + 1e-3), a + b - 1e-3)
            pole = (E - S) - d * ((E - S) @ d)
            if np.linalg.norm(pole) < 1e-6:          # arm straight in the source: bend toward its forearm's side
                pole = (Hc - E) - d * ((Hc - E) @ d)
            pole /= max(np.linalg.norm(pole), 1e-9)
            cs = (a * a + c * c - b * b) / (2 * a * c)
            E2 = S + d * a * cs + pole * a * math.sqrt(max(0.0, 1 - cs * cs)); H2 = S + d * c
            r1 = _turn(E - S, E2 - S); RU = r1 @ W[U][:3, :3]
            Hmid = S + r1 @ (Hc - S); RF = _turn(Hmid - E2, H2 - E2) @ r1 @ W[F][:3, :3]
            RH = _slerp_R(W[H][:3, :3], T[:3, :3], w)
            PU = W[nodes[U][1]][:3, :3]
            for k, Rl in ((U, PU.T @ RU), (F, RU.T @ RF), (H, RF.T @ RH)):
                frame[k] = tuple(frame[k][:3]) + m2e(Rl)
            W = worlds(nodes, frame)                 # the written frame, as studiomdl will read it
        miss = np.linalg.norm(W[H][:3, 3] - T[:3, 3]); deg = rot_deg(W[H][:3, :3], T[:3, :3])
        slid = float(np.linalg.norm(T[:3, 3] - self.target(gun_world)[:3, 3]))
        return W[H], miss, deg, short, slid

class Report:
    """per animation: frames held two-handed, worst miss before/after; check() stops the build over the bar"""
    def __init__(self):
        self.rows = {}
    def add(self, name, weights, before, after):
        """after = per frame (miss, deg, short, slid) from solve(); before = (miss, ...) with weight 0"""
        full = [i for i, w in enumerate(weights) if w >= 1.0 and after[i][2] <= 0.0]
        far = [i for i, w in enumerate(weights) if w >= 1.0 and after[i][2] > 0.0]
        held = full + far
        self.rows[name] = dict(frames=len(weights), held=len(held), far=len(far),
                               before=max((before[i][0] for i in held), default=0.0),
                               after=max((after[i][0] for i in full), default=0.0),
                               after_deg=max((after[i][1] for i in full), default=0.0),
                               short=max((after[i][2] for i in far), default=0.0),
                               slid=sum(1 for i in held if after[i][3] > 0.01),
                               slid_max=max((after[i][3] for i in held), default=0.0))
    def write(self, path):
        """SUPPORT_GRIP.txt next to the pack's SEQUENCES.txt: per animation, how much of it is held two-handed and how
        far the left hand was from DoD's grip there before the solve"""
        with open(path, 'w', newline='\r\n') as f:
            f.write('animation\tframes\theld\tslid along the rifle to reach\tout of reach\tworst miss before\tafter\tshort\n')
            for k, r in sorted(self.rows.items()):
                f.write(f'{k}\t{r["frames"]}\t{r["held"]}\t{r["slid"]} (up to {r["slid_max"]:.1f})\t{r["far"]}\t{r["before"]:.2f}\t{r["after"]:.2f}\t{r["short"]:.2f}\n')

    def check(self, label):
        held = {k: r for k, r in self.rows.items() if r['held']}
        bad = {k: r for k, r in held.items() if r['after'] > MISS_MAX or r['after_deg'] > MISS_MAX_DEG
               or r['short'] > REACH_SHORT_MAX}
        bef = sorted(r['before'] for r in held.values())
        med = bef[len(bef) // 2] if bef else 0.0
        far = {k: r for k, r in held.items() if r['far']}
        print(f'  support grip ({label}): {len(held)} animations hold the rifle two-handed somewhere; worst miss per '
              f'animation before: median {med:.1f}, max {max(bef, default=0):.1f} units; after: max '
              f'{max((r["after"] for r in held.values()), default=0):.2f} units / '
              f'{max((r["after_deg"] for r in held.values()), default=0):.1f} deg; slid along the rifle to reach: '
              f'{sum(r["slid"] for r in held.values())} frames (up to {max((r["slid_max"] for r in held.values()), default=0):.1f}); out of the arm\'s reach: '
              f'{sum(r["far"] for r in far.values())} frames in {len(far)} animations, worst '
              f'{max((r["short"] for r in far.values()), default=0):.2f} units short')
        for k, r in sorted(far.items()):
            print(f'    reach {k}: {r["far"]} frames, up to {r["short"]:.2f} units short (arm straight, clavicle forward)')
        if bad:
            for k, r in sorted(bad.items()):
                print(f'    MISS {k}: {r["after"]:.2f} units / {r["after_deg"]:.1f} deg after the solve, {r["short"]:.2f} short')
            raise SystemExit(f'INVARIANT support grip: {len(bad)} animations leave the left hand off DoD\'s grip '
                             f'(> {MISS_MAX} units / {MISS_MAX_DEG} deg, or > {REACH_SHORT_MAX} out of reach) in a '
                             f'frame held two-handed')
