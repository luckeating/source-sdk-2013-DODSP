"""THE rig tool for every CoD2 vehicle with running gear (replaces halftrack_rig.py): the port's mesh on CoD2's own bones
and vertex weights, the wheel and suspension facts measured into the model, the collision taken from the compiled .phy.

    python3 vehicle_rig.py <CoD2 pack dir> <port model dir> <out dir> [vehicle ...]

<CoD2 pack dir> holds xmodelparts/<name> + xmodelsurfs/<name> (the owner's cod2models.zip, or parts_<name> /
surfs_<name> files); <port model dir> holds the compiled ports (models/cod2/vehicles/*.mdl/.vvd/.vtx/.phy).

WHY THIS WORKS: every port is CoD2's mesh x PORT_SCALE turned into the mesh frame (mesh x = CoD y, mesh y = -CoD x) -
measured 2026-11-20, every port triangle of every vehicle here lies on a CoD triangle (median miss 0.03-0.05 units).
So each port corner takes the bones and weights of the matching corner of CoD's own model.

CoD2 RIGGED ITS RUNNING GEAR THE SAME WAY ON EVERY VEHICLE (2026-11-20 survey):
    tag_wheel_<front|middle|back>_<side>   suspension units, ON THE GROUND (z 0) under their wheels -> susp_<side>_<pos>
    tag_<side>_wheel_NN                    wheels: road wheels are children of their suspension unit; the drive
                                           sprocket and idler (first and last) sit on tag_origin   -> wheel_<side>_NN
    tag_wheel_upper_<front|middle>_<side>  the track's top run where it rests on the road wheels, a child of their
                                           unit (rises with them)                                  -> track_<side>_upper_<pos>
    the track belt                         skinned: bottom-run vertices on the suspension units, blended between them
                                           and tag_origin at the ends (max 3 bones a vertex)
    body_roll_jnt / tag_body / the rest    the hull                                                  -> body
    tag_origin                             the unsprung chassis (track ends, sprocket, idler)         -> root
    hatch_open_left/right (halftrack)      the rear doors                                            -> door_left/right
The halftrack's tag_wheel_front_<side> is its steered tyre's suspension (wheel_<side>_01 steers).

THE FACTS IN THE MODEL ($keyvalues "dodsp_wheels", read by game/shared/dodsp/dodsp_vehicle_wheels.cpp):
    "susp_<side>_<pos>"  "up <u> down <d>"   a suspension unit: the client puts it on the ground under it every frame,
                         within u up / d down; its wheels and its part of the track go with it. u = the room between the
                         top of its wheels and whatever does NOT move with it above them (hull, wings, the track's fixed
                         top run), measured on the mesh, less SUSP_MARGIN; d = u (no mesh says how far a wheel hangs).
    "wheel_<side>_NN"    "radius <r> [steer 1]"   r = the wheel's own radius about its axle; steer = turns with steering
    "wheelbase"          steered vehicles: front axle to the middle of the ground-touching wheels behind it
    "body_pivot"/"body_bone"   sprung bodies (the halftrack only; tanks keep the server's whole-hull lurch)
Every bone has no rotation at rest, so a wheel's axle is the model's left axis and every vertex is written in model space.

Writes per variant: <variant>.qc, <variant>_body.smd, <variant>_idle.smd (or the halftrack's door frames),
<variant>_physics.smd. Same $modelname as the port: the compiled model replaces it in place.
"""
import os, re, sys, numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from editor_models import mesh
from cod2_xmodel import parts, surfs
from phy_fit import write_from_phy, phy_pieces, phy_settings, read_physics_smd, G_from_mesh

PORT_SCALE = 1.0284           # measured on the halftrack (median miss 0.004); the tanks fit it to 0.03-0.05
MATCH = 0.1                   # units: a port triangle's middle this close to a CoD triangle's middle is that triangle
SUSP_MARGIN = 0.5             # units kept between a risen wheel and what's over it
SUSP_MAX = 12.0               # units: no suspension unit travels further than this
GROUND_SLACK = 4.0            # a wheel whose lowest point is this close to the lowest wheel point touches the ground
DOOR_OPEN = 100.0             # degrees, the halftrack's doors at "doors" = 1
# a variant whose running gear is another's under extra loose dressing takes that one's suspension travel: the brush
# Tiger's foliage hangs over its tracks (measured 1.9 / 6.9 / 2.9 against the leaves vs the Tiger's 6.2 / 10.3 / 6.3)
SUSP_FROM = {'tiger_brush_hull': 'tiger_hull'}
MEASURED = {}                 # variant -> its suspension facts, for SUSP_FROM

# rig name: (CoD2 model, steers, has doors, [port variants])
VEHICLES = {
    'panzer2':        ('vehicle_german_panzer_21', False, False, ['panzer2_hull', 'panzer2_winter_hull']),
    'tiger':          ('tiger_i1',                 False, False, ['tiger_hull']),
    'tiger_brush':    ('tiger_i_brush1',           False, False, ['tiger_brush_hull']),
    'sherman':        ('vehicle_sherman_tank1',    False, False, ['sherman_hull']),
    'sherman_desert': ('vehicle_sherman_desert1',  False, False, ['sherman_desert_hull']),
    'crusader':       ('vehicle_crusader21',       False, False, ['crusader_hull']),
    'carrier':        ('universal_carrier_new1',   False, False, ['carrier', 'carrier_desert']),
    'halftrack':      ('halftrack_mg1',            True,  True,  ['halftrack', 'halftrack_desert', 'halftrack_snow',
                                                                  'halftrack_rockets', 'halftrack_rockets_snow']),
}

def cod_to_mesh(p): return PORT_SCALE * np.array([p[1], -p[0], p[2]])
def mesh_to_game(p): return np.array([-p[1], p[0], p[2]])

def our_bone(name, doors):
    m = re.match(r'tag_wheel_(front|middle|back)_(left|right)$', name)
    if m: return 'susp_%s_%s' % (m.group(2), m.group(1))
    # the track's top run: where it RESTS on the road wheels (Tiger, carrier, halftrack) CoD hangs it on their unit, so it
    # rises with them; where it runs on return rollers (Sherman, Panzer II, Crusader) CoD has no such bone and the top
    # run stays on tag_origin. Kept exactly as CoD has it (tried on the chassis 2026-11-20: the Tiger's and halftrack's
    # wheels then sat jammed under a fixed top run, 0 travel).
    m = re.match(r'tag_wheel_upper_(front|middle)_(left|right)$', name)
    if m: return 'track_%s_upper_%s' % (m.group(2), m.group(1))
    m = re.match(r'tag_(left|right)_wheel_(\d+)$', name)
    if m: return 'wheel_%s_%02d' % (m.group(1), int(m.group(2)))
    if doors and name in ('hatch_open_left', 'hatch_open_right'): return 'door_' + name[11:]
    if name == 'tag_origin': return 'root'
    return 'body'

def find_cod(pack, name):
    for pp, sp in ((os.path.join(pack, 'xmodelparts', name), os.path.join(pack, 'xmodelsurfs', name)),
                   (os.path.join(pack, 'parts_' + name), os.path.join(pack, 'surfs_' + name))):
        if os.path.exists(pp) and os.path.exists(sp): return pp, sp
    raise SystemExit('no CoD2 parts/surfs for %s in %s' % (name, pack))

def cod_model(pack, name, doors):
    """CoD's triangles (middle, corner positions, corner weights [(our bone, share)], average normal) and our skeleton"""
    B = parts(find_cod(pack, name)[0]); S = surfs(find_cod(pack, name)[1])
    ours = [our_bone(b['name'], doors) for b in B]
    # our skeleton: every bone CoD has that we keep, its parent = the nearest kept ancestor, rest = CoD's position
    skel = {'root': (None, np.zeros(3)), 'body': ('root', np.zeros(3))}
    for i, b in enumerate(B):
        o = ours[i]
        if o in ('root', 'body') or o in skel: continue
        if o.startswith('door'):
            par = 'body'                    # the doors hang on the hull (CoD: hatch_open_* under body_roll_jnt)
        else:
            j = b['parent']                 # the nearest ancestor that is running gear; none = the chassis
            while j >= 0 and ours[j] in ('root', 'body'): j = B[j]['parent']
            par = ours[j] if j >= 0 else 'root'
        skel[o] = (par, cod_to_mesh(b['t']))
    mids, corners, weights, normals = [], [], [], []
    for s in S:
        W = []
        P = []; N = []
        for v in s['verts']:
            b = B[v['bone']]
            P.append(cod_to_mesh(b['R'] @ v['pos'] + b['t'])); N.append(cod_to_mesh(b['R'] @ v['n']) / PORT_SCALE)
            share = {}
            rest = 1.0
            for bi, sh, _ in v['w']:
                share[ours[bi]] = share.get(ours[bi], 0.0) + sh; rest -= sh
            share[ours[v['bone']]] = share.get(ours[v['bone']], 0.0) + rest
            W.append(share)
        for t in s['tris']:
            mids.append(np.mean([P[i] for i in t], axis=0)); corners.append([P[i] for i in t])
            weights.append([W[i] for i in t]); normals.append(np.mean([N[i] for i in t], axis=0))
    return skel, np.array(mids), corners, weights, np.array(normals)

def three(share):
    """at most 3 bones a vertex (Source), largest shares, renormalised"""
    top = sorted(share.items(), key=lambda kv: -kv[1])[:3]
    tot = sum(w for _, w in top)
    return [(b, w / tot) for b, w in top if w / tot > 0.001]

def rig_mesh(tris, cod):
    from scipy.spatial import cKDTree
    skel, mids, corners, weights, normals = cod
    tree = cKDTree(mids)
    rows, unmatched = [], 0
    for mat, vs in tris:
        P = np.array([p for p, n, uv in vs]); n_port = np.mean([n for p, n, uv in vs], axis=0)
        cand = tree.query_ball_point(P.mean(0), MATCH)
        if not cand:
            unmatched += 1
            rows.append((mat, [(p, n, uv, [('body', 1.0)]) for p, n, uv in vs])); continue
        # where two CoD triangles share a spot (the halftrack's door leaves meet face to face), the one facing the same way
        k = max(cand, key=lambda c: np.dot(normals[c], n_port))
        out = []
        for p, n, uv in vs:
            j = int(np.argmin([np.linalg.norm(p - q) for q in corners[k]]))
            out.append((p, n, uv, three(weights[k][j])))
        rows.append((mat, out))
    return rows, unmatched

def primary(w): return max(w, key=lambda bw: bw[1])[0]

def descendants(skel, b):
    out = {b}
    grew = True
    while grew:
        grew = False
        for n, (par, _) in skel.items():
            if par in out and n not in out: out.add(n); grew = True
    return out

def facts(rows, skel, steers):
    """wheel radii, suspension travel, wheelbase/pivot - from the port's own vertices"""
    verts = {}
    for mat, vs in rows:
        for p, n, uv, w in vs:
            verts.setdefault(primary(w), []).append(p)
    verts = {k: np.array(v) for k, v in verts.items()}
    wheels = sorted(n for n in skel if n.startswith('wheel_'))
    out_w, low = {}, {}
    for name in wheels:
        V = verts.get(name)
        if V is None or len(V) < 12: raise SystemExit('%s: %d vertices - is this the same mesh?' % (name, 0 if V is None else len(V)))
        c = skel_world(skel, name)
        r = float(np.sqrt((V[:, 1] - c[1]) ** 2 + (V[:, 2] - c[2]) ** 2).max())
        out_w[name] = (r, steers and name.endswith('_01'))
        low[name] = V[:, 2].min()
    # suspension travel: how far a unit can rise before anything that moves with it (its wheels, the track belt where
    # it rides the unit) meets what never moves (triangles weighted only to the chassis / hull / doors - the fixed top
    # run of the belt included): a ray straight up from every moving corner against every fixed triangle. Belt that is
    # SHARED with a unit bends, so it is neither; vertices that ride OTHER units are ignored (they move on their own).
    fixed_set = {'root', 'body', 'door_left', 'door_right'}
    treads = tread_materials()
    fixed_tris = np.array([[p for p, n, uv, w in vs] for mat, vs in rows
                           if all(set(dict(w)) <= fixed_set for p, n, uv, w in vs)])
    out_s = {}
    for su in sorted(n for n in skel if n.startswith('susp_')):
        moving = descendants(skel, su)
        mov = np.array([p for mat, vs in rows for p, n, uv, w in vs
                        if sum(x for b, x in w if b in moving) >= (0.5 if mat.lower() in treads else 0.01)])
        if not len(mov): raise SystemExit('%s carries nothing' % su)
        gap, where = ray_up(mov, fixed_tris)
        up = float(np.clip(gap - SUSP_MARGIN, 0.0, SUSP_MAX))
        out_s[su] = (up, up, gap, where)
    wb = pivot = None
    if steers:
        ground = min(low.values())
        front = np.mean([-skel_world(skel, n)[1] for n in wheels if out_w[n][1]])
        rear = [-skel_world(skel, n)[1] for n in wheels if not out_w[n][1] and low[n] <= ground + GROUND_SLACK]
        wb, pivot = float(front - np.mean(rear)), float(np.mean(rear))
    return out_w, out_s, wb, pivot

def tread_materials():
    """THE list of track-belt materials (dodsp_treads.txt, measured by tread_scroll.py)"""
    here = os.path.dirname(os.path.abspath(__file__))
    t = open(os.path.join(here, '..', 'content', 'scripts', 'dodsp_treads.txt')).read()
    return {m.lower() for m in re.findall(r'^\s*"([^"]+)"\s*\{', t, re.M)}

def ray_up(P, T):
    """the shortest vertical distance from any point of P up to any triangle of T, and the point it starts from"""
    if not len(T): return SUSP_MAX + SUSP_MARGIN, None
    lo, hi = P[:, :2].min(0) - 1, P[:, :2].max(0) + 1
    T = T[(T[:, :, 0].max(1) >= lo[0]) & (T[:, :, 0].min(1) <= hi[0]) & (T[:, :, 1].max(1) >= lo[1]) & (T[:, :, 1].min(1) <= hi[1])]
    best, where = SUSP_MAX + SUSP_MARGIN, None
    if not len(T): return best, where
    a, b, c = T[:, 0], T[:, 1], T[:, 2]
    d0 = (b[:, 1] - c[:, 1]) * (a[:, 0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (a[:, 1] - c[:, 1])
    ok = np.abs(d0) > 1e-9
    a, b, c, d0 = a[ok], b[ok], c[ok], d0[ok]
    for p in P:
        l1 = ((b[:, 1] - c[:, 1]) * (p[0] - c[:, 0]) + (c[:, 0] - b[:, 0]) * (p[1] - c[:, 1])) / d0
        l2 = ((c[:, 1] - a[:, 1]) * (p[0] - c[:, 0]) + (a[:, 0] - c[:, 0]) * (p[1] - c[:, 1])) / d0
        l3 = 1 - l1 - l2
        inside = (l1 >= 0) & (l2 >= 0) & (l3 >= 0)
        if not inside.any(): continue
        z = l1[inside] * a[inside, 2] + l2[inside] * b[inside, 2] + l3[inside] * c[inside, 2] - p[2]
        z = z[z > 0.01]
        if len(z) and z.min() < best: best, where = float(z.min()), p
    return best, where

def skel_world(skel, n):
    return skel[n][1]                               # rest positions are stored in model space

def skeleton_text(skel, order, door_deg=0.0):
    s = 'skeleton\ntime 0\n'
    for k, n in enumerate(order):
        par, p = skel[n]
        loc = p - (skel[par][1] if par else 0)
        rz = np.radians(door_deg * (-1.0 if n == 'door_left' else 1.0)) if n.startswith('door') else 0.0
        s += '%d %.6f %.6f %.6f 0 0 %.6f\n' % (k, loc[0], loc[1], loc[2], rz)
    return s + 'end\n'

def header(mdl):
    import struct
    d = open(mdl, 'rb').read()
    I = lambda o: struct.unpack_from('<i', d, o)[0]; F = lambda o: np.array(struct.unpack_from('<3f', d, o))
    cs = lambda o: d[o:d.index(b'\0', o)].decode('latin1')
    return dict(name=cs(12), illum=F(92), hmin=F(104), hmax=F(116), surfaceprop=cs(I(308)), skinfam=I(224))

def write_variant(rig, var, ports, out, cod, steers, doors):
    mdl = os.path.join(ports, var + '.mdl'); phy = os.path.join(ports, var + '.phy')
    h = header(mdl)
    if h['skinfam'] != 1: raise SystemExit('%s has %d skin families - carry them into the QC first' % (var, h['skinfam']))
    tris, cds = mesh(mdl)
    rows, unmatched = rig_mesh(tris, cod)
    skel = cod[0]
    used = {b for mat, vs in rows for p, n, uv, w in vs for b, _ in w}
    # keep every bone that carries a vertex, every bone on the way to it, and every suspension unit
    keep = set()
    for b in list(used) + [n for n in skel if n.startswith('susp_')]:
        while b:
            keep.add(b); b = skel[b][0]
    order = ['root'] + sorted((n for n in keep if n != 'root'), key=lambda n: (depth(skel, n), n))
    wf, sf, wb, pivot = facts(rows, skel, steers)
    MEASURED[var] = sf
    if var in SUSP_FROM:
        if SUSP_FROM[var] not in MEASURED: raise SystemExit('%s takes its travel from %s - rig that first' % (var, SUSP_FROM[var]))
        sf = MEASURED[SUSP_FROM[var]]
    wf = {k: v for k, v in wf.items() if k in keep}
    idx = {n: k for k, n in enumerate(order)}
    nodes = 'nodes\n' + ''.join('%d "%s" %d\n' % (k, n, idx[skel[n][0]] if skel[n][0] else -1) for k, n in enumerate(order)) + 'end\n'
    with open(os.path.join(out, var + '_body.smd'), 'w', newline='\n') as f:
        f.write('version 1\n' + nodes + skeleton_text(skel, order) + 'triangles\n')
        for mat, vs in rows:
            f.write(mat + '\n')
            for p, n, uv, w in (vs[0], vs[2], vs[1]):         # compiled winding -> SMD
                f.write('%d %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f %d %s\n' % (
                    idx[primary(w)], p[0], p[1], p[2], n[0], n[1], n[2], uv[0], 1.0 - uv[1], len(w),
                    ' '.join('%d %.6f' % (idx[b], x) for b, x in w)))
        f.write('end\n')
    if doors:
        for tag, ang in (('closed', 0.0), ('open', DOOR_OPEN)):
            open(os.path.join(out, '%s_%s.smd' % (var, tag)), 'w', newline='\n').write('version 1\n' + nodes + skeleton_text(skel, order, ang))
        seq = ('$poseparameter "doors" 0 1\n$animation "doors_closed" "%s_closed.smd" { fps 1 }\n$animation "doors_open" "%s_open.smd" { fps 1 }\n'
               '$sequence "idle" {\n\t"doors_closed"\n\t"doors_open"\n\tblend "doors" 0 1\n\tfadein 0.2\n\tfadeout 0.2\n}\n' % (var, var))
    else:
        open(os.path.join(out, var + '_idle.smd'), 'w', newline='\n').write('version 1\n' + nodes + skeleton_text(skel, order))
        seq = '$sequence "idle" "%s_idle.smd" { fps 1 }\n' % var
    # collision: the compiled .phy itself (phy_fit.py: a decompile's collision may be turned; the .phy can't be)
    pieces = write_from_phy(phy, os.path.join(out, var + '_physics.smd'))
    _, ptris = read_physics_smd(os.path.join(out, var + '_physics.smd'))
    from scipy.spatial import cKDTree
    W = np.array([p for _, vs in ptris for p, n, r in vs]) @ G_from_mesh().T
    Q = np.concatenate(phy_pieces(phy))
    miss = max(cKDTree(Q).query(W)[0].max(), cKDTree(W).query(Q)[0].max())
    if miss > 0.5: raise SystemExit('%s: the written collision misses the compiled one by %.2f' % (var, miss))
    ps = phy_settings(phy)
    il = h['illum']
    qc = ('// DOD-SP: %s rigged by dodsp_tools/vehicle_preview/vehicle_rig.py (CoD2 %s bones and weights on the port mesh).\n\n'
          '$modelname "%s"\n\n$bodygroup "body"\n{\n\tstudio "%s_body.smd"\n}\n\n$surfaceprop "%s"\n\n$contents "solid"\n\n'
          '$illumposition %.3f %.3f %.3f\n\n%s\n$cbox 0 0 0 0 0 0\n\n$bbox %.3f %.3f %.3f %.3f %.3f %.3f\n\n%s\n'
          '$collisionmodel "%s_physics.smd"\n{\n\t$mass %s\n\t$inertia %s\n\t$damping %s\n\t$rotdamping %s\n\t$rootbone " "\n%s}\n'
          % (var, cod_name(rig), h['name'], var, h['surfaceprop'], il[1], -il[0], il[2],
             ''.join('$cdmaterials "%s"\n' % c for c in cds), *h['hmin'], *h['hmax'], seq, var,
             fmt(ps.get('mass', '1000')), fmt(ps.get('inertia', '1')), fmt(ps.get('damping', '0')), fmt(ps.get('rotdamping', '0')),
             '\t$concave\n' if pieces > 1 else ''))
    kv = '\n$keyvalues\n{\n\t"dodsp_wheels"\n\t{\n'
    if wb is not None:
        kv += '\t\t"wheelbase"\t"%.1f"\n\t\t"body_pivot"\t"%.1f"\n\t\t"body_bone"\t"body"\n' % (wb, pivot)
    for su in sorted(n for n in keep if n.startswith('susp_')):
        kv += '\t\t"%s"\t"up %.1f down %.1f"\n' % (su, sf[su][0], sf[su][1])
    for name in sorted(wf):
        r, st = wf[name]
        kv += '\t\t"%s"\t"radius %.1f%s"\n' % (name, r, ' steer 1' if st else '')
    qc += kv + '\t}\n}\n'
    open(os.path.join(out, var + '.qc'), 'w', newline='\n').write(qc)
    counts = {}
    for mat, vs in rows:
        for p, n, uv, w in vs: counts[primary(w).split('_')[0]] = counts.get(primary(w).split('_')[0], 0) + 1
    blended = sum(1 for mat, vs in rows for p, n, uv, w in vs if len(w) > 1)
    print('%s: %d triangles (%d not on a CoD triangle -> body), %d bones, %d blended corners; collision %d piece(s), miss %.3f' % (
        var, len(rows), unmatched, len(order), blended, pieces, miss))
    print('    suspension: ' + ', '.join('%s up %.1f' % (k[5:], v[0]) + ('' if v[3] is None else ' (at %s)' % mesh_to_game(v[3]).round(0).astype(int).tolist())
                                       for k, v in sorted(sf.items()) if k in keep and 'left' in k))
    print('    wheels: ' + ', '.join('%s r%.1f%s' % (k[6:], v[0], ' steer' if v[1] else '') for k, v in sorted(wf.items()) if 'left' in k)
          + ('; wheelbase %.1f, body pivot %.1f' % (wb, pivot) if wb is not None else ''))
    return order, wf, sf

def fmt(v): return ('%.3f' % float(v)).rstrip('0').rstrip('.')
def cod_name(rig): return VEHICLES[rig][0]
def depth(skel, n):
    d = 0
    while skel[n][0]: n = skel[n][0]; d += 1
    return d

if __name__ == '__main__':
    pack, ports, out = sys.argv[1:4]
    which = sys.argv[4:] or list(VEHICLES)
    os.makedirs(out, exist_ok=True)
    for rig in which:
        name, steers, doors, variants = VEHICLES[rig]
        cod = cod_model(pack, name, doors)
        for var in variants:
            write_variant(rig, var, ports, out, cod, steers, doors)
