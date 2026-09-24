"""mcp_live_test.py -- drives a running Spec Editor ONLY through the MCP server (JSON-RPC over stdio), the way an
AI agent would: status, level, place, inspect, write -> check -> attach Luau, read the errors, fix, game mode.
Run with an editor in a server (tests/t_mcp_idle.txt keeps one idle)."""
import json
import os
import subprocess
import sys
import time

MCP = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "luau", "mcp", "rigel_mcp.py")
p = subprocess.Popen([sys.executable, MCP], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, encoding="utf-8")
nid = 0
fails = 0


def rpc(method, params=None):
    global nid
    nid += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": nid, "method": method, "params": params or {}}) + "\n")
    p.stdin.flush()
    return json.loads(p.stdout.readline())


def call(tool_name, **args):
    r = rpc("tools/call", {"name": tool_name, "arguments": args})["result"]
    txt = r["content"][0]["text"]
    try:
        val = json.loads(txt)
    except Exception:
        val = txt
    return (not r.get("isError")), val


def check(label, ok, detail=""):
    global fails
    print(("PASS " if ok else "FAIL ") + label + ("" if ok else f"   -> {str(detail)[:600]}"), flush=True)
    if not ok:
        fails += 1


print(rpc("initialize", {"protocolVersion": "2024-11-05"})["result"]["serverInfo"])
tools = rpc("tools/list")["result"]["tools"]
check(f"tools/list ({len(tools)} tools)", len(tools) > 40)

ok, st = call("editor_status")
check("editor_status", ok and st.get("worldReady"), st)
cam = st["camera"]["location"] if ok else [0, 0, 0]

ok, r = call("place_object", item="BP_BasicButton_C", location=[0, -2600, -27000])
check("place without a level is refused with a clear message", not ok and "level" in str(r).lower(), r)

ok, r = call("level_new", name="McpTestLevel")
check("level_new", ok, r)
time.sleep(1)
ok, st = call("editor_status")
check("a level is open", ok and st.get("openLevel") != "", st)
if not st.get("openLevel"):
    call("level_save", name="McpTestLevel")
    time.sleep(2)

ok, pal = call("search_palette", query="BasicButton")
check("search_palette finds the button", ok and any("BasicButton" in i["name"] for i in pal), pal)

ok, btn = call("place_object", item="BP_BasicButton_C", location=[0, -2600, -27000])
check("place_object returns the new object", ok and isinstance(btn, dict) and btn.get("handle"), btn)
ok, door = call("place_object", item="PrimitiveCubeForceField", location=[400, -2600, -27000])
if not ok:
    ok, door = call("place_object", item="ForceFieldA", location=[400, -2600, -27000])
check("place a second object", ok and door.get("handle"), door)

ok, objs = call("list_objects", filter="BasicButton", near=[0, -2600, -27000], radius=300)
check("list_objects near a point", ok and objs["total"] >= 1, objs)

ok, mv = call("move_object", handle=btn["handle"], location=[0, -2500, -27000], rotation=[0, 90, 0])
check("move_object", ok, mv)
time.sleep(1.5)
ok, objs = call("list_objects", filter="BasicButton", near=[0, -2500, -27000], radius=150)
check("the move replicated back", ok and objs["total"] >= 1, objs)
if ok and objs["objects"]:
    btn = objs["objects"][0]

ok, props = call("get_properties", handle=btn["handle"])
check("get_properties", ok and props.get("props"), props)

# a script with mistakes -> check catches them
bad = "local Btn: BasicButtonComponent = nil\nlocal cfg: {number} = {}\nfunction BeginPlay()\n\tBtn:presss()\nend\n"
ok, chk = call("check_luau", source=bad)
check("check_luau finds the crashing typed local and the bad method", ok and chk["errors"] >= 2, chk)
ok, r = call("attach_script", handle=btn["handle"], name="ZMcpBad", source=bad)
check("attach_script refuses a script that fails the check", ok and r.get("attached") is False, r)

# a script that errors at RUN time -> attach -> the log shows it -> fix -> clean
runtime_bad = ("function BeginPlay()\n\tlog(\"ZMCP hello from the agent\")\n"
               "\tlocal t = nil\n\tLuauClock.timeout(1):andThen(function() log(\"ZMCP tick\"); error(\"ZMCP deliberate failure\") end)\nend\n")
ok, r = call("attach_script", handle=btn["handle"], name="ZMcpRun", source=runtime_bad, wait_seconds=6)
lines = "\n".join(r.get("gameLog", [])) if ok else ""
check("attach_script attaches and returns the script's own log", ok and r.get("attached") and "ZMCP hello" in lines, r)
check("the runtime error is visible to the agent", "ZMCP deliberate failure" in lines, lines[-800:])
fixed = "function BeginPlay()\n\tlog(\"ZMCP fixed version running\")\nend\n"
ok, r = call("update_script", name="ZMcpRun", source=fixed)
time.sleep(3)
ok2, lg = call("script_logs", script="ZMcpRun", max_lines=30)
check("update_script -> the fixed version runs", ok and ok2 and any("fixed version" in l for l in lg["lines"]), lg)

# reference wiring: a second script drives the force field through a typed slot
ok, cands = call("find_reference_targets", type="BasicButtonComponent")
check("find_reference_targets", ok and isinstance(cands, list), cands)

# text
ok, txt = call("place_object", item="LE_BP_Text_C", location=[-400, -2600, -26900])
if ok:
    time.sleep(2)
    ok, r = call("set_text", handle=txt["handle"], text="Hello {ZMcpVar}")
    check("set_text", ok, r)

# game mode
ok, gm = call("gamemode_create", name="McpMode", center=[0, -2600, -27000], teams=2, team_names=["Ants", "Bees"], max_players=[3, 3])
check("gamemode_create", ok and isinstance(gm, dict) and gm.get("id"), gm)
if ok and gm.get("id"):
    ok, r = call("gamemode_set", mode="McpMode", key="round_time", value="30")
    check("gamemode_set", ok, r)
    ok, r = call("gamemode_team_changer", mode="McpMode", team=1, location=[-800, -4000, -27000])
    check("gamemode_team_changer", ok, r)
    ok, r = call("gamemode_set_role", mode="McpMode", handle=btn["handle"], role="start")
    check("gamemode_set_role", ok, r)
    code = "function OnRoundStart(round: number)\n\tlog(\"ZMCP round \" .. round)\n\tRigel.addScore(2, 3)\nend\n"
    ok, r = call("gamemode_set_code", mode="McpMode", source=code)
    check("gamemode_set_code applies", ok and r.get("written"), r)
    time.sleep(3)
    ok, r = call("gamemode_control", mode="McpMode", action="startnow")
    time.sleep(4)
    ok, modes = call("gamemode_list")
    m = next((x for x in modes if x.get("name") == "McpMode"), {})
    check("the round runs and the mode code scored", m.get("state") == "running" or m.get("state") == "ended" and m["teams"][1]["score"] == 3, m)
    check("team 2 has 3 points", m.get("teams", [{}, {}])[1].get("score") == 3, m)

ok, r = call("undo")
check("undo op", ok, r)
ok, r = call("editor_op", line="where")
check("editor_op", ok, r)
ok, r = call("luau_api_class", name="Rigel")
check("luau_api_class Rigel", ok and "addScore" in str(r), r)
ok, r = call("luau_guide")
check("luau_guide lists sections", ok and isinstance(r, list) and len(r) > 5, r)
ok, r = call("level_save", name="McpTestLevel")
check("level_save", ok, r)
print(f"\n{'ALL PASS' if not fails else str(fails) + ' FAILED'}")
p.stdin.close()
p.terminate()
