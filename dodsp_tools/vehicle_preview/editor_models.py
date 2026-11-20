"""Hammer preview models for DOD-SP vehicles: hull + turret merged into ONE model per vehicle type, so Hammer shows
what the game spawns (Hammer draws one model per entity; the game puts the turret on at run time).

usage: editor_models.py <dodsp_vehicles.txt> <folder with the compiled vehicle .mdl/.vvd/.dx90.vtx> <out folder>

Writes <out>/<type>/<type>.smd + .qc for every type with a turret_model (types without one show their own model in
Hammer). The owner compiles the folder with Crowbar -> models/dodsp/editor/<type>.mdl. Vertices are written in the
compiled meshes' own frame (CoD2 meshes face -Y, like every Source SMD); studiomdl's default rotation turns the result
to +X, exactly as it did the originals. The turret goes on at the type's turret_offset (game frame, +X forward),
turned into that frame: (x, y, z) -> (y, -x, z)."""
import os, re, struct, sys

def kv_parse(text):
    toks = re.findall(r'"((?:[^"\\]|\\.)*)"|([{}])', re.sub(r'//[^\n]*', '', text))
    pos = 0
    def block():
        nonlocal pos
        out = []
        while pos < len(toks):
            s, br = toks[pos]
            if br == '}':
                pos += 1
                return out
            key = s; pos += 1
            s2, br2 = toks[pos]
            if br2 == '{':
                pos += 1
                out.append((key, block()))
            else:
                out.append((key, s2)); pos += 1
        return out
    return block()

def types_from_script(path):
    top = dict(kv_parse(open(path, encoding='utf-8').read()))
    raw = dict((k, v) for k, v in top['DoDSP_Vehicles'] if isinstance(v, list))
    def merged(n, depth=0):
        t = raw[n]; b = dict(t).get('base')
        out = dict(merged(b, depth + 1)) if b and b in raw and depth < 5 else {}
        out.update((k, v) for k, v in t if k != 'base')
        return out
    return dict((n, merged(n)) for n in raw)

def mesh(mdl):
    """[(material, [(pos, normal, uv) x3])] in the compiled mesh frame, plus the model's material folders."""
    d = open(mdl, 'rb').read(); I = lambda o: struct.unpack_from('<i', d, o)[0]
    cs = lambda o: d[o:d.index(b'\0', o)].decode('latin1')
    tex = [cs(I(208) + i * 64 + I(I(208) + i * 64)) for i in range(I(204))]
    cd = [cs(I(I(216) + 4 * i)) for i in range(I(212))]
    v = open(mdl[:-4] + '.vvd', 'rb').read(); Iv = lambda o: struct.unpack_from('<i', v, o)[0]
    nfix, fixs, vstart, nv = Iv(48), Iv(52), Iv(56), Iv(16)
    if nfix:
        raw = b''
        for k in range(nfix):
            lod, src, cnt = struct.unpack_from('<iii', v, fixs + 12 * k)
            if lod >= 0:
                raw += v[vstart + src * 48: vstart + (src + cnt) * 48]
    else:
        raw = v[vstart: vstart + nv * 48]
    vert = lambda i: (struct.unpack_from('<3f', raw, i * 48 + 16), struct.unpack_from('<3f', raw, i * 48 + 28),
                      struct.unpack_from('<2f', raw, i * 48 + 40))
    x = open(mdl[:-4] + '.dx90.vtx', 'rb').read(); X = lambda o: struct.unpack_from('<i', x, o)[0]
    tris = []
    bpo, mbpi = X(32), I(236)
    for b in range(X(28)):
        bo = bpo + 8 * b; nm, mo = X(bo), X(bo + 4)
        mb = mbpi + 16 * b; mmi = I(mb + 12)
        for m in range(nm):
            mho = bo + mo + 8 * m; lo = X(mho + 4)
            mdlm = mb + mmi + 148 * m; meshi, vidx = I(mdlm + 76), I(mdlm + 84)
            lod0 = mho + lo; nmesh, meo = X(lod0), X(lod0 + 4)
            for k in range(nmesh):
                mh = lod0 + meo + 9 * k; nsg, sgo = X(mh), X(mh + 4)
                me = mdlm + meshi + 116 * k; mat, voff = I(me), I(me + 12)
                base = vidx // 48 + voff
                for g in range(nsg):
                    sg = mh + sgo + 25 * g
                    nvv, vo, ni, io = X(sg), X(sg + 4), X(sg + 8), X(sg + 12)
                    ids = [struct.unpack_from('<H', x, sg + vo + 9 * i + 4)[0] for i in range(nvv)]
                    idx = struct.unpack_from('<%dH' % ni, x, sg + io)
                    for t in range(0, ni, 3):
                        tris.append((tex[mat] if mat < len(tex) else 'missing', [vert(base + ids[idx[t + j]]) for j in range(3)]))
    return tris, cd

def write(out, name, tris):
    with open(out, 'w', newline='\n') as f:
        f.write('version 1\nnodes\n0 "root" -1\nend\nskeleton\ntime 0\n0 0 0 0 0 0 0\nend\ntriangles\n')
        for mat, vs in tris:
            f.write(mat + '\n')
            for (p, n, uv) in vs:
                f.write('0 %.6f %.6f %.6f %.6f %.6f %.6f %.6f %.6f 1 0 1\n' % (p[0], p[1], p[2], n[0], n[1], n[2], uv[0], 1.0 - uv[1]))
        f.write('end\n')

def main(script, models, out):
    done = 0
    for name, t in sorted(types_from_script(script).items()):
        if 'turret_model' not in t or 'model' not in t:
            continue
        hull = os.path.join(models, os.path.basename(t['model']))
        tur = os.path.join(models, os.path.basename(t['turret_model']))
        if not (os.path.exists(hull) and os.path.exists(tur)):
            print('skip %s: %s or %s not found' % (name, hull, tur)); continue
        ox, oy, oz = (float(a) for a in t.get('turret_offset', '0 0 0').split())
        h, cd1 = mesh(hull); tt, cd2 = mesh(tur)
        shift = (oy, -ox, oz)     # game frame -> mesh frame
        tt = [(m, [((p[0] + shift[0], p[1] + shift[1], p[2] + shift[2]), n, uv) for p, n, uv in vs]) for m, vs in tt]
        d = os.path.join(out, name); os.makedirs(d, exist_ok=True)
        write(os.path.join(d, name + '.smd'), name, h + tt)
        cds = ''.join('$cdmaterials "%s"\n' % c for c in dict.fromkeys(cd1 + cd2))
        open(os.path.join(d, name + '.qc'), 'w', newline='\n').write(
            '// Hammer preview only (generated by dodsp_tools/vehicle_preview/editor_models.py) - never spawned in game\n'
            '$modelname "dodsp\\editor\\%s.mdl"\n%s$surfaceprop "metal"\n$body "body" "%s.smd"\n$sequence "idle" "%s.smd"\n'
            % (name, cds, name, name))
        print('%s: %d hull + %d turret triangles, turret at %s' % (name, len(h), len(tt), t.get('turret_offset')))
        done += 1
    print('%d preview models' % done)

if __name__ == '__main__':
    main(*sys.argv[1:4])
