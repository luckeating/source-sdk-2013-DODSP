"""What the CLIENT draws when the server code moves an NPC - simulated, so a code-driven animation (a get-out, a
bail-out, a vault, a parachute landing) can be judged before a build.

    python3 motion_sim.py <scratchpad with the packs> [case ...]

Copies Source 2013 SP as the engine code does it (one owner each, re-read the engine before changing them):
  - server tick 0.015 s; an NPC thinks every 0.1 s (7 ticks); a snapshot every tick (single player).
  - the client draws a variable at  now - its interpolation amount  (c_baseentity.cpp GetInterpolationAmount):
    one tick for a SIMULATION var (origin) when the entity is "simulated every tick", one tick for an ANIMATION var
    (cycle, pose parameters) when "animated every tick", otherwise cl_interp 0.1 rounded up to ticks + 1 tick.
    Each var is drawn by linear interpolation between the two samples around that time; a sample's time is the
    simulation time (origin) or the anim time (animation vars) it arrived with.
  - MOVETYPE_NONE (what our scripted moves use): simulated every tick, NOT animated every tick.
  - MOVETYPE_STEP walking (sv_teststepsimulation 1, baseentity.cpp ComputeStepSimulationNetwork): the AI moves him
    at its think; the networked origin then glides from the last think's spot to this think's spot over the next
    think interval - Valve's way of putting position and pose on the same delay.
Clocks compared (the question: can position and pose disagree on screen?):
  stock  = V3b: origin set every tick, pose advanced at thinks (the shipped bug)
  tick   = V3c: animated every tick, the frame advanced every tick (both drawn one tick late)
  think  = origin set every tick but NOT simulated every tick: both drawn ~0.12 s late on one window
  step   = the engine's walking way: origin moved at thinks only and glided (root motion played by AutoMovement)
  delay  = THE BASE DESIGN (V3e, dodsp_npc_motion.cpp): origin set every tick, the NETWORKED origin = the real one one
           think ago (like the walking glide), animation clock stamped at start / sequence switch / end
Also: the walk -> move -> walk hand-off at a run (a clock that disagrees with walking jumps there) and a pose
parameter set at thinks (aim / head) drawn under each clock (steps = visible). Rule: every new code-driven animation
(vault, climb, jump-down, parachute, get-on/off) runs through this before a build, under 'delay', with the game's
path - worst bone error should stay within ~2 units outside sequence cuts (cuts show as one-frame spikes; the
engine's 0.2 s sequence blend covers them).
Measured per case: the worst distance between a drawn bone and the same bone in the TRUE motion delayed by the
clock's own constant latency (desync and stepping both show up here), how far a drawn foot goes under the floor,
and - for the "think-rate input" test - how a head turn set at thinks is drawn (steps vs smooth).
"""
import sys, os, re, numpy as np
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from smd import SMD
from render_pose import world

TICK = 0.015
THINK_TICKS = 7                          # 0.105 s: CAI_BaseNPC thinks every 0.1 s, on whole ticks
CL_INTERP = 0.1
SLOW_WIN = (int(np.ceil(CL_INTERP / TICK - 1e-6)) + 1) * TICK     # AdjustInterpolationAmount: ticks(cl_interp) + 1
FAST_WIN = TICK
RENDER_DT = 1.0 / 120.0

def G(p): return np.array([-p[1], p[0], p[2]])          # SMD frame -> +x forward (the retargeted packs)

class Anim:
    """bone world positions per frame, in place (the code's origin is added on top)"""
    def __init__(self, path, conv=G, fps=30.0):
        s = SMD(path); self.n = len(s.frames); self.fps = fps
        self.names = [n for n, p in s.nodes]
        W = [world(s, k) for k in range(self.n)]
        keep = sorted(W[0].keys())
        self.P = np.array([[conv(W[k][i][1]) for i in keep] for k in range(self.n)])     # frames x bones x 3
        self.idx = {self.names[i]: j for j, i in enumerate(keep)}
    def at(self, f):
        f = min(max(f, 0.0), self.n - 1.0); i = int(f); j = min(i + 1, self.n - 1); t = f - i
        return self.P[i] * (1 - t) + self.P[j] * t
    def bone(self, key):
        for n, j in self.idx.items():
            if key.lower() in n.lower().replace(' ', '_'): return j
        raise KeyError(key)

class Case:
    """one code-driven move: seq(t) -> (anim, frame), origin(t); t from 0 to dur; floor = the ground it ends on"""
    def __init__(self, name, segments, origin, dur, floor, note=''):
        self.name, self.segments, self.origin, self.dur, self.floor, self.note = name, segments, origin, dur, floor, note
    def pose(self, t):
        for a, t0, t1, frame in self.segments:          # frame(t - t0)
            if t < t1 or (a, t0, t1, frame) == self.segments[-1]:
                return a, frame(max(t - t0, 0.0))
    def truth(self, t):
        a, f = self.pose(t); return a.at(f) + self.origin(t)

def interp(samples, t):
    """samples: [(time, value)], sorted; linear between the two around t, held outside"""
    ts = [s[0] for s in samples]
    k = np.searchsorted(ts, t)
    if k <= 0: return samples[0][1]
    if k >= len(samples): return samples[-1][1]
    (t0, v0), (t1, v1) = samples[k - 1], samples[k]
    if t1 - t0 < 1e-9: return v1
    w = (t - t0) / (t1 - t0)
    if isinstance(v0, tuple):                              # (anim, frame): no blending across a sequence switch
        return v1 if v0[0] is not v1[0] else (v0[0], v0[1] * (1 - w) + v1[1] * w)
    return v0 * (1 - w) + v1 * w

def run(case, clock, phase=0):
    """phase = which tick of the NPC's think cycle the move starts on (0..6): the soldier's thinks don't line up with
    the vehicle's order to get off"""
    n_ticks = int(case.dur / TICK) + 60
    think = lambda k: (k + phase) % THINK_TICKS == 0
    org, anim, real = [], [], []                           # samples (time, value)
    last_think_org = None; prev_think_org = None; prev_think_tick = None
    anim_cycle = None                                      # what the server's cycle is, as advanced so far
    for k in range(-THINK_TICKS * 2, n_ticks):
        t = k * TICK
        tt = min(max(t, 0.0), case.dur)
        a, f = case.pose(tt)
        if clock in ('stock', 'think'):
            org.append((t, case.origin(tt)))
            # think: the move also stamps the animation clock where it starts, where it switches sequence and where
            # it ends (StudioFrameAdvance there, so the client's samples sit on those corners) - stock never did
            sync = clock == 'think' and any(abs(t - s0) < TICK / 2 for s0 in [seg[1] for seg in case.segments] + [case.dur])
            if think(k) or sync: anim.append((t, (a, f)))
        elif clock == 'delay':
            # the base design: the NETWORKED origin = the real one one think ago (what the engine's walking glide does),
            # read from real positions sampled every tick during the move and at thinks otherwise; pose as 'think'
            if 0.0 <= t <= case.dur or think(k) or not real: real.append((t, case.origin(tt)))
            org.append((t, interp(real, t - THINK_TICKS * TICK)))
            sync = any(abs(t - s0) < TICK / 2 for s0 in [seg[1] for seg in case.segments] + [case.dur])
            if think(k) or sync: anim.append((t, (a, f)))
        elif clock == 'tick':
            org.append((t, case.origin(tt))); anim.append((t, (a, f)))
        elif clock == 'step':
            if think(k):
                prev_think_org, last_think_org = last_think_org, case.origin(tt)
                prev_think_tick = k
                anim.append((t, (a, f)))
            if last_think_org is not None:
                if prev_think_org is None: o = last_think_org
                else: o = prev_think_org + (last_think_org - prev_think_org) * min((k - prev_think_tick) / THINK_TICKS, 1.0)
                org.append((t, o))
    wo = FAST_WIN if clock in ('stock', 'tick', 'step', 'delay') else SLOW_WIN
    wa = FAST_WIN if clock == 'tick' else SLOW_WIN
    lat = {'stock': None, 'tick': FAST_WIN, 'think': SLOW_WIN, 'step': SLOW_WIN, 'delay': THINK_TICKS * TICK + FAST_WIN}[clock]
    frames = np.arange(0.0, case.dur + 0.3, RENDER_DT)
    drawn = []
    for r in frames:
        o = interp(org, r - wo); a, f = interp(anim, r - wa)
        drawn.append(a.at(f) + o)
    drawn = np.array(drawn)
    # the true motion delayed by the clock's own constant latency (stock has none that fits: try the best one)
    best = None
    for L in ([lat] if lat is not None else np.arange(0.0, 0.16, 0.005)):
        tru = np.array([case.truth(min(max(r - L, 0.0), case.dur)) for r in frames])
        err = np.linalg.norm(drawn - tru, axis=2).max(axis=1)
        if best is None or err.max() < best[0].max(): best = (err, L, tru)
    err, L, tru = best
    feet = [case.segments[0][0].bone('L_Foot'), case.segments[0][0].bone('R_Foot')]
    under = max(0.0, (case.floor - drawn[:, feet, 2].min(axis=1)).max() - max(0.0, (case.floor - tru[:, feet, 2].min(axis=1)).max()))
    return dict(err=err.max(), err95=np.percentile(err, 95), lat=L, under=under, timeline=(frames, err))

def head_turn_test():
    """a pose parameter the AI sets at its thinks (CAI_BaseActor head turn, our aim): 0 -> 40 degrees over 0.5 s"""
    target = lambda t: 40.0 * min(max(t / 0.5, 0.0), 1.0)
    out = {}
    for clock in ('think', 'tick'):
        samples = []
        last = 0.0
        for k in range(0, 80):
            t = k * TICK
            if k % THINK_TICKS == 0: last = target(t)
            if clock == 'tick' or k % THINK_TICKS == 0: samples.append((t, last))
        w = FAST_WIN if clock == 'tick' else SLOW_WIN
        drawn = [interp(samples, r - w) for r in np.arange(0.2, 0.6, RENDER_DT)]
        steps = np.abs(np.diff(drawn))
        out[clock] = (steps.max(), np.median(steps))
    return out

def handoff_test(V=200.0, T1=0.6, T2=1.2, END=1.8):
    """a soldier running at V: walking (engine glide) until T1, a scripted move carrying on the same run until T2,
    walking again. A clock that disagrees with walking about the delay shows as a jump or a stall at a hand-off."""
    true = lambda t: V * t
    out = {}
    for clock in ('tick', 'think', 'delay'):
        org = []; real = []; prev = last = None; prevk = None
        for k in range(-20, int(END / TICK) + 20):
            t = k * TICK; moving = T1 <= t < T2; thinks = k % THINK_TICKS == 0
            if moving or thinks or not real: real.append((t, true(t)))
            if clock == 'delay' and T1 <= t < T2 + THINK_TICKS * TICK:
                o = interp(real, t - THINK_TICKS * TICK); w = FAST_WIN
                if thinks: prev, last, prevk = interp(real, t - THINK_TICKS * TICK), true(t), k
            elif moving:
                o = true(t); w = FAST_WIN if clock == 'tick' else SLOW_WIN
                prev, last, prevk = true(t), true(t), k
            else:
                if thinks: prev, last, prevk = last, true(t), k
                o = last if prev is None else prev + (last - prev) * min((k - prevk) / THINK_TICKS, 1.0); w = FAST_WIN
            org.append((t, o, w))
        d = []
        for r in np.arange(0.3, END, RENDER_DT):
            w = [x for x in org if x[0] <= r][-1][2]
            d.append(interp([(t, o) for t, o, _ in org], r - w))
        st = np.diff(np.array(d)); out[clock] = (np.median(st), st.max(), st.min())
    return out

# ---------------------------------------------------------------------------------------------------------- cases
def kv_paths(path):
    txt = open(path).read(); out = {}
    for m in re.finditer(r'"([\w]+)"\s*\{\s*"frames"\s*"(\d+)"\s*"take"\s*"\d+"\s*"land"\s*"\d+"(.*?)\n\t\}', txt, re.S):
        out[m.group(1)] = np.array([[float(x) for x in v.split()] for v in re.findall(r'"\d+"\s+"([-\d. ]+)"', m.group(3))])
    return out

def path_case(name, anim, P, note):
    n = anim.n
    origin = lambda t: (lambda f: P[min(int(f), n - 1)] * (1 - (f - int(f))) + P[min(int(f) + 1, n - 1)] * (f - int(f)))(min(t * 30.0, n - 1.0))[:3]
    floor = anim.P[-1][[anim.bone('L_Foot'), anim.bone('R_Foot')], 2].min() + P[-1][2] - 4.5
    return Case(name, [(anim, 0.0, 1e9, lambda t: t * anim.fps)], origin, (n - 1) / 30.0, floor, note)

def root_motion_case(name, path, conv=G, fps=30.0, note=''):
    """a CoD2 traversal made WITH its root motion: split like the ride paths (xy follows the pelvis, z the feet)"""
    raw = Anim(path, conv, fps)
    pel, lf, rf = raw.bone('Pelvis'), raw.bone('L_Foot'), raw.bone('R_Foot')
    feet = raw.P[:, [lf, rf], 2].min(axis=1)
    O = np.zeros((raw.n, 3)); O[:, :2] = raw.P[:, pel, :2] - raw.P[0, pel, :2]; O[:, 2] = feet - feet[0]
    inplace = Anim.__new__(Anim); inplace.__dict__.update(raw.__dict__); inplace.P = raw.P - O[:, None, :]
    origin = lambda t: (lambda f: O[int(f)] * (1 - (f - int(f))) + O[min(int(f) + 1, raw.n - 1)] * (f - int(f)))(min(t * fps, raw.n - 1.0))
    return Case(name, [(inplace, 0.0, 1e9, lambda t: t * fps)], origin, (raw.n - 1) / fps, feet[-1] - 4.5 + 0.0, note)

def bail_case(S):
    climb = Anim(os.path.join(S, 'cod1_v4b/c1_tank_climb_out.smd')); jump = Anim(os.path.join(S, 'cod1_v4b/c1_tank_jump_off.smd'))
    RISE, HOLD, HIDE = 1.2, 2.0 / 30.0, 42.0
    jdur = (jump.n - 1) / 30.0; t_jump = RISE + HOLD
    hatch, ground = np.array([0.0, 0.0, 70.0]), np.array([45.0, 0.0, 0.0])
    def origin(t):
        if t < t_jump:
            s = min(max(t / RISE, 0.0), 1.0); s = s * s * (3 - 2 * s); return hatch - np.array([0, 0, HIDE * (1 - s)])
        c = min(max((t - t_jump) / jdur, 0.0), 1.0)
        h = min(max((c - 10 / 39) / (25 / 39 - 10 / 39), 0), 1); v = min(max((c - 13 / 39) / (25 / 39 - 13 / 39), 0), 1)
        h = 1 - (1 - h) ** 2; p = hatch + (ground - hatch) * h; p[2] = hatch[2] + (ground[2] - hatch[2]) * v * v; return p
    climbframe = lambda t: 12.0 * min(t / RISE, 1.0) + max(t - RISE, 0.0) * 30.0
    return Case('bail-out (climb + jump, Panzer II hatch 70 up)', [(climb, 0.0, t_jump, climbframe), (jump, t_jump, 1e9, lambda t: t * 30.0)],
                origin, t_jump + jdur, 0.0, 'the V4 crew bail-out')

def para_case(S):
    """parachute: para_idle while coming down at 250 u/s, para_land from touchdown (CoD1 skeleton, raw)"""
    conv = lambda p: np.array([p[0], p[1], p[2]])
    idle = Anim(os.path.join(S, 'in_Assault/assault_anim_@para_idle.smd'), conv)
    land = Anim(os.path.join(S, 'in_Assault/assault_anim_@para_land.smd'), conv)
    feet0 = land.P[0][[land.bone('L_Foot'), land.bone('R_Foot')], 2].min()
    T = 1.0; speed = 250.0
    origin = lambda t: np.array([30.0 * t if t < T else 30.0 * T, 0.0, speed * (T - t) if t < T else 0.0])
    return Case('parachute touchdown (para_idle at 250 u/s -> para_land)', [(idle, 0.0, T, lambda t: (t * 30.0) % (idle.n - 1)),
                (land, T, 1e9, lambda t: t * 30.0)], origin, T + (land.n - 1) / 30.0, feet0 - 4.5, 'not built yet')

if __name__ == '__main__':
    S = sys.argv[1]; only = sys.argv[2:]
    cases = []
    paths = kv_paths(os.path.join(HERE, 'content/scripts/dodsp_ride_paths.txt'))
    for seq, anim in (('ww2_jump_ride1', 'tankride_guy1_jumptocombatrun'), ('ww2_jump_ride6', 'tankride_guy6_jumptocombatrun'),
                      ('ww2_jump_bren_carrier_back02_ridedismount', None)):
        if anim is None:
            m = [l.split('\t') for l in open(os.path.join(S, 'ride_pack2/SEQUENCES.txt')).read().splitlines()[1:]]
            anim = next(c[2][2:] for c in m if c[0] == seq)
        cases.append(path_case('rider get-out %s' % seq, Anim(os.path.join(S, 'ride_pack2/ww2_a_%s.smd' % anim)), paths[seq], 'V3 riders'))
    cases.append(bail_case(S))
    for f, fps in (('jump_down_56', 30), ('jump_down_96', 30), ('crouch_jump_down_40', 30), ('jump_over_low_wall', 30),
                   ('fenceclimb', 30), ('windowclimb', 30)):
        cases.append(root_motion_case('traversal %s (CoD2, root motion split off)' % f, os.path.join(S, 'pack4/ww2_a_%s.smd' % f), fps=fps,
                                      note='obstacle-course stage, not built'))
    cases.append(para_case(S))
    if only: cases = [c for c in cases if any(o in c.name for o in only)]
    print('worst bone error vs the true motion (units; 95th percentile in brackets) / feet drawn under the floor')
    CLOCKS = ('stock', 'tick', 'think', 'step', 'delay')
    print('%-56s %-21s %-21s %-21s %-21s %-21s' % ('case', 'stock (V3b)', 'tick (V3c)', 'think (synced)', 'step (engine walk)', 'delay (base design)'))
    for c in cases:
        row = []
        for clock in CLOCKS:
            worst = None
            for ph in range(THINK_TICKS):
                r = run(c, clock, ph)
                if worst is None or r['err'] > worst['err']: worst = r
            row.append('%5.1f (%4.1f) / %4.1f' % (worst['err'], worst['err95'], worst['under']))
        print('%-56s %-21s %-21s %-21s %-21s %-21s' % (c.name[:56], *row))
    for clock, (med, hi, lo) in handoff_test().items():
        print('hand-off walk -> move -> walk, running 200 u/s: %-5s clock drawn step per frame %.2f normally, %.1f largest, %.1f smallest'
              % (clock, med, hi, lo))
    h = head_turn_test()
    print('\nhead turn set at thinks (0 -> 40 deg in 0.5 s), drawn change per 1/120 s frame: '
          'think clock max %.2f deg (median %.2f) | tick clock max %.2f deg (median %.2f)' % (h['think'][0], h['think'][1], h['tick'][0], h['tick'][1]))
