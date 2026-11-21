"""V2b: CoD2's MG42 (mg42_with_bipod_test1, the gun CoD2 mounts on its turrets) as a Source model for the halftrack's
pintle - the owner compiles it with Crowbar.

    python3 mg42_model.py <CoD2 pack dir> <out dir>
        writes dodsp_mg42_pintle.qc + _ref.smd + _down.smd / _up.smd (the gun at mg_pitch -15 / +15)

THE MODEL'S FACTS (the server reads them from the compiled model, never from numbers of its own):
    origin       = CoD2's tag_aim (the gun's elevation pivot): the server puts the model's origin on the vehicle's
                   pintle (CoD2's tag_turret, cod2_vehicle_tags.txt) and turns the ENTITY for traverse
    bone mg_base = CoD2's tag_pivot part (the yoke: traverses with the entity, never elevates)
    bone mg_gun  = CoD2's tag_aim_animated part (the gun: elevated by the pose parameter "mg_pitch", + = muzzle up)
    attachment "muzzle" on mg_gun = CoD2's tag_flash (where the bullets and the flash start)
Units: CoD2's x PORT_SCALE (1.0284), like every CoD2 port. Frame: written so studiomdl's default 90-degree turn
faces it +x in game (an SMD faces -y: game (x, y, z) -> SMD (y, -x, z)), like editor_models.py. UVs: CoD's v runs
down, an SMD's up (v' = 1 - v). Material: CoD2's own name, mtl_weapon_mg42, in models\\cod2\\ like the vehicles.
No collision model (the entity is not solid: bullets pass it, as they pass a real gun's air gaps).
"""
import os, sys, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, HERE)
from cod2_xmodel import parts, surfs

PORT_SCALE = 1.0284
PITCH = 15.0                                   # mg_pitch range +-PITCH (the gunner's poses: mg_gunner_compose.py TOP)
NAME = 'dodsp_mg42_pintle'

def to_smd(p): return np.array([p[1], -p[0], p[2]])
def Rx(a): a = np.radians(a); c, s = np.cos(a), np.sin(a); return np.array([[1, 0, 0], [0, c, -s], [0, s, c]])

def main(cod2, out):
    os.makedirs(out, exist_ok=True)
    B = parts(os.path.join(cod2, 'xmodelparts', 'mg42_with_bipod_test1'))
    S = surfs(os.path.join(cod2, 'xmodelsurfs', 'mg42_with_bipod_test1'))
    T = {b['name']: b['t'] * PORT_SCALE for b in B}
    aim = T['tag_aim']
    bone_of = {'tag_pivot': 0, 'tag_aim_animated': 1}
    tris = []
    for s in S:
        bname = B[s['dbone']]['name']
        if bname not in bone_of: raise SystemExit('mesh on bone %s - not the yoke or the gun' % bname)
        bi = bone_of[bname]; b = B[s['dbone']]
        V = [(to_smd((b['R'] @ v['pos'] + b['t']) * PORT_SCALE - aim), to_smd(b['R'] @ v['n']), (v['uv'][0], 1.0 - v['uv'][1])) for v in s['verts']]
        for t in s['tris']:
            tris.append((bi, [V[k] for k in (t[0], t[2], t[1])]))   # CoD winds the other way (as the vehicle ports)
    nodes = 'nodes\n0 "mg_base" -1\n1 "mg_gun" 0\nend\n'
    def skel(pitch_deg):
        # mg_gun at the pivot (the origin), turned about the gun's left axis: game +y = SMD +x; muzzle (SMD -y) up = -angle
        return 'skeleton\ntime 0\n0 0 0 0 0 0 0\n1 0 0 0 %.6f 0 0\nend\n' % np.radians(-pitch_deg)
    ref = 'version 1\n' + nodes + skel(0) + 'triangles\n'
    for bi, vs in tris:
        ref += 'mtl_weapon_mg42\n' + ''.join('%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f\n' % (bi, *p, *n, *uv) for p, n, uv in vs)
    ref += 'end\n'
    open(os.path.join(out, NAME + '_ref.smd'), 'w', newline='\n').write(ref)
    for tag, deg in (('down', -PITCH), ('up', PITCH)):
        open(os.path.join(out, '%s_%s.smd' % (NAME, tag)), 'w', newline='\n').write('version 1\n' + nodes + skel(deg))
    m = to_smd(T['tag_flash'] - aim)
    qc = ('// DOD-SP: CoD2\'s MG42 (mg42_with_bipod_test1) for the halftrack\'s pintle - dodsp_tools/vehicle_preview/mg42_model.py.\n'
          '// Origin = the elevation pivot (CoD2 tag_aim); traverse = the entity\'s yaw; elevation = mg_pitch (+ = up).\n\n'
          '$modelname "dodsp/vehicles/%s.mdl"\n$cdmaterials "models\\cod2\\"\n$surfaceprop "metal"\n\n'
          '$body "gun" "%s_ref.smd"\n\n'
          '$poseparameter "mg_pitch" %g %g\n'
          '$animation "a_down" "%s_down.smd" { fps 1 }\n$animation "a_up" "%s_up.smd" { fps 1 }\n'
          '$sequence "idle" {\n\t"a_down"\n\t"a_up"\n\tblend "mg_pitch" %g %g\n\tfadein 0.2\n\tfadeout 0.2\n}\n\n'
          '$attachment "muzzle" "mg_gun" %.3f %.3f %.3f rotate 0 0 0\n'
          % (NAME, NAME, -PITCH, PITCH, NAME, NAME, -PITCH, PITCH, *m))
    open(os.path.join(out, NAME + '.qc'), 'w', newline='\n').write(qc)
    # check: the muzzle rises with mg_pitch (the sign the server relies on)
    def muzzle(deg): return Rx(-deg) @ m
    up, down = muzzle(PITCH), muzzle(-PITCH)
    if not up[2] > m[2] > down[2]: raise SystemExit('mg_pitch turns the muzzle the wrong way (%s / %s / %s)' % (down, m, up))
    print('%s: %d triangles; muzzle %.1f ahead of the pivot, %.1f up; mg_pitch +%g lifts it to %.1f, -%g drops it to %.1f'
          % (NAME, len(tris), -m[1], m[2], PITCH, up[2], PITCH, down[2]))

if __name__ == '__main__':
    main(*sys.argv[1:3])
