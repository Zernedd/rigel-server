#!/usr/bin/env python3
r"""
rigel_mcp.py -- an MCP server that lets an AI agent (Claude Code, Claude Desktop, Codex, Cursor, ...) use the
Rigel Spec Editor like a person does: look around, place / move / delete objects, edit their properties and game
data, write Luau, CHECK it, attach it, read the errors, fix it, build game modes and levels.

    It talks to the Spec Editor running on this PC (the editor mod listens on 127.0.0.1:47650).
    Nothing to install: Python 3.9+ only.

Add it to your agent (see Rigel-MCP-Guide.pdf / MCP.md next to this file):
    claude mcp add rigel -- python "%USERPROFILE%\Documents\RigelScripts\mcp\rigel_mcp.py"
    codex:  [mcp_servers.rigel] command = "python"  args = ["C:\\Users\\<you>\\Documents\\RigelScripts\\mcp\\rigel_mcp.py"]
"""
from __future__ import annotations

import glob
import json
import math
import os
import re
import socket
import subprocess
import sys
import tempfile
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
KIT = os.path.dirname(HERE)                                   # RigelLuau / RigelScripts (types, tools, examples ...)
DOCS = os.path.join(os.environ.get("USERPROFILE", os.path.expanduser("~")), "Documents")
SCRIPTS = os.path.join(DOCS, "RigelScripts")
LEVELS = os.path.join(DOCS, "RigelLevels")


def kit_path(*parts: str) -> str:
    for base in (KIT, SCRIPTS):
        p = os.path.join(base, *parts)
        if os.path.exists(p):
            return p
    return os.path.join(KIT, *parts)


# ────────────────────────────────────────────────────────────────────────────── editor bridge
class Bridge:
    def __init__(self):
        self.sock = None
        self.buf = b""
        self.next_id = 1
        self.lock = threading.Lock()

    def _ports(self):
        if os.environ.get("RIGEL_MCP_PORT"):        # pin one editor (several can run: tests next to the user's own)
            return [int(os.environ["RIGEL_MCP_PORT"])]
        ports = []
        try:
            with open(os.path.join(tempfile.gettempdir(), "rigel_mcp_port.txt")) as fh:
                ports.append(int(fh.read().strip()))
        except Exception:
            pass
        return ports + [p for p in range(47650, 47655) if p not in ports]

    def connect(self):
        last = None
        for port in self._ports():
            try:
                s = socket.create_connection(("127.0.0.1", port), timeout=2)
                s.settimeout(None)
                self.sock = s
                self.buf = b""
                return
            except OSError as e:
                last = e
        raise RuntimeError("The Spec Editor isn't running (nothing on 127.0.0.1:47650-47654). Start the editor build "
                           "and join a server; the MCP bridge starts with it. (" + str(last) + ")")

    def call(self, cmd: str, *args, timeout: float = 20.0):
        with self.lock:
            for attempt in (1, 2):
                try:
                    if not self.sock:
                        self.connect()
                    rid = self.next_id
                    self.next_id += 1
                    line = "\t".join([str(rid), cmd] + [str(a if a is not None else "").encode("utf-8").hex() for a in args]) + "\n"
                    self.sock.sendall(line.encode("utf-8"))
                    self.sock.settimeout(timeout)
                    while True:
                        while b"\n" not in self.buf:
                            chunk = self.sock.recv(1 << 16)
                            if not chunk:
                                raise ConnectionError("the editor closed the connection")
                            self.buf += chunk
                        raw, self.buf = self.buf.split(b"\n", 1)
                        msg = json.loads(raw.decode("utf-8", "replace"))
                        if msg.get("id") == rid:
                            self.sock.settimeout(None)
                            if not msg.get("ok"):
                                raise EditorError(msg.get("error", "failed"))
                            return msg.get("result")
                except EditorError:
                    raise
                except (OSError, ConnectionError, json.JSONDecodeError) as e:
                    try:
                        if self.sock:
                            self.sock.close()
                    except Exception:
                        pass
                    self.sock = None
                    if attempt == 2:
                        raise RuntimeError(f"Lost the editor connection: {e}")


class EditorError(Exception):
    pass


BR = Bridge()


# ────────────────────────────────────────────────────────────────────────────── game log (Luau output)
def game_log_path() -> str:
    try:
        st = BR.call("status")
        if st.get("gameLog"):
            return st["gameLog"]
    except Exception:
        pass
    logs = glob.glob(os.path.join(os.environ.get("LOCALAPPDATA", ""), "A2", "Saved", "Logs", "A2*.log"))
    return max(logs, key=os.path.getmtime) if logs else ""


_log_pos: dict[str, int] = {}


def read_game_log(since_mark: int | None = None, script: str = "", errors_only: bool = False, max_lines: int = 200):
    """Lines from the editor's own game log (every machine runs every script, so the editor's log has the
    script's log()/warn()/errors). since_mark: a byte offset from a previous call (returned as 'mark')."""
    path = game_log_path()
    if not path or not os.path.exists(path):
        return {"mark": 0, "lines": [], "note": "no game log found"}
    size = os.path.getsize(path)
    start = since_mark if since_mark is not None and since_mark <= size else max(0, size - 200_000)
    with open(path, "rb") as fh:
        fh.seek(start)
        data = fh.read().decode("utf-8", "replace")
    out = []
    keep = False                                          # a kept line's continuation (error text, traceback)
    for ln in data.splitlines():
        if not re.match(r"^\[\d{4}\.\d\d\.\d\d-", ln):         # not a new UE log entry: a continuation
            if keep and ln.strip():
                out.append("    " + ln.strip())
            continue
        keep = False
        if "LogLuau" not in ln and "RigelError" not in ln and "Luau" not in ln:
            continue
        if script and script not in ln and "Promise" not in ln:
            continue
        if errors_only and not re.search(r"Error|Warning|RigelError|error|failed|rejection", ln):
            continue
        out.append(re.sub(r"^\[[^\]]*\]\[\s*\d+\]", "", ln))
        keep = True
    return {"mark": size, "lines": out[-max_lines:], "log": path}


def log_mark() -> int:
    p = game_log_path()
    return os.path.getsize(p) if p and os.path.exists(p) else 0


# ────────────────────────────────────────────────────────────────────────────── Luau knowledge
_api = None


def api_index():
    global _api
    if _api is None:
        try:
            with open(kit_path("types", "api_index.json"), encoding="utf-8") as fh:
                _api = json.load(fh)
        except Exception:
            _api = {"classes": {}}
    return _api


def defs_text() -> str:
    try:
        with open(kit_path("types", "rigel.d.luau"), encoding="utf-8") as fh:
            return fh.read()
    except Exception:
        return ""


_event_sigs = None


def event_sig(cls: str, name: str) -> str:
    """An event's payload from the definitions (the index has none): `onOverlapByPlayerServer: Event<(number)>`."""
    global _event_sigs
    if _event_sigs is None:
        _event_sigs = {}
        cur = None
        for ln in defs_text().splitlines():
            m = re.match(r"declare extern type (\w+)", ln)
            if m:
                cur = m.group(1)
                continue
            m = re.match(r"\t(\w+): (Event<.*>)\s*$", ln)
            if cur and m:
                _event_sigs[(cur, m.group(1))] = m.group(2)
    return _event_sigs.get((cls, name), "")


def api_search(query: str, limit: int = 40):
    # Every word has to match (in any order), so "overlap player" finds onOverlapByPlayer and "vrpawn name" finds
    # VRPawn.getPlayerName. A method matches on its own name + signature + its class's name.
    terms = [t for t in re.split(r"[\s,.:]+", query.lower()) if t]
    if not terms:
        return []

    def hit(*texts):
        h = " ".join(t.lower() for t in texts if t)
        return all(t in h for t in terms)

    hits = []
    for cname, c in api_index().get("classes", {}).items():
        if hit(cname):
            hits.append({"class": cname, "parent": c.get("parent"), "why": c.get("why", "")})
        for kind in ("methods", "events", "properties", "static"):
            for m in c.get(kind, []) or []:
                name = m.get("name", "") if isinstance(m, dict) else str(m)
                sig = m.get("sig", "") if isinstance(m, dict) else ""
                if not sig and kind == "events":
                    sig = event_sig(cname, name)
                if hit(name, sig, cname):
                    hits.append({"class": cname, "kind": kind[:-1] if kind != "static" else "static", "name": name, "signature": sig,
                                 "cpp": m.get("cpp", "") if isinstance(m, dict) else ""})
    # globals / Rigel library in the definitions file
    for ln in defs_text().splitlines():
        if hit(ln) and (ln.startswith("declare") or ln.startswith("\t") and ":" in ln and "->" in ln):
            hits.append({"definition": ln.strip()})
    # exact-name hits first, then shorter names
    hits.sort(key=lambda h: (0 if h.get("name", "").lower() == query.lower().strip() else 1, len(h.get("name", h.get("class", "")) or "")))
    return hits[:limit]


def api_class(name: str) -> str:
    txt = defs_text()
    m = re.search(r"^declare extern type " + re.escape(name) + r"\b.*?^end\s*$", txt, re.S | re.M)
    statics = re.search(r"^declare " + re.escape(name) + r"\s*:\s*\{.*?^}\s*$", txt, re.S | re.M)
    if m:
        block = m.group(0)
        parent = re.search(r"extends (\w+)", block.splitlines()[0])
        extra = f"\n\n-- inherits from {parent.group(1)}: call luau_api_class('{parent.group(1)}') for its members" if parent else ""
        # the class's static functions are a separate global table (VRPawn.getPlayerName(id) ...): show them too
        st = f"\n\n-- static functions (call as {name}.fn(...)):\n{statics.group(0)}" if statics else ""
        return block + extra + st
    m = re.search(r"^declare " + re.escape(name) + r"\b.*?^}\s*$", txt, re.S | re.M)
    if m:
        return m.group(0)
    close = [c for c in api_index().get("classes", {}) if name.lower() in c.lower()][:20]
    return f"No class '{name}'. Close matches: {', '.join(close) or 'none'} (try luau_api_search)."


GUIDE_FILES = [("MCP guide (read first)", ("mcp", "MCP.md")), ("Game modes guide", ("GAMEMODES.md",)),
               ("Scripting kit README", ("README.md",))]


def guide_sections():
    secs = []
    for title, parts in GUIDE_FILES:
        p = kit_path(*parts)
        if not os.path.exists(p):
            continue
        with open(p, encoding="utf-8") as fh:
            txt = fh.read()
        cur, buf = title, []
        for ln in txt.splitlines():
            if ln.startswith("## "):
                if buf:
                    secs.append((cur, "\n".join(buf)))
                cur, buf = f"{title} / {ln[3:].strip()}", []
            else:
                buf.append(ln)
        if buf:
            secs.append((cur, "\n".join(buf)))
    return secs


# ────────────────────────────────────────────────────────────────────────────── checking Luau
PROPERTY_OK = re.compile(r"^(number|string|boolean|[A-Za-z_][A-Za-z0-9_]*Component)\??$")


def rigel_traps(src: str, gamemode_code: bool = False):
    """The game's own traps that the type checker doesn't know about (same rules as tools/rigel-check.ps1)."""
    out = []
    depth = 0
    for i, raw in enumerate(src.splitlines(), 1):
        line = re.sub(r"--.*$", "", raw)
        if depth == 0:
            m = re.match(r"\s*local\s+([A-Za-z_]\w*)\s*:\s*([^=]+?)\s*(=|$)", line)
            if m:
                t = m.group(2).strip()
                if not PROPERTY_OK.match(t):
                    out.append({"line": i, "severity": "error", "message":
                                f"Top-level typed local '{m.group(1)}: {t}': in this game every typed top-level local becomes an "
                                f"editor property, and only number/string/boolean/<Something>Component can be one. This type CRASHES "
                                f"the server and every client. Drop the annotation (local {m.group(1)} = ...) or use a supported type."})
                elif t.rstrip("?") in ("number", "string", "boolean") and m.group(3) == "=":
                    out.append({"line": i, "severity": "warning", "message":
                                f"'{m.group(1)}: {t}' is an editor property: its value comes from the editor (Details), and the "
                                f"literal here is NOT applied -- it runs as nil until set there. For a constant drop the annotation "
                                f"(local {m.group(1)} = ...)."})
        if re.search(r"\bspawn(Ball|BallWithParameters|PersonalBall|SinglePersonalBall|HeartBall)\s*\(|\bserver_SpawnBall\s*\(", line):
            out.append({"line": i, "severity": "error" if gamemode_code else "warning", "message":
                        "Don't spawn balls from a script: it runs on every machine and each makes its own client-only ball that "
                        "nobody else sees. Give the spawner the game mode 'Ball spawner' role (the server spawns/resets it) and "
                        "call Rigel.resetBalls(); or use getSpawnedBall() / resetBall() on a spawner's own ball."})
        if gamemode_code and re.match(r"\s*function\s+BeginPlay\s*\(", line):
            out.append({"line": i, "severity": "error", "message":
                        "Game mode code must not define BeginPlay: the generated controller owns it (yours would replace it and "
                        "the mode stops working). Use the hooks: OnLobby, OnCountdown, OnRoundStart, OnRoundEnd, OnScore, OnTime, "
                        "OnTeamChanged -- or run setup at the top level."})
        if re.search(r"\bwhile\s+true\s+do\b", line) and "wait" not in src:
            out.append({"line": i, "severity": "warning", "message":
                        "An endless loop with no yield freezes the game thread. Use LuauClock.createTimer / LuauClock.timeout instead."})
        if re.search(r"\b(os|io|require|loadstring|getfenv|setfenv)\s*[.(]", line) and "getfenv()[" not in line:
            out.append({"line": i, "severity": "warning", "message": "This library isn't available in the game's Luau sandbox."})
        depth += len(re.findall(r"\b(function|do|then|repeat)\b", line)) - len(re.findall(r"\b(end|until)\b", line))
        depth = max(depth, 0)
    return out


def luau_lsp() -> str:
    for p in (kit_path("tools", "luau-lsp.exe"), kit_path("vendor", "luau-lsp.exe")):
        if os.path.exists(p):
            return p
    ext = glob.glob(os.path.join(os.environ.get("USERPROFILE", ""), ".vscode", "extensions", "johnnymorganz.luau-lsp-*", "bin", "server.exe"))
    return ext[-1] if ext else ""


def check_luau(source: str, name: str = "Script", gamemode_code: bool = False):
    problems = []
    lsp = luau_lsp()
    defs = kit_path("types", "rigel.d.luau")
    tmpdir = tempfile.mkdtemp(prefix="rigel_check_")
    src = source
    if gamemode_code:
        # game mode code is spliced into the controller: its role slots are locals declared before it
        src = "".join(f"local {s}: any = nil\n" for s in re.findall(r"\b((?:Start|Score|TrapRound|TrapPulse|TrapFired|TrapButton|"
                                                                  r"WallLobby|WallRound|Timer|Ball|ScoreBoard|ScoreTable)\d+)\b", source)) + source
    shift = src.count("\n") - source.count("\n")
    fpath = os.path.join(tmpdir, re.sub(r"[^\w-]", "_", name) + ".luau")
    with open(fpath, "w", encoding="utf-8") as fh:
        fh.write(src)
    if lsp and os.path.exists(defs):
        try:
            r = subprocess.run([lsp, "analyze", f"--definitions={defs}", fpath], capture_output=True, text=True, timeout=60)
            for ln in (r.stdout + r.stderr).splitlines():
                m = re.match(r".*\((\d+),(\d+)\): (\w+): (.*)$", ln)
                if m:
                    kind = m.group(3)
                    if kind in ("LocalUnused", "FunctionUnused", "ImportUnused"):
                        continue
                    problems.append({"line": int(m.group(1)) - shift, "col": int(m.group(2)),
                                     "severity": "error" if kind.endswith("Error") else "warning", "message": f"{kind}: {m.group(4)}"})
        except Exception as e:
            problems.append({"line": 0, "severity": "warning", "message": f"type checker didn't run: {e}"})
    else:
        comp = kit_path("tools", "luau-compile.exe")
        if os.path.exists(comp):
            r = subprocess.run([comp, "--text", fpath], capture_output=True, text=True, timeout=60)
            for ln in (r.stdout + r.stderr).splitlines():
                m = re.match(r".*\((\d+),(\d+)\): (.*)$", ln)
                if m and "error" in ln.lower():
                    problems.append({"line": int(m.group(1)) - shift, "severity": "error", "message": m.group(3)})
        problems.append({"line": 0, "severity": "info", "message": "luau-lsp not found: only a syntax check ran (install the kit's tools)."})
    problems += rigel_traps(source, gamemode_code)
    errors = [p for p in problems if p["severity"] == "error"]
    return {"ok": not errors, "errors": len(errors), "problems": sorted(problems, key=lambda p: p.get("line", 0))}


# ────────────────────────────────────────────────────────────────────────────── game modes
def parse_mode(rec: str):
    f = rec.split("~")
    if len(f) < 14:
        return {"raw": rec}
    sets = {}
    for kv in f[6].split(";"):
        if "=" in kv:
            k, v = kv.split("=", 1)
            sets[k] = v
    n = int(f[3] or 0)
    names, maxes, sizes, scores = (f[4].split(","), f[5].split(","), f[11].split(","), f[12].split(","))
    return {"id": f[0], "name": f[1], "center": [float(x) for x in f[2].split(",")], "state": f[7], "level": f[8],
            "round": int(f[9] or 0), "timeLeft": int(f[10] or 0), "winner": int(f[13] or 0),
            "teams": [{"team": i + 1, "name": names[i] if i < len(names) else "", "max": int(maxes[i] or 0) if i < len(maxes) else 0,
                       "players": int(sizes[i] or 0) if i < len(sizes) else 0, "score": int(scores[i] or 0) if i < len(scores) else 0}
                      for i in range(n)],
            "rules": {k: v for k, v in sets.items() if not k.startswith(("role.", "custom.")) and k != "controller"},
            "custom": {k[7:]: v for k, v in sets.items() if k.startswith("custom.")},
            "roles": {k[5:]: v for k, v in sets.items() if k.startswith("role.")},
            "controller": sets.get("controller", ""), "halfSize": [3881, 4733, 950]}


def find_mode(mode: str):
    for r in BR.call("game_modes"):
        m = parse_mode(r)
        if m.get("id") == mode or m.get("name", "").lower() == mode.lower():
            return m
    raise EditorError(f"No game mode '{mode}' (gamemode_list shows them).")


def mode_code_path(name: str) -> str:
    return os.path.join(SCRIPTS, "GameModes", re.sub(r"[^\w-]", "_", name) + ".luau")


# ────────────────────────────────────────────────────────────────────────────── tools
TOOLS = []


def tool(name, desc, props=None, required=None):
    def deco(fn):
        TOOLS.append({"name": name, "description": desc, "inputSchema": {"type": "object", "properties": props or {},
                                                                          "required": required or []}, "fn": fn})
        return fn
    return deco


V3 = {"type": "array", "items": {"type": "number"}, "minItems": 3, "maxItems": 3}


def op_log(line: str):
    return BR.call("op", line)


# ---- look around
@tool("editor_status", "Where the editor is: world ready, editor mode on, camera position/rotation, the open level, counts, "
      "and the paths of its logs. Call this first.")
def t_status(a):
    return BR.call("status")


@tool("list_objects", "Placed/visible objects: handle (use it in every other tool), class, location, rotation, scale, owner lock. "
      "Filter by a substring of class/label/handle, and/or by distance from a point. Sorted by distance (from 'near' or the camera).",
      {"filter": {"type": "string"}, "near": V3, "radius": {"type": "number", "description": "cm, needs near"},
       "limit": {"type": "integer", "default": 60}})
def t_objects(a):
    near = a.get("near") or [0, 0, 0]
    return BR.call("objects", a.get("filter", ""), near[0], near[1], near[2], a.get("radius", "") if a.get("near") else "", a.get("limit", 60))


@tool("search_palette", "What can be placed: every placeable prefab (traps, buttons, timers, force fields, ball spawners, "
      "scoreboards, text signs, team changers, platforms ...). 'note' says what doesn't work when placed by the editor.",
      {"query": {"type": "string"}, "category": {"type": "string"}, "limit": {"type": "integer", "default": 60}})
def t_palette(a):
    return BR.call("palette", a.get("query", ""), a.get("category", ""), a.get("limit", 60))


# ---- build
# ---- the station's floor: a cylinder spinning about the world Y axis (x = 0, z = 0). "Up" anywhere on it points at the
# axis: +Z on the bottom floor, +X on the wall at x ~ -28600. (The server turns game mode areas and team doors the same way.)
def station_up(L):
    r = math.hypot(L[0], L[2])
    return (0.0, 0.0, 1.0) if r < 1000 else (-L[0] / r, 0.0, -L[2] / r)


def _q_from_rot(p, y, r):                 # FRotator -> FQuat (UE's own formula)
    p, y, r = (math.radians(v) * 0.5 for v in (p, y, r))
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    return (cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy)


def _q_mul(a, b):                         # a * b (apply b, then a)
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return (aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz)


def _rot_from_q(q):                       # FQuat -> FRotator (UE's FQuat::Rotator, singularities included)
    x, y, z, w = q
    st = z * x - w * y
    yaw = math.degrees(math.atan2(2 * (w * z + x * y), 1 - 2 * (y * y + z * z)))
    norm = lambda d: (d + 180.0) % 360.0 - 180.0
    if st < -0.4999995:
        return [-90.0, yaw, norm(-yaw - 2 * math.degrees(math.atan2(x, w)))]
    if st > 0.4999995:
        return [90.0, yaw, norm(yaw - 2 * math.degrees(math.atan2(x, w)))]
    return [math.degrees(math.asin(2 * st)), yaw, math.degrees(math.atan2(-2 * (w * x + y * z), 1 - 2 * (x * x + y * y)))]


def floor_rotation(L, rot):
    """`rot` as seen standing on the local floor at L -> the world rotation (identity tilt on the bottom floor)."""
    up = station_up(L)
    a = math.atan2(up[0], up[2])
    tilt = (0.0, math.sin(a / 2), 0.0, math.cos(a / 2))
    return [round(v, 3) for v in _rot_from_q(_q_mul(tilt, _q_from_rot(*(rot or [0, 0, 0]))))]


@tool("place_object", "Place a palette item at a world location (cm). Needs an open level (level_new / level_open). Returns the "
      "new object (with its handle) once it exists. The station's floor is the inside of a cylinder about the world Y axis: on "
      "the bottom floor up is +Z, on a wall it points at the axis (x ~ -28600: up is +X). By default the object is stood "
      "upright on the local floor and `rotation` is relative to that floor (yaw = turn about its up); align_to_floor=false "
      "takes `rotation` as a plain world rotation. Look at nearby objects for the floor's height.",
      {"item": {"type": "string", "description": "palette name (exact or substring) or sandbox id, e.g. BP_BasicButton_C"},
       "location": V3, "rotation": {**V3, "description": "pitch, yaw, roll (degrees)"}, "scale": V3,
       "align_to_floor": {"type": "boolean", "default": True}}, ["item", "location"])
def t_place(a):
    L = a["location"]
    r = a.get("rotation") or [0, 0, 0]
    if a.get("align_to_floor", True):
        r = floor_rotation(L, r)
    s = a.get("scale") or [1, 1, 1]
    return BR.call("place", a["item"], *L, *r, *s, timeout=20)


@tool("move_object", "Move / rotate / scale an object (anything left out stays as it is). Everyone sees it.",
      {"handle": {"type": "string"}, "location": V3, "rotation": V3, "scale": V3}, ["handle"])
def t_move(a):
    L, R, S = a.get("location"), a.get("rotation"), a.get("scale")
    args = [a["handle"]] + (L if L else ["", "", ""]) + (R if R else ["", "", ""]) + (S if S else ["", "", ""])
    return BR.call("transform", *args)


@tool("delete_object", "Delete an object (for everyone).", {"handle": {"type": "string"}}, ["handle"])
def t_delete(a):
    return BR.call("delete", a["handle"])


@tool("duplicate_object", "Copy an object (with its scripts and data) to a new location.",
      {"handle": {"type": "string"}, "location": V3}, ["handle"])
def t_dup(a):
    L = a.get("location") or ["", "", ""]
    return BR.call("duplicate", a["handle"], *L, timeout=20)


@tool("select_object", "Select an object in the editor UI (shows it in Details for the user).", {"handle": {"type": "string"}}, ["handle"])
def t_select(a):
    return BR.call("select", a["handle"])


@tool("set_camera", "Fly the editor camera to a point (optionally pitch/yaw), or frame an object by handle.",
      {"location": V3, "pitch": {"type": "number"}, "yaw": {"type": "number"}, "handle": {"type": "string"}})
def t_camera(a):
    if a.get("handle"):
        return BR.call("focus_object", a["handle"])
    L = a.get("location")
    if not L:
        raise EditorError("give location or handle")
    return BR.call("camera", *L, *( [a.get("pitch", 0), a.get("yaw", 0)] if ("pitch" in a or "yaw" in a) else []))


@tool("undo", "Undo the last edit (like Ctrl+Z).")
def t_undo(a):
    return op_log("undo")


@tool("redo", "Redo (like Ctrl+Y).")
def t_redo(a):
    return op_log("redo")


# ---- inspect / configure
@tool("get_properties", "An object's editable properties (Details panel): path, value (wire encoding), writable, replicated. "
      "sub_path steps into a component/child (paths marked subObject).", {"handle": {"type": "string"}, "sub_path": {"type": "string"}}, ["handle"])
def t_props(a):
    return BR.call("props", a["handle"], a.get("sub_path", ""), timeout=10)


@tool("set_property", "Set one property (value in the same wire encoding get_properties shows: numbers, true/false, "
      "'x,y,z' vectors, 'p,y,r' rotators, 'r,g,b,a' colours, text).",
      {"handle": {"type": "string"}, "path": {"type": "string"}, "value": {"type": "string"}}, ["handle", "path", "value"])
def t_setprop(a):
    return BR.call("set_prop", a["handle"], a["path"], a["value"])


@tool("get_game_data", "A sandbox object's synced Game data (serverData / gameData / Properties): what every player's copy uses "
      "(isEnabled, TeamIndex, Text, durations ...).", {"handle": {"type": "string"}}, ["handle"])
def t_data(a):
    return BR.call("data", a["handle"], timeout=12)


@tool("set_game_data", "Write one Game data value. path like 'sd/TeamIndex', 'gd/isEnabled', 'props/Text'; kind bool|num|str|text.",
      {"handle": {"type": "string"}, "path": {"type": "string"}, "kind": {"type": "string", "enum": ["bool", "num", "str", "text"]},
       "value": {"type": "string"}}, ["handle", "path", "kind", "value"])
def t_dataset(a):
    return BR.call("data_set", a["handle"], a["path"], a["kind"], a["value"])


@tool("set_text", "Set a Text object's text. {name} templates are filled live from scripts (Rigel.setText(name, value) or "
      "Gamemode:broadcastEventString(name, value)) and from game modes ({Mode.state}, {Mode.team1.score} ...).",
      {"handle": {"type": "string"}, "text": {"type": "string"}}, ["handle", "text"])
def t_text(a):
    return BR.call("set_text", a["handle"], a["text"])


# ---- Luau
@tool("luau_api_search", "Search the game's Luau API (every class, method, event, property, global and the Rigel.* library).",
      {"query": {"type": "string"}, "limit": {"type": "integer", "default": 40}}, ["query"])
def t_apisearch(a):
    return api_search(a["query"], a.get("limit", 40))


@tool("luau_api_class", "The full declaration of one class or global (methods with signatures, events, properties), e.g. "
      "BasicButtonComponent, ToggleableComponent, BallSpawnerComponent, DiscEntity, VRPawn, Gamemode, LuauClock, Rigel, Vector.",
      {"name": {"type": "string"}}, ["name"])
def t_apiclass(a):
    return api_class(a["name"])


@tool("luau_guide", "The scripting guides as sections: how scripts, properties, references, events, timers, text, game modes "
      "and this MCP work. No topic = the list of sections; topic = sections whose title or text matches.",
      {"topic": {"type": "string"}})
def t_guide(a):
    secs = guide_sections()
    t = (a.get("topic") or "").lower()
    if not t:
        return [s[0] for s in secs]
    hits = [f"## {title}\n{body}" for title, body in secs if t in title.lower()] or \
           [f"## {title}\n{body}" for title, body in secs if t in body.lower()][:4]
    return "\n\n".join(hits) or "nothing matches; call luau_guide with no topic for the list"


@tool("luau_examples", "Working example scripts (examples/*.luau, examples/gamemodes/*.luau) and the station's own game scripts "
      "(game-scripts/). No name = the list; name = the source.", {"name": {"type": "string"}})
def t_examples(a):
    files = sorted(glob.glob(kit_path("examples") + "/*.luau") + glob.glob(kit_path("examples", "gamemodes") + "/*.luau") +
                   glob.glob(os.path.join(KIT, "game-scripts", "*.luau")))
    if not a.get("name"):
        return [os.path.relpath(f, KIT) for f in files]
    for f in files:
        if a["name"].lower() in os.path.basename(f).lower():
            with open(f, encoding="utf-8") as fh:
                return {"file": os.path.relpath(f, KIT), "source": fh.read()}
    return "no such example"


@tool("check_luau", "Type-check a script against the game's API (luau-lsp + the Rigel definitions) and the game's own traps "
      "(typed locals that crash, client-side ball spawns, endless loops ...). ALWAYS run before attach_script. "
      "gamemode_code=true for a game mode's own code (GameModes/<mode>.luau).",
      {"source": {"type": "string"}, "name": {"type": "string"}, "gamemode_code": {"type": "boolean"}}, ["source"])
def t_check(a):
    return check_luau(a["source"], a.get("name", "Script"), bool(a.get("gamemode_code")))


@tool("write_script_file", "Save a script to Documents\\RigelScripts\\<name>.luau (the user sees it in the Scripts panel / VS Code). "
      "If objects already run a script of that name, save_and_update=true also sends the new source to all of them.",
      {"name": {"type": "string"}, "source": {"type": "string"}, "save_and_update": {"type": "boolean"}}, ["name", "source"])
def t_writefile(a):
    name = re.sub(r"\.luau$", "", a["name"])
    os.makedirs(SCRIPTS, exist_ok=True)
    p = os.path.join(SCRIPTS, name + ".luau")
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(a["source"])
    res = {"file": p}
    if a.get("save_and_update"):
        chk = check_luau(a["source"], name)
        if not chk["ok"]:
            return {**res, "updated": False, "check": chk}
        BR.call("luau_update", name + ".luau", a["source"])
        res["updated"] = True
    return res


@tool("read_script_file", "Read a script from Documents\\RigelScripts (name without .luau; 'GameModes/<mode>' for mode code).",
      {"name": {"type": "string"}}, ["name"])
def t_readfile(a):
    p = os.path.join(SCRIPTS, re.sub(r"\.luau$", "", a["name"]) + ".luau")
    if not os.path.exists(p):
        return f"no file {p}"
    with open(p, encoding="utf-8") as fh:
        return fh.read()


@tool("attach_script", "Attach Luau to an object (checked first; refused if the check finds errors unless force=true). The object "
      "is rebuilt with the script on every machine. Waits a few seconds and returns what the game logged (script log()/warn()/"
      "errors, server notes) so you can see it run -- or see why not and fix it.",
      {"handle": {"type": "string"}, "name": {"type": "string", "description": "script name, e.g. DoorLogic"},
       "source": {"type": "string"}, "force": {"type": "boolean"}, "wait_seconds": {"type": "number", "default": 4}},
      ["handle", "name", "source"])
def t_attach(a):
    name = re.sub(r"\.luau$", "", a["name"]) + ".luau"
    chk = check_luau(a["source"], name)
    if not chk["ok"] and not a.get("force"):
        return {"attached": False, "reason": "the check found errors -- fix them (or force=true)", "check": chk}
    mark, seq = log_mark(), BR.call("status").get("logSeq", 0)
    BR.call("luau_attach", a["handle"], name, a["source"])
    time.sleep(float(a.get("wait_seconds", 4)))
    # the editor's periodic "[repl]" object dumps are noise here (hundreds of lines of the station's objects)
    elog = [l for l in BR.call("logs", seq, "", 200)["lines"] if not l.startswith("[repl]")][:80]
    return {"attached": True, "check": chk, "gameLog": read_game_log(mark, "", False, 120)["lines"],
            "editorLog": elog, "problems": BR.call("problems")}


@tool("update_script", "Send new source to every object running this script (by name).",
      {"name": {"type": "string"}, "source": {"type": "string"}, "force": {"type": "boolean"}}, ["name", "source"])
def t_update(a):
    name = re.sub(r"\.luau$", "", a["name"]) + ".luau"
    chk = check_luau(a["source"], name)
    if not chk["ok"] and not a.get("force"):
        return {"updated": False, "check": chk}
    mark = log_mark()
    BR.call("luau_update", name, a["source"])
    time.sleep(3)
    return {"updated": True, "check": chk, "gameLog": read_game_log(mark)["lines"]}


@tool("remove_script", "Take a script off an object.", {"handle": {"type": "string"}, "name": {"type": "string"}}, ["handle", "name"])
def t_remove(a):
    return BR.call("luau_remove", a["handle"], re.sub(r"\.luau$", "", a["name"]) + ".luau")


@tool("set_script_reference", "Wire a script's typed slot (a top-level 'local Door: ToggleableComponent = nil') to another object's "
      "component. type = the Luau component type; target = the target object's handle ('' clears it). find_reference_targets "
      "lists objects that have a given component.",
      {"handle": {"type": "string", "description": "the object running the script"}, "script": {"type": "string"},
       "slot": {"type": "string"}, "type": {"type": "string"}, "target": {"type": "string"}}, ["handle", "script", "slot", "type", "target"])
def t_ref(a):
    return BR.call("luau_ref", a["handle"], re.sub(r"\.luau$", "", a["script"]) + ".luau", a["slot"], a["type"], a["target"])


@tool("find_reference_targets", "Objects that have a given Luau component type (e.g. BasicButtonComponent) -- valid slot targets. "
      "Each has handle, component, and for placed objects class + location. The station has many (golf holes, arena parts): "
      "pass near (+ radius) to get the ones around a spot, nearest first.",
      {"type": {"type": "string"}, "near": V3, "radius": {"type": "number", "description": "cm, with near (default 3000)"},
       "limit": {"type": "integer", "default": 40}}, ["type"])
def t_slotcands(a):
    cands = BR.call("slot_candidates", a["type"], timeout=8)
    if isinstance(cands, list) and a.get("near"):
        n, rad = a["near"], float(a.get("radius", 3000))

        def dist(c):
            L = c.get("location")
            return ((L[0] - n[0]) ** 2 + (L[1] - n[1]) ** 2 + (L[2] - n[2]) ** 2) ** 0.5 if L else 1e18
        cands = sorted((dict(c, distance=round(dist(c))) for c in cands if dist(c) <= rad), key=lambda c: c["distance"])
    return cands[:int(a.get("limit", 40))] if isinstance(cands, list) else cands


@tool("script_logs", "Recent Luau output from the editor's game log: log()/warn(), runtime errors ([RigelError] ...), compile errors. "
      "Pass 'mark' from a previous call to get only newer lines. script = only lines from that script.",
      {"mark": {"type": "integer"}, "script": {"type": "string"}, "errors_only": {"type": "boolean"}, "max_lines": {"type": "integer"}})
def t_scriptlogs(a):
    return read_game_log(a.get("mark"), a.get("script", ""), bool(a.get("errors_only")), a.get("max_lines", 200))


@tool("station_scripts", "The station's own Luau scripts (the game's gamemodes -- real API usage). No name = list; name = source.",
      {"name": {"type": "string"}})
def t_station(a):
    return BR.call("game_scripts", a.get("name", ""), timeout=15)


# ---- levels
@tool("level_list", "Levels: the local files (Documents\\RigelLevels\\*.a2level) and the ones saved on this server.")
def t_levels(a):
    files = [os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(LEVELS, "*.a2level"))]
    try:
        server = BR.call("levels_server")
    except Exception as e:
        server = str(e)
    st = BR.call("status")
    return {"open": st.get("openLevel"), "dirty": st.get("levelDirty"), "local": files, "server": server}


def ui(line: str):
    return BR.call("ui", line, timeout=10)


@tool("level_new", "Start a new, empty level with this name and open it (placing needs an open level). The level that was "
      "open is closed first (its objects leave this server -- save it before if it has changes).",
      {"name": {"type": "string"}}, ["name"])
def t_lvnew(a):
    name = re.sub(r"[^\w -]", "", a["name"]).strip() or "Untitled"
    closed = ui("scene close")
    made = ui("scene saveas " + name)
    st = BR.call("status")
    return {"open": st.get("openLevel"), "log": (closed.get("log", []) if isinstance(closed, dict) else []) +
            (made.get("log", []) if isinstance(made, dict) else [])}


@tool("level_open", "Open a saved level by name (loads its objects, game modes and scripts).", {"name": {"type": "string"}}, ["name"])
def t_lvopen(a):
    return ui("scene open " + a["name"])


@tool("level_save", "Save the open level (to Documents\\RigelLevels). name = save as a new name.", {"name": {"type": "string"}})
def t_lvsave(a):
    return ui("scene saveas " + a["name"]) if a.get("name") else ui("scene save")


@tool("level_close", "Close the open level (its objects leave this server).")
def t_lvclose(a):
    return ui("scene close")


@tool("level_delete", "Delete a local level file (to the Recycle Bin).", {"name": {"type": "string"}}, ["name"])
def t_lvdel(a):
    return ui("scene delete " + a["name"])


@tool("level_upload", "Upload the open level to the server's saved levels.")
def t_lvup(a):
    return ui("scene upload")


# ---- game modes
@tool("gamemode_list", "Game modes on this server: id, name, centre, state, round, time, teams (players/score/max), rules, custom "
      "settings, roles (object id -> role), the controller object. A mode's area is centre +/- halfSize (cm).")
def t_gmlist(a):
    return [parse_mode(r) for r in BR.call("game_modes")]


@tool("gamemode_create", "Make a game mode: an area (~78 x 95 x 19 m) with its own teams, rounds and scores, centred on 'center'. "
      "Team changers, traps, buttons ... placed inside belong to it.",
      {"name": {"type": "string"}, "center": V3, "teams": {"type": "integer", "minimum": 1, "maximum": 4},
       "team_names": {"type": "array", "items": {"type": "string"}}, "max_players": {"type": "array", "items": {"type": "integer"}}},
      ["name", "center"])
def t_gmnew(a):
    n = int(a.get("teams", 2))
    names = (a.get("team_names") or ["Blue", "Red", "Green", "Yellow"])[:n]
    maxes = (a.get("max_players") or [4] * n)[:n]
    c = a["center"]
    BR.call("raw", f"SE|GMNEW|{a['name']}|{c[0]:.0f},{c[1]:.0f},{c[2]:.0f}|{n}|{','.join(map(str, maxes))}|{','.join(names)}")
    for _ in range(40):
        time.sleep(0.5)
        try:
            return find_mode(a["name"])
        except EditorError:
            pass
    return {"sent": True, "note": "not listed yet -- check editor logs / problems (overlapping another mode? no level open?)"}


RULES = {"start_mode": "manual | button | auto", "min_players": "per team, for auto start", "countdown": "seconds",
         "round_time": "seconds, 0 = no limit", "score_to_win": "0 = none", "end_delay": "seconds the result shows",
         "auto_restart": "0/1", "stop_when_empty": "0/1"}


@tool("gamemode_set", "Change a game mode: key = name | teamN.name | teamN.max | a rule (" +
      ", ".join(f"{k}: {v}" for k, v in RULES.items()) + ") | custom.<key> (scripts read Rigel.setting('<key>')).",
      {"mode": {"type": "string"}, "key": {"type": "string"}, "value": {"type": "string"}}, ["mode", "key", "value"])
def t_gmset(a):
    m = find_mode(a["mode"])
    return BR.call("raw", f"SE|GMSET|{m['id']}|{a['key']}|{a['value']}")


@tool("gamemode_control", "Start / end / reset a mode's round. action: start (with countdown) | startnow | end | reset.",
      {"mode": {"type": "string"}, "action": {"type": "string", "enum": ["start", "startnow", "end", "reset"]}}, ["mode", "action"])
def t_gmctl(a):
    m = find_mode(a["mode"])
    return BR.call("raw", f"SE|GMCTL|{m['id']}|{a['action']}")


@tool("gamemode_delete", "Delete a game mode and every object in it.", {"mode": {"type": "string"}}, ["mode"])
def t_gmdel(a):
    m = find_mode(a["mode"])
    return BR.call("raw", f"SE|GMDEL|{m['id']}")


@tool("gamemode_team_changer", "Place a team changer for team N (1-based) inside the mode. Walking into it puts a player on that team. "
      "(If a player stands on the spot it's built beside it and moves in when they step off -- a changer spawned on a player "
      "would crash them.) It's a door with two sides: walking through one way JOINS the team, walking back out LEAVES it. "
      "The server stands it on the local floor under `location` (walls included; its middle is 1.5 m up) and, with no "
      "yaw, turns it to face the mode's centre -- walk in to join, walk out to leave. Give yaw only to override that.",
      {"mode": {"type": "string"}, "team": {"type": "integer"}, "location": V3,
       "yaw": {"type": "number", "description": "optional: degrees about the local up; walking along it joins (default: face the centre)"}},
      ["mode", "team", "location"])
def t_gmteam(a):
    m = find_mode(a["mode"])
    L = a["location"]
    yaw = f"|{float(a['yaw']):.0f}" if a.get("yaw") is not None else ""
    return BR.call("raw", f"SE|GMTEAM|{m['id']}|{a['team']}|{L[0]:.0f},{L[1]:.0f},{L[2]:.0f}{yaw}")


ROLES = {"start": "BasicButton: starts a round", "score:N": "BasicButton: +1 for team N while running",
         "trap_round": "Toggleable trap: on during rounds", "trap_pulse:S": "trap: flips every S seconds during rounds",
         "trap_fired": "trap: switched on by trap buttons", "trap_button:S": "BasicButton: fires 'trap_fired' traps for S s",
         "wall_lobby": "solid between rounds, gone during", "wall_round": "only there during rounds",
         "timer": "Timer: counts countdown / round", "ball": "BallSpawner: the SERVER spawns/resets its ball each round",
         "score_board": "BP_Score_C: shows team 1/2 points", "score_table": "LE_BP_TableScoreboard_C: a row per team", "": "(clear)"}


@tool("gamemode_set_role", "Give an object inside a mode a role; the generated controller script wires it (slot names Start1, "
      "Score1, TrapRound1, Ball1, ... usable from the mode's code). Roles: " + "; ".join(f"{k} = {v}" for k, v in ROLES.items()) +
      ". Call gamemode_apply_script afterwards.", {"mode": {"type": "string"}, "handle": {"type": "string"}, "role": {"type": "string"}},
      ["mode", "handle", "role"])
def t_gmrole(a):
    m = find_mode(a["mode"])
    guid = a["handle"].split("_")[0]
    return BR.call("raw", f"SE|GMSET|{m['id']}|role.{guid}|{a['role']}")


@tool("gamemode_get_code", "The mode's own code (Documents\\RigelScripts\\GameModes\\<mode>.luau): hooks OnLobby, OnCountdown(s), "
      "OnRoundStart(round), OnRoundEnd(winner), OnTeamChanged(team,size,old), OnScore(team,score,old), OnTime(secondsLeft).",
      {"mode": {"type": "string"}}, ["mode"])
def t_gmgetcode(a):
    m = find_mode(a["mode"])
    p = mode_code_path(m["name"])
    if not os.path.exists(p):
        return {"file": p, "source": "", "note": "no code yet -- gamemode_set_code writes it"}
    with open(p, encoding="utf-8") as fh:
        return {"file": p, "source": fh.read()}


@tool("gamemode_set_code", "Write the mode's own code (checked first), then apply the mode script (roles + your code -> the controller "
      "on every machine). Don't write BeginPlay in mode code; use the hooks. Returns the check and what the game logged.",
      {"mode": {"type": "string"}, "source": {"type": "string"}, "force": {"type": "boolean"}}, ["mode", "source"])
def t_gmsetcode(a):
    m = find_mode(a["mode"])
    chk = check_luau(a["source"], "GM_" + m["name"], gamemode_code=True)
    if not chk["ok"] and not a.get("force"):
        return {"written": False, "check": chk}
    p = mode_code_path(m["name"])
    os.makedirs(os.path.dirname(p), exist_ok=True)
    with open(p, "w", encoding="utf-8") as fh:
        fh.write(a["source"])
    mark = log_mark()
    applied = ui("gmapply " + m["name"])
    time.sleep(4)
    return {"written": True, "file": p, "check": chk, "apply": applied, "gameLog": read_game_log(mark)["lines"]}


@tool("gamemode_apply_script", "Rebuild and send a mode's controller script (after changing roles or code). Returns the generated "
      "script (in the log) so you can see the slot names.", {"mode": {"type": "string"}}, ["mode"])
def t_gmapply(a):
    m = find_mode(a["mode"])
    return ui("gmapply " + m["name"])


@tool("screenshot", "What the editor shows right now (the game view plus the editor UI), as an image you can look at. "
      "Use set_camera first to frame what you want to check. max_width shrinks it (default 1280).",
      {"max_width": {"type": "integer", "default": 1280}})
def t_screenshot(a):
    shots = os.path.join(tempfile.gettempdir(), "rigel_shots")
    os.makedirs(shots, exist_ok=True)
    bmp = os.path.join(shots, f"mcp_{int(time.time() * 1000)}.bmp")
    BR.call("op", "shot " + bmp)
    for _ in range(40):
        time.sleep(0.25)
        if os.path.exists(bmp) and os.path.getsize(bmp) > 1000:
            time.sleep(0.2)
            break
    else:
        raise EditorError("no frame came back (is the editor UI visible? F12 toggles it)")
    png = bmp[:-4] + ".png"
    try:
        from PIL import Image
        im = Image.open(bmp)
        mw = int(a.get("max_width", 1280))
        if im.width > mw:
            im = im.resize((mw, int(im.height * mw / im.width)))
        im.save(png)
        os.remove(bmp)
    except ImportError:
        png = bmp                                   # no Pillow: hand back the BMP
    return {"__image__": png}


# ---- anything else
@tool("editor_op", "Run one editor op (the same language as the editor's test scripts) -- for things without a dedicated tool: "
      "quests (quest/qstep/qpub ...), coin runs, triggers, probing (comps, where, slots ...). Returns what it logged. "
      "See the MCP guide's 'Editor ops' section.", {"line": {"type": "string"}}, ["line"])
def t_op(a):
    return op_log(a["line"])


@tool("editor_logs", "The editor's own log lines (what it did, server notes, errors). 'since' = a logSeq from editor_status.",
      {"since": {"type": "integer"}, "filter": {"type": "string"}, "max": {"type": "integer"}})
def t_elogs(a):
    r = BR.call("logs", a.get("since", 0), a.get("filter", ""), 600)
    # the editor's periodic "[repl]" dumps (hundreds of lines of the station's own objects) are noise unless asked for
    if isinstance(r, dict) and not (a.get("filter") or "").startswith("[repl"):
        r["lines"] = [l for l in r.get("lines", []) if not str(l).startswith("[repl]")]
    if isinstance(r, dict):
        r["lines"] = r.get("lines", [])[-int(a.get("max", 200)):]
    return r


@tool("editor_problems", "Problems the editor is showing the user (server refusals, script errors).")
def t_problems(a):
    return BR.call("problems")


@tool("wait", "Wait some seconds (let a round run, a script tick, players move).", {"seconds": {"type": "number"}}, ["seconds"])
def t_wait(a):
    time.sleep(min(float(a["seconds"]), 120))
    return {"waited": a["seconds"]}


# ────────────────────────────────────────────────────────────────────────────── MCP (JSON-RPC over stdio)
def resources():
    out = []
    for title, parts in GUIDE_FILES:
        p = kit_path(*parts)
        if os.path.exists(p):
            out.append({"uri": "rigel://" + "/".join(parts), "name": title, "mimeType": "text/markdown", "path": p})
    d = kit_path("types", "rigel.d.luau")
    if os.path.exists(d):
        out.append({"uri": "rigel://types/rigel.d.luau", "name": "Luau API definitions", "mimeType": "text/plain", "path": d})
    return out


def handle(msg):
    method, mid = msg.get("method"), msg.get("id")
    p = msg.get("params") or {}
    if method == "initialize":
        return {"protocolVersion": p.get("protocolVersion", "2024-11-05"),
                "capabilities": {"tools": {}, "resources": {}},
                "serverInfo": {"name": "rigel-spec-editor", "version": "1.0"},
                "instructions": "Drive the Rigel Spec Editor (Orion Drift). Start with editor_status. Before writing Luau read "
                                "luau_guide('MCP') and check every script with check_luau; after attaching read script_logs and fix "
                                "errors. Placing needs an open level (level_new/level_open)."}
    if method == "tools/list":
        return {"tools": [{k: v for k, v in t.items() if k != "fn"} for t in TOOLS]}
    if method == "tools/call":
        t = next((t for t in TOOLS if t["name"] == p.get("name")), None)
        if not t:
            return {"content": [{"type": "text", "text": f"unknown tool {p.get('name')}"}], "isError": True}
        try:
            res = t["fn"](p.get("arguments") or {})
            if isinstance(res, dict) and "__image__" in res:
                import base64
                path = res["__image__"]
                with open(path, "rb") as fh:
                    data = base64.b64encode(fh.read()).decode("ascii")
                mime = "image/png" if path.endswith(".png") else "image/bmp"
                return {"content": [{"type": "image", "data": data, "mimeType": mime}, {"type": "text", "text": path}]}
            text = res if isinstance(res, str) else json.dumps(res, indent=1, ensure_ascii=False)
            return {"content": [{"type": "text", "text": text}]}
        except Exception as e:
            return {"content": [{"type": "text", "text": f"{type(e).__name__}: {e}"}], "isError": True}
    if method == "resources/list":
        return {"resources": [{k: v for k, v in r.items() if k != "path"} for r in resources()]}
    if method == "resources/read":
        r = next((r for r in resources() if r["uri"] == p.get("uri")), None)
        if not r:
            raise ValueError("no such resource")
        with open(r["path"], encoding="utf-8") as fh:
            return {"contents": [{"uri": r["uri"], "mimeType": r["mimeType"], "text": fh.read()}]}
    if method == "ping":
        return {}
    if mid is None:
        return None                                   # a notification
    raise NotImplementedError(method)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "--selftest":
        print(json.dumps(BR.call("status"), indent=1))
        return
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            msg = json.loads(line)
        except json.JSONDecodeError:
            continue
        try:
            res = handle(msg)
            if msg.get("id") is not None:
                out = {"jsonrpc": "2.0", "id": msg["id"], "result": res if res is not None else {}}
            else:
                continue
        except Exception as e:
            if msg.get("id") is None:
                continue
            out = {"jsonrpc": "2.0", "id": msg["id"], "error": {"code": -32601 if isinstance(e, NotImplementedError) else -32603,
                                                               "message": str(e)}}
        sys.stdout.write(json.dumps(out, ensure_ascii=False) + "\n")
        sys.stdout.flush()


if __name__ == "__main__":
    main()
