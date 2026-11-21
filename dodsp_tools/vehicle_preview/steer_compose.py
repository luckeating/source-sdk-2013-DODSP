"""H2: the halftrack driver STEERING - CoD2's own halftrack_driver_idle (made for this seat) plus the steering motion of
CoD2's Kubelwagen driver (kubelwagon_elder_drivehard*), taken as a DELTA (his turn minus his own idle) on the upper
body only, local rotation by local rotation. The Kubelwagen's seat is lower and its posture cramped, so its whole
pose doesn't fit the halftrack (driver_preview.py shows it); its arm-and-shoulder motion does.

    python3 steer_compose.py <ride pack dir> <out dir> [pack .qc]
        writes ww2_a_halftrack_driver_steer_left/right.smd; with the pack's .qc, also adds (once) the driver's seated
        sequence "ww2_halftrack_driver": $poseparameter "steer" -1 1 blending steer_right / idle / steer_left -
        the sequence the halftrack's driver seat names (dodsp_vehicles.txt "crew_seats")

The game blends three ONE-FRAME poses by the pose parameter "steer" (-1 full right .. 0 idle .. 1 full left, = the
server's steering angle / full lock): halftrack_driver_idle (1 frame) and, from each 50-frame composed turn, the frame
where the upper body is turned furthest from the idle (sum of the local rotation angles) - a blend's animations must
hold the same number of frames, and a steering wheel held at an angle is a pose, not a motion.
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

def peak(s, base):
    """the frame turned furthest from the base pose (sum over bones of the local rotation angle between them)"""
    b0 = base.frames[0][1]; best = (-1.0, 0)
    for f, (t, fr) in enumerate(s.frames):
        d = 0.0
        for k, v in fr.items():
            if k in b0:
                R = rotm(b0[k][3:]).T @ rotm(v[3:]); d += np.degrees(np.arccos(np.clip((np.trace(R) - 1) / 2, -1, 1)))
        best = max(best, (d, f))
    return best

if __name__ == '__main__':
    pack, outdir = sys.argv[1:3]
    base = SMD(os.path.join(pack, 'ww2_a_%s.smd' % BASE))
    for name, turn in TURNS.items():
        s = compose(pack, turn)
        d, f = peak(s, base)
        s.frames = [(0, s.frames[f][1])]
        s.write(os.path.join(outdir, 'ww2_a_halftrack_driver_%s.smd' % name))
        print('ww2_a_halftrack_driver_%s.smd: frame %d of %s, upper body turned %.0f degrees (summed) from the idle' % (name, f, turn, d))
    if len(sys.argv) > 3:
        qc = sys.argv[3]; t = open(qc).read()
        if '"ww2_halftrack_driver"' not in t:
            ik = '\tikrule "rhand" release range 0 0 0 0\n\tikrule "lhand" release range 0 0 0 0\n'
            t += ('\n// H2: the halftrack driver, seated - steer -1 full right .. 0 idle .. 1 full left (steer_compose.py)\n'
                  '$poseparameter "steer" -1 1\n' +
                  ''.join('$animation "a_halftrack_driver_%s" "ww2_a_halftrack_driver_%s.smd" {\n\tfps 30\n\tloop\n%s}\n' % (n, n, ik)
                          for n in ('steer_left', 'steer_right')) +
                  '$sequence "ww2_halftrack_driver" {\n\t"a_halftrack_driver_steer_right"\n\t"a_halftrack_driver_idle"\n'
                  '\t"a_halftrack_driver_steer_left"\n\tblend "steer" -1 1\n\tloop\n\tfadein 0.2\n\tfadeout 0.2\n}\n')
            open(qc, 'w', newline='\n').write(t)
            print('%s: the driver sequence ww2_halftrack_driver added' % qc)
