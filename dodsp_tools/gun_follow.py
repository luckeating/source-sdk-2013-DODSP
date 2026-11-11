"""Shared helper: make the DoD gun follow whichever hand holds the gun in the SOURCE animation.

The CoD animations carry their own gun bone. Per frame we look at it and decide who has
the gun:
  R  right hand - the gun sits in the right hand the way it does in the reference aim pose;
  L  left hand  - the right hand isn't, and the left hand is on its usual grip spot (fore-end),
                  or near the gun and moving rigidly with it (CoD2 helmet hit, melee,
                  grenade throws where the left hand carries the rifle by its middle);
  F  free       - neither hand (dropped in a death, set down, slung by the animator).
The DoD gun is then placed through that hand in the DoD skeleton:
    T_dodgun = T_dodhand * (T_srchand^-1 * T_srcgun) * O
with O mapping the source gun frame onto DoD's gun bone, calibrated so that in the reference
aim pose the gun lands exactly on DoD's own grip. 'Free' frames use the source gun's place in
model space. Switches between holders are cross-faded over a few frames so the gun never pops.
All matrices are 4x4 numpy arrays in model space.
"""
import math
import numpy as np

RIGHT_POS_TOL = 4.0      # units: right hand may slide this far along its grip and still hold
RIGHT_ROT_TOL = 30.0     # degrees
LEFT_POS_TOL = 5.0       # units from the left hand's reference grip spot
LEFT_NEAR = 12.0         # ...or this close and rigid with the gun:
LEFT_SLIP = 0.5          # gun-in-left-hand change per frame (units + 0.1/deg)
SMOOTH_FRAMES = 4        # half-width of the cross-fade between holders (frames)

def inv(M):
    R = M[:3, :3]; t = M[:3, 3]; N = np.eye(4); N[:3, :3] = R.T; N[:3, 3] = -R.T @ t; return N

def rot_angle(A, B):
    return math.degrees(math.acos(max(-1.0, min(1.0, (np.trace(A[:3, :3].T @ B[:3, :3]) - 1) / 2))))

class GunReference:
    """Measured once from a reference aim pose of the SOURCE rig."""
    def __init__(self, src_R, src_L, src_G):
        self.rel_R = inv(src_R) @ src_G                 # gun in the right hand
        self.pL = (inv(src_G) @ src_L)[:3, 3]           # left hand's grip spot, in gun space

    def holder(self, src_R, src_L, src_G, slip_L=99.0):
        rel = inv(src_R) @ src_G
        if (np.linalg.norm(rel[:3, 3] - self.rel_R[:3, 3]) < RIGHT_POS_TOL
                and rot_angle(rel, self.rel_R) < RIGHT_ROT_TOL):
            return 0
        d = np.linalg.norm((inv(src_G) @ src_L)[:3, 3] - self.pL)
        if d < LEFT_POS_TOL or (d < LEFT_NEAR and slip_L < LEFT_SLIP):
            return 1
        return 2

def left_slip(src_frames):
    """Per frame: how much the gun moves inside the left hand (max over both neighbours)."""
    rel = [inv(L) @ G for _, L, G in src_frames]
    step = [np.linalg.norm(rel[k + 1][:3, 3] - rel[k][:3, 3]) + 0.1 * rot_angle(rel[k + 1], rel[k])
            for k in range(len(rel) - 1)]
    if not step: return [0.0] * len(rel)
    return [max(step[max(0, k - 1)], step[min(len(step) - 1, k)]) for k in range(len(rel))]

def _quat(R):
    w = math.sqrt(max(0.0, 1 + R[0, 0] + R[1, 1] + R[2, 2])) / 2
    x = math.copysign(math.sqrt(max(0.0, 1 + R[0, 0] - R[1, 1] - R[2, 2])) / 2, R[2, 1] - R[1, 2])
    y = math.copysign(math.sqrt(max(0.0, 1 - R[0, 0] + R[1, 1] - R[2, 2])) / 2, R[0, 2] - R[2, 0])
    z = math.copysign(math.sqrt(max(0.0, 1 - R[0, 0] - R[1, 1] + R[2, 2])) / 2, R[1, 0] - R[0, 1])
    return np.array([w, x, y, z])

def _mat(q):
    w, x, y, z = q / np.linalg.norm(q)
    return np.array([[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                     [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                     [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]])

def blend(mats, weights):
    q0 = None; qs = np.zeros(4); t = np.zeros(3); ws = 0.0
    for M, w in zip(mats, weights):
        if w <= 0: continue
        q = _quat(M[:3, :3])
        if q0 is None: q0 = q
        if np.dot(q, q0) < 0: q = -q
        qs += w * q; t += w * M[:3, 3]; ws += w
    out = np.eye(4); out[:3, :3] = _mat(qs); out[:3, 3] = t / ws
    return out

def holder_weights(labels):
    """One-hot holder per frame -> cross-faded weights (triangle kernel)."""
    n = len(labels); out = []
    for i in range(n):
        w = np.zeros(3)
        for k in range(-SMOOTH_FRAMES, SMOOTH_FRAMES + 1):
            j = min(n - 1, max(0, i + k))
            w[labels[j]] += SMOOTH_FRAMES + 1 - abs(k)
        out.append(w / w.sum())
    return out

def place_guns(ref, src_frames, dod_frames, offsets, free_scale=1.0):
    """src_frames: per frame (src_R, src_L, src_G); dod_frames: per frame (dod_R, dod_L).
    offsets: list of O matrices (one per DoD gun bone). Returns per frame a list of DoD
    model-space gun transforms (same order as offsets), plus the holder labels."""
    labels = [ref.holder(*f, slip) for f, slip in zip(src_frames, left_slip(src_frames))]
    weights = holder_weights(labels)
    result = []
    for (sR, sL, sG), (dR, dL), w in zip(src_frames, dod_frames, weights):
        free = sG.copy(); free[:3, 3] *= free_scale
        cands = [dR @ inv(sR) @ sG, dL @ inv(sL) @ sG, free]
        result.append([blend([c @ O for c in cands], w) for O in offsets])
    return result, labels
