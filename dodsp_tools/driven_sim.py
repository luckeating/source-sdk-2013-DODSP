"""What the CLIENT draws when the VEHICLE's code turns something that isn't the vehicle: the pintle MG and its gunner's
pose, a tank turret, the driver's steering pose. Simulated, so a vehicle-driven part can be judged before a build.

    python3 driven_sim.py

Engine rules copied (re-read the engine before changing one):
  - server tick 0.015 s, a snapshot every tick (single player). The vehicle thinks every 0.05 s (SetNextThink rounds
    to whole ticks: every 3). The gunner (an NPC, MOVETYPE_NONE, parented) thinks every 0.1 s (7 ticks) and his
    think calls StudioFrameAdvance (anim time = now).
  - our gun and turret are prop_dynamic_override with no default animation: CDynamicProp::Spawn gives them NO think,
    so their simulation time and anim time stay where spawning left them - nothing ever stamps their clock.
  - the client latches ANIMATION vars (pose parameters) only when the anim time changed, stamped with the anim time;
    SIMULATION vars (angles) when the angles / sim time changed, stamped with the simulation time
    (c_baseentity.cpp PostDataUpdate). A sample at a time <= the newest one replaces it (interpolatedvar.h AddToHead,
    bFlushNewer). While the entity is in the interpolation list, Interpolate() overwrites every var with its history
    - a networked value that was never latched doesn't show.
  - windows (GetInterpolationAmount): a sim var of an entity "simulated every tick" (any MOVETYPE_NONE prop) 1 tick,
    an anim var of one not "animated every tick" cl_interp 0.1 rounded up to ticks + 1 tick; the same for a sim var
    of an entity NOT simulated every tick. cl_interp_npcs (default 0) can widen it for NPCs and their children only.
Schemes:
  shipped = V2b as built: gun angles + gun pitch pose + gunner poses set in WeaponThink (0.05 s), nobody stamps the
            props' clocks, the gunner's clock moves at his own thinks
  driven  = THE FIX (dodsp_vehicle.cpp DrivenTick): every tick the vehicle turns its parts toward the live aim and
            stamps each part's clock (props: simulation + anim time, "not simulated every tick" so angles and poses
            share the NPC window; the gunner: anim time) - gun and gunner on one window, a sample every tick
Measured: the gun's drawn turn rate frame by frame (a hold or a jump = stutter: worst change of rate between frames,
and frames standing still while the true gun turns), and gun vs gunner yaw on screen (his hands on its grips).
"""
import math

TICK = 0.015
CL_INTERP = 0.1
SLOW_WIN = (math.ceil(CL_INTERP / TICK - 1e-6) + 1) * TICK
FAST_WIN = TICK
GRIP_R = 24.0          # units from the pintle to his hands (CoD2 MG42 on tag_turret, measured in mg_preview)

def interp(samples, t):
    if not samples: return 0.0
    if t <= samples[0][0]: return samples[0][1]
    for (t0, v0), (t1, v1) in zip(samples, samples[1:]):
        if t <= t1:
            return v1 if t1 - t0 < 1e-9 else v0 + (v1 - v0) * (t - t0) / (t1 - t0)
    return samples[-1][1]

def add(samples, stamp, v):
    while samples and samples[-1][0] + 1e-4 > stamp: samples.pop()       # AddToHead, bFlushNewer
    samples.append((stamp, v))

def want(t, case):
    if case == 'track':        # a man running across 500 units out at 200 u/s, then stopping
        x = -400 + 200 * min(t, 4.0)
        return math.degrees(math.atan2(x, 500.0))
    if case == 'swing':        # he shows up 25 degrees off the nose, the gunner swings onto him
        return 0.0 if t < 0.3 else 25.0
    raise ValueError(case)

def run(scheme, case, dur=5.0, rate=60.0, fps=120.0, npc_phase=0):
    n = int(dur / TICK)
    gun = 0.0; true = []
    gun_s, man_s = [(0.0, 0.0)], [(0.0, 0.0)]
    next_think = 0
    for k in range(n):
        t = k * TICK
        if scheme == 'shipped':
            if k >= next_think:                     # WeaponThink: aim from the scan's point (taken at that think)
                step = rate * 0.05
                w = want(t, case)
                gun += max(-step, min(step, w - gun))
                next_think = k + int(0.5 + 0.05 / TICK)     # SetNextThink rounds to ticks: every 3
                add(gun_s, 0.0, gun)                # stale simulation time: one sample, overwritten
            if (k + npc_phase) % 7 == 0:
                add(man_s, t, gun)                  # his think stamps the anim time with whatever the gun is now
        else:
            step = rate * TICK
            w = want(t, case)
            gun += max(-step, min(step, w - gun))
            add(gun_s, t, gun); add(man_s, t, gun)  # both clocks stamped every tick
        true.append((t, gun))
    win_gun = FAST_WIN if scheme == 'shipped' else SLOW_WIN
    frames, f = [], 0.0
    while f < dur - 0.2:
        f += 1.0 / fps
        arrived = lambda s: [x for x in s if x[0] <= f + 1e-9]
        if scheme == 'shipped':
            g = interp_shipped_gun(true, f)
        else:
            g = interp(arrived(gun_s), f - win_gun)
        m = interp(arrived(man_s), f - SLOW_WIN)
        frames.append((f, g, m, interp(true, f - win_gun)))
    rates = [(b[1] - a[1]) * fps for a, b in zip(frames, frames[1:])]
    jerk = max(abs(b - a) for a, b in zip(rates, rates[1:]))
    stills = sum(1 for (a, b, r) in zip(frames, frames[1:], rates) if abs(r) < 1e-6 and abs(b[3] - a[3]) * fps > 5.0)
    moving = sum(1 for a, b in zip(frames, frames[1:]) if abs(b[3] - a[3]) * fps > 5.0)
    gap = max(abs(g - m) for _, g, m, _ in frames)
    return jerk, stills, moving, gap

def interp_shipped_gun(true, f):
    # shipped: each WeaponThink's angle replaces the one stale-stamped sample, so the client draws the newest value
    # that has arrived - a staircase at the vehicle's think rate
    v = 0.0
    last = None
    for t, g in true:
        if t > f: break
        if last is None or g != last: v = g
        last = g
    return v

if __name__ == '__main__':
    print('gun traverse 60 deg/s, grips %.0f units from the pintle; rendered at 120 and 60 fps\n' % GRIP_R)
    print('%-8s %-6s %4s  %-28s %-26s %s' % ('scheme', 'case', 'fps', 'worst rate change (deg/s)', 'frames standing still', 'gun vs his hands'))
    for scheme in ('shipped', 'driven'):
        for case in ('track', 'swing'):
            for fps in (120.0, 60.0):
                worst = None
                for ph in range(7):
                    r = run(scheme, case, fps=fps, npc_phase=ph)
                    worst = r if worst is None or r[3] > worst[3] else worst
                jerk, stills, moving, gap = worst
                print('%-8s %-6s %4.0f  %-28.0f %-26s %.1f deg = %.1f units' % (scheme, case, fps, jerk,
                      '%d of %d moving' % (stills, moving), gap, math.radians(gap) * GRIP_R))
