"""Build the DOD-SP WW2 NPC animation pack from the decompiled CoD2 ranger.

Output: a folder with a QC + processed SMDs that compile (Crowbar/studiomdl)
into models/german/npc_ww2_anims.mdl, an animation-only model on the DoD
German skeleton. The German soldier's QC includes it.

Processing per animation:
  * bone translations (except the pelvis) are replaced with the DoD German's
    own bone lengths, so borrowed animations never change his proportions;
  * ValveBiped.weapon_bone (MP40 etc.) and ValveBiped.weapon_bone_left (K98)
    are written every frame so both guns keep DoD's own grip relative to the
    RIGHT (trigger) hand. CoD2 support-hand placement differs from DoD's, so
    hanging the K98 off the left hand (as DoD's skeleton does) tilts it ~35deg;
  * sequences get clean names (ww2_*) and activities: HL2 activities where the
    base AI already uses them (deaths, flinches, turns), ACT_WW2_* otherwise;
  * flinches also get delta (gesture) versions for ACT_GESTURE_FLINCH_*.
"""
import os, re, sys, shutil, math
import numpy as np
sys.path.insert(0, os.path.dirname(__file__))
from smd import SMD
from upper_layer import weightlist_lines, upper_sequence, ikchain_lines, hand_release_lines
from gun_follow import GunReference, place_guns, inv

# Upper-body pain overlays (see upper_layer.py): source animation -> activity
UPPER_PAINS = {'a_stand_helmet_hit': 'ACT_WW2_UPPER_PAIN_HEAD',
               'a_stand_pain_front_lshoulder': 'ACT_WW2_UPPER_PAIN_LARM',
               'a_stand_pain_front_rshoulder': 'ACT_WW2_UPPER_PAIN_RARM',
               'a_stand_pain_hunch': 'ACT_WW2_UPPER_PAIN_GUT'}

SRC_DIR, SRC_QC, DOD_REF, DOD_BOLT, DOD_MP40, OUT = sys.argv[1:7]

# ---------------------------------------------------------------- parse QC
qc = open(os.path.join(SRC_DIR, SRC_QC), encoding='latin1').read().replace('\r\n', '\n')
animations = {}
for m in re.finditer(r'^\$animation "([^"]+)" "([^"]+)" \{\n(.*?)^\}', qc, re.M | re.S):
    animations[m.group(1)] = (m.group(2), [l.strip() for l in m.group(3).split('\n') if l.strip()])
sequences = []
for m in re.finditer(r'^\$sequence "([^"]+)" \{\n(.*?)^\}', qc, re.M | re.S):
    body = [l.strip() for l in m.group(2).split('\n') if l.strip()]
    anims = [l.strip('"') for l in body if l.startswith('"')]
    act = None
    for l in body:
        mm = re.match(r'activity "([^"]+)"', l)
        if mm: act = mm.group(1)
    # other options worth keeping (blend axes and widths for the MG gunner grids, etc.)
    extra = [l for l in body if re.match(r'(blend|blendwidth|blendcenter|blendref|calcblend)\b', l)]
    sequences.append((m.group(1), anims, act, extra))
poseparams = {m.group(1): m.group(0) for m in re.finditer(r'^\$poseparameter "([^"]+)".*$', qc, re.M)}

WANT = re.compile(r'^cod2_(death|pain|turn|corn|cover|jump_(lowwall|fence|window|down)|drag|surrender|mg42|30cal|plant|rocket|grenade|melee)')
KEEP_HL2_ACT = re.compile(r'^ACT_(DIE|FLINCH|BIG_FLINCH|SMALL_FLINCH)')  # turns: later, need rotation extraction

# Flinch gestures (CoD deltas layered on DoD poses) are switched off: the deltas come
# from a different rifle hold and base pose, so they bent arms through the chest and
# made crouched legs spasm. Hit reactions are now a procedural kick (client code)
# plus the untouched full-body CoD pains below. Set MAKE_FLINCH_GESTURES = True to
# bring the gesture versions back for experiments.
MAKE_FLINCH_GESTURES = False
GESTURE_FOR = {   # full-body flinch activity -> gesture activity
    'ACT_FLINCH_HEAD': 'ACT_GESTURE_FLINCH_HEAD', 'ACT_FLINCH_LEFTARM': 'ACT_GESTURE_FLINCH_LEFTARM',
    'ACT_FLINCH_RIGHTARM': 'ACT_GESTURE_FLINCH_RIGHTARM', 'ACT_FLINCH_STOMACH': 'ACT_GESTURE_FLINCH_STOMACH',
    'ACT_FLINCH_CHEST': 'ACT_GESTURE_FLINCH_CHEST', 'ACT_FLINCH_LEFTLEG': 'ACT_GESTURE_FLINCH_LEFTLEG',
    'ACT_FLINCH_RIGHTLEG': 'ACT_GESTURE_FLINCH_RIGHTLEG', 'ACT_BIG_FLINCH': 'ACT_GESTURE_BIG_FLINCH',
    'ACT_SMALL_FLINCH': 'ACT_GESTURE_SMALL_FLINCH',
}
LEG_GESTURES = {'ACT_GESTURE_FLINCH_LEFTLEG', 'ACT_GESTURE_FLINCH_RIGHTLEG'}

chosen = [s for s in sequences if WANT.match(s[0])]
needed_params = []
for s in chosen:
    for l in s[3]:
        mm = re.match(r'blend "([^"]+)"', l)
        if mm and mm.group(1) not in needed_params: needed_params.append(mm.group(1))

# ---------------------------------------------------------------- DoD data
ref = SMD(DOD_REF)
ref_pos = {n.lower(): ref.frames[0][1][i][:3] for i, (n, _) in enumerate(ref.nodes)}
def euler_to_m(x, y, z):
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    Rx = np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]]); Ry = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    Rz = np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]])
    return Rz @ Ry @ Rx
def m_to_euler(R):
    y = math.asin(max(-1.0, min(1.0, -R[2, 0])))
    return math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0])
def to_mat(v):
    M = np.eye(4); M[:3, :3] = euler_to_m(*v[3:]); M[:3, 3] = v[:3]; return M
def world_mats(nodes, frame):
    W = {}
    for i, (_, p) in enumerate(nodes):
        if i not in frame: continue
        W[i] = to_mat(frame[i]) if p < 0 or p not in W else W[p] @ to_mat(frame[i])
    return W
def grip_from_right_hand(path, bone):
    """Gun bone transform relative to the right hand in a DoD aim pose."""
    s = SMD(path); W = world_mats(s.nodes, s.frames[0][1])
    return np.linalg.inv(W[s.index('ValveBiped.Bip01_R_Hand')]) @ W[s.index(bone)]
GRIP_MP40 = grip_from_right_hand(DOD_MP40, 'ValveBiped.weapon_bone')
GRIP_K98 = grip_from_right_hand(DOD_BOLT, 'ValveBiped.weapon_bone_left')
def local_from(parent_world, want_world):
    M = np.linalg.inv(parent_world) @ want_world
    return tuple(M[:3, 3]) + m_to_euler(M[:3, :3])

os.makedirs(OUT, exist_ok=True)

# skeleton-only reference: DoD German bones, bind pose, one tiny triangle
skel = SMD(); skel.nodes = list(ref.nodes); skel.frames = [ref.frames[0]]
# studiomdl drops bones no vertex is weighted to, so give every bone (except the
# knee, which the DoD mesh never uses either) one tiny triangle at its own position.
_W = None
def _bone_world(i):
    global _W
    if _W is None: _W = world_mats(skel.nodes, skel.frames[0][1])
    return _W[i][:3, 3]
tris = ['triangles']
for i, (n, _) in enumerate(skel.nodes):
    if 'knee' in n.lower(): continue
    x, y, z = _bone_world(i)
    tris.append('ww2_null')
    tris.append(f'{i} {x:.4f} {y:.4f} {z:.4f} 0 0 1 0 0')
    tris.append(f'{i} {x + 0.5:.4f} {y:.4f} {z:.4f} 0 0 1 1 0')
    tris.append(f'{i} {x:.4f} {y + 0.5:.4f} {z:.4f} 0 0 1 0 1')
tris.append('end')
skel.tail = '\n'.join(tris)
skel.write(os.path.join(OUT, 'ww2_skeleton.smd'))

# ---------------------------------------------------------------- SMDs
# The CoD2 gun: placed per frame through whichever hand holds it in the CoD animation
# (gun_follow.py). Reference = an aim pose with the gun in both hands.
_r = SMD(os.path.join(SRC_DIR, 'american_ranger_normandy__2_2_anim_a_stand_pain_front_lshoulder.smd'))
_W = world_mats(_r.nodes, _r.frames[0][1])
GUN_REF = GunReference(_W[_r.index('ValveBiped.Bip01_R_Hand')], _W[_r.index('ValveBiped.Bip01_L_Hand')],
                       _W[_r.index('ValveBiped.Weapon_bone')])
GUN_OFFSETS = [inv(GUN_REF.rel_R) @ GRIP_MP40, inv(GUN_REF.rel_R) @ GRIP_K98]   # weapon_bone, weapon_bone_left
holder_stats = {}

def process(src, dst):
    s = SMD(src)
    wr = s.index('ValveBiped.Weapon_bone')
    rh = s.index('ValveBiped.Bip01_R_Hand'); lh = s.index('ValveBiped.Bip01_L_Hand')
    has_gun = wr >= 0
    if wr < 0:
        s.nodes.append(('ValveBiped.weapon_bone', rh)); wr = len(s.nodes) - 1
    else:
        s.nodes[wr] = ('ValveBiped.weapon_bone', s.nodes[wr][1])
    s.nodes.append(('ValveBiped.weapon_bone_left', lh)); wl = len(s.nodes) - 1
    names = [n.lower() for n, _ in s.nodes]
    src_frames, dod_frames, dod_worlds = [], [], []
    for _, f in s.frames:
        if has_gun:
            Ws = world_mats(s.nodes, f)
            src_frames.append((Ws[rh], Ws[lh], Ws[wr]))
        for i, v in list(f.items()):
            n = names[i]
            if n != 'valvebiped.bip01_pelvis' and n in ref_pos:
                f[i] = tuple(ref_pos[n]) + tuple(v[3:])
        f.pop(wr, None); f.pop(wl, None)
        W = world_mats(s.nodes, f)
        dod_worlds.append(W); dod_frames.append((W[rh], W[lh]))
    if has_gun:
        guns, labels = place_guns(GUN_REF, src_frames, dod_frames, GUN_OFFSETS)
        holder_stats[os.path.basename(src)] = labels
    else:
        guns = [[W[rh] @ GRIP_MP40, W[rh] @ GRIP_K98] for W in dod_worlds]
    for (_, f), W, (g40, g98) in zip(s.frames, dod_worlds, guns):
        f[wr] = local_from(W[rh], g40)    # parent: right hand
        f[wl] = local_from(W[lh], g98)    # parent: left hand
    s.write(dst)
    return len(s.frames)

used_anims = []
for _, anims, _, _ in chosen:
    for a in anims:
        if a not in used_anims: used_anims.append(a)

frames = {}
for a in used_anims:
    smdfile, _ = animations[a]
    frames[a] = process(os.path.join(SRC_DIR, smdfile), os.path.join(OUT, f'ww2_{a}.smd'))

# ---------------------------------------------------------------- QC
L = []
L.append('// DOD-SP WW2 NPC animation pack (generated by build_ww2_pack.py - edit the generator, not this file)')
L.append('// Animations: Call of Duty 2 NPC set (via the "american_ranger_normandy" port), retargeted onto the')
L.append('// DoD:S German skeleton: DoD bone lengths, DoD weapon holds. For private testing only.')
L.append('$modelname "german\\npc_ww2_anims.mdl"')
L.append('$cdmaterials "models\\german\\"')
L.append('$bodygroup "skeleton"\n{\n\tstudio "ww2_skeleton.smd"\n}')
# studiomdl wants the brace on the $weightlist line; every bone is listed explicitly.
#
# Flinches are CoD2 pain animations turned into additive (delta) gestures and laid
# over DoD's own poses. Arm deltas from a CoD2 rifle hold don't fit DoD's holds (arms
# swing through the chest, the support hand leaves the gun), so flinch gestures only
# move the spine, neck and head: the arms ride along with the torso in DoD's grip.
# Leg flinches move only the legs.
def _torso_weight(name):
    n = name.lower()
    if n.endswith('bip01_spine'): return 0.4
    if n.endswith('bip01_spine1'): return 0.7
    if n.endswith('bip01_spine2') or n.endswith('bip01_spine4') or n.endswith('neck1') or n.endswith('head1'): return 1
    return 0      # pelvis, legs, clavicles, arms, hands, fingers, weapon bones
def _leg_weight(name):
    n = name.lower()
    return 1 if ('thigh' in n or 'calf' in n or 'foot' in n or 'toe' in n) else 0
for pp in needed_params:
    L.append(poseparams[pp])
L += ikchain_lines()
L += weightlist_lines(ref.nodes)
for wl_name, fn in (('ww2_torso', _torso_weight), ('ww2_legs', _leg_weight)):
    L.append(f'$weightlist "{wl_name}" {{')
    for n, _ in ref.nodes:
        if 'knee' in n.lower(): continue   # unweighted in the DoD mesh; studiomdl drops it
        L.append(f'\t"{n}" {fn(n)}')
    L.append('}')
L.append('')
for a in used_anims:
    _, opts = animations[a]
    L.append(f'$animation "{a}" "ww2_{a}.smd" {{')
    for o in opts: L.append('\t' + o)
    for o in hand_release_lines(frames[a]): L.append('\t' + o)
    L.append('}')
L.append('')

seq_rows = []
gestures_done = set()
gesture_anims_done = set()
seen = set()
for name, anims, act, extra in chosen:
    short = name[len('cod2_'):]
    # The port reuses the gut hunch for leg and physics flinches; the CoD1 pack has a real
    # leg-hit animation, so those become plain ACT_WW2_* names (and dedupe away).
    if act and KEEP_HL2_ACT.match(act) and act not in ('ACT_FLINCH_LEFTLEG', 'ACT_FLINCH_RIGHTLEG', 'ACT_FLINCH_PHYSICS'):
        activity = act
    else:
        activity = 'ACT_WW2_' + re.sub(r'_\d+$', '', short).upper()
    if (tuple(anims), activity) in seen:
        continue    # the port repeats one animation under several HL2 activities we don't keep
    seen.add((tuple(anims), activity))
    L.append(f'$sequence "ww2_{short}" {{')
    for a in anims: L.append(f'\t"{a}"')
    L.append(f'\tactivity "{activity}" 1')
    for l in extra: L.append('\t' + l)
    L.append('\tfadein 0.2\n\tfadeout 0.2')
    if any('loop' == o for o in animations[anims[0]][1]): L.append('\tloop')
    L.append('}')
    seq_rows.append((f'ww2_{short}', activity, anims[0], frames[anims[0]]))
    g = GESTURE_FOR.get(activity) if MAKE_FLINCH_GESTURES else None
    if g and g not in gestures_done and len(anims) == 1:
        gestures_done.add(g)
        a = anims[0]
        # The subtraction must live on an $animation of its own: "subtract" inside a
        # $sequence only applies to SMDs named inline in that sequence, so with a
        # named $animation it was silently ignored and the full pose got added on
        # top of the current one (soldiers folded into a ball when hit).
        if a not in gesture_anims_done:
            gesture_anims_done.add(a)
            L.append(f'$animation "g_{a}" "ww2_{a}.smd" {{')
            L.append('\tfps 30')
            L.append(f'\tsubtract "{a}" 0')
            L.append('}')
        L.append(f'$sequence "ww2_g_{g[len("ACT_GESTURE_"):].lower()}" {{')
        L.append(f'\t"g_{a}"')
        L.append(f'\tactivity "{g}" 1')
        L.append('\tdelta')
        # Every flinch gesture (leg hits too) moves only the torso and head. Leg deltas
        # from a standing CoD2 pain pose wobble running legs and make crouched legs spasm.
        L.append('\tweightlist "ww2_torso"')
        L.append('\tfadein 0.1\n\tfadeout 0.2')
        L.append('}')
        seq_rows.append((f'ww2_g_{g[len("ACT_GESTURE_"):].lower()}', g, a, frames[a]))

for a, act in UPPER_PAINS.items():
    if a in animations and a in frames:
        sn = 'ww2_upper_' + act[len('ACT_WW2_UPPER_'):].lower()
        L += upper_sequence(sn, f'ww2_{a}.smd', act, frames[a])
        seq_rows.append((sn, act, a, frames[a]))

open(os.path.join(OUT, 'npc_ww2_anims.qc'), 'w', newline='\r\n').write('\n'.join(L) + '\n')

with open(os.path.join(OUT, 'SEQUENCES.txt'), 'w', newline='\r\n') as f:
    f.write('sequence\tactivity\tsource animation\tframes\n')
    for r in seq_rows: f.write('\t'.join(str(x) for x in r) + '\n')

print('animations', len(used_anims), 'sequences', len(seq_rows), 'gestures', len(gestures_done))
