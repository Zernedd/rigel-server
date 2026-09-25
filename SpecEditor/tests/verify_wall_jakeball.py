"""verify_wall_jakeball.py -- checks the MiniJakeball arena build_wall_jakeball.py made, by measurement:
  1. every structural box's real bounds match what was intended (scale, rotation and position -- within 3 cm);
  2. nothing that was placed is missing;
  3. every boost pickup rests on the surface under it (nothing floats);
  4. no holes: rays from a grid of points inside the arena, in every direction, must hit the arena's own shell.
     The only openings allowed are the doorway (into the team doors) and the goal mouths (the ray must end on the
     goal box behind them).
Run with the editor connected (RIGEL_MCP_PORT) and the level open. Exit code 0 = all good."""
import json
import math
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "luau", "mcp"))
import rigel_mcp as m   # noqa: E402

FLOOR_X, Y0, Z0 = -28575.0, 11300.0, -300.0
L, W, HR, T = 5600.0, 3400.0, 860.0, 60.0
hl, hw = L / 2, W / 2
GOAL_GAP, GOAL_H, DEPTH = 900.0, 380.0, 380.0
DOOR_GAP, DOOR_H = 3 * 60.0 + 2 * 380.0, 330.0
LEDGE_H = 470.0


def at(u, v, h):
    return [FLOOR_X + h, Y0 + u, Z0 - v]


def frame(p):
    return (p[1] - Y0, Z0 - p[2], p[0] - FLOOR_X)


placed = json.load(open(os.path.join(os.environ.get("TEMP", "."), "wall_jakeball_placed.json")))
objs = m.BR.call("objects", "", *at(0, 0, 400), 6000, 3000)["objects"]
fails = []

# ---- 1 + 2: presence (by class and place -- reopening a level gives every object a new id) and sizes
nbox = 0
used = set()
for pc in placed:
    pu, pv, ph = pc["at"]
    best, bd = None, 1e9
    for o in objs:
        if o["class"] != pc["class"] or o["handle"] in used:
            continue
        u, v, h = frame(o["location"])
        d = math.hypot(u - pu, v - pv) + (0 if "Boost" in pc["class"] else abs(h - ph))   # boosts were settled along h
        if d <= 5.0 and pc["kind"] == "box" and o.get("boundsMin"):
            # pieces can share a centre (a goal net's upright and crossbar): among those, the one of the right size
            a, b = frame(o["boundsMin"]), frame(o["boundsMax"])
            got = [min(a[0], b[0]), max(a[0], b[0]), min(a[1], b[1]), max(a[1], b[1]), min(a[2], b[2]), max(a[2], b[2])]
            d = min(d, 5.0) / 100.0 + max(abs(g - w) for g, w in zip(got, pc["intended"])) / 1000.0
        if d < bd:
            best, bd = o, d
    if not best or bd > 5.0:
        fails.append(f"MISSING {pc['label']} ({pc['class']}) at {[round(x) for x in pc['at']]}")
        continue
    used.add(best["handle"])
    o = best
    if pc["kind"] != "box" or not o.get("boundsMin"):
        continue
    nbox += 1
    a, b = frame(o["boundsMin"]), frame(o["boundsMax"])
    got = [min(a[0], b[0]), max(a[0], b[0]), min(a[1], b[1]), max(a[1], b[1]), min(a[2], b[2]), max(a[2], b[2])]
    err = max(abs(g - w) for g, w in zip(got, pc["intended"]))
    if err > 3.0:
        fails.append(f"SIZE {pc['label']}: wanted {[round(x) for x in pc['intended']]} got {[round(x) for x in got]} (off {err:.0f} cm)")
print(f"checked {nbox} boxes against their intended size and place; {len(placed)} pieces placed")

# ---- 3: nothing floats
for o in objs:
    if "BoostPickup" not in o["class"] or not o.get("boundsMin"):
        continue
    u, v, h = frame(o["location"])
    if abs(u) > hl + 200 or abs(v) > hw + 200:
        continue
    bottom = o["boundsMin"][0] - FLOOR_X
    surface = LEDGE_H if abs(v) > hw - 300 and bottom > 200 else 0.0
    if abs(bottom - surface) > 3.0:
        fails.append(f"FLOATING {o['class']} at u={u:.0f} v={v:.0f}: bottom {bottom:.0f}, surface {surface:.0f}")
print("pickups checked")

# ---- 4: holes
dirs = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1)]
dirs += [(math.cos(a), math.sin(a), 0.3) for a in [i * math.pi / 4 + math.pi / 8 for i in range(8)]]
rays = holes = 0
for u in range(-2400, 2401, 600):
    for v in range(-1300, 1301, 650):
        for h in (150, 450, 750):
            for d in dirs:
                n = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
                du, dv, dh = d[0] / n, d[1] / n, d[2] / n
                start = at(u, v, h)
                wdir = [dh, du, -dv]        # frame -> world direction
                r = m.BR.call("trace", *start, *wdir, 12000)
                rays += 1
                # through the doorway (the way out to the team doors): judged where the ray crosses the door wall
                if dv < 0:
                    t = (-(hw + T) - v) / dv
                    if abs(u + du * t) < DOOR_GAP / 2 and 0 <= h + dh * t < DOOR_H:
                        continue
                if not r.get("hit"):
                    # openings: the doorway (-v, middle, low) and the goal mouths (+-u, middle, low)
                    fails.append(f"HOLE: ray from ({u},{v},{h}) dir ({du:.2f},{dv:.2f},{dh:.2f}) hit nothing")
                    holes += 1
                    continue
                hu, hv, hh = frame(r["location"])
                inside = abs(hu) <= hl + T + DEPTH + T + 15 and abs(hv) <= hw + T + 25 and -5 <= hh <= HR + T + 15
                in_door = hv < -hw - T + 5 and abs(hu) < DOOR_GAP / 2 and hh < DOOR_H + 5
                if not inside and not in_door:
                    fails.append(f"HOLE: ray from ({u},{v},{h}) dir ({du:.2f},{dv:.2f},{dh:.2f}) escaped to "
                                 f"({hu:.0f},{hv:.0f},{hh:.0f}) on {r.get('actor')}")
                    holes += 1
print(f"{rays} rays cast, {holes} escaped")

print("\n".join(fails[:80]) if fails else "ALL GOOD")
print(f"{len(fails)} problem(s)")
sys.exit(1 if fails else 0)
