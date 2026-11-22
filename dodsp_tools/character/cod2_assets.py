"""CoD2 (v20) character assets: xmodel (LODs + the material of each surface), material (its images), iwi (textures).
Layouts read from the files themselves (the owner's CoD3-for-CoD2 packs, 2026-11-21):
  xmodel:   u16 version 20, u8 flags, 6 floats (bounds), 4 x (float distance, NUL name of the LOD's parts/surfs),
            i32 (-1), i32 (0), then per LOD: u16 count + that many NUL material names - one per surface, in the
            surfs' order. The LOD names are the xmodelparts / xmodelsurfs file names as they are on disk.
  material: u32 name offset, ...; at 0x34 u32 count; at 0x44 count x (u32 map-name offset, u32 semantic, u32
            image-name offset): "colorMap" / "normalMap" / "specularMap" -> the images.
  iwi:      'IWi', u8 version 5, u8 format (0x0B DXT1, 0x0C DXT3, 0x0D DXT5, 0x01 ARGB8888, 0x02 RGB888), u8 usage,
            u16 width, u16 height, u16 depth, 4 x u32 (sizes at the picmip levels), then the mips SMALLEST FIRST - the
            full mip at the end of the file."""
import os, struct, numpy as np

def _cstr(d, o):
    e = d.index(b'\0', o); return d[o:e].decode('latin-1'), e + 1

def xmodel(path):
    d = open(path, 'rb').read()
    ver, = struct.unpack_from('<H', d, 0); assert ver == 20, (path, ver)
    o = 3 + 24; lods = []
    for i in range(4):
        dist, = struct.unpack_from('<f', d, o); o += 4
        name, o = _cstr(d, o); lods.append((dist, name))
    o += 8
    mats = []
    for i in range(4):
        if not lods[i][1]: break
        n, = struct.unpack_from('<H', d, o); o += 2; m = []
        for k in range(n):
            s, o = _cstr(d, o); m.append(s)
        mats.append(m)
    return dict(lods=lods, materials=mats)

def material(path):
    d = open(path, 'rb').read()
    n, = struct.unpack_from('<I', d, 0x34)
    out = {}
    for k in range(n):
        a, sem, b = struct.unpack_from('<3I', d, 0x44 + 12 * k)
        out[_cstr(d, a)[0]] = _cstr(d, b)[0]
    return out

def _dxt_colours(c0, c1, four):
    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31], -1).astype(np.float32)
    a, b = rgb(c0), rgb(c1)
    m = four[..., None]
    c2 = np.where(m, (2 * a + b) / 3, (a + b) / 2); c3 = np.where(m, (a + 2 * b) / 3, 0)
    return np.stack([a, b, c2, c3], -2)          # blocks x 4 x 3

def _decode_dxt(data, w, h, fmt):
    bw, bh = max(1, w // 4), max(1, h // 4)
    bs = 8 if fmt == 0x0B else 16
    blk = np.frombuffer(data, np.uint8, bw * bh * bs).reshape(bh * bw, bs)
    col = blk[:, -8:]
    c0 = col[:, 0].astype(np.uint32) | (col[:, 1].astype(np.uint32) << 8)
    c1 = col[:, 2].astype(np.uint32) | (col[:, 3].astype(np.uint32) << 8)
    pal = _dxt_colours(c0, c1, (c0 > c1) if fmt == 0x0B else np.ones_like(c0, bool))
    bits = col[:, 4:8].astype(np.uint32); idx = bits[:, 0] | (bits[:, 1] << 8) | (bits[:, 2] << 16) | (bits[:, 3] << 24)
    sel = np.stack([(idx >> (2 * i)) & 3 for i in range(16)], -1)          # blocks x 16
    rgb = np.take_along_axis(pal, sel[..., None].repeat(3, -1), 1)         # blocks x 16 x 3
    a = np.full(sel.shape, 255.0, np.float32)
    if fmt == 0x0B:
        a = np.where((c0 <= c1)[:, None] & (sel == 3), 0.0, 255.0)
    elif fmt == 0x0C:
        ab = blk[:, :8]; a = np.stack([((ab[:, i // 2] >> (4 * (i % 2))) & 15) * 17 for i in range(16)], -1).astype(np.float32)
    elif fmt == 0x0D:
        a0 = blk[:, 0].astype(np.float32); a1 = blk[:, 1].astype(np.float32)
        ab = blk[:, 2:8].astype(np.uint64); bits = np.zeros(len(blk), np.uint64)
        for i in range(6): bits |= ab[:, i] << np.uint64(8 * i)
        ai = np.stack([((bits >> np.uint64(3 * i)) & np.uint64(7)).astype(int) for i in range(16)], -1)
        tab = np.zeros((len(blk), 8), np.float32); tab[:, 0] = a0; tab[:, 1] = a1
        big = a0 > a1
        for k in range(2, 8):
            tab[:, k] = np.where(big, ((8 - k) * a0 + (k - 1) * a1) / 7, ((6 - k) * a0 + (k - 1) * a1) / 5 if k < 6 else (0 if k == 6 else 255))
        a = np.take_along_axis(tab, ai, 1)
    px = np.concatenate([rgb, a[..., None]], -1).reshape(bh, bw, 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(bh * 4, bw * 4, 4)
    return px[:h, :w].astype(np.uint8)

def iwi(path):
    """-> dict(fmt, w, h, rgba (h x w x 4 uint8, the full mip), mips [(w, h, bytes)] largest first)"""
    d = open(path, 'rb').read()
    assert d[:3] == b'IWi' and d[3] == 5, path
    fmt = d[4]; w, h, depth = struct.unpack_from('<HHH', d, 6)
    def size(w, h):
        if fmt in (0x0B, 0x0C, 0x0D): return max(1, (w + 3) // 4) * max(1, (h + 3) // 4) * (8 if fmt == 0x0B else 16)
        return w * h * {0x01: 4, 0x02: 3, 0x03: 2, 0x04: 1}[fmt]
    mips = []; end = len(d); mw, mh = w, h
    while True:
        s = size(mw, mh); mips.append((mw, mh, d[end - s:end])); end -= s
        if mw == 1 and mh == 1: break
        mw, mh = max(1, mw // 2), max(1, mh // 2)
    full = mips[0][2]
    if fmt in (0x0B, 0x0C, 0x0D): rgba = _decode_dxt(full, w, h, fmt)
    elif fmt == 0x01: rgba = np.frombuffer(full, np.uint8).reshape(h, w, 4)[..., [2, 1, 0, 3]]      # BGRA
    elif fmt == 0x02: rgba = np.concatenate([np.frombuffer(full, np.uint8).reshape(h, w, 3)[..., ::-1], np.full((h, w, 1), 255, np.uint8)], -1)
    else: raise ValueError('iwi format %d' % fmt)
    return dict(fmt=fmt, w=w, h=h, rgba=rgba, mips=mips, header_end=28, data_start=end)


SLOPE_MIN_Z = 1 / np.sqrt(3)      # the steepest normal the slope reading can give: (1, 1, 1) normalised, 54.7 degrees

def decode_normal(x, y):
    """A CoD '-gggr' texel (x from alpha, y from green, each -1..1) -> unit normal (h x w x 3). CoD3's maps are clipped
    PER AXIS: on all four maps of the first soldier the steep texels pile on the box edge |x| or |y| = 1 (body 20739,
    of them 9356 clearly outside the unit circle) rather than on the circle r = 1 (11177) - a unit-vector encoder never
    writes (1, 0.5). So x, y are SLOPES: n = normalize(x, y, 1). The first port read them as a unit vector's x, y with
    z = sqrt(1 - r^2): 2-4% of island texels (r >= 1) came out lying flat in the surface (z = 0) and drew as black
    streaks along every fold and finger crease - and no strength setting could lift them (z stayed 0)."""
    n = np.stack([x, y, np.ones_like(x)], -1)
    return n / np.linalg.norm(n, axis=-1, keepdims=True)

def normal_background(x, y, min_size=64):
    """The unused texels between UV islands: CoD's converter fills them with a constant corner value ((-1, 1) on the
    body, (-1, -1) on the head). Read as slopes they are 54.7-degree normals, and the mips and bilinear filtering pull
    them into the islands' edges (dark seams). -> mask of corner-valued regions of at least min_size texels."""
    from scipy import ndimage as nd
    corner = (np.abs(x) > 0.98) & (np.abs(y) > 0.98)
    lab, n = nd.label(corner)
    if not n: return corner
    sizes = nd.sum(corner, lab, range(1, n + 1))
    return np.isin(lab, 1 + np.where(sizes >= min_size)[0])

def pad_islands(n, background):
    """Each background texel takes the nearest island texel's normal (edge padding), so no mip level mixes in a
    normal that belongs to no surface."""
    from scipy import ndimage as nd
    if not background.any(): return n
    idx = nd.distance_transform_edt(background, return_distances=False, return_indices=True)
    return n[idx[0], idx[1]]

def green_points_down(x, y, valid):
    """Which way a normal map's green (Y) axis points, read from the map itself: a normal map baked from a height field
    has no curl, so d(x)/d(row) == d(y)/d(col) when +y tilts toward image-DOWN (Direct3D) and == -d(y)/d(col) when it
    tilts toward image-UP (OpenGL). Scored per quadrant at a 7-texel blur (DXT noise); returns (down?, ratios) and
    stops when the quadrants disagree. Source wants DOWN (measured on maps that draw right: DoD's german_body_normal
    2.0x, HL2 citizen_sheet_normal 2.1x). CoD3's maps are MIXED: c_wrmcht_body / helm1 UP (0.37 / 0.61), head / gear
    DOWN (2.4 / 1.2) - so every map is measured, never assumed (the body tunic shipped inverted: "extremely sharp")."""
    def blur(a, k=3):
        for ax in (0, 1): a = sum(np.roll(a, s, axis=ax) for s in range(-k, k + 1)) / (2 * k + 1)
        return a
    m = valid.copy()
    for _ in range(6):                                   # keep off the edges of the used islands (seams)
        m = m & np.roll(m, 1, 0) & np.roll(m, -1, 0) & np.roll(m, 1, 1) & np.roll(m, -1, 1)
    xb, yb = blur(x), blur(y); h, w = x.shape; ratios = []
    for i in range(2):
        for j in range(2):
            sl = (slice(i * h // 2, (i + 1) * h // 2), slice(j * w // 2, (j + 1) * w // 2))
            dxr = np.diff(xb[sl], axis=0)[:, :-1]; dyc = np.diff(yb[sl], axis=1)[:-1, :]; mm = m[sl][:-1, :-1]
            if mm.sum() < 500: continue
            ratios.append(float(np.abs(dxr + dyc)[mm].mean() / np.abs(dxr - dyc)[mm].mean()))
    votes = {r > 1 for r in ratios}
    if len(votes) != 1: raise SystemExit('normal map green axis: quadrants disagree %s - not guessing' % ratios)
    return votes.pop(), ratios
