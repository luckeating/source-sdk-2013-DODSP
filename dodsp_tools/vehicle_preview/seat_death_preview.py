"""Seated deaths on the halftrack: every seat x every candidate death, checked against the mesh and each other.

    python3 seat_death_preview.py <dodsp_tools dir> <rig out dir> <ride pack dir> <out dir>

A man who dies in a seat plays a death animation IN the seat and stays there (H2b: m_bDeadInSeat; a ragdoll can't ride
a vehicle). This tries each candidate death at each seat the way the game plays it - the idle, then the death blended
in over the sequence's 0.2 s fadein (bone space, at the seat), then held on its last frame - and prints per pair:
  jump     how far the pelvis moves at the switch (idle frame 0 vs death frame 0): a converted death next to a ported
           idle that isn't anchored (xanim_to_smd.py --anchor) rises 4-5 units here. FAIL over 3 units;
  mesh     how deep any limb goes under the vehicle's top surface (heightmap from above, as rider_preview);
  bay      into the bay floor / walls (vehicle_rig.py OPEN_TOP's boxes);
  others   the worst overlap with a man still sitting in another seat (limb capsules).
Picks per seat the candidate with the least trouble and renders the bay with everyone dying in turn
(h3_seat_deaths.gif). Seats and idles = what dodsp_vehicles.txt and the troop preview use (CoD2 tags)."""
import os, sys, numpy as np
from PIL import Image, ImageDraw
TOOLS, RIG, PACK, OUT = sys.argv[1:5]
sys.path.insert(0, TOOLS); sys.path.insert(0, os.path.join(TOOLS, 'vehicle_preview'))
from smd import SMD
from render_pose import world
from render3d import Camera, _raster, splat_figures
from track_preview import read_rig
from mesh_tris import heightmap
os.makedirs(OUT, exist_ok=True)
FPS, BLEND, JUMP_MAX = 30.0, 0.2, 3.0     # a pelvis slide of <= 3 units over the 0.2 s blend reads as the hit's jolt

def tags(vehicle):
    out, cur = {}, None
    for l in open(os.path.join(TOOLS, 'generated/cod2_vehicle_tags.txt')):
        if l and not l.startswith(' ') and '(' in l: cur = l.split()[0]
        elif cur == vehicle and l.strip().startswith('tag'):
            f = l.split(); out[f[0]] = np.array([float(x) for x in f[3:6]])
    return out
T = tags('halftrack')
BAY_DEATHS = ['halftrack_bay_death1', 'halftrack_bay_death2']
# seat: name, tag, yaw (CoD2 tag rotation), idle, candidate deaths
SEATS = [('driver', 'tag_driver', 0, 'halftrack_driver_idle', ['halftrack_driver_death']),
         ('commander', 'tag_passenger', 0, 'halftrack_passenger_idle', ['halftrack_commander_death', 'halftrack_guy_death2'])] + \
        [('guy0%d' % n, 'tag_guy0%d' % n, -90 if n in (1, 3, 6) else 90, 'halftrack_guy0%d_idle' % n, BAY_DEATHS)
         for n in (1, 2, 3, 4, 5, 6)]

def G(p): return np.array([-p[1], p[0], p[2]])
RADIUS = (('Head', 4.5), ('Neck', 2.5), ('Spine', 5.5), ('Pelvis', 5.5), ('Thigh', 3.5), ('Calf', 2.8), ('Foot', 2.2),
          ('Toe', 1.8), ('Clavicle', 2.2), ('UpperArm', 2.3), ('Forearm', 2.0), ('Hand', 1.8))
_c = {}
def anim(name):
    if name not in _c:
        s = SMD(os.path.join(PACK, 'ww2_a_' + name + '.smd')); fr = []
        for f in range(len(s.frames)):
            W = world(s, f); d = {}
            for i, (n, p) in enumerate(s.nodes):
                if not n.startswith('ValveBiped.Bip01') or 'Finger' in n or (p >= 0 and p not in W): continue
                d[n] = (G(W[p if p >= 0 else i][1]), G(W[i][1]), next((r for k, r in RADIUS if k in n), 1.5))
            fr.append(d)
        _c[name] = fr
    return _c[name]
def lerp(A, B, w): return {k: (A[k][0] + (B[k][0] - A[k][0]) * w, A[k][1] + (B[k][1] - A[k][1]) * w, A[k][2]) for k in A if k in B}
def rot(y):
    c, s = np.cos(np.radians(y)), np.sin(np.radians(y)); return np.array([[c, -s, 0], [s, c, 0], [0, 0, 1]])
def place(pose, o, y): R = rot(y); return [(R @ a + o, R @ b + o, r) for a, b, r in pose.values()]
def smooth(w): w = min(max(w, 0), 1); return w * w * (3 - 2 * w)
def pose_at(seat, death, t_die, t):
    name, tag, yaw, idle, _ = seat
    if death is None or t < t_die: return anim(idle)[0]
    D = anim(death); f = min(int((t - t_die) * FPS), len(D) - 1)
    return lerp(anim(idle)[0], D[f], smooth((t - t_die) / BLEND)) if t < t_die + BLEND else D[f]
def figure(seat, death, t_die, t): return place(pose_at(seat, death, t_die, t), T[seat[1]], seat[2])

names, parent, rest, tris, susp, wheels = read_rig(RIG, 'halftrack')
P0 = np.array([[p for p, w in vs] for m, vs in tris])
H, lo, _ = heightmap(P0, 1.0)
def top(x, y):
    i, j = int(y - lo[1]), int(x - lo[0])
    return H[i, j] if 0 <= i < H.shape[0] and 0 <= j < H.shape[1] and np.isfinite(H[i, j]) else None
BAY = dict(x0=-105.0, x1=20.0, floor=30.4, wall=30.0, top=75.0)
def segs_pts(fig):
    for a, b, r in fig:
        for t in (0, .25, .5, .75, 1): yield a + (b - a) * t, r

def overlap(fig, of):
    worst = 0.0
    for q, r in segs_pts(fig):
        if q[2] < 30: continue
        for a, b2, r2 in of:
            ab = b2 - a; u = np.clip(np.dot(q - a, ab) / max(np.dot(ab, ab), 1e-9), 0, 1)
            worst = max(worst, r + r2 - np.linalg.norm(a + ab * u - q))
    return worst
def check(seat, death):
    D = anim(death)
    jump = np.linalg.norm(D[0]['ValveBiped.Bip01_Pelvis'][1] - anim(seat[3])[0]['ValveBiped.Bip01_Pelvis'][1])
    # baseline: the idle's own depth (a seated man's legs already touch the bench); report what the death adds
    def depths(fig):
        mesh = bay = 0.0
        for q, r in segs_pts(fig):
            h = top(q[0], q[1])
            if h is not None and q[2] > 5: mesh = max(mesh, h - (q[2] - r) if h - q[2] < 30 else 0)
            if seat[0].startswith('guy') and BAY['x0'] < q[0] < BAY['x1']:
                if abs(q[1]) < BAY['wall']: bay = max(bay, BAY['floor'] - (q[2] - r))
                if BAY['floor'] < q[2] < BAY['top']: bay = max(bay, abs(q[1]) + r - BAY['wall'])
        return mesh, bay
    base = depths(figure(seat, None, 0, 0))
    base_over = {o[0]: overlap(figure(seat, None, 0, 0), figure(o, None, 0, 0)) for o in SEATS if o is not seat}
    mesh = bay = over = 0.0; who = ''
    others = [s for s in SEATS if s is not seat]
    for t in np.arange(0, BLEND + len(D) / FPS + 0.2, 1 / FPS):
        fig = figure(seat, death, 0.0, t)
        m, b = depths(fig); mesh, bay = max(mesh, m), max(bay, b)
        for o in others:
            of = figure(o, None, 0, 0)
            d = overlap(fig, of) - base_over[o[0]]
            if d > over: over, who = d, o[0]
    return dict(jump=jump, mesh=max(0, mesh - base[0]), bay=max(0, bay - base[1]), over=over, who=who)

print('seat        death                      jump  mesh   bay  others   (units; mesh/bay/others = what the death adds over the seated pose)')
pick = {}
for seat in SEATS:
    best = None
    for d in seat[4]:
        c = check(seat, d)
        bad = 'FAIL jump' if c['jump'] > JUMP_MAX else ''
        print('%-10s  %-24s %5.1f %5.1f %5.1f  %5.1f %s %s' % (seat[0], d, c['jump'], c['mesh'], c['bay'], c['over'], c['who'], bad))
        score = max(c['mesh'], c['bay'], c['over']) + (100 if bad else 0)
        if best is None or score < best[0]: best = (score, d)
    pick[seat[0]] = best[1]
print('picked: ' + ', '.join('%s %s' % (k, v.replace('halftrack_', '')) for k, v in pick.items()))

# render: everyone dies in turn, 0.5 s apart, in the bay
order = ['guy03', 'guy05', 'guy04', 'guy06', 'guy02', 'guy01', 'commander', 'driver']
t_die = {n: 0.6 + 0.8 * k for k, n in enumerate(order)}
COL = [(92, 98, 78), (110, 104, 82), (84, 90, 96), (120, 92, 72), (78, 100, 92), (104, 96, 110), (96, 86, 64), (70, 84, 70)]
cams = [Camera(205, 62, (-48, 0, 40), 3.6, 640, 560), Camera(270, 10, (-42, 0, 50), 3.4, 640, 560)]
CUT = [False, True]          # the side view (from the right) has the near wall cut away
def scene(cam, cut):
    Z = np.full((cam.h, cam.w), -1e9); C = np.zeros((cam.h, cam.w, 3)); C[:] = (235, 238, 242)
    n = np.cross(P0[:, 1] - P0[:, 0], P0[:, 2] - P0[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    L = np.array([0.35, 0.55, 0.76]); L /= np.linalg.norm(L)
    keep = P0[:, :, 1].mean(1) > -28 if cut else np.ones(len(P0), bool)
    _raster(cam, P0[keep], (np.array((212, 204, 180), float)[None] * (0.45 + 0.55 * np.abs(n[keep] @ L))[:, None]), Z, C)
    return Z, C
bg = [scene(c, k) for c, k in zip(cams, CUT)]
frames = []
end = max(t_die.values()) + BLEND + 35 / FPS + 0.8
for i in range(int(end * 15) + 1):
    t = i / 15.0; row = []
    for cam, (Z, C) in zip(cams, bg):
        figs = [(figure(s, pick[s[0]], t_die[s[0]], t), COL[k]) for k, s in enumerate(SEATS)]
        row.append(splat_figures(cam, Z, C, figs))
    im = Image.fromarray(np.concatenate(row, axis=1)); d = ImageDraw.Draw(im)
    d.text((8, 6), 'halftrack seated deaths, one every 0.8 s   t = %.2f s' % t, fill=(20, 20, 20))
    frames.append(im)
frames[0].save(os.path.join(OUT, 'h3_seat_deaths.gif'), save_all=True, append_images=frames[1:], duration=67, loop=0, optimize=True)
