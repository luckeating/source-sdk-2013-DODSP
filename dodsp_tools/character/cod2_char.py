"""A CoD2-skeleton character (CoD2 itself, or the CoD3-for-CoD2 packs) put onto OUR soldier's skeleton (DoD:S's
ValveBiped reference) so DoD's and our animation packs play on it.

    python3 -I cod2_char.py <pack x dir> <xanim_map.npz> <dod reference smd> <out dir> <xmodel> [<xmodel> ...]

    e.g. ... char_ger_wrmcht_body_1 char_ger_head_0 char_ger_wrmcht_gear_0 char_ger_wrmcht_gear_kar98 char_ger_wrmcht_helm1

How (one owner for each fact):
  - where a part's bones are: the BODY's bone of the same name (a CoD character is a body + head + gear + helmet, each
    file carrying the few body bones it hangs on - at the origin in its own file; the game joins them by name). Bones
    only a part has (face bones, j_head_end, j_helmet) hang off their parent with their local transform.
  - which of our bones a CoD bone drives: xanim_to_smd.MAP; a CoD bone not in it (twist / bulge / face / helmet) moves
    with its nearest ancestor that is.
  - each of our bones' turn against its CoD bone: the turn learned from the officer port (xanim_map.npz 'off', the same
    numbers the animation converter uses), and the global frame Rg - so CoD and Valve bone axes agree.
  - the reference skeleton = OUR bones' lengths and offsets (the DoD reference's local positions: what every animation
    sets) turned the way the CoD bind pose has them (so nothing is re-posed: a vertex keeps its place relative to its
    bone; only the bones' LENGTHS change).
  - the trunk (pelvis, spine, clavicles, neck, head) keeps CoD's shape, bent gently to meet OUR limb joints: placed by
    the global frame (CoD's j_mainroot on our pelvis), then moved by TrunkWarp - by height, how far our hip / shoulder /
    head joints sit from CoD's (and how much wider or narrower) - and bound to our bones where it lands. CoD's spine
    joints sit elsewhere along the trunk than ours (j_spine4 3.5 units lower than our Spine4) though the trunk is as long
    (pelvis-head 19.9 vs 19.4), so a per-bone stretch there squashed the chest to 0.4. Without the warp our shoulders
    sat 2.4 units BEHIND CoD's and our clavicles 2.3 above: the arms hung off the back of the coat and the trapezius
    rose in a ridge (owner 2026-11-21: "shoulders pretty sharp and bulky"). The clavicles are trunk for the same reason.
  - limbs: a vertex's coordinate ALONG a limb bone (x) is scaled by our length / CoD's between the joint and the next
    joint; beyond the next joint it is shifted (a hand's vertices weighted to the forearm stay a hand's size). So each
    limb hangs from OUR joint (where the animations put it); joint_report says how far that is from CoD's.
  - weights: CoD's (main bone = the rest of 1), merged per bone of ours, the 3 biggest kept (Source's limit), renormalised.
Writes <out>/reference.smd (our skeleton, every part, materials = CoD's material names), <out>/parts.json (counts)."""
import os, sys, json
import numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [HERE, os.path.join(os.path.dirname(HERE), 'vehicle_preview'), os.path.dirname(HERE)]
from cod2_xmodel import parts, surfs
from cod2_assets import xmodel
from xanim_to_smd import MAP, VB, rotm, euler
from smd import SMD

COD2VB = {c: VB + v for v, c in MAP.items()}
# the joint each bone's length runs to (ours: the child's local position; CoD's: the same pair of joints)
TRUNK = {VB + b for b in ('Pelvis', 'Spine', 'Spine1', 'Spine2', 'Spine4', 'Neck1', 'Head1', 'L_Clavicle', 'R_Clavicle')}
CHAIN = {'Clavicle': 'UpperArm', 'UpperArm': 'Forearm', 'Forearm': 'Hand', 'Thigh': 'Calf', 'Calf': 'Foot'}
WEIGHTS_MAX = 3
# CoD bone -> (share kept on the bone it maps to, our other bone, its share). CoD's shoulder twist bone takes only part
# of the upper arm's turn; ours has no twist bone, so its vertices go half to the clavicle - in the raised-arm aims the
# top of the shoulder then rounds off instead of peaking (preview 2026-11-21, shoulders_compare).
SPLIT = {'j_shouldertwist_le': (0.5, 'L_Clavicle', 0.5), 'j_shouldertwist_ri': (0.5, 'R_Clavicle', 0.5)}

def chain_child(vn):
    b = vn[len(VB):]
    side = b[:2] if b[:2] in ('L_', 'R_') else ''
    nxt = CHAIN.get(b[len(side):])
    return VB + side + nxt if nxt else None

def orth(R):
    U, _, Vt = np.linalg.svd(R); return U @ np.diag([1, 1, np.linalg.det(U @ Vt)]) @ Vt

class Rig:
    """our skeleton in the CoD bind pose, with our lengths"""
    def __init__(self, body_bones, mapping, dod_ref):
        self.ref = SMD(dod_ref); self.nodes = self.ref.nodes
        self.idx = {n: i for i, (n, p) in enumerate(self.nodes)}
        Rg = orth(mapping['Rg']); off = mapping['off']
        B = {b['name']: b for b in body_bones}
        f0 = self.ref.frames[0][1]
        self.W = {}
        for i, (vn, p) in enumerate(self.nodes):
            loc = np.array(f0[i], float)
            c = MAP.get(vn[len(VB):]) if vn.startswith(VB) else None
            if c and c in B and vn in off:
                R = orth(Rg @ B[c]['R'] @ off[vn])
            else:
                R = (self.W[p][0] if p >= 0 else np.eye(3)) @ rotm(loc[3:])
            if p < 0:
                t = Rg @ B['j_mainroot']['t']
            else:
                t = self.W[p][0] @ loc[:3] + self.W[p][1]
            self.W[i] = (R, t)
        # CoD's lengths for the stretch, in the same joints
        self.ratio = {}
        for vn, p in self.nodes:
            ch = chain_child(vn)
            if not ch or ch not in self.idx: continue
            c0, c1 = MAP[vn[len(VB):]], MAP[ch[len(VB):]]
            Lc = np.linalg.norm(B[c1]['t'] - B[c0]['t']); Ld = np.linalg.norm(np.array(f0[self.idx[ch]][:3]))
            self.ratio[self.idx[vn]] = (Lc, Ld)
        self.Rg = Rg; self.off = off; self.root_cod = B['j_mainroot']['t']
        self.B = B
    def trunk_place(self, x):
        """a trunk vertex: CoD's bind pose through the global frame, CoD's root on our pelvis"""
        return self.Rg @ (x - self.root_cod) + self.W[0][1]
    def build_warp(self):
        """TrunkWarp's control heights: hips, shoulders, head - each with how far our joint(s) sit from CoD's (placed
        like the trunk), and for a pair: CoD's half-span and how far each joint moved outward (- = inward)"""
        def at(names):
            ours = [self.W[self.idx[VB + n]][1] for n in names]; cod = [self.trunk_place(self.B[MAP[n]]['t']) for n in names]
            d = np.mean([o - c for o, c in zip(ours, cod)], 0); z = np.mean([c[2] for c in cod])
            if len(names) == 2:
                h = abs(cod[0][1] - cod[1][1]) / 2; s = (h, abs(ours[0][1] - ours[1][1]) / 2 - h)
            else: s = (1.0, 0.0)
            return z, d, s
        self.warp = [at(('L_Thigh', 'R_Thigh')), at(('L_UpperArm', 'R_UpperArm')), at(('Head1',))]
        self.yc = self.W[0][1][1]
    def trunk_warp(self, x):
        """a trunk vertex where it goes: trunk_place, then the warp at its height (linear between the control heights)"""
        p = self.trunk_place(x); z = p[2]; W = self.warp
        if z <= W[0][0]: k, f = 0, 0.0
        elif z >= W[-1][0]: k, f = len(W) - 2, 1.0
        else:
            k = next(k for k in range(len(W) - 1) if W[k][0] <= z <= W[k + 1][0]); f = (z - W[k][0]) / (W[k + 1][0] - W[k][0])
        d = W[k][1] + f * (W[k + 1][1] - W[k][1])
        y = p[1] - self.yc; side = 0.0
        for j, g in ((k, 1 - f), (k + 1, f)):
            h, out = W[j][2]; side += g * np.sign(y) * min(abs(y) / h, 1.0) * out
        return np.array([p[0] + d[0], p[1] + side, p[2] + d[2]])
    def joint_report(self):
        """our joints vs CoD's carried by the same rule as the trunk (where a limb's mesh moves from CoD's place)"""
        out = {}
        for i, (vn, p) in enumerate(self.nodes):
            c = MAP.get(vn[len(VB):]) if vn.startswith(VB) else None
            if c in self.B and 'Finger' not in vn:
                out[vn[len(VB):]] = round(float(np.linalg.norm(self.W[i][1] - self.trunk_place(self.B[c]['t']))), 2)
        return out
    def local(self):
        """the reference frame: local (pos, euler) per node"""
        out = {}
        for i, (n, p) in enumerate(self.nodes):
            R, t = self.W[i]
            if p < 0: lR, lt = R, t
            else:
                PR, Pt = self.W[p]; lR = PR.T @ R; lt = PR.T @ (t - Pt)
            out[i] = tuple(lt) + tuple(euler(lR))
        return out

def world_bones(part_bones, body):
    """a part's bones where the game puts them: the body's bone of the same name, else off its parent"""
    B = {b['name']: b for b in body}; W = []
    for b in part_bones:
        if b['name'] in B: W.append(dict(b, R=B[b['name']]['R'], t=B[b['name']]['t']))
        else:
            P = W[b['parent']]; W.append(dict(b, R=P['R'] @ b['lR'], t=P['R'] @ b['lt'] + P['t']))
    return W

def driver(bones, i):
    """the CoD bone (index) whose mapped bone of ours drives bone i: itself or its nearest mapped ancestor"""
    while i >= 0 and bones[i]['name'] not in COD2VB:
        i = bones[i]['parent']
    return i

def stretch(q, rl):
    if rl is None: return q
    Lc, Ld = rl; x = q[0]
    if 0.0 <= x <= Lc: x = x * Ld / Lc
    elif x > Lc: x = x + Ld - Lc
    return np.array([x, q[1], q[2]])

def build(pack, mapping, dod_ref, models):
    body = None; loaded = []
    for m in models:
        xm = xmodel(os.path.join(pack, 'xmodel', m)); lod = xm['lods'][0][1]
        bones = parts(os.path.join(pack, 'xmodelparts', lod)); sf = surfs(os.path.join(pack, 'xmodelsurfs', lod))
        if body is None and any(b['name'] == 'j_mainroot' for b in bones) and any(b['name'] == 'j_ankle_le' for b in bones):
            body = bones
        loaded.append((m, lod, xm['materials'][0], bones, sf))
    assert body is not None, 'no body (a part with the legs) among %s' % models
    rig = Rig(body, mapping, dod_ref); rig.build_warp()
    tris = []; tpart = []; counts = {}; dropped = 0.0; nverts = 0; tags = {}
    for m, lod, mats, bones0, sf in loaded:
        bones = world_bones(bones0, body)
        for b in bones:
            if b['name'].startswith('tag_'): tags[b['name']] = rig.trunk_warp(b['t'])     # tag_eye: on the head
        assert len(mats) == len(sf), (m, len(mats), len(sf))
        for mat, s in zip(mats, sf):
            V = []
            for v in s['verts']:
                infl = [(v['bone'], 1.0 - sum(w[1] for w in v['w']), v['pos'])] + [(b, w, p) for b, w, p in v['w']]
                acc = {}; pos = np.zeros(3); nrm = np.zeros(3)
                for b, w, p in infl:
                    if w <= 0: continue
                    x = bones[b]['R'] @ p + bones[b]['t']                 # CoD bind pose, world
                    d = driver(bones, b); assert d >= 0, (m, bones[b]['name'])
                    vn = COD2VB[bones[d]['name']]; vi = rig.idx[vn]
                    qc = bones[d]['R'].T @ (x - bones[d]['t'])            # in the driving CoD bone
                    qv = rig.off[vn].T @ qc                               # in our bone (no scale: CoD units = ours)
                    R, t = rig.W[vi]
                    if vn in TRUNK: pos += w * rig.trunk_warp(x)
                    else: pos += w * (R @ stretch(qv, rig.ratio.get(vi)) + t)
                    sp = SPLIT.get(bones[b]['name'])
                    if sp:
                        o = rig.idx[VB + sp[1]]; acc[o] = acc.get(o, 0.0) + w * sp[2]; w = w * sp[0]
                    acc[vi] = acc.get(vi, 0.0) + w
                # CoD stores a vertex's normal in its MAIN BONE's frame, like its position (taken raw it agreed with the
                # faces 19-71% of the time; turned by the bone, 100% - the other way round: see winding)
                nrm = rig.Rg @ (bones[v['bone']]['R'] @ v['n']); nrm /= np.linalg.norm(nrm) + 1e-12
                top = sorted(acc.items(), key=lambda kv: -kv[1])
                dropped = max(dropped, sum(w for _, w in top[WEIGHTS_MAX:]))
                top = top[:WEIGHTS_MAX]; tw = sum(w for _, w in top)
                V.append((pos, nrm, v['uv'], [(b, w / tw) for b, w in top]))
            nverts += len(V)
            for a, b, c in s['tris']:
                tris.append((mat, V[a], V[b], V[c])); tpart.append(m)
            counts[mat] = counts.get(mat, 0) + len(s['tris'])
    # standing on the ground: the lowest vertex at z 0 (the pelvis carries the rest)
    zmin = min(min(t[1][0][2], t[2][0][2], t[3][0][2]) for t in tris)
    sh = np.array([0, 0, -zmin])
    tris = [(t[0],) + tuple((p + sh, n, uv, w) for p, n, uv, w in t[1:]) for t in tris]
    R0, t0 = rig.W[0]; rig.W[0] = (R0, t0 + sh)
    for i, (n, p) in enumerate(rig.nodes):
        if p >= 0: R, t = rig.W[i]; rig.W[i] = (R, t + sh)
    rig.tri_part = tpart; rig.tags = {k: v + sh for k, v in tags.items()}
    return rig, tris, dict(joints_moved=rig.joint_report(), warp=[(round(float(z), 1), [round(float(v), 2) for v in d], [round(float(v), 2) for v in sc]) for z, d, sc in rig.warp], parts=[(m, lod) for m, lod, *_ in loaded], triangles=counts, vertices=nverts,
                           worst_weight_dropped=round(dropped, 3), height=round(float(max(max(t[1][0][2], t[2][0][2], t[3][0][2]) for t in tris)), 2))

# SMD's front face: corners in RIGHT-HAND order about the outward normal. Measured on SMDs that draw right in game (the
# halftrack rig's body.smd from Crowbar's decompile: 100%), not assumed. CoD2 winds the other way (mg42_model.py
# already reversed it; this converter first didn't, and the soldier drew inside out - 2026-11-21).
WINDING_AGREE_MIN = 0.9

def winding(tris):
    """share of triangles whose corner order turns the right-hand way about their vertex normals"""
    s = 0
    for _, a, b, c in tris:
        f = np.cross(b[0] - a[0], c[0] - a[0]); s += (f @ (a[1] + b[1] + c[1])) > 0
    return s / len(tris)

def needs_flip(tris):
    """True if the corners must be reversed for SMD; stops when the normals don't settle it (they'd be wrong)"""
    w = winding(tris)
    if WINDING_AGREE_MIN <= w: return False
    if w <= 1 - WINDING_AGREE_MIN: return True
    raise SystemExit('winding: only %.0f%% of triangles agree with their normals either way - the normals are wrong '
                     '(not in the space the positions are in?); not writing an SMD that may draw inside out' % (100 * max(w, 1 - w)))

def check_smd_winding(path):
    """a written SMD read back: its corner order must agree with its normals the SMD way"""
    sys.path.insert(0, HERE); import char_preview as CP
    _, _, P, N, _, _ = CP.read_mesh(path)
    f = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0]); a = np.mean(np.einsum('ij,ij->i', f, N.sum(1)) > 0)
    if a < WINDING_AGREE_MIN: raise SystemExit('%s: only %.0f%% of triangles face out the SMD way' % (path, 100 * a))
    return a

def write_smd(path, rig, tris, matname=lambda m: m, flip=None):
    if flip is None: flip = needs_flip(tris)
    L = rig.local()
    out = ['version 1', 'nodes'] + ['%d "%s" %d' % (i, n, p) for i, (n, p) in enumerate(rig.nodes)] + ['end', 'skeleton', 'time 0']
    out += ['%d %.6f %.6f %.6f %.6f %.6f %.6f' % ((i,) + L[i]) for i in range(len(rig.nodes))]
    out += ['end', 'triangles']
    for m, *corners in tris:
        if flip: corners = [corners[0], corners[2], corners[1]]
        out.append(matname(m))
        for p, n, uv, w in corners:
            out.append('0 %.5f %.5f %.5f %.5f %.5f %.5f %.6f %.6f %d %s' % (p[0], p[1], p[2], n[0], n[1], n[2], uv[0], 1.0 - uv[1],
                       len(w), ' '.join('%d %.5f' % bw for bw in w)))
    out.append('end')
    open(path, 'w').write('\n'.join(out) + '\n')
    return flip

if __name__ == '__main__':
    pack, npz, ref, out = sys.argv[1:5]; models = sys.argv[5:]
    z = np.load(npz, allow_pickle=False)
    mapping = dict(Rg=z['Rg'], off=dict(zip(z['off_names'], z['off'])))
    rig, tris, info = build(pack, mapping, ref, models)
    os.makedirs(out, exist_ok=True)
    info['flipped_winding'] = bool(write_smd(os.path.join(out, 'reference.smd'), rig, tris))
    info['faces_out'] = round(float(check_smd_winding(os.path.join(out, 'reference.smd'))), 4)
    info['stretch'] = {rig.nodes[i][0][len(VB):]: '%.2f -> %.2f' % rl for i, rl in rig.ratio.items()}
    json.dump(info, open(os.path.join(out, 'parts.json'), 'w'), indent=1)
    print(json.dumps(info, indent=1))
