// A2PlayerControl - a UE4SS native mod for A2 / Orion Drift (build 22284, "Nov15").
//
// WHAT IT DOES
//   F1          leave spectator: ASpectatorCameraManagerPawn::Server_ExitSpectator(pc)
//   WASD        fly the player around (force based - hold to accelerate, release to coast)
//   Space/Ctrl  up / down
//   Shift       boost (3x acceleration and speed cap)
//   F2          toggle the WASD system off/on (leave it off if a real headset is driving the pawn)
//
// WHY IT IS BUILT THIS WAY
//
// 1. Movement has to reach the SERVER, or it is not worth anything for replication testing.
//    On a client, AActor::K2_SetActorLocation is cosmetic: the server owns the pawn and will
//    just correct it. A real VR client feeds its transform to the server every tick through
//    UA2PlayerEntity::Server_SetFrequentData(FReplicatedFrequentData), so that is what this
//    mod drives. It copies the entity's own FrequentDataReplicationOnly (@0xF0, 0x110 bytes)
//    as a template - so every field we do not model keeps a valid value - then overwrites the
//    root and both hand positions, the baseVelocity, and the thruster/boost flags. That is the
//    same path a real player's hands and thrusters drive, which is exactly why a ball hit or a
//    position change made this way replicates to other clients like a genuine one.
//
// 2. Everything that touches a UObject runs on the GAME THREAD. UE4SS starts mods on its own
//    thread and calling ProcessEvent from there is a good way to kill the process (the Vivox
//    login RPC does exactly that). So the mod installs a MinHook on UObject::ProcessEvent and
//    uses it purely as a game-thread pump: every call, if enough time has passed, run one tick
//    and return. Keys are sampled with GetAsyncKeyState, which is safe from any thread.
//
// 3. It exports start_mod/uninstall_mod and returns nullptr from start_mod, the same trick
//    A2EntitlementPatch uses: UE4SS records the mod as "not started" and never calls back into
//    the DLL, so nothing here has to match the UE4SS C++ ABI and a UE4SS update cannot break it.
//    The mod's own worker thread does all the work.
//
// The offsets below were verified against A2-Win64-Shipping.exe build 22284 by decompiling the
// functions in IDA; they are build specific, like everything else in this repo.

#include "pch.h"
#include <MinHook.h>
#include <map>

// ---------------------------------------------------------------------------------------------
// Build-specific offsets (22284 / Nov15).
// ---------------------------------------------------------------------------------------------
//  UObject::ProcessEvent - used only as a game-thread pump. Taken from the generated SDK
//  (SDK::Offsets::ProcessEvent) rather than hardcoded, so it tracks whatever build the SDK
//  under gamesdk\<GameBuild>\ was dumped from.
//  UA2PlayerEntity::Pawn                        (AVRPawn*)
// Quest culling state of one sandbox prefab (UPrefabComponent): ULoadedGameMode::Enter/ExitImportanceVolume set
// byte +0x19 of entry [prefab+0x394] in the table at *(*(lgm+0x120)+0xD8)+0x28 (56-byte entries) for every prefab in
// lgm.loadedPrefabs (+0x2C0). A prefab whose flag is 0 while the player stands among them is hidden on Quest
// (HideOutsideImportanceVolume); the PC spec build ignores it. -1 = unreadable.
// The sandbox's per-prefab culling entry (56 bytes) in the manager at *(USandboxEngine+0xD8): entries @+0x28,
// count @+0x30, index = prefab+0x394. Rule (sub_1446CCF00): visible if engine+0x305 || (engine+0x303 && +0x304)
// [the PC shortcut]; else per entry: +26 forced hidden, +29 always, +28 ?, +32/+36 draw distances,
// then visible iff +25 (inside the importance volume) || !+53 (HideOutsideImportanceVolume off). +24 = last result.
static bool PrefabCullEntry(uintptr_t prefab, char* out, size_t n)
{
    __try
    {
        const uintptr_t lgm = *reinterpret_cast<uintptr_t*>(prefab + 0x440);
        if (!lgm) return false;
        const uintptr_t se = *reinterpret_cast<uintptr_t*>(lgm + 0x120);
        const uintptr_t mgr = se ? *reinterpret_cast<uintptr_t*>(se + 0xD8) : 0;
        if (!mgr) return false;
        const int idx = *reinterpret_cast<int*>(prefab + 0x394);
        const int cnt = *reinterpret_cast<int*>(mgr + 0x30);
        if (idx < 0 || idx >= cnt) { snprintf(out, n, "idx=%d/%d", idx, cnt); return true; }
        const uint8_t* e = *reinterpret_cast<uint8_t**>(mgr + 0x28) + 56 * static_cast<size_t>(idx);
        snprintf(out, n, "cull[vis=%d imp=%d forceHid=%d b28=%d always=%d maxD=%.0f showD=%.0f hideOutside=%d]",
                 e[24], e[25], e[26], e[28], e[29], *reinterpret_cast<const float*>(e + 32),
                 *reinterpret_cast<const float*>(e + 36), e[53]);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void Log(const char* fmt, ...);   // below
// questcull: the USandboxEngine's "PC shortcut" flags (+0x303/+0x304/+0x305) that make every sandbox object visible.
static void QuestCull(int mode)   // -1 = report only, 0 = restore, 1 = clear (cull like Quest)
{
    static uint8_t saved[3] = { 0xFF, 0xFF, 0xFF };
    auto* cls = SDK::UObject::FindClassFast("SandboxEngine");
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; cls && i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        uint8_t* b = reinterpret_cast<uint8_t*>(o) + 0x303;
        Log("[A2PlayerControl][QCULL] %s +0x303=%d +0x304=%d +0x305=%d (auth=%d)\n", o->GetName().c_str(), b[0], b[1], b[2],
            *(reinterpret_cast<uint8_t*>(o) + 0x300));
        if (mode == 1) { if (saved[0] == 0xFF) memcpy(saved, b, 3); b[0] = b[1] = b[2] = 0; Log("[A2PlayerControl][QCULL] cleared -> Quest culling\n"); }
        if (mode == 0 && saved[0] != 0xFF) { memcpy(b, saved, 3); Log("[A2PlayerControl][QCULL] restored\n"); }
    }
}
// A monitor's team-size source (what flips it between its pages): PrefabComponent(+0x2E8) +0x440 -> +0x320 -> +0x438,
// int array @+0x3E0. Raw reads, fenced.
static void BoardSizeChain(uintptr_t board, uintptr_t hops[3], int32_t sizes[8], int* n)
{
    __try
    {
        uintptr_t p = *reinterpret_cast<uintptr_t*>(board + 0x2E8);
        const int offs[3] = { 0x440, 0x320, 0x438 };
        for (int k = 0; k < 3 && p; ++k) hops[k] = p = *reinterpret_cast<uintptr_t*>(p + offs[k]);
        if (!p) return;
        const int32_t* d = *reinterpret_cast<int32_t**>(p + 0x3E0);
        const int32_t cnt = *reinterpret_cast<int32_t*>(p + 0x3E8);
        *n = 0;
        for (int32_t k = 0; d && k < cnt && k < 8; ++k) sizes[(*n)++] = d[k];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static int PrefabImportance(uintptr_t prefab, uintptr_t* lgmOut, int* inLoaded)
{
    __try
    {
        const uintptr_t lgm = *reinterpret_cast<uintptr_t*>(prefab + 0x440);
        *lgmOut = lgm;
        if (!lgm) return -2;
        const int idx = *reinterpret_cast<int*>(prefab + 0x394);
        const uintptr_t* arr = *reinterpret_cast<uintptr_t**>(lgm + 0x2C0);
        const int n = *reinterpret_cast<int*>(lgm + 0x2C8);
        *inLoaded = 0;
        for (int i = 0; i < n && i < 100000; ++i) if (arr[i] == prefab) { *inLoaded = 1; break; }
        if (idx < 0) return -3;
        const uintptr_t sys = *reinterpret_cast<uintptr_t*>(lgm + 0x120);
        const uintptr_t tbl = *reinterpret_cast<uintptr_t*>(sys + 0xD8);
        const uint8_t* ent = *reinterpret_cast<uint8_t**>(tbl + 0x28);
        return ent[56 * static_cast<size_t>(idx) + 25];
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
static constexpr uintptr_t kPlayerEntityPawnOff = 0xE8;
//  UA2PlayerEntity::FrequentDataReplicationOnly (FReplicatedFrequentData, 0x110 bytes)
static constexpr uintptr_t kPlayerEntityFreqOff = 0xF0;
//  Field offsets inside FReplicatedFrequentData.
static constexpr size_t kFreqRootPos   = 0x10;   // Root.position
static constexpr size_t kFreqLeftPos   = 0x58;   // leftHand.position
static constexpr size_t kFreqRightPos  = 0x88;   // rightHand.position
static constexpr size_t kFreqBaseVel   = 0xB8;   // baseVelocity
static constexpr size_t kFreqLThruster = 0xD1;
static constexpr size_t kFreqRThruster = 0xD2;
static constexpr size_t kFreqBigBoost  = 0xD6;
static constexpr size_t kFreqSize      = 0x110;

// ---------------------------------------------------------------------------------------------
// Tunables.
// ---------------------------------------------------------------------------------------------
// [VELMOVE] How an A2 player actually moves - established the hard way, two wrong turns first:
//
//   1. Setting the transform (K2_SetActorLocation) every tick at 90 Hz fought both the pawn's own
//      movement and the server's corrections: jittery, no collision, felt awful.
//   2. UPrimitiveComponent::AddForce on AVRPawn::rootCollision (@0x5D8) did nothing at all - the
//      mod's own log showed `sim=0`, i.e. that sphere is NOT Chaos-simulated on the client. A2
//      moves the pawn in code, not with rigid-body physics.
//
// The pawn exposes its own movement API, and that is the right lever:
//     AVRPawn::GetVRPawnVelocity() -> FVector
//     AVRPawn::SetVelocity(FVector vel, bool ignoreLimit)
// Reading the current velocity, adding acceleration to it and setting it back means the game's own
// movement code integrates the result, applies its own speed limit (ignoreLimit=false), handles
// collision, and replicates it exactly as it does for a thrusting VR player. WASD is now a real
// force applied through the game's own system rather than anything imposed from outside.
static constexpr double kAccel     = 2200.0;   // uu/s^2 added to the pawn's velocity while held
static constexpr double kBoostMul  = 2.5;      // Shift
static constexpr double kBrake     = 2.0;      // per second, bleeds speed when nothing is held
static constexpr int    kTickMs    = 11;       // ~90 Hz, matching the game's own tick rate

// ---------------------------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void Log(const char* fmt, ...)
{
    if (!g_log)
    {
        wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
        wchar_t path[MAX_PATH]{}; swprintf_s(path, L"%sA2PlayerControl.log", tmp);
        g_log = _wfsopen(path, L"a", _SH_DENYNO);
    }
    va_list ap; va_start(ap, fmt);
    if (g_log) { vfprintf(g_log, fmt, ap); fflush(g_log); }
    va_end(ap);
    va_list ap2; va_start(ap2, fmt);
    vprintf(fmt, ap2);
    va_end(ap2);
}

static uintptr_t GetBase()
{
    static uintptr_t b = 0;
    if (!b) b = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    return b;
}

static bool Down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Rising edge for the action keys, so one press is one action.
static bool Pressed(int vk, bool& prev)
{
    const bool now = Down(vk);
    const bool edge = now && !prev;
    prev = now;
    return edge;
}

// ---------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------
// [WASDTOGGLE] OFF by default. A real PCVR player moves with the headset/thrusters; the WASD
// system's per-tick velocity + idle BRAKE fought that movement and made real players feel slow.
// Enabled only with -HalcyonWASD (the headless desktop test clients pass it) or F2 in-game.
static bool          g_wasdEnabled = false;
static SDK::FVector    g_cmdTarget{};                 // [CMDFILE] where the test command file sends the pawn
static bool          g_cmdActive = false;
static bool          g_cmdThrow = false;                // [CMDFILE] "throw": the F4 throw, once
static constexpr uintptr_t kRootCollisionOff = 0x5D8;   // AVRPawn::rootCollision (USphereComponent*)
static bool          g_prevF1 = false, g_prevF2 = false;
static ULONGLONG     g_lastTickMs = 0;
static bool          g_banner = false;

// ---------------------------------------------------------------------------------------------
// Find the UA2PlayerEntity that belongs to our pawn (entity->Pawn @0xE8).
// ---------------------------------------------------------------------------------------------
static SDK::UObject* FindMyPlayerEntity(SDK::APawn* pawn)
{
    auto* cls = SDK::UObject::FindClassFast("A2PlayerEntity");
    if (!cls || !pawn) return nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        auto* owner = *reinterpret_cast<SDK::APawn**>(reinterpret_cast<uintptr_t>(o) + kPlayerEntityPawnOff);
        if (owner == pawn) return o;
    }
    return nullptr;
}

static SDK::FVector g_lastHandL, g_lastHandR;   // [HANDDRIVE] defined below

// Push position + velocity to the server over the same channel a real client uses.
static bool PushToServer(SDK::APawn* pawn, const SDK::FVector& pos, const SDK::FVector& vel, bool thrusting)
{
    SDK::UObject* ent = FindMyPlayerEntity(pawn);
    if (!ent) return false;
    auto* fn = ent->Class ? ent->Class->GetFunction("A2PlayerEntity", "Server_SetFrequentData") : nullptr;
    if (!fn) return false;

    alignas(16) uint8_t buf[kFreqSize];
    memcpy(buf, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ent) + kPlayerEntityFreqOff), sizeof(buf));

    auto put = [&](size_t off, const SDK::FVector& v) {
        auto* d = reinterpret_cast<SDK::FVector*>(buf + off);
        d->X = v.X; d->Y = v.Y; d->Z = v.Z;
    };
    put(kFreqRootPos,  pos);
    // [HANDDRIVE] the server validates hits against where IT thinks the hands are, so send the
    // positions we actually swept them to, not the pawn root.
    put(kFreqLeftPos,  (g_lastHandL.X || g_lastHandL.Y || g_lastHandL.Z) ? g_lastHandL : pos);
    put(kFreqRightPos, (g_lastHandR.X || g_lastHandR.Y || g_lastHandR.Z) ? g_lastHandR : pos);
    put(kFreqBaseVel,  vel);
    buf[kFreqLThruster] = thrusting ? 1 : 0;
    buf[kFreqRThruster] = thrusting ? 1 : 0;
    buf[kFreqBigBoost]  = (thrusting && Down(VK_SHIFT)) ? 1 : 0;

    ent->ProcessEvent(fn, buf);
    return true;
}

// ---------------------------------------------------------------------------------------------
// F1: leave spectator. The RPC must be sent on the pawn THIS PlayerController owns - a Server_
// RPC on an actor the local connection does not own is dropped by the net driver, which is why
// "any spectator pawn in GObjects" silently does nothing.
// ---------------------------------------------------------------------------------------------
static void ExitSpectator(SDK::APlayerController* pc)
{
    auto* specCls = SDK::UObject::FindClassFast("SpectatorCameraManagerPawn");
    if (!specCls) { Log("[A2PlayerControl] SpectatorCameraManagerPawn class not found\n"); return; }

    SDK::UObject* spec = nullptr;
    if (pc->Pawn && pc->Pawn->IsA(specCls)) spec = pc->Pawn;
    if (!spec)
    {
        const int32_t num = SDK::UObject::GObjects->Num();
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject() || !o->IsA(specCls)) continue;
            if (static_cast<SDK::AActor*>(o)->Owner == pc) { spec = o; break; }
        }
    }
    if (!spec)
    {
        Log("[A2PlayerControl] F1: no SpectatorCameraManagerPawn owned by this PlayerController "
            "(current pawn: %s)\n", pc->Pawn ? pc->Pawn->GetName().c_str() : "<none>");
        return;
    }
    auto* fn = spec->Class ? spec->Class->GetFunction("SpectatorCameraManagerPawn", "Server_ExitSpectator") : nullptr;
    if (!fn) { Log("[A2PlayerControl] F1: Server_ExitSpectator not found on %s\n", spec->GetName().c_str()); return; }

    struct { SDK::APlayerController* PlayerController; } p{ pc };
    spec->ProcessEvent(fn, &p);
    Log("[A2PlayerControl] F1: sent Server_ExitSpectator on %s\n", spec->GetName().c_str());
}


// =============================================================================================
// [BALLKEYS] Ball actions, so replication can be checked with two windows open.
//
//   F3  spawn a ball in front of you, already moving (AVRPawn::Server_SpawnBall). The server
//       creates it and replicates it, so it appears on EVERY connected client - the simplest
//       unambiguous "did the other client get it?" test.
//   F4  throw the nearest arena ball (AVRPawn::Server_HitProp + Server_SendPhysicsPropData).
//
// F4 follows the sequence the server actually requires, which was established by decompiling
// AVRPawn::Server_HitProp_Implementation (RVA 0x55027F0):
//   - the ball's UA2PhysicsSync (ball+0x4F0) has owningActor at +0xC8
//   - the FIRST accepted HitProp only CLAIMS ownership; it does not move anything
//   - Server_SendPhysicsPropData (RVA 0x5503050) is accepted ONLY while owningActor == sender
//   - the server RECLAIMS ownership again within a fraction of a second unless the owner keeps
//     asserting itself, so the claim has to be re-sent continuously while streaming
// Hence the throw is a state machine spread over ticks (~90 Hz), not a blocking loop: this runs
// on the GAME THREAD via the ProcessEvent pump, so it must never block.
// =============================================================================================
static constexpr uintptr_t kBallPhysSyncOff = 0x4F0;   // AActor -> UA2PhysicsSync
static constexpr uintptr_t kSyncOwnerOff    = 0xC8;    // PhysicsSyncRepData.owningActor
static constexpr uintptr_t kSyncRepDataOff  = 0xC0;    // FReplicatedPhysicsObjectData (0x88)
static constexpr uintptr_t kSyncTimestamp   = 0x160;   // the clock HitProp compares against
static constexpr size_t    kRepDataSize     = 0x88;

static bool         g_prevF3 = false, g_prevF4 = false, g_prevF5 = false;
static SDK::AActor* g_throwBall  = nullptr;
static int          g_throwTick  = -1;      // -1 = idle
static SDK::FVector g_throwStart{};
static int          g_throwOwned = 0;

// Nearest ball of EXACTLY class BP_JakeBall_C that is not parked in the pooling manager.
// (BP_TackleballTrainingBall_C etc. derive from it and sit as ~50 stacked inactive actors.)
static SDK::AActor* NearestArenaBall(const SDK::FVector& from, double& outDist)
{
    auto* cls = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!cls) return nullptr;
    SDK::AActor* best = nullptr; double bestD = 1e30;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || o->Class != cls) continue;
        auto* a = static_cast<SDK::AActor*>(o);
        if (a->Owner && a->Owner->GetName().find("PoolingManager") != std::string::npos) continue;
        const SDK::FVector p = a->K2_GetActorLocation();
        const double dx = p.X - from.X, dy = p.Y - from.Y, dz = p.Z - from.Z;
        const double d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = a; }
    }
    outDist = best ? sqrt(bestD) : 0.0;
    return best;
}

static void SpawnBallInFront(SDK::APawn* pawn, const SDK::FVector& fwd)
{
    auto* fn = pawn->Class ? pawn->Class->GetFunction("VRPawn", "Server_SpawnBall") : nullptr;
    if (!fn) { Log("[A2PlayerControl] F3: Server_SpawnBall not found\n"); return; }
    double d = 0.0;
    SDK::AActor* ref = NearestArenaBall(pawn->K2_GetActorLocation(), d);
    SDK::UClass* ballCls = ref ? ref->Class : SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!ballCls) { Log("[A2PlayerControl] F3: no BP_JakeBall_C class\n"); return; }

    SDK::FVector loc = pawn->K2_GetActorLocation();
    loc.X += fwd.X * 200.0; loc.Y += fwd.Y * 200.0; loc.Z += fwd.Z * 200.0 + 100.0;
    const SDK::FVector vel{ fwd.X * 900.0, fwd.Y * 900.0, fwd.Z * 900.0 + 250.0 };

    struct { SDK::UClass* BallClass; SDK::FVector Location; SDK::FVector StartingVelocity; SDK::FVector AngularVelocity; }
        p{ ballCls, loc, vel, SDK::FVector{ 0.0, 0.0, 2.0 } };
    pawn->ProcessEvent(fn, &p);
    Log("[A2PlayerControl] F3: Server_SpawnBall at (%.0f, %.0f, %.0f) vel=(%.0f, %.0f, %.0f) "
        "- this ball should appear on EVERY connected client\n",
        loc.X, loc.Y, loc.Z, vel.X, vel.Y, vel.Z);
}

static void ThrowTick(SDK::APawn* pawn)
{
    if (g_throwTick < 0 || !g_throwBall) return;
    auto* fnHit  = pawn->Class ? pawn->Class->GetFunction("VRPawn", "Server_HitProp") : nullptr;
    auto* fnSend = pawn->Class ? pawn->Class->GetFunction("VRPawn", "Server_SendPhysicsPropData") : nullptr;
    void* psync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_throwBall) + kBallPhysSyncOff);
    if (!fnHit || !fnSend || !psync) { g_throwTick = -1; return; }

    const double syncTs = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(psync) + kSyncTimestamp);
    auto* world = SDK::UWorld::GetWorld();
    const double localT = world ? SDK::UGameplayStatics::GetTimeSeconds(world) : 0.0;
    const double ts = (syncTs > localT ? syncTs : localT) + 0.5;

    const double t = g_throwTick / 90.0;
    SDK::FVector np{ g_throwStart.X + 1100.0 * t, g_throwStart.Y, g_throwStart.Z + 250.0 * t };
    const SDK::FVector vel{ 1100.0, 0.0, 250.0 };

    PushToServer(pawn, np, vel, true);                       // stay on the ball, moving

    if ((g_throwTick % 20) == 0)                             // keep re-claiming: the server reclaims fast
    {
        struct { SDK::AActor* Actor; SDK::AActor* prev; double lastSeen; double Timestamp;
                 SDK::FVector position; SDK::FVector Force; }
            hp{ g_throwBall, nullptr, ts, ts, np, SDK::FVector{ 400000.0, 0.0, 60000.0 } };
        pawn->ProcessEvent(fnHit, &hp);
    }

    struct SendParams { SDK::AActor* Actor; uint8_t data[kRepDataSize]; } sp{};
    sp.Actor = g_throwBall;
    memcpy(sp.data, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(psync) + kSyncRepDataOff), kRepDataSize);
    *reinterpret_cast<double*>(sp.data + 0x00) = ts;
    *reinterpret_cast<SDK::AActor**>(sp.data + 0x08) = pawn;
    sp.data[0x10] = 0;
    *reinterpret_cast<SDK::FVector*>(sp.data + 0x18) = np;
    *reinterpret_cast<SDK::FVector*>(sp.data + 0x48) = vel;
    sp.data[0x80] = 1; sp.data[0x81] = 1;
    pawn->ProcessEvent(fnSend, &sp);

    auto* own = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(psync) + kSyncOwnerOff);
    if (own == static_cast<SDK::AActor*>(pawn)) ++g_throwOwned;

    if ((g_throwTick % 90) == 0)
    {
        const SDK::FVector cur = g_throwBall->K2_GetActorLocation();
        Log("[A2PlayerControl] F4: t=%d want=(%.0f, %.0f, %.0f) ball=(%.0f, %.0f, %.0f) owner=%s\n",
            g_throwTick, np.X, np.Y, np.Z, cur.X, cur.Y, cur.Z,
            own ? own->GetName().c_str() : "<null>");
    }

    if (++g_throwTick > 270)                                  // ~3 s
    {
        const SDK::FVector fin = g_throwBall->K2_GetActorLocation();
        Log("[A2PlayerControl] F4: done. ownedFrames=%d start=(%.0f, %.0f, %.0f) end=(%.0f, %.0f, %.0f)\n",
            g_throwOwned, g_throwStart.X, g_throwStart.Y, g_throwStart.Z, fin.X, fin.Y, fin.Z);
        g_throwTick = -1; g_throwBall = nullptr;
    }
}


// [VELMOVE] The pawn's own velocity accessors (UFunctions on AVRPawn).
static SDK::FVector GetPawnVelocity(SDK::APawn* pawn)
{
    auto* fn = pawn->Class ? pawn->Class->GetFunction("VRPawn", "GetVRPawnVelocity") : nullptr;
    if (!fn) return SDK::FVector{};
    struct { SDK::FVector ReturnValue; } p{};
    pawn->ProcessEvent(fn, &p);
    return p.ReturnValue;
}

static bool SetPawnVelocity(SDK::APawn* pawn, const SDK::FVector& v, bool ignoreLimit)
{
    auto* fn = pawn->Class ? pawn->Class->GetFunction("VRPawn", "SetVelocity") : nullptr;
    if (!fn) return false;
    struct { SDK::FVector vel; bool ignoreLimit; uint8_t pad[7]; } p{ v, ignoreLimit, {} };
    pawn->ProcessEvent(fn, &p);
    return true;
}


// =============================================================================================
// [HANDDRIVE] Why "ram my hands into the ball" did nothing.
//
// A2 does not register a ball hit from the player's BODY. The hit comes from the HAND collision
// spheres: BP_VRPawn_C binds ComponentHit on leftHandCollision / rightHandCollision, and that
// event is what calls AVRPawn::Server_PlayerHitObject(OtherActor, OtherComp). Those two spheres
// are driven by the tracked controllers.
//
// Under -nohmd there are no controllers, so the hands never move - they sit at their default spot
// on the pawn and sweep through nothing. Flying the pawn into a ball therefore cannot produce a
// hit no matter how accurately the body is positioned, and the spectator sees nothing because no
// hit ever happened to replicate.
//
// So the mod has to drive the hands too:
//   - force enableHandCollision (@0xA60) on, and make sure the spheres actually collide and
//     report hits (SetCollisionEnabled / SetNotifyRigidBodyCollision / overlap events)
//   - each tick, SWEEP the spheres to a spot just in front of the view. K2_SetWorldLocation with
//     bSweep=true is the important part: a swept move generates the hit/overlap events the
//     Blueprint is bound to, whereas a teleport would silently pass through the ball.
//   - report the same hand positions to the server in FrequentData, because the server is what
//     ultimately validates a hit and it must agree about where the hands are.
// =============================================================================================
static constexpr uintptr_t kRightHandCollOff = 0x5F8;   // AVRPawn::rightHandCollision
static constexpr uintptr_t kLeftHandCollOff  = 0x600;   // AVRPawn::leftHandCollision
static constexpr uintptr_t kEnableHandCollOff = 0xA60;  // AVRPawn::enableHandCollision (bool)
// [HANDSRC] The SOURCE transforms the game drives the hands from (normally the tracked
// controllers). The collision spheres are moved to follow these by the pawn's own
// leftHandMoveUtil / rightHandMoveUtil (FSafeMoveUtil @0xFD0/0xFE8) - which is the code path that
// produces proper swept hits and the OnLeft/RightHandBeginOverlap events.
static constexpr uintptr_t kLeftHandLocalOff  = 0x5E8;  // AVRPawn::LeftHandLocal  (USceneComponent*)
static constexpr uintptr_t kRightHandLocalOff = 0x5F0;  // AVRPawn::RightHandLocal (USceneComponent*)

static bool g_handsEnabled = true;    // F5 toggles
static bool g_handSetupDone = false;

static void EnsureHandCollision(SDK::APawn* pawn, SDK::UPrimitiveComponent* l, SDK::UPrimitiveComponent* r)
{
    if (g_handSetupDone) return;
    g_handSetupDone = true;
    *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(pawn) + kEnableHandCollOff) = true;
    for (auto* c : { l, r })
    {
        if (!c) continue;
        c->SetCollisionEnabled(SDK::ECollisionEnabled::QueryAndPhysics);
        c->SetGenerateOverlapEvents(true);
        c->SetNotifyRigidBodyCollision(true);      // "Simulation Generates Hit Events"
    }
    Log("[A2PlayerControl][HANDS] collision enabled on both hand spheres (enableHandCollision=1)\n");
}

// [HANDSRC] Drive the hands from their SOURCE transforms.
//
// The previous attempt swept leftHandCollision / rightHandCollision straight to a world position.
// The mod's own log showed why that failed: the spheres came back reading only ~3 units from the
// pawn instead of the 70 they were sent to, because the pawn re-derives those collision spheres
// from LeftHandLocal / RightHandLocal every tick and simply overwrote the write. Occasionally one
// frame stuck (the log has a couple of lines at the intended +70 offset), which is exactly what a
// losing fight over the same value looks like.
//
// So write the source instead and let the pawn's own leftHandMoveUtil / rightHandMoveUtil carry it
// to the collision spheres - the same route a tracked controller takes, which is what generates
// the swept hit and the OnLeft/RightHandBeginOverlap events the ball logic listens for.
// [REACH] "reach x y z [seconds]": sweep the RIGHT hand to a world point (a button, a switch) and hold it there, so
// the object gets the same hand overlap a real player's press makes. The hand position also goes to the server in
// FrequentData, like tracked hands.
static bool        g_reachActive = false;
static SDK::FVector g_reachTarget{};
static ULONGLONG   g_reachUntil = 0;
static void DriveHands(SDK::APawn* pawn, const SDK::FVector& fwd, const SDK::FVector& right, double dt)
{
    auto* rl = *reinterpret_cast<SDK::USceneComponent**>(reinterpret_cast<uintptr_t>(pawn) + kRightHandLocalOff);
    auto* ll = *reinterpret_cast<SDK::USceneComponent**>(reinterpret_cast<uintptr_t>(pawn) + kLeftHandLocalOff);
    auto* rh = *reinterpret_cast<SDK::UPrimitiveComponent**>(reinterpret_cast<uintptr_t>(pawn) + kRightHandCollOff);
    auto* lh = *reinterpret_cast<SDK::UPrimitiveComponent**>(reinterpret_cast<uintptr_t>(pawn) + kLeftHandCollOff);
    if (rh && lh) EnsureHandCollision(pawn, lh, rh);
    if (!rl || !ll) return;

    // These are the LOCAL hand transforms, so position them in the pawn's own space: X forward,
    // Y right, Z up. Out in front and slightly apart, roughly where a player holds their hands.
    const double reach = 70.0, spread = 32.0, lift = -10.0;
    SDK::FHitResult h1{}, h2{};
    if (g_reachActive && GetTickCount64() > g_reachUntil) g_reachActive = false;
    if (g_reachActive)
    {
        // step toward the target (a swept move per tick) so the overlap is a real sweep, not a teleport
        const SDK::FVector cur = rl->K2_GetComponentLocation();
        SDK::FVector d{ g_reachTarget.X - cur.X, g_reachTarget.Y - cur.Y, g_reachTarget.Z - cur.Z };
        const double len = sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z), step = 25.0;
        const SDK::FVector to = len <= step ? g_reachTarget : SDK::FVector{ cur.X + d.X / len * step, cur.Y + d.Y / len * step, cur.Z + d.Z / len * step };
        rl->K2_SetWorldLocation(to, false, &h1, false);   // no sweep: a swept hand stops at the first blocker in front of the pawn; overlaps still update
    }
    else
        rl->K2_SetRelativeLocation(SDK::FVector{ reach,  spread, lift }, true, &h1, false);
    ll->K2_SetRelativeLocation(SDK::FVector{ reach, -spread, lift }, true, &h2, false);

    // Remember where they ended up in WORLD space for the FrequentData push - read it back from the
    // collision spheres, because that is what the game actually collides with.
    if (rh && lh) { g_lastHandR = rh->K2_GetComponentLocation(); g_lastHandL = lh->K2_GetComponentLocation(); }
}


// =============================================================================================
// [DIRECTCONNECT] -connectToServerByIPAndPort=<ip:port>
//
// That switch does NOT exist in these builds. Searching both binaries for it finds only the
// UFunction name, never a parsed argument:
//     A2-Win64-Shipping.exe (22284)      "ConnectToServerByIPAndPort" at +0x84AB2BE (FName pool)
//     libUnreal.so (Android 35232627)    "DirectConnectToServerByIPAndPort" at +0x275A26F
// and neither contains "UseInsecure" at all. So the command line from the internal/editor build
// would simply be ignored here.
//
// The function it would have called is present though:
//     UA2SessionSubsystem::DirectConnectToServerByIPAndPort(FString ConnectionString) -> bool
// so the switch is implemented here instead. This is better than the ` console "open" route: it
// goes through the game's own session subsystem, exactly as picking a server in the UI does,
// rather than a raw engine-level travel.
//
// Accepts either spelling, and -ConnectIP= as a short alias:
//     -connectToServerByIPAndPort=192.168.68.196:4949
//     -ConnectIP=127.0.0.1:7777
// =============================================================================================
static std::wstring g_directConnect;
static bool         g_connectTried = false;
static ULONGLONG    g_connectAtMs  = 0;

static bool ReadCmdArg(const wchar_t* flag, std::wstring& out)
{
    const wchar_t* cl = GetCommandLineW();
    if (!cl) return false;
    const wchar_t* p = wcsstr(cl, flag);
    if (!p) return false;
    p += wcslen(flag);
    if (*p == L'"') ++p;
    out.clear();
    while (*p && *p != L' ' && *p != L'"') { out += *p; ++p; }
    return !out.empty();
}

static SDK::UObject* FindSessionSubsystem()
{
    auto* cls = SDK::UObject::FindClassFast("A2SessionSubsystem");
    if (!cls) return nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(cls)) return o;
    }
    return nullptr;
}

static void TryDirectConnect()
{
    if (g_connectTried || g_directConnect.empty()) return;
    // Give the session subsystem and the frontend a moment to come up first; connecting during
    // early startup just fails silently.
    if (g_connectAtMs == 0) { g_connectAtMs = GetTickCount64(); return; }
    if (GetTickCount64() - g_connectAtMs < 8000) return;

    SDK::UObject* sess = FindSessionSubsystem();
    if (!sess) return;                                   // keep waiting; retried next tick
    auto* fn = sess->Class ? sess->Class->GetFunction("A2SessionSubsystem", "DirectConnectToServerByIPAndPort") : nullptr;
    if (!fn)
    {
        g_connectTried = true;
        Log("[A2PlayerControl][CONNECT] DirectConnectToServerByIPAndPort not found on this build\n");
        return;
    }
    g_connectTried = true;
    struct { SDK::FString ConnectionString; bool ReturnValue; uint8_t pad[7]; }
        p{ SDK::FString(g_directConnect.c_str()), false, {} };
    sess->ProcessEvent(fn, &p);
    Log("[A2PlayerControl][CONNECT] DirectConnectToServerByIPAndPort(%ls) -> %s\n",
        g_directConnect.c_str(), p.ReturnValue ? "accepted" : "REFUSED");
}

// ---------------------------------------------------------------------------------------------
// One tick, always on the game thread (called from the ProcessEvent pump).
// ---------------------------------------------------------------------------------------------
static void Tick()
{
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->OwningGameInstance) return;
    auto& lps = world->OwningGameInstance->LocalPlayers;
    if (lps.Num() <= 0 || !lps[0] || !lps[0]->PlayerController) return;
    SDK::APlayerController* pc = lps[0]->PlayerController;

    if (!g_banner)
    {
        g_banner = true;
        Log("[A2PlayerControl] ready. F1 = exit spectator, WASD/Space/Ctrl = move, Shift = boost, "
            "F2 = toggle movement, F3 = spawn a moving ball, F4 = throw nearest ball, F5 = hands "
            "(movement currently %s)\n", g_wasdEnabled ? "ON" : "OFF");
    }

    // [POSLOG] Every 5 s list every VR pawn we can see. Our own is ROLE_AutonomousProxy (2);
    // anyone else's is ROLE_SimulatedProxy (1) and only moves because the server replicated it,
    // so this doubles as a replication readout.
    {
        static ULONGLONG s_lastCensus = 0;
        const ULONGLONG nowc = GetTickCount64();
        if (nowc - s_lastCensus > 5000)
        {
            s_lastCensus = nowc;
            auto* pawnCls = SDK::UObject::FindClassFast("VRPawn");
            if (pawnCls)
            {
                const int32_t num = SDK::UObject::GObjects->Num();
                int n = 0;
                for (int32_t i = 0; i < num && n < 8; ++i)
                {
                    auto* o = SDK::UObject::GObjects->GetByIndex(i);
                    if (!o || o->IsDefaultObject() || !o->IsA(pawnCls)) continue;
                    auto* a = static_cast<SDK::AActor*>(o);
                    const SDK::FVector p = a->K2_GetActorLocation();
                    Log("[A2PlayerControl][PAWNS] %-28s pos=(%.0f, %.0f, %.0f) role=%d%s\n",
                        a->GetName().c_str(), p.X, p.Y, p.Z, (int)a->GetLocalRole(),
                        (a == pc->Pawn) ? "   <-- me" : "");
                    ++n;
                }
            }
        }
    }

    TryDirectConnect();

    if (Pressed(VK_F1, g_prevF1)) ExitSpectator(pc);
    // -HalcyonAutoSpawn (local tests on a machine nobody is at): leave spectator by ourselves once connected --
    // F1 is read with GetAsyncKeyState, which a locked remote desktop never delivers.
    {
        static ULONGLONG s_autoNext = 0;
        static int s_autoTries = 0;
        static const bool s_auto = wcsstr(GetCommandLineW(), L"-HalcyonAutoSpawn") != nullptr;
        static SDK::UClass* vrPawnCls = nullptr;
        if (!vrPawnCls) vrPawnCls = SDK::UObject::FindClassFast("VRPawn");
        const ULONGLONG nowa = GetTickCount64();
        if (s_auto && s_autoTries < 6 && vrPawnCls && !(pc->Pawn && pc->Pawn->IsA(vrPawnCls)))
        {
            if (!s_autoNext) s_autoNext = nowa + 15000;
            else if (nowa >= s_autoNext)
            {
                ++s_autoTries;
                s_autoNext = nowa + 10000;
                Log("[A2PlayerControl] -HalcyonAutoSpawn: leaving spectator (try %d)\n", s_autoTries);
                ExitSpectator(pc);
            }
        }
    }
    if (Pressed(VK_F5, g_prevF5))
    {
        g_handsEnabled = !g_handsEnabled;
        Log("[A2PlayerControl][HANDS] hand driving %s\n", g_handsEnabled ? "ON" : "OFF");
    }

    // [HANDDRIVE] where the hand spheres really are, once every 5 s. If these do not track the
    // pawn, hits cannot work and nothing else about the ball path matters.
    if (pc->Pawn)
    {
        static ULONGLONG s_lastHandLog = 0;
        const ULONGLONG nh = GetTickCount64();
        if (nh - s_lastHandLog > 5000)
        {
            s_lastHandLog = nh;
            const uintptr_t pw = reinterpret_cast<uintptr_t>(pc->Pawn);
            auto* rh = *reinterpret_cast<SDK::UPrimitiveComponent**>(pw + kRightHandCollOff);
            auto* lh = *reinterpret_cast<SDK::UPrimitiveComponent**>(pw + kLeftHandCollOff);
            auto* rl = *reinterpret_cast<SDK::USceneComponent**>(pw + kRightHandLocalOff);
            auto* ll = *reinterpret_cast<SDK::USceneComponent**>(pw + kLeftHandLocalOff);
            const SDK::FVector pp = pc->Pawn->K2_GetActorLocation();
            if (rh && lh && rl && ll)
            {
                const SDK::FVector rp = rh->K2_GetComponentLocation();
                const SDK::FVector lp = lh->K2_GetComponentLocation();
                const SDK::FVector rs = rl->K2_GetComponentLocation();
                const SDK::FVector ls = ll->K2_GetComponentLocation();
                // dist = how far the collision sphere is from the pawn. If the source moved but the
                // sphere did not follow, the pawn is re-deriving them from something else again.
                auto d = [&](const SDK::FVector& v) {
                    const double dx = v.X - pp.X, dy = v.Y - pp.Y, dz = v.Z - pp.Z;
                    return sqrt(dx * dx + dy * dy + dz * dz);
                };
                Log("[A2PlayerControl][HANDS] pawn=(%.0f, %.0f, %.0f) | src R=(%.0f, %.0f, %.0f) L=(%.0f, %.0f, %.0f) "
                    "| coll R=(%.0f, %.0f, %.0f) d=%.0f  L=(%.0f, %.0f, %.0f) d=%.0f | enable=%d driving=%d\n",
                    pp.X, pp.Y, pp.Z, rs.X, rs.Y, rs.Z, ls.X, ls.Y, ls.Z,
                    rp.X, rp.Y, rp.Z, d(rp), lp.X, lp.Y, lp.Z, d(lp),
                    (int)*reinterpret_cast<bool*>(pw + kEnableHandCollOff), (int)g_handsEnabled);
            }
            else Log("[A2PlayerControl][HANDS] hand components missing on %s (coll %p/%p src %p/%p)\n",
                     pc->Pawn->GetName().c_str(), (void*)rh, (void*)lh, (void*)rl, (void*)ll);
        }
    }

    // [BALLKEYS] F3 spawn a moving ball, F4 throw the nearest one. Both need a real pawn.
    if (pc->Pawn)
    {
        SDK::FVector vl{}; SDK::FRotator vr{};
        pc->GetPlayerViewPoint(&vl, &vr);
        const SDK::FVector vf = SDK::UKismetMathLibrary::GetForwardVector(vr);
        if (Pressed(VK_F3, g_prevF3)) SpawnBallInFront(pc->Pawn, vf);
        if (Pressed(VK_F4, g_prevF4) || g_cmdThrow)
        {
            g_cmdThrow = false;
            double d = 0.0;
            g_throwBall = NearestArenaBall(pc->Pawn->K2_GetActorLocation(), d);
            if (g_throwBall)
            {
                g_throwStart = g_throwBall->K2_GetActorLocation();
                g_throwTick = 0; g_throwOwned = 0;
                Log("[A2PlayerControl] F4: throwing %s at (%.0f, %.0f, %.0f), %.0f away@@",
                    g_throwBall->GetName().c_str(), g_throwStart.X, g_throwStart.Y, g_throwStart.Z, d);
            }
            else Log("[A2PlayerControl] F4: no arena ball found@@");
        }
        ThrowTick(pc->Pawn);
    }
    if (Pressed(VK_F2, g_prevF2))
    {
        g_wasdEnabled = !g_wasdEnabled;
        Log("[A2PlayerControl] F2: WASD movement %s\n", g_wasdEnabled ? "ON" : "OFF");
    }
    // [CMDFILE] Tests steer the pawn without touching the keyboard: %TEMP%\A2PlayerControl.cmd holds lines
    //   move <dx> <dy> <dz>   /   goto <x> <y> <z>
    // read once (the file is deleted), then the pawn flies to the target through the same velocity + server
    // push path as WASD, and stops within 60 cm.
    {
        static ULONGLONG s_cmdPoll = 0;
        const ULONGLONG nc = GetTickCount64();
        if (pc->Pawn && nc - s_cmdPoll > 250)
        {
            s_cmdPoll = nc;
            wchar_t tmp[MAX_PATH] = {};
            GetTempPathW(MAX_PATH, tmp);
            const std::wstring cmdPath = std::wstring(tmp) + L"A2PlayerControl.cmd";
            FILE* f = nullptr;
            if (!_wfopen_s(&f, cmdPath.c_str(), L"r") && f)
            {
                char line[256];
                while (fgets(line, sizeof(line), f))
                {
                    double x = 0, y = 0, z = 0;
                    const SDK::FVector p = pc->Pawn->K2_GetActorLocation();
                    if (sscanf_s(line, "move %lf %lf %lf", &x, &y, &z) == 3) { g_cmdTarget = { p.X + x, p.Y + y, p.Z + z }; g_cmdActive = true; }
                    else if (sscanf_s(line, "goto %lf %lf %lf", &x, &y, &z) == 3) { g_cmdTarget = { x, y, z }; g_cmdActive = true; }
                    else if (!strncmp(line, "throw", 5)) g_cmdThrow = true;
                    else if (!strncmp(line, "press", 5))
                    {
                        // press x y z: the KeypadButton nearest the point gets exactly what a hand overlap gives it --
                        // OnOverlapBegin(its mesh, this pawn, this pawn's right hand) -- on THIS client, so the game's
                        // own press path runs (the button component then tells the server).
                        double x = 0, y = 0, z = 0;
                        if (sscanf_s(line, "press %lf %lf %lf", &x, &y, &z) == 3 && pc->Pawn)
                        {
                            auto* kcls = SDK::UObject::FindClassFast("KeypadButton");
                            SDK::AActor* best = nullptr; double bd = 1e18;
                            const int32_t num = SDK::UObject::GObjects->Num();
                            for (int32_t i = 0; kcls && i < num; ++i)
                            {
                                auto* o = SDK::UObject::GObjects->GetByIndex(i);
                                if (!o || o->IsDefaultObject() || !o->IsA(kcls)) continue;
                                auto* ka = static_cast<SDK::AActor*>(o);
                                const SDK::FVector q = ka->K2_GetActorLocation();
                                const double d = (q.X - x) * (q.X - x) + (q.Y - y) * (q.Y - y) + (q.Z - z) * (q.Z - z);
                                if (d < bd) { bd = d; best = ka; }
                            }
                            if (!best) Log("[A2PlayerControl][PRESS] no KeypadButton\n");
                            else if (auto* fn = best->Class->GetFunction("KeypadButton", "OnOverlapBegin"))
                            {
                                struct {
                                    SDK::UPrimitiveComponent* OverlappedComp; SDK::AActor* OtherActor; SDK::UPrimitiveComponent* OtherComp;
                                    int32_t OtherBodyIndex; bool bFromSweep; uint8_t pad[3]; SDK::FHitResult SweepResult;
                                } prm{};
                                prm.OverlappedComp = *reinterpret_cast<SDK::UPrimitiveComponent**>(reinterpret_cast<uintptr_t>(best) + 0x298);   // buttonMesh
                                prm.OtherActor = pc->Pawn;
                                prm.OtherComp = *reinterpret_cast<SDK::UPrimitiveComponent**>(reinterpret_cast<uintptr_t>(pc->Pawn) + kRightHandCollOff);
                                best->ProcessEvent(fn, &prm);
                                Log("[A2PlayerControl][PRESS] %s (%.0f cm from the point): OnOverlapBegin sent (state now %d)\n", best->GetName().c_str(), sqrt(bd),
                                    (int)*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(best) + 0x2F0));
                            }
                        }
                    }
                    else if (!strncmp(line, "reach", 5))
                    {
                        double x = 0, y = 0, z = 0, sec = 4;
                        if (sscanf_s(line, "reach %lf %lf %lf %lf", &x, &y, &z, &sec) >= 3)
                        {
                            g_reachTarget = SDK::FVector{ x, y, z };
                            g_reachActive = true;
                            g_reachUntil = GetTickCount64() + static_cast<ULONGLONG>(sec * 1000.0);
                            Log("[A2PlayerControl][REACH] right hand -> (%.0f, %.0f, %.0f) for %.1fs (hand at %.0f, %.0f, %.0f)\n", x, y, z, sec,
                                g_lastHandR.X, g_lastHandR.Y, g_lastHandR.Z);
                        }
                    }
                    else if (!strncmp(line, "tcs", 3) && pc->Pawn)
                    {
                        // every team changer this client has: where, whether its door mesh is visible, and whether its
                        // TeamChangeComponent found a TicketManager (+0x508) -- Arena @0x4D8, TeamIndex @0x524
                        auto* tcc = SDK::UObject::FindClassFast("TeamChangeComponent");
                        const int32_t num = SDK::UObject::GObjects->Num();
                        for (int32_t i = 0; tcc && i < num; ++i)
                        {
                            auto* c = SDK::UObject::GObjects->GetByIndex(i);
                            if (!c || c->IsDefaultObject() || !c->IsA(tcc) || !c->Outer) continue;
                            auto* owner = static_cast<SDK::AActor*>(c->Outer);
                            if (!SDK::UKismetSystemLibrary::IsValid(owner)) continue;
                            const uintptr_t cb = reinterpret_cast<uintptr_t>(c);
                            int meshVis = -1;
                            for (int32_t j = 0; j < num; ++j)
                            {
                                auto* m = SDK::UObject::GObjects->GetByIndex(j);
                                if (m && m->Outer == owner && m->GetName() == "TeamSwitcherMesh")
                                { meshVis = static_cast<SDK::UPrimitiveComponent*>(m)->IsVisible() ? 1 : 0; break; }
                            }
                            const SDK::FVector q = owner->K2_GetActorLocation();
                            Log("[A2PlayerControl][TCS] %-40s at (%.0f,%.0f,%.0f) meshVisible=%d ticketManager=%d arena=%d team=%d\n",
                                owner->GetName().c_str(), q.X, q.Y, q.Z, meshVis, *reinterpret_cast<void**>(cb + 0x508) != nullptr,
                                *reinterpret_cast<int32_t*>(cb + 0x4D8), *reinterpret_cast<int32_t*>(cb + 0x524));
                        }
                    }
                    else if (!strncmp(line, "boards", 6) && pc->Pawn)
                    {
                        // every AScoreboard monitor within 60 m: which clock / score component it reads and what it shows.
                        // AScoreboard: bPlayersInArena @0x2A0, GameTimeComponent @0x320, ScoreComponent @0x328,
                        // StoredHomeScore @0x330, StoredAwayScore @0x334, StoredGameTime @0x348
                        auto* scls = SDK::UObject::FindClassFast("Scoreboard");
                        const SDK::FVector me = pc->Pawn->K2_GetActorLocation();
                        const int32_t num = SDK::UObject::GObjects->Num();
                        for (int32_t i = 0; scls && i < num; ++i)
                        {
                            auto* o = SDK::UObject::GObjects->GetByIndex(i);
                            if (!o || o->IsDefaultObject() || !o->IsA(scls)) continue;
                            auto* sa = static_cast<SDK::AActor*>(o);
                            const SDK::FVector q = sa->K2_GetActorLocation();
                            const double d = sqrt((q.X - me.X) * (q.X - me.X) + (q.Y - me.Y) * (q.Y - me.Y) + (q.Z - me.Z) * (q.Z - me.Z));
                            if (d > 6000.0) continue;
                            const uintptr_t b = reinterpret_cast<uintptr_t>(sa);
                            auto owner = [](uintptr_t comp) -> std::string {
                                if (!comp) return "null";
                                auto* c = reinterpret_cast<SDK::UObject*>(comp);
                                return c->GetName() + "@" + (c->Outer ? c->Outer->GetName() : "?");
                            };
                            Log("[A2PlayerControl][BOARDS] %s %.0fcm players=%d time=%s score=%s home=%d away=%d gametime=%.1f\n",
                                sa->GetName().c_str(), d, *reinterpret_cast<uint8_t*>(b + 0x2A0),
                                owner(*reinterpret_cast<uintptr_t*>(b + 0x320)).c_str(), owner(*reinterpret_cast<uintptr_t*>(b + 0x328)).c_str(),
                                *reinterpret_cast<int32_t*>(b + 0x330), *reinterpret_cast<int32_t*>(b + 0x334), *reinterpret_cast<float*>(b + 0x348));
                            // the clock it reads: GameTimeComponent netvars (ms) -- ClockStartedAt 0x478, SecondaryStartedAt 0x480,
                            // ClockEndLength 0x490, SecondaryEndLength 0x498, ClockPunishment 0x4B0, HasGameBegunTemp 0x4B8,
                            // StoredSecondaryTimerSecond 0x4D0. The countdown's per-second event only fires from this client's tick.
                            if (auto* gt = *reinterpret_cast<SDK::UActorComponent**>(b + 0x320))
                            {
                                const uintptr_t g = reinterpret_cast<uintptr_t>(gt);
                                auto* gtc = static_cast<SDK::UGameTimeComponent*>(gt);
                                Log("[A2PlayerControl][BOARDS]   clock start=%.0f 2nd=%.0f end=%.0f 2ndEnd=%.0f punish=%.0f begun=%d stored2nd=%d tick=%d cdRunning=%d text=%s cd=%s:%s\n",
                                    *reinterpret_cast<double*>(g + 0x478), *reinterpret_cast<double*>(g + 0x480), *reinterpret_cast<double*>(g + 0x490),
                                    *reinterpret_cast<double*>(g + 0x498), *reinterpret_cast<double*>(g + 0x4B0), *reinterpret_cast<uint8_t*>(g + 0x4B8),
                                    *reinterpret_cast<int32_t*>(g + 0x4D0), gt->IsComponentTickEnabled() ? 1 : 0, gtc->IsCountdownRunning() ? 1 : 0,
                                    gtc->GetGameTime().ToString().c_str(), gtc->GetCountdownMinutes().ToString().c_str(), gtc->GetCountdownSeconds().ToString().c_str());
                            }
                            // the page it shows (widget +0x300 -> ScoreboardSwitcher +0x328) and whether its countdown text is up;
                            // the page follows the team sizes it reads: PrefabComponent(+0x2E8) +0x440 -> +0x320 -> +0x438 -> int array @+0x3E0
                            if (auto* w = *reinterpret_cast<SDK::UTackleballScoreboardWidget**>(b + 0x300))
                            {
                                const int page = w->ScoreboardSwitcher ? w->ScoreboardSwitcher->GetActiveWidgetIndex() : -1;
                                std::string sizes, chain;
                                uintptr_t hops[3]{}; int32_t sz[8]{}; int nsz = -1;
                                BoardSizeChain(b, hops, sz, &nsz);
                                for (int k = 0; k < 3; ++k)
                                    chain += (k ? " > " : "") + (hops[k] ? reinterpret_cast<SDK::UObject*>(hops[k])->GetName() : std::string("null"));
                                if (nsz < 0) sizes = "?";
                                for (int k = 0; k < nsz; ++k) sizes += std::to_string(sz[k]) + " ";
                                Log("[A2PlayerControl][BOARDS]   page=%d countdownVisible=%d sizes=[%s] from %s\n", page,
                                    w->IsCountdownTextVisible() ? 1 : 0, sizes.c_str(), chain.c_str());
                            }
                        }
                    }
                    else if (!strncmp(line, "balls", 5) && pc->Pawn)
                    {
                        // every ball this client has: where, how big (actor scale + its collision sphere's scaled radius)
                        auto* dcls = SDK::UObject::FindClassFast("DiscEntity");
                        const int32_t num = SDK::UObject::GObjects->Num();
                        for (int32_t i = 0; dcls && i < num; ++i)
                        {
                            auto* o = SDK::UObject::GObjects->GetByIndex(i);
                            if (!o || o->IsDefaultObject() || !o->IsA(dcls)) continue;
                            auto* d = static_cast<SDK::ADiscEntity*>(o);
                            const SDK::FVector q = d->K2_GetActorLocation(), s = d->GetActorScale3D();
                            const float r = d->SphereComponent ? d->SphereComponent->GetScaledSphereRadius() : -1.0f;
                            Log("[A2PlayerControl][BALLS] %s (%s) at (%.0f,%.0f,%.0f) scale=(%.2f,%.2f,%.2f) radius=%.1f hidden=%d\n",
                                d->GetName().c_str(), d->Class->GetName().c_str(), q.X, q.Y, q.Z, s.X, s.Y, s.Z, r, d->bHidden ? 1 : 0);
                        }
                    }
                    else if (!strncmp(line, "grav", 4) && pc->Pawn)
                    {
                        // this client's view of its own gravity: AVRPawn.GravityComponent @0x930 -> TotalGravity @0x170
                        // (FA2Gravity: GravityDir double3, gravityScale float @+0x18; replicated), defaultPlayerGravity @0xB4,
                        // GravityModifiers TMap @0x120 (element count @+0x8)
                        const uintptr_t gc = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(pc->Pawn) + 0x930);
                        if (gc)
                        {
                            const double* d = reinterpret_cast<const double*>(gc + 0x170);
                            Log("[A2PlayerControl][GRAV] total dir=(%.2f,%.2f,%.2f) scale=%.3f  default=%.1f  modifiers=%d  pos=(%.0f,%.0f,%.0f)\n",
                                d[0], d[1], d[2], *reinterpret_cast<const float*>(gc + 0x170 + 0x18), *reinterpret_cast<const float*>(gc + 0xB4),
                                *reinterpret_cast<const int32_t*>(gc + 0x120 + 0x8), pc->Pawn->K2_GetActorLocation().X,
                                pc->Pawn->K2_GetActorLocation().Y, pc->Pawn->K2_GetActorLocation().Z);
                        }
                    }
                    else if (!strncmp(line, "cullstats", 9))
                    {
                        // Every sandbox prefab this client has: culling-entry stats split by host area, plus a few samples.
                        static SDK::UClass* pcls = SDK::UObject::FindClassFast("PrefabComponent");
                        struct St { int dead = 0, n = 0, vis = 0, imp = 0, hideOut = 0, always = 0, forceHid = 0, hidden = 0; std::string sample; };
                        std::map<std::string, St> by;
                        const int32_t num = SDK::UObject::GObjects->Num();
                        for (int32_t i = 0; pcls && i < num; ++i)
                        {
                            auto* c = SDK::UObject::GObjects->GetByIndex(i);
                            if (!c || c->IsDefaultObject() || !c->IsA(pcls)) continue;
                            const uintptr_t pc2 = reinterpret_cast<uintptr_t>(c);
                            uintptr_t lgm = 0; int inl = 0;
                            PrefabImportance(pc2, &lgm, &inl);
                            std::string area = "-";
                            if (lgm)
                                if (auto* slot = *reinterpret_cast<SDK::UObject**>(lgm + 0x320))
                                {
                                    const wchar_t* sid = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(slot) + 0x390);
                                    char b[80] = "?"; if (sid) WideCharToMultiByte(CP_UTF8, 0, sid, -1, b, sizeof b, nullptr, nullptr);
                                    area = std::string(b) + (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(slot) + 0x2FC) ? "[full]" : "");
                                }
                            char cb[200] = "";
                            if (!PrefabCullEntry(pc2, cb, sizeof cb)) continue;
                            St& s = by[area];
                            ++s.n;
                            auto has = [&](const char* k) { return strstr(cb, k) != nullptr; };
                            if (has("vis=1")) ++s.vis;
                            if (has("imp=1")) ++s.imp;
                            if (has("hideOutside=1")) ++s.hideOut;
                            if (has("always=1")) ++s.always;
                            if (has("forceHid=1")) ++s.forceHid;
                            auto* owner = c->Outer;
                            if (owner && (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(owner) + 0x60) >> 7)) ++s.hidden;
                            if (!owner || !SDK::UKismetSystemLibrary::IsValid(owner)) ++s.dead;   // destroyed, waiting for GC
                            if (s.sample.size() < 600 && owner) s.sample += "\n      " + owner->Class->GetName() + " " + cb;
                        }
                        for (auto& kv : by)
                            Log("[A2PlayerControl][CULLSTATS] %-34s dead=%d n=%d vis=%d imp=%d hideOutside=%d always=%d forceHid=%d bHidden=%d%s\n",
                                kv.first.c_str(), kv.second.dead, kv.second.n, kv.second.vis, kv.second.imp, kv.second.hideOut, kv.second.always,
                                kv.second.forceHid, kv.second.hidden, kv.second.sample.c_str());
                    }
                    else if (!strncmp(line, "questcull", 9))
                    {
                        int mode = -1;
                        sscanf_s(line, "questcull %d", &mode);
                        QuestCull(mode);
                    }
                    else if (!strncmp(line, "near", 4))
                    {
                        // near [radius cm]: what this player's game has around it and whether it is DRAWN: per actor,
                        // its primitive components (count), how many are visible, and how many the renderer drew
                        // in the last second (WasRecentlyRendered) -- the "I can't see the object" check.
                        double rad = 2000.0;
                        sscanf_s(line, "near %lf", &rad);
                        auto* primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
                        const int32_t num = SDK::UObject::GObjects->Num();
                        int shown = 0;
                        for (int32_t oi = 0; oi < num && shown < 60; ++oi)
                        {
                            auto* o = SDK::UObject::GObjects->GetByIndex(oi);
                            if (!o || o->IsDefaultObject() || !o->IsA(SDK::AActor::StaticClass())) continue;
                            auto* ac = static_cast<SDK::AActor*>(o);
                            if (ac == pc->Pawn || ac == pc) continue;
                            const std::string cn = ac->Class ? ac->Class->GetName() : "?";
                            if (cn.find("Controller") != std::string::npos || cn.find("PlayerState") != std::string::npos) continue;
                            {
                                const std::string an = ac->GetName();          // placed objects (GUID names) and area slots only
                                const bool placed = an.size() >= 36 && an[8] == '-' && an[13] == '-';
                                if (!placed && cn.find("ModuleSlot") == std::string::npos && cn.find("GamemodeSlot") == std::string::npos) continue;
                            }
                            const SDK::FVector q = ac->K2_GetActorLocation();
                            const double dx = q.X - p.X, dy = q.Y - p.Y, dz = q.Z - p.Z;
                            const double dist = sqrt(dx * dx + dy * dy + dz * dz);
                            if (dist > rad || (q.X == 0 && q.Y == 0 && q.Z == 0)) continue;
                            int prims = 0, vis = 0, drawn = 0, hiddenInGame = 0;
                            int imp = -9, inLoaded = -1; uintptr_t plgm = 0;
                            {
                                static SDK::UClass* prefabCls = SDK::UObject::FindClassFast("PrefabComponent");
                                for (int32_t ci = 0; prefabCls && ci < num; ++ci)
                                {
                                    auto* c = SDK::UObject::GObjects->GetByIndex(ci);
                                    if (!c || c->Outer != ac || !c->IsA(prefabCls)) continue;
                                    imp = PrefabImportance(reinterpret_cast<uintptr_t>(c), &plgm, &inLoaded);
                                    break;
                                }
                            }
                            std::string lgmName = "-";
                            if (plgm) lgmName = reinterpret_cast<SDK::UObject*>(plgm)->GetName();
                            if (cn.find("ModuleSlot") != std::string::npos || cn.find("GamemodeSlot") != std::string::npos)
                            {
                                const uintptr_t sa = reinterpret_cast<uintptr_t>(ac);
                                const uintptr_t sl = *reinterpret_cast<uintptr_t*>(sa + 0x440);
                                const wchar_t* sid = *reinterpret_cast<const wchar_t**>(sa + 0x390);
                                char sidn[80] = "?";
                                if (sid) WideCharToMultiByte(CP_UTF8, 0, sid, -1, sidn, sizeof sidn, nullptr, nullptr);
                                lgmName = std::string(sl ? reinterpret_cast<SDK::UObject*>(sl)->GetName() : "no-lgm") + " slotId=" + sidn +
                                          " allVisible=" + std::to_string(*reinterpret_cast<uint8_t*>(sa + 0x2FC));
                            }
                            if (primCls)
                                for (int32_t ci = 0; ci < num; ++ci)
                                {
                                    auto* c = SDK::UObject::GObjects->GetByIndex(ci);
                                    if (!c || c->Outer != ac || !c->IsA(primCls)) continue;
                                    auto* pcmp = static_cast<SDK::UPrimitiveComponent*>(c);
                                    if (cn.find("TeamChange") != std::string::npos)   // team changers: every part, to see what's hidden
                                        Log("[A2PlayerControl][NEAR]       part %-34s %-26s visible=%d hiddenInGame=%d\n", c->GetName().c_str(),
                                            c->Class->GetName().c_str(), (int)pcmp->IsVisible(),
                                            (int)((*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(c) + 0x2D7) & 0x40) != 0));
                                    ++prims;
                                    if (pcmp->IsVisible()) ++vis;
                                    if (pcmp->WasRecentlyRendered(1.0f)) ++drawn;
                                    if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(c) + 0x2D7) & 0x40) ++hiddenInGame;
                                }
                            Log("[A2PlayerControl][NEAR] %-44s %-34s %5.0fcm prims=%d visible=%d drawn=%d actorDrawn=%d important=%d inLoaded=%d area=%s\n",
                                ac->GetName().c_str(), cn.c_str(), dist, prims, vis, drawn, (int)ac->WasRecentlyRendered(1.0f),
                                imp, inLoaded, lgmName.c_str());
                            {   // each Luau component's sandbox links: LuauComp +0x430, ObjectPrefabComponent +0x438 (the press
                                // message is keyed by it), LoadedGameMode +0x440, ModuleSlot +0x448
                                static SDK::UClass* lbCls = SDK::UObject::FindClassFast("LuauBehavior");
                                for (int32_t ci = 0; lbCls && ci < num; ++ci)
                                {
                                    auto* c = SDK::UObject::GObjects->GetByIndex(ci);
                                    if (!c || c->Outer != ac || !c->IsA(lbCls)) continue;
                                    const uintptr_t cb = reinterpret_cast<uintptr_t>(c);
                                    Log("[A2PlayerControl][NEAR]     %s: luau=%d prefab=%d lgm=%d slot=%d\n", c->Class->GetName().c_str(),
                                        *reinterpret_cast<void**>(cb + 0x430) != nullptr, *reinterpret_cast<void**>(cb + 0x438) != nullptr,
                                        *reinterpret_cast<void**>(cb + 0x440) != nullptr, *reinterpret_cast<void**>(cb + 0x448) != nullptr);
                                }
                            }
                            if (plgm)
                            {
                                static SDK::UClass* prefabCls2 = SDK::UObject::FindClassFast("PrefabComponent");
                                for (int32_t ci = 0; prefabCls2 && ci < num; ++ci)
                                {
                                    auto* c = SDK::UObject::GObjects->GetByIndex(ci);
                                    if (!c || c->Outer != ac || !c->IsA(prefabCls2)) continue;
                                    char cb[200];
                                    if (PrefabCullEntry(reinterpret_cast<uintptr_t>(c), cb, sizeof cb))
                                        Log("[A2PlayerControl][NEAR]     %s bHidden=%d\n", cb,
                                            (int)(*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(ac) + 0x60) >> 7));
                                    break;
                                }
                            }
                            if (SDK::AActor* par = ac->GetAttachParentActor())
                                Log("[A2PlayerControl][NEAR]     attached to %s\n", par->GetName().c_str());
                            ++shown;
                        }
                        Log("[A2PlayerControl][NEAR] %d actor(s) within %.0f cm\n", shown, rad);
                    }
                    else if (!strncmp(line, "balls", 5))
                    {
                        double d = 0.0;
                        SDK::AActor* b = NearestArenaBall(p, d);
                        const SDK::FVector bp = b ? b->K2_GetActorLocation() : SDK::FVector{};
                        Log("[A2PlayerControl][CMD] nearest ball %s at (%.0f, %.0f, %.0f), %.0f away\n", b ? b->GetName().c_str() : "(none)", bp.X, bp.Y, bp.Z, d);
                    }
                    if (g_cmdActive) Log("[A2PlayerControl][CMD] %s -> target (%.0f, %.0f, %.0f) from (%.0f, %.0f, %.0f)\n", line,
                                         g_cmdTarget.X, g_cmdTarget.Y, g_cmdTarget.Z, p.X, p.Y, p.Z);
                }
                fclose(f);
                _wremove(cmdPath.c_str());
            }
        }
        if (g_cmdActive && pc->Pawn)
        {
            SDK::APawn* pw = pc->Pawn;
            const SDK::FVector p = pw->K2_GetActorLocation();
            const SDK::FVector d{ g_cmdTarget.X - p.X, g_cmdTarget.Y - p.Y, g_cmdTarget.Z - p.Z };
            const double dist = sqrt(d.X * d.X + d.Y * d.Y + d.Z * d.Z);
            SDK::FVector v{};
            if (dist < 60.0) { g_cmdActive = false; Log("[A2PlayerControl][CMD] arrived at (%.0f, %.0f, %.0f)\n", p.X, p.Y, p.Z); }
            else { const double s = (std::min)(400.0, dist * 2.0) / dist; v = { d.X * s, d.Y * s, d.Z * s }; }
            SetPawnVelocity(pw, v, false);
            PushToServer(pw, p, v, g_cmdActive);
            return;
        }
        // [REACH] a reach drives the right hand even while the pawn stands still (the movement path below only drives
        // the hands while moving, and not at all without WASD control).
        if (g_reachActive && pc->Pawn && g_handsEnabled)
        {
            SDK::APawn* pw = pc->Pawn;
            DriveHands(pw, SDK::FVector{}, SDK::FVector{}, 0.0);
            PushToServer(pw, pw->K2_GetActorLocation(), SDK::FVector{}, false);
            return;
        }
    }
    if (!g_wasdEnabled) return;

    SDK::APawn* pawn = pc->Pawn;
    if (!pawn) return;

    const ULONGLONG now = GetTickCount64();
    double dt = (now - g_lastTickMs) / 1000.0;
    g_lastTickMs = now;
    if (dt <= 0.0 || dt > 0.25) dt = 0.011;

    // [VIEWBASIS] W must go where the SCREEN is looking. APlayerController::GetControlRotation is
    // the wrong source here: a VR pawn drives its view from the HMD/camera, not from the
    // controller's control rotation, so with -nohmd that rotation just sits at (0,0,0) and W always
    // pushed along world +X no matter which way the view faced. GetPlayerViewPoint is the view the
    // frame is actually rendered from.
    SDK::FVector viewLoc{}; SDK::FRotator rot{};
    pc->GetPlayerViewPoint(&viewLoc, &rot);
    const char* rotSrc = "viewpoint";
    if (rot.Pitch == 0.0 && rot.Yaw == 0.0 && rot.Roll == 0.0)
    {
        if (pc->PlayerCameraManager) { rot = pc->PlayerCameraManager->GetCameraRotation(); rotSrc = "cameramgr"; }
        if (rot.Pitch == 0.0 && rot.Yaw == 0.0 && rot.Roll == 0.0) { rot = pc->GetControlRotation(); rotSrc = "control"; }
    }
    const SDK::FVector fwd   = SDK::UKismetMathLibrary::GetForwardVector(rot);
    const SDK::FVector right = SDK::UKismetMathLibrary::GetRightVector(rot);

    SDK::FVector dir{};
    if (Down('W')) { dir.X += fwd.X;   dir.Y += fwd.Y;   dir.Z += fwd.Z; }
    if (Down('S')) { dir.X -= fwd.X;   dir.Y -= fwd.Y;   dir.Z -= fwd.Z; }
    if (Down('D')) { dir.X += right.X; dir.Y += right.Y; dir.Z += right.Z; }
    if (Down('A')) { dir.X -= right.X; dir.Y -= right.Y; dir.Z -= right.Z; }
    if (Down(VK_SPACE))   dir.Z += 1.0;
    if (Down(VK_CONTROL)) dir.Z -= 1.0;

    const double len = sqrt(dir.X * dir.X + dir.Y * dir.Y + dir.Z * dir.Z);
    const bool thrusting = (len > 0.0001);
    const double boost = Down(VK_SHIFT) ? kBoostMul : 1.0;

    SDK::FVector vel = GetPawnVelocity(pawn);
    const double speed0 = sqrt(vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z);

    if (thrusting)
    {
        // Accelerate through the game's own velocity, so its limit and collision still apply.
        const double a = kAccel * boost * dt / len;
        vel.X += dir.X * a; vel.Y += dir.Y * a; vel.Z += dir.Z * a;
    }
    else if (speed0 > 1.0)
    {
        // Coast to a stop instead of stopping dead.
        const double k = 1.0 - kBrake * dt;
        const double d = (k < 0.0) ? 0.0 : k;
        vel.X *= d; vel.Y *= d; vel.Z *= d;
    }
    else return;   // parked

    // ignoreLimit = false: respect the pawn's own speed cap, so this feels like the real thing
    // and cannot launch the player past what the game considers legal.
    const bool applied = SetPawnVelocity(pawn, vel, false);

    // [HANDDRIVE] keep the hands out front and swept, so flying into a ball actually hits it,
    // and tell the server where they are.
    if (g_handsEnabled) DriveHands(pawn, fwd, right, dt);
    PushToServer(pawn, pawn->K2_GetActorLocation(), vel, thrusting);

    // [POSLOG] Once a second while moving. `applied` says whether SetVelocity exists; comparing
    // speed before/after says whether the game accepted it.
    static ULONGLONG s_lastLog = 0;
    if (now - s_lastLog > 1000)
    {
        s_lastLog = now;
        const SDK::FVector pos = pawn->K2_GetActorLocation();
        const SDK::FVector after = GetPawnVelocity(pawn);
        Log("[A2PlayerControl] pos=(%.0f, %.0f, %.0f) vel %.0f -> %.0f (want %.0f) setvel=%s "
            "view[%s]=(y%.0f p%.0f) fwd=(%.2f, %.2f, %.2f) pawn=%s\n",
            pos.X, pos.Y, pos.Z, speed0,
            sqrt(after.X * after.X + after.Y * after.Y + after.Z * after.Z),
            sqrt(vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z),
            applied ? "ok" : "MISSING", rotSrc, rot.Yaw, rot.Pitch,
            fwd.X, fwd.Y, fwd.Z, pawn->GetName().c_str());
    }
}

static void SafeTick() { __try { Tick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------------------------
// ProcessEvent hook: a game-thread pump, nothing more. Re-entrancy guarded, rate limited.
// ---------------------------------------------------------------------------------------------
using ProcessEvent_t = void(__fastcall*)(SDK::UObject*, SDK::UFunction*, void*);
static ProcessEvent_t ProcessEvent_Orig = nullptr;

static void __fastcall ProcessEvent_Hook(SDK::UObject* Context, SDK::UFunction* Function, void* Parms)
{
    static thread_local bool inHook = false;
    if (!inHook)
    {
        inHook = true;
        const ULONGLONG now = GetTickCount64();
        static ULONGLONG last = 0;
        if (now - last >= (ULONGLONG)kTickMs) { last = now; SafeTick(); }
        inHook = false;
    }
    ProcessEvent_Orig(Context, Function, Parms);
}

// ---------------------------------------------------------------------------------------------
static DWORD WINAPI ModThread(LPVOID)
{
    Log("\n[A2PlayerControl] starting (base 0x%llX)\n", (unsigned long long)GetBase());

    // [WASDTOGGLE] opt in to WASD flying (desktop/test clients). Real VR players leave it off.
    if (wcsstr(GetCommandLineW(), L"-HalcyonWASD"))
    {
        g_wasdEnabled = true;
        Log("[A2PlayerControl][WASD] -HalcyonWASD: keyboard flying ENABLED\n");
    }

    // [DIRECTCONNECT] pick up the connect switch (either spelling, plus a short alias).
    if (ReadCmdArg(L"-connectToServerByIPAndPort=", g_directConnect) ||
        ReadCmdArg(L"-ConnectToServerByIPAndPort=", g_directConnect) ||
        ReadCmdArg(L"-ConnectIP=", g_directConnect))
        Log("[A2PlayerControl][CONNECT] will connect to %ls once the session subsystem is up\n",
            g_directConnect.c_str());

    // Wait for a live world before hooking - GObjects/GWorld are not up when mods start.
    for (int i = 0; i < 600; ++i)
    {
        auto* w = SDK::UWorld::GetWorld();
        if (w && w->OwningGameInstance) break;
        Sleep(250);
    }
    if (!SDK::UWorld::GetWorld()) { Log("[A2PlayerControl] no world after 150s - giving up\n"); return 0; }

    if (MH_Initialize() != MH_OK) { Log("[A2PlayerControl] MH_Initialize failed\n"); return 0; }
    void* pe = reinterpret_cast<void*>(GetBase() + SDK::Offsets::ProcessEvent);
    const MH_STATUS c = MH_CreateHook(pe, &ProcessEvent_Hook, reinterpret_cast<void**>(&ProcessEvent_Orig));
    const MH_STATUS e = MH_EnableHook(pe);
    Log("[A2PlayerControl] ProcessEvent pump @ %p create=%d enable=%d\n", pe, (int)c, (int)e);
    if (c != MH_OK || e != MH_OK) Log("[A2PlayerControl] hook failed - mod inactive\n");
    return 0;
}

extern "C" __declspec(dllexport) void* start_mod()
{
    CloseHandle(CreateThread(nullptr, 0, &ModThread, nullptr, 0, nullptr));
    return nullptr;   // "not started" as far as UE4SS is concerned: no ABI coupling, no callbacks
}

extern "C" __declspec(dllexport) void uninstall_mod(void*) {}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(h);
    return TRUE;
}
