import sys, os
sys.path.insert(0, os.path.dirname(__file__))
import numpy as np
import matplotlib; matplotlib.use('Agg')
import matplotlib.pyplot as plt
from smd import SMD
from retarget_cod1 import worlds
def segs(s, f):
    W = worlds(s, f); out = []
    for i, (n, p) in enumerate(s.nodes):
        if i in W and p in W and ('bip01' in n.lower()) and 'finger' not in n.lower():
            out.append((W[p][1], W[i][1]))
    return W, out
def draw(ax, s, f, gun, view, title, col):
    W, sg = segs(s, f)
    a, b = (1, 2) if view == 'side' else (0, 2)
    for p, q in sg: ax.plot([p[a], q[a]], [p[b], q[b]], '-', color=col, lw=1.4)
    if gun:
        bone, muz = gun
        i = s.index(bone)
        if i in W:
            R, t = W[i]; m = R @ muz + t; ax.plot([t[a], m[a]], [t[b], m[b]], '-', color='tab:red', lw=3)
    ax.set_title(title, fontsize=7); ax.set_aspect('equal'); ax.set_xlim(-45, 45); ax.set_ylim(-5, 75); ax.axis('off')
