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

def upper_sequence(seqname, anim_or_smd, activity):
    return [f'$sequence "{seqname}" {{', f'\t"{anim_or_smd}"', f'\tactivity "{activity}" 1',
            '\tweightlist "ww2_upper"', '\tfadein 0.15', '\tfadeout 0.35', '}']
