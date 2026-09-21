# Spec Editor

An in-game level editor for A2 / Orion Drift build 22284, built on the systems the build already has
rather than from scratch.

## What it is built on

The build ships a real multi-user level editor and a sandbox authoring stack; this project drives them
from a client mod instead of reimplementing them.

| Build system | What we use it for |
|---|---|
| `AALevelEditor_Minimal` — `ALevelEditorPawn` | the flying editor camera, and its **replicated** RPCs |
| `Server_AttemptLockObject(FString)` | the client→server channel (see *Transport*) |
| `Server_UnlockObject`, `Client_*Lock*` | stock multi-user object locking, unchanged |
| `ULevelEditorMinimalSubsystem` | authoritative map of who is currently editing |
| `LE_*` prefabs (`/Game/A2/Prefabs`, `/Game/A2/Progression/TimedQuest`) | the Place Actors palette |
| `SandboxEngine` (`USandboxProjectQuestData`, `USandboxQuestFile`) | quest authoring target |

## Layout

```
SpecEditor/
  mod/                  the client mod, shipped as dsound.dll
    se_core.h           thread-split spine: snapshot + command queue
    se_render.cpp       D3D12 Present hook, Dear ImGui, Unreal-styled theme
    se_ui.cpp           menu bar, Place Actors, World Outliner, Details, Quests, gizmo
    se_game.cpp         ProcessEvent pump: palette discovery, object list, command dispatch
    se_sdk_glue.cpp     the single SDK symbol we implement to avoid a 5 MB compile
    build.ps1           builds dsound.dll
  ThirdParty/imgui      Dear ImGui 1.91.5 + DX12/Win32 backends
  Install-SpecBuild.ps1 creates SpecEditorBuild/ and installs the mod
```

## Threading — the rule that keeps it from crashing

Two halves that never touch each other's data:

* **Render thread** (inside `IDXGISwapChain::Present`) draws ImGui. It may read the published snapshot
  and push commands. **It must never touch a UObject.**
* **Game thread** (inside `UObject::ProcessEvent`) owns everything UObject. It drains commands and
  republishes the snapshot.

They communicate only through `EditorState`: a mutex-guarded snapshot and a bounded command queue.

## Transport

`ALevelEditorPawn::Server_AttemptLockObject` takes a free-form `FString` and is already replicated, so
editor commands are tunnelled through it with an `SE|` prefix:

```
SE|SPAWN|<class path>|<x,y,z>|<pitch,yaw,roll>
SE|XFORM|<handle>|<x,y,z>|<pitch,yaw,roll>|<sx,sy,sz>
SE|DELETE|<handle>
SE|QUEST|<questId>|<title>|<handle:kind,...>
```

A string **without** the prefix is a genuine lock request and reaches the game's own handler untouched,
so stock multi-user locking still works. Spawning happens on the **server**, with the actor marked
replicated, which is what puts it on the wire through Iris to every client. That is the whole reason
commands are tunnelled rather than executed locally — a client-spawned actor would be local-only.

## Build and install

`ThirdParty/imgui` is a clone and is **not tracked** (it carries its own `.git`). Fetch it once:

```powershell
git clone --branch v1.91.5 --depth 1 https://github.com/ocornut/imgui ThirdParty\imgui
```

```powershell
.\mod\build.ps1                 # -> mod\dsound.dll
.\Install-SpecBuild.ps1         # -> ..\SpecEditorBuild with the mod installed
.\Install-SpecBuild.ps1 -ModOnly  # refresh just the DLL after a rebuild
```

`Install-SpecBuild.ps1` hard-links the source build by default, so a full editor build costs seconds
and almost no disk. It also copies `LibOVRP2P64_1.dll` / `LibOVRPlatformImpl64_1.dll` if the source
build lacks them — `specnovbuild` does, and without them the client exits during start-up in a way that
looks exactly like "the mod broke the game".

Launch, then **INSERT** toggles the UI. Log: `%TEMP%\spec_editor.log`.

Controls: `Q` select, `W` move, `E` rotate, `R` scale, `Esc` deselect, `Del` delete. Drag a gizmo axis
handle to transform; snapping is configurable in Details.

## Status

Working: the mod builds and installs; ImGui overlay with an Unreal-styled theme; Place Actors palette
discovered from the build's own `LE_*` prefabs; World Outliner with lock state; Details with numeric
transform; interactive drag gizmo; quest authoring UI.

**Not yet wired: the server half.** The client emits `SE|…` commands but nothing on the server parses
them, so spawning and quest publishing do not happen yet. That handler belongs in the HalcyonA2 payload
and is written but not installed — it was held back because a server that executes commands from a
client-supplied string is an RCE-shaped surface. Before it ships it should be gated on: server opt-in
(`-SpecEdit`, default off) **and** an allowlist of editor user ids, so an arbitrary client cannot spawn
or destroy actors. See the note in the session log.

The mod has also not yet been confirmed running in a live client.
