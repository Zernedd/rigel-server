# Rigel Luau scripting kit

Write Luau scripts for objects you place with the Spec Editor, with autocomplete and type
checking in VS Code. The full guide is **[Rigel-Luau-Guide.pdf](Rigel-Luau-Guide.pdf)**.

```
luau/
  README.md                 this file
  Rigel-Luau-Guide.pdf      the guide (setup, API, examples, troubleshooting)
  Rigel-Quest-Guide.pdf     making quests, kiosks, start buttons, saving quests on the backend
  generate_defs.py          builds types/rigel.d.luau from the game's class dump
  types/rigel.d.luau        luau-lsp definitions: components, events, globals
  types/api_index.json      the same API as JSON (used to build the guide's appendix)
  examples/                 small scripts that type-check cleanly against the definitions
                            (07-10: light switch, vanishing platform, blinker, secret door)
  game-scripts/             the game's own scripts (TKBGolf, ScraprunPrime, ...): real API usage
  tools/build_guide.py      rebuilds the PDF
  .vscode/settings.json     luau-lsp settings for this folder
  .luaurc                   strict mode + lint settings
```

## VS Code setup

1. Install **VS Code**.
2. Open the Extensions view (`Ctrl+Shift+X`), search for **Luau Language Server** by
   *JohnnyMorganz* (id `JohnnyMorganz.luau-lsp`), and install it.
3. **File > Open Folder...** and pick this `SpecEditor\luau` folder itself, not the repo root.
   The paths in `.vscode/settings.json` are relative to it.
4. Open `examples/01_hello.luau`. The status bar should show *Luau*. Hover `log` or `VRPawn`:
   you should see their types. If nothing shows up, see *Troubleshooting* below.

The folder's `.vscode/settings.json` sets:

| Setting | Value | Why |
|---|---|---|
| `luau-lsp.platform.type` | `"standard"` | plain Luau, no Roblox globals |
| `luau-lsp.sourcemap.enabled` | `false` | no Rojo project here |
| `luau-lsp.types.definitionFiles` | `{ "@rigel": "types/rigel.d.luau" }` | loads the game API |
| `luau-lsp.ignoreGlobs` | `["game-scripts/**"]` | the dumped game scripts aren't meant to type-check |

`.luaurc` turns on strict mode for every file and switches off the `FunctionUnused` lint,
because the game calls `BeginPlay`/`EndPlay` and the linter can't see that.

### What IntelliSense gives you

* After `local Cup: GolfCupComponent = nil`, typing `Cup:` lists the component's methods
  (`setCupVisibility`, `setOutlineVisibility`, ...) and `Cup.` lists its events
  (`onBallInCup_Multicast`, ...).
* Wrong argument types, misspelt methods and missing arguments are underlined.
* Hover a method to see its Luau signature. The definitions file has the original C++
  declaration as a comment above each method (`types/rigel.d.luau`, *Go to Definition*).
* Globals: `log`, `warn`, `LuauClock`, `Promise`, `Vector`, `VRPawn.*`, `Quests.*`,
  `Gamemode`, `BallSpawnParameters.new`, `LinearColor.new`, `Enum.TextJustify`, ...

### Showing text from a script

The game never hands a `TextComponent` to a script (a `local Label: TextComponent = nil` slot stays nil),
so text is set through a **template** instead:

1. Place a Text object and, in **Details > Game data**, set its Text to something like `Score: {score}`.
2. In any script: `Gamemode:broadcastEventString("score", "3")`.

The server fills `{score}` into every Text object that uses it, for every player (Quest too). Use as many
`{names}` as you like; one not set yet is shown as written. The saved level keeps the template.

`VRPawn.getPlayerName(id)` falls back to the player's own name when the game's lookup says `"None"`.
A desktop spectator (and the server) has no local player, so there it is still `"None"`.

### Rigel checks (the traps IntelliSense can't see)

`tools/rigel-check.ps1` runs in the background as the VS Code task **Rigel: check scripts**
(`.vscode/tasks.json`, started when the folder opens -- VS Code asks once to *Allow Automatic Tasks*).
Every save is re-checked and findings appear in the **Problems** panel, underlined in the file:

* **Typed locals that can't be properties.** Every `local Name: Type = ...` becomes a property of the
  object in the editor. Only `...Component` slots, `number`, `string` and `boolean` can be properties;
  anything else -- `{ [string]: boolean }`, a function type, a union -- crashed the server and every
  player. The server removes such types before anyone runs the script (it still works), but write
  `local Name = ...` instead.
* **`local Name: number = 5` starts as nil.** A typed `number`/`string`/`boolean` is a property, so its
  value comes from the editor, not from the `= 5`. Drop the type to keep the value.
* **`defaultEnabledValue`** is in the definitions but errors at runtime.

Run it by hand: `powershell -File tools/rigel-check.ps1` (add `-Watch` to keep checking).
In the game, the editor also warns about these when a script is attached, and every problem popup has a
**Copy error** button (or Ctrl+C) that puts the full message on the clipboard.

## Quick start

1. Open the Spec Editor (**F12**), select an object, and in **Details > Luau script** click
   **Open folder in VS Code**. This opens `Documents\RigelScripts`, which already holds this kit.
2. Write a script as a `.luau` file there (or copy one from `examples/`). The file name is
   the script's name.
3. Back in the editor, pick the file and click **Attach to this object**. After that, every
   save in VS Code re-sends the script automatically.
4. If the script declares slots (`local Target: PhysicalComponent = nil`), they appear under
   the script in **Details > Game data**: click one, then click the object in the viewport or Outliner (or drag it there, or use the list button).
5. The object is rebuilt and every machine compiles and runs the script. Check the log:
   `%LOCALAPPDATA%\A2\Saved\Logs\A2*.log` should contain
   `LogLuau: Display: [<name>.luau]: Hello from player ...`.
6. Save the level in the **Levels** tab. The script is saved with it and attached again
   when the level loads.

## The API in one screen

```lua
--!strict
-- External Dependencies     (filled in by the game from the object's references wiring)
local Cup: GolfCupComponent = nil
-- End External Dependencies

function BeginPlay()
    log("hello", 42)                                         -- -> LogLuau in every machine's log
    Cup:setCupVisibility(true)                               -- UFunction SetCupVisibility, lowerCamel
    Cup.onBallInCup_Multicast.Listen(function(cup, score, disc) end)   -- event: DOT + Listen
    LuauClock.timeout(2):andThen(function() log("2 s later") end)      -- timer promise
    Promise.new(function() LuauClock.timeout(1):await(); log("1 s") end)
    local v = Vector.new(1, 2, 3) * 2 + Vector.new(0, 0, 1)            -- vector maths, v.x v.y v.z
    local me = VRPawn.getLocalPlayerIndex()                  -- static UFunction -> Type.func()
end
```

Naming rule: the C++ UFunction `UGolfCupComponent::SetCupVisibility(bool Show)` is
`cup:setCupVisibility(show)`. Type names drop the `U`/`A` prefix (`UGolfCupComponent` becomes
`GolfCupComponent`, `ADiscEntity` becomes `DiscEntity`).

## Regenerating the definitions

```
python generate_defs.py
```

This reads `HalcyonA2/HalcyonA2/gamesdk/22284/SDK/*_classes.hpp` and `*_structs.hpp` and
rewrites `types/rigel.d.luau` and `types/api_index.json`. Only the Python standard library is
used. To rebuild the PDF: `python tools/build_guide.py` (needs Microsoft Edge, which ships
with Windows).

## Limits (read before relying on something)

* The method list comes from the game's class dump. Not every listed UFunction is
  necessarily exposed to Luau or safe to call. Try it on a local server first.
* The External Dependencies locals (slots) are filled from the object's `references`
  wiring, which you set by dragging objects onto the slots. A target must be in the same
  gamemode area as the scripted object. An unwired slot stays `nil`, so check for it.
* Definition files use the current luau-lsp syntax (`declare extern type X with ... end`).
  Older luau-lsp releases only understand `declare class`; keep the extension up to date.

## Troubleshooting

* **No completions / "Unknown global 'log'"**: you opened the wrong folder. Open
  `SpecEditor\luau` itself, then run *Luau: Restart Language Server* from the command palette.
* **Where's my output?** In each machine's `%LOCALAPPDATA%\A2\Saved\Logs\A2*.log`:
  `log(...)` prints `LogLuau: Display: [Name.luau]: ...` and `warn(...)` prints
  `LogLuau: Warning: [Name.luau]: ...`. If your line never appears, search the same log for
  your script's name and for `LogLuau` lines: compile and runtime problems should be reported
  there. The exact error format hasn't been captured yet.
* **"Type ... could be nil" / comparing a component with `nil`**: cast first,
  `if (Door :: PhysicalComponent?) == nil then ... end` (see `examples/04_button_toggles_door.luau`).
