"""CoD2 xanim (version 14) reader - the raw animations in the owner's cod2models.zip (3669 files).

Layout, worked out 2026-11-20 against the files themselves (every one of the 3669 parses to its last byte but one,
duhoc__cliffidle_point, whose frame count sits at the index-size edge) and checked on joint positions against the
owner's Crowbar decompile of the CoD2 officer port (the same animations, retargeted to ValveBiped by the porter):

    u16 version (14), u16 frames, u16 bones, u8 flags (1 = looping: one more frame, 2 = root motion "delta"), u16 fps
    if delta:   u16 rot keys, [frame index table], keys x int16 (yaw: z of a z-only quaternion)
                u16 trans keys, [index table], keys x float32 x3
    bit table "flip"   (bones/8 rounded up): NOT the sign of w - read as one, joints missed the port by 2.6 units
                       (median); ignored, by 0.2 (inverted: 9-11). w is always rebuilt positive. Kept as 'flip'.
    bit table "simple" (same size): the bone turns about its z only - one int16 (z) per key instead of three (x y z)
    bone names, NUL-terminated
    per bone:   u16 rot keys, [index table], keys x (int16 x3, or x1 if simple)   - LOCAL rotation, w rebuilt
                u16 trans keys, [index table], keys x float32 x3                    - LOCAL position (none = rest)
    u8 notetracks, each: name NUL-terminated + u16 frame
An index table (the frame of each key) is there only when 1 < keys < frames (+1 if looping): u8 per key when the frame
count fits a byte, else u16. One key = held for the whole animation; as many keys as frames = one per frame.
Quaternion components are int16 / 32767.
"""
import struct, numpy as np

def _keys(d, o, count, nfr):
    """frame of each key (a table when 1 < count < frames)"""
    if count == 0:
        return o, np.zeros(0, int)
    if count == 1:
        return o, np.zeros(1, int)
    if count >= nfr:
        return o, np.arange(count)
    if nfr <= 255:
        return o + count, np.frombuffer(d, np.uint8, count, o).astype(int)
    return o + 2 * count, np.frombuffer(d, '<u2', count, o).astype(int)

def _quat(x, y, z, flip=False):
    """w rebuilt positive (the flip bit is not its sign - see the layout above)"""
    w = max(0.0, 1.0 - x * x - y * y - z * z) ** 0.5
    return np.array([x, y, z, w])

def read(path):
    d = open(path, 'rb').read()
    ver, nf, nb, flags, fps = struct.unpack_from('<HHHBH', d, 0)
    if ver != 14:
        raise ValueError('%s: xanim version %d (this reads 14)' % (path, ver))
    nfr = nf + (1 if flags & 1 else 0)
    o = 9
    delta = None
    if flags & 2:
        rc, = struct.unpack_from('<H', d, o); o += 2
        o, rf = _keys(d, o, rc, nfr)
        rz = np.frombuffer(d, '<i2', rc, o) / 32767.0; o += 2 * rc
        tc, = struct.unpack_from('<H', d, o); o += 2
        o, tf = _keys(d, o, tc, nfr)
        tv = np.frombuffer(d, '<f4', 3 * tc, o).reshape(-1, 3).astype(float); o += 12 * tc
        delta = dict(rot=(rf, rz), trans=(tf, tv))
    nbytes = (nb - 1) // 8 + 1
    flip = d[o:o + nbytes]; o += nbytes
    simple = d[o:o + nbytes]; o += nbytes
    names = []
    for i in range(nb):
        e = d.index(b'\0', o); names.append(d[o:e].decode('latin1')); o = e + 1
    bones = {}
    for i, name in enumerate(names):
        fl = (flip[i // 8] >> (i % 8)) & 1; sm = (simple[i // 8] >> (i % 8)) & 1
        rc, = struct.unpack_from('<H', d, o); o += 2
        o, rf = _keys(d, o, rc, nfr)
        if sm:
            z = np.frombuffer(d, '<i2', rc, o) / 32767.0; o += 2 * rc
            rq = np.array([_quat(0.0, 0.0, zz, fl) for zz in z]) if rc else np.zeros((0, 4))
        else:
            v = np.frombuffer(d, '<i2', 3 * rc, o).reshape(-1, 3) / 32767.0; o += 6 * rc
            rq = np.array([_quat(*k, fl) for k in v]) if rc else np.zeros((0, 4))
        tc, = struct.unpack_from('<H', d, o); o += 2
        o, tf = _keys(d, o, tc, nfr)
        tv = np.frombuffer(d, '<f4', 3 * tc, o).reshape(-1, 3).astype(float); o += 12 * tc
        bones[name] = dict(rot=(rf, rq), trans=(tf, tv), flip=bool(fl), simple=bool(sm))
    notes = []
    nn = d[o]; o += 1
    for k in range(nn):
        e = d.index(b'\0', o); n = d[o:e].decode('latin1'); o = e + 1
        f, = struct.unpack_from('<H', d, o); o += 2; notes.append((n, f))
    if o != len(d):
        raise ValueError('%s: %d bytes left over - not the layout this reads' % (path, len(d) - o))
    return dict(frames=nf, fps=fps, looping=bool(flags & 1), names=names, bones=bones, delta=delta, notes=notes)

def _at(frames, values, f):
    """linear between the keys around frame f (held outside)"""
    if len(frames) == 1:
        return values[0]
    k = np.searchsorted(frames, f, side='right')
    if k <= 0: return values[0]
    if k >= len(frames): return values[-1]
    f0, f1 = frames[k - 1], frames[k]; t = (f - f0) / max(f1 - f0, 1e-9)
    a, b = values[k - 1], values[k]
    if a.shape[-1] == 4 and np.dot(a, b) < 0: b = -b          # quaternions: the short way round
    v = a * (1 - t) + b * t
    return v / np.linalg.norm(v) if v.shape[-1] == 4 else v

def qmat(q):
    x, y, z, w = q
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])

def root_motion(anim, f):
    """(R, t) of tag_origin at frame f: the animation's own travel ("delta": yaw + translation), identity without one"""
    d = anim['delta']
    R, t = np.eye(3), np.zeros(3)
    if d is None:
        return R, t
    rf, rz = d['rot']
    if len(rf):
        q = _at(rf, np.array([_quat(0.0, 0.0, z, False) for z in rz]), f); R = qmat(q)
    tf, tv = d['trans']
    if len(tf):
        t = _at(tf, tv, f)
    return R, t

def world(anim, skeleton, f, travel=True, offsets=False):
    """{bone name: (R, t)} at frame f, on an xmodelparts skeleton (cod2_xmodel.parts): an animated bone takes the
    animation's local rotation / position, the rest keep the skeleton's; the root (tag_origin) carries the animation's
    root motion when travel is True.
    offsets: an xanim's bone TRANSLATION keys are OFFSETS from the bone's rest position, not positions (2026-11-21,
    over all 3669: j_head 0..0.4, j_elbow_bulge 0..2 along the arm, 312 animations with all-zero keys on every arm
    joint - read as positions those put each joint ON its parent). offsets=True reads them right. The default (False)
    keeps the old reading for xanim_to_smd: it takes only rotations from here, and its learned pelvis constants absorbed
    j_mainroot's misread translation - so the delivered animation packs stay byte-identical. New code passes True."""
    out = {}
    for i, b in enumerate(skeleton):
        lR, lt = b['lR'], b['lt']
        if b['parent'] < 0 and travel:
            lR, lt = root_motion(anim, f)
        a = anim['bones'].get(b['name'])
        if a is not None:
            if len(a['rot'][0]): lR = qmat(_at(a['rot'][0], a['rot'][1], f))
            if len(a['trans'][0]): lt = (lt if offsets and b['parent'] >= 0 else 0) + _at(a['trans'][0], a['trans'][1], f)
        if b['parent'] < 0:
            out[b['name']] = (lR, lt)
        else:
            PR, Pt = out[skeleton[b['parent']]['name']]
            out[b['name']] = (PR @ lR, PR @ lt + Pt)
    return out
