"""mcp_full_test.py -- EVERY tool of rigel_mcp.py (and its resources), driven only through the MCP the way an LLM agent
does, with a real mock player (PlayerNovBuild + A2PlayerControl) walking through what gets built:
  protocol + resources, looking around, building + properties + game data, the whole script workflow (checker traps,
  files, attach/update/remove, references, logs), the player Luau APIs on every machine, a game mode driven through all
  its hooks by a player joining a team, levels (save / upload / close / reopen / delete), and a self-reference.
At the end every tool in tools/list must have been called: an untested tool is a FAIL.

Needs a local server, the mock player in the world (it reads %TEMP%\\A2PlayerControl.cmd) and an editor idling in it
(tests/t_mcp_idle_long.txt). Each machine's Luau log(): %LOCALAPPDATA%\\A2\\Saved\\Logs\\A2*.log."""
import glob
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
MCP = os.path.join(HERE, "..", "luau", "mcp", "rigel_mcp.py")
TEMP = os.environ["TEMP"]
PC_CMD = os.path.join(TEMP, "A2PlayerControl.cmd")
PC_LOG = os.path.join(TEMP, "A2PlayerControl.log")
LOGS = os.path.join(os.environ["LOCALAPPDATA"], "A2", "Saved", "Logs")
FLOOR = -28625.0

p = subprocess.Popen([sys.executable, MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
nid = 0
fails = 0
results = []
used = set()


def rpc(method, params=None):
    global nid
    nid += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid, "method": method, "params": params or {}}) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


def call(tool_name, **args):
    used.add(tool_name)
    r = rpc("tools/call", {"name": tool_name, "arguments": args})
    if "result" not in r:
        return False, r.get("error")
    r = r["result"]
    content = r.get("content") or []
    txt = content[0].get("text", "") if content and content[0].get("type") == "text" else ""
    try:
        val = json.loads(txt) if txt else content
    except Exception:
        val = txt
    return (not r.get("isError")), val


def check(label, ok, detail=""):
    global fails
    line = ("PASS " if ok else "FAIL ") + label + ("" if ok else f"   -> {str(detail)[:700]}")
    print(line, flush=True)
    results.append(line)
    if not ok:
        fails += 1


# ---- every machine's game log, from a mark ----
_marks = {}


def mark_logs():
    for f in glob.glob(os.path.join(LOGS, "A2*.log")):
        _marks[f] = os.path.getsize(f)


def new_log_lines(pattern):
    out = []
    for f in glob.glob(os.path.join(LOGS, "A2*.log")):
        if "backup" in f:
            continue
        try:
            with open(f, "rb") as fh:
                fh.seek(_marks.get(f, 0))
                txt = fh.read().decode("utf-8", "replace")
        except OSError:
            continue
        for ln in txt.splitlines():
            if re.search(pattern, ln):
                out.append(f"{os.path.basename(f)}: {ln.strip()}")
    return out


def machines(lines):
    return sorted({l.split(":")[0] for l in lines})


def player(cmd, wait=0.0):
    with open(PC_CMD, "w") as fh:
        fh.write(cmd)
    if wait:
        time.sleep(wait)


def walk(a, b, each=9):
    player(f"goto {a[0]:.0f} {a[1]:.0f} {FLOOR + 25:.0f}", each)
    player(f"goto {b[0]:.0f} {b[1]:.0f} {FLOOR + 25:.0f}", each)


def player_alive():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process A2-Win64-Shipping -EA SilentlyContinue | % { $_.Path }"], capture_output=True, text=True).stdout
    return "PlayerNovBuild" in out


def near(objs, loc, tol=80):
    for o in objs:
        L = o.get("location") or [9e9] * 3
        if all(abs(L[k] - loc[k]) < tol for k in range(3)):
            return o
    return None


# ================================ protocol + resources ================================
init = rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "mcp_full_test", "version": "1"}})
check("initialize", "result" in init and init["result"].get("serverInfo"), init)
p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
p.stdin.flush()
tools = rpc("tools/list")["result"]["tools"]
ALL = {t["name"] for t in tools}
check(f"tools/list ({len(tools)} tools, each with a description and schema)",
      len(tools) >= 50 and all(t.get("description") and t.get("inputSchema") for t in tools), [t["name"] for t in tools])
res = rpc("resources/list").get("result", {}).get("resources", [])
check("resources/list", len(res) >= 3, res)
for r in res:
    rd = rpc("resources/read", {"uri": r["uri"]}).get("result", {}).get("contents", [])
    check(f"resources/read {r['uri']}", rd and len(rd[0].get("text", "")) > 200, str(rd)[:200])
bad = rpc("tools/call", {"name": "no_such_tool", "arguments": {}})
check("an unknown tool is an error, not a crash", ("error" in bad) or bad.get("result", {}).get("isError"), bad)

# ================================ look around ================================
ok, st = call("editor_status")
check("editor_status: world ready, in the editor", ok and st.get("worldReady") and st.get("inEditor"), st)
ok, pal = call("search_palette", query="button")
check("search_palette", ok and isinstance(pal, list) and any("Button" in i["name"] for i in pal), pal)
ok, r = call("level_list")
check("level_list", ok and isinstance(r, dict) and "local" in r, r)
ok, r = call("gamemode_list")
check("gamemode_list", ok and isinstance(r, list), r)
ok, r = call("station_scripts")
names = r if isinstance(r, list) else []
check("station_scripts lists the game's scripts", ok and len(names) >= 3, str(r)[:300])
if names:
    first = names[0]["name"] if isinstance(names[0], dict) else names[0]
    ok, r = call("station_scripts", name=first)
    check("station_scripts <name> returns source", ok and len(str(r)) > 200, str(r)[:200])
ok, secs = call("luau_guide")
check("luau_guide lists sections", ok and isinstance(secs, list) and len(secs) > 10, secs)
ok, r = call("luau_guide", topic="Teams and team changers")
check("luau_guide <topic> returns that section", ok and "team" in str(r).lower() and len(str(r)) > 200, str(r)[:200])
ok, ex = call("luau_examples")
check("luau_examples lists examples", ok and isinstance(ex, list) and len(ex) >= 10, ex)
ok, r = call("luau_examples", name="05_trigger_zone")
check("luau_examples <name> returns source", ok and "function" in str(r), str(r)[:200])
ok, r = call("luau_api_search", query="overlap player")
check("luau_api_search (multi-word) finds onOverlapByPlayerServer with its payload",
      ok and any(h.get("name") == "onOverlapByPlayerServer" and "number" in h.get("signature", "") for h in r), r)
ok, r = call("luau_api_class", name="VRPawn")
check("luau_api_class VRPawn lists the player statics", ok and "getPlayerName" in str(r) and "getPositionByID" in str(r), str(r)[:300])

# ================================ build ================================
call("level_close")                     # whatever an earlier session left open
time.sleep(2)
ok, r = call("place_object", item="PrimitiveCubeBlue", location=[0, 0, FLOOR + 50])
check("place_object without a level is refused with a clear message", not ok and "level" in str(r).lower(), r)
ok, r = call("level_new", name="ZMcpFull")
check("level_new", ok, r)
time.sleep(1.5)
ok, st = call("editor_status")
check("the new level is open", st.get("openLevel") == "ZMcpFull", st)

TRIG, HOST, TIMER, TEXT = [0.0, 1500.0, FLOOR + 100], [600.0, 1500.0, FLOOR + 50], [-600.0, 1500.0, FLOOR + 50], [-1200.0, 1500.0, FLOOR + 150]
ok, trig = call("place_object", item="PrimitiveCubeBlue", location=TRIG, scale=[2, 2, 2])
check("place_object by sandbox id (PrimitiveCubeBlue)", ok and isinstance(trig, dict) and trig.get("handle"), trig)
ok, host = call("place_object", item="Prefab_BP_CubePrimitive_Yellow_C", location=HOST)
check("place_object by palette name", ok and isinstance(host, dict) and host.get("handle"), host)
ok, timer = call("place_object", item="Timer", location=TIMER)
check("place_object by a word (Timer)", ok and isinstance(timer, dict) and timer.get("handle"), timer)
ok, text = call("place_object", item="LE_BP_Text_C", location=TEXT)
check("place a Text sign", ok and isinstance(text, dict) and text.get("handle"), text)
if not all(isinstance(x, dict) and x.get("handle") for x in (trig, host, timer, text)):
    print("cannot continue without the objects")
    sys.exit(1)
time.sleep(2)
ok, r = call("list_objects", near=HOST, radius=150)
check("list_objects near a point", ok and near(r.get("objects", []), HOST), r)
ok, r = call("list_objects", filter="TimerDisplay")
check("list_objects by class filter", ok and r.get("total", 0) >= 1, r)
ok, r = call("move_object", handle=host["handle"], location=[650, 1500, FLOOR + 50], rotation=[0, 45, 0])
check("move_object", ok, r)
time.sleep(2)
ok, r = call("list_objects", near=[650, 1500, FLOOR + 50], radius=60)
check("the move replicated back", ok and r.get("total", 0) >= 1, r)
ok, r = call("move_object", handle=host["handle"], location=HOST, rotation=[0, 0, 0])
time.sleep(2)
ok, dup = call("duplicate_object", handle=timer["handle"], location=[-600, 2100, FLOOR + 50])
check("duplicate_object returns the copy", ok and isinstance(dup, dict) and dup.get("handle") and dup["handle"] != timer["handle"], dup)
ok, r = call("select_object", handle=timer["handle"])
check("select_object", ok, r)
ok, r = call("set_camera", location=[0, 0, FLOOR + 400], pitch=-10, yaw=90)
time.sleep(1)
ok2, st = call("editor_status")
check("set_camera moves the camera", ok and ok2 and abs(st["camera"]["location"][1]) < 50 and abs(st["camera"]["rotation"][1] - 90) < 2, st.get("camera"))
ok, pr = call("get_properties", handle=timer["handle"])
check("get_properties", ok and pr.get("props"), pr)
ok, r = call("set_property", handle=timer["handle"], path="Minutes", value="5")
time.sleep(1.5)
ok2, pr = call("get_properties", handle=timer["handle"])
check("set_property Minutes=5 reads back", ok and any(x["path"] == "Minutes" and x["value"] == "5" for x in pr.get("props", [])),
      [x for x in pr.get("props", []) if x["path"] == "Minutes"])
ok, gd = call("get_game_data", handle=timer["handle"])
check("get_game_data", ok and isinstance(gd, list) and any(x["path"] == "gd/ClockEndLength" for x in gd), gd)
ok, r = call("set_game_data", handle=timer["handle"], path="gd/ClockEndLength", kind="num", value="60000")
time.sleep(2)
ok2, gd = call("get_game_data", handle=timer["handle"])
check("set_game_data ClockEndLength=60000 reads back", ok and any(x["path"] == "gd/ClockEndLength" and x["value"].startswith("60000") for x in gd),
      [x for x in gd if x.get("path") == "gd/ClockEndLength"] if isinstance(gd, list) else gd)
ok, r = call("set_text", handle=text["handle"], text="Hello {ZFullVar}")
time.sleep(2)
ok2, gd = call("get_game_data", handle=text["handle"])
check("set_text reads back in the game data", ok and "Hello {ZFullVar}" in str(gd), gd)
ok, r = call("delete_object", handle=dup["handle"])
time.sleep(2)
ok2, r2 = call("list_objects", near=[-600, 2100, FLOOR + 50], radius=60)
check("delete_object removes it", ok and r2.get("total", 1) == 0, r2)
ok, r = call("undo")
time.sleep(3)
ok2, r2 = call("list_objects", near=[-600, 2100, FLOOR + 50], radius=60)
check("undo brings the deleted object back", ok and r2.get("total", 0) >= 1, r2)
ok, r = call("redo")
time.sleep(3)
ok2, r2 = call("list_objects", near=[-600, 2100, FLOOR + 50], radius=60)
check("redo deletes it again", ok and r2.get("total", 1) == 0, r2)

# ================================ scripts ================================
TRAPS = {
    "a typed table local is an error": ("local cfg: {number} = {}\nfunction BeginPlay() end\n", False, "error"),
    "a typed number local with a value warns (runs as nil)": ("local N: number = 5\nfunction BeginPlay() log(N) end\n", False, "warning"),
    "spawnBall in a script warns": ("local B: BallSpawnerComponent = nil\nfunction BeginPlay() B:spawnBall(false, Vector.new(0,0,0)) end\n", False, "warning"),
    "spawnBall in mode code is an error": ("function OnRoundStart(r: number) Ball1:spawnBall(false, Vector.new(0,0,0)) end\n", True, "error"),
    "BeginPlay in mode code is an error": ("function BeginPlay() end\n", True, "error"),
    "an endless loop warns": ("function BeginPlay()\n\twhile true do\n\t\tlog(\"x\")\n\tend\nend\n", False, "warning"),
    "a wrong method is an error": ("local B: BasicButtonComponent = nil\nfunction BeginPlay() B:presss() end\n", False, "error"),
}
for label, (src, gm, sev) in TRAPS.items():
    ok, r = call("check_luau", source=src, gamemode_code=gm)
    check("check_luau: " + label, ok and any(x.get("severity") == sev for x in r.get("problems", [])), r)
ok, r = call("check_luau", source="function BeginPlay()\n\tlog(VRPawn.getPlayerName(VRPawn.getLocalPlayerIndex()))\nend\n")
check("check_luau: clean player code passes", ok and r.get("ok") and r.get("errors") == 0, r)
ok, r = call("write_script_file", name="ZFullFile", source='function BeginPlay()\n\tlog("ZFF version one")\nend\n')
check("write_script_file", ok, r)
ok, r = call("read_script_file", name="ZFullFile")
check("read_script_file returns it", ok and "ZFF version one" in str(r), r)
ok, r = call("attach_script", handle=timer["handle"], name="ZFullBad", source="local cfg: {number} = {}\nfunction BeginPlay() end\n")
check("attach_script refuses a script that fails the check", ok and r.get("attached") is False, r)
mark_logs()
ok, r = call("attach_script", handle=timer["handle"], name="ZFullFile",
             source='function BeginPlay()\n\tlog("ZFF version one")\n\tLuauClock.timeout(1):andThen(function() error("ZFF deliberate") end)\nend\n',
             wait_seconds=5)
glog = "\n".join(r.get("gameLog", [])) if ok and isinstance(r, dict) else ""
check("attach_script runs it and returns its log", ok and r.get("attached") and "ZFF version one" in glog, r)
check("attach_script's editor log has no [repl] dump", ok and not any(str(l).startswith("[repl]") for l in r.get("editorLog", [])), r.get("editorLog"))
check("the runtime error comes back to the agent", "ZFF deliberate" in glog, glog[-600:])
ok, r = call("script_logs", script="ZFullFile", errors_only=True, max_lines=20)
check("script_logs errors_only shows the error", ok and any("ZFF deliberate" in l for l in r.get("lines", [])), r)
m1 = r.get("mark") if isinstance(r, dict) else None
ok, r = call("update_script", name="ZFullFile", source='function BeginPlay()\n\tlog("ZFF version two")\nend\n')
check("update_script", ok and r.get("updated"), r)
time.sleep(2)
ok, r = call("script_logs", script="ZFullFile", mark=m1, max_lines=20)
check("script_logs mark= shows only newer lines (version two)", ok and any("version two" in l for l in r.get("lines", [])) and
      not any("version one" in l for l in r.get("lines", [])), r)
ok, r = call("write_script_file", name="ZFullFile", source='function BeginPlay()\n\tlog("ZFF version three")\nend\n', save_and_update=True)
time.sleep(4)
ok2, r2 = call("script_logs", script="ZFullFile", max_lines=10)
check("write_script_file save_and_update reaches the running object", ok and any("version three" in l for l in r2.get("lines", [])), r2)
mark_logs()
ok, r = call("remove_script", handle=timer["handle"], name="ZFullFile")
check("remove_script", ok, r)
time.sleep(4)
ok, r = call("update_script", name="ZFullFile", source='function BeginPlay()\n\tlog("ZFF after removal")\nend\n')
time.sleep(3)
check("a removed script no longer runs on the object", not new_log_lines("ZFF after removal"), new_log_lines("ZFF after removal"))

# ================================ player Luau APIs (the mock player walks through a trigger) ================================
PLAYER_API = r'''local Trig: PhysicalComponent = nil

local function describe(id: number): string
	local ok, s = pcall(function()
		local pos = VRPawn.getPositionByID(id)
		return `name={VRPawn.getPlayerName(id)} team={VRPawn.getTeamIndexByID(id)} pos={math.floor(pos.x)},{math.floor(pos.y)},{math.floor(pos.z)}`
	end)
	return if ok then s else "ERR " .. tostring(s)
end

function BeginPlay()
	log(`ZPL begin local={VRPawn.getLocalPlayerIndex()} trig={Trig ~= nil}`)
	if not Trig then
		return
	end
	Trig:setTriggerCollision()
	-- the documented "who is in the zone" pattern (MCP.md): enter by the server event, leave by distance
	local inside = {} :: { [number]: boolean }
	LuauClock.createTimer(0.5, function()
		for pid in inside do
			local p = VRPawn.getPositionByID(pid)
			if math.sqrt((p.x - 0) ^ 2 + (p.y - 1500) ^ 2) > 300 then
				inside[pid] = nil
				log(`ZPL left id={pid}`)
			end
		end
	end)
	Trig.onOverlapByPlayerServer.Listen(function(id: number)
		inside[id] = true
		log(`ZPL server overlap id={id} {describe(id)}`)
		local ok, err = pcall(function()
			local p = VRPawn.getPlayerByID(id)
			if p == nil then
				log("ZPL byID nil")
				return
			end
			-- a pawn exposes only a few members; the ones the station's scripts use must be there
			local has = type((p :: any).client_EmitStatEvent) == "function" and type((p :: any).client_SetQuestCompleted) == "function"
			log(`ZPL pawn idx=byID methods={has}`)
		end)
		if not ok then
			warn(`ZPL pawn methods failed: {err}`)
		end
		local ok2, err2 = pcall(function()
			Quests.event(id, "ZPL_TestEvent")
		end)
		log(`ZPL quests.event ok={ok2} {tostring(err2)}`)
	end)
	Trig.onOverlapByPlayer.Listen(function(id: number)
		log(`ZPL overlap id={id} local={VRPawn.getLocalPlayerIndex()}`)
	end)
	Trig.onOverlapByPlayerSimple.Listen(function()
		log("ZPL overlap simple")
	end)
	Trig.onOverlapByPlayerClientside.Listen(function()
		local me = VRPawn.getLocalPlayerIndex()
		log(`ZPL client overlap local={me} {describe(me)}`)
	end)
	Trig.onOverlapEndByPlayerSimple.Listen(function()
		log("ZPL overlap end")
	end)
end
'''
ok, r = call("attach_script", handle=host["handle"], name="ZPlayerApi", source=PLAYER_API, wait_seconds=3)
check("attach the player-API script", ok and r.get("attached"), r)
ok, cands = call("find_reference_targets", type="PhysicalComponent", near=TRIG, radius=300)
tr = near(cands if isinstance(cands, list) else [], TRIG)
check("find_reference_targets near= lists the trigger, with class + location", tr is not None and tr.get("class"), cands)
mark_logs()
# the ORIGINAL handles (attach rebuilt the host): they must still work
ok, r = call("set_script_reference", handle=host["handle"], script="ZPlayerApi", slot="Trig", type="PhysicalComponent", target=trig["handle"])
check("set_script_reference with handles from before the rebuild", ok, r)
time.sleep(6)
begins = new_log_lines(r"ZPL begin .*trig=true")
check("the wired script runs on every machine (server + editor + player)", len(machines(begins)) >= 3, begins)
check("getLocalPlayerIndex: -1 on the server, a real index on the player", any("local=-1" in b for b in begins) and
      any(re.search(r"local=\d", b) for b in begins), begins)
mark_logs()
walk([TRIG[0], TRIG[1] - 900], [TRIG[0], TRIG[1] + 900])
time.sleep(2)
zpl = new_log_lines(r"ZPL (server|overlap|client|pawn|quests|byID)")
for l in zpl:
    print("   ", l)
srv = [l for l in zpl if "server overlap" in l]
check("onOverlapByPlayerServer fires with the player's id", bool(srv), zpl)
check("getPlayerName / getTeamIndexByID / getPositionByID return real values",
      any(re.search(r"name=\S+ team=-?\d+ pos=-?\d+,-?\d+,-?\d+", l) and "name=None" not in l for l in srv), srv)
check("getPlayerByID -> a pawn with client_EmitStatEvent / client_SetQuestCompleted",
      any("ZPL pawn idx=byID methods=true" in l for l in zpl) and not any("failed" in l or "byID nil" in l for l in zpl), zpl)
check("Quests.event runs", any("quests.event ok=true" in l for l in zpl), zpl)
check("onOverlapByPlayerClientside fires on the player's machine (with its own name)",
      any("client overlap" in l and "name=None" not in l for l in zpl), zpl)
check("onOverlapByPlayer / onOverlapByPlayerSimple fire", any("ZPL overlap id=" in l or "overlap simple" in l for l in zpl), zpl)
check("leaving the zone is detected (the documented polling pattern)", any("ZPL left id=" in l for l in new_log_lines("ZPL left")),
      new_log_lines("ZPL left"))
# the game never fires onOverlapEndByPlayerSimple (documented); say so loudly if that ever changes
print("    note: onOverlapEndByPlayerSimple " + ("FIRED -- update MCP.md" if any("overlap end" in l for l in zpl) else "did not fire (as documented)"))
check("no script errors while the player walked through", not new_log_lines(r"RigelError|ZPlayerApi.*attempt to"),
      new_log_lines(r"RigelError|ZPlayerApi.*attempt to"))
check("the mock player's game is still running", player_alive())

# ================================ a game mode, every hook, a player joining ================================
ok, gm = call("gamemode_create", name="ZFullMode", center=[0, 2500, FLOOR], teams=2, team_names=["Reds", "Blues"], max_players=[4, 4])
check("gamemode_create", ok and isinstance(gm, dict) and gm.get("id"), gm)
for k, v in (("round_time", "12"), ("countdown", "3"), ("score_to_win", "0"), ("stop_when_empty", "0")):
    ok, r = call("gamemode_set", mode="ZFullMode", key=k, value=v)
    check(f"gamemode_set {k}={v}", ok, r)
DOOR = [-700.0, 3300.0, FLOOR]
ok, r = call("gamemode_team_changer", mode="ZFullMode", team=1, location=DOOR, yaw=0)
check("gamemode_team_changer (yaw 0: walk through along X)", ok, r)
ok, btn = call("place_object", item="BP_BasicButton_C", location=[400, 3300, FLOOR])
check("place a start button", ok and isinstance(btn, dict) and btn.get("handle"), btn)
time.sleep(2)
ok, r = call("gamemode_set_role", mode="ZFullMode", handle=btn["handle"], role="start")
check("gamemode_set_role start", ok, r)
ok, r = call("gamemode_set_code", mode="ZFullMode", source="function BeginPlay() end\n")
check("gamemode_set_code refuses BeginPlay", (not ok) or (isinstance(r, dict) and not r.get("written")), r)
HOOKS = ('function OnLobby()\n\tlog("ZGM lobby")\nend\n'
         'function OnCountdown(s: number)\n\tlog(`ZGM countdown {s}`)\nend\n'
         'function OnRoundStart(round: number)\n\tlog(`ZGM round {round} players={Rigel.players()}`)\n\tRigel.addScore(1, 2)\nend\n'
         'function OnScore(team: number, score: number, old: number)\n\tlog(`ZGM score t{team}={score} (was {old})`)\nend\n'
         'function OnTime(left: number)\n\tif left % 5 == 0 then log(`ZGM time {left}`) end\nend\n'
         'function OnRoundEnd(winner: number)\n\tlog(`ZGM end winner={winner}`)\nend\n'
         'function OnTeamChanged(team: number, size: number, old: number)\n'
         '\tlog(`ZGM team={team} size={size} old={old} players={Rigel.players()} t1={Rigel.teamSize(1)} name1={Rigel.teamName(1)} max1={Rigel.teamMax(1)}`)\nend\n')
ok, r = call("gamemode_set_code", mode="ZFullMode", source=HOOKS)
check("gamemode_set_code (every hook)", ok and r.get("written"), r)
ok, r = call("gamemode_get_code", mode="ZFullMode")
check("gamemode_get_code returns it", ok and "ZGM countdown" in str(r), str(r)[:200])
mark_logs()
ok, r = call("gamemode_apply_script", mode="ZFullMode")
check("gamemode_apply_script", ok, r)
time.sleep(6)
walk([DOOR[0] - 500, DOOR[1]], [DOOR[0] + 500, DOOR[1]])
time.sleep(3)
ok, modes = call("gamemode_list")
m = next((x for x in modes if x.get("name") == "ZFullMode"), {}) if ok and isinstance(modes, list) else {}
check("the player joined team 1 (gamemode_list)", (m.get("teams") or [{}])[0].get("players", 0) >= 1, m)
zt = new_log_lines(r"ZGM team=")
check("OnTeamChanged: size, players(), teamSize, teamName, teamMax", any(re.search(r"size=1 .*players=1 t1=1 name1=Reds max1=4", l) for l in zt), zt)
check("OnTeamChanged ran on every machine", len(machines(zt)) >= 3, zt)
ok, r = call("gamemode_control", mode="ZFullMode", action="start")
check("gamemode_control start", ok, r)
time.sleep(20)
ok, r = call("gamemode_control", mode="ZFullMode", action="end")
check("gamemode_control end", ok, r)
time.sleep(4)
ok, r = call("gamemode_control", mode="ZFullMode", action="reset")
check("gamemode_control reset", ok, r)
time.sleep(4)
zg = new_log_lines(r"ZGM (lobby|countdown|round|score|time|end)")
for l in zg:
    if l.startswith("A2.log"):
        print("   ", l)
srvz = [l for l in zg if l.startswith("A2.log")]
check("OnCountdown", any("ZGM countdown" in l for l in srvz), srvz)
check("OnRoundStart with players() = 1", any(re.search(r"ZGM round 1 players=1", l) for l in srvz), srvz)
check("OnScore after Rigel.addScore", any("ZGM score t1=2" in l for l in srvz), srvz)
check("OnTime", any("ZGM time" in l for l in srvz), srvz)
check("OnRoundEnd", any("ZGM end winner=" in l for l in srvz), srvz)
check("OnLobby after reset", any("ZGM lobby" in l for l in srvz), srvz)
check("the mock player's game is still running", player_alive())

# ================================ editor ================================
ok, r = call("screenshot", max_width=640)
check("screenshot returns an image", ok and ("image" in str(r) or "data" in str(r)), str(r)[:120])
ok, r = call("editor_op", line="where")
check("editor_op", ok, r)
ok, r = call("editor_logs", max=20)
check("editor_logs", ok and isinstance(r, dict) and r.get("lines") is not None, r)
ok, r = call("editor_logs", filter="luau", max=5)
check("editor_logs filter", ok, r)
ok, r = call("editor_problems")
check("editor_problems", ok, r)
t0 = time.time()
ok, r = call("wait", seconds=2)
check("wait waits", ok and time.time() - t0 >= 1.8, r)

# ================================ levels ================================
ok, r = call("level_save", name="ZMcpFull")
check("level_save", ok, r)
ok, r = call("level_list")
check("level_list shows the saved level", ok and "ZMcpFull" in str(r.get("local")), r)
ok, r = call("level_upload")
# (a local test server has no backend: "upload" loads the level here -- the editor says so -- rather than storing it)
check("level_upload runs the upload", ok and "upload 'ZMcpFull'" in json.dumps(r), r)
ok, r = call("gamemode_delete", mode="ZFullMode")
check("gamemode_delete", ok, r)
ok, r = call("level_close")
check("level_close", ok, r)
time.sleep(4)
ok, r = call("list_objects", near=TIMER, radius=100)
check("closing the level removes its objects", ok and r.get("total", 1) == 0, r)
ok, r = call("level_open", name="ZMcpFull")
check("level_open", ok, r)
time.sleep(8)
ok, r = call("list_objects", near=[0, 1500, FLOOR + 50], radius=1500)
check("the reopened level has its objects back", ok and r.get("total", 0) >= 4, r)
ok, r = call("level_close")
time.sleep(3)
ok, r = call("level_delete", name="ZMcpFull")
check("level_delete", ok, r)
ok, r = call("level_list")
check("the level is gone", ok and "ZMcpFull" not in str(r.get("local")), r)

# ================================ self-reference ================================
ok, r = call("level_new", name="ZMcpSelfRef")
time.sleep(1.5)
ok, cube = call("place_object", item="PrimitiveCubeBlue", location=[0, 1500, FLOOR + 50])
time.sleep(2)
ok, r = call("attach_script", handle=cube["handle"], name="ZSelfRef",
             source="local Me: PhysicalComponent = nil\nfunction BeginPlay()\n\tlog(`ZSR begin me={Me ~= nil}`)\nend\n", wait_seconds=3)
mark_logs()
ok, r = call("set_script_reference", handle=cube["handle"], script="ZSelfRef", slot="Me", type="PhysicalComponent", target=cube["handle"])
check("set_script_reference to the object itself", ok, r)
time.sleep(10)
zsr = new_log_lines(r"ZSR begin")
print("    self-ref:", zsr, flush=True)
check("self-reference: the slot resolves", any("me=true" in l for l in zsr), zsr)
check("self-reference: the mock player's game survives", player_alive(), zsr)
call("level_close")
time.sleep(2)
call("level_delete", name="ZMcpSelfRef")

# ================================ coverage ================================
missing = sorted(ALL - used)
check(f"every tool was exercised ({len(used & ALL)}/{len(ALL)})", not missing, missing)
print(f"\n{'ALL PASS' if not fails else str(fails) + ' FAILED'}")
with open(os.path.join(TEMP, "mcp_full_results.txt"), "w", encoding="utf-8") as fh:
    fh.write("\n".join(results) + f"\n{'ALL PASS' if not fails else str(fails) + ' FAILED'}\n")
p.stdin.close()
p.terminate()
