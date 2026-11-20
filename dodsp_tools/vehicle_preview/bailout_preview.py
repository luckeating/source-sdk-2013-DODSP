import sys, numpy as np
sys.path.insert(0, sys.argv[1]); sys.path.insert(0, sys.argv[2])
from mesh_tris import tris
from smd import SMD
from render_pose import world
import matplotlib; matplotlib.use('Agg'); import matplotlib.pyplot as plt
from matplotlib.collections import PolyCollection
V = sys.argv[3]
hull = tris(V + '/panzer2_hull.mdl'); tur = tris(V + '/panzer2_turret.mdl') + np.array([20, 0, 97])
veh = np.concatenate([hull, tur])
climb = SMD(sys.argv[4]); jump = SMD(sys.argv[5])
hatch = np.array([-15.6 + 20, -7.5, 6 + 97]); hw = 64.0
off = 7.5; ground = hatch + np.array([0, -(hw - off + 24), 0]); ground[2] = 4
def ss(x): return x * x * (3 - 2 * x)
shots = []
for t in (0.0, 0.25, 0.5, 0.75, 1.0, 1.27):
    f = 12 * t / 1.2 if t < 1.2 else min(12 + (t - 1.2) * 30, 14)
    lift = -42 * (1 - ss(min(t / 1.2, 1)))
    shots.append(('climb t=%.2fs f%.0f' % (t, f), climb, int(round(f)), hatch + [0, 0, lift], t >= 0.25))
for fr in (0, 10, 13, 16, 19, 22, 25, 39):
    c = fr / 39.0
    h = min(max((c - 10/39) / (15/39), 0), 1); v = min(max((c - 13/39) / (12/39), 0), 1)
    h = 1 - (1 - h) ** 2
    p = hatch + (ground - hatch) * h; p[2] = hatch[2] + (ground[2] - hatch[2]) * v * v
    shots.append(('jump f%d' % fr, jump, fr, p, True))
fig, axs = plt.subplots(2, 7, figsize=(28, 9))
for ax, (title, s, fr, o, vis) in zip(axs.ravel(), shots):
    ax.add_collection(PolyCollection(veh[:, :, [1, 2]], facecolor='#c8c0a8', edgecolor='none'))
    W = world(s, fr)
    for i, (n, pidx) in enumerate(s.nodes):
        if i in W and pidx in W and n.startswith('ValveBiped'):
            a = W[pidx][1] + o; b = W[i][1] + o
            ax.plot([a[1], b[1]], [a[2], b[2]], '-', color='k' if vis else '#bbbbbb', lw=1.6)
    ax.axhline(0, color='g'); ax.set_xlim(-130, 80); ax.set_ylim(-5, 190); ax.set_aspect('equal')
    ax.set_title(title + ('' if vis else ' (hidden)'), fontsize=10); ax.tick_params(labelsize=6)
axs.ravel()[-1].axis('off')
fig.suptitle('Panzer II seen from the front: commander bails out of the cupola toward the right (V4g timing)', fontsize=13)
plt.tight_layout(); plt.savefig(sys.argv[6], dpi=60)
