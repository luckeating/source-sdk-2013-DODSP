"""V3 halftrack: the rear doors on hinge bones, so riders can get out through them.

    python3 halftrack_doors.py <CoD2 xmodelparts> <CoD2 xmodelsurfs> <decompile dir> <vehicle model dir> <out dir> [open degrees]

The port (models/cod2/vehicles/halftrack*.mdl) has the whole halftrack on one bone. CoD2's own model (xmodelparts +
xmodelsurfs "halftrack_mg1", version 20) has the two door leaves as their own surfaces on the bones hatch_open_left /
hatch_open_right (the hinges). The port is that mesh x PORT_SCALE in the mesh frame (mesh x = CoD y, mesh y = -CoD x),
so each port vertex takes the bone of the CoD vertex at the same spot. Everything else is the port's own: its
materials, UVs, normals, collision (<variant>_physics.smd from the owner's Crowbar decompile) and QC settings.

Writes per variant: <variant>.qc + <variant>_body.smd (root + door_left + door_right), <variant>_closed.smd and
<variant>_open.smd (one frame each), blended by the pose parameter "doors" (0 closed .. 1 open) - the game sets it.
Same $modelname as the port, so the compiled model replaces it in place.
"""
import os, re, sys, shutil, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from editor_models import mesh
from cod2_xmodel import parts, surfs

PORT_SCALE = 1.0284           # measured: the port's vertices sit on CoD's x this (median miss 0.004 units)
VARIANTS = {                  # variant -> physics SMD in the decompile (halftrack_snow wasn't decompiled: same hull)
    'halftrack': 'halftrack_physics.smd', 'halftrack_desert': 'halftrack_desert_physics.smd',
    'halftrack_snow': 'halftrack_physics.smd', 'halftrack_rockets': 'halftrack_rockets_physics.smd',
    'halftrack_rockets_snow': 'halftrack_rockets_snow_physics.smd'}
DOOR_BONES = (('door_left', 'hatch_open_left', -1.0), ('door_right', 'hatch_open_right', 1.0))   # name, CoD bone, swing sign
MATCH = 0.05                  # a port triangle whose middle is this close to a CoD door triangle's is on that door

def cod_to_mesh(p): return PORT_SCALE * np.array([p[1], -p[0], p[2]])

def door_vertices(parts_path, surfs_path):
    B = parts(parts_path); S = surfs(surfs_path)
    hinge = {}
    pts, which = [], []
    for k, (name, cod, sign) in enumerate(DOOR_BONES):
        bi = [i for i, b in enumerate(B) if b['name'] == cod]
        if not bi: raise SystemExit('no %s in %s' % (cod, parts_path))
        hinge[name] = cod_to_mesh(B[bi[0]]['t'])
    # per TRIANGLE (the two leaves share vertex positions where they meet in the middle, so a vertex alone can't say
    # which door it is on): a triangle is on a door when most of its corners ride that door's bone in CoD's model
    for s in S:
        for t in s['tris']:
            vs = [s['verts'][i] for i in t]
            bn = [B[v['bone']]['name'] for v in vs]
            name = next((n for n, cod, _ in DOOR_BONES if bn.count(cod) >= 2), None)
            pts.append(np.mean([cod_to_mesh(B[v['bone']]['R'] @ v['pos'] + B[v['bone']]['t']) for v in vs], axis=0)); which.append(name)
    return hinge, np.array(pts), which

def write_variant(var, phys, decomp, models, out, hinge, P, which, open_deg):
    from scipy.spatial import cKDTree
    tris, cds = mesh(os.path.join(models, var + '.mdl'))
    tree = cKDTree(P)
    names = ['root'] + [n for n, _, _ in DOOR_BONES]
    counts = {n: 0 for n in names}
    rows = []
    for mat, vs in tris:
        d, i = tree.query(np.mean([p for p, n, uv in vs], axis=0), k=2)
        b = which[i[0]] if d[0] < MATCH and which[i[0]] else 'root'
        if d[1] < MATCH and which[i[0]] and which[i[1]] and which[i[0]] != which[i[1]]:
            # the leaves' end faces where they meet lie in one plane (mesh x = 0): the side a face looks to says
            # whose it is - the left leaf (mesh x > 0) looks across to -x
            b = 'door_left' if np.mean([n[0] for p, n, uv in vs]) < 0 else 'door_right'
        counts[b] += 1
        rows.append((mat, [(names.index(b), p, n, uv) for p, n, uv in (vs[0], vs[2], vs[1])]))   # compiled winding -> SMD
    if counts['door_left'] < 100 or counts['door_right'] < 100:     # triangles
        raise SystemExit('%s: only %d / %d door vertices matched - is this the same mesh?' % (var, counts['door_left'], counts['door_right']))
    nodes = 'nodes\n0 "root" -1\n' + ''.join('%d "%s" 0\n' % (k + 1, n) for k, (n, _, _) in enumerate(DOOR_BONES)) + 'end\n'
    def skel(angle):
        s = 'skeleton\ntime 0\n0 0 0 0 0 0 0\n'
        for k, (n, _, sign) in enumerate(DOOR_BONES):
            h = hinge[n]; s += '%d %.6f %.6f %.6f 0 0 %.6f\n' % (k + 1, h[0], h[1], h[2], np.radians(sign * angle))
        return s + 'end\n'
    # in the body the door vertices are in MODEL space (SMD triangles always are); the bones carry them
    with open(os.path.join(out, var + '_body.smd'), 'w', newline='\n') as f:
        f.write('version 1\n' + nodes + skel(0.0) + 'triangles\n')
        for mat, vs in rows:
            f.write(mat + '\n')
            for b, p, n, uv in vs:
                f.write('%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f 1 %d 1\n' % (b, p[0], p[1], p[2], n[0], n[1], n[2], uv[0], 1.0 - uv[1], b))
        f.write('end\n')
    for tag, ang in (('closed', 0.0), ('open', open_deg)):
        open(os.path.join(out, '%s_%s.smd' % (var, tag)), 'w', newline='\n').write('version 1\n' + nodes + skel(ang))
    shutil.copy(os.path.join(decomp, phys), os.path.join(out, var + '_physics.smd'))
    # the port's own QC settings (Crowbar's decompile), mesh and sequence swapped
    src = os.path.join(decomp, var + '.qc')
    if not os.path.exists(src): src = os.path.join(decomp, 'halftrack.qc')
    qc = open(src, encoding='latin1').read().replace('\r\n', '\n')
    qc = re.sub(r'\$modelname "[^"]+"', '$modelname "cod2/vehicles/%s.mdl"' % var, qc)
    qc = re.sub(r'studio "[^"]+"', 'studio "%s_body.smd"' % var, qc)
    qc = re.sub(r'\$sequence "idle" \{.*?\n\}', '$poseparameter "doors" 0 1\n'
                '$animation "doors_closed" "%s_closed.smd" { fps 1 }\n$animation "doors_open" "%s_open.smd" { fps 1 }\n'
                '$sequence "idle" {\n\t"doors_closed"\n\t"doors_open"\n\tblend "doors" 0 1\n\tfadein 0.2\n\tfadeout 0.2\n}' % (var, var),
                qc, flags=re.S)
    qc = re.sub(r'\$collisionmodel "[^"]+"', '$collisionmodel "%s_physics.smd"' % var, qc)
    qc = qc.replace('// Created by Crowbar 0.74', '// DOD-SP V3: the port with its rear doors on hinge bones (dodsp_tools/vehicle_preview/'
                    'halftrack_doors.py). Pose parameter "doors": 0 closed, 1 open (%.0f degrees).' % open_deg)
    open(os.path.join(out, var + '.qc'), 'w', newline='\n').write(qc)
    print('%s: %d triangles, door triangles left %d / right %d, hinges %s / %s' % (var, len(rows), counts['door_left'], counts['door_right'],
          hinge['door_left'].round(2), hinge['door_right'].round(2)))

if __name__ == '__main__':
    parts_path, surfs_path, decomp, models, out = sys.argv[1:6]
    open_deg = float(sys.argv[6]) if len(sys.argv) > 6 else 100.0
    os.makedirs(out, exist_ok=True)
    hinge, P, which = door_vertices(parts_path, surfs_path)
    for var, phys in VARIANTS.items():
        write_variant(var, phys, decomp, models, out, hinge, P, which, open_deg)
