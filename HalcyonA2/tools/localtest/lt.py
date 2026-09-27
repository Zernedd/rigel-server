"""lt.py -- the local smoke test's editor / mock-player steps (LocalSmoke.ps1 drives it). Every step is capped and
prints one line starting with PASS / FAIL / INFO.

  env: RIGEL_MCP_PORT (the test editor), MOCK_DIR (the mock player's TEMP: A2PlayerControl.cmd / .log live there)

  python lt.py open <level>          open a saved level in the test editor
  python lt.py teams                 print every game mode's team sizes
  python lt.py onteam                walk the mock onto MiniJakeball's team 1 (route over the wall arena's roof)
  python lt.py kickoff               ball into the ring -> the mode reaches RUNNING
  python lt.py goal                  ball in through the +u goal's mouth -> a score changes
  python lt.py reload <level>        close + re-open the level (players on teams are taken off first)
"""
import os, re, sys, time
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "..", "SpecEditor", "luau", "mcp"))
import rigel_mcp as m  # noqa: E402

MOCK = os.environ.get("MOCK_DIR", os.environ.get("TEMP", "."))
CMD, LOG = os.path.join(MOCK, "A2PlayerControl.cmd"), os.path.join(MOCK, "A2PlayerControl.log")
FX, Y0, Z0 = -28575.0, 11300.0, -300.0            # the wall pitch MiniJakeball stands on (floor x, centre y, z)


def out(kind, msg):
    print(f"{kind} {msg}", flush=True)


def me(pre=""):
    open(CMD, "w").write(pre + "pawns\n")
    time.sleep(1.0)
    lines = [l for l in open(LOG, encoding="utf-8", errors="replace") if "<-- me" in l] if os.path.exists(LOG) else []
    if not lines:
        return None
    return [float(v) for v in re.search(r"pos=\(([-\d.]+), ([-\d.]+), ([-\d.]+)\)", lines[-1]).groups()]


def goto(x, y, z, cap):
    t0 = time.time()
    p = me(f"goto {x:.0f} {y:.0f} {z:.0f}\n")
    while p and time.time() - t0 < cap and not all(abs(a - b) < 60 for a, b in zip(p, (x, y, z))):
        p = me(f"goto {x:.0f} {y:.0f} {z:.0f}\n")
    return p


def modes():
    return m.t_gmlist({})


def mode(name="MiniJakeball"):
    for g in modes():
        if g.get("name") == name:
            return g
    return None


def ball():
    b = [o for o in m.BR.call("objects", "BP_JakeBall_C", FX + 300, Y0, Z0, 6000, 20)["objects"] if "Spawner" not in o["class"]]
    return b[0] if b else None


def wait_state(want, cap):
    t0 = time.time()
    while time.time() - t0 < cap:
        g = mode()
        if g and g["state"] == want:
            return True
        time.sleep(2)
    return False


cmd = sys.argv[1] if len(sys.argv) > 1 else ""
try:
    if cmd == "open":
        m.t_lvopen({"name": sys.argv[2]})
        time.sleep(8)
        out("PASS" if m.t_status({}).get("openLevel") == sys.argv[2] else "FAIL", f"level {sys.argv[2]} open")
    elif cmd == "teams":
        for g in modes():
            out("INFO", f"{g['name']} {g['state']} " + " ".join(f"{t['name']}={t['players']}" for t in g["teams"]))
    elif cmd == "onteam":
        steps = [(100, -3250, -24500, 35), (-27450, 11080, 2700, 120), (-28521, 11080, 2150, 40), (-28521, 11080, 900, 40)]
        for x, y, z, cap in steps:
            p = goto(x, y, z, cap)
        time.sleep(3)
        g = mode()
        n = g["teams"][0]["players"] if g and g["teams"] else 0
        out("PASS" if n >= 1 else "FAIL", f"mock on team 1 (players {n}, mock at {[round(v) for v in p] if p else None})")
    elif cmd == "kickoff":
        b = ball()
        if not b:
            out("FAIL", "no ball"); sys.exit(0)
        m.t_ballcarry({"handle": b["handle"], "location": [FX + 260, Y0, Z0], "seconds": 2})
        out("PASS" if wait_state("running", 45) else "FAIL", "kickoff -> running")
        time.sleep(22)                                  # the countdown and the fling
    elif cmd == "goal":
        before = [t["score"] for t in mode()["teams"]]
        m.t_ballcarry({"handle": ball()["handle"], "location": [FX + 150, Y0 + 2450, Z0], "seconds": 2})
        time.sleep(2.5)
        m.t_ballcarry({"handle": ball()["handle"], "location": [FX + 120, Y0 + 3060, Z0], "seconds": 1.5})
        t0, after = time.time(), before
        while time.time() - t0 < 15 and after == before:
            time.sleep(1.5)
            after = [t["score"] for t in mode()["teams"]]
        out("PASS" if after != before else "FAIL", f"goal: scores {before} -> {after}")
    elif cmd == "reload":
        m.t_lvclose({})
        m.t_lvopen({"name": sys.argv[2]})
        time.sleep(12)
        out("PASS" if m.t_status({}).get("openLevel") == sys.argv[2] else "FAIL", f"level {sys.argv[2]} closed and re-opened")
    else:
        print(__doc__)
except Exception as e:  # a step that throws is a failed step, not a crashed harness
    out("FAIL", f"{cmd}: {type(e).__name__}: {e}")
