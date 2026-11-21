"""Halftrack troop preview: six riders in the bay, the rear doors kicked open, everyone out and running to the sides.

    python3 troop_preview.py <dodsp_tools dir> <rig out dir> <ride pack dir> <main pack dir> <CoD2 pack dir> <out dir> [norender]

What it draws is what the game will do (change both together):
  * the vehicle = the rigged halftrack (vehicle_rig.py output), its doors on door_left/door_right turned by the
    "doors" curve - CoD2's own (halftrack_halftrack_jumpout: the doors stay shut to frame 46, are kicked open in 5
    frames, bounce to 117 degrees and settle at 101), started when the dismount starts, like CoD2;
  * seats = CoD2's tag_guy01..06 (dodsp_tools/generated/cod2_vehicle_tags.txt; left seats face -y, right +y - their
    tag rotations), each with its own CoD2 idle;
  * the two door seats (guy03 left, guy05 right) get out with their own CoD2 get-outs (seated -> out of the door),
    origin = seat + the path table (content/scripts/dodsp_ride_paths.txt) + the landing correction, like V3;
  * the four aisle seats (guy01, 02, 04, 06) have no get-out of their own. CoD2's guy02 / guy02b get-outs start
    crouched in the aisle at tag_out (centre of the bay, facing the doors) and walk out. A man joins that walk at the
    frame where it passes his seat: STAND_IN seconds in which his origin goes from his seat to that point of the walk,
    his yaw turns to the walk's, and his pose blends from his idle to that frame of the walk (the engine blends two
    sequences in bone space at the current origin - so does this); then the walk plays on;
  * on the ground each man runs to his rally spot (CoD2's tag_walker0..5 = its escort spots beside the vehicle),
    round the rear corner if the straight line would cut through the vehicle; there he stops (the AI takes over);
  * a second clip: a man shot while running off plays CoD2's halftrack_guy_rundeath.
Start times: the door men at 0 (CoD2's sync with the doors); each aisle man as early as he can go without coming
closer than CLEAR units (pelvis, from above) to anyone - searched, printed, and these are the seats' "delay" values.
Checks printed: the clearance found per man, how close anyone's limbs came to the open doors, everyone out at."""
import os, sys, numpy as np
from PIL import Image, ImageDraw
TOOLS, RIG, PACK, MAIN, COD2, OUT = sys.argv[1:7]
sys.path.insert(0, TOOLS); sys.path.insert(0, os.path.join(TOOLS, 'vehicle_preview'))
from smd import SMD
from render_pose import world
from render3d import Camera, _raster, splat_figures
from track_preview import read_rig
from editor_models import kv_parse
os.makedirs(OUT, exist_ok=True)

FPS = 30.0
STAND_IN = 0.5           # s: seat -> the aisle walk
BLEND = 0.2              # s: the sequences' fadein
RUN_SPEED = 227.0        # units/s: combatrun_forward_1's own travel (121 units in 16 frames)
RUN_ACCEL = 0.35         # s to full speed from standing
TURN_RATE = 540.0        # deg/s while running
CLEAR = 18.0             # units: closest two men may come (pelvis, from above); seated legs reach into the aisle
DOOR_MAX = 120.0         # degrees at "doors" = 1 (the rig's DOOR_OPEN must match, so CoD2's 117-degree bounce fits)

# ---- data the game reads -------------------------------------------------------------------------------------
def tags(vehicle):
    out, cur = {}, None
    for l in open(os.path.join(TOOLS, 'generated/cod2_vehicle_tags.txt')):
        if l and not l.startswith(' ') and '(' in l: cur = l.split()[0]
        elif cur == vehicle and l.strip().startswith('tag'):
            f = l.split(); out[f[0]] = np.array([float(x) for x in f[3:6]])
    return out
T = tags('halftrack')
PATHS = {}
for seq, blk in dict(kv_parse(open(os.path.join(TOOLS, 'content/scripts/dodsp_ride_paths.txt')).read()))['RidePaths']:
    if not isinstance(blk, list): continue
    d = dict(blk)
    PATHS[seq] = dict(frames=int(d['frames']), take=int(d['take']), land=int(d['land']),
                      path=[np.array([float(x) for x in d[str(i)].split()]) for i in range(int(d['frames']))])
OUT_YAW = 179.1          # tag_out's rotation (faces the doors)
# seat: CoD2 tag, yaw, idle, how he gets out, the CoD2 walker tag he runs to. The first out run furthest (the front
# spots), the last out stop nearest (the rear ones), so nobody runs past a man already standing at his spot
SEATS = [('guy03', -90, 'halftrack_guy03_idle', ('own', 'halftrack_guy03_jumpout'), 'tag_walker0'),
         ('guy05', 90, 'halftrack_guy05_idle', ('own', 'halftrack_guy05_jumpout'), 'tag_walker1'),
         ('guy04', 90, 'halftrack_guy04_idle', ('aisle', 'halftrack_guy02_jumpout'), 'tag_walker3'),
         ('guy06', -90, 'halftrack_guy06_idle', ('aisle', 'halftrack_guy02b_jumpout'), 'tag_walker2'),
         ('guy02', 90, 'halftrack_guy02_idle', ('aisle', 'halftrack_guy02_jumpout'), 'tag_walker5'),
         ('guy01', -90, 'halftrack_guy01_idle', ('aisle', 'halftrack_guy02b_jumpout'), 'tag_walker4')]

# ---- poses ---------------------------------------------------------------------------------------------------
def G(p): return np.array([-p[1], p[0], p[2]])
RADIUS = (('Head', 4.5), ('Neck', 2.5), ('Spine', 5.5), ('Pelvis', 5.5), ('Thigh', 3.5), ('Calf', 2.8), ('Foot', 2.2),
          ('Toe', 1.8), ('Clavicle', 2.2), ('UpperArm', 2.3), ('Forearm', 2.0), ('Hand', 1.8))
_cache = {}
def anim(name):
    """per frame: {bone name: (parent point, point, radius)} in the animation's own frame (+x his forward)"""
    if name in _cache: return _cache[name]
    path = os.path.join(PACK, 'ww2_a_' + name + '.smd')
    if not os.path.exists(path): path = os.path.join(MAIN, 'ww2_a_' + name + '.smd')
    s = SMD(path); fr = []
    for f in range(len(s.frames)):
        W = world(s, f); d = {}
        for i, (n, p) in enumerate(s.nodes):
            if not n.startswith('ValveBiped.Bip01') or 'Finger' in n or (p >= 0 and p not in W): continue
            d[n] = (G(W[p if p >= 0 else i][1]), G(W[i][1]), next((r for k, r in RADIUS if k in n), 1.5))
        fr.append(d)
    _cache[name] = fr; return fr
def run_cycle():
    """combatrun_forward_1 in place (its forward travel taken out, the bob kept), frames 0..15"""
    if 'run' in _cache: return _cache['run']
    F = anim('combatrun_forward_1'); out = []
    for f in range(16):
        dx = F[f]['ValveBiped.Bip01_Pelvis'][1][0] - F[0]['ValveBiped.Bip01_Pelvis'][1][0]
        out.append({k: (a - [dx, 0, 0], b - [dx, 0, 0], r) for k, (a, b, r) in F[f].items()})
    _cache['run'] = out; return out
def lerp_pose(A, B, w): return {k: (A[k][0] + (B[k][0] - A[k][0]) * w, A[k][1] + (B[k][1] - A[k][1]) * w, A[k][2]) for k in A if k in B}
def rot(yaw):
    c, s = np.cos(np.radians(yaw)), np.sin(np.radians(yaw)); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
def place(pose, origin, yaw):
    R = rot(yaw); return [(R @ a + origin, R @ b + origin, r) for a, b, r in pose.values()]
def smooth(w): w = min(max(w, 0.0), 1.0); return w * w * (3 - 2 * w)
def angdiff(a, b): return (b - a + 180) % 360 - 180

# ---- the vehicle ---------------------------------------------------------------------------------------------
names, parent, rest, tris, susp, wheels = read_rig(RIG, 'halftrack')
dl, dr = names.index('door_left'), names.index('door_right')
P0 = np.array([[p for p, w in vs] for m, vs in tris])
door_of = np.array([max(vs[0][1], key=lambda bw: bw[1])[0] for m, vs in tris])
is_door = (door_of == dl) | (door_of == dr)
LO, HI = P0.reshape(-1, 3).min(0), P0.reshape(-1, 3).max(0)
def door_curve():
    import cod2_xanim as X
    a = X.read(os.path.join(COD2, 'xanim/halftrack_halftrack_jumpout'))
    fr, q = a['bones']['hatch_open_left']['rot']
    deg = -np.degrees(2 * np.arcsin(q[:, 2]))
    return lambda t: float(np.interp(t * FPS, fr, deg))
DOOR_DEG = door_curve()
def door_tris(t):
    th = DOOR_DEG(t); out = []
    for k in np.where(is_door)[0]:
        b = door_of[k]; c = rest[b]; R = rot(-th if b == dl else th)
        out.append([R @ (p - c) + c for p in P0[k]])
    return np.array(out)

# ---- one man on the code's timeline --------------------------------------------------------------------------
class Man:
    def __init__(self, seat, start, death_at=None):
        self.tag, self.yaw0, self.idle, (self.kind, self.seq), self.rally = seat
        self.seat = T['tag_' + self.tag]; self.start = start; self.death_at = death_at
        self.p = PATHS['ww2_jump_' + self.seq]
        F = anim(self.seq)
        if len(F) != self.p['frames']: raise SystemExit('%s: pack %d frames, path table %d' % (self.seq, len(F), self.p['frames']))
        if self.kind == 'own':
            self.base, self.byaw, self.fj, self.t_walk = self.seat, self.yaw0, 0, start
        else:   # join the aisle walk where it passes his seat
            self.base, self.byaw = T['tag_out'], OUT_YAW
            xs = [self.walk_origin(f, ground=False)[0] for f in range(self.p['frames'])]
            self.fj = next((f for f in range(self.p['frames']) if xs[f] <= self.seat[0]), 0)
            self.t_walk = start + STAND_IN
        self.t_out = self.t_walk + (self.p['frames'] - 1 - self.fj) / FPS
        self.build_run()
    def walk_origin(self, f, ground=True):
        x, y, z, s = self.p['path'][f]
        total = (0.0 - (self.base[2] + self.p['path'][-1][2])) if ground else 0.0
        return self.base + rot(self.byaw) @ np.array([x, y, z + total * s])
    def build_run(self):
        """the run off: end of the get-out -> (rear corner if the line cuts the vehicle) -> rally spot, sampled"""
        a = self.walk_origin(self.p['frames'] - 1); a[2] = 0; goal = T[self.rally].copy(); goal[2] = 0
        side = np.sign(goal[1]) or 1.0
        pts = [a]
        def cuts(p, q):
            for t in np.linspace(0, 1, 40):
                m = p + (q - p) * t
                if LO[0] - 14 < m[0] < HI[0] + 14 and abs(m[1]) < HI[1] + 16: return True
            return False
        if cuts(a, goal): pts.append(np.array([LO[0] - 30, side * (HI[1] + 34), 0.0]))
        pts.append(goal)
        self.route = pts
        L = [np.linalg.norm(pts[i + 1] - pts[i]) for i in range(len(pts) - 1)]
        self.route_len = sum(L)
        # time to cover s: accelerate over RUN_ACCEL, then RUN_SPEED
        acc_d = 0.5 * RUN_SPEED * RUN_ACCEL
        self.run_time = (RUN_ACCEL + (self.route_len - acc_d) / RUN_SPEED) if self.route_len > acc_d else np.sqrt(2 * self.route_len * RUN_ACCEL / RUN_SPEED)
        self.t_rally = self.t_out + self.run_time
        # yaw along the run: turn at TURN_RATE toward the leg's direction
        self.yaws = []; y = self.byaw if self.kind == 'aisle' else self.yaw0 + self.end_turn()
        for i in range(int(self.run_time * FPS) + 2):
            s_ = self.dist(i / FPS); p = self.at(s_); q = self.at(min(s_ + 8, self.route_len))
            want = np.degrees(np.arctan2(*(q - p)[[1, 0]])) if np.linalg.norm(q - p) > 1e-3 else y
            y += np.clip(angdiff(y, want), -TURN_RATE / FPS, TURN_RATE / FPS); self.yaws.append(y)
    def end_turn(self):
        return 0.0
    def dist(self, t):
        if t <= RUN_ACCEL: return min(0.5 * RUN_SPEED / RUN_ACCEL * t * t, self.route_len)
        return min(0.5 * RUN_SPEED * RUN_ACCEL + RUN_SPEED * (t - RUN_ACCEL), self.route_len)
    def at(self, s_):
        for i in range(len(self.route) - 1):
            L = np.linalg.norm(self.route[i + 1] - self.route[i])
            if s_ <= L or i == len(self.route) - 2: return self.route[i] + (self.route[i + 1] - self.route[i]) * min(s_ / max(L, 1e-6), 1)
            s_ -= L
    # ---- state at time t: (local pose, origin, yaw) ----
    def state(self, t):
        F = anim(self.seq)
        if t < self.start: return anim(self.idle)[0], self.seat, self.yaw0
        if self.kind == 'aisle' and t < self.t_walk:
            w = smooth((t - self.start) / STAND_IN)
            return lerp_pose(anim(self.idle)[0], F[self.fj], w), self.seat + (self.walk_origin(self.fj) - self.seat) * w, \
                   self.yaw0 + angdiff(self.yaw0, self.byaw) * w
        if t < self.t_out:
            f = min(self.fj + int(round((t - self.t_walk) * FPS)), self.p['frames'] - 1)
            pose = F[f]
            if self.kind == 'own' and t < self.start + BLEND: pose = lerp_pose(anim(self.idle)[0], pose, smooth((t - self.start) / BLEND))
            return pose, self.walk_origin(f), self.byaw
        end_pose, end_org = F[-1], self.walk_origin(self.p['frames'] - 1)
        if self.death_at is not None and t >= self.death_at:
            D = anim('halftrack_guy_rundeath'); f = min(int((t - self.death_at) * FPS), len(D) - 1)
            pose, org, yaw = self.run_state(self.death_at, end_pose)
            return lerp_pose(pose, D[f], smooth((t - self.death_at) / 0.12)), org, yaw
        return self.run_state(t, end_pose)
    def run_state(self, t, end_pose):
        tr = t - self.t_out; R = run_cycle()
        s_ = self.dist(tr); org = self.at(s_); yaw = self.yaws[min(int(tr * FPS), len(self.yaws) - 1)]
        pose = R[int(s_ / (RUN_SPEED / FPS)) % 16]
        if tr < BLEND: pose = lerp_pose(end_pose, pose, smooth(tr / BLEND))
        if t > self.t_rally: pose = lerp_pose(pose, end_pose, smooth((t - self.t_rally) / 0.3))
        return pose, org, yaw
    def pelvis(self, t):
        pose, org, yaw = self.state(t); return rot(yaw) @ pose['ValveBiped.Bip01_Pelvis'][1] + org
    def figure(self, t):
        pose, org, yaw = self.state(t); return place(pose, org, yaw)

# ---- start times: everyone as early as the clearance allows -------------------------------------------------
_pel = {}
def pel(m, t):
    k = (id(m), round(t * 15))
    if k not in _pel: _pel[k] = m.pelvis(t)[:2]
    return _pel[k]
def closest(men, k, t_end):
    worst, who, when = 1e9, None, None
    for t in np.arange(min(men[k].start, 1e9), t_end, 1 / 15.0):
        a = men[k].pelvis(t)[:2]
        for j, m in enumerate(men):
            if j == k: continue
            d = np.linalg.norm(a - pel(m, t))
            if d < worst: worst, who, when = d, m.tag, t
    return worst, who, when
men = []
for seat in SEATS:
    t0 = 0.0 if seat[3][0] == 'own' else max([m.start for m in men] + [0.0])
    while True:
        cand = men + [Man(seat, t0)]
        t_end = max(m.t_rally for m in cand) + 0.5
        d, who, when = closest(cand, len(cand) - 1, t_end)
        if d >= CLEAR or t0 > 12: break
        t0 += 0.05
    men = cand
    print('%s  %-6s start %.2f s  (out of the vehicle %.2f, at his spot %.2f)  closest %s'
          % (seat[0], seat[3][0], t0, men[-1].t_out, men[-1].t_rally, 'nobody near' if who is None else '%.1f units to %s at %.2f s' % (d, who, when)))
end = max(m.t_rally for m in men)
print('everyone at his spot after %.2f s' % end)
# limbs vs the open doors
DP = None
worst = 1e9
for t in np.arange(0, end, 1 / FPS):
    D = door_tris(t).reshape(-1, 3)
    if DOOR_DEG(t) < 60: continue
    for m in men:
        for a, b, r in m.figure(t):
            for q in (a, b):
                d = np.min(np.linalg.norm(D - q, axis=1)) - r
                if d < worst: worst, who_d, when_d = d, m.tag, t
print('closest any limb came to an open door: %.1f units (%s at %.2f s; door corners only, a rough check)' % (worst, who_d, when_d))

# the bay as boxes (vehicle_rig.py OPEN_TOP's numbers: floor z 31, walls |y| 30, rear plate/doors x -105, front x 20):
# how deep any limb goes into the floor, a wall, or the shut doors while the man is still in the vehicle
BAY = dict(x0=-105.0, x1=20.0, floor=31.0, wall=30.0, top=75.0)
for m in men:
    wf = ww = wd = 0.0; where = ''
    for t in np.arange(0, m.t_out, 1 / 15.0):
        for a, b, r in m.figure(t):
            for q in (a, (a + b) / 2, b):
                inside = BAY['x0'] < q[0] < BAY['x1'] and abs(q[1]) < BAY['wall']
                if inside and q[2] < BAY['floor'] + 12 and BAY['floor'] - (q[2] - r) > wf:
                    wf = BAY['floor'] - (q[2] - r); where = '%.2f s at %d %d %d' % (t, *q)
                if BAY['x0'] < q[0] < BAY['x1'] and BAY['floor'] < q[2] < BAY['top']: ww = max(ww, abs(q[1]) + r - BAY['wall'])
                if DOOR_DEG(t) < 5 and abs(q[1]) < BAY['wall'] and BAY['floor'] < q[2] < BAY['top']: wd = max(wd, BAY['x0'] - (q[0] - r))
    print('   %s in the vehicle: into the floor %.1f, into a wall %.1f, into the shut doors %.1f units  (floor: %s)' % (m.tag, max(wf, 0), max(ww, 0), max(wd, 0), where))

# ---- render --------------------------------------------------------------------------------------------------
COLOURS = [(92, 98, 78), (110, 104, 82), (84, 90, 96), (120, 92, 72), (78, 100, 92), (104, 96, 110)]
def ground_and_hull(cam, t):
    Z = np.full((cam.h, cam.w), -1e9); C = np.zeros((cam.h, cam.w, 3)); C[:] = (235, 238, 242)
    g, gc = [], []
    for i, x in enumerate(np.arange(-350, 250, 50)):
        for j, y in enumerate(np.arange(-250, 250, 50)):
            q = np.array([[x, y, 0], [x + 50, y, 0], [x + 50, y + 50, 0], [x, y + 50, 0]])
            g += [q[[0, 1, 2]], q[[0, 2, 3]]]; gc += [(196, 201, 186) if (i + j) % 2 else (186, 192, 176)] * 2
    _raster(cam, np.array(g, float), gc, Z, C)
    TT = np.concatenate([P0[~is_door], door_tris(t)])
    n = np.cross(TT[:, 1] - TT[:, 0], TT[:, 2] - TT[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    light = np.array([0.35, 0.55, 0.76]); light /= np.linalg.norm(light)
    col = np.array([(212, 204, 180)] * int((~is_door).sum()) + [(176, 160, 128)] * int(is_door.sum()), float)
    _raster(cam, TT, (col * (0.45 + 0.55 * np.abs(n @ light))[:, None]).clip(0, 255), Z, C)
    return Z, C
def gif(men, cams, path, caption, t_end, t_start=0.0):
    frames = []
    for i in range(int(t_start * 15), int(t_end * 15) + 1):
        t = i / 15.0; row = []
        for cam in cams:
            Z, C = ground_and_hull(cam, t)
            img = splat_figures(cam, Z, C, [(m.figure(t), COLOURS[k % 6]) for k, m in enumerate(men)])
            row.append(img)
        im = Image.fromarray(np.concatenate(row, axis=1)); d = ImageDraw.Draw(im)
        d.text((8, 6), '%s   t = %.2f s   doors %.0f deg' % (caption, t, DOOR_DEG(t)), fill=(20, 20, 20))
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=67, loop=0, optimize=True)
cams = [Camera(205, 30, (-90, 0, 30), 1.45, 640, 460), Camera(180, 89, (-60, 0, 0), 1.05, 400, 460)]
if 'norender' not in sys.argv: gif(men, cams, os.path.join(OUT, 'h3_troops_getout.gif'), 'halftrack: 6 troops out (doors kicked open, run to the sides)', end + 0.8)
# the bay close up: the aisle men standing into the aisle and walking out
if 'norender' not in sys.argv: gif(men, [Camera(200, 58, (-70, 0, 45), 3.0, 720, 520)], os.path.join(OUT, 'h3_troops_bay.gif'),
    'the bay: aisle men stand into the walk (STAND_IN %.1f s)' % STAND_IN, 4.6, 1.4)
# run-death: the last man out (guy01) shot 0.35 s into his run
dead = Man(SEATS[5], men[5].start, death_at=men[5].t_out + 0.35)
cams2 = [Camera(225, 24, (-150, 40, 25), 2.2, 640, 400)]
if 'norender' not in sys.argv: gif(men[:5] + [dead], cams2, os.path.join(OUT, 'h3_troops_rundeath.gif'), 'guy01 shot while running off: halftrack_guy_rundeath',
    dead.death_at + 19 / FPS + 1.0)
for m in men: print('   %s: %s, delay %.2f s, runs %.0f units to %s%s' % (m.tag, m.kind, m.start, m.route_len, m.rally,
                    ' (round the rear corner)' if len(m.route) > 2 else ''))
