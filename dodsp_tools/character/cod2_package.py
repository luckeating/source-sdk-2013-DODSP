"""A converted CoD2-skeleton character as a Crowbar-ready source pack: SMDs + QC + VTF/VMT, everything that isn't the
mesh copied from OUR soldier's COMPILED model (the owner's npc_german_rifleman .mdl/.phy - read with mdl48.py), so the
new body behaves like the old one in game: same includes (animations), attachments, IK chains, hitbox sets, ragdoll
(hulls + joint limits + masses), bodygroups (body, helmet with a blank), surfaceprop. Only the eyes attachment moves:
to CoD's own tag_eye on this head, with the soldier's eye turn.

    python3 -I cod2_package.py <pack x dir> <xanim_map.npz> <dod reference smd> <soldier .mdl (with its .phy)>
                               <out dir> <model name> <helmet xmodel> <xmodel> [<xmodel> ...]

    model name e.g. german/npc_german_cod3_rifleman  ->  models/german/npc_german_cod3_rifleman.mdl
Out: <out>/modelsrc/<base>/ (QC, body.smd, helmet.smd, physics.smd), <out>/materials/models/dodsp/cod3/ (VTF, VMT)."""
import os, re, sys, struct, json
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE]
import cod2_char as C, cod2_assets as A, normal_seams as NS, joint_volume as JV, layered_poses as LP, mdl48, vtf
from scipy import ndimage as nd

MATDIR = 'models/dodsp/cod3'
JV_WORSE_TOL = 0.02     # a joint helper may not leave any joint thinner than without it (median or worst 10% of poses)
JV_POSE_WORSE = 0.05    # a HELD pose (stance(): his weapons' stance files + every aim-layer extreme) that ends thinner by more
                        # than this AND under JV_BAR is listed in the report and printed - the first joint build passed the
                        # pooled check and made the crouched knee worse (0.22 -> 0.06), and nothing said so
JV_POSE_DROP_MAX = 0.2  # ...and none may lose more than this: the build stops (every design tried leaves the elbow's few
                        # already-collapsed extreme poses 0.1-0.16 lower - measured, listed, not hidden)
# How strongly a normal map tilts the surface (its x/y scaled, then renormalised), per map - the one owner.
# The owner's in-game isolation 2026-11-21: black streaks on the tunic folds with the normal maps on (same with our
# per-pixel sun off), gone with mat_bumpmap 0; the colour map and the compiled tangents are clean. CoD3's tunic map is
# strong (tilt median 0.37, 10% of texels past 0.83) and Source's ambient cube + sun turn its creases near black.
# How strong a map's relief is, matched to a Source character map that draws well (2026-11-21): the tilt that 90% of
# island texels stay under. HL2's citizen_sheet_normal: 23.2 deg (median 4.2). DoD's german_body_normal is far flatter
# (3.5). CoD3's maps, read right, were 43-51 (tunic median 20.6, 9% past 45 deg - 5x the citizen's): under Source's
# ambient cube + sun that drew every fold as a dark band (owner's screenshots after the sign fix). Each map's slopes are
# scaled so its 90% tilt lands here - one rule for every map, not a hand value per file.
NORMAL_TILT90_TARGET = 23.2

def relief_scale(x, y, island):
    """the slope scale that puts the map's 90th-percentile tilt (slope reading: tilt = atan |(x, y)|) on the target"""
    r90 = float(np.percentile(np.hypot(x, y)[island], 90))
    return min(1.0, np.tan(np.radians(NORMAL_TILT90_TARGET)) / max(r90, 1e-6)), np.degrees(np.arctan(r90))

# Signs read by eye where the mesh's seams can't decide (normal_seams.decide_signs): a feature of known shape in the
# map integrated to a height field. The helmet's normal map is a sibling helmet's (helm2 on helm1's colour), so its
# seams don't line up (disagreement ~1.3 for every choice). Its vent lug - a raised ring with a hole - comes out right
# only with green flipped.
NORMAL_SIGNS_CHECKED = {'c_wrmcht2_helm1_n': ((1, -1), 'helmet vent lug = raised ring with a hole')}

def check_normal_vtf(path, w, h):
    """Read a written BGR888 normal map back: every texel of the full mip a unit normal (8-bit rounding) no steeper
    than the slope reading allows. A z = sqrt(1 - r^2) decode puts z = 0 texels in (black creases) and fails here.
    -> the steepest tilt in degrees."""
    d = open(path, 'rb').read()
    top = np.frombuffer(d[len(d) - w * h * 3:], np.uint8).reshape(h, w, 3)[..., ::-1].astype(np.float32) / 127.5 - 1
    L = np.linalg.norm(top, axis=-1)
    if np.abs(L - 1).max() > 0.02 or top[..., 2].min() < A.SLOPE_MIN_Z - 0.01:
        raise SystemExit('%s: normals not unit (%.3f..%.3f) or too steep (min z %.3f, slope reading >= %.3f)'
                         % (path, L.min(), L.max(), top[..., 2].min(), A.SLOPE_MIN_Z))
    return float(np.degrees(np.arccos(np.clip(top[..., 2] / L, -1, 1)).max()))

def mat_angles(m):
    """Source MatrixAngles: (pitch, yaw, roll) degrees of a 3x4 whose columns are forward, left, up"""
    f, l, u = m[:, 0], m[:, 1], m[:, 2]; xy = np.hypot(f[0], f[1])
    if xy > 1e-3: return np.degrees([np.arctan2(-f[2], xy), np.arctan2(f[1], f[0]), np.arctan2(l[2], u[2])])
    return np.degrees([np.arctan2(-f[2], xy), np.arctan2(-l[0], l[1]), 0.0])

def phy_solids(path):
    """the ragdoll: [(bone name, points (n x 3, the bone's space, game units), triangles)] + the text section"""
    d = open(path, 'rb').read()
    hsize, _, nsolid, _ = struct.unpack_from('<iiii', d, 0); o = hsize; solids = []
    for s in range(nsolid):
        ssize, = struct.unpack_from('<i', d, o); start = o + 4; cs = start + 28
        assert d[start:start + 4] == b'VPHY', 'old .phy format'
        root, = struct.unpack_from('<i', d, cs + 32); L = cs + 48; pieces = []
        while L < cs + root:
            pofs, _, bits, ntri = struct.unpack_from('<iiIH', d, L); size = (bits >> 8) * 16
            tri = []
            for t in range(ntri):
                tri.append([struct.unpack_from('<I', d, L + 16 + 16 * t + 4 + 4 * e)[0] & 0xffff for e in range(3)])
            ids = sorted({i for t in tri for i in t}); pos = {i: k for k, i in enumerate(ids)}
            P = np.array([struct.unpack_from('<fff', d, L + pofs + 16 * i) for i in ids])
            pieces.append((np.stack([P[:, 0], P[:, 2], -P[:, 1]], 1) / 0.0254, [[pos[i] for i in t] for t in tri]))
            L += size
        solids.append(pieces); o = start + ssize
    text = d[o:].decode('latin-1')
    blocks = re.findall(r'(\w+) \{(.*?)\}', text, re.S)
    info = [(k, dict(re.findall(r'"([^"]+)" "([^"]*)"', v))) for k, v in blocks]
    names = [kv['name'] for k, kv in info if k == 'solid']
    assert len(names) == len(solids), (len(names), len(solids))
    return list(zip(names, solids)), info

def write_physics(path, rig, solids):
    """every ragdoll hull on OUR reference skeleton (rigid to its bone), wound outward"""
    L = rig.local()
    out = ['version 1', 'nodes'] + ['%d "%s" %d' % (i, n, p) for i, (n, p) in enumerate(rig.nodes)] + ['end', 'skeleton', 'time 0']
    out += ['%d %.6f %.6f %.6f %.6f %.6f %.6f' % ((i,) + L[i]) for i in range(len(rig.nodes))] + ['end', 'triangles']
    for name, pieces in solids:
        bi = rig.idx[name]; R, t = rig.W[bi]
        for P, tris in pieces:
            W = P @ R.T + t; c = W.mean(0)
            for a, b, cc in tris:
                n = np.cross(W[b] - W[a], W[cc] - W[a])
                if n @ (W[a] - c) < 0: b, cc = cc, b
                out.append('phy')
                for k in (a, b, cc):
                    nn = W[k] - c; nn /= np.linalg.norm(nn) + 1e-9
                    out.append('%d %.5f %.5f %.5f %.5f %.5f %.5f 0 0 1 %d 1' % (bi, *W[k], *nn, bi))
    out.append('end'); open(path, 'w').write('\n'.join(out) + '\n')

def write_ragdoll_pose(path, rig, S):
    """the 'ragdoll' sequence = the SOLDIER's own reference pose (its compiled bones' pos/rot): the ragdoll's joint
    limits are measured from that pose, so the new body keeps the soldier's ragdoll exactly. Our reference pose is
    CoD's bind pose (arms out) - used there, every limit would sit somewhere else."""
    sb = {b['name']: b for b in S['bones']}; L = rig.local(); f0 = rig.ref.frames[0][1]
    out = ['version 1', 'nodes'] + ['%d "%s" %d' % (i, n, p) for i, (n, p) in enumerate(rig.nodes)] + ['end', 'skeleton', 'time 0']
    for i, (n, p) in enumerate(rig.nodes):
        v = tuple(sb[n]['pos']) + tuple(sb[n]['rot']) if n in sb else (tuple(f0[i]) if i in f0 else L[i])   # helpers: ours
        out.append('%d %.6f %.6f %.6f %.6f %.6f %.6f' % ((i,) + v))
    out += ['end', 'triangles', 'end']
    open(path, 'w').write('\n'.join(out) + '\n')

def textures(pack, tris, outdir):
    """VTF + VMT per CoD material: colour = the iwi's DXT mips as they are; normal = CoD's '-gggr' (X in alpha, Y in
    green) read as slopes, turned into Source's convention by MEASURED signs (normal_seams.decide_signs), BGR888 with
    box mips. tris = the mesh (cod2_char.build's), for the seams."""
    os.makedirs(outdir, exist_ok=True); done = {}; report = {}
    mats = [t[0] for t in tris]
    seam_tris = {}                                      # normal map -> its triangles in normal_seams' form (v as SMD's)
    for t in tris:
        nm = A.material(os.path.join(pack, 'materials', t[0])).get('normalMap')
        seam_tris.setdefault(nm, []).append((t[0], [(p, n, (uv[0], 1.0 - uv[1])) for p, n, uv, w in t[1:]]))
    for m in sorted(set(mats)):
        mm = A.material(os.path.join(pack, 'materials', m))
        col = A.iwi(os.path.join(pack, 'images', mm['colorMap'] + '.iwi'))
        assert col['fmt'] in (0x0B, 0x0D), (m, col['fmt'])
        lin = (col['rgba'][..., :3].reshape(-1, 3).astype(float) / 255) ** 2.2
        vtf.write(os.path.join(outdir, m + '.vtf'), col['w'], col['h'], 'DXT1' if col['fmt'] == 0x0B else 'DXT5',
                  [mp[2] for mp in reversed(col['mips'])], flags=0 if col['fmt'] == 0x0B else vtf.TEXTUREFLAGS_EIGHTBITALPHA,
                  reflectivity=tuple(lin.mean(0)))
        vmt = ['"VertexLitGeneric"', '{', '\t"$basetexture" "%s/%s"' % (MATDIR, m)]
        nmap = mm.get('normalMap')
        if nmap and os.path.exists(os.path.join(pack, 'images', nmap + '.iwi')):
            nname = nmap.lstrip('~').split('-')[0]
            if nname not in done:
                t = A.iwi(os.path.join(pack, 'images', nmap + '.iwi'))['rgba'].astype(np.float32) / 127.5 - 1
                x, y = t[..., 3], t[..., 1]
                bg = A.normal_background(x, y)
                down, ratios = A.green_points_down(x, y, ~bg & (np.maximum(np.abs(x), np.abs(y)) < 0.97))
                sx, sy, how = NS.decide_signs(nname, x, y, seam_tris[nmap], A.decode_normal, ~nd.binary_dilation(bg),
                                              down, NORMAL_SIGNS_CHECKED)
                k, tilt90 = relief_scale(x, y, ~bg)     # scales the slopes = the height of the relief
                nrm = A.pad_islands(A.decode_normal(sx * k * x, sy * k * y), bg)
                rgb = np.clip((nrm + 1) * 127.5 + 0.5, 0, 255).astype(np.uint8)
                chain = vtf.mip_chain(rgb)
                path = os.path.join(outdir, nname + '.vtf')
                vtf.write(path, rgb.shape[1], rgb.shape[0], 'BGR888',
                          [c[..., ::-1].tobytes() for c in reversed(chain)], flags=vtf.TEXTUREFLAGS_NORMAL)
                steepest = check_normal_vtf(path, rgb.shape[1], rgb.shape[0])
                done[nname] = 'relief x %.2f (90%% tilt %.1f -> %.1f deg), signs red %+d green %+d by %s (curl: %s, ratios %s), background %.1f%% padded, steepest %.1f deg' % (
                    k, tilt90, NORMAL_TILT90_TARGET, sx, sy, how, 'agree' if down else 'disagree', ' '.join('%.2f' % r for r in ratios), 100 * bg.mean(), steepest)
            vmt.append('\t"$bumpmap" "%s/%s"' % (MATDIR, nname))
        vmt += ['}']
        open(os.path.join(outdir, m + '.vmt'), 'w').write('\n'.join(vmt) + '\n')
        report[m] = dict(colour=mm['colorMap'], size='%dx%d' % (col['w'], col['h']), normal=nmap,
                         green=done.get(nmap.lstrip('~').split('-')[0]) if nmap else None)
    return report

# the DoD animation sets a German soldier's weapons use (npc_german_soldier.txt "anims": k98 BOLT, mp40, mp44, mg42 MG,
# p38 PISTOL, c96, stick grenade, spade, Panzerschreck; RIFLE is the script's default). Allied sets (TOMMY, BAR, 30CAL,
# BAZOOKA, GREN_FRAG ...) are never in his hands, so their stances don't bind the design. A US soldier's package
# passes its own list.
HELD_SETS = ('BOLT', 'RIFLE', 'MP40', 'MP44', 'MG', 'PISTOL', 'C96', 'GREN_STICK', 'SPADE', 'KNIFE', 'PSCHRECK')

def stance(label, sets=HELD_SETS):
    """a pose the soldier HOLDS with one of his weapons (not a one-off): DoD's stance files and the aim-layer poses
    built from them (layered_poses: 'Stance_SET+layer')"""
    m = re.match(r'@?(?:a_)?(?:pose_)?(?:blend_)?(?:Stand|Crouch|Prone|walk|run|crouch|sprint)[A-Za-z]*?_?[A-Za-z]*_(%s)(?:_Deploy)?(?:[:+]|$)' % '|'.join(sets), label)
    return bool(m)

def build(pack, npz, ref, soldier, out, name, helmet, models, pose_dirs, layered_qc=None):
    z = np.load(npz, allow_pickle=False)
    mapping = dict(Rg=z['Rg'], off=dict(zip(z['off_names'], z['off'])))
    rig, tris, info = C.build(pack, mapping, ref, [helmet] + models)
    # half-angle joint helpers (joint_volume.py): Source's linear blend skinning collapses every bent joint; a helper at
    # each joint turning half as far keeps it. Triggers fitted to every full-pose animation we play (pose_dirs).
    poses = JV.load_poses(pose_dirs)
    base_rig_nodes, base_local, base_tris = list(rig.nodes), rig.local(), tris
    if layered_qc:      # + the poses as the engine draws them: stance + aim layer + the left hand's touch IK
        sk_ = JV.Skeleton(base_rig_nodes, base_local); qdir = os.path.dirname(layered_qc)
        LP.check_order(layered_qc, qdir, sk_)
        poses = poses + LP.layered(layered_qc, qdir, sk_)
    tris, helpers = JV.add_to_rig(rig, tris, poses, stance=stance)
    S = mdl48.read(soldier); bn = [b['name'] for b in S['bones']]
    assert set(bn) <= set(n for n, p in rig.nodes), set(bn) - set(n for n, p in rig.nodes)
    base = os.path.basename(name); src = os.path.join(out, 'modelsrc', base); os.makedirs(src, exist_ok=True)
    flip = C.needs_flip(tris)
    body = [t for t, p in zip(tris, rig.tri_part) if p != helmet]; helm = [t for t, p in zip(tris, rig.tri_part) if p == helmet]
    C.write_smd(os.path.join(src, 'body.smd'), rig, body, flip=flip)
    C.write_smd(os.path.join(src, 'helmet.smd'), rig, helm, flip=flip)
    solids, pinfo = phy_solids(soldier[:-4] + '.phy')
    info['faces_out'] = [round(float(C.check_smd_winding(os.path.join(src, f))), 4) for f in ('body.smd', 'helmet.smd')]
    write_physics(os.path.join(src, 'physics.smd'), rig, solids)
    write_ragdoll_pose(os.path.join(src, 'ragdoll.smd'), rig, S)
    tex = textures(pack, tris, os.path.join(out, 'materials', *MATDIR.split('/')))
    # eyes: CoD's tag_eye in OUR head bone, the soldier's eye turn
    hi = rig.idx[C.VB + 'Head1']; R, t = rig.W[hi]; eye = R.T @ (rig.tags['tag_eye'] - t)
    q = ['// %s - generated by dodsp_tools/character/cod2_package.py (CoD3-for-CoD2 pack parts: %s)' % (name, ', '.join([helmet] + models)),
         '// Everything but the mesh is copied from the compiled %s (includes, attachments, IK, hitboxes, ragdoll).' % S['h']['name'],
         '$modelname "%s.mdl"' % name, '$cdmaterials "%s/"' % MATDIR, '$surfaceprop "%s"' % S['h']['surfaceprop'],
         '$illumposition %.3f %.3f %.3f' % tuple(S['h']['illum']), '',
         '$bodygroup "body"', '{', '\tstudio "body.smd"', '}', '$bodygroup "helmet"', '{', '\tstudio "helmet.smd"', '\tblank', '}', '']
    q += ['$includemodel "%s"' % (i[len('models/'):] if i.startswith('models/') else i) for i in S['includes']] + ['']
    for a in S['attachments']:
        o = a['m'][:, 3] if a['name'] != 'eyes' else eye
        p, y, r = mat_angles(a['m'])
        q.append('$attachment "%s" "%s" %.3f %.3f %.3f rotate %.3f %.3f %.3f' % (a['name'], bn[a['bone']], *o, p, y, r))
    q.append('')
    for c in S['ik']:
        k = c['links'][0][1]
        q.append('$ikchain "%s" "%s" knee %.3f %.3f %.3f' % (c['name'], bn[c['links'][-1][0]], *k))
    q.append('')
    for s in S['hitboxsets']:
        q.append('$hboxset "%s"' % s['name'])
        for b in s['boxes']:
            q.append('$hbox %d "%s" %.3f %.3f %.3f %.3f %.3f %.3f' % (b['group'], bn[b['bone']], *b['mn'], *b['mx']))
    vrd = os.path.join(src, base + '.vrd')
    JV.write_vrd(vrd, rig, helpers); JV.check_vrd(vrd, rig, helpers)
    # the guard: every joint measured over every pose, before (no helpers) and after (the .vrd as written, through the
    # engine's DoQuatInterpBone) - no joint may come out thinner, and the report carries both
    sk0 = JV.Skeleton(base_rig_nodes, base_local); m0 = JV.Mesh(base_tris, len(sk0.nodes))
    r0, _, _ = JV.measure(sk0, m0, poses)
    sk1 = JV.engine_skeleton(rig, helpers, vrd); m1 = JV.Mesh(tris, len(sk1.nodes))
    r1, w1, _ = JV.measure(sk1, m1, poses)
    labels = [l for l, fr in poses if JV.pose_valid(sk0, fr)]      # measure() keeps the valid poses, in order
    joints = {}
    for k in r0:
        a, b = r0[k], r1[k]
        joints['%s %s' % k] = dict(before=[round(float(np.median(a)), 2), round(float(np.percentile(a, 10)), 2)],
                                   after=[round(float(np.median(b)), 2), round(float(np.percentile(b, 10)), 2)],
                                   worst_after=[(round(q, 2), l) for q, l in w1[k][:3]])
        if np.percentile(b, 10) < np.percentile(a, 10) - JV_WORSE_TOL or np.median(b) < np.median(a) - JV_WORSE_TOL:
            raise SystemExit('joint helpers make the %s %s thinner (median / worst 10%%: %.2f / %.2f -> %.2f / %.2f) - not shipping'
                             % (k + (np.median(a), np.percentile(a, 10), np.median(b), np.percentile(b, 10))))
        bad = sorted([(round(float(x), 2), round(float(y), 2), l) for x, y, l in zip(a, b, labels)
                      if stance(l) and y < x - JV_POSE_WORSE and y < JV.JV_BAR], key=lambda t: t[1] - t[0])
        joints['%s %s' % k]['held_poses_worse'] = bad
        if bad: print('NOTE: the %s %s ends thinner in %d held pose(s): %s' % (k + (len(bad), bad[:5])))
        if bad and bad[0][0] - bad[0][1] > JV_POSE_DROP_MAX:
            raise SystemExit('joint helpers make the %s %s thinner by %.2f in a held pose (%s) - not shipping' % (k + (bad[0][0] - bad[0][1], bad[0])))
    q += ['', '// joint helpers (joint_volume.py JOINT_DESIGN): part of a joint\'s turn or roll each, run by the engine (bone_setup.cpp DoQuatInterpBone)',
          '$proceduralbones "%s.vrd"' % base]
    q += ['', '$sequence "ragdoll" "ragdoll.smd" ACT_DIERAGDOLL 1 fps 30', '']
    sol = [kv for k, kv in pinfo if k == 'solid']; total = sum(float(s['mass']) for s in sol)
    q += ['$collisionjoints "physics.smd"', '{', '\t$mass %.3f' % total, '\t$inertia %s' % sol[0].get('inertia', '10'),
          '\t$damping %s' % sol[0].get('damping', '0'), '\t$rotdamping 0', '\t$rootbone "%s"' % sol[0]['name'], '']
    for s in sol:
        if float(s.get('rotdamping', 0)): q.append('\t$jointrotdamping "%s" %s' % (s['name'], s['rotdamping']))
        if 'massbias' in s: q.append('\t$jointmassbias "%s" %s' % (s['name'], s['massbias']))
    q.append('')
    for k, kv in pinfo:
        if k != 'ragdollconstraint': continue
        child = sol[int(kv['child'])]['name']
        for ax in 'xyz':
            lo, hi_, fr = float(kv[ax + 'min']), float(kv[ax + 'max']), float(kv[ax + 'friction'])
            q.append('\t$jointconstrain "%s" %s limit %.3f %.3f %.3f' % (child, ax, lo, hi_, fr))
    q += ['}', '']
    open(os.path.join(src, base + '.qc'), 'w').write('\n'.join(q))
    info.update(eye=[round(float(v), 2) for v in eye], soldier_eye=[round(float(v), 2) for v in S['attachments'][[a['name'] for a in S['attachments']].index('eyes')]['m'][:, 3]],
                joint_helpers=[dict(helper=h['name'], kind=h['kind'], share=round(h['frac'], 3), band=h['band'], cut=round(h['cut'], 1), poses_near_cut=h.get('near_cut'), triggers=len(h['triggers']), tol=h['tol'], error_deg=h['error'], poses=h['samples']) for h in helpers],
                joints=joints, poses=len(poses),
                textures=tex, ragdoll_solids=len(solids), ragdoll_mass=round(total, 2), body_tris=len(body), helmet_tris=len(helm))
    json.dump(info, open(os.path.join(src, 'report.json'), 'w'), indent=1, default=str)
    return info

if __name__ == '__main__':
    a = sys.argv[1:]
    k = a.index('--poses'); pose_dirs = a[k + 1].split(','); a = a[:k] + a[k + 2:]
    lq = None
    if '--layered' in a: k = a.index('--layered'); lq = a[k + 1]; a = a[:k] + a[k + 2:]
    info = build(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7:], pose_dirs, lq)
    print(json.dumps({k: info[k] for k in ('faces_out', 'eye', 'soldier_eye', 'ragdoll_solids', 'ragdoll_mass', 'body_tris', 'helmet_tris', 'height', 'joints', 'joint_helpers')}, indent=1))
