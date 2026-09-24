"""make_example_levels.py -- writes t_ex_<Name>.txt: editor test scripts that BUILD each example game mode level
(mode, team changers, pieces, roles, signs, scoreboards, the example's code applied), play a round, and save it
as Documents\\RigelLevels\\<Name>.a2level. The saved levels ship with the kit (SpecEditor/luau/levels).

    python make_example_levels.py      then run each t_ex_*.txt through the suite (fresh server per level)
"""
import os

HERE = os.path.dirname(os.path.abspath(__file__))
C = (0.0, 2500.0, -28625.0)           # the mode's centre: the flat floor in front of the spawn hub (traced: z -28625 over x +-2000, y 1000..4000)
MODE = "RigelMode1"                   # a fresh server's first mode

# class substring used by gmrole -> the SBADD id
CLS = {"BasicButton": "BP_BasicButton_C", "Scoreboard": "BP_ScoreboardA_C", "Scoreboard_OLD": "BP_Score_C",
       "ScoreboardSideboard": "LE_BP_ScoreboardA_Sideboard_C", "DataTable": "LE_BP_TableScoreboard_C",
       "aa_se_LE_BP_Text": "LE_BP_Text_C", "Timer": "BP_Timer_C", "TimerDisplay": "BP_TimerDisplay_C",
       "ForceFieldA": "BP_ForceFieldA_C", "TrapTrapDoor": "BP_Trap_TrapDoor_C", "TrapFallingBlock": "BP_Trap_FallingBlock_C",
       "TrapFlipper": "BP_Trap_Flipper_C", "TrapSpinner": "BP_Trap_Spinner_C", "TrapLaser": "BP_Trap_Laser_C",
       "JakeBallSpawner": "BP_JakeBallSpawner_C"}

DISPLAYS = {"Scoreboard", "Scoreboard_OLD", "ScoreboardSideboard", "DataTable", "aa_se_LE_BP_Text", "TimerDisplay"}

EX = {
    "FirstToTen": dict(
        rules={"start_mode": "button", "score_to_win": "10", "round_time": "0", "countdown": "3"},
        objs=[("BasicButton", (-1300, 0, 0), "score:1", None), ("BasicButton", (0, -800, 0), "start", None),
              ("BasicButton", (1300, 0, 0), "score:2", None), ("Scoreboard", (0, 1300, 250), None, None),
              ("Scoreboard_OLD", (1300, 1100, 0), "score_board", None), ("aa_se_LE_BP_Text", (-1300, 1100, 250), None, "{Leader}")]),
    "AutoStartLobby": dict(
        rules={"start_mode": "manual", "countdown": "5", "round_time": "90", "auto_restart": "1", "custom.min_each": "1"},
        objs=[("ForceFieldA", (0, -500, 0), "wall_lobby", None), ("Timer", (700, -900, 0), "timer", None),
              ("TimerDisplay", (1300, -900, 150), None, None), ("aa_se_LE_BP_Text", (-700, -900, 250), None, "{Lobby}"),
              ("BasicButton", (-1300, 600, 0), "score:1", None), ("BasicButton", (1300, 600, 0), "score:2", None),
              ("Scoreboard", (0, 1300, 250), None, None)]),
    "SuddenDeath": dict(
        rules={"start_mode": "button", "round_time": "120", "countdown": "3"},
        objs=[("BasicButton", (-1300, 0, 0), "score:1", None), ("BasicButton", (0, -800, 0), "start", None),
              ("BasicButton", (1300, 0, 0), "score:2", None), ("Timer", (700, -800, 0), "timer", None),
              ("aa_se_LE_BP_Text", (0, 400, 250), None, "{Note}"), ("DataTable", (0, 1300, 150), "score_table", None),
              ("Scoreboard", (-1300, 1200, 250), None, None)]),
    "TrapGauntlet": dict(
        rules={"start_mode": "button", "round_time": "90", "countdown": "3"},
        objs=[("BasicButton", (-600, -1000, 0), "trap_button:5", None), ("BasicButton", (-400, 1400, 0), "score:1", None),
              ("BasicButton", (0, -1000, 0), "start", None), ("BasicButton", (400, 1400, 0), "score:2", None),
              ("TrapTrapDoor", (0, -400, 0), "trap_fired", None), ("TrapFallingBlock", (0, 200, 300), "trap_fired", None),
              ("TrapFlipper", (0, 800, 0), "trap_fired", None), ("TrapSpinner", (-1300, 200, 0), "trap_round", None),
              ("TrapLaser", (1300, 200, 0), "trap_pulse:3", None), ("Timer", (600, -1000, 0), "timer", None),
              ("aa_se_LE_BP_Text", (-1300, -1000, 250), None, "{Warning}")]),
    "BallBattle": dict(
        rules={"start_mode": "button", "score_to_win": "5", "round_time": "300", "countdown": "3"},
        objs=[("BasicButton", (-1500, 1100, 0), "score:1", None), ("BasicButton", (0, -900, 0), "start", None),
              ("BasicButton", (1500, 1100, 0), "score:2", None), ("JakeBallSpawner", (0, 200, 100), "ball", None),
              ("Scoreboard", (0, 1400, 250), None, None), ("Scoreboard_OLD", (1400, -900, 0), "score_board", None),
              ("aa_se_LE_BP_Text", (-1400, -900, 250), None, "{Match}")]),
    "ScoreboardRace": dict(
        rules={"start_mode": "button", "round_time": "45", "countdown": "3"},
        objs=[("BasicButton", (-800, -200, 0), "score:1", None), ("BasicButton", (0, -900, 0), "start", None),
              ("BasicButton", (800, -200, 0), "score:2", None), ("Scoreboard", (0, 1300, 250), None, None),
              ("ScoreboardSideboard", (-1400, 1100, 150), None, None), ("Scoreboard_OLD", (1400, 1100, 0), "score_board", None),
              ("DataTable", (-700, 600, 150), "score_table", None), ("aa_se_LE_BP_Text", (700, 600, 250), None, "{Race}"),
              ("aa_se_LE_BP_Text", (0, 400, 350), None,
               "{ScoreboardRace.team1.name} {ScoreboardRace.team1.score} - {ScoreboardRace.team2.score} {ScoreboardRace.team2.name}")]),
}


def at(off):
    return (C[0] + off[0], C[1] + off[1], C[2] + off[2])


# Team doors: the team changers stand on the floor facing the spawn hub (players arrive from -Y and walk through
# along +Y: yaw 90), each framed like a doorway -- two posts outside the 3.5 m opening and a beam over the top, in
# the team's colour -- with the team's name on a sign above. Nothing crosses the opening.
DOORS = [(1, (-700, -1300), "PrimitiveCubeBlue", "Prefab_BP_CubePrimitive_Blue_C"),
         (2, (700, -1300), "PrimitiveCubeYellow", "Prefab_BP_CubePrimitive_Yellow_C")]
POST_X, POST_H, BEAM_Z = 200.0, 300.0, 320.0      # cm: post centre off the door's middle, post height, beam height


def door_lines(name):
    L = []
    for team, (dx, dy), cube, cube_cls in DOORS:
        x, y, z = at((dx, dy, 0))
        L.append(f"raw SE|GMTEAM|{MODE}|{team}|{x:.0f},{y:.0f},{z:.0f}|90")
        parts = [((x - POST_X, y, z + POST_H / 2), (0.4, 0.4, POST_H / 100.0)),
                 ((x + POST_X, y, z + POST_H / 2), (0.4, 0.4, POST_H / 100.0)),
                 ((x, y, z + BEAM_Z), (2 * POST_X / 100.0 + 0.4, 0.4, 0.4))]
        for (px, py, pz), _ in parts:
            L.append(f"raw SE|SBADD|{cube}|{px:.1f},{py:.1f},{pz:.1f}")
        L.append("wait 4")
        for (px, py, pz), (sx, sy, sz) in parts:
            L.append(f"raw SE|XFORM|{cube_cls}@{px:.3f},{py:.3f},{pz:.3f}|{px:.1f},{py:.1f},{pz:.1f}|0,0,0|{sx:.2f},{sy:.2f},{sz:.2f}")
        sx, sy, sz = x, y, z + BEAM_Z + 70
        L.append(f"raw SE|SBADD|aa_se_LE_BP_Text|{sx:.1f},{sy:.1f},{sz:.1f}")
        L.append("wait 3")
        L.append(f"pickat LE_BP_Text_C {sx:.0f} {sy:.0f} {sz:.0f}")
        L.append("rotate 180")
        L.append("wait 2")
        L.append(f"raw SE|SBSET|LE_BP_Text_C@{sx:.3f},{sy:.3f},{sz:.3f}|props/Text|text|{{{name}.team{team}.name}} team")
    return L


for name, ex in EX.items():
    L = [f"log EXLEVEL {name} start", "enter", "wait 3",
         f"raw SE|GMNEW|{name}|{C[0]:.0f},{C[1]:.0f},{C[2]:.0f}|2|4,4|Blue,Red", "wait 10"]
    for k, v in ex["rules"].items():
        L.append(f"raw SE|GMSET|{MODE}|{k}|{v}")
    L += door_lines(name)
    for uid, off, role, text in ex["objs"]:
        x, y, z = at(off)
        L.append(f"raw SE|SBADD|{uid}|{x:.1f},{y:.1f},{z:.1f}")
    L.append("wait 10")
    # roles: n-th object of a class, by x
    by_cls = {}
    for uid, off, role, text in ex["objs"]:
        by_cls.setdefault(CLS[uid], []).append((off[0], role))
    for cls, items in by_cls.items():
        for n, (_, role) in enumerate(sorted(items, key=lambda t: t[0])):
            if role:
                L.append(f"gmrole {cls} {name} {role} {n}")
    # displays face the players, who come from the spawn hub (-Y): turn them around
    for uid, off, role, text in ex["objs"]:
        if uid in DISPLAYS:
            x, y, z = at(off)
            L.append(f"pickat {CLS[uid]} {x:.0f} {y:.0f} {z:.0f}")
            L.append("rotate 180")
            L.append("wait 2")
    for uid, off, role, text in ex["objs"]:
        if text:
            x, y, z = at(off)
            L.append(f"raw SE|SBSET|LE_BP_Text_C@{x:.3f},{y:.3f},{z:.3f}|props/Text|text|{text}")
    L += ["wait 6", f"gmapply {name}", "wait 10",
          f"raw SE|GMCTL|{MODE}|start", "wait 8", f"expectgm {name} running",
          f"raw SE|GMCTL|{MODE}|reset", "wait 4", f"expectgm {name} idle",
          f"scene saveas {name}", "wait 10", f"expectlvfile {name} 1", f"log EXLEVEL {name} done"]
    with open(os.path.join(HERE, f"t_ex_{name}.txt"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    print("wrote", f"t_ex_{name}.txt", len(L), "lines")
