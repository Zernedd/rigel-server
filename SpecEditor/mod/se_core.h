// se_core.h - shared spine of the Spec Editor mod.
//
// The editor is split so that the two halves can never be confused with one another, because getting
// that wrong is the classic way a UE mod crashes:
//
//   * RENDER THREAD  - everything ImGui. Runs inside our IDXGISwapChain::Present hook. It may read the
//                      snapshot the game thread published, and it may push commands onto the queue.
//                      It must never touch a UObject.
//   * GAME THREAD    - everything UObject. Runs inside our ProcessEvent hook. It drains the command
//                      queue, performs spawns/transform edits/RPCs, and republishes the snapshot.
//
// The two communicate only through EditorState below: a double-buffered snapshot the render thread
// reads, and a lock-free-ish command queue the render thread writes. No UObject pointer is ever
// dereferenced off the game thread.

#pragma once

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <string>
#include <vector>
#include <mutex>

namespace se {

void Log(const char* fmt, ...);          // %TEMP%\spec_editor.log

// ── what the render thread is allowed to know about the world ────────────────────────────────
struct Vec3 { double x = 0, y = 0, z = 0; };
struct Rot  { double pitch = 0, yaw = 0, roll = 0; };

// One placeable entry in the palette, discovered from the game's own /Game/A2/LevelEditor and
// /Game/A2/Prefabs content (the LE_* prefabs) rather than hardcoded.
struct PaletteItem
{
    std::string name;        // display name, e.g. "LE_BP_RedCoin"
    std::string path;        // full package path used to load the class
    std::string category;    // derived from the folder, e.g. "Quests", "Progression"
};

// A live object in the editor's world view. Handle is the FString index the game's own level editor
// locking protocol uses (ALevelEditorPawn::Server_AttemptLockObject takes exactly this).
struct SceneObject
{
    std::string handle;
    std::string label;
    std::string className;
    Vec3        location;
    Rot         rotation;
    Vec3        scale{ 1, 1, 1 };
    bool        lockedByMe = false;
    bool        lockedByOther = false;
};

struct Snapshot
{
    bool                      inEditor = false;
    bool                      worldReady = false;
    Vec3                      cameraPos;
    Rot                       cameraRot;
    std::vector<SceneObject>  objects;
    std::vector<PaletteItem>  palette;
    std::string               status;
};

// ── commands: render thread -> game thread ───────────────────────────────────────────────────
enum class CmdType
{
    EnterEditor,
    ExitEditor,
    SpawnItem,        // str = palette path, loc/rot = where
    SelectObject,     // str = handle (asks the server for the lock)
    DeselectObject,   // str = handle
    SetTransform,     // str = handle, loc/rot/scale
    DeleteObject,     // str = handle
    RefreshPalette,
    QuestAddStep,     // str = handle of the object to bind as a quest step
    QuestCompile,     // push the authored quest to the server
};

struct Command
{
    CmdType     type;
    std::string str;
    std::string str2;
    Vec3        loc;
    Rot         rot;
    Vec3        scale{ 1, 1, 1 };
};

class EditorState
{
public:
    // render thread
    Snapshot ReadSnapshot()
    {
        std::lock_guard<std::mutex> lk(m_snapMx);
        return m_snapshot;
    }
    void Push(Command c)
    {
        std::lock_guard<std::mutex> lk(m_cmdMx);
        if (m_commands.size() < 256) m_commands.push_back(std::move(c));
    }

    // game thread
    void Publish(Snapshot s)
    {
        std::lock_guard<std::mutex> lk(m_snapMx);
        m_snapshot = std::move(s);
    }
    std::vector<Command> Drain()
    {
        std::lock_guard<std::mutex> lk(m_cmdMx);
        std::vector<Command> out;
        out.swap(m_commands);
        return out;
    }

private:
    std::mutex           m_snapMx;
    std::mutex           m_cmdMx;
    Snapshot             m_snapshot;
    std::vector<Command> m_commands;
};

EditorState& State();

// ── the two halves ───────────────────────────────────────────────────────────────────────────
bool InstallRenderHook();     // D3D12 swapchain Present -> ImGui
bool InstallGameHook();       // UObject::ProcessEvent -> game-thread pump
void DrawEditorUI();          // render thread; the whole UE-styled editor
void GameThreadPump();        // game thread; drains commands, republishes the snapshot

extern bool g_uiVisible;      // INSERT toggles

}  // namespace se
