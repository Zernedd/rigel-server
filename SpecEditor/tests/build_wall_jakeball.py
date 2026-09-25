"""build_wall_jakeball.py -- builds MiniJakeball, a Jakeball game mode on the station's flat wall pitch, through the MCP
(as an agent would). Modelled on the station's own Jakeball arena: a sealed pitch of gridded primitive cubes with cut
corners, quarter-pipe ramps along the walls, framed goals in sealed goal boxes with a scoreboard over each, tiered stands
on both sides, obstacles and floating platforms, boost pickups, team doors in a framed doorway (walk in = join, walk out
= leave), the "place the ball here" ring, one ball, live score signs and an out-of-bounds reset.
  python build_wall_jakeball.py            build (on the wall pitch)
  python build_wall_jakeball.py --bottom   build on the bottom floor instead (up = +Z)
  python build_wall_jakeball.py --remove   delete the mode (and everything in it)
Needs the editor running with a level open."""
import json
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

# ---- the frame. u runs along the pitch, v across it, h up from its floor.
# The wall pitch is FLAT: the plane x = FLOOR_X, facing +X (traced: flat and clear for 15 m from y 6300 to 18300 and
# z -3300 to 3700). There u = world +Y, v = world -Z, h = world +X, and every piece stands square to it (floor_up = +X).
FLOOR_X, Y0, Z0 = -28575.0, 11300.0, -300.0      # centred on the wall's painted pitch
UP = [1.0, 0.0, 0.0]
BOTTOM = "--bottom" in sys.argv          # the bottom floor (up = +Z): u = +Y, v = +X, h = +Z
if BOTTOM:
    BX, BY, BZ = 0.0, 2500.0, -28625.0
    UP = [0.0, 0.0, 1.0]


def at(u, v, h):
    if BOTTOM:
        return [round(BX + v, 1), round(BY + u, 1), round(BZ + h, 1)]
    return [round(FLOOR_X + h, 1), round(Y0 + u, 1), round(Z0 - v, 1)]


# ---- sizes (cm). The pitch is L x W inside its walls; the station's own Jakeball pitch is ~65 x 45 m.
L, W, H = 5600.0, 3400.0, 600.0         # pitch length (u) and width (v); wall height
T = 60.0                                 # wall thickness
hl, hw = L / 2, W / 2
GOAL_GAP, GOAL_H, DEPTH = 900.0, 320.0, 360.0      # goal mouth width / height; goal box depth
DOOR_W, POST = 380.0, 60.0                          # a team door's opening; a frame post
DOOR_GAP = 3 * POST + 2 * DOOR_W                    # post | Blue door | post | Orange door | post
CHAMFER = 500.0                                     # the cut corners: a 500 x 500 cube turned 45 degrees
RAMP = 220.0                                        # quarter-pipe ramps along the wall feet: a 220 cube turned 45 degrees
STAND_D, STAND_STEP = 320.0, 170.0                  # the stands: depth of a tier, rise of a tier over the wall top

# Only the gridded primitive cubes: the others (Small / Blue / Yellow) cull -- they vanish at some distances.
# Their scale is NOT centimetres: the navy cube is 100 cm per unit, the purple one 64 (measured with list_objects
# bounds). Sizing both as 100 is what pulled the old goal boxes apart.
NAVY, PURPLE = "Prefab_BP_StandardCubePrimitive_C", "Prefab_BP_Cube2Primitive_C"
UNIT = {NAVY: 100.0, PURPLE: 64.0}

placed = []


SIGN = (3, 3, 3)                         # text signs at 1x are unreadable across the pitch


def put(item, u, v, h, yaw=0.0, pitch=0.0, scale=(1, 1, 1), label=""):
    ok, o = call("place_object", item=item, location=at(u, v, h), rotation=[pitch, yaw, 0], scale=list(scale), floor_up=UP)
    if ok and isinstance(o, dict):
        placed.append((label or item, o["handle"]))
        print(f"  + {label or item:40s} {o['handle']}", flush=True)
        return o
    print(f"  !! failed: {label or item}", flush=True)
    return None


def box(cube, u0, u1, v0, v1, h0, h1, label, yaw=0.0, pitch=0.0):
    """A cube filling u0..u1 x v0..v1 x h0..h1 (cube axes: X across = v, Y along = u, Z up), sized for its class."""
    k = UNIT[cube]
    return put(cube, (u0 + u1) / 2, (v0 + v1) / 2, (h0 + h1) / 2, yaw=yaw, pitch=pitch,
               scale=(abs(v1 - v0) / k, abs(u1 - u0) / k, abs(h1 - h0) / k), label=label)


def turned(cube, u, v, h, size_u, size_v, size_h, label, yaw=0.0, pitch=0.0):
    k = UNIT[cube]
    return put(cube, u, v, h, yaw=yaw, pitch=pitch, scale=(size_v / k, size_u / k, size_h / k), label=label)


ok, st = call("editor_status")
print("editor:", st.get("openLevel"), st.get("worldReady"))
ok, gm = call("gamemode_create", name=MODE, center=at(0, 0, 0), teams=2, team_names=["Blue", "Orange"], max_players=[4, 4])
print("mode:", gm if not ok else gm.get("id"))
if not ok:
    sys.exit(1)
for k, v in (("start_mode", "manual"), ("countdown", "5"), ("round_time", "300"), ("score_to_win", "5"),
             ("end_delay", "8"), ("auto_restart", "0"), ("stop_when_empty", "1"), ("reset_after_goal", "1")):
    call("gamemode_set", mode=MODE, key=k, value=v)
time.sleep(4)

# ================================================================== the shell
# Side walls run the full length plus both end walls' thickness, so every corner is closed. The near wall (-v) has
# the doorway in its middle.
box(NAVY, -hl - T, hl + T, hw, hw + T, 0, H, "wall far side")
box(NAVY, -hl - T, -DOOR_GAP / 2, -hw - T, -hw, 0, H, "wall near side (-u)")
box(NAVY, DOOR_GAP / 2, hl + T, -hw - T, -hw, 0, H, "wall near side (+u)")
for pu in (-DOOR_GAP / 2, -POST / 2, DOOR_GAP / 2 - POST):
    box(PURPLE, pu, pu + POST, -hw - T - 20, -hw + 20, 0, H, "doorway post")
box(PURPLE, -DOOR_GAP / 2, DOOR_GAP / 2, -hw - T - 20, -hw + 20, 330, H, "doorway lintel")
door_u = {1: -(POST / 2 + DOOR_W / 2), 2: POST / 2 + DOOR_W / 2}

# The ends: the wall beside the goal mouth on both sides, and over it. The end walls sit BETWEEN the side walls'
# inner faces (v -hw..hw), and the side walls run past them -- no seam to see through.
for team, s in ((1, -1), (2, 1)):
    e0, e1 = sorted((s * hl, s * (hl + T)))
    box(NAVY, e0, e1, GOAL_GAP / 2, hw, 0, H, f"end wall {team} (+v)")
    box(NAVY, e0, e1, -hw, -GOAL_GAP / 2, 0, H, f"end wall {team} (-v)")
    box(NAVY, e0, e1, -GOAL_GAP / 2, GOAL_GAP / 2, GOAL_H, H, f"end wall {team} over the goal")

# The rail round the top of every wall (purple, a little wider than the wall): the station arena's rim.
RIM = 30.0
box(PURPLE, -hl - T - RIM, hl + T + RIM, hw - RIM, hw + T + RIM, H, H + 40, "rim far side")
box(PURPLE, -hl - T - RIM, -DOOR_GAP / 2, -hw - T - RIM, -hw + RIM, H, H + 40, "rim near side (-u)")
box(PURPLE, DOOR_GAP / 2, hl + T + RIM, -hw - T - RIM, -hw + RIM, H, H + 40, "rim near side (+u)")
box(PURPLE, -DOOR_GAP / 2, DOOR_GAP / 2, -hw - T - RIM, -hw + RIM, H, H + 40, "rim over the doorway")
for team, s in ((1, -1), (2, 1)):
    e0, e1 = sorted((s * (hl - RIM), s * (hl + T + RIM)))
    box(PURPLE, e0, e1, -hw, hw, H, H + 40, f"rim end {team}")

# Cut corners: a cube turned 45 degrees standing on each corner -- the inside half is the diagonal face, the outside
# half is hidden behind the walls.
for su in (-1, 1):
    for sv in (-1, 1):
        turned(NAVY, su * hl, sv * hw, H / 2, CHAMFER, CHAMFER, H, f"corner {su:+d}{sv:+d}", yaw=45)

# Quarter-pipe ramps along the feet of the side walls (and of the end walls beside the goals): a square bar turned 45
# degrees about its length, its axis on the wall/floor edge -- the ball rolls up the walls instead of stopping dead.
for sv in (-1, 1):
    ramp_segments = [(-hl + CHAMFER / 2, hl - CHAMFER / 2)] if sv > 0 else \
        [(-hl + CHAMFER / 2, -DOOR_GAP / 2 - 150), (DOOR_GAP / 2 + 150, hl - CHAMFER / 2)]
    for a, b in ramp_segments:
        turned(PURPLE, (a + b) / 2, sv * hw, 0, b - a, RAMP, RAMP, f"ramp side {sv:+d}", pitch=45)
for su in (-1, 1):
    for sv in (-1, 1):
        a, b = sorted((sv * (GOAL_GAP / 2 + 120), sv * (hw - CHAMFER / 2)))
        turned(PURPLE, su * hl, (a + b) / 2, 0, b - a, RAMP, RAMP, f"ramp end {su:+d}{sv:+d}", yaw=90, pitch=45)

# ================================================================== the goals
# Each goal: a purple frame (two posts and a bar) proud of the end wall, then a sealed box behind the mouth -- sides
# and back reach INTO the end wall and each other by a wall's thickness, and the roof covers all of them.
goals = {}
for team, s in ((1, 1), (2, -1)):          # team 1 (Blue) scores in the +u goal, team 2 (Orange) in the -u goal
    face = s * hl                           # the end wall's inner face
    f0, f1 = sorted((face - s * 40, face + s * T))
    for sv in (-1, 1):
        a, b = sorted((sv * GOAL_GAP / 2, sv * (GOAL_GAP / 2 + 80)))
        box(PURPLE, f0, f1, a, b, 0, GOAL_H + 80, f"goal {team} post {sv:+d}")
    box(PURPLE, f0, f1, -GOAL_GAP / 2 - 80, GOAL_GAP / 2 + 80, GOAL_H, GOAL_H + 80, f"goal {team} bar")
    b0, b1 = sorted((s * hl, s * (hl + T + DEPTH + T)))            # sides: from inside the end wall to the back's far face
    for sv in (-1, 1):
        a, b = sorted((sv * GOAL_GAP / 2, sv * (GOAL_GAP / 2 + T)))
        box(NAVY, b0, b1, a, b, 0, GOAL_H + T, f"goal {team} side {sv:+d}")
    k0, k1 = sorted((s * (hl + T + DEPTH), s * (hl + T + DEPTH + T)))
    box(NAVY, k0, k1, -GOAL_GAP / 2 - T, GOAL_GAP / 2 + T, 0, GOAL_H + T, f"goal {team} back")
    box(NAVY, b0, b1, -GOAL_GAP / 2 - T, GOAL_GAP / 2 + T, GOAL_H, GOAL_H + T, f"goal {team} roof")
    # the goal itself: the station's Jakeball goal trigger (1.5x), in the box
    # (its trigger is a thin plane along the goal's local X: yaw 0 / 180 lays it ACROSS the mouth, as in the station's
    # arenas -- at yaw 90 it ran along the pitch through the middle of the box and real shots missed it)
    goals[team] = put("BP_GoalJakeBall_C", s * (hl + T + DEPTH / 2), 0, GOAL_H / 2 - 20, yaw=0 if s > 0 else 180,
                      scale=(1.5 * GOAL_GAP / 636.0, 1.5, 1.5),   # a 1.5x goal is 6.36 m wide: stretch it across the whole mouth
                      label=f"goal: {'Blue' if team == 1 else 'Orange'} scores here")
    # a scoreboard over each goal, facing the pitch (its screen faces its local Y), like the station's arena
    put("BP_ScoreboardA_C", s * (hl + T / 2), 0, H + 330, yaw=180 if s > 0 else 0, label=f"scoreboard over goal {team}")

# ================================================================== the stands
# Three tiers along both sides, outside the walls, the first level with the rim: spectators look in over the rail.
# On the near side they stop short of the doorway.
for sv in (-1, 1):
    spans = [(-hl + 200, hl - 200)] if sv > 0 else [(-hl + 200, -DOOR_GAP / 2 - 250), (DOOR_GAP / 2 + 250, hl - 200)]
    for a, b in spans:
        for tier in range(3):
            v0, v1 = sorted((sv * (hw + T + tier * STAND_D), sv * (hw + T + (tier + 1) * STAND_D)))
            box(NAVY if tier % 2 == 0 else PURPLE, a, b, v0, v1, 0, H + tier * STAND_STEP, f"stand {sv:+d} tier {tier}")

# ================================================================== the pitch
# Centre line (a flat purple strip), four diamond bumpers, two floating platforms over the halves.
box(PURPLE, -20, 20, -hw + RAMP / 2, hw - RAMP / 2, 0, 6, "centre line")
for su in (-1, 1):
    for sv in (-1, 1):
        turned(NAVY, su * hl * 0.45, sv * hw * 0.5, 110, 160, 160, 220, f"bumper {su:+d}{sv:+d}", yaw=45)
for su in (-1, 1):
    box(PURPLE, su * hl * 0.55 - 250, su * hl * 0.55 + 250, -350, 350, 380, 420, f"platform {su:+d}")

# boost pickups, as the station arena scatters them: large in the corners, small in rings round the centre and the goals
for su in (-1, 1):
    for sv in (-1, 1):
        put("BP_LargeBoostPickup_C", su * (hl - 600), sv * (hw - 500), 120, label="large boost")
for (u, v, h) in ((0, 700, 100), (0, -700, 100), (900, 0, 100), (-900, 0, 100),
                  (hl * 0.55, 0, 520), (-hl * 0.55, 0, 520), (hl - 900, 0, 150), (-hl + 900, 0, 150)):
    put("BP_SmallBoostPickup_C", u, v, h, label="small boost")

# ================================================================== start ring, ball, signs, doors
ring = put("Prefab_BP_CylinderPrimitive_Trigger_C", 0, 0, 5, label="start ring (place the ball here)")
ball = put("BP_JakeBallSpawner_C", 0, 600, 100, label="ball spawner")
side = put("LE_BP_ScoreboardA_Sideboard_C", 0, -hw - T / 2, H + 260, yaw=-90, scale=(0.5, 0.5, 0.5), label="sideboard over the doors")
table = put("LE_BP_TableScoreboard_C", -hl * 0.5, -hw - T - STAND_D / 2, H + 10, yaw=-90, scale=(0.1, 0.1, 0.1),
            label="score table (on the near stand)")
score_in = put("LE_BP_Text_C", 0, hw - 20, H + 90, yaw=180, scale=SIGN, label="score sign (far wall)")
play = [put("LE_BP_Text_C", su * hl * 0.5, hw - 20, H + 90, yaw=180, scale=SIGN, label="PLAY BALL sign") for su in (-1, 1)]
time.sleep(4)

door_signs = []
for team in (1, 2):
    call("gamemode_team_changer", mode=MODE, team=team, location=at(door_u[team], -hw - T / 2, 50), yaw=180)
    print(f"  + team door {team}", flush=True)
    door_signs.append((team, put("LE_BP_Text_C", door_u[team], -hw - T - 30, H + 90, yaw=180, scale=SIGN, label=f"door {team} sign (outside)")))
time.sleep(3)
for team, s in door_signs:
    if s:
        call("set_text", handle=s["handle"], text="{MiniJakeball.team%d.name} team" % team)
for s in play:
    if s:
        call("set_text", handle=s["handle"], text="PLAY BALL")
if score_in:
    call("set_text", handle=score_in["handle"],
         text="{MiniJakeball.team1.name} {MiniJakeball.team1.score} - {MiniJakeball.team2.score} {MiniJakeball.team2.name}")

# ---- roles
if goals.get(1): call("gamemode_set_role", mode=MODE, handle=goals[1]["handle"], role="goal:1")
if goals.get(2): call("gamemode_set_role", mode=MODE, handle=goals[2]["handle"], role="goal:2")
if ring: call("gamemode_set_role", mode=MODE, handle=ring["handle"], role="start_ring")
if ball: call("gamemode_set_role", mode=MODE, handle=ball["handle"], role="ball")
if table: call("gamemode_set_role", mode=MODE, handle=table["handle"], role="score_table")

# ---- the mode's code
C0 = at(0, 0, 0)
AX = {k: [a - b for a, b in zip(at(*d), C0)] for k, d in (("u", (1, 0, 0)), ("v", (0, 1, 0)), ("h", (0, 0, 1)))}
CODE = f'''-- MiniJakeball: carry the ball into the ring to start; first to 5; the ball comes back if it leaves the arena.
-- The pitch's frame: centre C, u along it, v across it, h up from its floor.
local CX, CY, CZ = {C0[0]}, {C0[1]}, {C0[2]}
local U = {{ {AX["u"][0]}, {AX["u"][1]}, {AX["u"][2]} }}
local V = {{ {AX["v"][0]}, {AX["v"][1]}, {AX["v"][2]} }}
local UPV = {{ {AX["h"][0]}, {AX["h"][1]}, {AX["h"][2]} }}
local HALF_L, HALF_W, TOP = {hl + T + DEPTH + T + 150}, {hw + T + 150}, {H + 900}
local watching = false

local function along(p: Vector, a): number
	return (p.x - CX) * a[1] + (p.y - CY) * a[2] + (p.z - CZ) * a[3]
end

local function outOfArena(p: Vector): boolean
	local h = along(p, UPV)
	return math.abs(along(p, U)) > HALF_L or math.abs(along(p, V)) > HALF_W or h > TOP or h < -200
end

local function watchBall()
	if watching then
		return
	end
	watching = true
	LuauClock.createTimer(1, function()
		if not Rigel.isRunning() then
			return
		end
		pcall(function()
			local b = Ball1:getSpawnedBall()
			if b and outOfArena(b:getDiscPosition()) then
				log("MiniJakeball: ball out of the arena -- back to the spawner")
				Rigel.resetBalls()
			end
		end)
	end)
end

function OnLobby()
	watchBall()
end

function OnCountdown(seconds: number)
	log(`MiniJakeball: kick-off in {{seconds}}`)
end

function OnRoundStart(round: number)
	watchBall()
	log(`MiniJakeball round {{round}}: {{Rigel.teamName(1)}} vs {{Rigel.teamName(2)}}`)
end

function OnScore(team: number, score: number, old: number)
	if score > old then -- (a new round puts the scores back to 0 too)
		log(`MiniJakeball: {{Rigel.teamName(team)}} scores ({{score}})`)
	end
end

function OnRoundEnd(winner: number)
	log(`MiniJakeball: round over, winner {{winner}}`)
end
'''
ok, chk = call("check_luau", source=CODE, name="GM_" + MODE, gamemode_code=True)
print("code check:", chk.get("ok") if isinstance(chk, dict) else chk, chk.get("problems") if isinstance(chk, dict) else "")
ok, r = call("gamemode_set_code", mode=MODE, source=CODE)
print("code:", r.get("written") if isinstance(r, dict) else r)
print("pieces placed:", len(placed))
ok, lvl = call("level_save")
print("saved:", ok)
# Monitors bind to the mode's game state manager only when the level loads: reopen it so the new scoreboards work.
name = st.get("openLevel") if isinstance(st, dict) else ""
if ok and name:
    call("level_close")
    time.sleep(3)
    ok, r = call("level_open", name=name)
    print("reopened:", name, ok)
p.terminate()
