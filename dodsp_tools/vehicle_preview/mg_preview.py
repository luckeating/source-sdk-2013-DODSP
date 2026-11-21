"""V2b preview: the halftrack's front MG42 and its gunner - the gun sweeping its arc with the bay empty and with six
riders aboard, and the gunner shot at the gun.

    python3 mg_preview.py <dodsp_tools dir> <rig out dir> <ride pack dir> <gunner poses dir> <MG42 model dir> <CoD2 pack> <DoD decompile dir> <out dir> [norender]

What it draws is what the game will do (change both together):
  * the vehicle = the rigged halftrack (vehicle_rig.py output, doors shut); the crew and riders on their script seats
    (content/scripts/dodsp_vehicles.txt: one owner), each in his idle;
  * the gun = dodsp_mg42_pintle (mg42_model.py): its origin on CoD2's tag_turret, the yoke turned by the yaw, the gun
    by mg_pitch too;
  * the gunner = the sequence ww2_halftrack_mg (mg_gunner_compose.py's poses) blended the ENGINE's way: per bone,
    the local rotations blended by normalised quaternion lerp (bone_setup.cpp BlendBones), first along mg_yaw then
    mg_pitch, positions lerped - so the hands' drift off the grips BETWEEN the composed poses is measured, not assumed;
  * his death = ww2_halftrack_mg_death blended by mg_yaw, the switch from the aim blended over the sequences' fadein
    (0.2 s) like the engine's sequence transition.
Checks printed: hands off the grips over the whole arc (2-degree grid), the closest the gunner comes to a seated rider
(bones + thickness) at each arc, and the dead gunner's overlap with seated riders (the owner accepted clipping)."""
import os, sys, re, numpy as np
from PIL import Image, ImageDraw
TOOLS, RIG, PACK, POSES, GUN, COD2, DOD, OUT = sys.argv[1:9]
sys.path.insert(0, TOOLS); sys.path.insert(0, os.path.join(TOOLS, 'vehicle_preview'))
from smd import SMD
from render3d import Camera, _raster, splat_figures
from track_preview import read_rig
from editor_models import types_from_script
from xanim_to_smd import rotm
import mg_gunner_compose as MGC
from scipy.spatial.transform import Rotation
os.makedirs(OUT, exist_ok=True)

FPS, BLEND = 30.0, 0.2
VB = MGC.VB
ARC_EMPTY, ARC_RIDERS = 30, 20          # mg_yaw limits (the server's: bay empty / riders aboard)

# ---- the script's seats (one owner) --------------------------------------------------------------------------
TYPE = next(t for t in types_from_script(os.path.join(TOOLS, 'content/scripts/dodsp_vehicles.txt')).values()
            if os.path.basename(t.get('model', '')) == 'halftrack.mdl')
def seat_list(key):
    out = []
    for role, blk in TYPE.get(key, []):
        d = dict(blk); out.append((role, np.array([float(v) for v in d['origin'].split()]), float(d.get('yaw', 0)),
                                   re.sub(r'^ww2_(vi_)?', '', d['idle'])))
    return out
CREW = [s for s in seat_list('crew_seats') if s[0] != 'gunner']
RIDERS = seat_list('seats')

# ---- skeletons -----------------------------------------------------------------------------------------------
def G(p): return np.array([-p[1], p[0], p[2]])
_smd = {}
def smd(path):
    if path not in _smd: _smd[path] = SMD(path)
    return _smd[path]
def pack_smd(name):
    path = os.path.join(PACK, 'ww2_a_%s.smd' % name)       # a blend sequence (the driver's steering): its middle = the idle
    return smd(path if os.path.exists(path) else os.path.join(PACK, 'ww2_a_%s_idle.smd' % name))
def local(s, f):
    """per bone: (quaternion xyzw, position) in the SMD's own frame"""
    fr = s.frames[f][1]
    return {i: (Rotation.from_matrix(rotm(v[3:])).as_quat(), np.array(v[:3])) for i, v in fr.items()}
def nlerp(a, b, w):
    """Source's QuaternionBlend: the shorter way, lerped and normalised"""
    if np.dot(a, b) < 0: b = -b
    q = a * (1 - w) + b * w; return q / np.linalg.norm(q)
def blend(A, B, w): return {i: (nlerp(A[i][0], B[i][0], w), A[i][1] * (1 - w) + B[i][1] * w) for i in A if i in B}
def world(s, L):
    W = {}
    for i, (n, p) in enumerate(s.nodes):
        if i not in L: continue
        R = Rotation.from_quat(L[i][0]).as_matrix(); t = L[i][1]
        W[i] = (R, t) if p < 0 or p not in W else (W[p][0] @ R, W[p][0] @ t + W[p][1])
    return {i: (MGC.Gm @ R, MGC.Gm @ t) for i, (R, t) in W.items()}
def figure(s, W, origin, yaw=0.0):
    c, si = np.cos(np.radians(yaw)), np.sin(np.radians(yaw)); R = np.array([[c, -si, 0], [si, c, 0], [0, 0, 1]])
    segs = []
    for i, (n, p) in enumerate(s.nodes):
        if not n.startswith('ValveBiped.Bip01') or 'Finger' in n or i not in W: continue
        a = W[p][1] if p >= 0 and p in W else W[i][1]
        segs.append((R @ a + origin, R @ W[i][1] + origin, MGC.radius(n)))
    return segs

# ---- the gunner's two sequences (the engine's blends) ---------------------------------------------------------
comp = MGC.main(PACK, COD2, os.path.join(TOOLS, 'generated/cod2_vehicle_tags.txt'), DOD, POSES)
YAWS, ROWS, STAND, MOUNT, HOLD = MGC.YAWS, comp['rows'], comp['stand'], comp['mount'], comp['hold_t']
IDLE = smd(os.path.join(POSES, 'ww2_a_%s.smd' % comp['poses'][(0, comp['rows'][0])]))     # his skeleton (the pack's)
AIM = {(y, p): local(smd(os.path.join(POSES, 'ww2_a_%s.smd' % comp['poses'][(y, p)])), 0) for y in YAWS for p in ROWS}
DEATH = {y: smd(os.path.join(POSES, 'ww2_a_%s.smd' % comp['deaths'][y])) for y in YAWS}
def cell(v, keys):
    v = min(max(v, keys[0]), keys[-1]); k = max(i for i in range(len(keys) - 1) if keys[i] <= v) if v < keys[-1] else len(keys) - 2
    return k, (v - keys[k]) / float(keys[k + 1] - keys[k])
def aim_local(y, p):
    i, wy = cell(y, YAWS); j, wp = cell(p, list(ROWS))
    lo = blend(AIM[(YAWS[i], ROWS[j])], AIM[(YAWS[i + 1], ROWS[j])], wy)
    hi = blend(AIM[(YAWS[i], ROWS[j + 1])], AIM[(YAWS[i + 1], ROWS[j + 1])], wy)
    return blend(lo, hi, wp)
def death_local(y, f):
    i, wy = cell(y, YAWS); f = min(int(f), len(DEATH[YAWS[0]].frames) - 1)
    return blend(local(DEATH[YAWS[i]], f), local(DEATH[YAWS[i + 1]], f), wy)
ix = lambda n: IDLE.index(VB + n)
def grip_error(y, p):
    W = world(IDLE, aim_local(y, p)); G_ = MGC.gun_rot(y, p)
    return max(np.linalg.norm(W[ix(side + '_Hand')][1] + STAND - (G_ @ HOLD[k] + MOUNT)) for k, side in enumerate('RL'))

# ---- the gun -------------------------------------------------------------------------------------------------
def gun_mesh():
    L = open(os.path.join(GUN, 'dodsp_mg42_pintle_ref.smd')).read().split('\n'); i = L.index('triangles') + 1; T, B = [], []
    while i < len(L) and L[i] != 'end':
        vs = [L[i + 1 + k].split() for k in range(3)]
        T.append([G(np.array(list(map(float, v[1:4])))) for v in vs]); B.append(int(vs[0][0])); i += 4
    return np.array(T), np.array(B)
GUN_T, GUN_B = gun_mesh()
def gun_tris(y, p):
    Ry_, R = MGC.Rz(y), MGC.gun_rot(y, p)
    return np.array([[(R if b else Ry_) @ q + MOUNT for q in t] for t, b in zip(GUN_T, GUN_B)])

# ---- the vehicle ---------------------------------------------------------------------------------------------
names, parent, rest, tris, susp, wheels = read_rig(RIG, 'halftrack')
HULL = np.array([[p for p, w in vs] for m, vs in tris])
def scene(cam, y, p):
    Z = np.full((cam.h, cam.w), -1e9); C = np.zeros((cam.h, cam.w, 3)); C[:] = (235, 238, 242)
    T = np.concatenate([HULL, gun_tris(y, p)])
    col = np.array([(212, 204, 180)] * len(HULL) + [(70, 72, 76)] * len(GUN_T), float)
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    light = np.array([0.35, 0.55, 0.76]); light /= np.linalg.norm(light)
    _raster(cam, T, (col * (0.45 + 0.55 * np.abs(n @ light))[:, None]).clip(0, 255), Z, C)
    return Z, C

SEATED = {}
def seated(role, origin, yaw, idle):
    if (role, idle) not in SEATED:
        s = pack_smd(idle); SEATED[(role, idle)] = figure(s, world(s, local(s, 0)), origin, yaw)
    return SEATED[(role, idle)]
def others(riders): return [seated(*c) for c in CREW] + ([seated(*r) for r in RIDERS] if riders else [])
def closest(gun_segs, segs_list):
    """the closest two bodies come: bone axes, less both thicknesses (< 0 = overlap)"""
    A = np.array([b for a, b, r in gun_segs]); Ar = np.array([r for a, b, r in gun_segs]); best = 1e9
    for segs in segs_list:
        Bp = np.array([b for a, b, r in segs]); Br = np.array([r for a, b, r in segs])
        d = np.linalg.norm(A[:, None] - Bp[None], axis=2) - Ar[:, None] - Br[None]; best = min(best, d.min())
    return best

# ---- checks --------------------------------------------------------------------------------------------------
grid = [(y, p) for y in np.arange(-30, 30.01, 2.0) for p in np.arange(ROWS[0], ROWS[-1] + 0.01, 1.0)]
ge = max((grip_error(y, p), y, p) for y, p in grid)
print('hands off the grips over the arc (engine blend, every 2 deg yaw / 1 deg pitch): worst %.2f units at yaw %+g pitch %+g' % ge)
for arc in (ARC_EMPTY, ARC_RIDERS):
    worst = min((closest(figure(IDLE, world(IDLE, aim_local(y, p)), STAND), [seated(*r) for r in RIDERS]), y, p)
                for y, p in grid if abs(y) <= arc + 1e-6)
    print('gunner vs seated riders, arc +-%d: closest %.1f units (< 0 = overlap) at yaw %+g pitch %+g' % ((arc,) + worst))
for y in (ARC_EMPTY, 0, -ARC_EMPTY):
    ov = min(closest(figure(IDLE, world(IDLE, death_local(y, f)), STAND), [seated(*r) for r in RIDERS])
             for f in range(len(DEATH[0].frames)))
    print('dead gunner (shot at yaw %+d) vs seated riders: closest %.1f units (< 0 = overlap, accepted)' % (y, ov))

# ---- render --------------------------------------------------------------------------------------------------
def gunner_at(t, track):
    """track(t) -> (yaw, pitch, death time or None)"""
    y, p, td = track(t)
    if td is None or t < td: return y, p, figure(IDLE, world(IDLE, aim_local(y, p)), STAND)
    f = (t - td) * FPS; L = death_local(y, f); w = min((t - td) / BLEND, 1.0)
    if w < 1: L = blend(aim_local(y, p), L, w)
    return y, p, figure(IDLE, world(IDLE, L), STAND)
def gif(track, riders, cams, path, caption, t_end):
    frames = []
    for i in range(int(t_end * 15) + 1):
        t = i / 15.0; y, p, segs = gunner_at(t, track); row = []
        for cam in cams:
            Z, C = scene(cam, y, p)
            figs = [(segs, (150, 70, 60))] + [(s, (92, 98, 78)) for s in others(riders)]
            row.append(splat_figures(cam, Z, C, figs))
        im = Image.fromarray(np.concatenate(row, axis=1)); d = ImageDraw.Draw(im)
        d.text((8, 6), '%s   t = %.2f s   mg_yaw %+.0f  mg_pitch %+.0f' % (caption, t, y, p), fill=(20, 20, 20))
        frames.append(im)
    frames[0].save(path, save_all=True, append_images=frames[1:], duration=67, loop=0, optimize=True)
def sweep(arc, T=6.0):
    mid, half = (ROWS[0] + ROWS[-1]) / 2.0, (ROWS[-1] - ROWS[0]) / 2.0
    return lambda t: (arc * np.sin(2 * np.pi * t / T), mid + half * np.sin(2 * np.pi * t / (T / 2.0) + 0.6), None)
cams = [Camera(200, 32, (-10, 0, 60), 2.4, 620, 460), Camera(180, 89, (-20, 0, 0), 2.0, 380, 460)]
if 'norender' not in sys.argv:
    gif(sweep(ARC_EMPTY), False, cams, os.path.join(OUT, 'v2b_mg_sweep_empty.gif'), 'bay empty: arc +-%d' % ARC_EMPTY, 6.0)
    gif(sweep(ARC_RIDERS), True, cams, os.path.join(OUT, 'v2b_mg_sweep_riders.gif'), 'six riders aboard: arc +-%d' % ARC_RIDERS, 6.0)
    def shot(t):
        y = min(t / 0.8, 1.0) * ARC_EMPTY; return y, 4.0, 1.0
    gif(shot, False, cams, os.path.join(OUT, 'v2b_mg_death.gif'), 'gunner shot at yaw +%d (bay empty)' % ARC_EMPTY, 1.0 + 48 / FPS + 0.8)
    def shot2(t): return -10.0, -6.0, 0.6
    gif(shot2, True, cams, os.path.join(OUT, 'v2b_mg_death_riders.gif'), 'gunner shot at yaw -10, riders aboard (clipping accepted)', 0.6 + 48 / FPS + 0.8)
