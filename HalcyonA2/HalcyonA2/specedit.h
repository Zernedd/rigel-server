// specedit.h - Spec Editor, server side.
//
// Counterpart to the Spec Editor client mod (OrionDriftStuff\SpecEditor). The client is an ImGui level
// editor. It cannot spawn anything itself: an actor spawned on a client is local-only and nobody else
// would ever see it. So the client tunnels editor commands to us over an RPC the build already has and
// already replicates -- ALevelEditorPawn::Server_AttemptLockObject(const FString& idx) -- and WE spawn,
// on the server, with the actor marked replicated. That is what puts it on the wire through Iris to
// every client. A string without the "SE|" prefix is a genuine lock request and falls through to the
// game's own handler untouched, so stock multi-user locking is unaffected.
//
// ============================================================================================
// SECURITY - read this before changing anything here
// ============================================================================================
// This makes a server act on a string that arrived from a client, which is exactly the shape of a
// remote-code-execution surface. It is deliberately fenced in, and the fences are the point:
//
//   1. OFF BY DEFAULT. Requires -SpecEdit on the server command line. A normal station cannot be
//      driven at all, no matter what a client sends.
//   2. ALLOWLISTED. Even on an editor server, commands are honoured only from user ids listed in
//      run\backend\spec_editors.txt (same convention as auto_complete_quests.txt: one id per line,
//      '#' comments, re-read when the file changes). No file, or an empty file, means NOBODY.
//   3. SPAWNS ARE RESTRICTED TO THE EDITOR PALETTE. Only classes whose name begins with "LE_" can be
//      spawned - the level-editor prefabs. An arbitrary UClass name is refused, so this cannot be used
//      to instantiate engine or gameplay classes.
//   4. EDITS ARE RESTRICTED TO WHAT THE EDITOR MADE. Transform and delete only accept actors whose
//      class is likewise "LE_"-prefixed, so no client can move or destroy real station geometry,
//      players, or gamemode actors.
//   5. BOUNDED. Command strings are length-capped, the command rate is capped per player, and the
//      handler is wrapped in SEH that disables the whole feature on a fault.
//
// Relax any of these and a joining player can rearrange or delete the station. Do not widen the class
// filter to "anything", and do not default g_specEdit to true.

#pragma once

#include "../../SpecEditor/mod/se_reflect.h"   // property reflection shared with the client mod
#include <deque>
#include <array>
#include <algorithm>

// ---- state ---------------------------------------------------------------------------------
static bool       g_specEdit = false;          // -SpecEdit turns it on; see the note above
static bool       g_seLocalTest = false;       // -SpecEditLocalTest: admit org-less callers. LOCAL ONLY.
static SDK::FName g_seLockFnName{};
static bool       g_seLockFnResolved = false;
static ULONGLONG  g_seLastCmd = 0;
static int        g_seCmdsThisSecond = 0;

static std::vector<std::string> g_seEditors;   // allowlisted user ids
static FILETIME                 g_seEditorsStamp{};


// ---- helpers -------------------------------------------------------------------------------
static std::vector<std::string> SeSplit(const std::string& s, char sep, size_t cap = 64)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size() && out.size() < cap; ++i)
        if (i == s.size() || s[i] == sep) { out.push_back(s.substr(start, i - start)); start = i + 1; }
    return out;
}

static bool SeVec(const std::string& s, double* out3)
{
    const auto p = SeSplit(s, ',');
    if (p.size() < 3) return false;
    for (int i = 0; i < 3; ++i)
    {
        out3[i] = atof(p[i].c_str());
        if (!_finite(out3[i]) || fabs(out3[i]) > 1.0e7) return false;   // keep junk out of the world
    }
    return true;
}

// Only the level-editor prefabs may be touched. This is fence 3/4 above.
static bool SeSandboxClassOk(const std::string& className);   // below: a sandbox-placeable class, sandbox mode
// What the editor may place and edit: the LE_ prefabs, and -- with -SpecEditSandbox, where they are placed
// through the game's own object system -- anything the sandbox itself can place (boost pads, traps...).
// Station level actors the sandbox has no prefab for, but that are worth placing: the yellow speed pads
// and boost tanks (21 / 19 of them on the station). Placed as ordinary replicated actors.
static bool SeExtraPlaceable(const std::string& className)
{
    return className == "BP_BoostPad_Omnidirectional_C" || className == "BP_BoostTank_World_C";
}
static bool SeIsEditorClass(const std::string& className)
{
    if (className.rfind("LE_", 0) == 0 || SeExtraPlaceable(className)) return true;
    return SeSandboxClassOk(className);
}

// run\backend\spec_editors.txt, re-read when it changes. No file = nobody is allowed.
static const std::vector<std::string>& SeEditors()
{
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    const size_t cut = p.find_last_of(L'\\');
    if (cut != std::wstring::npos) p = p.substr(0, cut);
    p += L"\\spec_editors.txt";

    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &fa)) { g_seEditors.clear(); return g_seEditors; }
    if (CompareFileTime(&fa.ftLastWriteTime, &g_seEditorsStamp) == 0) return g_seEditors;
    g_seEditorsStamp = fa.ftLastWriteTime;

    g_seEditors.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, p.c_str(), L"r") == 0 && f)
    {
        char line[512];
        while (fgets(line, sizeof(line), f))
        {
            std::string s(line);
            const size_t h = s.find('#');
            if (h != std::string::npos) s = s.substr(0, h);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
            size_t b = s.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            g_seEditors.push_back(s.substr(b));
        }
        fclose(f);
    }
    HxLog("[HalcyonA2][SPECEDIT] editor allowlist: %zu id(s)\n", g_seEditors.size());
    return g_seEditors;
}

// Is the caller allowed to edit? The RPC arrives on either the player's controller (the normal path,
// Server_SetVivoxParticipantID) or a level-editor pawn (Server_AttemptLockObject); both resolve to the
// same controller, so a client cannot claim to be someone else - the identity is the server's own record.
static bool SeAuthorised(SDK::UObject* ctx)
{
    const auto& allow = SeEditors();
    if (!ctx) return false;

    static SDK::UClass* pcCls = nullptr;
    if (!pcCls) pcCls = SDK::UObject::FindClassFast("PlayerController");
    const bool isPc = pcCls && ctx->IsA(pcCls);

    // Identity comes from the SERVER's own record, never from anything the client put in this command:
    // controller -> org-scoped id (AVRPlayerController+0xA30), which Server_LoginToStationDashboard writes
    // after the dashboard login. A pawn reaches its controller through APawn+0x2D0. The local host never
    // gets an org, so it is naturally refused. Same id the dashboard shows as user_id.
    void* pc = isPc ? static_cast<void*>(ctx)
                    : *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ctx) + 0x2D0);
    if (!pc) return false;
    const std::string uid = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0xA30));

    // -SpecEditLocalTest: LOCAL TESTING ONLY. A no-auth local server has no dashboard, so no player ever
    // gets an org and nobody could be authorised. This admits the org-less caller -- and it is loud about
    // it every time, because it must never be on a server that real players can reach.
    if (uid.empty() && g_seLocalTest)
    {
        static SDK::UObject* s_announced = nullptr;       // once per caller, not once per command
        if (s_announced != ctx)
        {
            s_announced = ctx;
            HxLog("[HalcyonA2][SPECEDIT] LOCAL TEST: admitting org-less caller %s\n", ctx->GetName().c_str());
        }
        return true;
    }
    if (allow.empty() || uid.empty()) return false;
    for (const auto& a : allow)
        if (_stricmp(a.c_str(), uid.c_str()) == 0) return true;

    static ULONGLONG s_lastWarn = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastWarn > 5000)
    {
        s_lastWarn = now;
        HxLog("[HalcyonA2][SPECEDIT] REFUSED: %s is not in spec_editors.txt\n", uid.c_str());
    }
    return false;
}

// Actors are addressed by CLASS AND CURRENT POSITION, as "<ClassName>@<x>,<y>,<z>" -- never by name.
// UE does not replicate actor names: an actor spawned here generally carries a different name on the
// client, so a name round-tripped back from the editor would resolve to nothing and XFORM/DELETE would
// silently do nothing at all. Class and position are the two things both sides genuinely agree on,
// because transform replication keeps the position in sync.
//
// Not on any hot path: this runs only when a command arrives, which fence 5 caps at 60/second. It must
// never be called from the tick -- a full GObjects walk there is what makes driftball jitter.
static SDK::AActor* SeFindEditorActor(const std::string& ident)
{
    const size_t at = ident.find('@');
    if (at == std::string::npos) return nullptr;
    const std::string wantCls = ident.substr(0, at);
    double want[3]{};
    if (!SeVec(ident.substr(at + 1), want)) return nullptr;
    if (!SeIsEditorClass(wantCls)) return nullptr;              // fence 4: LE_ prefabs only

    // [PERF] This runs for every editor command -- ~30/s while someone drags -- inside the RPC handler, where
    // [PROF] never sees it. It used to walk all ~167k objects and build a class-name string for every actor,
    // each time: tens of ms of game thread per command, which is what drops ball-sim frames (MI). Now: the
    // class is resolved once by name, sandbox objects come from the shared PrefabComponent index, and only a
    // miss (a non-sandbox actor, or one spawned in the last second) falls back to a walk -- a pointer compare.
    static std::unordered_map<std::string, SDK::UClass*> s_cls;
    SDK::UClass* wantUCls = nullptr;
    {
        auto it = s_cls.find(wantCls);
        if (it != s_cls.end()) wantUCls = it->second;
        else if ((wantUCls = SDK::UObject::FindClassFast(wantCls)) != nullptr) s_cls[wantCls] = wantUCls;
    }
    if (!wantUCls) return nullptr;

    SDK::AActor* best   = nullptr;
    double       bestD2 = 1.0e18;
    int          within = 0;
    const double tol2   = 100.0 * 100.0;                        // 1m, absorbs replication lag
    auto consider = [&](SDK::UObject* o)
    {
        if (!o || o->Class != wantUCls || o->IsDefaultObject()) return;
        if (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01) return;   // bActorIsBeingDestroyed
        // RootComponent@0x1A8 -> ComponentToWorld@0x1D0, translation at +0x20
        void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1A8);
        if (!root) return;
        const double* t = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20);
        const double dx = t[0] - want[0], dy = t[1] - want[1], dz = t[2] - want[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > tol2) return;
        ++within;
        if (d2 < bestD2) { bestD2 = d2; best = static_cast<SDK::AActor*>(o); }
    };
    static SDK::UClass* pcCls = nullptr;
    if (!pcCls) pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    for (SDK::UObject* pc : ClassObjects(pcCls)) consider(pc->Outer);
    if (!best)
    {
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < n; ++i) consider(SDK::UObject::GObjects->GetByIndex(i));
    }

    if (within > 1)
        HxLog("[HalcyonA2][SPECEDIT] %d %s within 1m -- taking the nearest\n", within, wantCls.c_str());
    if (!best)
        HxLog("[HalcyonA2][SPECEDIT] no %s near (%.0f,%.0f,%.0f)\n", wantCls.c_str(), want[0], want[1], want[2]);
    return best;
}

// The client sends a package path; the leaf after the last '.' is the generated class name.
static SDK::UClass* SeResolveEditorClass(const std::string& path)
{
    std::string leaf = path;
    const size_t dot = leaf.find_last_of('.');
    if (dot != std::string::npos) leaf = leaf.substr(dot + 1);
    const size_t slash = leaf.find_last_of('/');
    if (slash != std::string::npos) leaf = leaf.substr(slash + 1);
    if (!SeIsEditorClass(leaf)) { HxLog("[HalcyonA2][SPECEDIT] refused non-palette class '%s'\n", leaf.c_str()); return nullptr; }
    if (SDK::UClass* loaded = SDK::UObject::FindClassFast(leaf)) return loaded;

    // Not in memory yet. The station only ever loads a handful of the ~97 LE prefabs the game ships, so
    // refusing here would cap the editor at whatever the map happened to reference. Load it by path --
    // but only a /Game/ path, and the result is re-checked below, so a crafted path cannot load and
    // spawn anything that is not an LE prefab. Blocking, so the first spawn of each class hitches the
    // server briefly; every later spawn of it is an in-memory lookup.
    if (path.rfind("/Game/", 0) != 0 || path.find("..") != std::string::npos)
    {
        HxLog("[HalcyonA2][SPECEDIT] refused load of '%s' (not a /Game/ path)\n", path.c_str());
        return nullptr;
    }
    const std::wstring w(path.begin(), path.end());
    const ULONGLONG t0 = GetTickCount64();
    SDK::FSoftClassPath scp = SDK::UKismetSystemLibrary::MakeSoftClassPath(SDK::FString(w.c_str()));
    SDK::UClass* cls = SDK::UKismetSystemLibrary::LoadClassAsset_Blocking(
        SDK::UKismetSystemLibrary::Conv_SoftClassPathToSoftClassRef(scp));
    if (!cls) { HxLog("[HalcyonA2][SPECEDIT] could not load %s\n", path.c_str()); return nullptr; }
    if (!SeIsEditorClass(cls->GetName()))
    {
        HxLog("[HalcyonA2][SPECEDIT] refused: %s loaded as non-LE class %s\n", path.c_str(), cls->GetName().c_str());
        return nullptr;
    }
    HxLog("[HalcyonA2][SPECEDIT] loaded %s on demand (%llu ms)\n", cls->GetName().c_str(), GetTickCount64() - t0);
    return cls;
}

// ---- operations ----------------------------------------------------------------------------
static void SeReplicateTransform(SDK::AActor* a);   // below
static bool g_seSandbox;   // -SpecEditSandbox: place through the sandbox's own object system
static std::vector<std::array<std::string, 3>> g_sbPendingProps;   // {comp, key, value} for the next sandbox spawn
static std::string SbTypeFor(const std::string& className);   // below
static std::string SeSandboxSpawn(const std::string& uniqueId, const double* loc, const double* rot, const double* scl);   // below
static bool SeHasComponent(SDK::AActor* a, const char* cls);   // below
static void SeMarkProxyDirty(SDK::AActor* a);   // below
static void SeTrack(SDK::AActor* a);   // below
static SDK::UObject* SbPrefabOf(SDK::AActor* a);   // below
static bool SeSandboxXform(SDK::AActor* a, const double* loc, const double* rot, const double* scl);   // below
static bool SeSandboxDelete(SDK::AActor* a);   // below
static bool SbOwnedXform(const std::string& ident, const std::string& locs, const std::string& rots, const std::string& scls);   // below
static bool SbOwnedDelete(const std::string& ident);   // below
static void SeError(SDK::UObject* caller, const std::string& title, const std::string& text);   // below
static void SbOwnedForget(const std::string& idx);     // below
struct SbOwned;
static std::string SbRespawnKeep(SDK::AActor* a, const SbOwned& keep, const char* why);   // below
static SDK::AActor* SbActorForIdx(const std::string& idx);                                  // below
static SDK::AActor* SbActorForIdx(const std::string& idx, bool indexOnly);                  // below
static double g_sbHostOutsideCm = 0;   // set by SbHostGamemode: how far outside every importance volume (cm; 0 inside)
static void SbMoveFlush();                                                                  // below (moves settle)
static void SbEditorSlotTick(ULONGLONG now);                                                // below (editor-made slot)
static void SbEditorSlotClearTick(ULONGLONG now);                                           // below
static std::string g_sbSpawnCls;   // the editor class of the sandbox spawn in flight (for SbOwned)
static bool SeTeamChangerAllowedAt(const double* loc);   // below (game modes)
static void SeModeTick();   // below (game modes)
static std::vector<SDK::AActor*> g_seFabSlots;   // game mode fabricator: every fabricated mode's slot (hosts first)
static bool SeRefusedPrefab(const std::string& name);   // below (team changers)
static std::string g_sbSpawnPath;  // ...and the palette path it was asked for (saved levels reload by it)
static std::vector<std::string> g_sbPendingScripts;   // custom Luau script names for the spawn in flight
static std::string   g_sbForceIdx;                  // respawn under this idx (a rebuilt scripted object keeps its id)
static SDK::UObject* g_sbForceLgm = nullptr;         // host the spawn in flight in this gamemode (script references)
static std::string g_lvLoading;    // the saved level whose content is being created right now ("" = editor work)
// Per-object owner locks. Every sandbox object remembers who placed it; its owner can lock it, and then no
// other editor can move, delete, re-script or edit it. Keyed by sandbox idx, which survives the rebuild a
// move does. g_seCaller is whoever sent the command being handled ("" for the server's own work).
struct SeOwnLock { std::string owner, ownerName; bool locked = false; };
static std::unordered_map<std::string, SeOwnLock> g_ownLocks;
static std::string g_seCaller, g_seCallerName;
// Each editor's open level (SE|LVSCENE): what they place belongs to it from the start, so another editor
// saving THEIR level never sweeps these objects into the wrong file.
static std::unordered_map<std::string, std::string> g_seCallerScene;
static std::string SeCallerScene() { auto it = g_seCallerScene.find(g_seCaller); return it == g_seCallerScene.end() ? std::string() : it->second; }
static void SeOwnLocksPush(SDK::UObject* onlyPc);   // below
static void SeLvRecordPlain(SDK::AActor* a, const std::string& path);   // below (saved levels)
static std::string SeLvIdent(SDK::AActor* a);                            // below (saved levels)
static int  SeBroadcast(const std::string& msg, SDK::UObject* pc);   // below

// The player controller behind a command's context object: the controller itself (Vivox transport) or a
// pawn (editor-pawn transport), whose APawn::Controller is at +0x2D0.
static SDK::UObject* SeCallerPC(SDK::UObject* ctx)
{
    if (ctx && !ctx->IsA(SDK::APlayerController::StaticClass()))
        ctx = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(ctx) + 0x2D0);
    return ctx && ctx->IsA(SDK::APlayerController::StaticClass()) ? ctx : nullptr;
}

static void SeSpawn(SDK::UObject* pawn, const std::string& path, const std::string& locs, const std::string& rots,
                    const std::string& scls = std::string())
{
    double loc[3]{}, rot[3]{}, scl[3] = { 1, 1, 1 };
    if (!SeVec(locs, loc) || !SeVec(rots, rot)) { HxLog("[HalcyonA2][SPECEDIT] bad transform\n"); return; }
    // Optional scale (a duplicate carries its source's size); older clients send none.
    if (!scls.empty() && !SeVec(scls, scl)) scl[0] = scl[1] = scl[2] = 1;
    for (double& v : scl) if (!(v > 0.01 && v < 100.0)) v = 1;

    SDK::UClass* cls = SeResolveEditorClass(path);
    if (!cls) return;
    if (SeRefusedPrefab(cls->GetName()))
    {
        double tl[3] = { 0, 0, 0 };
        if (!(SeVec(locs, tl) && SeTeamChangerAllowedAt(tl)))
        {
            HxLog("[HalcyonA2][SPECEDIT] refused %s: team changers only work inside a game mode\n", cls->GetName().c_str());
            if (SDK::UObject* pc = SeCallerPC(pawn))
                SeBroadcast("SE|NOTE|Team changers only work inside a game mode: open Window > Game Modes, make one, and place team changers inside its area.", pc);
            return;
        }
    }

    // -SpecEditSandbox: a prefab the sandbox knows is placed as a real sandbox object instead, so every
    // client (vanilla Quest included) builds it with its Luau bound -- text, switches, Physical collision.
    // A sandbox red coin is hidden until a run shows it, and the server builds no actor for it -- placed
    // on its own it was invisible and could not be moved. Coins placed in the editor (checkpoints, coin
    // run previews) are ordinary replicated actors instead: visible to everyone and movable.
    if (g_seSandbox && cls->GetName() != "LE_BP_RedCoin_C")
    {
        const std::string type = SbTypeFor(cls->GetName());
        if (!type.empty())
        {
            g_sbSpawnCls = cls->GetName();
            g_sbSpawnPath = path;
            const std::string placed = SeSandboxSpawn(type, loc, rot, scl);
            g_sbSpawnCls.clear();
            g_sbSpawnPath.clear();
            if (!placed.empty())
            {
                if (g_sbHostOutsideCm > 0)                   // no module's importance volume covers this spot
                    if (SDK::UObject* pc = SeCallerPC(pawn))
                        SeBroadcast("SE|NOTE|This spot is outside every module area (" + std::to_string(static_cast<int>(g_sbHostOutsideCm / 100.0)) +
                                    " m from the nearest). Players away from it may not see what you place here - build inside a game area.", pc);
                // The sandbox spawned the server's own copy synchronously. Placed meshes still need the
                // collision stand-in: their Physical component does not switch collision on by itself.
                char ident[256];
                snprintf(ident, sizeof(ident), "%s@%.1f,%.1f,%.1f", cls->GetName().c_str(), loc[0], loc[1], loc[2]);
                if (SDK::AActor* mine = SeFindEditorActor(ident)) SeMarkProxyDirty(mine);
                return;
            }
            HxLog("[HalcyonA2][SPECEDIT] sandbox spawn of %s failed -- falling back to a plain spawn\n", cls->GetName().c_str());
        }
    }

    SDK::FTransform xf{};
    xf.Rotation    = SDK::FQuat{ 0, 0, 0, 1 };
    xf.Translation = SDK::FVector{ loc[0], loc[1], loc[2] };
    xf.Scale3D     = SDK::FVector{ scl[0], scl[1], scl[2] };

    // Deferred spawn so bReplicates is set BEFORE the actor is finished: after FinishSpawningActor it
    // is too late for the initial replication decision, and the actor would never reach clients.
    SDK::AActor* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        pawn, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr,
        SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!actor) { HxLog("[HalcyonA2][SPECEDIT] deferred spawn returned null\n"); return; }

    actor->bAlwaysRelevant  = true;     // an editor placement should reach everyone, not just nearby
    SDK::UGameplayStatics::FinishSpawningActor(actor, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

    // Replication has to be switched on through the engine, not by writing the bReplicates bit. UE decides
    // whether to register an actor with the net driver while SpawnActor runs -- before a deferred spawn
    // even returns -- using the class default. Most LE prefabs default to NOT replicated (they are placed
    // in maps, not spawned), so poking the bit afterwards left them known to the server and invisible to
    // every client: the first live test spawned a red coin that never reached the client that asked for
    // it. SetReplicates(true) registers the actor (and only acts on a false->true change, which is why the
    // bit is no longer pre-set). Movement replication is what carries later XFORMs to clients.
    // Prefabs whose blueprint graph calls into a LuauBlueprintComponent (the light switch, the teleporter)
    // depend on the sandbox's Luau runtime, which only binds for prefabs the sandbox engine spawned. Spawned
    // here they are broken, and interacting with one crashed players' games. Refuse them BEFORE they are
    // ever replicated: the actor exists only on the server for this instant.
    if (SeHasComponent(actor, "LuauBlueprintComponent") || SeHasComponent(actor, "ToggleableComponent"))
    {
        HxLog("[HalcyonA2][SPECEDIT] refused %s: its behaviour runs in the sandbox Luau runtime, which a server-spawned copy never gets\n",
              cls->GetName().c_str());
        actor->K2_DestroyActor();
        if (SDK::UObject* pc = SeCallerPC(pawn))
            SeBroadcast("SE|NOTE|" + cls->GetName() + " can't be placed: it needs the game's sandbox scripts, and would crash players who touch it.", pc);
        return;
    }
    SeReplicateTransform(actor);
    actor->ForceNetUpdate();
    SeMarkProxyDirty(actor);

    // Let the engine build the quaternion; hand-rolled rotator maths has already cost this project a
    // misplaced level once.
    actor->K2_SetActorRotation(SDK::FRotator{ rot[0], rot[1], rot[2] }, false);

    SeLvRecordPlain(actor, path);
    HxLog("[HalcyonA2][SPECEDIT] spawned %s -> %s at (%.0f,%.0f,%.0f) replicated\n",
          cls->GetName().c_str(), actor->GetName().c_str(), loc[0], loc[1], loc[2]);
}

// Make an actor's WHOLE transform reach clients that are already connected. Replicated movement carries
// location and rotation only; scale travels in the spawn data a client gets when it first sees the actor,
// which is why a scale edit showed up only after a rejoin. Replicating the root component adds its
// RelativeScale3D (USceneComponent replicates its relative transform), and that reaches every client,
// vanilla Quest included, with no client-side help. Idempotent: the engine ignores repeat calls.
static void SeReplicateTransform(SDK::AActor* a)
{
    if (!a) return;
    a->SetReplicates(true);
    a->SetReplicateMovement(true);
    if (a->RootComponent)
    {
        a->RootComponent->SetIsReplicated(true);
        // Station props (the yellow omni boost pads, boost tanks) have a STATIC root: SetActorLocation silently
        // refuses to move it, on the server and on every client applying the replicated move. Mobility is a
        // replicated property of the (now replicated) root, so Movable here reaches every client too.
        if (a->RootComponent->Mobility != SDK::EComponentMobility::Movable)
            a->RootComponent->SetMobility(SDK::EComponentMobility::Movable);
    }
}

// SE|AUDIT|<class path>: spawn the class privately (never replicated -- LE prefabs default to
// bReplicates=false), log every component it ends up with and its collision, destroy it. Used to sort the
// palette into prefabs that work when the server spawns them and ones whose behaviour lives in the
// sandbox's Luau runtime, which only binds for prefabs the sandbox engine itself spawned.
static void SeAudit(SDK::UObject* ctx, const std::string& path)
{
    SDK::UClass* cls = SeResolveEditorClass(path);
    if (!cls) return;
    SDK::FTransform xf{};
    xf.Rotation = SDK::FQuat{ 0, 0, 0, 1 };
    xf.Translation = SDK::FVector{ 0, 0, -200000 };
    xf.Scale3D = SDK::FVector{ 1, 1, 1 };
    SDK::AActor* a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        ctx, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!a) { HxLog("[HalcyonA2][SPECEDIT] AUDIT %s: spawn failed\n", cls->GetName().c_str()); return; }
    SDK::UGameplayStatics::FinishSpawningActor(a, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    auto* compCls = SDK::UObject::FindClassFast("ActorComponent");
    auto* primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
    std::string list;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
        if (!c || c->Outer != a || !compCls || !c->IsA(compCls)) continue;
        list += " " + (c->Class ? c->Class->GetName() : std::string("?"));
        if (primCls && c->IsA(primCls))
        {
            auto* pc = static_cast<SDK::UPrimitiveComponent*>(c);
            list += "(coll=" + std::to_string((int)pc->GetCollisionEnabled()) + ")";
        }
    }
    HxLog("[HalcyonA2][SPECEDIT] AUDIT %s:%s\n", cls->GetName().c_str(), list.c_str());
    a->K2_DestroyActor();
}

static bool SeActorAlive(SDK::AActor* a);   // below

// ---- collision for placed meshes ------------------------------------------------------------------
// LE_SM_* prefabs default to OverlapAll/QueryOnly: in the stock sandbox their Luau "Physical" component
// switches collision on, and that script only runs for prefabs the sandbox engine spawned itself. Collision
// settings are not replicated, so the server cannot switch it on in anyone else's copy -- but it CAN spawn a
// replicated stand-in every client builds for itself: a hidden stock StaticMeshActor with the same mesh
// (UStaticMeshComponent::StaticMesh is a replicated property) and BlockAll collision. Vanilla Quest clients
// then collide with the placed mesh like any other level geometry. Stand-ins are respawned (not moved) after
// a transform edit settles, because a client may build them with Static mobility, which ignores moves.
struct SeCollisionProxy { SDK::AActor* owner; std::vector<SDK::AActor*> proxies; ULONGLONG dirtyAt; };
static std::vector<SeCollisionProxy> g_seProxies;

// A script's own collision calls on a piece: PhysicalComponent setTriggerCollision() ("OverlapAll"),
// disableCollisionLua() ("NoCollision") and setDefaultCollision() ("BlockAll") only change the piece's own mesh --
// its solid stand-in (below) kept blocking, so a trigger zone or a "vanished" platform stayed a wall (measured
// 2026-09-25: the mock player stopped dead at the trigger cube's face). The server follows those calls: a stand-in
// is solid only while the script's last call was setDefaultCollision (or it never made one). Keyed by the actor,
// checked against its object id (a freed actor's pointer can come back as another object).
struct SeScriptCollision { std::string idx; bool off; };
static std::unordered_map<SDK::AActor*, SeScriptCollision> g_seScriptCollision;
static std::string SeObjIdx(SDK::AActor* a);   // below
static bool SeScriptCollisionOff(SDK::AActor* a)
{
    auto it = g_seScriptCollision.find(a);
    if (it == g_seScriptCollision.end()) return false;
    if (it->second.idx != SeObjIdx(a)) { g_seScriptCollision.erase(it); return false; }
    return it->second.off;
}
static void SeApplyScriptCollision(SDK::AActor* a)
{
    const bool off = SeScriptCollisionOff(a);
    for (auto& e : g_seProxies)
        if (e.owner == a)
            for (SDK::AActor* p : e.proxies)
                if (p) { p->SetActorEnableCollision(!off); p->ForceNetUpdate(); }
}

// Does `a` own a component of this class (by name)? One GObjects walk; only used on spawn/edit, not per tick.
static bool SeHasComponent(SDK::AActor* a, const char* cls)
{
    if (!a) return false;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && o->Outer == a && o->Class && o->Class->GetName() == cls) return true;
    }
    return false;
}

// Every prefab whose collision the sandbox's Luau "Physical" component would switch on (all LE_SM_* meshes,
// DefaultMeshObject, DiscGolfHole -- from the SE|AUDIT sweep of the whole catalogue).
static bool SeNeedsProxy(SDK::AActor* a)
{
    if (!a || !a->Class) return false;
    const std::string cn = a->Class->GetName();
    // Volumes (the gravity volume) are areas players pass THROUGH: their mesh is only the region's shape and is
    // query-only, and the game reacts to the overlap. A BlockAll stand-in turned them into a solid invisible box.
    if (cn.find("Volume") != std::string::npos) return false;
    return cn.rfind("LE_SM_", 0) == 0 || SeHasComponent(a, "PhysicalComponent");
}

static void SeDestroyProxies(SeCollisionProxy& e)
{
    for (SDK::AActor* p : e.proxies) if (SeActorAlive(p)) p->K2_DestroyActor();
    e.proxies.clear();
}

static int SeBuildProxies(SeCollisionProxy& e)
{
    SeDestroyProxies(e);
    SDK::AActor* a = e.owner;
    if (!SeActorAlive(a)) return 0;
    auto* smcCls = SDK::UStaticMeshComponent::StaticClass();
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->Outer != a || !o->IsA(smcCls)) continue;
        auto* smc = static_cast<SDK::UStaticMeshComponent*>(o);
        SDK::UStaticMesh* mesh = *reinterpret_cast<SDK::UStaticMesh**>(reinterpret_cast<uintptr_t>(smc) + 0x560);
        if (!mesh || smc->GetCollisionEnabled() == SDK::ECollisionEnabled::QueryAndPhysics) continue;
        const SDK::FTransform xf = smc->K2_GetComponentToWorld();
        SDK::AActor* p = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
            a, SDK::AStaticMeshActor::StaticClass(), xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr,
            SDK::ESpawnActorScaleMethod::OverrideRootScale);
        if (!p) continue;
        auto* sma = static_cast<SDK::AStaticMeshActor*>(p);
        SDK::UStaticMeshComponent* c = sma->StaticMeshComponent;
        if (c)
        {
            c->SetMobility(SDK::EComponentMobility::Movable);
            c->SetStaticMesh(mesh);
            c->SetCollisionProfileName(SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(L"BlockAll")), false);
            c->SetIsReplicated(true);
        }
        SDK::UGameplayStatics::FinishSpawningActor(p, xf, SDK::ESpawnActorScaleMethod::OverrideRootScale);
        p->SetActorHiddenInGame(true);
        p->SetReplicates(true);
        p->ForceNetUpdate();
        SeTrack(p);
        e.proxies.push_back(p);
    }
    if (SeScriptCollisionOff(a)) SeApplyScriptCollision(a);   // a rebuilt stand-in keeps what the script set
    return static_cast<int>(e.proxies.size());
}

// After a spawn or a transform edit: (re)build this actor's stand-ins once it has been still for 400 ms.
static void SeMarkProxyDirty(SDK::AActor* a)
{
    if (!SeNeedsProxy(a)) return;
    SeTrack(a);
    for (auto& e : g_seProxies) if (e.owner == a) { e.dirtyAt = GetTickCount64(); return; }
    g_seProxies.push_back({ a, {}, GetTickCount64() });
}

// Every sandbox spawn asks for stand-ins by object id -- the editor's own placements, saved levels loading,
// and the rebuild a move or a script change does (a new server actor each time). Only the editor's spawn
// used to ask, so meshes from a loaded level, or any mesh after a move, had no collision for anyone.
// The server's copy is looked up by id for a few seconds (throttled), then the normal 400 ms settle applies.
static std::vector<std::pair<std::string, ULONGLONG>> g_seProxyWant;
static void SeWantProxy(const std::string& idx) { if (!idx.empty()) g_seProxyWant.push_back({ idx, GetTickCount64() }); }
static void SeProxyWantTick(ULONGLONG now)
{
    // [PERF] ONE pass over the shared PrefabComponent index resolves every pending id -- never a GObjects walk.
    // (It used to call SbActorForIdx per pending id, each a full walk: a saved level loading 144 objects meant
    // 144 walks of ~167k objects in one tick, every 250 ms. The index is ~1 s behind, hence the longer wait.)
    static ULONGLONG s_last = 0;
    if (g_seProxyWant.empty() || now - s_last < 250) return;
    s_last = now;
    static SDK::UClass* pcCls = nullptr;
    if (!pcCls) pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    if (!pcCls) return;
    std::unordered_map<std::string, SDK::AActor*> live;
    for (SDK::UObject* o : ClassObjects(pcCls))
    {
        if (!o || o->IsDefaultObject() || !o->Outer) continue;
        auto* act = static_cast<SDK::AActor*>(o->Outer);
        if (*(reinterpret_cast<const uint8_t*>(act) + 0x65) & 0x01) continue;
        live[FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(o) + 0x248))] = act;
    }
    for (auto it = g_seProxyWant.begin(); it != g_seProxyWant.end();)
    {
        auto f = live.find(it->first);
        if (f != live.end()) { SeMarkProxyDirty(f->second); it = g_seProxyWant.erase(it); continue; }
        if (now - it->second > 15000)
        {
            HxLog("[HalcyonA2][SPECEDIT] collision: the server never built %s, so it has no stand-in\n", it->first.c_str());
            it = g_seProxyWant.erase(it);
            continue;
        }
        ++it;
    }
}

static void SeDropProxies(SDK::AActor* a)
{
    for (auto it = g_seProxies.begin(); it != g_seProxies.end(); ++it)
        if (it->owner == a) { SeDestroyProxies(*it); g_seProxies.erase(it); return; }
}

static void SeQuestTick();   // below

static void SeTextTick();   // below (text from scripts)
static void SbDeferredSetsTick();   // below (Game data writes that had to wait)
static std::unordered_map<std::string, ULONGLONG> g_sbSpawnAt;   // sandbox idx -> when its node was created (see SbDeferredSet)
static void SeLvTick();   // below (saved levels)
static void SeCamMarkersTick(ULONGLONG now);   // below (editor camera markers)
static void SpecEditTick()
{
    if (!g_specEdit) return;
    SeQuestTick();
    SeLvTick();
    SbDeferredSetsTick();
    SeTextTick();
    SeModeTick();
    SbMoveFlush();
    const ULONGLONG now = GetTickCount64();
    SeCamMarkersTick(now);
    SeProxyWantTick(now);
    SbEditorSlotTick(now);
    SbEditorSlotClearTick(now);
    if (g_seProxies.empty()) return;
    static ULONGLONG s_last = 0;
    if (now - s_last < 100) return;
    s_last = now;
    for (auto it = g_seProxies.begin(); it != g_seProxies.end();)
    {
        if (!SeActorAlive(it->owner)) { SeDestroyProxies(*it); it = g_seProxies.erase(it); continue; }
        if (it->dirtyAt && now - it->dirtyAt >= 400)
        {
            it->dirtyAt = 0;
            const int n = SeBuildProxies(*it);
            HxLog("[HalcyonA2][SPECEDIT] collision: %d stand-in(s) for %s\n", n, it->owner->GetName().c_str());
        }
        ++it;
    }
}
static void SafeSpecEditTick() { __try { SpecEditTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// "Class@x,y,z#<sandbox id>": moves and deletes carry the object's id as well as where the client sees it.
// Position alone picked the wrong object whenever two of the same class stood within 1 m -- a duplicate
// sits 1 cm from its original, and a group drag's client-side positions run ahead of the server's -- so a
// copy's move landed on the original (and the copy's next one found nothing). The id names the object
// outright; this turns it into the server's own exact ident for it, else falls back to the position.
static std::string SeResolveIdent(const std::string& name)
{
    const size_t h = name.find('#');
    if (h == std::string::npos) return name;
    const std::string base = name.substr(0, h), idx = name.substr(h + 1);
    if (idx.size() < 32) return base;
    SDK::AActor* a = SbActorForIdx(idx, true);   // index only: never a full walk per drag step
    if (!a || !a->Class || base.compare(0, base.find('@'), a->Class->GetName()) != 0) return base;
    const std::string own = SeLvIdent(a);
    return own.empty() ? base : own;
}
static void SeBallFollow(SDK::AActor* spawner, const double* at, const double* scl);   // (below, with the game modes)
static void SeBallScale(SDK::AActor* ball, const double* scl);
static void SeBallMatchClients(SDK::AActor* ball, SDK::AActor* spawner);
static bool SeAlive(SDK::UObject* o);
static SDK::AActor* SeBallAt(const std::string& ident);
static void SeTransform(const std::string& name, const std::string& locs, const std::string& rots,
                        const std::string& scls)
{
    // A ball is not a placed object -- its SPAWNER is (the ball is made at run time, and remade every round). So
    // editing a ball edits its spawner: the spawner goes to where the ball was put (the ball sits 1 m above it) and
    // takes its scale, and the ball follows. A ball whose spawner is not a sandbox object just moves.
    if (SDK::AActor* ball = SeBallAt(name))
    {
        auto* sp = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(ball) + 0x328);   // ADiscEntity::Spawner
        SDK::AActor* owner = sp && SeAlive(sp) && sp->Outer ? static_cast<SDK::AActor*>(sp->Outer) : nullptr;
        double l[3], sc[3];
        const SDK::FVector bl = ball->K2_GetActorLocation(), bs = ball->GetActorScale3D();
        if (!SeVec(locs, l)) { l[0] = bl.X; l[1] = bl.Y; l[2] = bl.Z; }
        if (!SeVec(scls, sc)) { sc[0] = bs.X; sc[1] = bs.Y; sc[2] = bs.Z; }
        if (owner && SbPrefabOf(owner))
        {
            // Any change to the spawner REBUILDS it (and it makes a fresh ball), so only touch it when the edit needs to:
            //  * moved   -> the spawner moves BY what the ball moved (a resting ball is ~18 cm up, not the 1 m over its
            //               spawner it was made at: "1 m under the ball" sank spawners into the floor);
            //  * scaled  -> every machine sizes its own copy of the ball at 1 / its spawner's scale (measured), so a
            //               ball of size s = its spawner at 1/s -- the game then sizes it the same everywhere;
            //  * rotated -> nothing: a ball's rotation is its physics' (it rolls), so there is nothing to keep, and a
            //               rebuild for it would only swap the ball for a new one.
            const SDK::FRotator cr = owner->K2_GetActorRotation();
            const SDK::FVector ol = owner->K2_GetActorLocation(), os = owner->GetActorScale3D();
            const double dx = l[0] - bl.X, dy = l[1] - bl.Y, dz = l[2] - bl.Z;
            const bool moved = dx * dx + dy * dy + dz * dz > 1.0;
            const double want = (sc[0] + sc[1] + sc[2]) / 3.0, have = (bs.X + bs.Y + bs.Z) / 3.0;
            const bool scaled = fabs(want - have) > 0.005;
            if (!moved && !scaled)
            {
                HxLog("[HalcyonA2][SPECEDIT] ball %s: rotation only -- nothing to change (a ball's rotation is its physics')\n",
                      ball->GetName().c_str());
                return;
            }
            double sl[3] = { ol.X + dx, ol.Y + dy, ol.Z + dz }, r[3] = { cr.Pitch, cr.Yaw, cr.Roll }, ss[3] = { os.X, os.Y, os.Z };
            if (!moved) { sl[0] = ol.X; sl[1] = ol.Y; sl[2] = ol.Z; }
            if (scaled)
            {
                const double s = (std::min)(4.0, (std::max)(0.25, want));
                ss[0] = ss[1] = ss[2] = 1.0 / s;
            }
            SeSandboxXform(owner, sl, r, ss);
            SeMarkProxyDirty(owner);
            SeBallFollow(owner, sl, ss);
            HxLog("[HalcyonA2][SPECEDIT] ball %s edited -> spawner %s%s%s (spawner scale %.2f)\n", ball->GetName().c_str(),
                  owner->GetName().c_str(), moved ? " moved" : "", scaled ? " resized" : "", ss[0]);
        }
        else
        {
            double sl[3] = { l[0], l[1], l[2] - 100.0 };   // ResetBallTo puts the ball 1 m over this: exactly at l
            if (owner) SeBallFollow(owner, sl, sc);
            else { SDK::FHitResult hit{}; ball->K2_SetActorLocation(SDK::FVector{ l[0], l[1], l[2] }, false, &hit, true); }
            HxLog("[HalcyonA2][SPECEDIT] ball %s moved (spawner %s is not a placed object: not saved)\n", ball->GetName().c_str(),
                  owner ? owner->GetName().c_str() : "none");
        }
        return;
    }
    SDK::AActor* a = SeFindEditorActor(name);
    if (!a) { SbOwnedXform(name, locs, rots, scls); return; }   // a node the server never built an actor for
    if (SbPrefabOf(a))                                   // a sandbox object: every machine owns its own copy
    {
        double l[3], r[3], sc[3];
        const SDK::FVector cl = a->K2_GetActorLocation(); const SDK::FRotator cr = a->K2_GetActorRotation();
        const SDK::FVector cs = a->GetActorScale3D();
        if (!SeVec(locs, l)) { l[0] = cl.X; l[1] = cl.Y; l[2] = cl.Z; }
        if (!SeVec(rots, r)) { r[0] = cr.Pitch; r[1] = cr.Yaw; r[2] = cr.Roll; }
        if (!SeVec(scls, sc)) { sc[0] = cs.X; sc[1] = cs.Y; sc[2] = cs.Z; }
        SeSandboxXform(a, l, r, sc);
        SeMarkProxyDirty(a);                             // its collision stand-in follows once it settles
        SeBallFollow(a, l, sc);                          // a ball spawner: its ball comes along (no-op otherwise)
        return;
    }
    SeReplicateTransform(a);
    double v[3]{};
    SDK::FHitResult hit{};   // K2_SetActorLocation writes the sweep result back through this pointer
    if (SeVec(locs, v)) a->K2_SetActorLocation(SDK::FVector{ v[0], v[1], v[2] }, false, &hit, false);
    if (SeVec(rots, v)) a->K2_SetActorRotation(SDK::FRotator{ v[0], v[1], v[2] }, false);
    if (SeVec(scls, v)) a->SetActorScale3D(SDK::FVector{ v[0], v[1], v[2] });
    a->ForceNetUpdate();
    SeMarkProxyDirty(a);
}

static void SeDelete(const std::string& name)
{
    SDK::AActor* a = SeFindEditorActor(name);
    if (!a) { SbOwnedDelete(name); return; }             // a node the server never built an actor for
    std::string idx;
    if (SDK::UObject* pc = SbPrefabOf(a)) idx = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248));
    if (SeSandboxDelete(a))                              // a sandbox object: remove its node, every machine follows
    {
        SeDropProxies(a);
        SbOwnedForget(idx);
        return;
    }
    if (SbOwnedDelete(name)) { SeDropProxies(a); return; }   // ours, but the actor path failed: remove the node
    SeDropProxies(a);
    a->K2_DestroyActor();
    HxLog("[HalcyonA2][SPECEDIT] destroyed %s\n", name.c_str());
}

// ---- authored quests -------------------------------------------------------------------------
// An authored quest becomes a real quest definition every player receives, in its own bundle.
//
// How the game models a parkour quest (from the 194-row bundle this server sends): the climbing
// subquests ("Subquest_PKR_Climber_Advanced_NN") are rows with a PKRClimbN glyph, one quest-type tag and
// a parent prerequisite, and each is completed by a progression button whose A2QuestProgressComponent
// carries that subquest's QuestID. So an authored quest is built the same way:
//
//   * its row is a CLONE of a live Subquest_PKR_Climber row (so tags, dates and every field the client
//     validates are genuine), with a new ID, the author's title / description / glyph / repetition, and
//     the prerequisite and child lists cleared so it is available at once;
//   * every step actor the author picked gets its A2QuestProgressComponent.QuestID (+0xC4) and
//     QuestName (+0xBC) pointed at it -- the same link the climbing buttons use;
//   * the rows go out in their OWN bundle ("SpecEditorQuests"), so they cannot collide with the
//     [PKRQUESTS] publisher's bundle, to every player at once and to anyone who joins later.
//
// The game derives a quest's state from its definition (the progression we register carries no State),
// which is why a well-formed row with no prerequisite is the whole trick -- no state is forced.
//
// FAAQuestEntry (0x120): ID@0x00, QuestType@0x10 (tags TArray @0x10, parents @0x20), VersionNumber@0x30,
// StartDate@0x38, EndDate@0x40, Repetition@0x48, ValidLengthSeconds@0x4C, PrerequisiteQuests@0x50..0xA0,
// ChildQuests@0xA0..0xE0, Runtime_Glyph@0xE0, OptionalRequiredProgress@0xE8, Title FString@0xF0,
// GlyphID FName@0x100, Description FString@0x108, bTestQuest@0x118.
// FAAQuestBundle (0x40): UniqueID FString@0x00, Dependencies@0x10, Table rows Data@0x20 / Num@0x28 / Max@0x2C.
struct SeAuthoredQuest
{
    uint32_t     id[4]{};
    std::string  questId;
    std::wstring title, desc;        // the row's FStrings point into these, so they must not move
    std::vector<uint64_t> tags, parentTags;
    uint8_t      row[0x120]{};
    int          steps = 0;
    std::string  glyph;              // kept so the row can be (re)built once a template exists
    int          rep = 0;
    int          validSec = 0;       // ValidLengthSeconds: how long an activation stays valid (0 = open-ended)
    float        reqProgress = 0.0f; // OptionalRequiredProgress: progress needed to complete (0 = row default)
    // Quest group: ChildQuests (+0xA0), 20-byte {FGuid, float Weight} each. The client computes the parent's
    // progress as the weighted average of its children and completes it when they are all done; each child
    // shows its own icon -- "one quest, several icons".
    std::vector<uint8_t> children;
    // A quest GROUP is sent as an FAAQuestFolder (bundle Table.Folders, 0x68 bytes), not as a quest row:
    // a parent row with ChildQuests broke its parts (the parent's progress is computed from the children
    // and never lined up). A folder only groups: it has its own title and icon, and lists its quests, each
    // keeping its own icon and completing on its own.
    bool         isFolder = false;
    uint8_t      folder[0x68]{};
    std::vector<uint32_t> subQuests;     // 4 per GUID
    std::string  level;                  // saved level that owns it ("" = editor work)
    std::string  childIds;               // group: the quest ids in it, ';'-separated
    // Deleted from the editor (SE|QDEL) or with its level. Kept, not erased: every row's FStrings point
    // into its quest's own strings, and erasing from the middle of the deque would move them.
    bool         deleted = false;

    // Checkpoint run, driven by the server (see SeQuestTick): reach each checkpoint in order.
    std::vector<SDK::AActor*> checkpoints;
    double       radius = 250.0;     // touch distance, cm
    int          timeLimit = 0;      // seconds from the first checkpoint; 0 = none
    struct Run { int next = 0; ULONGLONG startedAt = 0, doneAt = 0; };
    std::unordered_map<int32_t, Run> runs;   // by the player's pawn GObjects index
    std::vector<std::string> completedBy;    // player ids (org id, else pawn slot) done this session
};
static std::deque<SeAuthoredQuest> g_seAuthored;     // deque: element addresses stay stable
static uint8_t  g_seTemplate[0x120];
static bool     g_seHaveTemplate = false;
static std::vector<SDK::UObject*> g_seQuestComps;    // every player quest component quests went to
static std::vector<uint8_t> g_seRowsBuf;
static std::vector<uint8_t> g_seFoldersBuf;
static uint8_t  g_seBundle[0x40];
static const wchar_t kSeBundleId[] = L"SpecEditorQuests";

static void SetFString(uint8_t* at, const std::wstring& s)
{
    *reinterpret_cast<const wchar_t**>(at) = s.c_str();
    *reinterpret_cast<int32_t*>(at + 8)  = static_cast<int32_t>(s.size() + 1);
    *reinterpret_cast<int32_t*>(at + 12) = static_cast<int32_t>(s.size() + 1);
}
static void SetTArray(uint8_t* at, const void* data, int n)
{
    *reinterpret_cast<const void**>(at) = n ? data : nullptr;
    *reinterpret_cast<int32_t*>(at + 8)  = n;
    *reinterpret_cast<int32_t*>(at + 12) = n;
}
static bool SeAlive(SDK::UObject* o)
{
    return o && SDK::UObject::GObjects->GetByIndex(o->Index) == o;
}

// Remember a proven parkour row as the template, the first time the server sends a real bundle.
static void SeCaptureTemplate(void* bundle)
{
    if (g_seHaveTemplate || !bundle) return;
    const uint8_t* rows = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uint8_t*>(bundle) + 0x20);
    const int n = *reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(bundle) + 0x28);
    if (!rows || n <= 0 || n > 4096) return;
    for (int i = 0; i < n; ++i)
    {
        const uint8_t* r = rows + static_cast<size_t>(i) * 0x120;
        const wchar_t* t = *reinterpret_cast<const wchar_t* const*>(r + 0xF0);
        if (t && wcsncmp(t, L"Subquest_PKR_Climber", 20) == 0)
        {
            memcpy(g_seTemplate, r, 0x120);
            g_seHaveTemplate = true;
            HxLog("[HalcyonA2][SPECEDIT] quest template captured from row %d (%ls)\n", i, t);
            return;
        }
    }
}

// Fill an authored quest's row from the template. Called again whenever the template arrives late.
static bool SeBuildRow(SeAuthoredQuest& q, const std::string& glyph, int repetition)
{
    if (!g_seHaveTemplate) return false;
    memcpy(q.row, g_seTemplate, 0x120);
    memcpy(q.row + 0x00, q.id, 16);
    *reinterpret_cast<int32_t*>(q.row + 0x30) = 1;                         // VersionNumber
    q.row[0x48] = static_cast<uint8_t>(repetition & 3);                    // Once / Daily / Weekly / Monthly
    memset(q.row + 0x50, 0, 0xE8 - 0x50);        // no prerequisites, no children, no cached glyph texture
    *reinterpret_cast<int32_t*>(q.row + 0x4C) = q.validSec;               // ValidLengthSeconds
    *reinterpret_cast<float*>(q.row + 0xE8) = q.reqProgress;               // OptionalRequiredProgress
    q.row[0x118] = 0;                                                      // bTestQuest

    // Deep-copy the quest-type tags so the row does not borrow the template bundle's memory.
    auto copyTags = [](const uint8_t* src, std::vector<uint64_t>& dst) {
        const uint64_t* d = *reinterpret_cast<const uint64_t* const*>(src);
        const int n = *reinterpret_cast<const int32_t*>(src + 8);
        dst.assign(d && n > 0 && n < 64 ? d : nullptr, d && n > 0 && n < 64 ? d + n : nullptr);
    };
    copyTags(g_seTemplate + 0x10, q.tags);
    copyTags(g_seTemplate + 0x20, q.parentTags);
    SetTArray(q.row + 0x10, q.tags.data(), static_cast<int>(q.tags.size()));
    SetTArray(q.row + 0xA0, q.children.data(), static_cast<int>(q.children.size() / 20));
    SetTArray(q.row + 0x20, q.parentTags.data(), static_cast<int>(q.parentTags.size()));

    SetFString(q.row + 0xF0, q.title);
    SetFString(q.row + 0x108, q.desc);
    if (!glyph.empty())
    {
        const std::wstring g(glyph.begin(), glyph.end());
        const SDK::FName fn = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(g.c_str()));
        memcpy(q.row + 0x100, &fn, sizeof(fn));
    }
    return true;
}

// Send every authored quest to one player's quest component, as its own bundle.
static void SeSendAuthored(void* comp)
{
    if (!comp || g_seAuthored.empty() || !QSendSet_Orig) return;
    bool anyDeleted = false;
    for (const auto& q : g_seAuthored) anyDeleted |= q.deleted;
    g_seRowsBuf.assign(g_seAuthored.size() * 0x120, 0);
    int n = 0;
    g_seFoldersBuf.clear();
    int nf = 0;
    for (const auto& q : g_seAuthored)
    {
        if (q.deleted) continue;
        if (q.isFolder)
        {
            g_seFoldersBuf.insert(g_seFoldersBuf.end(), q.folder, q.folder + sizeof(q.folder));
            ++nf;
            continue;
        }
        if (!*reinterpret_cast<const void* const*>(q.row + 0xF0)) continue;   // row not built yet
        memcpy(g_seRowsBuf.data() + static_cast<size_t>(n) * 0x120, q.row, 0x120);
        ++n;
    }
    if (!n && !nf && !anyDeleted) return;   // (all deleted: an empty bundle clears the player's list)
    memset(g_seBundle, 0, sizeof(g_seBundle));
    *reinterpret_cast<const wchar_t**>(g_seBundle) = kSeBundleId;
    *reinterpret_cast<int32_t*>(g_seBundle + 8)  = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    *reinterpret_cast<int32_t*>(g_seBundle + 12) = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    SetTArray(g_seBundle + 0x20, g_seRowsBuf.data(), n);
    SetTArray(g_seBundle + 0x30, g_seFoldersBuf.data(), nf);
    __try { QSendSet_Orig(comp, g_seBundle, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][SPECEDIT] quest send FAULTED\n"); return; }
    HxLog("[HalcyonA2][SPECEDIT] sent %d authored quest(s) to comp=%p\n", n, comp);
}

// Register the authored quests with the SERVER's own quest table, through the game's
// UA2PlayerQuestComponent::Server_SetQuests_Impl(comp, bundle, bIsRemoving) (0x46902F0 -- the path a client's
// bundle takes). Sending rows to players alone isn't enough: Server_SetProgress ignores a quest id the server
// doesn't know, so completions were never stored and the quest never turned green. On every change the
// previous registration is removed first (same bundle id), then the current one added. Buffers are kept
// alive for good: the server may keep pointers into them.
struct SeSrvBundle { std::vector<uint8_t> rows, folders; uint8_t bundle[0x40]{}; };
static std::deque<SeSrvBundle> g_seSrvBundles;
static SeSrvBundle* g_seSrvLast = nullptr;
static bool g_seRegistering = false;
static void SeCallSetQuests(void* comp, void* bundle, unsigned char removing)
{
    __try { reinterpret_cast<__int64(__fastcall*)(void*, void*, unsigned char)>(GetBase() + 0x46902F0)(comp, bundle, removing); }
    __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][SPECEDIT] server quest registration FAULTED\n"); }
}
static void SeRegisterOnServer()
{
    void* comp = nullptr;
    for (auto* c : g_seQuestComps) if (SeAlive(c)) { comp = c; break; }
    if (!comp || g_seRegistering) return;                  // no player yet: done when the first one joins
    g_seSrvBundles.emplace_back();
    SeSrvBundle& b = g_seSrvBundles.back();
    int n = 0, nf = 0;
    for (const auto& q : g_seAuthored)
    {
        if (q.deleted) continue;
        if (q.isFolder) { b.folders.insert(b.folders.end(), q.folder, q.folder + sizeof(q.folder)); ++nf; continue; }
        if (!*reinterpret_cast<const void* const*>(q.row + 0xF0)) continue;
        b.rows.insert(b.rows.end(), q.row, q.row + 0x120);
        ++n;
    }
    *reinterpret_cast<const wchar_t**>(b.bundle) = kSeBundleId;
    *reinterpret_cast<int32_t*>(b.bundle + 8)  = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    *reinterpret_cast<int32_t*>(b.bundle + 12) = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    SetTArray(b.bundle + 0x20, b.rows.data(), n);
    SetTArray(b.bundle + 0x30, b.folders.data(), nf);
    g_seRegistering = true;
    if (g_seSrvLast) SeCallSetQuests(comp, g_seSrvLast->bundle, 1);
    if (n || nf) SeCallSetQuests(comp, b.bundle, 0);
    g_seRegistering = false;
    g_seSrvLast = &b;
    HxLog("[HalcyonA2][SPECEDIT] registered %d authored quest(s) + %d group(s) with the server's quest table\n", n, nf);
}

// Every quest the game has sent any player (id, title, icon): the editor is a spectator and never gets a
// bundle of its own, so its quest pickers were empty -- it asks the server for this list (SE|QLIST).
struct SeCatQuest { std::string id, title, glyph; };
static std::vector<SeCatQuest> g_seCatalogue;
static void SeCatalogueAdd(void* bundle)
{
    if (!bundle) return;
    if (const wchar_t* uid = *reinterpret_cast<const wchar_t* const*>(bundle))
        if (wcscmp(uid, kSeBundleId) == 0) return;          // ours (sent or registered): not a station quest
    const uint8_t* rows = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uint8_t*>(bundle) + 0x20);
    const int n = *reinterpret_cast<const int32_t*>(reinterpret_cast<uint8_t*>(bundle) + 0x28);
    if (!rows || n <= 0 || n > 4096) return;
    for (int i = 0; i < n; ++i)
    {
        const uint8_t* r = rows + static_cast<size_t>(i) * 0x120;
        const uint32_t* g = reinterpret_cast<const uint32_t*>(r);
        char hx[40];
        snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", g[0], g[1], g[2], g[3]);
        bool known = false;
        for (const auto& q : g_seCatalogue) if (q.id == hx) { known = true; break; }
        if (known || g_seCatalogue.size() >= 4096) continue;
        const wchar_t* t = *reinterpret_cast<const wchar_t* const*>(r + 0xF0);
        std::string title;
        for (int k = 0; t && t[k] && k < 80; ++k) title.push_back(t[k] < 128 && t[k] != '|' ? static_cast<char>(t[k]) : '?');
        g_seCatalogue.push_back({ hx, title, reinterpret_cast<const SDK::FName*>(r + 0x100)->ToString() });
    }
}
static void SeCatalogueCore(void* bundle) { __try { SeCatalogueAdd(bundle); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SE|QLIST -> SE|QLIST|<id>\x1F<title>\x1F<glyph>\x1E... to the caller (the station's quests + ours).
static void SeQuestList(SDK::UObject* ctx)
{
    SDK::UObject* caller = SeCallerPC(ctx);
    if (!caller) return;
    std::string out;
    auto add = [&](const std::string& id, const std::string& title, const std::string& glyph) {
        if (!out.empty()) out += '\x1E';
        out += id + '\x1F' + title + '\x1F' + glyph;
    };
    for (const auto& q : g_seAuthored)
    {
        if (q.deleted) continue;
        char hx[40];
        snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", q.id[0], q.id[1], q.id[2], q.id[3]);
        add(hx, std::string(q.title.begin(), q.title.end()) + " (editor)", q.glyph);
    }
    for (const auto& q : g_seCatalogue) add(q.id, q.title, q.glyph);
    SeBroadcast("SE|QLIST|" + out, caller);
    HxLog("[HalcyonA2][SPECEDIT] QLIST: %zu station + %zu editor quest(s) sent\n", g_seCatalogue.size(), g_seAuthored.size());
}

// Called from QSendSet_Hook each time the game sends a player their real quests: remember the
// component, capture a template, and follow up with the authored bundle.
static void SeOnQuestsSent(void* comp, void* bundle)
{
    if (!comp) return;
    SeCatalogueCore(bundle);
    if (!g_seHaveTemplate)
    {
        SeCaptureTemplate(bundle);
        // Quests authored before any player had received quests had no template to clone; build them now.
        if (g_seHaveTemplate) for (auto& q : g_seAuthored) SeBuildRow(q, q.glyph, q.rep);
    }
    // The game sends each joining player several bundles in a row; follow only the first with ours.
    // (Mid-session publishes go to every known component anyway, from SeQuest.)
    auto* c = static_cast<SDK::UObject*>(comp);
    if (std::find(g_seQuestComps.begin(), g_seQuestComps.end(), c) != g_seQuestComps.end()) return;
    if (g_seQuestComps.size() < 256) g_seQuestComps.push_back(c);
    SeSendAuthored(comp);
    if (!g_seSrvLast && !g_seAuthored.empty()) SeRegisterOnServer();
}

// Point every A2QuestProgressComponent on `actor` at the authored quest.
static int SeBindStep(SDK::AActor* actor, const SeAuthoredQuest& q)
{
    static SDK::UClass* qpc = nullptr;
    if (!qpc) qpc = SDK::UObject::FindClassFast("A2QuestProgressComponent");
    if (!qpc || !actor) return 0;
    const std::wstring wid(q.questId.begin(), q.questId.end());
    const SDK::FName qname = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(wid.c_str()));
    int bound = 0;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->Outer != actor || !o->IsA(qpc)) continue;
        memcpy(reinterpret_cast<uint8_t*>(o) + 0xBC, &qname, sizeof(qname));   // QuestName
        memcpy(reinterpret_cast<uint8_t*>(o) + 0xC4, q.id, 16);                 // QuestID
        ++bound;
    }
    return bound;
}

// SE|QUEST|<questId>|<title>|<glyph>|<repetition>|<ident:kind;ident:kind...>
// The GUID comes from the quest id ALONE, so renaming or re-publishing the same quest updates it in
// place instead of minting a new quest and orphaning everyone's progress on the old one.
static void SeQuestGuid(const std::string& questId, uint32_t id[4])
{
    uint64_t h = 1469598103934665603ULL, h2 = 0x9E3779B97F4A7C15ULL;
    for (char c : questId) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
    for (char c : questId) { h2 ^= static_cast<unsigned char>(c); h2 *= 0x100000001B3ULL; h2 ^= h2 >> 29; }
    id[0] = static_cast<uint32_t>(h); id[1] = static_cast<uint32_t>(h >> 32);
    id[2] = static_cast<uint32_t>(h2); id[3] = static_cast<uint32_t>(h2 >> 32) | 1u;
}

static void SeQuest(const std::string& questId, const std::string& title, const std::string& glyph,
                    int repetition, const std::string& steps, int validSec = 0, float reqProgress = 0.0f,
                    const std::string& description = std::string(), double radius = 250.0, int timeLimit = 0,
                    const std::string& children = std::string())
{
    uint32_t id[4];
    SeQuestGuid(questId, id);

    SeAuthoredQuest* q = nullptr;
    for (auto& e : g_seAuthored) if (e.questId == questId) { q = &e; break; }
    if (!q) { g_seAuthored.emplace_back(); q = &g_seAuthored.back(); }
    memcpy(q->id, id, sizeof(id));
    q->questId = questId;
    q->deleted = false;
    q->title.assign(title.begin(), title.end());
    q->desc = description.empty() ? q->title : std::wstring(description.begin(), description.end());
    q->glyph = glyph;
    q->rep = repetition;
    q->validSec = validSec < 0 ? 0 : validSec;
    q->reqProgress = reqProgress < 0 ? 0.0f : reqProgress;
    if (!g_lvLoading.empty()) q->level = g_lvLoading;
    q->childIds = children;
    q->children.clear();
    q->subQuests.clear();
    q->isFolder = !children.empty();
    for (const auto& c : SeSplit(children, ';', 32))
    {
        if (c.empty() || c == questId) continue;
        bool sent = false;                              // only list children players actually receive
        for (const auto& e : g_seAuthored) if (e.questId == c && !e.deleted) { sent = true; break; }
        if (!sent) continue;
        uint32_t cg[4];
        SeQuestGuid(c, cg);
        q->subQuests.insert(q->subQuests.end(), cg, cg + 4);
    }
    if (q->isFolder)
    {
        // FAAQuestFolder: ID +0, ParentQuest +0x10, Title +0x20, GlyphID +0x30, Description +0x38,
        // SubQuests TArray<FGuid> +0x48, SubFolders +0x58.
        memset(q->folder, 0, sizeof(q->folder));
        memcpy(q->folder, id, 16);
        SetFString(q->folder + 0x20, q->title);
        if (!glyph.empty())
        {
            const std::wstring g(glyph.begin(), glyph.end());
            const SDK::FName fn = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(g.c_str()));
            memcpy(q->folder + 0x30, &fn, sizeof(fn));
        }
        SetFString(q->folder + 0x38, q->desc);
        SetTArray(q->folder + 0x48, q->subQuests.data(), static_cast<int>(q->subQuests.size() / 4));
    }

    const bool built = SeBuildRow(*q, glyph, repetition);

    int bound = 0, missing = 0;
    q->steps = 0;
    q->checkpoints.clear();
    q->runs.clear();                                     // an edited quest starts everyone afresh
    q->completedBy.clear();
    q->radius = (radius >= 50.0 && radius <= 5000.0) ? radius : 250.0;
    q->timeLimit = (timeLimit > 0 && timeLimit <= 3600) ? timeLimit : 0;
    for (const auto& s : SeSplit(steps, ';', 64))
    {
        if (s.empty()) continue;
        ++q->steps;
        SDK::AActor* a = SeFindEditorActor(s.substr(0, s.find(':')));
        if (a) { bound += SeBindStep(a, *q); SeTrack(a); q->checkpoints.push_back(a); }
        else ++missing;
    }

    // Everyone gets it now, not just players who join later.
    int sent = 0;
    for (auto* c : g_seQuestComps)
        if (SeAlive(c)) { SeSendAuthored(c); ++sent; }
    SeRegisterOnServer();                                // so completions are stored (and it turns green)

    HxLog("[HalcyonA2][SPECEDIT] quest %s '%s' id=%08X%08X%08X%08X: row %s, %d checkpoint(s) (%d not found), radius %.0fcm, "
          "time limit %ds, %zu child quest(s), pushed to %d player(s)\n",
          questId.c_str(), title.c_str(), id[0], id[1], id[2], id[3],
          built ? "built" : "WAITING FOR TEMPLATE (no player has received quests yet)",
          static_cast<int>(q->checkpoints.size()), missing, q->radius, q->timeLimit, q->children.size() / 20, sent);
    (void)bound;
}

// Take a quest out: it stops being sent and tracked, its checkpoints stop binding, and any group listing
// it drops it. The caller re-sends the bundle (SeQuestResendAll).
static void SeQuestRemove(SeAuthoredQuest& q)
{
    q.deleted = true;
    q.checkpoints.clear();
    q.runs.clear();
    q.completedBy.clear();
    for (auto& f : g_seAuthored)
    {
        if (!f.isFolder || f.deleted) continue;
        for (size_t i = 0; i + 4 <= f.subQuests.size();)
            if (memcmp(&f.subQuests[i], q.id, 16) == 0) f.subQuests.erase(f.subQuests.begin() + i, f.subQuests.begin() + i + 4);
            else i += 4;
        SetTArray(f.folder + 0x48, f.subQuests.data(), static_cast<int>(f.subQuests.size() / 4));
    }
}
// Client_SetQuests(bundle, bRemoving=true) with the deleted rows: the game's own "take these out" path
// (seen on the wire as removing=1). The remaining quests are re-sent afterwards either way.
static void SeSendRemoval(void* comp, const std::vector<const SeAuthoredQuest*>& gone)
{
    if (!comp || gone.empty() || !QSendSet_Orig) return;
    static std::vector<uint8_t> rows, folders;
    static uint8_t bundle[0x40];
    rows.clear(); folders.clear();
    int n = 0, nf = 0;
    for (const SeAuthoredQuest* q : gone)
    {
        if (q->isFolder) { folders.insert(folders.end(), q->folder, q->folder + sizeof(q->folder)); ++nf; continue; }
        if (!*reinterpret_cast<const void* const*>(q->row + 0xF0)) continue;
        rows.insert(rows.end(), q->row, q->row + 0x120); ++n;
    }
    if (!n && !nf) return;
    memset(bundle, 0, sizeof(bundle));
    *reinterpret_cast<const wchar_t**>(bundle) = kSeBundleId;
    *reinterpret_cast<int32_t*>(bundle + 8)  = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    *reinterpret_cast<int32_t*>(bundle + 12) = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    SetTArray(bundle + 0x20, rows.data(), n);
    SetTArray(bundle + 0x30, folders.data(), nf);
    __try { QSendSet_Orig(comp, bundle, 1); }
    __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][SPECEDIT] quest removal FAULTED\n"); }
}
static int SeQuestResendAll(const std::vector<const SeAuthoredQuest*>& gone = {})
{
    int sent = 0;
    for (auto* c : g_seQuestComps)
        if (SeAlive(c)) { SeSendRemoval(c, gone); SeSendAuthored(c); ++sent; }
    SeRegisterOnServer();
    return sent;
}
static void SeCoinRunRemove(const std::string& questRef);   // below

// SE|QDEL|<questId>: the editor deleted a quest it published.
static void SeQuestDelete(SDK::UObject* ctx, const std::string& questId)
{
    SDK::UObject* caller = SeCallerPC(ctx);
    SeAuthoredQuest* q = nullptr;
    for (auto& e : g_seAuthored)
    {
        if (e.deleted) continue;
        char hx[40];
        snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", e.id[0], e.id[1], e.id[2], e.id[3]);
        if (e.questId == questId || _stricmp(hx, questId.c_str()) == 0) { q = &e; break; }   // by id or GUID
    }
    SeCoinRunRemove(q ? q->questId : questId);
    if (!q)
    {
        HxLog("[HalcyonA2][SPECEDIT] QDEL %s: not a published quest (coin run only, if any)\n", questId.c_str());
        if (caller) SeBroadcast("SE|QLIST", caller);
        return;
    }
    SeQuestRemove(*q);
    const int sent = SeQuestResendAll({ q });
    HxLog("[HalcyonA2][SPECEDIT] QDEL %s: removed, re-sent to %d player(s)\n", questId.c_str(), sent);
    if (caller) SeQuestList(ctx);
}

// ---- checkpoint runs (server-driven, so they work for every player, Quest included) -----------------
// Five times a second: every player pawn's position (the server's working copy of its root,
// AVRPawn.Entity@0x928 -> +0x100, the same cheap read the Deathrun finish detector uses) against each
// published quest's next checkpoint. Reaching the last one completes the quest THROUGH THE GAME'S OWN PATH:
// A player's stored progress: UA2PlayerQuestComponent.QuestProgression (FA2QuestProgression, Net) at comp+0x120 =
// { FDateTime LastUpdate; TArray<FA2QuestStorage> Quests @+0x128 (count @+0x130); int32 SavedVersion @+0x138 }.
// FA2QuestStorage (0x20): FGuid ID, uint8 Progress @+0x10, int32 CompletedVersion @+0x14, FDateTime @+0x18.
// A quest shows green once CompletedVersion == the row's VersionNumber (1). Returns the array count (-1 on fault).
static int SeReadStoredQuest(void* qc, const uint32_t id[4], int* progress, int* completedVersion)
{
    __try
    {
        const uint8_t* data = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uintptr_t>(qc) + 0x128);
        const int n = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(qc) + 0x130);
        for (int i = 0; data && i < n && i < 4096; ++i)
        {
            const uint8_t* e = data + static_cast<size_t>(i) * 0x20;
            if (memcmp(e, id, 16) != 0) continue;
            *progress = e[0x10];
            *completedVersion = *reinterpret_cast<const int32_t*>(e + 0x14);
            break;
        }
        return n;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Store "completed" in the player's progression through the game's own register worker (sub_468BE60 --
// SafeRegisterQuest, the call that loads each player's saved quests on join and that the client picks up
// through the replicated QuestProgression). Server_SetProgress alone stored nothing for editor quests.
// The progression passed in = the player's current Quests + this quest {Progress 255, CompletedVersion 1}.
static int SeCommitCompleted(void* qc, const uint32_t id[4])
{
    static SDK::UClass* spCls = nullptr;
    if (!spCls) spCls = SDK::UObject::FindClassFast("ServerProgression");
    static SDK::UObject* sp = nullptr;
    if (!sp || !SeAlive(sp))
    {
        sp = nullptr;
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; spCls && i < n && !sp; ++i)
        {
            SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
            if (o && !o->IsDefaultObject() && o->IsA(spCls)) sp = o;
        }
    }
    if (!sp || !qc) return -1;
    static std::vector<uint8_t> entries;                  // the register deep-copies; reused
    static uint8_t prog[0x20];
    entries.clear();
    const uint8_t* cur = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uintptr_t>(qc) + 0x128);
    const int n = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(qc) + 0x130);
    FILETIME ft; GetSystemTimeAsFileTime(&ft);
    const int64_t nowTicks = static_cast<int64_t>((static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) + 504911232000000000LL;
    bool found = false;
    for (int i = 0; cur && i < n && i < 4096; ++i)
    {
        uint8_t e[0x20];
        memcpy(e, cur + static_cast<size_t>(i) * 0x20, 0x20);
        if (memcmp(e, id, 16) == 0)
        {
            e[0x10] = 0xFF;
            *reinterpret_cast<int32_t*>(e + 0x14) = 1;
            *reinterpret_cast<int64_t*>(e + 0x18) = nowTicks;
            found = true;
        }
        entries.insert(entries.end(), e, e + 0x20);
    }
    if (!found)
    {
        uint8_t e[0x20] = {};
        memcpy(e, id, 16);
        e[0x10] = 0xFF;
        *reinterpret_cast<int32_t*>(e + 0x14) = 1;             // == the row's VersionNumber: shows as done (green)
        *reinterpret_cast<int64_t*>(e + 0x18) = nowTicks;
        entries.insert(entries.end(), e, e + 0x20);
    }
    memset(prog, 0, sizeof(prog));
    *reinterpret_cast<int64_t*>(prog + 0x00) = nowTicks;                          // LastUpdate
    *reinterpret_cast<void**>(prog + 0x08) = entries.data();                       // Quests
    *reinterpret_cast<int32_t*>(prog + 0x10) = static_cast<int32_t>(entries.size() / 0x20);
    *reinterpret_cast<int32_t*>(prog + 0x14) = static_cast<int32_t>(entries.size() / 0x20);
    *reinterpret_cast<int32_t*>(prog + 0x18) = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(qc) + 0x138);   // saved version
    SafeRegisterQuest(sp, qc, prog);
    return static_cast<int>(entries.size() / 0x20);
}

// Complete an authored quest for one player (the pawn). See the notes inside for why it takes two calls.
static void SeGrantQuest(SDK::UObject* o, const SeAuthoredQuest& q)
{
    char guid[40];
    snprintf(guid, sizeof(guid), "%08X%08X%08X%08X", q.id[0], q.id[1], q.id[2], q.id[3]);
    const std::wstring w(guid, guid + 32);
    static_cast<SDK::AVRPawn*>(o)->Client_SetQuestCompleted(SDK::FString(w.c_str()));
    // That RPC only sets the client's progress byte; a quest shows as done (green) only once its
    // stored CompletedVersion matches, which the game writes when progress is committed through
    // the player's quest component. Commit it server-side too, so it turns green and is saved.
    if (auto* qc = *reinterpret_cast<SDK::UA2PlayerQuestComponent**>(reinterpret_cast<uintptr_t>(o) + 0x1E70))
    {
        SDK::FGuid gid{};
        memcpy(&gid, q.id, sizeof(gid));
        int prog0 = -1, cver0 = -1;
        const int before = SeReadStoredQuest(qc, q.id, &prog0, &cver0);
        qc->Server_SetProgress(gid, 0xFF);
        if (prog0 != 0xFF || cver0 != 1) SeCommitCompleted(qc, q.id);   // store it the way saved progress is loaded
        int prog = -1, cver = -1;
        const int stored = SeReadStoredQuest(qc, q.id, &prog, &cver);
        HxLog("[HalcyonA2][SPECEDIT] quest %s: Server_SetProgress(255) for %s -> stored quests %d -> %d; this one: progress=%d completedVersion=%d\n",
              q.questId.c_str(), o->GetName().c_str(), before, stored, prog, cver);
    }
}

// SE|QCOMPLETE|<questId> (local test): complete it for every connected player, as the checkpoint tracker does.
static void SeTestCompleteQuest(const std::string& questId)
{
    if (!g_seLocalTest) return;
    SeAuthoredQuest* q = nullptr;
    for (auto& e : g_seAuthored) if (e.questId == questId && !e.deleted) q = &e;
    if (!q) { HxLog("[HalcyonA2][SPECEDIT] QCOMPLETE %s: no such quest\n", questId.c_str()); return; }
    static SDK::UClass* pawnCls = nullptr;
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
    int seen = 0, granted = 0;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; pawnCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pawnCls)) continue;
        ++seen;
        if (!*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1E70)) continue;  // no quest component
        SeGrantQuest(o, *q);
        ++granted;
    }
    HxLog("[HalcyonA2][SPECEDIT] QCOMPLETE %s: %d pawn(s), granted to %d\n", questId.c_str(), seen, granted);
}

// AVRPawn::Client_SetQuestCompleted(FString) -- the RPC the sandbox's SetQuestCompletedForAllPlayers uses.
// Its client side (RVA 0x54E3740 -> 0x468B470, disassembled 2026-09-21) parses the string with FGuid::Parse
// and calls the player's quest component with progress 0xFF, i.e. complete. Pawns are re-resolved by
// GObjects index every pass and nothing is called on one that is not live (see DetectRunnerAtFinish's crash
// history for why).
static void SeQuestTick()
{
    if (g_seAuthored.empty()) return;
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_last < 200) return;
    s_last = now;

    bool any = false;
    for (auto& q : g_seAuthored) if (!q.checkpoints.empty()) { any = true; break; }
    if (!any) return;

    static SDK::UClass* pawnCls = nullptr;
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
    if (!pawnCls) return;

    for (const ObjIdxEntry& e : ClassObjectEntries(pawnCls))
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(e.idx);
        if (!o || o != e.obj || o->IsDefaultObject()) continue;
        const uintptr_t pw = reinterpret_cast<uintptr_t>(o);
        if (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01) continue;             // being destroyed
        if (!*reinterpret_cast<void**>(pw + 0x2D0)) continue;                            // no controller
        void* entity = *reinterpret_cast<void**>(pw + 0x928);
        if (!entity) continue;
        const double* pos = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(entity) + 0x100);
        if (pos[0] == 0.0 && pos[1] == 0.0 && pos[2] == 0.0) continue;                   // no pose yet
        // Who this is, for "once per player": the org id the server recorded on the controller
        // (AVRPlayerController+0xA30, as SeAuthorised reads it), else the pawn's slot.
        void* ctrl = *reinterpret_cast<void**>(pw + 0x2D0);
        std::string who = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(ctrl) + 0xA30));
        if (who.empty()) who = "pawn:" + std::to_string(e.idx);

        for (auto& q : g_seAuthored)
        {
            if (q.deleted || q.checkpoints.empty()) continue;
            if (std::find(q.completedBy.begin(), q.completedBy.end(), who) != q.completedBy.end()) continue;
            auto& run = q.runs[e.idx];
            if (run.doneAt && now - run.doneAt < 15000) continue;                        // just finished
            if (q.timeLimit && run.next > 0 && now - run.startedAt > static_cast<ULONGLONG>(q.timeLimit) * 1000)
            {
                HxLog("[HalcyonA2][SPECEDIT] quest %s: %s ran out of time at checkpoint %d/%zu\n",
                      q.questId.c_str(), o->GetName().c_str(), run.next, q.checkpoints.size());
                run.next = 0;
            }
            SDK::AActor* cp = q.checkpoints[run.next];
            if (!SeActorAlive(cp)) continue;
            void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(cp) + 0x1A8);
            if (!root) continue;
            const double* t = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20);
            const double dx = pos[0] - t[0], dy = pos[1] - t[1], dz = pos[2] - t[2];
            if (dx * dx + dy * dy + dz * dz > q.radius * q.radius) continue;

            if (run.next == 0) run.startedAt = now;
            ++run.next;
            HxLog("[HalcyonA2][SPECEDIT] quest %s: %s reached checkpoint %d/%zu\n",
                  q.questId.c_str(), o->GetName().c_str(), run.next, q.checkpoints.size());
            if (run.next < static_cast<int>(q.checkpoints.size())) continue;

            SeGrantQuest(o, q);
            char guid[40];
            snprintf(guid, sizeof(guid), "%08X%08X%08X%08X", q.id[0], q.id[1], q.id[2], q.id[3]);
            run.next = 0;
            run.doneAt = now;
            q.completedBy.push_back(who);
            HxLog("[HalcyonA2][SPECEDIT] quest %s COMPLETED by %s (%.1fs) -> Client_SetQuestCompleted(%s)\n",
                  q.questId.c_str(), o->GetName().c_str(), (now - run.startedAt) / 1000.0, guid);
            SeBroadcast("SE|NOTE|Quest '" + std::string(q.title.begin(), q.title.end()) + "' completed by a player", nullptr);
        }
    }
}

// ---- local test only: read-only look at the sandbox's own object system -------------------------------
// SE|SANDBOX. Logs USandboxEngine's prefab definitions (Blueprint class -> Settings.UniqueID, the key a
// sandbox object node names its prefabType by), the loaded gamemode slots, how many sandbox objects exist,
// and whether the replicated NetVar path loadedGamemodes/<SlotID>/objects resolves for each slot. Nothing
// is written. See memory a2-22284-sandbox-object-netvars.
static void SeSandboxProbe()
{
    if (!g_seLocalTest) return;
    auto* sbCls = SDK::UObject::FindClassFast("SandboxEngine");
    SDK::UObject* sb = nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; sbCls && i < n && !sb; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(sbCls)) sb = o;
    }
    if (!sb) { HxLog("[HalcyonA2][SPECEDIT] SANDBOX: no SandboxEngine\n"); return; }
    const uintptr_t b = reinterpret_cast<uintptr_t>(sb);
    const int raw = *reinterpret_cast<int32_t*>(b + 0xF8 + 8);
    const int types = *reinterpret_cast<int32_t*>(b + 0x108 + 8);           // TMap: TSet elements Num (approx)
    const int lgms = *reinterpret_cast<int32_t*>(b + 0x180 + 8);
    const int objs = *reinterpret_cast<int32_t*>(b + 0x2A0 + 8);
    HxLog("[HalcyonA2][SPECEDIT] SANDBOX: RawPrefabs=%d prefabTypes~%d loadedGamemodes=%d objectMap~%d IsAuthority=%d\n",
          raw, types, lgms, objs, *reinterpret_cast<uint8_t*>(b + 0x300));
    SDK::UObject** rp = *reinterpret_cast<SDK::UObject***>(b + 0xF8);
    for (int i = 0; rp && i < raw && i < 400; ++i)
    {
        SDK::UObject* def = rp[i];
        if (!def) continue;
        SDK::UObject* bp = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x30);
        SDK::UObject* st = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x38);
        const std::string uid = st ? reinterpret_cast<SDK::FName*>(reinterpret_cast<uintptr_t>(st) + 0xF0)->ToString() : "-";
        const std::string bpn = bp ? bp->GetName() : "-";
        if (bpn.rfind("LE_", 0) == 0 || i < 5)
            HxLog("[HalcyonA2][SPECEDIT] SANDBOX   prefab %-40s UniqueID=%s\n", bpn.c_str(), uid.c_str());
    }
    SDK::UObject** lg = *reinterpret_cast<SDK::UObject***>(b + 0x180);
    void* root = NvWorldRoot();
    for (int i = 0; lg && i < lgms && i < 64; ++i)
    {
        SDK::UObject* l = lg[i];
        if (!l) continue;
        SDK::FString slot = static_cast<SDK::ULoadedGameMode*>(l)->GetSlotID();
        const std::string slotA = slot.ToString();
        int found = -9;
        if (root)
        {
            const uint64_t segs[3] = { NvNameBits(NvName("loadedGamemodes")), NvNameBits(NvName(slotA)), NvNameBits(NvName("objects")) };
            NvWalk w{}; void* parent = nullptr; int failAt = -1;
            found = NvWorldWalk(root, segs, 3, &w, &parent, &failAt) ? 1 : -failAt - 1;
            NvWalkRelease(&w);
        }
        HxLog("[HalcyonA2][SPECEDIT] SANDBOX   gamemode[%d] %s slot='%s' objectsPath=%s\n", i, l->GetName().c_str(), slotA.c_str(),
              found == 1 ? "FOUND" : found == -9 ? "no world root" : ("missing at segment " + std::to_string(-found - 1)).c_str());
    }
}

// ---- local test only: PROTOTYPE -- place an object through the sandbox's own object system -------------
// SE|SBADD|<prefab UniqueID>|x,y,z. Static RE (memory a2-22284-sandbox-object-netvars), untested before this:
// a sandbox object is a node in the replicated NetVar tree under <loaded gamemode>/objects; every machine's
// sandbox spawns its own actor from it (Luau bound, NetworkGUID set) -- which is what would carry text and
// Luau behaviour to vanilla clients. Component-less object first: prove the spawn chain.
namespace SeSb {
    constexpr uintptr_t DescInit = 0x46BEDA0, NodeBuild = 0x4716FE0, NodeFinish = 0x46BC8C0, AddChild = 0x463C090,
                        Malloc = 0x5361530, FindChild = 0x463FE20,
                        NodeDesc = 0x47236C0,      // (node) -> cached Desc* (node+168), parsed on first use
                        DescCopy = 0x46BEBC0,      // (dst, src)
                        DescFree = 0x46C1C40,      // (desc)
                        NodeMerge = 0x46B5780,     // (handle*, Desc*) -> rebuild + zipper-merge into the same node
                        NodeRemove = 0x465BE70,    // (handle*) -> detach from parent, replicated
                        PropString = 0x46B3EB0,    // (FString* key, FString* value) -> shared-ref controller
                        ContainerCtor = 0x464B970, // (node[160], FName*, unused) -> type-1 container
                        ContainerStat = 0x4648020, // (node) memory stats only
                        BindProps = 0x46E56D0,     // LuauBehavior (behavior, container) -> adds "Properties" with a
                                                   // type-6 blob leaf per bound field, default values
                        Templates = 0x46D50F0,     // (TArray<UObject*>*, UClass** actorCls, UClass** filterCls)
                        RawAdd = 0x46ADE50,        // (container*, node**) offline add, takes ownership
                        MemFree = 0x10153A0,
                        LeafCtor = 0x464C150,      // (node[80], FName*, uint8 type) -> base node ctor
                        LeafStat = 0x46BC830,      // (node) memory stats only
                        LeafVtbl = 0x804BDA0,      // type-6 blob leaf vtable
                        // The game's own string-node factory is at 0x4643350: malloc(0x50), LeafCtor(.., 4),
                        // vtable 0x8021318, FString at +0x38 cleared, +0x48 = 0x200, then these two.
                        StrAssign = 0xFC1250,      // (FString*, const wchar_t*, int32 len) -> assign, len w/o NUL
                        StringStat = 0x4647E40;    // (node) memory stats for a string node
}

// A serverData value we want on an object: in "Properties" (typed, 02 01 <desc> <value>) or directly under
// serverData (e.g. a red-coin run's CoinTransforms_v2). Queued for the next spawn, or written live.
struct SbLeaf { bool inProps; std::string name; std::vector<uint8_t> blob; bool inRefs = false; };   // inRefs: references container
static std::vector<SbLeaf> g_sbPendingLeaves;

static std::vector<uint8_t> SbBlobFloat(float v)
{
    std::vector<uint8_t> b = { 0x02, 0x01, 0x02 };
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&v);
    b.insert(b.end(), p, p + 4);
    return b;
}
static std::vector<uint8_t> SbBlobGuid(const uint32_t g[4])
{
    std::vector<uint8_t> b = { 0x02, 0x01, 0x3D, 0x00 };
    const uint8_t* p = reinterpret_cast<const uint8_t*>(g);
    b.insert(b.end(), p, p + 16);
    return b;
}
// CoinTransforms_v2: int32 2, int32 count, per coin FTransform (quat, translation relative to the run
// actor, scale; 10 doubles) + 8 zero bytes + FString "None". Decoded from the station's own run.
static std::vector<uint8_t> SbBlobCoins(const std::vector<std::array<double, 3>>& rel)
{
    std::vector<uint8_t> b;
    auto put = [&](const void* p, size_t n) { b.insert(b.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    const int32_t ver = 2, cnt = static_cast<int32_t>(rel.size());
    put(&ver, 4); put(&cnt, 4);
    for (const auto& c : rel)
    {
        const double t[10] = { 0, 0, 0, 1, c[0], c[1], c[2], 1, 1, 1 };
        put(t, sizeof(t));
        const uint8_t tail[17] = { 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, 'N', 'o', 'n', 'e', 0 };
        put(tail, sizeof(tail));
    }
    return b;
}

// Component prop overrides we have written, per sandbox object (by idx): compId -> key -> string value.
// Rebuilt into the node's Desc components array (+160) on every change, so no override is ever lost.
static std::unordered_map<std::string, std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>>> g_sbProps;

// Fill desc+160 with the overrides for `idx`. Every array and string here is engine-allocated (the Desc is
// later freed by the engine). Entry (48 bytes): +0 FName compId, +8 TArray<TSharedPtr<Prop>>, +24 FString
// script, +40 bool serverOnly. TSharedPtr = { ctrl+16 (the Prop), ctrl }.
// Custom Luau: script names attached to an object (by idx). The station attaches a script exactly this way:
// a component entry whose id and script are both "<name>.luau" (seen on TKBGolf's course Cube), with the
// source in <gamemode>/Scripts/<name>.luau -- every machine compiles it from there.
static std::unordered_map<std::string, std::vector<std::string>> g_sbScripts;
static int SbFillComponents(uint8_t* desc, const std::string& idx)
{
    static const std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> kNone;
    auto it = g_sbProps.find(idx);
    auto sc = g_sbScripts.find(idx);
    const auto& comps = it == g_sbProps.end() ? kNone : it->second;
    const size_t nScripts = sc == g_sbScripts.end() ? 0 : sc->second.size();
    if (comps.empty() && !nScripts) return 0;
    const uintptr_t base = GetBase();
    auto mal = reinterpret_cast<void*(__fastcall*)(size_t)>(base + SeSb::Malloc);
    auto mkProp = reinterpret_cast<uint8_t*(__fastcall*)(SDK::FString*, SDK::FString*)>(base + SeSb::PropString);
    const size_t total = comps.size() + nScripts;
    uint8_t* arr = static_cast<uint8_t*>(mal(48 * total));
    memset(arr, 0, 48 * total);
    for (size_t k = 0; k < nScripts; ++k)                 // {FName id = script, no props, FString script}
    {
        uint8_t* e = arr + 48 * (comps.size() + k);
        const std::string& nm = sc->second[k];
        const SDK::FName id = NvName(nm);
        memcpy(e, &id, sizeof(id));
        const std::wstring w(nm.begin(), nm.end());
        SDK::FString scr = SDK::UKismetStringLibrary::Concat_StrStr(SDK::FString(w.c_str()), SDK::FString(L""));
        memcpy(e + 24, &scr, sizeof(scr));
    }
    for (size_t i = 0; i < comps.size(); ++i)
    {
        uint8_t* e = arr + 48 * i;
        const SDK::FName id = NvName(comps[i].first);
        memcpy(e, &id, sizeof(id));
        const auto& props = comps[i].second;
        uint8_t* pa = static_cast<uint8_t*>(mal(16 * props.size()));
        memset(pa, 0, 16 * props.size());
        for (size_t j = 0; j < props.size(); ++j)
        {
            const std::wstring k(props[j].first.begin(), props[j].first.end()), v(props[j].second.begin(), props[j].second.end());
            SDK::FString key = SDK::UKismetStringLibrary::Concat_StrStr(SDK::FString(k.c_str()), SDK::FString(L""));
            SDK::FString val = SDK::UKismetStringLibrary::Concat_StrStr(SDK::FString(v.c_str()), SDK::FString(L""));
            uint8_t* ctrl = mkProp(&key, &val);
            *reinterpret_cast<uint8_t**>(pa + 16 * j) = ctrl ? ctrl + 16 : nullptr;
            *reinterpret_cast<uint8_t**>(pa + 16 * j + 8) = ctrl;
        }
        *reinterpret_cast<uint8_t**>(e + 8) = pa;
        *reinterpret_cast<int32_t*>(e + 16) = static_cast<int32_t>(props.size());
        *reinterpret_cast<int32_t*>(e + 20) = static_cast<int32_t>(props.size());
    }
    *reinterpret_cast<uint8_t**>(desc + 160) = arr;
    *reinterpret_cast<int32_t*>(desc + 168) = static_cast<int32_t>(total);
    *reinterpret_cast<int32_t*>(desc + 172) = static_cast<int32_t>(total);
    return static_cast<int>(total);
}

static void SbSetOverride(const std::string& idx, const std::string& comp, const std::string& key, const std::string& value)
{
    auto& comps = g_sbProps[idx];
    for (auto& c : comps)
        if (c.first == comp)
        {
            for (auto& kv : c.second) if (kv.first == key) { kv.second = value; return; }
            c.second.push_back({ key, value });
            return;
        }
    comps.push_back({ comp, { { key, value } } });
}

// A NetVar handle (UPrefabComponent+0x3A0, or a FindChild result) -> its node. Same indexing as NvNodeType.
static uintptr_t SbNodeOf(const uint8_t* handle)
{
    const uint32_t idx = *reinterpret_cast<const uint32_t*>(handle + 0x20);
    const uintptr_t sys = *reinterpret_cast<const uintptr_t*>(handle + 0x28);
    if (!sys) return 0;
    const uintptr_t chunks = *reinterpret_cast<const uintptr_t*>(sys + ((idx & 0xFF) ? 408 : 432));
    if (!chunks) return 0;
    const uintptr_t entry = *reinterpret_cast<const uintptr_t*>(chunks + 8 * (idx >> 16));
    return entry ? *reinterpret_cast<const uintptr_t*>(entry + 8) : 0;
}

// The sandbox bookkeeping of an actor the sandbox spawned: its UPrefabComponent (NetworkGUID set, GameMode
// set), or null for anything else (e.g. an actor our plain spawn made). Cached: this runs per transform.
// Is this sandbox bookkeeping still live? Districts stream actors and whole loaded gamemodes out and back
// in, and freed addresses get reused. Acting on a stale PrefabComponent or gamemode fed garbage handles
// to NodeRemove / FindChild (seen live on the VPS: caught faults in both, then heap corruption that took
// the server down minutes later, "when going into another district").
static bool SbPrefabLive(SDK::AActor* a, SDK::UObject* pc)
{
    if (!a || !pc || !SeAlive(a) || !SeAlive(pc) || pc->Outer != a) return false;
    if (*(reinterpret_cast<const uint8_t*>(a) + 0x65) & 0x01) return false;          // bActorIsBeingDestroyed
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
    return lgm && SeAlive(lgm);
}

static SDK::UObject* SbPrefabOf(SDK::AActor* a)
{
    struct Entry { int32_t actorIdx; SDK::UObject* pc; };
    static std::unordered_map<SDK::AActor*, Entry> s_cache;
    if (!a || !SeAlive(a)) return nullptr;
    if (auto it = s_cache.find(a); it != s_cache.end())
    {
        // Same address, same object (GObjects index) and still wired up -- else look again.
        if (it->second.actorIdx == a->Index && (!it->second.pc || SbPrefabLive(a, it->second.pc))) return it->second.pc;
        s_cache.erase(it);
    }
    SDK::UObject* found = nullptr;
    static SDK::UClass* pcCls = nullptr;
    if (!pcCls) pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    auto take = [&](SDK::UObject* o)
    {
        if (found || !o || o->Outer != a) return;
        const uintptr_t pc = reinterpret_cast<uintptr_t>(o);
        if (*reinterpret_cast<void**>(pc + 0x440) && *reinterpret_cast<int32_t*>(pc + 0x248 + 8) > 1) found = o;
    };
    for (SDK::UObject* o : ClassObjects(pcCls)) take(o);     // [PERF] shared index first (see SeFindEditorActor)
    if (!found)                                              // just spawned (index is ~1 s behind) or not ours
    {
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; pcCls && i < n && !found; ++i)
        {
            SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
            if (o && o->Outer == a && o->IsA(pcCls)) take(o);
        }
    }
    if (found && !SbPrefabLive(a, found)) found = nullptr;
    s_cache[a] = { a->Index, found };
    return found;
}

static const char* g_sbMergeIdx = nullptr;     // set around a merge that should (re)write component overrides
static void SbFillComponentsSafe(uint8_t* buf, const char* idx) { SbFillComponents(buf, std::string(idx)); }

// POD-only cores (SEH).
// Merge a descriptor into a live node (the move path for objects that DO have a server actor).
// The probe logs whether the node ends up pointing AT the descriptor we passed: if it does, the buffer is
// owned by the node and must be neither reused nor freed.
static int SbMergeCore(uint8_t* handle, uint8_t* buf, const double* pos, const double* rot, const double* scl)
{
    int step = 0;
    __try
    {
        const uintptr_t base = GetBase();
        const uintptr_t node = SbNodeOf(handle);
        if (!node) return -100;
        step = 1;
        void* cached = reinterpret_cast<void*(__fastcall*)(uintptr_t)>(base + SeSb::NodeDesc)(node);
        if (!cached) return -101;
        step = 2;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void*)>(base + SeSb::DescCopy)(buf, cached);
        step = 3;
        if (g_sbMergeIdx && g_sbMergeIdx[0]) SbFillComponentsSafe(buf, g_sbMergeIdx);
        if (pos) memcpy(buf + 48, pos, 3 * sizeof(double));
        if (rot) memcpy(buf + 72, rot, 3 * sizeof(double));
        if (scl) memcpy(buf + 96, scl, 3 * sizeof(double));
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t*)>(base + SeSb::NodeMerge)(handle, buf);
        step = 4;
        uint8_t* after = reinterpret_cast<uint8_t*>(reinterpret_cast<void*(__fastcall*)(uintptr_t)>(base + SeSb::NodeDesc)(SbNodeOf(handle)));
        // Does the node now point INTO the copy we made? (idx FString data @+16, components array @+160.)
        void* bufIdx = *reinterpret_cast<void**>(buf + 16);
        void* nodeIdx = after ? *reinterpret_cast<void**>(after + 16) : nullptr;
        void* bufComp = *reinterpret_cast<void**>(buf + 160);
        void* nodeComp = after ? *reinterpret_cast<void**>(after + 160) : nullptr;
        if (bufIdx == nodeIdx || after == buf)        // would mean the node kept our copy: never free it then
        {
            HxLog("[HalcyonA2][SPECEDIT] merge: the node kept our descriptor (idx %p, desc %p) -- not freeing it\n", bufIdx, static_cast<void*>(after));
            return 5;
        }
        reinterpret_cast<void(__fastcall*)(uint8_t*)>(base + SeSb::DescFree)(buf);   // our copy, deep-copied by the merge
        return 5;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -step; }
}

static int SbRemoveCore(uint8_t* handle)
{
    __try
    {
        reinterpret_cast<void(__fastcall*)(uint8_t*)>(GetBase() + SeSb::NodeRemove)(handle);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}


// ---- sandbox objects we placed, tracked by node ------------------------------------------------------
// Some prefab types never get an actor on the server (seen live: floating pipes, scoreboard sideboards,
// DefaultMeshObject -- the node is added, clients build it, the server's object map does not grow). Every
// later move/delete then failed ("no X near"), because those are resolved through server actors. So the
// server keeps its own record of what it placed and edits those nodes directly when there is no actor.
struct SbOwned
{
    std::string idx, cls; SDK::UObject* lgm = nullptr; double loc[3]{}, rot[3]{}, scl[3]{ 1, 1, 1 };
    std::string uniqueId, path, level;                  // saved levels: prefab type, palette path, owning level
    std::vector<std::array<std::string, 3>> data;       // Game-data edits {path, kind, value}, replayed on load
    std::vector<std::pair<std::string, std::string>> scripts;   // custom Luau {name, source}
    std::vector<std::array<std::string, 4>> refs;                // script slots {script, slot, target idx, key}
};
static std::vector<SbOwned> g_sbOwned;
static void SbOwnedForget(const std::string& idx)      // the object is gone: stop tracking (and saving) it
{
    for (size_t i = 0; !idx.empty() && i < g_sbOwned.size(); ++i)
        if (g_sbOwned[i].idx == idx) { g_sbOwned.erase(g_sbOwned.begin() + i); return; }
}

static SbOwned* SbOwnedFind(const std::string& ident)
{
    const size_t at = ident.find('@');
    double want[3];
    if (at == std::string::npos || !SeVec(ident.substr(at + 1), want)) return nullptr;
    const std::string cls = ident.substr(0, at);
    SbOwned* best = nullptr;
    double bestD2 = 300.0 * 300.0;                        // 3 m: the client may still show a pre-move spot
    for (auto& o : g_sbOwned)
    {
        if (o.cls != cls) continue;
        const double dx = o.loc[0] - want[0], dy = o.loc[1] - want[1], dz = o.loc[2] - want[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < bestD2) { bestD2 = d2; best = &o; }
    }
    return best;
}
static SbOwned* SbOwnedByIdx(const std::string& idx)
{
    for (auto& o : g_sbOwned) if (o.idx == idx) return &o;
    return nullptr;
}
// Remember a Game-data edit on the object it was made to (replayed when a saved level loads).
static void SeLvRecordData(SDK::UObject* prefab, const std::string& path, const std::string& kind, const std::string& value)
{
    if (!prefab) return;
    SbOwned* o = SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(prefab) + 0x248)));
    if (!o) return;
    for (auto& d : o->data) if (d[0] == path) { d[1] = kind; d[2] = value; return; }
    o->data.push_back({ path, kind, value });
}

// POD core: resolve <lgm>/objects/<idx> to an iterator (a valid node handle) and merge or remove it.
// Move or remove one of our object nodes by id, for objects the server has no actor for.
static int SbOwnedNodeCore(void* lgmHandle, uint64_t objectsBits, uint64_t idxBits, bool remove, uint8_t* buf,
                           const double* pos, const double* rot, const double* scl)
{
    uint8_t itO[0x100] = {}, itN[0x100] = {};
    int r = -1;
    __try
    {
        const uintptr_t base = GetBase();
        auto find = reinterpret_cast<void*(__fastcall*)(void*, uint8_t*, uint64_t, char)>(base + SeSb::FindChild);
        find(lgmHandle, itO, objectsBits, 0);
        if (!itO[0x48]) { NvReleaseIter(base, itO); return -10; }
        find(itO, itN, idxBits, 0);
        if (!itN[0x48]) { NvReleaseIter(base, itN); NvReleaseIter(base, itO); return -11; }
        r = remove ? SbRemoveCore(itN) : SbMergeCore(itN, buf, pos, rot, scl);
        // The node iterator is NOT released: NodeRemove consumes it, and a merge rebuilds the node, so a
        // release afterwards could touch a replaced entry. At worst one reference is kept.
        NvReleaseIter(base, itO);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -12; }
    return r;
}

// ---- team changers never appear on top of a player ----
// A team changer that spawns (or is rebuilt by a move) where a player stands gets its begin-overlap DURING the
// spawn, before its component is bound to its node (TeamChangeComponent+0x268 still null). The game's handler
// (0x53C48B0) then reads through that null on the player's own machine and the player's game crashes -- seen
// 2026-09-24 with an unmodded PlayerNovBuild client standing where GMTEAM placed a changer (not every time:
// it is a race). So a changer is only ever built where no player is: moves wait, new ones start at a free
// spot in the same area and move in when the player has stepped off.
constexpr double kSeTcClearCm = 500.0;
static bool SeIsTeamChangerName(const std::string& n)
{
    std::string lo;
    for (char c : n) lo += static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return lo.find("teamchange") != std::string::npos || lo.find("teamswitch") != std::string::npos;
}
// Where a VR player really is. The server's pawn actor does not follow the player's pose; the pose arrives in
// the player entity's replicated frequent data (AVRPawn.Entity @0x928, FReplicatedFrequentData @0xF0, root
// position @+0x10 -> entity+0x100). Falls back to the actor location while there is no entity/pose yet.
static SDK::FVector SePawnPos(SDK::AActor* pawn)
{
    const uintptr_t ent = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(pawn) + 0x928);
    if (ent)
    {
        const double* r = reinterpret_cast<const double*>(ent + 0x100);
        if (r[0] != 0.0 || r[1] != 0.0 || r[2] != 0.0) return SDK::FVector{ r[0], r[1], r[2] };
    }
    return pawn->K2_GetActorLocation();
}
static bool SePawnNear(const double* loc, double radius)
{
    static SDK::UClass* cls = nullptr;
    if (!cls) cls = SDK::UObject::FindClassFast("VRPawn");
    if (!cls) return false;
    for (SDK::UObject* o : ClassObjects(cls))
    {
        if (!o || o->IsDefaultObject() || !*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x2D0)) continue;   // no controller
        const SDK::FVector l = SePawnPos(static_cast<SDK::AActor*>(o));
        const double dx = l.X - loc[0], dy = l.Y - loc[1], dz = l.Z - loc[2];
        if (dx * dx + dy * dy + dz * dz < radius * radius) return true;
    }
    return false;
}

// Moves waiting to be applied: object idx -> when it last moved.
static std::unordered_map<std::string, ULONGLONG> g_sbMovePending;
constexpr ULONGLONG kSbMoveSettleMs = 300;
static void SbMoveFlush()
{
    if (g_sbMovePending.empty()) return;
    const ULONGLONG now = GetTickCount64();
    std::vector<std::string> due;
    for (const auto& kv : g_sbMovePending) if (now - kv.second >= kSbMoveSettleMs) due.push_back(kv.first);
    for (const auto& idx : due)
    {
        g_sbMovePending.erase(idx);
        const SbOwned* o = SbOwnedByIdx(idx);
        if (!o) continue;                                // deleted or rebuilt meanwhile
        if ((SeIsTeamChangerName(o->cls) || SeIsTeamChangerName(o->uniqueId)) && SePawnNear(o->loc, kSeTcClearCm))
        {
            static ULONGLONG s_said = 0;
            if (now - s_said > 5000) { s_said = now; HxLog("[HalcyonA2][SPECEDIT] team changer %s waits: a player is standing on its spot\n", idx.c_str()); }
            g_sbMovePending[idx] = now;                   // try again once they have stepped off
            continue;
        }
        const SbOwned keep = *o;                         // o lives in g_sbOwned, which the rebuild changes
        SbRespawnKeep(SbActorForIdx(keep.idx), keep, "move");
    }
}

static bool SbOwnedApply(SbOwned& o, bool remove)
{
    if (!o.lgm || !SeAlive(o.lgm))                       // its gamemode was unloaded (district change): gone
    {
        HxLog("[HalcyonA2][SPECEDIT] node %s %s: its gamemode is gone, dropping the record\n", remove ? "delete" : "move", o.idx.c_str());
        return false;
    }
    if (!remove)
    {
        // Move = rebuild in place (same id, area, scripts, slot wiring and Game data). It used to merge a new
        // descriptor into the live node; that left the node's parent-id bookkeeping cyclic, and the next time
        // the node was removed -- attaching a script, deleting it, unloading its level -- the game's path walk
        // (+0x4665F80) recursed until the stack ran out and the server died. Reproduced: tests/movethenluau.txt.
        // A drag sends a move ~30 times a second: the record (o.loc/rot/scl, already updated by the caller) takes
        // every one, and the object is rebuilt once, when it has been still for kSbMoveSettleMs (SbMoveFlush).
        g_sbMovePending[o.idx] = GetTickCount64();
        return true;
    }
    SDK::AActor* slot = o.lgm ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(o.lgm) + 0x320) : nullptr;
    double pos[3] = { o.loc[0], o.loc[1], o.loc[2] }, r3[3] = { o.rot[0], o.rot[1], o.rot[2] };
    if (slot && !remove)
    {
        const SDK::FTransform sx = slot->GetTransform();
        const SDK::FVector rl = SDK::UKismetMathLibrary::InverseTransformLocation(sx, SDK::FVector{ pos[0], pos[1], pos[2] });
        const SDK::FRotator rr = SDK::UKismetMathLibrary::InverseTransformRotation(sx, SDK::FRotator{ r3[0], r3[1], r3[2] });
        pos[0] = rl.X; pos[1] = rl.Y; pos[2] = rl.Z; r3[0] = rr.Pitch; r3[1] = rr.Yaw; r3[2] = rr.Roll;
    }
    alignas(16) static uint8_t buf[0x400];
    memset(buf, 0, sizeof(buf));
    const int r = SbOwnedNodeCore(reinterpret_cast<uint8_t*>(o.lgm) + 0x218, NvNameBits(NvName("objects")),
                                  NvNameBits(NvName(o.idx)), remove, buf, pos, r3, o.scl);
    const bool ok = remove ? r == 1 : r == 5;
    if (ok && slot) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    HxLog("[HalcyonA2][SPECEDIT] node %s %s (no server actor): %s (%d)\n", remove ? "delete" : "move", o.idx.c_str(),
          ok ? "ok" : "FAILED", r);
    return ok;
}

static bool SbOwnedXform(const std::string& ident, const std::string& locs, const std::string& rots, const std::string& scls)
{
    SbOwned* o = SbOwnedFind(ident);
    if (!o) return false;
    SeVec(locs, o->loc); SeVec(rots, o->rot); SeVec(scls, o->scl);
    return SbOwnedApply(*o, false);
}

static bool SbOwnedDelete(const std::string& ident)
{
    SbOwned* o = SbOwnedFind(ident);
    if (!o) return false;
    const bool ok = SbOwnedApply(*o, true);
    if (ok || !o->lgm || !SeAlive(o->lgm)) g_sbOwned.erase(g_sbOwned.begin() + (o - g_sbOwned.data()));
    return ok;
}

// Move / rotate / scale a sandbox object for everyone: rewrite its node (slot-relative transform).
static bool SeSandboxXform(SDK::AActor* a, const double* loc, const double* rot, const double* scl)
{
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) return false;
    if (SbOwned* o = SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248))))
    {
        memcpy(o->loc, loc, sizeof(o->loc)); memcpy(o->rot, rot, sizeof(o->rot)); memcpy(o->scl, scl, sizeof(o->scl));
        return SbOwnedApply(*o, false);                  // ours: rebuild in place, never merge (see SbOwnedApply)
    }
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
    SDK::AActor* slot = lgm ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320) : nullptr;
    double pos[3] = { loc[0], loc[1], loc[2] }, r3[3] = { rot[0], rot[1], rot[2] };
    if (slot)
    {
        const SDK::FTransform sx = slot->GetTransform();
        const SDK::FVector rl = SDK::UKismetMathLibrary::InverseTransformLocation(sx, SDK::FVector{ loc[0], loc[1], loc[2] });
        const SDK::FRotator rr = SDK::UKismetMathLibrary::InverseTransformRotation(sx, SDK::FRotator{ rot[0], rot[1], rot[2] });
        pos[0] = rl.X; pos[1] = rl.Y; pos[2] = rl.Z; r3[0] = rr.Pitch; r3[1] = rr.Yaw; r3[2] = rr.Roll;
    }
    alignas(16) static uint8_t buf[0x400];
    memset(buf, 0, sizeof(buf));
    const int r = SbMergeCore(reinterpret_cast<uint8_t*>(pc) + 0x3A0, buf, pos, r3, scl);
    if (r != 5) HxLog("[HalcyonA2][SPECEDIT] sandbox move of %s failed at step %d\n", a->GetName().c_str(), r);
    return r == 5;
}

static bool SeSandboxDelete(SDK::AActor* a)
{
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) return false;
    // A station/project team changer must never lose its node: every client keeps its actor with a cleared
    // node handle and crashes when a player overlaps it (see SeModeClearStarters). Ours rebuild cleanly.
    if (a->Class && a->Class->GetName().find("TeamChange") != std::string::npos &&
        !SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248))))
    {
        HxLog("[HalcyonA2][SPECEDIT] sandbox delete of %s refused: a station team changer (clients would crash on it)\n", a->GetName().c_str());
        return false;
    }
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
    SDK::AActor* slot = lgm ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320) : nullptr;
    const int r = SbRemoveCore(reinterpret_cast<uint8_t*>(pc) + 0x3A0);
    if (r == 1 && slot) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    HxLog("[HalcyonA2][SPECEDIT] sandbox delete of %s: %s\n", a->GetName().c_str(), r == 1 ? "removed" : "FAILED");
    return r == 1;
}


// ---- bound Luau properties (serverData/Properties/<field>) ------------------------------------------
// This is how the game carries a component's editable values (a Text's text, a red-coin quest's Duration)
// to every machine: a "Properties" container under the object's serverData holding one type-6 blob leaf
// per bound field of the LuauBehavior (behavior+0x3C0: 16-byte {FName, FProperty*}). Station objects have
// it; objects we add never got one, so vanilla clients kept the prefab defaults. We let the game build the
// container (BindProps) in a detached holder, splice the value into the blob -- header bytes + FArchive
// FString tail, default "" = 00 00 00 00 -- and attach the result with AddChild, which replicates (and
// replaces a same-named child).
static int SbFindChildRaw(uint8_t* container, uint64_t nameBits, uint8_t** out)
{
    uint8_t** arr = *reinterpret_cast<uint8_t***>(container + 136);
    const int n = *reinterpret_cast<int32_t*>(container + 144);
    for (int i = 0; arr && i < n && i < 256; ++i)
        if (arr[i] && *reinterpret_cast<uint64_t*>(arr[i] + 48) == nameBits) { *out = arr[i]; return i; }
    return -1;
}

// POD-only core (SEH). 1 = attached a new Properties container, 2 = replaced one leaf, <0 = failed step.
static int SbBoundPropCore(uint8_t* objHandle, void* behavior, uint64_t keyBits, const uint8_t* val, int valLen,
                           uint64_t serverDataBits, uint64_t propsBits)
{
    int step = 0;
    uint8_t itSD[0x100] = {}, itP[0x100] = {};
    __try
    {
        const uintptr_t base = GetBase();
        auto mal = reinterpret_cast<uint8_t*(__fastcall*)(size_t)>(base + SeSb::Malloc);
        auto find = reinterpret_cast<void*(__fastcall*)(void*, uint8_t*, uint64_t, char)>(base + SeSb::FindChild);
        auto add = reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t**)>(base + SeSb::AddChild);
        step = 1;
        uint8_t* holder = mal(160);
        if (!holder) return -100;
        memset(holder, 0, 160);
        uint64_t hn = propsBits;                                             // any name will do for the holder
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint64_t*, int)>(base + SeSb::ContainerCtor)(holder, &hn, 0);
        reinterpret_cast<void(__fastcall*)(uint8_t*)>(base + SeSb::ContainerStat)(holder);
        step = 2;
        reinterpret_cast<void(__fastcall*)(void*, uint8_t*)>(base + SeSb::BindProps)(behavior, holder);
        step = 3;
        uint8_t* props = nullptr;
        const int pi = SbFindChildRaw(holder, propsBits, &props);
        if (pi < 0) return -101;                                              // behavior has no bound fields
        uint8_t* leaf = nullptr;
        const int li = SbFindChildRaw(props, keyBits, &leaf);
        if (li < 0) return -102;                                              // not a bound field
        if (leaf[24] != 6) return -103;
        step = 4;
        uint8_t* old = *reinterpret_cast<uint8_t**>(leaf + 56);
        const int n = *reinterpret_cast<int32_t*>(leaf + 64);
        if (!old || n < 6 || *reinterpret_cast<uint32_t*>(old + n - 4) != 0) return -104;   // not an empty FString
        uint8_t* blob = mal(n - 4 + valLen);
        memcpy(blob, old, n - 4);
        memcpy(blob + n - 4, val, valLen);
        *reinterpret_cast<uint8_t**>(leaf + 56) = blob;                       // old default blob leaks (tiny)
        *reinterpret_cast<int32_t*>(leaf + 64) = n - 4 + valLen;
        *reinterpret_cast<int32_t*>(leaf + 68) = n - 4 + valLen;
        step = 5;
        find(objHandle, itSD, serverDataBits, 0);
        if (!itSD[0x48]) { NvReleaseIter(base, itSD); return -105; }
        step = 6;
        find(itSD, itP, propsBits, 0);
        int rc;
        if (itP[0x48])
        {
            (*reinterpret_cast<uint8_t***>(props + 136))[li] = nullptr;       // detach the leaf from the holder
            uint8_t itOld[0x100] = {};                                        // drop the old leaf (AddChild keeps both)
            find(itP, itOld, keyBits, 0);
            if (itOld[0x48]) SbRemoveCore(itOld);
            else NvReleaseIter(base, itOld);
            add(itP, &leaf);
            rc = 2;
        }
        else
        {
            (*reinterpret_cast<uint8_t***>(holder + 136))[pi] = nullptr;      // detach Properties from the holder
            add(itSD, &props);
            rc = 1;
        }
        step = 7;
        NvReleaseIter(base, itP);
        NvReleaseIter(base, itSD);
        return rc;                                                            // the holder is leaked (160 bytes)
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -step; }
}

// The LuauBehavior on `a` that binds `field` (and whether it is a string property).
static void* SbBehaviorFor(SDK::AActor* a, const std::string& field, bool* isStr)
{
    auto* lbCls = SDK::UObject::FindClassFast("LuauBehavior");
    if (!lbCls || !a) return nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->Outer != a || !o->IsA(lbCls)) continue;
        const uint8_t* arr = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uintptr_t>(o) + 0x3C0);
        const int cnt = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x3C8);
        for (int k = 0; arr && k < cnt && k < 64; ++k)
        {
            if (reinterpret_cast<const SDK::FName*>(arr + k * 16)->ToString() != field) continue;
            const SDK::FField* fp = *reinterpret_cast<SDK::FField* const*>(arr + k * 16 + 8);
            *isStr = fp && fp->ClassPrivate && fp->ClassPrivate->Name.ToString() == "StrProperty";
            return o;
        }
    }
    return nullptr;
}

// Set a bound string field on a sandbox object for every machine. False when it is not one.
static bool SbSetBoundProp(SDK::AActor* a, const std::string& field, const std::string& value)
{
    SDK::UObject* pc = SbPrefabOf(a);
    bool isStr = false;
    void* beh = pc ? SbBehaviorFor(a, field, &isStr) : nullptr;
    if (!beh || !isStr) return false;
    std::wstring w;                                                           // UTF-8 -> UTF-16
    if (!value.empty())
    {
        w.resize(MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0));
        MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), w.data(), static_cast<int>(w.size()));
    }
    bool ansi = true;
    for (wchar_t c : w) if (c > 127) { ansi = false; break; }
    const int32_t num = static_cast<int32_t>(w.size()) + 1;                   // FArchive << FString
    const int32_t save = w.empty() ? 0 : (ansi ? num : -num);
    std::vector<uint8_t> v(4);
    memcpy(v.data(), &save, 4);
    if (!w.empty())
    {
        if (ansi) { for (wchar_t c : w) v.push_back(static_cast<uint8_t>(c)); v.push_back(0); }
        else for (size_t i = 0; i <= w.size(); ++i) { const wchar_t c = i < w.size() ? w[i] : 0; v.push_back(c & 0xFF); v.push_back(static_cast<uint8_t>(c >> 8)); }
    }
    const int r = SbBoundPropCore(reinterpret_cast<uint8_t*>(pc) + 0x3A0, beh, NvNameBits(NvName(field)), v.data(),
                                  static_cast<int>(v.size()), NvNameBits(NvName("serverData")), NvNameBits(NvName("Properties")));
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
    SDK::AActor* slot = lgm ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320) : nullptr;
    if (r > 0 && slot) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    if (r > 0) SeLvRecordData(pc, "props/" + field, "text", value);
    HxLog("[HalcyonA2][SPECEDIT] bound prop %s.%s = '%s': %s (%d)\n", a->GetName().c_str(), field.c_str(), value.c_str(),
          r == 1 ? "Properties attached" : r == 2 ? "leaf replaced" : "FAILED", r);
    return r > 0;
}

// The server's own actor for a sandbox object idx (its PrefabComponent's NetworkGUID).
// [PERF] The shared PrefabComponent index first; a full walk only if that misses and the caller allows it (an
// object spawned in the last second isn't in the index yet). Tick-driven callers pass indexOnly.
static SDK::AActor* SbActorForIdx(const std::string& idx, bool indexOnly)
{
    static SDK::UClass* pcCls = nullptr;
    if (!pcCls) pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    if (!pcCls) return nullptr;
    auto match = [&](SDK::UObject* o) -> SDK::AActor*
    {
        if (!o || o->IsDefaultObject()) return nullptr;
        if (FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(o) + 0x248)) != idx) return nullptr;
        auto* act = static_cast<SDK::AActor*>(o->Outer);
        if (!act || (*(reinterpret_cast<const uint8_t*>(act) + 0x65) & 0x01)) return nullptr;   // being destroyed
        if (!SbNodeOf(reinterpret_cast<uint8_t*>(o) + 0x3A0)) return nullptr;                    // its node is gone
        return act;
    };
    for (SDK::UObject* o : ClassObjects(pcCls)) if (SDK::AActor* a = match(o)) return a;
    if (indexOnly) return nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && o->IsA(pcCls)) if (SDK::AActor* a = match(o)) return a;
    }
    return nullptr;
}
static SDK::AActor* SbActorForIdx(const std::string& idx) { return SbActorForIdx(idx, false); }

// SE|SBDESC|<ident> (local test): print an object's component list from its node Desc -- ids, scripts, prop
// keys/types/values -- so we can see how a component value (e.g. the text) is named before writing one.
static void SbDescDumpImpl(uint8_t* d, char* out, size_t cap);
static void SbDescDumpCore(uintptr_t node, char* out, size_t cap)
{
    __try
    {
        uint8_t* d = reinterpret_cast<uint8_t*>(reinterpret_cast<void*(__fastcall*)(uintptr_t)>(GetBase() + SeSb::NodeDesc)(node));
        if (!d) { snprintf(out, cap, "no desc"); return; }
        SbDescDumpImpl(d, out, cap);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { snprintf(out, cap, "fault reading desc"); }
}
static void SbDescDumpImpl(uint8_t* d, char* out, size_t cap)
{
    {
        uint8_t* comps = *reinterpret_cast<uint8_t**>(d + 160);
        const int nc = *reinterpret_cast<int32_t*>(d + 168);
        size_t len = snprintf(out, cap, "type=%s comps=%d", reinterpret_cast<SDK::FName*>(d)->ToString().c_str(), nc);
        for (int i = 0; comps && i < nc && i < 16 && len < cap - 200; ++i)
        {
            uint8_t* e = comps + i * 48;
            const wchar_t* script = *reinterpret_cast<wchar_t**>(e + 24);
            len += snprintf(out + len, cap - len, " | %s script=%ls", reinterpret_cast<SDK::FName*>(e)->ToString().c_str(), script ? script : L"");
            uint8_t* props = *reinterpret_cast<uint8_t**>(e + 8);
            const int np = *reinterpret_cast<int32_t*>(e + 16);
            for (int j = 0; props && j < np && j < 12 && len < cap - 120; ++j)
            {
                uint8_t* prop = *reinterpret_cast<uint8_t**>(props + j * 16);
                if (!prop) continue;
                const wchar_t* key = *reinterpret_cast<wchar_t**>(prop + 8);
                const int type = *reinterpret_cast<int32_t*>(prop + 24);
                if (type == 0) { const wchar_t* v = *reinterpret_cast<wchar_t**>(prop + 32); len += snprintf(out + len, cap - len, " [%ls=\"%.40ls\"]", key ? key : L"?", v ? v : L""); }
                else if (type == 1) len += snprintf(out + len, cap - len, " [%ls=%g]", key ? key : L"?", *reinterpret_cast<double*>(prop + 32));
                else len += snprintf(out + len, cap - len, " [%ls=%s]", key ? key : L"?", *reinterpret_cast<uint8_t*>(prop + 32) ? "true" : "false");
            }
        }
    }
}
// SE|SBPROBE|<ident> (local test): where do a sandbox object's Luau-bound UPROPERTYs live in the NetVar
// tree? Lists each LuauBehavior's bound fields (behavior+0x3C0: 16-byte {FName, FProperty*}) and tests the
// candidate paths under the object's node (prefab+0x3A0 is a handle usable as a FindChild container).
static void SbProbeOne(void* objHandle, const std::vector<std::string>& path, std::string& out)
{
    std::vector<uint64_t> segs;
    for (const auto& seg : path) segs.push_back(NvNameBits(NvName(seg)));
    NvWalk w{}; void* parent = nullptr; int failAt = -1;
    const bool ok = NvWorldWalk(objHandle, segs.data(), static_cast<int>(segs.size()) - 1, &w, &parent, &failAt);
    if (ok)
    {
        const int type = NvNodeType(parent, segs.back());
        if (type >= 0)
        {
            std::string p;
            for (const auto& seg : path) p += "/" + seg;
            char b[700];
            if (type == NvNative::TString || type == NvNative::TNumber || type == NvNative::TBool)
            {
                NvOrig o{};
                NvReadNative(parent, segs.back(), type, &o);
                if (type == NvNative::TString) snprintf(b, sizeof(b), " %s=\"%.60ls\"", p.c_str(), o.str);
                else if (type == NvNative::TNumber) snprintf(b, sizeof(b), " %s=%g", p.c_str(), o.num);
                else snprintf(b, sizeof(b), " %s=%s", p.c_str(), o.b ? "true" : "false");
            }
            else snprintf(b, sizeof(b), " %s(type %d)", p.c_str(), type);
            out += b;
        }
    }
    NvWalkRelease(&w);
}

static void SeSandboxProbePaths(const std::string& ident)
{
    if (!g_seLocalTest) return;
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) { HxLog("[HalcyonA2][SPECEDIT] SBPROBE %s: not a sandbox object\n", ident.c_str()); return; }
    void* h = reinterpret_cast<uint8_t*>(pc) + 0x3A0;
    auto* lbCls = SDK::UObject::FindClassFast("LuauBehavior");
    std::vector<std::pair<std::string, std::vector<std::string>>> comps;   // component name -> bound fields
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; lbCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->Outer != a || !o->IsA(lbCls)) continue;
        std::vector<std::string> fields;
        const uint8_t* arr = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uintptr_t>(o) + 0x3C0);
        const int cnt = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x3C8);
        for (int k = 0; arr && k < cnt && k < 64; ++k) fields.push_back(reinterpret_cast<const SDK::FName*>(arr + k * 16)->ToString());
        comps.push_back({ o->GetName(), fields });
    }
    std::string out;
    for (const char* top : { "serverData", "gameData", "components", "__metadata", "references", "outgoingConnections" })
    {
        SbProbeOne(h, { top }, out);
        SbProbeOne(h, { top, "Properties" }, out);
        for (const auto& c : comps)
        {
            SbProbeOne(h, { top, c.first }, out);
            SbProbeOne(h, { top, c.first, "Properties" }, out);
            for (const auto& f : c.second)
            {
                SbProbeOne(h, { top, "Properties", f }, out);
                SbProbeOne(h, { top, c.first, f }, out);
                SbProbeOne(h, { top, c.first, "Properties", f }, out);
            }
        }
    }
    std::string fl;
    for (const auto& c : comps)
    {
        fl += " " + c.first + "{";
        for (const auto& f : c.second) fl += f + ",";
        fl += "}";
    }
    HxLog("[HalcyonA2][SPECEDIT] SBPROBE %s bound:%s\n", a->GetName().c_str(), fl.c_str());
    HxLog("[HalcyonA2][SPECEDIT] SBPROBE %s found:%s\n", a->GetName().c_str(), out.empty() ? " (nothing)" : out.c_str());
}

static SDK::UObject* SbEngine();   // below
// SE|ACTORS|<filter> (local test): every actor class in the world whose name contains <filter>, with counts.
static void SeActorClasses(const std::string& filter)
{
    if (!g_seLocalTest) return;
    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    std::map<std::string, int> counts;
    std::string lf = filter; for (auto& ch : lf) ch = static_cast<char>(tolower(ch));
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; actorCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(actorCls) || !o->Class) continue;
        std::string cn = o->Class->GetName(), lc = cn;
        for (auto& ch : lc) ch = static_cast<char>(tolower(ch));
        if (lc.find(lf) != std::string::npos) ++counts[cn];
    }
    std::string out;
    for (const auto& kv : counts) out += "  " + kv.first + " x" + std::to_string(kv.second) + "\n";
    HxLog("[HalcyonA2][SPECEDIT] ACTORS '%s': %zu class(es)\n%s", filter.c_str(), counts.size(), out.c_str());
}

// SE|LGMTREE (local test): each loaded gamemode's NetVar subtree (2 levels, names/types) and the node at
// LGM+0x1C0 (the scripts container) -- to find where Luau sources live in the replicated tree.
static void SbLgmWalk(uint8_t* n, int depth, std::string& out)
{
    if (!n || depth > 3 || out.size() > 40000) return;
    char b[200];
    snprintf(b, sizeof(b), "%s%s (t%d)\n", std::string(depth * 2, ' ').c_str(), reinterpret_cast<SDK::FName*>(n + 48)->ToString().c_str(), n[24]);
    out += b;
    if (n[24] != 1 || depth >= 3) return;
    uint8_t** arr = *reinterpret_cast<uint8_t***>(n + 136);
    const int cnt = *reinterpret_cast<int32_t*>(n + 144);
    for (int i = 0; arr && i < cnt && i < 40; ++i) SbLgmWalk(arr[i], depth + 1, out);
    if (cnt > 40) { snprintf(b, sizeof(b), "%s... %d children\n", std::string((depth + 1) * 2, ' ').c_str(), cnt); out += b; }
}
static void SbLgmTreeImpl(std::string* out)
{
    auto* cls = SDK::UObject::FindClassFast("LoadedGameMode");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; cls && i < n; ++i)
    {
        SDK::UObject* l = SDK::UObject::GObjects->GetByIndex(i);
        if (!l || l->IsDefaultObject() || !l->IsA(cls)) continue;
        const uintptr_t lb = reinterpret_cast<uintptr_t>(l);
        uint8_t* sc = *reinterpret_cast<uint8_t**>(lb + 0x1C0);
        *out += "LGM " + l->GetName() + " scripts@0x1C0=" + (sc ? reinterpret_cast<SDK::FName*>(sc + 48)->ToString() : std::string("null")) + "\n";
        SbLgmWalk(reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(lb + 0x218))), 1, *out);
    }
}
static void SbLgmTreeCore(std::string* out)
{
    __try { SbLgmTreeImpl(out); } __except (EXCEPTION_EXECUTE_HANDLER) { out->append("(fault)"); }
}
// Is the replicated tree still sane? A node that appears twice (or points at itself) makes the game's own
// walk recurse until the stack runs out, which takes the server with it. This walks our gamemode trees with a
// depth cap and a seen-set after every edit, names the first bad node, and says which operation produced it.
static bool SbTreeCheckNode(uint8_t* n, int depth, std::unordered_set<void*>* seen, std::string* bad, int* nodes)
{
    if (!n || depth > 24) { if (bad->empty()) *bad = "deeper than 24 levels"; return false; }
    if (++*nodes > 20000) return true;                       // big but finite: stop walking, not an error
    if (!seen->insert(n).second)
    {
        char b[160];
        snprintf(b, sizeof(b), "node %p (%s) is in the tree twice", static_cast<void*>(n), reinterpret_cast<SDK::FName*>(n + 48)->ToString().c_str());
        *bad = b;
        return false;
    }
    const int type = n[24];
    if (type == 9)
    {
        for (int off : { 552, 560, 608, 616 })
        {
            uint8_t* c = *reinterpret_cast<uint8_t**>(n + off);
            if (!c || IsBadReadPtr(c, 160) || c[24] > 12) continue;
            if (!SbTreeCheckNode(c, depth + 1, seen, bad, nodes)) return false;
        }
        return true;
    }
    if (type != 1) return true;
    uint8_t** arr = *reinterpret_cast<uint8_t***>(n + 136);
    const int cnt = *reinterpret_cast<int32_t*>(n + 144);
    if (cnt < 0 || cnt > 20000 || (cnt > 0 && (!arr || IsBadReadPtr(arr, sizeof(void*) * cnt))))
    {
        char b[160];
        snprintf(b, sizeof(b), "container %s has a bad child list (%d)", reinterpret_cast<SDK::FName*>(n + 48)->ToString().c_str(), cnt);
        *bad = b;
        return false;
    }
    for (int i = 0; i < cnt; ++i)
        if (!SbTreeCheckNode(arr[i], depth + 1, seen, bad, nodes)) return false;
    return true;
}
static void SbTreeCheckImpl(std::string* bad, int* nodes)
{
    static SDK::UClass* cls = nullptr;
    if (!cls) cls = SDK::UObject::FindClassFast("LoadedGameMode");
    std::unordered_set<void*> seen;
    for (SDK::UObject* l : ClassObjects(cls))                // [PERF] shared index, not a GObjects walk
    {
        if (!bad->empty()) break;
        if (!l || l->IsDefaultObject()) continue;
        uint8_t* root = reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(l) + 0x218)));
        if (root) SbTreeCheckNode(root, 0, &seen, bad, nodes);
    }
}
static void SbTreeCheckCore(std::string* bad, int* nodes)
{
    __try { SbTreeCheckImpl(bad, nodes); } __except (EXCEPTION_EXECUTE_HANDLER) { if (bad->empty()) *bad = "faulted while walking"; }
}
// After an edit that adds or removes nodes; the first failure is logged once (and repeated at most every 30 s).
// [PERF] Not after moves / property / data edits (a drag sends ~30 moves a second and each check walks ~8000
// nodes on the game thread), and at most once per 5 s: the crash it was built to catch is found and fixed.
static void SeTreeCheck(const char* after)
{
    if (!g_specEdit) return;
    static const char* kStructural[] = { "SPAWN", "DELETE", "LUAU", "LUAUREF", "LUAUDEL", "COINRUN", "QUEST", "LEVEL",
                                         "LOADLEVEL", "UNLOADLEVEL", "SBADD", "SBDEL" };
    bool structural = false;
    for (const char* k : kStructural) if (strcmp(after, k) == 0) { structural = true; break; }
    if (!structural) return;
    static ULONGLONG s_lastRun = 0;
    if (s_lastRun && GetTickCount64() - s_lastRun < 5000) return;
    s_lastRun = GetTickCount64();
    std::string bad;
    int nodes = 0;
    const ULONGLONG t0 = GetTickCount64();
    SbTreeCheckCore(&bad, &nodes);
    static ULONGLONG s_lastLog = 0;
    if (bad.empty()) return;
    if (s_lastLog && GetTickCount64() - s_lastLog < 30000) return;
    s_lastLog = GetTickCount64();
    HxLog("[HalcyonA2][SPECEDIT] TREE BROKEN after %s: %s (%d node(s) walked, %llu ms)\n",
          after, bad.c_str(), nodes, GetTickCount64() - t0);
}

// SE|LUAUDUMP|<slot name> (local test): the first bytes of every node in that gamemode's Scripts container.
// The game's own script nodes vs the ones we build: any field we don't set shows up as a difference, and a
// malformed node is exactly what makes the replication walk recurse or fault.
static void SbScriptNodesImpl(const std::string& slot, std::string* out)
{
    auto* cls = SDK::UObject::FindClassFast("LoadedGameMode");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; cls && i < n; ++i)
    {
        SDK::UObject* l = SDK::UObject::GObjects->GetByIndex(i);
        if (!l || l->IsDefaultObject() || !l->IsA(cls)) continue;
        uint8_t* root = reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(l) + 0x218)));
        if (!root) continue;
        const std::string thisSlot = reinterpret_cast<SDK::FName*>(root + 48)->ToString();
        if (!slot.empty() && thisSlot != slot) continue;
        uint8_t* scripts = nullptr;
        if (SbFindChildRaw(root, NvNameBits(NvName("Scripts")), &scripts) < 0) continue;
        uint8_t** arr = *reinterpret_cast<uint8_t***>(scripts + 136);
        const int cnt = *reinterpret_cast<int32_t*>(scripts + 144);
        char b[400];
        snprintf(b, sizeof(b), "slot %s: Scripts container %p, %d node(s)\n", thisSlot.c_str(), static_cast<void*>(scripts), cnt);
        *out += b;
        for (int k = 0; arr && k < cnt && k < 12; ++k)
        {
            uint8_t* nd = arr[k];
            if (!nd) continue;
            snprintf(b, sizeof(b), "  [%d] %s (t%d) node=%p\n     ", k, reinterpret_cast<SDK::FName*>(nd + 48)->ToString().c_str(), nd[24], static_cast<void*>(nd));
            *out += b;
            for (int off = 0; off < 128; ++off)
            {
                snprintf(b, sizeof(b), "%02X ", nd[off]);
                *out += b;
                if (off % 16 == 15) { snprintf(b, sizeof(b), "\n     [+%02X] ", off + 1); *out += b; }
            }
            *out += "\n";
        }
    }
}
static void SbScriptNodesCore(const std::string* slot, std::string* out)
{
    __try { SbScriptNodesImpl(*slot, out); } __except (EXCEPTION_EXECUTE_HANDLER) { out->append("(fault)\n"); }
}
// SE|NODEMOVE|<ident>|<x,y,z> (local test): move through the no-server-actor path on purpose.
static void SeTestNodeMove(const std::string& ident, const std::string& locs)
{
    if (!g_seLocalTest) return;
    SbOwned* o = SbOwnedFind(ident);
    if (!o) { HxLog("[HalcyonA2][SPECEDIT] NODEMOVE: %s is not one of ours\n", ident.c_str()); return; }
    SeVec(locs, o->loc);
    const bool ok = SbOwnedApply(*o, false);
    HxLog("[HalcyonA2][SPECEDIT] NODEMOVE %s -> %s: %s\n", ident.c_str(), locs.c_str(), ok ? "ok" : "FAILED");
}

static void SeLuauDump(const std::string& slot)
{
    if (!g_seLocalTest) return;
    std::string out;
    SbScriptNodesCore(&slot, &out);
    HxLog("[HalcyonA2][SPECEDIT] LUAUDUMP %s:\n%s", slot.c_str(), out.c_str());
}

// SE|LGMDESC|<slot name> (local test): the component/script entries of every object in that gamemode.
static void SbTreeWalk(uint8_t* n, int depth, std::string& out);   // below
static void SbLgmDescImpl(const std::string& slot0, std::string* out)
{
    const bool all = slot0.size() > 4 && slot0.compare(slot0.size() - 4, 4, ":all") == 0;   // every object, desc only
    const bool tree = slot0.size() > 5 && slot0.compare(slot0.size() - 5, 5, ":tree") == 0; // every object, desc + tree
    const std::string slot = all ? slot0.substr(0, slot0.size() - 4) : tree ? slot0.substr(0, slot0.size() - 5) : slot0;
    auto* cls = SDK::UObject::FindClassFast("LoadedGameMode");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; cls && i < n; ++i)
    {
        SDK::UObject* l = SDK::UObject::GObjects->GetByIndex(i);
        if (!l || l->IsDefaultObject() || !l->IsA(cls)) continue;
        uint8_t* root = reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(l) + 0x218)));
        if (!root || reinterpret_cast<SDK::FName*>(root + 48)->ToString() != slot) continue;
        uint8_t* objs = nullptr;
        if (SbFindChildRaw(root, NvNameBits(NvName("Objects")), &objs) < 0) continue;
        uint8_t** arr = *reinterpret_cast<uint8_t***>(objs + 136);
        const int cnt = *reinterpret_cast<int32_t*>(objs + 144);
        for (int k = 0; arr && k < cnt && k < 80; ++k)
        {
            if (!arr[k]) continue;
            char buf[2048];
            SbDescDumpCore(reinterpret_cast<uintptr_t>(arr[k]), buf, sizeof(buf));
            if (all) { *out += std::string(buf) + "\n"; continue; }
            if (tree) { *out += std::string(buf) + "\n"; SbTreeWalk(arr[k], 0, *out); continue; }
            if (strstr(buf, "script=") && !strstr(buf, "comps=0"))
            {
                *out += std::string(buf) + "\n";
                SbTreeWalk(arr[k], 0, *out);            // incl. references / outgoingConnections
            }
        }
    }
}
static void SbLgmDescCore(const std::string* slot, std::string* out)
{
    __try { SbLgmDescImpl(*slot, out); } __except (EXCEPTION_EXECUTE_HANDLER) { out->append("(fault)"); }
}
static void SeLgmDesc(const std::string& slot)
{
    if (!g_seLocalTest) return;
    std::string out;
    SbLgmDescCore(&slot, &out);
    HxLog("[HalcyonA2][SPECEDIT] LGMDESC %s:\n%s", slot.c_str(), out.c_str());
}

static void SeLgmTree()
{
    if (!g_seLocalTest) return;
    std::string out;
    SbLgmTreeCore(&out);
    HxLog("[HalcyonA2][SPECEDIT] LGMTREE:\n%s", out.c_str());
}

// SE|SBTYPES (local test): every sandbox prefab type -- UniqueID and Blueprint class.
static void SeSandboxTypes()
{
    if (!g_seLocalTest) return;
    SDK::UObject* sb = SbEngine();
    if (!sb) return;
    const uintptr_t b = reinterpret_cast<uintptr_t>(sb);
    SDK::UObject** rp = *reinterpret_cast<SDK::UObject***>(b + 0xF8);
    const int raw = *reinterpret_cast<int32_t*>(b + 0xF8 + 8);
    std::string out;
    for (int i = 0; rp && i < raw && i < 1000; ++i)
    {
        SDK::UObject* def = rp[i];
        if (!def) continue;
        SDK::UObject* bp = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x30);
        SDK::UObject* st = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x38);
        out += (st ? reinterpret_cast<SDK::FName*>(reinterpret_cast<uintptr_t>(st) + 0xF0)->ToString() : std::string("?"));
        out += " = " + (bp ? bp->GetName() : std::string("?")) + "\n";
    }
    HxLog("[HalcyonA2][SPECEDIT] SBTYPES %d:\n%s", raw, out.c_str());
}

// SE|SBTREE|<ident> (local test): the object's whole NetVar subtree, read from the raw nodes (children at
// +136/+144, name FName +48, type byte +24; blob leaves +56/+64). Read-only.
static void SbTreeWalk(uint8_t* n, int depth, std::string& out)
{
    if (!n || depth > 8 || out.size() > 60000) return;
    char b[400];
    const int type = n[24];
    std::string line(depth * 2, ' ');
    line += reinterpret_cast<SDK::FName*>(n + 48)->ToString();
    snprintf(b, sizeof(b), " (t%d)", type);
    line += b;
    if (type == 6 || type == 7)
    {
        const uint8_t* d = *reinterpret_cast<uint8_t**>(n + 56);
        const int len = *reinterpret_cast<int32_t*>(n + 64);
        line += " blob[" + std::to_string(len) + "]";
        for (int i = 0; d && i < len && i < (len > 48 ? 2400 : 48); ++i) { snprintf(b, sizeof(b), " %02X", d[i]); line += b; }
    }
    else if (type == 2) { snprintf(b, sizeof(b), " = %d", n[56]); line += b; }
    else if (type == 3) { snprintf(b, sizeof(b), " = %g / %g", *reinterpret_cast<float*>(n + 56), *reinterpret_cast<double*>(n + 56)); line += b; }
    else if (type == 4)
    {
        const wchar_t* w = *reinterpret_cast<wchar_t**>(n + 56);
        snprintf(b, sizeof(b), " = \"%.80ls\"", w ? w : L"");
        line += b;
    }
    out += line + "\n";
    if (type == 9)                                     // object node: its sub-containers are fields
    {
        for (int off : { 552, 560, 608, 616 })          // serverData, gameData, outgoingConnections, references
        {
            uint8_t* c = *reinterpret_cast<uint8_t**>(n + off);
            if (!c || IsBadReadPtr(c, 160) || c[24] > 12) continue;
            snprintf(b, sizeof(b), "%s[+%d]", std::string((depth + 1) * 2, ' ').c_str(), off);
            out += b;
            SbTreeWalk(c, depth + 1, out);
        }
        return;
    }
    if (type != 1) return;
    uint8_t** arr = *reinterpret_cast<uint8_t***>(n + 136);
    const int cnt = *reinterpret_cast<int32_t*>(n + 144);
    for (int i = 0; arr && i < cnt && i < 200; ++i) SbTreeWalk(arr[i], depth + 1, out);
}
static void SbTreeCore(uintptr_t node, std::string* out)
{
    __try { SbTreeWalk(reinterpret_cast<uint8_t*>(node), 0, *out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *out += "(fault)\n"; }
}
static void SeSandboxTree(const std::string& ident)
{
    if (!g_seLocalTest) return;
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) { HxLog("[HalcyonA2][SPECEDIT] SBTREE %s: not a sandbox object\n", ident.c_str()); return; }
    std::string out;
    SbTreeCore(SbNodeOf(reinterpret_cast<uint8_t*>(pc) + 0x3A0), &out);
    HxLog("[HalcyonA2][SPECEDIT] SBTREE %s:\n%s", a->GetName().c_str(), out.c_str());
}

static void SeSandboxDescDump(const std::string& ident)
{
    if (!g_seLocalTest) return;
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) { HxLog("[HalcyonA2][SPECEDIT] SBDESC %s: not a sandbox object\n", ident.c_str()); return; }
    char out[4096];
    SbDescDumpCore(SbNodeOf(reinterpret_cast<uint8_t*>(pc) + 0x3A0), out, sizeof(out));
    HxLog("[HalcyonA2][SPECEDIT] SBDESC %s: %s\n", a->GetName().c_str(), out);
}

static int SbObjectMapCount(SDK::UObject* sb)
{
    return sb ? *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(sb) + 0x2A0 + 8) : -1;
}


// ---- component defaults for a new object --------------------------------------------------------------
// A station object gets its components' default data when its project loads: each LuauBehavior template of
// the prefab adds its defaults to serverData (+1624: "Properties", or e.g. the light switch's
// default__isEnabled), then +1616 copies every default__X into gameData as X. Objects we add skipped that,
// and a switch's toggle reads gameData/isEnabled unchecked -- every player who JOINED afterwards crashed.
// Run the same two passes on the new node before it is attached. Templates come from the Blueprint's class
// defaults, the way the load path gets them (no live instance needed).
static int SbDefaultsCore(uint8_t* node, void* bpClass, void* lbClass)
{
    int step = 0;
    struct { void** data; int32_t num, max; } tpl{};
    __try
    {
        const uintptr_t base = GetBase();
        auto mal = reinterpret_cast<uint8_t*(__fastcall*)(size_t)>(base + SeSb::Malloc);
        auto rawAdd = reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t**)>(base + SeSb::RawAdd);
        uint8_t* sd = *reinterpret_cast<uint8_t**>(node + 552);
        uint8_t* gd = *reinterpret_cast<uint8_t**>(node + 560);
        if (!sd || !gd) return -100;
        step = 1;
        void* ac = bpClass; void* fc = lbClass;
        reinterpret_cast<void(__fastcall*)(void*, void**, void**)>(base + SeSb::Templates)(&tpl, &ac, &fc);
        step = 2;
        int moved = 0;
        for (int i = 0; tpl.data && i < tpl.num && i < 64; ++i)
        {
            void* t = tpl.data[i];
            if (!t || !static_cast<SDK::UObject*>(t)->IsA(static_cast<SDK::UClass*>(lbClass))) continue;
            uint8_t* holder = mal(160);
            memset(holder, 0, 160);
            uint64_t hn = *reinterpret_cast<uint64_t*>(sd + 48);
            reinterpret_cast<void(__fastcall*)(uint8_t*, uint64_t*, int)>(base + SeSb::ContainerCtor)(holder, &hn, 0);
            reinterpret_cast<void(__fastcall*)(uint8_t*)>(base + SeSb::ContainerStat)(holder);
            auto vt = *reinterpret_cast<void***>(t);
            reinterpret_cast<void(__fastcall*)(void*, uint8_t*)>(vt[1624 / 8])(t, holder);
            uint8_t** arr = *reinterpret_cast<uint8_t***>(holder + 136);
            const int n = *reinterpret_cast<int32_t*>(holder + 144);
            for (int k = 0; arr && k < n; ++k)
            {
                uint8_t* c = arr[k];
                if (!c) continue;
                uint8_t* existing = nullptr;
                if (SbFindChildRaw(sd, *reinterpret_cast<uint64_t*>(c + 48), &existing) >= 0) continue;   // first wins
                arr[k] = nullptr;
                rawAdd(sd, &c);
                ++moved;
            }
        }
        step = 3;
        for (int i = 0; tpl.data && i < tpl.num && i < 64; ++i)
        {
            void* t = tpl.data[i];
            if (!t || !static_cast<SDK::UObject*>(t)->IsA(static_cast<SDK::UClass*>(lbClass))) continue;
            auto vt = *reinterpret_cast<void***>(t);
            reinterpret_cast<void(__fastcall*)(void*, uint8_t*, uint8_t*)>(vt[1616 / 8])(t, sd, gd);
        }
        const int count = tpl.num;
        if (tpl.data) reinterpret_cast<void(__fastcall*)(void*)>(base + SeSb::MemFree)(tpl.data);
        return (count << 8) | (moved & 0xFF);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1 - step; }
}


// A fresh, detached type-6 leaf holding `blob` (engine-allocated), named `nameBits`.
static uint8_t* SbMakeLeaf(uint64_t nameBits, const uint8_t* blob, int len)
{
    const uintptr_t base = GetBase();
    auto mal = reinterpret_cast<uint8_t*(__fastcall*)(size_t)>(base + SeSb::Malloc);
    uint8_t* n = mal(80);
    memset(n, 0, 80);
    uint64_t nm = nameBits;
    reinterpret_cast<void(__fastcall*)(uint8_t*, uint64_t*, uint8_t)>(base + SeSb::LeafCtor)(n, &nm, 6);
    *reinterpret_cast<uintptr_t*>(n) = base + SeSb::LeafVtbl;
    uint8_t* d = mal(len > 0 ? len : 1);
    memcpy(d, blob, len);
    *reinterpret_cast<uint8_t**>(n + 56) = d;
    *reinterpret_cast<int32_t*>(n + 64) = len;
    *reinterpret_cast<int32_t*>(n + 68) = len;
    reinterpret_cast<void(__fastcall*)(uint8_t*)>(base + SeSb::LeafStat)(n);
    return n;
}

// Offline (node not attached yet): put a leaf into serverData or serverData/Properties, replacing the
// blob of an existing same-named leaf in place. 1 = ok.
static int SbOfflineLeafCore(uint8_t* sd, bool inProps, uint64_t propsBits, uint64_t nameBits, const uint8_t* blob, int len)
{
    __try
    {
        const uintptr_t base = GetBase();
        uint8_t* parent = sd;
        if (inProps && SbFindChildRaw(sd, propsBits, &parent) < 0) return -1;
        uint8_t* existing = nullptr;
        if (SbFindChildRaw(parent, nameBits, &existing) >= 0 && existing[24] == 6)
        {
            uint8_t* d = reinterpret_cast<uint8_t*(__fastcall*)(size_t)>(base + SeSb::Malloc)(len > 0 ? len : 1);
            memcpy(d, blob, len);
            *reinterpret_cast<uint8_t**>(existing + 56) = d;
            *reinterpret_cast<int32_t*>(existing + 64) = len;
            *reinterpret_cast<int32_t*>(existing + 68) = len;
            return 1;
        }
        uint8_t* leaf = SbMakeLeaf(nameBits, blob, len);
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t**)>(base + SeSb::RawAdd)(parent, &leaf);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -2; }
}

// Live (object attached): replace/add the leaf through the replicated AddChild path. 1 = ok.
static int SbLiveLeafCore(uint8_t* objHandle, bool inProps, uint64_t sdBits, uint64_t propsBits, uint64_t nameBits,
                          const uint8_t* blob, int len)
{
    int step = 0;
    uint8_t itSD[0x100] = {}, itP[0x100] = {};
    __try
    {
        const uintptr_t base = GetBase();
        auto find = reinterpret_cast<void*(__fastcall*)(void*, uint8_t*, uint64_t, char)>(base + SeSb::FindChild);
        find(objHandle, itSD, sdBits, 0);
        if (!itSD[0x48]) { NvReleaseIter(base, itSD); return -10; }
        step = 1;
        uint8_t* target = itSD;
        if (inProps)
        {
            find(itSD, itP, propsBits, 0);
            if (!itP[0x48]) { NvReleaseIter(base, itP); NvReleaseIter(base, itSD); return -11; }
            target = itP;
        }
        step = 2;
        // Remove the old leaf first. AddChild on an existing name left BOTH in the tree (seen live: two
        // Duration leaves after one edit), and a player joining later could read the stale one.
        uint8_t itOld[0x100] = {};
        find(target, itOld, nameBits, 0);
        if (itOld[0x48]) SbRemoveCore(itOld);
        else NvReleaseIter(base, itOld);
        uint8_t* leaf = SbMakeLeaf(nameBits, blob, len);
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t**)>(base + SeSb::AddChild)(target, &leaf);
        step = 3;
        if (inProps) NvReleaseIter(base, itP);
        NvReleaseIter(base, itSD);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -20 - step; }
}

static bool SbWriteLeaves(SDK::AActor* a, const std::vector<SbLeaf>& leaves)
{
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) return false;
    bool ok = true;
    for (const auto& l : leaves)
    {
        const int r = SbLiveLeafCore(reinterpret_cast<uint8_t*>(pc) + 0x3A0, l.inProps, NvNameBits(NvName("serverData")),
                                     NvNameBits(NvName("Properties")), NvNameBits(NvName(l.name)), l.blob.data(),
                                     static_cast<int>(l.blob.size()));
        HxLog("[HalcyonA2][SPECEDIT] leaf %s%s (%zu bytes) on %s: %d\n", l.inProps ? "Properties/" : "", l.name.c_str(),
              l.blob.size(), a->GetName().c_str(), r);
        ok = ok && r == 1;
    }
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
    SDK::AActor* slot = lgm ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320) : nullptr;
    if (slot) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    return ok;
}

// POD-only core (holds the SEH frame). Returns a step number reached; negative = fault at that step.
static void* g_sbDefBp = nullptr;      // set around SbAddCore: the prefab's Blueprint class (defaults fill)
static int   g_sbDefResult = 0;
static void* g_sbLbClass = nullptr;    // ULuauBehavior (looked up outside the SEH frame)
struct SbLeafPod { bool inProps; uint64_t props, name; const uint8_t* blob; int len; bool inRefs; };
static SbLeafPod g_sbLeafArr[16];      // POD view of g_sbPendingLeaves for the SEH core
static size_t    g_sbLeafCount = 0;
static int SbAddCore(void* lgmHandle, uint64_t objectsName, uint8_t* desc, uint64_t idxName)
{
    int step = 0;
    uint8_t iter[0x100] = {};
    __try
    {
        const uintptr_t base = GetBase();
        step = 1;
        reinterpret_cast<void*(__fastcall*)(void*, uint8_t*, uint64_t, char)>(base + SeSb::FindChild)(lgmHandle, iter, objectsName, 0);
        if (!iter[0x48]) return -100;                                   // no "objects" under this gamemode
        step = 2;
        void* node = reinterpret_cast<void*(__fastcall*)(size_t)>(base + SeSb::Malloc)(632);
        if (!node) return -101;
        memset(node, 0, 632);
        step = 3;
        reinterpret_cast<void(__fastcall*)(void*, uint64_t*, uint8_t*)>(base + SeSb::NodeBuild)(node, &idxName, desc);
        step = 4;
        reinterpret_cast<void(__fastcall*)(void*)>(base + SeSb::NodeFinish)(node);
        step = 5;
        g_sbDefResult = g_sbDefBp ? SbDefaultsCore(static_cast<uint8_t*>(node), g_sbDefBp, g_sbLbClass) : 0;
        if (g_sbDefBp && g_sbDefResult < 0) return -200 + g_sbDefResult;   // never attach a half-built object
        for (size_t i = 0; i < g_sbLeafCount; ++i)
            SbOfflineLeafCore(*reinterpret_cast<uint8_t**>(static_cast<uint8_t*>(node) + (g_sbLeafArr[i].inRefs ? 616 : 552)),
                              g_sbLeafArr[i].inRefs ? false : g_sbLeafArr[i].inProps,
                              g_sbLeafArr[i].props, g_sbLeafArr[i].name, g_sbLeafArr[i].blob, g_sbLeafArr[i].len);
        void* nodeRef = node;
        reinterpret_cast<void(__fastcall*)(uint8_t*, void**)>(base + SeSb::AddChild)(iter, &nodeRef);
        step = 6;
        NvReleaseIter(base, iter);
        return step;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -step; }
}


static SDK::UObject* SbEngine()
{
    static int32_t s_idx = -1;
    static SDK::UObject* s_sb = nullptr;
    if (s_sb && s_idx >= 0 && SDK::UObject::GObjects->GetByIndex(s_idx) == s_sb) return s_sb;
    s_sb = nullptr; s_idx = -1;
    auto* cls = SDK::UObject::FindClassFast("SandboxEngine");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; cls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(cls)) { s_sb = o; s_idx = i; break; }
    }
    return s_sb;
}

// Blueprint class name -> the sandbox prefab type (UObjectPrefabDefinitionSettings::UniqueID), from
// USandboxEngine::RawPrefabs (0xF8): definition +0x30 Blueprint class, +0x38 Settings, Settings +0xF0 UniqueID.
static std::string SbTypeFor(const std::string& className)
{
    static std::vector<std::pair<std::string, std::string>> s_map;
    if (s_map.empty())
        if (SDK::UObject* sb = SbEngine())
        {
            const uintptr_t b = reinterpret_cast<uintptr_t>(sb);
            SDK::UObject** rp = *reinterpret_cast<SDK::UObject***>(b + 0xF8);
            const int raw = *reinterpret_cast<int32_t*>(b + 0xF8 + 8);
            for (int i = 0; rp && i < raw && i < 1000; ++i)
            {
                SDK::UObject* def = rp[i];
                if (!def) continue;
                SDK::UObject* bp = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x30);
                SDK::UObject* st = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x38);
                if (bp && st) s_map.push_back({ bp->GetName(), reinterpret_cast<SDK::FName*>(reinterpret_cast<uintptr_t>(st) + 0xF0)->ToString() });
            }
        }
    for (const auto& e : s_map) if (e.first == className) return e.second;
    return std::string();
}

// The Blueprint class of a prefab type (RawPrefabs: definition +0x30 Blueprint, +0x38 Settings +0xF0 UniqueID).
static void* SbBlueprintFor(const std::string& uniqueId)
{
    SDK::UObject* sb = SbEngine();
    if (!sb) return nullptr;
    const uintptr_t b = reinterpret_cast<uintptr_t>(sb);
    SDK::UObject** rp = *reinterpret_cast<SDK::UObject***>(b + 0xF8);
    const int raw = *reinterpret_cast<int32_t*>(b + 0xF8 + 8);
    for (int i = 0; rp && i < raw && i < 1000; ++i)
    {
        SDK::UObject* def = rp[i];
        if (!def) continue;
        SDK::UObject* st = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(def) + 0x38);
        if (st && reinterpret_cast<SDK::FName*>(reinterpret_cast<uintptr_t>(st) + 0xF0)->ToString() == uniqueId)
            return *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(def) + 0x30);
    }
    return nullptr;
}

static bool SeSandboxClassOk(const std::string& className)
{
    return g_seSandbox && !SbTypeFor(className).empty();
}

// Which loaded gamemode should host a new object -- decided by WHERE it is being placed.
//
// A game area only draws its own objects: one hosted by a different area shows up for a moment when it is
// created and is hidden again as soon as the client settles, which looked like "items spawn in and vanish".
// (Re-showing it with a script's SetVisible was the only way back.) It also split objects placed side by
// side across areas, so wiring one to another was refused as cross-area.
//
// So: the area whose slot is nearest the spawn point wins. Among areas the same distance away -- the usual
// case is one -- an area that already hosts this prefab type is preferred, because a prefab whose behaviour
// comes from a Luau script needs a project that carries it.
// ---- culling: module slots with an importance volume -------------------------------------------------
// The culling system only treats a gamemode's objects as important where its slot is a
// BP_ModuleSlotWithImportanceVolume_C and they sit inside that slot's ImportanceVolume box (+0x458).
static SDK::UBoxComponent* SbImportanceBox(SDK::AActor* slot)
{
    static SDK::UClass* cls = nullptr;
    if (!cls) cls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    if (!slot || !cls || !slot->IsA(cls)) return nullptr;
    return *reinterpret_cast<SDK::UBoxComponent**>(reinterpret_cast<uintptr_t>(slot) + 0x458);
}
// How far (cm, world) a point is outside the box; 0 = inside.
static double SbOutsideBox(SDK::UBoxComponent* box, const double* p)
{
    const SDK::FTransform xf = box->K2_GetComponentToWorld();
    const SDK::FVector l = SDK::UKismetMathLibrary::InverseTransformLocation(xf, SDK::FVector{ p[0], p[1], p[2] });
    const SDK::FVector e = box->GetUnscaledBoxExtent();
    const double dx = (std::max)(0.0, fabs(l.X) - e.X) * xf.Scale3D.X;
    const double dy = (std::max)(0.0, fabs(l.Y) - e.Y) * xf.Scale3D.Y;
    const double dz = (std::max)(0.0, fabs(l.Z) - e.Z) * xf.Scale3D.Z;
    return sqrt(dx * dx + dy * dy + dz * dz);
}
// SE|SLOTS[|x,y,z] (local test): every loaded gamemode's slot -- class, importance box, and how far the point is from it.
static SDK::UObject* SbGamemodesManager();   // below
// A map-placed module slot by its SlotID (every client has these, with their real IDs and boxes).
static SDK::AActor* SeMapSlotById(const std::string& id)
{
    auto* slotCls = SDK::UObject::FindClassFast("ModuleSlot");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; slotCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(slotCls)) continue;
        const wchar_t* sid = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(o) + 0x390);
        if (!sid) continue;
        std::string s8;
        for (const wchar_t* w = sid; *w; ++w) s8 += static_cast<char>(*w);
        if (s8 == id) return static_cast<SDK::AActor*>(o);
    }
    return nullptr;
}
// Load <project> into an EMPTY map slot at runtime: set DefaultGamemodePath, then GamemodesManager::AddSlot again.
// AddSlot (0x46C9580) loads the path when it is set (the slot is already known under its own id, so it doesn't bail).
static SDK::UObject* SeLoadMapSlot(const std::string& id, const std::string& path)
{
    SDK::AActor* slot = SeMapSlotById(id);
    SDK::UObject* mgr = SbGamemodesManager();
    if (!slot || !mgr) { HxLog("[HalcyonA2][SPECEDIT] LOADSLOT %s: %s\n", id.c_str(), slot ? "no gamemodes manager" : "no such slot"); return nullptr; }
    SDK::UObject* before = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x440);
    if (before) { HxLog("[HalcyonA2][SPECEDIT] LOADSLOT %s: already has %s\n", id.c_str(), before->GetName().c_str()); return before; }
    const std::wstring wpath(path.begin(), path.end());
    SetSlotFString(slot, 0x418, wpath.c_str());
    struct { SDK::AActor* s; } parms{ slot };
    if (auto* fn = mgr->Class->GetFunction("GamemodesManager", "AddSlot")) SafeProcessEvent(mgr, fn, &parms);
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x440);
    if (lgm)
    {
        if (auto* fn = slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(slot, fn, nullptr);
        if (auto* fn = lgm->Class->GetFunction("LoadedGameMode", "PushNetVars")) SafeProcessEvent(lgm, fn, nullptr);
    }
    SDK::UObject* tm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x438);
    HxLog("[HalcyonA2][SPECEDIT] LOADSLOT %s <- '%s': LoadedGameMode=%s TicketManager=%s\n", id.c_str(), path.c_str(),
          lgm ? lgm->GetName().c_str() : "none", tm ? tm->GetName().c_str() : "none");
    return lgm;
}
static void SeSlotsDump(const std::string& at)
{
    if (!g_seLocalTest) return;
    double p[3]{};
    const bool havePt = SeVec(at, p);
    auto* cls = SDK::UObject::FindClassFast("LoadedGameMode");
    auto* pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    std::unordered_map<SDK::UObject*, int> hosted;       // gamemodes that host sandbox objects (candidates today)
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; pcCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pcCls)) continue;
        if (SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(o) + 0x440)) ++hosted[lgm];
    }
    for (int32_t i = 0; cls && i < n; ++i)
    {
        SDK::UObject* l = SDK::UObject::GObjects->GetByIndex(i);
        if (!l || l->IsDefaultObject() || !l->IsA(cls)) continue;
        uint8_t* root = reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(l) + 0x218)));
        const std::string mode = root ? reinterpret_cast<SDK::FName*>(root + 48)->ToString() : std::string("?");
        SDK::AActor* slot = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(l) + 0x320);
        char b[512];
        if (!slot) { HxLog("[HalcyonA2][SPECEDIT] SLOT %s (%s): no slot\n", l->GetName().c_str(), mode.c_str()); continue; }
        const SDK::FVector sl = slot->K2_GetActorLocation();
        std::string boxs = "no importance volume";
        if (SDK::UBoxComponent* box = SbImportanceBox(slot))
        {
            const SDK::FVector c = box->K2_GetComponentLocation(), e = box->GetScaledBoxExtent();
            const SDK::FVector u = box->GetUnscaledBoxExtent(), cs = box->K2_GetComponentScale(), as = slot->GetActorScale3D();
            snprintf(b, sizeof(b), "IMPORTANCE box centre (%.0f,%.0f,%.0f) half-size (%.0f,%.0f,%.0f) [unscaled %.0f,%.0f,%.0f comp scale %.2f,%.2f,%.2f actor scale %.2f]%s",
                     c.X, c.Y, c.Z, e.X, e.Y, e.Z, u.X, u.Y, u.Z, cs.X, cs.Y, cs.Z, as.X,
                     havePt ? (" -- point " + std::to_string(static_cast<int>(SbOutsideBox(box, p))) + "cm outside").c_str() : "");
            boxs = b;
        }
        const wchar_t* sid = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(slot) + 0x390);   // SlotID
        const wchar_t* gmp = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(slot) + 0x418);   // DefaultGamemodePath
        const std::wstring wsid = sid ? sid : L"", wgmp = gmp ? gmp : L"";
        snprintf(b, sizeof(b), "SLOT %s (%s): %s %s id '%s' path '%s' at (%.0f,%.0f,%.0f), %d sandbox object(s); %s",
                 l->GetName().c_str(), mode.c_str(), slot->Class->GetName().c_str(), slot->GetName().c_str(),
                 std::string(wsid.begin(), wsid.end()).c_str(), std::string(wgmp.begin(), wgmp.end()).c_str(), sl.X, sl.Y, sl.Z,
                 hosted.count(l) ? hosted[l] : 0, boxs.c_str());
        HxLog("[HalcyonA2][SPECEDIT] %s\n", b);
        {   // teams: MaximumTeamSizes (+0x428 TArray<int32>) and the TicketManager (+0x438)
            const int32_t* ts = *reinterpret_cast<int32_t* const*>(reinterpret_cast<uintptr_t>(slot) + 0x428);
            const int tn = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(slot) + 0x430);
            std::string sizes;
            for (int k = 0; ts && k < tn && k < 16; ++k) sizes += (k ? "," : "") + std::to_string(ts[k]);
            SDK::UObject* tm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x438);
            HxLog("[HalcyonA2][SPECEDIT]   teams of %s: MaximumTeamSizes=[%s] TicketManager=%s\n", slot->GetName().c_str(), sizes.c_str(),
                  tm ? tm->GetName().c_str() : "none");
        }
    }
    // Slots with NO game area loaded (map-placed, so every client has them with their real IDs and boxes).
    auto* slotCls = SDK::UObject::FindClassFast("ModuleSlot");
    for (int32_t i = 0; slotCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(slotCls)) continue;
        if (*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x440)) continue;   // has a LoadedGameMode
        auto* slot = static_cast<SDK::AActor*>(o);
        const wchar_t* sid = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(o) + 0x390);
        const std::wstring wsid = sid ? sid : L"";
        const SDK::FVector sl = slot->K2_GetActorLocation();
        char b[400];
        std::string boxs = "no importance volume";
        if (SDK::UBoxComponent* box = SbImportanceBox(slot))
        {
            const SDK::FVector c = box->K2_GetComponentLocation(), e = box->GetScaledBoxExtent();
            snprintf(b, sizeof(b), "IMPORTANCE box centre (%.0f,%.0f,%.0f) half-size (%.0f,%.0f,%.0f)%s", c.X, c.Y, c.Z, e.X, e.Y, e.Z,
                     havePt ? (" -- point " + std::to_string(static_cast<int>(SbOutsideBox(box, p))) + "cm outside").c_str() : "");
            boxs = b;
        }
        snprintf(b, sizeof(b), "EMPTY SLOT '%s': %s %s at (%.0f,%.0f,%.0f); %s", std::string(wsid.begin(), wsid.end()).c_str(),
                 slot->Class->GetName().c_str(), slot->GetName().c_str(), sl.X, sl.Y, sl.Z, boxs.c_str());
        HxLog("[HalcyonA2][SPECEDIT] %s\n", b);
    }
}

// Which loaded gamemode hosts a new object: the module slot whose ImportanceVolume the spot is inside, so the
// culling system treats the object as important wherever that module is (objects hosted in a module whose
// volume doesn't contain them were culled -- the old "nearest slot origin" rule picked, e.g., a tackleball
// training pod 44 m away for a spot in the middle of TKBGolf's volume). In order:
//   0. a BP_ModuleSlotWithImportanceVolume_C (exactly) whose box contains the spot -- the smallest one;
//   1. any importance-volume slot (JakeBall / Tackleball subclasses) whose box contains it -- the smallest;
//   2. the importance box the spot is nearest to (it will be culled out there: the editor is told);
//   3. no importance slot at all: the nearest slot origin.
// Only gamemodes that already host sandbox objects are candidates (they carry the "objects" container).
// SE|SLOTBOX (local test): the importance box a RUNTIME-spawned slot gets (class defaults -- what every client
// uses, since the box size doesn't replicate). Spawned server-only, measured, destroyed.
static void SeSlotBoxProbe()
{
    if (!g_seLocalTest) return;
    auto* cls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    auto* world = SDK::UWorld::GetWorld();
    if (!cls || !world) return;
    SDK::FTransform xf{}; xf.Rotation = SDK::FQuat{ 0, 0, 0, 1 }; xf.Translation = SDK::FVector{ 0, 0, 100000 }; xf.Scale3D = SDK::FVector{ 1, 1, 1 };
    SDK::AActor* s = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                                                                           nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!s) { HxLog("[HalcyonA2][SPECEDIT] SLOTBOX: spawn failed\n"); return; }
    SDK::UGameplayStatics::FinishSpawningActor(s, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (SDK::UBoxComponent* box = SbImportanceBox(s))
    {
        const SDK::FVector u = box->GetUnscaledBoxExtent(), e = box->GetScaledBoxExtent(), c = box->K2_GetComponentLocation();
        HxLog("[HalcyonA2][SPECEDIT] SLOTBOX default importance box: unscaled (%.0f,%.0f,%.0f) scaled (%.0f,%.0f,%.0f), centre offset (%.0f,%.0f,%.0f)\n",
              u.X, u.Y, u.Z, e.X, e.Y, e.Z, c.X, c.Y, c.Z - 100000);
    }
    else HxLog("[HalcyonA2][SPECEDIT] SLOTBOX: spawned slot has no importance box\n");
    s->K2_DestroyActor();
}

// ---- an editor-made module slot, for building outside every importance volume ---------------------------
// Spawned like the deathrun runtime slot (SpawnSlotAndLoad): a replicated BP_ModuleSlotWithImportanceVolume_C
// loaded through GamemodesManager.AddSlot. Two limits come from the game: a runtime slot's SlotID and box size
// don't replicate -- every client sees SlotID "defaultslot" and the class-default box (~78 x 95 x 19 m) -- so
// there can be ONE such slot, and its box is that default size, centred on the spot it was made for.
static SDK::AActor* g_seEditorSlot = nullptr;
static int          g_seEditorSlotState = 0;      // 0 none, 1 loading, 2 ready, -1 failed
static ULONGLONG    g_seEditorSlotAt = 0;
static ULONGLONG    g_seEditorSlotClearAt = 0;   // when to remove the project's starter objects
static wchar_t      g_seEditorSlotPath[96] = L"";  // persistent: the slot's FString points at it
static const double kSlotBoxOffset[3] = { -38.0, 340.0, 434.0 };   // default box centre relative to the slot (measured)
// The station is a cylinder spinning about the world Y axis (x = 0, z = 0): its floor is the inside of the hull, and
// "up" anywhere on it points at the axis -- +Z on the bottom floor (x ~ 0), +X on the wall at x ~ -28600. Game mode
// areas and team doors are turned to that up, so a mode can sit on any part of the floor, walls included.
static void SeStationUp(const double* at, double up[3])
{
    const double r = sqrt(at[0] * at[0] + at[2] * at[2]);
    if (r < 1000.0) { up[0] = 0; up[1] = 0; up[2] = 1; return; }   // near the axis: no floor to stand on, keep +Z
    up[0] = -at[0] / r; up[1] = 0; up[2] = -at[2] / r;
}
static SDK::FQuat SeStationUpQuat(const double* at)       // turns +Z to the local up (about the world Y axis)
{
    double up[3]; SeStationUp(at, up);
    const double a = atan2(up[0], up[2]);
    return SDK::FQuat{ 0.0, sin(a * 0.5), 0.0, cos(a * 0.5) };
}
static SDK::FRotator SeStationUpRot(const double* at) { return SDK::UKismetMathLibrary::Quat_Rotator(SeStationUpQuat(at)); }
static SDK::UObject* SbGamemodesManager()
{
    static SDK::UClass* cls = nullptr;
    if (!cls) cls = SDK::UObject::FindClassFast("GamemodesManager");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; cls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(cls)) return o;
    }
    return nullptr;
}
static bool SbCreateEditorSlot(const double* at, const std::string& path)
{
    if (g_seEditorSlot || g_seEditorSlotState != 0) return false;
    auto* cls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    auto* world = SDK::UWorld::GetWorld();
    SDK::UObject* mgr = SbGamemodesManager();
    if (!cls || !world || !mgr) { HxLog("[HalcyonA2][SPECEDIT] editor slot: not ready (cls=%p world=%p mgr=%p)\n", cls, world, mgr); return false; }
    SDK::FTransform xf{};
    xf.Rotation = SDK::FQuat{ 0, 0, 0, 1 };
    xf.Translation = SDK::FVector{ at[0] - kSlotBoxOffset[0], at[1] - kSlotBoxOffset[1], at[2] - kSlotBoxOffset[2] };
    xf.Scale3D = SDK::FVector{ 1, 1, 1 };
    SDK::AActor* slot = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                                                                              nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!slot) { HxLog("[HalcyonA2][SPECEDIT] editor slot: spawn failed\n"); return false; }
    // The path must be in place BEFORE the spawn finishes: the slot registers itself (GamemodesManager::AddSlot,
    // native 0x46C9580) during BeginPlay, and AddSlot only loads DefaultGamemodePath if it is set right then.
    // Set afterwards, the slot was already registered empty and a second AddSlot changed nothing.
    const std::wstring wp(path.begin(), path.end());
    wcsncpy_s(g_seEditorSlotPath, wp.c_str(), _TRUNCATE);
    SetSlotFString(slot, 0x418, g_seEditorSlotPath);                       // DefaultGamemodePath
    SetSlotFString(slot, 0x390, L"RigelEditorArea_Full");                  // SlotID: "Full" = always visible (see SbCreateSlotWithId)
    const uintptr_t s = reinterpret_cast<uintptr_t>(slot);
    SDK::UObject* mgrPre = mgr;
    const int32_t gmBefore = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(mgrPre) + 0x368 + 8);
    ++g_lodForceVisible;                                                   // its project's DefaultLODSettings -> VisibleAlways
    SDK::UGameplayStatics::FinishSpawningActor(slot, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    --g_lodForceVisible;
    HxLog("[HalcyonA2][SPECEDIT] editor slot: after spawn, loaded game areas %d -> %d, LoadedGameMode=%p\n", gmBefore,
          *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(mgrPre) + 0x368 + 8), *reinterpret_cast<void**>(s + 0x440));
    if (auto* fn = slot->Class->GetFunction("Actor", "SetReplicates")) { struct { bool b; } rp{ true }; SafeProcessEvent(slot, fn, &rp); }
    *reinterpret_cast<uint8_t*>(s + 0x60) |= 0x08 | 0x10;                 // bAlwaysRelevant | bReplicateMovement
    *reinterpret_cast<float*>(s + 0x170) = 1.0e12f;                        // NetCullDistanceSquared: never cull the slot itself
    if (auto* fn = slot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(slot, fn, nullptr);
    if (auto* fn = slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(slot, fn, nullptr);
    g_seEditorSlot = slot; g_seEditorSlotState = 1; g_seEditorSlotAt = GetTickCount64();
    HxLog("[HalcyonA2][SPECEDIT] editor slot: spawned at (%.0f,%.0f,%.0f) for a box centred on (%.0f,%.0f,%.0f), loading '%s'\n",
          xf.Translation.X, xf.Translation.Y, xf.Translation.Z, at[0], at[1], at[2], path.c_str());
    SbEditorSlotTick(GetTickCount64());                    // the load binds during the spawn: usually ready right now
    return true;
}
// A runtime slot with its OWN SlotID (set before the spawn finishes, like the path), loading `path`. Used by the
// game mode fabricator: one slot per game mode, so each gets its own gamemode, ticket manager and teams.
static std::vector<std::wstring*> g_seSlotStrings;      // FString buffers must outlive the slot (never freed)
static SDK::AActor* SbCreateSlotWithId(const double* at, const std::string& path, const std::string& slotId, const int* teamSizes, int nTeams)
{
    auto* cls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    auto* world = SDK::UWorld::GetWorld();
    SDK::UObject* mgr = SbGamemodesManager();
    if (!cls || !world || !mgr) return nullptr;
    SDK::FTransform xf{};
    xf.Rotation = SeStationUpQuat(at);                                 // the area stands on the local floor (walls too)
    const SDK::FVector off = SDK::UKismetMathLibrary::Quat_RotateVector(xf.Rotation, SDK::FVector{ kSlotBoxOffset[0], kSlotBoxOffset[1], kSlotBoxOffset[2] });
    xf.Translation = SDK::FVector{ at[0] - off.X, at[1] - off.Y, at[2] - off.Z };
    xf.Scale3D = SDK::FVector{ 1, 1, 1 };
    SDK::AActor* slot = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                                                                              nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!slot) return nullptr;
    auto* wp = new std::wstring(path.begin(), path.end());
    // "_Full": AModuleSlot::BeginPlay (native 0x46CBD00) sets bAllObjectsVisibleAtAllTimes (+0x2FC) on any slot whose
    // SlotID contains "Full" -- that is why the station's TKB_*_Full slots always draw. The SlotID replicates when set
    // before the spawn finishes, so every client (vanilla Quest included) runs it on its own copy. Without it, Quest
    // draws a runtime slot's objects only if they existed when the player walked into its box: anything placed or
    // moved while someone stands inside was collision-only (invisible) for them until they left and came back.
    const std::string fullId = slotId.find("Full") == std::string::npos ? slotId + "_Full" : slotId;
    auto* wid = new std::wstring(fullId.begin(), fullId.end());
    g_seSlotStrings.push_back(wp); g_seSlotStrings.push_back(wid);
    SetSlotFString(slot, 0x418, wp->c_str());                          // DefaultGamemodePath
    SetSlotFString(slot, 0x390, wid->c_str());                         // SlotID (replicated)
    if (teamSizes && nTeams > 0)                                       // MaximumTeamSizes (TArray<int32> @0x428)
    {
        int32_t* arr = *reinterpret_cast<int32_t**>(reinterpret_cast<uintptr_t>(slot) + 0x428);
        const int32_t num = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(slot) + 0x430);
        const int32_t max = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(slot) + 0x434);
        if (arr && max >= nTeams) { for (int k = 0; k < nTeams; ++k) arr[k] = teamSizes[k]; *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(slot) + 0x430) = nTeams; }
        else if (arr && num > 0) for (int k = 0; k < num && k < nTeams; ++k) arr[k] = teamSizes[k];
    }
    ++g_lodForceVisible;                                               // its project's DefaultLODSettings -> VisibleAlways
    SDK::UGameplayStatics::FinishSpawningActor(slot, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    --g_lodForceVisible;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(slot);
    if (auto* f = slot->Class->GetFunction("Actor", "SetReplicates")) { struct { bool b; } rp{ true }; SafeProcessEvent(slot, f, &rp); }
    *reinterpret_cast<uint8_t*>(sp + 0x60) |= 0x08 | 0x10;               // bAlwaysRelevant | bReplicateMovement
    *reinterpret_cast<float*>(sp + 0x170) = 1.0e12f;
    if (auto* f = slot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(slot, f, nullptr);
    if (auto* f = slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(slot, f, nullptr);
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(sp + 0x440);
    if (lgm) if (auto* f = lgm->Class->GetFunction("LoadedGameMode", "PushNetVars")) SafeProcessEvent(lgm, f, nullptr);
    SDK::UObject* tm = *reinterpret_cast<SDK::UObject**>(sp + 0x438);
    HxLog("[HalcyonA2][SPECEDIT] slot '%s' <- '%s' at (%.0f,%.0f,%.0f): LoadedGameMode=%s TicketManager=%s\n", slotId.c_str(), path.c_str(),
          at[0], at[1], at[2], lgm ? lgm->GetName().c_str() : "none", tm ? tm->GetName().c_str() : "none");
    g_seFabSlots.push_back(slot);
    return slot;
}
// Tick: once the load binds a LoadedGameMode, push the slot's and gamemode's netvars so clients build it.
static void SbEditorSlotTick(ULONGLONG now)
{
    static bool s_in = false;
    if (s_in || g_seEditorSlotState != 1 || !g_seEditorSlot) return;
    struct Guard { bool& b; Guard(bool& x) : b(x) { b = true; } ~Guard() { b = false; } } guard(s_in);
    const uintptr_t s = reinterpret_cast<uintptr_t>(g_seEditorSlot);
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(s + 0x440);
    if (!lgm)
    {
        static ULONGLONG s_diag = 0;
        if (now - g_seEditorSlotAt > 3000 && now - s_diag > 8000)
        {
            s_diag = now;
            if (SDK::UObject* mgr = SbGamemodesManager())
            {
                const uintptr_t m = reinterpret_cast<uintptr_t>(mgr);
                auto mapNum = [&](uint32_t off) { return *reinterpret_cast<int32_t*>(m + off + 8) - *reinterpret_cast<int32_t*>(m + off + 0x34); };
                HxLog("[HalcyonA2][SPECEDIT] editor slot: waiting -- mgr knownSlots=%d waitingForSlots=%d loadedSlots=%d loadedGamemodes=%d "
                      "slot.ModuleState=%p slot.SlotID='%ls'\n", mapNum(0x68), mapNum(0x2B0), mapNum(0x310), *reinterpret_cast<int32_t*>(m + 0x368 + 8),
                      *reinterpret_cast<void**>(s + 0x300), *reinterpret_cast<const wchar_t**>(s + 0x390));
            }
        }
        if (now - g_seEditorSlotAt > 30000) { g_seEditorSlotState = -1; HxLog("[HalcyonA2][SPECEDIT] editor slot: its game area never loaded\n"); }
        return;
    }
    if (auto* fn = g_seEditorSlot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(g_seEditorSlot, fn, nullptr);
    if (auto* fn = lgm->Class->GetFunction("LoadedGameMode", "PushNetVars")) SafeProcessEvent(lgm, fn, nullptr);
    if (auto* fn = g_seEditorSlot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(g_seEditorSlot, fn, nullptr);
    uint8_t* root = reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(lgm) + 0x218)));
    uint8_t* objs = nullptr;
    const bool hasObjects = root && SbFindChildRaw(root, NvNameBits(NvName("objects")), &objs) >= 0;
    g_seEditorSlotState = hasObjects ? 2 : -1;
    g_seEditorSlotClearAt = GetTickCount64() + 5000;     // starter objects go once it has settled (see below)
    HxLog("[HalcyonA2][SPECEDIT] editor slot: game area %s loaded (%s), objects container: %s\n", lgm->GetName().c_str(),
          root ? reinterpret_cast<SDK::FName*>(root + 48)->ToString().c_str() : "?", hasObjects ? "yes -- ready" : "NO -- can't hold objects");
}
// The project the area is loaded from ships a few objects of its own (Testing/TempProject123: three bridge
// meshes, no scripts). Remove them -- but not straight after the load: removing nodes while the fresh load
// was still settling faulted in the node code and sent the path walk into the parent-cycle recursion
// (stack overflow at +0x4665FAB). A few seconds later they are ordinary nodes. Our own objects are kept.
static void SbEditorSlotClearTick(ULONGLONG now)
{
    if (!g_seEditorSlotClearAt || now < g_seEditorSlotClearAt) return;
    g_seEditorSlotClearAt = 0;
    SDK::UObject* lgm = (g_seEditorSlotState == 2 && g_seEditorSlot && SeAlive(g_seEditorSlot))
                            ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(g_seEditorSlot) + 0x440) : nullptr;
    if (!lgm) return;
    // Through each object's server actor (its prefab component's own handle), the way a station object is
    // deleted cleanly -- walking to the node by name from the root faulted in NodeRemove for these.
    auto* pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    std::vector<SDK::AActor*> doomed;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; pcCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pcCls)) continue;
        if (*reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(o) + 0x440) != lgm) continue;
        if (SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(o) + 0x248)))) continue;   // ours: keep
        if (o->Outer && SeAlive(o->Outer)) doomed.push_back(static_cast<SDK::AActor*>(o->Outer));
    }
    int cleared = 0;
    for (SDK::AActor* a : doomed) if (SeSandboxDelete(a)) ++cleared;
    if (auto* fn = g_seEditorSlot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(g_seEditorSlot, fn, nullptr);
    HxLog("[HalcyonA2][SPECEDIT] editor slot: %d starter object(s) removed\n", cleared);
}
// SE|PROJPROBE|<path> (local test): load a project into a throwaway slot with its own id, far away, and report
// what it brings (objects, scripts) -- to pick an empty project for the editor area.
static void SeProjProbe(const std::string& path)
{
    if (!g_seLocalTest) return;
    static int s_n = 0;
    static wchar_t s_paths[16][96], s_ids[16][32];
    if (s_n >= 16) return;
    auto* cls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    auto* world = SDK::UWorld::GetWorld();
    if (!cls || !world) return;
    SDK::FTransform xf{}; xf.Rotation = SDK::FQuat{ 0, 0, 0, 1 };
    xf.Translation = SDK::FVector{ 200000.0 + 20000.0 * s_n, 200000.0, 0 }; xf.Scale3D = SDK::FVector{ 1, 1, 1 };
    SDK::AActor* slot = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(world, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
                                                                              nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!slot) return;
    const std::wstring wp(path.begin(), path.end());
    wcsncpy_s(s_paths[s_n], wp.c_str(), _TRUNCATE);
    swprintf_s(s_ids[s_n], L"probe_%d", s_n);
    SetSlotFString(slot, 0x418, s_paths[s_n]);
    SetSlotFString(slot, 0x390, s_ids[s_n]);
    ++s_n;
    SDK::UGameplayStatics::FinishSpawningActor(slot, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x440);
    int objects = -1, scripts = -1;
    std::string types;
    if (lgm)
    {
        uint8_t* root = reinterpret_cast<uint8_t*>(SbNodeOf(reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(lgm) + 0x218)));
        uint8_t* c = nullptr;
        if (root && SbFindChildRaw(root, NvNameBits(NvName("objects")), &c) >= 0)
        {
            objects = *reinterpret_cast<int32_t*>(c + 144);
            uint8_t** arr = *reinterpret_cast<uint8_t***>(c + 136);
            for (int k = 0; arr && k < objects && k < 6; ++k)
                if (arr[k]) if (void* d = reinterpret_cast<void*(__fastcall*)(uintptr_t)>(GetBase() + SeSb::NodeDesc)(reinterpret_cast<uintptr_t>(arr[k])))
                    types += reinterpret_cast<SDK::FName*>(d)->ToString() + " ";
        }
        if (root && SbFindChildRaw(root, NvNameBits(NvName("Scripts")), &c) >= 0) scripts = *reinterpret_cast<int32_t*>(c + 144);
    }
    HxLog("[HalcyonA2][SPECEDIT] PROJPROBE '%s': %s, %d object(s), %d script(s) %s\n", path.c_str(), lgm ? "loaded" : "DID NOT LOAD",
          objects, scripts, types.c_str());
}

static SDK::UObject* SbEditorSlotLgm()   // the editor area's gamemode once it is ready, else null
{
    if (g_seEditorSlotState != 2 || !g_seEditorSlot || !SeAlive(g_seEditorSlot)) return nullptr;
    return *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(g_seEditorSlot) + 0x440);
}
// The project loaded into the editor area. Probed (SE|PROJPROBE): Testing/TestingProject20 loads with no objects
// and no scripts. (TempProject123 brings 3 bridge meshes that can't be removed safely; UnitTests/EmptyBase and
// SelectionBase fault in the loader; the rest bring cubes, bridges, team changers or scripts.)
static const char kSeEditorProject[] = "Testing/TestingProject20";


// Always-visible areas made for spots no "Full" slot covers (see the end of SbHostGamemode).
static std::vector<SDK::AActor*> g_seAreaSlots;
static SDK::UObject* SbFullAreaFor(const double* loc)
{
    for (SDK::AActor* a : g_seAreaSlots)
    {
        if (!a || !SeAlive(a)) continue;
        SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(a) + 0x440);
        SDK::UBoxComponent* box = SbImportanceBox(a);
        if (lgm && box && SbOutsideBox(box, loc) <= 1.0) return lgm;
    }
    if (g_seAreaSlots.size() >= 32) return nullptr;
    const std::string id = "RigelArea" + std::to_string(g_seAreaSlots.size() + 1);
    SDK::AActor* slot = SbCreateSlotWithId(loc, kSeEditorProject, id, nullptr, 0);   // SlotID gets "_Full"
    if (!slot) return nullptr;
    g_seFabSlots.erase(std::remove(g_seFabSlots.begin(), g_seFabSlots.end(), slot), g_seFabSlots.end());   // not a game mode
    g_seAreaSlots.push_back(slot);
    return *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x440);
}

static SDK::UObject* SbHostGamemode(const std::string& uniqueId, const double* loc)
{
    static SDK::UClass* exactCls = nullptr;
    if (!exactCls) exactCls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    auto* pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    std::unordered_set<SDK::UObject*> seen;
    SDK::UObject* best = nullptr; int bestTier = 99; double bestScore = 1.0e300;
    std::string bestWhy;
    std::vector<SDK::UObject*> cands;
    // A fabricated game mode owns everything inside its box: checked first, whatever the object index holds.
    for (SDK::AActor* fs : g_seFabSlots)
    {
        if (!fs || !SeAlive(fs)) continue;
        SDK::UObject* flgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(fs) + 0x440);
        SDK::UBoxComponent* fbox = SbImportanceBox(fs);
        if (flgm && fbox && SbOutsideBox(fbox, loc) <= 1.0)
        {
            HxLog("[HalcyonA2][SPECEDIT] host for %s at (%.0f,%.0f,%.0f): %s (game mode %ls)\n", uniqueId.c_str(), loc[0], loc[1], loc[2],
                  flgm->GetName().c_str(), *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(fs) + 0x390));
            g_sbHostOutsideCm = 0.0;
            return flgm;
        }
    }
    if (SDK::UObject* ed = SbEditorSlotLgm()) { seen.insert(ed); cands.push_back(ed); }   // empty, so no PrefabComponents
    for (SDK::UObject* o : ClassObjects(pcCls))           // [PERF] shared index: this runs for every placed object
    {
        if (!o || o->IsDefaultObject()) continue;
        SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(o) + 0x440);
        if (lgm && seen.insert(lgm).second) cands.push_back(lgm);
    }
    for (SDK::UObject* lgm : cands)
    {
        SDK::AActor* slot = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320);
        int tier = 3; double score = 1.0e299;
        if (SDK::UBoxComponent* box = slot ? SbImportanceBox(slot) : nullptr)
        {
            const double out = SbOutsideBox(box, loc);
            const SDK::FVector e = box->GetScaledBoxExtent();
            if (out <= 1.0)
            {
                tier = slot->Class == exactCls ? 0 : 1;
                score = e.X * e.Y * e.Z;                                               // smallest area wins...
                if (!*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(slot) + 0x2FC)) score += 1.0e200;   // ...always-visible first
            }
            else            { tier = 2; score = out; }
        }
        else if (slot)
        {
            const SDK::FVector s = slot->GetTransform().Translation;
            const double dx = s.X - loc[0], dy = s.Y - loc[1], dz = s.Z - loc[2];
            score = dx * dx + dy * dy + dz * dz;
        }
        if (tier < bestTier || (tier == bestTier && score < bestScore))
        {
            best = lgm; bestTier = tier; bestScore = score;
            bestWhy = slot ? slot->Class->GetName() : std::string("no slot");
        }
    }
    // Outside every importance volume: make the editor's own module area here (once per server run -- see
    // SbCreateEditorSlot) and put the object in it.
    if (bestTier >= 2 && g_specEdit && g_seEditorSlotState == 0 && SbCreateEditorSlot(loc, kSeEditorProject))
        if (SDK::UObject* ed = SbEditorSlotLgm())
        {
            best = ed; bestTier = 0; bestScore = 0;
            bestWhy = "editor area, created for this spot";
        }
    // Quest draws a sandbox object only if its host slot is always-visible ("Full" SlotID -> SpawnPrefab calls
    // SetHidden(false) on it) or the player walked into that slot's box AFTER it spawned. Any other host leaves it
    // collision-only on Quest (the PC spec builds draw it anyway). So never settle for such a host: use -- or make --
    // an always-visible area around the spot.
    {
        SDK::AActor* bs = best ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(best) + 0x320) : nullptr;
        const bool full = bs && SeAlive(bs) && *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(bs) + 0x2FC);
        if (g_specEdit && !(full && bestTier <= 1))
            if (SDK::UObject* area = SbFullAreaFor(loc))
            {
                best = area; bestTier = 0; bestScore = 0;
                bestWhy = "always-visible area";
            }
    }
    g_sbHostOutsideCm = bestTier == 2 ? bestScore : (bestTier == 3 ? -1.0 : 0.0);
    static const char* kTier[] = { "inside its importance volume", "inside its importance volume (subclass slot)",
                                   "OUTSIDE every importance volume -- nearest one", "no importance volume anywhere -- nearest slot" };
    if (best)
        HxLog("[HalcyonA2][SPECEDIT] host for %s at (%.0f,%.0f,%.0f): %s (%s), %s%s\n", uniqueId.c_str(), loc[0], loc[1], loc[2],
              best->GetName().c_str(), bestWhy.c_str(), kTier[bestTier < 4 ? bestTier : 3],
              bestTier == 2 ? (" " + std::to_string(static_cast<int>(bestScore / 100.0)) + " m away").c_str() : "");
    return best;
}

// Place `uniqueId` as a real sandbox object at a world transform. Every machine -- vanilla Quest included --
// then spawns its own copy with the prefab's Luau bound. Returns the object's idx (its NetworkGUID) or "".
// Team changers are refused on EVERY placement path (palette, duplicate, catalogue, level loads): placed
// outside a game mode's arena they have no arena (-1), block players, and their team-change logic -- which
// runs on every client, Quest included -- crashed players who walked into one.
static bool g_seAllowTeamChangers = false;   // SE|ALLOWTEAM|1 (local test) -- experiments with team changers
static bool SeRefusedPrefab(const std::string& name)
{
    if (g_seAllowTeamChangers) return false;
    std::string lo = name;
    for (char& ch : lo) ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    return lo.find("teamchange") != std::string::npos || lo.find("teamswitch") != std::string::npos;
}
static std::string SeSandboxSpawn(const std::string& uniqueId, const double* loc, const double* rot, const double* scl)
{
    if ((SeRefusedPrefab(uniqueId) || SeRefusedPrefab(g_sbSpawnCls)) && !SeTeamChangerAllowedAt(loc))
    {
        HxLog("[HalcyonA2][SPECEDIT] refused %s at (%.0f,%.0f,%.0f): team changers only work inside a game mode\n", uniqueId.c_str(),
              loc[0], loc[1], loc[2]);
        return std::string();
    }
    SDK::UObject* sb = SbEngine();
    SDK::UObject* lgm = (g_sbForceLgm && SeAlive(g_sbForceLgm)) ? g_sbForceLgm : SbHostGamemode(uniqueId, loc);
    if (!sb || !lgm) { HxLog("[HalcyonA2][SPECEDIT] sandbox spawn: sandbox=%p gamemode=%p\n", sb, lgm); return std::string(); }
    SDK::AActor* slot = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320);
    // A team changer never spawns on a player (see SeIsTeamChangerName): start at a free spot in the same area,
    // and the move queue brings it to `loc` once nobody stands there.
    double buildAt[3] = { loc[0], loc[1], loc[2] };
    bool tcDeferred = false;
    if ((SeIsTeamChangerName(uniqueId) || SeIsTeamChangerName(g_sbSpawnCls)) && SePawnNear(loc, kSeTcClearCm))
    {
        static const double offs[][3] = { { 0, 0, 700 }, { 700, 0, 0 }, { -700, 0, 0 }, { 0, 700, 0 }, { 0, -700, 0 }, { 0, 0, -700 },
                                          { 1400, 0, 0 }, { -1400, 0, 0 }, { 0, 1400, 0 }, { 0, -1400, 0 }, { 0, 0, 1400 } };
        for (const auto& d : offs)
        {
            const double c[3] = { loc[0] + d[0], loc[1] + d[1], loc[2] + d[2] };
            if (SePawnNear(c, kSeTcClearCm) || !SeTeamChangerAllowedAt(c) || SbHostGamemode(uniqueId, c) != lgm) continue;
            memcpy(buildAt, c, sizeof(buildAt));
            tcDeferred = true;
            break;
        }
        if (!tcDeferred)
        {
            HxLog("[HalcyonA2][SPECEDIT] refused %s at (%.0f,%.0f,%.0f): a player is standing there and no free spot nearby\n",
                  uniqueId.c_str(), loc[0], loc[1], loc[2]);
            return std::string();
        }
        HxLog("[HalcyonA2][SPECEDIT] %s: a player stands at (%.0f,%.0f,%.0f) -- built at (%.0f,%.0f,%.0f), moves in when they step off\n",
              uniqueId.c_str(), loc[0], loc[1], loc[2], buildAt[0], buildAt[1], buildAt[2]);
    }
    SDK::FVector relLoc{ buildAt[0], buildAt[1], buildAt[2] };
    SDK::FRotator relRot{ rot[0], rot[1], rot[2] };
    if (slot)
    {
        const SDK::FTransform sx = slot->GetTransform();
        relLoc = SDK::UKismetMathLibrary::InverseTransformLocation(sx, relLoc);
        relRot = SDK::UKismetMathLibrary::InverseTransformRotation(sx, relRot);
    }
    static ULONGLONG s_seq = 0;
    wchar_t idxw[64];
    {
        // A GUID named for the hosting slot: "%08X(slot)-BBBB-BBBB-CCCC-CCCCDDDDDDDD". Script references
        // store parts 2-4 and rebuild part 1 from the slot, so only ids shaped like this can be referenced.
        const uint32_t slotNo = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(lgm) + 0x118);
        LARGE_INTEGER qpc; QueryPerformanceCounter(&qpc);
        uint64_t x = qpc.QuadPart ^ (GetTickCount64() << 20) ^ (++s_seq * 0x9E3779B97F4A7C15ULL);
        auto next = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return static_cast<uint32_t>(x); };
        const uint32_t b = next(), c = next(), d = next();
        swprintf_s(idxw, L"%08X-%04X-%04X-%04X-%04X%08X", slotNo, b >> 16, b & 0xFFFF, c >> 16, c & 0xFFFF, d);
        // A rebuild keeps the object's id, so script slots pointing at it (its own included) stay valid.
        char want[12];
        snprintf(want, sizeof(want), "%08X-", slotNo);
        if (g_sbForceIdx.size() == 36 && g_sbForceIdx.compare(0, 9, want) == 0)
            for (size_t k = 0; k <= g_sbForceIdx.size(); ++k) idxw[k] = static_cast<wchar_t>(k < g_sbForceIdx.size() ? g_sbForceIdx[k] : 0);
    }
    alignas(16) static uint8_t desc[0x400];
    memset(desc, 0, sizeof(desc));
    reinterpret_cast<void(__fastcall*)(uint8_t*)>(GetBase() + SeSb::DescInit)(desc);
    const SDK::FName type = NvName(uniqueId);
    memcpy(desc + 0, &type, sizeof(type));
    SDK::FString idx = SDK::UKismetStringLibrary::Concat_StrStr(SDK::FString(idxw), SDK::FString(L""));
    SDK::FString name = SDK::UKismetStringLibrary::Concat_StrStr(SDK::FString(idxw), SDK::FString(L""));
    memcpy(desc + 16, &idx, sizeof(idx));
    memcpy(desc + 32, &name, sizeof(name));
    const double pos[3] = { relLoc.X, relLoc.Y, relLoc.Z }, r3[3] = { relRot.Pitch, relRot.Yaw, relRot.Roll };
    memcpy(desc + 48, pos, sizeof(pos));
    memcpy(desc + 72, r3, sizeof(r3));
    memcpy(desc + 96, scl, 3 * sizeof(double));
    desc[120] = 0;                                                        // serverOnly = false

    const std::string idxA(idxw, idxw + wcslen(idxw));
    if (!g_sbPendingProps.empty())                                        // overrides requested for this spawn
    {
        for (const auto& pp : g_sbPendingProps) SbSetOverride(idxA, pp[0], pp[1], pp[2]);
        g_sbPendingProps.clear();
    }
    if (!g_sbPendingScripts.empty()) g_sbScripts[idxA] = g_sbPendingScripts;
    else g_sbScripts.erase(idxA);                          // a rebuild keeps the id: don't bring back removed scripts
    const int ncomp = SbFillComponents(desc, idxA);
    if (ncomp) HxLog("[HalcyonA2][SPECEDIT] sandbox spawn %s: %d component override(s)\n", idxA.c_str(), ncomp);
    const int before = SbObjectMapCount(sb);
    g_sbDefBp = SbBlueprintFor(uniqueId);
    // The object's class for level saves. Palette placements say it (g_sbSpawnCls); game mode pieces (team
    // changers, the controller) and SE|SBADD don't -- and a saved level skips objects without one, which lost a
    // mode's team changers and controller. Take it from the prefab's blueprint class.
    std::string bpName;
    if (g_sbDefBp && SeAlive(static_cast<SDK::UObject*>(g_sbDefBp))) bpName = static_cast<SDK::UObject*>(g_sbDefBp)->GetName();
    g_sbLbClass = SDK::UObject::FindClassFast("LuauBehavior");
    const std::vector<SbLeaf> leaves = std::move(g_sbPendingLeaves);
    g_sbPendingLeaves.clear();
    g_sbLeafCount = 0;
    for (const auto& l : leaves)
        if (g_sbLeafCount < 16)
            g_sbLeafArr[g_sbLeafCount++] = { l.inProps, NvNameBits(NvName("Properties")), NvNameBits(NvName(l.name)),
                                             l.blob.data(), static_cast<int>(l.blob.size()), l.inRefs };
    if (!g_sbDefBp)
    {
        HxLog("[HalcyonA2][SPECEDIT] sandbox spawn %s: no prefab definition -- refusing (a half-built object crashes joiners)\n", uniqueId.c_str());
        return std::string();
    }
    const int r = SbAddCore(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(lgm) + 0x218),
                            NvNameBits(NvName("objects")), desc, NvNameBits(NvName(idxA)));
    g_sbDefBp = nullptr;
    g_sbLeafCount = 0;
    if (r == 6)
    {
        SbOwned o;
        o.idx = idxA; o.cls = g_sbSpawnCls.empty() ? bpName : g_sbSpawnCls; o.lgm = lgm;
        o.uniqueId = uniqueId; o.path = g_sbSpawnPath; o.level = g_lvLoading;
        if (!g_seCaller.empty() && !g_ownLocks.count(idxA)) g_ownLocks[idxA] = { g_seCaller, g_seCallerName, false };
        memcpy(o.loc, loc, sizeof(o.loc)); memcpy(o.rot, rot, sizeof(o.rot)); memcpy(o.scl, scl, sizeof(o.scl));
        g_sbOwned.push_back(o);
        SeWantProxy(idxA);                               // collision stand-ins, whichever path spawned it
        if (tcDeferred) g_sbMovePending[idxA] = GetTickCount64();   // the record holds the real spot
        g_sbSpawnAt[idxA] = GetTickCount64();            // Game data writes wait until the node is settled
    }
    HxLog("[HalcyonA2][SPECEDIT] sandbox spawn %s: defaults from %d template(s), %d default node(s)\n", idxA.c_str(),
          g_sbDefResult > 0 ? g_sbDefResult >> 8 : g_sbDefResult, g_sbDefResult > 0 ? g_sbDefResult & 0xFF : 0);
    if (r == 6 && slot) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    if (r == 6 && g_sbProps.count(idxA))                                  // requested values: the game's own sync
        if (SDK::AActor* mine = SbActorForIdx(idxA))
            for (const auto& c : g_sbProps[idxA]) for (const auto& kv : c.second) SbSetBoundProp(mine, kv.first, kv.second);
    HxLog("[HalcyonA2][SPECEDIT] sandbox spawn %s idx=%s at (%.0f,%.0f,%.0f): result %d, objectMap %d -> %d\n",
          uniqueId.c_str(), idxA.c_str(), loc[0], loc[1], loc[2], r, before, SbObjectMapCount(sb));
    return r == 6 ? idxA : std::string();
}

// SE|COINRUN|<ident|new>|x,y,z|durationSec|questGuid32hex|x,y,z;x,y,z;...
// A TKB-style timed red-coin run, built on the game's own sandbox prefab (the station's runs are the same
// object): coins are stored in serverData/CoinTransforms_v2 relative to the run actor, and the run completes
// TargetQuest on the player's machine when every coin is collected inside Duration. "new" places one at x,y,z.
// The start button of a run: the game's ProgressionButton reads serverData/TargetQuest (a 32-hex string)
// when pressed and activates that quest on the presser's machine, which starts every run registered for it.
static bool SbSetButtonQuest(SDK::AActor* button, const std::string& questHex)
{
    SDK::UObject* pc = SbPrefabOf(button);
    if (!pc) return false;
    const uint64_t segs[2] = { NvNameBits(NvName("serverData")), NvNameBits(NvName("TargetQuest")) };
    NvWalk w{}; void* parent = nullptr; int failAt = -1;
    int r = -1;
    if (NvWorldWalk(reinterpret_cast<uint8_t*>(pc) + 0x3A0, segs, 1, &w, &parent, &failAt))
    {
        const std::wstring v(questHex.begin(), questHex.end());
        r = NvWriteNative(parent, segs[1], NvNative::TString, 0, 0, v.c_str(), static_cast<int32_t>(v.size()) + 1, nullptr);
    }
    NvWalkRelease(&w);
    HxLog("[HalcyonA2][SPECEDIT] button %s TargetQuest = %s: %d\n", button->GetName().c_str(), questHex.c_str(), r);
    return r == 1;
}

// questRef -> the run and button objects placed for it (sandbox idx). A run reads its coins and duration
// only when it is built, so re-publishing replaces both instead of editing them.
static std::unordered_map<std::string, std::pair<std::string, std::string>> g_seCoinRuns;
struct SeLvRun { std::string questRef, level; double at[3]{}, button[3]{}; float dur = 0; int thr = 2; std::string coins; };
static std::vector<SeLvRun> g_lvRuns;               // coin runs as published (saved levels rebuild them)

static void SeCoinRun(const std::string& ident, const std::string& locs, const std::string& durs,
                      const std::string& questRef, const std::string& coins, const std::string& buttonLoc,
                      const std::string& thrusters)
{
    // questRef: a 32-hex GUID, or the id of a quest published from the editor (its GUID is derived the same way).
    std::string questHex = questRef;
    if (questRef.size() != 32 && !questRef.empty())
    {
        uint32_t g[4];
        SeQuestGuid(questRef, g);
        char hx[40];
        snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", g[0], g[1], g[2], g[3]);
        questHex = hx;
    }
    double at[3]{};
    SDK::AActor* a = ident == "new" ? nullptr : SeFindEditorActor(ident);
    if (a) { const SDK::FVector l = a->K2_GetActorLocation(); at[0] = l.X; at[1] = l.Y; at[2] = l.Z; }
    else if (!SeVec(locs, at)) { HxLog("[HalcyonA2][SPECEDIT] COINRUN: bad location\n"); return; }
    std::vector<std::array<double, 3>> rel;
    for (size_t b = 0; b < coins.size() && rel.size() < 200; )
    {
        size_t e = coins.find(';', b);
        if (e == std::string::npos) e = coins.size();
        double c[3];
        if (SeVec(coins.substr(b, e - b), c)) rel.push_back({ c[0] - at[0], c[1] - at[1], c[2] - at[2] });
        b = e + 1;
    }
    std::vector<SbLeaf> leaves;
    const float dur = static_cast<float>(atof(durs.c_str()));
    if (dur > 0) leaves.push_back({ true, "Duration", SbBlobFloat(dur) });
    if (dur > 0) leaves.push_back({ true, "DoubleTickSoundDuration", SbBlobFloat(dur > 15 ? dur - 15 : dur * 0.8f) });
    uint32_t g[4]{};
    if (questHex.size() == 32)
    {
        for (int i = 0; i < 4; ++i) g[i] = static_cast<uint32_t>(strtoul(questHex.substr(i * 8, 8).c_str(), nullptr, 16));
        leaves.push_back({ true, "TargetQuest", SbBlobGuid(g) });
    }
    if (!rel.empty()) leaves.push_back({ false, "CoinTransforms_v2", SbBlobCoins(rel) });
    // ThrusterResponseType MUST be written: the prefab default is 0 = PreventThrusters, which locks the
    // player's thrusters at run start and only releases them on one end path -- players who finished an
    // editor-made run could never boost again. 2 = AllowThrusters (never touches them), 1 = FailOnThrusters
    // (the TKB behaviour: boosting fails the run; binds a delegate, never locks). Never 0.
    {
        int mode = atoi(thrusters.c_str());
        if (mode != 1) mode = 2;
        leaves.push_back({ true, "ThrusterResponseType", { 0x02, 0x01, 0x3F, 0x00, static_cast<uint8_t>(mode) } });
    }
    if (!a)
    {
        {
            SeLvRun def;
            def.questRef = questRef; def.level = g_lvLoading; def.dur = dur; def.coins = coins;
            def.thr = atoi(thrusters.c_str()) == 1 ? 1 : 2;
            memcpy(def.at, at, sizeof(def.at));
            if (!SeVec(buttonLoc, def.button)) memcpy(def.button, at, sizeof(def.button));
            bool found = false;
            for (auto& r : g_lvRuns) if (r.questRef == questRef) { if (!g_lvLoading.empty() || r.level.empty()) r = def; else { def.level = r.level; r = def; } found = true; }
            if (!found) g_lvRuns.push_back(def);
        }
        if (auto old = g_seCoinRuns.find(questRef); old != g_seCoinRuns.end())
        {
            for (const std::string& idx : { old->second.first, old->second.second })
                if (SDK::AActor* prev = idx.empty() ? nullptr : SbActorForIdx(idx)) SeSandboxDelete(prev);
            g_seCoinRuns.erase(old);
        }
        g_sbPendingLeaves = leaves;
        const double rot[3] = { 0, 0, 0 }, scl[3] = { 1, 1, 1 };
        const std::string idx = SeSandboxSpawn("aa_se_BP_New_RedCoinTimedQuest", at, rot, scl);
        double bl[3];
        if (!idx.empty() && questHex.size() == 32 && SeVec(buttonLoc, bl))
        {
            const std::string bidx = SeSandboxSpawn("aa_se_BP_ProgressionButton", bl, rot, scl);
            if (SDK::AActor* btn = bidx.empty() ? nullptr : SbActorForIdx(bidx)) SbSetButtonQuest(btn, questHex);
            g_seCoinRuns[questRef] = { idx, bidx };
        }
        else if (!idx.empty()) g_seCoinRuns[questRef] = { idx, std::string() };
        HxLog("[HalcyonA2][SPECEDIT] COINRUN new at (%.0f,%.0f,%.0f): %zu coin(s), %.0fs, quest %s -> %s\n", at[0], at[1], at[2],
              rel.size(), dur, questHex.c_str(), idx.empty() ? "FAILED" : idx.c_str());
        return;
    }
    const bool ok = SbWriteLeaves(a, leaves);
    HxLog("[HalcyonA2][SPECEDIT] COINRUN update %s: %zu coin(s), %.0fs, quest %s -> %s\n", a->GetName().c_str(), rel.size(), dur,
          questHex.c_str(), ok ? "ok" : "FAILED");
}

// A deleted quest's red coin run goes too: the run and its start button, and its saved-level record.
static void SeCoinRunRemove(const std::string& questRef)
{
    if (auto old = g_seCoinRuns.find(questRef); old != g_seCoinRuns.end())
    {
        for (const std::string& idx : { old->second.first, old->second.second })
            if (SDK::AActor* prev = idx.empty() ? nullptr : SbActorForIdx(idx)) SeSandboxDelete(prev);
        g_seCoinRuns.erase(old);
        HxLog("[HalcyonA2][SPECEDIT] coin run for %s removed\n", questRef.c_str());
    }
    g_lvRuns.erase(std::remove_if(g_lvRuns.begin(), g_lvRuns.end(), [&](const SeLvRun& r) { return r.questRef == questRef; }), g_lvRuns.end());
}


// ---- Game data: every value the game syncs for a sandbox object, readable and editable -------------------
// What the in-game level editor configures is exactly this: an object's serverData leaves (a button's
// TargetQuest string...), its bound component fields under serverData/Properties (a kiosk's TargetQuests,
// a run's Duration...), and its live gameData state (a switch's IsEnabled). Listing them and writing them
// through the replicated setters makes every blueprint configurable, for every player, vanilla included.
// Wire format (server -> editor, ClientMessage): SE|SBDATA|<ident>|<entry>\x1E<entry>...,
// entry = <path>\x1F<kind>\x1F<value>; kinds: bool num str float double int byte guid text hex(read-only).

// The bound field classes of an actor: field name -> FProperty class name (StrProperty, FloatProperty...),
// plus the struct name for StructProperty (FStructProperty::Struct @0x78).
static std::unordered_map<std::string, std::string> SbFieldClasses(SDK::AActor* a)
{
    std::unordered_map<std::string, std::string> out;
    auto* lbCls = SDK::UObject::FindClassFast("LuauBehavior");
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; lbCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->Outer != a || !o->IsA(lbCls)) continue;
        const uint8_t* arr = *reinterpret_cast<uint8_t* const*>(reinterpret_cast<uintptr_t>(o) + 0x3C0);
        const int cnt = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x3C8);
        for (int k = 0; arr && k < cnt && k < 64; ++k)
        {
            const std::string name = reinterpret_cast<const SDK::FName*>(arr + k * 16)->ToString();
            const SDK::FField* fp = *reinterpret_cast<SDK::FField* const*>(arr + k * 16 + 8);
            if (!fp || !fp->ClassPrivate) continue;
            std::string cls = fp->ClassPrivate->Name.ToString();
            if (cls == "StructProperty")
            {
                SDK::UObject* st = *reinterpret_cast<SDK::UObject* const*>(reinterpret_cast<uintptr_t>(fp) + 0x78);
                if (st) cls += ":" + st->GetName();
            }
            else if (cls == "ArrayProperty")                 // FArrayProperty::Inner @0x78
            {
                const SDK::FField* in = *reinterpret_cast<SDK::FField* const*>(reinterpret_cast<uintptr_t>(fp) + 0x78);
                if (in && in->ClassPrivate)
                {
                    std::string ic = in->ClassPrivate->Name.ToString();
                    if (ic == "StructProperty")
                        if (SDK::UObject* st = *reinterpret_cast<SDK::UObject* const*>(reinterpret_cast<uintptr_t>(in) + 0x78)) ic += ":" + st->GetName();
                    cls += ":" + ic;
                }
            }
            out[name] = cls;
        }
    }
    return out;
}

// The type header of a Properties blob (02 01 <type desc>) for each field class -- needed to write a field the
// object has never stored (a fresh kiosk has no TargetQuests leaf yet). Seeded with the headers verified in
// earlier sessions, and learned from every stored leaf we read (SeSbData), never guessed.
static std::unordered_map<std::string, std::vector<uint8_t>> g_sbPrefix = {
    { "FloatProperty",                        { 0x02, 0x01, 0x02 } },
    { "StrProperty",                          { 0x02, 0x01, 0x06 } },
    { "StructProperty:Guid",                  { 0x02, 0x01, 0x3D, 0x00 } },
    { "EnumProperty",                         { 0x02, 0x01, 0x3F, 0x00 } },
    { "ArrayProperty:StructProperty:Guid",    { 0x02, 0x01, 0x0A } },
};
// The editor kind (and empty value) for a field class, or "" if we can't edit that class.
static std::string SbKindFor(const std::string& cls, std::string* empty)
{
    *empty = "";
    if (cls == "FloatProperty") { *empty = "0"; return "float"; }
    if (cls == "DoubleProperty") { *empty = "0"; return "double"; }
    if (cls == "IntProperty") { *empty = "0"; return "int"; }
    if (cls == "ByteProperty" || cls == "EnumProperty") { *empty = "0"; return "byte"; }
    if (cls == "BoolProperty") { *empty = "0"; return "bool"; }
    if (cls == "StrProperty" || cls == "NameProperty" || cls == "TextProperty") return "text";
    if (cls == "StructProperty:Guid") return "guid";
    if (cls == "ArrayProperty:StructProperty:Guid") return "guids";
    return std::string();
}

// Where the value starts in a Properties blob (after 02 01 <type desc>), and its encoded size, by class.
// Strings: find the offset whose int32 SaveNum exactly accounts for the rest of the blob.
static bool SbBlobValue(const std::string& cls, const uint8_t* d, int len, int* off, int* vlen)
{
    int fixed = 0;
    if (cls == "FloatProperty" || cls == "IntProperty" || cls == "UInt32Property") fixed = 4;
    else if (cls == "DoubleProperty" || cls == "Int64Property") fixed = 8;
    else if (cls == "ByteProperty" || cls == "EnumProperty" || cls == "BoolProperty") fixed = 1;
    else if (cls == "StructProperty:Guid" || (cls.rfind("StructProperty", 0) == 0 && len == 20)) fixed = 16;   // FGuid-shaped
    if (fixed) { if (len < fixed + 3) return false; *off = len - fixed; *vlen = fixed; return true; }
    if (cls.rfind("ArrayProperty", 0) == 0)          // a kiosk's TargetQuests: 02 01 0A <array body>
    {
        if (len < 5 || d[2] != 0x0A) return false;
        *off = 3; *vlen = len - 3;
        return true;
    }
    if (cls == "StrProperty" || cls == "NameProperty" || cls == "TextProperty")
    {
        for (int o = 3; o + 4 <= len && o < 12; ++o)
        {
            const int32_t save = *reinterpret_cast<const int32_t*>(d + o);
            const int64_t need = save >= 0 ? 4 + int64_t(save) : 4 + int64_t(-save) * 2;
            if (o + need == len) { *off = o; *vlen = len - o; return true; }
        }
    }
    return false;
}

static std::string SbHex(const uint8_t* d, int n)
{
    std::string h;
    char b[4];
    for (int i = 0; i < n; ++i) { snprintf(b, sizeof(b), "%02X", d[i]); h += b; }
    return h;
}

static std::string SbDecode(const std::string& cls, const uint8_t* v, int n, std::string* kind)
{
    char b[64];
    if (cls == "FloatProperty") { *kind = "float"; snprintf(b, sizeof(b), "%g", *reinterpret_cast<const float*>(v)); return b; }
    if (cls == "DoubleProperty") { *kind = "double"; snprintf(b, sizeof(b), "%g", *reinterpret_cast<const double*>(v)); return b; }
    if (cls == "IntProperty") { *kind = "int"; snprintf(b, sizeof(b), "%d", *reinterpret_cast<const int32_t*>(v)); return b; }
    if (cls == "ByteProperty" || cls == "EnumProperty") { *kind = "byte"; snprintf(b, sizeof(b), "%u", v[0]); return b; }
    if (cls == "BoolProperty") { *kind = "bool"; return v[0] ? "1" : "0"; }
    if (cls == "StructProperty:Guid" || (cls.rfind("StructProperty", 0) == 0 && n == 16))
    {
        *kind = "guid";
        const uint32_t* g = reinterpret_cast<const uint32_t*>(v);
        snprintf(b, sizeof(b), "%08X%08X%08X%08X", g[0], g[1], g[2], g[3]);
        return b;
    }
    if (cls == "StrProperty" || cls == "NameProperty" || cls == "TextProperty")
    {
        *kind = "text";
        const int32_t save = *reinterpret_cast<const int32_t*>(v);
        std::string out;
        if (save > 0) for (int i = 0; i < save - 1 && 4 + i < n; ++i) out.push_back(static_cast<char>(v[4 + i]));
        else if (save < 0) for (int i = 0; i < -save - 1 && 4 + 2 * i + 1 < n; ++i) { const wchar_t c = v[4 + 2 * i] | (v[5 + 2 * i] << 8); out.push_back(c < 128 ? static_cast<char>(c) : '?'); }
        return out;
    }
    if (cls.rfind("ArrayProperty", 0) == 0)
    {
        // Array body: header byte (bit0 all-same-type, bit1 count<17, count in bits 2..7), the element type
        // once (1 byte, or 2 when its bit0 is set: FGuid-struct is 3D 00), then 16-byte FGuids.
        const uint8_t h = v[0];
        if (!(h & 1) || !(h & 2)) { *kind = "hex"; return SbHex(v, n); }   // mixed types / 17+: not ours to edit
        const int cnt = h >> 2;
        const int dlen = (n > 1 && (v[1] & 1)) ? 2 : 1;
        if (cnt && (dlen != 2 || v[1] != 0x3D)) { *kind = "hex"; return SbHex(v, n); }   // not FGuid elements
        *kind = "guids";
        std::string out;
        for (int i = 0; i < cnt && 1 + dlen + 16 * (i + 1) <= n; ++i)
        {
            const uint32_t* g = reinterpret_cast<const uint32_t*>(v + 1 + dlen + 16 * i);
            snprintf(b, sizeof(b), "%s%08X%08X%08X%08X", i ? "," : "", g[0], g[1], g[2], g[3]);
            out += b;
        }
        return out;
    }
    *kind = "hex";
    return SbHex(v, n);
}

static std::vector<uint8_t> SbEncode(const std::string& kind, const std::string& value)
{
    std::vector<uint8_t> v;
    auto put = [&](const void* p, size_t n) { v.insert(v.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); };
    if (kind == "float") { const float f = static_cast<float>(atof(value.c_str())); put(&f, 4); }
    else if (kind == "double") { const double d = atof(value.c_str()); put(&d, 8); }
    else if (kind == "int") { const int32_t i = atoi(value.c_str()); put(&i, 4); }
    else if (kind == "byte" || kind == "bool") v.push_back(static_cast<uint8_t>(atoi(value.c_str())));
    else if (kind == "guid" && value.size() == 32)
    {
        uint32_t g[4];
        for (int i = 0; i < 4; ++i) g[i] = static_cast<uint32_t>(strtoul(value.substr(i * 8, 8).c_str(), nullptr, 16));
        put(g, 16);
    }
    else if (kind == "guids")
    {
        std::vector<std::string> ids;
        for (size_t b = 0; b < value.size();) { size_t e = value.find(',', b); if (e == std::string::npos) e = value.size(); if (e - b == 32) ids.push_back(value.substr(b, 32)); b = e + 1; }
        if (ids.size() > 16) ids.resize(16);             // the one-byte header holds up to 16
        v.push_back(static_cast<uint8_t>(0x03 | (ids.size() << 2)));
        if (ids.empty()) v.push_back(0x00);
        else { v.push_back(0x3D); v.push_back(0x00); }
        for (const auto& id : ids)
        {
            uint32_t g[4];
            for (int i = 0; i < 4; ++i) g[i] = static_cast<uint32_t>(strtoul(id.substr(i * 8, 8).c_str(), nullptr, 16));
            put(g, 16);
        }
    }
    else if (kind == "text")
    {
        const int32_t save = value.empty() ? 0 : static_cast<int32_t>(value.size()) + 1;
        put(&save, 4);
        if (save) { put(value.data(), value.size()); v.push_back(0); }
    }
    return v;
}

// POD core: read serverData / gameData leaves of types 2/3/4 through the replicated API, and the raw
// Properties blobs. Output is plain text lines "<where>\t<name>\t<type>\t<payload>" (payload hex for blobs).
static void SbListImpl(uint8_t* node, std::string* out)
{
    {
        for (int which = 0; which < 2; ++which)
        {
            uint8_t* c = *reinterpret_cast<uint8_t**>(node + (which ? 560 : 552));
            if (!c) continue;
            uint8_t** arr = *reinterpret_cast<uint8_t***>(c + 136);
            const int cnt = *reinterpret_cast<int32_t*>(c + 144);
            for (int i = 0; arr && i < cnt && i < 200; ++i)
            {
                uint8_t* ch = arr[i];
                if (!ch) continue;
                const int t = ch[24];
                const std::string name = reinterpret_cast<SDK::FName*>(ch + 48)->ToString();
                if (t == 1 && name == "Properties" && !which)
                {
                    uint8_t** pa = *reinterpret_cast<uint8_t***>(ch + 136);
                    const int pc = *reinterpret_cast<int32_t*>(ch + 144);
                    for (int k = 0; pa && k < pc && k < 64; ++k)
                    {
                        uint8_t* lf = pa[k];
                        if (!lf || lf[24] != 6) continue;
                        const uint8_t* d = *reinterpret_cast<uint8_t**>(lf + 56);
                        const int len = *reinterpret_cast<int32_t*>(lf + 64);
                        if (!d || len <= 0 || len > 4096) continue;
                        *out += "P\t" + reinterpret_cast<SDK::FName*>(lf + 48)->ToString() + "\t6\t" + SbHex(d, len) + "\n";
                    }
                    continue;
                }
                if (t == 2 || t == 3 || t == 4) *out += std::string(which ? "G" : "S") + "\t" + name + "\t" + std::to_string(t) + "\t\n";
            }
        }
    }
}
static void SbListCore(uint8_t* node, std::string* out)
{
    __try { SbListImpl(node, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { out->append("!fault\n"); }
}

// The Luau scripts an object carries: its Desc component entries (+160, 48 bytes each) with a script FString
// at +24 -- the game's own objects (TKBGolf's course Cube -> Course.luau) and ours alike.
static void SbScriptsImpl(uintptr_t node, std::vector<std::string>* out)
{
    const uint8_t* d = reinterpret_cast<const uint8_t*>(reinterpret_cast<void*(__fastcall*)(uintptr_t)>(GetBase() + SeSb::NodeDesc)(node));
    if (!d) return;
    const uint8_t* comps = *reinterpret_cast<uint8_t* const*>(d + 160);
    const int nc = *reinterpret_cast<const int32_t*>(d + 168);
    for (int i = 0; comps && i < nc && i < 32; ++i)
    {
        const wchar_t* w = *reinterpret_cast<wchar_t* const*>(comps + i * 48 + 24);
        if (!w || !w[0]) continue;
        std::string n;
        for (int k = 0; w[k] && k < 120; ++k) n.push_back(w[k] < 128 ? static_cast<char>(w[k]) : '?');
        out->push_back(n);
    }
}
static void SbScriptsCore(uintptr_t node, std::vector<std::string>* out)
{
    __try { SbScriptsImpl(node, out); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static std::vector<std::string> SbObjectScripts(uint8_t* handle)
{
    std::vector<std::string> v;
    if (const uintptr_t node = SbNodeOf(handle)) SbScriptsCore(node, &v);
    return v;
}

static void SeSbData(SDK::UObject* ctx, const std::string& ident)
{
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    SDK::UObject* caller = SeCallerPC(ctx);
    if (!pc || !caller) return;
    uint8_t* handle = reinterpret_cast<uint8_t*>(pc) + 0x3A0;
    std::string raw;
    SbListCore(reinterpret_cast<uint8_t*>(SbNodeOf(handle)), &raw);
    const auto fields = SbFieldClasses(a);
    std::string entries;
    std::unordered_set<std::string> listed;
    auto add = [&](const std::string& path, const std::string& kind, const std::string& value) {
        if (!entries.empty()) entries += '\x1E';
        entries += path + '\x1F' + kind + '\x1F' + value;
    };
    for (size_t b = 0; b < raw.size();)
    {
        size_t e = raw.find('\n', b);
        if (e == std::string::npos) e = raw.size();
        const std::string line = raw.substr(b, e - b);
        b = e + 1;
        std::vector<std::string> f;
        for (size_t x = 0; x <= line.size();) { size_t y = line.find('\t', x); if (y == std::string::npos) y = line.size(); f.push_back(line.substr(x, y - x)); x = y + 1; }
        if (f.size() < 4) continue;
        const std::string& name = f[1];
        if (name.rfind("default__", 0) == 0 || name == "Blueprint") continue;
        if (f[0] == "P")
        {
            std::vector<uint8_t> d;
            for (size_t i = 0; i + 1 < f[3].size(); i += 2) d.push_back(static_cast<uint8_t>(strtoul(f[3].substr(i, 2).c_str(), nullptr, 16)));
            auto it = fields.find(name);
            const std::string cls = it == fields.end() ? std::string() : it->second;
            int off = 0, vlen = 0;
            std::string kind = "hex", value = f[3];
            if (!cls.empty() && SbBlobValue(cls, d.data(), static_cast<int>(d.size()), &off, &vlen))
            {
                value = SbDecode(cls, d.data() + off, vlen, &kind);
                if (kind != "hex" && off >= 3 && off <= 8 && !g_sbPrefix.count(cls))   // learn this class's header
                    g_sbPrefix[cls] = std::vector<uint8_t>(d.begin(), d.begin() + off);
            }
            add("props/" + name, kind, value);
            listed.insert(name);
            continue;
        }
        // serverData / gameData scalar leaves, read through the replicated API
        const std::string top = f[0] == "S" ? "serverData" : "gameData";
        const int type = atoi(f[2].c_str());
        const uint64_t segs[2] = { NvNameBits(NvName(top)), NvNameBits(NvName(name)) };
        NvWalk w{}; void* parent = nullptr; int failAt = -1;
        std::string value;
        if (NvWorldWalk(handle, segs, 1, &w, &parent, &failAt))
        {
            NvOrig o{};
            if (NvReadNative(parent, segs[1], type, &o) == 1)
            {
                char bb[64];
                if (type == NvNative::TBool) value = o.b ? "1" : "0";
                else if (type == NvNative::TNumber) { snprintf(bb, sizeof(bb), "%g", o.num); value = bb; }
                else { for (int i = 0; o.str[i] && i < 511; ++i) value.push_back(o.str[i] < 128 ? static_cast<char>(o.str[i]) : '?'); }
            }
        }
        NvWalkRelease(&w);
        add((f[0] == "S" ? "sd/" : "gd/") + name, type == NvNative::TBool ? "bool" : type == NvNative::TNumber ? "num" : "str", value);
    }
    // Fields the object has never stored (a fresh kiosk's TargetQuests): listed empty, so they can be set.
    for (const auto& [fname, cls] : fields)
    {
        if (listed.count(fname) || !g_sbPrefix.count(cls)) continue;
        std::string empty;
        const std::string kind = SbKindFor(cls, &empty);
        if (!kind.empty()) add("props/" + fname, kind, empty);
    }
    for (const auto& sn : SbObjectScripts(handle)) add("script/" + sn, "script", sn);
    if (SbOwned* own = SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248))))
        for (const auto& r : own->refs)
            if (SDK::AActor* t = SbActorForIdx(r[2])) add("slot/" + r[0] + "/" + r[1], "slot", SeLvIdent(t));
    SeBroadcast("SE|SBDATA|" + ident + "|" + entries, caller);
    HxLog("[HalcyonA2][SPECEDIT] SBDATA %s: %zu byte(s) sent\n", a->GetName().c_str(), entries.size());
}

// SE|SBSET|<ident>|<path>|<kind>|<value>
static bool g_seSbNoRecord = false;   // SeTextTick: write a rendered text without replacing the saved template
static void SeTextQueueObject(SDK::UObject* prefab);   // below
// When each object's Game data was last written. Replacing the same leaf twice in one frame (a {template} text set,
// then its rendered value) left a null entry in the netvar change list and the server faulted in the replication
// hash (+0x465C4FD, seen 2026-09-24): the template renderer waits until an object's last write is 300 ms old.
static std::unordered_map<std::string, ULONGLONG> g_sbLeafWriteAt;
// ...and when each object's node was created. A write in the same frame as the object's creation (level loads
// replay Game data right after the spawn) faulted the same way -- six times loading one level (2026-09-24). So
// every write to an object whose node is < 400 ms old, or was written < 400 ms ago, waits in this queue.
struct SbDeferredSet { std::string idx, path, kind, value; bool noRecord = false; ULONGLONG due = 0; };
static std::vector<SbDeferredSet> g_sbDeferredSets;
constexpr ULONGLONG kSbWriteGapMs = 400;
static bool g_sbApplyingDeferred = false;
static void SeSbSet(SDK::UObject* ctx, const std::string& ident, const std::string& path, const std::string& kind,
                    const std::string& value)
{
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) return;
    {
        const std::string widx = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248));
        const ULONGLONG nowW = GetTickCount64();
        auto sp = g_sbSpawnAt.find(widx);
        auto lw = g_sbLeafWriteAt.find(widx);
        const bool young = sp != g_sbSpawnAt.end() && nowW - sp->second < kSbWriteGapMs;
        const bool recent = lw != g_sbLeafWriteAt.end() && nowW - lw->second < kSbWriteGapMs;
        bool queued = false;                                  // keep writes to one object in order
        for (const auto& d : g_sbDeferredSets) if (d.idx == widx) queued = true;
        // only props/ writes replace a leaf (the faulting operation); sd/ gd/ values are written in place and
        // must land at once (a team changer's TeamIndex has to be there before clients build it)
        if (!widx.empty() && path.rfind("props/", 0) == 0 && (young || recent || (queued && !g_sbApplyingDeferred)))
        {
            // the same path queued again: the newer value wins
            for (auto it = g_sbDeferredSets.begin(); it != g_sbDeferredSets.end(); ++it)
                if (it->idx == widx && it->path == path) { g_sbDeferredSets.erase(it); break; }
            ULONGLONG due = nowW + kSbWriteGapMs;
            for (const auto& d : g_sbDeferredSets) if (d.idx == widx && d.due + kSbWriteGapMs > due) due = d.due + kSbWriteGapMs;
            g_sbDeferredSets.push_back({ widx, path, kind, value, g_seSbNoRecord, due });
            return;
        }
    }
    uint8_t* handle = reinterpret_cast<uint8_t*>(pc) + 0x3A0;
    bool ok = false;
    if (path.rfind("props/", 0) == 0)
    {
        // Rebuild the leaf: the current blob's prefix (02 01 <type desc>) + the new value.
        const std::string name = path.substr(6);
        std::string raw;
        SbListCore(reinterpret_cast<uint8_t*>(SbNodeOf(handle)), &raw);
        const std::string key = "P\t" + name + "\t6\t";
        const size_t at = raw.find(key);
        const auto fields = SbFieldClasses(a);
        auto it = fields.find(name);
        if (at == std::string::npos && it != fields.end() && g_sbPrefix.count(it->second))
        {
            // Never stored on this object: header for its class + the value.
            const std::vector<uint8_t> val = SbEncode(kind, value);
            if (!val.empty())
            {
                std::vector<uint8_t> blob = g_sbPrefix[it->second];
                blob.insert(blob.end(), val.begin(), val.end());
                ok = SbWriteLeaves(a, { { true, name, blob } });
            }
        }
        else if (at != std::string::npos && it != fields.end())
        {
            const size_t e = raw.find('\n', at);
            const std::string hex = raw.substr(at + key.size(), e - at - key.size());
            std::vector<uint8_t> d;
            for (size_t i = 0; i + 1 < hex.size(); i += 2) d.push_back(static_cast<uint8_t>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
            int off = 0, vlen = 0;
            const std::vector<uint8_t> val = SbEncode(kind, value);
            if (!val.empty() && SbBlobValue(it->second, d.data(), static_cast<int>(d.size()), &off, &vlen))
            {
                std::vector<uint8_t> blob(d.begin(), d.begin() + off);
                blob.insert(blob.end(), val.begin(), val.end());
                ok = SbWriteLeaves(a, { { true, name, blob } });
            }
        }
    }
    else if (path.rfind("sd/", 0) == 0 || path.rfind("gd/", 0) == 0)
    {
        const std::string top = path[0] == 's' ? "serverData" : "gameData";
        const uint64_t segs[2] = { NvNameBits(NvName(top)), NvNameBits(NvName(path.substr(3))) };
        NvWalk w{}; void* parent = nullptr; int failAt = -1;
        if (NvWorldWalk(handle, segs, 1, &w, &parent, &failAt))
        {
            const std::wstring ws(value.begin(), value.end());
            const int type = kind == "bool" ? NvNative::TBool : kind == "num" ? NvNative::TNumber : NvNative::TString;
            ok = NvWriteNative(parent, segs[1], type, static_cast<float>(atof(value.c_str())), static_cast<uint8_t>(atoi(value.c_str()) != 0),
                               ws.c_str(), static_cast<int32_t>(ws.size()) + 1, nullptr) == 1;
        }
        NvWalkRelease(&w);
        SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
        SDK::AActor* slot = lgm ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320) : nullptr;
        if (ok && slot) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    }
    g_sbLeafWriteAt[FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248))] = GetTickCount64();
    if (g_seSbNoRecord) return;                          // a rendered template: the saved value stays the template
    HxLog("[HalcyonA2][SPECEDIT] SBSET %s %s (%s) = '%s': %s\n", a->GetName().c_str(), path.c_str(), kind.c_str(), value.c_str(),
          ok ? "ok" : "FAILED");
    if (ok) SeLvRecordData(pc, path, kind, value);
    if (ok && path == "props/Text" && value.find('{') != std::string::npos) SeTextQueueObject(pc);   // show it filled in
    SeSbData(ctx, ident);                               // refresh the editor's view
}

// SE|SBTEXTS|<text> (local test): four sandbox Text objects, 4 m N/E/S/W of the first real player, each
// created with a Text override, so an unmodded client standing there must be looking at one of them.
static void SeSandboxTextsAtPlayer(const std::string& text)
{
    if (!g_seLocalTest) return;
    static SDK::UClass* pawnCls = nullptr;
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
    for (const ObjIdxEntry& e : ClassObjectEntries(pawnCls))
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(e.idx);
        if (!o || o != e.obj || o->IsDefaultObject()) continue;
        const uintptr_t pw = reinterpret_cast<uintptr_t>(o);
        if (!*reinterpret_cast<void**>(pw + 0x2D0)) continue;
        void* entity = *reinterpret_cast<void**>(pw + 0x928);
        if (!entity) continue;
        const double* p3 = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(entity) + 0x100);
        if (p3[0] == 0.0 && p3[1] == 0.0 && p3[2] == 0.0) continue;
        const double off[4][2] = { { 400, 0 }, { 0, 400 }, { -400, 0 }, { 0, -400 } };
        for (int i = 0; i < 4; ++i)
        {
            const double loc[3] = { p3[0] + off[i][0], p3[1] + off[i][1], p3[2] + 120.0 };
            const double yaw = atan2(-off[i][1], -off[i][0]) * 57.29577951308232;   // face the player
            const double rot[3] = { 0, yaw, 0 }, scl[3] = { 1, 1, 1 };
            g_sbPendingProps = { { "Text", "Text", text } };
            SeSandboxSpawn("aa_se_LE_BP_Text", loc, rot, scl);
        }
        HxLog("[HalcyonA2][SPECEDIT] SBTEXTS: four texts around %s at (%.0f,%.0f,%.0f)\n", o->GetName().c_str(), p3[0], p3[1], p3[2]);
        return;
    }
    HxLog("[HalcyonA2][SPECEDIT] SBTEXTS: no real player\n");
}

static void SeSandboxAdd(const std::string& uniqueId, const std::string& locs)
{
    if (!g_seLocalTest) return;
    double loc[3]{}; const double rot[3] = { 0, 0, 0 }, scl[3] = { 1, 1, 1 };
    if (SeVec(locs, loc)) SeSandboxSpawn(uniqueId, loc, rot, scl);
}

// ---- local test only: a checkpoint quest right where a real player stands --------------------------
// SE|TESTQUEST|<questId>. Only with -SpecEditLocalTest. The editor client cannot see a distant player's
// pawn (net relevancy culls it), so the server, which sees everyone, places two red coins at the first
// real player's position and publishes a checkpoint quest over them -- exercising SeQuestTick end to end.
// SE|SBADDATPLAYER|<UniqueID>|dx,dy,dz -- LOCAL TEST: a catalogue item on another (non-caller) player, placed
// by the server (the editor client cannot see a distant pawn). Used to walk a player into a team changer.
static bool SeOtherPlayerPos(SDK::UObject* ctx, double* out)   // LOCAL TEST: the first non-caller player's pawn
{
    // Walk the VR pawns, not the controllers: a player that left spectator can still show its spectator pawn in
    // the controller's Pawn field on the server (seen 2026-09-24), while the VR pawn's own Controller is set.
    SDK::UObject* callerPc = SeCallerPC(ctx);
    static SDK::UClass* cls = nullptr;
    if (!cls) cls = SDK::UObject::FindClassFast("VRPawn");
    for (SDK::UObject* o : cls ? ClassObjects(cls) : std::vector<SDK::UObject*>())
    {
        if (!o || o->IsDefaultObject()) continue;
        SDK::UObject* ctrl = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(o) + 0x2D0);
        auto* pawn = static_cast<SDK::AActor*>(o);
        const SDK::FVector al = SePawnPos(pawn);
        HxLog("[HalcyonA2][SPECEDIT] @P pawn %s ctrl=%s caller=%d at (%.0f,%.0f,%.0f)\n", o->GetName().c_str(), ctrl ? ctrl->GetName().c_str() : "-",
              ctrl == callerPc, al.X, al.Y, al.Z);
        if (!ctrl || ctrl == callerPc) continue;
        if (al.Z < -50000.0 || (al.X == 0.0 && al.Y == 0.0 && al.Z == 0.0)) continue;
        out[0] = al.X; out[1] = al.Y; out[2] = al.Z;
        return true;
    }
    return false;
}
static void SeSandboxAddAtPlayer(SDK::UObject* ctx, const std::string& uniqueId, const std::string& offs)
{
    if (!g_seLocalTest) return;
    SDK::UObject* callerPc = SeCallerPC(ctx);
    double off[3] = { 0, 0, 0 };
    SeVec(offs, off);
    for (SDK::UObject* pc : ClassObjects(SDK::APlayerController::StaticClass()))
    {
        if (!pc || pc == callerPc || pc->IsDefaultObject() || (*(reinterpret_cast<const uint8_t*>(pc) + 0x65) & 0x01)) continue;
        // Its pawn if it has one (a VR player), else where its camera is (a desktop client may be a spectator).
        double p3[3];
        auto* pawn = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(pc) + 0x2D8);
        if (pawn && !pawn->IsDefaultObject()) { const SDK::FVector al = pawn->K2_GetActorLocation(); p3[0] = al.X; p3[1] = al.Y; p3[2] = al.Z; }
        else
        {
            void* pcm = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pc) + 0x350);
            if (!pcm) continue;
            const double* l = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(pcm) + 0x13A0 + 0x10);
            p3[0] = l[0]; p3[1] = l[1]; p3[2] = l[2];
        }
        if (p3[2] < -50000.0 || (p3[0] == 0.0 && p3[1] == 0.0 && p3[2] == 0.0)) continue;
        const double loc[3] = { p3[0] + off[0], p3[1] + off[1], p3[2] + off[2] }, rot[3] = { 0, 0, 0 }, scl[3] = { 1, 1, 1 };
        HxLog("[HalcyonA2][SPECEDIT] SBADDATPLAYER %s on %s (%s) at (%.0f,%.0f,%.0f)\n", uniqueId.c_str(), pc->GetName().c_str(),
              pawn ? pawn->GetName().c_str() : "no pawn", loc[0], loc[1], loc[2]);
        SeSandboxSpawn(uniqueId, loc, rot, scl);
        return;
    }
    HxLog("[HalcyonA2][SPECEDIT] SBADDATPLAYER: no other player\n");
}
static void SeTestQuestAtPlayer(SDK::UObject* ctx, const std::string& questId)
{
    if (!g_seLocalTest) return;
    static SDK::UClass* pawnCls = nullptr;
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
    const double* pos = nullptr;
    for (const ObjIdxEntry& e : ClassObjectEntries(pawnCls))
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(e.idx);
        if (!o || o != e.obj || o->IsDefaultObject()) continue;
        const uintptr_t pw = reinterpret_cast<uintptr_t>(o);
        if (!*reinterpret_cast<void**>(pw + 0x2D0)) continue;
        void* entity = *reinterpret_cast<void**>(pw + 0x928);
        if (!entity) continue;
        const double* p3 = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(entity) + 0x100);
        if (p3[0] == 0.0 && p3[1] == 0.0 && p3[2] == 0.0) continue;
        pos = p3;
        HxLog("[HalcyonA2][SPECEDIT] TESTQUEST: player %s at (%.0f,%.0f,%.0f)\n", o->GetName().c_str(), p3[0], p3[1], p3[2]);
        break;
    }
    if (!pos) { HxLog("[HalcyonA2][SPECEDIT] TESTQUEST: no real player pawn with a pose\n"); return; }
    const std::string path = "/Game/A2/Progression/TimedQuest/LE_BP_RedCoin.LE_BP_RedCoin_C";
    std::string steps;
    for (int i = 0; i < 2; ++i)
    {
        char loc[96];
        snprintf(loc, sizeof(loc), "%.1f,%.1f,%.1f", pos[0] + i * 80.0, pos[1], pos[2]);
        SeSpawn(ctx, path, loc, "0,0,0");
        if (!steps.empty()) steps += ";";
        steps += std::string("LE_BP_RedCoin_C@") + loc + ":0";
    }
    SeQuest(questId, "Checkpoint Test", "PKRClimb5", 0, steps, 0, 0.0f, "Local test quest", 500.0, 0);
}

// ---- property edits (the Details panel) ------------------------------------------------------
// SE|PROP|<Class@x,y,z>|<path>|<value>. `path` is "Prop" on the placed actor, or "Sub.Prop" to reach into
// something that actor owns (a component, or a child actor such as a quest prefab's inner quest actor).
// Fences: the root must be an LE_ actor (SeFindEditorActor), navigation only enters objects that root
// owns (sereflect::Hop), and only plain value types are written -- never strings or object references.
// ---- property edits reach every client --------------------------------------------------------
// Almost nothing an LE prefab exposes is replicated (no CPF_Net), so a server-side write alone changes
// only what the SERVER computes from it; anything a client draws from its own copy (text, colours, sizes)
// never hears about it. So each edit is also sent to every player controller through the stock reliable
// APlayerController::ClientMessage RPC: the Spec Editor mod applies it there, and a vanilla client just
// prints a console line nobody sees.
struct SeEdit { SDK::AActor* actor; std::string path, value; };
static std::vector<SeEdit> g_seEdits;

// Liveness by the object's GObjects slot, never by dereferencing a pointer that may be stale and never by
// walking GObjects (a full walk costs ~50 ms on the VPS; the collision tick asks this 10x a second per
// placed mesh -- see the [PROF] perf notes). The slot index is recorded when we first see the actor (it is
// alive then); afterwards a destroyed or garbage-collected actor's slot no longer holds the same pointer.
static std::unordered_map<SDK::AActor*, int32_t> g_seIdx;
static void SeTrack(SDK::AActor* a) { if (a) g_seIdx[a] = a->Index; }
static bool SeActorAlive(SDK::AActor* a)
{
    if (!a) return false;
    const auto it = g_seIdx.find(a);
    if (it == g_seIdx.end()) return false;
    if (SDK::UObject::GObjects->GetByIndex(it->second) != a) { g_seIdx.erase(it); return false; }
    return !(*(reinterpret_cast<const uint8_t*>(a) + 0x65) & 0x01);   // bActorIsBeingDestroyed
}

// To one controller, or (pc == nullptr) to every live one. Returns how many it went to.
static int SeBroadcast(const std::string& msg, SDK::UObject* pc)
{
    const std::wstring w(msg.begin(), msg.end());
    auto send = [&](SDK::UObject* o)
    { static_cast<SDK::APlayerController*>(o)->ClientMessage(SDK::FString(w.c_str()), SDK::FName(), 0.0f); };
    if (pc) { send(pc); return 1; }
    auto* pcCls = SDK::APlayerController::StaticClass();
    int n = 0;
    for (SDK::UObject* o : ClassObjects(pcCls))          // [PERF] shared index, not a GObjects walk per message
    {
        if (!o || o->IsDefaultObject()) continue;
        if (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01) continue;
        send(o);
        ++n;
    }
    return n;
}

static void SeSetProp(const std::string& ident, const std::string& path, const std::string& value)
{
    SDK::AActor* a = SeFindEditorActor(ident);
    if (!a) return;
    if (SbPrefabOf(a))
    {
        // A sandbox object: a Luau-bound field (a Text's text) goes through the game's own sync --
        // serverData/Properties -- so every machine, vanilla Quest included, applies it. Everything else
        // takes the normal path below (server write + broadcast), as for any other actor.
        const size_t dot = path.find_last_of('.');
        if (dot != std::string::npos) SbSetBoundProp(a, path.substr(dot + 1), value);
    }
    const size_t dot = path.find_last_of('.');
    const std::string objPath = dot == std::string::npos ? std::string() : path.substr(0, dot);
    const std::string prop    = dot == std::string::npos ? path : path.substr(dot + 1);
    SDK::UObject* obj = sereflect::Resolve(a, objPath);
    if (!obj) { HxLog("[HalcyonA2][SPECEDIT] prop: cannot reach '%s' from %s\n", objPath.c_str(), ident.c_str()); return; }
    sereflect::PType t{};
    SDK::FProperty* p = sereflect::Find(obj, prop, &t);
    if (!p || !sereflect::Writable(t)) { HxLog("[HalcyonA2][SPECEDIT] prop: '%s' is not an editable value on %s\n", prop.c_str(), obj->GetName().c_str()); return; }
    const std::string before = sereflect::Read(obj, p, t);
    if (!sereflect::Write(obj, p, t, value)) { HxLog("[HalcyonA2][SPECEDIT] prop: bad value '%s' for %s\n", value.c_str(), path.c_str()); return; }
    sereflect::AfterWrite(a, obj, p);
    HxLog("[HalcyonA2][SPECEDIT] set %s.%s = %s (was %s)%s\n", obj->GetName().c_str(), prop.c_str(),
          sereflect::Read(obj, p, t).c_str(), before.c_str(), sereflect::Replicated(p) ? " [replicated]" : "");

    // Remember it (an editor who enters later gets it replayed) and tell every client now.
    auto it = std::find_if(g_seEdits.begin(), g_seEdits.end(),
                           [&](const SeEdit& e) { return e.actor == a && e.path == path; });
    SeTrack(a);
    if (it != g_seEdits.end()) it->value = value; else g_seEdits.push_back({ a, path, value });
    const int n = SeBroadcast("SE|PROP|" + ident + "|" + path + "|" + value, nullptr);
    HxLog("[HalcyonA2][SPECEDIT] broadcast %s to %d client(s)\n", path.c_str(), n);
}

// Replay every live edit to one player controller (an editor that just entered), addressed by where each
// actor is NOW -- it may have been moved since it was edited.
static void SeReplayEdits(SDK::UObject* pc)
{
    int sent = 0;
    for (auto it = g_seEdits.begin(); it != g_seEdits.end();)
    {
        SDK::AActor* a = it->actor;
        if (!SeActorAlive(a)) { it = g_seEdits.erase(it); continue; }
        const SDK::FVector l = a->K2_GetActorLocation();
        char id[256];
        snprintf(id, sizeof(id), "%s@%.1f,%.1f,%.1f", a->Class->GetName().c_str(), l.X, l.Y, l.Z);
        SeBroadcast("SE|PROP|" + std::string(id) + "|" + it->path + "|" + it->value, pc);
        ++sent; ++it;
    }
    if (sent) HxLog("[HalcyonA2][SPECEDIT] replayed %d edit(s) to %s\n", sent, pc->GetName().c_str());
}


// ============================================================================================

// ---- Custom Luau ----------------------------------------------------------------------------------------
// SE|LUAUPART|<name>|<hex chunk>   (repeated; commands are capped at 1 KB)
// SE|LUAU|<ident>|<name>           attach the collected source to the object
// The source goes to <gamemode>/Scripts/<name> as a string node (replicated; every machine compiles it),
// then the object is rebuilt with a component entry {id = script = <name>} -- the way the station itself
// attaches its Luau (TKBGolf's Course.luau sits on a Cube exactly like this).
static std::unordered_map<std::string, std::string> g_luauParts;
static std::string SeLvIdent(SDK::AActor* a);   // below (saved levels)
static std::vector<SbLeaf> SbRefLeaves(const std::vector<std::array<std::string, 4>>& refs);   // below (script slots)
static std::string SeLuauName(const std::string& raw0)
{
    std::string raw = raw0;
    if (raw.size() > 5 && _stricmp(raw.c_str() + raw.size() - 5, ".luau") == 0) raw.resize(raw.size() - 5);
    std::string n;
    for (char c : raw) if (isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '-') n += c;
    if (n.empty() || n.size() > 48) return std::string();
    return n + ".luau";
}
// POD core: replace <scripts container>/<name> with a string node holding src (engine-allocated).
static int SeLuauNodeCore(void* lgmHandle, uint64_t scriptsBits, uint64_t nameBits, const wchar_t* src, int len)
{
    uint8_t itS[0x100] = {}, itOld[0x100] = {};
    __try
    {
        const uintptr_t base = GetBase();
        auto find = reinterpret_cast<void*(__fastcall*)(void*, uint8_t*, uint64_t, char)>(base + SeSb::FindChild);
        auto mal = reinterpret_cast<uint8_t*(__fastcall*)(size_t)>(base + SeSb::Malloc);
        find(lgmHandle, itS, scriptsBits, 0);
        if (!itS[0x48]) { NvReleaseIter(base, itS); return -10; }
        find(itS, itOld, nameBits, 0);
        const bool exists = itOld[0x48] != 0;
        NvReleaseIter(base, itOld);
        if (exists) { NvReleaseIter(base, itS); return -11; }   // never remove a live source node (see SeLuauPutSource)
        // Built exactly like the game's own string-node factory (0x4643350), field for field. The field at
        // +0x48 is the one that matters: the factory sets it to 0x200, we used to leave it zero, and a node
        // with zero there sent the replication walk into endless recursion -- the server died with a stack
        // overflow the moment a player in that area was sent the new script. It only showed up once objects
        // started being hosted in the area they are placed in (the editor's own area, always subscribed).
        uint8_t* n = mal(0x50);
        memset(n, 0, 0x50);
        uint64_t nm = nameBits;
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint64_t*, uint8_t)>(base + SeSb::LeafCtor)(n, &nm, 4);
        *reinterpret_cast<uintptr_t*>(n) = base + 0x8021318;                    // string node vtable
        *reinterpret_cast<uint64_t*>(n + 0x38) = 0;                             // FString: data
        *reinterpret_cast<uint64_t*>(n + 0x40) = 0;                             //          num, max
        *reinterpret_cast<uint32_t*>(n + 0x48) = 0x200;
        reinterpret_cast<void(__fastcall*)(uint8_t*, const wchar_t*, int32_t)>(base + SeSb::StrAssign)(n + 0x38, src, len);
        reinterpret_cast<void(__fastcall*)(uint8_t*)>(base + SeSb::StringStat)(n);
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t**)>(base + SeSb::AddChild)(itS, &n);
        NvReleaseIter(base, itS);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -20; }
}
// POD core: add a string leaf `name` = src directly under the container `parent` (a netvar handle or a FindChild
// iterator -- the game treats both as containers). Built exactly like SeLuauNodeCore's node. 1 = added.
static int SeNvAddStringCore(void* parent, uint64_t nameBits, const wchar_t* src, int len)
{
    uint8_t itOld[0x100] = {};
    __try
    {
        const uintptr_t base = GetBase();
        auto find = reinterpret_cast<void*(__fastcall*)(void*, uint8_t*, uint64_t, char)>(base + SeSb::FindChild);
        auto mal = reinterpret_cast<uint8_t*(__fastcall*)(size_t)>(base + SeSb::Malloc);
        find(parent, itOld, nameBits, 0);
        const bool exists = itOld[0x48] != 0;
        NvReleaseIter(base, itOld);
        if (exists) return -11;
        uint8_t* n = mal(0x50);
        memset(n, 0, 0x50);
        uint64_t nm = nameBits;
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint64_t*, uint8_t)>(base + SeSb::LeafCtor)(n, &nm, 4);
        *reinterpret_cast<uintptr_t*>(n) = base + 0x8021318;                    // string node vtable
        *reinterpret_cast<uint64_t*>(n + 0x38) = 0;
        *reinterpret_cast<uint64_t*>(n + 0x40) = 0;
        *reinterpret_cast<uint32_t*>(n + 0x48) = 0x200;
        reinterpret_cast<void(__fastcall*)(uint8_t*, const wchar_t*, int32_t)>(base + SeSb::StrAssign)(n + 0x38, src, len);
        reinterpret_cast<void(__fastcall*)(uint8_t*)>(base + SeSb::StringStat)(n);
        reinterpret_cast<void(__fastcall*)(uint8_t*, uint8_t**)>(base + SeSb::AddChild)(static_cast<uint8_t*>(parent), &n);
        return 1;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -20; }
}
// A string config variable of a module slot's gamemode (what Luau's Gamemode:getStringConfigVariable reads, on
// every machine): written in place when it exists, created otherwise. Then pushed so clients get it.
static int SeModeConfigSet(SDK::AActor* slot, const std::string& name, const std::string& value)
{
    if (!slot || !SeAlive(slot)) return -1;
    SDK::UObject* ms = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x300);
    if (!ms || !SeAlive(ms)) return -2;
    void* cfg = reinterpret_cast<uint8_t*>(ms) + 0xE8;
    const uint64_t nb = NvNameBits(NvName(name));
    const std::wstring w(value.begin(), value.end());
    int r = NvWriteNative(cfg, nb, NvNative::TString, 0.0f, 0, w.c_str(), static_cast<int32_t>(w.size()) + 1, nullptr);
    if (r != 1) r = SeNvAddStringCore(cfg, nb, w.c_str(), static_cast<int>(w.size()));
    if (auto* f = slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(slot, f, nullptr);
    if (SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x440))
        if (auto* f = lgm->Class->GetFunction("LoadedGameMode", "PushNetVars")) SafeProcessEvent(lgm, f, nullptr);
    return r;
}

// Every editor script gets this appended: the lifecycle functions the game calls run inside pcall, so a
// runtime error (a nil slot, a typo'd method) is logged and the object keeps working. Without it an error in
// BeginPlay left the object half-built and every client -- joiners included -- crashed on it moments later.
// Appended (not prepended) so the line numbers in error messages still match the user's file.
static const char kSeLuauGuard[] = R"LUAU(

-- [Rigel] safety net added by the server: errors are reported, never fatal
local __rigelWrap = function(name: string, f: any): any
	return function(...)
		local ok, err = pcall(f, ...)
		if not ok then warn("[RigelError] " .. name .. ": " .. tostring(err)) end
	end
end
if type(BeginPlay) == "function" then BeginPlay = __rigelWrap("BeginPlay", BeginPlay) end
if type(EndPlay) == "function" then EndPlay = __rigelWrap("EndPlay", EndPlay) end
if type(Tick) == "function" then Tick = __rigelWrap("Tick", Tick) end
-- (A VRPawn.getPlayerName fallback used to sit here. It never ran: the VRPawn table is read-only userdata -- a
-- script can't replace its functions -- and every call it relied on (getLocalPlayer, getPlayer, getPlayerIndex,
-- getStoredName) isn't bound. The game's own getPlayerName resolves every player on every machine, including
-- after a rejoin (new index) and a team change: verified 2026-09-25 with the mock player.)
-- [Rigel] game modes: Rigel.* (see the scripting guide, "Game modes"). Available inside functions -- BeginPlay
-- and later -- not at the top of the file (this part of the script runs after yours).
Rigel = {}
do
	local function cfg(k: string): string
		local ok, v = pcall(function() return Gamemode:getStringConfigVariable(k) end)
		if ok and type(v) == "string" then return v end
		return ""
	end
	local function num(k: string): number return tonumber(cfg(k)) or 0 end
	local function send(ev: string, arg: string) pcall(function() Gamemode:broadcastEventString(ev, arg) end) end
	Rigel.isGameMode = function(): boolean return cfg("rgm.id") ~= "" end
	Rigel.name = function(): string return cfg("rgm.name") end
	Rigel.state = function(): string local st = cfg("rgm.state"); return if st == "" then "none" else st end
	Rigel.isRunning = function(): boolean return cfg("rgm.state") == "running" end
	Rigel.round = function(): number return num("rgm.round") end
	Rigel.timeLeft = function(): number return num("rgm.time") end
	Rigel.teams = function(): number return num("rgm.teams") end
	Rigel.teamName = function(team: number): string return cfg("rgm.team" .. team .. ".name") end
	Rigel.teamSize = function(team: number): number return num("rgm.team" .. team .. ".size") end
	Rigel.teamMax = function(team: number): number return num("rgm.team" .. team .. ".max") end
	Rigel.score = function(team: number): number return num("rgm.team" .. team .. ".score") end
	Rigel.roundsWon = function(team: number): number return num("rgm.team" .. team .. ".wins") end
	Rigel.players = function(): number return num("rgm.players") end
	Rigel.winner = function(): number return num("rgm.winner") end
	Rigel.setting = function(key: string): string
		local v = cfg("rgm.set." .. key)
		if v == "" then v = cfg("rgm.set.custom." .. key) end
		return v
	end
	Rigel.settingNumber = function(key: string): number return tonumber(Rigel.setting(key)) or 0 end
	Rigel.startRound = function() send("rgm:start", "") end
	Rigel.ringStart = function() send("rgm:ringstart", "") end
	Rigel.startNow = function() send("rgm:startnow", "") end
	Rigel.endRound = function(winner: number?) send("rgm:end", tostring(winner or 0)) end
	Rigel.resetGame = function() send("rgm:reset", "") end
	Rigel.resetBalls = function() send("rgm:balls", "") end
	Rigel.addScore = function(team: number, amount: number?) send("rgm:score", team .. "," .. (amount or 1)) end
	Rigel.goal = function(team: number, amount: number?, key: string?) send("rgm:goal", team .. "," .. (amount or 1) .. "," .. (key or "")) end
	Rigel.setScore = function(team: number, value: number) send("rgm:setscore", team .. "," .. value) end
	Rigel.setCustom = function(key: string, value: string) send("rgm:set", key .. "=" .. value) end
	Rigel.setText = function(name: string, value: string) send(name, value) end
	-- change callbacks: one shared 0.2 s poll per script
	local subs = { state = {}, team = {}, score = {}, time = {} }
	local last = { state = nil, team = {}, score = {}, time = nil }
	local polling = false
	local function poll()
		local st = Rigel.state()
		if st ~= last.state then
			local old = last.state
			last.state = st
			for _, f in subs.state do pcall(f, st, old or "none") end
		end
		for t = 1, Rigel.teams() do
			local sz, sc = Rigel.teamSize(t), Rigel.score(t)
			if last.team[t] ~= sz then local old = last.team[t]; last.team[t] = sz; if old ~= nil then for _, f in subs.team do pcall(f, t, sz, old) end end end
			if last.score[t] ~= sc then local old = last.score[t]; last.score[t] = sc; if old ~= nil then for _, f in subs.score do pcall(f, t, sc, old) end end end
		end
		local tl = Rigel.timeLeft()
		if tl ~= last.time then last.time = tl; for _, f in subs.time do pcall(f, tl) end end
	end
	local function watch(list: any, f: any)
		table.insert(list, f)
		if not polling then polling = true; pcall(function() LuauClock.createTimer(0.2, poll) end) end
	end
	Rigel.onStateChanged = function(f: (string, string) -> ()) watch(subs.state, f) end
	Rigel.onTeamChanged = function(f: (number, number, number) -> ()) watch(subs.team, f) end
	Rigel.onScoreChanged = function(f: (number, number, number) -> ()) watch(subs.score, f) end
	Rigel.onTimeChanged = function(f: (number) -> ()) watch(subs.time, f) end
end
)LUAU";

// Typed `local` declarations become the object's editor properties (the game's "External Dependencies"):
// the component is built with one property per typed local, and the local then takes that property's value
// (its initialiser is ignored). A type the property system can't represent -- a table type like
// `{ [string]: boolean }`, a function type, a union -- made that build read garbage and crash, natively, on
// the server AND on every client that built the object (joiners included). pcall can't catch that.
// So every script is cleaned first: only the annotations that ARE properties survive (`…Component` slots,
// number, string, boolean); any other annotation on a local is removed. Removing a type annotation never
// changes what Luau code does, and such a local now keeps its initialiser (what the author meant anyway).
// Strings and comments are left alone; line numbers are kept (a removed multi-line type leaves its newlines).
static SDK::UObject* g_seLuauCtx = nullptr;   // the editor whose script is being stored (for SeLuauPutSource's note)
static bool SeLuauTypeIsProperty(std::string t)
{
    while (!t.empty() && isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
    size_t a = 0; while (a < t.size() && isspace(static_cast<unsigned char>(t[a]))) ++a;
    t = t.substr(a);
    if (!t.empty() && t.back() == '?') t.pop_back();
    if (t == "number" || t == "string" || t == "boolean") return true;
    if (t.size() > 9 && t.compare(t.size() - 9, 9, "Component") == 0)
    {
        for (char c : t) if (!(isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
        return true;
    }
    return false;
}
static std::string SeLuauSanitize(const std::string& src, int* stripped, std::string* where = nullptr)
{
    std::string out = src;
    int n = 0;
    auto isId = [](char c) { return isalnum(static_cast<unsigned char>(c)) || c == '_'; };
    // [[ ... ]] / [==[ ... ]==]: returns the index just past the close, or npos when `i` doesn't open one.
    auto longBracket = [&](size_t i) -> size_t {
        if (i >= out.size() || out[i] != '[') return std::string::npos;
        size_t j = i + 1, eq = 0;
        while (j < out.size() && out[j] == '=') { ++eq; ++j; }
        if (j >= out.size() || out[j] != '[') return std::string::npos;
        const std::string close = "]" + std::string(eq, '=') + "]";
        const size_t e = out.find(close, j + 1);
        return e == std::string::npos ? out.size() : e + close.size();
    };
    size_t i = 0;
    while (i < out.size())
    {
        const char c = out[i];
        if (c == '-' && i + 1 < out.size() && out[i + 1] == '-')            // comment
        {
            const size_t lb = longBracket(i + 2);
            if (lb != std::string::npos) { i = lb; continue; }
            const size_t e = out.find('\n', i);
            i = e == std::string::npos ? out.size() : e;
            continue;
        }
        if (c == '"' || c == '\'' || c == '`')                              // string
        {
            size_t j = i + 1;
            while (j < out.size() && out[j] != c && out[j] != '\n') { if (out[j] == '\\') ++j; ++j; }
            i = j + 1;
            continue;
        }
        if (c == '[')
        {
            const size_t lb = longBracket(i);
            if (lb != std::string::npos) { i = lb; continue; }
        }
        // `local` as a whole word, not `local function`
        if (c == 'l' && out.compare(i, 5, "local") == 0 && (i == 0 || !isId(out[i - 1])) && i + 5 < out.size() && !isId(out[i + 5]))
        {
            size_t j = i + 5;
            for (;;)                                                        // each `name [: type]` of the statement
            {
                while (j < out.size() && (out[j] == ' ' || out[j] == '\t')) ++j;
                const size_t n0 = j;
                while (j < out.size() && isId(out[j])) ++j;
                if (j == n0 || (j - n0 == 8 && out.compare(n0, 8, "function") == 0)) break;
                const std::string varName = out.substr(n0, j - n0);
                while (j < out.size() && (out[j] == ' ' || out[j] == '\t')) ++j;
                if (j < out.size() && out[j] == '<')                        // attribute: <const> / <close>
                {
                    const size_t e = out.find('>', j);
                    if (e == std::string::npos) break;
                    j = e + 1;
                    while (j < out.size() && (out[j] == ' ' || out[j] == '\t')) ++j;
                }
                if (j < out.size() && out[j] == ':')
                {
                    // The type runs to the first `=` / `,` / end of line at bracket depth 0.
                    const size_t colon = j;
                    size_t k = j + 1;
                    int depth = 0;
                    while (k < out.size())
                    {
                        const char d = out[k];
                        if (d == '{' || d == '(' || d == '<') ++depth;
                        else if (d == '}' || d == ')' || d == '>') { if (depth > 0) --depth; }
                        else if (depth == 0 && (d == '=' || d == ',' || d == '\n' || d == ';')) break;
                        else if (d == '-' && k + 1 < out.size() && out[k + 1] == '-' && depth == 0) break;
                        ++k;
                    }
                    const std::string type = out.substr(colon + 1, k - colon - 1);
                    if (!SeLuauTypeIsProperty(type))
                    {
                        for (size_t q = colon; q < k; ++q) if (out[q] != '\n') out[q] = ' ';   // keep the line count
                        ++n;
                        if (where && where->size() < 400)
                            *where += (where->empty() ? "" : ", ") + std::string("line ") +
                                      std::to_string(1 + std::count(out.begin(), out.begin() + static_cast<std::ptrdiff_t>(colon), '\n')) +
                                      " (" + varName + ")";
                    }
                    j = k;
                }
                while (j < out.size() && (out[j] == ' ' || out[j] == '\t')) ++j;
                if (j < out.size() && out[j] == ',') { ++j; continue; }
                break;
            }
            i = j;
            continue;
        }
        ++i;
    }
    if (stripped) *stripped = n;
    return out;
}

static bool SeLuauPutSource(SDK::UObject* lgm, const std::string& name, const std::string& src0)
{
    if (!lgm || !SeAlive(lgm)) return false;
    int stripped = 0;
    std::string where;
    const std::string clean = SeLuauSanitize(src0, &stripped, &where);
    if (stripped)
    {
        HxLog("[HalcyonA2][SPECEDIT] luau %s: removed %d type annotation(s) the property system can't build (they crash every machine): %s\n",
              name.c_str(), stripped, where.c_str());
        if (SDK::UObject* pc = g_seLuauCtx ? SeCallerPC(g_seLuauCtx) : nullptr)
            SeBroadcast("SE|NOTE|" + name + ": the type on " + where + " was removed - typed locals become editor properties, "
                        "and that type isn't one (it would crash every player). The script works the same; the variable keeps its value.", pc);
    }
    const std::string src = clean.find("[Rigel] safety net") == std::string::npos ? clean + kSeLuauGuard : clean;
    std::wstring w;
    if (!src.empty())
    {
        w.resize(MultiByteToWideChar(CP_UTF8, 0, src.data(), static_cast<int>(src.size()), nullptr, 0));
        MultiByteToWideChar(CP_UTF8, 0, src.data(), static_cast<int>(src.size()), w.data(), static_cast<int>(w.size()));
    }
    // Already on the server (a re-apply, or a second object running it): change its text in place with the
    // game's replicated string setter. Removing the node and adding a new one crashed the server -- the
    // replication pass still walked the removed node.
    int r = 0;
    {
        const uint64_t segs[2] = { NvNameBits(NvName("Scripts")), NvNameBits(NvName(name)) };
        NvWalk wk{}; void* parent = nullptr; int failAt = -1;
        if (NvWorldWalk(reinterpret_cast<uint8_t*>(lgm) + 0x218, segs, 1, &wk, &parent, &failAt))
            r = NvWriteNative(parent, segs[1], NvNative::TString, 0.0f, 0, w.c_str(), static_cast<int32_t>(w.size()) + 1, nullptr);
        NvWalkRelease(&wk);
        if (r == 1) HxLog("[HalcyonA2][SPECEDIT] luau %s: source updated in place\n", name.c_str());
    }
    if (r == -1 || r == 0)                               // not there yet: create it
    {
        r = SeLuauNodeCore(reinterpret_cast<uint8_t*>(lgm) + 0x218, NvNameBits(NvName("Scripts")), NvNameBits(NvName(name)),
                           w.c_str(), static_cast<int>(w.size()));
        if (r == 1 && g_seLocalTest) SeLuauDump(std::string());   // the new node, byte for byte, next to the game's own
    }
    if (SDK::AActor* slot = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(lgm) + 0x320))
        if (r == 1) static_cast<SDK::AModuleSlot*>(slot)->PushNetVars();
    HxLog("[HalcyonA2][SPECEDIT] luau %s (%zu chars) -> %s: %d\n", name.c_str(), src.size(), lgm->GetName().c_str(), r);
    return r == 1;
}
// Attach `name` (source already in the gamemode) to the object: rebuild it with the script component.
// Rebuild one of our objects from `keep` (same id, same gamemode, same Game data): how a script change reaches
// every machine. The old node goes first -- by actor, else directly by id, so two nodes never share an id.
static std::string SbRespawnKeep(SDK::AActor* a, const SbOwned& keep, const char* why)
{
    const std::string oldIdx = keep.idx;
    g_sbMovePending.erase(oldIdx);                          // this rebuild already uses the latest transform
    if (!(a && SeSandboxDelete(a)))
    {
        SbOwned tmp = keep;
        if (!SbOwnedApply(tmp, true)) { HxLog("[HalcyonA2][SPECEDIT] rebuild %s (%s): could not remove the old copy\n", oldIdx.c_str(), why); return std::string(); }
    }
    SbOwnedForget(oldIdx);
    std::vector<std::string> names;
    for (const auto& s2 : keep.scripts) names.push_back(s2.first);
    g_sbPendingScripts = names;                             // Desc component entries for the scripts
    g_sbPendingLeaves = SbRefLeaves(keep.refs);             // script slots -> references/<script> blobs
    g_sbForceLgm = keep.lgm;                                // stay in the same slot (targets live there)
    g_sbForceIdx = oldIdx;                                  // and keep the id (slots that point at it)
    const std::string prevLoading = g_lvLoading;
    g_lvLoading = keep.level;
    g_sbSpawnCls = keep.cls; g_sbSpawnPath = keep.path;
    const std::string idx = SeSandboxSpawn(keep.uniqueId, keep.loc, keep.rot, keep.scl);
    g_sbSpawnCls.clear(); g_sbSpawnPath.clear();
    g_lvLoading = prevLoading;
    g_sbPendingScripts.clear();
    g_sbForceLgm = nullptr;
    g_sbForceIdx.clear();
    if (SbOwned* n = idx.empty() ? nullptr : SbOwnedByIdx(idx)) { n->data = keep.data; n->scripts = keep.scripts; n->refs = keep.refs; }
    if (!idx.empty()) if (SDK::AActor* na = SbActorForIdx(idx)) for (const auto& d : keep.data) SeSbSet(nullptr, SeLvIdent(na), d[0], d[1], d[2]);
    HxLog("[HalcyonA2][SPECEDIT] rebuild (%s): %s -> %s\n", why, oldIdx.c_str(), idx.empty() ? "FAILED" : idx.c_str());
    return idx;
}

// name/src by VALUE: callers pass strings that live inside g_sbOwned, which the rebuild below changes
// (a reference into it dangled -- use-after-free, seen as a fault in the log line).
static std::string SeLuauAttach(SDK::AActor* a, const std::string name, const std::string src)
{
    SDK::UObject* pc = SbPrefabOf(a);
    if (!pc) return std::string();
    SbOwned* o = SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248)));
    if (!o || o->uniqueId.empty()) return std::string();
    SbOwned keep = *o;
    bool had = false;
    for (auto& s2 : keep.scripts) if (s2.first == name) { s2.second = src; had = true; }
    if (!had) keep.scripts.push_back({ name, src });
    const std::string idx = SbRespawnKeep(a, keep, "attach script");
    HxLog("[HalcyonA2][SPECEDIT] luau %s attached: %s -> %s\n", name.c_str(), keep.idx.c_str(), idx.empty() ? "FAILED" : idx.c_str());
    return idx;
}

// SE|LUAUDEL|<ident>|<script>: take a script off an object we placed (and its slot wiring); the object is
// rebuilt without it, on every machine.
static void SeLuauRemove(SDK::UObject* ctx, const std::string& ident, const std::string& script)
{
    SDK::UObject* caller = SeCallerPC(ctx);
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    SbOwned* o = pc ? SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248))) : nullptr;
    const std::string name = SeLuauName(script);
    if (!o)
    {
        SeError(caller, "Can't remove that script", "Scripts can only be removed from objects you placed with the editor. "
                "This one belongs to the station. To change what it does, use Replace with a file from your folder.");
        return;
    }
    SbOwned keep = *o;
    const size_t before = keep.scripts.size();
    keep.scripts.erase(std::remove_if(keep.scripts.begin(), keep.scripts.end(), [&](const auto& s2) { return s2.first == name; }), keep.scripts.end());
    keep.refs.erase(std::remove_if(keep.refs.begin(), keep.refs.end(), [&](const std::array<std::string, 4>& r) { return r[0] == name; }), keep.refs.end());
    if (keep.scripts.size() == before) { if (caller) SeBroadcast("SE|NOTE|" + name + " isn't on that object.", caller); return; }
    const std::string idx = SbRespawnKeep(a, keep, "remove script");
    HxLog("[HalcyonA2][SPECEDIT] luau %s removed from %s: %s\n", name.c_str(), keep.idx.c_str(), idx.empty() ? "FAILED" : "ok");
    if (caller) SeBroadcast(idx.empty() ? "SE|NOTE|Removing " + name + " failed - see the server log."
                                        : "SE|NOTE|Removed " + name + " (the object was rebuilt without it).", caller);
    if (!idx.empty()) if (SDK::AActor* na = SbActorForIdx(idx)) SeSbData(ctx, SeLvIdent(na));
}
static void SeLuauPart(const std::string& rawName, const std::string& hex)
{
    const std::string name = SeLuauName(rawName);
    if (name.empty()) return;
    std::string& buf = g_luauParts[name];
    if (buf.size() > 200000) return;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) buf.push_back(static_cast<char>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
}
// SE|LUAUSRC|<name> -- a script file was saved in VS Code: re-send it to every object we placed that runs
// it (each is rebuilt to pick up the new code). Sent after the LUAUPART chunks, like SE|LUAU.
static void SeLuauUpdateAllImpl(SDK::UObject* ctx, const std::string& rawName);
static void SeLuauUpdateAll(SDK::UObject* ctx, const std::string& rawName)
{
    g_seLuauCtx = ctx;
    SeLuauUpdateAllImpl(ctx, rawName);
    g_seLuauCtx = nullptr;
}
static void SeLuauUpdateAllImpl(SDK::UObject* ctx, const std::string& rawName)
{
    SDK::UObject* caller = SeCallerPC(ctx);
    const std::string name = SeLuauName(rawName);
    auto it = g_luauParts.find(name);
    if (name.empty() || it == g_luauParts.end()) return;
    const std::string src = it->second;
    g_luauParts.erase(it);
    std::vector<std::string> idxs;
    for (const auto& o : g_sbOwned)
        for (const auto& sc : o.scripts) if (sc.first == name) { idxs.push_back(o.idx); break; }
    int n = 0;
    for (const auto& idx : idxs)
    {
        SDK::AActor* a = SbActorForIdx(idx);
        SDK::UObject* pc = a ? SbPrefabOf(a) : nullptr;
        SDK::UObject* lgm = pc ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440) : nullptr;
        if (lgm && SeLuauPutSource(lgm, name, src) && !SeLuauAttach(a, name, src).empty()) ++n;
    }
    if (caller) SeBroadcast("SE|NOTE|" + name + " updated on " + std::to_string(n) + " object(s).", caller);
}

static void SeLuauImpl(SDK::UObject* ctx, const std::string& ident, const std::string& rawName);
static void SeLuau(SDK::UObject* ctx, const std::string& ident, const std::string& rawName)
{
    g_seLuauCtx = ctx;
    SeLuauImpl(ctx, ident, rawName);
    g_seLuauCtx = nullptr;
}
static void SeLuauImpl(SDK::UObject* ctx, const std::string& ident, const std::string& rawName)
{
    SDK::UObject* caller = SeCallerPC(ctx);
    const std::string name = SeLuauName(rawName);
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    auto it = g_luauParts.find(name);
    if (name.empty() || !pc || it == g_luauParts.end())
    {
        if (caller) SeBroadcast("SE|NOTE|Script not attached: pick an object placed through the sandbox, and a name of letters/numbers.", caller);
        return;
    }
    const std::string src = it->second;
    g_luauParts.erase(it);
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440);
    SbOwned* own = SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248)));
    if (!own)
    {
        // Not ours (a station object): update the source everyone compiles; the object's already-built
        // script component keeps the old code until the object is rebuilt (rejoin / gamemode reload).
        const bool put = SeLuauPutSource(lgm, name, src);
        if (caller) SeBroadcast(put ? "SE|NOTE|Updated " + name + " on the server. Players who join from now get the new code; players "
                                      "already here keep running the old copy until they rejoin." :
                                      "SE|NOTE|Updating " + name + " FAILED - see the server log.", caller);
        return;
    }
    std::string newIdx;
    const bool ok = SeLuauPutSource(lgm, name, src) && !(newIdx = SeLuauAttach(a, name, src)).empty();
    if (caller) SeBroadcast(ok ? "SE|NOTE|Script " + name + " is running on the object for everyone (it was rebuilt to pick it up)." :
                                 "SE|NOTE|Attaching " + name + " FAILED - see the server log.", caller);
    if (ok) if (SDK::AActor* na = SbActorForIdx(newIdx)) SeSbData(ctx, SeLvIdent(na));   // fields, straight away
}


// ---- Script slots (object references) ------------------------------------------------------------------
// A script declares typed slots (`local Target: PhysicalComponent = nil`); the editor fills one by dropping
// an object on it. The game binds them from the scripted object's references/<script> blob:
//   02 01 00 00 00 | int32 count | per slot: 2D 00, FString slot, 02, uint32 B, C, D (target GUID parts
//   2-4; part 1 = the slot number), FString component key (Luau type minus "Component", first letter low).
// References are read when the object is built, so wiring rebuilds the scripted object; the target must
// live in the same gamemode slot (it is moved there if not).
static bool SbGuidParts(const std::string& idx, uint32_t* b, uint32_t* c, uint32_t* d)
{
    unsigned a1, b1, b2, c1, c2, dd;
    if (idx.size() != 36 || sscanf_s(idx.c_str(), "%8x-%4x-%4x-%4x-%4x%8x", &a1, &b1, &b2, &c1, &c2, &dd) != 6) return false;
    *b = (b1 << 16) | b2; *c = (c1 << 16) | c2; *d = dd;
    return true;
}
static std::vector<SbLeaf> SbRefLeaves(const std::vector<std::array<std::string, 4>>& refs)
{
    std::vector<SbLeaf> out;
    std::vector<std::string> scripts;
    for (const auto& r : refs) if (std::find(scripts.begin(), scripts.end(), r[0]) == scripts.end()) scripts.push_back(r[0]);
    auto putStr = [](std::vector<uint8_t>& v, const std::string& str) {
        const int32_t n = static_cast<int32_t>(str.size()) + 1;
        v.insert(v.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
        v.insert(v.end(), str.begin(), str.end());
        v.push_back(0);
    };
    for (const auto& sc : scripts)
    {
        std::vector<uint8_t> v = { 0x02, 0x01, 0x00, 0x00, 0x00 };
        int32_t count = 0;
        const size_t countAt = v.size();
        v.insert(v.end(), 4, 0);
        for (const auto& r : refs)
        {
            uint32_t b, c, d;
            if (r[0] != sc || !SbGuidParts(r[2], &b, &c, &d)) continue;
            v.push_back(0x2D); v.push_back(0x00);
            putStr(v, r[1]);
            v.push_back(0x02);
            for (uint32_t w : { b, c, d }) v.insert(v.end(), reinterpret_cast<const uint8_t*>(&w), reinterpret_cast<const uint8_t*>(&w) + 4);
            putStr(v, r[3]);
            ++count;
        }
        memcpy(v.data() + countAt, &count, 4);
        SbLeaf l;
        l.inProps = false; l.name = sc; l.blob = v; l.inRefs = true;
        out.push_back(l);
    }
    return out;
}
static std::string SbKeyForType(const std::string& type)
{
    std::string k = type;
    if (k.size() > 9 && k.compare(k.size() - 9, 9, "Component") == 0) k.resize(k.size() - 9);
    if (!k.empty()) k[0] = static_cast<char>(tolower(static_cast<unsigned char>(k[0])));
    return k;
}
// Move a placed object into another gamemode (respawn there, keeping everything). Returns the new idx.
static std::string SbRehost(SbOwned& o, SDK::UObject* lgm)
{
    SDK::AActor* a = SbActorForIdx(o.idx);
    SbOwned keep = o;
    if (a) SeSandboxDelete(a); else SbOwnedApply(o, true);
    for (size_t i = 0; i < g_sbOwned.size(); ++i) if (g_sbOwned[i].idx == keep.idx) { g_sbOwned.erase(g_sbOwned.begin() + i); break; }
    std::vector<std::string> names;
    for (const auto& s2 : keep.scripts) names.push_back(s2.first);
    g_sbPendingScripts = names;
    g_sbPendingLeaves = SbRefLeaves(keep.refs);
    g_sbForceLgm = lgm;
    const std::string prevLoading = g_lvLoading;
    g_lvLoading = keep.level;
    g_sbSpawnCls = keep.cls; g_sbSpawnPath = keep.path;
    const std::string idx = SeSandboxSpawn(keep.uniqueId, keep.loc, keep.rot, keep.scl);
    g_sbSpawnCls.clear(); g_sbSpawnPath.clear();
    g_lvLoading = prevLoading;
    g_sbPendingScripts.clear();
    g_sbForceLgm = nullptr;
    if (SbOwned* n = idx.empty() ? nullptr : SbOwnedByIdx(idx)) { n->data = keep.data; n->scripts = keep.scripts; n->refs = keep.refs; }
    if (!idx.empty()) if (SDK::AActor* na = SbActorForIdx(idx)) for (const auto& d : keep.data) SeSbSet(nullptr, SeLvIdent(na), d[0], d[1], d[2]);
    // anything that pointed at the old id now points at the new one
    for (auto& other : g_sbOwned) for (auto& r : other.refs) if (r[2] == keep.idx) r[2] = idx;
    if (auto ol = g_ownLocks.find(keep.idx); ol != g_ownLocks.end() && !idx.empty())
    {
        const SeOwnLock moved = ol->second;
        g_ownLocks.erase(ol);
        g_ownLocks[idx] = moved;
    }
    return idx;
}
// SE|SBDUP|<ident#id>|loc|rot|scale -- Duplicate. A copy of a sandbox object made from the object itself:
// same catalogue entry, Game data, scripts and script slots, at the given transform, hosted where the
// source is. Duplicate used to re-spawn from the Level Editor palette, so catalogue-only pieces (the
// yellow boost pad is PrimitiveCubeYellow) had no palette entry and could not be duplicated at all.
static void SeSandboxDuplicate(SDK::UObject* pawn, const std::string& name, const std::string& locs,
                               const std::string& rots, const std::string& scls)
{
    const size_t h = name.find('#');
    SbOwned* src = h == std::string::npos ? nullptr : SbOwnedByIdx(name.substr(h + 1));
    if (!src || src->uniqueId.empty())
    {
        HxLog("[HalcyonA2][SPECEDIT] duplicate: %s is not a placed sandbox object\n", name.c_str());
        if (SDK::UObject* pc = SeCallerPC(pawn)) SeBroadcast("SE|NOTE|That object can't be duplicated.", pc);
        return;
    }
    const SbOwned keep = *src;                           // copies: the spawn below may grow g_sbOwned
    double loc[3], rot[3], scl[3];
    memcpy(loc, keep.loc, sizeof(loc)); memcpy(rot, keep.rot, sizeof(rot)); memcpy(scl, keep.scl, sizeof(scl));
    SeVec(locs, loc); SeVec(rots, rot); SeVec(scls, scl);
    for (double& v : scl) if (!(v > 0.01 && v < 100.0)) v = 1;
    std::vector<std::string> names;
    for (const auto& s2 : keep.scripts) names.push_back(s2.first);
    g_sbPendingScripts = names;
    g_sbPendingLeaves = SbRefLeaves(keep.refs);
    g_sbForceLgm = keep.lgm && SeAlive(keep.lgm) ? keep.lgm : nullptr;   // its scripts' source lives there
    const std::string prevLoading = g_lvLoading;
    g_lvLoading = SeCallerScene();                       // the copy belongs to the duplicating editor's open level
    g_sbSpawnCls = keep.cls; g_sbSpawnPath = keep.path;
    const std::string idx = SeSandboxSpawn(keep.uniqueId, loc, rot, scl);
    g_sbSpawnCls.clear(); g_sbSpawnPath.clear();
    g_lvLoading = prevLoading;
    g_sbPendingScripts.clear();
    g_sbForceLgm = nullptr;
    if (SbOwned* n = idx.empty() ? nullptr : SbOwnedByIdx(idx)) { n->data = keep.data; n->scripts = keep.scripts; n->refs = keep.refs; }
    if (SDK::AActor* na = idx.empty() ? nullptr : SbActorForIdx(idx))
    {
        for (const auto& d : keep.data) SeSbSet(nullptr, SeLvIdent(na), d[0], d[1], d[2]);
        SeMarkProxyDirty(na);
    }
    HxLog("[HalcyonA2][SPECEDIT] duplicate %s (%s) -> %s\n", keep.idx.c_str(), keep.uniqueId.c_str(), idx.empty() ? "FAILED" : idx.c_str());
}
// Rebuild a scripted object so its script picks up its (new) references.
static std::string SbRebuildScripted(SbOwned& o)
{
    SDK::AActor* a = SbActorForIdx(o.idx);
    if (!a || o.scripts.empty()) return std::string();
    const std::string name = o.scripts.front().first, src = o.scripts.front().second;   // copies: o is rebuilt
    return SeLuauAttach(a, name, src);
}

// A script slot binds to one of the target's Luau components by its NAME, first letter lowered: the game's
// own golf course points "Hole_1" at key "physical", the GolfHole's component named "Physical". Find the
// target's component of the slot's type; `have` lists what it does have (for the error message).
static std::string SbComponentKey(SDK::AActor* t, const std::string& type, std::string* have)
{
    SDK::UClass* want = SDK::UObject::FindClassFast(type);
    static SDK::UClass* luauCls = nullptr;
    if (!luauCls) luauCls = SDK::UObject::FindClassFast("LuauBehavior");
    std::string key;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; t && i < n; ++i)
    {
        SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
        if (!c || c->Outer != t || c->IsDefaultObject() || !c->Class) continue;
        const std::string cn = c->Class->GetName();
        if (luauCls && c->IsA(luauCls) && cn.find("Luau") == std::string::npos && have->find(cn) == std::string::npos)
            *have += (have->empty() ? "" : ", ") + cn;
        if (key.empty() && want && c->IsA(want))
        {
            // The key is the component's Luau name: its CLASS name without "Component", first letter lowered -- as the
            // station's own wiring has it (ScoreComponent -> "score", GameTimeComponent -> "gameTime",
            // GameStateManagerComponent -> "gameStateManager", PhysicalComponent -> "physical"). The component
            // OBJECT name only matched by luck: the Game State Manager's timer object is "GameTimeManager" and its
            // score object "ScoreComponent", so those slots came up nil on every machine.
            key = cn;
            if (key.size() > 9 && key.compare(key.size() - 9, 9, "Component") == 0) key.resize(key.size() - 9);
            if (!key.empty()) key[0] = static_cast<char>(tolower(static_cast<unsigned char>(key[0])));
        }
    }
    return key;
}

// SE|ERR|<title>|<what happened and how to fix it>: shown to the editor as a popup.
static void SeError(SDK::UObject* caller, const std::string& title, const std::string& text)
{
    HxLog("[HalcyonA2][SPECEDIT] error to editor: %s - %s\n", title.c_str(), text.c_str());
    if (caller) SeBroadcast("SE|ERR|" + title + "|" + text, caller);
}

// SE|LUAUREF|<scripted object ident>|<script>|<slot>|<type>|<target ident, or "-" to clear>
// A script variable of the gamemode hosting `a` (what Luau's Gamemode:setStringVariable / setNumberVariable /
// setBoolVariable write): LoadedGameMode(+0x320 slot) -> AModuleSlot::ModuleState(+0x300) -> SubVariables(+0xA0).
// Returns false when the object, its gamemode or the variable isn't there.
static bool SeGamemodeVar(SDK::AActor* a, const std::string& name, std::string* out)
{
    SDK::UObject* pc = SbPrefabOf(a);
    SDK::UObject* lgm = pc ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440) : nullptr;
    if (!lgm || !SeAlive(lgm)) return false;
    SDK::UObject* slot = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(lgm) + 0x320);
    SDK::UObject* ms = slot && SeAlive(slot) ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(slot) + 0x300) : nullptr;
    if (!ms || !SeAlive(ms)) return false;
    void* vars = reinterpret_cast<uint8_t*>(ms) + 0xA0;
    const uint64_t fn = NvNameBits(NvName(name));
    NvOrig o{};
    if (NvReadNative(vars, fn, NvNative::TString, &o) != 1) return false;
    out->clear();
    for (const wchar_t* w = o.str; *w; ++w) *out += static_cast<char>(*w < 128 ? *w : '?');
    return true;
}
static std::string g_seForceRefKey;   // SE|REFKEY|<key> (local test): the next slot wiring uses this component key
static void SeLuauRef(SDK::UObject* ctx, const std::string& ident, const std::string& script, const std::string& slot,
                      const std::string& type, const std::string& targetIdent)
{
    SDK::UObject* caller = SeCallerPC(ctx);
    auto note = [&](const std::string& m) { if (caller) SeBroadcast("SE|NOTE|" + m, caller); };
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = SbPrefabOf(a);
    SbOwned* o = pc ? SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248))) : nullptr;
    const std::string sname = SeLuauName(script);
    if (!o || sname.empty()) { note("Wire slots on an object you placed that runs the script."); return; }
    const std::string myIdx = o->idx;                    // g_sbOwned may reallocate below: find it again by id
    o->refs.erase(std::remove_if(o->refs.begin(), o->refs.end(), [&](const std::array<std::string, 4>& r) { return r[0] == sname && r[1] == slot; }),
                  o->refs.end());
    std::string targetName = "nothing";
    if (targetIdent != "-")
    {
        SDK::AActor* t = SeFindEditorActor(targetIdent);
        SDK::UObject* tpc = SbPrefabOf(t);
        SbOwned* to = tpc ? SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(tpc) + 0x248))) : nullptr;
        const std::string tname = t && t->Class ? t->Class->GetName() : std::string("that object");
        if (!to)
        {
            SeError(caller, "Can't use that object",
                    "Slot '" + slot + "' can only point at objects you placed with the editor. " + tname +
                    " belongs to the station. Place your own copy and drag that onto the slot instead.");
            return;
        }
        std::string have;
        std::string key = SbComponentKey(t, type, &have);
        if (!g_seForceRefKey.empty() && !key.empty()) { HxLog("[HalcyonA2][SPECEDIT] slot key %s forced to %s\n", key.c_str(), g_seForceRefKey.c_str()); key = g_seForceRefKey; }
        g_seForceRefKey.clear();
        if (key.empty())
        {
            SeError(caller, "Wrong kind of object for " + slot,
                    "Slot '" + slot + "' needs an object with a " + type + ", but " + tname + " has " +
                    (have.empty() ? std::string("no scriptable components at all") : "only: " + have) +
                    ". Pick a different object (the slot's list shows the ones that fit), or change the slot's type in "
                    "the script, e.g.  local " + slot + ": " + (have.empty() ? std::string("PhysicalComponent") : have.substr(0, have.find(','))) + " = nil");
            return;
        }
        std::string tidx = to->idx;
        uint32_t b, c, d;
        // An object only draws inside the game area that hosts it, so wiring one in from somewhere else used
        // to make it vanish where it stood. Two objects standing side by side belong in one area, though --
        // older placements could land in different ones -- and moving such a target into the scripted
        // object's area is exactly the repair. Only a target that really belongs elsewhere is refused.
        if (to->lgm != o->lgm)
        {
            if (SbHostGamemode(to->uniqueId, to->loc) != o->lgm)
            {
                SeError(caller, "Those are in different game areas",
                        "Slot '" + slot + "' can only point at an object in the same game area as the scripted object. "
                        "Place " + tname + " next to the scripted object (same area) and wire it again.");
                return;
            }
            HxLog("[HalcyonA2][SPECEDIT] %s sits in the scripted object's area but is hosted elsewhere -- moving it there\n", tname.c_str());
            tidx = SbRehost(*to, o->lgm);
            if ((o = SbOwnedByIdx(myIdx)) == nullptr || tidx.empty())
            {
                SeError(caller, "Wiring " + slot + " failed",
                        "Couldn't move " + tname + " into the scripted object's game area. Delete it, place it again "
                        "next to the scripted object, and wire it.");
                return;
            }
        }
        else if (!SbGuidParts(tidx, &b, &c, &d))
            tidx = SbRehost(*to, o->lgm);                 // same area, older id: re-create it so it can be referenced
        // A rehost re-created both the object and the vector holding it: nothing from before it still stands.
        o = SbOwnedByIdx(myIdx);
        if (!o || tidx.empty())
        {
            SeError(caller, "Wiring " + slot + " failed", "The object couldn't be moved into the scripted object's gamemode area. "
                    "Place the object closer to the scripted one (in the same game area) and try again.");
            return;
        }
        t = SbActorForIdx(tidx);
        o->refs.push_back({ sname, slot, tidx, key });
        targetName = t && t->Class ? t->Class->GetName() : tidx;
    }
    SbOwned* cur = SbOwnedByIdx(myIdx);                   // rebuild THIS object (it's the one whose wiring changed)
    const std::string idx = cur ? SbRebuildScripted(*cur) : std::string();
    HxLog("[HalcyonA2][SPECEDIT] script slot %s.%s (%s) -> %s: %s\n", sname.c_str(), slot.c_str(), type.c_str(), targetName.c_str(),
          idx.empty() ? "FAILED" : idx.c_str());
    note(idx.empty() ? "Wiring " + slot + " failed - see the server log." : slot + " -> " + targetName + " (the scripted object was rebuilt to pick it up).");
    if (!idx.empty()) if (SDK::AActor* na = SbActorForIdx(idx)) SeSbData(ctx, SeLvIdent(na));
}

// Saved levels. What the editor builds -- sandbox objects, speed pads and coins, their Game data,
// quests, quest groups and coin runs -- can be saved to the backend as a named level, and loaded back on
// any server: at boot (if the level is set to autoload on the dashboard) or live (Load/Unload on the
// dashboard or in the editor). The server polls the backend every 15 s for what should be loaded.
// Kiosk / button / run quest references are stored as quest ids ({q:<id>}), and an authored quest's
// GUID is derived from its id, so a loaded level wires itself back up.
// Format: plain text, one record per line, tab-separated (see SeLvBuild).
// ============================================================================================
struct SeLvPlain { SDK::AActor* actor = nullptr; int32_t index = -1; std::string path, cls, level; };
static std::vector<SeLvPlain> g_lvPlain;
static std::vector<std::string> g_lvLoaded;         // levels loaded on this server
// Levels an editor put here itself (an opened scene, an imported file, a save): the backend's "desired" list
// doesn't know them, so the poller must leave them alone -- only Close / Unload removes them.
static std::vector<std::string> g_lvLocal;
static void SeLvMarkLocal(const std::string& n) { if (std::find(g_lvLocal.begin(), g_lvLocal.end(), n) == g_lvLocal.end()) g_lvLocal.push_back(n); }
static std::mutex g_lvMu;                            // guards the queue below (worker thread -> game thread)
struct SeLvJob { int kind; std::string name, text; SDK::UObject* pc = nullptr; };   // 1 load 2 unload 3 note-to-caller
static std::vector<SeLvJob> g_lvJobs;
static std::atomic<bool> g_lvStatusDirty{ false };
static std::atomic<bool> g_lvPollNow{ false };

static void SeLvRecordPlain(SDK::AActor* a, const std::string& path)
{
    if (!a) return;
    g_lvPlain.push_back({ a, a->Index, path, a->Class ? a->Class->GetName() : std::string(), g_lvLoading });
}
static bool SeLvPlainAlive(const SeLvPlain& p)
{
    return p.actor && SeAlive(p.actor) && p.actor->Index == p.index && !(*(reinterpret_cast<const uint8_t*>(p.actor) + 0x65) & 0x01);
}

static std::string SeLvClean(std::string v)
{
    for (auto& c : v) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return v;
}
static std::string SeLvVec(const double* v)
{
    char b[96];
    snprintf(b, sizeof(b), "%.2f,%.2f,%.2f", v[0], v[1], v[2]);
    return b;
}
static std::string SeLvGuidOf(const std::string& questId)
{
    uint32_t g[4];
    SeQuestGuid(questId, g);
    char hx[40];
    snprintf(hx, sizeof(hx), "%08X%08X%08X%08X", g[0], g[1], g[2], g[3]);
    return hx;
}
// GUID(s) of our own quests -> {q:<id>} so a saved level re-wires to its quests wherever it loads.
static std::string SeLvSymbolic(const std::string& value)
{
    std::string out;
    size_t b = 0;
    while (true)
    {
        size_t e = value.find(',', b);
        const bool last = e == std::string::npos;
        std::string part = value.substr(b, last ? std::string::npos : e - b);
        for (const auto& q : g_seAuthored) if (!q.deleted && part.size() == 32 && SeLvGuidOf(q.questId) == part) { part = "{q:" + q.questId + "}"; break; }
        out += part;
        if (last) break;
        out += ',';
        b = e + 1;
    }
    return out;
}
static std::string SeLvResolve(const std::string& value)
{
    std::string out = value;
    for (size_t at; (at = out.find("{q:")) != std::string::npos;)
    {
        const size_t end = out.find('}', at);
        if (end == std::string::npos) break;
        out = out.substr(0, at) + SeLvGuidOf(out.substr(at + 3, end - at - 3)) + out.substr(end + 1);
    }
    return out;
}

// Build a level from the editor's unsaved work plus everything already belonging to `name`, and tag it
// all as belonging to `name`.
// A save captures EVERYTHING built on this server -- unsaved work and every loaded level's content -- and
// the saved objects become that level's. It used to take only unsaved work plus content already tagged
// with this exact name, so loading a level, editing it and saving (under any other name, or a different
// capitalisation) wrote only the new pieces and silently dropped everything that had been loaded.
static std::string SeModesForLevel(const std::string& name, bool tag, const std::unordered_map<std::string, int>& ownedIdx);   // below
static void SeModesLoadCreate(const std::string& level, const std::vector<std::vector<std::string>>& lines);                   // below
static void SeModesLoadFixup(const std::string& level, const std::unordered_map<int, std::string>& objIdx);                    // below
static void SeModesUnload(const std::string& level);                                                                          // below
static std::string SeLvBuild(const std::string& name, int* counts, bool tag = true)   // tag=false: export only
{
    std::string t = "L\t" + SeLvClean(name) + "\t1\n";
    struct Wire { int from; std::string script, slot, targetIdx, key; };
    std::vector<Wire> wiring;
    std::unordered_map<std::string, int> ownedIdx;       // sandbox idx -> object number
    std::unordered_map<SDK::AActor*, int> actorIdx;      // actor -> object number (checkpoints)
    std::unordered_set<std::string> runObjs;             // run + button nodes (rebuilt from R records)
    for (const auto& kv : g_seCoinRuns) { runObjs.insert(kv.second.first); runObjs.insert(kv.second.second); }
    int n = 0;
    for (auto& o : g_sbOwned)
    {
        if (runObjs.count(o.idx) || o.uniqueId.empty() || o.cls.empty()) continue;
        if (o.lgm && !SeAlive(o.lgm)) continue;
        if (tag && !o.level.empty() && o.level != name) continue;   // another level's (another editor's scene)
        if (tag) o.level = name;
        ownedIdx[o.idx] = n;
        if (SDK::AActor* a = SbActorForIdx(o.idx)) actorIdx[a] = n;
        t += "O\t" + std::to_string(n) + "\tS\t" + o.uniqueId + "\t" + o.cls + "\t" + SeLvClean(o.path) + "\t" + SeLvVec(o.loc) + "\t" +
             SeLvVec(o.rot) + "\t" + SeLvVec(o.scl) + "\n";
        for (const auto& d : o.data)
            t += "D\t" + std::to_string(n) + "\t" + d[0] + "\t" + d[1] + "\t" + SeLvClean(SeLvSymbolic(d[2])) + "\n";
        for (const auto& r : o.refs) wiring.push_back({ n, r[0], r[1], r[2], r[3] });
        for (const auto& sc : o.scripts)
        {
            std::string hex;
            char hb[4];
            for (unsigned char c : sc.second) { snprintf(hb, sizeof(hb), "%02X", c); hex += hb; }
            t += "X\t" + std::to_string(n) + "\t" + sc.first + "\t" + hex + "\n";
        }
        ++n;
    }
    for (auto& p : g_lvPlain)
    {
        if (!SeLvPlainAlive(p)) continue;
        if (tag && !p.level.empty() && p.level != name) continue;
        if (tag) p.level = name;
        const SDK::FVector l = p.actor->K2_GetActorLocation(); const SDK::FRotator r = p.actor->K2_GetActorRotation();
        const SDK::FVector sc = p.actor->GetActorScale3D();
        const double L[3] = { l.X, l.Y, l.Z }, R[3] = { r.Pitch, r.Yaw, r.Roll }, S[3] = { sc.X, sc.Y, sc.Z };
        actorIdx[p.actor] = n;
        t += "O\t" + std::to_string(n) + "\tP\t-\t" + p.cls + "\t" + SeLvClean(p.path) + "\t" + SeLvVec(L) + "\t" + SeLvVec(R) + "\t" + SeLvVec(S) + "\n";
        for (const auto& e : g_seEdits)
            if (e.actor == p.actor) t += "E\t" + std::to_string(n) + "\t" + e.path + "\t" + SeLvClean(e.value) + "\n";
        ++n;
    }
    for (const auto& w : wiring)                          // W <from obj> <script> <slot> <target obj> <key>
        if (auto it = ownedIdx.find(w.targetIdx); it != ownedIdx.end())
            t += "W\t" + std::to_string(w.from) + "\t" + w.script + "\t" + w.slot + "\t" + std::to_string(it->second) + "\t" + w.key + "\n";
    int nq = 0, nr = 0;
    for (auto& q : g_seAuthored)
    {
        if (q.deleted) continue;
        if (tag && !q.level.empty() && q.level != name) continue;
        if (tag) q.level = name;
        std::string steps;
        for (SDK::AActor* a : q.checkpoints)
        {
            auto it = actorIdx.find(a);
            if (it != actorIdx.end()) steps += (steps.empty() ? "" : ";") + std::to_string(it->second);
        }
        t += "Q\t" + q.questId + "\t" + SeLvClean(std::string(q.title.begin(), q.title.end())) + "\t" + q.glyph + "\t" +
             std::to_string(q.rep) + "\t" + SeLvClean(std::string(q.desc.begin(), q.desc.end())) + "\t" +
             std::to_string(static_cast<int>(q.radius)) + "\t" + std::to_string(q.timeLimit) + "\t" + steps + "\t" + q.childIds + "\n";
        ++nq;
    }
    for (auto& r : g_lvRuns)
    {
        if (tag && !r.level.empty() && r.level != name) continue;
        if (tag) r.level = name;
        char b[64];
        snprintf(b, sizeof(b), "%.0f", r.dur);
        t += "R\t" + r.questRef + "\t" + b + "\t" + std::to_string(r.thr) + "\t" + SeLvVec(r.at) + "\t" + SeLvVec(r.button) + "\t" + r.coins + "\n";
        ++nr;
    }
    t += SeModesForLevel(name, tag, ownedIdx);             // G: the level's game modes (roles by object number)
    if (counts) { counts[0] = n; counts[1] = nq; counts[2] = nr; }
    return t;
}

static std::vector<std::string> SeLvFields(const std::string& line)
{
    std::vector<std::string> f;
    for (size_t b = 0; b <= line.size();)
    {
        size_t e = line.find('\t', b);
        if (e == std::string::npos) e = line.size();
        f.push_back(line.substr(b, e - b));
        b = e + 1;
        if (e == line.size()) break;
    }
    return f;
}

static std::string SeLvIdent(SDK::AActor* a)
{
    if (!a || !a->Class) return std::string();
    const SDK::FVector l = a->K2_GetActorLocation();
    char b[256];
    snprintf(b, sizeof(b), "%s@%.1f,%.1f,%.1f", a->Class->GetName().c_str(), l.X, l.Y, l.Z);
    return b;
}

// Load a level (game thread). Objects first, then their data, then quests (checkpoints point at objects),
// then coin runs (which carry their own coins and button).
static void SeLvLoad(const std::string& name, const std::string& text)
{
    if (std::find(g_lvLoaded.begin(), g_lvLoaded.end(), name) != g_lvLoaded.end()) return;
    SDK::UObject* ctx = SbEngine() ? static_cast<SDK::UObject*>(SDK::UWorld::GetWorld()) : nullptr;
    if (!ctx) { HxLog("[HalcyonA2][LEVELS] %s: sandbox not ready, will retry\n", name.c_str()); g_lvPollNow = true; return; }
    g_lvLoading = name;
    std::unordered_map<int, SDK::AActor*> objs;
    std::unordered_map<int, std::string> objIdx;
    int no = 0, nd = 0, nq = 0, nr = 0;
    std::vector<std::vector<std::string>> lines;
    for (size_t b = 0; b < text.size();)
    {
        size_t e = text.find('\n', b);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(b, e - b);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        b = e + 1;
        if (!line.empty()) lines.push_back(SeLvFields(line));
    }
    SeModesLoadCreate(name, lines);                       // game modes first: their objects are hosted inside them
    for (const auto& f : lines)
    {
        if (f[0] != "O" || f.size() < 9) continue;
        const int i = atoi(f[1].c_str());
        double loc[3], rot[3], scl[3] = { 1, 1, 1 };
        if (!SeVec(f[6], loc) || !SeVec(f[7], rot)) continue;
        SeVec(f[8], scl);
        if (f[2] == "S")
        {
            g_sbSpawnCls = f[4]; g_sbSpawnPath = f[5];
            const std::string idx = SeSandboxSpawn(f[3], loc, rot, scl);
            g_sbSpawnCls.clear(); g_sbSpawnPath.clear();
            if (!idx.empty()) { objIdx[i] = idx; objs[i] = SbActorForIdx(idx); ++no; }
        }
        else
        {
            const size_t before = g_lvPlain.size();
            SeSpawn(ctx, f[5], f[6], f[7]);
            if (g_lvPlain.size() > before)
            {
                SDK::AActor* a = g_lvPlain.back().actor;
                if (scl[0] != 1 || scl[1] != 1 || scl[2] != 1) a->SetActorScale3D(SDK::FVector{ scl[0], scl[1], scl[2] });
                objs[i] = a; ++no;
            }
        }
    }
    for (const auto& f : lines)
    {
        if ((f[0] == "D" && f.size() >= 5) || (f[0] == "E" && f.size() >= 4))
        {
            auto it = objs.find(atoi(f[1].c_str()));
            if (it == objs.end() || !it->second) continue;
            const std::string ident = SeLvIdent(it->second);
            if (f[0] == "D") SeSbSet(nullptr, ident, f[2], f[3], SeLvResolve(f[4]));
            else SeSetProp(ident, f[2], f[3]);
            ++nd;
        }
    }
    // Script slots: record them on the scripted objects first (all in the first object's slot), so the
    // X pass below builds them with their references.
    for (const auto& f : lines)
    {
        if (f[0] != "W" || f.size() < 6) continue;
        auto from = objs.find(atoi(f[1].c_str())), to = objs.find(atoi(f[4].c_str()));
        if (from == objs.end() || to == objs.end() || !from->second || !to->second) continue;
        SDK::UObject* fpc = SbPrefabOf(from->second);
        SDK::UObject* tpc = SbPrefabOf(to->second);
        SbOwned* fo = fpc ? SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fpc) + 0x248))) : nullptr;
        SbOwned* tobj = tpc ? SbOwnedByIdx(FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(tpc) + 0x248))) : nullptr;
        if (!fo || !tobj) continue;
        std::string tidx = tobj->idx;
        const std::string fromIdx = fo->idx;
        if (tobj->lgm != fo->lgm) { tidx = SbRehost(*tobj, fo->lgm); to->second = SbActorForIdx(tidx); }
        if (SbOwned* again = SbOwnedByIdx(fromIdx)) again->refs.push_back({ SeLuauName(f[2]), f[3], tidx, f[5] });
    }
    for (const auto& f : lines)                            // custom Luau: source into the gamemode, then attach
    {
        if (f[0] != "X" || f.size() < 4) continue;
        auto it = objs.find(atoi(f[1].c_str()));
        if (it == objs.end() || !it->second) continue;
        std::string src;
        for (size_t i = 0; i + 1 < f[3].size(); i += 2) src.push_back(static_cast<char>(strtoul(f[3].substr(i, 2).c_str(), nullptr, 16)));
        SDK::UObject* pc = SbPrefabOf(it->second);
        SDK::UObject* lgm = pc ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440) : nullptr;
        if (SeLuauPutSource(lgm, f[2], src))
        {
            const std::string idx = SeLuauAttach(it->second, f[2], src);
            if (!idx.empty()) it->second = SbActorForIdx(idx);
        }
    }
    for (const auto& f : lines)
    {
        if (f[0] != "Q" || f.size() < 10) continue;
        std::string steps;
        for (const auto& s : SeSplit(f[8], ';', 64))
        {
            auto it = objs.find(atoi(s.c_str()));
            if (!s.empty() && it != objs.end() && it->second) steps += (steps.empty() ? "" : ";") + SeLvIdent(it->second) + ":0";
        }
        SeQuest(f[1], f[2], f[3], atoi(f[4].c_str()), steps, 0, 0.0f, f[5], atof(f[6].c_str()), atoi(f[7].c_str()), f[9]);
        ++nq;
    }
    for (const auto& f : lines)
    {
        if (f[0] != "R" || f.size() < 7) continue;
        SeCoinRun("new", f[4], f[2], f[1], f[6], f[5], f[3]);
        ++nr;
    }
    SeModesLoadFixup(name, objIdx);                        // roles / controller -> the new object ids
    g_lvLoading.clear();
    g_lvLoaded.push_back(name);
    g_lvStatusDirty = true;
    HxLog("[HalcyonA2][LEVELS] loaded '%s': %d object(s), %d data edit(s), %d quest(s), %d coin run(s)\n", name.c_str(), no, nd, nq, nr);
}

// Remove everything a level put on this server.
static void SeLvUnload(const std::string& name)
{
    int n = 0;
    for (size_t i = 0; i < g_sbOwned.size();)
    {
        SbOwned& o = g_sbOwned[i];
        if (o.level != name) { ++i; continue; }
        // By actor when there is one; otherwise -- or if that fails (a scripted object that was just rebuilt) --
        // by removing its node directly, so nothing a level placed is ever left behind.
        SDK::AActor* a = SbActorForIdx(o.idx);
        if (!(a && SeSandboxDelete(a))) SbOwnedApply(o, true);
        g_sbOwned.erase(g_sbOwned.begin() + i);
        ++n;
    }
    for (auto& p : g_lvPlain)
        if (p.level == name && SeLvPlainAlive(p)) { SeDropProxies(p.actor); p.actor->K2_DestroyActor(); p.actor = nullptr; ++n; }
    for (auto it = g_lvRuns.begin(); it != g_lvRuns.end();)
    {
        if (it->level != name) { ++it; continue; }
        if (auto old = g_seCoinRuns.find(it->questRef); old != g_seCoinRuns.end())
        {
            for (const std::string& idx : { old->second.first, old->second.second })
                if (SDK::AActor* prev = idx.empty() ? nullptr : SbActorForIdx(idx)) SeSandboxDelete(prev);
            g_seCoinRuns.erase(old);
        }
        it = g_lvRuns.erase(it);
        ++n;
    }
    // Its quests leave every player's list (the authored bundle is re-sent without them).
    std::vector<const SeAuthoredQuest*> gone;
    for (auto& q : g_seAuthored)
        if (q.level == name && !q.deleted) { SeQuestRemove(q); gone.push_back(&q); ++n; }
    if (!gone.empty()) SeQuestResendAll(gone);
    SeModesUnload(name);
    g_lvLoaded.erase(std::remove(g_lvLoaded.begin(), g_lvLoaded.end(), name), g_lvLoaded.end());
    g_lvLocal.erase(std::remove(g_lvLocal.begin(), g_lvLocal.end(), name), g_lvLocal.end());
    g_lvStatusDirty = true;
    HxLog("[HalcyonA2][LEVELS] unloaded '%s': %d item(s) removed\n", name.c_str(), n);
}

// ---- backend I/O (worker threads; results go back to the game thread through g_lvJobs) ----
static std::wstring SeLvW(const std::string& s) { return std::wstring(s.begin(), s.end()); }
static std::string SeLvHttp(const wchar_t* method, const std::string& path, const std::string& body, DWORD* status)
{
    const std::wstring hdr = L"x-server-api-key: " + std::wstring(kServerApiKey) + L"\r\nContent-Type: text/plain\r\n";
    return HttpReq(kBackendHost, kBackendPort, method, SeLvW(path).c_str(), body, hdr, status);
}
static std::string SeLvUrlName(const std::string& n)
{
    std::string o;
    for (char c : n) { if (c == ' ') o += "%20"; else o += c; }
    return o;
}
static void SeLvQueue(SeLvJob j) { std::lock_guard<std::mutex> lk(g_lvMu); g_lvJobs.push_back(std::move(j)); }
static std::vector<std::string> SeLvLines(const std::string& body)
{
    std::vector<std::string> v;
    for (size_t b = 0; b < body.size();)
    {
        size_t e = body.find('\n', b);
        if (e == std::string::npos) e = body.size();
        std::string l = body.substr(b, e - b);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (!l.empty()) v.push_back(l);
        b = e + 1;
    }
    return v;
}

static std::vector<std::string> g_lvLoadedSnapshot;     // for the worker (copied under g_lvMu)
static std::vector<std::string> g_lvLocalSnapshot;
static void SeLvPollerBody()
{
    // Wait until the server has registered with the backend (the deployment id is known).
    for (int i = 0; i < 300 && g_deploymentId.empty() && !g_seLocalTest; ++i) Sleep(2000);
    const std::string dep = g_deploymentId.empty() ? std::string("local") : g_deploymentId;
    DWORD st = 0;
    const std::string boot = SeLvHttp(L"POST", "/v1/spec/boot?deployment=" + dep, "", &st);
    HxLog("[HalcyonA2][LEVELS] boot: HTTP %lu, %zu level(s) to autoload\n", st, st == 200 ? SeLvLines(boot).size() : 0);
    std::vector<std::string> desired = st == 200 ? SeLvLines(boot) : std::vector<std::string>();
    std::unordered_set<std::string> requested;
    for (int tick = 0;; ++tick)
    {
        if (tick > 0)
        {
            for (int w = 0; w < 30 && !g_lvPollNow.exchange(false); ++w) Sleep(500);   // 15 s, or sooner on request
            DWORD s2 = 0;
            const std::string d = SeLvHttp(L"GET", "/v1/spec/desired", "", &s2);
            if (s2 == 200) desired = SeLvLines(d);
            else if (g_seLocalTest) desired.clear();       // local tests: no backend = nothing desired (what live does to an editor's level)
            else continue;
        }
        std::vector<std::string> loaded, local;
        { std::lock_guard<std::mutex> lk(g_lvMu); loaded = g_lvLoadedSnapshot; local = g_lvLocalSnapshot; }
        for (const auto& name : desired)
        {
            if (std::find(loaded.begin(), loaded.end(), name) != loaded.end() || requested.count(name)) continue;
            DWORD s3 = 0;
            const std::string text = SeLvHttp(L"GET", "/v1/spec/levels/" + SeLvUrlName(name), "", &s3);
            if (s3 == 200) { SeLvQueue({ 1, name, text }); requested.insert(name); }
        }
        for (const auto& name : loaded)
            if (std::find(desired.begin(), desired.end(), name) == desired.end() && std::find(local.begin(), local.end(), name) == local.end())
            { SeLvQueue({ 2, name, "" }); requested.erase(name); }
        for (auto it = requested.begin(); it != requested.end();)   // loaded ones no longer need the guard
            if (std::find(loaded.begin(), loaded.end(), *it) != loaded.end()) it = requested.erase(it); else ++it;
        if (g_lvStatusDirty.exchange(false))
        {
            std::string body;
            for (const auto& l : loaded) body += l + "\n";
            DWORD s4 = 0;
            SeLvHttp(L"POST", "/v1/spec/status?deployment=" + dep, body, &s4);
        }
    }
}

// The poller thread must never take the server down: anything unexpected (a malformed backend reply,
// an allocation failure) is logged and the poller starts over a minute later.
static void SeLvPollerGuarded()
{
    try { SeLvPollerBody(); }
    catch (const std::exception& e) { HxLog("[HalcyonA2][LEVELS] poller error: %s -- restarting in 60s\n", e.what()); }
    catch (...) { HxLog("[HalcyonA2][LEVELS] poller error -- restarting in 60s\n"); }
}
static void SeLvPollerSeh()
{
    __try { SeLvPollerGuarded(); }
    __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][LEVELS] poller fault -- restarting in 60s\n"); }
}
static void SeLvPoller()
{
    for (;;) { SeLvPollerSeh(); Sleep(60000); }
}

// Game thread: start the poller once, and apply what it fetched.
// ---- text from scripts -----------------------------------------------------------------------------------
// The game never hands a TextComponent to a Luau script (a script slot of that type is always nil), so a
// script can't set text directly. Instead: an editor types a template into a Text object's Text in Game data,
// e.g. "Score: {score}", and a script calls  Gamemode:broadcastEventString("score", "3")  -- the server sees
// that call (the server runs every script too), fills {score} into every Text object that uses it, and writes
// the result through the same synced path the Game data panel uses, so every player sees it (Quest too).
// The saved value stays the template; unknown {names} are shown as written until something sets them.
static std::unordered_map<std::string, std::string> g_seTextVars;
static std::vector<std::pair<std::string, std::string>> g_seTextQueue;   // (name, value) from scripts
static std::vector<std::pair<void*, std::pair<std::string, std::string>>> g_seModeEvents;   // (Gamemode API, (name, value))
static std::vector<std::string> g_seTextObjQueue;                        // object ids to (re)render
static SRWLOCK g_seTextLock = SRWLOCK_INIT;
static std::string SeTextRender(const std::string& t)
{
    std::string out;
    for (size_t i = 0; i < t.size();)
    {
        if (t[i] == '{')
        {
            const size_t e = t.find('}', i + 1);
            if (e != std::string::npos && e - i - 1 <= 64)
            {
                auto it = g_seTextVars.find(t.substr(i + 1, e - i - 1));
                if (it != g_seTextVars.end()) { out += it->second; i = e + 1; continue; }
            }
        }
        out += t[i++];
    }
    return out;
}
using SeBesFn = __int64(__fastcall*)(void*, void*, void*);
static SeBesFn g_seBesOrig = nullptr;
struct SeFStr { wchar_t* d; int32_t n; int32_t m; };
// POD: copy an FString's text into buf (ASCII, '?' for the rest). 0 on a bad pointer.
static int SeFStrCopy(const void* f, char* buf, int cap)
{
    int k = 0;
    __try
    {
        const SeFStr* fs = static_cast<const SeFStr*>(f);
        if (fs && fs->d) for (int32_t i = 0; i < fs->n && k < cap - 1 && fs->d[i]; ++i) buf[k++] = static_cast<char>(fs->d[i] < 128 ? fs->d[i] : '?');
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { k = 0; }
    buf[k] = 0;
    return k;
}
// UModuleStateLuaAPI::BroadcastEventString (Luau Gamemode:broadcastEventString). The strings are copied before
// the original runs (it takes ownership of them).
static void SeBesQueue(void* api, void* name, void* value)
{
    static char nb[80], vb[1100];
    if (SeFStrCopy(name, nb, sizeof(nb)) <= 0 || strlen(nb) > 64) return;
    SeFStrCopy(value, vb, sizeof(vb));
    AcquireSRWLockExclusive(&g_seTextLock);
    if (strncmp(nb, "rgm:", 4) == 0) { if (g_seModeEvents.size() < 4096) g_seModeEvents.push_back({ api, { nb, vb } }); }
    else if (g_seTextQueue.size() < 4096) g_seTextQueue.push_back({ nb, vb });
    ReleaseSRWLockExclusive(&g_seTextLock);
}
static __int64 __fastcall SeBes_Hook(void* api, void* name, void* value)
{
    SeBesQueue(api, name, value);
    return g_seBesOrig(api, name, value);
}
static void SbDeferredSetsTick()
{
    if (g_sbDeferredSets.empty()) return;
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_sbDeferredSets.size();)
    {
        const SbDeferredSet d = g_sbDeferredSets[i];
        if (now < d.due) { ++i; continue; }
        g_sbDeferredSets.erase(g_sbDeferredSets.begin() + i);
        SDK::AActor* a = SbActorForIdx(d.idx);
        if (!a) continue;                                 // gone meanwhile
        const bool prevNoRec = g_seSbNoRecord;
        g_seSbNoRecord = d.noRecord;
        g_sbApplyingDeferred = true;
        SeSbSet(nullptr, SeLvIdent(a), d.path, d.kind, d.value);
        g_sbApplyingDeferred = false;
        g_seSbNoRecord = prevNoRec;
        return;                                           // one write per tick
    }
}
static void SeTextQueueObject(SDK::UObject* prefab)
{
    if (!prefab) return;
    const std::string idx = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(prefab) + 0x248));
    AcquireSRWLockExclusive(&g_seTextLock);
    g_seTextObjQueue.push_back(idx);
    ReleaseSRWLockExclusive(&g_seTextLock);
}
static std::string SeObjIdx(SDK::AActor* a)
{
    SDK::UObject* pc = a ? SbPrefabOf(a) : nullptr;
    return pc ? FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248)) : std::string();
}
// UPhysicalComponent's collision-profile setter (0x53BE1F0: component, FName* profile, flag). Only calls made by
// the three Luau functions count -- identified by the return address (their bodies are 46 bytes each) -- so the
// component's own setup calls never switch a stand-in off.
static constexpr uintptr_t kPhysSetProfile = 0x53BE1F0, kLuaTrigger = 0x53C0C80, kLuaNoColl = 0x539AF50, kLuaDefault = 0x53BE880;
using PhysSetProfile_t = __int64(__fastcall*)(void*, void*, uint64_t);
static PhysSetProfile_t g_physSetProfileOrig = nullptr;
static void SeOnScriptCollision(void* comp, bool on)
{
    auto* c = static_cast<SDK::UObject*>(comp);
    auto* owner = c && c->Outer ? static_cast<SDK::AActor*>(c->Outer) : nullptr;
    if (!owner || !SeAlive(owner) || !SeNeedsProxy(owner)) return;
    g_seScriptCollision[owner] = { SeObjIdx(owner), !on };
    SeApplyScriptCollision(owner);
}
static void SeOnScriptCollisionSafe(void* comp, bool on) { __try { SeOnScriptCollision(comp, on); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static __int64 __fastcall SePhysSetProfile_Hook(void* comp, void* profile, uint64_t flag)
{
    const __int64 r = g_physSetProfileOrig(comp, profile, flag);
    const uintptr_t ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - GetBase();
    auto from = [ret](uintptr_t f) { return ret > f && ret < f + 64; };
    const bool trig = from(kLuaTrigger), none = from(kLuaNoColl), deflt = from(kLuaDefault);
    if (trig || none || deflt) SeOnScriptCollisionSafe(comp, deflt);
    return r;
}
static void SeTextTick()
{
    static bool s_hooked = false;
    if (!s_hooked && g_seSandbox)
    {
        s_hooked = true;
        {
            void* at = reinterpret_cast<void*>(GetBase() + kPhysSetProfile);
            const int c = MH_CreateHook(at, &SePhysSetProfile_Hook, reinterpret_cast<void**>(&g_physSetProfileOrig));
            const int e = c == MH_OK ? MH_EnableHook(at) : -1;
            HxLog("[HalcyonA2][SPECEDIT] script collision -> stand-ins: hook create=%d enable=%d\n", c, e);
        }
        void* at = reinterpret_cast<void*>(GetBase() + 0x46CC430);   // BroadcastEventString (luau binding 0x46A5B00 -> here)
        const int c = MH_CreateHook(at, &SeBes_Hook, reinterpret_cast<void**>(&g_seBesOrig));
        const int e = c == MH_OK ? MH_EnableHook(at) : -1;
        HxLog("[HalcyonA2][SPECEDIT] text bridge: broadcastEventString hook create=%d enable=%d\n", c, e);
    }
    std::vector<std::pair<std::string, std::string>> q;
    std::vector<std::string> objs;
    AcquireSRWLockExclusive(&g_seTextLock);
    q.swap(g_seTextQueue);
    objs.swap(g_seTextObjQueue);
    ReleaseSRWLockExclusive(&g_seTextLock);
    if (q.empty() && objs.empty()) return;
    std::unordered_set<std::string> changed;
    for (auto& kv : q)
    {
        auto it = g_seTextVars.find(kv.first);
        if (it != g_seTextVars.end() && it->second == kv.second) continue;
        g_seTextVars[kv.first] = kv.second;
        changed.insert(kv.first);
    }
    const std::unordered_set<std::string> only(objs.begin(), objs.end());
    int n = 0;
    const ULONGLONG nowT = GetTickCount64();
    std::vector<std::string> later;
    for (size_t i = 0; i < g_sbOwned.size() && n < 200; ++i)
    {
        const SbOwned& o = g_sbOwned[i];
        const std::array<std::string, 3>* tpl = nullptr;
        for (const auto& d : o.data) if (d[0] == "props/Text" && d[2].find('{') != std::string::npos) tpl = &d;
        if (!tpl) continue;
        bool want = only.count(o.idx) > 0;
        for (const auto& c : changed) if (!want && (*tpl)[2].find("{" + c + "}") != std::string::npos) want = true;
        if (!want) continue;
        auto lw = g_sbLeafWriteAt.find(o.idx);
        if (lw != g_sbLeafWriteAt.end() && nowT - lw->second < 300) { later.push_back(o.idx); continue; }   // written just now: next tick
        SDK::AActor* a = SbActorForIdx(o.idx);
        if (!a) continue;
        const std::string idx = o.idx, kind = (*tpl)[1], text = SeTextRender((*tpl)[2]);
        g_seSbNoRecord = true;
        SeSbSet(nullptr, SeLvIdent(a), "props/Text", kind, text);   // (g_sbOwned may not move: no spawn happens here)
        g_seSbNoRecord = false;
        ++n;
    }
    if (!later.empty())
    {
        AcquireSRWLockExclusive(&g_seTextLock);
        for (auto& l : later) g_seTextObjQueue.push_back(l);
        ReleaseSRWLockExclusive(&g_seTextLock);
    }
    if (n || !changed.empty())
        HxLog("[HalcyonA2][SPECEDIT] text bridge: %zu value(s) from scripts, %d text object(s) updated\n", changed.size(), n);
}

// ==== GAME MODE FABRICATOR ================================================================================
// A game mode made in the editor is a module slot of its own (SbCreateSlotWithId): a unique SlotID set before
// the spawn finishes -- clients get it, verified -- loading tag/teamchanger (two team changers, no scripts) so
// the slot gets its TicketManager, with MaximumTeamSizes = the mode's teams. Everything placed inside its box
// is hosted by it (SbHostGamemode checks g_seFabSlots first), so team changers placed there join ITS teams.
// The server runs the mode (idle -> countdown -> running -> ended), reads team sizes from the team changers,
// and publishes everything as the mode's config variables ("rgm.*", Gamemode:getStringConfigVariable on every
// machine) plus text-template values; scripts drive it back with Gamemode:broadcastEventString("rgm:...").
struct SeGameMode
{
    std::string id, name, level;
    double at[3]{};
    int nTeams = 2;
    int teamMax[8]{};
    std::string teamName[8];
    std::map<std::string, std::string> settings;
    SDK::AActor* slot = nullptr;
    std::string state = "idle";
    ULONGLONG stateAt = 0;
    int score[8]{};
    int wins[8]{};                                       // rounds won (reset by "reset")
    int teamSize[8]{};
    int round = 0, winner = 0, timeLeft = 0;
    ULONGLONG starterClearAt = 0;
    bool deleted = false;
    std::unordered_map<std::string, std::string> pushed;   // config values already pushed
    std::string statSent;                                   // last status line sent to editors
    std::map<std::string, ULONGLONG> goalAt;                // Rigel.goal key -> when it last counted (de-dupe)
    bool hadPlayers = false;                                // this round has had someone on a team (stop_when_empty)
};
static std::vector<SeGameMode> g_seModes;
static int g_seModeCounter = 0;
static const char* kSeModeDefaults[][2] = {
    { "start_mode", "manual" },   // manual | button | auto
    { "min_players", "1" },       // per team, for auto start
    { "countdown", "5" },         // seconds before a round
    { "round_time", "180" },      // seconds; 0 = no limit
    { "score_to_win", "0" },      // 0 = none
    { "end_delay", "10" },        // seconds the result shows before idle
    { "auto_restart", "0" },      // 1 = auto mode starts again after a round
    { "stop_when_empty", "1" },   // 1 = a running round ends when every team is empty
    { "reset_after_goal", "1" },  // 1 = a goal / score box point sends the ball back to its spawner
};
static const double kSeModeHalf[3] = { 3881.0, 4733.0, 950.0 };   // the slot class's box (half-size, cm)

static SeGameMode* SeModeById(const std::string& id)
{
    for (auto& m : g_seModes) if (!m.deleted && m.id == id) return &m;
    return nullptr;
}
static SeGameMode* SeModeByApi(void* api)
{
    for (auto& m : g_seModes)
    {
        if (m.deleted || !m.slot || !SeAlive(m.slot)) continue;
        SDK::UObject* ms = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(m.slot) + 0x300);
        if (ms && *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ms) + 0x90) == api) return &m;
    }
    return nullptr;
}
static SeGameMode* SeModeAt(const double* loc)
{
    for (auto& m : g_seModes)
    {
        if (m.deleted || !m.slot || !SeAlive(m.slot)) continue;
        if (SDK::UBoxComponent* box = SbImportanceBox(m.slot)) if (SbOutsideBox(box, loc) <= 1.0) return &m;
    }
    return nullptr;
}
static bool SeTeamChangerAllowedAt(const double* loc)
{
    if (g_seAllowTeamChangers) return true;
    SeGameMode* m = SeModeAt(loc);
    return m && *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(m->slot) + 0x438) != nullptr;
}
static std::string SeModeSetting(const SeGameMode& m, const std::string& k)
{
    auto it = m.settings.find(k);
    if (it != m.settings.end()) return it->second;
    for (auto& d : kSeModeDefaults) if (k == d[0]) return d[1];
    return std::string();
}
static int SeModeSettingInt(const SeGameMode& m, const std::string& k) { return atoi(SeModeSetting(m, k).c_str()); }
static std::string SeModeClean(const std::string& raw, size_t maxLen)
{
    std::string o;
    for (char c : raw) if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') o += c;
    return o.size() > maxLen ? o.substr(0, maxLen) : o;
}
// Push one config value to the mode (only when it changed): scripts on every machine read it.
static void SeModePush(SeGameMode& m, const std::string& k, const std::string& v)
{
    auto it = m.pushed.find(k);
    if (it != m.pushed.end() && it->second == v) return;
    if (SeModeConfigSet(m.slot, k, v) == 1) m.pushed[k] = v;
    // and as a text template value: {<mode name>.<key without rgm.>}
    const std::string tk = m.name + "." + (k.rfind("rgm.", 0) == 0 ? k.substr(4) : k);
    AcquireSRWLockExclusive(&g_seTextLock);
    if (g_seTextQueue.size() < 4096) g_seTextQueue.push_back({ tk, v });
    ReleaseSRWLockExclusive(&g_seTextLock);
}
static void SeModePushAll(SeGameMode& m)
{
    SeModePush(m, "rgm.id", m.id);
    SeModePush(m, "rgm.name", m.name);
    SeModePush(m, "rgm.state", m.state);
    SeModePush(m, "rgm.round", std::to_string(m.round));
    SeModePush(m, "rgm.time", std::to_string(m.timeLeft));
    SeModePush(m, "rgm.winner", std::to_string(m.winner));
    SeModePush(m, "rgm.teams", std::to_string(m.nTeams));
    int players = 0;
    for (int t = 0; t < m.nTeams; ++t)
    {
        const std::string p = "rgm.team" + std::to_string(t + 1) + ".";
        SeModePush(m, p + "name", m.teamName[t]);
        SeModePush(m, p + "size", std::to_string(m.teamSize[t]));
        SeModePush(m, p + "max", std::to_string(m.teamMax[t]));
        SeModePush(m, p + "score", std::to_string(m.score[t]));
        SeModePush(m, p + "wins", std::to_string(m.wins[t]));
        players += m.teamSize[t];
    }
    SeModePush(m, "rgm.players", std::to_string(players));
    for (auto& d : kSeModeDefaults) SeModePush(m, std::string("rgm.set.") + d[0], SeModeSetting(m, d[0]));
    for (auto& kv : m.settings) if (kv.first.rfind("custom.", 0) == 0) SeModePush(m, "rgm.set." + kv.first, kv.second);
}
static std::string SeModeRecord(const SeGameMode& m)
{
    // id~name~x,y,z~teams~names~maxes~k=v;k=v~state~level~round~time~sizes~scores~winner
    std::string names, maxes, sets, sizes, scores;
    for (int t = 0; t < m.nTeams; ++t)
    {
        names += (t ? "," : "") + m.teamName[t];
        maxes += (t ? "," : "") + std::to_string(m.teamMax[t]);
        sizes += (t ? "," : "") + std::to_string(m.teamSize[t]);
        scores += (t ? "," : "") + std::to_string(m.score[t]);
    }
    for (auto& kv : m.settings) sets += (sets.empty() ? "" : ";") + kv.first + "=" + kv.second;
    char at[96];
    snprintf(at, sizeof(at), "%.0f,%.0f,%.0f", m.at[0], m.at[1], m.at[2]);
    return m.id + "~" + m.name + "~" + at + "~" + std::to_string(m.nTeams) + "~" + names + "~" + maxes + "~" + sets + "~" + m.state + "~" +
           m.level + "~" + std::to_string(m.round) + "~" + std::to_string(m.timeLeft) + "~" + sizes + "~" + scores + "~" + std::to_string(m.winner);
}
static void SeModeListTo(SDK::UObject* pc)
{
    std::string out;
    for (auto& m : g_seModes) if (!m.deleted) out += (out.empty() ? "" : "\x1E") + SeModeRecord(m);
    SeBroadcast("SE|GMLIST|" + out, pc);
}
// Balls for objects with the "ball" role: spawned (or reset) by the SERVER only. A Luau spawnBall runs on every
// machine and each client then makes its own client-only ball the server never heard of (Iris "failed to
// resolve clientassigned ref", seen 2026-09-24 on an unmodded player) -- so the mode does it natively here.
static int SeModeBalls(SeGameMode& m)
{
    static SDK::UClass* bsc = nullptr;
    if (!bsc) bsc = SDK::UObject::FindClassFast("BallSpawnerComponent");
    if (!bsc) return 0;
    int n = 0;
    for (const auto& kv : m.settings)
    {
        if (kv.first.rfind("role.", 0) != 0 || kv.second.rfind("ball", 0) != 0) continue;
        SDK::AActor* a = SbActorForIdx(kv.first.substr(5));
        if (!a || !SeAlive(a)) continue;
        for (SDK::UObject* c : ClassObjects(bsc))
        {
            if (!c || c->Outer != a) continue;
            struct { SDK::UObject* ReturnValue; } gb{};
            auto* fGet = c->Class->GetFunction("BallSpawnerComponent", "GetSpawnedBall");
            if (fGet) SafeProcessEvent(c, fGet, &gb);
            if (gb.ReturnValue && SeAlive(gb.ReturnValue))
            {
                if (auto* f = c->Class->GetFunction("BallSpawnerComponent", "ResetBall")) SafeProcessEvent(c, f, nullptr);
                HxLog("[HalcyonA2][GAMEMODE] %s: ball of %s reset\n", m.name.c_str(), a->GetName().c_str());
            }
            else if (auto* f = c->Class->GetFunction("BallSpawnerComponent", "SpawnBall"))
            {
                struct { bool CanSpawnMany; uint8_t pad[7]; SDK::FVector OverrideSpawnLocation; } sp{};
                sp.CanSpawnMany = false;
                sp.OverrideSpawnLocation = a->K2_GetActorLocation();
                sp.OverrideSpawnLocation.Z += 100.0;
                SafeProcessEvent(c, f, &sp);
                struct { SDK::UObject* ReturnValue; } after{};
                if (fGet) SafeProcessEvent(c, fGet, &after);
                HxLog("[HalcyonA2][GAMEMODE] %s: ball spawned by %s -> %s\n", m.name.c_str(), a->GetName().c_str(),
                      after.ReturnValue ? after.ReturnValue->GetName().c_str() : "(none)");
            }
            // A spawned / reset ball is FROZEN (it hangs at the spawner) until something unfreezes it -- the stock arena
            // script does primaryBall:unFreeze(true) at kick-off and after every point. Without it the ball never
            // plays ("it's like the round never starts"). The ball is the server's, so unfreeze it here.
            {
                struct { SDK::UObject* ReturnValue; } now{};
                if (fGet) SafeProcessEvent(c, fGet, &now);
                if (now.ReturnValue && SeAlive(now.ReturnValue))
                    if (auto* fu = now.ReturnValue->Class->GetFunction("DiscEntity", "UnFreeze"))
                    {
                        struct { bool reenableSimulation; } up{ true };
                        const bool ok = SafeProcessEvent(now.ReturnValue, fu, &up);
                        HxLog("[HalcyonA2][GAMEMODE] %s: ball %s unfrozen (%s)\n", m.name.c_str(), now.ReturnValue->GetName().c_str(), ok ? "ok" : "FAULT");
                    }
                if (now.ReturnValue && SeAlive(now.ReturnValue))   // the server's copy the size every player's is
                    SeBallMatchClients(static_cast<SDK::AActor*>(now.ReturnValue), a);
            }
            ++n;
            break;
        }
    }
    return n;
}
// The ball an editor means by "<ball class>@x,y,z" (the editor lists balls so they can be picked): the nearest
// DiscEntity of that class within 3 m (a ball keeps moving while it is being dragged). Null for anything else.
static SDK::AActor* SeBallAt(const std::string& ident)
{
    const size_t at = ident.find('@');
    if (at == std::string::npos) return nullptr;
    static SDK::UClass* disc = nullptr;
    if (!disc) disc = SDK::UObject::FindClassFast("DiscEntity");
    SDK::UClass* cls = SDK::UObject::FindClassFast(ident.substr(0, at));
    double want[3];
    if (!disc || !cls || !SeVec(ident.substr(at + 1), want)) return nullptr;
    SDK::AActor* best = nullptr;
    double bestD2 = 300.0 * 300.0;
    for (SDK::UObject* o : ClassObjects(disc))
    {
        if (!o || o->Class != cls || !SeAlive(o) || (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01)) continue;
        const SDK::FVector p = static_cast<SDK::AActor*>(o)->K2_GetActorLocation();
        const double d2 = (p.X - want[0]) * (p.X - want[0]) + (p.Y - want[1]) * (p.Y - want[1]) + (p.Z - want[2]) * (p.Z - want[2]);
        if (d2 < bestD2) { bestD2 = d2; best = static_cast<SDK::AActor*>(o); }
    }
    return best;
}
// A ball's size can NOT be set: every machine sizes its own copy of the ball (1 / spawner scale, measured 2026-09-24),
// the actor scale never replicates, and a server-side scale then makes the server's ball simulate at a different size
// than every player's -- measured: the client's ball fell 290 m through the floor. So this is off (kept for a test).
static bool g_seBallScaleOn = false;
static void SeBallScale(SDK::AActor* ball, const double* scl)
{
    if (!g_seBallScaleOn || !ball || !scl) return;
    const double s = (std::max)(0.05, (scl[0] + scl[1] + scl[2]) / 3.0);
    const SDK::FVector cur = ball->GetActorScale3D();
    if (fabs(cur.X - s) < 1e-3 && fabs(cur.Y - s) < 1e-3 && fabs(cur.Z - s) < 1e-3) return;
    ball->SetActorScale3D(SDK::FVector{ s, s, s });
    ball->ForceNetUpdate();
    HxLog("[HalcyonA2][SPECEDIT] ball %s scale -> %.2f\n", ball->GetName().c_str(), s);
}
// After a ball spawner moves or is scaled, its ball goes with it: back to 1 m above the spawner (the game's own
// ResetBallTo -- a real physics reset every machine sees), at the spawner's scale, and playing again (a reset ball
// is frozen). `at`/`scl` are the spawner's NEW transform (a sandbox move lands on the server a moment later).
// A sandbox move REBUILDS the spawner: the old one (and its ball) goes, the new one spawns a fresh ball a moment later
// -- sized 1/spawner-scale by the game. So the follow is also queued by the spawner's idx and applied again to the
// rebuilt spawner's new ball when it turns up.
struct SeBallPending { std::string idx; double at[3], scl[3]; SDK::UObject* oldBall; ULONGLONG until; };
static std::vector<SeBallPending> g_seBallPending;
static SDK::AActor* SeSpawnerBall(SDK::AActor* spawner, SDK::UObject** compOut)
{
    static SDK::UClass* bsc = nullptr;
    if (!bsc) bsc = SDK::UObject::FindClassFast("BallSpawnerComponent");
    if (!spawner || !bsc) return nullptr;
    for (SDK::UObject* c : ClassObjects(bsc))
    {
        if (!c || c->Outer != spawner) continue;
        if (compOut) *compOut = c;
        struct { SDK::UObject* ReturnValue; } gb{};
        if (auto* fGet = c->Class->GetFunction("BallSpawnerComponent", "GetSpawnedBall")) SafeProcessEvent(c, fGet, &gb);
        return gb.ReturnValue && SeAlive(gb.ReturnValue) ? static_cast<SDK::AActor*>(gb.ReturnValue) : nullptr;
    }
    return nullptr;
}
static void SeBallFollowNow(SDK::UObject* c, SDK::AActor* ball, const double* at, const double* scl);
// Every player's game sizes a spawner's ball at 1 / the spawner's scale; the server's copy stays at 1 (measured
// 2026-09-25: client 2.00, server 1.00 for a spawner at 0.5). So the server simulated a normal ball while players
// saw a big one half inside the floor. Give the server's copy the same size -- then every copy agrees.
static void SeBallMatchClients(SDK::AActor* ball, SDK::AActor* spawner)
{
    if (!ball || !spawner || !SeAlive(ball)) return;
    const SDK::FVector ss = spawner->GetActorScale3D();
    const double avg = (ss.X + ss.Y + ss.Z) / 3.0;
    if (avg <= 0.01) return;
    const double want = 1.0 / avg;
    const SDK::FVector cur = ball->GetActorScale3D();
    if (fabs(cur.X - want) < 0.01 && fabs(cur.Y - want) < 0.01 && fabs(cur.Z - want) < 0.01) return;
    ball->SetActorScale3D(SDK::FVector{ want, want, want });
    ball->ForceNetUpdate();
    HxLog("[HalcyonA2][SPECEDIT] ball %s sized %.2f like every player's copy (spawner scale %.2f)\n", ball->GetName().c_str(), want, avg);
}
static void SeBallFollow(SDK::AActor* spawner, const double* at, const double* scl)
{
    SDK::UObject* c = nullptr;
    SDK::AActor* ball = SeSpawnerBall(spawner, &c);
    if (!c) return;                                        // not a ball spawner
    if (SDK::UObject* pc = SbPrefabOf(spawner))
    {
        SeBallPending p{ FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248)), {}, {}, ball, GetTickCount64() + 15000 };
        for (int k = 0; k < 3; ++k) { p.at[k] = at ? at[k] : 0.0; p.scl[k] = scl ? scl[k] : 1.0; }
        if (!p.idx.empty())
        {
            g_seBallPending.erase(std::remove_if(g_seBallPending.begin(), g_seBallPending.end(),
                                                 [&](const SeBallPending& q) { return q.idx == p.idx; }), g_seBallPending.end());
            g_seBallPending.push_back(p);
        }
        return;   // a placed spawner is rebuilt by the move and makes its own ball there: nothing to push around
    }
    if (ball) SeBallFollowNow(c, ball, at, scl);   // a station spawner (not rebuilt): reset its ball to the new spot
}
static void SeBallPendingTick()
{
    const ULONGLONG now = GetTickCount64();
    for (size_t i = 0; i < g_seBallPending.size(); )
    {
        SeBallPending& p = g_seBallPending[i];
        SDK::AActor* sp = SbActorForIdx(p.idx, true);
        SDK::UObject* c = nullptr;
        SDK::AActor* ball = sp && SeAlive(sp) ? SeSpawnerBall(sp, &c) : nullptr;
        // The rebuilt spawner's new ball: it was made at the new spot -- leave it be. Resetting / unfreezing a ball
        // this young (measured 2026-09-24) sent every player's copy 290 m under the floor while the server's sat on it.
        if (ball && ball != p.oldBall)
        {
            SeBallMatchClients(ball, sp);
            HxLog("[HalcyonA2][SPECEDIT] spawner %s rebuilt with its ball %s (server scale %.2f)\n", p.idx.c_str(),
                  ball->GetName().c_str(), ball->GetActorScale3D().X);
            g_seBallPending.erase(g_seBallPending.begin() + i);
            continue;
        }
        // The rebuilt spawner made no ball of its own (the old one went with the old spawner): make it one, so an
        // edit never loses the ball. Given the rebuild ~2.5 s to settle first.
        if (p.oldBall && c && sp != nullptr && (!ball || !SeAlive(ball)) && now + 15000 - p.until > 2500)
            if (auto* f = c->Class->GetFunction("BallSpawnerComponent", "SpawnBall"))
            {
                struct { bool CanSpawnMany; uint8_t pad[7]; SDK::FVector OverrideSpawnLocation; } spn{};
                spn.OverrideSpawnLocation = SDK::FVector{ p.at[0], p.at[1], p.at[2] + 100.0 };
                SafeProcessEvent(c, f, &spn);
                SDK::AActor* nb = SeSpawnerBall(sp, nullptr);
                HxLog("[HalcyonA2][SPECEDIT] spawner %s had no ball after its rebuild -> spawned %s\n", p.idx.c_str(),
                      nb ? nb->GetName().c_str() : "(none)");
                if (nb) { g_seBallPending.erase(g_seBallPending.begin() + i); continue; }
            }
        if (now > p.until) { g_seBallPending.erase(g_seBallPending.begin() + i); continue; }
        ++i;
    }
}
static void SeBallFollowNow(SDK::UObject* c, SDK::AActor* ball, const double* at, const double* scl)
{
    {
        SDK::AActor* spawner = static_cast<SDK::AActor*>(c->Outer);
        if (at)
            if (auto* f = c->Class->GetFunction("BallSpawnerComponent", "ResetBallTo"))
            {
                struct { SDK::FVector position; } rp{ SDK::FVector{ at[0], at[1], at[2] + 100.0 } };
                SafeProcessEvent(c, f, &rp);
                if (auto* fu = ball->Class->GetFunction("DiscEntity", "UnFreeze"))
                {
                    struct { bool reenableSimulation; } up{ true };
                    SafeProcessEvent(ball, fu, &up);
                }
            }
        SeBallScale(ball, scl);
        HxLog("[HalcyonA2][SPECEDIT] ball %s follows its spawner %s to (%.0f,%.0f,%.0f)\n", ball->GetName().c_str(),
              spawner->GetName().c_str(), at ? at[0] : 0.0, at ? at[1] : 0.0, at ? at[2] + 100.0 : 0.0);
        return;
    }
}
// A "Ball start ring" mode waits for its ball to be carried into the ring, so it needs the ball between rounds.
static bool SeModeHasStartRing(const SeGameMode& m)
{
    for (const auto& kv : m.settings)
        if (kv.first.rfind("role.", 0) == 0 && kv.second.rfind("start_ring", 0) == 0) return true;
    return false;
}
static void SeModeLobbyBall(SeGameMode& m)
{
    if ((m.state == "idle" || m.state == "ended") && SeModeHasStartRing(m)) SeModeBalls(m);
}
static void SeModeSetState(SeGameMode& m, const std::string& st)
{
    if (m.state == st) return;
    HxLog("[HalcyonA2][GAMEMODE] %s: %s -> %s\n", m.name.c_str(), m.state.c_str(), st.c_str());
    m.state = st;
    m.stateAt = GetTickCount64();
    if (st == "countdown" || (st == "running" && m.round == 0)) { for (int t = 0; t < 8; ++t) m.score[t] = 0; m.winner = 0; }
    if (st == "running") ++m.round;
    if (st == "countdown" || st == "running") m.hadPlayers = false;   // re-learned from the next tick's team sizes
    if (st == "idle") { m.winner = 0; }
    SeModePushAll(m);
    if (st == "running") SeModeBalls(m);
    else SeModeLobbyBall(m);                               // a "Ball start ring" needs a ball in the lobby to carry in
}
static void SeModeEnd(SeGameMode& m, int winner)   // winner: 1-based team, 0 = work it out from the scores (tie = 0)
{
    if (m.state != "running" && m.state != "countdown") return;
    if (winner <= 0)
    {
        int best = -1, bestScore = -2147483647; bool tie = false;
        for (int t = 0; t < m.nTeams; ++t)
        {
            if (m.score[t] > bestScore) { bestScore = m.score[t]; best = t; tie = false; }
            else if (m.score[t] == bestScore) tie = true;
        }
        winner = (best >= 0 && !tie) ? best + 1 : 0;
    }
    m.winner = winner;
    if (winner >= 1 && winner <= m.nTeams) ++m.wins[winner - 1];
    SeModeSetState(m, "ended");
}
// The mode's SCRIPT HOST: an invisible object (a Timer prefab: no mesh) that carries the generated controller script.
// The script references the Game State Manager (the "controller") -- its clock, state and score components drive the
// arena monitors and the announcer. A script must never reference the object it sits on: a self-reference makes a
// cycle in the netvar tree and every client crashed walking it (2026-09-24, both clients, +0x46645DD recursion). The
// station's own arenas do the same: gamemode.luau sits on a Cube and references the Game State Manager.
static void SeModeEnsureHost(SeGameMode& m, const std::string& level)
{
    auto it = m.settings.find("scripthost");
    if (it != m.settings.end() && !it->second.empty())
        if (SDK::AActor* a = SbActorForIdx(it->second)) if (SeAlive(a)) return;
    double at[3] = { m.at[0], m.at[1], m.at[2] + 60.0 };
    const double rot0[3] = { 0, 0, 0 }, scl1[3] = { 1, 1, 1 };
    const std::string prevLoading = g_lvLoading;
    g_lvLoading = level;
    const std::string hidx = SeSandboxSpawn("Timer", at, rot0, scl1);
    g_lvLoading = prevLoading;
    if (SeGameMode* again = SeModeById(m.id))
        if (!hidx.empty()) { again->settings["scripthost"] = hidx; SeModePushAll(*again); }
    HxLog("[HalcyonA2][GAMEMODE] %s: script host %s\n", m.name.c_str(), hidx.empty() ? "COULD NOT BE PLACED" : hidx.c_str());
}
// Create: a slot of its own, teams sized as asked. Returns the mode (nullptr + a note when it can't).
static SeGameMode* SeModeCreate(const std::string& rawName, const double* at, int nTeams, const int* maxes, const std::string* names,
                                const std::string& level, const std::string& wantId, SDK::UObject* pc, bool withController = true)
{
    const std::string name = SeModeClean(rawName, 32);
    auto note = [&](const std::string& t) { if (pc) SeBroadcast("SE|NOTE|" + t, pc); HxLog("[HalcyonA2][GAMEMODE] %s\n", t.c_str()); };
    if (name.empty()) { note("Give the game mode a name."); return nullptr; }
    for (auto& m : g_seModes)
        if (!m.deleted && _stricmp(m.name.c_str(), name.c_str()) == 0) { note("There is already a game mode called '" + name + "'."); return nullptr; }
    int live = 0;
    for (auto& m : g_seModes) if (!m.deleted) ++live;
    if (live >= 12) { note("12 game modes is the limit for one server run."); return nullptr; }
    for (auto& m : g_seModes)
        if (!m.deleted && fabs(m.at[0] - at[0]) < 2 * kSeModeHalf[0] && fabs(m.at[1] - at[1]) < 2 * kSeModeHalf[1] && fabs(m.at[2] - at[2]) < 2 * kSeModeHalf[2])
        { note("That area overlaps game mode '" + m.name + "'. Move the camera further away (each game mode is about 78 x 95 x 19 m)."); return nullptr; }
    nTeams = (std::max)(1, (std::min)(8, nTeams));
    int sizes[8];
    for (int t = 0; t < nTeams; ++t) sizes[t] = (std::max)(1, (std::min)(64, maxes ? maxes[t] : 4));
    std::string id = wantId;
    if (id.empty() || SeModeById(id)) id = "RigelMode" + std::to_string(++g_seModeCounter);
    SDK::AActor* slot = SbCreateSlotWithId(at, "tag/teamchanger", id, sizes, nTeams);
    if (!slot || !*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(slot) + 0x440))
    { note("Couldn't create the game mode area here (the game refused the slot)."); return nullptr; }
    SeGameMode m;
    m.id = id; m.name = name; m.level = level; m.slot = slot; m.nTeams = nTeams;
    for (int i = 0; i < 3; ++i) m.at[i] = at[i];
    static const char* kDefaultNames[] = { "Blue", "Red", "Green", "Yellow", "Purple", "Orange", "Pink", "White" };
    for (int t = 0; t < nTeams; ++t) { m.teamMax[t] = sizes[t]; m.teamName[t] = names && !names[t].empty() ? SeModeClean(names[t], 20) : kDefaultNames[t]; }
    m.stateAt = GetTickCount64();
    m.starterClearAt = GetTickCount64() + 6000;          // the project's own two team changers go once it settles
    g_seModes.push_back(m);
    SeGameMode& r = g_seModes.back();
    if (withController)
    {
        // The mode's controller: a Game State Manager at the centre. The Game Modes window attaches the mode's
        // generated script to it (with its slots wired to the objects that have roles).
        const double rot0[3] = { 0, 0, 0 }, scl1[3] = { 1, 1, 1 };
        const std::string prevLoading = g_lvLoading;
        g_lvLoading = level;
        const std::string cidx = SeSandboxSpawn("GameStateManager", at, rot0, scl1);
        g_lvLoading = prevLoading;
        SeGameMode* again = SeModeById(id);                   // (the spawn cannot move g_seModes, but be safe)
        if (again && !cidx.empty()) again->settings["controller"] = cidx;
        if (SeGameMode* m2 = SeModeById(id)) SeModeEnsureHost(*m2, level);
    }
    SeGameMode& rr = *SeModeById(id);
    SeModePushAll(rr);
    const bool tm = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(slot) + 0x438) != nullptr;
    HxLog("[HalcyonA2][GAMEMODE] created '%s' (%s) at (%.0f,%.0f,%.0f): %d team(s), ticket manager %s, controller %s\n", name.c_str(),
          id.c_str(), at[0], at[1], at[2], nTeams, tm ? "yes" : "NO", rr.settings.count("controller") ? rr.settings["controller"].c_str() : "none");
    return &rr;
}
// Delete: every object we placed in it goes (the slot itself stays -- it is inert once empty).
static void SeModeDelete(SeGameMode& m)
{
    SDK::UObject* lgm = m.slot && SeAlive(m.slot) ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(m.slot) + 0x440) : nullptr;
    int n = 0;
    for (size_t i = 0; lgm && i < g_sbOwned.size();)
    {
        if (g_sbOwned[i].lgm != lgm) { ++i; continue; }
        SDK::AActor* a = SbActorForIdx(g_sbOwned[i].idx);
        const std::string idx = g_sbOwned[i].idx;
        if (a && SeSandboxDelete(a)) { SbOwnedForget(idx); ++n; continue; }
        if (SbOwnedApply(g_sbOwned[i], true)) { g_sbOwned.erase(g_sbOwned.begin() + i); ++n; continue; }
        ++i;
    }
    g_seFabSlots.erase(std::remove(g_seFabSlots.begin(), g_seFabSlots.end(), m.slot), g_seFabSlots.end());
    m.deleted = true;
    HxLog("[HalcyonA2][GAMEMODE] deleted '%s': %d object(s) removed\n", m.name.c_str(), n);
}
// The objects the project loaded with (not ours): the two starter team changers. Our own are kept.
static int SeModeClearStarters(SeGameMode& m)
{
    SDK::UObject* lgm = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(m.slot) + 0x440);
    if (!lgm) return 0;
    auto* pcCls = SDK::UObject::FindClassFast("PrefabComponent");
    std::vector<SDK::AActor*> doomed;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; pcCls && i < n; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pcCls)) continue;
        if (*reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(o) + 0x440) != lgm) continue;
        const std::string pidx = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(o) + 0x248));
        if (!pidx.empty() && SbOwnedByIdx(pidx)) continue;
        if (o->Outer && SeAlive(o->Outer)) doomed.push_back(static_cast<SDK::AActor*>(o->Outer));
    }
    // The project's team changers carry no prefab link to the gamemode: find them by their ticket manager.
    void* tm = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(m.slot) + 0x438);
    if (auto* tcc = SDK::UObject::FindClassFast("TeamChangeComponent"))
        for (int32_t i = 0; tm && i < n; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (!c || c->IsDefaultObject() || !c->IsA(tcc) || *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(c) + 0x508) != tm) continue;
            SDK::AActor* owner = static_cast<SDK::AActor*>(c->Outer);
            if (!owner || !SeAlive(owner)) continue;             // (SeActorAlive only knows actors we track)
            SDK::UObject* opc = SbPrefabOf(owner);
            const std::string oidx = opc ? FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(opc) + 0x248)) : std::string();
            if (!oidx.empty() && SbOwnedByIdx(oidx)) continue;   // ours
            if (std::find(doomed.begin(), doomed.end(), owner) == doomed.end()) doomed.push_back(owner);
        }
    // PARK them, never delete. Removing a project object's node leaves every CLIENT's copy of the actor alive
    // with its node handle cleared (TeamChangeComponent+0x268 = null), and the team changer's own overlap
    // handler (0x53C48B0) dereferences that handle on the overlapping player's machine -> a real player
    // walking through the spot crashed (seen 2026-09-24 with an unmodded PlayerNovBuild client). Moving a
    // project node (a merge) is safe and every machine follows it: put them outside the station's hull
    // (radially out from the cylinder axis, world Y), where nobody can reach them.
    double rx = m.at[0], rz = m.at[2];
    const double r = sqrt(rx * rx + rz * rz);
    if (r < 1.0) { rx = 0; rz = -1; } else { rx /= r; rz /= r; }
    int cleared = 0, k = 0;
    for (SDK::AActor* a : doomed)
    {
        const double park[3] = { rx * 80000.0, m.at[1] + 600.0 * k++, rz * 80000.0 }, rot0[3] = { 0, 0, 0 }, scl1[3] = { 1, 1, 1 };
        { const SDK::FVector was = a->K2_GetActorLocation(); HxLog("[HalcyonA2][GAMEMODE] starter %s was at centre%+.0f,%+.0f,%+.0f\n", a->GetName().c_str(), was.X - m.at[0], was.Y - m.at[1], was.Z - m.at[2]); }
        if (SeSandboxXform(a, park, rot0, scl1)) ++cleared;
    }
    HxLog("[HalcyonA2][GAMEMODE] %s: parked %d of %zu starter object(s) outside the hull (lgm=%p tm=%p)\n", m.name.c_str(), cleared,
          doomed.size(), (void*)lgm, tm);
    if (auto* f = m.slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(m.slot, f, nullptr);
    return cleared;
}
// Team sizes: every team changer bound to this mode's ticket manager reports its team's size.
static void SeModeReadTeams(SeGameMode& m)
{
    void* tm = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(m.slot) + 0x438);
    int sz[8] = {};
    static SDK::UClass* tcc = nullptr;
    if (!tcc) tcc = SDK::UObject::FindClassFast("TeamChangeComponent");
    if (tm && tcc)
        for (SDK::UObject* c : ClassObjects(tcc))
        {
            if (!c || c->IsDefaultObject()) continue;
            const uintptr_t b = reinterpret_cast<uintptr_t>(c);
            if (*reinterpret_cast<void**>(b + 0x508) != tm) continue;
            const int team = *reinterpret_cast<int32_t*>(b + 0x524), size = *reinterpret_cast<int32_t*>(b + 0x4DC);
            if (team >= 0 && team < 8 && size > sz[team]) sz[team] = size;
        }
    for (int t = 0; t < 8; ++t) m.teamSize[t] = sz[t];
}
static void SeModeEvent(SeGameMode& m, const std::string& ev, const std::string& arg)
{
    const std::string e = ev.substr(4);
    HxLog("[HalcyonA2][GAMEMODE] %s: event %s(%s) received in state %s\n", m.name.c_str(), e.c_str(), arg.c_str(), m.state.c_str());
    auto teamArg = [&](int* team, int* val) {
        const size_t c = arg.find(',');
        *team = atoi(arg.c_str());
        *val = c == std::string::npos ? 1 : atoi(arg.c_str() + c + 1);
    };
    if (e == "start") { if (m.state == "idle" || m.state == "ended") SeModeSetState(m, SeModeSettingInt(m, "countdown") > 0 ? "countdown" : "running"); }
    else if (e == "ringstart")                             // the ball carried into a "Ball start ring"
    {
        // Entering the lobby puts the ball back at its spawner; if that is inside the ring, the overlap is not a
        // player carrying it in. Scripts only learn the new state by polling, so the server makes this call: the
        // first 3 s of a lobby don't count.
        if ((m.state == "idle" || m.state == "ended") && GetTickCount64() - m.stateAt >= 3000)
            SeModeSetState(m, SeModeSettingInt(m, "countdown") > 0 ? "countdown" : "running");
    }
    else if (e == "startnow") { if (m.state != "running") SeModeSetState(m, "running"); }
    else if (e == "end") SeModeEnd(m, atoi(arg.c_str()));
    else if (e == "reset") { for (int t = 0; t < 8; ++t) { m.score[t] = 0; m.wins[t] = 0; } m.winner = 0; m.round = 0; m.state = ""; SeModeSetState(m, "idle"); }
    else if (e == "score" || e == "setscore")
    {
        int team = 0, val = 0;
        teamArg(&team, &val);
        if (team < 1 || team > m.nTeams) return;
        if (e == "score" && m.state != "running") return;       // points only count while a round is on
        if (e == "score") m.score[team - 1] += val; else m.score[team - 1] = val;
        SeModePushAll(m);
        const int toWin = SeModeSettingInt(m, "score_to_win");
        if (m.state == "running" && toWin > 0 && m.score[team - 1] >= toWin) SeModeEnd(m, team);
    }
    else if (e == "goal")                                  // Rigel.goal(team, points, key): a goal / score box
    {
        // Every machine runs the controller script, so the same goal can be reported more than once: count a key
        // once per 2.5 s. Then the ball goes back to its spawner (setting reset_after_goal).
        int team = 0, val = 0;
        teamArg(&team, &val);
        const size_t c2 = arg.find(',', arg.find(',') + 1);
        const std::string key = c2 == std::string::npos ? std::string() : arg.substr(c2 + 1, 64);
        if (team < 1 || team > m.nTeams || m.state != "running") return;
        const ULONGLONG now = GetTickCount64();
        ULONGLONG& last = m.goalAt[key];
        if (last && now - last < 2500) return;
        last = now;
        m.score[team - 1] += (std::max)(1, (std::min)(100, val));
        SeModePushAll(m);
        HxLog("[HalcyonA2][GAMEMODE] %s: goal '%s' -> %s +%d (%d)\n", m.name.c_str(), key.c_str(), m.teamName[team - 1].c_str(), val, m.score[team - 1]);
        const int toWin = SeModeSettingInt(m, "score_to_win");
        if (toWin > 0 && m.score[team - 1] >= toWin) { SeModeEnd(m, team); return; }
        if (SeModeSettingInt(m, "reset_after_goal")) SeModeBalls(m);
    }
    else if (e == "balls") SeModeBalls(m);                 // Rigel.resetBalls(): spawn / reset every ball-role spawner
    else if (e == "set")                                   // "key=value": scripts can change a custom setting
    {
        const size_t q = arg.find('=');
        if (q != std::string::npos && q > 0)
        {
            std::string val;
            for (char c : arg.substr(q + 1, 200)) if (c != ';' && c != '~' && c != '=' && c != '|' && static_cast<unsigned char>(c) >= 0x20) val += c;
            const std::string key = SeModeClean(arg.substr(0, q), 32);
            if (!key.empty()) { m.settings["custom." + key] = val; SeModePushAll(m); }
        }
    }
    HxLog("[HalcyonA2][GAMEMODE] %s: script event %s(%s)\n", m.name.c_str(), e.c_str(), arg.c_str());
}
// ---- game modes in saved levels / .a2level files ----
// G <id> <name> <x,y,z> <teams> <names> <maxes> <settings hex>. Settings that name objects (role.<id>, controller)
// are written as object NUMBERS of the level (#n) -- ids change every time a level loads -- and mapped back after.
static std::string SeHexOf(const std::string& v)
{
    static const char* hx = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : v) { o += hx[c >> 4]; o += hx[c & 15]; }
    return o;
}
static std::string SeUnhex(const std::string& h)
{
    std::string o;
    for (size_t i = 0; i + 1 < h.size(); i += 2) o += static_cast<char>(strtoul(h.substr(i, 2).c_str(), nullptr, 16));
    return o;
}
static std::string SeModesForLevel(const std::string& name, bool tag, const std::unordered_map<std::string, int>& ownedIdx)
{
    std::string t;
    for (auto& m : g_seModes)
    {
        if (m.deleted) continue;
        if (tag && !m.level.empty() && m.level != name) continue;
        if (tag) m.level = name;
        std::string sets, names, maxes;
        for (auto& kv : m.settings)
        {
            std::string k = kv.first, v = kv.second;
            if (k.rfind("role.", 0) == 0)
            {
                auto it = ownedIdx.find(k.substr(5));
                if (it == ownedIdx.end()) continue;                 // an object that isn't part of the level
                k = "role.#" + std::to_string(it->second);
            }
            if (k == "controller" || k == "scripthost") { auto it = ownedIdx.find(v); if (it == ownedIdx.end()) continue; v = "#" + std::to_string(it->second); }
            sets += (sets.empty() ? "" : "\x1F") + k + "\x1E" + v;
        }
        for (int i = 0; i < m.nTeams; ++i) { names += (i ? "," : "") + m.teamName[i]; maxes += (i ? "," : "") + std::to_string(m.teamMax[i]); }
        char at[96];
        snprintf(at, sizeof(at), "%.1f,%.1f,%.1f", m.at[0], m.at[1], m.at[2]);
        t += "G\t" + m.id + "\t" + m.name + "\t" + at + "\t" + std::to_string(m.nTeams) + "\t" + names + "\t" + maxes + "\t" + SeHexOf(sets) + "\n";
    }
    return t;
}
static std::unordered_map<std::string, std::string> g_seModeLoadSets;   // mode id -> raw settings text, until fixup
static void SeModesLoadCreate(const std::string& level, const std::vector<std::vector<std::string>>& lines)
{
    for (const auto& f : lines)
    {
        if (f[0] != "G" || f.size() < 8) continue;
        double at[3];
        if (!SeVec(f[3], at)) continue;
        int maxes[8] = { 4, 4, 4, 4, 4, 4, 4, 4 };
        std::string names[8];
        { auto v = SeSplit(f[5], ',', 8); for (size_t i = 0; i < v.size(); ++i) names[i] = v[i]; }
        { auto v = SeSplit(f[6], ',', 8); for (size_t i = 0; i < v.size(); ++i) maxes[i] = atoi(v[i].c_str()); }
        SeGameMode* m = SeModeCreate(f[2], at, atoi(f[4].c_str()), maxes, names, level, std::string(), nullptr, false);
        if (!m) continue;
        g_seModeLoadSets[m->id] = SeUnhex(f[7]);
    }
}
static void SeModesLoadFixup(const std::string& level, const std::unordered_map<int, std::string>& objIdx)
{
    for (auto& m : g_seModes)
    {
        if (m.deleted || m.level != level) continue;
        auto raw = g_seModeLoadSets.find(m.id);
        if (raw == g_seModeLoadSets.end()) continue;
        for (const auto& kvs : SeSplit(raw->second, '\x1F', 512))
        {
            const size_t e = kvs.find('\x1E');
            if (e == std::string::npos) continue;
            std::string k = kvs.substr(0, e), v = kvs.substr(e + 1);
            if (k.rfind("role.#", 0) == 0) { auto it = objIdx.find(atoi(k.c_str() + 6)); if (it == objIdx.end()) continue; k = "role." + it->second; }
            if ((k == "controller" || k == "scripthost") && !v.empty() && v[0] == '#') { auto it = objIdx.find(atoi(v.c_str() + 1)); if (it == objIdx.end()) continue; v = it->second; }
            m.settings[k] = v;
        }
        g_seModeLoadSets.erase(raw);
        if (m.settings.count("controller")) SeModeEnsureHost(m, level);   // levels saved before script hosts existed get one
        SeModePushAll(m);
        SeModeLobbyBall(m);
        HxLog("[HalcyonA2][GAMEMODE] '%s' loaded with level '%s' (%zu setting(s))\n", m.name.c_str(), level.c_str(), m.settings.size());
    }
    SeModeListTo(nullptr);
}
static void SeModesUnload(const std::string& level)
{
    bool any = false;
    for (auto& m : g_seModes) if (!m.deleted && !level.empty() && m.level == level) { SeModeDelete(m); any = true; }
    if (any) SeModeListTo(nullptr);
}

static void SeModeTick()
{
    static ULONGLONG s_last = 0;
    const ULONGLONG now = GetTickCount64();
    std::vector<std::pair<void*, std::pair<std::string, std::string>>> evs;
    AcquireSRWLockExclusive(&g_seTextLock);
    evs.swap(g_seModeEvents);
    ReleaseSRWLockExclusive(&g_seTextLock);
    for (auto& ev : evs)
        if (SeGameMode* m = SeModeByApi(ev.first)) SeModeEvent(*m, ev.second.first, ev.second.second);
    if (now - s_last < 250) return;
    s_last = now;
    if (!g_seBallPending.empty()) SeBallPendingTick();
    for (auto& m : g_seModes)
    {
        if (m.deleted || !m.slot || !SeAlive(m.slot)) continue;
        if (m.starterClearAt && now >= m.starterClearAt)
        {
            static std::unordered_map<std::string, int> s_tries;   // the project's objects appear a few seconds after the load
            const int tries = ++s_tries[m.id];
            const int n = SeModeClearStarters(m);
            m.starterClearAt = (n == 0 && tries < 4) ? now + 6000ULL * tries : 0;
            HxLog("[HalcyonA2][GAMEMODE] %s: %d starter object(s) parked, ticket manager %s\n", m.name.c_str(), n,
                  *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(m.slot) + 0x438) ? "kept" : "GONE");
        }
        SeModeReadTeams(m);
        int players = 0; bool everyTeam = true;
        const int minP = (std::max)(1, SeModeSettingInt(m, "min_players"));
        for (int t = 0; t < m.nTeams; ++t) { players += m.teamSize[t]; if (m.teamSize[t] < minP) everyTeam = false; }
        const double el = (now - m.stateAt) / 1000.0;
        const std::string mode = SeModeSetting(m, "start_mode");
        if (m.state == "idle")
        {
            m.timeLeft = 0;
            if (mode == "auto" && everyTeam && players > 0) SeModeSetState(m, SeModeSettingInt(m, "countdown") > 0 ? "countdown" : "running");
        }
        else if (m.state == "countdown")
        {
            const int cd = SeModeSettingInt(m, "countdown");
            m.timeLeft = (std::max)(0, static_cast<int>(ceil(cd - el)));
            if (el >= cd) SeModeSetState(m, "running");
            else if (mode == "auto" && !everyTeam) SeModeSetState(m, "idle");      // someone left before it began
        }
        else if (m.state == "running")
        {
            const int rt = SeModeSettingInt(m, "round_time");
            m.timeLeft = rt > 0 ? (std::max)(0, static_cast<int>(ceil(rt - el))) : 0;
            if (rt > 0 && el >= rt) SeModeEnd(m, 0);
            // "End a round when every team is empty" is for ABANDONED rounds: only once someone has been on a team this
            // round. A round started with empty teams (a solo test, or people who haven't picked a side yet) kept
            // ending the instant it began -- the ball never played and score buttons did nothing (2026-09-24).
            if (players > 0) m.hadPlayers = true;
            if (SeModeSettingInt(m, "stop_when_empty") && m.hadPlayers && players == 0 && el > 3.0) SeModeEnd(m, 0);
        }
        else if (m.state == "ended")
        {
            const int ed = SeModeSettingInt(m, "end_delay");
            m.timeLeft = (std::max)(0, static_cast<int>(ceil(ed - el)));
            if (el >= ed) SeModeSetState(m, (mode == "auto" && SeModeSettingInt(m, "auto_restart")) ? "idle" : "idle");
        }
        SeModePushAll(m);
        // editors see the live status (once a second at most, and only when it changed)
        const std::string rec = SeModeRecord(m);
        static ULONGLONG s_stat = 0;
        if (rec != m.statSent && now - s_stat > 900) { m.statSent = rec; s_stat = now; SeBroadcast("SE|GMSTAT|" + rec, nullptr); }
    }
}
static void SeLvTick()
{
    static bool s_started = false;
    if (!s_started && g_seSandbox && SbEngine()) { s_started = true; std::thread(SeLvPoller).detach(); }
    std::vector<SeLvJob> jobs;
    {
        std::lock_guard<std::mutex> lk(g_lvMu);
        jobs.swap(g_lvJobs);
        g_lvLoadedSnapshot = g_lvLoaded;
        g_lvLocalSnapshot = g_lvLocal;
    }
    for (auto& j : jobs)
    {
        if (j.kind == 1) SeLvLoad(j.name, j.text);
        else if (j.kind == 2) { if (std::find(g_lvLocal.begin(), g_lvLocal.end(), j.name) == g_lvLocal.end()) SeLvUnload(j.name); }
        else if (j.kind == 3 && j.pc && SeAlive(j.pc)) SeBroadcast(j.text, j.pc);
    }
    if (!jobs.empty()) { std::lock_guard<std::mutex> lk(g_lvMu); g_lvLoadedSnapshot = g_lvLoaded; g_lvLocalSnapshot = g_lvLocal; }
}

// Editor commands: SE|LVSAVE|name, SE|LVLIST, SE|LVLOAD|name, SE|LVUNLOAD|name
static void SeLvSave(SDK::UObject* ctx, const std::string& rawName)
{
    std::string name;
    for (char c : rawName) if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') name += c;
    SDK::UObject* pc = SeCallerPC(ctx);
    if (name.empty() || name.size() > 64) { if (pc) SeBroadcast("SE|NOTE|Level names: letters, numbers, space, - and _ (up to 64).", pc); return; }
    int counts[3] = {};
    const std::string text = SeLvBuild(name, counts);
    if (std::find(g_lvLoaded.begin(), g_lvLoaded.end(), name) == g_lvLoaded.end()) g_lvLoaded.push_back(name);
    SeLvMarkLocal(name);
    g_lvStatusDirty = true;
    char summary[160];
    snprintf(summary, sizeof(summary), "%d object(s), %d quest(s), %d coin run(s)", counts[0], counts[1], counts[2]);
    const std::string sum = summary;
    std::thread([name, text, pc, sum]() {
        DWORD st = 0;
        SeLvHttp(L"PUT", "/v1/spec/levels/" + SeLvUrlName(name), text, &st);
        SeLvQueue({ 3, name, st == 200 ? "SE|NOTE|Saved level '" + name + "' (" + sum + ") to the backend." :
                                         "SE|NOTE|Saving level '" + name + "' FAILED (backend HTTP " + std::to_string(st) + ").", pc });
        g_lvPollNow = true;
    }).detach();
    HxLog("[HalcyonA2][LEVELS] saving '%s': %s\n", name.c_str(), summary);
}
// ---- local projects (.a2level) -------------------------------------------------------------------------
// A level file on the editor's own PC: the same text a saved level is (see SeLvBuild), so it can be taken
// home, shared, and brought back to any server -- loaded here as a level, or uploaded as a saved level.
// Export: SE|LVEXPORT|<name> -> SE|LVDATA|<name>|<i>|<n>|<hex> to the caller (hex: every byte survives the
// trip, titles included). Import: SE|LVPART|<hex> (repeated, in order) then SE|LVIMPORT|<name>|load|save.
static std::unordered_map<std::string, std::string> g_lvImportBuf;   // caller id -> text so far
// tag = the editor's open scene is being saved: what is built becomes that level's (so closing the scene
// removes it, and opening another scene starts clean), exactly like a Unity scene owns its objects.
static void SeLvExport(SDK::UObject* ctx, const std::string& rawName, bool tag = false)
{
    SDK::UObject* pc = SeCallerPC(ctx);
    if (!pc) return;
    std::string name;
    for (char c : rawName) if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') name += c;
    if (name.empty()) name = "Untitled";
    int counts[3] = {};
    const std::string text = SeLvBuild(name, counts, tag);
    if (tag && std::find(g_lvLoaded.begin(), g_lvLoaded.end(), name) == g_lvLoaded.end()) { g_lvLoaded.push_back(name); g_lvStatusDirty = true; }
    if (tag) SeLvMarkLocal(name);
    static const char* hx = "0123456789ABCDEF";
    std::string hex;
    hex.reserve(text.size() * 2);
    for (unsigned char c : text) { hex.push_back(hx[c >> 4]); hex.push_back(hx[c & 15]); }
    const size_t kChunk = 16000;
    const size_t n = (std::max<size_t>)(1, (hex.size() + kChunk - 1) / kChunk);
    for (size_t i = 0; i < n; ++i)
        SeBroadcast("SE|LVDATA|" + name + "|" + std::to_string(i) + "|" + std::to_string(n) + "|" + hex.substr(i * kChunk, kChunk), pc);
    HxLog("[HalcyonA2][LEVELS] exported '%s' to the caller: %d object(s), %zu byte(s), %zu message(s)\n", name.c_str(), counts[0], text.size(), n);
}
static void SeLvImportPart(const std::string& hex)
{
    std::string& buf = g_lvImportBuf[g_seCaller];
    if (buf.size() > 4 * 1024 * 1024) return;                   // a level is tens of KB; refuse runaway uploads
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        buf.push_back(static_cast<char>(strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
}
static void SeLvImport(SDK::UObject* ctx, const std::string& rawName, const std::string& mode)
{
    SDK::UObject* pc = SeCallerPC(ctx);
    std::string text;
    text.swap(g_lvImportBuf[g_seCaller]);
    g_lvImportBuf.erase(g_seCaller);
    std::string name;
    for (char c : rawName) if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') name += c;
    if (name.empty() || name.size() > 64 || text.rfind("L\t", 0) != 0)
    {
        if (pc) SeBroadcast("SE|NOTE|That file isn't a level (or its name is invalid) - nothing loaded.", pc);
        HxLog("[HalcyonA2][LEVELS] import '%s' refused: %zu byte(s), bad header or name\n", name.c_str(), text.size());
        return;
    }
    // The file's own name line is replaced by the one it is imported as.
    const size_t eol = text.find('\n');
    text = "L\t" + SeLvClean(name) + "\t1" + (eol == std::string::npos ? std::string("\n") : text.substr(eol));
    if (std::find(g_lvLoaded.begin(), g_lvLoaded.end(), name) != g_lvLoaded.end()) SeLvUnload(name);   // replace, don't stack
    SeLvLoad(name, text);
    SeLvMarkLocal(name);
    if (mode == "save")
    {
        std::thread([name, text, pc]() {
            DWORD st = 0;
            SeLvHttp(L"PUT", "/v1/spec/levels/" + SeLvUrlName(name), text, &st);
            SeLvQueue({ 3, name, st == 200 ? "SE|NOTE|Uploaded '" + name + "' to the server's saved levels (and loaded it here)." :
                                             "SE|NOTE|Loaded '" + name + "' here, but uploading it FAILED (backend HTTP " + std::to_string(st) + ").", pc });
            g_lvPollNow = true;
        }).detach();
    }
    else if (pc) SeBroadcast("SE|NOTE|Loaded '" + name + "' on this server (not uploaded - Unload removes it).", pc);
    g_lvStatusDirty = true;
    HxLog("[HalcyonA2][LEVELS] imported '%s' (%zu byte(s), %s)\n", name.c_str(), text.size(), mode.c_str());
}

static void SeLvList(SDK::UObject* ctx)
{
    SDK::UObject* pc = SeCallerPC(ctx);
    std::string loaded;
    for (const auto& l : g_lvLoaded) loaded += (loaded.empty() ? "" : ";") + l;
    std::thread([pc, loaded]() {
        DWORD st = 0;
        const std::string body = SeLvHttp(L"GET", "/v1/spec/levels", "", &st);
        std::string out;
        for (auto l : SeLvLines(body)) { for (auto& c : l) if (c == '|') c = '/'; out += (out.empty() ? "" : "\x1E") + l; }
        SeLvQueue({ 3, "", st == 200 ? "SE|LVLIST|" + loaded + "|" + out : "SE|NOTE|Could not reach the backend for the level list (HTTP " + std::to_string(st) + ").", pc });
    }).detach();
}
static void SeLvSetLoaded(SDK::UObject* ctx, const std::string& name, bool load)
{
    SDK::UObject* pc = SeCallerPC(ctx);
    if (!load && std::find(g_lvLocal.begin(), g_lvLocal.end(), name) != g_lvLocal.end()) SeLvUnload(name);   // the poller won't
    std::thread([name, load, pc]() {
        DWORD st = 0;
        SeLvHttp(L"POST", "/v1/spec/levels/" + SeLvUrlName(name) + "/loaded?value=" + (load ? "1" : "0"), "", &st);
        g_lvPollNow = true;
        SeLvQueue({ 3, name, st == 200 ? std::string("SE|NOTE|") + (load ? "Loading" : "Unloading") + " level '" + name + "'..." :
                                         "SE|NOTE|Backend refused (HTTP " + std::to_string(st) + ").", pc });
    }).detach();
}

// ---- owner locks ---------------------------------------------------------------------------
static std::string SeCallerIdOf(SDK::UObject* ctx)
{
    SDK::UObject* pc = SeCallerPC(ctx);
    if (!pc) return std::string();
    std::string uid = FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0xA30));
    if (uid.empty() && g_seLocalTest) uid = "local:" + pc->GetName();    // no dashboard login on a test server
    return uid;
}
static std::string SeCallerNameOf(SDK::UObject* ctx)
{
    SDK::UObject* pc = SeCallerPC(ctx);
    auto* ps = pc ? static_cast<SDK::APlayerController*>(pc)->PlayerState : nullptr;
    std::string n = ps ? ps->GetPlayerName().ToString() : std::string();
    for (char& c : n) if (c == '|' || c == ';' || c == ',' || static_cast<unsigned char>(c) < 32) c = ' ';
    return n.empty() ? std::string("another editor") : n;
}
// The sandbox idx an ident names: "#<id>" when the client sent one, else the actor at that spot.
static std::string SeIdxOfIdent(const std::string& ident)
{
    const size_t h = ident.find('#');
    if (h != std::string::npos && ident.size() - h - 1 >= 32) return ident.substr(h + 1);
    SDK::AActor* a = SeFindEditorActor(ident);
    SDK::UObject* pc = a ? SbPrefabOf(a) : nullptr;
    return pc ? FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0x248)) : std::string();
}
// Tell editors which objects are locked: SE|OWNLOCKS|<idx>,<1 if theirs>,<owner name>;...  Each controller
// gets its own copy (the "theirs" flag differs). Locks on objects that no longer exist are dropped.
static void SeOwnLocksPush(SDK::UObject* onlyPc)
{
    for (auto it = g_ownLocks.begin(); it != g_ownLocks.end();)
    {
        bool alive = false;
        for (const auto& o : g_sbOwned) if (o.idx == it->first) { alive = true; break; }
        it = alive ? std::next(it) : g_ownLocks.erase(it);
    }
    auto sendTo = [&](SDK::UObject* pc) {
        const std::string me = SeCallerIdOf(pc);
        std::string msg = "SE|OWNLOCKS|";
        for (const auto& kv : g_ownLocks)
            if (kv.second.locked)
                msg += kv.first + "," + (kv.second.owner == me ? "1" : "0") + "," + kv.second.ownerName + ";";
        SeBroadcast(msg, pc);
    };
    if (onlyPc) { sendTo(onlyPc); return; }
    for (SDK::UObject* o : ClassObjects(SDK::APlayerController::StaticClass()))
    {
        if (!o || o->IsDefaultObject() || (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01)) continue;
        if (SeAuthorised(o)) sendTo(o);
    }
}
// SE|OWNLOCK|<ident#id>|1|0. Only the placer may lock or unlock. An object nobody is recorded as owning
// (placed before owners were tracked, or loaded from a saved level) goes to the first editor who locks it.
static void SeOwnLockSet(SDK::UObject* pawn, const std::string& ident, bool on)
{
    SDK::UObject* pc = SeCallerPC(pawn);
    const std::string idx = SeIdxOfIdent(ident);
    if (idx.empty() || !SbOwnedByIdx(idx))
    {
        if (pc) SeBroadcast("SE|NOTE|Only objects placed with the editor can be locked.", pc);
        return;
    }
    SeOwnLock& L = g_ownLocks[idx];
    if (L.owner.empty()) { L.owner = g_seCaller; L.ownerName = g_seCallerName; }
    if (L.owner != g_seCaller)
    {
        if (pc) SeBroadcast("SE|NOTE|Only " + L.ownerName + " (who placed it) can lock or unlock this object.", pc);
        return;
    }
    L.locked = on;
    HxLog("[HalcyonA2][SPECEDIT] owner lock %s: %s by %s\n", idx.c_str(), on ? "LOCKED" : "unlocked", L.ownerName.c_str());
    SeOwnLocksPush(nullptr);
}
// Refuse an edit to an object its owner has locked. True = refused (the command must not run).
static bool SeOwnLockRefuses(SDK::UObject* pawn, const std::string& op, const std::string& ident)
{
    static const char* kGuarded[] = { "XFORM", "DELETE", "SBSET", "LUAU", "LUAUREF", "LUAUDEL", "PROP" };
    bool guarded = false;
    for (const char* g : kGuarded) if (op == g) guarded = true;
    if (!guarded || g_ownLocks.empty()) return false;
    bool anyLocked = false;
    for (const auto& kv : g_ownLocks) if (kv.second.locked) { anyLocked = true; break; }
    if (!anyLocked) return false;                        // the common case: no lookup at all
    const std::string idx = SeIdxOfIdent(ident);
    auto it = idx.empty() ? g_ownLocks.end() : g_ownLocks.find(idx);
    if (it == g_ownLocks.end() || !it->second.locked || it->second.owner == g_seCaller) return false;
    static ULONGLONG s_lastNote = 0;
    const ULONGLONG now = GetTickCount64();
    if (now - s_lastNote > 1500)                         // a drag sends 25/s: one note, not a flood
    {
        s_lastNote = now;
        if (SDK::UObject* pc = SeCallerPC(pawn)) SeBroadcast("SE|NOTE|Locked by " + it->second.ownerName + " - only they can change it.", pc);
    }
    return true;
}

// ---- editor camera markers -------------------------------------------------------------------
// Someone editing flies a client-local camera; to everyone else they were a VR avatar standing wherever
// they entered the editor. Each editor now gets the game's own level-editor pawn (BP_LevelEditor_Pawn: a
// floating cube), spawned by the server, replicated, and moved to the editor camera the client reports
// (SE|CAMPOS|x,y,z|p,y,r, ~8/s while it moves, a heartbeat otherwise). It goes on EXIT, or 20 s after the
// last report (left / crashed). The editor hides their own copy locally.
struct SeCamMarker { SDK::AActor* actor = nullptr; int32_t index = -1; ULONGLONG seen = 0; };
static std::unordered_map<std::string, SeCamMarker> g_camMarkers;   // caller id -> marker
static bool SeCamMarkerAlive(const SeCamMarker& m)
{
    return m.actor && SDK::UObject::GObjects->GetByIndex(m.index) == m.actor &&
           !(*(reinterpret_cast<const uint8_t*>(m.actor) + 0x65) & 0x01);
}
static void SeCamMarkerDrop(SeCamMarker& m)
{
    if (SeCamMarkerAlive(m)) m.actor->K2_DestroyActor();
    m.actor = nullptr; m.index = -1;
}
static void SeCamMarkersExpire()
{
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_camMarkers.begin(); it != g_camMarkers.end();)
    {
        if (now - it->second.seen > 20000) { SeCamMarkerDrop(it->second); it = g_camMarkers.erase(it); }
        else ++it;
    }
}
static std::unordered_map<std::string, ULONGLONG> g_camExitAt;   // caller -> when they left the editor
static void SeCamMarkersTick(ULONGLONG now)
{
    static ULONGLONG s_last = 0;
    if (g_camMarkers.empty() || now - s_last < 1000) return;   // a map of a few entries, once a second
    s_last = now;
    SeCamMarkersExpire();
}
static SDK::UClass* SeCamMarkerClass()
{
    static SDK::UClass* s_cls = nullptr;
    static bool s_tried = false;
    if (s_cls || s_tried) return s_cls;
    s_tried = true;
    if ((s_cls = SDK::UObject::FindClassFast("BP_LevelEditor_Pawn_C")) != nullptr) return s_cls;
    SDK::FSoftClassPath scp = SDK::UKismetSystemLibrary::MakeSoftClassPath(SDK::FString(L"/Game/BP_LevelEditor_Pawn.BP_LevelEditor_Pawn_C"));
    s_cls = SDK::UKismetSystemLibrary::LoadClassAsset_Blocking(SDK::UKismetSystemLibrary::Conv_SoftClassPathToSoftClassRef(scp));
    HxLog("[HalcyonA2][SPECEDIT] editor camera marker class: %s\n", s_cls ? "loaded" : "NOT FOUND - markers off");
    return s_cls;
}
static void SeCamMarkerMove(SDK::UObject* pawn, const std::string& locs, const std::string& rots)
{
    double l[3], r[3] = { 0, 0, 0 };
    if (g_seCaller.empty() || !SeVec(locs, l)) return;
    // A report already in flight when the editor left must not bring the marker back.
    if (auto ex = g_camExitAt.find(g_seCaller); ex != g_camExitAt.end() && GetTickCount64() - ex->second < 5000) return;
    SeVec(rots, r);
    SeCamMarker& m = g_camMarkers[g_seCaller];
    m.seen = GetTickCount64();
    const SDK::FVector loc{ l[0], l[1], l[2] };
    const SDK::FRotator rot{ r[0], r[1], r[2] };
    if (!SeCamMarkerAlive(m))
    {
        SDK::UClass* cls = SeCamMarkerClass();
        if (!cls) return;
        SDK::FTransform xf{};
        xf.Rotation = SDK::FQuat{ 0, 0, 0, 1 };
        xf.Translation = loc;
        xf.Scale3D = SDK::FVector{ 1, 1, 1 };
        SDK::AActor* a = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
            pawn, cls, xf, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn, nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
        if (!a) return;
        SDK::UGameplayStatics::FinishSpawningActor(a, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
        a->SetActorEnableCollision(false);                // a marker: nothing may bump into it
        if (SDK::UObject* pc = SeCallerPC(pawn)) a->SetOwner(static_cast<SDK::AActor*>(pc));   // its editor hides their own copy
        a->SetReplicates(true);
        a->SetReplicateMovement(true);
        m.actor = a; m.index = a->Index;
        HxLog("[HalcyonA2][SPECEDIT] editor camera marker for %s: %s\n", g_seCallerName.c_str(), a->GetName().c_str());
    }
    // The pawn's own replicated pose (RepData: NetQuantize10 position + rotation, OnRep_Data) and its actor
    // transform (replicated movement): whichever a client applies, both say the same thing.
    double* rep = reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(m.actor) + 0x400);
    rep[0] = l[0]; rep[1] = l[1]; rep[2] = l[2]; rep[3] = r[0]; rep[4] = r[1]; rep[5] = r[2];
    SDK::FHitResult hit{};
    m.actor->K2_SetActorLocationAndRotation(loc, rot, false, &hit, true);
    m.actor->ForceNetUpdate();
}

// ---- entry point ---------------------------------------------------------------------------
// Returns true when the string was ours and the original lock RPC should NOT run.
static bool SpecEditHandle(SDK::UObject* pawn, const std::string& cmd)
{
    if (cmd.size() < 4 || cmd.size() > 1024) return false;            // fence 5: bounded
    if (cmd.rfind("SE|", 0) != 0) return false;                       // not ours: let the game have it

    if (!g_specEdit) return true;                                     // fence 1: swallow, do nothing
    if (!SeAuthorised(pawn)) return true;                             // fence 2

    const ULONGLONG now = GetTickCount64();                           // fence 5: rate cap
    if (now - g_seLastCmd > 1000) { g_seLastCmd = now; g_seCmdsThisSecond = 0; }
    if (++g_seCmdsThisSecond > 60) return true;

    auto p = SeSplit(cmd, '|', 16);
    const std::string op = p.size() > 1 ? p[1] : std::string();
    // LOCAL TEST: a position field "@P" or "@Pdx,dy,dz" = the first other player's position (+ offset). Lets the
    // tests put game modes / team changers where a real (unmodded) player stands.
    if (g_seLocalTest)
        for (size_t i = 2; i < p.size(); ++i)
            if (p[i].rfind("@P", 0) == 0)
            {
                double at[3], off[3] = { 0, 0, 0 };
                if (p[i].size() > 2) SeVec(p[i].substr(2), off);
                if (!SeOtherPlayerPos(pawn, at)) { HxLog("[HalcyonA2][SPECEDIT] @P: no other player\n"); return true; }
                char b[96];
                snprintf(b, sizeof(b), "%.1f,%.1f,%.1f", at[0] + off[0], at[1] + off[1], at[2] + off[2]);
                HxLog("[HalcyonA2][SPECEDIT] @P -> %s (%s)\n", b, op.c_str());
                p[i] = b;
            }
    g_seCaller = SeCallerIdOf(pawn);
    g_seCallerName = SeCallerNameOf(pawn);
    struct CallerReset { ~CallerReset() { g_seCaller.clear(); g_seCallerName.clear(); } } callerReset;
    if (p.size() >= 3 && SeOwnLockRefuses(pawn, op, p[2])) return true;

    if (!g_camMarkers.empty()) SeCamMarkersExpire();
    if      (op == "OWNLOCK" && p.size() >= 4) SeOwnLockSet(pawn, p[2], p[3] == "1");
    else if (op == "CAMPOS" && p.size() >= 4) SeCamMarkerMove(pawn, p[2], p[3]);
    else if (op == "SPAWN"  && p.size() >= 5)
    {
        const std::string prev = g_lvLoading;
        g_lvLoading = SeCallerScene();                   // placed into the editor's open level
        SeSpawn(pawn, p[2], p[3], p[4], p.size() >= 6 ? p[5] : std::string());
        g_lvLoading = prev;
    }
    else if (op == "LVSCENE" && p.size() >= 2)          // SE|LVSCENE|<name> ("" = no level open)
    {
        std::string n;
        if (p.size() >= 3) for (char c : p[2]) if (isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '-') n += c;
        std::string& cur = g_seCallerScene[g_seCaller];
        if (cur != n) HxLog("[HalcyonA2][LEVELS] %s's open level: '%s'\n", g_seCallerName.c_str(), n.c_str());
        cur = n;
    }
    else if (op == "XFORM"  && p.size() >= 6) SeTransform(SeResolveIdent(p[2]), p[3], p[4], p[5]);
    else if (op == "DELETE" && p.size() >= 3) SeDelete(SeResolveIdent(p[2]));
    else if (op == "PROP"   && p.size() >= 5) SeSetProp(p[2], p[3], p[4]);
    else if (op == "AUDIT"  && p.size() >= 3) SeAudit(pawn, p[2]);
    else if (op == "TESTQUEST" && p.size() >= 3) SeTestQuestAtPlayer(pawn, p[2]);
    else if (op == "SANDBOX") SeSandboxProbe();
    else if (op == "SBADD"  && p.size() >= 4)
    {
        const std::string prev = g_lvLoading;
        g_lvLoading = SeCallerScene();
        SeSandboxAdd(p[2], p[3]);
        g_lvLoading = prev;
    }
    else if (op == "SBADDATPLAYER" && p.size() >= 4) SeSandboxAddAtPlayer(pawn, p[2], p[3]);
    else if (op == "SBDESC" && p.size() >= 3) SeSandboxDescDump(p[2]);
    else if (op == "SBTEXTS" && p.size() >= 3) SeSandboxTextsAtPlayer(p[2]);
    else if (op == "SBPROBE" && p.size() >= 3) SeSandboxProbePaths(p[2]);
    else if (op == "SBTREE" && p.size() >= 3) SeSandboxTree(p[2]);
    else if (op == "QLIST") SeQuestList(pawn);
    else if (op == "QDEL" && p.size() >= 3) SeQuestDelete(pawn, p[2]);
    else if (op == "LUAUDEL" && p.size() >= 4) SeLuauRemove(pawn, p[2], p[3]);
    else if (op == "QCOMPLETE" && p.size() >= 3) SeTestCompleteQuest(p[2]);
    else if (op == "LVSAVE" && p.size() >= 3) SeLvSave(pawn, p[2]);
    else if (op == "SBDUP"  && p.size() >= 6) SeSandboxDuplicate(pawn, p[2], p[3], p[4], p[5]);
    else if (op == "LVLIST") SeLvList(pawn);
    else if (op == "LVEXPORT" && p.size() >= 3) SeLvExport(pawn, p[2], p.size() >= 4 && p[3] == "1");
    else if (op == "LVCLOSE" && p.size() >= 3)                // the editor closes its open scene: remove it from this server
    {
        if (std::find(g_lvLoaded.begin(), g_lvLoaded.end(), p[2]) != g_lvLoaded.end()) SeLvUnload(p[2]);
        HxLog("[HalcyonA2][LEVELS] scene '%s' closed by the editor\n", p[2].c_str());
    }
    else if (op == "LVPART" && p.size() >= 3) SeLvImportPart(p[2]);
    else if (op == "LVIMPORT" && p.size() >= 4) SeLvImport(pawn, p[2], p[3]);
    else if (op == "LVLOAD" && p.size() >= 3) SeLvSetLoaded(pawn, p[2], true);
    else if (op == "LVUNLOAD" && p.size() >= 3) SeLvSetLoaded(pawn, p[2], false);
    else if (op == "LUAUPART" && p.size() >= 4) SeLuauPart(p[2], p[3]);
    else if (op == "LUAU" && p.size() >= 4) SeLuau(pawn, p[2], p[3]);
    else if (op == "LUAUSRC" && p.size() >= 3) SeLuauUpdateAll(pawn, p[2]);
    else if (op == "LUAUREF" && p.size() >= 7) SeLuauRef(pawn, p[2], p[3], p[4], p[5], p[6]);
    else if (op == "REFKEY" && p.size() >= 3 && g_seLocalTest) g_seForceRefKey = p[2];
    else if (op == "ALLOWTEAM" && p.size() >= 3 && g_seLocalTest) g_seAllowTeamChangers = p[2] == "1";
    else if (op == "LOADSLOT" && p.size() >= 4 && g_seLocalTest) SeLoadMapSlot(p[2], p[3]);   // SE|LOADSLOT|<slot id>|<project>
    // ---- game modes (the Game Modes window) ----
    else if (op == "GMNEW" && p.size() >= 5)              // SE|GMNEW|name|x,y,z|teams|max csv|names csv
    {
        double at[3];
        if (SeVec(p[3], at))
        {
            const int nT = atoi(p[4].c_str());
            int maxes[8] = { 4, 4, 4, 4, 4, 4, 4, 4 };
            std::string names[8];
            if (p.size() >= 6) { auto v = SeSplit(p[5], ',', 8); for (size_t i = 0; i < v.size(); ++i) maxes[i] = atoi(v[i].c_str()); }
            if (p.size() >= 7) { auto v = SeSplit(p[6], ',', 8); for (size_t i = 0; i < v.size(); ++i) names[i] = v[i]; }
            SDK::UObject* pc = SeCallerPC(pawn);
            if (SeGameMode* m = SeModeCreate(p[2], at, nT, maxes, names, SeCallerScene(), std::string(), pc))
                if (pc) SeBroadcast("SE|NOTE|Game mode '" + m->name + "' created. Place its objects inside the orange box.", pc);
            SeModeListTo(nullptr);
        }
    }
    else if (op == "GMSET" && p.size() >= 5)              // SE|GMSET|id|key|value
    {
        if (SeGameMode* m = SeModeById(p[2]))
        {
            const std::string k = p[3];
            if (k == "name") { const std::string n = SeModeClean(p[4], 32); if (!n.empty()) m->name = n; }
            else if (k.rfind("team", 0) == 0 && k.find('.') != std::string::npos)      // teamN.name / teamN.max
            {
                const int t = atoi(k.c_str() + 4) - 1;
                if (t >= 0 && t < m->nTeams)
                {
                    if (k.find(".name") != std::string::npos) m->teamName[t] = SeModeClean(p[4], 20);
                    else if (k.find(".max") != std::string::npos)
                    {
                        m->teamMax[t] = (std::max)(1, (std::min)(64, atoi(p[4].c_str())));
                        int32_t* arr = *reinterpret_cast<int32_t**>(reinterpret_cast<uintptr_t>(m->slot) + 0x428);
                        if (arr && t < *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(m->slot) + 0x430)) arr[t] = m->teamMax[t];
                    }
                }
            }
            else
            {
                // keys: role.<object id>, custom.<name>, the rules -- letters, digits, '.', '_', '-'
                std::string key, val;
                for (char c : k) if (isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '_' || c == '-') key += c;
                for (char c : p[4]) if (c != ';' && c != '~' && c != '=' && c != '|' && static_cast<unsigned char>(c) >= 0x20) val += c;
                key = key.substr(0, 64);
                if (!key.empty()) { if (val.empty() && key.rfind("role.", 0) == 0) m->settings.erase(key); else m->settings[key] = val.substr(0, 200); }
                if (key.rfind("role.", 0) == 0 && (val.rfind("start_ring", 0) == 0 || val.rfind("ball", 0) == 0)) SeModeLobbyBall(*m);
            }
            SeModePushAll(*m);
            SeModeListTo(nullptr);
        }
    }
    else if (op == "GMCTL" && p.size() >= 4)              // SE|GMCTL|id|start|stop|reset|end[,winner]
    {
        if (SeGameMode* m = SeModeById(p[2]))
        {
            const std::string c = p[3];
            if (c == "start") SeModeEvent(*m, "rgm:start", "");
            else if (c == "startnow") SeModeEvent(*m, "rgm:startnow", "");
            else if (c == "stop" || c == "end") SeModeEvent(*m, "rgm:end", p.size() >= 5 ? p[4] : std::string());
            else if (c == "reset") SeModeEvent(*m, "rgm:reset", "");
            SeModeListTo(nullptr);
        }
    }
    else if (op == "GMDEL" && p.size() >= 3) { if (SeGameMode* m = SeModeById(p[2])) { SeModeDelete(*m); SeModeListTo(nullptr); } }
    else if (op == "GMLIST") SeModeListTo(SeCallerPC(pawn));
    else if (op == "GMTEAM" && p.size() >= 5)             // SE|GMTEAM|id|team (1-based)|x,y,z[|yaw] : a team changer for that team
    {
        SeGameMode* m = SeModeById(p[2]);
        double at[3];
        const int team = atoi(p[3].c_str());
        if (m && SeVec(p[4], at) && team >= 1 && team <= m->nTeams)
        {
            SeGameMode* here = SeModeAt(at);
            if (here != m) { if (SDK::UObject* pc = SeCallerPC(pawn)) SeBroadcast("SE|NOTE|Team changers for '" + m->name + "' go inside its area.", pc); }
            else
            {
                // A team changer's origin is the middle of its door, ~1.5 m above the floor (the station's arena doors
                // sit 150 cm over the floor; the door's collision reaches 151 cm below the origin). Placed with the
                // origin ON the floor -- as game modes did -- the door was sunk into the ground and all but vanished.
                // So: find the floor under the spot and stand the door on it. Yaw = the way players walk through it.
                // "down" is along the station's local up (toward the hull): straight down on the bottom floor, sideways
                // on the walls. The door stands on what it hits, upright for that floor, turned `yaw` about its up.
                double up[3]; SeStationUp(at, up);
                double door[3] = { at[0], at[1], at[2] };
                {
                    SDK::FHitResult hit{};
                    SDK::TArray<SDK::AActor*> ignore{};
                    const bool ok = SDK::UKismetSystemLibrary::LineTraceSingle(SDK::UWorld::GetWorld(),
                        SDK::FVector{ at[0] + up[0] * 100.0, at[1] + up[1] * 100.0, at[2] + up[2] * 100.0 },
                        SDK::FVector{ at[0] - up[0] * 3000.0, at[1] - up[1] * 3000.0, at[2] - up[2] * 3000.0 }, SDK::ETraceTypeQuery::TraceTypeQuery1,
                        false, ignore, SDK::EDrawDebugTrace::None, &hit, true, SDK::FLinearColor{}, SDK::FLinearColor{}, 0.0f);
                    if (ok && hit.bBlockingHit) { door[0] = hit.Location.X; door[1] = hit.Location.Y; door[2] = hit.Location.Z; }
                    for (int k = 0; k < 3; ++k) door[k] += up[k] * 150.0;
                }
                // Yaw: walking through along the door's +X joins its team, walking back out clears it (measured with a
                // real player). With no yaw given the door faces the mode's centre, so walking IN joins and walking out
                // leaves -- a door can't end up backwards.
                double yaw = 0.0;
                if (p.size() >= 6 && !p[5].empty()) yaw = atof(p[5].c_str());
                else
                {
                    const SDK::FQuat q = SeStationUpQuat(at);
                    const SDK::FVector lx = SDK::UKismetMathLibrary::Quat_RotateVector(q, SDK::FVector{ 1, 0, 0 });
                    const SDK::FVector ly = SDK::UKismetMathLibrary::Quat_RotateVector(q, SDK::FVector{ 0, 1, 0 });
                    const double d[3] = { m->at[0] - door[0], m->at[1] - door[1], m->at[2] - door[2] };
                    yaw = atan2(d[0] * ly.X + d[1] * ly.Y + d[2] * ly.Z, d[0] * lx.X + d[1] * lx.Y + d[2] * lx.Z) * 57.29577951308232;
                }
                const SDK::FRotator dr = SDK::UKismetMathLibrary::ComposeRotators(SDK::FRotator{ 0.0, yaw, 0.0 }, SeStationUpRot(at));
                const double rot[3] = { dr.Pitch, dr.Yaw, dr.Roll }, scl[3] = { 1, 1, 1 };
                const std::string prevLoading = g_lvLoading;
                g_lvLoading = SeCallerScene();
                const std::string idx = SeSandboxSpawn("teamChange", door, rot, scl);
                g_lvLoading = prevLoading;
                if (!idx.empty())
                    if (SDK::AActor* a = SbActorForIdx(idx))
                    {
                        const std::string ident = SeLvIdent(a), ti = std::to_string(team - 1), mx = std::to_string(m->teamMax[team - 1]);
                        SeSbSet(nullptr, ident, "sd/TeamIndex", "num", ti);
                        SeSbSet(nullptr, ident, "gd/TeamIndex", "num", ti);
                        SeSbSet(nullptr, ident, "sd/TeamMaxSize", "num", mx);
                    }
            }
        }
    }
    else if (op == "CFGSET" && p.size() >= 5 && g_seLocalTest)                // SE|CFGSET|<slot id>|<name>|<value>
    {
        SDK::AActor* slot = nullptr;
        for (SDK::AActor* fs : g_seFabSlots)
            if (fs && SeAlive(fs) && FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(fs) + 0x390)) == p[2]) slot = fs;
        if (!slot) slot = SeMapSlotById(p[2]);
        const int r = SeModeConfigSet(slot, p[3], p[4]);
        HxLog("[HalcyonA2][SPECEDIT] CFGSET %s.%s = '%s': %d\n", p[2].c_str(), p[3].c_str(), p[4].c_str(), r);
    }
    else if (op == "PRESS" && p.size() >= 3 && g_seLocalTest)          // SE|PRESS|<idx>: a button press arriving at the server
    {
        // Exactly what a client's press sends (sub_1453A93A0): "<object idx>_ButtonComponent_OnPress_Server" through
        // the world's sandbox message subsystem (0x46B2E20) -> post (0x53C9EE0), which on the server dispatches
        // locally to whatever subscribed when the button started up (sub_1453913E0).
        const uintptr_t base = GetBase();
        SDK::UWorld* w = SDK::UWorld::GetWorld();
        void* sub = w ? reinterpret_cast<void*(__fastcall*)(void*)>(base + 0x46B2E20)(w) : nullptr;
        std::vector<std::string> ids;                        // an object idx, or a role name: every object with that role
        for (const auto& m : g_seModes)
            if (!m.deleted)
                for (const auto& kv : m.settings)
                    if (kv.first.rfind("role.", 0) == 0 && kv.second.rfind(p[2], 0) == 0) ids.push_back(kv.first.substr(5));
        if (ids.empty()) ids.push_back(p[2]);
        for (const auto& id : ids)
        {
            std::wstring msg(id.begin(), id.end());
            msg += L"_ButtonComponent_OnPress_Server";
            struct { const wchar_t* d; int32_t n; int32_t m; } fs{ msg.c_str(), static_cast<int32_t>(msg.size() + 1), static_cast<int32_t>(msg.size() + 1) };
            if (sub) reinterpret_cast<void(__fastcall*)(void*, void*)>(base + 0x53C9EE0)(sub, &fs);
            HxLog("[HalcyonA2][SPECEDIT] PRESS %s -> message subsystem %p (server %d)\n", id.c_str(), sub,
                  sub ? static_cast<int>(*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(sub) + 264)) : -1);
        }
    }
    else if (op == "LODFORCE" && p.size() >= 3 && g_seLocalTest) { g_lodForceOff = p[2] == "0"; HxLog("[HalcyonA2][SPECEDIT] area LOD force %s\n", g_lodForceOff ? "OFF" : "on"); }
    else if (op == "MKSLOTID" && p.size() >= 5 && g_seLocalTest) { double at[3]; const int ts[2] = { 4, 4 }; if (SeVec(p[2], at)) SbCreateSlotWithId(at, p[3], p[4], ts, 2); }
    else if (op == "TEAMCHK" && g_seLocalTest)            // every TeamChangeComponent: arena / team / ticket manager
    {
        auto* tcc = SDK::UObject::FindClassFast("TeamChangeComponent");
        const int32_t n = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; tcc && i < n; ++i)
        {
            SDK::UObject* c = SDK::UObject::GObjects->GetByIndex(i);
            if (!c || c->IsDefaultObject() || !c->IsA(tcc)) continue;
            const uintptr_t b = reinterpret_cast<uintptr_t>(c);
            SDK::UObject* owner = c->Outer;
            SDK::UObject* tm = *reinterpret_cast<SDK::UObject**>(b + 0x508);
            SDK::AActor* oa = static_cast<SDK::AActor*>(owner);
            const SDK::FVector l = oa ? oa->K2_GetActorLocation() : SDK::FVector{};
            SDK::UObject* pc = oa ? SbPrefabOf(oa) : nullptr;
            SDK::UObject* lgm = pc ? *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x440) : nullptr;
            HxLog("[HalcyonA2][SPECEDIT] TEAMCHK %s at (%.0f,%.0f,%.0f) gm=%s arena=%d team=%d size=%d max=%d tm=%s\n",
                  owner ? owner->GetName().c_str() : "?", l.X, l.Y, l.Z, lgm ? lgm->GetName().c_str() : "-",
                  *reinterpret_cast<int32_t*>(b + 0x4D8), *reinterpret_cast<int32_t*>(b + 0x524), *reinterpret_cast<int32_t*>(b + 0x4DC),
                  *reinterpret_cast<int32_t*>(b + 0x4E0), tm ? tm->GetName().c_str() : "none");
        }
    }
    else if (op == "GMVAR" && p.size() >= 4 && g_seLocalTest)                 // SE|GMVAR|<object>|<name> (local test)
    {
        std::string v;
        const bool ok = SeGamemodeVar(SeFindEditorActor(p[2]), p[3], &v);
        HxLog("[HalcyonA2][SPECEDIT] GMVAR %s.%s -> %s '%s'\n", p[2].c_str(), p[3].c_str(), ok ? "ok" : "MISSING", v.c_str());
    }
    else if (op == "LGMTREE") SeLgmTree();
    else if (op == "LGMDESC" && p.size() >= 3) SeLgmDesc(p[2]);
    else if (op == "LUAUDUMP") SeLuauDump(p.size() >= 3 ? p[2] : std::string());
    else if (op == "SLOTS") SeSlotsDump(p.size() >= 3 ? p[2] : std::string());
    else if (op == "SLOTBOX") SeSlotBoxProbe();
    else if (op == "PROJPROBE" && p.size() >= 3) SeProjProbe(p[2]);
    else if (op == "MKSLOT" && g_seLocalTest && p.size() >= 4) { double at[3]; if (SeVec(p[2], at)) SbCreateEditorSlot(at, p[3] == "-" ? std::string() : p[3]); }
    else if (op == "NODEMOVE" && p.size() >= 4) SeTestNodeMove(p[2], p[3]);
    else if (op == "ACTORS" && p.size() >= 3) SeActorClasses(p[2]);
    else if (op == "SBDATA" && p.size() >= 3) SeSbData(pawn, p[2]);
    else if (op == "SBSET"  && p.size() >= 6) SeSbSet(pawn, p[2], p[3], p[4], p[5]);
    else if (op == "SBTYPES") SeSandboxTypes();
    else if (op == "COINRUN" && p.size() >= 7) SeCoinRun(p[2], p[3], p[4], p[5], p[6], p.size() >= 8 ? p[7] : std::string(),
                                                     p.size() >= 9 ? p[8] : std::string());
    else if (op == "QUEST"  && p.size() >= 12)
        SeQuest(p[2], p[3], p[4], atoi(p[5].c_str()), p[6], atoi(p[7].c_str()), static_cast<float>(atof(p[8].c_str())), p[9],
                atof(p[10].c_str()), atoi(p[11].c_str()), p.size() >= 13 ? p[12] : std::string());
    else if (op == "QUEST"  && p.size() >= 10)
        SeQuest(p[2], p[3], p[4], atoi(p[5].c_str()), p[6], atoi(p[7].c_str()), static_cast<float>(atof(p[8].c_str())), p[9]);
    else if (op == "QUEST"  && p.size() >= 7) SeQuest(p[2], p[3], p[4], atoi(p[5].c_str()), p[6]);
    else if (op == "QUEST"  && p.size() >= 5) SeQuest(p[2], p[3], "", 0, p[4]);   // older client: no glyph/rep
    else if (op == "ENTER" || op == "EXIT")
    {
        HxLog("[HalcyonA2][SPECEDIT] editor %s\n", op.c_str());
        if (op == "ENTER")
        {
            // The caller's controller: the context is the controller itself (Vivox path) or a pawn.
            if (SDK::UObject* pc = SeCallerPC(pawn)) { SeReplayEdits(pc); SeOwnLocksPush(pc); }
            g_camExitAt.erase(g_seCaller);
        }
        else
        {
            g_camExitAt[g_seCaller] = GetTickCount64();
            if (auto m = g_camMarkers.find(g_seCaller); m != g_camMarkers.end()) { SeCamMarkerDrop(m->second); g_camMarkers.erase(m); }
        }
    }
    else HxLog("[HalcyonA2][SPECEDIT] unknown command\n");
    SeTreeCheck(op.c_str());          // after every edit: is the replicated tree still walkable?
    return true;
}

// ---- ProcessEvent glue ----------------------------------------------------------------------
// ProcessEvent_Hook must never build a string per dispatch (doing that once cost this project ~40fps
// and made the ball predict 30 frames ahead), so recognition is a single integer compare against the
// cached FName index of ALevelEditorPawn::Server_AttemptLockObject. Only once that matches do we touch
// the payload at all.
//
// TWO transports, because the editor pawn alone was a chicken-and-egg: Server_AttemptLockObject lives on
// ALevelEditorPawn, and a player on the station is never in one, so "Enter Level Editor" could not even
// be sent. AVRPlayerController::Server_SetVivoxParticipantID also takes a single FString, and every
// client owns its controller -- spectators included -- so that is the normal path. A real Vivox id never
// starts with "SE|", so voice keeps working untouched.
static int32_t g_seLockIdx     = 0;     // ALevelEditorPawn::Server_AttemptLockObject
static int32_t g_seVivoxIdx    = 0;     // AVRPlayerController::Server_SetVivoxParticipantID
static bool    g_seLockIdxDone = false;

static void ResolveSpecEditIndex()
{
    if (g_seLockIdxDone) return;
    static SDK::UClass* lep = nullptr;
    static SDK::UClass* vpc = nullptr;
    if (!lep) lep = SDK::UObject::FindClassFast("LevelEditorPawn");
    if (!vpc) vpc = SDK::UObject::FindClassFast("VRPlayerController");
    if (!lep || !vpc) return;               // not loaded yet -- retry next pass
    if (auto* f = lep->GetFunction("LevelEditorPawn", "Server_AttemptLockObject"))
        g_seLockIdx = f->Name.ComparisonIndex;
    if (auto* f = vpc->GetFunction("VRPlayerController", "Server_SetVivoxParticipantID"))
        g_seVivoxIdx = f->Name.ComparisonIndex;
    g_seLockIdxDone = true;
    HxLog("[HalcyonA2][SPECEDIT] transports: Server_AttemptLockObject idx=%d, Server_SetVivoxParticipantID idx=%d\n",
          g_seLockIdx, g_seVivoxIdx);
}

// Server_AttemptLockObject(const FString& idx): the params block is a single FString at offset 0,
// which FStringToNarrow reads by raw offset (Data@0, Num@8). Out-of-line because it holds std::string
// temporaries, which cannot live inside a __try frame (C2712) -- SafeSpecEditDispatch owns the SEH.
static bool SpecEditDispatch(SDK::UObject* pawn, void* parms)
{
    // [SEPROF] Editor commands run inside the RPC handler, where [PROF] never looks -- that is how full-object
    // walks per command (~30/s during a drag) went unseen while they stalled the tick and dropped ball-sim
    // frames (MI). One line every 5 s while anyone is editing: commands, game-thread ms, slowest command.
    static LARGE_INTEGER s_freq{};
    if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    const std::string cmd = FStringToNarrow(parms);
    const bool r = SpecEditHandle(pawn, cmd);
    QueryPerformanceCounter(&t1);
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(s_freq.QuadPart);
    static ULONGLONG s_window = 0;
    static int s_n = 0;
    static double s_ms = 0, s_max = 0;
    static std::string s_maxOp;
    ++s_n; s_ms += ms;
    if (ms > s_max) { s_max = ms; s_maxOp = cmd.substr(0, cmd.find('|', 3)); }
    const ULONGLONG now = GetTickCount64();
    if (!s_window) s_window = now;
    if (now - s_window >= 5000)
    {
        const double secs = (now - s_window) / 1000.0;
        HxLog("[HalcyonA2][SEPROF] cmds/s=%.1f editor=%.1fms/s max=%.1fms (%s)\n", s_n / secs, s_ms / secs, s_max, s_maxOp.c_str());
        s_window = now; s_n = 0; s_ms = 0; s_max = 0; s_maxOp.clear();
    }
    return r;
}

static bool SafeSpecEditDispatch(SDK::UObject* pawn, void* parms)
{
    __try { return SpecEditDispatch(pawn, parms); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        // A malformed command must never take the server down. Swallow it and keep serving; returning
        // true also means the half-parsed string is NOT handed on to the game's own lock handler.
        HxLog("[HalcyonA2][SPECEDIT] dispatch FAULTED -- command dropped\n");
        return true;
    }
}

// Read by QSendSet_Hook, which sits above this header in dllmain.cpp.
static bool g_specEditOn() { return g_specEdit; }
