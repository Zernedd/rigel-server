"""Repro: 2026-09-27 18:35 (VPS) the Funhouse server died right after an editor placed TackleballTrainingSystem
(sandbox spawn: 5 templates, 23 default nodes) -- AV in the netvar tree walk (+0x54B6D15 under a +0x467101C recursion).
Places it on the bottom floor the way the editor does, then watches the local server for 3 min (it died 2 min after the placement).
Prints PASS if the server survives, FAIL if it dies. (LocalSmoke.ps1 -Repro <this>)"""
import os, subprocess, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "..", "SpecEditor", "luau", "mcp"))
import rigel_mcp as m  # noqa: E402


def server_alive():
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-Process A2-Win64-Shipping -EA SilentlyContinue | Where-Object { $_.Path -like '*\\Nov15\\*' } | Select-Object -First 1 -ExpandProperty Id"],
                         capture_output=True, text=True).stdout.strip()
    return out


pid = server_alive()
if not pid:
    print("FAIL no local server before the repro"); sys.exit(0)
# The raw spawn command, as an older editor (or any script) sends it -- the current editor's palette blocks the item,
# so this exercises the server's own refusal.
items = m.t_palette({"query": "BP_TackleballTraining_C"})
items = items if isinstance(items, list) else items.get("items", [])
path = next((i.get("path") for i in items if i.get("name") == "BP_TackleballTraining_C"), None)
if not path:
    print("FAIL BP_TackleballTraining_C is not in the palette"); sys.exit(0)
print("INFO", m.BR.call("raw", f"SE|SPAWN|{path}|-900.0,4400.0,-28625.0|0,0,0|1,1,1"))
t0 = time.time()
while time.time() - t0 < 180:            # it faults once at once (caught), then dies ~2 min later
    time.sleep(3)
    if server_alive() != pid:
        print(f"FAIL the local server (pid {pid}) died after TackleballTrainingSystem was placed"); sys.exit(0)
print(f"PASS the local server (pid {pid}) survived placing TackleballTrainingSystem")
