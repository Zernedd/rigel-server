"""build_wall_jakeball.py -- builds MiniJakeball, a covered Jakeball arena on the station's flat wall pitch, through
the MCP (as an agent would), to the detail of the station's own Jakeball arenas, from gridded primitive cubes.

Every piece has a job:
  * shell -- walls, a full roof, cut corners (no dead corners for the ball), quarter-pipe ramps where the walls meet
    the floor and the roof (the ball rolls back into play instead of stopping dead), pilaster ribs, a trim band;
  * side-wall ledges at the wings -- aerial routes for wall play, each with a boost on it as the reward;
  * goals -- framed mouths, sealed goal boxes with a net lattice behind the station's glowing forcefield, the
    station's goal trigger across the mouth;
  * roof lights -- two forcefield light strips along the roof over the wings;
  * markings -- centre line, centre circle (the kickoff zone), penalty boxes (the defending zone), flat on the floor;
  * boards -- one score monitor on the left end wall (as you come in through the doors) above that goal; the team
    board right above the team doors, where the station arena has it;
  * four diamond bumpers at the midfield quarter points -- they break straight wing runs and make rebounds, and leave
    the central kickoff channel open;
  * boosts, as the station arena places them -- large in the four corners by the goals, small on each penalty spot
    (the defender's refill), by each side wall at midfield (the wing runs after kickoff) and on the ledges;
  * the "place the ball here" ring in the centre circle, one ball beside it, two team doors in a framed doorway at
    midfield (walk in = join, walk out = leave), a team label over each door (the only text).
Pickups are snapped onto the surface under them after placing (measured bounds): nothing floats.
  python build_wall_jakeball.py            build (on the wall pitch)
  python build_wall_jakeball.py --remove   delete the mode (and everything in it)
Needs the editor running with a level open. Check the result with verify_wall_jakeball.py."""
import json
import math
import os
import subprocess
import sys
import time

MCP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "luau", "mcp", "rigel_mcp.py")
p = subprocess.Popen([sys.executable, MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
nid = 0


def call(tool, **args):
    global nid
    nid += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid, "method": "tools/call", "params": {"name": tool, "arguments": args}}) + "\n")
    p.stdin.flush()
    r = json.loads(p.stdout.readline()).get("result", {})
    txt = (r.get("content") or [{}])[0].get("text", "")
    try:
        val = json.loads(txt)
    except Exception:
        val = txt
    if r.get("isError"):
        print("  !!", tool, str(val)[:300], flush=True)
    return (not r.get("isError")), val


p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": 0, "method": "initialize", "params": {}}) + "\n")
p.stdin.flush()
p.stdout.readline()

MODE = "MiniJakeball"
if "--remove" in sys.argv:
    print(call("gamemode_delete", mode=MODE))
    sys.exit(0)

# ---- the frame: the wall pitch is the plane x = FLOOR_X facing +X (flat and clear from y 6300 to 18300, z -3300 to
# 3700), centred on the painted pitch. u = world +Y (along), v = world -Z (across), h = world +X (up).
FLOOR_X, Y0, Z0 = -28575.0, 11300.0, -300.0
UP = [1.0, 0.0, 0.0]


def at(u, v, h):
    return [round(FLOOR_X + h, 1), round(Y0 + u, 1), round(Z0 - v, 1)]


# ---- sizes (cm)
L, W = 5600.0, 3400.0          # pitch inside the walls: length (u), width (v)
HR = 860.0                     # ceiling height. The roof must stay inside the mode's area (+-950 up from the floor):
T = 60.0                       # objects outside the area that hosts them don't draw
hl, hw = L / 2, W / 2
GOAL_GAP, GOAL_H, DEPTH = 900.0, 380.0, 380.0
DOOR_W, POST, DOOR_H = 380.0, 60.0, 330.0
DOOR_GAP = 3 * POST + 2 * DOOR_W
CHAMFER, RAMP, TOPRAMP = 520.0, 240.0, 300.0
MON_S = 0.8                                  # the score monitor's scale; BP_ScoreboardA_C is 7.6 m wide and 3.94 m tall at 1
MON_W, MON_HGT = 760.0 * MON_S, 394.0 * MON_S
RAMP_REACH = RAMP / math.sqrt(2)          # how far a 45-degree ramp reaches up the wall / out over the floor

# Only the gridded primitive cubes (the others cull). Scale is not centimetres: navy = 100 cm per unit, purple = 64.
NAVY, PURPLE = "Prefab_BP_StandardCubePrimitive_C", "Prefab_BP_Cube2Primitive_C"
# The station's Jakeball forcefield (MI_JakeBall_Forcefield_Interior), a 1 m cube: the glowing goal nets and the roof's
# light strips. (Its _Yellow_ twin renders almost black, so both goals use this one.)
SHIELD = "Prefab_BP_Primitive_Cube_Shield_C"
UNIT = {NAVY: 100.0, PURPLE: 64.0, SHIELD: 100.0}

placed = []


def put(item, u, v, h, yaw=0.0, pitch=0.0, scale=(1, 1, 1), label="", kind="item", intended=None):
    ok, o = call("place_object", item=item, location=at(u, v, h), rotation=[pitch, yaw, 0], scale=list(scale), floor_up=UP)
    if ok and isinstance(o, dict):
        placed.append({"label": label or item, "handle": o["handle"], "kind": kind, "intended": intended, "class": item,
                       "at": [u, v, h]})
        return o
    print(f"  !! failed: {label or item}", flush=True)
    return None


def box(cube, u0, u1, v0, v1, h0, h1, label):
    """A cube filling u0..u1 x v0..v1 x h0..h1 (cube axes: X across = v, Y along = u, Z up), sized for its class."""
    k = UNIT[cube]
    u0, u1 = sorted((u0, u1)); v0, v1 = sorted((v0, v1)); h0, h1 = sorted((h0, h1))
    return put(cube, (u0 + u1) / 2, (v0 + v1) / 2, (h0 + h1) / 2, scale=((v1 - v0) / k, (u1 - u0) / k, (h1 - h0) / k),
               label=label, kind="box", intended=[u0, u1, v0, v1, h0, h1])


def turned(cube, u, v, h, size_u, size_v, size_h, label, yaw=0.0, pitch=0.0):
    k = UNIT[cube]
    return put(cube, u, v, h, yaw=yaw, pitch=pitch, scale=(size_v / k, size_u / k, size_h / k), label=label, kind="turned")


def bounds_of(handle, near):
    ok, r = call("list_objects", near=near, radius=1500, limit=400)
    if not ok:
        return None
    base = handle.split("_")[0]
    for o in r.get("objects", []):
        if o["handle"].split("_")[0] == base and o.get("boundsMin"):
            return o
    return None


def settle(o, surface_h, label):
    """Move a placed item along the up axis so its bounds rest on surface_h (nothing floats, nothing sinks)."""
    if not o:
        return
    for _ in range(4):
        time.sleep(0.8)
        b = bounds_of(o["handle"], o["location"])
        if not b:
            continue
        d = surface_h - (b["boundsMin"][0] - FLOOR_X)
        if abs(d) < 1.5:
            print(f"  = {label}: resting on {surface_h:.0f}", flush=True)
            return
        loc = b["location"]
        call("move_object", handle=b["handle"], location=[loc[0] + d, loc[1], loc[2]])
        o["location"] = [loc[0] + d, loc[1], loc[2]]
    print(f"  ~ {label}: could not confirm it rests on {surface_h:.0f}", flush=True)


ok, st = call("editor_status")
print("editor:", st.get("openLevel"), st.get("worldReady"), flush=True)
ok, gm = call("gamemode_create", name=MODE, center=at(0, 0, 0), teams=2, team_names=["Blue", "Orange"], max_players=[4, 4])
print("mode:", gm if not ok else gm.get("id"), flush=True)
if not ok:
    sys.exit(1)
for k, v in (("script_flow", "1"), ("start_mode", "manual"), ("countdown", "0"), ("round_time", "0"), ("score_to_win", "0"),
             ("end_delay", "8"), ("auto_restart", "0"), ("stop_when_empty", "1"), ("reset_after_goal", "0")):
    call("gamemode_set", mode=MODE, key=k, value=v)
time.sleep(4)

# ====================================================================================================== the shell
print("shell...", flush=True)
# side walls run past the end walls by a wall's thickness (closed corners); the near one (-v) has the doorway
box(NAVY, -hl - T, hl + T, hw, hw + T, 0, HR, "wall far")
box(NAVY, -hl - T, -DOOR_GAP / 2, -hw - T, -hw, 0, HR, "wall near -u")
box(NAVY, DOOR_GAP / 2, hl + T, -hw - T, -hw, 0, HR, "wall near +u")
box(NAVY, -DOOR_GAP / 2, DOOR_GAP / 2, -hw - T, -hw, DOOR_H, HR, "wall over the doorway")
for pu in (-DOOR_GAP / 2, -POST / 2, DOOR_GAP / 2 - POST):   # door frame posts, proud of the wall on both faces
    box(PURPLE, pu, pu + POST, -hw - T - 20, -hw + 20, 0, DOOR_H + 40, "door post")
box(PURPLE, -DOOR_GAP / 2, DOOR_GAP / 2, -hw - T - 20, -hw + 20, DOOR_H, DOOR_H + 40, "door lintel")
door_u = {1: -(POST / 2 + DOOR_W / 2), 2: POST / 2 + DOOR_W / 2}
for team, s in ((1, -1), (2, 1)):               # end walls between the side walls, open for the goal mouth
    e0, e1 = s * hl, s * (hl + T)
    box(NAVY, e0, e1, GOAL_GAP / 2, hw, 0, HR, f"end wall {team} +v")
    box(NAVY, e0, e1, -hw, -GOAL_GAP / 2, 0, HR, f"end wall {team} -v")
    box(NAVY, e0, e1, -GOAL_GAP / 2, GOAL_GAP / 2, GOAL_H, HR, f"end wall {team} over the goal")
box(NAVY, -hl - T, hl + T, -hw - T, hw + T, HR, HR + T, "roof")    # the roof: one slab resting on every wall
for su in (-1, 1):                              # cut corners, floor to roof
    for sv in (-1, 1):
        turned(NAVY, su * hl, sv * hw, HR / 2, CHAMFER, CHAMFER, HR, f"corner {su:+d}{sv:+d}", yaw=45)

# ============================================================================================ ramps (quarter pipes)
print("ramps...", flush=True)
for sv in (-1, 1):
    spans = [(-hl + CHAMFER / 2, hl - CHAMFER / 2)] if sv > 0 else \
        [(-hl + CHAMFER / 2, -DOOR_GAP / 2 - 160), (DOOR_GAP / 2 + 160, hl - CHAMFER / 2)]
    for a, b in spans:
        turned(PURPLE, (a + b) / 2, sv * hw, 0, b - a, RAMP, RAMP, f"floor ramp side {sv:+d}", pitch=45)
    turned(NAVY, 0, sv * hw, HR, L - CHAMFER, TOPRAMP, TOPRAMP, f"roof ramp side {sv:+d}", pitch=45)
for su in (-1, 1):
    for sv in (-1, 1):
        a, b = sorted((sv * (GOAL_GAP / 2 + 140), sv * (hw - CHAMFER / 2)))
        turned(PURPLE, su * hl, (a + b) / 2, 0, b - a, RAMP, RAMP, f"floor ramp end {su:+d}{sv:+d}", yaw=90, pitch=45)
    if su > 0:
        turned(NAVY, su * hl, 0, HR, W - CHAMFER, TOPRAMP, TOPRAMP, f"roof ramp end {su:+d}", yaw=90, pitch=45)
    else:                                       # the score monitor hangs in the middle of this one
        for sv in (-1, 1):
            a, b = sorted((sv * (MON_W / 2 + 20), sv * (hw - CHAMFER / 2)))
            turned(NAVY, su * hl, (a + b) / 2, HR, b - a, TOPRAMP, TOPRAMP, f"roof ramp end {su:+d}", yaw=90, pitch=45)

# ================================================================================================ wall detailing
print("wall detail...", flush=True)
RIB = 70.0
RIB_H0, RIB_H1 = RAMP_REACH, HR - TOPRAMP / math.sqrt(2)
for sv in (-1, 1):                              # pilaster ribs on the side walls, from the floor ramp to the roof ramp
    for i in range(-3, 4):
        u = i * 800.0
        if sv < 0 and abs(u) < DOOR_GAP / 2 + 200:
            continue
        box(PURPLE, u - RIB / 2, u + RIB / 2, sv * (hw - 25), sv * hw, RIB_H0, RIB_H1, f"rib {sv:+d}")
for su in (-1, 1):
    for vv in (-1100.0, 1100.0):
        box(PURPLE, su * (hl - 25), su * hl, vv - RIB / 2, vv + RIB / 2, RIB_H0, RIB_H1, f"end rib {su:+d}")
TB0, TB1 = 260.0, 290.0                         # a trim band at 2.6 m, the station arena's rail
for sv in (-1, 1):
    spans = [(-hl + CHAMFER / 2, hl - CHAMFER / 2)] if sv > 0 else \
        [(-hl + CHAMFER / 2, -DOOR_GAP / 2 - 20), (DOOR_GAP / 2 + 20, hl - CHAMFER / 2)]
    for a, b in spans:
        box(PURPLE, a, b, sv * (hw - 30), sv * hw, TB0, TB1, f"trim {sv:+d}")
for su in (-1, 1):
    for sv in (-1, 1):
        a, b = sorted((sv * (GOAL_GAP / 2 + 90), sv * (hw - CHAMFER / 2)))
        box(PURPLE, su * (hl - 30), su * hl, a, b, TB0, TB1, f"end trim {su:+d}{sv:+d}")
# side-wall ledges at the wings: they stand on the wall, a 45-degree brace under each turns the wall into a kicker
LEDGE_H, LEDGE_D, LEDGE_L = 470.0, 260.0, 1000.0
ledges = []
for sv in (-1, 1):
    for su in (-1, 1):
        uc = su * 1500.0
        box(NAVY, uc - LEDGE_L / 2, uc + LEDGE_L / 2, sv * (hw - LEDGE_D), sv * hw, LEDGE_H - 40, LEDGE_H, "ledge")
        box(PURPLE, uc - LEDGE_L / 2, uc + LEDGE_L / 2, sv * (hw - LEDGE_D), sv * (hw - LEDGE_D + 20), LEDGE_H, LEDGE_H + 30, "ledge lip")
        turned(NAVY, uc, sv * hw, LEDGE_H - 40, LEDGE_L, 340, 340, "ledge brace", pitch=45)
        ledges.append((uc, sv * (hw - LEDGE_D / 2)))

# ========================================================================================================== ceiling
# two forcefield light strips along the flat of the roof, over the wings (where the play runs), 4 cm under it
print("ceiling...", flush=True)
ROOF_FLAT_U = hl - TOPRAMP / math.sqrt(2) - 120
for sv in (-1, 1):
    box(SHIELD, -ROOF_FLAT_U, ROOF_FLAT_U, sv * 700 - 25, sv * 700 + 25, HR - 4, HR, "roof light strip")
# ============================================================================================================ goals
print("goals...", flush=True)
goals = {}
for team, s in ((1, 1), (2, -1)):             # team 1 (Blue) scores in the +u goal, team 2 (Orange) in the -u goal
    face = s * hl
    f0, f1 = sorted((face - s * 50, face + s * T))
    for sv in (-1, 1):
        a, b = sorted((sv * GOAL_GAP / 2, sv * (GOAL_GAP / 2 + 90)))
        box(PURPLE, f0, f1, a, b, 0, GOAL_H + 90, "goal post")
    box(PURPLE, f0, f1, -GOAL_GAP / 2 - 90, GOAL_GAP / 2 + 90, GOAL_H, GOAL_H + 90, "goal bar")
    b0, b1 = sorted((s * hl, s * (hl + T + DEPTH + T)))
    for sv in (-1, 1):
        a, b = sorted((sv * GOAL_GAP / 2, sv * (GOAL_GAP / 2 + T)))
        box(NAVY, b0, b1, a, b, 0, GOAL_H + T, "goal side")
    k0, k1 = sorted((s * (hl + T + DEPTH), s * (hl + T + DEPTH + T)))
    box(NAVY, k0, k1, -GOAL_GAP / 2 - T, GOAL_GAP / 2 + T, 0, GOAL_H + T, "goal back")
    box(NAVY, b0, b1, -GOAL_GAP / 2 - T, GOAL_GAP / 2 + T, GOAL_H, GOAL_H + T, "goal roof")
    n0, n1 = sorted((s * (hl + T + DEPTH), s * (hl + T + DEPTH - 20)))   # the net, on the back wall's inner face
    for vv in (-300.0, -150.0, 0.0, 150.0, 300.0):
        box(PURPLE, n0, n1, vv - 10, vv + 10, 0, GOAL_H, "goal net")
    for hh in (95.0, 190.0, 285.0):
        box(PURPLE, n0, n1, -GOAL_GAP / 2, GOAL_GAP / 2, hh - 10, hh + 10, "goal net")
    # the station's forcefield just in front of the net: the goal glows, the net shows through it
    f0, f1 = sorted((s * (hl + T + DEPTH - 20), s * (hl + T + DEPTH - 26)))
    box(SHIELD, f0, f1, -GOAL_GAP / 2, GOAL_GAP / 2, 0, GOAL_H, "goal forcefield")
    # the station's goal trigger: a thin plane along its local X -- yaw 0/180 lays it across the mouth -- stretched to
    # the whole mouth (1.5x is 6.36 m wide)
    goals[team] = put("BP_GoalJakeBall_C", s * (hl + T + DEPTH / 2), 0, GOAL_H / 2 - 20, yaw=0 if s > 0 else 180,
                      scale=(1.5 * GOAL_GAP / 636.0, 1.5, 1.5), label="goal trigger")

# ============================================================================================================ pitch
print("markings, bumpers...", flush=True)
LINE, LH = 30.0, 3.0
box(PURPLE, -LINE / 2, LINE / 2, -hw + RAMP_REACH, hw - RAMP_REACH, 0, LH, "centre line")
R = 450.0
for i in range(16):
    a = (i + 0.5) * 2 * math.pi / 16
    turned(PURPLE, R * math.cos(a), R * math.sin(a), LH / 2, 2 * R * math.sin(math.pi / 16) + 12, LINE, LH, "centre circle",
           yaw=-math.degrees(a) + 90)
PB_D, PB_W = 750.0, 1900.0
for s in (1, -1):
    uf = s * (hl - PB_D)
    box(PURPLE, uf - LINE / 2, uf + LINE / 2, -PB_W / 2, PB_W / 2, 0, LH, "penalty box front")
    for sv in (-1, 1):
        a, b = sorted((uf, s * (hl - RAMP_REACH)))
        box(PURPLE, a, b, sv * PB_W / 2 - LINE / 2, sv * PB_W / 2 + LINE / 2, 0, LH, "penalty box side")
for su in (-1, 1):
    for sv in (-1, 1):
        turned(NAVY, su * 840.0, sv * 935.0, 110, 170, 170, 220, "bumper", yaw=45)
        turned(PURPLE, su * 840.0, sv * 935.0, 230, 190, 190, 20, "bumper cap", yaw=45)   # a lip turned with it

# ================================================================================== ring, ball, boosts, doors
print("ring, ball, boosts, doors...", flush=True)
# As in the station arena: the ring hangs above the floor, the spawner 95 cm over its centre. Between games the ball
# drops out of it to the floor; carrying it back up into the ring starts a game; at kickoff it glides back up to the
# spawner, dips 90 cm into the ring and is flung out of it when the countdown ends.
RING_H = 260.0
ring = put("Prefab_BP_CylinderPrimitive_Trigger_C", 0, 0, RING_H, label="start ring")
ball = put("BP_JakeBallSpawner_C", 0, 0, RING_H + 95, label="ball spawner")
# the goal celebrations (off until a team scores), and the two midfield triggers: carrying the ball over one ends the
# next-point countdown early
celebs = {team: put("LE_BP_VFX_TackleBallGoal01a_C", 0, 0, 0, yaw=90 if team == 1 else -90, label=f"celebration {team}") for team in (1, 2)}
mids = [put("BP_DiscTriggerC_C", su * 780.0, 0, 0, yaw=180, label="midfield trigger") for su in (-1, 1)]
items = []
for su in (-1, 1):
    for sv in (-1, 1):                           # corners by the goals
        items.append((put("BP_LargeBoostPickup_C", su * (hl - 650), sv * (hw - 480), 80, label="large boost"), 0.0, "large boost (corner)"))
for s in (1, -1):                                # the penalty spots: the defender's refill
    items.append((put("BP_SmallBoostPickup_C", s * (hl - 480), 0, 40, label="small boost"), 0.0, "small boost (penalty spot)"))
for sv in (-1, 1):                               # the wings at midfield: the run after kickoff
    items.append((put("BP_SmallBoostPickup_C", 0, sv * (hw - 420), 40, label="small boost"), 0.0, "small boost (wing)"))
for (uc, vc) in ledges:                          # the reward for wall play
    items.append((put("BP_SmallBoostPickup_C", uc, vc, LEDGE_H + 40, label="small boost"), LEDGE_H, "small boost (ledge)"))
time.sleep(3)
for o, surf, lbl in items:
    settle(o, surf, lbl)

# the scores: one monitor, on the left end wall as you come in through the doors, above that goal, facing the pitch
# (its screen faces its local Y); the teams: the team sideboard right above the team doors, as the station arena has it
put("BP_ScoreboardA_C", -(hl - 80 * MON_S / 2), 0, GOAL_H + 90 + 20 + MON_HGT / 2, yaw=0, scale=(MON_S, MON_S, MON_S), label="score monitor")
put("LE_BP_ScoreboardA_Sideboard_C", 0, -hw + 20, DOOR_H + 40 + 110, yaw=-90, scale=(0.5, 0.5, 0.5), label="team board")
door_signs = []
for team in (1, 2):
    call("gamemode_team_changer", mode=MODE, team=team, location=at(door_u[team], -hw - T / 2, 50), yaw=180)
    print(f"  + team door {team}", flush=True)
    door_signs.append((team, put("LE_BP_Text_C", door_u[team], -hw - T - 22, DOOR_H + 90, yaw=180, scale=(3, 3, 3),
                                 label="door label")))
time.sleep(3)
for team, sgn in door_signs:
    if sgn:
        call("set_text", handle=sgn["handle"], text="{MiniJakeball.team%d.name}" % team)

if goals.get(1): call("gamemode_set_role", mode=MODE, handle=goals[1]["handle"], role="goal:1")
if goals.get(2): call("gamemode_set_role", mode=MODE, handle=goals[2]["handle"], role="goal:2")
if ring: call("gamemode_set_role", mode=MODE, handle=ring["handle"], role="start_ring")
if ball: call("gamemode_set_role", mode=MODE, handle=ball["handle"], role="ball")
for team, c in celebs.items():
    if c: call("gamemode_set_role", mode=MODE, handle=c["handle"], role=f"celebrate:{team}")
for mo in mids:
    if mo: call("gamemode_set_role", mode=MODE, handle=mo["handle"], role="midfield")

# ---- the mode's code: the station's Jakeball flow (TKB gamemode.luau), ported onto this arena's slots
C0 = at(0, 0, 0)
AX = {k: [a - b for a, b in zip(at(*d), C0)] for k, d in (("u", (1, 0, 0)), ("v", (0, 1, 0)), ("h", (0, 0, 1)))}
CODE = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "minijakeball_flow.luau"), encoding="utf-8").read()
for key, val in (("@CX@", C0[0]), ("@CY@", C0[1]), ("@CZ@", C0[2]), ("@HL@", hl + T + DEPTH + T + 100), ("@HW@", hw + T + 100),
                 ("@TOP@", HR + 200), ("@U@", ", ".join(str(x) for x in AX["u"])), ("@V@", ", ".join(str(x) for x in AX["v"])),
                 ("@UP@", ", ".join(str(x) for x in AX["h"]))):
    CODE = CODE.replace(key, str(val))
ok, chk = call("check_luau", source=CODE, name="GM_" + MODE, gamemode_code=True)
print("code check:", chk.get("ok") if isinstance(chk, dict) else chk, chk.get("problems") if isinstance(chk, dict) else "")
ok, r = call("gamemode_set_code", mode=MODE, source=CODE)
print("code:", r.get("written") if isinstance(r, dict) else r)
with open(os.path.join(os.environ.get("TEMP", "."), "wall_jakeball_placed.json"), "w") as fh:
    json.dump(placed, fh)
print("pieces placed:", len(placed), flush=True)
ok, lvl = call("level_save")
print("saved:", ok)
name = st.get("openLevel") if isinstance(st, dict) else ""     # monitors bind to the mode only when the level loads
if ok and name:
    call("level_close")
    time.sleep(3)
    ok, r = call("level_open", name=name)
    print("reopened:", name, ok)
p.terminate()
