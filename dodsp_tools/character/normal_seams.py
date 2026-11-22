"""Which way a normal map's red AND green axes point, measured on the mesh that wears it (UV seams).

A baked normal map is continuous across a UV seam: the two triangles that share a 3D edge but sit in different places
(often mirrored or turned) on the texture must give the same normal in OBJECT space. Turned into object space with
each side's own tangent frame, only the right signs of x and y make both sides agree; a wrong sign turns one side's
normal differently from the other's wherever the two islands are oriented differently.

Why it exists (2026-11-21): green_points_down's curl test reads only whether x and y agree with EACH OTHER (red flipped
and green flipped give the same curl). CoD3's tunic map is red-flipped (its buttons came out as dents once green was
flipped), its helmet map green-flipped (the vent lug a raised ring with a hole either way round would not be). Only a
test against the mesh settles both. Source's convention (what the result is in): +x toward image-right (+u),
+y toward image-DOWN, z out of the surface.

mesh_seams(smd) -> per material: list of (P, N, uv) triangle pairs across seams; seam_signs(...) scores the 4 sign
combinations of (x, y) and returns the best with its margin."""
import numpy as np, collections

def read_smd_tris(path):
    """(material, [(pos, normal, (u, v_smd))] x3) per triangle"""
    L = open(path).read().split('\n'); i = L.index('triangles') + 1; out = []
    while i < len(L) and L[i] != 'end':
        m = L[i]; c = []
        for s in L[i + 1:i + 4]:
            p = s.split(); c.append((np.array(p[1:4], float), np.array(p[4:7], float), (float(p[7]), float(p[8]))))
        out.append((m, c)); i += 4
    return out

def _frame(c, W, H):
    """tangent frame of a triangle in image texels: T = dP/dcol, B = dP/drow (row = (1 - v_smd) x H, image-down)"""
    P = np.array([q[0] for q in c]); UV = np.array([[q[2][0] * W, (1 - q[2][1]) * H] for q in c])
    dP = np.array([P[1] - P[0], P[2] - P[0]]); dU = np.array([UV[1] - UV[0], UV[2] - UV[0]])
    det = np.linalg.det(dU)
    if abs(det) < 1e-9: return None
    TB = np.linalg.solve(dU, dP)                      # rows: dP/dcol, dP/drow
    N = np.cross(P[1] - P[0], P[2] - P[0]); N /= np.linalg.norm(N) + 1e-12
    vn = sum(q[1] for q in c)
    if vn @ N < 0: N = -N                             # face normal on the side the vertex normals give
    T = TB[0] - (TB[0] @ N) * N; B = TB[1] - (TB[1] @ N) * N
    T /= np.linalg.norm(T) + 1e-12; B /= np.linalg.norm(B) + 1e-12
    return T, B, N, UV

def seam_samples(tris, W, H, inset=2.5):
    """for every 3D edge shared by two triangles with different UVs there: (frame A, texel A, frame B, texel B)"""
    key = lambda p: tuple(np.round(p, 3))
    edges = collections.defaultdict(list)
    for ti, (m, c) in enumerate(tris):
        for a, b in ((0, 1), (1, 2), (2, 0)):
            edges[tuple(sorted((key(c[a][0]), key(c[b][0]))))].append((ti, a, b))
    out = []
    for e, lst in edges.items():
        if len(lst) != 2: continue
        sides = []
        for ti, a, b in lst:
            f = _frame(tris[ti][1], W, H)
            if f is None: break
            T, B, N, UV = f; mid = (UV[a] + UV[b]) / 2; cen = UV.mean(0); d = cen - mid
            if np.linalg.norm(d) < 1e-6: break
            q = mid + d / np.linalg.norm(d) * inset
            sides.append((T, B, N, int(np.clip(q[1], 0, H - 1)), int(np.clip(q[0], 0, W - 1)), tuple(np.round(UV[a], 2)), tuple(np.round(UV[b], 2))))
        if len(sides) != 2: continue
        A_, B_ = sides
        if {A_[5], A_[6]} == {B_[5], B_[6]}: continue    # not a UV seam
        out.append((A_, B_))
    return out

def seam_signs(x, y, samples, decode, valid=None):
    """x, y: the map's raw channels (-1..1, image rows/cols); decode(x, y) -> unit tangent-space normals; valid: texels
    inside the UV islands (samples landing elsewhere are dropped). -> (sx, sy), scores {combo: median object-space
    disagreement}, number of seams used"""
    if valid is not None:
        samples = [(a, b) for a, b in samples if valid[a[3], a[4]] and valid[b[3], b[4]]]
    scores = {}
    for sx in (1, -1):
        for sy in (1, -1):
            n = decode(sx * x, sy * y); errs = []; tilt = []
            for A_, B_ in samples:
                oa = A_[0] * n[A_[3], A_[4], 0] + A_[1] * n[A_[3], A_[4], 1] + A_[2] * n[A_[3], A_[4], 2]
                ob = B_[0] * n[B_[3], B_[4], 0] + B_[1] * n[B_[3], B_[4], 1] + B_[2] * n[B_[3], B_[4], 2]
                errs.append(np.linalg.norm(oa - ob))
            scores[(sx, sy)] = float(np.median(errs))
    best = min(scores, key=scores.get)
    return best, scores, len(samples)

SEAM_INSETS = (1.5, 2.5, 4.0)       # texels inside each island: the verdict must not depend on where we sample
SEAM_MARGIN_MIN = 0.02              # median disagreement between the best and the next sign choice

def decide_signs(name, x, y, tris, decode, valid, curl_down, checked):
    """THE answer to "which way do this map's red and green point" (Source: +x image-right, +y image-down).
    curl_down: green_points_down's verdict = whether x and y already agree (product of the signs = +1).
    Decided by the seams when every inset picks the same choice, it agrees with the curl test, and the margin holds;
    else by `checked` (name -> ((sx, sy), why) - a verdict read off a feature of known shape by eye); else stops.
    -> (sx, sy, how)"""
    H, W = x.shape; picks = []
    for inset in SEAM_INSETS:
        best, sc, n = seam_signs(x, y, seam_samples(tris, W, H, inset), decode, valid)
        o = sorted(sc.values()); picks.append((best, o[1] - o[0], n))
    bests = {p[0] for p in picks}; margin = min(p[1] for p in picks)
    if len(bests) == 1:
        sx, sy = picks[0][0]
        if (sx * sy > 0) == curl_down and margin >= SEAM_MARGIN_MIN:
            return sx, sy, 'seams (%d, margin %.3f)' % (picks[0][2], margin)
    if name in checked:
        (sx, sy), why = checked[name]
        if (sx * sy > 0) != curl_down:
            raise SystemExit('%s: the checked signs %s disagree with the curl test - re-check by eye' % (name, (sx, sy)))
        return sx, sy, 'checked by eye: %s (seams undecided: %s)' % (why, ', '.join('%s %.3f' % (p[0], p[1]) for p in picks))
    raise SystemExit('%s: the seams do not settle its signs (%s) and nobody has checked it by eye - not guessing. Integrate '
                     'it to a height field and look at a feature of known shape (a button, a rivet), then add it to '
                     'NORMAL_SIGNS_CHECKED' % (name, ', '.join('%s margin %.3f' % (p[0], p[1]) for p in picks)))
