// se_game.cpp - the game-thread half. Everything UObject happens here, inside our ProcessEvent hook.
//
// HOW THE CLIENT TALKS TO THE SERVER
// ---------------------------------
// Editor commands are tunnelled through replicated client->server RPCs that already take a free-form
// string, rather than inventing netcode:
//
//   * AVRPlayerController::Server_SetVivoxParticipantID -- the normal path. Every client owns its
//     controller (spectators too), so the editor works from anywhere on the station.
//   * ALevelEditorPawn::Server_AttemptLockObject -- fallback, only while inside the game's own editor pawn.
//
//     SE|SPAWN|<class path>|<x>,<y>,<z>|<pitch>,<yaw>,<roll>
//     SE|XFORM|<Class>@<x>,<y>,<z>|<x>,<y>,<z>|<pitch>,<yaw>,<roll>|<sx>,<sy>,<sz>
//     SE|DELETE|<Class>@<x>,<y>,<z>
//     SE|QUEST|<questId>|<title>|<handle:kind>,<handle:kind>,...
//     SE|ENTER / SE|EXIT
//
// Actors are addressed as Class@position, never by name: UE does not replicate actor names, so a server-
// spawned actor has a different name here and a name sent back would resolve to nothing (see IdentFor).
//
// The server-side counterpart lives in the HalcyonA2 payload (specedit.h): it claims anything starting
// "SE|" and does the work there. Because the actor is spawned on the SERVER it replicates to every client
// through Iris with no extra work -- exactly why commands are tunnelled rather than spawned locally.
//
// Selection inside the game's editor pawn still uses the stock lock protocol (a bare handle to
// Server_AttemptLockObject), so two people editing cannot grab the same actor. A bare string is never
// sent through the Vivox RPC, where it would genuinely become the player's voice participant id.

#include "se_core.h"
#include "../../HalcyonA2/HalcyonA2/gamesdk/22284/SDK.hpp"
#include "../../HalcyonA2/HalcyonA2/gamesdk/22284/SDK/Engine_parameters.hpp"
#include "se_reflect.h"
#include "MinHook.h"

static_assert(static_cast<int>(sereflect::PType::Object) == se::PT_Object &&
              static_cast<int>(sereflect::PType::Enum) == se::PT_Enum &&
              static_cast<int>(sereflect::PType::Vector) == se::PT_Vector,
              "se::PropType must mirror sereflect::PType");
#include <cmath>
#include <unordered_set>
#include <unordered_map>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <algorithm>
#include <cstdlib>

namespace se {
namespace {

typedef void(__fastcall* ProcessEvent_t)(void*, void*, void*);
ProcessEvent_t g_peOrig = nullptr;

SDK::UClass* g_lePawnClass = nullptr;
SDK::UClass* g_leSubsysClass = nullptr;
bool         g_paletteDirty = true;
ULONGLONG    g_lastPump = 0;
bool         g_editorMode = false;   // client-side "Enter Level Editor" state
SDK::UFunction* g_setQuestsFn = nullptr;   // A2PlayerQuestComponent::Client_SetQuests (see LogIncomingQuests)
std::vector<Snapshot::GameScript> g_gameScripts;   // ScanScripts result
std::string g_dumpScriptsTo;                       // test op: also write the scan to this folder
std::vector<Snapshot::LevelInfo> g_levels;       // saved levels (SE|LVLIST), for the Levels tab
// Where this editor's camera is, for the marker everyone else sees (SE|CAMPOS): ~8/s while it moves or
// turns, a heartbeat every 5 s while it sits still (the server drops a marker 20 s after the last report).
struct CamReport { double x, y, z, p, yw, r; };
CamReport CamPose();                                     // below (the editor camera's pose)
bool      CamActive();                                   // below
void SendToServer(const std::string& payload);           // below
void CamReportTick()
{
    static ULONGLONG s_last = 0;
    static CamReport s_sent{ 1e30, 0, 0, 0, 0, 0 };
    extern bool g_editorMode;
    if (!g_editorMode || !CamActive()) { s_sent.x = 1e30; return; }
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 125) return;
    const CamReport c = CamPose();
    const double dx = c.x - s_sent.x, dy = c.y - s_sent.y, dz = c.z - s_sent.z;
    const bool moved = dx * dx + dy * dy + dz * dz > 25.0 || std::fabs(c.yw - s_sent.yw) > 2.0 || std::fabs(c.p - s_sent.p) > 2.0 ||
                       std::fabs(c.r - s_sent.r) > 2.0;
    if (!moved && now - s_last < 5000) return;
    s_last = now;
    s_sent = c;
    char b[160];
    snprintf(b, sizeof(b), "SE|CAMPOS|%.1f,%.1f,%.1f|%.1f,%.1f,%.1f", c.x, c.y, c.z, c.p, c.yw, c.r);
    SendToServer(b);
}

// Commands paced out to the server (it takes at most 60 a second): a level upload is dozens of parts.
void SendToServer(const std::string& payload);   // below
std::vector<std::string> g_paced;
size_t g_pacedAt = 0;
void PacedTick()
{
    static ULONGLONG s_last = 0;
    if (g_pacedAt >= g_paced.size()) { if (!g_paced.empty()) { g_paced.clear(); g_pacedAt = 0; } return; }
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 30) return;                                 // ~33/s, leaving room for everything else
    s_last = now;
    SendToServer(g_paced[g_pacedAt++]);
}
// Level export arriving from the server in hex chunks: name -> parts.
std::unordered_map<std::string, std::vector<std::string>> g_lvExportParts;

// Owner locks the server told us about (SE|OWNLOCKS): sandbox id -> whose, and whether it is ours.
struct OwnLockInfo { bool mine = false; std::string owner; };
std::unordered_map<std::string, OwnLockInfo> g_ownLockMap;
// The sandbox id at the front of a handle (<GUID> or <GUID>_<n>), or "".
std::string HandleGuid(const std::string& h)
{
    if (h.size() < 36 || h[8] != '-' || h[13] != '-' || h[18] != '-' || h[23] != '-') return std::string();
    if (h.size() > 36 && h[36] != '_') return std::string();
    return h.substr(0, 36);
}
// Someone else's owner lock is on this object: don't move, delete or predict it here either (the server
// refuses, and a locally predicted move would leave this client showing it somewhere it isn't).
bool LockedByOther(const std::string& handle)
{
    if (g_ownLockMap.empty()) return false;
    auto it = g_ownLockMap.find(HandleGuid(handle));
    if (it == g_ownLockMap.end() || it->second.mine) return false;
    static ULONGLONG s_last = 0;
    if (GetTickCount64() - s_last > 1500) { s_last = GetTickCount64(); Notes().Set("Locked by " + it->second.owner + " - only they can change it."); }
    return true;
}
int g_levelsSerial = 0;
std::vector<Snapshot::QuestRef> g_knownQuests;   // every quest row seen, for the Game data quest pickers
std::string g_dataHandle, g_dataIdent;             // Game data: the object asked about, and how we named it
std::vector<Snapshot::DataEntry> g_data;
int g_dataSerial = 0;
std::vector<std::string> g_glyphs;        // quest icon ids seen in any bundle (for the Quest Editor's picker)
SDK::UFunction* g_clientMsgFn = nullptr;   // APlayerController::ClientMessage (see ApplyRemoteProp)

struct PendingStep { std::string handle; int kind; };
std::vector<PendingStep> g_questSteps;

bool SafePE(SDK::UObject* obj, SDK::UFunction* fn, void* parms)
{
    if (!obj || !fn || !g_peOrig) return false;
    __try { g_peOrig(obj, fn, parms); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("[game] ProcessEvent faulted on %s", "fn"); return false; }
}

// The local editor pawn, if we are in the level editor. The subsystem keeps the authoritative map of
// who is editing, so ask it rather than guessing from the object list.
SDK::UObject* FindLocalEditorPawn()
{
    if (!g_lePawnClass) g_lePawnClass = SDK::UObject::FindClassFast("LevelEditorPawn");
    if (!g_lePawnClass) return nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(g_lePawnClass)) continue;
        // Only the locally controlled one has a live camera + movement component wired up.
        void* cam = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x3F8);  // FollowCamera
        if (cam) return o;
    }
    return nullptr;
}

// The local player's controller. Only OUR controller exists on a client (other players' controllers
// are never replicated), so the one live VRPlayerController instance is ours. Cached and re-validated
// through its GObjects slot so the 4 Hz pump is not a full object walk every time.
SDK::UClass*  g_vrpcClass = nullptr;
SDK::UObject* g_pcCache   = nullptr;

SDK::UObject* FindLocalController()
{
    if (g_pcCache)
    {
        auto* again = SDK::UObject::GObjects->GetByIndex(g_pcCache->Index);
        if (again == g_pcCache && !g_pcCache->IsDefaultObject()) return g_pcCache;
        g_pcCache = nullptr;
    }
    if (!g_vrpcClass) g_vrpcClass = SDK::UObject::FindClassFast("VRPlayerController");
    if (!g_vrpcClass) return nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(g_vrpcClass)) continue;
        g_pcCache = o;
        return o;
    }
    return nullptr;
}

// Set by the pump every pass: the transports available right now.
SDK::UObject* g_pc      = nullptr;   // always present once in a world
SDK::UObject* g_lePawn  = nullptr;   // only while the game has us in its own level-editor pawn

bool CallStringRpc(SDK::UObject* target, const char* cls, const char* fnName, const std::string& s)
{
    auto* fn = target && target->Class ? target->Class->GetFunction(cls, fnName) : nullptr;
    if (!fn) { Log("[game] %s::%s not found", cls, fnName); return false; }
    std::wstring w(s.begin(), s.end());
    struct { SDK::FString v; } parms{};
    parms.v = SDK::FString(w.c_str());
    return SafePE(target, fn, &parms);
}

// Send one "SE|..." editor command. The normal transport is the player controller's
// Server_SetVivoxParticipantID: every client owns its controller, spectators included, so the editor
// works from anywhere on the station. (The first version used only the level-editor pawn's
// Server_AttemptLockObject, which a player on the station is never in -- so "Enter Level Editor" could
// not even be sent.) The server claims anything starting "SE|"; a real Vivox id never does.
void SendToServer(const std::string& payload)
{
    if (payload.rfind("SE|", 0) != 0) { Log("[game] refusing to send a non-editor string"); return; }
    if (g_pc && CallStringRpc(g_pc, "VRPlayerController", "Server_SetVivoxParticipantID", payload)) return;
    if (g_lePawn && CallStringRpc(g_lePawn, "LevelEditorPawn", "Server_AttemptLockObject", payload)) return;
    Log("[game] no transport (no controller yet) - dropped: %.60s", payload.c_str());
}

// The stock multi-user lock. Only meaningful inside the game's own editor pawn -- and it must never go
// through the Vivox RPC, where a bare string would genuinely become your voice participant id.
void SendLock(const std::string& handle, bool lock)
{
    if (!g_lePawn) return;   // outside the editor pawn, selection is purely client-side
    CallStringRpc(g_lePawn, "LevelEditorPawn", lock ? "Server_AttemptLockObject" : "Server_UnlockObject", handle);
}

std::string Fmt3(const Vec3& v)
{
    char b[96];
    snprintf(b, sizeof(b), "%.3f,%.3f,%.3f", v.x, v.y, v.z);
    return b;
}
std::string Fmt3(const Rot& r)
{
    char b[96];
    snprintf(b, sizeof(b), "%.3f,%.3f,%.3f", r.pitch, r.yaw, r.roll);
    return b;
}

// The full catalogue of LE prefabs the game ships (le_catalogue.txt, beside the exe; generated by
// tools/make_catalogue.py). Read once. Without it the palette could only offer classes already loaded in
// memory -- 2 of ~97 on the station -- because a class has to be loaded to be found by walking GObjects.
// The server loads a catalogue class on demand when one is spawned.
// From the server-side SE|AUDIT sweep of all 97 LE prefabs. BLOCKED: the blueprint graph calls into a
// LuauBlueprintComponent -- the sandbox's Luau runtime, which only binds for prefabs the sandbox engine
// spawned itself; placed by the editor they are broken and interacting with one crashed players' games (the
// server refuses them too). LIMITED: they place and show fine, but their behaviour is Luau-driven.
void ClassifyPrefab(PaletteItem& it)
{
    static const char* blocked[] = { "LE_BP_LightSwitch_C", "LE_BP_Teleporter_C" };
    static const char* limited[] = { "LE_BP_ArenaModerationPanel_C", "LE_BP_TableScoreboard_C", "LE_BP_ScoreboardA_Sideboard_C",
                                     "LE_BP_VFX_TackleBallGoal01a_C", "LE_BP_QuestDisplayKiosk_C", "LE_BP_RockWallQuestManager_C",
                                     "LE_BP_DiscGolfHole_C" };
    // No longer refused here: the server places these through the game's own sandbox system when it runs
    // with -SpecEditSandbox, and otherwise refuses them itself and says why (SE|NOTE).
    for (const char* b : blocked)
        if (it.name == b) it.limited = "Scripted by the game's sandbox. It works when the server places it through the sandbox "
                                       "system; a server without that refuses it (it used to crash players who touched it).";
    for (const char* l : limited)
        if (it.name == l) it.limited = "Places and displays fine, but its behaviour (scores, panels, quest logic) comes from the "
                                       "game's sandbox scripts, which don't run for editor-placed objects.";
}

const std::vector<PaletteItem>& Catalogue()
{
    static std::vector<PaletteItem> s_items;
    static bool s_loaded = false;
    if (s_loaded) return s_items;
    s_loaded = true;

    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const size_t cut = p.find_last_of(L'\\');
    if (cut != std::wstring::npos) p = p.substr(0, cut);
    p += L"\\le_catalogue.txt";

    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r") || !f) { Log("[game] no le_catalogue.txt beside the exe - palette limited to loaded classes"); return s_items; }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty() || s[0] == '#') continue;
        const size_t bar = s.find('|');
        if (bar == std::string::npos) continue;
        PaletteItem it;
        it.category = s.substr(0, bar);
        it.path     = s.substr(bar + 1);
        const size_t dot = it.path.find_last_of('.');
        it.name = dot == std::string::npos ? it.path : it.path.substr(dot + 1);
        ClassifyPrefab(it);
        if (it.name.rfind("LE_", 0) == 0) s_items.push_back(std::move(it));
    }
    fclose(f);
    Log("[game] catalogue: %d LE prefab(s) from le_catalogue.txt", (int)s_items.size());
    return s_items;
}

// Every prefab the game's own sandbox can place (USandboxEngine::RawPrefabs @0xF8: definition +0x30
// Blueprint class, +0x38 Settings, Settings +0xF0 UniqueID). Far more than the LE_ catalogue: boost pads,
// traps, sliding platforms, gravity volumes, force fields, buttons, timers... The server places these
// through the sandbox, so every client builds them with their scripts.
struct SandboxPrefab { std::string cls, path, uniqueId; };
const std::vector<SandboxPrefab>& SandboxPrefabs()
{
    static std::vector<SandboxPrefab> s_list;
    if (!s_list.empty()) return s_list;
    auto* sbCls = SDK::UObject::FindClassFast("SandboxEngine");
    SDK::UObject* sb = nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; sbCls && i < n && !sb; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(sbCls)) sb = o;
    }
    if (!sb) return s_list;
    const uintptr_t b = reinterpret_cast<uintptr_t>(sb);
    SDK::UObject** rp = *reinterpret_cast<SDK::UObject***>(b + 0xF8);
    const int raw = *reinterpret_cast<int32_t*>(b + 0xF8 + 8);
    for (int i = 0; rp && i < raw && i < 1000; ++i)
    {
        SDK::UObject* def = rp[i];
        if (!def || IsBadReadPtr(def, 0x40)) continue;
        SDK::UObject* bp = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x30);
        SDK::UObject* st = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x38);
        if (!bp || !st) continue;
        const std::string full = bp->GetFullName();          // "BlueprintGeneratedClass /Game/...Asset.Asset_C"
        const size_t sp = full.find(' ');
        s_list.push_back({ bp->GetName(), sp == std::string::npos ? full : full.substr(sp + 1),
                           reinterpret_cast<SDK::FName*>(reinterpret_cast<uintptr_t>(st) + 0xF0)->ToString() });
    }
    if (!s_list.empty()) Log("[game] sandbox prefabs: %d", (int)s_list.size());
    return s_list;
}
bool IsSandboxClass(const std::string& cls)
{
    static std::unordered_set<std::string> s_set;              // hot: asked for every actor, every refresh
    static ULONGLONG s_tried = 0;
    if (s_set.empty() && GetTickCount64() - s_tried > 3000)    // registry not filled yet: retry, not per call
    {
        s_tried = GetTickCount64();
        for (const auto& p : SandboxPrefabs()) s_set.insert(p.cls);
    }
    return s_set.count(cls) != 0;
}
// A palette category for a sandbox prefab from its UniqueID.
std::string SandboxCategory(const std::string& id, const std::string& cls)
{
    auto has = [&](const char* k) { return id.find(k) != std::string::npos || cls.find(k) != std::string::npos; };
    if (has("Boost")) return "Parkour - Boost pads";
    if (has("Trap") || has("Sliding") || has("Gravity") || has("HandHold") || has("Deathrun")) return "Parkour - Traps & movers";
    if (has("ForceField") || has("Shield")) return "Force fields";
    if (has("Primitive") || has("Cube") || has("Sphere") || has("Cylinder")) return "Shapes";
    if (has("Golf")) return "Golf";
    if (has("Quest") || has("Progression") || has("RedCoin") || has("Kiosk")) return "Quests";
    if (has("Trigger") || has("Button") || has("Timer") || has("Repeater") || has("NetVar") || has("Speaker") ||
        has("Sound") || has("Swapper") || has("Switch") || has("Teleporter") || has("Text")) return "Logic & interaction";
    if (has("Ball") || has("Goal") || has("Score") || has("team") || has("Team") || has("Tackleball") || has("driftball") ||
        has("Passer") || has("Ticket") || has("GameState") || has("Volley") || has("Net")) return "Sports & games";
    if (id.rfind("aa_se_SM_", 0) == 0) return "";           // meshes: already in the catalogue by folder
    return "Sandbox - other";
}

// Placeable items: the whole catalogue, plus any LE class loaded in memory that it does not list (so a
// newer build's prefabs still show up even before the catalogue is regenerated).
void BuildPalette(Snapshot& snap)
{
    snap.palette = Catalogue();
    auto have = [&](const std::string& name) {
        for (const auto& it : snap.palette) if (it.name == name) return true;
        return false;
    };

    int extra = 0;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n && snap.palette.size() < 1024; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o) continue;
        const std::string name = o->GetName();
        if (name.rfind("LE_", 0) != 0) continue;            // the level-editor prefab convention
        if (name.find("_C") == std::string::npos) continue;  // want the generated classes
        if (have(name)) continue;
        const std::string full = o->GetFullName();
        // Only the CLASS itself. A placed instance is also named LE_..._C_<n>, but the server resolves the
        // leaf as a class name, so an instance in the palette could never be spawned.
        if (full.rfind("BlueprintGeneratedClass ", 0) != 0) continue;

        PaletteItem it;
        it.name = name;
        it.category = "Loaded (not in catalogue)";
        // Full name is "Class /Game/Path/Asset.Asset_C"; keep the path part for the server to load.
        const size_t sp = full.find(' ');
        it.path = (sp == std::string::npos) ? full : full.substr(sp + 1);
        ClassifyPrefab(it);
        snap.palette.push_back(std::move(it));
        ++extra;
    }
    int sandboxAdded = 0;
    for (const auto& sp : SandboxPrefabs())
    {
        if (have(sp.cls)) continue;
        const std::string cat = SandboxCategory(sp.uniqueId, sp.cls);
        if (cat.empty()) continue;
        PaletteItem it;
        it.name = sp.cls;
        it.path = sp.path;
        it.category = cat;
        ClassifyPrefab(it);
        snap.palette.push_back(std::move(it));
        ++sandboxAdded;
    }
    if (sandboxAdded) Log("[game] palette: +%d sandbox prefab(s) beyond the LE catalogue", sandboxAdded);
    // Station actors with no sandbox prefab that are still worth placing (the server allows exactly these):
    // the yellow speed pads and boost tanks. Their class is loaded because the station uses them.
    for (const char* extra : { "BP_BoostPad_Omnidirectional_C", "BP_BoostTank_World_C" })
    {
        if (have(extra)) continue;
        if (SDK::UClass* c = SDK::UObject::FindClassFast(extra))
        {
            const std::string full = c->GetFullName();
            const size_t sp = full.find(' ');
            PaletteItem it;
            it.name = extra;
            it.path = sp == std::string::npos ? full : full.substr(sp + 1);
            it.category = "Parkour - Boost pads";
            snap.palette.push_back(std::move(it));
        }
    }
    std::sort(snap.palette.begin(), snap.palette.end(),
              [](const PaletteItem& a, const PaletteItem& b) {
                  if (a.category != b.category) return a.category < b.category;
                  return a.name < b.name;
              });
    Log("[game] palette: %d placeable LE item(s) (%d beyond the catalogue)", (int)snap.palette.size(), extra);
}

// FQuat::Rotator(), transcribed from UE. Rotation MUST be read: every transform command carries a full
// rotation, so leaving it at zero meant that merely moving an object silently reset its rotation.
// Transcribed, not re-derived: a hand-derived quat->rotator has already placed things wrong in this
// project once. The roll term in particular is atan2(-2*(W*X + Y*Z), 1 - 2*(X*X + Y*Y)).
Rot QuatToRot(double X, double Y, double Z, double W)
{
    const double r2d = 180.0 / 3.14159265358979;
    const double sing = Z * X - W * Y;
    const double yawY = 2.0 * (W * Z + X * Y);
    const double yawX = 1.0 - 2.0 * (Y * Y + Z * Z);
    Rot r;
    r.yaw = std::atan2(yawY, yawX) * r2d;
    if (sing < -0.4999995)
    {
        r.pitch = -90.0;
        r.roll  = std::remainder(-r.yaw - 2.0 * std::atan2(X, W) * r2d, 360.0);
    }
    else if (sing > 0.4999995)
    {
        r.pitch = 90.0;
        r.roll  = std::remainder(r.yaw - 2.0 * std::atan2(X, W) * r2d, 360.0);
    }
    else
    {
        r.pitch = std::asin(2.0 * sing) * r2d;
        r.roll  = std::atan2(-2.0 * (W * X + Y * Z), 1.0 - 2.0 * (X * X + Y * Y)) * r2d;
    }
    return r;
}

// Everything currently placed that the editor should be able to select. We list actors whose class is
// one of the LE prefabs, which is what this editor creates - the rest of the level is not ours to move.
template <typename P> bool CallNative(SDK::UObject* obj, const char* cls, const char* fn, P& parms);   // below

Vec3 g_markPos;                                      // markpos / expectdelta
double g_memFirstMB = -1.0;                          // first `mem` sample (leak checks)
int g_markersSeen = 0;                               // camera markers found by the last BuildObjects
void BuildObjects(Snapshot& snap)
{
    g_markersSeen = 0;
    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    if (!actorCls) return;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n && snap.objects.size() < 4096; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(actorCls)) continue;
        auto* c = o->Class;
        if (!c) continue;
        // A destroyed actor stays in GObjects until the next garbage collection -- up to a minute -- so
        // without this a deleted object lingered in the Outliner and viewport long after the server
        // removed it. AActor::bActorIsBeingDestroyed is byte 0x65, bit 0.
        if (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01) continue;
        const std::string cn = c->GetName();
        if (cn == "BP_LevelEditor_Pawn_C")                  // an editor's camera marker (server-spawned, SE|CAMPOS)
        {
            auto* ma = static_cast<SDK::AActor*>(o);
            const bool mine = g_pc && ma->Owner == static_cast<SDK::AActor*>(g_pc);
            if (mine != ma->bHidden)                        // hide our own (we are inside it); show everyone else's
            {
                SDK::Params::Actor_SetActorHiddenInGame h{}; h.bNewHidden = mine;
                CallNative(o, "Actor", "SetActorHiddenInGame", h);
            }
            ++g_markersSeen;
            continue;
        }
        if (cn.rfind("LE_", 0) != 0 && !IsSandboxClass(cn) && cn != "BP_BoostPad_Omnidirectional_C" && cn != "BP_BoostTank_World_C") continue;

        SceneObject so;
        so.ptr = o;
        so.handle = o->GetName();
        so.label = so.handle;
        so.className = cn;
        if (!g_ownLockMap.empty())
            if (auto ol = g_ownLockMap.find(HandleGuid(so.handle)); ol != g_ownLockMap.end())
            {
                so.lockedByMe = ol->second.mine;
                so.lockedByOther = !ol->second.mine;
                so.lockOwner = ol->second.owner;
            }

        // RootComponent@0x1A8 -> ComponentToWorld@0x1D0 (quat@+0, translation@+0x20, scale@+0x40)
        void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1A8);
        if (root)
        {
            const double* q = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x00);
            const double* t = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20);
            const double* s = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x40);
            so.location = { t[0], t[1], t[2] };
            so.scale    = { s[0], s[1], s[2] };
            so.rotation = QuatToRot(q[0], q[1], q[2], q[3]);
        }
        // Bounds for click-picking in the viewport (only while editing: it walks every component).
        if (g_editorMode)
        {
            SDK::Params::Actor_GetActorBounds b{};
            b.bOnlyCollidingComponents = false;
            b.bIncludeFromChildActors = true;
            if (CallNative(o, "Actor", "GetActorBounds", b))
            {
                so.boundsOff = { b.Origin.X - so.location.x, b.Origin.Y - so.location.y, b.Origin.Z - so.location.z };
                so.boundsExt = { b.BoxExtent.X, b.BoxExtent.Y, b.BoxExtent.Z };
            }
        }
        snap.objects.push_back(std::move(so));
    }
}

// Actor NAMES are not replicated. An actor the server spawns generally carries a DIFFERENT name here on
// the client, so sending a name back would resolve to nothing server-side and XFORM/DELETE would
// silently do nothing. Address actors by what both sides genuinely agree on instead: the class, plus
// the position that transform replication already keeps in sync.
//
// Game-thread only; never read from the render thread.
static std::vector<SceneObject> g_lastObjects;

std::string MakeIdent(const std::string& className, const Vec3& loc)
{
    return className + "@" + Fmt3(loc);
}

// Where we last TOLD the server to put each object. During a drag the actor moves on the server faster
// than our 4 Hz snapshot sees it, so addressing it by the snapshot position would miss (the server only
// matches within 1m) and the drag would stop mid-way. Addressing it by where we last sent it always
// matches, because that is exactly where the server put it.
struct Commanded { Vec3 loc; ULONGLONG at; };
std::vector<std::pair<std::string, Commanded>> g_commanded;

Commanded* FindCommanded(const std::string& handle)
{
    for (auto& e : g_commanded) if (e.first == handle) return &e.second;
    return nullptr;
}

// Resolve a UI handle (the actor's local name, which the stock lock protocol still uses) into the
// class+position identity the server addresses by.
std::string IdentFor(const std::string& handle)
{
    for (const SceneObject& o : g_lastObjects)
    {
        if (o.handle != handle) continue;
        const Commanded* c = FindCommanded(handle);
        const bool fresh = c && GetTickCount64() - c->at < 3000;
        return MakeIdent(o.className, fresh ? c->loc : o.location);
    }
    return "";
}

// For moves and deletes: the ident plus the sandbox id, which every machine's copy of a sandbox object
// carries at the front of its name (<GUID> or <GUID>_<n>). The server resolves the id first.
std::string IdentForEdit(const std::string& handle)
{
    std::string id = IdentFor(handle);
    if (id.empty() || handle.size() < 36) return id;
    const std::string g = handle.substr(0, 36);
    if (g[8] != '-' || g[13] != '-' || g[18] != '-' || g[23] != '-') return id;
    if (handle.size() > 36 && handle[36] != '_') return id;
    return id + "#" + g;
}

void NoteCommanded(const std::string& handle, const Vec3& loc)
{
    if (Commanded* c = FindCommanded(handle)) { c->loc = loc; c->at = GetTickCount64(); return; }
    g_commanded.push_back({ handle, { loc, GetTickCount64() } });
    if (g_commanded.size() > 256) g_commanded.erase(g_commanded.begin());
}

// ── Details > Properties ──────────────────────────────────────────────────────────────────────
std::string g_inspHandle, g_inspPath;     // what the Details panel is inspecting

bool Alive(SDK::UObject* o) { return o && SDK::UObject::GObjects->GetByIndex(o->Index) == o; }

SDK::UObject* ActorFor(const std::string& handle)
{
    for (const SceneObject& o : g_lastObjects)
        if (o.handle == handle) { auto* a = static_cast<SDK::UObject*>(o.ptr); return Alive(a) ? a : nullptr; }
    return nullptr;
}

// Read the inspected object's editable properties into the snapshot (4 Hz, with the pump).
void FillProps(Snapshot& snap)
{
    if (g_inspHandle.empty()) return;
    SDK::UObject* root = ActorFor(g_inspHandle);
    if (!root) { g_inspHandle.clear(); g_inspPath.clear(); return; }
    SDK::UObject* obj = sereflect::Resolve(root, g_inspPath);
    if (!obj) { g_inspPath.clear(); obj = root; }             // the sub-object went away: back to the actor
    snap.inspectHandle = g_inspHandle;
    snap.inspectPath   = g_inspPath;
    snap.inspectClass  = obj->Class ? obj->Class->GetName() : std::string();

    std::vector<sereflect::Prop> props;
    sereflect::List(obj, props);
    for (const auto& pr : props)
    {
        PropInfo pi;
        pi.name  = pr.name;
        pi.owner = pr.owner;
        pi.type  = static_cast<int>(pr.t);
        pi.path  = g_inspPath.empty() ? pr.name : g_inspPath + "." + pr.name;
        pi.value = sereflect::Read(obj, pr.p, pr.t);
        pi.writable = sereflect::Writable(pr.t);
        pi.net = sereflect::Replicated(pr.p);
        pi.inert = sereflect::IsA(obj, "LuauBehavior") && !sereflect::IsA(obj, "TextComponent");
        if (pr.t == sereflect::PType::Object) pi.link = sereflect::Hop(obj, pr.name) != nullptr;
        if (pr.t == sereflect::PType::Enum)
            for (const auto& e : sereflect::EnumNames(sereflect::EnumOf(pr.p))) pi.enumNames.push_back({ e.first, e.second });
        snap.props.push_back(std::move(pi));
    }
}

// Apply a property locally (instant feedback) and send it to the server, which applies it for real.
void SetPropertyCmd(const Command& c)
{
    SDK::UObject* root = ActorFor(c.str);
    if (!root) { Log("[props] %s is gone", c.str.c_str()); return; }
    const size_t dot = c.str2.find_last_of('.');
    const std::string objPath = dot == std::string::npos ? std::string() : c.str2.substr(0, dot);
    const std::string prop    = dot == std::string::npos ? c.str2 : c.str2.substr(dot + 1);
    std::string value = c.str3;
    for (auto& ch : value) if (ch == '|') ch = ' ';           // '|' separates the command's fields
    SDK::UObject* obj = sereflect::Resolve(root, objPath);
    sereflect::PType t{};
    SDK::FProperty* p = obj ? sereflect::Find(obj, prop, &t) : nullptr;
    if (!p || !sereflect::Writable(t) || !sereflect::Write(obj, p, t, value))
    { Log("[props] cannot set %s = %s", c.str2.c_str(), value.c_str()); return; }
    sereflect::AfterWrite(root, obj, p);                     // redraw / OnRep here, now
    const std::string id = IdentFor(c.str);
    if (!id.empty()) SendToServer("SE|PROP|" + id + "|" + c.str2 + "|" + value);
    Log("[props] %s = %s", c.str2.c_str(), value.c_str());
}

// The server re-broadcasts every property edit to every player controller through the stock
// APlayerController::ClientMessage RPC (a vanilla client just prints it to its console). This client
// applies it to its own copy of the actor, found the same way the server finds it: class + position.
SDK::UObject* ActorForIdent(const std::string& ident)
{
    const size_t at = ident.find('@');
    if (at == std::string::npos) return nullptr;
    const std::string cls = ident.substr(0, at);
    double x = 0, y = 0, z = 0;
    if (sscanf_s(ident.c_str() + at + 1, "%lf,%lf,%lf", &x, &y, &z) != 3) return nullptr;
    SDK::UObject* best = nullptr;
    double bestD = 100.0 * 100.0;                            // 1 m, as the server
    for (const SceneObject& o : g_lastObjects)
    {
        if (o.className != cls) continue;
        const double dx = o.location.x - x, dy = o.location.y - y, dz = o.location.z - z;
        const double d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = static_cast<SDK::UObject*>(o.ptr); }
    }
    return best;
}

void ApplyRemoteProp(const std::string& msg)             // "SE|PROP|<ident>|<path>|<value>"
{
    std::vector<std::string> f;
    size_t from = 0;
    for (int i = 0; i < 4; ++i)
    {
        const size_t bar = msg.find('|', from);
        if (bar == std::string::npos) return;
        f.push_back(msg.substr(from, bar - from));
        from = bar + 1;
    }
    f.push_back(msg.substr(from));                           // the value may itself be anything
    const std::string& ident = f[2], & path = f[3], & value = f[4];
    SDK::UObject* root = ActorForIdent(ident);
    if (!root) { Log("[props] broadcast for %s: no such object here yet", ident.c_str()); return; }
    const size_t dot = path.find_last_of('.');
    SDK::UObject* obj = sereflect::Resolve(root, dot == std::string::npos ? std::string() : path.substr(0, dot));
    sereflect::PType t{};
    SDK::FProperty* p = obj ? sereflect::Find(obj, dot == std::string::npos ? path : path.substr(dot + 1), &t) : nullptr;
    if (!p) { Log("[props] broadcast: %s not found on %s", path.c_str(), ident.c_str()); return; }
    if (sereflect::Read(obj, p, t) == value) { sereflect::AfterWrite(root, obj, p); return; }   // ours, echoed back
    if (!sereflect::Write(obj, p, t, value)) { Log("[props] broadcast: bad value for %s", path.c_str()); return; }
    sereflect::AfterWrite(root, obj, p);
    Log("[props] applied from server: %s.%s = %s", ident.c_str(), path.c_str(), value.c_str());
}

void PredictTransform(const std::string& handle, const Vec3& loc, const Rot& rot, const Vec3& scale);   // below

bool TraceWorld(const Vec3& from, const Vec3& dir, double maxDist, Vec3& hit);   // editor camera section
void CameraFocus(const Vec3& p);

std::vector<Vec3> g_clickPlaced;                         // PlaceTraced results (see Snapshot::clickPlaced)
std::string g_slotCandType;                             // SlotScan result (see Snapshot::slotCands)
std::vector<Snapshot::SlotCand> g_slotCands;

void HandleCommands()
{
    for (const Command& c : State().Drain())
    {
        switch (c.type)
        {
        case CmdType::RefreshPalette:
            g_paletteDirty = true;
            break;

        case CmdType::SpawnItem:
            SendToServer("SE|SPAWN|" + c.str + "|" + Fmt3(c.loc) + "|" + Fmt3(c.rot) + "|" + Fmt3(c.scale));
            Log("[game] spawn request: %s", c.str.c_str());
            break;

        case CmdType::SetTransform:
        {
            // c.loc/rot/scale are the NEW transform; the identity has to be built from where the actor
            // still is, which is what the server will match on.
            if (LockedByOther(c.str)) break;
            const std::string id = IdentForEdit(c.str);
            if (id.empty()) { Log("[game] xform: no identity for handle %s", c.str.c_str()); break; }
            SendToServer("SE|XFORM|" + id + "|" + Fmt3(c.loc) + "|" + Fmt3(c.rot) + "|" + Fmt3(c.scale));
            NoteCommanded(c.str, c.loc);
            PredictTransform(c.str, c.loc, c.rot, c.scale);   // show it now; the server's copy follows
            break;
        }

        case CmdType::DeleteObject:
        {
            if (LockedByOther(c.str)) break;
            const std::string id = IdentForEdit(c.str);
            if (id.empty()) { Log("[game] delete: no identity for handle %s", c.str.c_str()); break; }
            SendToServer("SE|DELETE|" + id);
            break;
        }

        case CmdType::SelectObject:
            // Stock protocol: a bare handle is a genuine lock request -- editor pawn only.
            SendLock(c.str, true);
            break;

        case CmdType::DeselectObject:
            SendLock(c.str, false);
            break;

        case CmdType::SendRaw:
            SendToServer(c.str);
            break;

        case CmdType::LevelExport:
            SendToServer("SE|LVEXPORT|" + c.str);
            Log("[levels] export '%s' requested", c.str.c_str());
            break;

        case CmdType::LevelImport:
        {
            static const char* hx = "0123456789ABCDEF";
            std::string hex;
            for (unsigned char ch : c.str3) { hex.push_back(hx[ch >> 4]); hex.push_back(hx[ch & 15]); }
            size_t parts = 0;
            for (size_t i = 0; i < hex.size(); i += 960, ++parts) g_paced.push_back("SE|LVPART|" + hex.substr(i, 960));
            g_paced.push_back("SE|LVIMPORT|" + c.str + "|" + c.str2);
            Notes().Set((c.str2 == "save" ? "Uploading '" : "Loading '") + c.str + "' (" + std::to_string(c.str3.size() / 1024 + 1) + " KB)...");
            Log("[levels] import '%s' (%s): %zu byte(s) in %zu part(s)", c.str.c_str(), c.str2.c_str(), c.str3.size(), parts);
            break;
        }

        case CmdType::OwnLock:
        {
            const std::string id = IdentForEdit(c.str);
            if (id.empty()) break;
            SendToServer("SE|OWNLOCK|" + id + "|" + c.str2);
            Log("[locks] %s %s", c.str2 == "1" ? "lock" : "unlock", id.c_str());
            break;
        }

        case CmdType::Duplicate:
        {
            // A sandbox object is copied by the server from the object itself (catalogue pieces like the
            // boost pad have no palette entry; Game data and scripts come along). Anything else re-spawns
            // from the palette.
            const std::string id = IdentForEdit(c.str);
            if (id.find('#') != std::string::npos)
                SendToServer("SE|SBDUP|" + id + "|" + Fmt3(c.loc) + "|" + Fmt3(c.rot) + "|" + Fmt3(c.scale));
            else if (!c.str2.empty())
                SendToServer("SE|SPAWN|" + c.str2 + "|" + Fmt3(c.loc) + "|" + Fmt3(c.rot) + "|" + Fmt3(c.scale));
            else
                Log("[game] duplicate: %s has no palette entry and no sandbox id", c.str.c_str());
            break;
        }

        case CmdType::LuauAttach:
        {
            // The source goes up in hex chunks (commands are capped at 1 KB), then one attach command.
            const std::string id = IdentFor(c.str);
            if (id.empty() || c.str2.empty()) break;
            std::string hex;
            char hb[4];
            for (unsigned char ch : c.str3) { snprintf(hb, sizeof(hb), "%02X", ch); hex += hb; }
            for (size_t i = 0; i < hex.size(); i += 900) SendToServer("SE|LUAUPART|" + c.str2 + "|" + hex.substr(i, 900));
            if (hex.empty()) SendToServer("SE|LUAUPART|" + c.str2 + "|");
            SendToServer("SE|LUAU|" + id + "|" + c.str2);
            Log("[luau] attach %s (%zu chars) to %s", c.str2.c_str(), c.str3.size(), id.c_str());
            break;
        }

        case CmdType::LuauRemove:
        {
            const std::string id = IdentFor(c.str);
            if (id.empty() || c.str2.empty()) break;
            SendToServer("SE|LUAUDEL|" + id + "|" + c.str2);
            Log("[luau] remove %s from %s", c.str2.c_str(), id.c_str());
            break;
        }

        case CmdType::SlotScan:
        {
            // Which listed objects have a component of this type? (their name is the slot's key)
            g_slotCandType = c.str;
            g_slotCands.clear();
            SDK::UClass* want = SDK::UObject::FindClassFast(c.str);
            if (!want) break;
            std::unordered_map<SDK::UObject*, std::string> byActor;
            for (const SceneObject& s : g_lastObjects) byActor[static_cast<SDK::UObject*>(s.ptr)] = s.handle;
            const int32_t n = SDK::UObject::GObjects->Num();
            for (int32_t i = 0; i < n && g_slotCands.size() < 512; ++i)
            {
                SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
                if (!o || !o->Outer || o->IsDefaultObject() || !o->IsA(want)) continue;
                auto it = byActor.find(o->Outer);
                if (it != byActor.end()) g_slotCands.push_back({ it->second, o->GetName() });
            }
            break;
        }

        case CmdType::LuauRef:
        {
            const std::string id = IdentFor(c.str);
            const std::string tid = c.str5.empty() ? std::string("-") : IdentFor(c.str5);
            if (id.empty() || tid.empty()) break;
            SendToServer("SE|LUAUREF|" + id + "|" + c.str2 + "|" + c.str3 + "|" + c.str4 + "|" + tid);
            Log("[luau] slot %s.%s -> %s", c.str2.c_str(), c.str3.c_str(), tid.c_str());
            break;
        }

        case CmdType::LuauUpdate:
        {
            if (c.str2.empty()) break;
            std::string hex;
            char hb[4];
            for (unsigned char ch : c.str3) { snprintf(hb, sizeof(hb), "%02X", ch); hex += hb; }
            for (size_t i = 0; i < hex.size(); i += 900) SendToServer("SE|LUAUPART|" + c.str2 + "|" + hex.substr(i, 900));
            if (hex.empty()) SendToServer("SE|LUAUPART|" + c.str2 + "|");
            SendToServer("SE|LUAUSRC|" + c.str2);
            Log("[luau] re-sent %s (%zu chars)", c.str2.c_str(), c.str3.size());
            break;
        }

        case CmdType::ScanScripts:
        {
            // Every loaded gamemode's Luau (LGM+720: FString name -> FString source, stride from the pair).
            g_gameScripts.clear();
            auto* cls = SDK::UObject::FindClassFast("LoadedGameMode");
            const int32_t n = SDK::UObject::GObjects->Num();
            auto readable = [](const void* p, size_t len) { return p && !IsBadReadPtr(p, len); };
            auto wstr = [&](const void* elem, std::string& out, int maxLen) -> bool {
                if (!readable(elem, 16)) return false;
                const wchar_t* w = *reinterpret_cast<wchar_t* const*>(elem);
                const int len = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(elem) + 8);
                if (len <= 1 || len > maxLen || !readable(w, len * 2)) return false;
                out.clear();
                for (int i = 0; i < len - 1; ++i) out.push_back(w[i] < 128 ? static_cast<char>(w[i]) : '?');
                return true;
            };
            for (int32_t i = 0; cls && i < n && g_gameScripts.size() < 200; ++i)
            {
                SDK::UObject* l = SDK::UObject::GObjects->GetByIndex(i);
                if (!l || l->IsDefaultObject() || !l->IsA(cls)) continue;
                const uintptr_t map = reinterpret_cast<uintptr_t>(l) + 720;
                const uint8_t* data = *reinterpret_cast<uint8_t* const*>(map);
                const int num = *reinterpret_cast<const int32_t*>(map + 8);
                std::string slot = l->GetName();
                if (void* slotActor = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(l) + 0x320))
                    wstr(reinterpret_cast<uint8_t*>(slotActor) + 0x390, slot, 200);
                for (int k = 0; data && k < num && k < 100; ++k)
                {
                    std::string key, src;
                    if (!wstr(data + k * 0x28, key, 200) || !wstr(data + k * 0x28 + 16, src, 400000)) continue;
                    g_gameScripts.push_back({ slot, key, src });
                }
            }
            Log("[luau] %zu game script(s) found", g_gameScripts.size());
            if (!g_dumpScriptsTo.empty())
            {
                CreateDirectoryA(g_dumpScriptsTo.c_str(), nullptr);
                for (const auto& gs : g_gameScripts)
                {
                    std::string fn = g_dumpScriptsTo + "\\" + gs.where + "__" + gs.name;
                    for (size_t k = g_dumpScriptsTo.size() + 1; k < fn.size(); ++k) if (fn[k] == '/' || fn[k] == ':') fn[k] = '_';
                    if (FILE* f = fopen(fn.c_str(), "wb")) { fwrite(gs.source.data(), 1, gs.source.size(), f); fclose(f); }
                }
                Log("[luau] wrote %zu script(s) to %s", g_gameScripts.size(), g_dumpScriptsTo.c_str());
                g_dumpScriptsTo.clear();
            }
            break;
        }

        case CmdType::DataRequest:
        {
            const std::string id = IdentFor(c.str);
            if (id.empty()) break;
            g_dataHandle = c.str; g_dataIdent = id;
            SendToServer("SE|SBDATA|" + id);
            break;
        }

        case CmdType::DataSet:
        {
            const std::string id = IdentFor(c.str);
            if (id.empty()) break;
            g_dataHandle = c.str; g_dataIdent = id;
            SendToServer("SE|SBSET|" + id + "|" + c.str2 + "|" + c.str3 + "|" + c.str4);
            break;
        }

        case CmdType::QuestAddStep:
            g_questSteps.push_back({ c.str, atoi(c.str2.c_str()) });
            break;

        case CmdType::QuestCompile:
        {
            // Steps go out as Class@position identities, like moves -- the server can resolve those, and it
            // could never resolve this client's actor names. ';' separates steps because an identity
            // itself contains commas.
            std::string steps;
            int unresolved = 0;
            for (const auto& st : g_questSteps)
            {
                const std::string id = IdentFor(st.handle);
                if (id.empty()) { ++unresolved; continue; }
                if (!steps.empty()) steps += ";";
                steps += id + ":" + std::to_string(st.kind);
            }
            char extra[64], tail[64];
            snprintf(extra, sizeof(extra), "|%d|%g|", c.num2, c.f1);
            snprintf(tail, sizeof(tail), "|%.0f|%d", c.radius, c.timeLimit);
            std::string desc = c.str4, title = c.str2;
            for (auto& ch : desc)  if (ch == '|') ch = '/';       // '|' is the field separator
            for (auto& ch : title) if (ch == '|') ch = '/';
            SendToServer("SE|QUEST|" + c.str + "|" + title + "|" + c.str3 + "|" + std::to_string(c.num) + "|" + steps +
                         extra + desc + tail + "|" + c.str5);
            Log("[game] quest publish: %s '%s' glyph=%s rep=%d, %d step(s)%s", c.str.c_str(), c.str2.c_str(),
                c.str3.c_str(), c.num, (int)g_questSteps.size() - unresolved,
                unresolved ? " (some steps no longer exist and were skipped)" : "");
            g_questSteps.clear();
            break;
        }

        case CmdType::PlaceTraced:
        {
            // Construction mode: on the surface the click hit, lifted so the coin floats at pickup height.
            // A red coin's box (its pickup trigger, what the selection box shows) starts at its origin and reaches
            // 1 m up, so the origin goes on the surface: the box rests on the ground, the coin floats in its middle.
            const double lift = 2.0;
            Vec3 at;
            if (TraceWorld(c.loc, c.dir, 50000.0, at)) at.z += lift;
            else at = { c.loc.x + c.dir.x * 1500.0, c.loc.y + c.dir.y * 1500.0, c.loc.z + c.dir.z * 1500.0 };
            SendToServer("SE|SPAWN|" + c.str + "|" + Fmt3(at) + "|0,0,0");
            g_clickPlaced.push_back(at);
            Log("[game] construction: placed at (%.0f, %.0f, %.0f), lift %.0f", at.x, at.y, at.z, lift);
            break;
        }

        case CmdType::SpawnTraced:
        {
            // On the surface under the ray if there is one, else a fixed distance down it. Grid snap moves
            // it in X/Y only, so it stays sitting on the surface it was dropped on.
            Vec3 at;
            if (!TraceWorld(c.loc, c.dir, 50000.0, at))
                at = { c.loc.x + c.dir.x * c.fallback, c.loc.y + c.dir.y * c.fallback, c.loc.z + c.dir.z * c.fallback };
            if (c.snap > 0.0f)
            {
                at.x = std::round(at.x / c.snap) * c.snap;
                at.y = std::round(at.y / c.snap) * c.snap;
            }
            SendToServer("SE|SPAWN|" + c.str + "|" + Fmt3(at) + "|" + Fmt3(c.rot));
            Log("[game] placed %s at (%.0f, %.0f, %.0f)", c.str.c_str(), at.x, at.y, at.z);
            break;
        }

        case CmdType::FocusCamera:
            CameraFocus(c.loc);
            break;

        case CmdType::Inspect:
            g_inspHandle = c.str;
            g_inspPath = c.str2;
            break;

        case CmdType::SetProperty:
            SetPropertyCmd(c);
            break;

        case CmdType::EnterEditor:
        case CmdType::ExitEditor:
            // Editor mode is client-side: it arms the gizmo and placement. The server is told so its
            // log shows who is editing, but nothing there depends on it.
            g_editorMode = (c.type == CmdType::EnterEditor);
            SendToServer(g_editorMode ? "SE|ENTER" : "SE|EXIT");
            Log("[game] editor mode %s", g_editorMode ? "ON" : "OFF");
            break;
        }
    }
}

// ── editor camera ────────────────────────────────────────────────────────────────────────────
// The game's own spectator movement is built for VR and was miserable to edit with. While editing, a
// client-local ACameraActor becomes the player controller's view target and is flown like Unreal's
// viewport camera: RMB + mouse to look, RMB + WASD to fly, Q/E down/up, wheel (while RMB) for speed,
// Shift to go faster, F to frame the selection. The actor is local to this client -- it is never sent to
// the server, it only changes what this player sees -- and the view goes back to the pawn on exit.
struct EditorCamera
{
    SDK::UObject* actor = nullptr;
    double x = 0, y = 0, z = 0, pitch = 0, yaw = 0;   // pitch/yaw are relative to the gravity frame below
    float  speed = 1200.0f;          // units per second
    LARGE_INTEGER last{};
    bool   primed = false;
    // Gravity frame: `up` is the way up where the camera is (world Z outside every gravity zone), `tan` a
    // horizontal reference carried along as `up` turns (so the view never flips), and the world rotator the
    // camera actor really gets.
    double up[3] = { 0, 0, 1 }, tan[3] = { 1, 0, 0 }, fwd[3] = { 1, 0, 0 };
    double rp = 0, ry = 0, rr = 0;
};
EditorCamera g_cam;

template <typename P> bool CallNative(SDK::UObject* obj, const char* cls, const char* fn, P& parms)
{
    auto* f = obj && obj->Class ? obj->Class->GetFunction(cls, fn) : nullptr;
    if (!f) return false;
    const auto saved = f->FunctionFlags;
    f->FunctionFlags |= 0x400;                                   // FUNC_Native, as the generated SDK does
    const bool ok = SafePE(obj, f, &parms);
    f->FunctionFlags = saved;
    return ok;
}
template <typename P> bool CallStatic(const char* cls, const char* fn, P& parms)
{
    auto* c = SDK::UObject::FindClassFast(cls);
    auto* f = c ? c->GetFunction(cls, fn) : nullptr;
    if (!f) return false;
    const auto saved = f->FunctionFlags;
    f->FunctionFlags |= 0x400;
    const bool ok = SafePE(c->ClassDefaultObject, f, &parms);
    f->FunctionFlags = saved;
    return ok;
}

bool ObjectAlive(SDK::UObject* o)
{
    return o && SDK::UObject::GObjects->GetByIndex(o->Index) == o;
}

SDK::FTransform MakeXf(double x, double y, double z)
{
    SDK::FTransform t{};
    t.Rotation = SDK::FQuat{ 0, 0, 0, 1 };
    t.Translation = SDK::FVector{ x, y, z };
    t.Scale3D = SDK::FVector{ 1, 1, 1 };
    return t;
}

void SetViewTarget(SDK::UObject* target)
{
    if (!g_pc || !target) return;
    SDK::Params::PlayerController_SetViewTargetWithBlend p{};
    p.NewViewTarget = static_cast<SDK::AActor*>(target);
    p.BlendTime = 0.0f;
    p.BlendFunc = SDK::EViewTargetBlendFunction::VTBlend_Linear;
    p.BlendExp = 0.0f;
    p.bLockOutgoing = false;
    CallNative(g_pc, "PlayerController", "SetViewTargetWithBlend", p);
}

// ── gravity zones ──────────────────────────────────────────────────────────────────────────────
// The station is an O'Neill cylinder (gravity points away from its axis, so "up" is toward it), with
// gravity volumes -- boxes whose own +Z is up while you are inside them -- for the ramps and side rooms.
// The game turns players and its spectator camera with them; the editor camera used world Z as up, so
// anywhere but the bottom of the cylinder the level showed sideways or upside down. The zones only act on
// real pawns (a gravity component on the camera is never picked up), so the camera works out the same
// "up" from the zones' own data: the smallest non-additive volume it is inside, else the cylinder around
// it, else world Z.
struct GravZone
{
    SDK::UObject* actor = nullptr;
    bool   cylinder = false, invert = false;
    double lo[3]{}, hi[3]{};                  // world bounds (a cheap first test)
    double centre[3]{}, axis[3]{ 0, 1, 0 }, maxDist = 0;   // cylinder
    SDK::FTransform box{};                    // volume: its trigger box's transform and unscaled extent
    double extent[3]{}, up[3]{ 0, 0, 1 }, size = 0;
};
static std::vector<GravZone> g_gravZones;
static ULONGLONG g_gravZonesAt = 0;
struct GravVec { SDK::FVector ReturnValue; };

static void GravZonesRefresh()
{
    g_gravZones.clear();
    auto* ocls = SDK::UObject::FindClassFast("OneillGravityActor");
    auto* vcls = SDK::UObject::FindClassFast("GravityVolumeActor");
    auto* bcls = SDK::UObject::FindClassFast("GravityBoxActor");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        SDK::UObject* ob = SDK::UObject::GObjects->GetByIndex(i);
        if (!ob || !ob->Class || ob->IsDefaultObject()) continue;
        const bool isO = ocls && ob->IsA(ocls), isV = vcls && ob->IsA(vcls), isB = bcls && ob->IsA(bcls);
        if (!isO && !isV && !isB) continue;
        const uintptr_t b = reinterpret_cast<uintptr_t>(ob);
        GravZone z;
        z.actor = ob;
        SDK::Params::Actor_GetActorBounds ab{};
        ab.bOnlyCollidingComponents = true;
        if (!CallNative(ob, "Actor", "GetActorBounds", ab)) continue;
        if (ab.BoxExtent.X + ab.BoxExtent.Y + ab.BoxExtent.Z < 1.0)     // no colliding part: take the whole actor
        {
            ab.bOnlyCollidingComponents = false;
            if (!CallNative(ob, "Actor", "GetActorBounds", ab) || ab.BoxExtent.X + ab.BoxExtent.Y + ab.BoxExtent.Z < 1.0) continue;
        }
        z.lo[0] = ab.Origin.X - ab.BoxExtent.X; z.lo[1] = ab.Origin.Y - ab.BoxExtent.Y; z.lo[2] = ab.Origin.Z - ab.BoxExtent.Z;
        z.hi[0] = ab.Origin.X + ab.BoxExtent.X; z.hi[1] = ab.Origin.Y + ab.BoxExtent.Y; z.hi[2] = ab.Origin.Z + ab.BoxExtent.Z;
        if (isO)
        {
            z.cylinder = true;
            SDK::Params::Actor_K2_GetActorLocation l{};
            CallNative(ob, "Actor", "K2_GetActorLocation", l);
            z.centre[0] = l.ReturnValue.X; z.centre[1] = l.ReturnValue.Y; z.centre[2] = l.ReturnValue.Z;
            const double* ax = reinterpret_cast<const double*>(b + 0x308);           // RotationAxis
            const double an = std::sqrt(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
            if (an > 0.5) { z.axis[0] = ax[0] / an; z.axis[1] = ax[1] / an; z.axis[2] = ax[2] / an; }
            z.maxDist = *reinterpret_cast<float*>(b + 0x29C);                         // MaxGravityDistance
            z.invert = *reinterpret_cast<bool*>(b + 0x302);                           // InvertGravity
            z.size = 1e30;                                                             // volumes inside it win
        }
        else
        {
            if (isV && *reinterpret_cast<bool*>(b + 0x298)) continue;                // bIsAdditive: a nudge, not a floor
            SDK::UObject* boxc = *reinterpret_cast<SDK::UObject**>(b + (isV ? 0x2A0 : 0x2B0));   // boxTrigger
            if (!boxc || !ObjectAlive(boxc)) continue;
            SDK::Params::SceneComponent_K2_GetComponentToWorld w{};
            if (!CallNative(boxc, "SceneComponent", "K2_GetComponentToWorld", w)) continue;
            z.box = w.ReturnValue;
            const double* ext = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(boxc) + 0x540);   // BoxExtent
            z.extent[0] = ext[0]; z.extent[1] = ext[1]; z.extent[2] = ext[2];
            GravVec u{};
            if (!CallNative(ob, "Actor", "GetActorUpVector", u)) continue;
            z.up[0] = u.ReturnValue.X; z.up[1] = u.ReturnValue.Y; z.up[2] = u.ReturnValue.Z;
            if (isB && *reinterpret_cast<bool*>(b + 0x2A6)) { z.up[0] = -z.up[0]; z.up[1] = -z.up[1]; z.up[2] = -z.up[2]; }
            z.size = (z.hi[0] - z.lo[0]) * (z.hi[1] - z.lo[1]) * (z.hi[2] - z.lo[2]);
        }
        g_gravZones.push_back(z);
    }
}

// The way up where the camera is (world Z outside every zone). Returns the zone that decided it, if any.
static const GravZone* CameraGravityUp(double out[3])
{
    out[0] = 0; out[1] = 0; out[2] = 1;
    const ULONGLONG now = GetTickCount64();
    if (now - g_gravZonesAt > 3000) { g_gravZonesAt = now; GravZonesRefresh(); }   // streaming levels come and go
    const double pt[3] = { g_cam.x, g_cam.y, g_cam.z };
    const GravZone* best = nullptr;
    double bestUp[3] = { 0, 0, 1 };
    for (const GravZone& z : g_gravZones)
    {
        if (best && z.size >= best->size) continue;
        if (!ObjectAlive(z.actor)) continue;
        bool inBounds = true;
        for (int k = 0; k < 3; ++k) if (pt[k] < z.lo[k] - 1.0 || pt[k] > z.hi[k] + 1.0) inBounds = false;
        if (!inBounds) continue;
        double up[3];
        if (z.cylinder)
        {
            double d[3] = { pt[0] - z.centre[0], pt[1] - z.centre[1], pt[2] - z.centre[2] };
            const double along = d[0] * z.axis[0] + d[1] * z.axis[1] + d[2] * z.axis[2];
            for (int k = 0; k < 3; ++k) d[k] -= along * z.axis[k];
            const double r = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (r < 50.0 || (z.maxDist > 0 && r > z.maxDist)) continue;              // on the axis: no floor
            const double sgn = z.invert ? 1.0 : -1.0;                                 // up is toward the axis
            for (int k = 0; k < 3; ++k) up[k] = sgn * d[k] / r;
        }
        else
        {
            SDK::Params::KismetMathLibrary_InverseTransformLocation it{};
            it.T = z.box;
            it.Location = SDK::FVector{ pt[0], pt[1], pt[2] };
            if (!CallStatic("KismetMathLibrary", "InverseTransformLocation", it)) continue;
            if (std::fabs(it.ReturnValue.X) > z.extent[0] || std::fabs(it.ReturnValue.Y) > z.extent[1] ||
                std::fabs(it.ReturnValue.Z) > z.extent[2]) continue;
            for (int k = 0; k < 3; ++k) up[k] = z.up[k];
        }
        best = &z;
        for (int k = 0; k < 3; ++k) bestUp[k] = up[k];
    }
    if (best) for (int k = 0; k < 3; ++k) out[k] = bestUp[k];
    return best;
}
static void Normalize3(double v[3], const double fallback[3])
{
    const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (n < 1e-6) { v[0] = fallback[0]; v[1] = fallback[1]; v[2] = fallback[2]; return; }
    v[0] /= n; v[1] /= n; v[2] /= n;
}
// Swing `up` toward the game's up over ~1/4 s (a zone boundary should not snap the view).
static void CameraTurnToGravity(double dt)
{
    double target[3];
    CameraGravityUp(target);
    double* u = g_cam.up;
    const double d = u[0] * target[0] + u[1] * target[1] + u[2] * target[2];
    const double k = dt <= 0.0 ? 1.0 : (std::min)(1.0, dt * 5.0);
    if (d < -0.95)                                               // a straight flip: go round via the reference
        for (int i = 0; i < 3; ++i) u[i] += g_cam.tan[i] * 0.3;
    for (int i = 0; i < 3; ++i) u[i] += (target[i] - u[i]) * k;
    Normalize3(u, target);
}
// Build the view from the frame: forward and right from the local pitch/yaw, and the camera actor's world
// rotator (the engine builds it from forward + up -- no hand-rolled rotator maths).
static void CameraFrame(double right[3] = nullptr)
{
    double* u = g_cam.up;
    double* t = g_cam.tan;
    static const double wx[3] = { 1, 0, 0 }, wy[3] = { 0, 1, 0 };
    const double tu = t[0] * u[0] + t[1] * u[1] + t[2] * u[2];
    for (int i = 0; i < 3; ++i) t[i] -= tu * u[i];               // carry the reference along (no flips)
    const double n = std::sqrt(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
    if (n < 1e-3)                                                // it lined up with up: pick a fresh one
    {
        const double* w = std::fabs(u[0]) < 0.9 ? wx : wy;
        const double wu = w[0] * u[0] + w[1] * u[1] + w[2] * u[2];
        for (int i = 0; i < 3; ++i) t[i] = w[i] - wu * u[i];
    }
    Normalize3(t, wy);
    const double b[3] = { u[1] * t[2] - u[2] * t[1], u[2] * t[0] - u[0] * t[2], u[0] * t[1] - u[1] * t[0] };   // up x tan
    const double d2r = 3.14159265358979 / 180.0;
    const double cp = std::cos(g_cam.pitch * d2r), sp = std::sin(g_cam.pitch * d2r);
    const double cy = std::cos(g_cam.yaw * d2r),   sy = std::sin(g_cam.yaw * d2r);
    for (int i = 0; i < 3; ++i) g_cam.fwd[i] = cp * cy * t[i] + cp * sy * b[i] + sp * u[i];
    if (right) for (int i = 0; i < 3; ++i) right[i] = -sy * t[i] + cy * b[i];
    if (u[2] > 0.99999)                                          // plain world-up: exactly the old camera
    {
        g_cam.rp = g_cam.pitch; g_cam.ry = g_cam.yaw + std::atan2(t[1], t[0]) / d2r; g_cam.rr = 0;
        return;
    }
    SDK::Params::KismetMathLibrary_MakeRotFromXZ m{};
    m.X = SDK::FVector{ g_cam.fwd[0], g_cam.fwd[1], g_cam.fwd[2] };
    m.Z = SDK::FVector{ u[0], u[1], u[2] };
    if (CallStatic("KismetMathLibrary", "MakeRotFromXZ", m))
    {
        g_cam.rp = m.ReturnValue.Pitch; g_cam.ry = m.ReturnValue.Yaw; g_cam.rr = m.ReturnValue.Roll;
    }
}

void CameraActivate(const Snapshot& snap)
{
    if (!ObjectAlive(g_cam.actor))
    {
        g_cam.actor = nullptr;
        auto* cls = SDK::UObject::FindClassFast("CameraActor");
        if (!cls) { Log("[cam] CameraActor class not found"); return; }
        SDK::Params::GameplayStatics_BeginDeferredActorSpawnFromClass b{};
        b.WorldContextObject = g_pc;
        b.ActorClass = cls;
        b.SpawnTransform = MakeXf(snap.cameraPos.x, snap.cameraPos.y, snap.cameraPos.z);
        b.CollisionHandlingOverride = SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
        b.Owner = nullptr;
        b.TransformScaleMethod = SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
        if (!CallStatic("GameplayStatics", "BeginDeferredActorSpawnFromClass", b) || !b.ReturnValue)
        { Log("[cam] could not spawn the editor camera"); return; }
        SDK::Params::GameplayStatics_FinishSpawningActor f{};
        f.Actor = b.ReturnValue;
        f.SpawnTransform = b.SpawnTransform;
        f.TransformScaleMethod = SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
        CallStatic("GameplayStatics", "FinishSpawningActor", f);
        g_cam.actor = b.ReturnValue;
        Log("[cam] editor camera spawned");
    }
    // Start exactly where the player was looking, so entering the editor does not jump the view.
    g_cam.x = snap.cameraPos.x; g_cam.y = snap.cameraPos.y; g_cam.z = snap.cameraPos.z;
    g_cam.pitch = snap.cameraRot.pitch; g_cam.yaw = snap.cameraRot.yaw;
    g_cam.up[0] = 0; g_cam.up[1] = 0; g_cam.up[2] = 1;
    g_cam.tan[0] = 1; g_cam.tan[1] = 0; g_cam.tan[2] = 0;
    g_cam.primed = false;
    g_gravZonesAt = 0;                                           // re-read the zones for this session
    CameraGravityUp(g_cam.up);                                   // start already the right way up
    CameraFrame();
    SetViewTarget(g_cam.actor);
    Cam().active = true;
}

void CameraDeactivate()
{
    Cam().active = false;
    Cam().looking = false;
    if (!g_pc) return;
    // Back to the player's own pawn (APlayerController.AcknowledgedPawn @0x340, else Controller.Pawn @0x2D8).
    SDK::UObject* pawn = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(g_pc) + 0x340);
    if (!pawn) pawn = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(g_pc) + 0x2D8);
    SetViewTarget(pawn ? pawn : g_pc);
}

CamReport CamPose() { return { g_cam.x, g_cam.y, g_cam.z, g_cam.rp, g_cam.ry, g_cam.rr }; }
bool CamActive() { return Cam().active && ObjectAlive(g_cam.actor); }

void CameraApply()
{
    if (!ObjectAlive(g_cam.actor)) return;
    SDK::Params::Actor_K2_SetActorLocationAndRotation p{};
    p.NewLocation = SDK::FVector{ g_cam.x, g_cam.y, g_cam.z };
    p.NewRotation = SDK::FRotator{ g_cam.rp, g_cam.ry, g_cam.rr };
    p.bSweep = false;
    p.bTeleport = true;
    CallNative(g_cam.actor, "Actor", "K2_SetActorLocationAndRotation", p);
}

// Once per frame, from the ProcessEvent hook (game thread).
void CameraTick()
{
    CameraInput& in = Cam();
    if (!in.active || !ObjectAlive(g_cam.actor)) return;

    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    double dt = g_cam.primed ? double(now.QuadPart - g_cam.last.QuadPart) / double(freq.QuadPart) : 0.0;
    g_cam.last = now;
    g_cam.primed = true;
    if (dt > 0.1) dt = 0.1;                                      // a hitch must not fling the camera

    const float dx = in.dx.exchange(0.0f), dy = in.dy.exchange(0.0f);
    const int wheel = in.wheel.exchange(0);
    if (wheel) g_cam.speed = static_cast<float>((std::min)(20000.0, (std::max)(50.0, g_cam.speed * std::pow(1.25, wheel))));

    if (in.looking)
    {
        g_cam.yaw   += dx * 0.15;
        g_cam.pitch -= dy * 0.15;
        g_cam.pitch  = (std::max)(-89.0, (std::min)(89.0, g_cam.pitch));
    }
    CameraTurnToGravity(dt);
    double r[3];
    CameraFrame(r);
    if (in.looking)
    {
        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        double mv[3] = { 0, 0, 0 };
        const double fwd = (down('W') ? 1 : 0) - (down('S') ? 1 : 0);
        const double rgt = (down('D') ? 1 : 0) - (down('A') ? 1 : 0);
        const double up  = (down('E') ? 1 : 0) - (down('Q') ? 1 : 0);   // Q/E: along the local up
        for (int i = 0; i < 3; ++i) mv[i] = g_cam.fwd[i] * fwd + r[i] * rgt + g_cam.up[i] * up;
        const double step = g_cam.speed * (down(VK_SHIFT) ? 3.0 : 1.0) * dt;
        g_cam.x += mv[0] * step; g_cam.y += mv[1] * step; g_cam.z += mv[2] * step;
    }
    CameraApply();
}

// F: keep the view direction and move back far enough to frame the point.
void CameraFocus(const Vec3& p)
{
    if (!Cam().active) return;
    g_cam.x = p.x - g_cam.fwd[0] * 500.0; g_cam.y = p.y - g_cam.fwd[1] * 500.0; g_cam.z = p.z - g_cam.fwd[2] * 500.0;
    CameraApply();
}

// ── placing on geometry ──────────────────────────────────────────────────────────────────────
// Line-trace against the level (Visibility channel, complex collision) from `from` along `dir`. Drops
// and double-clicks land on whatever surface is under the cursor, as Unreal's asset drop does, instead
// of floating a fixed distance down the ray.
bool TraceWorld(const Vec3& from, const Vec3& dir, double maxDist, Vec3& hit)
{
    if (!g_pc) return false;
    SDK::Params::KismetSystemLibrary_LineTraceSingle p{};
    p.WorldContextObject = g_pc;
    p.Start = SDK::FVector{ from.x, from.y, from.z };
    p.End   = SDK::FVector{ from.x + dir.x * maxDist, from.y + dir.y * maxDist, from.z + dir.z * maxDist };
    p.TraceChannel = SDK::ETraceTypeQuery::TraceTypeQuery1;      // Visibility
    p.bTraceComplex = true;
    p.DrawDebugType = SDK::EDrawDebugTrace::None;
    p.bIgnoreSelf = true;
    if (!CallStatic("KismetSystemLibrary", "LineTraceSingle", p) || !p.ReturnValue || !p.OutHit.bBlockingHit)
        return false;
    hit = { p.OutHit.ImpactPoint.X, p.OutHit.ImpactPoint.Y, p.OutHit.ImpactPoint.Z };
    return true;
}

// ── making drags feel immediate ───────────────────────────────────────────────────────────────
// A transform used to be sent to the server and shown only once it replicated back, and the editor only
// re-read positions at 4 Hz -- so the gizmo trailed the mouse and moved in steps. Now the client moves
// its own copy at once (prediction; the server still decides, and its replicated value lands on top of
// ours), and every frame the known objects' transforms are re-read so the gizmo and markers track them.
void PredictTransform(const std::string& handle, const Vec3& loc, const Rot& rot, const Vec3& scale)
{
    SDK::UObject* a = ActorFor(handle);
    if (!a) return;
    SDK::Params::Actor_K2_SetActorLocationAndRotation p{};
    p.NewLocation = SDK::FVector{ loc.x, loc.y, loc.z };
    p.NewRotation = SDK::FRotator{ rot.pitch, rot.yaw, rot.roll };
    p.bSweep = false;
    p.bTeleport = true;
    CallNative(a, "Actor", "K2_SetActorLocationAndRotation", p);
    SDK::Params::Actor_SetActorScale3D s{};
    s.NewScale3D = SDK::FVector{ scale.x, scale.y, scale.z };
    CallNative(a, "Actor", "SetActorScale3D", s);
}

// The render view, every frame. While the editor camera flies, use its own pose -- set this frame, so
// exactly what is about to be drawn; otherwise the camera manager's cached POV.
void PublishCamera()
{
    float fov = 0.0f;
    Vec3 pos; Rot rot;
    bool have = false;
    if (g_pc)
        if (void* pcm = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_pc) + 0x350))
        {
            const uintptr_t pov = reinterpret_cast<uintptr_t>(pcm) + 0x13A0 + 0x10;
            const double* l = reinterpret_cast<const double*>(pov + 0x00);
            const double* r = reinterpret_cast<const double*>(pov + 0x18);
            pos = { l[0], l[1], l[2] }; rot = { r[0], r[1], r[2] };
            fov = *reinterpret_cast<const float*>(pov + 0x30);
            have = true;
        }
    if (Cam().active && ObjectAlive(g_cam.actor))
    {
        pos = { g_cam.x, g_cam.y, g_cam.z }; rot = { g_cam.rp, g_cam.ry, g_cam.rr };
        have = true;
    }
    if (have) State().ApplyCamera(pos, rot, fov);
}

void RefreshTransforms()
{
    for (SceneObject& o : g_lastObjects)
    {
        auto* a = static_cast<SDK::UObject*>(o.ptr);
        if (!Alive(a) || (*(reinterpret_cast<const uint8_t*>(a) + 0x65) & 0x01)) continue;
        void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(a) + 0x1A8);
        if (!root) continue;
        const double* q = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x00);
        const double* t = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20);
        const double* s = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x40);
        o.location = { t[0], t[1], t[2] };
        o.scale    = { s[0], s[1], s[2] };
        o.rotation = QuatToRot(q[0], q[1], q[2], q[3]);
    }
    State().ApplyTransforms(g_lastObjects);
}

// Gizmo drags (see LiveDrag). Runs up to 240 times a second on the game thread:
//  * the local actor is put where the gizmo is, the same frame -- no queue, no 4 Hz pump;
//  * it is HELD there until ~1.2 s after release, because replicated transforms from the server lag
//    the drag and would otherwise yank it backwards (the "object trails the gizmo" feel);
//  * the server gets at most 30 updates a second, plus the final one on release.
void DragTick()
{
    // A drag is the gizmo's object plus, for a Ctrl+click selection, the rest of the group. Every member is
    // moved here each frame; the server hears about them at a capped rate. It takes at most 60 editor
    // commands a second and each move costs it ~1.5 ms of game thread, so a group's updates share one
    // ~25/s budget (round-robin), and the release always sends every member's final place.
    static uint32_t    s_sentSeq = 0;
    static ULONGLONG   s_lastSend = 0, s_holdUntil = 0;
    static std::vector<LiveDrag::Member> s_all;       // [0] = the gizmo's object
    static size_t      s_next = 0;                    // round-robin position while dragging
    static bool        s_active = false;

    uint32_t seq;
    {
        LiveDrag& d = Drag();
        std::lock_guard<std::mutex> lk(d.mx);
        seq = d.seq;
        if (seq != s_sentSeq || d.active)
        {
            s_all.clear();
            if (!LockedByOther(d.handle)) s_all.push_back({ d.handle, d.loc, d.scale, d.rot });
            for (const auto& m : d.group) if (!LockedByOther(m.handle)) s_all.push_back(m);
            s_active = d.active;
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (s_all.empty() || s_all[0].handle.empty()) return;
    if (s_active) s_holdUntil = now + 1200;

    auto send = [&](const LiveDrag::Member& m) {
        const std::string id = IdentForEdit(m.handle);
        if (id.empty()) return;
        SendToServer("SE|XFORM|" + id + "|" + Fmt3(m.loc) + "|" + Fmt3(m.rot) + "|" + Fmt3(m.scale));
        NoteCommanded(m.handle, m.loc);
    };
    if (seq != s_sentSeq)
    {
        if (!s_active)                                                   // released: everyone's final place
        {
            for (const auto& m : s_all) send(m);
            s_sentSeq = seq;
            s_lastSend = now;
            s_holdUntil = now + 1200;
        }
        else if (now - s_lastSend >= (s_all.size() > 1 ? 40u : 33u))
        {
            if (s_next >= s_all.size()) s_next = 0;
            send(s_all[s_next++]);
            if (s_all.size() == 1 || s_next >= s_all.size()) s_sentSeq = seq;   // a full round went out
            s_lastSend = now;
        }
    }
    if (now < s_holdUntil) { for (const auto& m : s_all) PredictTransform(m.handle, m.loc, m.rot, m.scale); }
    else if (!s_active) { s_all.clear(); s_next = 0; }
}

void* CurrentWorld()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return *reinterpret_cast<void**>(base + SDK::Offsets::GWorld);
}
void*     g_connectFromWorld = nullptr;   // world we issued `open` from; the script waits for a new one
ULONGLONG g_arrivedAt = 0;

// ── test script (-SpecEditScript=<file>) ─────────────────────────────────────────────────────
// Drives the editor without a human, through EXACTLY the queue the UI pushes into, so a scripted run
// exercises the real path: game thread -> RPC -> server -> Iris replication -> back into our object
// list. One command per line; '#' comments:
//
//   wait <seconds>             enter | exit             log <text>
//   spawn <palette substring>  (4m along the camera's look direction, like the UI)
//   move <dx> <dy> <dz>        rotate <yaw delta>        delete        (act on the last spawned object)
//   quest <id> <title...>      (the last spawned object becomes the quest's single step)
//   expect <n>                 (logs PASS/FAIL: n objects of the last spawned class are visible)
std::vector<PaletteItem> g_palette;
std::vector<std::string> g_script;
size_t     g_scriptPc = 0;
ULONGLONG  g_scriptNext = 0;
bool       g_scriptLoaded = false;
std::string g_lastSpawnClass;
Vec3       g_lastSpawnLoc;
std::string g_lastHandle;
Rot        g_lastRot;

void LoadScriptOnce()
{
    if (g_scriptLoaded) return;
    g_scriptLoaded = true;
    const wchar_t* cl = GetCommandLineW();
    const wchar_t* p = wcsstr(cl, L"-SpecEditScript=");
    if (!p) return;
    p += wcslen(L"-SpecEditScript=");
    std::wstring path;
    const bool quoted = *p == L'"';
    if (quoted) ++p;
    while (*p && (quoted ? *p != L'"' : *p != L' ')) path += *p++;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"r") || !f) { Log("[script] cannot open %ls", path.c_str()); return; }
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string s(line);
        if (const size_t h = s.find('#'); h != std::string::npos) s = s.substr(0, h);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        if (!s.empty()) g_script.push_back(s);
    }
    fclose(f);
    Log("[script] loaded %d line(s) from %ls", (int)g_script.size(), path.c_str());
}

// ---- picking (see PickState) ----
SDK::UObject* WeakGet(const void* weak)                    // TWeakObjectPtr: ObjectIndex, SerialNumber
{
    const int32_t idx = *static_cast<const int32_t*>(weak);
    if (idx <= 0 || idx >= SDK::UObject::GObjects->Num()) return nullptr;
    return SDK::UObject::GObjects->GetByIndex(idx);
}

bool RayBoxT(const Vec3& e, const Vec3& r, const Vec3& c, const Vec3& h, double& tIn, double& tOut)
{
    const double o[3] = { e.x - c.x, e.y - c.y, e.z - c.z }, d[3] = { r.x, r.y, r.z }, x[3] = { h.x, h.y, h.z };
    tIn = -1e30; tOut = 1e30;
    for (int i = 0; i < 3; ++i)
    {
        if (std::fabs(d[i]) < 1e-12) { if (std::fabs(o[i]) > x[i]) return false; continue; }
        double t1 = (-x[i] - o[i]) / d[i], t2 = (x[i] - o[i]) / d[i];
        if (t1 > t2) std::swap(t1, t2);
        tIn = (std::max)(tIn, t1); tOut = (std::min)(tOut, t2);
        if (tIn > tOut) return false;
    }
    return tOut > 0.0;
}

// Red coin stand-ins (see CoinStandinTick): coin actor -> its local mesh actor.
struct CoinStandin { SDK::AActor* actor = nullptr; Vec3 at; Vec3 scale; bool hidden = false; Vec3 meshOff; bool measured = false; };
std::unordered_map<SDK::UObject*, CoinStandin> g_coinStandins;

std::string PickRay(const Vec3& eye, const Vec3& dir);
void PickTick()
{
    Vec3 eye, dir;
    {
        PickState& ps = Pick();
        std::lock_guard<std::mutex> lk(ps.mx);
        if (!ps.valid) { ps.hovered.clear(); return; }
        eye = ps.eye; dir = ps.dir;
    }
    const std::string hovered = PickRay(eye, dir);
    Vec3 hit;
    const bool hasHit = TraceWorld(eye, dir, 50000.0, hit);
    PickState& ps = Pick();
    std::lock_guard<std::mutex> lk(ps.mx);
    ps.hovered = hovered;
    ps.hasHit = hasHit;
    ps.hit = hit;
}

// What a click along this ray selects (the viewport's pick; also the clickat test op).
std::string PickRay(const Vec3& eye, const Vec3& dir)
{
    std::string hovered;
    double surface = 1e30;

    // 1) What the ray really hits. A placed object's own geometry answers directly -- a component of it,
    //    or of an actor it spawned (ChildActorComponent), so walk up Outer and the parent-actor link.
    SDK::Params::KismetSystemLibrary_LineTraceSingle p{};
    p.WorldContextObject = g_pc;
    p.Start = SDK::FVector{ eye.x, eye.y, eye.z };
    p.End   = SDK::FVector{ eye.x + dir.x * 200000.0, eye.y + dir.y * 200000.0, eye.z + dir.z * 200000.0 };
    p.TraceChannel = SDK::ETraceTypeQuery::TraceTypeQuery1;
    p.bTraceComplex = true;
    p.DrawDebugType = SDK::EDrawDebugTrace::None;
    p.bIgnoreSelf = true;
    if (g_pc && CallStatic("KismetSystemLibrary", "LineTraceSingle", p) && p.ReturnValue && p.OutHit.bBlockingHit)
    {
        surface = p.OutHit.Distance;
        SDK::UObject* o = WeakGet(reinterpret_cast<const uint8_t*>(&p.OutHit) + 0xD8);   // Component
        for (int depth = 0; o && depth < 6 && hovered.empty(); ++depth)
        {
            for (const auto& [coin, st] : g_coinStandins)            // a coin stand-in answers for its coin
                if (st.actor == o) { o = coin; break; }
            for (const SceneObject& so : g_lastObjects) if (so.ptr == o) { hovered = so.handle; break; }
            if (!hovered.empty()) break;
            SDK::UObject* next = o->Outer;
            if (auto* actorCls = SDK::UObject::FindClassFast("Actor"); actorCls && o->IsA(actorCls))
            {
                SDK::Params::Actor_GetParentActor pa{};
                if (CallNative(o, "Actor", "GetParentActor", pa) && pa.ReturnValue) next = pa.ReturnValue;
            }
            o = next;
        }
    }

    // 2) Objects with no collision (text, triggers): their boxes, in front of whatever the trace hit.
    if (hovered.empty())
    {
        double bestVol = 1e300;
        for (const SceneObject& so : g_lastObjects)
        {
            const Vec3 c{ so.location.x + so.boundsOff.x, so.location.y + so.boundsOff.y, so.location.z + so.boundsOff.z };
            const Vec3 h{ (std::max)(25.0, so.boundsExt.x), (std::max)(25.0, so.boundsExt.y), (std::max)(25.0, so.boundsExt.z) };
            double tIn, tOut;
            if (!RayBoxT(eye, dir, c, h, tIn, tOut) || tIn < 0.0 || tIn > surface + 20.0) continue;
            const double vol = h.x * h.y * h.z;
            if (vol < bestVol) { bestVol = vol; hovered = so.handle; }
        }
    }
    return hovered;
}

// Script-driven drag: feeds LiveDrag exactly as the gizmo does, one step per game frame.
struct SimDrag { bool on = false; std::string handle; Vec3 from, to, scale; Rot rot; ULONGLONG start = 0, ms = 1000; int frames = 0; };
SimDrag g_sim;

void SimDragTick()
{
    if (!g_sim.on) return;
    const ULONGLONG now = GetTickCount64();
    const double t = g_sim.ms ? (std::min)(1.0, double(now - g_sim.start) / double(g_sim.ms)) : 1.0;
    LiveDrag& d = Drag();
    std::lock_guard<std::mutex> lk(d.mx);
    d.handle = g_sim.handle;
    d.loc = { g_sim.from.x + (g_sim.to.x - g_sim.from.x) * t, g_sim.from.y + (g_sim.to.y - g_sim.from.y) * t,
              g_sim.from.z + (g_sim.to.z - g_sim.from.z) * t };
    d.rot = g_sim.rot; d.scale = g_sim.scale;
    d.active = t < 1.0;
    ++d.seq;
    ++g_sim.frames;
    if (t >= 1.0)
    {
        g_sim.on = false;
        Log("[script] drag released after %d step(s)", g_sim.frames);
    }
}

HWND GameWindow()
{
    struct Ctx { DWORD pid; HWND h; } ctx{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        auto* c = reinterpret_cast<Ctx*>(lp);
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        char cls[64] = {};
        GetClassNameA(h, cls, sizeof(cls));
        if (pid == c->pid && !strcmp(cls, "UnrealWindow") && IsWindowVisible(h)) { c->h = h; return FALSE; }
        return TRUE; }, reinterpret_cast<LPARAM>(&ctx));
    return ctx.h;
}

// The same projection the overlay uses (se_ui.cpp MakeView/W2S), in SCREEN pixels, so an external
// input driver can click exactly where the object and its handles are drawn.
void LogScreenPos(const Snapshot& snap, const SceneObject& o)
{
    HWND h = GameWindow();
    RECT rc{};
    if (!h || !GetClientRect(h, &rc) || snap.cameraFov <= 1.0f) { Log("[script] screenpos: no window/camera"); return; }
    POINT org{ 0, 0 };
    ClientToScreen(h, &org);
    const double W = rc.right, H = rc.bottom;
    const double d2r = 3.14159265358979 / 180.0;
    const double cp = std::cos(snap.cameraRot.pitch * d2r), sp = std::sin(snap.cameraRot.pitch * d2r);
    const double cy = std::cos(snap.cameraRot.yaw * d2r),   sy = std::sin(snap.cameraRot.yaw * d2r);
    const double cr = std::cos(snap.cameraRot.roll * d2r),  sr = std::sin(snap.cameraRot.roll * d2r);
    const Vec3 fwd{ cp * cy, cp * sy, sp };
    const Vec3 right{ sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp };
    const Vec3 up{ -(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp };
    const double focal = (W * 0.5) / std::tan(snap.cameraFov * 0.5 * d2r);
    auto proj = [&](const Vec3& p, double& x, double& y, double* depth) {
        const Vec3 d{ p.x - snap.cameraPos.x, p.y - snap.cameraPos.y, p.z - snap.cameraPos.z };
        const double z = d.x * fwd.x + d.y * fwd.y + d.z * fwd.z;
        if (z < 1.0) return false;
        x = org.x + W * 0.5 + (d.x * right.x + d.y * right.y + d.z * right.z) * focal / z;
        y = org.y + H * 0.5 - (d.x * up.x + d.y * up.y + d.z * up.z) * focal / z;
        if (depth) *depth = z;
        return true;
    };
    double cx, cyy, depth;
    if (!proj(o.location, cx, cyy, &depth)) { Log("[script] screenpos: behind the camera"); return; }
    const double wpp = depth / focal, L = 95.0;
    double hx[3], hy[3];
    const Vec3 ax[3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 } };
    for (int i = 0; i < 3; ++i)
    {
        const Vec3 tip{ o.location.x + ax[i].x * L * 0.7 * wpp, o.location.y + ax[i].y * L * 0.7 * wpp, o.location.z + ax[i].z * L * 0.7 * wpp };
        if (!proj(tip, hx[i], hy[i], nullptr)) { hx[i] = hy[i] = -1; }
    }
    // Z ring (Rotate): the point at angle 0 of the circle in the X/Y plane.
    double rzx = -1, rzy = -1;
    const Vec3 ringZ{ o.location.x + L * wpp, o.location.y, o.location.z };
    proj(ringZ, rzx, rzy, nullptr);
    // The XY plane square (Move): its centre sits 0.32 of the handle length along both X and Y.
    double pxx = -1, pxy = -1;
    const Vec3 planeXY{ o.location.x + 0.32 * L * wpp, o.location.y + 0.32 * L * wpp, o.location.z };
    proj(planeXY, pxx, pxy, nullptr);
    Log("[script] SCREENPOS center=%.0f,%.0f handleX=%.0f,%.0f handleY=%.0f,%.0f handleZ=%.0f,%.0f ringZ=%.0f,%.0f planeXY=%.0f,%.0f worldPerPx=%.4f",
        cx, cyy, hx[0], hy[0], hx[1], hy[1], hx[2], hy[2], rzx, rzy, pxx, pxy, wpp);
}

// The replicated copy of the object we last spawned: same class, nearest the spot we asked for.
const SceneObject* FindLastSpawned()
{
    const SceneObject* best = nullptr;
    double bestD = 600.0 * 600.0;
    for (const SceneObject& o : g_lastObjects)
    {
        if (o.className != g_lastSpawnClass) continue;
        const double dx = o.location.x - g_lastSpawnLoc.x, dy = o.location.y - g_lastSpawnLoc.y,
                     dz = o.location.z - g_lastSpawnLoc.z;
        const double d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = &o; }
    }
    return best;
}

// TEST: count the quest definitions this client holds whose title contains `needle` (ClientProgression,
// sub_1446849A0(world); Definitions TSet @+336: elements of FAAQuestEntry 0x120 + 8 bytes of hash links).
int CountQuestDefsImpl(const wchar_t* needle, int* total)
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* world = *reinterpret_cast<void**>(base + SDK::Offsets::GWorld);
    if (!world) return -1;
    auto* cp = reinterpret_cast<uint8_t*>(reinterpret_cast<void*(__fastcall*)(void*)>(base + 0x46849A0)(world));
    if (!cp) return -2;
    const uint8_t* data = *reinterpret_cast<uint8_t* const*>(cp + 336);
    const int n = *reinterpret_cast<const int32_t*>(cp + 344);
    int hits = 0;
    *total = 0;
    for (int i = 0; data && i < n && i < 8192; ++i)
    {
        const uint8_t* e = data + static_cast<size_t>(i) * 0x128;
        const wchar_t* t = *reinterpret_cast<const wchar_t* const*>(e + 0xF0);
        const int len = *reinterpret_cast<const int32_t*>(e + 0xF8);
        if (!t || len <= 0 || len > 256) continue;
        __try { ++*total; if (wcsstr(t, needle)) ++hits; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return hits;
}
int CountQuestDefs(const wchar_t* needle, int* total)
{
    __try { return CountQuestDefsImpl(needle, total); } __except (EXCEPTION_EXECUTE_HANDLER) { return -3; }
}

void RunScript(const Snapshot& snap)
{
    LoadScriptOnce();
    if (g_scriptPc >= g_script.size())
    {
        static bool s_done = false;                           // the marker test runners wait for
        if (!s_done && !g_script.empty()) { s_done = true; Log("[script] SCRIPT COMPLETE"); }
        return;
    }
    if (!g_pc || !snap.worldReady) return;
    const ULONGLONG now = GetTickCount64();
    // With -SpecEditConnect, do not start until we have actually travelled: the entry map has a
    // controller too, and commands sent from it would go nowhere. Then give the station 8s to settle.
    if (wcsstr(GetCommandLineW(), L"-SpecEditConnect="))
    {
        if (!g_connectFromWorld || CurrentWorld() == g_connectFromWorld) { g_arrivedAt = 0; return; }
        if (!g_arrivedAt) { g_arrivedAt = now; Log("[script] arrived in the server's world; starting in 8s"); }
        if (now - g_arrivedAt < 8000) return;
    }
    if (now < g_scriptNext) return;

    const std::string line = g_script[g_scriptPc++];
    char op[32] = {};
    sscanf_s(line.c_str(), "%31s", op, (unsigned)sizeof(op));
    const std::string rest = line.size() > strlen(op) ? line.substr(strlen(op) + 1) : "";
    Log("[script] > %s", line.c_str());

    if (!strcmp(op, "wait"))       { g_scriptNext = now + (ULONGLONG)(atof(rest.c_str()) * 1000.0); return; }
    if (!strcmp(op, "log"))        { Log("[script] %s", rest.c_str()); return; }
    if (!strcmp(op, "enter"))      { State().Push({ CmdType::EnterEditor }); return; }
    if (!strcmp(op, "exit"))       { State().Push({ CmdType::ExitEditor }); return; }

    if (!strcmp(op, "spawn") || !strcmp(op, "spawnat"))   // spawnat <Class> x y z yaw -- at a fixed spot
    {
        std::string what = rest;
        double fx = 0, fy = 0, fz = 0, fyaw = 0;
        const bool fixed = !strcmp(op, "spawnat");
        if (fixed)
        {
            char name[128] = {};
            if (sscanf_s(rest.c_str(), "%127s %lf %lf %lf %lf", name, (unsigned)sizeof(name), &fx, &fy, &fz, &fyaw) < 4)
            { Log("[script] FAIL spawnat: want <Class> x y z [yaw]"); return; }
            what = name;
        }
        const PaletteItem* pick = nullptr;
        for (const auto& it : g_palette) if (it.name.find(what) != std::string::npos) { pick = &it; break; }
        if (!pick) { Log("[script] FAIL spawn: nothing in the %d-item palette matches '%s'", (int)g_palette.size(), what.c_str()); return; }
        const double d2r = 3.14159265358979 / 180.0;
        const double cp = std::cos(snap.cameraRot.pitch * d2r), sp = std::sin(snap.cameraRot.pitch * d2r);
        const double cy = std::cos(snap.cameraRot.yaw * d2r),   sy = std::sin(snap.cameraRot.yaw * d2r);
        Command c{ CmdType::SpawnItem };
        c.str = pick->path;
        c.loc = { snap.cameraPos.x + cp * cy * 400.0, snap.cameraPos.y + cp * sy * 400.0, snap.cameraPos.z + sp * 400.0 };
        c.rot = { 0.0, snap.cameraRot.yaw + 180.0, 0.0 };   // facing the camera, as the UI places things
        if (fixed) { c.loc = { fx, fy, fz }; c.rot = { 0.0, fyaw, 0.0 }; }
        g_lastSpawnClass = pick->name;
        g_lastSpawnLoc = c.loc;
        g_lastRot = c.rot;
        g_lastHandle.clear();
        State().Push(c);
        Log("[script] spawn %s at (%.0f,%.0f,%.0f)", pick->name.c_str(), c.loc.x, c.loc.y, c.loc.z);
        return;
    }

    if (!strcmp(op, "classes"))               // classes -- every LE class present, with counts (probing)
    {
        std::vector<std::pair<std::string, int>> counts;
        for (const SceneObject& s : g_lastObjects)
        {
            bool found = false;
            for (auto& c : counts) if (c.first == s.className) { ++c.second; found = true; break; }
            if (!found) counts.push_back({ s.className, 1 });
        }
        for (const auto& c : counts) Log("[script]   %4d  %s", c.second, c.first.c_str());
        return;
    }
    if (!strcmp(op, "spawnatplayer"))         // spawnatplayer <Class> <dx> <dy> <dz> -- next to another player
    {
        char cls[128] = {};
        double dx = 0, dy = 0, dz = 0;
        sscanf_s(rest.c_str(), "%127s %lf %lf %lf", cls, (unsigned)sizeof(cls), &dx, &dy, &dz);
        auto* pawnCls = SDK::UObject::FindClassFast("VRPawn");
        void* mine = g_pc ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_pc) + 0x340) : nullptr;   // AcknowledgedPawn
        SDK::UObject* other = nullptr;
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; pawnCls && i < n && !other; ++i)
        {
            SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o == mine || o->IsDefaultObject() || !o->IsA(pawnCls) || (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 1)) continue;
            void* r = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1A8);
            if (!r) continue;
            const double z = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(r) + 0x1D0 + 0x20)[2];
            if (z < -50000.0) continue;                        // an unpossessed pawn parked out of the world
            other = o;
        }
        const PaletteItem* pick = nullptr;
        for (const auto& it : g_palette) if (it.name.find(cls) != std::string::npos) { pick = &it; break; }
        if (!other || !pick) { Log("[script] FAIL spawnatplayer: %s", !other ? "no other player pawn" : "class not in palette"); return; }
        void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(other) + 0x1A8);
        const double* t = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20);
        Command c{ CmdType::SpawnItem };
        c.str = pick->path;
        c.loc = { t[0] + dx, t[1] + dy, t[2] + dz };
        c.rot = { 0, 0, 0 };
        g_lastSpawnClass = pick->name; g_lastSpawnLoc = c.loc;
        State().Push(c);
        Log("[script] spawnatplayer %s at (%.0f,%.0f,%.0f) next to %s", pick->name.c_str(), c.loc.x, c.loc.y, c.loc.z, other->GetName().c_str());
        return;
    }
    if (!strcmp(op, "dumpscripts"))           // dumpscripts <folder> -- write every game Luau script to files
    {
        Command c{ CmdType::ScanScripts }; State().Push(c);
        g_dumpScriptsTo = rest;
        return;
    }
    if (!strcmp(op, "sbtypes")) { SendToServer("SE|SBTYPES"); return; }   // LOCAL TEST: server lists every prefab type
    if (!strcmp(op, "raw")) { SendToServer(rest); Log("[script] raw %s", rest.c_str()); return; }   // raw <SE|...>
    if (!strcmp(op, "traceat"))               // traceat x y z -- what does THIS client's world block, straight down
    {                                         // through a point (6 m above to 6 m below)?
        Vec3 at{};
        if (sscanf_s(rest.c_str(), "%lf %lf %lf", &at.x, &at.y, &at.z) != 3) { Log("[script] FAIL traceat: want x y z"); return; }
        SDK::Params::KismetSystemLibrary_LineTraceSingle p{};
        p.WorldContextObject = g_pc;
        p.Start = SDK::FVector{ at.x, at.y, at.z + 600.0 };
        p.End   = SDK::FVector{ at.x, at.y, at.z - 600.0 };
        p.TraceChannel = SDK::ETraceTypeQuery::TraceTypeQuery1;
        p.DrawDebugType = SDK::EDrawDebugTrace::None;
        p.bIgnoreSelf = true;
        std::string what = "nothing";
        if (g_pc && CallStatic("KismetSystemLibrary", "LineTraceSingle", p) && p.ReturnValue && p.OutHit.bBlockingHit)
        {
            SDK::UObject* c = WeakGet(reinterpret_cast<const uint8_t*>(&p.OutHit) + 0xD8);
            SDK::UObject* owner = c ? c->Outer : nullptr;
            char b[256];
            snprintf(b, sizeof(b), "%s (%s) at z=%.0f", owner ? owner->GetName().c_str() : "?", owner && owner->Class ? owner->Class->GetName().c_str() : "?",
                     p.OutHit.ImpactPoint.Z);
            what = b;
        }
        Log("[script] TRACEAT (%.0f,%.0f,%.0f): hit %s", at.x, at.y, at.z, what.c_str());
        return;
    }
    if (!strcmp(op, "vis"))                   // vis <Class> x y z [0|1] -- is the instance nearest a point SHOWING on
    {                                         // this client? (a script's hideLua/showLua; other players' view)
        char name[128] = {};
        Vec3 at{};
        int want = -1;
        const int got = sscanf_s(rest.c_str(), "%127s %lf %lf %lf %d", name, (unsigned)sizeof(name), &at.x, &at.y, &at.z, &want);
        if (got < 4) { Log("[script] FAIL vis: want <Class> x y z [0|1]"); return; }
        const SceneObject* best = nullptr;
        double bestD = 1e30;
        for (const SceneObject& s : g_lastObjects)
        {
            if (s.className.find(name) == std::string::npos) continue;
            const double dx = s.location.x - at.x, dy = s.location.y - at.y, dz = s.location.z - at.z;
            const double d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = &s; }
        }
        if (!best || bestD > 600.0 * 600.0) { Log("[script] %s vis: no %s within 6m of (%.0f,%.0f,%.0f)", want == -1 ? "INFO" : "FAIL", name, at.x, at.y, at.z); return; }
        auto* a = static_cast<SDK::AActor*>(best->ptr);
        int prims = 0, shown = 0;                             // the actor's primitives (its default subobjects)
        auto* pcls = SDK::UObject::FindClassFast("PrimitiveComponent");
        const int32_t nObj = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; pcls && i < nObj; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (!c || c->Outer != a || !c->IsA(pcls)) continue;
            auto* p = static_cast<SDK::UPrimitiveComponent*>(c);
            ++prims;
            if (p->bVisible && !p->bHiddenInGame) ++shown;
        }
        const bool showing = !a->bHidden && shown > 0;
        const char* verdict = want == -1 ? "INFO" : (showing == (want == 1) ? "PASS" : "FAIL");
        Log("[script] %s vis %s at (%.0f,%.0f,%.0f) rot (%.0f,%.0f,%.0f) scale (%.2f,%.2f,%.2f): %s (actor hidden=%d, %d/%d primitive(s) visible)", verdict,
            a->GetName().c_str(), best->location.x, best->location.y, best->location.z,
            best->rotation.pitch, best->rotation.yaw, best->rotation.roll, best->scale.x, best->scale.y, best->scale.z,
            showing ? "SHOWING" : "HIDDEN", a->bHidden ? 1 : 0, shown, prims);
        return;
    }
    if (!strcmp(op, "coinrun"))               // coinrun <sec> <questHex32> -- a red-coin run 4 m ahead: 3 coins + a start button
    {
        char q[64] = {};
        double sec = 30;
        if (sscanf_s(rest.c_str(), "%lf %63s", &sec, q, (unsigned)sizeof(q)) != 2) { Log("[script] FAIL coinrun: want <sec> <questHex32>"); return; }
        const double d2r = 3.14159265358979 / 180.0;
        const double cy = std::cos(snap.cameraRot.yaw * d2r), sy = std::sin(snap.cameraRot.yaw * d2r);
        const double bx = snap.cameraPos.x + cy * 400.0, by = snap.cameraPos.y + sy * 400.0, bz = snap.cameraPos.z - 80.0;
        char msg[600];
        snprintf(msg, sizeof(msg), "SE|COINRUN|new|%.1f,%.1f,%.1f|%.0f|%s|%.1f,%.1f,%.1f;%.1f,%.1f,%.1f;%.1f,%.1f,%.1f|%.1f,%.1f,%.1f",
                 bx, by, bz, sec, q,
                 bx + cy * 300.0, by + sy * 300.0, bz + 60.0,
                 bx - sy * 300.0, by + cy * 300.0, bz + 60.0,
                 bx + sy * 300.0, by - cy * 300.0, bz + 60.0,
                 bx - sy * 150.0, by + cy * 150.0, bz);
        SendToServer(msg);
        g_lastSpawnClass = "LE_BP_New_RedCoinTimedQuest_C";
        g_lastSpawnLoc = { bx, by, bz };
        g_lastHandle.clear();
        Log("[script] coinrun at (%.0f,%.0f,%.0f) %0.fs quest %s", bx, by, bz, sec, q);
        return;
    }
    if (!strcmp(op, "gravprobe"))             // gravprobe -- the gravity zones in the level, and the camera's gravity
    {
        const int32_t n = SDK::UObject::GObjects->Num();
        auto* acls = SDK::UObject::FindClassFast("Actor");
        int found = 0;
        for (int32_t i = 0; acls && i < n; ++i)
        {
            SDK::UObject* ob = SDK::UObject::GObjects->GetByIndex(i);
            if (!ob || !ob->Class || ob->IsDefaultObject() || !ob->IsA(acls)) continue;
            const std::string cn = ob->Class->GetName();
            if (cn.find("Gravity") == std::string::npos) continue;
            auto* a = static_cast<SDK::AActor*>(ob);
            const SDK::FVector l = a->RootComponent ? a->RootComponent->RelativeLocation : SDK::FVector{};
            const SDK::FRotator rr = a->RootComponent ? a->RootComponent->RelativeRotation : SDK::FRotator{};
            Log("[script] GRAV %s (%s) at (%.0f,%.0f,%.0f) rot(%.0f,%.0f,%.0f)", ob->GetName().c_str(), cn.c_str(), l.X, l.Y, l.Z,
                rr.Pitch, rr.Yaw, rr.Roll);
            if (cn.find("Oneill") != std::string::npos)
            {
                const uintptr_t b = reinterpret_cast<uintptr_t>(ob);
                const double* ax = reinterpret_cast<const double*>(b + 0x308);
                Log("[script] GRAV   oneill type=%d axis=%d maxDist=%.0f width=%.0f meshR=%.0f arena=%.0f rotAxis=(%.2f,%.2f,%.2f) pawns=%d props=%d",
                    *reinterpret_cast<uint8_t*>(b + 0x298), *reinterpret_cast<uint8_t*>(b + 0x29A), *reinterpret_cast<float*>(b + 0x29C),
                    *reinterpret_cast<float*>(b + 0x2A0), *reinterpret_cast<float*>(b + 0x304), *reinterpret_cast<float*>(b + 0x2E4),
                    ax[0], ax[1], ax[2], *reinterpret_cast<int32_t*>(b + 0x358), *reinterpret_cast<int32_t*>(b + 0x370));
            }
            if (++found >= 40) break;
        }
        double up[3];
        g_gravZonesAt = 0;
        const GravZone* by = CameraGravityUp(up);
        for (const GravZone& z : g_gravZones)
            Log("[script] GRAV   model %s %s bounds (%.0f,%.0f,%.0f)-(%.0f,%.0f,%.0f) up=(%.2f,%.2f,%.2f) maxDist=%.0f", z.actor->GetName().c_str(),
                z.cylinder ? "CYL" : "VOL", z.lo[0], z.lo[1], z.lo[2], z.hi[0], z.hi[1], z.hi[2], z.up[0], z.up[1], z.up[2], z.maxDist);
        Log("[script] GRAV camera at (%.0f,%.0f,%.0f) zone=%s up=(%.2f,%.2f,%.2f) frameUp=(%.2f,%.2f,%.2f) rot(p=%.1f,y=%.1f,r=%.1f); %d zone actor(s), %zu modelled",
            g_cam.x, g_cam.y, g_cam.z, by ? by->actor->GetName().c_str() : "none", up[0], up[1], up[2],
            g_cam.up[0], g_cam.up[1], g_cam.up[2], g_cam.rp, g_cam.ry, g_cam.rr, found, g_gravZones.size());
        return;
    }
    if (!strcmp(op, "camto"))                 // camto x y z -- put the editor camera there
    {
        double x = 0, y = 0, z = 0;
        if (sscanf_s(rest.c_str(), "%lf %lf %lf", &x, &y, &z) != 3) { Log("[script] FAIL camto: want x y z"); return; }
        g_cam.x = x; g_cam.y = y; g_cam.z = z;
        CameraApply();
        Log("[script] camto (%.0f,%.0f,%.0f)", x, y, z);
        return;
    }
    if (!strcmp(op, "dupsel")) { RequestUiSelect("!dup"); Log("[script] dupsel"); return; }       // Ctrl+D
    if (!strcmp(op, "selinfo")) { RequestUiSelect("!sel"); return; }                            // log the selection
    if (!strcmp(op, "gmove"))                 // gmove dx dy dz -- move the whole selection with the gizmo's release
    {
        RequestUiSelect("!gmove " + rest);
        Log("[script] gmove %s", rest.c_str());
        return;
    }
    // ---- assertions (each logs PASS or FAIL, which the regression suite counts) ----
    if (!strcmp(op, "expectnear"))            // expectnear <Class> x y z <count> [radius=60] -- that many of it there
    {
        char name[128] = {}; Vec3 at; int want = 0; double rad = 60.0;
        if (sscanf_s(rest.c_str(), "%127s %lf %lf %lf %d %lf", name, (unsigned)sizeof(name), &at.x, &at.y, &at.z, &want, &rad) < 5)
        { Log("[script] FAIL expectnear: want <Class> x y z <count> [radius]"); return; }
        int n = 0;
        for (const SceneObject& so : g_lastObjects)
        {
            if (so.className.find(name) == std::string::npos) continue;
            const double dx = so.location.x - at.x, dy = so.location.y - at.y, dz = so.location.z - at.z;
            if (dx * dx + dy * dy + dz * dz <= rad * rad) ++n;
        }
        Log("[script] %s expectnear %s at (%.0f,%.0f,%.0f) r=%.0f: %d (wanted %d)", n == want ? "PASS" : "FAIL", name, at.x, at.y, at.z, rad, n, want);
        return;
    }
    if (!strcmp(op, "expectxf"))              // expectxf <Class> x y z pitch yaw roll sx sy sz -- the one nearest has that transform
    {
        char name[128] = {}; Vec3 at, sc; Rot r;
        if (sscanf_s(rest.c_str(), "%127s %lf %lf %lf %lf %lf %lf %lf %lf %lf", name, (unsigned)sizeof(name), &at.x, &at.y, &at.z,
                     &r.pitch, &r.yaw, &r.roll, &sc.x, &sc.y, &sc.z) != 10) { Log("[script] FAIL expectxf: bad args"); return; }
        const SceneObject* best = nullptr; double bd = 1e30;
        for (const SceneObject& so : g_lastObjects)
        {
            if (so.className.find(name) == std::string::npos) continue;
            const double dx = so.location.x - at.x, dy = so.location.y - at.y, dz = so.location.z - at.z, d = dx * dx + dy * dy + dz * dz;
            if (d < bd) { bd = d; best = &so; }
        }
        if (!best || bd > 60.0 * 60.0) { Log("[script] FAIL expectxf: no %s near (%.0f,%.0f,%.0f)", name, at.x, at.y, at.z); return; }
        auto angOk = [](double a, double b) { double d = std::fmod(std::fabs(a - b), 360.0); return (std::min)(d, 360.0 - d) < 1.5; };
        // A rotator has two spellings for one orientation (p,y,r) == (180-p, y+180, r+180): compare either.
        const bool rotOk = (angOk(best->rotation.pitch, r.pitch) && angOk(best->rotation.yaw, r.yaw) && angOk(best->rotation.roll, r.roll)) ||
                           (angOk(best->rotation.pitch, 180 - r.pitch) && angOk(best->rotation.yaw, r.yaw + 180) && angOk(best->rotation.roll, r.roll + 180));
        const bool sOk = std::fabs(best->scale.x - sc.x) < 0.02 && std::fabs(best->scale.y - sc.y) < 0.02 && std::fabs(best->scale.z - sc.z) < 0.02;
        Log("[script] %s expectxf %s: rot(%.1f,%.1f,%.1f) scale(%.2f,%.2f,%.2f), wanted rot(%.1f,%.1f,%.1f) scale(%.2f,%.2f,%.2f)",
            rotOk && sOk ? "PASS" : "FAIL", name, best->rotation.pitch, best->rotation.yaw, best->rotation.roll, best->scale.x, best->scale.y,
            best->scale.z, r.pitch, r.yaw, r.roll, sc.x, sc.y, sc.z);
        return;
    }
    if (!strcmp(op, "mem"))                   // mem [label] -- this client's private memory (leak checks)
    {
        PROCESS_MEMORY_COUNTERS_EX pm{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
        Log("[script] MEM %s private=%.1f MB working=%.1f MB", rest.c_str(), pm.PrivateUsage / 1048576.0, pm.WorkingSetSize / 1048576.0);
        if (g_memFirstMB < 0) g_memFirstMB = pm.PrivateUsage / 1048576.0;
        return;
    }
    if (!strcmp(op, "expectmemgrowth"))       // expectmemgrowth <max MB> -- private memory since the first `mem`
    {
        PROCESS_MEMORY_COUNTERS_EX pm{};
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof(pm));
        const double grew = pm.PrivateUsage / 1048576.0 - g_memFirstMB, lim = atof(rest.c_str());
        Log("[script] %s expectmemgrowth: +%.1f MB since the first sample (limit %.1f)", g_memFirstMB >= 0 && grew <= lim ? "PASS" : "FAIL", grew, lim);
        return;
    }
    if (!strcmp(op, "expecticon"))            // expecticon <item name> 0|1 -- the game's own icon is loaded for it
    {
        char name[128] = {}; int want = 1;
        sscanf_s(rest.c_str(), "%127s %d", name, (unsigned)sizeof(name), &want);
        const bool has = IconTexture(name) != 0;
        Log("[script] %s expecticon %s: %s", has == (want != 0) ? "PASS" : "FAIL", name, has ? "game icon" : "drawn fallback");
        return;
    }
    if (!strcmp(op, "expectmarkers"))         // expectmarkers <n> -- camera markers this client can see (not its own)
    {
        int want = atoi(rest.c_str()), shown = 0, mine = 0;
        auto* acls = SDK::UObject::FindClassFast("BP_LevelEditor_Pawn_C");
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; acls && i < n; ++i)
        {
            SDK::UObject* ob = SDK::UObject::GObjects->GetByIndex(i);
            if (!ob || ob->IsDefaultObject() || ob->Class != acls) continue;
            if (*(reinterpret_cast<const uint8_t*>(ob) + 0x65) & 0x01) continue;
            auto* ma = static_cast<SDK::AActor*>(ob);
            if (g_pc && ma->Owner == static_cast<SDK::AActor*>(g_pc)) { ++mine; continue; }
            if (ma->bHidden) continue;
            // Really drawn? Its Cube mesh (BP_LevelEditor_Pawn_C +0x440) must be visible, not just the actor.
            auto* cube = *reinterpret_cast<SDK::UPrimitiveComponent**>(reinterpret_cast<uintptr_t>(ob) + 0x440);
            const bool drawn = cube && cube->bVisible && !cube->bHiddenInGame;
            const SDK::FVector l = ma->RootComponent ? ma->RootComponent->RelativeLocation : SDK::FVector{};
            Log("[script] marker %s at (%.0f,%.0f,%.0f) cube=%s", ob->GetName().c_str(), l.X, l.Y, l.Z, drawn ? "VISIBLE" : "not drawn");
            if (drawn) ++shown;
        }
        Log("[script] %s expectmarkers: %d visible other-editor marker(s) (wanted %d), %d of my own (hidden)", shown == want ? "PASS" : "FAIL",
            shown, want, mine);
        return;
    }
    if (!strcmp(op, "expectup"))              // expectup ux uy uz -- the editor camera's gravity frame up (after camto)
    {
        double u[3] = {};
        sscanf_s(rest.c_str(), "%lf %lf %lf", &u[0], &u[1], &u[2]);
        const double d = std::fabs(g_cam.up[0] - u[0]) + std::fabs(g_cam.up[1] - u[1]) + std::fabs(g_cam.up[2] - u[2]);
        Log("[script] %s expectup: camera up (%.2f,%.2f,%.2f), wanted (%.2f,%.2f,%.2f); rot(p=%.1f,y=%.1f,r=%.1f)", d < 0.15 ? "PASS" : "FAIL",
            g_cam.up[0], g_cam.up[1], g_cam.up[2], u[0], u[1], u[2], g_cam.rp, g_cam.ry, g_cam.rr);
        return;
    }
    if (!strcmp(op, "fav"))                   // fav <palette substring> -- star/unstar it (Favorites), then log its state
    {
        RequestUiSelect("!fav " + rest);
        return;
    }
    if (!strcmp(op, "expectfav"))             // expectfav <palette substring> 0|1
    {
        RequestUiSelect("!expectfav " + rest);
        return;
    }
    if (!strcmp(op, "lvexport")) { Command c{ CmdType::LevelExport }; c.str = rest; State().Push(c); return; }   // lvexport <name>
    if (!strcmp(op, "lvimport"))              // lvimport <file name> <load|save> -- Documents\RigelLevels\<file>.a2level
    {
        char nm[80] = {}, mode[16] = {};
        sscanf_s(rest.c_str(), "%79s %15s", nm, (unsigned)sizeof(nm), mode, (unsigned)sizeof(mode));
        const std::string sn = nm;
        const std::wstring path = LevelsDir() + L"\\" + std::wstring(sn.begin(), sn.end()) + L".a2level";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) { Log("[script] FAIL lvimport: no %ls", path.c_str()); return; }
        std::string text;
        char buf[4096];
        for (size_t r; (r = fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, r);
        fclose(f);
        Command c{ CmdType::LevelImport }; c.str = sn; c.str2 = !strcmp(mode, "save") ? "save" : "load"; c.str3 = text;
        State().Push(c);
        Log("[script] lvimport %s (%zu byte(s), %s)", nm, text.size(), c.str2.c_str());
        return;
    }
    if (!strcmp(op, "sbadd"))                 // sbadd <UniqueID> -- LOCAL TEST: sandbox-system placement in front of us
    {
        const double d2r = 3.14159265358979 / 180.0;
        const double cp = std::cos(snap.cameraRot.pitch * d2r), sp = std::sin(snap.cameraRot.pitch * d2r);
        const double cy = std::cos(snap.cameraRot.yaw * d2r),   sy = std::sin(snap.cameraRot.yaw * d2r);
        char loc[96];
        snprintf(loc, sizeof(loc), "%.1f,%.1f,%.1f", snap.cameraPos.x + cp * cy * 400.0, snap.cameraPos.y + cp * sy * 400.0,
                 snap.cameraPos.z + sp * 400.0);
        SendToServer("SE|SBADD|" + rest + "|" + loc);
        Log("[script] sbadd %s at %s", rest.c_str(), loc);
        return;
    }
    if (!strcmp(op, "scripts"))               // scripts [filter] -- Luau scripts each loaded gamemode carries (LGM+720)
    {
        auto* cls = SDK::UObject::FindClassFast("LoadedGameMode");
        const int32_t n = SDK::UObject::GObjects->Num();
        auto readable = [](const void* p, size_t len) { return p && !IsBadReadPtr(p, len); };
        auto wstr = [&](const void* elem, std::string& out) -> bool {
            if (!readable(elem, 16)) return false;
            const wchar_t* w = *reinterpret_cast<wchar_t* const*>(elem);
            const int len = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(elem) + 8);
            if (len <= 1 || len > 200 || !readable(w, len * 2)) return false;
            out.clear();
            for (int i = 0; i < len - 1; ++i) { if (w[i] < 32 || w[i] > 126) return false; out.push_back(static_cast<char>(w[i])); }
            return true;
        };
        for (int32_t i = 0; cls && i < n; ++i)
        {
            SDK::UObject* l = SDK::UObject::GObjects->GetByIndex(i);
            if (!l || l->IsDefaultObject() || !l->IsA(cls)) continue;
            const uintptr_t map = reinterpret_cast<uintptr_t>(l) + 720;
            const uint8_t* data = *reinterpret_cast<uint8_t* const*>(map);
            const int num = *reinterpret_cast<const int32_t*>(map + 8);
            size_t stride = 0;
            for (size_t st : { (size_t)0x20, (size_t)0x28, (size_t)0x30, (size_t)0x38, (size_t)0x40 })
            {
                std::string t0, t1;
                if (num >= 2 && wstr(data, t0) && wstr(data + st, t1)) { stride = st; break; }
            }
            std::string slotId = "?";
            if (void* slotActor = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(l) + 0x320))
                wstr(reinterpret_cast<uint8_t*>(slotActor) + 0x390, slotId);
            Log("[script] SCRIPTS %s slot='%s' entries=%d stride=0x%zX", l->GetName().c_str(), slotId.c_str(), num, stride);
            for (int k = 0; stride && k < num && k < 400; ++k)
            {
                std::string key;
                if (!wstr(data + k * stride, key)) continue;
                if (!rest.empty() && key.find(rest) == std::string::npos) continue;
                Log("[script]   script '%s'", key.c_str());
                if (rest.empty() || stride < 0x20) continue;
                // A filter was given: print the source too (the value FString right after the key).
                const uint8_t* ve = data + k * stride + 16;
                const wchar_t* src = readable(ve, 16) ? *reinterpret_cast<wchar_t* const*>(ve) : nullptr;
                const int len = src ? *reinterpret_cast<const int32_t*>(ve + 8) : 0;
                if (!src || len <= 1 || len > 200000 || !readable(src, len * 2)) { Log("[script]     (no readable source)"); continue; }
                std::string line;
                for (int c = 0; c < len - 1; ++c)
                {
                    if (src[c] == L'\n') { Log("[src] %s", line.c_str()); line.clear(); continue; }
                    if (src[c] != L'\r') line.push_back(src[c] < 128 ? static_cast<char>(src[c]) : '?');
                }
                if (!line.empty()) Log("[src] %s", line.c_str());
            }
        }
        return;
    }
    if (!strcmp(op, "sbtexts"))               // sbtexts <text> -- LOCAL TEST: four sandbox texts around a real player
    {
        SendToServer("SE|SBTEXTS|" + rest);
        return;
    }
    if (!strcmp(op, "sandbox"))               // sandbox -- LOCAL TEST: server logs the sandbox object system
    {
        SendToServer("SE|SANDBOX");
        return;
    }
    if (!strcmp(op, "testquest"))             // testquest <id> -- LOCAL TEST: server builds a quest at a real player
    {
        SendToServer("SE|TESTQUEST|" + rest);
        return;
    }
    if (!strcmp(op, "qprev"))                 // qprev <n> <title> -- red coin run draft with n preview coins
    {
        int n = 3, used = 0;
        sscanf_s(rest.c_str(), "%d %n", &n, &used);
        ScriptCoinPreview(snap, rest.substr(used), n);
        Log("[script] qprev %d coins", n);
        return;
    }
    if (!strcmp(op, "qprevcheck"))            // qprevcheck <n> -- PASS when n preview coins were adopted
    {
        const int want = atoi(rest.c_str()), got = ScriptCoinPreviewAdopted();
        Log("[script] %s qprevcheck adopted %d, want %d", got == want ? "PASS" : "FAIL", got, want);
        return;
    }
    if (!strcmp(op, "qprevpub")) { ScriptCoinPreviewPublish(); Log("[script] qprevpub"); return; }
    if (!strcmp(op, "placeclick"))            // placeclick x y z -- construction mode: a click aimed at this point
    {
        Vec3 at{};
        if (sscanf_s(rest.c_str(), "%lf %lf %lf", &at.x, &at.y, &at.z) != 3) { Log("[script] FAIL placeclick: want x y z"); return; }
        ScriptConstructClick(snap, at);
        Log("[script] placeclick toward (%.0f,%.0f,%.0f)", at.x, at.y, at.z);
        return;
    }
    if (!strcmp(op, "qcoinrun"))              // qcoinrun <seconds> <coins> <title...> -- publish a red coin run via the UI path
    {
        int secs = 60, n = 4, used = 0;
        sscanf_s(rest.c_str(), "%d %d %n", &secs, &n, &used);
        ScriptCoinRun(snap, rest.substr(used), secs, n);
        Log("[script] qcoinrun '%s' %ds %d coins", rest.substr(used).c_str(), secs, n);
        return;
    }
    if (!strcmp(op, "qpub"))                  // qpub <id> <radiusCm> <timeLimitS> <title...> -- publish the qstep list
    {
        char id[64] = {};
        double radius = 250; int tl = 0; int used = 0;
        sscanf_s(rest.c_str(), "%63s %lf %d %n", id, (unsigned)sizeof(id), &radius, &tl, &used);
        Command q{ CmdType::QuestCompile };
        q.str = id; q.str2 = rest.substr(used); q.str3 = "PKRClimb5"; q.num = 0; q.num2 = 0; q.f1 = 0;
        q.str4 = q.str2; q.radius = radius; q.timeLimit = tl;
        State().Push(q);
        return;
    }
    if (!strcmp(op, "audit"))                 // audit <first> <count> -- server spawns each catalogue class
    {                                           // privately and logs its components (probing)
        int first = 0, count = 40;
        sscanf_s(rest.c_str(), "%d %d", &first, &count);
        const auto& cat = Catalogue();
        for (int i = first; i < first + count && i < (int)cat.size(); ++i) SendToServer("SE|AUDIT|" + cat[i].path);
        Log("[script] audit sent %d..%d of %d", first, (std::min)(first + count, (int)cat.size()) - 1, (int)cat.size());
        return;
    }
    if (!strcmp(op, "fnaddr"))                // fnaddr <Class> <Function> -- native thunk as an image offset
    {
        char cls[128] = {}, fn[128] = {};
        sscanf_s(rest.c_str(), "%127s %127s", cls, (unsigned)sizeof(cls), fn, (unsigned)sizeof(fn));
        auto* c = SDK::UObject::FindClassFast(cls);
        auto* f = c ? c->GetFunction(cls, fn) : nullptr;
        const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const uintptr_t exec = f ? reinterpret_cast<uintptr_t>(*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(f) + 0xD8)) : 0;
        Log("[script] FNADDR %s::%s exec=%p rva=0x%llX flags=%08x", cls, fn, (void*)exec,
            (unsigned long long)(exec ? exec - base : 0), f ? (unsigned)f->FunctionFlags : 0);
        return;
    }
    if (!strcmp(op, "slotscan"))              // slotscan <Type> -- TEST: which listed objects have a <Type> component
    {
        Command c{ CmdType::SlotScan }; c.str = rest; State().Push(c);
        return;
    }
    if (!strcmp(op, "slotlist"))              // slotlist -- TEST: log the last slotscan result
    {
        Log("[script] slotlist %s: %d object(s)", g_slotCandType.c_str(), (int)g_slotCands.size());
        for (size_t i = 0; i < g_slotCands.size() && i < 25; ++i)
        {
            std::string cls;
            for (const SceneObject& so : g_lastObjects) if (so.handle == g_slotCands[i].handle) cls = so.className;
            Log("[script]   %s (%s) comp=%s", cls.c_str(), g_slotCands[i].handle.c_str(), g_slotCands[i].comp.c_str());
        }
        return;
    }
    if (!strcmp(op, "qprog"))                 // qprog <guid hex> -- TEST: this client's replicated QuestProgression entry
    {
        uint32_t g[4] = {};
        for (int i = 0; i < 4 && rest.size() >= 32; ++i) g[i] = static_cast<uint32_t>(strtoul(rest.substr(i * 8, 8).c_str(), nullptr, 16));
        auto* qcCls = SDK::UObject::FindClassFast("A2PlayerQuestComponent");
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; qcCls && i < n; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (!c || c->IsDefaultObject() || !c->IsA(qcCls) || !c->Outer || c->Outer->IsDefaultObject()) continue;
            const uintptr_t p = reinterpret_cast<uintptr_t>(c);
            const uint8_t* data = *reinterpret_cast<uint8_t* const*>(p + 0x128);
            const int cnt = *reinterpret_cast<const int32_t*>(p + 0x130);
            int prog = -1, cver = -1;
            for (int k = 0; data && k < cnt && k < 4096; ++k)
                if (memcmp(data + k * 0x20, g, 16) == 0) { prog = data[k * 0x20 + 0x10]; cver = *reinterpret_cast<const int32_t*>(data + k * 0x20 + 0x14); }
            Log("[script] qprog %s on %s: %d stored, this quest progress=%d completedVersion=%d init=%d local=%d",
                rest.c_str(), c->Outer->GetName().c_str(), cnt, prog, cver, *reinterpret_cast<const uint8_t*>(p + 0x140), *reinterpret_cast<const uint8_t*>(p + 0xF8));
        }
        return;
    }
    if (!strcmp(op, "clickat"))               // clickat x y z -- TEST: what a viewport click from the camera toward this point selects
    {
        Vec3 at{};
        if (sscanf_s(rest.c_str(), "%lf %lf %lf", &at.x, &at.y, &at.z) != 3) { Log("[script] FAIL clickat: want x y z"); return; }
        Vec3 d{ at.x - snap.cameraPos.x, at.y - snap.cameraPos.y, at.z - snap.cameraPos.z };
        const double len = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (len < 1.0) { Log("[script] FAIL clickat: point is at the camera"); return; }
        d = { d.x / len, d.y / len, d.z / len };
        const std::string h = PickRay(snap.cameraPos, d);
        std::string cls;
        for (const SceneObject& so : g_lastObjects) if (so.handle == h) cls = so.className;
        Log("[script] clickat (%.0f,%.0f,%.0f): %s", at.x, at.y, at.z, h.empty() ? "nothing" : (cls + " " + h).c_str());
        return;
    }
    if (!strcmp(op, "qdefs"))                 // qdefs <text> -- TEST: how many quest definitions here have <text> in the title
    {
        const std::wstring w(rest.begin(), rest.end());
        int total = 0;
        const int hits = CountQuestDefs(w.c_str(), &total);
        Log("[script] qdefs '%s': %d match(es) of %d definition(s)", rest.c_str(), hits, total);
        return;
    }
    if (!strcmp(op, "pick") || !strcmp(op, "pickat"))   // pick <Class> -- the instance nearest the camera
    {                                                     // pickat <Class> x y z -- the instance nearest a point
        std::string cls = rest;
        Vec3 at = snap.cameraPos;
        if (!strcmp(op, "pickat"))
        {
            char name[128] = {};
            if (sscanf_s(rest.c_str(), "%127s %lf %lf %lf", name, (unsigned)sizeof(name), &at.x, &at.y, &at.z) != 4)
            { Log("[script] FAIL pickat: want <Class> x y z"); return; }
            cls = name;
        }
        const SceneObject* best = nullptr;
        double bestD = 1e30;
        for (const SceneObject& s : g_lastObjects)
        {
            if (s.className.find(cls) == std::string::npos) continue;
            const double dx = s.location.x - at.x, dy = s.location.y - at.y, dz = s.location.z - at.z;
            const double d = dx * dx + dy * dy + dz * dz;
            if (d < bestD) { bestD = d; best = &s; }
        }
        if (!best) { Log("[script] FAIL pick: no %s in the level", rest.c_str()); return; }
        g_lastSpawnClass = best->className;
        g_lastSpawnLoc = best->location;
        Log("[script] picked %s at (%.0f,%.0f,%.0f)", best->className.c_str(), best->location.x, best->location.y, best->location.z);
        return;
    }
    const SceneObject* o = FindLastSpawned();
    if (!strcmp(op, "expect"))
    {
        // Count the class NEAR the spot we spawned at, not across the level: the station already holds
        // dozens of some LE classes (30 red coins), so a level-wide count proves nothing.
        int n = 0;
        for (const SceneObject& s : g_lastObjects)
        {
            if (s.className != g_lastSpawnClass) continue;
            const double dx = s.location.x - g_lastSpawnLoc.x, dy = s.location.y - g_lastSpawnLoc.y,
                         dz = s.location.z - g_lastSpawnLoc.z;
            if (dx * dx + dy * dy + dz * dz < 600.0 * 600.0) ++n;
        }
        const int want = atoi(rest.c_str());
        Log("[script] %s expect %d %s within 6m of (%.0f,%.0f,%.0f), saw %d", n == want ? "PASS" : "FAIL", want,
            g_lastSpawnClass.c_str(), g_lastSpawnLoc.x, g_lastSpawnLoc.y, g_lastSpawnLoc.z, n);
        return;
    }
    if (!o) { Log("[script] FAIL %s: the spawned %s never replicated to this client", op, g_lastSpawnClass.c_str()); return; }

    if (!strcmp(op, "ownlock"))               // ownlock 1|0 -- owner-lock the last object (Details > Lock)
    {
        Command c{ CmdType::OwnLock }; c.str = o->handle; c.str2 = rest == "0" ? "0" : "1"; State().Push(c);
        Log("[script] ownlock %s %s", c.str2.c_str(), o->handle.c_str());
        return;
    }
    if (!strcmp(op, "markpos"))               // markpos -- remember where the last object is now (for expectdelta)
    {
        g_markPos = o->location;
        Log("[script] markpos (%.1f,%.1f,%.1f)", o->location.x, o->location.y, o->location.z);
        return;
    }
    if (!strcmp(op, "expectdelta"))           // expectdelta <x> <y> <z> -- per axis 1 = must have moved (>10), 0 = must not (<3)
    {
        int f[3] = {};
        sscanf_s(rest.c_str(), "%d %d %d", &f[0], &f[1], &f[2]);
        const double d[3] = { o->location.x - g_markPos.x, o->location.y - g_markPos.y, o->location.z - g_markPos.z };
        bool ok = true;
        for (int k = 0; k < 3; ++k) if (f[k] != 2) ok = ok && (f[k] ? std::fabs(d[k]) > 10.0 : std::fabs(d[k]) < 3.0);   // 2 = either
        Log("[script] %s expectdelta: moved (%.1f,%.1f,%.1f), wanted change x=%d y=%d z=%d", ok ? "PASS" : "FAIL", d[0], d[1], d[2], f[0], f[1], f[2]);
        return;
    }
    if (!strcmp(op, "expectlock"))            // expectlock <mine 0|1> <other 0|1> -- the last object's owner-lock state
    {
        int m = 0, ot = 0;
        sscanf_s(rest.c_str(), "%d %d", &m, &ot);
        const bool ok = o->lockedByMe == (m != 0) && o->lockedByOther == (ot != 0);
        Log("[script] %s expectlock: mine=%d other=%d owner='%s' (wanted mine=%d other=%d)", ok ? "PASS" : "FAIL", o->lockedByMe,
            o->lockedByOther, o->lockOwner.c_str(), m, ot);
        return;
    }
    if (!strcmp(op, "lockinfo"))              // lockinfo -- what this client thinks of the last object's lock
    {
        Log("[script] LOCK %s mine=%d other=%d owner='%s'", o->handle.c_str(), o->lockedByMe, o->lockedByOther, o->lockOwner.c_str());
        return;
    }
    if (!strcmp(op, "move"))
    {
        double dx = 0, dy = 0, dz = 0;
        sscanf_s(rest.c_str(), "%lf %lf %lf", &dx, &dy, &dz);
        Command c{ CmdType::SetTransform };
        c.str = o->handle;
        c.loc = { o->location.x + dx, o->location.y + dy, o->location.z + dz };
        c.rot = o->rotation; c.scale = o->scale;
        g_lastSpawnLoc = c.loc;                  // follow it
        State().Push(c);
        return;
    }
    if (!strcmp(op, "scale"))                 // scale <sx> <sy> <sz>
    {
        Command c{ CmdType::SetTransform };
        c.str = o->handle;
        c.loc = o->location; c.rot = o->rotation;
        sscanf_s(rest.c_str(), "%lf %lf %lf", &c.scale.x, &c.scale.y, &c.scale.z);
        State().Push(c);
        return;
    }
    if (!strcmp(op, "rotate"))
    {
        Command c{ CmdType::SetTransform };
        c.str = o->handle;
        c.loc = o->location; c.scale = o->scale;
        c.rot = o->rotation; c.rot.yaw += atof(rest.c_str());
        State().Push(c);
        return;
    }
    if (!strcmp(op, "delete"))
    {
        Command c{ CmdType::DeleteObject }; c.str = o->handle; State().Push(c);
        return;
    }
    if (!strcmp(op, "props"))                 // props [sub.path] -- log what Details > Properties would show
    {
        const std::string saveH = g_inspHandle, saveP = g_inspPath;
        g_inspHandle = o->handle; g_inspPath = rest;
        Snapshot tmp;
        FillProps(tmp);
        g_inspHandle = saveH; g_inspPath = saveP;
        Log("[script] props of %s%s%s (%s): %d", o->className.c_str(), rest.empty() ? "" : " > ", rest.c_str(),
            tmp.inspectClass.c_str(), (int)tmp.props.size());
        for (const auto& pi : tmp.props)
            Log("[script]   %-28s %-10s = %s%s   [%s]", pi.name.c_str(),
                pi.type == PT_Object ? (pi.link ? "Object >" : "Object") : "", pi.value.c_str(),
                pi.writable ? "" : " (read-only)", pi.owner.c_str());
        return;
    }
    if (!strcmp(op, "callfn"))                // callfn <Function> -- run it on THIS client's copy, zeroed args
    {
        auto* target = static_cast<SDK::UObject*>(o->ptr);
        SDK::UFunction* fn = sereflect::FindFn(target, rest.c_str());
        if (!fn) { Log("[script] FAIL callfn: no %s", rest.c_str()); return; }
        static uint8_t zero[4096];
        memset(zero, 0, sizeof(zero));
        Log("[script] callfn %s ...", rest.c_str());
        sereflect::CallFn(target, fn, zero);
        Log("[script] callfn %s returned (client alive)", rest.c_str());
        return;
    }
    if (!strcmp(op, "compids"))               // compids -- the prefab's component tables: ID/name -> component
    {
        SDK::UObject* pc = sereflect::Hop(static_cast<SDK::UObject*>(o->ptr), "Prefab");
        if (!pc) pc = sereflect::Hop(static_cast<SDK::UObject*>(o->ptr), "ObjectPrefab");
        if (!pc) { Log("[script] compids: no prefab component"); return; }
        for (int t = 0; t < 2; ++t)
        {
            const uintptr_t map = reinterpret_cast<uintptr_t>(pc) + (t == 0 ? 0x2F0 : 0x340);
            const uint8_t* data = *reinterpret_cast<uint8_t* const*>(map);
            const int num = *reinterpret_cast<const int32_t*>(map + 8);
            for (int i = 0; data && i < num && i < 32; ++i)
            {
                const uint8_t* e = data + i * 0x20;
                const wchar_t* k = *reinterpret_cast<wchar_t* const*>(e);
                auto* v = *reinterpret_cast<SDK::UObject* const*>(e + 0x10);
                if (!k) continue;
                Log("[script] COMPID %s '%ls' -> %s (%s)", t == 0 ? "id  " : "name", k, v ? v->GetName().c_str() : "-",
                    v && v->Class ? v->Class->GetName().c_str() : "-");
            }
        }
        return;
    }
    if (!strcmp(op, "sbtree"))                // sbtree -- LOCAL TEST: server dumps this object's NetVar subtree
    {
        SendToServer("SE|SBTREE|" + IdentFor(o->handle));
        return;
    }
    if (!strcmp(op, "sbprobe"))               // sbprobe -- LOCAL TEST: server probes this object's NetVar subtree
    {
        SendToServer("SE|SBPROBE|" + IdentFor(o->handle));
        return;
    }
    if (!strcmp(op, "sbdesc"))                // sbdesc -- LOCAL TEST: server prints this object's sandbox Desc
    {
        SendToServer("SE|SBDESC|" + IdentFor(o->handle));
        return;
    }
    if (!strcmp(op, "qstep"))                 // qstep -- the last spawned object becomes the next checkpoint
    {
        Command s2{ CmdType::QuestAddStep }; s2.str = o->handle; s2.str2 = "0"; State().Push(s2);
        Log("[script] qstep %s", o->handle.c_str());
        return;
    }
    if (!strcmp(op, "tracedown"))             // tracedown -- what does THIS client's world block from above it?
    {
        SDK::Params::KismetSystemLibrary_LineTraceSingle p{};
        p.WorldContextObject = g_pc;
        p.Start = SDK::FVector{ o->location.x, o->location.y, o->location.z + 600.0 };
        p.End   = SDK::FVector{ o->location.x, o->location.y, o->location.z - 600.0 };
        p.TraceChannel = SDK::ETraceTypeQuery::TraceTypeQuery1;
        p.bTraceComplex = false;
        p.DrawDebugType = SDK::EDrawDebugTrace::None;
        p.bIgnoreSelf = true;
        std::string what = "nothing";
        if (g_pc && CallStatic("KismetSystemLibrary", "LineTraceSingle", p) && p.ReturnValue && p.OutHit.bBlockingHit)
        {
            SDK::UObject* c = WeakGet(reinterpret_cast<const uint8_t*>(&p.OutHit) + 0xD8);
            SDK::UObject* owner = c ? c->Outer : nullptr;
            char b[256];
            snprintf(b, sizeof(b), "%s (%s) at z=%.0f", owner ? owner->GetName().c_str() : "?", owner && owner->Class ? owner->Class->GetName().c_str() : "?",
                     p.OutHit.ImpactPoint.Z);
            what = b;
        }
        Log("[script] TRACEDOWN over %s: hit %s", o->className.c_str(), what.c_str());
        return;
    }
    if (!strcmp(op, "data"))                  // data -- ask the server for this object's Game data (logged on arrival)
    {
        Command c{ CmdType::DataRequest }; c.str = o->handle; State().Push(c);
        return;
    }
    if (!strcmp(op, "dataset"))               // dataset <path> <kind> <value...>
    {
        char path[128] = {}, kind[32] = {}; int used = 0;
        sscanf_s(rest.c_str(), "%127s %31s %n", path, (unsigned)sizeof(path), kind, (unsigned)sizeof(kind), &used);
        Command c{ CmdType::DataSet }; c.str = o->handle; c.str2 = path; c.str3 = kind; c.str4 = rest.substr(used); State().Push(c);
        return;
    }
    if (!strcmp(op, "datadump"))              // datadump -- log the Game data we have
    {
        for (const auto& e : g_data) Log("[script]   data %s (%s) = %s", e.path.c_str(), e.kind.c_str(), e.value.c_str());
        return;
    }
    if (!strcmp(op, "luauref"))               // luauref <script> <slot> <Type> self|<Class> [x y z] -- wire a script slot
    {                                         // of the last object (as dragging from the Outliner does)
        char sc[64] = {}, sl[64] = {}, ty[64] = {}, tc[128] = {};
        Vec3 at = o->location;
        const int got = sscanf_s(rest.c_str(), "%63s %63s %63s %127s %lf %lf %lf", sc, (unsigned)sizeof(sc), sl, (unsigned)sizeof(sl),
                                 ty, (unsigned)sizeof(ty), tc, (unsigned)sizeof(tc), &at.x, &at.y, &at.z);
        if (got < 4) { Log("[script] FAIL luauref: want <script> <slot> <Type> self|<Class> [x y z]"); return; }
        std::string target;
        if (!strcmp(tc, "self")) target = o->handle;
        else if (strcmp(tc, "-") != 0)
        {
            double bestD = 1e30;
            for (const SceneObject& t : g_lastObjects)
            {
                if (t.className.find(tc) == std::string::npos || t.handle == o->handle) continue;
                const double dx = t.location.x - at.x, dy = t.location.y - at.y, dz = t.location.z - at.z;
                const double d = dx * dx + dy * dy + dz * dz;
                if (d < bestD) { bestD = d; target = t.handle; }
            }
            if (target.empty()) { Log("[script] FAIL luauref: no %s", tc); return; }
        }
        std::string script = sc;
        if (script.find(".luau") == std::string::npos) script += ".luau";
        Command c{ CmdType::LuauRef }; c.str = o->handle; c.str2 = script; c.str3 = sl; c.str4 = ty; c.str5 = target;
        State().Push(c);
        Log("[script] luauref %s.%s -> %s", script.c_str(), sl, target.empty() ? "(cleared)" : target.c_str());
        return;
    }
    if (!strcmp(op, "luaudel"))               // luaudel <name> -- take <name>.luau off the last object
    {
        std::string nm = rest;
        if (nm.find(".luau") == std::string::npos) nm += ".luau";
        Command c{ CmdType::LuauRemove }; c.str = o->handle; c.str2 = nm; State().Push(c);
        return;
    }
    if (!strcmp(op, "selectadd"))             // selectadd -- Ctrl+click the last object (toggle it in the selection)
    {
        RequestUiSelect("+" + o->handle);
        Log("[script] selectadd %s", o->handle.c_str());
        return;
    }
    if (!strcmp(op, "select"))                // select -- the UI selects the last object, as a click would
    {
        RequestUiSelect(o->handle);
        Log("[script] select %s", o->handle.c_str());
        return;
    }
    if (!strcmp(op, "luaufile"))              // luaufile <name> -- attach RigelScripts/<name>.luau to the last object
    {
        Log("[script] luaufile %s: %s", rest.c_str(), ScriptAttachFile(o->handle, rest) ? "attached" : "FAIL no such file");
        return;
    }
    if (!strcmp(op, "luau"))                  // luau <name> <source...> -- attach a script to the last object
    {
        char nm[48] = {}; int used = 0;
        sscanf_s(rest.c_str(), "%47s %n", nm, (unsigned)sizeof(nm), &used);
        Command c{ CmdType::LuauAttach }; c.str = o->handle; c.str2 = nm; c.str3 = rest.substr(used); State().Push(c);
        return;
    }
    if (!strcmp(op, "press"))                 // press -- LOCAL TEST: press a ProgressionButton on THIS client (OnPressed)
    {
        auto* root = static_cast<SDK::UObject*>(o->ptr);
        auto* bcls = SDK::UObject::FindClassFast("ProgressionButtonComponent");
        const int32_t n = SDK::UObject::GObjects->Num();
        SDK::UObject* comp = nullptr;
        for (int32_t i = 0; bcls && i < n && !comp; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (c && c->Outer == root && c->IsA(bcls)) comp = c;
        }
        if (!comp) { Log("[script] FAIL press: %s has no ProgressionButtonComponent", root->GetName().c_str()); return; }
        SDK::UFunction* fn = sereflect::FindFn(comp, "OnPressed");
        if (!fn) { Log("[script] FAIL press: no OnPressed"); return; }
        static uint8_t zeroArgs[256] = {};
        sereflect::CallFn(comp, fn, zeroArgs);
        Log("[script] pressed %s", comp->GetName().c_str());
        return;
    }
    if (!strcmp(op, "toggle"))                // toggle on|off -- LOCAL TEST: flip a switch the way a player's press does
    {                                         // (UToggleableComponent enable/disable writer, on THIS client)
        auto* root = static_cast<SDK::UObject*>(o->ptr);
        auto* tcls = SDK::UObject::FindClassFast("ToggleableComponent");
        const int32_t n = SDK::UObject::GObjects->Num();
        SDK::UObject* comp = nullptr;
        for (int32_t i = 0; tcls && i < n && !comp; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (c && c->Outer == root && c->IsA(tcls)) comp = c;
        }
        if (!comp) { Log("[script] FAIL toggle: %s has no ToggleableComponent", root->GetName().c_str()); return; }
        const bool on = rest != "off";
        const uintptr_t fn = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)) + (on ? 0x53A8DC0 : 0x53A8C90);
        const uintptr_t gd = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(comp) + 0x288);
        Log("[script] toggle %s on %s (gameData handle %s)", on ? "ON" : "OFF", comp->GetName().c_str(), gd ? "bound" : "UNBOUND");
        reinterpret_cast<void(__fastcall*)(void*)>(fn)(comp);
        const uint8_t now = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(comp) + 0x479);
        Log("[script] toggle done: IsEnabled=%d", now);
        return;
    }
    if (!strcmp(op, "comps"))                 // comps -- every component this actor owns, with collision (probing)
    {
        auto* root = static_cast<SDK::UObject*>(o->ptr);
        auto* primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
        auto* compCls = SDK::UObject::FindClassFast("ActorComponent");
        auto* smcCls  = SDK::UObject::FindClassFast("StaticMeshComponent");
        const int32_t n = SDK::UObject::GObjects->Num();
        Log("[script] comps of %s", root->GetName().c_str());
        for (int32_t i = 0; i < n; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (!c || c->Outer != root || !compCls || !c->IsA(compCls)) continue;
            std::string extra;
            if (primCls && c->IsA(primCls))
            {
                SDK::Params::PrimitiveComponent_GetCollisionEnabled ce{};
                SDK::Params::PrimitiveComponent_GetCollisionProfileName pn{};
                SDK::Params::PrimitiveComponent_GetCollisionObjectType ot{};
                CallNative(c, "PrimitiveComponent", "GetCollisionEnabled", ce);
                CallNative(c, "PrimitiveComponent", "GetCollisionProfileName", pn);
                CallNative(c, "PrimitiveComponent", "GetCollisionObjectType", ot);
                char b[200];
                SDK::Params::SceneComponent_IsVisible vis{};
                CallNative(c, "SceneComponent", "IsVisible", vis);
                snprintf(b, sizeof(b), " coll=%d profile=%s objtype=%d visible=%d", (int)ce.ReturnValue, pn.ReturnValue.ToString().c_str(),
                         (int)ot.ReturnValue, (int)vis.ReturnValue);
                extra = b;
                if (smcCls && c->IsA(smcCls))
                {
                    auto* mesh = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(c) + 0x560);
                    extra += " mesh=" + (mesh ? mesh->GetName() : std::string("None"));
                }
            }
            Log("[script]   %-40s %-32s%s", c->GetName().c_str(), c->Class ? c->Class->GetName().c_str() : "?", extra.c_str());
        }
        return;
    }
    if (!strcmp(op, "funcs"))                 // funcs -- the blueprint's own functions (probing)
    {
        auto* root = static_cast<SDK::UObject*>(o->ptr);
        for (SDK::UStruct* st = root->Class; st; st = st->SuperStruct)
        {
            const std::string owner = st->GetName();
            if (sereflect::IsEngineBase(owner)) break;
            for (SDK::UField* f = st->Children; f; f = f->Next)
                Log("[script]   fn %-40s flags=%08x [%s]", f->Name.ToString().c_str(),
                    (unsigned)static_cast<SDK::UFunction*>(f)->FunctionFlags, owner.c_str());
        }
        return;
    }
    if (!strcmp(op, "dump"))                  // dump [sub.path] -- every field, any type or flag (probing)
    {
        SDK::UObject* obj = sereflect::Resolve(static_cast<SDK::UObject*>(o->ptr), rest);
        if (!obj) { Log("[script] dump: cannot reach '%s'", rest.c_str()); return; }
        Log("[script] dump of %s (%s) outer=%s", obj->GetName().c_str(), obj->Class ? obj->Class->GetName().c_str() : "?",
            obj->Outer ? obj->Outer->GetName().c_str() : "-");
        if (auto* actorCls = SDK::UObject::FindClassFast("Actor"); actorCls && obj->IsA(actorCls))
        {
            SDK::Params::Actor_GetOwner ow{};
            SDK::Params::Actor_GetAttachParentActor ap{};
            CallNative(obj, "Actor", "GetOwner", ow);
            CallNative(obj, "Actor", "GetAttachParentActor", ap);
            Log("[script]   owner=%s attachParent=%s (%s)", ow.ReturnValue ? ow.ReturnValue->GetName().c_str() : "-",
                ap.ReturnValue ? ap.ReturnValue->GetName().c_str() : "-",
                ap.ReturnValue && ap.ReturnValue->Class ? ap.ReturnValue->Class->GetName().c_str() : "-");
        }
        int n = 0;
        for (SDK::UStruct* st = obj->Class; st && n < 400; st = st->SuperStruct)
        {
            const std::string owner = st->GetName();
            if (sereflect::IsEngineBase(owner)) continue;
            for (SDK::FField* f = st->ChildProperties; f && n < 400; f = f->Next, ++n)
            {
                auto* fp = static_cast<SDK::FProperty*>(f);
                const sereflect::PType t = sereflect::TypeOf(fp);
                Log("[script]   %-30s %-18s flags=%016llx off=0x%X  [%s]%s%s", f->Name.ToString().c_str(),
                    sereflect::FieldClassName(f).c_str(), (unsigned long long)fp->PropertyFlags, fp->Offset, owner.c_str(),
                    t != sereflect::PType::Unsupported ? "  = " : "",
                    t != sereflect::PType::Unsupported ? sereflect::Read(obj, fp, t).c_str()
                    : (sereflect::FieldClassName(f) == "ArrayProperty" || sereflect::FieldClassName(f) == "MapProperty")
                        ? ("  num=" + std::to_string(*reinterpret_cast<int32_t*>(sereflect::Addr(obj, fp) + 8))).c_str() : "");
            }
        }
        return;
    }
    if (!strcmp(op, "sprop"))                 // sprop <path> <value> -- SERVER ONLY: this client must learn
    {                                           // the value from the server's broadcast, not from itself
        const size_t sp = rest.find(' ');
        const std::string id = IdentFor(o->handle);
        SendToServer("SE|PROP|" + id + "|" + rest.substr(0, sp) + "|" + (sp == std::string::npos ? std::string() : rest.substr(sp + 1)));
        return;
    }
    if (!strcmp(op, "expectprop"))            // expectprop <path> <value> -- this client's copy, now
    {
        const size_t sp = rest.find(' ');
        const std::string path = rest.substr(0, sp), want = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        const size_t dot = path.find_last_of('.');
        SDK::UObject* obj = sereflect::Resolve(static_cast<SDK::UObject*>(o->ptr), dot == std::string::npos ? std::string() : path.substr(0, dot));
        sereflect::PType t{};
        SDK::FProperty* p = obj ? sereflect::Find(obj, dot == std::string::npos ? path : path.substr(dot + 1), &t) : nullptr;
        const std::string got = p ? sereflect::Read(obj, p, t) : std::string("<missing>");
        const bool ok = got == want || (atof(got.c_str()) == atof(want.c_str()) && !want.empty() && (isdigit((unsigned char)want[0]) || want[0] == '-'));
        Log("[script] %s expectprop %s = '%s', got '%s'", ok ? "PASS" : "FAIL", path.c_str(), want.c_str(), got.c_str());
        return;
    }
    if (!strcmp(op, "drag"))                  // drag <dx> <dy> <dz> <seconds> -- through the gizmo's LiveDrag path
    {
        double dx = 0, dy = 0, dz = 0, secs = 1.0;
        sscanf_s(rest.c_str(), "%lf %lf %lf %lf", &dx, &dy, &dz, &secs);
        g_sim.handle = o->handle;
        g_sim.from = o->location; g_sim.rot = o->rotation; g_sim.scale = o->scale;
        g_sim.to = { o->location.x + dx, o->location.y + dy, o->location.z + dz };
        g_sim.start = GetTickCount64();
        g_sim.ms = static_cast<ULONGLONG>(secs * 1000.0);
        g_sim.frames = 0;
        g_sim.on = true;
        g_lastSpawnLoc = g_sim.to;               // follow it
        return;
    }
    if (!strcmp(op, "expectat"))              // expectat <tolerance> -- the object is where the last drag/move left it
    {
        const double tol = rest.empty() ? 5.0 : atof(rest.c_str());
        const double ex = o->location.x - g_lastSpawnLoc.x, ey = o->location.y - g_lastSpawnLoc.y, ez = o->location.z - g_lastSpawnLoc.z;
        const double d = std::sqrt(ex * ex + ey * ey + ez * ez);
        Log("[script] %s expectat: at (%.1f,%.1f,%.1f), wanted (%.1f,%.1f,%.1f), off by %.1f", d <= tol ? "PASS" : "FAIL",
            o->location.x, o->location.y, o->location.z, g_lastSpawnLoc.x, g_lastSpawnLoc.y, g_lastSpawnLoc.z, d);
        return;
    }
    if (!strcmp(op, "where"))                 // where -- log the object's transform (for mouse-driven tests)
    {
        Log("[script] WHERE %s at (%.1f,%.1f,%.1f) rot(p=%.1f,y=%.1f,r=%.1f) scale(%.2f,%.2f,%.2f)", o->className.c_str(),
            o->location.x, o->location.y, o->location.z, o->rotation.pitch, o->rotation.yaw, o->rotation.roll,
            o->scale.x, o->scale.y, o->scale.z);
        g_lastSpawnLoc = o->location;            // keep following it wherever the mouse put it
        return;
    }
    if (!strcmp(op, "screenpos"))             // screenpos -- where the object and its gizmo handles are on screen
    {
        LogScreenPos(snap, *o);
        return;
    }
    if (!strcmp(op, "prop"))                  // prop <path> <value>  -- edit through the real command path
    {
        const size_t sp = rest.find(' ');
        Command c{ CmdType::SetProperty };
        c.str = o->handle;
        c.str2 = rest.substr(0, sp);
        c.str3 = sp == std::string::npos ? std::string() : rest.substr(sp + 1);
        State().Push(c);
        return;
    }
    if (!strcmp(op, "quest"))
    {
        const size_t sp1 = rest.find(' ');
        Command s{ CmdType::QuestAddStep }; s.str = o->handle; s.str2 = "0"; State().Push(s);
        Command q{ CmdType::QuestCompile };
        q.str  = rest.substr(0, sp1);
        q.str2 = sp1 == std::string::npos ? q.str : rest.substr(sp1 + 1);
        q.str3 = "PKRClimb5";                    // a glyph the climbing subquests use
        State().Push(q);
        return;
    }
    Log("[script] unknown op '%s'", op);
}

// -SpecEditConnect=<ip:port>: join a server on start-up. The spectator build has no auto-connect of its
// own (-connectToServerByIPAndPort comes from a UE4SS mod it does not carry), so without this every test
// run needed someone to type `open` into the console. Same mechanism as the payload's -HalcyonConnect:
// KismetSystemLibrary::ExecuteConsoleCommand, dispatched through ProcessEvent.
void ConnectOnce(const Snapshot& snap)
{
    static bool      s_off = false;
    static int       s_tries = 0;
    static ULONGLONG s_firstReady = 0, s_lastTry = 0;
    if (s_off || !snap.worldReady) return;
    const wchar_t* p = wcsstr(GetCommandLineW(), L"-SpecEditConnect=");
    if (!p) { s_off = true; return; }

    // Arrived: a VR player controller exists in a world other than the one we last issued `open` from.
    if (s_tries && g_pc && CurrentWorld() != g_connectFromWorld) { s_off = true; Log("[connect] arrived after %d attempt(s)", s_tries); return; }

    // The first attempt used to wait for a VRPlayerController -- which the front-end never has (it runs
    // the legal screens and the startup sequence on a different controller), so it never fired. Use the
    // world as the context instead, like the payload's -HalcyonConnect, let the startup sequence finish,
    // and retry: an `open` issued mid-sequence can simply be dropped.
    const ULONGLONG now = GetTickCount64();
    if (!s_firstReady) { s_firstReady = now; Log("[connect] world up; waiting for the startup sequence"); }
    if (now - s_firstReady < 15000) return;
    if (s_tries && now - s_lastTry < 30000) return;           // give each attempt time to travel
    if (s_tries >= 4) { s_off = true; Log("[connect] gave up after %d attempts", s_tries); return; }

    std::wstring addr;
    for (p += wcslen(L"-SpecEditConnect="); *p && *p != L' ' && *p != L'"'; ++p) addr += *p;
    auto* cls = SDK::UObject::FindClassFast("KismetSystemLibrary");
    auto* fn  = cls ? cls->GetFunction("KismetSystemLibrary", "ExecuteConsoleCommand") : nullptr;
    if (!fn) { Log("[connect] ExecuteConsoleCommand not found"); s_off = true; return; }

    const std::wstring cmd = L"open " + addr;
    struct { const SDK::UObject* ctx; SDK::FString command; SDK::UObject* player; } parms{};
    parms.ctx = static_cast<SDK::UObject*>(CurrentWorld());
    parms.command = SDK::FString(cmd.c_str());
    parms.player = nullptr;
    const auto saved = fn->FunctionFlags;
    fn->FunctionFlags |= 0x400;                               // FUNC_Native, as the generated SDK does
    SafePE(SDK::UKismetSystemLibrary::GetDefaultObj(), fn, &parms);
    fn->FunctionFlags = saved;
    g_connectFromWorld = CurrentWorld();
    ++s_tries; s_lastTry = now;
    Log("[connect] attempt %d: %ls", s_tries, cmd.c_str());
}

// Log the LE objects this client can see whenever that set changes -- the evidence that a server-side
// spawn, move or delete actually replicated here.
void LogReplicationChanges(const Snapshot& snap)
{
    static std::string s_last;
    std::string sig;
    char b[160];
    for (const SceneObject& o : snap.objects)
    {
        snprintf(b, sizeof(b), "%s@%.0f,%.0f,%.0f/y%.0f;", o.className.c_str(), o.location.x, o.location.y, o.location.z, o.rotation.yaw);
        sig += b;
    }
    if (sig == s_last) return;
    s_last = sig;
    Log("[repl] %d LE object(s) visible:", (int)snap.objects.size());
    int shown = 0;
    for (const SceneObject& o : snap.objects)
    {
        if (++shown > 24) { Log("[repl]   ..."); break; }
        Log("[repl]   %s  (%.0f, %.0f, %.0f)  rot(%.0f, %.0f, %.0f)  %s", o.className.c_str(),
            o.location.x, o.location.y, o.location.z, o.rotation.pitch, o.rotation.yaw, o.rotation.roll, o.handle.c_str());
    }
}

// ---- red coin stand-ins (this editor only, never replicated) ----------------------------------------------
// A placed red coin (LE_BP_RedCoin_C) draws nothing until a run is live, so authors had to guess where their
// coins were. Every coin in the level gets a local mesh actor here -- spawned on this machine only, no
// collision (clicks and traces go through), following the coin as it's moved, hidden outside the editor.

// The mesh to show: the coin's own (its first StaticMeshComponent's StaticMesh @0x560), else any loaded
// coin mesh, else the engine sphere.
// The look of a live coin: BP_RedCoin (/Game/A2/Progression/TimedQuest/BP_RedCoin, what a running red coin
// quest spawns). Loaded here, one copy spawned far below the map just long enough to read its mesh,
// materials and scale, then destroyed; the stand-ins wear the same.
struct CoinLook { SDK::UObject* mesh = nullptr; std::vector<SDK::UObject*> mats; Vec3 scale{ 1, 1, 1 }; bool tried = false; };
CoinLook g_coinLook;

const CoinLook& CoinStandinLook()
{
    if (g_coinLook.mesh && ObjectAlive(g_coinLook.mesh)) return g_coinLook;
    if (g_coinLook.tried) return g_coinLook;
    g_coinLook.tried = true;
    SDK::Params::KismetSystemLibrary_MakeSoftClassPath mk{};
    mk.PathString = SDK::FString(L"/Game/A2/Progression/TimedQuest/BP_RedCoin.BP_RedCoin_C");
    SDK::Params::KismetSystemLibrary_Conv_SoftClassPathToSoftClassRef cv{};
    SDK::Params::KismetSystemLibrary_LoadClassAsset_Blocking ld{};
    SDK::UClass* cls = nullptr;
    if (CallStatic("KismetSystemLibrary", "MakeSoftClassPath", mk))
    {
        cv.SoftClassPath = mk.ReturnValue;
        if (CallStatic("KismetSystemLibrary", "Conv_SoftClassPathToSoftClassRef", cv))
        {
            ld.AssetClass = cv.ReturnValue;
            if (CallStatic("KismetSystemLibrary", "LoadClassAsset_Blocking", ld)) cls = ld.ReturnValue;
        }
    }
    if (!cls || !g_pc) { Log("[coins] BP_RedCoin could not be loaded"); return g_coinLook; }
    SDK::Params::GameplayStatics_BeginDeferredActorSpawnFromClass b{};
    b.WorldContextObject = g_pc;
    b.ActorClass = cls;
    b.SpawnTransform = MakeXf(0.0, 0.0, -1000000.0);          // far below everything: nothing touches it
    b.CollisionHandlingOverride = SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    b.TransformScaleMethod = SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
    if (!CallStatic("GameplayStatics", "BeginDeferredActorSpawnFromClass", b) || !b.ReturnValue) { Log("[coins] BP_RedCoin template spawn failed"); return g_coinLook; }
    SDK::Params::GameplayStatics_FinishSpawningActor f{};
    f.Actor = b.ReturnValue;
    f.SpawnTransform = b.SpawnTransform;
    f.TransformScaleMethod = SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
    CallStatic("GameplayStatics", "FinishSpawningActor", f);
    SDK::AActor* tmpl = b.ReturnValue;
    auto* smcCls = SDK::UObject::FindClassFast("StaticMeshComponent");
    SDK::Params::Actor_GetComponentByClass gc{};
    gc.ComponentClass = static_cast<SDK::UClass*>(smcCls);
    if (smcCls && CallNative(tmpl, "Actor", "GetComponentByClass", gc) && gc.ReturnValue)
    {
        SDK::UObject* smc = gc.ReturnValue;
        g_coinLook.mesh = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(smc) + 0x560);
        SDK::Params::PrimitiveComponent_GetNumMaterials nm{};
        CallNative(smc, "PrimitiveComponent", "GetNumMaterials", nm);
        for (int k = 0; k < nm.ReturnValue && k < 8; ++k)
        {
            SDK::Params::PrimitiveComponent_GetMaterial gm{};
            gm.ElementIndex = k;
            CallNative(smc, "PrimitiveComponent", "GetMaterial", gm);
            g_coinLook.mats.push_back(gm.ReturnValue);
        }
        SDK::Params::SceneComponent_K2_GetComponentScale sc{};
        if (CallNative(smc, "SceneComponent", "K2_GetComponentScale", sc)) g_coinLook.scale = { sc.ReturnValue.X, sc.ReturnValue.Y, sc.ReturnValue.Z };
    }
    struct { uint8_t pad[8]; } d{};
    CallNative(tmpl, "Actor", "K2_DestroyActor", d);
    Log("[coins] stand-in look from BP_RedCoin: mesh %s, %zu material(s), scale %.2f",
        g_coinLook.mesh ? g_coinLook.mesh->GetName().c_str() : "(none)", g_coinLook.mats.size(), g_coinLook.scale.x);
    return g_coinLook;
}

SDK::AActor* SpawnCoinStandin(SDK::UObject* coin, const Vec3& at, const Vec3& scale0)
{
    auto* cls = SDK::UObject::FindClassFast("StaticMeshActor");
    const CoinLook& look = CoinStandinLook();
    SDK::UObject* mesh = look.mesh;
    if (!cls || !mesh || !g_pc) return nullptr;
    const Vec3 scale{ scale0.x * look.scale.x, scale0.y * look.scale.y, scale0.z * look.scale.z };
    (void)coin;
    SDK::Params::GameplayStatics_BeginDeferredActorSpawnFromClass b{};
    b.WorldContextObject = g_pc;
    b.ActorClass = cls;
    b.SpawnTransform = MakeXf(at.x, at.y, at.z);
    b.CollisionHandlingOverride = SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
    b.TransformScaleMethod = SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
    if (!CallStatic("GameplayStatics", "BeginDeferredActorSpawnFromClass", b) || !b.ReturnValue) return nullptr;
    SDK::AActor* a = b.ReturnValue;
    SDK::Params::Actor_K2_GetRootComponent r{};
    CallNative(a, "Actor", "K2_GetRootComponent", r);
    if (SDK::UObject* root = r.ReturnValue)
    {
        SDK::Params::SceneComponent_SetMobility mob{};
        mob.NewMobility = SDK::EComponentMobility::Movable;
        CallNative(root, "SceneComponent", "SetMobility", mob);
        SDK::Params::StaticMeshComponent_SetStaticMesh sm{};
        sm.NewMesh = static_cast<SDK::UStaticMesh*>(mesh);
        CallNative(root, "StaticMeshComponent", "SetStaticMesh", sm);
        for (size_t k = 0; k < look.mats.size(); ++k)
        {
            if (!look.mats[k]) continue;
            SDK::Params::PrimitiveComponent_SetMaterial mt{};
            mt.ElementIndex = static_cast<int32_t>(k);
            mt.Material = static_cast<SDK::UMaterialInterface*>(look.mats[k]);
            CallNative(root, "PrimitiveComponent", "SetMaterial", mt);
        }
        // Traces only (this machine's clicks): nothing can bump into it, but clicking it selects the coin.
        SDK::Params::PrimitiveComponent_SetCollisionEnabled col{};
        col.NewType = SDK::ECollisionEnabled::QueryOnly;
        CallNative(root, "PrimitiveComponent", "SetCollisionEnabled", col);
    }
    SDK::Params::GameplayStatics_FinishSpawningActor f{};
    f.Actor = a;
    f.SpawnTransform = b.SpawnTransform;
    f.TransformScaleMethod = SDK::ESpawnActorScaleMethod::MultiplyWithRoot;
    CallStatic("GameplayStatics", "FinishSpawningActor", f);
    SDK::Params::Actor_SetActorScale3D sc{};
    sc.NewScale3D = SDK::FVector{ scale.x, scale.y, scale.z };
    CallNative(a, "Actor", "SetActorScale3D", sc);
    return a;
}

void CoinStandinTick(const Snapshot& snap)
{
    if (!snap.worldReady || !g_pc) return;
    const bool show = snap.inEditor;
    std::unordered_set<SDK::UObject*> live;
    for (const SceneObject& o : snap.objects)
    {
        if (o.className != "LE_BP_RedCoin_C" || !o.ptr) continue;
        // Only coins near the editor camera (the station has hundreds): shown as you approach, freed as you leave.
        const double cx = o.location.x - snap.cameraPos.x, cy = o.location.y - snap.cameraPos.y, cz = o.location.z - snap.cameraPos.z;
        if (cx * cx + cy * cy + cz * cz > 8000.0 * 8000.0) continue;
        SDK::UObject* coin = static_cast<SDK::UObject*>(o.ptr);
        live.insert(coin);
        CoinStandin& st = g_coinStandins[coin];
        if (!st.actor || !ObjectAlive(st.actor))
        {
            if (!show) continue;                               // only built once you're editing
            st.actor = SpawnCoinStandin(coin, o.location, o.scale);
            Log("[coins] stand-in for %s at (%.0f,%.0f,%.0f): %s", o.handle.c_str(), o.location.x, o.location.y, o.location.z,
                st.actor ? "shown" : "FAILED");
            st.at = o.location; st.scale = o.scale; st.hidden = false;
            if (!st.actor) continue;
        }
        if (!st.measured)                                          // where the mesh's centre sits relative to its pivot
        {
            SDK::Params::Actor_GetActorBounds b{};
            b.bOnlyCollidingComponents = false;
            b.bIncludeFromChildActors = false;
            if (CallNative(st.actor, "Actor", "GetActorBounds", b))
            {
                st.meshOff = { b.Origin.X - st.at.x, b.Origin.Y - st.at.y, b.Origin.Z - st.at.z };
                st.measured = true;
                Log("[coins] stand-in mesh centre offset z=%.0f; coin box offset z=%.0f half-height %.0f", st.meshOff.z, o.boundsOff.z, o.boundsExt.z);
                st.at = { 1e30, 1e30, 1e30 };                        // re-place it centred below
            }
        }
        // Centre the coin in its box (the coin's own bounds = its pickup trigger, what the selection box shows).
        const Vec3 want{ o.location.x + o.boundsOff.x - st.meshOff.x, o.location.y + o.boundsOff.y - st.meshOff.y,
                         o.location.z + o.boundsOff.z - st.meshOff.z };
        const double dx = st.at.x - want.x, dy = st.at.y - want.y, dz = st.at.z - want.z;
        if (dx * dx + dy * dy + dz * dz > 0.25)                  // follow the coin (gizmo drags, server moves)
        {
            SDK::Params::Actor_K2_SetActorLocationAndRotation p{};
            p.NewLocation = SDK::FVector{ want.x, want.y, want.z };
            p.NewRotation = SDK::FRotator{ o.rotation.pitch, o.rotation.yaw, o.rotation.roll };
            p.bSweep = false;
            p.bTeleport = true;
            CallNative(st.actor, "Actor", "K2_SetActorLocationAndRotation", p);
            st.at = want;
        }
        if (st.hidden == show)
        {
            SDK::Params::Actor_SetActorHiddenInGame h{};
            h.bNewHidden = !show;
            CallNative(st.actor, "Actor", "SetActorHiddenInGame", h);
            st.hidden = !show;
        }
    }
    for (auto it = g_coinStandins.begin(); it != g_coinStandins.end();)   // coin deleted: its stand-in goes too
    {
        if (live.count(it->first)) { ++it; continue; }
        if (it->second.actor && ObjectAlive(it->second.actor))
        {
            struct { uint8_t pad[8]; } d{};                        // K2_DestroyActor takes no parameters
            CallNative(it->second.actor, "Actor", "K2_DestroyActor", d);
        }
        it = g_coinStandins.erase(it);
    }
}

void PumpImpl()
{
    Snapshot snap;
    // Read GWorld directly rather than SDK::UWorld::GetWorld(): that one lives in Engine_functions.cpp,
    // 5 MB of generated code we would otherwise have to compile for a single pointer load.
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    snap.worldReady = *reinterpret_cast<void**>(base + SDK::Offsets::GWorld) != nullptr;

    if (!g_clientMsgFn && snap.worldReady)
        if (auto* pcCls = SDK::UObject::FindClassFast("PlayerController"))
            g_clientMsgFn = pcCls->GetFunction("PlayerController", "ClientMessage");
    if (!g_setQuestsFn && snap.worldReady)
        if (auto* qc = SDK::UObject::FindClassFast("A2PlayerQuestComponent"))
        {
            g_setQuestsFn = qc->GetFunction("A2PlayerQuestComponent", "Client_SetQuests");
            Log("[quests] watching Client_SetQuests: class=%p fn=%p", (void*)qc, (void*)g_setQuestsFn);
        }
    g_pc     = snap.worldReady ? FindLocalController() : nullptr;
    g_lePawn = snap.worldReady ? FindLocalEditorPawn() : nullptr;
    snap.inEditor = g_editorMode;

    // The camera is the RENDER view: APlayerController::PlayerCameraManager (+0x350) ->
    // CameraCachePrivate (+0x13A0) -> POV (+0x10) = FMinimalViewInfo { Location, Rotation, FOV }.
    // This is what the gizmo projects with and what placement goes in front of. (The first version took
    // the editor pawn's root position and never set the rotation at all, so projection was off-axis and
    // there was no camera whatsoever outside the editor pawn.)
    if (g_pc)
    {
        void* pcm = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_pc) + 0x350);
        if (pcm)
        {
            const uintptr_t pov = reinterpret_cast<uintptr_t>(pcm) + 0x13A0 + 0x10;
            const double* l = reinterpret_cast<const double*>(pov + 0x00);
            const double* r = reinterpret_cast<const double*>(pov + 0x18);
            snap.cameraPos = { l[0], l[1], l[2] };
            snap.cameraRot = { r[0], r[1], r[2] };
            snap.cameraFov = *reinterpret_cast<const float*>(pov + 0x30);
        }
    }

    // Rebuild the palette whenever the world changes: a connecting client first loads the entry map and
    // then travels to the station, so a palette built on the first world is stale. While it is still
    // empty, retry every few seconds -- LE classes can finish loading after the world does.
    static void*     s_world = nullptr;
    static ULONGLONG s_lastPaletteTry = 0;
    void* world = *reinterpret_cast<void**>(base + SDK::Offsets::GWorld);
    static bool s_paletteHasSandbox = false;
    if (world != s_world) { s_world = world; g_paletteDirty = true; s_paletteHasSandbox = false; }
    if (snap.worldReady && g_palette.empty() && GetTickCount64() - s_lastPaletteTry > 5000) g_paletteDirty = true;
    // The sandbox's prefab registry fills a few seconds AFTER the world is ready (it loads the station's
    // project), so a palette built at arrival has none of the sandbox-only prefabs (boost pads, traps...).
    if (snap.worldReady && !s_paletteHasSandbox && GetTickCount64() - s_lastPaletteTry > 3000 && !SandboxPrefabs().empty())
    {
        g_paletteDirty = true;
        s_paletteHasSandbox = true;
    }
    if (g_paletteDirty && snap.worldReady)
    {
        Snapshot tmp;
        BuildPalette(tmp);
        g_palette = tmp.palette;
        g_paletteDirty = false;
        s_lastPaletteTry = GetTickCount64();
    }
    snap.palette = g_palette;
    BuildObjects(snap);
    snap.status = !snap.worldReady ? "waiting for world" : (!g_pc ? "no player controller yet" : (snap.inEditor ? "editing" : "ready"));

    // Keep a game-thread copy of what we just listed, so HandleCommands can turn a handle into the
    // class+position identity the server addresses actors by (see IdentFor).
    g_lastObjects = snap.objects;
    ConnectOnce(snap);
    LogReplicationChanges(snap);
    RunScript(snap);
    CoinStandinTick(snap);

    // The editor camera follows state: it is the view while editing with the UI up, and F12 or Stop
    // Editing hands the player their own view back. A travel destroys the camera actor, so a dead actor
    // just re-arms activation.
    if (Cam().active && !ObjectAlive(g_cam.actor)) Cam().active = false;
    const bool wantCam = snap.worldReady && g_pc && g_editorMode && g_uiVisible;
    if (wantCam && !Cam().active) CameraActivate(snap);
    else if (!wantCam && Cam().active) CameraDeactivate();

    FillProps(snap);
    static bool s_iconsRead = false;
    if (!s_iconsRead && snap.worldReady)
    {
        // Every icon the game has art for (UQuestAssets::TextureIcons, keys like RedCoin/TKB/Golf/Climb...),
        // not only the ones used by quests this client happened to receive.
        if (auto* qaCls = SDK::UObject::FindClassFast("QuestAssets"))
        {
            const int32_t n = SDK::UObject::GObjects->Num();
            for (int32_t i = 0; i < n; ++i)
            {
                SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
                if (!o || o->IsDefaultObject() || !o->IsA(qaCls)) continue;
                const auto& icons = *reinterpret_cast<const SDK::TMap<SDK::FName, SDK::UObject*>*>(reinterpret_cast<uintptr_t>(o) + 0x30);
                int added = 0;
                for (auto it = SDK::begin(icons); it != SDK::end(icons); ++it)
                {
                    const std::string k = it->Key().ToString();
                    if (!k.empty() && k != "None" && std::find(g_glyphs.begin(), g_glyphs.end(), k) == g_glyphs.end()) { g_glyphs.push_back(k); ++added; }
                }
                std::sort(g_glyphs.begin(), g_glyphs.end());
                Log("[quests] %d icon(s) from QuestAssets", added);
                s_iconsRead = true;
                break;
            }
        }
    }
    static void* s_qlistWorld = nullptr;                 // ask the server for its quest list once per world
    if (snap.inEditor && g_pc && s_qlistWorld != world) { s_qlistWorld = world; SendToServer("SE|QLIST"); }
    snap.glyphs = g_glyphs;
    snap.quests = g_knownQuests;
    snap.levels = g_levels;
    snap.gameScripts = g_gameScripts;
    snap.levelsSerial = g_levelsSerial;
    snap.dataHandle = g_dataHandle;
    snap.data = g_data;
    snap.dataSerial = g_dataSerial;
    snap.slotCandType = g_slotCandType;
    snap.clickPlaced = g_clickPlaced;
    snap.slotCands = g_slotCands;
    State().Publish(std::move(snap));
    HandleCommands();
}

// Evidence that authored quests reach this client: A2PlayerQuestComponent::Client_SetQuests(
// FAAQuestBundle FullTable @0x00, bool IsRemoving @0x40) runs through ProcessEvent when it arrives.
// (g_setQuestsFn is declared with the other globals at the top; the pump resolves it.)

void LogIncomingQuests(const void* parms)
{
    const uint8_t* b = static_cast<const uint8_t*>(parms);
    const wchar_t* uid = *reinterpret_cast<const wchar_t* const*>(b);
    const uint8_t* rows = *reinterpret_cast<const uint8_t* const*>(b + 0x20);
    const int n = *reinterpret_cast<const int32_t*>(b + 0x28);
    Log("[quests] Client_SetQuests arrived: bundle '%ls', %d row(s)%s", uid ? uid : L"", n, b[0x40] ? " (removing)" : "");
    for (int i = 0; rows && i < n && i < 1024; ++i)             // GlyphID FName @0x100 of each FAAQuestEntry
    {
        const std::string g = reinterpret_cast<const SDK::FName*>(rows + static_cast<size_t>(i) * 0x120 + 0x100)->ToString();
        if (!g.empty() && g != "None" && std::find(g_glyphs.begin(), g_glyphs.end(), g) == g_glyphs.end()) g_glyphs.push_back(g);
        const uint8_t* r = rows + static_cast<size_t>(i) * 0x120;
        const uint32_t* id = reinterpret_cast<const uint32_t*>(r);
        char hx[40];
        snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", id[0], id[1], id[2], id[3]);
        const wchar_t* t = *reinterpret_cast<const wchar_t* const*>(r + 0xF0);
        std::string title;
        for (int k = 0; t && t[k] && k < 80; ++k) title.push_back(t[k] < 128 ? static_cast<char>(t[k]) : '?');
        bool known = false;
        for (auto& q : g_knownQuests) if (q.id == hx) { q.title = title; q.glyph = g; known = true; break; }
        if (!known) g_knownQuests.push_back({ hx, title, g });
    }
    std::sort(g_glyphs.begin(), g_glyphs.end());
    if (!uid || wcscmp(uid, L"SpecEditorQuests") != 0 || !rows) return;
    for (int i = 0; i < n && i < 32; ++i)
    {
        const uint8_t* r = rows + static_cast<size_t>(i) * 0x120;
        const uint32_t* g = reinterpret_cast<const uint32_t*>(r);
        const wchar_t* t = *reinterpret_cast<const wchar_t* const*>(r + 0xF0);
        Log("[quests]   authored quest '%ls' id=%08X%08X%08X%08X prereqs=%d", t ? t : L"?",
            g[0], g[1], g[2], g[3], *reinterpret_cast<const int32_t*>(r + 0x58));
    }
}

void HandleServerMessage(const wchar_t* w)
{
    std::string msg;
    for (int i = 0; w[i] && i < 60000; ++i) msg.push_back(static_cast<char>(w[i] < 128 ? w[i] : '?'));
    if (msg.rfind("SE|PROP|", 0) == 0) ApplyRemoteProp(msg);
    else if (msg.rfind("SE|NOTE|", 0) == 0) { Notes().Set(msg.substr(8)); Log("[note] %s", msg.substr(8).c_str()); }
    else if (msg.rfind("SE|LVDATA|", 0) == 0)
    {
        // SE|LVDATA|<name>|<i>|<n>|<hex>: assemble, then write Documents\RigelLevels\<name>.a2level.
        const size_t a = 10, b = msg.find('|', a), c2 = b == std::string::npos ? b : msg.find('|', b + 1),
                     d = c2 == std::string::npos ? c2 : msg.find('|', c2 + 1);
        if (d == std::string::npos) return;
        const std::string name = msg.substr(a, b - a);
        const int i = atoi(msg.substr(b + 1, c2 - b - 1).c_str()), n = atoi(msg.substr(c2 + 1, d - c2 - 1).c_str());
        if (n <= 0 || n > 4096 || i < 0 || i >= n) return;
        auto& parts = g_lvExportParts[name];
        if (static_cast<int>(parts.size()) != n) parts.assign(n, std::string());
        parts[i] = msg.substr(d + 1);
        if (parts[i].empty()) parts[i] = " ";                     // an empty level still completes
        for (const auto& p : parts) if (p.empty()) return;
        std::string text;
        for (const auto& p : parts)
            for (size_t k = 0; k + 1 < p.size(); k += 2) text.push_back(static_cast<char>(strtoul(p.substr(k, 2).c_str(), nullptr, 16)));
        g_lvExportParts.erase(name);
        const std::wstring path = LevelsDir() + L"\\" + std::wstring(name.begin(), name.end()) + L".a2level";
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f)
        {
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            int objs = 0;
            for (size_t k = 0; (k = text.find("\nO\t", k)) != std::string::npos; ++k) ++objs;
            Notes().Set("Saved '" + name + "' to Documents\\RigelLevels (" + std::to_string(objs) + " object(s)).");
            Log("[levels] exported '%s' -> %ls (%zu byte(s), %d object(s))", name.c_str(), path.c_str(), text.size(), objs);
        }
        else Notes().Set("Couldn't write the level file to Documents\\RigelLevels.");
    }
    else if (msg.rfind("SE|OWNLOCKS|", 0) == 0)
    {
        g_ownLockMap.clear();
        const std::string body = msg.substr(12);
        for (size_t b = 0; b < body.size();)
        {
            size_t e = body.find(';', b);
            if (e == std::string::npos) e = body.size();
            const std::string ent = body.substr(b, e - b);
            b = e + 1;
            const size_t c1 = ent.find(','), c2 = c1 == std::string::npos ? std::string::npos : ent.find(',', c1 + 1);
            if (c2 == std::string::npos) continue;
            g_ownLockMap[ent.substr(0, c1)] = { ent.substr(c1 + 1, c2 - c1 - 1) == "1", ent.substr(c2 + 1) };
        }
        Log("[locks] %zu object(s) owner-locked", g_ownLockMap.size());
    }
    else if (msg.rfind("SE|ERR|", 0) == 0)
    {
        // SE|ERR|<title>|<what happened and how to fix it>
        const std::string rest = msg.substr(7);
        const size_t bar = rest.find('|');
        ProblemBox::Item it;
        it.title = bar == std::string::npos ? std::string("Problem") : rest.substr(0, bar);
        it.text = bar == std::string::npos ? rest : rest.substr(bar + 1);
        Problems().Push(it);
        Log("[error] %s: %s", it.title.c_str(), it.text.c_str());
    }
    else if (msg.rfind("SE|LVLIST|", 0) == 0)
    {
        // SE|LVLIST|<loaded here ;-separated>|<name\tautoload\tloaded\tupdated\tsize>\x1E...
        const size_t bar = msg.find('|', 10);
        if (bar == std::string::npos) return;
        const std::string here = ";" + msg.substr(10, bar - 10) + ";";
        const std::string body = msg.substr(bar + 1);
        g_levels.clear();
        for (size_t b = 0; b < body.size();)
        {
            size_t e = body.find('\x1E', b);
            if (e == std::string::npos) e = body.size();
            const std::string ent = body.substr(b, e - b);
            b = e + 1;
            std::vector<std::string> f;
            for (size_t x = 0; x <= ent.size();) { size_t y = ent.find('\t', x); if (y == std::string::npos) y = ent.size(); f.push_back(ent.substr(x, y - x)); x = y + 1; }
            if (f.empty() || f[0].empty()) continue;
            Snapshot::LevelInfo li;
            li.name = f[0];
            li.autoload = f.size() > 1 && f[1] == "1";
            li.wanted = f.size() > 2 && f[2] == "1";
            li.updated = f.size() > 3 ? f[3] : "";
            li.size = f.size() > 4 ? atoi(f[4].c_str()) : 0;
            li.here = here.find(";" + li.name + ";") != std::string::npos;
            g_levels.push_back(li);
        }
        ++g_levelsSerial;
        Log("[levels] %zu saved level(s)", g_levels.size());
    }
    else if (msg.rfind("SE|QLIST|", 0) == 0)
    {
        // Every quest the server knows (the station's and ours), for the quest pickers and icon list.
        const std::string body = msg.substr(9);
        for (size_t b = 0; b < body.size();)
        {
            size_t e = body.find('\x1E', b);
            if (e == std::string::npos) e = body.size();
            const std::string ent = body.substr(b, e - b);
            b = e + 1;
            const size_t f1 = ent.find('\x1F'), f2 = f1 == std::string::npos ? f1 : ent.find('\x1F', f1 + 1);
            if (f2 == std::string::npos) continue;
            const std::string id = ent.substr(0, f1), title = ent.substr(f1 + 1, f2 - f1 - 1), glyph = ent.substr(f2 + 1);
            bool known = false;
            for (auto& q : g_knownQuests) if (q.id == id) { q.title = title; q.glyph = glyph; known = true; break; }
            if (!known) g_knownQuests.push_back({ id, title, glyph });
            if (!glyph.empty() && glyph != "None" && std::find(g_glyphs.begin(), g_glyphs.end(), glyph) == g_glyphs.end())
                g_glyphs.push_back(glyph);
        }
        std::sort(g_glyphs.begin(), g_glyphs.end());
        Log("[quests] catalogue: %zu quest(s) known", g_knownQuests.size());
    }
    else if (msg.rfind("SE|SBDATA|", 0) == 0)
    {
        // SE|SBDATA|<ident>|<path>\x1F<kind>\x1F<value>\x1E...
        const size_t bar = msg.find('|', 10);
        if (bar == std::string::npos) return;
        const std::string ident = msg.substr(10, bar - 10);
        if (ident != g_dataIdent)
        {
            // Not something we asked for: the server pushes an object's data after it rebuilds it (a script
            // attached / removed / wired comes back as a new actor). Take it if we can match the object.
            std::string handle;
            for (const SceneObject& so : g_lastObjects) if (IdentFor(so.handle) == ident) { handle = so.handle; break; }
            if (handle.empty()) return;
            g_dataIdent = ident;
            g_dataHandle = handle;
        }
        g_data.clear();
        const std::string body = msg.substr(bar + 1);
        for (size_t b = 0; b < body.size();)
        {
            size_t e = body.find('\x1E', b);
            if (e == std::string::npos) e = body.size();
            const std::string ent = body.substr(b, e - b);
            b = e + 1;
            const size_t f1 = ent.find('\x1F'), f2 = f1 == std::string::npos ? f1 : ent.find('\x1F', f1 + 1);
            if (f2 == std::string::npos) continue;
            g_data.push_back({ ent.substr(0, f1), ent.substr(f1 + 1, f2 - f1 - 1), ent.substr(f2 + 1) });
        }
        ++g_dataSerial;
        Log("[data] %zu value(s) for %s", g_data.size(), g_dataIdent.c_str());
    }
}

#ifdef RIGEL_EOS
}  // namespace
}  // namespace se
void GlyphFix_PeTick();
namespace se {
namespace {
#endif

void __fastcall PE_Hook(void* ctx, void* fn, void* parms)
{
    if (fn && fn == g_setQuestsFn && parms && GetCurrentThreadId() == g_mainThread)
        __try { LogIncomingQuests(parms); } __except (EXCEPTION_EXECUTE_HANDLER) {}

    // Editor broadcasts from the server ride APlayerController::ClientMessage (see ApplyRemoteProp).
    if (fn && fn == g_clientMsgFn && parms && GetCurrentThreadId() == g_mainThread)
    {
        const wchar_t* w = *static_cast<const wchar_t* const*>(parms);
        if (w && w[0] == L'S' && w[1] == L'E' && w[2] == L'|')
        {
            __try { HandleServerMessage(w); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[props] broadcast apply faulted"); }
            return;                                          // ours: not for the console
        }
    }

    // ProcessEvent also runs on loader/worker threads. Everything here touches UObjects and the camera,
    // so it only runs on the game thread -- UE's main thread, recorded in DllMain.
    if (GetCurrentThreadId() != g_mainThread) { g_peOrig(ctx, fn, parms); return; }
#ifdef RIGEL_EOS
    GlyphFix_PeTick();   // the Rift build's parkour glyph fix shares this hook (see glyphfix.cpp)
#endif

    // The editor camera moves every frame (well, at most 240 times a second), not at the pump's 4 Hz.
    if (Cam().active)
    {
        static LARGE_INTEGER s_last{}, s_freq{};
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
        if (t.QuadPart - s_last.QuadPart >= s_freq.QuadPart / 240)
        {
            s_last = t;
            __try { CameraTick(); } __except (EXCEPTION_EXECUTE_HANDLER) { Cam().active = false; Log("[cam] tick faulted - camera released"); }
        }
    }

    // Drags and queued commands every frame, not at the pump's 4 Hz -- that wait was most of the lag.
    // The guard stops re-entry: both call ProcessEvent, which lands back here.
    {
        static bool s_in = false;
        static LARGE_INTEGER s_lastD{}, s_freqD{};
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        if (!s_freqD.QuadPart) QueryPerformanceFrequency(&s_freqD);
        if (!s_in && t.QuadPart - s_lastD.QuadPart >= s_freqD.QuadPart / 240)
        {
            s_in = true;
            s_lastD = t;
            __try { SimDragTick(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_sim.on = false; }
            if (g_uiVisible)
            {
                __try { RefreshTransforms(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
                __try { PublishCamera(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            static ULONGLONG s_lastPick = 0;
            if (GetTickCount64() - s_lastPick >= 16)
            {
                s_lastPick = GetTickCount64();
                __try { PickTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
            }
            __try { DragTick(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[drag] tick faulted"); }
            CamReportTick();
            PacedTick();
            __try { HandleCommands(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[game] commands faulted"); }
            s_in = false;
        }
    }

    const ULONGLONG now = GetTickCount64();
    if (now - g_lastPump >= 250)             // 4 Hz is plenty for an editor and costs nothing
    {
        g_lastPump = now;
        static volatile long busy = 0;
        if (InterlockedCompareExchange(&busy, 1, 0) == 0)
        {
            __try { PumpImpl(); } __except (EXCEPTION_EXECUTE_HANDLER) { Log("[game] pump faulted"); }
            InterlockedExchange(&busy, 0);
        }
    }
    g_peOrig(ctx, fn, parms);
}

}  // namespace

void GameThreadPump() { PumpImpl(); }

bool InstallGameHook()
{
    // Idempotent: the boot worker may call this twice, and a second MH_CreateHook on the same target
    // fails -- which used to log "game hook not installed" on a hook that was working fine.
    if (g_peOrig) return true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    void* pe = reinterpret_cast<void*>(base + SDK::Offsets::ProcessEvent);
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(pe, &PE_Hook, reinterpret_cast<void**>(&g_peOrig)) != MH_OK) return false;
    if (MH_EnableHook(pe) != MH_OK) return false;
    Log("[game] ProcessEvent hooked @%p", pe);
    return true;
}

}  // namespace se
