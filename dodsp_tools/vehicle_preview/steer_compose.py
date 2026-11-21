"""H2: the halftrack driver STEERING - CoD2's own halftrack_driver_idle (made for this seat) plus the steering motion of
CoD2's Kubelwagen driver (kubelwagon_elder_drivehard*), taken as a DELTA (his turn minus his own idle) on the upper
body only, local rotation by local rotation. The Kubelwagen's seat is lower and its posture cramped, so its whole
pose doesn't fit the halftrack (driver_preview.py shows it); its arm-and-shoulder motion does.

    python3 steer_compose.py <ride pack dir> <out dir>      writes ww2_a_halftrack_driver_steer_left/right.smd
"""
import os, sys, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__)); sys.path.insert(0, os.path.dirname(HERE)); sys.path.insert(0, HERE)
from smd import SMD
from xanim_to_smd import rotm, euler

UPPER = ('Spine1', 'Spine2', 'Spine4', 'Neck1', 'Head1', 'Clavicle', 'UpperArm', 'Forearm', 'Hand', 'Finger')
BASE, IDLE = 'halftrack_driver_idle', 'kubelwagon_elder_driveidleb'
TURNS = {'steer_left': 'kubelwagon_elder_drivehardleft', 'steer_right': 'kubelwagon_elder_drivehardright'}

def compose(pack, turn):
    base = SMD(os.path.join(pack, 'ww2_a_%s.smd' % BASE)); idle = SMD(os.path.join(pack, 'ww2_a_%s.smd' % IDLE))
    tn = SMD(os.path.join(pack, 'ww2_a_%s.smd' % turn))
    out = SMD(); out.nodes = list(base.nodes); out.frames = []
    b0, i0 = base.frames[0][1], idle.frames[0][1]
    for t, fr in tn.frames:
        new = {}
        for k, v in b0.items():
            n = base.nodes[k][0]
            if any(u in n for u in UPPER) and k in i0 and k in fr:
                # local delta of the Kubelwagen driver from his idle, on the halftrack driver's own pose
                D = rotm(i0[k][3:]).T @ rotm(fr[k][3:])
                new[k] = list(v[:3]) + list(euler(rotm(v[3:]) @ D))
            else:
                new[k] = list(v)
        out.frames.append((t, new))
    return out

if __name__ == '__main__':
    pack, outdir = sys.argv[1:3]
    for name, turn in TURNS.items():
        s = compose(pack, turn); s.write(os.path.join(outdir, 'ww2_a_halftrack_driver_%s.smd' % name))
        print('ww2_a_halftrack_driver_%s.smd: %d frames (from %s)' % (name, len(s.frames), turn))
