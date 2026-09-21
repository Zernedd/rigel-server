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
#include <atomic>

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
    void*       ptr = nullptr;      // the actor -- GAME THREAD ONLY; the render thread must never deref it
    Vec3        boundsOff;          // world bounding box: centre = location + boundsOff, half-size = boundsExt
    Vec3        boundsExt;          // (what clicking in the viewport picks against)
};

// Property kinds, mirroring sereflect::PType (se_reflect.h) so the UI need not include the SDK.
// se_game.cpp static_asserts the two stay in step.
enum PropType { PT_Unsupported, PT_Bool, PT_Float, PT_Double, PT_Int, PT_Int64, PT_Byte, PT_Enum, PT_Name,
                PT_Str, PT_Vector, PT_Rotator, PT_Color, PT_Vector2D, PT_Object, PT_Text };

// One editable property of the inspected object, for the Details panel (see se_reflect.h).
struct PropInfo
{
    std::string path;               // relative to the placed actor: "Duration" or "RedCoinTimedQuest.Duration"
    std::string name, owner, value; // value in the wire encoding
    int         type = 0;           // sereflect::PType
    bool        writable = false;
    bool        link = false;       // an owned component / child actor you can step into
    bool        net = false;        // replicated: every player (Quest too) gets the server's value
    std::vector<std::pair<std::string, long long>> enumNames;
};

struct Snapshot
{
    bool                      inEditor = false;
    bool                      worldReady = false;
    Vec3                      cameraPos;
    Rot                       cameraRot;
    float                     cameraFov = 90.f;   // horizontal, degrees -- from the camera manager's POV
    std::vector<SceneObject>  objects;
    std::vector<PaletteItem>  palette;
    std::string               status;

    // Details > Properties: the object being inspected and its editable properties.
    std::string               inspectHandle;    // the placed actor
    std::string               inspectPath;      // sub-object path inside it ("" = the actor itself)
    std::string               inspectClass;     // class of the object at inspectPath
    std::vector<PropInfo>     props;
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
    SpawnTraced,      // str = palette path; loc = ray origin, dir = ray direction, snap = grid step (0 = off)
                      // placed where the ray first hits level geometry (drag-drop and double-click)
    FocusCamera,      // loc = point to frame (F)
    Inspect,          // str = handle, str2 = sub-object path ("" = the actor) -- what Details shows
    SetProperty,      // str = handle, str2 = property path, str3 = value (wire encoding)
};

struct Command
{
    CmdType     type;
    std::string str;
    std::string str2;
    Vec3        loc;
    Rot         rot;
    Vec3        scale{ 1, 1, 1 };
    Vec3        dir;
    std::string str3;               // QuestCompile: glyph id; SetProperty: value
    int         num = 0;            // QuestCompile: repetition (Once / Daily / Weekly / Monthly)
    int         num2 = 0;           // QuestCompile: valid length, seconds (0 = open-ended)
    float       f1 = 0.0f;          // QuestCompile: required progress (0 = template default)
    std::string str4;               // QuestCompile: description
    float       snap = 0.0f;
    double      fallback = 400.0;   // SpawnTraced: distance to use when the ray hits nothing
};

// ── editor camera input: window thread -> game thread ─────────────────────────────────────────
// The window thread only records what the mouse and wheel did; the game thread owns the camera actor
// and applies it once per frame. Atomics, because this is written from WndProc and read from the
// ProcessEvent hook with no other synchronisation.
struct CameraInput
{
    std::atomic<float> dx{ 0.0f }, dy{ 0.0f };   // accumulated look deltas, pixels
    std::atomic<int>   wheel{ 0 };               // wheel notches while looking (adjusts fly speed)
    std::atomic<bool>  looking{ false };         // RMB held in the viewport
    std::atomic<bool>  active{ false };          // the editor camera is currently the view target
};
CameraInput& Cam();

// ── live gizmo drag: render thread -> game thread, every frame ─────────────────────────────────
// A drag must not wait in the command queue: the render thread writes where the object should be each
// frame, and the game thread moves its local copy in the same frame, keeps holding it there against
// older transforms still arriving from the server, and forwards it to the server at a capped rate.
struct LiveDrag
{
    std::mutex  mx;
    bool        active = false;     // mouse still down
    std::string handle;
    Vec3        loc, scale{ 1, 1, 1 };
    Rot         rot;
    uint32_t    seq = 0;            // bumped on every change, and once more on release
};
LiveDrag& Drag();

// ── viewport picking: render thread -> game thread -> render thread ───────────────────────────
// The render thread publishes the mouse ray every frame; the game thread line-traces it against the real
// level (what you actually see) and answers with the placed object under the cursor. Bounding boxes are
// only the fallback, for objects with no collision (text): among boxes the ray enters in front of the
// traced surface, the smallest wins, so a big neighbour's box cannot steal the click.
struct PickState
{
    std::mutex  mx;
    bool        valid = false;      // the mouse is over the bare viewport
    Vec3        eye, dir;
    std::string hovered;            // answer: handle of the object under the cursor ("" = none)
};
PickState& Pick();

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
    // Per-frame transform refresh between full publishes, so the gizmo and markers track moving objects
    // smoothly instead of stepping at the 4 Hz publish rate. `objs` is the game thread's copy of the
    // last published object list, in the same order.
    void ApplyTransforms(const std::vector<SceneObject>& objs)
    {
        std::lock_guard<std::mutex> lk(m_snapMx);
        auto& dst = m_snapshot.objects;
        const size_t n = dst.size() < objs.size() ? dst.size() : objs.size();
        for (size_t i = 0; i < n; ++i)
            if (dst[i].handle == objs[i].handle)
            { dst[i].location = objs[i].location; dst[i].rotation = objs[i].rotation; dst[i].scale = objs[i].scale; }
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
extern DWORD g_mainThread;    // UE's game thread = the process main thread (recorded in DllMain)

}  // namespace se
