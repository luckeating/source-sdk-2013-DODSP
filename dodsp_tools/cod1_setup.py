"""Shared setup for retargeting the CoD1 'assault' decompile onto the DoD German."""
import numpy as np, math
from smd import SMD
from retarget_cod1 import Retargeter, worlds

K98_MUZZLE = np.array([1.3, 4.3, 35.5])     # w_k98 muzzle_flash, in its weapon_bone_left frame
MP40_MUZZLE = np.array([0.8, 4.5, 26.5])    # w_mp40 muzzle_flash, in its weapon_bone frame

def _grip(path, bone):
    s = SMD(path); W = worlds(s, s.frames[0][1])
    R, t = W[s.index('ValveBiped.Bip01_R_Hand')]; R2, t2 = W[s.index(bone)]
    M = np.eye(4); M[:3, :3] = R.T @ R2; M[:3, 3] = R.T @ (t2 - t); return M

def _align(a, b):
    """Smallest rotation taking direction a to direction b."""
    a = a / np.linalg.norm(a); b = b / np.linalg.norm(b)
    v = np.cross(a, b); c = float(np.dot(a, b))
    if np.linalg.norm(v) < 1e-9: return np.eye(3)
    K = np.array([[0, -v[2], v[1]], [v[2], 0, -v[0]], [-v[1], v[0], 0]])
    return np.eye(3) + K + K @ K * (1 / (1 + c))

def make_retargeter(assault_dir, dod_dir, dod_ref):
    aim = SMD(f'{assault_dir}/assault_anim_@aim.smd'); Wa = worlds(aim, aim.frames[0][1])
    Rtag, ttag = Wa[aim.index('tag_weapon')]
    cod_dir_tag = Rtag.T @ (Wa[aim.index('kar98_tag_flash')][1] - ttag)     # barrel in tag space
    offs = {}
    for key, path, bone, muzzle in (('k98', f'{dod_dir}/dod_player_shared_anim_@StandAim_BOLT.smd', 'ValveBiped.weapon_bone_left', K98_MUZZLE),
                                    ('mp40', f'{dod_dir}/dod_player_shared_anim_@StandAim_MP40.smd', 'ValveBiped.weapon_bone', MP40_MUZZLE)):
        s = SMD(path); W = worlds(s, s.frames[0][1])
        off = Rtag.T @ W[s.index(bone)][0]                  # roll/orientation from matching aim poses
        off = _align(off @ muzzle, cod_dir_tag) @ off        # then make the barrel exactly CoD's barrel
        offs[key] = off
    g40 = _grip(f'{dod_dir}/dod_player_shared_anim_@StandAim_MP40.smd', 'ValveBiped.weapon_bone')
    g98 = _grip(f'{dod_dir}/dod_player_shared_anim_@StandAim_BOLT.smd', 'ValveBiped.weapon_bone_left')
    rt = Retargeter(f'{assault_dir}/assault_anim_@ragdoll.smd', dod_ref, g40, g98, offs)
    # Hands: CoD1's rest pose has the wrists turned, so the rest-to-rest comparison can't be
    # trusted for them. The hand bones point the same way in both rigs (checked against CoD1's
    # gun); what differs is the roll about that axis. It's measured from the animations: the
    # roll (multiple of 90deg) that makes CoD1's wrist twist (hand vs forearm) match DoD's own
    # animations. (It came out 180deg: with none, CoD1 palms faced backwards.)
    for side in ('l', 'r'):
        rt.C[f'valvebiped.bip01_{side}_hand'] = hand_roll(assault_dir, dod_dir, side, rt.C.get(f'valvebiped.bip01_{side}_forearm', np.eye(3)))
    # Fingers: same story (their rest poses curl differently). Each finger's roll is chosen so
    # its knuckles bend about the same axis as DoD's fingers do in DoD's own animations.
    for side in ('l', 'r'):
        for f in range(5):
            C = finger_roll(assault_dir, dod_dir, side, f)
            for suffix in ('', '1', '2'):
                rt.C[f'valvebiped.bip01_{side}_finger{f}{suffix}'] = C
    m = lambda b, C=np.eye(3): (lambda R, t: np.block([[R @ C, t[:, None]], [np.zeros((1, 3)), np.ones((1, 1))]]))(*Wa[aim.index(b)])
    rt.set_gun_reference(m('bip01 r hand', rt.C['valvebiped.bip01_r_hand']), m('bip01 l hand', rt.C['valvebiped.bip01_l_hand']),
                         m('tag_weapon'))
    return rt

def _twist(R):
    w = math.sqrt(max(1e-12, 1 + np.trace(R))) / 2
    return 2 * math.atan2((R[2, 1] - R[1, 2]) / (4 * w), w)

def _roll_x(a):
    return np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])

def hand_roll(assault_dir, dod_dir, side, C_forearm):
    import glob
    def rels(files, hand, fore, step):
        out = []
        for f in files:
            s = SMD(f); h, fa = s.index(hand), s.index(fore)
            if h < 0 or fa < 0: continue
            for _, fr in s.frames[::step]:
                W = worlds(s, fr); out.append(W[fa][0].T @ W[h][0])
        return out
    S = side.upper()
    dod = rels(sorted(glob.glob(f'{dod_dir}/dod_player_shared_anim_*.smd'))[::6],
               f'ValveBiped.Bip01_{S}_Hand', f'ValveBiped.Bip01_{S}_Forearm', 5)
    dmean = math.atan2(np.mean([math.sin(_twist(R)) for R in dod]), np.mean([math.cos(_twist(R)) for R in dod]))
    cod = rels(sorted(glob.glob(f'{assault_dir}/assault_anim_*.smd'))[::4], f'bip01 {side} hand', f'bip01 {side} forearm', 5)
    best = max(range(4), key=lambda k: np.mean([math.cos(_twist(C_forearm.T @ R @ _roll_x(k * math.pi / 2)) - dmean) for R in cod]))
    return _roll_x(best * math.pi / 2)


def _axis_angle(R):
    a = math.acos(max(-1.0, min(1.0, (np.trace(R) - 1) / 2)))
    if a < 1e-4: return np.zeros(3)
    return np.array([R[2, 1] - R[1, 2], R[0, 2] - R[2, 0], R[1, 0] - R[0, 1]]) / (2 * math.sin(a)) * a

_bend_cache = {}
def _mean_bend(files, pairs, step):
    key = (tuple(files), tuple(pairs))
    if key in _bend_cache: return _bend_cache[key]
    acc = np.zeros(3)
    for f in files:
        s = SMD(f)
        idx = [(s.index(c), s.index(p)) for c, p in pairs]
        for _, fr in s.frames[::step]:
            W = worlds(s, fr)
            for i, j in idx:
                if i >= 0 and j >= 0 and i in W and j in W:
                    acc += _axis_angle(W[j][0].T @ W[i][0])
    _bend_cache[key] = acc
    return acc

def finger_roll(assault_dir, dod_dir, side, f):
    """Roll about the finger's own axis (multiple of 90deg) so CoD1's knuckle bend axis matches DoD's."""
    import glob
    S = side.upper()
    dod = _mean_bend(sorted(glob.glob(f'{dod_dir}/dod_player_shared_anim_*.smd'))[::6],
                     [(f'ValveBiped.Bip01_{S}_Finger{f}1', f'ValveBiped.Bip01_{S}_Finger{f}'),
                      (f'ValveBiped.Bip01_{S}_Finger{f}2', f'ValveBiped.Bip01_{S}_Finger{f}1')], 5)
    cod = _mean_bend(sorted(glob.glob(f'{assault_dir}/assault_anim_*.smd'))[::4],
                     [(f'bip01 {side} finger{f}1', f'bip01 {side} finger{f}'),
                      (f'bip01 {side} finger{f}2', f'bip01 {side} finger{f}1')], 5)
    best = max(range(4), key=lambda k: float(np.dot(_roll_x(k * math.pi / 2).T @ cod, dod)))
    return _roll_x(best * math.pi / 2)
