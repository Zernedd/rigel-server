#!/usr/bin/env python3
"""
build_guide.py -- builds SpecEditor/luau/Rigel-Luau-Guide.pdf

Writes an HTML version of the guide (tools/guide.html) and prints it to PDF with the
Microsoft Edge that ships with Windows (headless --print-to-pdf). Run generate_defs.py
first so types/api_index.json is current.

    python tools/build_guide.py
"""
from __future__ import annotations

import html
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
INDEX = os.path.join(ROOT, "types", "api_index.json")
EXAMPLES = os.path.join(ROOT, "examples")
GAME = os.path.join(ROOT, "game-scripts")
OUT_HTML = os.path.join(HERE, "guide.html")
OUT_PDF = os.path.join(ROOT, "Rigel-Luau-Guide.pdf")

KEYWORDS = r"\b(and|break|do|else|elseif|end|false|for|function|if|in|local|nil|not|or|repeat|return|then|true|until|while|continue|type|export)\b"


def hl(code: str) -> str:
    """Tiny Luau highlighter: comments, strings, keywords, numbers."""
    out = []
    token_re = re.compile(r"(--[^\n]*)|(\"(?:\\.|[^\"\\])*\"|'(?:\\.|[^'\\])*'|`(?:\\.|[^`\\])*`)|" + KEYWORDS +
                          r"|(\b\d+(?:\.\d+)?\b)")
    pos = 0
    for m in token_re.finditer(code):
        out.append(html.escape(code[pos:m.start()]))
        t = html.escape(m.group(0))
        if m.group(1):
            out.append(f'<span class="c">{t}</span>')
        elif m.group(2):
            out.append(f'<span class="s">{t}</span>')
        elif m.group(3):
            out.append(f'<span class="k">{t}</span>')
        else:
            out.append(f'<span class="n">{t}</span>')
        pos = m.end()
    out.append(html.escape(code[pos:]))
    return "".join(out)


def code(src: str, title: str | None = None) -> str:
    cap = f'<div class="cap">{html.escape(title)}</div>' if title else ""
    return f'<figure class="code">{cap}<pre>{hl(src.strip(chr(10)))}</pre></figure>'


def example(name: str) -> str:
    with open(os.path.join(EXAMPLES, name), encoding="utf-8") as fh:
        return code(fh.read(), f"examples/{name}")


def game_lines(fname: str, first: int, last: int) -> str:
    with open(os.path.join(GAME, fname), encoding="utf-8") as fh:
        lines = fh.read().splitlines()
    return code("\n".join(lines[first - 1:last]), f"game-scripts/{fname}, lines {first}-{last}")


def c(t: str) -> str:
    return f"<code>{html.escape(t)}</code>"


# --------------------------------------------------------------------------------------------
def appendix(index: dict) -> str:
    rows = []
    skip = re.compile(r"^(onHit|overlapBegin|overlapEnd|onGoalBeginOverlap|onComponent|handle[A-Z]|"
                      r"multicast|server|client|Server|Client|Multicast|bndEvt|receive|tick|debug|test|run[A-Z]\w*Tests)")
    for name, e in index["classes"].items():
        if name in ("Quests", "LuauClock"):
            continue
        methods = [m for m in e["methods"] if not skip.match(m["name"]) and m["name"][:1].islower()]
        total_methods = len([m for m in e["methods"] if m["name"][:1].islower()])
        shown = methods[:6]
        evs = e["events"][:5]
        props = []
        mhtml = "".join(f"<li>{c(':' + m['sig'].replace('(self, ', '(').replace('(self)', '()'))}</li>" for m in shown)
        more = total_methods - len(shown)
        if more > 0:
            mhtml += f'<li class="more">+ {more} more in rigel.d.luau</li>'
        ehtml = ", ".join(c("." + ev["name"]) for ev in evs) or "<em>none</em>"
        if len(e["events"]) > len(evs):
            ehtml += f' <span class="more">+{len(e["events"]) - len(evs)} more</span>'
        phtml = ", ".join(c(p["name"]) for p in props)
        parent = f' <span class="par">extends {html.escape(e["parent"])}</span>' if e.get("parent") else ""
        origin = html.escape(e["cpp"])
        rows.append(f'''<div class="api">
  <h4>{html.escape(name)}{parent}</h4>
  <div class="origin">C++ {origin} &middot; {html.escape(e["package"])} &middot; {html.escape(e["why"])}</div>
  <ul class="methods">{mhtml or '<li class="more">no methods of its own</li>'}</ul>
  <div class="evs"><b>Events:</b> {ehtml}</div>
  {f'<div class="evs"><b>Some properties:</b> {phtml}</div>' if phtml else ''}
</div>''')
    return '<div class="apigrid">' + "\n".join(rows) + "</div>"


def globals_table(index: dict) -> str:
    rows = []
    for g, members in index["globals"].items():
        items = "<br>".join(c(m) for m in members[:12])
        rows.append(f"<tr><td>{c(g)}</td><td>{items}</td></tr>")
    return "<table class='t'><tr><th>Global</th><th>Members</th></tr>" + "".join(rows) + "</table>"


# --------------------------------------------------------------------------------------------
CSS = r"""
@page { size: A4; margin: 18mm 17mm 20mm 17mm;
  @bottom-center { content: counter(page); font: 9pt 'Segoe UI', sans-serif; color: #888; } }
@page :first { @bottom-center { content: none; } }
:root { --ink:#1d2330; --muted:#5a6475; --accent:#3a5bd9; --accent2:#0d8f7a; --bg:#f4f6fb; --line:#dde2ee; }
* { box-sizing: border-box; }
body { font: 10pt/1.48 'Segoe UI', 'Helvetica Neue', Arial, sans-serif; color: var(--ink); margin: 0; }
h1, h2, h3, h4 { font-family: 'Segoe UI Semibold', 'Segoe UI', sans-serif; line-height: 1.2; }
h2 { font-size: 19pt; margin: 0 0 10px; padding-bottom: 6px; border-bottom: 2px solid var(--accent); break-after: avoid; }
h2 .num { color: var(--accent); margin-right: 8px; }
h3 { font-size: 12.5pt; margin: 18px 0 6px; color: #23304f; break-after: avoid; }
h4 { font-size: 10pt; margin: 0 0 1px; }
section.chapter { margin-top: 26px; }
section.chapter.newpage { break-before: page; margin-top: 0; }
p { margin: 6px 0 8px; }
code { font: 9pt 'Cascadia Mono', Consolas, monospace; background: #eef1f8; padding: 0 3px; border-radius: 3px; }
figure.code { margin: 8px 0 12px; border: 1px solid var(--line); border-radius: 6px; background: #fbfcfe; break-inside: avoid; }
figure.code .cap { font: 8.5pt 'Segoe UI', sans-serif; color: var(--muted); padding: 4px 10px; border-bottom: 1px solid var(--line); background: var(--bg); border-radius: 6px 6px 0 0; }
figure.code pre { margin: 0; padding: 6px 10px; font: 8.2pt/1.36 'Cascadia Mono', Consolas, monospace; white-space: pre-wrap; word-break: break-word; tab-size: 4; }
figure.code pre code { background: none; padding: 0; }
.k { color: #7a2bb8; font-weight: 600; } .s { color: #a3451b; } .c { color: #6b7a8c; font-style: italic; } .n { color: #0d7a68; }
.note, .warn, .tip { border-left: 4px solid var(--accent); background: #eef2fd; padding: 8px 12px; margin: 10px 0; border-radius: 0 6px 6px 0; break-inside: avoid; }
.warn { border-color: #d9822b; background: #fdf4ea; }
.tip { border-color: var(--accent2); background: #e9f7f4; }
.note b:first-child, .warn b:first-child, .tip b:first-child { display: block; margin-bottom: 2px; }
ol.steps { padding-left: 0; counter-reset: s; list-style: none; }
ol.steps > li { counter-increment: s; position: relative; padding: 4px 0 6px 36px; }
ol.steps > li::before { content: counter(s); position: absolute; left: 0; top: 3px; width: 24px; height: 24px; border-radius: 50%; background: var(--accent); color: white; font-weight: 600; text-align: center; line-height: 24px; font-size: 10pt; }
table.t { border-collapse: collapse; width: 100%; margin: 8px 0 12px; font-size: 9.4pt; break-inside: auto; }
table.t th, table.t td { border: 1px solid var(--line); padding: 5px 7px; vertical-align: top; text-align: left; }
table.t th { background: var(--bg); }
table.t tr { break-inside: avoid; }
.cover { height: 250mm; display: flex; flex-direction: column; justify-content: space-between; }
.cover .band { background: linear-gradient(135deg, #1f2d5c, #3a5bd9 60%, #0d8f7a); color: white; padding: 38mm 14mm 16mm; border-radius: 10px; }
.cover h1 { font-size: 34pt; margin: 0 0 6px; }
.cover .sub { font-size: 14pt; opacity: .92; }
.cover .meta { color: var(--muted); font-size: 10pt; }
.toc { columns: 2; column-gap: 24px; font-size: 10.5pt; }
.toc div { break-inside: avoid; padding: 2px 0; }
.toc .n { display: inline-block; width: 26px; color: var(--accent); font-weight: 600; font-style: normal; }
.apigrid { columns: 2; column-gap: 12px; }
.api { border: 1px solid var(--line); border-radius: 6px; padding: 5px 8px 4px; margin: 0 0 7px; break-inside: avoid; display: inline-block; width: 100%; }
.api .origin { font-size: 7.3pt; color: var(--muted); margin-bottom: 3px; }
.api .par { font-weight: 400; font-size: 9pt; color: var(--muted); }
.api ul.methods { margin: 1px 0 2px; padding-left: 14px; font-size: 8pt; line-height: 1.3; }
.api ul.methods li { margin: 0; }
.api ul.methods code { font-size: 7.3pt; background: none; padding: 0; }
.api .evs { font-size: 7.8pt; margin-top: 1px; line-height: 1.3; } .api .evs code { font-size: 7.2pt; background: none; padding: 0; }
.more { color: var(--muted); font-style: italic; }
.two { display: grid; grid-template-columns: 1fr 1fr; gap: 12px; }
.linebyline { width: 100%; border-collapse: collapse; font-size: 9.4pt; margin: 6px 0 12px; }
.linebyline td { border-bottom: 1px solid var(--line); padding: 4px 6px; vertical-align: top; }
.linebyline td:first-child { width: 42%; }
.linebyline td:first-child code { background: none; font-size: 8.4pt; }
.diagram { display: flex; gap: 8px; align-items: stretch; margin: 10px 0 14px; }
.diagram .box { flex: 1; border: 1px solid var(--line); border-radius: 8px; padding: 8px; text-align: center; background: var(--bg); font-size: 9.2pt; }
.diagram .box b { display: block; color: var(--accent); margin-bottom: 3px; }
.diagram .arrow { align-self: center; color: var(--muted); font-size: 16pt; }
"""


def build_html(index: dict) -> str:
    st = index["stats"]
    comp_count = sum(1 for e in index["classes"].values() if e["why"] == "LuauBehavior component")
    chapters = []

    def chapter(num, title, body):
        chapters.append((num, title))
        cls = "chapter newpage" if num in (1,) else "chapter"
        return f'<section class="{cls}" id="ch{num}"><h2><span class="num">{num}</span>{html.escape(title)}</h2>{body}</section>'

    parts = []

    # 1 ------------------------------------------------------------------------------------
    parts.append(chapter(1, "What a script is and where it runs", f"""
<p>A <b>Luau script</b> is a small program attached to an object in the station. Orion Drift's own
game modes are written this way. TKBGolf's <code>Course.luau</code>, for example, sits on a plain
Cube object and runs the whole mini-golf course. The Spec Editor lets you attach your own scripts
to objects you place, using the same mechanism the game uses.</p>

<div class="diagram">
  <div class="box"><b>Spec Editor</b>You write the source and click <i>Attach</i></div>
  <div class="arrow">&rarr;</div>
  <div class="box"><b>Server</b>Stores it as <code>Scripts/&lt;name&gt;.luau</code> in the object's gamemode and rebuilds the object</div>
  <div class="arrow">&rarr;</div>
  <div class="box"><b>Every machine</b>Server, your editor client and every player's client compile and run it</div>
</div>

<h3>It runs everywhere</h3>
<p>This is the most important fact about scripts. The same source runs <b>on every machine that has
the object</b>: the dedicated server, your own client, and every other player's client, including
players with no mods. The test that confirmed this was a one-line script:</p>
{code('log("RIGEL LUAU OK " .. tostring(20 + 22))')}
<p>and the line <code>[RigelTest.luau]: RIGEL LUAU OK 42</code> appeared in the logs of the server,
the editor client and an unmodded player's client.</p>

<h3>What that means for your code</h3>
<ul>
<li><b>Side effects happen once per machine.</b> A <code>log</code> call prints once in each log. An
action that changes shared game state (scoring, spawning, teleporting) may run on several
machines at once. The game scripts handle this by listening to <i>server</i> events for authoritative work
(<code>onOverlapByPlayerServer</code>, <code>serverEventData</code>) and to <i>client-side</i> events for
per-player feedback (<code>onOverlapByPlayerClientside</code>).</li>
<li><b>"The local player" differs per machine.</b> <code>VRPawn.getLocalPlayerIndex()</code> returns a
different value on each client. The golf course uses exactly that to give every player their own
personal ball.</li>
<li><b>Variables are not shared.</b> Each machine has its own copy of every variable. To tell other
machines something, the game scripts use components such as <code>EventComponent</code>
(<code>triggerEventWithData</code> &rarr; <code>serverEventData</code>).</li>
</ul>

<div class="note"><b>Scope of this guide</b>
The kit is built from three sources: the game's own scripts (in <code>game-scripts/</code>), the
game's class dump (a Dumper-7 C++ SDK of build 22284) and tests run in Rigel. Where something
has only been inferred, the guide says so. Chapter 12 lists the limits.</div>
"""))

    # 2 ------------------------------------------------------------------------------------
    parts.append(chapter(2, "Attaching a script in the editor", f"""
<p>Scripts are ordinary <code>.luau</code> files in your <b>RigelScripts</b> folder
(<code>Documents\\RigelScripts</code>). The editor creates it the first time and copies this kit into it
(types, examples, VS Code settings), so autocomplete works straight away. You write scripts in
VS Code, not inside the editor.</p>
<ol class="steps">
<li><b>Open the editor</b> with <b>F12</b>, and place or select an object.</li>
<li>In <b>Details</b>, open <b>Luau script</b> and click <b>Open folder in VS Code</b>.</li>
<li><b>Write the script</b> as a new file, e.g. <code>DoorLogic.luau</code>, or copy one from <code>examples/</code>.
The file name is the script's name: it is stored as <code>Scripts/DoorLogic.luau</code>, and
<code>[DoorLogic.luau]</code> is what you'll see in logs.</li>
<li>Back in the editor, pick the file in the list (press <b>Refresh</b> if it's new) and click
<b>Attach to this object</b>.</li>
<li><b>Wire its slots</b> (chapter 4): under the script in <b>Details &rarr; Game data</b>, click a slot and then
click the object it should use, in the viewport or the Outliner. You can also drag the object onto the slot, or pick
from the slot's <b>&#9660;</b> list.</li>
<li><b>Keep editing in VS Code.</b> Every time you save the file, the editor re-sends it, and every object
running that script is rebuilt with the new code, on every machine.</li>
</ol>

<h3>What happens when you attach</h3>
<p>The server stores your source in the object's gamemode under <code>Scripts/&lt;name&gt;.luau</code>,
then <b>rebuilds the object</b> with a component entry whose id and script are both
<code>&lt;name&gt;.luau</code>. The game attaches its own scripts in exactly this way. Because the object is
rebuilt:</p>
<ul>
<li>the old copy of the object (and any script that was running on it) goes away, and the new one runs
from the top, <code>BeginPlay</code> included;</li>
<li>anything your script had set up (timers, listeners, variables) starts fresh;</li>
<li>attaching the <b>same file</b> again replaces the script, and a different file adds another script entry.</li>
</ul>

<h3>Scripts already on an object</h3>
<p>Selecting any object, including the station's own, lists its scripts in <b>Game data</b> as
<i>Script &lt;name&gt;.luau</i>. <b>Replace</b> swaps in a file from your folder. On objects you placed this
happens right away. On the station's objects, players who join from then on get the new code.</p>

<h3>Borrowing from the game's scripts</h3>
<p><b>Copy the game's own scripts into the folder (examples)</b> copies the station's scripts (golf,
Scraprun, Tackleball, ...) into <code>RigelScripts\\game-scripts</code>, where you can read them in VS Code.</p>
"""))

    # 3 ------------------------------------------------------------------------------------
    parts.append(chapter(3, "Saving scripts with levels", """
<p>Scripts are part of the level. When you save a level in the editor's <b>Levels</b> tab, every script
attached to its objects is saved with it. When the level is loaded again, the scripts are
<b>re-attached</b> to their objects, so the objects are rebuilt and the scripts start from the top on
every machine.</p>
<ul>
<li>Save the level after attaching or changing a script. The attached script lives on the running
server, and only the saved level brings it back after a restart.</li>
<li>Your RigelScripts files stay the source of truth. A level stores a copy of each script and its slot
wiring.</li>
<li>Loading a level re-runs <code>BeginPlay</code> for its scripts. Write <code>BeginPlay</code> so that running
it again on a fresh object is harmless.</li>
</ul>
<div class="tip"><b>Workflow that works well</b>
Attach once, then just edit and save in VS Code: each save re-sends the script. Read the log, repeat.
When it behaves, save the level.</div>
"""))

    # 4 ------------------------------------------------------------------------------------
    parts.append(chapter(4, "Anatomy of a script", f"""
<p>Every game script follows the same shape. Here is the start of TKBGolf's course script, cut
down:</p>
{game_lines("TKBGolf__Course.luau", 1, 12)}
<p>and here is how a script built in the same shape reads, top to bottom:</p>
{code('''--!strict                                   -- 1. mode line

-- External Dependencies                    -- 2. components this script talks to
local Button: BasicButtonComponent = nil
local Door: PhysicalComponent = nil
-- End External Dependencies

local doorOpen = false                      -- 3. your own state

local function toggle()                     -- 4. helpers
    doorOpen = not doorOpen
    if doorOpen then Door:hideLua() else Door:showLua() end
end

function BeginPlay()                        -- 5. lifecycle entry point
    Button.onButtonPressEvent.Listen(toggle)
end

function EndPlay()                          -- 6. optional clean-up
end''', "the parts of a script")}

<h3>1. <code>--!strict</code></h3>
<p>The first line picks the Luau type-checking mode. The golf scripts use <code>--!strict</code>, and the
gamemode scripts (Scraprun, Tackleball) have no mode line. The mode matters to the type checker in
VS Code, and <code>--!strict</code> gets you the most help from it. This kit's <code>.luaurc</code> turns
strict mode on for every file.</p>

<h3>2. The External Dependencies block</h3>
<p>Typed locals set to <code>nil</code>, one per component the script uses. <b>The game fills these in
before <code>BeginPlay</code></b> from the object's <code>references</code> wiring: a reference named
<code>Cup_1</code> becomes the value of <code>local Cup_1</code>. The type after the colon is the
component's Luau type (<code>GolfCupComponent</code>, <code>PhysicalComponent</code>, ...), which is also
what gives you autocomplete.</p>
<div class="tip"><b>Slots: wire them by dragging</b>
Each dependency local is a <b>slot</b>. Under the script in <b>Details &rarr; Game data</b>, the editor lists
every slot (<code>Switch (ToggleableComponent)</code>, <code>Target (PhysicalComponent)</code>, ...). Click a slot
and then click the object (in the viewport or the Outliner). Dragging an object onto the slot also works, and the
<b>&#9660;</b> button lists every object you placed that fits. <b>X</b> clears the slot, and <b>Esc</b> cancels a
pick. The object must have a component of that type; if it doesn't, the editor explains which components it
does have. Objects are moved into the scripted object's gamemode area if needed. The script is rebuilt with the
new wiring, and the wiring is saved with the level. Still check for <code>nil</code>
in <code>BeginPlay</code>, so an unwired slot logs a hint instead of an error.</div>

<h3>3&ndash;4. State and helpers</h3>
<p>Plain Luau. Prefer <code>local</code> variables and functions. The game scripts also use globals freely
(<code>currentState = ...</code>). That works, but everything in the script can then change them.</p>

<h3>5&ndash;6. Lifecycle</h3>
<table class="t">
<tr><th>When</th><th>What runs</th></tr>
<tr><td>Script loaded</td><td>All top-level code, in order. The dependency locals are still <code>nil</code> here.</td></tr>
<tr><td>Object ready</td><td><code>function BeginPlay()</code>, if you define it. Every game script does its setup here:
hooking events and setting the initial state.</td></tr>
<tr><td>Object/gamemode torn down</td><td><code>function EndPlay()</code>, if defined (Scraprun clears its tables here).
Attaching a new version of your script rebuilds the object, which ends the old one.</td></tr>
<tr><td>Later</td><td>Only what you scheduled: event listeners and timers/promises.</td></tr>
</table>
<p><code>BeginPlay</code> and <code>EndPlay</code> must be <b>global</b> functions (<code>function BeginPlay()</code>,
not <code>local function</code>) so that the game can find them. No per-frame <code>Tick</code> function appears in any game
script, so for anything repeating use timers (chapter 5).</p>
"""))

    # 5 ------------------------------------------------------------------------------------
    parts.append(chapter(5, "Core globals and patterns", f"""
<h3>Logging</h3>
{code('''log("score is", score)            -- LogLuau: Display: [MyScript.luau]: score is 3
warn("something odd")             -- LogLuau: Warning: [MyScript.luau]: something odd
Logging.warn("Category", value)   -- used by TKBGolf's ServerGolfLogic''')}
<p><code>log</code> takes any number of values. Build strings with <code>..</code> or backtick
interpolation (<code>`hole {{i}} done`</code>), as the game scripts do.</p>

<h3>Timers: <code>LuauClock</code></h3>
<table class="t">
<tr><th>Call</th><th>Meaning</th></tr>
<tr><td><code>LuauClock.timeout(seconds)</code></td><td>A promise that resolves after <i>seconds</i>. <code>timeout(0)</code> means "next moment".</td></tr>
<tr><td><code>LuauClock.getTime()</code></td><td>Current time in seconds. Subtract two readings to measure a duration.</td></tr>
<tr><td><code>LuauClock.createTimer(interval, fn)</code></td><td>Found in the class dump (<code>ULuauClock::CreateTimer</code>), but no game script uses it. Untested.</td></tr>
</table>
{code('''LuauClock.timeout(5):andThen(function()
    Cup:setEnableArrowIndicator(false)        -- hide the arrow 5 s later (ClubGolf)
end)''')}

<h3>Promises</h3>
<p>Asynchronous work comes back as a promise. The game scripts use three shapes:</p>
{code('''-- a) chain: run a function when it resolves
Tee:spawnBallWithParameters(params):andThen(function(Disc: DiscEntity)
    Disc:setEnableHUDIndicator(true)
end)

-- b) straight-line: :await() pauses this function until the promise resolves
Promise.new(function()
    ResetTrigger:luaEnable()
    LuauClock.timeout(0.25):await()
    ResetTrigger:luaDisable()
end)

-- c) whichever finishes first
Promise.race({ LuauClock.timeout(45), earlyOut }):andThen(function() ... end)''')}
<p><code>:await()</code> only makes sense inside <code>Promise.new(function() ... end)</code>, which runs its
function as its own coroutine. Scraprun uses this for every "do X, wait, do Y" sequence.
<code>Promise.resolve(v)</code>, <code>Promise.try(fn)</code> and <code>:expect()</code> appear in commented-out
code in the golf scripts.</p>

<h3>Events</h3>
<p>Components expose C++ multicast delegates as events. Listen with a <b>dot</b>:</p>
{code('''Disc.onGoToSleep.Listen(function()
    Disc:addSmoothOffset(Disc:getGravityDirection() * -10)
end)
Cup.onBallInCup_Multicast.Listen(function(Cup: GolfCupComponent, Score: number, Disc: DiscEntity)
    ...
end)
BasicTimer.finished.Listen(BasicTimerFinished)   -- pass a named function''')}
<p>The callback gets the delegate's arguments, in order: <code>TMulticastInlineDelegate&lt;void(UGolfCupComponent*
Cup, int32 Score, ADiscEntity* Entity)&gt;</code> becomes <code>(GolfCupComponent, number, DiscEntity)</code>.
The colon form <code>Player.onBeforeDestroy:Listen(fn)</code> also appears in a game script, and the definitions
accept both. Event names are lowerCamel (<code>onGoToSleep</code>), but the game scripts also use the
original C++ spelling (<code>BlueprintOnEnabled</code>, <code>TeamSizeIncreased</code>), so both spellings are in the
definitions.</p>

<h3>Vectors</h3>
{code('''local v = Vector.new(0, 0, 1)
local d = disc:getDiscPosition() - VRPawn.getPositionByID(playerId)   -- Vector - Vector
local dist = math.sqrt(d.x * d.x + d.y * d.y + d.z * d.z)              -- .x .y .z
local dir = d / dist                                                  -- Vector / number
ball:setVelocity(dir * 3500 + Vector.new(0, 0, 200))                  -- *, +''')}
<p>Units are Unreal units (centimetres). <code>Vec3.new</code> is an older spelling used once in the
Tackleball script. C++ <code>FRotator</code> parameters take a <code>Vector</code> of (pitch, yaw, roll), as
<code>requestTrainingBallAtLocation(Location, Rotation)</code> shows.</p>

<h3>Other globals</h3>
<table class="t">
<tr><th>Global</th><th>What it's for (as used in the game scripts)</th></tr>
<tr><td><code>VRPawn.getLocalPlayerIndex()</code></td><td>Index of this machine's player.</td></tr>
<tr><td><code>VRPawn.getPlayerByID(id)</code>, <code>getPlayerName(id)</code>, <code>getTeamIndexByID(id)</code>, <code>getPositionByID(id)</code></td><td>Look up players by index. A name of <code>"None"</code> means unknown.</td></tr>
<tr><td><code>Quests.event(player, id)</code>, <code>Quests.eventNumber(player, id, n)</code>, <code>Quests.complete(player, questId)</code></td><td>Quest/stat progress. The golf scripts pass <code>-1</code> from client-side handlers.</td></tr>
<tr><td><code>Gamemode:getNumberConfigVariable(name)</code>, <code>getBoolConfigVariable</code>, <code>startGame()</code>, <code>stopGame()</code>, <code>setTeamScore(t, n)</code>, <code>onConfigChanged</code></td><td>The gamemode the script belongs to. Only the gamemode scripts use it, and it's untested from a sandbox object's script.</td></tr>
<tr><td><code>BallSpawnParameters.new{{...}}</code>, <code>GoalInfo.new{{...}}</code></td><td>Build C++ structs from a table with lowerCamel keys.</td></tr>
<tr><td><code>LinearColor.new(r, g, b, a)</code>, <code>CellStyle.new</code>, <code>LayoutSettings.new</code>, <code>LayoutGap.new</code>, <code>StyleOverRange.new</code>, <code>Enum.TextJustify</code></td><td>Scoreboard styling (<code>DataTableComponent</code>).</td></tr>
<tr><td><code>require("Name")</code></td><td>Loads another script from the same gamemode (TKBGolf's <code>ServerCourseLogic</code> requires <code>ServerGolfLogic</code>). Not tested with editor-attached scripts, and VS Code can't resolve it.</td></tr>
</table>
"""))

    # 6 ------------------------------------------------------------------------------------
    parts.append(chapter(6, "Components and how to find their methods", f"""
<p>A <b>component</b> is a C++ class deriving from <code>ULuauBehavior</code>. Buttons, triggers, timers,
ball spawners, goals, scoreboards and toggles are all components. Scripts reach them through the External
Dependencies block and then call their methods and listen to their events.</p>

<h3>The naming rule</h3>
<table class="t">
<tr><th>In C++ (class dump)</th><th>In Luau</th></tr>
<tr><td><code>class UGolfCupComponent : public ULuauBehavior</code></td><td>type <code>GolfCupComponent</code> (drop the <code>U</code>)</td></tr>
<tr><td><code>class ADiscEntity : public AActor</code></td><td>type <code>DiscEntity</code> (drop the <code>A</code>)</td></tr>
<tr><td><code>void SetCupVisibility(bool Show);</code></td><td><code>cup:setCupVisibility(show)</code> (lowerCamel, colon call)</td></tr>
<tr><td><code>static int32 GetLocalPlayerIndex(UWorld* World);</code></td><td><code>VRPawn.getLocalPlayerIndex()</code> (dot call, world supplied for you)</td></tr>
<tr><td><code>TMulticastInlineDelegate&lt;void()&gt; OnGoToSleep;</code></td><td><code>disc.onGoToSleep.Listen(function() end)</code></td></tr>
<tr><td><code>void Client_EmitStatEvent(const FString&amp;, int32);</code></td><td><code>pawn:client_EmitStatEvent("Stat", 1)</code> (only the first letter changes)</td></tr>
</table>

<h3>The generated definitions</h3>
<p><code>generate_defs.py</code> reads the class dump (<code>HalcyonA2/HalcyonA2/gamesdk/22284/SDK</code>) and writes
<code>types/rigel.d.luau</code>, a luau-lsp definition file. The current build covers
<b>{st["classes"]} classes</b> ({comp_count} of them <code>ULuauBehavior</code> components) with <b>{st["methods"]} methods</b>,
<b>{st["events"]} events</b>, <b>{st["properties"]} properties</b>, <b>{st["static"]} static functions</b> and
<b>{st["structs"]} structs</b>. Each method carries its original C++ declaration as a comment:</p>
{code('''declare extern type GolfCupComponent extends LuauBehavior with
	-- C++: void SetCupVisibility(bool Show);
	function setCupVisibility(self, show: boolean): ()
	...
	onBallInCup_Multicast: Event<(GolfCupComponent, number, DiscEntity)>
end''', "types/rigel.d.luau (excerpt)")}

<h3>Type mapping</h3>
<table class="t">
<tr><th>C++</th><th>Luau</th></tr>
<tr><td><code>bool</code></td><td><code>boolean</code></td></tr>
<tr><td><code>int32</code>, <code>float</code>, <code>double</code>, <code>uint8</code>...</td><td><code>number</code> (the game scripts also write <code>int</code>, an alias)</td></tr>
<tr><td><code>FString</code>, <code>FName</code>, <code>FText</code></td><td><code>string</code></td></tr>
<tr><td><code>FVector</code>, <code>FRotator</code></td><td><code>Vector</code></td></tr>
<tr><td><code>UFoo*</code> / <code>AFoo*</code></td><td><code>Foo</code> when it is in the definitions, otherwise <code>any</code></td></tr>
<tr><td><code>TArray&lt;T&gt;</code></td><td><code>{{T}}</code></td></tr>
<tr><td><code>FLuaRef</code></td><td><code>Ref</code> (any Luau table)</td></tr>
<tr><td>game structs (<code>FGoalInfo</code>, ...)</td><td>a type with lowerCamel fields (<code>goalInfo.scoringTeam</code>)</td></tr>
<tr><td>out-parameters (<code>T* Out</code>)</td><td>returned as extra results</td></tr>
<tr><td>anything else</td><td><code>any</code></td></tr>
</table>

<h3>Blueprint-backed types</h3>
<p>Some types in the game scripts aren't C++ components. They're Blueprint actors with a Luau
bridge: <code>GolfTeeComponent</code>, <code>TrapSpinnerComponent</code>, <code>SlidingPlatformComponent</code>,
<code>GameStateManagerBlueprintComponent</code> and others. The generator maps each one to the Blueprint class whose
functions match the calls in the scripts (for example <code>GolfTeeComponent</code> &rarr; <code>BP_GolfTee_C</code>, which has
<code>onManualStart</code> and <code>Set_Enable_Arrow</code>). Blueprint functions are listed under several spellings,
because the scripts call them as written (<code>GameStateManagerBP:PlaySaveEvent()</code>), in lowerCamel
(<code>setIsRestartable</code>) and without underscores (<code>setEnableArrow</code>). These mappings are
inferred from the scripts.</p>

<h3>Finding what you need</h3>
<ol>
<li>In VS Code, type the variable name and <code>:</code> (methods) or <code>.</code> (events and properties).</li>
<li><i>Go to Definition</i> (F12) on a type name opens <code>rigel.d.luau</code> at that class, with the C++ comments.</li>
<li>Search <code>game-scripts/</code> for the method name. A method the game itself calls is much more likely to
work than one that only appears in the dump.</li>
<li>Appendix A lists every class in the definitions with its key methods and events.</li>
</ol>
"""))

    # 7 + 8 examples ---------------------------------------------------------------------------
    parts.append(chapter(7, "Worked examples from this kit", f"""
<p>All six files in <code>examples/</code> type-check with no errors against the definitions
(<code>luau-lsp analyze</code>, strict mode). Examples 1&ndash;3 only use globals, so they can run on any object
you place. Examples 4&ndash;6 need their slots wired (chapter 4).</p>

<h3>Example 1: hello</h3>
{example("01_hello.luau")}
<table class="linebyline">
<tr><td><code>--!strict</code></td><td>Full type checking in VS Code.</td></tr>
<tr><td><code>local answer = 20 + 22</code></td><td>Top-level code runs as soon as the script loads, on every machine.</td></tr>
<tr><td><code>function BeginPlay()</code></td><td>Global, so the game can call it once the object is ready.</td></tr>
<tr><td><code>VRPawn.getLocalPlayerIndex()</code></td><td>A static UFunction, called with a dot. Each client gets its own index.</td></tr>
<tr><td><code>log("Hello from player " .. ...)</code></td><td>One line per machine in <code>LogLuau</code>.</td></tr>
<tr><td><code>function EndPlay()</code></td><td>Runs when the object is rebuilt or removed.</td></tr>
</table>

<h3>Example 2: repeating timer</h3>
{example("02_repeating_timer.luau")}
<table class="linebyline">
<tr><td><code>LuauClock.getTime()</code></td><td>Record the start time so each beat can report the elapsed time.</td></tr>
<tr><td><code>LuauClock.timeout(INTERVAL):andThen(beat)</code></td><td>Schedule <code>beat</code>. Inside <code>beat</code> the same call schedules the next one, which makes a loop.</td></tr>
<tr><td><code>if beats &lt; MAX_BEATS then ... else log("done")</code></td><td>Always give a loop a way to stop, or it keeps running on every machine for as long as the object exists.</td></tr>
<tr><td><code>`beat {{beats}} at ...`</code></td><td>Backtick string interpolation, as used in the game scripts.</td></tr>
</table>

<h3>Example 3: promises</h3>
{example("03_promise_sequence.luau")}
<table class="linebyline">
<tr><td><code>Promise.new(function() ... end)</code></td><td>Runs the function as its own coroutine, so <code>:await()</code> can pause it.</td></tr>
<tr><td><code>LuauClock.timeout(1):await()</code></td><td>Wait one second, then continue with the next line.</td></tr>
<tr><td><code>Promise.new(function(resolve, _reject) ... end)</code></td><td>A promise you resolve yourself. It keeps <code>resolve</code> in <code>cancel</code> for later.</td></tr>
<tr><td><code>Promise.race({{ timeout, signalled }})</code></td><td>Resolves when the first of the two resolves. TKBGolf uses this to hide an arrow early.</td></tr>
</table>
"""))

    parts.append(chapter(8, "Fun scripts to drop in", f"""
<p>Ready-made scripts with slots, in <code>examples/</code> (and in your RigelScripts folder). Attach one and drag
objects onto its slots.</p>
<h3>Light switch: show and hide an object</h3>
{example("07_light_switch.luau")}
<h3>Vanishing platform</h3>
{example("08_vanishing_platform.luau")}
<h3>Blinker</h3>
{example("09_blinker.luau")}
<h3>Secret door</h3>
{example("10_secret_door.luau")}
"""))

    parts.append(chapter(9, "Worked examples: components and the game's scripts", f"""
<h3>Example 4: a button that toggles a door</h3>
{example("04_button_toggles_door.luau")}
<table class="linebyline">
<tr><td><code>local Button: BasicButtonComponent = nil</code></td><td>Filled from a reference named <code>Button</code>. The type gives you <code>Button.onButtonPressEvent</code> and <code>Button:luaEnableButton()</code>.</td></tr>
<tr><td><code>(Button :: BasicButtonComponent?) == nil</code></td><td>The cast to an optional type lets strict mode accept a <code>nil</code> comparison. At runtime it's a plain nil check.</td></tr>
<tr><td><code>Door:hideLua()</code> / <code>disableCollisionLua()</code></td><td><code>PhysicalComponent</code> methods. Scraprun opens its forcefields with this exact pair.</td></tr>
<tr><td><code>Button.onButtonPressEvent.Listen(...)</code></td><td>Dot + <code>Listen</code>. Scraprun hooks every trap button this way.</td></tr>
</table>

<h3>Example 5: a trigger zone</h3>
{example("05_trigger_zone.luau")}
<p><code>onOverlapByPlayerServer</code> passes the player index, and <code>onOverlapByPlayerClientside</code>
passes nothing and fires on the entering player's own machine. That split is the one
<code>TKBGolf__Course.luau</code> uses to start a hole for the player who walked in.</p>

<h3>Example 6: vectors and balls</h3>
{example("06_vectors_and_balls.luau")}
<p>The spawn call follows TKBGolf's course script. <code>spawnBallWithParameters</code> is declared in C++ as
returning <code>ADiscEntity*</code>, but in Luau it returns a <b>promise</b> of the ball (the game script
chains <code>:andThen(function(Disc: DiscEntity) ...)</code>). The definitions include that correction.</p>

<h3>From the game: starting a hole (TKBGolf)</h3>
{game_lines("TKBGolf__Course.luau", 130, 151)}
<table class="linebyline">
<tr><td><code>BallSpawnParameters.new({{ ... }})</code></td><td>C++ struct <code>FBallSpawnParameters</code>, keys in lowerCamel.</td></tr>
<tr><td><code>targetPlayerIndex = VRPawn.getLocalPlayerIndex()</code></td><td>Each client spawns its own personal ball.</td></tr>
<tr><td><code>Tee:spawnBallWithParameters(Parameters):andThen(...)</code></td><td>A promise of the spawned <code>DiscEntity</code>.</td></tr>
<tr><td><code>Disc.onGoToSleep.Listen(...)</code></td><td>An event on the ball, fired when it comes to rest.</td></tr>
<tr><td><code>Disc:getGravityDirection() * -10</code></td><td>Vector times number. (<code>addSmoothOffset</code> is a <code>BallSpawnerComponent</code> method in the dump, not a <code>DiscEntity</code> one, so this call can't be confirmed from the dump.)</td></tr>
</table>

<h3>From the game: a state machine driven by timers (ScraprunPrime)</h3>
{game_lines("ScraprunPrime__gamemode.luau", 362, 370)}
{game_lines("ScraprunPrime__gamemode.luau", 757, 764)}
<p><code>BasicTimer</code> is a <code>TimerComponent</code>. <code>BasicTimer:start(length)</code> starts it, and
<code>BasicTimer.finished</code> fires when it runs out. The script keeps a table of states, and each state has an
<code>onEnter</code> function, an optional <code>nextState</code> and an optional timer length.</p>

<h3>From the game: passing data to the server (TKBGolf)</h3>
{game_lines("TKBGolf__ServerCourseLogic.luau", 108, 116)}
<p>The course script (on every client) calls <code>MainEventRepeater:triggerEventWithData({{ EventType = ..., ... }})</code>,
and the server-side scoreboard script receives the same table through <code>EventComponent.serverEventData</code>.
Any Luau table can be sent this way (the <code>Ref</code> type).</p>
"""))

    # 9 VS Code ---------------------------------------------------------------------------------
    parts.append(chapter(10, "Setting up VS Code", f"""
<ol class="steps">
<li><b>Install VS Code</b> from <code>code.visualstudio.com</code>.</li>
<li><b>Install the extension.</b> Open Extensions (<code>Ctrl+Shift+X</code>), search <b>Luau Language Server</b>
by <i>JohnnyMorganz</i> (<code>JohnnyMorganz.luau-lsp</code>) and click Install.</li>
<li><b>Open the kit folder.</b> <i>File &rarr; Open Folder...</i> and choose <code>SpecEditor\\luau</code>
itself. The settings in <code>.vscode/settings.json</code> use paths relative to this folder.</li>
<li><b>Check it works.</b> Open <code>examples/01_hello.luau</code> and hover <code>log</code>: you should see
<code>log(...: any): ()</code>. Type <code>VRPawn.</code> and the static functions appear.</li>
<li><b>Write your script</b> as a new <code>.luau</code> file in <code>Documents\\RigelScripts</code> (the editor's
<b>Open folder in VS Code</b> button opens it with these settings), then attach it (chapter 2). Saving the
file re-sends it.</li>
</ol>

<h3>What the settings do</h3>
{code('''{
    "luau-lsp.platform.type": "standard",
    "luau-lsp.sourcemap.enabled": false,
    "luau-lsp.types.definitionFiles": { "@rigel": "types/rigel.d.luau" },
    "luau-lsp.ignoreGlobs": ["game-scripts/**"]
}''', ".vscode/settings.json")}
<ul>
<li><code>platform.type: "standard"</code> makes this plain Luau, with no Roblox globals.</li>
<li><code>types.definitionFiles</code> loads the game API. The setting is a map from a name to a file, as
the current extension expects.</li>
<li><code>ignoreGlobs</code> hides problems in the dumped game scripts, which call a few things the
definitions can't know about.</li>
</ul>
{code('''{
    "languageMode": "strict",
    "lint": { "*": true, "FunctionUnused": false },
    "lintErrors": false,
    "typeErrors": true
}''', ".luaurc")}
<p><code>FunctionUnused</code> is off because nothing in the file calls <code>BeginPlay</code> and <code>EndPlay</code>
(the game does).</p>

<h3>What you get</h3>
<ul>
<li>Completion after <code>obj:</code> (methods) and <code>obj.</code> (events, properties), with parameter hints.</li>
<li>Red underlines for misspelt methods, wrong argument types and wrong argument counts.</li>
<li>Typed event callbacks: in <code>Cup.onBallInCup_Multicast.Listen(function(cup, score, disc) ...)</code> the
editor knows <code>score</code> is a number.</li>
</ul>
<div class="note"><b>Definition syntax</b>
<code>rigel.d.luau</code> uses <code>declare extern type Name with ... end</code>, the syntax current luau-lsp
releases expect (checked with luau-lsp 1.70). Older releases used <code>declare class</code> and newer ones reject it.
If an old extension shows "Failed to read definitions file", update it.</div>
<div class="tip"><b>Regenerating</b>
Run <code>python generate_defs.py</code> in the folder after the SDK dump changes, then
<i>Luau: Restart Language Server</i> from the command palette.</div>
"""))

    # 10 troubleshooting -----------------------------------------------------------------------
    parts.append(chapter(11, "Troubleshooting", """
<h3>Where the output goes</h3>
<p>Every machine writes its own log. On a PC client it is
<code>%LOCALAPPDATA%\\A2\\Saved\\Logs\\A2*.log</code> (the newest is <code>A2.log</code>; older runs are
<code>A2_2.log</code> and so on). The server writes its own log on the server machine.</p>
<table class="t">
<tr><th>You wrote</th><th>The log shows</th></tr>
<tr><td><code>log("hi")</code></td><td><code>LogLuau: Display: [MyScript.luau]: hi</code></td></tr>
<tr><td><code>warn("hi")</code></td><td><code>LogLuau: Warning: [MyScript.luau]: hi</code></td></tr>
</table>
<p>Search for <code>[MyScript.luau]</code> to see only your script's lines.</p>

<div class="tip"><b>Errors pop up in the editor</b>
When one of your scripts hits an error on any machine, the editor shows a popup with the file, the line, what
went wrong in plain words and how to fix it, plus an <b>Open in VS Code</b> button that jumps to the line.</div>
<div class="note"><b>Errors can't crash the game</b>
The server adds a small safety net to the end of every script: <code>BeginPlay</code>, <code>EndPlay</code> and
<code>Tick</code> run inside <code>pcall</code>, and the game already catches errors in event listeners and timers.
A mistake stops that one function and is reported; the object, the server and every player keep running.
(The safety net is added after your last line, so line numbers in errors match your file.)</div>

<h3>Nothing appears</h3>
<ul>
<li><b>Compile errors.</b> The editor checks every script for typos before sending it. If there's a typo, a
popup names the line and what's wrong, and the script isn't sent. VS Code also shows syntax errors as you type. Then search the log for your script name and for <code>LogLuau</code> lines
of Warning or Error verbosity. The exact error format hasn't been captured yet.</li>
<li><b>Only top-level code ran.</b> Check that <code>BeginPlay</code> is a global function with that exact spelling.</li>
<li><b>An early error in BeginPlay.</b> A runtime error stops the rest of the function. A <code>nil</code> dependency
(no references wiring) is the usual cause, so check for it and log it, as the examples do.</li>
<li><b>Wrong machine.</b> Server-side events (<code>onOverlapByPlayerServer</code>) only fire on the server, so
look in the server log for those.</li>
</ul>

<h3>The object changed after I attached a script</h3>
<p>That's expected. Attaching <b>rebuilds the object</b> with the new component entry. Any runtime state
it had is lost, and the script starts again from <code>BeginPlay</code>. Reloading a level does the same.</p>

<h3>Something runs twice (or once per player)</h3>
<p>The script runs on every machine (chapter 1). Guard work that should happen once: do authoritative work
in server events, and per-player work in client-side events or behind a
<code>VRPawn.getLocalPlayerIndex()</code> check.</p>

<h3>VS Code problems</h3>
<table class="t">
<tr><th>Symptom</th><th>Fix</th></tr>
<tr><td>"Unknown global 'log'", no completions</td><td>Open <code>SpecEditor\\luau</code> as the folder (not the repo root), then <i>Luau: Restart Language Server</i>.</td></tr>
<tr><td>"Failed to read definitions file"</td><td>Update the Luau Language Server extension, or re-run <code>generate_defs.py</code>.</td></tr>
<tr><td>"Key 'Foo' not found in external type"</td><td>That method isn't in the class dump for that type. Check spelling (lowerCamel) and Appendix A.</td></tr>
<tr><td>"cannot be compared with ~= / =="</td><td>Cast to an optional type first: <code>(Door :: PhysicalComponent?) == nil</code>.</td></tr>
<tr><td>"Unknown require"</td><td><code>require("OtherScript")</code> resolves inside the game, not on disk.</td></tr>
</table>
"""))

    # 11 limitations ---------------------------------------------------------------------------
    parts.append(chapter(12, "Limitations", """
<div class="warn"><b>Read this before building something big</b>
The kit combines verified facts, game scripts and a class dump. They aren't equally reliable.</div>
<table class="t">
<tr><th>Topic</th><th>Status</th></tr>
<tr><td>Attaching, compiling and running on every machine; <code>log</code></td><td><b>Verified</b> in Rigel (server, editor client, unmodded client).</td></tr>
<tr><td>Saving scripts with levels, re-attaching on load</td><td><b>Verified</b> as editor behaviour.</td></tr>
<tr><td><code>LuauClock</code>, <code>Promise</code>, events, <code>Vector</code>, <code>VRPawn.*</code>, <code>Quests.*</code>, <code>Gamemode</code></td><td>Used by the game's own scripts, so they exist in the game's Luau environment. Not yet exercised from an editor-attached script.</td></tr>
<tr><td>Method lists in <code>rigel.d.luau</code></td><td><b>Generated from the class dump.</b> Every UFunction of every component is listed, but not every one is necessarily exposed to Luau or safe to call (some are network RPCs or engine callbacks such as <code>overlapBegin</code>). Prefer methods the game scripts call. <b>Test in a local server first.</b></td></tr>
<tr><td>Properties</td><td>Public simple-typed fields are listed (the scripts read e.g. <code>disc.rollbackRecentPlayerHit</code>), but whether a particular field is readable or writable from Luau is unknown.</td></tr>
<tr><td>External Dependencies / references</td><td><b>Wired by dragging</b> from the Outliner onto a slot. The target must be in the same gamemode area as the scripted object.</td></tr>
<tr><td>Blueprint-backed types, PascalCase spellings</td><td>Inferred from the game scripts (e.g. <code>GolfTeeComponent</code> &rarr; <code>BP_GolfTee_C</code>).</td></tr>
<tr><td>Return values</td><td>Object returns are typed as never-nil for convenience, but at runtime they can be <code>nil</code> (e.g. <code>getSpawnedBall()</code> before a ball exists). Check before use.</td></tr>
<tr><td>The game scripts themselves</td><td>They contain a few calls the dump doesn't back (<code>Disc:addSmoothOffset</code> on a <code>DiscEntity</code>, <code>resetBall</code> with extra arguments). Don't copy those as-is.</td></tr>
</table>
<h3>Safe habits</h3>
<ul>
<li>Test on a local server with a mock or second client before a live station. Never try a new script on a server with players on it.</li>
<li>Start with <code>log</code> lines around every call you haven't used before.</li>
<li>Check every dependency for <code>nil</code>, and give every loop a stop condition.</li>
</ul>
"""))

    # Appendix ---------------------------------------------------------------------------------
    parts.append(f"""<section class="chapter newpage" id="appA"><h2><span class="num">A</span>Appendix: component and class reference</h2>
<p>Every class in <code>rigel.d.luau</code>, with up to six key methods (engine callbacks and network
RPCs left out), and its first few events. Call methods with <code>:</code> and listen to events with
<code>.name.Listen(fn)</code>. The full lists, with C++ declarations, are in the definitions file.</p>
{appendix(index)}
</section>
<section class="chapter" id="appB"><h2><span class="num">B</span>Appendix: global tables</h2>
<p>Static functions and constructors available as globals, in addition to <code>log</code>, <code>warn</code>,
<code>Logging</code>, <code>Promise</code>, <code>Vector</code>/<code>Vec3</code>, <code>LinearColor</code> and
<code>Enum.TextJustify</code>.</p>
{globals_table(index)}
</section>""")

    toc = "".join(f'<div><span class="n">{n}</span>{html.escape(t)}</div>' for n, t in chapters)
    toc += '<div><span class="n">A</span>Appendix: component and class reference</div><div><span class="n">B</span>Appendix: global tables</div>'
    cover = f"""<section class="cover">
  <div class="band">
    <div style="font-size:11pt;letter-spacing:.14em;text-transform:uppercase;opacity:.85">Rigel &middot; Spec Editor</div>
    <h1>Luau Scripting Guide</h1>
    <div class="sub">Write scripts for the objects you place, and run them on every player's machine.</div>
  </div>
  <div>
    <h3>Contents</h3>
    <div class="toc">{toc}</div>
  </div>
  <div class="meta">Orion Drift build 22284 &middot; definitions: {st["classes"]} classes, {st["methods"]} methods,
  {st["events"]} events &middot; kit folder: <code>SpecEditor/luau</code></div>
</section>"""
    return f"""<!doctype html><html><head><meta charset="utf-8"><title>Rigel Luau Scripting Guide</title>
<style>{CSS}</style></head><body>{cover}{''.join(parts)}</body></html>"""


def find_edge() -> str | None:
    for p in (r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
              r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
              shutil.which("msedge") or "", shutil.which("chrome") or ""):
        if p and os.path.exists(p):
            return p
    return None


def main():
    with open(INDEX, encoding="utf-8") as fh:
        index = json.load(fh)
    doc = build_html(index)
    with open(OUT_HTML, "w", encoding="utf-8", newline="\n") as fh:
        fh.write(doc)
    print(f"wrote {OUT_HTML}")
    edge = find_edge()
    if not edge:
        sys.exit("Microsoft Edge not found; open tools/guide.html in a browser and print it to PDF")
    url = "file:///" + OUT_HTML.replace("\\", "/")
    cmd = [edge, "--headless=new", "--disable-gpu", "--no-pdf-header-footer",
           "--no-first-run", f"--user-data-dir={os.path.join(os.environ.get('TEMP', HERE), 'rigel-guide-edge')}",
           f"--print-to-pdf={OUT_PDF}", url]
    subprocess.run(cmd, check=True, timeout=180, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(f"wrote {OUT_PDF}")


if __name__ == "__main__":
    main()
