# Field notes: things that went wrong

Every mistake, trap and surprise hit while building with the MCP or the game-mode tools. Each one is either
something to fix (**fix**) or something the docs must say (**doc**). When it's dealt with, say how and where.

Format: `date | area | what happened | status`

2026-09-25: every **doc** row below is now in MCP.md ("Building an arena: what goes wrong", tools reference, roles
table, balls, monitors) and GAMEMODES.md (scoreboards, balls, troubleshooting). Still open: monitors binding without
a level reload; flagging the culling cubes in `search_palette`; warning about extra ball spawners in a mode's area.

## Building MiniJakeball (the wall pitch), 2026-09-23/24

| Area | What happened | Status |
|---|---|---|
| Placement | I built the first try on the station's own Jakeball arena instead of where the user's camera was. | **doc**: MCP.md says "build where the camera is (`editor_status.camera`) unless told otherwise". |
| Camera | Moving the user's editor camera to look at my work annoyed them. | **doc**: don't `set_camera` in a user's session without asking; take screenshots with their camera or put it back. |
| Primitives | `Prefab_BP_SmallCubePrimitive_C`, `Prefab_BP_CubePrimitive_Blue_C` and `..._Yellow_C` vanish at some distances (culling). | **doc**: use only the gridded cubes (`Prefab_BP_StandardCubePrimitive_C` navy, `Prefab_BP_Cube2Primitive_C` purple) for arena walls. **fix?**: flag the culling ones in `search_palette`. |
| Start ring | Scaling the cylinder trigger to fit stretched the "PLACE BALL" hologram. | **doc**: keep `Prefab_BP_CylinderPrimitive_Trigger_C` at scale 1, like the station's rings. |
| Arena | The first layout had gaps at the corners, the doorway and the goal mouths. | **doc**: side walls must run past the ends by one wall thickness; fill every doorway with posts and a lintel; a goal needs a box behind it (sides, back, roof). `tests/build_wall_jakeball.py` is the reference. |
| Team doors | The doors faced backwards: walking in left the team. | **fixed** (specedit.h): auto yaw now makes a door's +X point OUT of the arena. **doc**: walk in = join, walk out = leave; yaw 180 in the builder. |
| Tilt | The arena was 0.6° off because pieces used the cylinder's radial "up" at each spot, not the flat wall's normal. | **fixed**: `place_object floor_up` and hit-normal doors. **doc**: on the flat wall pitch pass `floor_up=[1,0,0]`. |
| Second ball | A `TackleballTrainingBallSpawner` already in the level gave a second ball. | **doc**: `list_objects filter=BallSpawner` inside the area before adding a ball; a mode has exactly one. **fix?**: `gamemode_list` could warn about extra spawners in the area. |
| Scoreboard | The monitor's screen faces its local Y, so it was edge-on at yaw 0. | **doc**: `BP_ScoreboardA_C` at yaw 90 to face along X. |
| Scoreboard | It said "Inactive Game Message" until the level was reloaded: monitors only bind to the mode's Game State Manager when the level loads. | **fix (open)**: re-bind without a reload. **doc** for now: `level_save`, `level_close`, `level_open` after adding a monitor. |
| Mode code | My out-of-bounds check hard-coded the wall's axes, so a copy anywhere else would reset the ball every second. | **fixed** in the builder: the frame (centre + u/v/h axes) is written into the code. **doc**: never hard-code world axes in mode code. |
| Checker | `check_luau(gamemode_code=true)` didn't know the slots `StartRing1`, `Goal1`, `ScoreZone1` or `ModeTimer`/`ModeState`/`ModeScore`, so correct code failed ("Unknown global"). | **fixed** (rigel_mcp.py `SLOT_TYPES`): every slot is declared with its real type. |
| Roles doc | `gamemode_set_role`'s role list left out `goal:N`, `score_zone:N` and `start_ring`. | **fixed** (rigel_mcp.py `ROLES`). |
| Ball edits | Moving or resizing the ball rebuilds its spawner, and the mode script's `Ball1` still pointed at the destroyed one: `getSpawnedBall()` gave nil, so the ring (and the goals' ball check) ignored the ball. | **fixed** (se_ui.cpp `GmRewireRebuilt`): the editor re-wires a mode's script when an object it points at is rebuilt. |
| Clients | `BallSpawner:getSpawnedBall()` is nil on clients, and only the server fires the ring's `onOverlapByDisc`. The ring start and goals work because the server runs the script too. | **doc**: ball logic in mode code only works on the server copy; don't build client-side ball checks. |
| Testing | The mock player (PlayerNovBuild) can't move a ball: the server keeps ownership (see memory "a2-22284-ball-ownership"). | **doc**: test ring and goals by moving the ball with `move_object`, which rebuilds the spawner in place. The mock player has `throw x y z [sec]`, but the ball doesn't follow. |
| Goal area | The goal areas came apart because the purple `Prefab_BP_Cube2Primitive_C` is **64 cm per unit of scale**, not 100 like `Prefab_BP_StandardCubePrimitive_C`. Every purple piece came out at 64% size. Measured with the new bounds. | **fixed** in the builder (size per class). **doc**: "a prefab's scale is not centimetres; check `boundsMin/boundsMax` from `list_objects` after placing one". **fixed** (MCP): `list_objects` now returns world bounds. |
| Arena scale | 36 x 22 m with bare walls felt too small and plain next to the station's Jakeball arena. | **fix (open)**: after the logic is confirmed, rebuild larger with more geometry, modelled on the real arena. |
| Testing | No way to carry a ball into a ring or a goal from a test: `move_object` rebuilds the spawner, and the mock player can't hold the ball. | **fixed**: server `SE|BALLPUT` + MCP `ball_carry` (moves the live ball in swept steps). |
| Planning | There was no way to find how far a floor goes or whether it's clear. | **fixed**: MCP `trace` (hit point, normal, actor). The flat wall floor spans y 6300 to 18300 and z -3300 to 3700 (normal +X), clear for 15 m above. |
| Mode hooks | `OnCountdown(seconds)` got 0: the state change runs before the clock updates. | **fixed** (se_ui.cpp): it passes `max(timeLeft, countdown setting)`. |
| Testing | `mcp_full_test.py` failed 16 player checks because I had left the mock player on the wall arena, 28 m from the test area. | **fix**: the test sends the player to its start spot itself before the player checks. |
| Testing | Bounds checks must allow for floor alignment: a cube tilted 0.6 degrees to the hull is 64.6 cm across, not 64. | **fixed** in the test (±2 cm). |
| Testing | With the user's editor running, the local server logs to `A2_2.log`, not `A2.log`, so the test read the wrong machine's log and every mode hook "failed". | **fixed**: the test finds the server's log by its `-nullrhi` command line. |
| Testing | `find_reference_targets near=` right after `attach_script` can come back empty while the host is being rebuilt. | **fixed** in the test (retries); **doc**: after attaching, wait a moment before listing reference targets. |
| Testing | After the door flip (+X points out) the full test still walked along +X through its door, which is walking OUT. It passed only when the path to the start spot happened to cross the door inward. | **fixed**: the test walks in against +X and waits for arrival instead of fixed 9 s legs. |
| Build scripts | Bash heredocs mangle `\n`/`\d` in Python patches. | **doc** (internal): write patch files, don't heredoc them. |

## Earlier in the session

| Area | What happened | Status |
|---|---|---|
| Countdown | A 0 ms countdown never ends (`IsCountdownRunning` counts 0 as "no end"). | **fixed**: always `startTimerWithCountdown(max(1, ms))`. |
| Ball edits | Editing a ball destroyed it; resets of a young ball sent client copies 290 m under the floor. | **fixed**: edits move/scale the spawner; young balls are left alone. |
| MCP | `set_property` sent the wrong command; undo ignored MCP edits; handles went stale after rebuilds. | **fixed**. |
| Luau defs | The types listed members the runtime never binds (212 of VRPawn's 218, BasicButton `setButtonText`/`setOnCooldown`, `isNetworkReady`). | **fixed**: pruned to runtime-verified members. |
| Events | `onOverlapEndByPlayerSimple` never fires; player overlaps fire once per body part; `onOverlapByPlayerServer` fires only on the server. | **doc**: in GAMEMODES.md (events table, polling pattern). |
| Rift | spec29 shipped the dev DLL (no login, UI on). | **fixed**: upload only `build.ps1 -Rift` output. |
