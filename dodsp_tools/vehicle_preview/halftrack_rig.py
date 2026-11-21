"""The halftrack's moving parts: rear doors on hinge bones (V3) and every wheel on its own bone (H1).

    python3 halftrack_rig.py <CoD2 xmodelparts> <CoD2 xmodelsurfs> <decompile dir> <vehicle model dir> <out dir> [open degrees]

The port (models/cod2/vehicles/halftrack*.mdl) has the whole halftrack on one bone. CoD2's own model (xmodelparts +
xmodelsurfs "halftrack_mg1", version 20) has the moving parts as their own surfaces on their own bones: the two door
leaves on hatch_open_left / hatch_open_right (the hinges), and every wheel on tag_<side>_wheel_01..09 (01 the steered
front tyre, 02 the drive sprocket, 03..08 the road wheels, 09 the idler). The port is that mesh x PORT_SCALE in the
mesh frame (mesh x = CoD y, mesh y = -CoD x), so each port triangle takes the bone of the CoD triangle at the same spot
(measured 2026-11-20: all 5928 port triangles match one). Everything else is the port's own: its materials, UVs,
normals, collision (<variant>_physics.smd from the owner's Crowbar decompile) and QC settings. CoD's track-run strips
(tag_wheel_back/middle/upper_*) are track, not wheels: they stay on the root.

The wheel facts ride IN THE MODEL ($keyvalues "dodsp_wheels"), measured here from the mesh - one owner, read by the
client (spin, steer) and the server (wheelbase -> steering angle):
    "wheelbase"  front axle to the middle of the ground-touching wheels behind it (units, along the vehicle)
    "body_pivot" where the body tips on its springs: that rear middle, game x (forward) from the model origin - the
                 body squats about its rear running gear, the wheels stay on the ground (client: H1 suspension)
    "body_bone"  the sprung body: that bone and its children tip (CoD2's body_roll_jnt hull + the doors); the root
                 (tag_origin's tread belt + the track-run strips) and the wheels are the unsprung running gear
    "<bone>"     "radius <r> [steer 1 clear <c>]"  r = the wheel's own radius (largest vertex distance from its
                                         centre, about the axle); steer = it turns with the steering; clear = the gap
                                         from its top to the wing over it (a tyre following the ground never rises
                                         into its wing: the client allows clear minus however far the body has come
                                         down there)
Writes per variant: <variant>.qc + <variant>_body.smd, <variant>_closed.smd and <variant>_open.smd (one frame each),
blended by the pose parameter "doors" (0 closed .. 1 open) - the game sets it. Same $modelname as the port, so the
compiled model replaces it in place.
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
# name, CoD bone, steers. "wheel" in the name is what the client's wheel list keys on.
WHEEL_BONES = tuple(('wheel_%s_%02d' % (side, n), 'tag_%s_wheel_%02d' % (side, n), n == 1)
                    for side in ('left', 'right') for n in range(1, 10))
BODY_COD = 'body_roll_jnt'    # CoD2's own sprung body bone: the hull rides on it, the tread belt and wheels don't
MATCH = 0.05                  # a port triangle whose middle is this close to a CoD triangle's is on that triangle's bone
GROUND_SLACK = 4.0            # a wheel whose lowest point is this close to the lowest wheel point touches the ground

def cod_to_mesh(p): return PORT_SCALE * np.array([p[1], -p[0], p[2]])

def cod_triangles(parts_path, surfs_path):
    """every CoD triangle's middle (mesh frame) and the bone most of its corners ride; the moving bones' pivots"""
    B = parts(parts_path); S = surfs(surfs_path)
    pivot = {}
    for name, cod, _ in DOOR_BONES + WHEEL_BONES:
        bi = [i for i, b in enumerate(B) if b['name'] == cod]
        if not bi: raise SystemExit('no %s in %s' % (cod, parts_path))
        pivot[name] = cod_to_mesh(B[bi[0]]['t'])
    ours = {cod: name for name, cod, _ in DOOR_BONES + WHEEL_BONES}
    ours[BODY_COD] = 'body'             # CoD's sprung body; tag_origin + the track-run strips = the unsprung chassis
    # per TRIANGLE (the two door leaves share vertex positions where they meet in the middle, so a vertex alone can't
    # say which door it is on): a triangle is on a bone when most of its corners ride that bone in CoD's model
    pts, which = [], []
    for s in S:
        for t in s['tris']:
            vs = [s['verts'][i] for i in t]
            bn = [B[v['bone']]['name'] for v in vs]
            top = max(set(bn), key=bn.count)
            pts.append(np.mean([cod_to_mesh(B[v['bone']]['R'] @ v['pos'] + B[v['bone']]['t']) for v in vs], axis=0))
            which.append(ours.get(top) if bn.count(top) >= 2 else None)
    return pivot, np.array(pts), which

def wheel_facts(rows, names, pivot):
    """radius per wheel (about the axle = mesh x) and the wheelbase, measured from the port's own triangles"""
    verts = {n: [] for n, _, _ in WHEEL_BONES}
    for mat, vs in rows:
        for b, p, n, uv in vs:
            if names[b] in verts: verts[names[b]].append(p)
    hull = np.array([p for mat, vs in rows for b, p, n, uv in vs if names[b] == 'body'])
    facts, low = {}, {}
    for name, _, steer in WHEEL_BONES:
        V = np.array(verts[name]); c = pivot[name]
        if len(V) < 12: raise SystemExit('%s: only %d vertices - is this the same mesh?' % (name, len(V)))
        r = float(np.sqrt((V[:, 1] - c[1]) ** 2 + (V[:, 2] - c[2]) ** 2).max())
        clear = 0.0
        if steer:
            # the hull over the tyre's footprint (mesh x = across, y = along), above its top
            top = V[:, 2].max()
            over = hull[(np.abs(hull[:, 1] - c[1]) < r * 0.8) & (np.abs(hull[:, 0] - c[0]) < np.ptp(V[:, 0]) / 2) & (hull[:, 2] > top)]
            clear = float(over[:, 2].min() - top) if len(over) else 0.0
            if clear < 1.0: raise SystemExit('%s: no room over the tyre (%.1f) - is this the same mesh?' % (name, clear))
        facts[name] = (r, steer, clear)
        low[name] = V[:, 2].min()
    ground = min(low.values())
    # along the vehicle = game x = -mesh y
    front = np.mean([-pivot[n][1] for n, _, s in WHEEL_BONES if s])
    rear = [-pivot[n][1] for n, _, s in WHEEL_BONES if not s and low[n] <= ground + GROUND_SLACK]
    if not rear: raise SystemExit('no ground-touching wheel behind the steered ones')
    return facts, float(front - np.mean(rear)), len(rear), float(np.mean(rear))

def write_variant(var, phys, decomp, models, out, pivot, P, which, open_deg):
    from scipy.spatial import cKDTree
    tris, cds = mesh(os.path.join(models, var + '.mdl'))
    tree = cKDTree(P)
    # root = the unsprung chassis (tread belt, track-run strips), body = the sprung hull with the doors on it, wheels
    bones = ['body'] + [n for n, _, _ in DOOR_BONES] + [n for n, _, _ in WHEEL_BONES]
    names = ['root'] + bones
    parent = {n: (1 if n.startswith('door') else 0) for n in bones}
    counts = {n: 0 for n in names}
    rows = []
    for mat, vs in tris:
        d, i = tree.query(np.mean([p for p, n, uv in vs], axis=0), k=2)
        # a triangle CoD has on tag_origin / the strips is chassis; one the port has and CoD hasn't (the rocket racks) is hull
        b = (which[i[0]] or 'root') if d[0] < MATCH else 'body'
        if d[1] < MATCH and which[i[0]] in ('door_left', 'door_right') and which[i[1]] in ('door_left', 'door_right') \
                and which[i[0]] != which[i[1]]:
            # the leaves' end faces where they meet lie in one plane (mesh x = 0): the side a face looks to says
            # whose it is - the left leaf (mesh x > 0) looks across to -x
            b = 'door_left' if np.mean([n[0] for p, n, uv in vs]) < 0 else 'door_right'
        counts[b] += 1
        rows.append((mat, [(names.index(b), p, n, uv) for p, n, uv in (vs[0], vs[2], vs[1])]))   # compiled winding -> SMD
    if counts['door_left'] < 100 or counts['door_right'] < 100:     # triangles
        raise SystemExit('%s: only %d / %d door triangles matched - is this the same mesh?' % (var, counts['door_left'], counts['door_right']))
    facts, wheelbase, nrear, body_pivot = wheel_facts(rows, names, pivot)
    nodes = 'nodes\n0 "root" -1\n' + ''.join('%d "%s" %d\n' % (k + 1, n, parent[n]) for k, n in enumerate(bones)) + 'end\n'
    swing = {n: s for n, _, s in DOOR_BONES}
    def skel(angle):
        s = 'skeleton\ntime 0\n0 0 0 0 0 0 0\n'
        for k, n in enumerate(bones):
            # the body sits at the origin unturned, so its children's local positions are model positions
            h = pivot.get(n, np.zeros(3)); s += '%d %.6f %.6f %.6f 0 0 %.6f\n' % (k + 1, h[0], h[1], h[2], np.radians(swing.get(n, 0.0) * angle))
        return s + 'end\n'
    # in the body the vertices are in MODEL space (SMD triangles always are); the bones carry them
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
    # the port's own QC settings (Crowbar's decompile), mesh and sequence swapped, the wheel facts added
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
    if '$keyvalues' in qc: raise SystemExit('%s: the port QC already has $keyvalues - merge dodsp_wheels into it' % src)
    kv = ('\n$keyvalues\n{\n\t"dodsp_wheels"\n\t{\n\t\t"wheelbase"\t"%.1f"\n\t\t"body_pivot"\t"%.1f"\n\t\t"body_bone"\t"body"\n'
          % (wheelbase, body_pivot))
    for name, _, _ in WHEEL_BONES:
        r, steer, clear = facts[name]
        kv += '\t\t"%s"\t"radius %.1f%s"\n' % (name, r, ' steer 1 clear %.1f' % clear if steer else '')
    qc += kv + '\t}\n}\n'
    qc = qc.replace('// Created by Crowbar 0.74', '// DOD-SP H1: the port with its rear doors on hinge bones and every wheel on its own bone '
                    '(dodsp_tools/vehicle_preview/halftrack_rig.py). Pose parameter "doors": 0 closed, 1 open (%.0f degrees). '
                    'Wheel sizes and steering in $keyvalues "dodsp_wheels".' % open_deg)
    open(os.path.join(out, var + '.qc'), 'w', newline='\n').write(qc)
    print('%s: %d triangles, chassis %d, body %d, doors %d / %d, wheels %d (%s), wheelbase %.1f over %d ground wheels, body pivot x %.1f' % (
        var, len(rows), counts['root'], counts['body'], counts['door_left'], counts['door_right'], sum(counts[n] for n, _, _ in WHEEL_BONES),
        ', '.join('%s r%.1f' % (n[6:], facts[n][0]) for n, _, _ in WHEEL_BONES[:9]), wheelbase, nrear, body_pivot))
    return facts, wheelbase

if __name__ == '__main__':
    parts_path, surfs_path, decomp, models, out = sys.argv[1:6]
    open_deg = float(sys.argv[6]) if len(sys.argv) > 6 else 100.0
    os.makedirs(out, exist_ok=True)
    pivot, P, which = cod_triangles(parts_path, surfs_path)
    for var, phys in VARIANTS.items():
        write_variant(var, phys, decomp, models, out, pivot, P, which, open_deg)
