"""build_wall_jakeball.py -- builds a small Jakeball game mode on the station's wall pitch through the MCP (as an agent
would): an arena of primitive cubes, two team doors that face in (walk in = join, walk out = leave), goals, the
"place the ball here" start ring, a ball, a scoreboard and a live score sign. Run with the editor in the server.
  python build_wall_jakeball.py            build
  python build_wall_jakeball.py --remove   delete the mode (and everything in it)"""
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

# ---- the pitch: on the wall of the station's cylinder (axis = world Y). Floor radius R, centre at (y0, z0).
R = 28576.0
Y0, Z0 = 11300.0, -300.0
PHI0 = math.atan2(Z0, -math.sqrt(R * R - Z0 * Z0))     # angle of the centre on the x/z circle
L, W, H = 3600.0, 2200.0, 250.0                          # arena length (along Y), width (across), wall height (cm)
GOAL_GAP, DOOR_GAP = 700.0, 900.0


def at(u, v, h):
    """u along the pitch (world Y), v across it (along the floor), h above the floor -> world point."""
    phi = PHI0 + v / R
    rr = R - h
    return [round(rr * math.cos(phi), 1), round(Y0 + u, 1), round(rr * math.sin(phi), 1)]


placed = []


def put(item, u, v, h, yaw=0.0, scale=(1, 1, 1), label=""):
    ok, o = call("place_object", item=item, location=at(u, v, h), rotation=[0, yaw, 0], scale=list(scale))
    if ok and isinstance(o, dict):
        placed.append((label or item, o["handle"]))
        print(f"  + {label or item:28s} {o['handle']}", flush=True)
        return o
    return None


ok, st = call("editor_status")
print("editor:", st.get("openLevel"), st.get("worldReady"))
centre = at(0, 0, 0)
ok, gm = call("gamemode_create", name=MODE, center=centre, teams=2, team_names=["Blue", "Orange"], max_players=[3, 3])
print("mode:", gm if not ok else gm.get("id"))
if not ok:
    sys.exit(1)
for k, v in (("start_mode", "manual"), ("countdown", "5"), ("round_time", "300"), ("score_to_win", "5"),
             ("end_delay", "8"), ("auto_restart", "0"), ("stop_when_empty", "1"), ("reset_after_goal", "1")):
    call("gamemode_set", mode=MODE, key=k, value=v)
time.sleep(4)

# ---- walls (primitive cubes; a cube is 100 cm, scaled along its own axes: X across the pitch, Y along it, Z up)
T = 40.0                                                  # wall thickness
side_len = L
# the long side walls: the far side whole, the near side with the entrance gap in the middle
put("Prefab_BP_StandardCubePrimitive_C", 0, W / 2 + T / 2, H / 2, scale=(T / 100, side_len / 100, H / 100), label="wall far side")
seg = (L - DOOR_GAP) / 2
for s in (-1, 1):
    put("Prefab_BP_StandardCubePrimitive_C", s * (DOOR_GAP / 2 + seg / 2), -(W / 2 + T / 2), H / 2, scale=(T / 100, seg / 100, H / 100),
        label=f"wall near side {'+' if s > 0 else '-'}")
# the ends: team coloured, a gap for the goal in the middle
for team, u_end, cube in ((1, -L / 2, "PrimitiveCubeBlue"), (2, L / 2, "PrimitiveCubeYellow")):
    seg = (W - GOAL_GAP) / 2
    for s in (-1, 1):
        put(cube, u_end + (-T / 2 if u_end < 0 else T / 2), s * (GOAL_GAP / 2 + seg / 2), H / 2,
            scale=(seg / 100, T / 100, H / 100), label=f"end wall team {team} {'+' if s > 0 else '-'}")
    # a beam over the goal gap
    put(cube, u_end + (-T / 2 if u_end < 0 else T / 2), 0, H + 60, scale=((GOAL_GAP + 2 * T) / 100, T / 100, 0.4), label=f"goal beam team {team}")

# ---- goals, built like the station's Jakeball arenas: the goal itself is only a trigger (BP_GoalJakeBall_C, 1.5x,
# 1.83 m up); the arena makes the goal you see. Each goal mouth (the end wall's gap, under its beam) opens into a box
# of primitive cubes -- back and two sides -- so a scored ball stays in, and the trigger sits just behind the line.
DEPTH = 300.0
for team, s, cube in ((1, -1, "PrimitiveCubeBlue"), (2, 1, "PrimitiveCubeYellow")):
    line = s * L / 2
    put(cube, line + s * (T + DEPTH + T / 2), 0, H / 2, scale=((GOAL_GAP + 2 * T) / 100, T / 100, H / 100), label=f"goal back team {team}")
    for side in (-1, 1):
        put(cube, line + s * (T + DEPTH / 2), side * (GOAL_GAP / 2 + T / 2), H / 2, scale=(T / 100, DEPTH / 100, H / 100),
            label=f"goal side team {team} {'+' if side > 0 else '-'}")
    put(cube, line + s * (T + DEPTH / 2), 0, H + 60, scale=((GOAL_GAP + 2 * T) / 100, (DEPTH + T) / 100, 0.4), label=f"goal roof team {team}")
# team 1 (Blue) defends the -Y end, so Blue scores in the +Y goal (role goal:1), Orange in the -Y one (goal:2)
g1 = put("BP_GoalJakeBall_C", L / 2 + T + 120, 0, 183, yaw=90, scale=(1.5, 1.5, 1.5), label="goal Blue scores (+Y end)")
g2 = put("BP_GoalJakeBall_C", -L / 2 - T - 120, 0, 183, yaw=-90, scale=(1.5, 1.5, 1.5), label="goal Orange scores (-Y end)")
# ---- the start ring ("place the ball here") in the centre and the ball beside it
ring = put("Prefab_BP_CylinderPrimitive_Trigger_C", 0, 0, 5, label="start ring (place the ball here)")
ball = put("BP_JakeBallSpawner_C", 0, 500, 100, label="ball spawner")
# ---- scoreboard monitor over the far side wall, facing the pitch; a live score sign over the entrance
board = put("BP_ScoreboardA_C", 0, W / 2 + 60, H + 250, yaw=180, label="scoreboard")
sign = put("LE_BP_Text_C", 0, -(W / 2 + 60), H + 220, yaw=0, label="score sign")
time.sleep(4)

# ---- team doors in the entrance gap: they face the centre by themselves (walk in = join, walk out = leave)
door_v = -(W / 2)
for team, du in ((1, -220), (2, 220)):
    call("gamemode_team_changer", mode=MODE, team=team, location=at(du, door_v, 50), yaw=0)   # yaw 0 = straight across, into the pitch
    print(f"  + team door {team}", flush=True)
time.sleep(3)

# ---- roles + mode code
if g1: call("gamemode_set_role", mode=MODE, handle=g1["handle"], role="goal:1")
if g2: call("gamemode_set_role", mode=MODE, handle=g2["handle"], role="goal:2")
if ring: call("gamemode_set_role", mode=MODE, handle=ring["handle"], role="start_ring")
if ball: call("gamemode_set_role", mode=MODE, handle=ball["handle"], role="ball")
if sign: call("set_text", handle=sign["handle"], text="{MiniJakeball.team1.name} {MiniJakeball.team1.score} - {MiniJakeball.team2.score} {MiniJakeball.team2.name}")

# bounds of the pitch in the wall's own frame, for the out-of-bounds rule
CODE = f'''-- MiniJakeball: carry the ball into the ring to start; first to 5; the ball comes back if it leaves the arena.
local R, Y0, PHI0 = {R}, {Y0}, {PHI0}
local HALF_L, HALF_W, TOP = {L / 2 + 150}, {W / 2 + 150}, {H + 1200}
local watching = false

local function outOfArena(p: Vector): boolean
	local r = math.sqrt(p.x * p.x + p.z * p.z)
	local phi = math.atan2(p.z, p.x)
	local across = (phi - PHI0) * R
	return math.abs(p.y - Y0) > HALF_L or math.abs(across) > HALF_W or (R - r) > TOP
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
			local ball = Ball1:getSpawnedBall()
			if ball and outOfArena(ball:getDiscPosition()) then
				log("MiniJakeball: ball out of the arena -- back to the spawner")
				Rigel.resetBalls()
			end
		end)
	end)
end

function OnLobby()
	watchBall()
end

function OnRoundStart(round: number)
	watchBall()
	log(`MiniJakeball round {{round}}: {{Rigel.teamName(1)}} vs {{Rigel.teamName(2)}}`)
end

function OnScore(team: number, score: number, old: number)
	log(`MiniJakeball: {{Rigel.teamName(team)}} scores ({{score}})`)
end

function OnRoundEnd(winner: number)
	log(`MiniJakeball: round over, winner {{winner}}`)
end
'''
ok, chk = call("check_luau", source=CODE, name="GM_" + MODE, gamemode_code=True)
print("code check:", chk.get("ok") if isinstance(chk, dict) else chk, (chk.get("problems") if isinstance(chk, dict) else "")[:3] if isinstance(chk, dict) else "")
ok, r = call("gamemode_set_code", mode=MODE, source=CODE)
print("code:", r.get("written") if isinstance(r, dict) else r)
ok, lvl = call("level_save")
print("saved:", ok)
print(json.dumps({"placed": placed}, indent=1))
p.terminate()
