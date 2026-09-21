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

// ---- state ---------------------------------------------------------------------------------
static bool       g_specEdit = false;          // -SpecEdit turns it on; see the note above
static SDK::FName g_seLockFnName{};
static bool       g_seLockFnResolved = false;
static ULONGLONG  g_seLastCmd = 0;
static int        g_seCmdsThisSecond = 0;

static std::vector<std::string> g_seEditors;   // allowlisted user ids
static FILETIME                 g_seEditorsStamp{};

struct SpecEditQuest
{
    uint32_t                 id[4]{};
    std::string              questId;
    std::string              title;
    std::vector<std::string> stepActors;
};
static std::vector<SpecEditQuest> g_seQuests;

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

// Is the player driving this pawn allowed to edit? Resolved through the pawn's owning controller, so a
// client cannot claim to be someone else - the identity comes from the server's own player state.
static bool SeAuthorised(SDK::UObject* pawn)
{
    const auto& allow = SeEditors();
    if (allow.empty()) return false;
    if (!pawn) return false;

    const std::string uid = ResolveUserIdForPawn(pawn);     // existing payload helper
    if (uid.empty()) return false;
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

static SDK::AActor* SeFindEditorActor(const std::string& name)
{
    auto* actorCls = SDK::UObject::FindClassFast("Actor");
    if (!actorCls) return nullptr;
    const int32_t n = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < n; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(actorCls)) continue;
        if (o->GetName() != name) continue;
        auto* c = o->Class;
        if (!c || !SeIsEditorClass(c->GetName())) return nullptr;   // fence 4: not ours, refuse
        return static_cast<SDK::AActor*>(o);
    }
    return nullptr;
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
    return SDK::UObject::FindClassFast(leaf);
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

    actor->bReplicates      = true;
    actor->bAlwaysRelevant  = true;     // an editor placement should reach everyone, not just nearby
    SDK::UGameplayStatics::FinishSpawningActor(actor, xf, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

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
    if (SeVec(locs, v)) a->K2_SetActorLocation(SDK::FVector{ v[0], v[1], v[2] }, false, nullptr, false);
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

// Authored quests are published into the bundle every player receives, through the same [PKRQUESTS]
// path that publishes definitions the station never registers - so a client that joins sees the quest
// and can play it. The id is derived from the quest name so recompiling updates in place instead of
// orphaning progress.
static void SeQuest(const std::string& questId, const std::string& title, const std::string& steps)
{
    SpecEditQuest q;
    q.questId = questId;
    q.title   = title;
    for (const auto& s : SeSplit(steps, ',', 64))
        if (!s.empty()) q.stepActors.push_back(s.substr(0, s.find(':')));

    uint64_t h = 1469598103934665603ULL;
    for (char c : questId) { h ^= static_cast<unsigned char>(c); h *= 1099511628211ULL; }
    uint64_t h2 = h;
    for (char c : title) { h2 ^= static_cast<unsigned char>(c); h2 *= 1099511628211ULL; }
    q.id[0] = static_cast<uint32_t>(h);
    q.id[1] = static_cast<uint32_t>(h >> 32);
    q.id[2] = static_cast<uint32_t>(h2);
    q.id[3] = static_cast<uint32_t>(h2 >> 32) | 1u;          // never all-zero

    for (auto& e : g_seQuests)
        if (e.questId == questId) { e = q; HxLog("[HalcyonA2][SPECEDIT] quest updated: %s\n", questId.c_str()); return; }
    g_seQuests.push_back(q);

    if (g_pubCount < kMaxPubQuests)
    {
        for (int c = 0; c < 4; ++c) g_pubIds[g_pubCount][c] = q.id[c];
        ++g_pubCount;
    }
    HxLog("[HalcyonA2][SPECEDIT] quest authored: %s '%s' %zu step(s) id=%08X%08X%08X%08X\n",
          questId.c_str(), title.c_str(), q.stepActors.size(), q.id[0], q.id[1], q.id[2], q.id[3]);
}

// ---- entry point ---------------------------------------------------------------------------
// Returns true when the string was ours and the original lock RPC should NOT run.
static bool SpecEditHandle(SDK::UObject* pawn, const std::wstring& cmdW)
{
    if (cmdW.size() < 4 || cmdW.size() > 1024) return false;          // fence 5: bounded
    std::string cmd(cmdW.begin(), cmdW.end());
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
    else if (op == "QUEST"  && p.size() >= 5) SeQuest(p[2], p[3], p[4]);
    else if (op == "ENTER" || op == "EXIT")   HxLog("[HalcyonA2][SPECEDIT] editor %s\n", op.c_str());
    else HxLog("[HalcyonA2][SPECEDIT] unknown command\n");
    return true;
}
