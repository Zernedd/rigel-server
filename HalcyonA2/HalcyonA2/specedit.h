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
static bool SeIsEditorClass(const std::string& className)
{
    return className.rfind("LE_", 0) == 0;
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

    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    if (!actorCls) return nullptr;

    SDK::AActor* best   = nullptr;
    double       bestD2 = 1.0e18;
    int          within = 0;
    const double tol2   = 100.0 * 100.0;                        // 1m, absorbs replication lag
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(actorCls)) continue;
        auto* c = o->Class;
        if (!c || c->GetName() != wantCls) continue;
        if (*(reinterpret_cast<const uint8_t*>(o) + 0x65) & 0x01) continue;   // bActorIsBeingDestroyed

        // RootComponent@0x1A8 -> ComponentToWorld@0x1D0, translation at +0x20
        void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1A8);
        if (!root) continue;
        const double* t = reinterpret_cast<const double*>(reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20);
        const double dx = t[0] - want[0], dy = t[1] - want[1], dz = t[2] - want[2];
        const double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > tol2) continue;
        ++within;
        if (d2 < bestD2) { bestD2 = d2; best = static_cast<SDK::AActor*>(o); }
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
static void SeSpawn(SDK::UObject* pawn, const std::string& path, const std::string& locs, const std::string& rots)
{
    double loc[3]{}, rot[3]{};
    if (!SeVec(locs, loc) || !SeVec(rots, rot)) { HxLog("[HalcyonA2][SPECEDIT] bad transform\n"); return; }

    SDK::UClass* cls = SeResolveEditorClass(path);
    if (!cls) return;

    SDK::FTransform xf{};
    xf.Rotation    = SDK::FQuat{ 0, 0, 0, 1 };
    xf.Translation = SDK::FVector{ loc[0], loc[1], loc[2] };
    xf.Scale3D     = SDK::FVector{ 1, 1, 1 };

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
    actor->SetReplicates(true);
    actor->SetReplicateMovement(true);
    actor->ForceNetUpdate();

    // Let the engine build the quaternion; hand-rolled rotator maths has already cost this project a
    // misplaced level once.
    actor->K2_SetActorRotation(SDK::FRotator{ rot[0], rot[1], rot[2] }, false);

    HxLog("[HalcyonA2][SPECEDIT] spawned %s -> %s at (%.0f,%.0f,%.0f) replicated\n",
          cls->GetName().c_str(), actor->GetName().c_str(), loc[0], loc[1], loc[2]);
}

static void SeTransform(const std::string& name, const std::string& locs, const std::string& rots,
                        const std::string& scls)
{
    SDK::AActor* a = SeFindEditorActor(name);
    if (!a) return;
    double v[3]{};
    SDK::FHitResult hit{};   // K2_SetActorLocation writes the sweep result back through this pointer
    if (SeVec(locs, v)) a->K2_SetActorLocation(SDK::FVector{ v[0], v[1], v[2] }, false, &hit, false);
    if (SeVec(rots, v)) a->K2_SetActorRotation(SDK::FRotator{ v[0], v[1], v[2] }, false);
    if (SeVec(scls, v)) a->SetActorScale3D(SDK::FVector{ v[0], v[1], v[2] });
}

static void SeDelete(const std::string& name)
{
    SDK::AActor* a = SeFindEditorActor(name);
    if (!a) return;
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
};
static std::deque<SeAuthoredQuest> g_seAuthored;     // deque: element addresses stay stable
static uint8_t  g_seTemplate[0x120];
static bool     g_seHaveTemplate = false;
static std::vector<SDK::UObject*> g_seQuestComps;    // every player quest component quests went to
static std::vector<uint8_t> g_seRowsBuf;
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
    g_seRowsBuf.assign(g_seAuthored.size() * 0x120, 0);
    int n = 0;
    for (const auto& q : g_seAuthored)
    {
        if (!*reinterpret_cast<const void* const*>(q.row + 0xF0)) continue;   // row not built yet
        memcpy(g_seRowsBuf.data() + static_cast<size_t>(n) * 0x120, q.row, 0x120);
        ++n;
    }
    if (!n) return;
    memset(g_seBundle, 0, sizeof(g_seBundle));
    *reinterpret_cast<const wchar_t**>(g_seBundle) = kSeBundleId;
    *reinterpret_cast<int32_t*>(g_seBundle + 8)  = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    *reinterpret_cast<int32_t*>(g_seBundle + 12) = static_cast<int32_t>(wcslen(kSeBundleId) + 1);
    SetTArray(g_seBundle + 0x20, g_seRowsBuf.data(), n);
    __try { QSendSet_Orig(comp, g_seBundle, 0); }
    __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][SPECEDIT] quest send FAULTED\n"); return; }
    HxLog("[HalcyonA2][SPECEDIT] sent %d authored quest(s) to comp=%p\n", n, comp);
}

// Called from QSendSet_Hook each time the game sends a player their real quests: remember the
// component, capture a template, and follow up with the authored bundle.
static void SeOnQuestsSent(void* comp, void* bundle)
{
    if (!comp) return;
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
static void SeQuest(const std::string& questId, const std::string& title, const std::string& glyph,
                    int repetition, const std::string& steps, int validSec = 0, float reqProgress = 0.0f,
                    const std::string& description = std::string())
{
    // The GUID comes from the quest id ALONE, so renaming or re-publishing the same quest updates it in
    // place instead of minting a new quest and orphaning everyone's progress on the old one.
    uint64_t h = 1469598103934665603ULL, h2 = 0x9E3779B97F4A7C15ULL;
    for (char c : questId) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
    for (char c : questId) { h2 ^= static_cast<unsigned char>(c); h2 *= 0x100000001B3ULL; h2 ^= h2 >> 29; }
    uint32_t id[4] = { static_cast<uint32_t>(h), static_cast<uint32_t>(h >> 32),
                       static_cast<uint32_t>(h2), static_cast<uint32_t>(h2 >> 32) | 1u };

    SeAuthoredQuest* q = nullptr;
    for (auto& e : g_seAuthored) if (e.questId == questId) { q = &e; break; }
    if (!q) { g_seAuthored.emplace_back(); q = &g_seAuthored.back(); }
    memcpy(q->id, id, sizeof(id));
    q->questId = questId;
    q->title.assign(title.begin(), title.end());
    q->desc = description.empty() ? q->title : std::wstring(description.begin(), description.end());
    q->glyph = glyph;
    q->rep = repetition;
    q->validSec = validSec < 0 ? 0 : validSec;
    q->reqProgress = reqProgress < 0 ? 0.0f : reqProgress;

    const bool built = SeBuildRow(*q, glyph, repetition);

    int bound = 0, missing = 0;
    q->steps = 0;
    for (const auto& s : SeSplit(steps, ';', 64))
    {
        if (s.empty()) continue;
        ++q->steps;
        SDK::AActor* a = SeFindEditorActor(s.substr(0, s.find(':')));
        if (a) bound += SeBindStep(a, *q); else ++missing;
    }

    // Everyone gets it now, not just players who join later.
    int sent = 0;
    for (auto* c : g_seQuestComps)
        if (SeAlive(c)) { SeSendAuthored(c); ++sent; }

    HxLog("[HalcyonA2][SPECEDIT] quest %s '%s' id=%08X%08X%08X%08X: row %s, %d step(s), %d progress component(s) bound, "
          "%d step actor(s) not found, pushed to %d player(s)\n",
          questId.c_str(), title.c_str(), id[0], id[1], id[2], id[3],
          built ? "built" : "WAITING FOR TEMPLATE (no player has received quests yet)",
          q->steps, bound, missing, sent);
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

static bool SeActorAlive(SDK::AActor* a)
{
    if (!a) return false;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
        if (SDK::UObject::GObjects->GetByIndex(i) == a)
            return !(*(reinterpret_cast<const uint8_t*>(a) + 0x65) & 0x01);
    return false;
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
    const int32_t count = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < count; ++i)
    {
        SDK::UObject* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pcCls)) continue;
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

    const auto p = SeSplit(cmd, '|', 16);
    const std::string op = p.size() > 1 ? p[1] : std::string();

    if      (op == "SPAWN"  && p.size() >= 5) SeSpawn(pawn, p[2], p[3], p[4]);
    else if (op == "XFORM"  && p.size() >= 6) SeTransform(p[2], p[3], p[4], p[5]);
    else if (op == "DELETE" && p.size() >= 3) SeDelete(p[2]);
    else if (op == "PROP"   && p.size() >= 5) SeSetProp(p[2], p[3], p[4]);
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
            SDK::UObject* pc = pawn;
            if (pc && !pc->IsA(SDK::APlayerController::StaticClass()))
                pc = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pc) + 0x2D0);   // APawn::Controller
            if (pc && pc->IsA(SDK::APlayerController::StaticClass())) SeReplayEdits(pc);
        }
    }
    else HxLog("[HalcyonA2][SPECEDIT] unknown command\n");
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
    return SpecEditHandle(pawn, FStringToNarrow(parms));
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
