"""Collision SMDs taken from a Crowbar decompile, checked against the COMPILED collision (.phy) they came from.

LESSON (2026-11-20, owner's Hammer screenshot): Crowbar 0.74's halftrack_*_physics.smd is NOT in the reference mesh's
frame - its axes are (length, height, -width) while body.smd is (width, -length, height) before studiomdl's default
90-degree turn. Recompiled as written, the halftrack's collision stood on its side across the vehicle (the door rig and
H1 both shipped that way; the port's own .phy was right). So a physics SMD is never copied blind: its frame is FITTED
onto the compiled .phy (every signed axis permutation tried, the one that puts every SMD corner on a .phy point wins)
and written in the reference SMD's frame, or the tool stops.

    python3 phy_fit.py <decompiled physics .smd> <compiled .phy>      prints the fit

phy_points: the corner points of every convex piece of a .phy (Source VPHY compact surfaces), in game units and axes
(x forward, y left, z up). Mesh frame (what reference SMDs use before studiomdl's turn): game = (-mesh y, mesh x, mesh z).
"""
import itertools, struct, sys, numpy as np

FIT_TOLERANCE = 0.5          # units: every SMD corner must land this close to a corner of the compiled collision

def phy_points(path):
    d = open(path, 'rb').read()
    hsize, _, nsolid, _ = struct.unpack_from('<iiii', d, 0)
    o, out = hsize, []
    for s in range(nsolid):
        size, = struct.unpack_from('<i', d, o); start = o + 4
        if d[start:start + 4] != b'VPHY':
            raise SystemExit('%s: solid %d is not a VPHY compact surface (old .phy format?)' % (path, s))
        cs = start + 28                                         # compactsurfaceheader_t -> IVP_Compact_Surface
        root, = struct.unpack_from('<i', d, cs + 32)           # offset_ledgetree_root: the ledges end there
        L, pts = cs + 48, set()
        while L < cs + root:
            pofs, _, _, ntri = struct.unpack_from('<iiiH', d, L)
            for t in range(ntri):
                for e in range(3):
                    v, = struct.unpack_from('<I', d, L + 16 + 16 * t + 4 + 4 * e)
                    pts.add(L + pofs + 16 * (v & 0xffff))
            L += 16 + 16 * ntri
        for p in pts:
            x, y, z = struct.unpack_from('<fff', d, p)
            out.append((x, z, -y))                              # IVP axes -> game axes
        o = start + size
    return np.array(out) / 0.0254                               # metres -> units

def read_physics_smd(path):
    L = open(path, encoding='latin1').read().replace('\r\n', '\n').split('\n')
    head = L[:L.index('triangles') + 1]
    if sum(1 for l in head if l.strip().startswith('time')) != 1 or 'end' not in head:
        raise SystemExit('%s: expected one skeleton frame' % path)
    skel = head[head.index('nodes') + 1:]
    bones = []
    for l in skel:
        if l.strip() == 'end': break
        bones.append(l)
    if len(bones) != 1:
        raise SystemExit('%s: %d bones - this fits single-bone collision only' % (path, len(bones)))
    rest = head[[k for k, l in enumerate(head) if l.strip().startswith('time')][0] + 1].split()
    if any(abs(float(v)) > 1e-5 for v in rest[1:]):
        raise SystemExit('%s: the root bone is not at rest (%s) - fit the posed vertices first' % (path, ' '.join(rest[1:])))
    i, tris = len(head), []
    while L[i].strip() != 'end':
        mat = L[i].strip(); vs = []
        for k in range(3):
            f = L[i + 1 + k].split(); vs.append((np.array(list(map(float, f[1:4]))), np.array(list(map(float, f[4:7]))), f[7:]))
        tris.append((mat, vs)); i += 4
    return bones[0].strip(), tris

def G_from_mesh():
    return np.array([[0, -1, 0], [1, 0, 0], [0, 0, 1]], float)  # game = this @ mesh

def fit(smd, phy):
    """(M, miss): M maps the SMD's corners into the MESH frame; miss = worst corner's distance to the .phy (units)"""
    from scipy.spatial import cKDTree
    _, tris = read_physics_smd(smd)
    S = np.array([p for _, vs in tris for p, n, r in vs]); Q = phy_points(phy); tree = cKDTree(Q)
    best = None
    for perm in itertools.permutations(range(3)):
        for signs in itertools.product((1, -1), repeat=3):
            A = np.zeros((3, 3))
            for r, (c, sg) in enumerate(zip(perm, signs)): A[r, c] = sg     # SMD -> game
            miss = tree.query(S @ A.T)[0].max()
            # and the other way: every .phy corner must be an SMD corner too (no piece missing)
            back = cKDTree(S @ A.T).query(Q)[0].max()
            score = max(miss, back)
            if best is None or score < best[1]: best = (A, score)
    A, miss = best
    return G_from_mesh().T @ A, miss                            # SMD -> game -> mesh

def write_fitted(smd, phy, out):
    """the decompile's physics SMD, written in the reference mesh's frame; stops if it doesn't fit the .phy"""
    M, miss = fit(smd, phy)
    if miss > FIT_TOLERANCE:
        raise SystemExit('%s: no axis order puts it on %s (best miss %.2f units) - not the same collision?' % (smd, phy, miss))
    bone, tris = read_physics_smd(smd)
    flip = np.linalg.det(M) < 0                                 # a mirror turns the winding: swap two corners back
    with open(out, 'w', newline='\n') as f:
        f.write('version 1\nnodes\n%s\nend\nskeleton\ntime 0\n0 0 0 0 0 0 0\nend\ntriangles\n' % bone)
        for mat, vs in tris:
            f.write(mat + '\n')
            for p, n, r in ((vs[0], vs[2], vs[1]) if flip else vs):
                q, m = M @ p, M @ n
                f.write('0 %.6f %.6f %.6f %.6f %.6f %.6f %s\n' % (q[0], q[1], q[2], m[0], m[1], m[2], ' '.join(r)))
        f.write('end\n')
    return M, miss

def check_written(out, phy):
    """the written SMD, turned the way studiomdl turns the mesh, lands on the compiled collision"""
    from scipy.spatial import cKDTree
    _, tris = read_physics_smd(out)
    S = np.array([p for _, vs in tris for p, n, r in vs]) @ G_from_mesh().T
    Q = phy_points(phy)
    return max(cKDTree(Q).query(S)[0].max(), cKDTree(S).query(Q)[0].max())

if __name__ == '__main__':
    M, miss = fit(sys.argv[1], sys.argv[2])
    print('SMD -> mesh frame:\n%s\nworst corner miss %.3f units (tolerance %.1f)' % (M.astype(int), miss, FIT_TOLERANCE))
