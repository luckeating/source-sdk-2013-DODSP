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
    # Hands: CoD1's rest pose has the wrists turned 90deg; the rigs' hand axes actually
    # match (checked against CoD1's own gun direction), so no hand correction.
    for side in ('l', 'r'):
        rt.C[f'valvebiped.bip01_{side}_hand'] = np.eye(3)
    return rt
