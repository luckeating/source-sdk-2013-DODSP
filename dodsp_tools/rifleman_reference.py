"""The German rifleman's reference skeleton as an SMD, rebuilt from his COMPILED model (2026-11-22).

The CoD pack builders (build_ww2_pack.py, build_cod1_pack.py) take the soldier's reference SMD; the original file is
gone from the work area. The compiled npc_german_rifleman.mdl keeps every bone's reference position and rotation
(mstudiobone_t pos / rot), so this writes them back in the node order of DoD's a_reference SMD (same names). The one
bone the compile dropped (Bip01_L_Knee - no vertex uses it) takes DoD's value. Checked: build_cod1_pack.py with this
reproduces the delivered npc_ww2_anims_cod1 SMDs exactly but for that unused knee; build_ww2_pack.py within 2e-5.

    python3 -I rifleman_reference.py <npc_german_rifleman.mdl> <dod_player_shared_anim_a_reference.smd> <out.smd>
"""
import os, sys
sys.path[:0] = [os.path.dirname(os.path.abspath(__file__)), os.path.join(os.path.dirname(os.path.abspath(__file__)), 'character')]
import mdl48
from smd import SMD

def main(mdl, dod_ref, out):
    S = mdl48.read(mdl); sb = {b['name']: b for b in S['bones']}
    d = SMD(dod_ref); f0 = d.frames[0][1]
    L = ['version 1', 'nodes'] + ['%d "%s" %d' % (i, n, p) for i, (n, p) in enumerate(d.nodes)] + ['end', 'skeleton', 'time 0']
    for i, (n, p) in enumerate(d.nodes):
        v = tuple(sb[n]['pos']) + tuple(sb[n]['rot']) if n in sb else tuple(f0[i])
        L.append('%d %.6f %.6f %.6f %.6f %.6f %.6f' % ((i,) + v))
    L += ['end']
    open(out, 'w').write('\n'.join(L) + '\n')
    print('wrote', out, '-', sum(1 for n, _ in d.nodes if n in sb), 'bones from the model,',
          sum(1 for n, _ in d.nodes if n not in sb), 'from DoD\'s reference')

if __name__ == '__main__':
    main(*sys.argv[1:4])
