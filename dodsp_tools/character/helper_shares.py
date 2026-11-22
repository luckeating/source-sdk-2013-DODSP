"""How much of each CoD helper bone goes to which of OUR bones - measured, not set by hand.

CoD's skeleton has helper bones ours hasn't (j_hiptwist, j_legextra, j_elbow_bulge, j_shouldertwist, j_wristtwist...):
in CoD's animations they turn and move part-way between two joints. cod2_char maps every CoD bone to one of ours; a
helper's vertices used to go WHOLE to its nearest mapped ancestor (j_legextra -> the pelvis, though it follows the
thigh; owner 2026-11-21: pinched crotch / thigh, crushed inner elbows, wrists). With linear blend skinning a vertex
weighted (1 - k) to bone A and k to bone B moves to the mix of A's and B's moves, so k is fitted on VERTEX POSITIONS:
the helper's own vertices skinned by CoD's animated helper (the truth) against the A/B mix, over many CoD2 animations.
That covers twist (a turn) and bulge (a move) alike. B is tried among A's mapped neighbours (its mapped parent and
children); the best B is kept only when it cuts the error by HELPER_GAIN_MIN, else the helper stays whole on A.
Left and right are fitted apart and the share averaged (the model stays symmetric).

    python3 -I helper_shares.py <pack x dir> <xanim dir> <out json> <xmodel> [<xmodel> ...]
Out: { helper: {"to": our bone, "share": k, "err_whole": .., "err_split": .., "weight": total vertex weight} }"""
import os, sys, json, random
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), 'vehicle_preview'), os.path.dirname(HERE)]
import cod2_xanim as XA
from cod2_xmodel import parts, surfs
from cod2_assets import xmodel
import cod2_char as C

HELPER_ANIMS = 700          # CoD2 animations sampled (seeded), player/viewmodel ones left out
HELPER_FRAMES = 6           # frames per animation
HELPER_GAIN_MIN = 0.25      # a split must cut the vertex error by a quarter to be used
HELPER_WEIGHT_MIN = 1.0     # helpers carrying less total vertex weight are left whole (nothing to fit)

def influences(pack, models):
    """{helper name: [(bind world position, weight)]} over every part, on the body's skeleton"""
    body = None; loaded = []
    for m in models:
        lod = xmodel(os.path.join(pack, 'xmodel', m))['lods'][0][1]
        bones = parts(os.path.join(pack, 'xmodelparts', lod)); sf = surfs(os.path.join(pack, 'xmodelsurfs', lod))
        if body is None and any(b['name'] == 'j_ankle_le' for b in bones): body = bones
        loaded.append((bones, sf))
    out = {}
    for bones0, sf in loaded:
        bones = C.world_bones(bones0, body)
        for s in sf:
            for v in s['verts']:
                for b, w, p in [(v['bone'], 1.0 - sum(x[1] for x in v['w']), v['pos'])] + list(v['w']):
                    n = bones[b]['name']
                    if w > 0 and n not in C.COD2VB and not n.startswith('tag_'):
                        out.setdefault(n, []).append((bones[b]['R'] @ p + bones[b]['t'], w))
    return body, out

def neighbours(skel, a):
    """mapped bones next to mapped bone a: its nearest mapped ancestor and its mapped descendants reached through
    unmapped bones only"""
    names = [b['name'] for b in skel]; i = names.index(a); out = []
    p = skel[i]['parent']
    while p >= 0 and names[p] not in C.COD2VB: p = skel[p]['parent']
    if p >= 0: out.append(names[p])
    stack = [j for j, b in enumerate(skel) if b['parent'] == i]
    while stack:
        j = stack.pop()
        if names[j] in C.COD2VB: out.append(names[j])
        else: stack += [k for k, b in enumerate(skel) if b['parent'] == j]
    return out

def fit(pack, xdir, models):
    skel, infl = influences(pack, models)
    names = [b['name'] for b in skel]
    bind = {b['name']: (b['R'], b['t']) for b in skel}
    files = sorted(f for f in os.listdir(xdir) if not f.startswith(('pb_', 'pt_', 'viewmodel')))
    random.Random(1).shuffle(files)
    poses = []
    for f in files:
        if len(poses) >= HELPER_ANIMS * HELPER_FRAMES: break
        try: a = XA.read(os.path.join(xdir, f))
        except Exception: continue
        if 'j_hip_le' not in a['bones']: continue
        for fr in range(0, a['frames'], max(1, a['frames'] // HELPER_FRAMES)):
            poses.append(XA.world(a, skel, fr, False, offsets=True))
    move = lambda W, n, X: (W[n][0] @ bind[n][0].T @ (X - bind[n][1]).T).T + W[n][1]
    res = {}
    for h, lst in sorted(infl.items()):
        tw = sum(w for _, w in lst)
        if h not in names or tw < HELPER_WEIGHT_MIN: continue
        A = names[C.driver(skel, names.index(h))]
        X = np.array([p for p, _ in lst]); w = np.array([w for _, w in lst])
        best = None
        for B in neighbours(skel, A):
            num = den = e0 = 0.0; D = []
            for W in poses:
                T = move(W, h, X); PA = move(W, A, X); PB = move(W, B, X)
                d = PB - PA; r = T - PA
                num += (w[:, None] * r * d).sum(); den += (w[:, None] * d * d).sum(); D.append((T, PA, PB))
            k = float(np.clip(num / max(den, 1e-12), 0, 1))
            err = lambda kk: float(np.mean([(w * np.linalg.norm(T - ((1 - kk) * PA + kk * PB), axis=1)).sum() / w.sum() for T, PA, PB in D]))
            e_whole, e_split = err(0.0), err(k)
            if best is None or e_split < best[3]: best = (B, k, e_whole, e_split)
        B, k, e_whole, e_split = best
        use = e_split <= (1 - HELPER_GAIN_MIN) * e_whole
        res[h] = dict(on=C.COD2VB[A], to=C.COD2VB[B] if use else None, share=round(k, 3) if use else 0.0,
                      share_fitted=round(k, 3), via=B, err_whole=round(e_whole, 3), err_split=round(e_split, 3),
                      weight=round(float(tw), 1))
    # one share per pair of sides
    for h, r in res.items():
        o = h[:-3] + ('_ri' if h.endswith('_le') else '_le') if h.endswith(('_le', '_ri')) else None
        if o in res and r['to'] and res[o]['to']:
            r['share'] = res[o]['share'] = round((r['share_fitted'] + res[o]['share_fitted']) / 2, 3)
        elif o in res and bool(r['to']) != bool(res[o]['to']):
            raise SystemExit('%s / %s: one side splits and the other does not (%s / %s) - not making the body lopsided'
                             % (h, o, r, res[o]))
    return res, len(poses)

if __name__ == '__main__':
    pack, xdir, out = sys.argv[1:4]
    res, n = fit(pack, xdir, sys.argv[4:])
    json.dump(dict(poses=n, helpers=res), open(out, 'w'), indent=1)
    for h, r in res.items(): print(h, r)
