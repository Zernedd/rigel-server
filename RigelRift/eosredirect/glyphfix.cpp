// glyphfix.cpp - client-side parkour glyph fix, shipped inside dsound.dll.
//
// ============================================================================================
// WHY THIS EXISTS
// ============================================================================================
// In A2 22284 the parkour quest glyphs never appear. The cause is in the cooked content, not in our
// backend: Prime_LIs_PKR instances /Game/A2/Maps/Districts/Parkour/PKR_Quests_01, while the glyph
// buttons for the parkour quests live in PKR_Quests_Unbugged. Nothing streams that level, so the
// AA2ProgressionButtonGlyphActor instances for parkour simply do not exist, and the progression binder
// logs "Unable to find the glyph with the GlyphID 'None' for title LCN_PKR_Climb_Adv".
//
// The server cannot fix it. AA2ProgressionButtonActor is client-local (its whole surface is
// OnLocalButtonBeginOverlap / OnLocalQuestUpdated - no Server_ RPCs, no replicated properties), and
// ULevelStreamingDynamic::LoadLevelInstance creates a RUNTIME package (/Temp/Game/..._LevelInstance_<hash>)
// which a client can never load. Loading it server-side was measured (glyph actors 15 -> 20, correct
// placement) and reaches nobody. So the same call has to run on each client - which is what this is.
//
// WHAT IT DOES
// ------------
// Once the parkour district has streamed in (i.e. the player is actually at parkour), call
//     ULevelStreamingDynamic::LoadLevelInstance(world, "/Game/A2/Maps/Districts/Parkour/PKR_Quests_Unbugged",
//                                               location, rotation)
// with the placement copied from the ULevelStreaming for PKR_Quests_01, so the glyph level lands exactly
// where the bugged one did. This exact recipe was verified on a server (success=1, actors 15 -> 20).
//
// HOW IT STAYS CHEAP
// ------------------
// * Nothing happens until the ULevelStreaming for PKR_Quests_01 exists, and the poll is once every 10s.
// * The poll never resolves names for the whole object array. One name-resolving walk happens ONCE, to
//   cache the LevelStreaming* UClass pointers plus the LoadLevelInstance UFunction and the
//   LevelStreamingDynamic CDO; every later pass is a pointer compare against those cached classes.
// * Once the level is in, the tick short-circuits on a single bool for the rest of the session.
// * -NoGlyphFix on the command line turns the whole thing off.
//
// Offsets are for A2 build 22284 (the exe this build ships), taken from the Dumper7 SDK at
// HalcyonA2\HalcyonA2\gamesdk\22284. Deliberately NO SDK include: that would drag Basic.cpp and the
// generated function bodies into this DLL. Everything needed is a handful of field offsets, below.

#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstdarg>
#include <cwchar>
#include <cmath>
#include "MinHook.h"

namespace {

// -- build 22284 offsets ---------------------------------------------------------------------
constexpr uintptr_t kOffGObjects     = 0x097C7EA0;   // TUObjectArray (the array itself, not a pointer)
constexpr uintptr_t kOffAppendString = 0x0113F2E0;   // void FName::AppendString(FString&) const
constexpr uintptr_t kOffGWorld       = 0x09A387B8;   // UWorld**
constexpr uintptr_t kOffProcessEvent = 0x01357960;   // void UObject::ProcessEvent(UFunction*, void*)
constexpr uintptr_t kOffLoadInstance = 0x03AAF180;   // ULevelStreamingLevelInstance::LoadInstance

// UObject: VTable@0x00 Flags@0x08 Index@0x0C Class@0x10 Name@0x18 Outer@0x20
constexpr uintptr_t kUObj_Class = 0x10, kUObj_Name = 0x18, kUObj_Outer = 0x20;
// ULevelStreaming: PackageNameToLoad@0x54 (FName), LevelTransform@0x80 (FTransform: quat@+0, trans@+0x20)
constexpr uintptr_t kULS_PackageNameToLoad = 0x54, kULS_LevelTransform = 0x80;
// ULevelStreaming::LoadedLevel@0x158 -> ULevel::Actors@0xA0 (TArray: Data@0, Num@8), OwningWorld@0xC0.
// Name-free, so the Quest hook can run the same check without any reflection at all.
constexpr uintptr_t kULS_LoadedLevel = 0x158, kULevel_Actors = 0xA0, kULevel_OwningWorld = 0xC0;

const wchar_t* const kGlyphLevel  = L"/Game/A2/Maps/Districts/Parkour/PKR_Quests_Unbugged";
const wchar_t* const kGlyphLeaf   = L"PKR_Quests_Unbugged";
const wchar_t* const kBuggedLevel = L"PKR_Quests_01";

// Fallback placement, measured on a live server from the PKR_Quests_01 ULevelStreaming. Only used if
// that streaming level is present but carries an identity transform (i.e. it was not populated).
const double kFallbackLoc[3] = { -24790.0, 0.0, 14312.0 };
const double kFallbackRot[3] = { -60.0, 180.0, -180.0 };

// -- minimal UE types ------------------------------------------------------------------------
struct FStr { wchar_t* Data; int32_t Num; int32_t Max; };
struct FUObjectItem { void* Object; uint8_t Pad[0x10]; };
struct TUObjectArray
{
    FUObjectItem** Objects;
    uint8_t        Pad8[0x08];
    int32_t        MaxElements;
    int32_t        NumElements;
    int32_t        MaxChunks;
    int32_t        NumChunks;

    enum { ElementsPerChunk = 0x10000 };

    void* GetByIndex(int32_t i) const
    {
        if (i < 0 || i >= NumElements) return nullptr;
        const int32_t chunk = i / ElementsPerChunk;
        if (chunk >= NumChunks) return nullptr;
        FUObjectItem* c = Objects[chunk];
        return c ? c[i % ElementsPerChunk].Object : nullptr;
    }
};

typedef void (*AppendString_t)(const void* /*FName*/, FStr&);
typedef void (__fastcall *ProcessEvent_t)(void*, void*, void*);

uintptr_t      g_base   = 0;
TUObjectArray* g_objs   = nullptr;
AppendString_t g_append = nullptr;
ProcessEvent_t g_peOrig = nullptr;

// -- log (a handful of lines per session, %TEMP%\rigel_glyph.log) -----------------------------
void GLog(const char* fmt, ...)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) strcpy_s(path, MAX_PATH, ".\\");
    strcat_s(path, MAX_PATH, "rigel_glyph.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// FName -> wide text. Always leaves out[] terminated, even on a null name or a refused append.
void NameText(const void* fname, wchar_t* out, int cap)
{
    out[0] = 0;
    if (!fname || !g_append || cap < 2) return;
    FStr s = { out, 0, cap - 1 };
    g_append(fname, s);
    if (s.Num < 0 || s.Num >= cap) s.Num = 0;
    out[s.Num] = 0;
}
void ObjName(void* o, wchar_t* out, int cap)
{
    out[0] = 0;
    if (!o) return;
    NameText(reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(o) + kUObj_Name), out, cap);
}

// -- cached, resolved once by a single name-resolving walk ------------------------------------
// 12 was not enough: a live process has 19 objects whose name contains "LevelStreaming" and every one
// of the native classes among them has to be in the set, or a streaming level of the missing class is
// invisible to the pass below.
enum { kMaxLsClasses = 32 };
void* g_lsClasses[kMaxLsClasses] = {};
int   g_lsClassCount = 0;
void* g_lliFn  = nullptr;   // UFunction ULevelStreamingDynamic::LoadLevelInstance
void* g_lliCdo = nullptr;   // Default__LevelStreamingDynamic

bool IsLsClass(void* cls)
{
    for (int i = 0; i < g_lsClassCount; ++i) if (g_lsClasses[i] == cls) return true;
    return false;
}

// One full walk, once: every UClass whose name contains "LevelStreaming" (ULevelStreaming and each of
// its subclasses share the PackageNameToLoad / LevelTransform layout), plus the function and the CDO we
// call. Everything after this is pointer comparisons.
//
// Name alone is NOT enough to tell a class from an instance here - live ULevelStreaming objects are
// named "LevelStreamingDynamic_12" and would crowd the list out. A native class object is identified by
// its own Class pointer, which is the UClass class; bootstrap that from the object named exactly
// "LevelStreaming" (an instance always carries a _N suffix, so the exact name is the class).
bool ResolveStatics()
{
    if (g_lliFn && g_lliCdo && g_lsClassCount > 0) return true;

    enum { kMaxCandidates = 256 };
    void* cand[kMaxCandidates];
    int   candCount = 0;
    void* uclassObj = nullptr;

    wchar_t name[512], outer[512];
    const int32_t num = g_objs->NumElements;
    for (int32_t i = 0; i < num; ++i)
    {
        void* o = g_objs->GetByIndex(i);
        if (!o) continue;
        ObjName(o, name, 512);
        if (!name[0]) continue;

        if (wcsncmp(name, L"Default__", 9) == 0)
        {
            if (!g_lliCdo && wcscmp(name + 9, L"LevelStreamingDynamic") == 0) g_lliCdo = o;
            continue;                                   // a CDO is never the class object itself
        }
        if (wcsstr(name, L"LevelStreaming"))
        {
            if (candCount < kMaxCandidates) cand[candCount++] = o;
            if (!uclassObj && wcscmp(name, L"LevelStreaming") == 0)
                uclassObj = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + kUObj_Class);
        }
        else if (!g_lliFn && wcscmp(name, L"LoadLevelInstance") == 0)
        {
            void* out = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + kUObj_Outer);
            ObjName(out, outer, 512);
            if (wcscmp(outer, L"LevelStreamingDynamic") == 0) g_lliFn = o;
        }
    }

    if (uclassObj)
        for (int i = 0; i < candCount && g_lsClassCount < kMaxLsClasses; ++i)
            if (*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(cand[i]) + kUObj_Class) == uclassObj)
                g_lsClasses[g_lsClassCount++] = cand[i];

    GLog("resolve: lsClasses=%d of %d candidates, loadFn=%p cdo=%p (objects=%d)",
         g_lsClassCount, candCount, g_lliFn, g_lliCdo, (int)num);
    return g_lliFn && g_lliCdo && g_lsClassCount > 0;
}

// FTransform quat -> UE rotator (pitch, yaw, roll) in degrees; the maths the server-side probe used.
void QuatToRotator(const double* q, double* rot)
{
    const double X = q[0], Y = q[1], Z = q[2], W = q[3];
    const double sing = Z * X - W * Y;
    const double R2D = 57.295779513082321;
    const double yaw = atan2(2.0 * (W * Z + X * Y), 1.0 - 2.0 * (Y * Y + Z * Z)) * R2D;
    if (sing > 0.4999995)       { rot[0] =  90.0; rot[1] = yaw; rot[2] = 0.0; }
    else if (sing < -0.4999995) { rot[0] = -90.0; rot[1] = yaw; rot[2] = 0.0; }
    else
    {
        rot[0] = asin(2.0 * sing) * R2D;
        rot[1] = yaw;
        rot[2] = atan2(2.0 * (W * X + Y * Z), 1.0 - 2.0 * (X * X + Y * Y)) * R2D;
    }
}

// Set by -GlyphDiag. The residency line is always logged (two pointer reads); the full object walk
// that enumerates and places every button is diagnostic only, because it resolves a name for every
// UObject in the process, which is a visible hitch on a client.
bool g_diag = false;

bool VerifyPassSafe();      // defined below; also called once before the load, as a baseline

// -- state ------------------------------------------------------------------------------------
volatile long g_busy     = 0;       // re-entrancy guard: our own ProcessEvent call comes back through
bool          g_enabled  = true;
bool          g_done     = false;   // glyph level is in for this world - stop polling
void*         g_world     = nullptr;
ULONGLONG     g_nextCheck = 0;
// Raised by the LoadInstance hook: the engine has begun streaming a level instance, so this world
// has them (the station map, never the menu). Same trigger the Quest build uses, so exercising it
// here exercises that too.
volatile long g_sawLevelInstance = 0;
ULONGLONG     g_firedAt   = 0;
void*         g_streaming = nullptr;   // the ULevelStreamingDynamic LoadLevelInstance handed back
bool          g_verified  = false;
int           g_polls     = 0;
int           g_announced = 0;      // keep the "watching" line from repeating on every travel

// The real work. Returns true when the glyph level is in (or already was).
bool GlyphPass()
{
    void* world = g_world;
    if (!world) return false;

    // Resolving is the only expensive step (it names every object), so do not grind on it forever: the
    // engine classes exist from start-up, so a few failures mean the offsets are wrong for this exe.
    if (!ResolveStatics())
    {
        static int resolveTries = 0;
        if (++resolveTries >= 10) { GLog("could not resolve the engine bits after %d tries - off", resolveTries); g_enabled = false; }
        return false;
    }

    // Cheap pass: only objects whose class is one of the cached LevelStreaming classes.
    double loc[3] = {}, rot[3] = {};
    bool found = false, already = false;
    wchar_t pkg[512];
    const int32_t num = g_objs->NumElements;
    for (int32_t i = 0; i < num; ++i)
    {
        void* o = g_objs->GetByIndex(i);
        if (!o) continue;
        const uintptr_t a = reinterpret_cast<uintptr_t>(o);
        if (!IsLsClass(*reinterpret_cast<void**>(a + kUObj_Class))) continue;

        NameText(reinterpret_cast<const void*>(a + kULS_PackageNameToLoad), pkg, 512);
        if (!pkg[0]) continue;
        if (wcsstr(pkg, kGlyphLeaf)) { already = true; break; }
        if (!found && wcsstr(pkg, kBuggedLevel))
        {
            const double* q = reinterpret_cast<const double*>(a + kULS_LevelTransform);
            const double* t = reinterpret_cast<const double*>(a + kULS_LevelTransform + 0x20);
            loc[0] = t[0]; loc[1] = t[1]; loc[2] = t[2];
            QuatToRotator(q, rot);
            found = true;
        }
    }

    if (already) { GLog("PKR_Quests_Unbugged is already streaming - nothing to do"); return true; }
    if (!found)
    {
        // PKR_Quests_01 is only resident once the player is near parkour, so waiting for it makes
        // the glyphs late or absent. Once the engine has streamed ANY level instance we know this is
        // the station map, and the placement is a fixed world-space transform, so go ahead.
        if (!g_sawLevelInstance) return false;
        GLog("no PKR_Quests_01 yet, but a level instance has streamed - using the measured placement");
        for (int i = 0; i < 3; ++i) { loc[i] = kFallbackLoc[i]; rot[i] = kFallbackRot[i]; }
    }

    // A level instance at the origin with no rotation is not a placement we ever measured, so treat it
    // as "transform not populated" and use the values read off a live server instead.
    if (loc[0] == 0.0 && loc[1] == 0.0 && loc[2] == 0.0)
    {
        GLog("PKR_Quests_01 transform is identity - using the measured fallback placement");
        for (int i = 0; i < 3; ++i) { loc[i] = kFallbackLoc[i]; rot[i] = kFallbackRot[i]; }
    }

    struct LLIParms
    {
        void*   WorldContextObject;
        FStr    LevelName;
        double  Location[3];
        double  Rotation[3];
        bool    bOutSuccess;
        uint8_t Pad49[7];
        FStr    OptionalLevelNameOverride;
        void*   OptionalLevelStreamingClass;
        bool    bLoadAsTempPackage;
        uint8_t Pad69[7];
        void*   ReturnValue;
    } p = {};
    static_assert(sizeof(LLIParms) == 0x78, "LoadLevelInstance parms layout");

    const int len = (int)wcslen(kGlyphLevel);
    p.WorldContextObject = world;
    p.LevelName.Data = const_cast<wchar_t*>(kGlyphLevel);
    p.LevelName.Num  = len + 1;
    p.LevelName.Max  = len + 1;
    for (int i = 0; i < 3; ++i) { p.Location[i] = loc[i]; p.Rotation[i] = rot[i]; }

    if (g_diag) VerifyPassSafe();                   // baseline, so "after" means something
    GLog("streaming %ls at loc=(%.0f,%.0f,%.0f) rot=(P%.1f,Y%.1f,R%.1f)",
         kGlyphLevel, loc[0], loc[1], loc[2], rot[0], rot[1], rot[2]);

    g_peOrig(g_lliCdo, g_lliFn, &p);                // call past our own hook

    GLog("LoadLevelInstance -> success=%d streaming=%p", p.bOutSuccess ? 1 : 0, p.ReturnValue);
    if (p.bOutSuccess) { g_firedAt = GetTickCount64(); g_streaming = p.ReturnValue; }
    return p.bOutSuccess;
}

// SEH wrapper: this frame holds no C++ objects needing unwinding, so /EHsc is happy with __try here.
bool GlyphPassSafe()
{
    __try { return GlyphPass(); }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        GLog("glyph pass faulted - disabling");
        g_enabled = false;
        return false;
    }
}

// The only honest answer to "did it work": walk the Outer chain of every object and list the ones
// that belong to OUR level instance, with their classes. Counting by object NAME is not enough -
// "ProgressionButton_Glyph" also matches the Blueprint class and its CDO, which exist either way.
bool InPackage(void* o, const wchar_t* needle)
{
    wchar_t n[512];
    for (int d = 0; d < 8 && o; ++d)
    {
        ObjName(o, n, 512);
        if (n[0] && wcsstr(n, needle)) return true;
        o = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + kUObj_Outer);
    }
    return false;
}

void VerifyPass(const char* when)
{
    // The name-free check first: did the streaming level actually finish loading?
    if (g_streaming)
    {
        void* lvl = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_streaming) + kULS_LoadedLevel);
        if (!lvl) GLog("  streaming->LoadedLevel is still null (not resident yet)");
        else
        {
            const int32_t nActors = *reinterpret_cast<int32_t*>(
                reinterpret_cast<uintptr_t>(lvl) + kULevel_Actors + 8);
            void* owner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(lvl) + kULevel_OwningWorld);
            GLog("  streaming->LoadedLevel=%p Actors.Num=%d OwningWorld=%p", lvl, nActors, owner);
        }
    }

    if (!g_diag) return;

    int pkg = 0, mine = 0;
    wchar_t name[512], cls[512];
    const int32_t num = g_objs->NumElements;
    for (int32_t i = 0; i < num; ++i)
    {
        void* o = g_objs->GetByIndex(i);
        if (!o) continue;
        ObjName(o, name, 512);
        if (!name[0]) continue;
        if (wcsstr(name, kGlyphLeaf)) ++pkg;
        if (wcsncmp(name, L"Default__", 9) == 0) continue;
        const bool ours = InPackage(o, kGlyphLeaf);

        // Dump EVERY glyph button in the world, ours or not. The ones the game already has are the
        // control group: if they carry the same state ours do, our spawned buttons are not the
        // reason nothing shows up.
        cls[0] = 0;
        {
            void* cc = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + kUObj_Class);
            if (cc) ObjName(cc, cls, 512);
        }
        if (cls[0] && wcsstr(cls, L"ProgressionButton_Glyph"))
        {
            void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1A8);
            const double* t = root ? reinterpret_cast<const double*>(
                                  reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20) : nullptr;
            const bool locked = *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(o) + 0x2B4);
            const uint32_t* q = reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(o) + 0x2BC);
            GLog("  BUTTON[%s] %-34ls locked=%d quest=%08X%08X%08X%08X at (%.0f,%.0f,%.0f)",
                 ours ? "ours " : "stock", name, locked ? 1 : 0, q[0], q[1], q[2], q[3],
                 t ? t[0] : 0.0, t ? t[1] : 0.0, t ? t[2] : 0.0);
        }
        if (!ours) continue;
        ++mine;
        if (mine <= 12) GLog("  in-level: %-46ls class=%ls", name, cls[0] ? cls : L"?");
    }
    GLog("verify(%s): %d object(s) named *%ls*, %d object(s) inside our level instance",
         when, pkg, kGlyphLeaf, mine);

    // Reference point: where did the level the game DOES instance (PKR_Quests_01) put its actors?
    int shown = 0;
    for (int32_t i = 0; i < num && shown < 4; ++i)
    {
        void* o = g_objs->GetByIndex(i);
        if (!o) continue;
        ObjName(o, name, 512);
        if (!name[0] || wcsncmp(name, L"Default__", 9) == 0) continue;
        if (!wcsstr(name, L"QuestDisplayKiosk") && !wcsstr(name, L"QuestArrows")) continue;
        if (!InPackage(o, kBuggedLevel)) continue;
        void* root = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1A8);
        const double* t = root ? reinterpret_cast<const double*>(
                              reinterpret_cast<uintptr_t>(root) + 0x1D0 + 0x20) : nullptr;
        GLog("  reference (PKR_Quests_01) %-30ls at (%.0f,%.0f,%.0f)", name,
             t ? t[0] : 0.0, t ? t[1] : 0.0, t ? t[2] : 0.0);
        ++shown;
    }
}
bool VerifyPassSafe()
{
    __try { VerifyPass(g_firedAt ? "after" : "before"); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { GLog("verify faulted"); return false; }
}

void GlyphTick()
{
    // A level instance belongs to the world it was streamed into, so a travel (menu -> station, or a
    // reconnect) means the glyph level has to go in again - re-arm rather than stay "done" forever.
    void* world = *reinterpret_cast<void**>(g_base + kOffGWorld);
    if (world != g_world) { g_world = world; g_done = false; g_polls = 0; }
    if (!world || g_done) return;      // VerifyTick still runs - see PE_Hook

    if (++g_polls == 1 && g_announced < 4) { ++g_announced; GLog("watching for the parkour district"); }

    if (GlyphPassSafe())
    {
        g_done = true;
        GLog("glyph level is in - idle from here");
    }
}

// Same 10s gate, but keeps running after g_done so the streaming has time to finish first.
void VerifyTick()
{
    if (g_verified || !g_firedAt || GetTickCount64() - g_firedAt < 20000) return;
    g_verified = true;
    VerifyPassSafe();
}

// -- ULevelStreamingLevelInstance::LoadInstance: the engine's own level-instance loader --------
// Only raises a flag; the work still happens on the ProcessEvent tick, on the game thread.
typedef void* (__fastcall *LoadInstance_t)(void*);
LoadInstance_t g_loadInstOrig = nullptr;

void* __fastcall LoadInstance_Hook(void* self)
{
    if (!g_sawLevelInstance) InterlockedExchange(&g_sawLevelInstance, 1);
    return g_loadInstOrig(self);
}

// -- ProcessEvent hook: the only place it is safe to touch the world (game thread) -------------
// Runs on every UFunction call in the game, so the fast path has to be exactly this: a bool, a clock
// read, and an interlocked flag. Everything else is behind the 10-second gate.
void __fastcall PE_Hook(void* ctx, void* fn, void* parms)
{
    if (g_enabled && GetTickCount64() >= g_nextCheck &&
        InterlockedCompareExchange(&g_busy, 1, 0) == 0)
    {
        g_nextCheck = GetTickCount64() + 10000;
        GlyphTick();
        VerifyTick();
        InterlockedExchange(&g_busy, 0);
    }
    g_peOrig(ctx, fn, parms);
}

} // namespace

// Called once from the worker thread, i.e. after the loader lock is released (MH_EnableHook suspends
// threads, which is not safe under it).
void GlyphFix_Start()
{
    if (wcsstr(GetCommandLineW(), L"-NoGlyphFix")) return;
    g_diag = wcsstr(GetCommandLineW(), L"-GlyphDiag") != nullptr;

    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!g_base) return;
    g_objs   = reinterpret_cast<TUObjectArray*>(g_base + kOffGObjects);
    g_append = reinterpret_cast<AppendString_t>(g_base + kOffAppendString);

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return;
    void* pe = reinterpret_cast<void*>(g_base + kOffProcessEvent);
    if (MH_CreateHook(pe, reinterpret_cast<void*>(&PE_Hook), reinterpret_cast<void**>(&g_peOrig)) != MH_OK)
    {
        GLog("ProcessEvent hook create failed - glyph fix off");
        return;
    }
    if (MH_EnableHook(pe) != MH_OK)
    {
        GLog("ProcessEvent hook enable failed - glyph fix off");
        return;
    }
    void* li = reinterpret_cast<void*>(g_base + kOffLoadInstance);
    if (MH_CreateHook(li, reinterpret_cast<void*>(&LoadInstance_Hook),
                      reinterpret_cast<void**>(&g_loadInstOrig)) == MH_OK && MH_EnableHook(li) == MH_OK)
        GLog("LoadInstance hooked @%p", li);
    else
        GLog("LoadInstance hook failed - falling back to waiting for PKR_Quests_01");
    GLog("armed (the parkour glyph level will stream in when the district loads)");
}
