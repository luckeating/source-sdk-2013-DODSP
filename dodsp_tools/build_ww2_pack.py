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
    sequences.append((m.group(1), anims, act))

WANT = re.compile(r'^cod2_(death|pain|turn|corn|cover|jump_(lowwall|fence|window|down)|drag|surrender|mg42|30cal|plant|rocket|grenade|melee)')
KEEP_HL2_ACT = re.compile(r'^ACT_(DIE|FLINCH|BIG_FLINCH|SMALL_FLINCH)')  # turns: later, need rotation extraction

GESTURE_FOR = {   # full-body flinch activity -> gesture activity
    'ACT_FLINCH_HEAD': 'ACT_GESTURE_FLINCH_HEAD', 'ACT_FLINCH_LEFTARM': 'ACT_GESTURE_FLINCH_LEFTARM',
    'ACT_FLINCH_RIGHTARM': 'ACT_GESTURE_FLINCH_RIGHTARM', 'ACT_FLINCH_STOMACH': 'ACT_GESTURE_FLINCH_STOMACH',
    'ACT_FLINCH_CHEST': 'ACT_GESTURE_FLINCH_CHEST', 'ACT_FLINCH_LEFTLEG': 'ACT_GESTURE_FLINCH_LEFTLEG',
    'ACT_FLINCH_RIGHTLEG': 'ACT_GESTURE_FLINCH_RIGHTLEG', 'ACT_BIG_FLINCH': 'ACT_GESTURE_BIG_FLINCH',
    'ACT_SMALL_FLINCH': 'ACT_GESTURE_SMALL_FLINCH',
}
LEG_GESTURES = {'ACT_GESTURE_FLINCH_LEFTLEG', 'ACT_GESTURE_FLINCH_RIGHTLEG'}

chosen = [s for s in sequences if WANT.match(s[0])]

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
pelvis = skel.nodes.index(next(n for n in skel.nodes if n[0].lower() == 'valvebiped.bip01_pelvis'))
skel.tail = '\n'.join(['triangles', 'ww2_null',
    f'{pelvis} 0 0 38 0 0 1 0 0', f'{pelvis} 0.1 0 38 0 0 1 1 0', f'{pelvis} 0 0.1 38 0 0 1 0 1', 'end'])
skel.write(os.path.join(OUT, 'ww2_skeleton.smd'))

# ---------------------------------------------------------------- SMDs
def process(src, dst):
    s = SMD(src)
    wr = s.index('ValveBiped.Weapon_bone')
    rh = s.index('ValveBiped.Bip01_R_Hand'); lh = s.index('ValveBiped.Bip01_L_Hand')
    if wr < 0:
        s.nodes.append(('ValveBiped.weapon_bone', rh)); wr = len(s.nodes) - 1
    else:
        s.nodes[wr] = ('ValveBiped.weapon_bone', s.nodes[wr][1])
    s.nodes.append(('ValveBiped.weapon_bone_left', lh)); wl = len(s.nodes) - 1
    names = [n.lower() for n, _ in s.nodes]
    for _, f in s.frames:
        for i, v in list(f.items()):
            n = names[i]
            if n != 'valvebiped.bip01_pelvis' and n in ref_pos:
                f[i] = tuple(ref_pos[n]) + tuple(v[3:])
        f.pop(wr, None); f.pop(wl, None)
        W = world_mats(s.nodes, f)
        f[wr] = local_from(W[rh], W[rh] @ GRIP_MP40)    # parent: right hand
        f[wl] = local_from(W[lh], W[rh] @ GRIP_K98)     # parent: left hand, placed from the right
    s.write(dst)
    return len(s.frames)

used_anims = []
for _, anims, _ in chosen:
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
# studiomdl wants the brace on the $weightlist line; every bone is listed explicitly
# (legs and pelvis 0, a soft ramp up the spine, everything above it 1).
def _upper_weight(name):
    n = name.lower()
    if 'thigh' in n or 'calf' in n or 'foot' in n or 'toe' in n or 'knee' in n or n.endswith('pelvis'): return 0
    if n.endswith('bip01_spine'): return 0.3
    if n.endswith('bip01_spine1'): return 0.7
    return 1
L.append('$weightlist "ww2_upperbody" {')
for n, _ in ref.nodes:
    if 'knee' in n.lower(): continue   # unweighted in the DoD mesh; studiomdl drops it
    L.append(f'\t"{n}" {_upper_weight(n)}')
L.append('}')
L.append('')
for a in used_anims:
    _, opts = animations[a]
    L.append(f'$animation "{a}" "ww2_{a}.smd" {{')
    for o in opts: L.append('\t' + o)
    L.append('}')
L.append('')

seq_rows = []
gestures_done = set()
seen = set()
for name, anims, act in chosen:
    short = name[len('cod2_'):]
    if act and KEEP_HL2_ACT.match(act):
        activity = act
    else:
        activity = 'ACT_WW2_' + re.sub(r'_\d+$', '', short).upper()
    if (tuple(anims), activity) in seen:
        continue    # the port repeats one animation under several HL2 activities we don't keep
    seen.add((tuple(anims), activity))
    L.append(f'$sequence "ww2_{short}" {{')
    for a in anims: L.append(f'\t"{a}"')
    L.append(f'\tactivity "{activity}" 1')
    L.append('\tfadein 0.2\n\tfadeout 0.2')
    if any('loop' == o for o in animations[anims[0]][1]): L.append('\tloop')
    L.append('}')
    seq_rows.append((f'ww2_{short}', activity, anims[0], frames[anims[0]]))
    g = GESTURE_FOR.get(activity)
    if g and g not in gestures_done and len(anims) == 1:
        gestures_done.add(g)
        a = anims[0]
        L.append(f'$sequence "ww2_g_{g[len("ACT_GESTURE_"):].lower()}" {{')
        L.append(f'\t"{a}"')
        L.append(f'\tactivity "{g}" 1')
        L.append('\tdelta')
        L.append(f'\tsubtract "{a}" 0')
        if g not in LEG_GESTURES: L.append('\tweightlist "ww2_upperbody"')
        L.append('\tfadein 0.1\n\tfadeout 0.2')
        L.append('}')
        seq_rows.append((f'ww2_g_{g[len("ACT_GESTURE_"):].lower()}', g, a, frames[a]))

open(os.path.join(OUT, 'npc_ww2_anims.qc'), 'w', newline='\r\n').write('\n'.join(L) + '\n')

with open(os.path.join(OUT, 'SEQUENCES.txt'), 'w', newline='\r\n') as f:
    f.write('sequence\tactivity\tsource animation\tframes\n')
    for r in seq_rows: f.write('\t'.join(str(x) for x in r) + '\n')

print('animations', len(used_anims), 'sequences', len(seq_rows), 'gestures', len(gestures_done))
