"""V3 rider preview: every seat's ride idle and get-out drawn against the vehicle's real mesh.

    python3 rider_preview.py <dodsp_tools dir> <vehicle model dir> <ride pack dir> <out dir>

Positions follow the game code's rule (dodsp_vehicle.cpp V3, change both together):
  - a rider's animation is relative to his seat: the CoD tag = the seat origin, the tag's +x = the seat's yaw.
  - the get-out lands where the animation lands, horizontally; vertically the code adds the difference between
    the ground under the landing point and the ground the animation was made for, spread over the airborne
    frames (takeoff -> landing, measured from the feet).
Checks printed per seat: the deepest any body point goes under the vehicle's top surface (a leg through the
deck), seated and during the get-out, and how far the landing is corrected.
"""
import sys, os, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); sys.path.insert(0, sys.argv[1])
from mesh_tris import tris, heightmap
from smd import SMD
from render_pose import world
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
from render3d import Camera, background, splat_figures
from PIL import Image, ImageDraw

V, PACK, OUT = sys.argv[2], sys.argv[3], sys.argv[4]
FOOT_H = 4.5          # foot bone above the sole (DoD reference skeleton, standing)
os.makedirs(OUT, exist_ok=True)

# A seat: (name, origin x y z, yaw, idle, get-out). yaw +90 = faces the vehicle's left. The get-out is a list of
# segments (animation, first frame, last frame or None, tag origin, tag yaw) played one after another; the
# landing correction applies to the last one.
def out1(anim, org, yaw): return [(anim, 0, None, org, yaw)]
def tank_seats(front_x, rear_x, y, zf, zr):
    seats = []
    for side, sy, yaw in (('left', 1, 90), ('right', -1, -90)):
        o = (front_x, sy * y, zf); seats.append(('front ' + side, o, yaw, 'tankride_guy1_leanleft', out1('tankride_guy1_jumptocombatrun', o, yaw)))
        o = (rear_x, sy * y, zr); seats.append(('rear ' + side, o, yaw, 'tankride_guy6_driving_idle', out1('tankride_guy6_jumptocombatrun', o, yaw)))
    return seats
# halftrack: benches z 40 at |y| 10..24, x -100..-12; aisle floor z 30; rear doors at x -104..-112.
HT_SILL = -110.0         # where the floor ends at the open rear doors
HT_GUY02_SILL = 90.0     # guy02_jumpout steps off its floor 90 units ahead of its tag (feet, frames 28-30)
HT_FLOOR = 30.0
def ht_seats():
    seats = []
    for k, x in enumerate((-88, -66, -44, -22)):
        for side, sy, yaw, stand, f0, f1 in (('left', 1, -90, 'halftrack_guy03_jumpout', 35, 85),
                                              ('right', -1, 90, 'halftrack_guy05_jumpout', 44, 66)):
            o = (x, sy * 17, 42)
            tag = (HT_SILL + HT_GUY02_SILL, sy * 3, HT_FLOOR + 1.5)   # each side's file keeps to its own half of the doorway
            start = first_frame_ahead('halftrack_guy02_jumpout', tag[0] - x)
            seats.append(('%s %d' % (side, 4 - k), o, yaw, 'halftrack_guy0%d_idle' % (3 if sy > 0 else 5),
                          [(stand, f0, f1, o, yaw), ('halftrack_guy02_jumpout', start, None, tag, 180)]))
    return seats

def G(p): return np.array([-p[1], p[0], p[2]])     # SMD frame -> the tag's frame (+x = his forward)
_cache = {}
def anim(name):
    if name not in _cache:
        s = SMD(os.path.join(PACK, 'ww2_a_' + name + '.smd'))
        bones = [i for i, (n, p) in enumerate(s.nodes) if n.startswith('ValveBiped.Bip01') and p >= 0]
        _cache[name] = (s, bones, [{i: G(t) for i, (R, t) in world(s, fr).items()} for fr in range(len(s.frames))])
    return _cache[name]
def bone(name, frame, b): s, _, F = anim(name); return F[frame][s.index('ValveBiped.Bip01_' + b)]
def feet(name, frame): return min(bone(name, frame, 'L_Foot')[2], bone(name, frame, 'R_Foot')[2])
def nframes(name): return len(anim(name)[2])
def first_frame_ahead(name, d):
    return next(f for f in range(nframes(name)) if bone(name, f, 'Pelvis')[0] >= d)

def measure_landing(name, f0):
    """takeoff = last frame (from f0) whose feet are still within 6 of their height at f0; landing = first frame
    after which the feet stay within 3 of their final height. landing None = it ends still falling."""
    n = nframes(name); f = [feet(name, i) for i in range(n)]
    take = f0
    while take + 1 < n and f[take + 1] > f[f0] - 6: take += 1
    land = next((i for i in range(take, n) if all(abs(x - f[-1]) < 3 for x in f[i:])), None)
    if land is not None and land >= n - 2 and f[-2] - f[-1] > 2: land = None
    return take, land, f[-1]

def place(p, origin, yaw):
    c, s_ = np.cos(np.radians(yaw)), np.sin(np.radians(yaw))
    return np.array([origin[0] + c * p[0] - s_ * p[1], origin[1] + s_ * p[0] + c * p[1], origin[2] + p[2]])

class Getout:
    """the get-out as the code will play it: segments back to back, the last one's landing corrected to the ground"""
    def __init__(self, segs, ground=0.0):
        self.segs = []
        for k, (a, f0, f1, org, yaw) in enumerate(segs):
            f1 = nframes(a) - 1 if f1 is None else f1
            self.segs.append((a, f0, f1, org, yaw))
        a, f0, f1, org, yaw = self.segs[-1]
        self.take, self.land, end = measure_landing(a, f0)
        self.total = 0.0 if self.land is None else (ground + FOOT_H) - (org[2] + end)
    def frames(self):
        return [(k, f) for k, (a, f0, f1, o, y) in enumerate(self.segs) for f in range(f0, f1 + 1)]
    def dz(self, k, f):
        if k != len(self.segs) - 1 or self.land is None or f <= self.take: return 0.0
        v = min((f - self.take) / max(self.land - self.take, 1), 1.0); return self.total * v * v
    def pose(self, k, f):
        a, f0, f1, o, y = self.segs[k]; return pose_points(a, f, o, y, self.dz(k, f))
    def pelvis(self, k, f):
        a, f0, f1, o, y = self.segs[k]; return place(bone(a, f, 'Pelvis'), o, y) + [0, 0, self.dz(k, f)]
    def describe(self):
        a, f0, f1, o, y = self.segs[-1]
        if self.land is None: return 'still falling when the animation ends - gravity finishes it'
        return 'landing corrected %+.1f units (frames %d..%d of %s)' % (self.total, self.take, self.land, a)

def pose_points(name, frame, origin, yaw, dz):
    s, bones, F = anim(name); fr = F[frame]
    return [(place(fr[s.nodes[i][1]], origin, yaw) + [0, 0, dz], place(fr[i], origin, yaw) + [0, 0, dz])
            for i in bones if s.nodes[i][1] in fr and i in fr]

class Surface:
    def __init__(self, T): self.H, self.lo, _ = heightmap(T, 1.0)
    def top(self, x, y):
        i, j = int(y - self.lo[1]), int(x - self.lo[0])
        if 0 <= i < self.H.shape[0] and 0 <= j < self.H.shape[1] and np.isfinite(self.H[i, j]): return self.H[i, j]
        return None
    def depth(self, segs):
        worst = 0.0
        for a, b in segs:
            for t in np.linspace(0, 1, 5):
                q = a + (b - a) * t; h = self.top(q[0], q[1])
                if q[2] < 5: continue          # at ground level: under an overhang, not inside
                if h is not None and h - q[2] > worst: worst = h - q[2]
        return worst

def draw_vehicle(ax, T, a, b):
    ax.add_collection(PolyCollection(T[:, :, [a, b]], facecolor='#d4ccb4', edgecolor='#bfb79f', linewidths=.05))
def draw_segs(ax, segs, a, b, col='k', lw=1.6):
    for p, q in segs: ax.plot([p[a], q[a]], [p[b], q[b]], '-', color=col, lw=lw, solid_capstyle='round')


RADIUS = (('Head', 4.5), ('Neck', 2.5), ('Spine', 5.5), ('Pelvis', 5.5), ('Thigh', 3.5), ('Calf', 2.8), ('Foot', 2.2),
          ('Toe', 1.8), ('Clavicle', 2.2), ('UpperArm', 2.3), ('Forearm', 2.0), ('Hand', 1.8), ('Finger', 0.9))
def limb_radius(bone_name):
    return next((r for k, r in RADIUS if k in bone_name), 1.5)
def figure_at(name, frame, origin, yaw, dz):
    s, bones, F = anim(name); fr = F[frame]
    return [(place(fr[s.nodes[i][1]], origin, yaw) + [0, 0, dz], place(fr[i], origin, yaw) + [0, 0, dz], limb_radius(s.nodes[i][0]))
            for i in bones if s.nodes[i][1] in fr and i in fr and 'Finger' not in s.nodes[i][0]]
BLEND = 0.2   # the sequences' fadein: the engine blends the previous pose into a new sequence over this long

class Rider:
    """one rider on the code's timeline: idle (looping) until 'start', then each get-out segment in turn at 30 fps,
    every change of sequence blended over BLEND seconds; after the last frame he holds it (the AI takes over)."""
    def __init__(self, seat, start):
        self.name, self.org, self.yaw, self.idle, segs = seat
        self.g = Getout(segs); self.start = start
        self.t = [start]
        for a, f0, f1, o, y in self.g.segs: self.t.append(self.t[-1] + (f1 - f0) / 30.0)
    def end(self): return self.t[-1]
    def raw(self, t):
        if t < self.start:
            n = nframes(self.idle); return figure_at(self.idle, int(t * 30) % n, self.org, self.yaw, 0)
        for k, (a, f0, f1, o, y) in enumerate(self.g.segs):
            if t <= self.t[k + 1] or k == len(self.g.segs) - 1:
                f = min(f0 + int(round((t - self.t[k]) * 30)), f1)
                return figure_at(a, f, o, y, self.g.dz(k, f))
    def pose(self, t):
        cur = self.raw(t)
        for tk in self.t[:-1]:
            if tk <= t < tk + BLEND:
                prev = self.raw(tk - 1e-3); w = (t - tk) / BLEND; w = w * w * (3 - 2 * w)
                return [(pa + (ca - pa) * w, pb + (cb - pb) * w, r) for (pa, pb, r), (ca, cb, _) in zip(prev, cur)]
        return cur

COLOURS = [(92, 98, 78), (110, 104, 82), (84, 90, 96), (120, 92, 72), (78, 100, 92), (104, 96, 110), (96, 86, 64), (70, 84, 70)]
def make_gif(vname, T, seats, starts, cam_args, path, caption, hold=1.0, tail=1.2):
    cam = Camera(*cam_args)
    Z, C = background(cam, T)
    riders = [Rider(seat, hold + st) for seat, st in zip(seats, starts)]
    end = max(r.end() for r in riders) + tail
    frames = []
    for i in range(int(end * 15) + 1):
        t = i / 15.0
        img = Image.fromarray(splat_figures(cam, Z, C, [(r.pose(t), COLOURS[k % len(COLOURS)]) for k, r in enumerate(riders)]))
        d = ImageDraw.Draw(img); d.text((8, 6), '%s   t = %.2f s' % (caption, t), fill=(20, 20, 20))
        frames.append(img)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=67, loop=0, optimize=True)
    return end

def tank_starts(seats): return [0.0, 0.25, 0.1, 0.35][:len(seats)]
def ht_starts(seats):
    # rearmost first, sides alternating, 0.45 s apart (seats are listed rear to front, left then right)
    return [0.45 * k for k in range(len(seats))]
def ht_doors_open(T):
    c = T.mean(1); keep = ~((c[:, 0] < -103) & (np.abs(c[:, 1]) < 23) & (c[:, 2] > 26) & (c[:, 2] < 80))
    return T[keep]
# carrier: riders sit on the side walls (top z 37) beside the rear wells, facing out, and vault over. Only
# back02's pair fits our carrier; back01's climb-out goes through its rear deck wherever it sits (searched).
def carrier_seats():
    seats = []
    for sn, o, yaw in (('left', (-10, 34, 42), 90), ('right', (-28, -34, 42), -90)):
        seats.append(('rear ' + sn, o, yaw, 'bren_carrier_back02_rideidle', out1('bren_carrier_back02_ridedismount', o, yaw)))
    return seats
VEHICLES = [
    dict(name='carrier', tur=None, seats=carrier_seats(), view='rear',
         starts=lambda s: [0.0, 0.3], cam=(215, 32, (-20, 0, 30), 2.6, 720, 460)),
    dict(name='panzer2', tur=(20, 0, 97), seats=tank_seats(-50, -90, 50, 68, 68), view='rear', starts=tank_starts,
         cam=(215, 28, (-60, 0, 50), 2.2, 720, 440)),
    dict(name='sherman', tur=(15, 0, 116), seats=tank_seats(-62, -100, 46, 69, 65), view='rear', starts=tank_starts,
         cam=(215, 28, (-60, 0, 55), 2.0, 720, 440)),
    dict(name='tiger', tur=(11, 0, 108), seats=tank_seats(-45, -95, 60, 75, 76), view='rear', starts=tank_starts,
         cam=(215, 28, (-50, 0, 55), 1.9, 720, 440)),
    dict(name='halftrack', tur=None, seats=ht_seats(), view='side', starts=ht_starts, edit=ht_doors_open,
         cam=(205, 38, (-90, 0, 35), 2.3, 720, 460), note='rear doors drawn OPEN'),
]
if len(sys.argv) > 5: VEHICLES = [v for v in VEHICLES if v['name'] in sys.argv[5:]]
VIEWS = {'rear': (1, 2, 'seen from behind (the vehicle\'s left = right)'), 'side': (0, 2, 'seen from its right side, cut open (front = right)')}

report = []
for veh in VEHICLES:
    vname, tur, seats, view = veh['name'], veh['tur'], veh['seats'], veh['view']
    T = tris(V + '/' + vname + ('_hull.mdl' if tur else '.mdl'))
    if tur: T = np.concatenate([T, tris(V + '/' + vname + '_turret.mdl') + np.array(tur)])
    if 'edit' in veh: T = veh['edit'](T)
    end = make_gif(vname, T, seats, veh['starts'](seats), veh['cam'], '%s/%s_getout.gif' % (OUT, vname),
                   '%s: %d riders get out%s' % (vname, len(seats), (' - ' + veh['note']) if 'note' in veh else ''))
    report.append('%-9s all out after %.1f s' % (vname, end - 1.2))
    surf = Surface(T)
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    va, vb, vtitle = VIEWS[view]
    Tview = T if view == 'rear' else T[T.mean(1)[:, 1] < -24]       # side view: the far (left) half only
    fig, axs = plt.subplots(1, 2, figsize=(26, 9), gridspec_kw={'width_ratios': [2.2, 1]})
    draw_vehicle(axs[0], T, 0, 1); draw_vehicle(axs[1], T, 1, 2)
    for k, (sn, org, yaw, idle, segs) in enumerate(seats):
        col = 'C%d' % (k % 10); g = Getout(segs)
        sp = pose_points(idle, 0, org, yaw, 0)
        draw_segs(axs[0], sp, 0, 1, col); draw_segs(axs[1], sp, 1, 2, col)
        path = np.array([g.pelvis(kk, f) for kk, f in g.frames()])
        axs[0].plot(path[:, 0], path[:, 1], ':', color=col); axs[0].plot(path[-1, 0], path[-1, 1], 'o', color=col)
        axs[0].text(org[0], org[1] + (6 if org[1] > 0 else -9), sn, color=col, fontsize=9, ha='center')
        d_idle = surf.depth(sp)
        d_out = max(surf.depth(g.pose(kk, f)) for kk, f in g.frames())
        report.append('%-9s %-12s seat %4d %4d %3d yaw %4d  under the surface: seated %4.1f, getting out %4.1f;  %s'
                      % (vname, sn, *org, yaw, d_idle, d_out, g.describe()))
    for ax in axs: ax.set_aspect('equal'); ax.grid(alpha=.3)
    axs[0].set_xlim(lo[0] - 20, hi[0] + 20); axs[0].set_ylim(-140, 140)
    axs[1].set_xlim(-140, 140); axs[1].set_ylim(-5, max(hi[2], 100) + 10); axs[1].axhline(0, color='g')
    axs[0].set_title('%s from above (front = right): riders seated, dotted = their way out' % vname, fontsize=13)
    axs[1].set_title('from behind', fontsize=13)
    plt.tight_layout(); plt.savefig('%s/%s_seats.png' % (OUT, vname), dpi=55); plt.close()
    # each seat's get-out, 8 moments, in the vehicle's view
    fig, axs = plt.subplots(len(seats), 8, figsize=(32, 4.0 * len(seats)))
    for row, (sn, org, yaw, idle, segs) in zip(axs, seats):
        g = Getout(segs); fl = g.frames(); n = len(fl)
        picks = sorted(set([0] + [int(round((n - 1) * t)) for t in (.15, .3, .45, .6, .72, .86)] + [n - 1]))
        while len(picks) < 8: picks.append(n - 1)
        for ax, i in zip(row, picks[:8]):
            kk, f = fl[i]
            draw_vehicle(ax, Tview, va, vb)
            draw_segs(ax, g.pose(kk, f), va, vb)
            ax.axhline(0, color='g'); ax.set_aspect('equal')
            if view == 'rear': ax.set_xlim(-150, 150)
            else: ax.set_xlim(-200, 80)
            ax.set_ylim(-5, 140)
            ax.set_title('%s  %.2f s  %s f%d%s' % (sn, i / 30.0, g.segs[kk][0].split('_', 1)[1][:22], f,
                         ' takeoff' if kk == len(g.segs) - 1 and f == g.take else ' landed' if kk == len(g.segs) - 1 and f == g.land else ''), fontsize=9)
            ax.tick_params(labelsize=6)
    fig.suptitle('%s: riders getting out, %s (30 fps)' % (vname, vtitle), fontsize=15)
    plt.tight_layout(); plt.savefig('%s/%s_getout.png' % (OUT, vname), dpi=50); plt.close()
open(OUT + '/report.txt', 'w').write('\n'.join(report) + '\n')
print('\n'.join(report))
