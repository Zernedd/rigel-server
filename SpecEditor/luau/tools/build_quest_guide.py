#!/usr/bin/env python3
"""
build_quest_guide.py -- builds SpecEditor/luau/Rigel-Quest-Guide.pdf

How to make quests in the Spec Editor and hook them up to kiosks, start buttons and saved levels.
Uses the same styling and Edge print step as build_guide.py.

    python tools/build_quest_guide.py
"""
from __future__ import annotations

import os
import subprocess
import sys

import build_guide as bg

OUT_HTML = os.path.join(bg.HERE, "quest_guide.html")
OUT_PDF = os.path.join(bg.ROOT, "Rigel-Quest-Guide.pdf")

CHAPTERS = [
    ("1", "How quests work on Rigel", """
<p>A quest is a row in the player's quest list: a name, an icon, a description and a progress bar. The editor
publishes quests <b>to the server</b>, which sends them to every player, so they work for everyone,
Quest headsets and unmodded PC players included. The server also decides when a quest is done: it
watches where players are (checkpoint runs) or lets the game's own red coin run complete it, and then marks it
completed through the game's own completion path, so the row turns green.</p>
<ul>
<li><b>Checkpoint run</b>: reach objects you placed, in order, optionally against the clock.</li>
<li><b>Red coin run</b>: the TKB-style run. A start button starts it, and the player has a time limit to collect every
coin.</li>
<li><b>Quest group</b>: a folder that shows several quests together. Each quest keeps its own icon and completes
on its own.</li>
</ul>
<p>Open the <b>Quests</b> tab next to Details (F12 shows and hides the editor).</p>
"""),
    ("2", "Making a checkpoint run", """
<ol>
<li>Click <b>+ New quest</b>, type a <b>Name</b> (what players see) and an optional <b>Description</b>.</li>
<li>Pick an <b>Icon</b>. Every icon the game ships is in the list (climbing, red coin, racing and so on).</li>
<li>Leave <b>Type</b> on <i>Checkpoint run</i>.</li>
<li>Add checkpoints: fly to each spot and click <b>Place new checkpoint here</b>, or select any placed object and
click <b>Add selected object</b>. Reorder them with the arrows; <b>Go</b> flies you to one, and <b>X</b> removes it.</li>
<li>Set the <b>Touch distance</b> (how close counts as reached) and, if you want, tick <b>Time limit</b>; it counts from
the first checkpoint.</li>
<li>Click <b>Publish quest</b>. The quest appears in every player's list straight away. When you change it later, the
list shows <i>(changed)</i>; publish again to update it in place. Players' progress is kept because the quest's id
never changes.</li>
</ol>
"""),
    ("3", "Making a red coin run", """
<ol>
<li><b>+ New quest</b>, then set <b>Type</b> to <i>Red coin run</i>.</li>
<li><b>Place start button here</b> puts the button in front of you. <b>Move here</b> moves it later.</li>
<li><b>Place coins by clicking (construction mode)</b>: click anywhere in the world and a coin drops there,
resting on the surface you clicked. A red marker shows where it will land. Press <b>Esc</b> or the
button again to stop. <b>Add coin here</b> still places one in front of the camera.</li>
<li><b>Add coin here</b> places a real red coin at the spot in front of you. While you build the course the coins
are real objects, shown in the editor with the real coin's look: click one and move it with the gizmo like
anything else. <b>Select</b>, <b>Go</b> and <b>X</b> work
on each coin in the list.</li>
<li>Set the <b>run time</b> in seconds.</li>
<li><b>Boosting</b>: <i>Allowed</i> means boost pads and thrusters work during the run. <i>Boosting fails the run</i> is the
TKB rule.</li>
<li>Click <b>Publish run</b>. The preview coins are replaced by the real run. <b>Edit coins (show them)</b> brings the preview
back when you want to change the course.</li>
</ol>
<p class="note">Players press the start button, collect every coin before the timer ends, and the game completes the
quest on their machine. Their thrusters are never locked after the run.</p>
"""),
    ("4", "Grouping quests", """
<p>Publish the quests first. Then make a new quest with <b>Type</b> <i>Quest group</i>, add each quest with
<b>Add a quest to the group...</b>, give the group a name and an icon, and click <b>Publish group</b>. Players see a folder with your quests inside, each with its own
icon and progress.</p>
<p class="note">A group only lists quests that are published. If you delete a quest, it leaves every group that
listed it.</p>
"""),
    ("5", "Kiosks and start buttons", """
<p>A <b>quest kiosk</b> (Quest Display Kiosk in the Place panel) is a board that shows chosen quests with their
progress. To set one up:</p>
<ol>
<li>Place a kiosk, or select one already on the station.</li>
<li>In <b>Details</b>, open <b>Game data (synced to everyone)</b>.</li>
<li><b>Target Quests</b> is the kiosk's list. Add a quest with the searchable picker under the list (your
published quests are marked <i>(editor)</i>, next to all the station's own quests). <b>X</b> removes a row, and
the order here is the order on the board.</li>
<li><b>Kiosk Header</b> is the title shown above the list. Every setting a kiosk has is listed, even ones it
hasn't stored yet, so a brand-new kiosk can be set up straight away.</li>
</ol>
<p>Changes go through the game's own synced values, so every player sees the new board immediately.</p>
<p>A <b>Progression Button</b> works the same way. Its <b>Target Quest</b> field in Game data chooses which quest
pressing it activates. That is how a red coin run's start button is wired, and you can point one at any quest.</p>
<p class="note">Save the level (chapter 7) to keep kiosk setups. Saved levels record every Game data edit and
put the quests back by name, so a kiosk still points at the right quest when the level is loaded on another
server.</p>
"""),
    ("6", "Changing and deleting quests", """
<ul>
<li>Pick a quest in the list to edit it, then publish again.</li>
<li><b>Delete quest</b> (under the list) removes it from every player's quest list, together with its start button and
coins, and from any group.</li>
<li>Quests published on this server that aren't in your list (made in an earlier session, or by a loaded
level) are under <b>On the server, not in your list</b>, each with a <b>Delete</b> button.</li>
</ul>
"""),
    ("7", "Saving quests on the backend", """
<p>The <b>Levels</b> tab saves everything you built, including quests, red coin runs, kiosk settings, scripts and
script slots, as a named level on the Rigel backend.</p>
<ol>
<li>Type a name and click <b>Save level</b>.</li>
<li><b>Load</b> and <b>Unload</b> put a level on the current server or take it off. Unloading also removes its quests
from players' lists.</li>
<li>On the dashboard, the station's <b>Editor levels</b> tab lists every saved level. There you choose which levels
are loaded now and which <b>load automatically when the server boots</b>. The game server checks every 15 seconds
and applies your choices, with no restart needed.</li>
</ol>
<p class="note">Quests from a level are wired to their kiosks and buttons automatically when it loads: the level
stores quest references by name and resolves them on the server it loads on.</p>
"""),
    ("8", "Scripting quest logic", """
<p>For custom behaviour, such as completing a quest when a switch is flipped, attach a Luau script to an object. See the
<i>Rigel Luau Scripting Guide</i> (Rigel-Luau-Guide.pdf in the same folder). Scripts can hold <b>slots</b>
(<code>local Target: PhysicalComponent = nil</code>): click the slot in Details, then click the object to wire it.</p>
"""),
    ("9", "Troubleshooting", """
<ul>
<li><b>The quest doesn't show up</b>: a player must have received the station's quests once, because the server copies
their row format. Publish again after someone has joined.</li>
<li><b>The quest doesn't turn green</b>: checkpoint runs need the player to reach every checkpoint in order, within the
touch distance. Red coin runs need every coin before the timer ends. The server stores the completion in the player's
progression the same way their saved quests are loaded, so it shows as done.</li>
<li><b>The kiosk is empty</b>: its Target Quests must be quests the player has, meaning published and not deleted.</li>
</ul>
"""),
]


def build_html() -> str:
    toc = "".join(f'<div><span class="n">{n}</span>{t}</div>' for n, t, _ in CHAPTERS)
    cover = f"""<section class="cover">
  <div class="band">
    <div style="font-size:11pt;letter-spacing:.14em;text-transform:uppercase;opacity:.85">Rigel &middot; Spec Editor</div>
    <h1>Quest Guide</h1>
    <div class="sub">Make quests, wire them to kiosks and buttons, and save them on the backend.</div>
  </div>
  <div><h3>Contents</h3><div class="toc">{toc}</div></div>
  <div class="meta">Orion Drift build 22284 &middot; F12 shows the editor</div>
</section>"""
    body = "".join(f'<section class="chapter"><h2><span class="num">{n}</span>{t}</h2>{b}</section>' for n, t, b in CHAPTERS)
    return f"""<!doctype html><html><head><meta charset="utf-8"><title>Rigel Quest Guide</title>
<style>{bg.CSS}</style></head><body>{cover}{body}</body></html>"""


def main():
    with open(OUT_HTML, "w", encoding="utf-8") as fh:
        fh.write(build_html())
    edge = bg.find_edge()
    if not edge:
        sys.exit("Edge not found; open tools/quest_guide.html and print it to PDF.")
    url = "file:///" + OUT_HTML.replace(os.sep, "/")
    subprocess.run([edge, "--headless=new", "--disable-gpu", "--no-pdf-header-footer", "--no-first-run",
                    f"--user-data-dir={os.path.join(os.environ.get('TEMP', bg.HERE), 'rigel-guide-edge')}",
                    f"--print-to-pdf={OUT_PDF}", url], check=True, timeout=180,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    print(OUT_PDF, os.path.getsize(OUT_PDF), "bytes")


if __name__ == "__main__":
    main()
