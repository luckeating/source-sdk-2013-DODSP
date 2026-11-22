# V5 hull aim preview: DriveThink + HullAim for a wheeled vehicle (the halftrack: min_turn_radius 300, reverse 70, accel 60,
# brake 160, +-30 degree MG42) on open ground - how long and how many back/forward legs to bring the gun onto a target
# N degrees off the nose. Copies the code's rules (steer VEHICLE_HULLAIM_AIM inside the arc, stop at _MARGIN, 0.5-degree
# progress within _STALL). Run: python3 hullaim_sim.py
import math
def approach(t, v, d): return min(v + d, t) if t > v else max(v - d, t)
def angdiff(a, b): return (a - b + 180) % 360 - 180
def run(bearing, R=300.0, vmax=200, vrev=70, acc=60, brk=160, turnrate=28, half=120.0, arc=30, dt=0.05):
    x = y = 0.0; head = 0.0; spd = 0.0; t = 0.0
    legdir = 0; legstart = (0, 0); legs = 0; active = False; best = 1e9; bestat = 0
    tx, ty = 1000 * math.cos(math.radians(bearing)), 1000 * math.sin(math.radians(bearing))
    while t < 60:
        b = math.degrees(math.atan2(ty - y, tx - x)) - head
        b = angdiff(b, 0)
        half_arc = arc - (4 if active else -2)
        if abs(b) <= half_arc:
            return t, legs, math.hypot(x, y)
        aim_arc = arc - 7        # steer 3 degrees deeper than the stop margin (no asymptotic crawl)
        turn = b - (aim_arc if b > 0 else -aim_arc)
        if active:
            if abs(turn) < best - 0.5: best, bestat = abs(turn), t
            elif t - bestat > 5 or legs > 10: return None, legs, 'gave up %.0f short' % abs(turn)
        else: best, bestat = 1e9, t
        leg = half + 32
        if legdir == 0 or math.hypot(x - legstart[0], y - legstart[1]) >= leg:
            legdir = -1 if legdir == 0 else -legdir; legstart = (x, y); legs += 1
        active = True
        target = legdir * min(80, vrev if legdir < 0 else vmax)
        ratemax = min(math.degrees(abs(spd) / R), turnrate)
        yaw = math.copysign(min(abs(turn) * 2, ratemax), turn)
        rate = acc if abs(target) > abs(spd) else brk
        spd = approach(target, spd, rate * dt)
        head += yaw * dt
        x += math.cos(math.radians(head)) * spd * dt; y += math.sin(math.radians(head)) * spd * dt
        t += dt
    return None, legs, 'timeout'
for b in (45, 60, 90, 120, 150, 180):
    r = run(b)
    print('target %3d deg off the nose: %s' % (b, 'gun bears after %.1f s, %d legs, ends %.0f units from where it stood' % r if r[0] is not None else 'FAILED %s after %d legs' % (r[2], r[1])))
