"""V3 rider preview: every seat's ride idle and get-out drawn against the vehicle's real mesh.

    python3 rider_preview.py <dodsp_tools dir> <vehicle model dir> <ride pack dir> <out dir> [vehicle type ...]

Reads what the GAME reads (one owner each): the seats from content/scripts/dodsp_vehicles.txt, the get-out paths
from content/scripts/dodsp_ride_paths.txt, the in-place get-out animations from the ride pack built with
build_ww2_pack.py --ride-paths. Positions follow the game code (npc_german_soldier.cpp RidePosition, change both
together): seated, his origin is the seat; getting out, origin = seat + the path's O(f) turned by the seat's yaw +
(real ground under the landing point - the animation's ground) x s(f).
Checks printed per seat: the deepest any body point goes under the vehicle's top surface (a leg through the deck;
the heightmap sees surfaces from above only: a hand on a thin wall counts too), seated and getting out, what is
under him when seated (nothing = floating), and the landing correction.
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

TOOLS, V, PACK, OUT = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
sys.path.insert(0, os.path.join(TOOLS, 'vehicle_preview'))
from editor_models import types_from_script, kv_parse
FOOT_H = 4.5          # foot bone above the sole (DoD reference skeleton, standing)
os.makedirs(OUT, exist_ok=True)

# sequence name -> its animation file (the pack's SEQUENCES.txt), and the get-out paths
SEQ_ANIM = {}
for line in open(os.path.join(PACK, 'SEQUENCES.txt')).read().splitlines()[1:]:
    c = line.split('\t')
    if len(c) >= 3: SEQ_ANIM[c[0]] = c[2][2:]          # 'a_tankride_guy1_leanleft' -> 'tankride_guy1_leanleft'
PATHS = {}
for seq, blk in dict(kv_parse(open(os.path.join(TOOLS, 'content/scripts/dodsp_ride_paths.txt')).read()))['RidePaths']:
    if not isinstance(blk, list): continue
    d = dict(blk)
    PATHS[seq] = dict(frames=int(d['frames']), take=int(d['take']), land=int(d['land']),
                      path=[[float(x) for x in d[str(i)].split()] for i in range(int(d['frames']))])
TYPES = types_from_script(os.path.join(TOOLS, 'content/scripts/dodsp_vehicles.txt'))
def seats_of(tname):
    out = []
    for k, (key, blk) in enumerate(TYPES[tname].get('seats', [])):
        d = dict(blk); o = tuple(float(x) for x in d['origin'].split()); yaw = float(d['yaw'])
        name = '%s %s' % ('front' if k % 2 == 0 else 'rear', 'left' if o[1] > 0 else 'right') if len(TYPES[tname]['seats']) == 4 else \
               ('left' if o[1] > 0 else 'right')
        out.append((name, o, yaw, SEQ_ANIM[d['idle']], d['getout'], float(d.get('delay', 0))))
    return out

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
def place(p, origin, yaw):
    c, s_ = np.cos(np.radians(yaw)), np.sin(np.radians(yaw))
    return np.array([origin[0] + c * p[0] - s_ * p[1], origin[1] + s_ * p[0] + c * p[1], origin[2] + p[2]])

class Getout:
    """the get-out as the code plays it: the in-place animation on an origin that follows the path table"""
    def __init__(self, seq, org, yaw, ground=0.0):
        self.seq, self.anim, self.org, self.yaw = seq, SEQ_ANIM[seq], org, yaw
        self.p = PATHS[seq]
        if self.p['frames'] != nframes(self.anim):
            raise SystemExit('%s: the path table has %d frames, the pack %d - rebuild both in one run' % (seq, self.p['frames'], nframes(self.anim)))
        end = self.p['path'][-1]
        self.total = ground - (org[2] + end[2])          # what the code adds by the end (s = 1)
        self.take, self.land = self.p['take'], self.p['land']
    def frames(self): return [(0, f) for f in range(self.p['frames'])]
    def origin(self, f):
        x, y, z, sv = self.p['path'][f]
        return place((x, y, z + self.total * sv), self.org, self.yaw)
    def pose(self, k, f): return pose_points(self.anim, f, self.origin(f), self.yaw, 0.0)
    def pelvis(self, k, f): return place(bone(self.anim, f, 'Pelvis'), self.origin(f), self.yaw)
    def describe(self):
        return 'lands %+.1f units off the animation\'s own drop (frames %d..%d of %s)' % (self.total, self.take, self.land, self.seq)

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
    """one rider on the code's timeline: idle (looping) until 'start', then the get-out at 30 fps (the code sets the
    frame from its own clock), the change of sequence blended over BLEND seconds; after the last frame he holds it."""
    def __init__(self, seat, start):
        self.name, self.org, self.yaw, self.idle, seq, _ = seat
        self.g = Getout(seq, self.org, self.yaw); self.start = start
        self.t = [start, start + (self.g.p['frames'] - 1) / 30.0]
    def end(self): return self.t[-1]
    def raw(self, t):
        if t < self.start:
            n = nframes(self.idle); return figure_at(self.idle, int(t * 30) % n, self.org, self.yaw, 0)
        f = min(int(round((t - self.start) * 30)), self.g.p['frames'] - 1)
        return figure_at(self.g.anim, f, self.g.origin(f), self.yaw, 0)
    def pose(self, t):
        cur = self.raw(t)
        if self.start <= t < self.start + BLEND:
            prev = self.raw(self.start - 1e-3); w = (t - self.start) / BLEND; w = w * w * (3 - 2 * w)
            return [(pa + (ca - pa) * w, pb + (cb - pb) * w, r) for (pa, pb, r), (ca, cb, _) in zip(prev, cur)]
        return cur

COLOURS = [(92, 98, 78), (110, 104, 82), (84, 90, 96), (120, 92, 72), (78, 100, 92), (104, 96, 110), (96, 86, 64), (70, 84, 70)]
def make_gif(vname, T, seats, cam_args, path, caption, hold=1.0, tail=1.2):
    cam = Camera(*cam_args)
    Z, C = background(cam, T)
    riders = [Rider(seat, hold + seat[5]) for seat in seats]
    end = max(r.end() for r in riders) + tail
    frames = []
    for i in range(int(end * 15) + 1):
        t = i / 15.0
        img = Image.fromarray(splat_figures(cam, Z, C, [(r.pose(t), COLOURS[k % len(COLOURS)]) for k, r in enumerate(riders)]))
        d = ImageDraw.Draw(img); d.text((8, 6), '%s   t = %.2f s' % (caption, t), fill=(20, 20, 20))
        frames.append(img)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=67, loop=0, optimize=True)
    return end

# cameras (az, elevation, centre, pixels per unit, width, height) - everything else comes from the script
CAMERAS = {'panzer2': (215, 28, (-60, 0, 50), 2.2, 720, 440), 'sherman': (215, 28, (-60, 0, 55), 2.0, 720, 440),
           'tiger': (215, 28, (-50, 0, 55), 1.9, 720, 440), 'carrier': (215, 32, (-20, 0, 30), 2.6, 720, 460)}
VEHICLES = [t for t in TYPES if TYPES[t].get('seats') and t in CAMERAS]
if len(sys.argv) > 5: VEHICLES = [v for v in VEHICLES if v in sys.argv[5:]]
def mesh_of(t):
    d = TYPES[t]; T = tris(os.path.join(V, os.path.basename(d['model'])))
    if d.get('turret_model'):
        T = np.concatenate([T, tris(os.path.join(V, os.path.basename(d['turret_model']))) + np.array([float(x) for x in d['turret_offset'].split()])])
    return T

report = []
for vname in VEHICLES:
    seats = seats_of(vname); T = mesh_of(vname)
    end = make_gif(vname, T, seats, CAMERAS[vname], '%s/%s_getout.gif' % (OUT, vname), '%s: %d riders get out' % (vname, len(seats)))
    report.append('%-9s all out after %.1f s' % (vname, end - 1.0 - 1.2))
    surf = Surface(T)
    lo, hi = T.reshape(-1, 3).min(0), T.reshape(-1, 3).max(0)
    fig, axs = plt.subplots(1, 2, figsize=(26, 9), gridspec_kw={'width_ratios': [2.2, 1]})
    draw_vehicle(axs[0], T, 0, 1); draw_vehicle(axs[1], T, 1, 2)
    for k, (sn, org, yaw, idle, seq, delay) in enumerate(seats):
        col = 'C%d' % (k % 10); g = Getout(seq, org, yaw)
        sp = pose_points(idle, 0, org, yaw, 0)
        draw_segs(axs[0], sp, 0, 1, col); draw_segs(axs[1], sp, 1, 2, col)
        path = np.array([g.pelvis(kk, f) for kk, f in g.frames()])
        axs[0].plot(path[:, 0], path[:, 1], ':', color=col); axs[0].plot(path[-1, 0], path[-1, 1], 'o', color=col)
        axs[0].text(org[0], org[1] + (6 if org[1] > 0 else -9), sn, color=col, fontsize=9, ha='center')
        d_idle = surf.depth(sp)
        d_out = max(surf.depth(g.pose(kk, f)) for kk, f in g.frames())
        under = surf.top(org[0], org[1])
        report.append('%-9s %-12s seat %4d %4d %3d yaw %4d  under the surface: seated %4.1f, getting out %4.1f;  seat %s;  %s'
                      % (vname, sn, *org, yaw, d_idle, d_out,
                         'on a surface %.1f below' % (org[2] - under) if under is not None else 'OVER NOTHING', g.describe()))
    for ax in axs: ax.set_aspect('equal'); ax.grid(alpha=.3)
    axs[0].set_xlim(lo[0] - 20, hi[0] + 20); axs[0].set_ylim(-140, 140)
    axs[1].set_xlim(-140, 140); axs[1].set_ylim(-5, max(hi[2], 100) + 10); axs[1].axhline(0, color='g')
    axs[0].set_title('%s from above (front = right): riders seated, dotted = their way out' % vname, fontsize=13)
    axs[1].set_title('from behind', fontsize=13)
    plt.tight_layout(); plt.savefig('%s/%s_seats.png' % (OUT, vname), dpi=55); plt.close()
open(OUT + '/report.txt', 'w').write('\n'.join(report) + '\n')
print('\n'.join(report))
