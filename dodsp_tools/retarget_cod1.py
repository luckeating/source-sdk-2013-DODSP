"""Retarget CoD1 (Max biped) animations onto the DoD ValveBiped skeleton.

Both rigs are 3ds Max bipeds, so every bone runs along its local X axis in both;
they differ by a fixed rotation about that axis per bone (0/90/180/270 deg), and
the pelvis frames differ by an axis swap. Corrections C (W_dod = W_cod @ C) are
measured once from each rig's rest pose (CoD1 "ragdoll" vs DoD bind) and snapped
to exact axis rotations, so pose differences between the two rest poses can't
leak in. Then per frame: world rotations via the correction, DoD bone lengths,
pelvis translation scaled by leg length, Spine2 (missing in CoD1) halfway between
Spine1 and Spine4. Guns: orientation follows CoD1's own weapon tag (so the barrel
points where the CoD animator aimed it), position stays at DoD's grip in the
right hand.
"""
import math, itertools
import numpy as np
from smd import SMD
from cod1_map import cod1_to_dod

def rotm(x, y, z):
    cx, sx, cy, sy, cz, sz = math.cos(x), math.sin(x), math.cos(y), math.sin(y), math.cos(z), math.sin(z)
    return (np.array([[cz, -sz, 0], [sz, cz, 0], [0, 0, 1]]) @ np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
            @ np.array([[1, 0, 0], [0, cx, -sx], [0, sx, cx]]))
def euler(R):
    y = math.asin(max(-1.0, min(1.0, -R[2, 0])))
    return math.atan2(R[2, 1], R[2, 2]), y, math.atan2(R[1, 0], R[0, 0])
def worlds(s, frame):
    W = {}
    for i, (_, p) in enumerate(s.nodes):
        if i not in frame: continue
        v = frame[i]; R = rotm(*v[3:]); t = np.array(v[:3])
        if p < 0 or p not in W: W[i] = (R, t)
        else:
            PR, Pt = W[p]; W[i] = (PR @ R, PR @ t + Pt)
    return W

AXIS_ROTS = []
for perm in itertools.permutations(range(3)):
    for signs in itertools.product((1, -1), repeat=3):
        M = np.zeros((3, 3))
        for r, (c, s) in enumerate(zip(perm, signs)): M[r, c] = s
        if abs(np.linalg.det(M) - 1) < 1e-6: AXIS_ROTS.append(M)
def snap(M):
    return max(AXIS_ROTS, key=lambda A: np.trace(A.T @ M))

def twist_about_x(Rc, Rd):
    """Correction for bones that both run along X: rotation about X by a multiple of 90deg."""
    best, bestscore = None, -9
    for k in range(4):
        a = k * math.pi / 2
        C = np.array([[1, 0, 0], [0, math.cos(a), -math.sin(a)], [0, math.sin(a), math.cos(a)]])
        R = Rc @ C
        score = np.dot(R[:, 1], Rd[:, 1]) + np.dot(R[:, 2], Rd[:, 2])
        if score > bestscore: best, bestscore = C, score
    return best

def slerp_m(A, B, t):
    # rotation halfway (t) from A to B via axis-angle of A^T B
    D = A.T @ B
    ang = math.acos(max(-1.0, min(1.0, (np.trace(D) - 1) / 2)))
    if ang < 1e-6: return A.copy()
    ax = np.array([D[2, 1] - D[1, 2], D[0, 2] - D[2, 0], D[1, 0] - D[0, 1]]) / (2 * math.sin(ang))
    a = ang * t; K = np.array([[0, -ax[2], ax[1]], [ax[2], 0, -ax[0]], [-ax[1], ax[0], 0]])
    return A @ (np.eye(3) + math.sin(a) * K + (1 - math.cos(a)) * K @ K)

class Retargeter:
    def __init__(self, cod1_rest_smd, dod_ref_smd, dod_grip_mp40, dod_grip_k98, gun_offsets):
        """gun_offsets: {'mp40': R, 'k98': R} rotation from CoD1 tag_weapon to the DoD gun bone."""
        self.gun_off = gun_offsets
        self.map = cod1_to_dod()
        self.cr = SMD(cod1_rest_smd); self.dr = SMD(dod_ref_smd)
        Wc = worlds(self.cr, self.cr.frames[0][1]); Wd = worlds(self.dr, self.dr.frames[0][1])
        self.C = {}
        for a, b in self.map.items():
            i = self.cr.index(a); j = self.dr.index(b)
            if i < 0 or j < 0: continue
            Rc, Rd = Wc[i][0], Wd[j][0]
            if 'pelvis' in a:
                self.C[b.lower()] = snap(Rc.T @ Rd)          # axis swap between the rigs
            elif a.endswith(' foot'):
                # CoD1's foot bone runs straight down to the ankle floor, DoD's runs toward
                # the toes: not a pure twist. Both rest poses stand flat-footed, so the exact
                # rest-to-rest rotation is the right correction here.
                self.C[b.lower()] = Rc.T @ Rd
            else:
                self.C[b.lower()] = twist_about_x(Rc, Rd)   # same bone axis, fixed roll
        # DoD bone lengths
        self.dod_local = {n.lower(): self.dr.frames[0][1][i] for i, (n, _) in enumerate(self.dr.nodes)}
        # leg length ratio for pelvis translation
        def leg(s, W, names):
            return sum(np.linalg.norm(W[s.index(names[k + 1])][1] - W[s.index(names[k])][1]) for k in range(len(names) - 1))
        lc = leg(self.cr, Wc, ['bip01 l thigh', 'bip01 l calf', 'bip01 l foot'])
        ld = leg(self.dr, Wd, ['ValveBiped.Bip01_L_Thigh', 'ValveBiped.Bip01_L_Calf', 'ValveBiped.Bip01_L_Foot'])
        self.scale = ld / lc
        self.grip_mp40 = dod_grip_mp40; self.grip_k98 = dod_grip_k98

    def frame(self, cod_smd, frame):
        Wc = worlds(cod_smd, frame)
        name_to_ci = {n.lower(): i for i, (n, _) in enumerate(cod_smd.nodes)}
        out = SMD(); out.nodes = list(self.dr.nodes)
        Wt = {}   # dod index -> (R, t) world
        for j, (n, p) in enumerate(self.dr.nodes):
            nl = n.lower()
            src = next((a for a, b in self.map.items() if b.lower() == nl), None)
            ci = name_to_ci.get(src) if src else None
            if ci is not None and ci in Wc and nl in self.C:
                R = Wc[ci][0] @ self.C[nl]
            elif nl.endswith('bip01_spine2') and j in range(len(self.dr.nodes)):
                R = None  # filled after Spine4
            else:
                R = None
            Wt[j] = R
        # Spine2: halfway between Spine1 and Spine4
        s1 = self.dr.index('ValveBiped.Bip01_Spine1'); s2 = self.dr.index('ValveBiped.Bip01_Spine2'); s4 = self.dr.index('ValveBiped.Bip01_Spine4')
        if Wt[s1] is not None and Wt[s4] is not None:
            Wt[s2] = slerp_m(Wt[s1], Wt[s4], 0.5)
        # assemble locals parents-first
        frame_out = {}; Wfull = {}
        for j, (n, p) in enumerate(self.dr.nodes):
            nl = n.lower()
            if nl.endswith('weapon_bone') or nl.endswith('weapon_bone_left'): continue
            bind = self.dod_local[nl]
            if p < 0:
                ci = name_to_ci['bip01 pelvis']
                t = Wc[ci][1] * self.scale
                R = Wt[j] if Wt[j] is not None else rotm(*bind[3:])
                Wfull[j] = (R, t)
                frame_out[j] = tuple(t) + euler(R)
                continue
            PR, Pt = Wfull[p]
            t = PR @ np.array(bind[:3]) + Pt
            R = Wt[j] if Wt[j] is not None else PR @ rotm(*bind[3:])
            Wfull[j] = (R, t)
            frame_out[j] = tuple(bind[:3]) + euler(PR.T @ R)
        # weapon grips from the right hand
        rh = self.dr.index('ValveBiped.Bip01_R_Hand'); lh = self.dr.index('ValveBiped.Bip01_L_Hand')
        tag = name_to_ci.get('tag_weapon')
        for bone, grip, parent, key in (('ValveBiped.weapon_bone', self.grip_mp40, rh, 'mp40'), ('ValveBiped.weapon_bone_left', self.grip_k98, lh, 'k98')):
            j = self.dr.index(bone)
            HR, Ht = Wfull[rh]
            Wtt = HR @ grip[:3, 3] + Ht
            WR = (Wc[tag][0] @ self.gun_off[key]) if tag is not None and tag in Wc else HR @ grip[:3, :3]
            PR, Pt = Wfull[parent]
            frame_out[j] = tuple(PR.T @ (Wtt - Pt)) + euler(PR.T @ WR)
        return frame_out

    def convert(self, src_path, dst_path):
        s = SMD(src_path)
        out = SMD(); out.nodes = list(self.dr.nodes)
        out.frames = [(t, self.frame(s, f)) for t, f in s.frames]
        out.write(dst_path)
        return len(out.frames)
