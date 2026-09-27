"""Repro: 2026-09-27 19:56 / 23:07 (VPS) the server died while text signs were placed and edited.
Live sequence (payload log, pid 3012): a level was loading (dozens of spawns into one area) while text signs got their
text -> `leaf Properties/Text (32 bytes) on <sign>: 1` -> SeSbSet -> SbWriteLeaves -> AModuleSlot::PushNetVars faulted in
the game's node code (+0x465C436, reading 0xFFFF...: the replaced leaf) -> the next text write overflowed the stack in the
node-path builder (+0x4665FAB, the id-cycle loop) and the server died. Replacing a Properties leaf on a LIVE node
(NodeRemove + AddChild) while its area is busy is what breaks the tree.

This does the same, harder: signs placed and re-texted in bursts WHILE BallBattle is closed and re-opened (175 spawns,
templated team-name text rendered by the text bridge), then moved, re-texted and some deleted.
FAIL if the server dies, or if the payload log shows ANY fault in the game's netvar node code (+0x4640000..+0x46F0000)
during the run -- those are caught, but they are what corrupts the tree that kills the server later.
Prints PASS otherwise. (LocalSmoke.ps1 -Repro <this>)"""
import os, re, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "SpecEditor", "luau", "mcp"))
import rigel_mcp as m  # noqa: E402

PAYLOAD_LOG = os.path.join(os.environ.get("TEMP", ""), "HalcyonA2.log")
NODE_CODE = (0x4640000, 0x46F0000)


def server_alive():
    return subprocess.run(["powershell", "-NoProfile", "-Command",
                           "Get-Process A2-Win64-Shipping -EA SilentlyContinue | Where-Object { $_.Path -like '*\\Nov15\\*' } | Select-Object -First 1 -ExpandProperty Id"],
                          capture_output=True, text=True).stdout.strip()


def log_size():
    try:
        return os.path.getsize(PAYLOAD_LOG)
    except OSError:
        return 0


def node_faults(since):
    """FATAL headers in the payload log after byte `since` whose rip is in the game's netvar node code."""
    out = []
    try:
        with open(PAYLOAD_LOG, "rb") as f:
            f.seek(since)
            data = f.read().decode("utf-8", "replace")
    except OSError:
        return out
    for mt in re.finditer(r"FATAL code=(\S+) rip=0x([0-9A-F]+) faultAddr=(\S+) gameBase=0x([0-9A-F]+)", data):
        off = int(mt.group(2), 16) - int(mt.group(4), 16)
        if NODE_CODE[0] <= off < NODE_CODE[1]:
            out.append(f"{mt.group(1)} game+0x{off:X} fa={mt.group(3)}")
    return out


def check(pid, since, what):
    if server_alive() != pid:
        print(f"FAIL the local server (pid {pid}) died {what}")
        sys.exit(0)
    nf = node_faults(since)
    if nf:
        print(f"FAIL {len(nf)} fault(s) in the game's node code {what}: {nf[0]}")
        sys.exit(0)


def handle_of(resp):
    if isinstance(resp, dict):
        for k in ("handle", "ident", "id", "idx"):
            if isinstance(resp.get(k), str) and resp[k]:
                return resp[k]
    return None


pid = server_alive()
if not pid:
    print("FAIL no local server before the repro"); sys.exit(0)
since = log_size()
items = m.t_palette({"query": "Text"})
items = items if isinstance(items, list) else items.get("items", [])
item = next((i for i in items if i.get("name") == "LE_BP_Text_C"), None) or \
       next((i for i in items if "BP_Text" in (i.get("name") or "")), None)
if not item:
    print("FAIL no Text sign in the palette"); sys.exit(0)
path = item.get("path") or item.get("name")
level = (m.t_levels({}) or {}).get("open") or "BallBattle"
print("INFO text item:", item.get("name"), "| level:", level)

def safe(fn, a):
    try:
        return fn(a)
    except m.EditorError as e:            # a sign the reload took away
        return {"error": str(e)}


handles = []
placed = 0
for rnd in range(3):
    handles = []                          # a reload takes the previous round's signs with the level
    # the level reload runs in the editor while we keep placing and re-texting
    print("INFO close:", str(m.t_lvclose({}))[:100])
    print("INFO open:", str(m.t_lvopen({"name": level}))[:100])
    for i in range(8):
        at = [-900.0 + 120 * i, 4400.0 + 300 * rnd, -28560.0]
        h = handle_of(safe(m.t_place, {"item": path, "location": at}))
        if h:
            handles.append((h, at)); placed += 1
            safe(m.t_text, {"handle": h, "text": f"Do not feed the specimen {rnd}.{i}"})
    for h, at in handles:
        safe(m.t_text, {"handle": h, "text": f"round {rnd} {h[-4:]}"})
    check(pid, since, f"in round {rnd} (level reload + {len(handles)} signs)")
if placed < 10 or len(handles) < 4:
    print(f"FAIL only {placed} text sign(s) could be placed -- the repro did not exercise the path"); sys.exit(0)
for n, (h, at) in enumerate(handles):
    safe(m.t_move, {"handle": h, "location": [at[0], at[1] + 150, at[2]]})
time.sleep(3)                             # a move rebuilds the object; the editor can't address it until that lands
errs = []
for n, (h, at) in enumerate(handles):
    r = safe(m.t_text, {"handle": h, "text": f"sign {n} moved"})
    if isinstance(r, dict) and r.get("error"):
        errs.append(r["error"])
if errs:
    print(f"INFO {len(errs)} re-text(s) after the moves were refused: {errs[0]}")
time.sleep(4)
check(pid, since, "after moving and re-texting the signs")
# the text must actually be on the objects (a fix that loses it is no fix)
got = safe(m.t_data, {"handle": handles[-1][0]})
if "moved" not in str(got):
    print(f"FAIL the last sign's text did not stick: {str(got)[:200]}"); sys.exit(0)
for h, at in handles[::2]:
    safe(m.t_delete, {"handle": h})
t0 = time.time()
while time.time() - t0 < 150:
    time.sleep(5)
    check(pid, since, "after text signs were placed, edited, moved and deleted")
print(f"PASS the local server (pid {pid}) survived {placed} text signs through 3 level reloads, with no node-code faults")
