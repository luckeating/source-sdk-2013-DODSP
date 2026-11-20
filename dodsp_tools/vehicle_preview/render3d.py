"""Tiny software renderer for the vehicle previews: a shaded, depth-buffered vehicle mesh (rendered once per
camera) and stick-figure riders splatted into it with the same depth test, so a rider inside a halftrack's bay
is hidden by its walls the way he would be in game. Orthographic camera; everything in game units."""
import numpy as np

class Camera:
    def __init__(self, az, el, centre, scale, w, h):
        a, e = np.radians(az), np.radians(el)
        self.fwd = np.array([np.cos(e) * np.cos(a), np.cos(e) * np.sin(a), np.sin(e)])   # target -> camera
        self.right = np.array([-np.sin(a), np.cos(a), 0.0])
        self.up = np.cross(self.fwd, self.right)
        self.c, self.s, self.w, self.h = np.asarray(centre, float), scale, w, h
    def project(self, P):
        P = np.asarray(P, float) - self.c
        return np.stack([self.w / 2 + P @ self.right * self.s, self.h / 2 - P @ self.up * self.s, P @ self.fwd], -1)

def _raster(cam, T, colours, Z, C):
    S = cam.project(T.reshape(-1, 3)).reshape(-1, 3, 3)
    for tri, col in zip(S, colours):
        x0, y0 = np.floor(tri[:, :2].min(0)).astype(int); x1, y1 = np.ceil(tri[:, :2].max(0)).astype(int)
        x0, y0, x1, y1 = max(x0, 0), max(y0, 0), min(x1, cam.w - 1), min(y1, cam.h - 1)
        if x0 > x1 or y0 > y1: continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + .5, np.arange(y0, y1 + 1) + .5)
        a, b, c = tri
        den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(den) < 1e-9: continue
        l1 = ((b[1] - c[1]) * (gx - c[0]) + (c[0] - b[0]) * (gy - c[1])) / den
        l2 = ((c[1] - a[1]) * (gx - c[0]) + (a[0] - c[0]) * (gy - c[1])) / den
        l3 = 1 - l1 - l2
        ins = (l1 >= -1e-4) & (l2 >= -1e-4) & (l3 >= -1e-4)
        d = l1 * a[2] + l2 * b[2] + l3 * c[2]
        zs = Z[y0:y1 + 1, x0:x1 + 1]; m = ins & (d > zs)
        zs[m] = d[m]; C[y0:y1 + 1, x0:x1 + 1][m] = col

def background(cam, T, colour=(212, 204, 180), ground=0.0, extent=600, grid=50):
    """the vehicle on a ground plane with a 50-unit grid (for judging distances), depth-buffered"""
    Z = np.full((cam.h, cam.w), -1e9); C = np.zeros((cam.h, cam.w, 3)); C[:] = (235, 238, 242)
    # ground: grid squares in two shades
    g = []; gc = []
    for i, x in enumerate(np.arange(-extent, extent, grid)):
        for j, y in enumerate(np.arange(-extent, extent, grid)):
            q = np.array([[x, y, ground], [x + grid, y, ground], [x + grid, y + grid, ground], [x, y + grid, ground]])
            g += [q[[0, 1, 2]], q[[0, 2, 3]]]; gc += [(196, 201, 186) if (i + j) % 2 else (186, 192, 176)] * 2
    _raster(cam, np.array(g), gc, Z, C)
    n = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); n /= np.linalg.norm(n, axis=1, keepdims=True) + 1e-9
    light = np.array([0.35, 0.55, 0.76]); light /= np.linalg.norm(light)
    k = 0.45 + 0.55 * np.abs(n @ light)
    _raster(cam, T, (np.array(colour)[None] * k[:, None]).clip(0, 255), Z, C)
    return Z, C

def splat_figures(cam, Z, C, figures):
    """figures: list of (segments [(a, b)...], colour). Limbs drawn as fat lines with the depth test."""
    Z = Z.copy(); C = C.copy()
    for segs, col in figures:
        pts = []; rad = []
        for a, b, r in segs:
            n = max(int(np.linalg.norm(b - a) / 0.8), 1)
            t = np.linspace(0, 1, n + 1)[:, None]
            pts.append(a + (b - a) * t); rad.append(np.full(n + 1, r))
        P = cam.project(np.concatenate(pts)); R = np.concatenate(rad) * cam.s
        shade = np.array(col, float)
        for (x, y, d), r in zip(P, R):
            ri = int(np.ceil(r)); x0, x1 = int(x) - ri, int(x) + ri; y0, y1 = int(y) - ri, int(y) + ri
            if x1 < 0 or y1 < 0 or x0 >= cam.w or y0 >= cam.h: continue
            gx, gy = np.meshgrid(np.arange(max(x0, 0), min(x1, cam.w - 1) + 1), np.arange(max(y0, 0), min(y1, cam.h - 1) + 1))
            dd = (gx + .5 - x) ** 2 + (gy + .5 - y) ** 2
            m = dd <= r * r
            depth = d + np.sqrt(np.maximum(r * r - dd, 0)) / cam.s
            zs = Z[gy, gx]; m &= depth > zs
            Z[gy[m], gx[m]] = depth[m]
            C[gy[m], gx[m]] = shade * (0.75 + 0.25 * np.sqrt(np.maximum(r * r - dd[m], 0)) / max(r, 1e-6))[:, None]
    return C.clip(0, 255).astype(np.uint8)
