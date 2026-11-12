"""Shared helpers: upper-body replacement layers for full-body pain animations.

A pain animation is played as an overlay layer that REPLACES the upper body
(Spine1 up: torso, arms, head, gun) while the hips, lowest spine bone and legs keep
doing whatever DoD's base animation does (standing, crouched, walking). Unlike the old
additive flinches, nothing is added on top of a different pose, so the CoD detail
survives intact; and since the legs never leave DoD's stance, nothing slides or snaps.

The CoD upper body is rebased onto DoD's hips first (rebase_upper): CoD's soldier stands
with his hips turned ~30deg differently from DoD's, and CoD's torso was animated relative
to CoD's hips, so laid on DoD's hips as-is the whole upper body (and the gun) came out
turned ~30deg sideways. The overlay copy is turned so the gun starts where DoD's aim
points it, and its Spine1 is re-expressed relative to DoD's own Spine bone.
"""
import math
import numpy as np
from smd import SMD
from retarget_cod1 import worlds, rotm, euler
def upper_weight(name):
    n = name.lower()
    if 'thigh' in n or 'calf' in n or 'foot' in n or 'toe' in n or 'knee' in n or n.endswith('pelvis'):
        return 0
    if n.endswith('bip01_spine'): return 0
    return 1

def weightlist_lines(nodes, name='ww2_upper'):
    out = [f'$weightlist "{name}" {{']
    for n, _ in nodes:
        if 'knee' in n.lower(): continue
        out.append(f'\t"{n}" {upper_weight(n)}')
    out.append('}')
    return out

# IK. DoD's own sequences carry  ikrule "lhand" touch "ValveBiped.Bip01_R_Hand" : after all
# layers are blended, the engine pulls the left hand back onto the gun relative to the right
# hand. Under a pain overlay that yanked the left hand (and the K98, which hangs from it) away
# from where the CoD animation put it. Every CoD animation therefore carries "release" rules
# for both hands: weighted by the layer's own weight, they hand the hands back to the
# animation as the overlay fades in, and give them back to DoD's rule as it fades out.
# The chains must match the German model's own $ikchain list, in the same order (rule data
# stores the chain by index).
def ikchain_lines():
    return ['$ikchain "rhand" "ValveBiped.Bip01_R_Hand" knee 0.707 0.707 0',
            '$ikchain "lhand" "ValveBiped.Bip01_L_Hand" knee 0.707 0.707 0',
            '$ikchain "rfoot" "ValveBiped.Bip01_R_Foot" knee 0.707 -0.707 0',
            '$ikchain "lfoot" "ValveBiped.Bip01_L_Foot" knee 0.707 -0.707 0']

def hand_release_lines(nframes):
    e = max(0, nframes - 1)
    return [f'ikrule "rhand" release range 0 0 {e} {e}', f'ikrule "lhand" release range 0 0 {e} {e}']

def upper_sequence(seqname, smd, activity, nframes):
    """An upper-body overlay sequence on its own $animation (so its IK rules are its own)."""
    out = [f'$animation "a_{seqname}" "{smd}" {{', '\tfps 30']
    out += ['\t' + l for l in hand_release_lines(nframes)]
    out += ['}', f'$sequence "{seqname}" {{', f'\t"a_{seqname}"', f'\tactivity "{activity}" 1',
            '\tweightlist "ww2_upper"', '\tfadein 0.15', '\tfadeout 0.35', '}']
    return out


MP40_MUZZLE = np.array([0.8, 4.5, 26.5])     # w_mp40 muzzle, in its weapon_bone frame

def _yaw(v):
    return math.atan2(v[1], v[0])

def rebase_upper(src_path, dst_path, stance_path):
    """Write an overlay copy of a (DoD-skeleton) CoD animation: whole pose turned about the
    vertical so the gun starts pointing where DoD's aim (stance_path, frame 0) points it,
    hips / Spine / legs from DoD's stance, Spine1 re-expressed relative to DoD's Spine."""
    s = SMD(src_path); st = SMD(stance_path)
    gun = s.index('ValveBiped.weapon_bone'); spine = s.index('ValveBiped.Bip01_Spine'); spine1 = s.index('ValveBiped.Bip01_Spine1')
    Ws = worlds(st, st.frames[0][1])
    W0 = worlds(s, s.frames[0][1])
    psi = _yaw(Ws[st.index('ValveBiped.weapon_bone')][0] @ MP40_MUZZLE) - _yaw(W0[gun][0] @ MP40_MUZZLE)
    Y = rotm(0, 0, psi)
    stance_local = {n.lower(): st.frames[0][1][i] for i, (n, _) in enumerate(st.nodes) if i in st.frames[0][1]}
    R_spine = Ws[st.index('ValveBiped.Bip01_Spine')][0]
    lower = [i for i, (n, _) in enumerate(s.nodes) if upper_weight(n) == 0]
    for _, f in s.frames:
        W = worlds(s, f)
        R1 = R_spine.T @ (Y @ W[spine1][0])
        for i in lower:
            v = stance_local.get(s.nodes[i][0].lower())
            if v is not None: f[i] = tuple(v)
        f[spine1] = tuple(f[spine1][:3]) + euler(R1)
    s.write(dst_path)
    return math.degrees(psi)
