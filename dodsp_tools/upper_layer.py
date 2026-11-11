"""Shared helpers: upper-body replacement layers for full-body pain animations.

A pain animation is played as an overlay layer that REPLACES the upper body
(spine up, arms, head, gun) while the legs keep doing whatever DoD's base
animation does (standing, crouched, walking). Unlike the old additive flinches,
nothing is added on top of a different pose, so the CoD detail survives intact;
and since the legs never leave DoD's stance, nothing slides or snaps.
"""
def upper_weight(name):
    n = name.lower()
    if 'thigh' in n or 'calf' in n or 'foot' in n or 'toe' in n or 'knee' in n or n.endswith('pelvis'):
        return 0
    if n.endswith('bip01_spine'): return 0.3
    if n.endswith('bip01_spine1'): return 0.7
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
