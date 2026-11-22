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
import cod2_char as C, cod2_assets as A, mdl48, vtf

MATDIR = 'models/dodsp/cod3'

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
        v = tuple(sb[n]['pos']) + tuple(sb[n]['rot']) if n in sb else tuple(f0[i])
        out.append('%d %.6f %.6f %.6f %.6f %.6f %.6f' % ((i,) + v))
    out += ['end', 'triangles', 'end']
    open(path, 'w').write('\n'.join(out) + '\n')

def textures(pack, mats, outdir):
    """VTF + VMT per CoD material: colour = the iwi's DXT mips as they are; normal = CoD's '-gggr' (X in alpha, Y in
    green) rebuilt to RGB, BGR888 with box mips. CoD2 is a Direct3D game like Source - the green axis is kept as is."""
    os.makedirs(outdir, exist_ok=True); done = {}; report = {}
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
                down, ratios = A.green_points_down(x, y, (x * x + y * y) < 0.95)
                if not down: y = -y                     # Source's green points image-down
                z = np.sqrt(np.clip(1 - x * x - y * y, 0, 1))
                rgb = np.clip((np.stack([x, y, z], -1) + 1) * 127.5 + 0.5, 0, 255).astype(np.uint8)
                chain = vtf.mip_chain(rgb)
                vtf.write(os.path.join(outdir, nname + '.vtf'), rgb.shape[1], rgb.shape[0], 'BGR888',
                          [c[..., ::-1].tobytes() for c in reversed(chain)], flags=vtf.TEXTUREFLAGS_NORMAL)
                done[nname] = 'green %s%s (curl ratios %s)' % ('down' if down else 'UP', '' if down else ' -> flipped',
                                                               ' '.join('%.2f' % r for r in ratios))
            vmt.append('\t"$bumpmap" "%s/%s"' % (MATDIR, nname))
        vmt += ['}']
        open(os.path.join(outdir, m + '.vmt'), 'w').write('\n'.join(vmt) + '\n')
        report[m] = dict(colour=mm['colorMap'], size='%dx%d' % (col['w'], col['h']), normal=nmap,
                         green=done.get(nmap.lstrip('~').split('-')[0]) if nmap else None)
    return report

def build(pack, npz, ref, soldier, out, name, helmet, models):
    z = np.load(npz, allow_pickle=False)
    mapping = dict(Rg=z['Rg'], off=dict(zip(z['off_names'], z['off'])))
    rig, tris, info = C.build(pack, mapping, ref, [helmet] + models)
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
    tex = textures(pack, [t[0] for t in tris], os.path.join(out, 'materials', *MATDIR.split('/')))
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
                textures=tex, ragdoll_solids=len(solids), ragdoll_mass=round(total, 2), body_tris=len(body), helmet_tris=len(helm))
    json.dump(info, open(os.path.join(src, 'report.json'), 'w'), indent=1, default=str)
    return info

if __name__ == '__main__':
    a = sys.argv[1:]
    info = build(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7:])
    print(json.dumps({k: info[k] for k in ('faces_out', 'eye', 'soldier_eye', 'ragdoll_solids', 'ragdoll_mass', 'body_tris', 'helmet_tris', 'height', 'textures')}, indent=1))
