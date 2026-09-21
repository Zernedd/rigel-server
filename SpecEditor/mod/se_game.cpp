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
        if (it.name.rfind("LE_", 0) == 0) s_items.push_back(std::move(it));
    }
    fclose(f);
    Log("[game] catalogue: %d LE prefab(s) from le_catalogue.txt", (int)s_items.size());
    return s_items;
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
        snap.palette.push_back(std::move(it));
        ++extra;
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

void BuildObjects(Snapshot& snap)
{
    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    if (!actorCls) return;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n && snap.objects.size() < 1024; ++i)
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
        if (cn.rfind("LE_", 0) != 0) continue;

        SceneObject so;
        so.ptr = o;
        so.handle = o->GetName();
        so.label = so.handle;
        so.className = cn;

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
            SendToServer("SE|SPAWN|" + c.str + "|" + Fmt3(c.loc) + "|" + Fmt3(c.rot));
            Log("[game] spawn request: %s", c.str.c_str());
            break;

        case CmdType::SetTransform:
        {
            // c.loc/rot/scale are the NEW transform; the identity has to be built from where the actor
            // still is, which is what the server will match on.
            const std::string id = IdentFor(c.str);
            if (id.empty()) { Log("[game] xform: no identity for handle %s", c.str.c_str()); break; }
            SendToServer("SE|XFORM|" + id + "|" + Fmt3(c.loc) + "|" + Fmt3(c.rot) + "|" + Fmt3(c.scale));
            NoteCommanded(c.str, c.loc);
            PredictTransform(c.str, c.loc, c.rot, c.scale);   // show it now; the server's copy follows
            break;
        }

        case CmdType::DeleteObject:
        {
            const std::string id = IdentFor(c.str);
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
            char extra[64];
            snprintf(extra, sizeof(extra), "|%d|%g|", c.num2, c.f1);
            std::string desc = c.str4;
            for (auto& ch : desc) if (ch == '|') ch = '/';        // '|' is the field separator
            SendToServer("SE|QUEST|" + c.str + "|" + c.str2 + "|" + c.str3 + "|" + std::to_string(c.num) + "|" + steps +
                         extra + desc);
            Log("[game] quest publish: %s '%s' glyph=%s rep=%d, %d step(s)%s", c.str.c_str(), c.str2.c_str(),
                c.str3.c_str(), c.num, (int)g_questSteps.size() - unresolved,
                unresolved ? " (some steps no longer exist and were skipped)" : "");
            g_questSteps.clear();
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
    double x = 0, y = 0, z = 0, pitch = 0, yaw = 0;
    float  speed = 1200.0f;          // units per second
    LARGE_INTEGER last{};
    bool   primed = false;
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
    g_cam.primed = false;
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

void CameraApply()
{
    if (!ObjectAlive(g_cam.actor)) return;
    SDK::Params::Actor_K2_SetActorLocationAndRotation p{};
    p.NewLocation = SDK::FVector{ g_cam.x, g_cam.y, g_cam.z };
    p.NewRotation = SDK::FRotator{ g_cam.pitch, g_cam.yaw, 0.0 };
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

        auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
        const double d2r = 3.14159265358979 / 180.0;
        const double cp = std::cos(g_cam.pitch * d2r), sp = std::sin(g_cam.pitch * d2r);
        const double cy = std::cos(g_cam.yaw * d2r),   sy = std::sin(g_cam.yaw * d2r);
        const double f[3] = { cp * cy, cp * sy, sp }, r[3] = { -sy, cy, 0 };
        double mv[3] = { 0, 0, 0 };
        const double fwd = (down('W') ? 1 : 0) - (down('S') ? 1 : 0);
        const double rgt = (down('D') ? 1 : 0) - (down('A') ? 1 : 0);
        const double up  = (down('E') ? 1 : 0) - (down('Q') ? 1 : 0);
        for (int i = 0; i < 3; ++i) mv[i] = f[i] * fwd + r[i] * rgt;
        mv[2] += up;
        const double step = g_cam.speed * (down(VK_SHIFT) ? 3.0 : 1.0) * dt;
        g_cam.x += mv[0] * step; g_cam.y += mv[1] * step; g_cam.z += mv[2] * step;
    }
    CameraApply();
}

// F: keep the view direction and move back far enough to frame the point.
void CameraFocus(const Vec3& p)
{
    if (!Cam().active) return;
    const double d2r = 3.14159265358979 / 180.0;
    const double cp = std::cos(g_cam.pitch * d2r), sp = std::sin(g_cam.pitch * d2r);
    const double cy = std::cos(g_cam.yaw * d2r),   sy = std::sin(g_cam.yaw * d2r);
    g_cam.x = p.x - cp * cy * 500.0; g_cam.y = p.y - cp * sy * 500.0; g_cam.z = p.z - sp * 500.0;
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
        pos = { g_cam.x, g_cam.y, g_cam.z }; rot = { g_cam.pitch, g_cam.yaw, 0.0 };
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
    static uint32_t    s_sentSeq = 0;
    static ULONGLONG   s_lastSend = 0, s_holdUntil = 0;
    static std::string s_handle;
    static Vec3        s_loc, s_scale{ 1, 1, 1 };
    static Rot         s_rot;
    static bool        s_active = false;

    uint32_t seq;
    {
        LiveDrag& d = Drag();
        std::lock_guard<std::mutex> lk(d.mx);
        seq = d.seq;
        if (seq != s_sentSeq || d.active)
        {
            s_handle = d.handle; s_loc = d.loc; s_rot = d.rot; s_scale = d.scale; s_active = d.active;
        }
    }
    const ULONGLONG now = GetTickCount64();
    if (s_handle.empty()) return;
    if (s_active) s_holdUntil = now + 1200;

    if (seq != s_sentSeq && (!s_active || now - s_lastSend >= 33))
    {
        const std::string id = IdentFor(s_handle);
        if (!id.empty())
        {
            SendToServer("SE|XFORM|" + id + "|" + Fmt3(s_loc) + "|" + Fmt3(s_rot) + "|" + Fmt3(s_scale));
            NoteCommanded(s_handle, s_loc);
        }
        s_sentSeq = seq;
        s_lastSend = now;
        if (!s_active) s_holdUntil = now + 1200;
    }
    if (now < s_holdUntil) PredictTransform(s_handle, s_loc, s_rot, s_scale);
    else if (!s_active) s_handle.clear();
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

void PickTick()
{
    Vec3 eye, dir;
    {
        PickState& ps = Pick();
        std::lock_guard<std::mutex> lk(ps.mx);
        if (!ps.valid) { ps.hovered.clear(); return; }
        eye = ps.eye; dir = ps.dir;
    }
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
    PickState& ps = Pick();
    std::lock_guard<std::mutex> lk(ps.mx);
    ps.hovered = hovered;
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
    Log("[script] SCREENPOS center=%.0f,%.0f handleX=%.0f,%.0f handleY=%.0f,%.0f handleZ=%.0f,%.0f ringZ=%.0f,%.0f worldPerPx=%.4f",
        cx, cyy, hx[0], hy[0], hx[1], hy[1], hx[2], hy[2], rzx, rzy, wpp);
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

void RunScript(const Snapshot& snap)
{
    LoadScriptOnce();
    if (g_scriptPc >= g_script.size() || !g_pc || !snap.worldReady) return;
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

    if (!strcmp(op, "spawn"))
    {
        const PaletteItem* pick = nullptr;
        for (const auto& it : g_palette) if (it.name.find(rest) != std::string::npos) { pick = &it; break; }
        if (!pick) { Log("[script] FAIL spawn: nothing in the %d-item palette matches '%s'", (int)g_palette.size(), rest.c_str()); return; }
        const double d2r = 3.14159265358979 / 180.0;
        const double cp = std::cos(snap.cameraRot.pitch * d2r), sp = std::sin(snap.cameraRot.pitch * d2r);
        const double cy = std::cos(snap.cameraRot.yaw * d2r),   sy = std::sin(snap.cameraRot.yaw * d2r);
        Command c{ CmdType::SpawnItem };
        c.str = pick->path;
        c.loc = { snap.cameraPos.x + cp * cy * 400.0, snap.cameraPos.y + cp * sy * 400.0, snap.cameraPos.z + sp * 400.0 };
        c.rot = { 0.0, snap.cameraRot.yaw + 180.0, 0.0 };   // facing the camera, as the UI places things
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
    if (!strcmp(op, "pick"))                  // pick <Class> -- target the existing instance nearest the camera
    {
        const SceneObject* best = nullptr;
        double bestD = 1e30;
        for (const SceneObject& s : g_lastObjects)
        {
            if (s.className.find(rest) == std::string::npos) continue;
            const double dx = s.location.x - snap.cameraPos.x, dy = s.location.y - snap.cameraPos.y, dz = s.location.z - snap.cameraPos.z;
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
                    t != sereflect::PType::Unsupported ? sereflect::Read(obj, fp, t).c_str() : "");
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
    if (world != s_world) { s_world = world; g_paletteDirty = true; }
    if (snap.worldReady && g_palette.empty() && GetTickCount64() - s_lastPaletteTry > 5000) g_paletteDirty = true;
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

    // The editor camera follows state: it is the view while editing with the UI up, and INSERT or Stop
    // Editing hands the player their own view back. A travel destroys the camera actor, so a dead actor
    // just re-arms activation.
    if (Cam().active && !ObjectAlive(g_cam.actor)) Cam().active = false;
    const bool wantCam = snap.worldReady && g_pc && g_editorMode && g_uiVisible;
    if (wantCam && !Cam().active) CameraActivate(snap);
    else if (!wantCam && Cam().active) CameraDeactivate();

    FillProps(snap);
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
    for (int i = 0; w[i] && i < 1100; ++i) msg.push_back(static_cast<char>(w[i] < 128 ? w[i] : '?'));
    if (msg.rfind("SE|PROP|", 0) == 0) ApplyRemoteProp(msg);
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
