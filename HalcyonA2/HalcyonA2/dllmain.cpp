// dllmain.cpp — HalcyonA2 bootstrap.
//
// Milestone 1: open the real map, drop local player 0, and flip the client
// into server mode. This game auto-listens once GIsServer/GIsClient are set,
// so no manual net-driver / InitListen code is needed (unlike the Fortnite
// reference project, see CLAUDE.md §4.2).
#include "pch.h"
#include "MinHook.h"
#include <thread>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <cstdarg>
#include <cmath>
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include <iphlpapi.h>

// [PROF] Full-GObjects-walk accounting (declared early: walk sites start well before the profiler).
static volatile long g_walks = 0;
static volatile long g_objN  = 0;
static volatile long g_rebuilds = 0;   // [PROF] cache-rebuild count (should be rare)
#pragma comment(lib, "iphlpapi.lib")

// ---------------------------------------------------------------------------
// Globals located in IDA (RVAs from module base). Both are adjacent bool bytes.
//   GIsClient : RVA 0x9650702
//   GIsServer : RVA 0x9650703
// ---------------------------------------------------------------------------
static constexpr uintptr_t GIsClient_RVA = 0x96D8672;  // [22284] was 0x9650702
static constexpr uintptr_t GIsServer_RVA = 0x96D8673;  // [22284] was 0x9650703

// ---------------------------------------------------------------------------
// Station ID. The server-login path that would normally populate this was
// compiled out of the client build, so this global has no writer in the image.
// It is a UE FString stored inline (a TArray<wchar_t>); the readers
// sub_541CB00 / sub_541C1B0 use it as the {stationId} in the REST path
// "/stations/{stationId}/users/{uid}/roles".
//   0x95DF980 : wchar_t* Data      ("qword_95579C0")
//   0x95DF988 : int32    ArrayNum  ("dword_95579C8" -- the "is set" / length check)
//   0x95DF98C : int32    ArrayMax
// ArrayNum counts the null terminator (empty FString -> Num 0 -> "not set").
// ---------------------------------------------------------------------------
static constexpr uintptr_t StationStr_Data_RVA = 0x95DF980;
static constexpr uintptr_t StationStr_Num_RVA  = 0x95DF988;
static constexpr uintptr_t StationStr_Max_RVA  = 0x95DF98C;

// The station id now comes from the backend at spin-up (POST /register_server).
// g_stationId holds it for the process lifetime so the pointer handed to the game
// stays valid; kStationFallback is used only if registration fails.
static const wchar_t kStationFallback[] = L"2832873737dsfhjbsdbds3832";
static std::wstring   g_stationId;
static std::wstring   g_deploymentIdW;   // -DashboardDeploymentId (unique per running server); used to scope the Vivox channel per-server
static std::string    g_deploymentId;    // same id, narrow — the key for the periodic player-count heartbeat (POST /update_player_count)

// Backend (Astra emulator) host the DLL registers itself with — POST /register_server and the periodic
// POST /update_player_count. Overridable: -BackendHost=<host> -BackendPort=<n>.
// [2026-09-08] Default changed from the hardcoded literal 157.173.194.216 (which is NOT this project's
// server — servers were registering with, and pulling quests from, a THIRD PARTY's backend) to the
// project's own Cloudflare-fronted host. Those domains answer on 443 ONLY (Cloudflare does not proxy
// :90/:78 — they time out), which is why a raw IP was used before; HttpPostLocal/HttpReq now set
// WINHTTP_FLAG_SECURE for port 443 so the real domain works.
static std::wstring g_backendHost = L"rigel.wwiggles.org";
static int          g_backendPort = 443;   // A2StationDbServer behind Cloudflare/Caddy TLS
#define kBackendHost (g_backendHost.c_str())
#define kBackendPort (g_backendPort)

// The A2 client build this DLL runs on. Reported to the backend in /register_server so it stamps our
// DB row + EOS session (Bucket/BuildId) with the RIGHT build — the client's server search matches on
// build, so this MUST equal the client's build or our servers get filtered out of the browser.
static const char* kBuildVersion = "22284";

static uintptr_t GetBase()
{
    return reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
}

// ---- GC null-reference guard (VEH) --------------------------------------------------------------
// The engine's parallel GC cluster-reference pass batches UObject references and bulk-dereferences
// them with NO null check (worker-thread fns sub_124b9d0 / sub_124db00 / sub_124ece0; they radix-sort
// the batched refs to test which point into a cluster's [base,size) memory). On our headless -nullrhi
// server some component/entity reference is null, so the deref AVs on a Background/Foreground Worker
// thread (reading address 0) — which ProcessEvent-side SEH can't reach. A null reference belongs to no
// cluster, so the CORRECT behaviour is simply to skip it; that's the null-check UE's fast path is
// missing. On a near-null READ fault inside exactly those three functions this VEH repoints the
// faulting memory operand's BASE register at a zero page and resumes, so the load reads 0 (== skip).
// Bisected: independent of spectator AND ballsim; this is the long-standing project-history GC crash
// (see memory a2-random-crash). Scoped to 3 function ranges so no unrelated AV is ever masked.
static void*     g_gcZeroPage = nullptr;
static uintptr_t g_gcRanges[6] = {0,0,0,0,0,0}; // [f1s,f1e, f2s,f2e, f3s,f3e]

static LONG CALLBACK GcNullRefVeh(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (er->NumberParameters < 2 || er->ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH; // read only
    // NO fault-address range check. The bad reference is a2 = objectBase(null/dead) + fieldOffset,
    // and fieldOffset can be ANY size as the walk steps through the class's reference fields — a
    // threshold just whack-a-moles (the crash climbed 0x0 -> 0x1000 -> 0x10000, tracking each cutoff).
    // Inside these three dedicated GC ref-collector functions every read AV is a dead reference to
    // skip, so we repoint the faulting base reg -> zero page regardless of the address. The RIP-range
    // scope + read-only + anti-loop checks keep this from masking anything unrelated.
    const uintptr_t fault = static_cast<uintptr_t>(er->ExceptionInformation[1]); (void)fault;
    const uintptr_t rip = ep->ContextRecord->Rip;
    if (!((rip >= g_gcRanges[0] && rip < g_gcRanges[1]) ||
          (rip >= g_gcRanges[2] && rip < g_gcRanges[3]) ||
          (rip >= g_gcRanges[4] && rip < g_gcRanges[5])))
        return EXCEPTION_CONTINUE_SEARCH;
    // Decode the faulting instruction's ModRM memory operand BASE register. Opcode-agnostic: the base
    // register comes from ModRM/SIB the same way for mov(8B)/cmp(39)/add(03)/test(85)/movzx(0F B6)/...
    // so we only need to locate the ModRM byte (after prefixes + 1- or 2-byte opcode).
    const uint8_t* p = reinterpret_cast<const uint8_t*>(rip);
    int i = 0;
    while (p[i]==0x66||p[i]==0x67||p[i]==0xF0||p[i]==0xF2||p[i]==0xF3||
           p[i]==0x2E||p[i]==0x3E||p[i]==0x26||p[i]==0x64||p[i]==0x65||p[i]==0x36) ++i;  // legacy prefixes
    uint8_t rexB = 0;
    if ((p[i] & 0xF0) == 0x40) { rexB = static_cast<uint8_t>(p[i] & 0x01); ++i; }        // REX (B extends base)
    if (p[i] == 0x0F) i += 2; else i += 1;                                               // 2- vs 1-byte opcode
    const uint8_t modrm = p[i];
    const int mod = (modrm >> 6) & 3;
    const int rm  = modrm & 7;
    if (mod == 3) return EXCEPTION_CONTINUE_SEARCH;   // register operand, no memory access
    int baseReg;
    if (rm == 4) {                                     // SIB byte follows ModRM
        const uint8_t sib = p[i + 1];
        if ((sib & 7) == 5 && mod == 0) return EXCEPTION_CONTINUE_SEARCH; // disp32, no base reg
        baseReg = (sib & 7) | (rexB << 3);
    } else if (rm == 5 && mod == 0) {
        return EXCEPTION_CONTINUE_SEARCH;              // RIP-relative, no base reg
    } else {
        baseReg = rm | (rexB << 3);
    }
    if (baseReg == 4 || baseReg == 5) return EXCEPTION_CONTINUE_SEARCH; // never touch RSP/RBP
    DWORD64* regs[16] = {
        &ep->ContextRecord->Rax, &ep->ContextRecord->Rcx, &ep->ContextRecord->Rdx, &ep->ContextRecord->Rbx,
        &ep->ContextRecord->Rsp, &ep->ContextRecord->Rbp, &ep->ContextRecord->Rsi, &ep->ContextRecord->Rdi,
        &ep->ContextRecord->R8,  &ep->ContextRecord->R9,  &ep->ContextRecord->R10, &ep->ContextRecord->R11,
        &ep->ContextRecord->R12, &ep->ContextRecord->R13, &ep->ContextRecord->R14, &ep->ContextRecord->R15 };
    if (*regs[baseReg] == reinterpret_cast<DWORD64>(g_gcZeroPage))
        return EXCEPTION_CONTINUE_SEARCH;  // already repointed once & still faulting -> real fault, don't loop
    *regs[baseReg] = reinterpret_cast<DWORD64>(g_gcZeroPage);
    static volatile LONG s_cnt = 0;
    LONG n = InterlockedIncrement(&s_cnt);
    if (n == 1)
    {
        // The corrupt array is at RDI+0x3C8 (from the disasm of sub_7FF6732AF4C0). RDI = the object being
        // GC-scanned. Log RDI, its vtable (RVA -> identify the class in IDA), and RDI+8/+0x10 (the collector's
        // valid-object address range) so we know EXACTLY what object has the corrupt reference descriptor.
        // ★ Capture the CULPRIT OBJECT live. The GC batcher sub_7FF6732AF4C0 is called by the token
        // interpreter sub_7FF6732C1A80 as `sub_7FF6732AF4C0(v2 + 0x180)`, so at fault time RDI = v2+0x180,
        // i.e. the interpreter's context v2 = RDI - 0x180. The interpreter stores the CURRENT object it's
        // GC-walking at v2+0x28, and the array-token Num it read at v2+0x88 (case 9). Read those + the
        // object's vtable (RVA) so we can name the exact class/object whose array is corrupt on the server.
        const uintptr_t base = GetBase();
        uintptr_t rdi  = ep->ContextRecord->Rdi;
        uintptr_t v2   = rdi - 0x180;
        uintptr_t obj = 0, objVt = 0, arrData = 0; int arrNum = -1;
        __try { obj     = *reinterpret_cast<uintptr_t*>(v2 + 0x28); } __except (EXCEPTION_EXECUTE_HANDLER) { obj = 0xBAD; }
        __try { if (obj && obj != 0xBAD) objVt = *reinterpret_cast<uintptr_t*>(obj); } __except (EXCEPTION_EXECUTE_HANDLER) { objVt = 0xBAD; }
        __try { arrData = *reinterpret_cast<uintptr_t*>(v2 + 0x80); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        __try { arrNum  = *reinterpret_cast<int*>(v2 + 0x88); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        printf("[HalcyonA2][GC-GUARD] FIRST fault: rip=+0x%llX RDI=0x%llX  CULPRIT obj=0x%llX vtbl(rva)=+0x%llX arrData=0x%llX arrNum=%d\n",
               (unsigned long long)(rip - base), (unsigned long long)rdi,
               (unsigned long long)obj,
               (unsigned long long)((objVt > base && objVt < base + 0x0C000000) ? objVt - base : objVt),
               (unsigned long long)arrData, arrNum);
        // ★★ ROOT-CAUSE PIN. Dump the culprit's UObject header, then locate the corrupt array TWO ways
        // (robust across fault sites — the captured arrData/arrNum offsets vary run-to-run):
        //   (A) if arrData is a real field of obj, report its offset (maps to the class layout);
        //   (B) scan obj+0..0x800 for ANY TArray-shaped field with a HUGE Num (>=1M, the flood driver):
        //       Data==0 or Data<4GB (garbage) paired with Num in [0x100000, 0x7FFFFFFF]. That's the array
        //       the token interpreter walks Num times = the flood. Its offset names the exact property.
        if (obj && obj != 0xBAD)
        {
            __try
            {
                uint32_t objFlags = *reinterpret_cast<uint32_t*>(obj + 0x08);
                int32_t  objIndex = *reinterpret_cast<int32_t*>(obj + 0x0C);
                uintptr_t clsPtr  = *reinterpret_cast<uintptr_t*>(obj + 0x10);
                uintptr_t clsVt   = clsPtr ? *reinterpret_cast<uintptr_t*>(clsPtr) : 0;
                uint32_t nameCmp  = *reinterpret_cast<uint32_t*>(obj + 0x18);
                printf("[HalcyonA2][GC-GUARD]     HDR flags=0x%08X internalIdx=%d class=0x%llX clsVtbl(rva)=+0x%llX nameRaw=0x%08X\n",
                       objFlags, objIndex, (unsigned long long)clsPtr,
                       (unsigned long long)((clsVt > base && clsVt < base + 0x0C000000) ? clsVt - base : clsVt), nameCmp);
                if (arrData)
                {
                    for (int off = 0; off <= 0x800; off += 8)
                        if (*reinterpret_cast<uintptr_t*>(obj + off) == arrData)
                        {
                            printf("[HalcyonA2][GC-GUARD]     ARR(A) matches arrData at obj+0x%X\n", off); break;
                        }
                }
                // (B) huge-Num scan — the real flood driver
                int hits = 0;
                for (int off = 0; off <= 0x800 && hits < 4; off += 8)
                {
                    uintptr_t d = *reinterpret_cast<uintptr_t*>(obj + off);
                    int32_t   n = *reinterpret_cast<int32_t*>(obj + off + 8);
                    if ((d == 0 || d < 0x100000000ull) && n >= 0x100000 && n <= 0x7FFFFFFF)
                    {
                        int32_t mx = *reinterpret_cast<int32_t*>(obj + off + 12);
                        printf("[HalcyonA2][GC-GUARD]     ★ HUGE-NUM ARRAY at obj+0x%X : Data=0x%llX Num=%d Max=%d  <-- map to class layout\n",
                               off, (unsigned long long)d, n, mx);
                        ++hits;
                    }
                }
                if (!hits)
                    printf("[HalcyonA2][GC-GUARD]     (no huge-Num TArray field in obj+0..0x800 — corruption is nested/indirect; obj itself may be the array's owner-of-owner)\n");
            }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
        // Also dump a window of the interpreter context so we can spot the object/array if offsets differ.
        for (int off = 0x18; off <= 0x98; off += 8)
        {
            uintptr_t val = 0;
            __try { val = *reinterpret_cast<uintptr_t*>(v2 + off); } __except (EXCEPTION_EXECUTE_HANDLER) { continue; }
            uintptr_t vt2 = 0;
            if (val > base && val < base + 0x0C000000)
                printf("[HalcyonA2][GC-GUARD]     v2+0x%X = +0x%llX (image)\n", off, (unsigned long long)(val - base));
            else if (val > 0x100000000ull)
            {
                __try { vt2 = *reinterpret_cast<uintptr_t*>(val); } __except (EXCEPTION_EXECUTE_HANDLER) { vt2 = 0; }
                printf("[HalcyonA2][GC-GUARD]     v2+0x%X = 0x%llX vtbl(rva)=+0x%llX\n", off, (unsigned long long)val,
                       (unsigned long long)((vt2 > base && vt2 < base + 0x0C000000) ? vt2 - base : vt2));
            }
            else
                printf("[HalcyonA2][GC-GUARD]     v2+0x%X = 0x%llX\n", off, (unsigned long long)val);
        }
    }
    if (n == 1 || (n % 250000) == 0)
        printf("[HalcyonA2][GC-GUARD] skipped null GC ref (#%ld) rip=+0x%llX reg=%d faultAddr=0x%llX\n",
               n, (unsigned long long)(rip - GetBase()), baseReg, (unsigned long long)fault);
    return EXCEPTION_CONTINUE_EXECUTION;  // re-run the load; base now -> zero page -> reads 0 -> skipped
}

// ─────────────────────────────────────────────────────────────────────────────
// [2026-09-02] PAWN-SPAWN ARCHETYPE-COPY CRASH GUARD (VEH). When a real remote VR player joins, the
// server spawns their VR pawn: sub_7FF677458340 (VR spawn+possess) -> SpawnActor sub_7FF675B1B120 ->
// deep in construction the TArray<UObject*> element relocate sub_7FF673386AD0 copies from a BAD ARCHETYPE
// pointer (half-loaded BP: the boot log's "Could not find template object for CosmeticLoadout / Default
// Settings" errors). The faulting instruction is `mov rdi,[r14+rbx]` at RVA 0x1326B07 (bytes 49 8B 3C 1E,
// 4 long): [r14+rbx] = source element, where r14 is a DELTA (src_base-dst_base) and rbx the real dst
// pointer -> so GcNullRefVeh's base-repoint does NOT apply here (repointing r14 would move the read to
// zeropage+rbx = still unmapped). Frame-SEH can't help: UE's own vectored handler fires first. FIX: on an
// AV at exactly this instruction, load 0 into rdi and step Rip over the 4-byte load. The loop then skips
// the AddRef (guarded by `test rdi,rdi; jz`) and stores null for that element -> the pawn spawns with a
// null archetype element instead of crashing. Exact-RIP match => cannot mask any other fault. Decoded
// with the CORRECT boot base 0x7FF69F360000 (the crash callstack bottoms out cleanly at WinMain).
static uintptr_t g_pawnSpawnCrashRip = 0;  // set at boot = base + 0x1326B07
static bool      g_pawnSpawnGuard    = false;  // [2026-09-02] OFF — superseded by ArrCopyDrv_Hook (root fix: sanitize corrupt Num at the driver, not skip the leaf fault). Emergency fallback only.
static LONG CALLBACK PawnSpawnCrashVeh(EXCEPTION_POINTERS* ep)
{
    EXCEPTION_RECORD* er = ep->ExceptionRecord;
    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    if (er->NumberParameters < 2 || er->ExceptionInformation[0] != 0) return EXCEPTION_CONTINUE_SEARCH; // read only
    if (!g_pawnSpawnCrashRip || ep->ContextRecord->Rip != g_pawnSpawnCrashRip) return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG s_cnt = 0;
    LONG n = InterlockedIncrement(&s_cnt);
    if (n == 1)
    {
        // The >0x100000 clamp on rsi did NOT stop the grind -> this is NOT one call with a huge inner
        // count; the relocate is being called from a corrupt-huge OUTER loop. Dump rsi (inner count),
        // the fault addr, and the first 2 return addresses on the stack (the callers) so we can find and
        // clamp the OUTER loop instead. SEH-guard the stack scan.
        const uintptr_t base = GetBase();
        const uintptr_t imgEnd = base + 0x0A5BB000;
        uintptr_t rsp = ep->ContextRecord->Rsp;
        uintptr_t c1 = 0, c2 = 0, c3 = 0;
        __try {
            for (int i = 0; i < 96; ++i) {
                uintptr_t v = *reinterpret_cast<uintptr_t*>(rsp + (uintptr_t)i * 8);
                if (v > base && v < imgEnd) {
                    if (!c1) c1 = v; else if (!c2) c2 = v; else { c3 = v; break; }
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
        printf("[HalcyonA2][PAWN-GUARD] FIRST fault: rsi(count)=%llu faultAddr=0x%llX  callers(rva): +0x%llX +0x%llX +0x%llX\n",
               (unsigned long long)ep->ContextRecord->Rsi,
               (unsigned long long)er->ExceptionInformation[1],
               (unsigned long long)(c1 ? c1 - base : 0),
               (unsigned long long)(c2 ? c2 - base : 0),
               (unsigned long long)(c3 ? c3 - base : 0));
    }
    ep->ContextRecord->Rdi = 0;      // unreadable archetype element -> treat as null
    // Aggressively bound the inner loop: cap remaining iters to 64 for ANY faulting relocate (a genuine
    // relocate that faults on its source is already garbage). This alone can't stop a corrupt OUTER loop,
    // but it minimizes wasted work per call while we identify the outer loop from the caller RVAs above.
    if (ep->ContextRecord->Rsi > 64) ep->ContextRecord->Rsi = 64;
    ep->ContextRecord->Rip += 4;     // step over `mov rdi,[r14+rbx]` (49 8B 3C 1E)
    if (n == 1 || (n % 1000000) == 0)
        printf("[HalcyonA2][PAWN-GUARD] skipped bad archetype element (#%ld) in pawn-spawn relocate (sub_7FF673386AD0)\n", n);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// ★ ROOT FIX for the remote-join crash+grind (replaces the PAWN-GUARD VEH above, which only skipped the
// leaf fault and could not stop the outer loop). Diagnosis from the live FIRST-fault dump:
//   rsi(count)=1  faultAddr=0x0  callers +0x12F0F01 (sub_7FF673350CB0) ...
// The leaf sub_7FF673386AD0 (FObjectProperty::CopyValuesInternal) reads [src] and faulted at 0x0 => its
// src pointer is NULL. Its only real caller is sub_7FF673350CB0 (RVA 0x12F0CB0), FArrayProperty's element-
// copy driver: `for (i = Num; i; --i) (*(vtable+0xF0))(prop, dst, src=Data, ArrayDim)`, where
// Num = *(int*)(src+8) and Data = *(void**)src. A half-loaded BP archetype (the "Could not find template
// object for CosmeticLoadout / Default Settings" failed exports) leaves a property's SOURCE FScriptArray
// corrupt: { Data == 0, Num == huge }. So the driver loops Num (~78M) times, each reading [Data]=[0] ->
// AV at 0x0 (the crash) AND a multi-minute grind. The bad Num is read one frame ABOVE the leaf, which is
// why clamping the leaf's inner rsi did nothing. FIX at the source of the bad count (same idea as
// InstallGcNumClamp): before the copy, if src's Data is null (nothing to copy) or Num is outside any sane
// range, zero Num -> the copy becomes a clean empty no-op. No fault, no grind. Legit arrays (Data != 0,
// Num in range) are untouched. Called via vtable, so MinHook on the body catches every dispatch.
using ArrCopyDrv_t = unsigned char (__fastcall*)(void* prop, void* dst, void* src);
static ArrCopyDrv_t ArrCopyDrv_Orig = nullptr;
static unsigned char __fastcall ArrCopyDrv_Hook(void* prop, void* dst, void* src)
{
    if (src)
    {
        void* data = *reinterpret_cast<void**>(src);
        int*  pNum = reinterpret_cast<int*>(reinterpret_cast<char*>(src) + 8);
        int   num  = *pNum;
        if (num != 0 && (data == nullptr || (unsigned)num > 0x1000000u))
        {
            static volatile LONG s_arrfix = 0;
            LONG k = InterlockedIncrement(&s_arrfix);
            if (k <= 12)
                printf("[HalcyonA2][ARRFIX] corrupt source array {Data=%p Num=%d} -> Num=0 (half-loaded archetype property copy)\n",
                       data, num);
            *pNum = 0;   // treat as empty: nothing to copy (was garbage)
        }
    }
    return ArrCopyDrv_Orig(prop, dst, src);
}

// [2026-09-02 ★ COSMETIC JOIN-CRASH FIX] Joining on a VR pawn AV'd with EXCEPTION_ACCESS_VIOLATION
// reading 0x82. Callstack (base 0x7FF69F360000): APlayerRenderer::ApplyCosmeticMeshMetadata_Impl ->
// sub_7FF6774742B0 -> sub_7FF677453260 -> sub_7FF677452EE0 -> sub_7FF67743A880 (RVA 0x53DA880) ->
// FName::ToString (sub_7FF6731AD690) [FAULT]. sub_7FF67743A880 is a cosmetic-mesh entry comparator run
// in a loop: for the joining pawn's skeletal mesh it walks PlayerRenderer's cosmetic-mesh entry list and
// for each entry calls ToString on the entry sub-object's UObject::NamePrivate (FName @ *(entry+8)+24) and
// on the mesh's own name (FName @ *meshptr+24). A half-loaded cosmetic archetype (same CosmeticLoadout
// family as ArrCopyDrv_Hook) leaves an entry whose sub-object carries a GARBAGE FName: its name-pool block
// index resolves to a NULL block pointer, so ToString computes Blocks[block](=0)+offset*2 (~0x82) and AVs.
// FIX: validate both operands' FNames against the real name pool (stru_7FF67B770B80: block = handle>>16
// must be < FNameMaxBlocks(0x2000) AND Blocks[block] != null). A corrupt entry is reported as "no match"
// (return 0) so the caller's loop skips it and falls through to its create-new path -- never stringifying
// garbage. Install after MH_Initialize (like ArrCopyDrv_Hook). No AV -> UE's frame VEH is never engaged.
static constexpr uintptr_t CosmeticCmp_RVA  = 0x53DA880;   // sub_7FF67743A880 (mesh-entry comparator)
static constexpr uintptr_t FNamePoolTbl_RVA = 0x9710B80;   // stru_7FF67B770B80 (name-pool block table)
using CosmeticCmp_t = unsigned char (__fastcall*)(void* a1, void* a2);
static CosmeticCmp_t CosmeticCmp_Orig = nullptr;
static inline bool CosmeticFNameBad(void* obj)
{
    if (!obj) return true;
    static const uintptr_t s_pool = GetBase() + FNamePoolTbl_RVA;  // cache: no GetModuleHandle per call
    uint32_t handle = *reinterpret_cast<uint32_t*>(reinterpret_cast<char*>(obj) + 24); // UObject::NamePrivate
    uint32_t block  = handle >> 16;
    if (block >= 0x2000) return true;                              // FNameMaxBlocks -> out of range = corrupt
    void* blk = *reinterpret_cast<void**>(s_pool + 0x10 + static_cast<size_t>(block) * 8); // Blocks[block]
    return blk == nullptr;                                         // NULL block ptr = the crash condition
}
static unsigned char __fastcall CosmeticCmp_Hook(void* a1, void* a2)
{
    void* eA2 = a2 ? *reinterpret_cast<void**>(reinterpret_cast<char*>(a2) + 8) : nullptr; // entry sub-object
    void* eA1 = a1 ? *reinterpret_cast<void**>(a1) : nullptr;                              // mesh ptr
    if (CosmeticFNameBad(eA2) || CosmeticFNameBad(eA1))
    {
        static volatile LONG s_cos = 0;
        LONG k = InterlockedIncrement(&s_cos);
        if (k <= 12)
            printf("[HalcyonA2][COSFIX] corrupt cosmetic-mesh entry FName (eA1=%p eA2=%p) -> skip (no-match) (half-loaded CosmeticLoadout)\n", eA1, eA2);
        return 0;   // treat as non-match: caller skips this entry (never ToString's the garbage FName)
    }
    return CosmeticCmp_Orig(a1, a2);
}

// [2026-09-03 ★ SCRAPRUN GATE FIX] The deathrun2 / "ScraprunPrime" serverOnly gamemode.luau opens the
// scraprun area (drops ScraprunBlocker) only when Gamemode:getBoolConfigVariable("bScraprunOpen") is true,
// re-checking on Gamemode.onConfigChanged. That reads the MODULE's OWN merged config — loaded from the level
// definition (moduleState.Config, default false) by the level loader sub_7FF676760210 — via
// UModuleStateLuaAPI::GetBoolConfigVariable impl sub_7FF67673A020 (RVA 0x46DA020). It is a DIFFERENT namespace
// from the station /config NetVars (StationAnnouncement/boards): confirmed via the server_events netvars dump
// (bScraprunOpen landed in the world `config` block, while the ScraprunPrime module's DashboardConfigOverrides
// stayed {}), so the backend station-config route can NOT reach it. FIX server-side: when the requested config
// var name is "bScraprunOpen", return true; every other var passes through untouched. Authoritative on our
// (client-as-)server, no backend dependency; onConfigChanged re-reads it so the wall drops live.
static constexpr uintptr_t GetBoolCfg_RVA = 0x46DA020;   // sub_7FF67673A020 (UModuleStateLuaAPI::GetBoolConfigVariable impl)
using GetBoolCfg_t = unsigned char (__fastcall*)(void* ctx, void* nameFStr, __int64 a3, __int64 a4);
static GetBoolCfg_t GetBoolCfg_Orig = nullptr;
static unsigned char __fastcall GetBoolCfg_Hook(void* ctx, void* nameFStr, __int64 a3, __int64 a4)
{
    if (nameFStr)
    {
        const wchar_t* data = *reinterpret_cast<const wchar_t**>(nameFStr);           // FString.Data
        int num = *reinterpret_cast<int*>(reinterpret_cast<char*>(nameFStr) + 8);     // FString.Num (incl. null)
        if (data && num >= 13 && wcsncmp(data, L"bScraprunOpen", 13) == 0)
        {
            static volatile LONG s_scr = 0;
            if (InterlockedIncrement(&s_scr) <= 4)
                printf("[HalcyonA2][SCRAPRUN] getBoolConfigVariable(\"bScraprunOpen\") -> forced TRUE (scraprun open)\n");
            return 1;
        }
    }
    return GetBoolCfg_Orig(ctx, nameFStr, a3, a4);
}

// [2026-09-03 ★ DEATHRUN/SCRAPRUN MATCH-STATE DIAG] Log every match-state transition. Every Luau
// SwitchState() -> GameStateManager:updateGameState(n) routes through
// UGameStateManagerComponent::UpdateGameState_Implementation(this, uint8 newState) (RVA 0x53CB6D0),
// which sets the module's "currentState" NetVar. The deathrun/scraprun conductor's own log() calls are
// all commented out, so this is the only way to SEE the state sequence. Symptom: gamemode starts then
// snaps back to WAITING_TO_START while the timer keeps ticking -> capture the exact order+timing of the
// premature bail (expected buggy path on a headless server: GAME_BEGIN(3) -> ... -> GAME_EXIT(10) ->
// BEGIN_PLAY(14) -> WAITING_TO_START(1)). Deathrun2 state ids: 0 NOT_RUNNING, 1 WAITING_TO_START,
// 2 GAME_BEGIN_ENTER, 3 GAME_BEGIN, 5 RUNNING, 6 TEAM_SCORED_ENTER, 7 TEAM_SCORED, 10 GAME_EXIT,
// 12 OVERTIME_COUNTDOWN, 13 OVERTIME_RUNNING, 14 BEGIN_PLAY.
static constexpr uintptr_t UpdGameState_RVA = 0x53CB6D0;   // UGameStateManagerComponent::UpdateGameState_Implementation
using UpdGameState_t = __int64 (__fastcall*)(void*, unsigned char);
static UpdGameState_t UpdGameState_Orig = nullptr;
// [2026-09-03 ★ DEATHRUN GAME_BEGIN->RUNNING WATCHDOG] The deathrun/scraprun conductor's GAME_BEGIN(3)
// starts a 10s GameTimer countdown, but the GameTimeComponent is pure NetVar state (ClockStartedAt/
// ClockPunishment) whose onCountdownEnd is only broadcast from the component's TickComponent — which does
// NOT run on our headless -nullrhi server. So countdownFinishedDirections[GAME_BEGIN]->RUNNING never fires
// and the match freezes at GAME_BEGIN while the clock keeps ticking client-side. FIX: stamp each GSM's
// entry into GAME_BEGIN(3); the ticker (WatchdogGameBegin) drives it to RUNNING(5) after ~11s (the 10s
// countdown + slack). Keyed by GSM ptr. NOTE: this sets the currentState NetVar via the orig; it does not
// re-run the Luau SwitchState onEnter — if RUNNING-entry side effects (DisableForcefields/EnableGoals) turn
// out to be needed, upgrade to broadcasting the GameTimer onCountdownEnd delegate instead.
static std::unordered_map<void*, unsigned long long> g_gameBeginAt;   // GSM -> GetTickCount64() at GAME_BEGIN entry
static bool g_endTriggerFired = false;   // deathrun finish->overtime latch; re-armed at each GAME_BEGIN (see DetectRunnerAtFinish)
// [SCRAPRUN] defined after g_gameTimers; dumps every learned clock when a match ENDS, so the
// next 50/50 auto-end is captured with the timer state that caused it.
static void DumpGameTimersOnEnd(unsigned char newState);
static __int64 __fastcall UpdGameState_Hook(void* self, unsigned char newState)
{
    static const char* const kNm[] = {
        "NOT_RUNNING","WAITING_TO_START","GAME_BEGIN_ENTER","GAME_BEGIN","GAME_START_COUNTDOWN",
        "RUNNING","TEAM_SCORED_ENTER","TEAM_SCORED","?8","?9","GAME_EXIT","?11",
        "OVERTIME_COUNTDOWN","OVERTIME_RUNNING","BEGIN_PLAY" };
    const char* s = (newState < 15) ? kNm[newState] : "?";
    printf("[HalcyonA2][GSMSTATE] mgr=%p -> %u (%s)\n", self, static_cast<unsigned>(newState), s);
    // 0 NOT_RUNNING / 6 TEAM_SCORED_ENTER / 7 TEAM_SCORED / 10 GAME_EXIT are the ways a match ends.
    // Capture what every learned clock looked like at that instant.
    if (newState == 0 || newState == 6 || newState == 7 || newState == 10)
        DumpGameTimersOnEnd(newState);
    if (newState == 3)                    // entered GAME_BEGIN -> arm the countdown watchdog
    {
        g_gameBeginAt[self] = GetTickCount64();
        g_endTriggerFired = false;        // new match -> re-arm the finish->overtime detector
    }
    else
        g_gameBeginAt.erase(self);        // left GAME_BEGIN (advanced naturally or reset) -> disarm
    return UpdGameState_Orig(self, newState);
}
// Ticker: any GSM that has sat at GAME_BEGIN(3) past the ~10s countdown is force-advanced to RUNNING(5).
static void WatchdogGameBegin()
{
    if (g_gameBeginAt.empty()) return;
    unsigned long long now = GetTickCount64();
    for (auto it = g_gameBeginAt.begin(); it != g_gameBeginAt.end(); )
    {
        if (now - it->second >= 11000)    // 10s countdown + 1s slack
        {
            printf("[HalcyonA2][GSMSTATE] mgr=%p stuck at GAME_BEGIN ~11s -> forcing RUNNING(5)\n", it->first);
            if (UpdGameState_Orig) UpdGameState_Orig(it->first, 5);
            it = g_gameBeginAt.erase(it);
        }
        else ++it;
    }
}
static void SafeWatchdogGameBegin() { __try { WatchdogGameBegin(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-03 ★ GAMETIMECOMPONENT TICK DRIVER — the real fix for the GAME_BEGIN stall]
// UGameTimeComponent::TickComponent (vtable slot 121 = sub_7FF677428AF0, RVA 0x53C8AF0) is what
// actually broadcasts onCountdownEnd / onTimerStopped: it computes elapsed-vs-countdown from the
// ClockStartedAt/ClockPunishment NetVars and, on the crossing edge, fires the Luau event — gated on
// GetNetMode()!=3 (so it DOES fire on our dedicated server). It simply never runs headless (component
// tick is disabled), which is why the Luau's countdownFinishedDirections[GAME_BEGIN]->RUNNING (and the
// main-timer OnTimerFinished, overtime, team-scored countdowns) never advance. FIX: learn the deathrun
// GameTimeComponent instance by hooking StartTimerWithCountdown, then call its TickComponent every frame
// from our ticker. That runs the full Luau SwitchState path (real onEnter -> DisableForcefields) instead
// of the NetVar-only shortcut. The 11s WatchdogGameBegin above stays as a pure fallback: if this tick
// fires first, updateGameState(5) -> UpdGameState_Hook disarms the watchdog before it triggers.
static constexpr uintptr_t StartTimerCd_RVA = 0x53C4060;   // UGameTimeComponent::StartTimerWithCountdown_Implementation
static constexpr uintptr_t GtcTick_RVA      = 0x53C8AF0;   // UGameTimeComponent::TickComponent (vtable slot 121)
static constexpr uintptr_t GtcVft_RVA       = 0x84E6D20;   // UGameTimeComponent_VFT (liveness validation)
using StartTimerCd_t = __int64 (__fastcall*)(void*, double);
using GtcTick_t      = void (__fastcall*)(void*);
static StartTimerCd_t StartTimerCd_Orig = nullptr;
static std::unordered_set<void*> g_gameTimers;             // live UGameTimeComponent instances to drive
// [2026-09-09 *** SCRAPRUN 50/50 AUTO-END] UGameTimeComponent field offsets (SDK 22284,
// A2_classes.hpp:2155): ClockStartedAt@0x478, IsMainCountdown@0x488, IsSecondaryCountdown@0x489,
// ClockEndLength@0x490, ClockPunishment@0x4B0, OnCountdownEnd@0x520.
//
// Why this matters: we drive TickComponent from OUR ProcessEvent dispatch, which is a DIFFERENT
// point in the frame than the engine's own component tick. ScrapRun restarts a timer when a player
// dies/respawns. If we tick in the window where ClockStartedAt has been rewritten but the rest of
// the clock fields have not, the computed remaining momentarily reads <= 0 and TickComponent
// broadcasts OnCountdownEnd -> the Luau conductor takes countdownFinishedDirections[currentState]
// -> the match ends. Whether we land inside that window is a coin flip, which is exactly the
// reported "50/50 chance the gamemode auto-ends on respawn".
//
// The set also only ever dropped entries on a vtable mismatch, so STOPPED timers were driven
// forever -- a second way to fire a spurious countdown end.
static const uintptr_t GTC_ClockStartedAt = 0x478;
static const uintptr_t GTC_IsMainCd       = 0x488;
static const uintptr_t GTC_IsSecondaryCd  = 0x489;
static const uintptr_t GTC_ClockEndLength = 0x490;
static bool g_gateTimers = true;            // -NoTimerGate restores the old drive-everything behaviour
static volatile long g_gtcTicked = 0;
static volatile long g_gtcSkipStopped = 0;  // ClockStartedAt <= 0 -> timer is not running

static __int64 __fastcall StartTimerCd_Hook(void* self, double lenMs)
{
    if (self && g_gameTimers.insert(self).second)
        printf("[HalcyonA2][GTC] learned GameTimeComponent=%p (startTimerWithCountdown %.0fms) -> now driving its tick\n", self, lenMs);
    return StartTimerCd_Orig(self, lenMs);
}
// Ticker: run each learned GameTimeComponent's native TickComponent so its countdowns actually elapse
// and broadcast their Luau events. Validate the vtable first (drops freed/reused instances after a level
// reload). Cheap no-op when no timer is active (the tick's own branches are gated on the NetVars).
static void TickGameTimers()
{
    if (g_gameTimers.empty()) return;
    const uintptr_t base = GetBase();
    void* const gtcVft = reinterpret_cast<void*>(base + GtcVft_RVA);
    GtcTick_t tick = reinterpret_cast<GtcTick_t>(base + GtcTick_RVA);
    for (auto it = g_gameTimers.begin(); it != g_gameTimers.end(); )
    {
        void* inst = *it;
        if (!inst || *reinterpret_cast<void**>(inst) != gtcVft)   // vtable mismatch -> freed/reused, drop
        {
            it = g_gameTimers.erase(it);
            continue;
        }
        // [SCRAPRUN FIX] Only drive a timer that is actually running. A component whose
        // ClockStartedAt is <= 0 has been reset/stopped; ticking it can only produce a spurious
        // OnCountdownEnd, never useful progress. This deliberately does NOT gate on
        // IsMainCountdown, because the main match clock counts UP with that flag clear and still
        // needs driving -- gating on it would stop the match from ever ending on time.
        if (g_gateTimers)
        {
            const uintptr_t c = reinterpret_cast<uintptr_t>(inst);
            const double startedAt = *reinterpret_cast<double*>(c + GTC_ClockStartedAt);
            if (!(startedAt > 0.0))
            {
                InterlockedIncrement(&g_gtcSkipStopped);
                ++it;
                continue;
            }
        }
        tick(inst);
        InterlockedIncrement(&g_gtcTicked);
        ++it;
    }
}
static void SafeTickGameTimers() { __try { TickGameTimers(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [SCRAPRUN 50/50 AUTO-END EVIDENCE] When the conductor leaves RUNNING for an end state, print every
// learned UGameTimeComponent. If an auto-end on respawn is caused by a spurious OnCountdownEnd, the
// culprit clock shows up here with a just-reset ClockStartedAt and/or an elapsed that has only barely
// crossed ClockEndLength. ticked/skipStopped say whether the new gate is doing anything.
// SEH cannot coexist with C++ object unwinding in one frame (C2712), and the range-for below builds
// iterators -- so the __try lives in the wrapper, exactly like SafeTickGameTimers.
static void DumpGameTimersOnEndImpl(unsigned char newState)
{
    {
        const uintptr_t base = GetBase();
        void* const gtcVft = reinterpret_cast<void*>(base + GtcVft_RVA);
        printf("[HalcyonA2][SCRAPEND] state->%u  timers=%d  ticked=%ld skipStopped=%ld (gate=%d)\n",
               static_cast<unsigned>(newState), static_cast<int>(g_gameTimers.size()),
               g_gtcTicked, g_gtcSkipStopped, static_cast<int>(g_gateTimers));
        for (auto* inst : g_gameTimers)
        {
            if (!inst || *reinterpret_cast<void**>(inst) != gtcVft) continue;
            const uintptr_t c = reinterpret_cast<uintptr_t>(inst);
            printf("[HalcyonA2][SCRAPEND]   gtc=%p startedAt=%.3f endLen=%.3f mainCd=%d secCd=%d\n",
                   inst,
                   *reinterpret_cast<double*>(c + GTC_ClockStartedAt),
                   *reinterpret_cast<double*>(c + GTC_ClockEndLength),
                   (int)*reinterpret_cast<unsigned char*>(c + GTC_IsMainCd),
                   (int)*reinterpret_cast<unsigned char*>(c + GTC_IsSecondaryCd));
        }
    }
}
static void DumpGameTimersOnEnd(unsigned char newState)
{
    __try { DumpGameTimersOnEndImpl(newState); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// ★ THE GC-FLOOD FIX. sub_7FF6732AF4C0 (RVA 0x124F4C0) is a GC reference-batch COMPACTOR: it reads
// ctx->count @+0x3C0 entries from the inline batch buffer @+0x3C8, compacts the live ones into +0x508,
// then resets +0x3C0=0. When the ballsim manager exists, some object's reference gather sets +0x3C0 to a
// CORRUPT huge value (millions) while the buffer physically holds only ~hundreds -> the compactor walks
// millions of entries off the end of the buffer into adjacent garbage heap = millions of null/garbage
// pointer derefs = the flood (the VEH above then turns each would-be crash into a skip -> multi-million
// hang). Offline (same binary) never hits this. FIX: clamp the count on entry. A real batch is bounded by
// the buffer capacity (the code's own cap constant is 0x400=1024); anything larger is corruption, so cap
// it at 1024. Real batches (<=1024) are untouched; the corrupt million-count is bounded to <=1024 entries,
// and the VEH mops up whatever few of those are garbage (hundreds of skips, not millions).
using GcBatchCompact_t = void (__fastcall*)(void*);
static GcBatchCompact_t GcBatchCompact_Orig = nullptr;
static bool g_gcBatchClamp = true;   // [2026-09-04] RE-ENABLED as the sweep's race-window safety net. The earlier "batch-hook crash" was actually the stale-FString-RVA crash (SeedDashboardApiKey) firing in the same window — now fixed — so the batch-hook is cleared (same story as the sweep). It repoints climbing-from-null batch slot-pointers to the zero page so the compactor reads 0 fault-free: exactly the Data=0/Num=huge floods the 2Hz sweep hasn't repaired yet (this run's UA2SoundComponent culprit had Data=0). Converts a race-window fault-storm into a bounded fault-free pass; the sweep then repairs the source so it never recurs.
static bool g_neuterGC = false;      // [2026-09-02] DISABLED — the full neuter (BF7B0 reachability driver -> ret) also breaks async-package FINALIZATION (every GC path AND the loader's RemoveUnreachableObjects funnel through BF7B0), which deadlocks FlushAsyncLoading during a remote join (1 QueuedPackages / 0 AsyncPackages forever). The flood's real root (corrupt Num) is now clamped at source by InstallGcNumClamp + ArrCopyDrv_Hook, so this blanket neuter is redundant. Re-enable ONLY if the flood returns AND the clamp proves insufficient.
// The batcher derefs `**(ctx+0x3C8+8*i)` for i in 0..count-1: each buffer slot holds a pointer P
// (=&array[j] from the token interpreter's array handler), and it reads *P. When a TArray<UObject*>
// has Data=null and a CORRUPT Num=millions, the interpreter batches P = null + j*8 = tiny climbing
// addresses; *P then faults on unmapped low memory, and the VEH turns each into an expensive skip =
// the multi-second hang. FIX: before the original runs, sanitize the batch — every REAL object
// pointer on this heap is in the multi-TB range (image ~0x7FF6..., heap ~0x1B1.../0x2A7...), so any
// slot pointer below 4GB is garbage from a null-Data/huge-Num array; repoint it at the readable zero
// page so *P reads 0 (a null object the batcher already tolerates) instead of faulting. No faults ->
// no VEH -> the interpreter's cheap loop finishes in ms. Count field @+0x3C0 caps at 32 (flush size),
// so clamping IT never fired — the garbage is in the slot VALUES, not the count.
static void __fastcall GcBatchCompact_Hook(void* ctx)
{
    if (g_gcBatchClamp && ctx && g_gcZeroPage)
    {
        __try
        {
            uintptr_t base = reinterpret_cast<uintptr_t>(ctx) + 0x3C8;   // batch buffer of slot-pointers
            uint32_t  cnt  = *reinterpret_cast<uint32_t*>(reinterpret_cast<uintptr_t>(ctx) + 0x3C0);
            if (cnt > 34u) cnt = 34u;   // the batcher processes a fixed 34; never scan past that
            // 24GB. Real GC ref-slot addresses are always inside heap objects (~1.3TB) or the image
            // (~140TB), never this low, so everything under the cutoff is garbage from a null/small-Data
            // huge-Num array. Raised from 4GB: a run flooded with slots climbing from ~4GB (Data≈0x100000000)
            // that the old 4GB cutoff let through -> faults resumed. 24GB covers a 4GB-Data array walked to
            // full int32-max Num (~21GB) yet stays below the stack (~38GB) and heap (~1.3TB), which never
            // hold ref-slot addresses anyway.
            const uintptr_t kMinValid = 0x600000000ull;   // 24GB
            uintptr_t* buf = reinterpret_cast<uintptr_t*>(base);
            bool sanitized = false;
            for (uint32_t i = 0; i < 34u; ++i)
            {
                if (buf[i] < kMinValid)   // includes 0 — a null slot pointer also faults when the compactor derefs *P
                {
                    buf[i] = reinterpret_cast<uintptr_t>(g_gcZeroPage);   // *P -> 0, no fault
                    sanitized = true;
                }
            }
            if (sanitized)
            {
                static volatile LONG s_sanCnt = 0;
                LONG c = InterlockedIncrement(&s_sanCnt);
                static uint64_t s_last = 0; uint64_t now = GetTickCount64();
                if (c == 1 || now - s_last > 3000)
                {
                    s_last = now;
                    printf("[HalcyonA2][GCBATCH] sanitized garbage GC ref-batch (null-Data/huge-Num array) (#%ld)\n", c);
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    GcBatchCompact_Orig(ctx);
}

// Write a value of arbitrary width to an address, restoring protection after.
template <typename T>
static void WriteMem(uintptr_t addr, T val)
{
    DWORD old;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), sizeof(T), PAGE_EXECUTE_READWRITE, &old);
    *reinterpret_cast<volatile T*>(addr) = val;
    VirtualProtect(reinterpret_cast<LPVOID>(addr), sizeof(T), old, &old);
}

static void WriteByte(uintptr_t addr, uint8_t val) { WriteMem<uint8_t>(addr, val); }

// ---------------------------------------------------------------------------
// GC token-interpreter Num clamp (the class-agnostic flood fix).
// The UE reference-token interpreter sub_7FF6732C1A80 has two opcodes that walk a UObject field's
// TArray-of-objects and enqueue Num object refs into the reference compactor, WITH NO BOUNDS CHECK
// on Num:
//   case 9 (RVA 0x1261F1A): `mov eax,[rsi+8]` -> Num, `if(Num)` enqueue Num ptrs (batched to compactor)
//   case 8 (RVA 0x12621D6): `movsxd r14,[rsi+8]; shl r14,5` -> loop bound = Data + 32*Num
// On the headless server, half-initialized BP component fields are {Data=0, Num=0xFFFFFFFF}, so the
// interpreter tries to enqueue ~4 billion refs -> the "GC flood"/hang (faults land in the compactor
// sub_7FF6732AF4C0). This is NOT specific to BallSpawnerComponent — every corrupt class hits it
// (BallSpawnerComponent, UA2SoundComponent, ...). We inline-patch both Num reads to clamp Num>1M to 0
// (a real object array can't exceed GMaxObjects~2.3M; >1M == uninitialized garbage -> treat as empty).
// One fix at the source, class-agnostic, touches ZERO object memory. Replaces the per-class ARRCLAMP.
static bool g_gcNumClampInstalled = false;

// Allocate an executable page within +-2GB of `anchor` so an E9 rel32 jmp can reach it.
static uint8_t* AllocCaveNear(uintptr_t anchor)
{
    SYSTEM_INFO si; GetSystemInfo(&si);
    const uintptr_t gran = si.dwAllocationGranularity ? si.dwAllocationGranularity : 0x10000;
    const uintptr_t REACH = 0x78000000ull;   // stay well under the 2GB rel32 limit
    // scan downward from just below the anchor
    for (uintptr_t a = (anchor - 0x100000) & ~(gran - 1); a > gran && (anchor - a) < REACH; a -= gran)
    {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return static_cast<uint8_t*>(p);
    }
    // scan upward
    for (uintptr_t a = (anchor + 0x100000) & ~(gran - 1); (a - anchor) < REACH; a += gran)
    {
        void* p = VirtualAlloc(reinterpret_cast<void*>(a), 0x1000, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p) return static_cast<uint8_t*>(p);
    }
    return nullptr;
}

// Overwrite `overwriteLen` bytes at `site` with `jmp stub` (E9 rel32) + NOP padding.
static void PatchJmpToStub(uintptr_t site, size_t overwriteLen, uint8_t* stub)
{
    DWORD op; VirtualProtect(reinterpret_cast<void*>(site), overwriteLen, PAGE_EXECUTE_READWRITE, &op);
    uint8_t* p = reinterpret_cast<uint8_t*>(site);
    p[0] = 0xE9;
    int32_t rel = static_cast<int32_t>(reinterpret_cast<intptr_t>(stub) - static_cast<intptr_t>(site + 5));
    memcpy(p + 1, &rel, 4);
    for (size_t i = 5; i < overwriteLen; ++i) p[i] = 0x90;   // NOP pad
    VirtualProtect(reinterpret_cast<void*>(site), overwriteLen, op, &op);
}

static void InstallGcNumClamp(uintptr_t base)
{
    if (g_gcNumClampInstalled) return;

    // There are SEVEN TFastReferenceCollector template instantiations in the GC cluster
    // (sub_7FF6732C1A80, C3090, C4BD0, C5860, C7AC0, C8BC0 — plus B4D40 which is structured
    // differently and left to the VEH). Each inlines its OWN unchecked Num read, so clamping one
    // collector only moves the flood to a sibling (observed: C1A80 fixed -> flood reappeared in a
    // sibling). Patch every one. kind 9 = `mov eax,[base+8]` array-enqueue Num (clamp EAX); kind 8 =
    // `movsxd r14,[base+8]; shl r14,5` 32-byte-stride bound (clamp R14). orig[] = exact bytes we
    // relocate into the cave; readLen = where to splice the clamp (right after the Num read); total =
    // bytes overwritten at the site (instruction-aligned, >=5 for the E9 rel32). All relocated tails
    // are position-independent (rbp-relative mov / lea reg,[reg+imm] / shl), so verbatim copy is safe.
    struct Site { uint32_t rva; uint8_t kind; uint8_t readLen; uint8_t total; uint8_t orig[10]; };
    static const Site kSites[] = {
        // --- case 9 (clamp eax) : mov r32,[base+8] + relocated tail ---
        { 0x1261F1A, 9, 3, 9,  {0x8B,0x46,0x08, 0x89,0x85,0x88,0x00,0x00,0x00} },      // C1A80  mov [rbp+88h],eax
        { 0x126361F, 9, 4,10,  {0x41,0x8B,0x46,0x08, 0x89,0x85,0xA8,0x00,0x00,0x00} }, // C3090
        { 0x1265139, 9, 3, 6,  {0x8B,0x46,0x08, 0x89,0x45,0x78, 0,0,0,0} },            // C4BD0
        { 0x1265D26, 9, 4,10,  {0x41,0x8B,0x46,0x08, 0x89,0x85,0xA8,0x00,0x00,0x00} }, // C5860
        { 0x1268F6E, 9, 3, 6,  {0x8B,0x47,0x08, 0x89,0x45,0xA8, 0,0,0,0} },            // C8BC0
        { 0x1267DB3, 9, 3, 7,  {0x8B,0x46,0x08, 0x49,0x8D,0x77,0x40, 0,0,0} },         // C7AC0  lea rsi,[r15+40h]
        // --- B4D40 loop1 (clamp esi) : mov esi,[rax+8] + mov rbx,[rax] ---  (the incremental collector
        //     that actually runs during level streaming; feeds the compactor from the direct-ref list)
        { 0x1254DB5, 1, 3, 6,  {0x8B,0x70,0x08, 0x48,0x8B,0x18, 0,0,0,0} },            // B4D40
        // --- case 8 (clamp r14) : movsxd r14,[base+8] + shl r14,5 ---
        { 0x12621D6, 8, 4, 8,  {0x4C,0x63,0x76,0x08, 0x49,0xC1,0xE6,0x05, 0,0} },      // C1A80
        { 0x12639B8, 8, 4, 8,  {0x4D,0x63,0x76,0x08, 0x49,0xC1,0xE6,0x05, 0,0} },      // C3090
        { 0x12654C5, 8, 4, 8,  {0x4C,0x63,0x76,0x08, 0x49,0xC1,0xE6,0x05, 0,0} },      // C4BD0
        { 0x1265FE9, 8, 4, 8,  {0x4D,0x63,0x76,0x08, 0x49,0xC1,0xE6,0x05, 0,0} },      // C5860
        { 0x1268073, 8, 4, 8,  {0x4C,0x63,0x76,0x08, 0x49,0xC1,0xE6,0x05, 0,0} },      // C7AC0
        { 0x1269351, 8, 4, 8,  {0x4C,0x63,0x77,0x08, 0x49,0xC1,0xE6,0x05, 0,0} },      // C8BC0
    };
    static const uint8_t kClampEax[] = { 0x3D,0x00,0x00,0x10,0x00, 0x76,0x02, 0x31,0xC0 };                // cmp eax,100000h; jbe +2; xor eax,eax
    static const uint8_t kClampR14[] = { 0x49,0x81,0xFE,0x00,0x00,0x10,0x00, 0x76,0x03, 0x4D,0x31,0xF6 }; // cmp r14,100000h; jbe +3; xor r14,r14
    static const uint8_t kClampEsi[] = { 0x81,0xFE,0x00,0x00,0x10,0x00, 0x76,0x02, 0x31,0xF6 };           // cmp esi,100000h; jbe +2; xor esi,esi

    uint8_t* cave = AllocCaveNear(base + kSites[0].rva);
    if (!cave) { printf("[HalcyonA2] GC Num-clamp: cave alloc failed (flood NOT fixed)\n"); return; }

    size_t off = 0; int done = 0; int stale = 0;
    for (const Site& s : kSites)
    {
        const uintptr_t site = base + s.rva;
        // ★ [2026-09-03] SELF-VERIFY: the RVAs here were found on an earlier build. On a new build a stale
        // RVA points at DIFFERENT bytes — patching it blindly (a) doesn't clamp the real Num read (flood
        // survives) AND (b) corrupts whatever code IS there + copies wrong bytes into the cave. So compare
        // the site's actual bytes to the expected `orig[]` (the whole relocated region) BEFORE patching;
        // on mismatch, SKIP + log. If the flood persists with sites logged STALE, that's the smoking gun:
        // the clamp was never actually on the Num reads for this build -> re-find those RVAs.
        bool match = true;
        for (int k = 0; k < s.total; ++k)
            if (*reinterpret_cast<uint8_t*>(site + k) != s.orig[k]) { match = false; break; }
        if (!match)
        {
            printf("[HalcyonA2] GC Num-clamp: *** STALE SITE rva=0x%X (%s) expect %02X %02X %02X %02X  got %02X %02X %02X %02X -> SKIPPED (clamp NOT applied here)\n",
                   s.rva,
                   s.kind == 8 ? "case8" : (s.kind == 1 ? "B4D40" : "case9"),
                   s.orig[0], s.orig[1], s.orig[2], s.orig[3],
                   *reinterpret_cast<uint8_t*>(site + 0), *reinterpret_cast<uint8_t*>(site + 1),
                   *reinterpret_cast<uint8_t*>(site + 2), *reinterpret_cast<uint8_t*>(site + 3));
            ++stale;
            continue;
        }
        uint8_t* stub = cave + off;
        uint8_t b[64]; int n = 0;
        memcpy(b + n, s.orig, s.readLen); n += s.readLen;                                  // the Num read
        if      (s.kind == 9) { memcpy(b + n, kClampEax, sizeof(kClampEax)); n += (int)sizeof(kClampEax); }
        else if (s.kind == 8) { memcpy(b + n, kClampR14, sizeof(kClampR14)); n += (int)sizeof(kClampR14); }
        else                  { memcpy(b + n, kClampEsi, sizeof(kClampEsi)); n += (int)sizeof(kClampEsi); }  // kind 1 = esi
        memcpy(b + n, s.orig + s.readLen, (size_t)(s.total - s.readLen)); n += (s.total - s.readLen); // relocated tail
        b[n++] = 0xE9; int imm = n; n += 4;                                                // jmp back
        int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(site + s.total) - reinterpret_cast<intptr_t>(stub + n));
        memcpy(b + imm, &rel, 4);
        memcpy(stub, b, n);
        PatchJmpToStub(site, s.total, stub);
        off += n;
        ++done;
    }

    // ---- B4D40 loop2 (struct-array refs) : ENTRY-TEST clamp @ +0x1254F50 ----
    // Loop2 reads Num from the OWORD/xmm and drives a stride-fill; its Num is used at multiple points
    // (mem-cmp + xmm extract), so a mid-loop clamp is fragile. Instead patch the loop's entry test
    // `cmp dword[r14+8],0` (which sets ZF for the following `jz <skip-entry>`): make it ALSO set ZF when
    // Num>1M, so a corrupt entry is skipped whole (no fill) and never reaches the big/small-path split.
    // B4D40 is the array-struct sub-collector shared by ALL main collectors, so this covers every class.
    {
        const uintptr_t site2 = base + 0x1254F50;   // cmp dword ptr [r14+8], 0   (5 bytes)
        // self-verify: expect `cmp dword ptr [r14+8], 0` = 41 83 7E 08 00
        static const uint8_t exp2[] = {0x41,0x83,0x7E,0x08,0x00};
        bool m2 = true;
        for (int k = 0; k < 5; ++k) if (*reinterpret_cast<uint8_t*>(site2 + k) != exp2[k]) { m2 = false; break; }
        if (!m2)
        {
            printf("[HalcyonA2] GC Num-clamp: *** STALE SITE rva=0x1254F50 (B4D40 loop2 entry-test) got %02X %02X %02X %02X %02X -> SKIPPED\n",
                   *reinterpret_cast<uint8_t*>(site2 + 0), *reinterpret_cast<uint8_t*>(site2 + 1),
                   *reinterpret_cast<uint8_t*>(site2 + 2), *reinterpret_cast<uint8_t*>(site2 + 3),
                   *reinterpret_cast<uint8_t*>(site2 + 4));
            ++stale;
            goto clamp_done;
        }
        uint8_t* stub = cave + off;
        uint8_t e[] = {
            0x41,0x8B,0x46,0x08,                 // mov  eax,[r14+8]   (Num)
            0x3D,0x00,0x00,0x10,0x00,            // cmp  eax,00100000h
            0x76,0x02,                           // jbe  +2 (small: keep Num, test it)
            0x31,0xC0,                           // xor  eax,eax  (>1M -> force ZF=1 => skip entry)
            0x85,0xC0,                           // test eax,eax  (ZF = Num==0, or =1 when clamped)
            0xE9,0,0,0,0                         // jmp  back (site2+5)
        };
        int n = (int)sizeof(e);
        int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(site2 + 5) - reinterpret_cast<intptr_t>(stub + n));
        memcpy(e + (n - 4), &rel, 4);
        memcpy(stub, e, n);
        PatchJmpToStub(site2, 5, stub);
        off += n;
        ++done;
    }

clamp_done:
    g_gcNumClampInstalled = true;
    printf("[HalcyonA2] GC Num-clamp: %d/%d sites VERIFIED+patched, %d STALE (skipped) (cave %p)%s\n",
           done, done + stale, stale, (void*)cave,
           stale ? "  <-- STALE sites = clamp NOT on those Num reads = flood source; re-find those RVAs" : "  - all sites good");
}

// ─────────────────────────────────────────────────────────────────────────────
// [2026-09-03 ★ ROOT FLOOD FIX — REFLECTION-SCOPED] Repair corrupt archetype TArray headers at the source.
// The GC flood (250M+ skips = permanent hang) is the reference collector walking a TArray whose header is
// {Data=0, Max=0, Num=huge} on a COMPONENT ARCHETYPE (UA2SoundComponent / UA2HeartCoreComponent /
// UWidgetInteractionComponent / … — a different one each collection; all RF_ArchetypeObject). That state is
// IMPOSSIBLE for a real TArray: Num>0 REQUIRES Data!=0 and Max>=Num. So {Data==0 && Max==0 && Num>0} is
// unambiguously corrupt and its only valid value is Num=0 (empty). The 8 Num-clamp sites VERIFY CORRECT in
// IDA, so the collector isn't the bug — the object's stored Num genuinely is garbage. We keep it valid by
// zeroing that Num — but ONLY on REAL array properties, found via reflection (never raw offsets: the raw
// version zeroed a legit {null,bignum,0} field and crashed the server twice).
//
// Walk the object's UClass hierarchy and, for each FArrayProperty, check the array at its Offset. Layout
// (SDK Basic.hpp, verified): UObject.ClassPrivate@0x10; UStruct.SuperStruct@0x40, .ChildProperties(FField*)
// @0x50; FField.ClassPrivate(FFieldClass*)@0x08, .Next@0x18, .Offset(FProperty)@0x44; FFieldClass.CastFlags
// @0x10; EClassCastFlags::ArrayProperty=0x200000. SELF-VALIDATING: it only ever writes when it finds an
// FFieldClass whose CastFlags has EXACTLY the ArrayProperty bit at a plausible Offset holding the impossible
// {Data=0,Max=0,Num>1M} pattern. If any UStruct offset were wrong for this build, the chain yields garbage
// FFields whose CastFlags won't equal ArrayProperty at a sane offset over that exact pattern -> it matches
// NOTHING and writes NOTHING (no crash), unlike the raw-offset version. All derefs are pointer-plausibility
// gated + wrapped in one SEH per object.
static void HxLog(const char* fmt, ...);   // fwd-decl (defined later; used by the dry-run sweep log)
bool g_gcSweepDryRun = false;         // [2026-09-03] WRITE MODE. Dry-run confirmed the reflection walk correctly detects corrupt archetype FArrayProperties (real class offsets, no crash). Now actually repair them (Num impossibly large -> reset header to clean empty).
volatile long g_gcSweepLogN = 0;      // caps dry-run log spam
static inline bool GcPtrPlaus(uint64_t p) { return p > 0x10000ull && p < 0x7FFFFFFFFFFFull && (p & 7) == 0; }
static uint32_t SafeReadU32(uintptr_t a) { __try { return *reinterpret_cast<uint32_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }
static uint64_t SafeReadPtr(uintptr_t a) { __try { return *reinterpret_cast<uint64_t*>(a); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

// [2026-09-04 ★ CACHED, LOAD-COMPLETED-WIDE] Walk a UClass's property chain (super hierarchy + ChildProperties)
// ONCE and collect its FArrayProperty byte-offsets. The flood culprits are NOT only archetypes — a loaded
// ASSET (RF_LoadCompleted, no RF_ArchetypeObject: vtbl+0x8421CB0 flags 0x0028000B) also carried a corrupt
// array. So we must scan every RF_LoadCompleted object, which is far too many to full-reflection-walk at 2Hz
// -> cache the offsets per class (classes are static after load) and per-object just check those offsets.
static void GcCollectClassArrayOffsets(uint64_t classPtr, std::vector<uint16_t>& out)   // raw reads; caller SEH-wraps
{
    uint64_t uc = classPtr;
    int superGuard = 0;
    while (GcPtrPlaus(uc) && superGuard++ < 48)
    {
        uint64_t field = *reinterpret_cast<uint64_t*>(uc + 0x50);    // UStruct::ChildProperties (FField*)
        int fieldGuard = 0;
        while (GcPtrPlaus(field) && fieldGuard++ < 8192)
        {
            uint64_t fc = *reinterpret_cast<uint64_t*>(field + 0x08);            // FField::ClassPrivate (FFieldClass*)
            if (GcPtrPlaus(fc) && (*reinterpret_cast<uint64_t*>(fc + 0x10) & 0x200000ull))   // CastFlags & ArrayProperty
            {
                int32_t off = *reinterpret_cast<int32_t*>(field + 0x44);         // FProperty::Offset
                if (off >= 0x10 && off <= 0x4000 && out.size() < 64) out.push_back(static_cast<uint16_t>(off));
            }
            field = *reinterpret_cast<uint64_t*>(field + 0x18);      // FField::Next
        }
        uc = *reinterpret_cast<uint64_t*>(uc + 0x40);               // UStruct::SuperStruct
    }
}
static bool SafeGcCollectClassArrayOffsets(uint64_t classPtr, std::vector<uint16_t>& out)
{ __try { GcCollectClassArrayOffsets(classPtr, out); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; } }

bool g_gcSweep = true;
// Fix corrupt arrays on one object given its class's cached FArrayProperty offsets. The rock-solid corruption
// signal: Num impossibly large (>2M) — a loaded template/asset array never holds millions of elements, and a
// TArray<UObject*> can't exceed the ~2.3M object count. Reset the whole header {Data=0,Num=0,Max=0} (GC walks
// 0 elements = no flood; a later free sees Data==0 = no free of a garbage ptr = no crash). Raw reads; SEH-wrapped.
static int GcFixObjectCached(uintptr_t obj, const uint16_t* offs, size_t nOffs)
{
    int fixed = 0;
    for (size_t k = 0; k < nOffs; ++k)
    {
        const uint16_t off = offs[k];
        uint32_t nnum = *reinterpret_cast<uint32_t*>(obj + off + 8);            // Num
        if (nnum > 0x200000u && nnum <= 0x7FFFFFFFu)                            // >2M -> corrupt
        {
            // ★ [2026-09-04 CLIENT-FREEZE FIX] Don't zero a value that is actually a LIVE FLOAT the game
            // writes each frame — its bit-pattern looks like a huge Num (e.g. 100.0 == 0x42C80000 ==
            // 1120403456) but it's real (often replicated) data; zeroing it corrupts the object -> the
            // joining client got 0.0 instead of 100.0 and its view froze. Real corrupt array Nums
            // reinterpret to DENORMAL/HUGE/NaN floats (33562720==9e-38, 1.5e9==6e17), which fall OUTSIDE a
            // normal range; a genuine float field is a sane magnitude. So skip when Num-as-float is a
            // normal value in [1e-4, 1e7]. (NaN/inf compare false -> not skipped -> still repaired.)
            float asFloat; memcpy(&asFloat, &nnum, 4);
            const float absF = asFloat < 0.0f ? -asFloat : asFloat;
            if (absF >= 1e-4f && absF <= 1e7f) continue;   // live float field, not a corrupt Num — leave it
            if (g_gcSweepDryRun)
            {
                if (_InterlockedIncrement(&g_gcSweepLogN) <= 40)
                {
                    const uint64_t b2 = GetBase();
                    const uint64_t vt = *reinterpret_cast<uint64_t*>(obj + 0);
                    HxLog("[HalcyonA2][GCSWEEP-DRY] would-fix vtbl(rva)=+%llX off=0x%X Num=%u Data=%llX Max=%d\n",
                          (unsigned long long)((vt > b2 && vt < b2 + 0x0C000000) ? vt - b2 : vt), off, nnum,
                          (unsigned long long)*reinterpret_cast<uint64_t*>(obj + off),
                          *reinterpret_cast<int32_t*>(obj + off + 12));
                }
            }
            else
            {
                // [2026-09-04] Throttled: name the re-corrupting class (it repairs "1 per pass" forever, so
                // one array re-corrupts each frame). If off is really a float/struct field the game writes
                // (Num reinterpreted as a float printing a clean value like 96.0), we're zeroing REAL data
                // every frame -> if that object is replicated, the client gets garbage -> client freeze. The
                // "wasFloat" readout tells us: a clean float => exclude this class/off; NaN/huge => real corruption.
                static uint64_t s_lastFixLog = 0; const uint64_t now2 = GetTickCount64();
                if (now2 - s_lastFixLog > 2000)
                {
                    s_lastFixLog = now2;
                    const uint64_t b2 = GetBase();
                    const uint64_t vt = *reinterpret_cast<uint64_t*>(obj + 0);
                    float asF; memcpy(&asF, &nnum, 4);
                    const int32_t objFlags = *reinterpret_cast<int32_t*>(obj + 0x08);
                    HxLog("[HalcyonA2][GCSWEEP-FIX] vtbl(rva)=+%llX off=0x%X Num=%u wasFloat=%.4f flags=0x%08X\n",
                          (unsigned long long)((vt > b2 && vt < b2 + 0x0C000000) ? vt - b2 : vt), off, nnum, asF, objFlags);
                }
                *reinterpret_cast<uint64_t*>(obj + off)      = 0;   // Data
                *reinterpret_cast<int32_t*>(obj + off + 8)   = 0;   // Num
                *reinterpret_cast<int32_t*>(obj + off + 12)  = 0;   // Max
            }
            ++fixed;
        }
    }
    return fixed;
}
static int SafeGcFixObjectCached(uintptr_t obj, const uint16_t* offs, size_t nOffs)
{ __try { return GcFixObjectCached(obj, offs, nOffs); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; } }

static std::unordered_map<uint64_t, std::vector<uint16_t>>* g_gcClassArrayCache = nullptr;
static void GcCorruptArraySweep()
{
    if (!g_gcSweep) return;
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_last < 100) return;    // ~10 Hz — shrink the race window before a GC walks a freshly-streamed corrupt object (batch-hook covers the rest). The per-class offset cache makes a full 50k-obj scan cheap enough to run this often.
    s_last = now;
    if (!g_gcClassArrayCache) g_gcClassArrayCache = new std::unordered_map<uint64_t, std::vector<uint16_t>>();
    auto& cache = *g_gcClassArrayCache;
    const int32_t num = SDK::UObject::GObjects->Num();
    g_objN = num;   // [PROF] chunked below; g_walks counts COMPLETED passes, not chunks
    // [PERF 2026-09-09] This was a FULL 165,982-object walk at 10Hz = ~10 of the ~17 walks/s the
    // server was doing, roughly 330ms/s - a THIRD of the game thread - and because it is called
    // outside the PROF wrappers it never appeared in the per-function profile. The gate comment
    // above sized it for a 50k array; the array is now 166k.
    //
    // It is a crash guard, so it is NOT disabled or slowed down. The sweep is now INCREMENTAL: each
    // 10Hz call walks one CHUNK and a persistent cursor carries into the next call, so the entire
    // array is still covered - just across ~5 calls instead of within one. Same objects checked,
    // ~1/5 the cost, and the 33ms spike becomes 5 x ~7ms, removing a per-frame hitch that was
    // feeding the ball-sim burst-stepping behind MI.
    //
    // DELIBERATE TRADE-OFF: worst-case time-to-detect a freshly-corrupt object goes 100ms -> ~500ms.
    // Acceptable because this is defence-in-depth; the batch-hook named in the gate comment is the
    // primary guard and still fires immediately.
    static int32_t s_cursor = 0;
    const int32_t GC_SWEEP_CHUNK = 32768;
    if (s_cursor >= num) s_cursor = 0;
    const int32_t sweepEnd = (s_cursor + GC_SWEEP_CHUNK < num) ? (s_cursor + GC_SWEEP_CHUNK) : num;
    int fixed = 0, scanned = 0;
    for (int32_t i = s_cursor; i < sweepEnd; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o) continue;
        const uintptr_t obj = reinterpret_cast<uintptr_t>(o);
        if (!(SafeReadU32(obj + 0x08) & 0x200000u)) continue;   // RF_LoadCompleted — loaded assets/CDOs/archetypes
        const uint64_t classPtr = SafeReadPtr(obj + 0x10);      // ClassPrivate
        if (!GcPtrPlaus(classPtr)) continue;
        auto it = cache.find(classPtr);
        if (it == cache.end())
        {
            std::vector<uint16_t> offs;
            SafeGcCollectClassArrayOffsets(classPtr, offs);     // walk this class ONCE
            it = cache.emplace(classPtr, std::move(offs)).first;
        }
        ++scanned;
        if (!it->second.empty())
            fixed += SafeGcFixObjectCached(obj, it->second.data(), it->second.size());
    }
    if (sweepEnd >= num) InterlockedIncrement(&g_walks);   // count only a COMPLETED full pass
    s_cursor = (sweepEnd >= num) ? 0 : sweepEnd;           // wrap once the array is fully covered
    if (fixed)
    {
        static int total = 0; total += fixed;
        printf("[HalcyonA2][GCSWEEP] repaired %d corrupt FArrayProperty Num(s) -> 0 (scanned %d loaded objs, %zu classes cached, total %d)\n",
               fixed, scanned, cache.size(), total);
    }
}

// [2026-09-02 ★ JOIN NULL-GUARD] sub_7FF677487660 (ballsim player-index->sim lookup) crashes when a 2nd
// player joins: it iterates the ballsim player list, calls a vtable getter (`call [rdx+540h]`) that returns
// the player's pawn/entity, and immediately does `cmp [rax+1D54h], ecx` — but for the just-joined player
// that getter returns NULL (pawn not wired yet) -> AV reading 0x1D54. The game omitted the null-check; the
// correct behavior is "this player isn't a match, skip it". FIX: patch the 6-byte cmp @+0x54277AD to jump
// into a cave that null-checks rax first: rax!=0 -> real cmp (ZF as before); rax==0 -> `test rsp,rsp` forces
// ZF=0 so the following `jz match` falls through to the loop's ++index/next-player path. In-place, class-
// exact, no VEH, no per-fault handler. (This is the join-time analogue of the GC Num-clamp cave.)
static bool g_joinNullGuardInstalled = false;
static void InstallJakeballLookupNullGuard(uintptr_t base)
{
    if (g_joinNullGuardInstalled) return;
    const uintptr_t site = base + 0x54277AD;   // cmp [rax+1D54h], ecx  (39 88 54 1D 00 00)
    // sanity: only patch if the bytes are exactly what we expect
    const uint8_t expect[6] = {0x39,0x88,0x54,0x1D,0x00,0x00};
    for (int i = 0; i < 6; ++i)
        if (*reinterpret_cast<uint8_t*>(site + i) != expect[i]) {
            printf("[HalcyonA2] JOIN-GUARD: site bytes mismatch, NOT patching (join crash unguarded)\n");
            return;
        }
    uint8_t* cave = AllocCaveNear(site);
    if (!cave) { printf("[HalcyonA2] JOIN-GUARD: cave alloc failed\n"); return; }
    uint8_t c[] = {
        0x48,0x85,0xC0,               // test rax,rax
        0x74,0x08,                    // jz  +8  -> not_ready (c+13)
        0x39,0x88,0x54,0x1D,0x00,0x00,// cmp [rax+1D54h],ecx   (real compare, sets ZF)
        0xEB,0x03,                    // jmp +3  -> done (c+16)
        0x48,0x85,0xE4,               // not_ready: test rsp,rsp  (rsp!=0 => ZF=0 => skip player)
        0xE9,0,0,0,0                  // done: jmp back to site+6
    };
    int n = (int)sizeof(c);
    int32_t rel = static_cast<int32_t>(static_cast<intptr_t>(site + 6) - reinterpret_cast<intptr_t>(cave + n));
    memcpy(c + (n - 4), &rel, 4);
    memcpy(cave, c, n);
    PatchJmpToStub(site, 6, cave);   // E9 rel32 + 1 NOP over the 6-byte cmp
    g_joinNullGuardInstalled = true;
    printf("[HalcyonA2] JOIN null-guard installed @ +0x54277AD (cave %p) - skip not-ready player in ballsim lookup (fixes 2nd-player join AV @ 0x1D54)\n",
           (void*)cave);
}

// Minimal blocking WinHTTP POST to a local server. Returns the response body
// (empty string on any failure).
static std::string HttpPostLocal(const wchar_t* host, int port, const wchar_t* path, const std::string& body)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"HalcyonA2/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession)
        return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (hConnect)
    {
        // [2026-09-08 TLS] port 443 => real HTTPS. Without this the DLL was plain-HTTP only, so it could
        // NOT reach a Cloudflare-fronted backend (rigel-*.<domain> answer on 443 only; :90/:78 time out) —
        // which is exactly why this was hardcoded to a raw IP. Now it can talk to the real domains.
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, L"POST", path, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                (port == 443) ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest)
        {
            const wchar_t* headers = L"Content-Type: application/json\r\n";
            if (WinHttpSendRequest(hRequest, headers, (DWORD)-1L,
                                   (LPVOID)body.data(), (DWORD)body.size(), (DWORD)body.size(), 0)
                && WinHttpReceiveResponse(hRequest, nullptr))
            {
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail)
                {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read))
                        break;
                    chunk.resize(read);
                    result += chunk;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// Log to BOTH the console and %TEMP%\HalcyonA2.log (our console prints don't land in the UE .log,
// which makes headless-server diagnosis painful — the file persists).
static wchar_t g_hxLogSuffix[32] = {0};   // [CLIENTMODE] L"client" in mock-client mode
static FILE* g_hxLog = nullptr;
static void HxLog(const char* fmt, ...)
{
    if (!g_hxLog)
    {
        wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
        wchar_t path[MAX_PATH]{};
        // [CLIENTMODE] per-process log name, so a mock client injected alongside the server does not
        // fight it for the same file; _SH_DENYNO so both can be tailed while they are still running.
        swprintf_s(path, g_hxLogSuffix[0] ? L"%sHalcyonA2-%s.log" : L"%sHalcyonA2.log", tmp, g_hxLogSuffix);
        g_hxLog = _wfsopen(path, L"a", _SH_DENYNO);
        if (g_hxLog) fwprintf(stdout, L"[HalcyonA2] file log -> %s\n", path);
    }
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_hxLog) { vfprintf(g_hxLog, fmt, ap2); fflush(g_hxLog); }
    va_end(ap2);
}

// [2026-09-03 ★ CUSTOM CRASH REPORTER] The server dies instantly with no UE callstack (headless, crash
// reporter suppressed). This VEH fires on the FATAL fault (registered LAST, so our handled GC/PE/pawn
// faults are consumed first and never reach here) and dumps the faulting RIP as an RVA — paste it and look
// it up in IDA to see EXACTLY where it crashed — plus registers and a stack scan for image-range return
// addresses (the callstack). Writes via HxLog (fflush'd) so it survives an instant exit. Returns
// CONTINUE_SEARCH so the process still crashes/handles normally; we've just recorded it first.
// [2026-09-06] Our HalcyonA2.dll load base (via VirtualQuery on our own code), so the crash reporter can
// resolve a fault whose RIP is inside our DLL (rip - selfBase = offset -> look up in HalcyonA2.map).
static uintptr_t CrashSelfBase()
{
    static uintptr_t s = 0;
    if (!s) { MEMORY_BASIC_INFORMATION mbi; if (VirtualQuery(reinterpret_cast<void*>(&CrashSelfBase), &mbi, sizeof(mbi))) s = reinterpret_cast<uintptr_t>(mbi.AllocationBase); }
    return s;
}
// [2026-09-06] Thread-local depth counter our INTENTIONAL SEH-probe faulters bump (the GC sweep, the SIMPART
// read). While >0, a fault whose RIP is in our DLL is a designed-and-caught probe fault, NOT a crash — the
// reporter skips it so probe noise never exhausts the cap or masks the real fatal fault.
static thread_local int g_probeGuard = 0;

// [DISCARRAY] SEH-guarded read of the seater's per-player entity array (see the DISC log below).
static void ReadEntityArray(void* f440, void*& outData, int& outNum)
{
    __try
    {
        outData = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(f440) + 0x2A0);
        outNum  = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(f440) + 0x2A8);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { outData = nullptr; outNum = -2; }
}


// ============================================================================================
// [EXITTRACE] The 22284 server dies with exit code 3 and NO SEH exception (CrashReporterVeh
// never fires), so something calls exit/TerminateProcess deliberately. UE's
// FWindowsPlatformMisc::RequestExit(Force=true) does TerminateProcess(self, GIsCriticalError?3:0),
// and TerminateProcess drops the log on the floor, which is why stdout ends mid-frame. Hook both
// kernel32 exit paths, dump a game-module return-address chain to %TEMP%\HalcyonA2.log, then let
// the call through unchanged.
// ============================================================================================
static void HxLog(const char* fmt, ...);
static uintptr_t GetBase();
static uintptr_t CrashSelfBase();

static void ExitTraceDump(const char* who, unsigned code)
{
    const uintptr_t base = GetBase();
    const uintptr_t self = CrashSelfBase();
    HxLog("\n[HalcyonA2][EXITTRACE] ***** %s(code=%u) *****  gameBase=0x%llX selfBase=0x%llX\n",
          who, code, (unsigned long long)base, (unsigned long long)self);
    void* frames[62] = {};
    USHORT n = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < n; ++i)
    {
        const uintptr_t v = reinterpret_cast<uintptr_t>(frames[i]);
        if (v >= base && v < base + 0x0C000000ull)
            HxLog("[HalcyonA2][EXITTRACE]   #%02u GAME +0x%llX  (IDB 0x%llX)\n",
                  (unsigned)i, (unsigned long long)(v - base),
                  (unsigned long long)(0x140000000ull + (v - base)));
        else if (self && v >= self && v < self + 0x00400000ull)
            HxLog("[HalcyonA2][EXITTRACE]   #%02u SELF +0x%llX\n", (unsigned)i, (unsigned long long)(v - self));
        else
        {
            HMODULE m = nullptr; char nm[MAX_PATH] = {0};
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCSTR>(v), &m) && m)
            {
                GetModuleFileNameA(m, nm, MAX_PATH);
                const char* b = strrchr(nm, (char)92); b = b ? b + 1 : nm;
                HxLog("[HalcyonA2][EXITTRACE]   #%02u %s +0x%llX\n", (unsigned)i, b,
                      (unsigned long long)(v - reinterpret_cast<uintptr_t>(m)));
            }
            else HxLog("[HalcyonA2][EXITTRACE]   #%02u 0x%llX\n", (unsigned)i, (unsigned long long)v);
        }
    }
    HxLog("[HalcyonA2][EXITTRACE] ***** end *****\n");
}

using TerminateProcess_t = BOOL(WINAPI*)(HANDLE, UINT);
using ExitProcess_t      = void(WINAPI*)(UINT);
static TerminateProcess_t TerminateProcess_Orig = nullptr;
static ExitProcess_t      ExitProcess_Orig      = nullptr;

static BOOL WINAPI TerminateProcess_Hook(HANDLE h, UINT code)
{
    if (h == GetCurrentProcess() || h == (HANDLE)(LONG_PTR)-1)
        ExitTraceDump("TerminateProcess(self)", code);
    return TerminateProcess_Orig(h, code);
}
static void WINAPI ExitProcess_Hook(UINT code)
{
    ExitTraceDump("ExitProcess", code);
    ExitProcess_Orig(code);
}

static LONG CALLBACK CrashReporterVeh(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    // Only fatal-class codes (skip the myriad benign/first-chance C++ EH 0xE06D7363, DBG, thread-name, etc.)
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION &&
        code != EXCEPTION_PRIV_INSTRUCTION && code != EXCEPTION_IN_PAGE_ERROR &&
        code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_INT_DIVIDE_BY_ZERO &&
        code != 0xC0000409u /* fastfail / stack-buffer overrun */ && code != 0xC000001Du /* illegal instr */ &&
        code != 0xC0000008u /* [EXITTRACE] STATUS_INVALID_HANDLE (bad CRITICAL_SECTION / CloseHandle) */ &&
        code != 0xC0000374u /* [EXITTRACE] heap corruption */ && code != 0xC00000FDu /* stack overflow (alt) */)
        return EXCEPTION_CONTINUE_SEARCH;
    const uintptr_t base = GetBase();
    const uintptr_t self = CrashSelfBase();
    const uintptr_t ripEarly = ep->ContextRecord->Rip;
    const bool inGame = (ripEarly >= base && ripEarly < base + 0x0C000000ull);
    const bool inSelf = (self && ripEarly >= self && ripEarly < self + 0x00400000ull);
    // [2026-09-06] Report faults in the GAME image OR in HalcyonA2.dll (crashes inside our own hooks were
    // previously invisible — they aren't in the game IDB). BUT a fault in our DLL while a known SEH probe is
    // running (g_probeGuard>0) is by-design and caught locally -> skip it (no noise). System/other-module rip -> skip.
    if (inSelf && g_probeGuard > 0) return EXCEPTION_CONTINUE_SEARCH;
    // [CONSOLELOG] Do NOT drop faults raised outside the game image / our DLL: the 22284 server's
    // real killer was an AV raised INSIDE ntdll (RtlEnterCriticalSection on a zeroed CRITICAL_SECTION),
    // and the old "inGame || inSelf" filter threw it away, leaving exit code 3 with no record at all.
    // Everything fatal-class is now reported; the stack scan still resolves game/self return addresses.
    static volatile LONG s_n = 0;
    if (InterlockedIncrement(&s_n) > 12) return EXCEPTION_CONTINUE_SEARCH;   // cap — don't loop on cascading faults
    auto* c = ep->ContextRecord;
    const uintptr_t rip = c->Rip;
    const unsigned long long faultAddr =
        (code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR)
        ? (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1] : 0ull;
    HxLog("\n[HalcyonA2][CRASH] ***** FATAL code=0x%08X rip=0x%llX faultAddr=0x%llX gameBase=0x%llX selfBase=0x%llX *****\n",
          code, (unsigned long long)rip, faultAddr, (unsigned long long)base, (unsigned long long)self);
    if (!inGame && !inSelf)
    {
        HMODULE hm = nullptr; char mn[MAX_PATH] = {0};
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(rip), &hm) && hm)
        {
            GetModuleFileNameA(hm, mn, MAX_PATH);
            const char* bn = strrchr(mn, (char)92); bn = bn ? bn + 1 : mn;
            HxLog("[HalcyonA2][CRASH]   rip in %s +0x%llX (outside the game image)\n",
                  bn, (unsigned long long)(rip - reinterpret_cast<uintptr_t>(hm)));
        }
        else HxLog("[HalcyonA2][CRASH]   rip 0x%llX is in no loaded module\n", (unsigned long long)rip);
    }
    else if (inSelf)
        HxLog("[HalcyonA2][CRASH]   rip in HalcyonA2.dll +0x%llX  <-- resolve in HalcyonA2.map (this build)\n",
              (unsigned long long)(rip - self));
    else
        HxLog("[HalcyonA2][CRASH]   rip in GAME +0x%llX  <-- IDA 0x%llX (IDB base 0x7FF672060000)\n",
              (unsigned long long)(rip - base), (unsigned long long)(0x7FF672060000ull + (rip - base)));
    HxLog("[HalcyonA2][CRASH]  RAX=%llX RBX=%llX RCX=%llX RDX=%llX RSI=%llX RDI=%llX RBP=%llX RSP=%llX\n",
          (unsigned long long)c->Rax, (unsigned long long)c->Rbx, (unsigned long long)c->Rcx, (unsigned long long)c->Rdx,
          (unsigned long long)c->Rsi, (unsigned long long)c->Rdi, (unsigned long long)c->Rbp, (unsigned long long)c->Rsp);
    HxLog("[HalcyonA2][CRASH]  R8=%llX R9=%llX R10=%llX R11=%llX R12=%llX R13=%llX R14=%llX R15=%llX\n",
          (unsigned long long)c->R8, (unsigned long long)c->R9, (unsigned long long)c->R10, (unsigned long long)c->R11,
          (unsigned long long)c->R12, (unsigned long long)c->R13, (unsigned long long)c->R14, (unsigned long long)c->R15);
    __try
    {
        const uintptr_t sp = c->Rsp;
        int printed = 0;
        for (int i = 0; i < 800 && printed < 44; ++i)
        {
            const uintptr_t v = *reinterpret_cast<uintptr_t*>(sp + (uintptr_t)i * 8);
            if (v > base && v < base + 0x0C000000)
            {
                HxLog("[HalcyonA2][CRASH]   stack[+0x%03X] GAME +0x%llX  (IDA 0x%llX)\n",
                      i * 8, (unsigned long long)(v - base), (unsigned long long)(0x7FF672060000ull + (v - base)));
                ++printed;
            }
            else if (self && v > self && v < self + 0x00400000)
            {
                HxLog("[HalcyonA2][CRASH]   stack[+0x%03X] SELF +0x%llX  (HalcyonA2.map)\n",
                      i * 8, (unsigned long long)(v - self));
                ++printed;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    HxLog("[HalcyonA2][CRASH] ***** end (GAME rvas -> IDA; SELF rvas -> HalcyonA2.map) *****\n\n");
    return EXCEPTION_CONTINUE_SEARCH;
}

// General blocking WinHTTP request: any method, custom headers (CRLF-terminated), optional
// body. Returns the response body (empty on failure); *status receives the HTTP status code.
static std::string HttpReq(const wchar_t* host, int port, const wchar_t* method,
                           const wchar_t* path, const std::string& body,
                           const std::wstring& extraHeaders, DWORD* status = nullptr)
{
    std::string result;
    HINTERNET hSession = WinHttpOpen(L"HalcyonA2/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY,
                                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return result;
    HINTERNET hConnect = WinHttpConnect(hSession, host, (INTERNET_PORT)port, 0);
    if (hConnect)
    {
        // [2026-09-08 TLS] see HttpPostLocal — port 443 => HTTPS, so the mothership/quest calls can reach
        // a Cloudflare-fronted host (https://rigel-ms.<domain>) instead of only a raw ip:90.
        HINTERNET hRequest = WinHttpOpenRequest(hConnect, method, path, nullptr,
                                                WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                                (port == 443) ? WINHTTP_FLAG_SECURE : 0);
        if (hRequest)
        {
            const wchar_t* hdr = extraHeaders.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS
                                                      : extraHeaders.c_str();
            DWORD hdrLen = extraHeaders.empty() ? 0 : (DWORD)-1L;
            if (WinHttpSendRequest(hRequest, hdr, hdrLen,
                                   body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)body.data(),
                                   (DWORD)body.size(), (DWORD)body.size(), 0)
                && WinHttpReceiveResponse(hRequest, nullptr))
            {
                if (status)
                {
                    DWORD code = 0, sz = sizeof(code);
                    WinHttpQueryHeaders(hRequest,
                        WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                        WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX);
                    *status = code;
                }
                DWORD avail = 0;
                while (WinHttpQueryDataAvailable(hRequest, &avail) && avail)
                {
                    std::string chunk(avail, '\0');
                    DWORD read = 0;
                    if (!WinHttpReadData(hRequest, chunk.data(), avail, &read)) break;
                    chunk.resize(read);
                    result += chunk;
                }
            }
            WinHttpCloseHandle(hRequest);
        }
        WinHttpCloseHandle(hConnect);
    }
    WinHttpCloseHandle(hSession);
    return result;
}

// Detect this box's PUBLIC (WAN) IP at runtime — the address clients actually connect to. Queries a
// plain-text IP-echo service, so it returns the public IP even behind NAT (AWS/VPS/home), NOT the
// private adapter IP. Tries a couple of services; returns "" if none give a valid IPv4.
static std::string GetPublicIp()
{
    struct EchoSvc { const wchar_t* host; const wchar_t* path; };
    static const EchoSvc svcs[] = {
        { L"checkip.amazonaws.com", L"/" },   // returns "1.2.3.4\n" (AWS-hosted, reliable on/off EC2)
        { L"api.ipify.org",         L"/" },   // returns "1.2.3.4"
        { L"ifconfig.me",           L"/ip" }, // returns "1.2.3.4"
    };
    for (const auto& s : svcs)
    {
        DWORD code = 0;
        std::string r = HttpReq(s.host, 80, L"GET", s.path, "", L"", &code);
        if (code != 200) continue;
        // trim surrounding whitespace/newlines
        size_t b = r.find_first_not_of(" \t\r\n");
        size_t e = r.find_last_not_of(" \t\r\n");
        if (b == std::string::npos) continue;
        r = r.substr(b, e - b + 1);
        // validate it's a bare IPv4 (7..15 chars, exactly 3 dots, digits+dots only)
        int dots = 0; bool ok = !r.empty() && r.size() <= 15;
        for (char c : r) { if (c == '.') ++dots; else if (c < '0' || c > '9') { ok = false; break; } }
        if (ok && dots == 3 && r.size() >= 7) return r;
    }
    return "";
}

// Pull a string value out of a flat JSON object: "key":"value".
static std::string ExtractJsonString(const std::string& json, const std::string& key)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle);
    if (k == std::string::npos) return "";
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return "";
    size_t q1 = json.find('"', colon);
    if (q1 == std::string::npos) return "";
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return "";
    return json.substr(q1 + 1, q2 - q1 - 1);
}

// Pull an integer out of JSON: "key": 123  (search starts at `from`).
static int ExtractJsonInt(const std::string& json, const std::string& key, size_t from = 0)
{
    const std::string needle = "\"" + key + "\"";
    size_t k = json.find(needle, from);
    if (k == std::string::npos) return 0;
    size_t colon = json.find(':', k + needle.size());
    if (colon == std::string::npos) return 0;
    return atoi(json.c_str() + colon + 1);
}

// Standard base64 decode (ignores non-alphabet chars / padding).
static std::string Base64Decode(const std::string& in)
{
    static int T[256];
    static bool init = false;
    if (!init)
    {
        for (int i = 0; i < 256; ++i) T[i] = -1;
        const char* a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[(unsigned char)a[i]] = i;
        init = true;
    }
    std::string out;
    int val = 0, bits = -8;
    for (unsigned char c : in)
    {
        if (T[c] == -1) continue;
        val = (val << 6) + T[c];
        bits += 6;
        if (bits >= 0) { out.push_back(char((val >> bits) & 0xFF)); bits -= 8; }
    }
    return out;
}

// ---- Mothership per-player quest fetch (backend on 157.173.194.216:90) -------
// Confirmed flow: POST auth begin+complete (QUEST attestation is not validated) -> token
// (JWT sub = PlayerId GUID) -> GET userdata key_name=player_quests. The stored `value` is
// base64( each JSON byte - 1 ); we decode + (+1) back to the FA2QuestProgression JSON.
// [2026-09-08 DEPLOYABILITY] These were hardcoded (a public VPS IP + the shared secret baked into the
// binary), so the DLL broke the moment the backend moved and the key shipped inside the .dll. Now
// overridable from the process command line, with the previous values kept as defaults so existing
// setups are unchanged:
//     -MothershipHost=<ip-or-host>   -MothershipPort=<n>   -ServerApiKey=<hex>
// Set these in tools\server.config.psd1 (MothershipHost / MothershipPort / ServerApiKey) and
// Start-Server.ps1 passes them through. ROTATE the key if the binary has been shared.
// [2026-09-08] Default changed from 157.173.194.216 (a THIRD PARTY's server — not this project's) to
// the project's own Cloudflare-fronted mothership, which answers on 443 only.
static std::wstring g_motherHost   = L"rigel-ms.wwiggles.org";
static int          g_motherPort   = 443;
// Shared secret with the backend (MothershipServer.SERVER_API_KEY). MUST match that value exactly.
// Sent as x-server-api-key so (a) our dummy-attestation quest-fetch logins are exempt from Meta
// verification, and (b) we can call the server-only /v1/server/authorized join-gate endpoint.
static std::wstring g_serverApiKey = L"7b93a32d659d7bb73eb74c6f3f46a6a1934d47b383295a878f91ea2852d917d2";
#define kMotherHost   (g_motherHost.c_str())
#define kMotherPort   (g_motherPort)
#define kServerApiKey (g_serverApiKey.c_str())

// Return THIS process's bound UE game UDP port (default 7777, which UE auto-increments to 7778,
// 7779... if the port is already taken by another instance). Enumerates our own process's UDP
// sockets so multiple server instances on one box each register their REAL port instead of a
// hardcoded 7777 (which would publish the same ip:port for all of them). Returns 0 if not bound yet.
// AF_INET is the literal 2 to avoid winsock header-ordering hassle.
static int GetOurListenPort()
{
    const DWORD pid = GetCurrentProcessId();
    ULONG sz = 0;
    GetExtendedUdpTable(nullptr, &sz, FALSE, 2 /*AF_INET*/, UDP_TABLE_OWNER_PID, 0);
    if (!sz) return 0;
    std::vector<unsigned char> buf(sz);
    if (GetExtendedUdpTable(buf.data(), &sz, FALSE, 2 /*AF_INET*/, UDP_TABLE_OWNER_PID, 0) != NO_ERROR)
        return 0;
    auto* tbl = reinterpret_cast<PMIB_UDPTABLE_OWNER_PID>(buf.data());
    int best = 0;
    for (DWORD i = 0; i < tbl->dwNumEntries; ++i)
    {
        if (tbl->table[i].dwOwningPid != pid) continue;
        const unsigned short np = static_cast<unsigned short>(tbl->table[i].dwLocalPort & 0xFFFF); // net order
        const int port = (np >> 8) | ((np & 0xFF) << 8);   // -> host order
        if (port >= 7777 && port <= 7877 && (best == 0 || port < best)) best = port;   // lowest UE port
    }
    return best;
}

static std::string MothershipLogin(const std::string& orgId)
{
    const std::wstring hdr = L"Content-Type: application/json\r\nx-server-api-key: " + std::wstring(kServerApiKey) + L"\r\n";
    HttpReq(kMotherHost, kMotherPort, L"POST", L"/v2/player/client/auth/begin/QUEST",
            "{\"UserId\":\"" + orgId + "\"}", hdr);
    std::string resp = HttpReq(kMotherHost, kMotherPort, L"POST",
            L"/v2/player/client/auth/complete/QUEST",
            "{\"UserId\":\"" + orgId + "\",\"AttestationToken\":\"x\",\"MetaNonce\":\"x\"}", hdr);
    return ExtractJsonString(resp, "Token");
}

// Returns the decoded player_quests JSON for an org id (empty on any failure).
static std::string FetchPlayerQuestsJson(const std::string& orgId)
{
    std::string token = MothershipLogin(orgId);
    HxLog("[HalcyonA2][QUEST] login org=%s tokenLen=%zu\n", orgId.c_str(), token.size());
    if (token.empty()) return "";
    std::wstring wtok(token.begin(), token.end());   // JWT is ASCII
    std::wstring hdr = L"x-server-api-key: " + std::wstring(kServerApiKey) + L"\r\nx-mothership-token: " + wtok + L"\r\n";
    DWORD code = 0;
    std::string resp = HttpReq(kMotherHost, kMotherPort, L"GET",
            L"/v1/userdata/client?key_name=player_quests", "", hdr, &code);
    std::string val = ExtractJsonString(resp, "value");
    HxLog("[HalcyonA2][QUEST] userdata GET http=%lu respLen=%zu valueLen=%zu\n",
          (unsigned long)code, resp.size(), val.size());
    if (code != 200 || val.empty()) return "";
    std::string raw = Base64Decode(val);
    for (char& c : raw) c = char((unsigned char)c + 1);   // Mothership stores each byte -1
    return raw;
}

using ProcessEvent_t = void (*)(SDK::UObject*, SDK::UFunction*, void*);
static ProcessEvent_t ProcessEvent_Orig = nullptr;

// ---------------------------------------------------------------------------
// Vivox VOIP config. UA2VOIPSubsystem has four Config FStrings normally loaded from
// the [/Script/A2.A2VOIPSubsystem] ini section — but only a real dedicated server's
// config carries them, so on our client-as-server they're empty and every getter
// early-outs ("...not running a dedicated server" / "we don't know our issuer or
// domain"). We set them on the live subsystem ourselves. Member offsets from the SDK:
//   Issuer @0x108, Domain @0x118, SigningKey @0x128, Server @0x138  (each an FString).
// Values are string literals (static storage), so the FString Data pointers stay valid
// for the process lifetime; these members are only read, never reassigned/freed.
// ---------------------------------------------------------------------------
static const wchar_t* kVoipIssuer     = L"20066-a2-61679-udash";
static const wchar_t* kVoipDomain     = L"mt2p.vivox.com";
static const wchar_t* kVoipSigningKey = L"1i5DWqTkWfNctrpVhiI6yvFyf8woZ6jZ";
static const wchar_t* kVoipServer     = L"https://mt2p.www.vivox.com/api2";
static bool g_voipConfigDone = false;

// Point an FString member (Data@+0, ArrayNum@+8, ArrayMax@+12) at a wchar_t* literal.
static void SetFStringMember(void* obj, size_t off, const wchar_t* str)
{
    int32_t n = 1;                          // count the null terminator
    for (const wchar_t* p = str; *p; ++p) ++n;
    const uintptr_t p = reinterpret_cast<uintptr_t>(obj) + off;
    *reinterpret_cast<const wchar_t**>(p) = str;   // Data
    *reinterpret_cast<int32_t*>(p + 8)    = n;     // ArrayNum
    *reinterpret_cast<int32_t*>(p + 12)   = n;     // ArrayMax
}

// One-shot: fill the Vivox config on every live UA2VOIPSubsystem. Retries each tick
// until the subsystem exists, then latches.
static void ApplyVoipConfigIfNeeded()
{
    // [PERF 2026-09-09] FindClassFast is NOT a lookup - SDK FindObjectFastImpl walks all
    // ~167k UObjects building a std::string per object to compare names, and a class that
    // is absent scans the WHOLE array. These ran unconditionally on every detector call
    // (DetectGoals did two per call, 8x/s, before its own gN==0 early-return) and were
    // invisible to the walks/s counter because they live in the SDK, not our loops.
    // UClass pointers are rooted and stable for the process lifetime, so cache them.
    // Plain zero-init static: no thread-safe-init guard, so it is legal inside __try (C2712).
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("A2VOIPSubsystem");
    if (!cls)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int applied = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls))
            continue;
        // [2026-09-03 ★ VIVOX 20122 FIX] NEW-BUILD OFFSETS. Confirmed from the login-token builder
        // (UA2OnlineCommunicationsComponent::RequestVivoxLoginToken_Implementation -> sub_7FF677493210):
        // it reads Issuer from subsystem+0xF0 and the HMAC SigningKey from subsystem+0x138. The old 20996
        // offsets (Issuer 0x108 / Domain 0x118 / SigningKey 0x128 / Server 0x138) are STALE here — in
        // particular we were writing kVoipServer to 0x138, which on this build IS the SigningKey field, so
        // Vivox login tokens were HMAC-signed with the server URL as the key -> the Android client got
        // "20122 Invalid Access Token Signature" and voice never connected. (Verified offline: HMAC-SHA256
        // of the client's token with key="https://mt2p.www.vivox.com/api2" reproduces its signature exactly.)
        // Domain + Server load correctly from the packaged [/Script/A2.A2VOIPSubsystem] ini on this build
        // (the runtime AccountHandle domain and the Connector.Create server were both correct WITHOUT our
        // writes), so we no longer poke them — writing them at the old offsets clobbered unrelated new-build
        // fields. Only Issuer (harmless, redundant with the ini) and SigningKey (the fix) are set.
        SetFStringMember(o, 0xF0,  kVoipIssuer);       // new-build Issuer offset (was 0x108)
        SetFStringMember(o, 0x138, kVoipSigningKey);   // new-build SigningKey offset (was 0x128) — 0x138 is NOT Server here
        ++applied;
    }
    if (applied > 0)
    {
        g_voipConfigDone = true;
        printf("[HalcyonA2] applied Vivox VOIP config to %d subsystem instance(s)\n", applied);
    }
}

// NOTE: station board population is handled the ORIGINAL way — the server fetches its
// deployment with include_station_config (sub_54198E0 -> GET {DashboardApiUrl}/v1/deployments/
// {DashboardDeploymentId}?include_station_config=true, x-api-key), and sub_5409770 natively
// applies the returned station_config to the netvars. That requires the -DashboardApiUrl /
// -DashboardApiKey / -DashboardDeploymentId cmdline args + a backend serving those endpoints;
// it is NOT done in this DLL. (The old LoadJSONIntoRootObject path was a client stub and the
// sub_52A0BF0 builder-hook / SetValue netvar hacks were removed once the real path was found.)

// ---------------------------------------------------------------------------
// Vivox login-token "f" claim fix.
//
// RequestVivoxLoginToken_Implementation (sub_53EF080) mints the client's Vivox login
// token but ignores the AccountId and builds the token's "f" (user URI) claim EMPTY
// (via sub_53E7F00 -> the JWT assembler sub_53FD500). Vivox then rejects the login with
// 20123 "Access Token Claims Mismatch" because "f" must equal the account being logged
// in: sip:.<issuer>.<userid>.@<domain>.
//
// We fix it with two hooks: the login-impl hook captures the AccountId and builds the
// URI into a thread-local; the JWT-assembler hook substitutes it into the empty "f"
// argument (a7) before signing. Claims are UTF-8 std::string (MSVC layout: size@0x10,
// capacity@0x18), same ABI as our DLL, so we pass our own std::string straight in.
// ---------------------------------------------------------------------------
static const char* kVoipIssuerA = "20066-a2-61679-udash";
static const char* kVoipDomainA = "mt2p.vivox.com";
static thread_local std::string t_voipLoginF;   // "f" URI for the in-flight login token

// Read a UE FString (wchar Data@0, Num@8) into a narrow ASCII std::string.
static std::string FStringToNarrow(void* fstr)
{
    if (!fstr) return "";
    wchar_t* data = *reinterpret_cast<wchar_t**>(fstr);
    int32_t  num  = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(fstr) + 8);
    if (!data || num <= 1) return "";
    std::string s;
    s.reserve(num);
    for (int32_t i = 0; i < num - 1 && data[i]; ++i) s.push_back(static_cast<char>(data[i]));
    return s;
}

// Build the Vivox user URI for the "f" claim. If AccountId is already a full sip: URI,
// use it verbatim; otherwise wrap the bare id: sip:.<issuer>.<id>.@<domain>.
static std::string BuildVoipUserUri(const std::string& accountId)
{
    if (accountId.empty()) return "";
    if (accountId.rfind("sip:", 0) == 0) return accountId;
    return std::string("sip:.") + kVoipIssuerA + "." + accountId + ".@" + kVoipDomainA;
}

// sub_53FD500 — the Vivox JWT assembler. (out, key, iss, exp, vxa, vxi, f, t); f/t are
// std::string*. When we're inside a login-token request (t_voipLoginF set) and "f" (a7)
// is empty, pass our URI instead so Vivox's claims check passes.
static constexpr uintptr_t JwtBuild_RVA = 0x544D9B0;
using JwtBuild_t = void* (__fastcall*)(void*, void*, void*, unsigned, void*, unsigned, void*, void*);
static JwtBuild_t JwtBuild_Orig = nullptr;
static void* __fastcall JwtBuild_Hook(void* out, void* key, void* iss, unsigned exp,
                                      void* vxa, unsigned vxi, void* f, void* t)
{
    if (!t_voipLoginF.empty() && f &&
        *reinterpret_cast<size_t*>(reinterpret_cast<uintptr_t>(f) + 0x10) == 0)  // f is empty std::string
    {
        std::string uri = t_voipLoginF;   // lives through the (synchronous) build
        return JwtBuild_Orig(out, key, iss, exp, vxa, vxi, &uri, t);
    }
    return JwtBuild_Orig(out, key, iss, exp, vxa, vxi, f, t);
}

// sub_53EF080 — RequestVivoxLoginToken_Implementation. rdx = AccountId (FString*). The
// stock code drops it; we capture it, build the "f" URI, and let JwtBuild_Hook inject it.
static constexpr uintptr_t ReqLogin_RVA = 0x543C7B0;
using ReqLogin_t = __int64 (__fastcall*)(void*, void*);
static ReqLogin_t ReqLogin_Orig = nullptr;
static __int64 __fastcall ReqLogin_Hook(void* comp, void* accountId)
{
    std::string prev = t_voipLoginF;
    std::string acct = FStringToNarrow(accountId);
    t_voipLoginF = BuildVoipUserUri(acct);
    printf("[HalcyonA2] VOIP login-token request: AccountId='%s' -> f='%s'\n",
           acct.c_str(), t_voipLoginF.c_str());
    __int64 r = ReqLogin_Orig(comp, accountId);
    t_voipLoginF = prev;
    return r;
}

// ---------------------------------------------------------------------------
// Vivox channel-join fix. RequestChannelJoinTokens_Impl = UA2VOIPSubsystem::BuildChannelUrl,
// which mints three join tokens (spatial/PTT/echo) via sub_53E7730 but with an empty "f"
// (user URI) AND an empty channel (both the token's "t" and the channel-ids sent to the
// client) — A2 keys the channel off voice-zone game state absent on our headless server. We
// inject one fixed GROUP channel ("confctl-g" = non-positional; everyone hears everyone) so
// connected players share a voice channel. Positional falloff is a later upgrade (per-zone
// channels + the subsystem's AudibleDistance/ConversationalDistance params).
//   sub_53E7730 (join-token builder): set the user URI (JwtBuild_Hook injects "f") + force
//     the channel arg (a4) so the token's "t" = our channel URI.
//   sub_52AD8B0 (sender): fill the empty channel-id outputs with the same URI. token "t" ==
//     channel id, so Vivox's claims check passes.
// [VOIPFIX 2026-09-07] This default used to be the GROUP channel ("confctl-g" = non-positional).
// g_voipChannel is what JoinBuild_Hook / SendJoin_Hook actually inject, and EnsureVoipChannel()
// RETURNED EARLY (leaving this default in place) whenever the deployment/station id was not known
// yet or the UA2VOIPSubsystem was not live yet -- the normal case on a server started without a
// dashboard. Result: clients got a 2D channel and voice had no falloff at all. The default is now
// positional with the game's own authored parameters, so the worst case is "positional with
// default distances" instead of "not positional".
static const wchar_t kVoipChannelUri[] =
    L"sip:confctl-d-20066-a2-61679-udash.halcyon!p-16000-50-0.100-1@mt2p.vivox.com";  // fade %.3f: must match VivoxCore's SIP re-serialization (else token "t" != joined channel -> 20123)
struct FStringView { const wchar_t* Data; int32_t Num; int32_t Max; };
static FStringView g_voipChannel = {
    kVoipChannelUri,
    static_cast<int32_t>(sizeof(kVoipChannelUri) / sizeof(wchar_t)),   // Num incl. null
    static_cast<int32_t>(sizeof(kVoipChannelUri) / sizeof(wchar_t)),
};

// [VOIPFIX] tiny "-Flag=<number>" reader for the game command line (returns def when absent/unparsable).
static double GetCmdLineNumber(const wchar_t* flag, double def)
{
    const wchar_t* cl = GetCommandLineW();
    if (!cl || !flag) return def;
    const wchar_t* p = wcsstr(cl, flag);
    if (!p) return def;
    p += wcslen(flag);
    while (*p == L'"' || *p == L'\'') ++p;
    wchar_t* end = nullptr;
    const double v = wcstod(p, &end);
    return (end == p) ? def : v;
}

// Read the game's own positional-audio params off the live UA2VOIPSubsystem so the channel's
// falloff distances match whatever coordinate units the client's native Set3DPosition push uses
// (the game authored both together).
// ★ 22284 offsets (were STALE 20996 0xF0/0xF4/0xF8 — on this build 0xF0 is the Issuer FString, so
// the old reads returned garbage distances → the positional channel URI got junk falloff params →
// Vivox connected but nobody was audible). Correct 22284 layout (A2_classes.hpp UA2VOIPSubsystem):
// AudibleDistance@0xB8 (int32), AudioFadeIntensityByDistance@0xC0 (double), ConversationalDistance@0xC8 (int32).
static bool ReadVoipPositionalParams(int32_t& audible, int32_t& conversational, double& fade)
{
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("A2VOIPSubsystem");
    if (!cls) return false;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        audible        = *reinterpret_cast<int32_t*>(p + 0xB8);
        fade           = *reinterpret_cast<double*>(p + 0xC0);
        conversational = *reinterpret_cast<int32_t*>(p + 0xC8);
        return true;
    }
    return false;
}

// The channel name above was FIXED ("halcyon") so EVERY station joined the same Vivox channel ->
// players in different stations heard each other. Scope it per-SERVER instead: append this server's
// deployment id (unique per running server; falls back to the station id) to the channel name, so
// each server gets its own channel and no two ever share one. Built once, lazily, when the id is
// known (register runs before any client joins); the persistent std::wstring backs the FStringView.
//
// POSITIONAL by default: Vivox encodes positional-ness IN the channel URI -- "confctl-d" (vs "-g"
// group) plus a "!p-<audibleDistance>-<conversationalDistance>-<audioFadeIntensity>-<audioFadeModel>"
// spec. A "-d" channel makes the client's native Vivox integration push its own transform every tick
// (client-local, no server RPC) and apply distance falloff -> proximity voice. All three token pairs
// (spatial/PTT/echo) route to this one URI, so flipping it flips everyone with no double-audio.
// Kill-switch: env GS_VOIP_POSITIONAL=0 falls back to the old group channel.
static std::wstring g_voipChannelStr;
static int g_voipPositional = -1;   // -1 = unread; 1 = positional; 0 = group (from GS_VOIP_POSITIONAL)
static void EnsureVoipChannel()
{
    if (!g_voipChannelStr.empty()) return;                       // already built
    if (g_voipPositional < 0)
    {
        g_voipPositional = 1;                                    // default ON
        if (auto* e = _wgetenv(L"GS_VOIP_POSITIONAL"))
            g_voipPositional = (e[0] == L'0') ? 0 : 1;
    }
    // [VOIPFIX] Scope the channel per server. If neither id is known yet (no dashboard registration --
    // the default config) fall back to a stable per-process token instead of RETURNING: returning left
    // the static default channel in place, and that default used to be the 2D group channel.
    std::wstring id = !g_deploymentIdW.empty() ? g_deploymentIdW : g_stationId;
    if (id.empty()) id = L"local" + std::to_wstring(static_cast<unsigned>(GetCurrentProcessId()));
    std::wstring safe;                                           // Vivox channel token: alnum/-/_ only
    for (wchar_t c : id) if (iswalnum(c) || c == L'-' || c == L'_') safe += c;
    if (safe.empty()) safe = L"halcyon";

    if (g_voipPositional)
    {
        // Pull distances from the game's own subsystem; defer (retry next call) until it's live so we
        // latch the authored values, not a premature fallback. Guard each with a sane floor anyway.
        // [VOIPFIX] Fall back to the values UA2VOIPSubsystem's own constructor writes (sub_145307BB0:
        // +0xB8 AudibleDistance = 16000, +0xC0 AudioFadeIntensityByDistance = 0.1, +0xC8
        // ConversationalDistance = 50) instead of the old 3200/100/1.0 guesses, and NEVER return -- a
        // return leaves the static default channel in place for this join.
        int32_t audible = 0, conv = 0; double fade = 0.0;
        if (!ReadVoipPositionalParams(audible, conv, fade))
            HxLog("[HalcyonA2][VOIPFIX] no live UA2VOIPSubsystem yet - using the authored defaults\n");
        if (audible <= 0) audible = 16000;  // game default (sub_145307BB0 +0xB8)
        if (conv    <= 0) conv    = 50;     // game default (+0xC8)
        if (fade    <= 0.0) fade  = 0.1;    // game default (+0xC0)
        int fadeModel = 1;                  // 1 = inverse by distance (Vivox default); not a subsystem property
        // Command-line overrides, so the falloff can be tuned without a rebuild:
        //   -VoipAudible=3000 -VoipConv=100 -VoipFade=1.0 -VoipModel=2
        { const double v = GetCmdLineNumber(L"-VoipAudible=", 0.0); if (v > 0) audible   = (int32_t)v; }
        { const double v = GetCmdLineNumber(L"-VoipConv=",    0.0); if (v > 0) conv      = (int32_t)v; }
        { const double v = GetCmdLineNumber(L"-VoipFade=",    0.0); if (v > 0) fade      = v; }
        { const double v = GetCmdLineNumber(L"-VoipModel=",   0.0); if (v > 0) fadeModel = (int)v; }
        if (conv > audible) conv = audible;  // Vivox requires conversationalDistance <= audibleDistance
        wchar_t fadeBuf[32];
        // [VOIPFIX 2026-09-07 — 20123 media-reject fix] Format the fade with EXACTLY 3 decimals, not %g.
        // The token's "t" claim is this string verbatim (JoinBuild_Orig), but VivoxCore on the client
        // re-serializes the channel's Channel3DProperties as "!p-%d-%d-%.3f-%d" when it builds the SIP
        // AddSession URI. So our "%g" fade "0.1" was signed into "t", while the client actually joined
        // "...-0.100-1" -> token target != joined channel -> Vivox drops the media stream with 20123
        // (proven in the Android log: same channel name, fade re-emitted as 0.100). Matching Vivox's
        // %.3f makes our signed "t" already canonical, so parse->serialize on the client is identity.
        swprintf(fadeBuf, 32, L"%.3f", fade);                        // 0.1 -> "0.100"  (matches VivoxCore)
        g_voipChannelStr = L"sip:confctl-d-20066-a2-61679-udash.halcyon" + safe +
                           L"!p-" + std::to_wstring(audible) + L"-" + std::to_wstring(conv) +
                           L"-" + fadeBuf + L"-" + std::to_wstring(fadeModel) + L"@mt2p.vivox.com";
        printf("[HalcyonA2] VOIP channel POSITIONAL (audible=%d conv=%d fade=%g model=%d): %ls\n",
               audible, conv, fade, fadeModel, g_voipChannelStr.c_str());
    }
    else
    {
        g_voipChannelStr = L"sip:confctl-g-20066-a2-61679-udash.halcyon" + safe + L"@mt2p.vivox.com";
        printf("[HalcyonA2] VOIP channel scoped per-server (group): %ls\n", g_voipChannelStr.c_str());
    }
    g_voipChannel.Data = g_voipChannelStr.c_str();
    g_voipChannel.Num  = static_cast<int32_t>(g_voipChannelStr.size()) + 1;   // incl null
    g_voipChannel.Max  = g_voipChannel.Num;
}

// sub_53E7730 — Vivox join-token builder (a1=subsystem, a2=&out, a3=AccountId FString,
// a4=channel FString). Set the user URI for the "f" injection, and force a4 to our channel.
static constexpr uintptr_t JoinBuild_RVA = 0x5432A40;
using JoinBuild_t = __int64 (__fastcall*)(void*, void*, void*, void*);
static JoinBuild_t JoinBuild_Orig = nullptr;
static __int64 __fastcall JoinBuild_Hook(void* subsystem, void* out, void* accountId, void* /*channel*/)
{
    EnsureVoipChannel();   // scope the channel to this server before injecting it
    std::string prev = t_voipLoginF;
    std::string acct = FStringToNarrow(accountId);
    t_voipLoginF = BuildVoipUserUri(acct);
    // [VOIPFIX] log EVERY join (was first-only). A client that joined before the channel was built
    // silently got the static default, and there was no way to see that after the fact.
    {
        const bool positional = g_voipChannel.Data && wcsstr(g_voipChannel.Data, L"confctl-d") != nullptr;
        HxLog("[HalcyonA2][VOIPFIX] join-token: AccountId='%s' f='%s' %s channel='%ls'\n",
              acct.c_str(), t_voipLoginF.c_str(), positional ? "POSITIONAL" : "GROUP(2D!)",
              g_voipChannel.Data);
        printf("[HalcyonA2] VOIP join-token: AccountId='%s' %s channel='%ls'\n",
               acct.c_str(), positional ? "POSITIONAL" : "GROUP(2D!)", g_voipChannel.Data);
    }
    __int64 r = JoinBuild_Orig(subsystem, out, accountId, &g_voipChannel);   // t = our channel
    t_voipLoginF = prev;
    return r;
}

// sub_52AD8B0 — packs 3 (joinToken, channelId) pairs and sends ReceiveChannelJoinTokens.
// channelId args (a3/a5/a7) come in empty; substitute our channel URI for any empty one.
static constexpr uintptr_t SendJoin_RVA = 0x52EE2E0;
using SendJoin_t = __int64 (__fastcall*)(void*, void*, void*, void*, void*, void*, void*);
static SendJoin_t SendJoin_Orig = nullptr;
static void* PickChannel(void* chan)   // FString*: Num@0x8. Fill only if empty.
{
    if (chan && *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(chan) + 8) == 0)
        return &g_voipChannel;
    return chan;
}
static __int64 __fastcall SendJoin_Hook(void* a1, void* tok1, void* chan1, void* tok2,
                                        void* chan2, void* tok3, void* chan3)
{
    EnsureVoipChannel();   // ensure the per-server channel is built before substituting empties
    return SendJoin_Orig(a1, tok1, PickChannel(chan1), tok2, PickChannel(chan2),
                         tok3, PickChannel(chan3));
}


// ============================================================================================
// [REPLAYGUARD] 22284-only server crash: "Unhandled Exception: EXCEPTION_ACCESS_VIOLATION
// writing address 0x24" inside EnterCriticalSection, reached from
//   sub_1453F8EB0 (A2 GameState actor tick, run as a task-graph tick task)
//     -> sub_145358580  GetSubsystem<UA2ReplaySystem>(world+2104)
//       -> sub_145487560 (post-step)  -> sub_145482110 (+0x24 = the EnterCriticalSection return)
// The faulting write at 0x24 is RTL_CRITICAL_SECTION_DEBUG::ContentionCount off a NULL DebugInfo:
// that is what RtlEnterCriticalSection does when the CRITICAL_SECTION is ALL ZEROES (LockCount 0
// reads as "already locked" -> contention path -> DebugInfo->ContentionCount++ -> write to 0x24).
// So the UA2ReplaySystem we are handed has never run its constructor's
// InitializeCriticalSection(this+240) (sub_1452F3BA0) -- i.e. it is a dead/torn-down subsystem the
// collection still hands out across the level travel our server performs.
//
// 20996 does not have this problem at all: the 20996 counterpart of the post-step (0x5440E40) has
// ZERO callers, so the replay recorder never ticked on the old build's server either. Skipping it
// here therefore restores exactly the behaviour the rest of this payload was written against, and
// a headless private server has nothing to record anyway.
//
// The guard is conservative: it only skips when the object does NOT look like a live
// UA2ReplaySystem (vtable off_1484A2B38 + an initialised CRITICAL_SECTION). A healthy subsystem
// ticks normally.
// ============================================================================================
static constexpr uintptr_t ReplayPostStep_RVA = 0x5487560;   // sub_145487560
static constexpr uintptr_t ReplayVtable_RVA   = 0x84A2B38;   // off_1484A2B38 (UA2ReplaySystem)
using ReplayPostStep_t = __int64(__fastcall*)(void*, float);
static ReplayPostStep_t ReplayPostStep_Orig = nullptr;
static volatile LONG    g_replaySkips = 0;
static bool             g_guardReplay = true;   // -NoReplayGuard turns it off

static bool ReplaySystemLooksLive(void* self)
{
    if (!self) return false;
    __try
    {
        const uintptr_t vt = *reinterpret_cast<uintptr_t*>(self);
        if (vt != GetBase() + ReplayVtable_RVA) return false;
        const CRITICAL_SECTION* cs =
            reinterpret_cast<const CRITICAL_SECTION*>(reinterpret_cast<uintptr_t>(self) + 240);
        // ctor does InitializeCriticalSection + SetCriticalSectionSpinCount(0xFA0): an initialised,
        // uncontended CS has LockCount == -1 and a non-null DebugInfo. All-zero == never initialised.
        if (cs->DebugInfo == nullptr && cs->LockCount == 0) return false;
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static __int64 __fastcall ReplayPostStep_Hook(void* self, float dt)
{
    if (!ReplaySystemLooksLive(self))
    {
        const LONG n = InterlockedIncrement(&g_replaySkips);
        if (n == 1 || n == 64 || (n % 4096) == 0)
            HxLog("[HalcyonA2][REPLAYGUARD] skipped UA2ReplaySystem post-step #%ld (self=%p: dead object / uninitialised CRITICAL_SECTION)\n",
                  n, self);
        return 0;
    }
    // Trace what the CRITICAL_SECTION actually looks like on a live object, so a future failure can be
    // told apart from "never initialised" (all zero) and "freed under us" (garbage DebugInfo).
    static volatile LONG s_seen = 0;
    const LONG k = InterlockedIncrement(&s_seen);
    if (k == 1 || (k % 8192) == 0)
    {
        const CRITICAL_SECTION* cs =
            reinterpret_cast<const CRITICAL_SECTION*>(reinterpret_cast<uintptr_t>(self) + 240);
        HxLog("[HalcyonA2][REPLAYGUARD] post-step #%ld self=%p cs=%p DebugInfo=%p LockCount=%d Recursion=%d Owner=%p Sem=%p\n",
              k, self, (const void*)cs, (void*)cs->DebugInfo, (int)cs->LockCount, (int)cs->RecursionCount,
              (void*)cs->OwningThread, (void*)cs->LockSemaphore);
    }
    return ReplayPostStep_Orig(self, dt);
}

// ---------------------------------------------------------------------------------------------
// [REPLAYGUARD] The snapshot itself (sub_145482110) is the only thing in the replay system that
// takes that CRITICAL_SECTION, and it is called from exactly one place (sub_145487560 + 0xA7).
// It walks the world's players/spectators/balls and copies them into the live replay buffer -- a
// pure recording side effect with no gameplay meaning on a headless private server, and one that
// 20996 never ran at all (its counterpart of the post-step, 0x5440E40, has no callers). Skipping
// just this call keeps the post-step's three queue flushes and the per-entity time advance running
// while removing the CRITICAL_SECTION the server keeps dying on.
// -AllowReplaySnapshot restores the stock call.
// ---------------------------------------------------------------------------------------------
static constexpr uintptr_t ReplaySnapshot_RVA = 0x5482110;   // sub_145482110
using ReplaySnapshot_t = void(__fastcall*)(void*, float);
static ReplaySnapshot_t ReplaySnapshot_Orig = nullptr;
static bool             g_skipReplaySnapshot = true;
static volatile LONG    g_replaySnapshotSkips = 0;

static void __fastcall ReplaySnapshot_Hook(void* self, float dt)
{
    if (g_skipReplaySnapshot)
    {
        const LONG n = InterlockedIncrement(&g_replaySnapshotSkips);
        if (n == 1 || (n % 16384) == 0)
            HxLog("[HalcyonA2][REPLAYGUARD] replay snapshot skipped #%ld (self=%p) - no replay recording on a headless server\n", n, self);
        return;
    }
    if (!ReplaySystemLooksLive(self)) return;
    ReplaySnapshot_Orig(self, dt);
}

// Latches / tick timers driven from the ProcessEvent hook (game thread).
static bool      g_trackerDone         = false;
static bool      g_ballSimDone         = false;
static void*     g_ballSimMgr          = nullptr;
static void*     g_ballSimSub          = nullptr;
static ULONGLONG g_lastPhysTick        = 0;
static ULONGLONG g_lastEnableGoals     = 0;
static ULONGLONG g_lastDetectGoals     = 0;
static ULONGLONG g_lastStationCfg      = 0;
static ULONGLONG g_lastOwnWatch        = 0;   // TEMP ownership-watch timer
static int       g_rbLogged            = 0;   // TEMP rollback-RPC trace counter
static int       g_vrPawnCount         = 0;   // live VRPawn count (set by WireVRPawns)
// [PERF/SAFETY] Bumped whenever the live VRPawn count changes - i.e. a match is forming or breaking.
// The cache-rebuild backoff re-arms to its fast interval on any change, so newly-spawned goals/cups
// are picked up within ~500ms at exactly the moments that matter, instead of waiting out an 8s backoff.
static volatile long g_cacheEpoch = 0;
static std::unordered_map<void*, int> g_discSeen;   // TEMP: pawn -> last IN-SIM/SKIPPED verdict
static int       g_lastReconcileCount  = -1;  // player count at last reconcile
static int       g_newestInFrame       = -1;  // newest client input frame seen (rollback clock)
static int*      g_activeSimFramePtr    = nullptr; // frame counter of the sim a player occupies
static int       g_activeSimIdx         = -1;  // simIdx of g_activeSimFramePtr
// [2026-09-08 HOLD-AT-TARGET] The pump publishes its confirmed target (= min(active players' newest) - DELAY)
// here so StepSim_Hook can enforce the input-delay buffer: the NATIVE actor tick advances the sim by
// wall-clock and outran the pump's hold, leaving the sim ~2-4 frames AHEAD of the slowest player's confirmed
// input -> it predicted their inputs (ball reacts before the hit lands) then corrected (ball lags) = the
// residual MI (~30 under active play) and the "ball hit before players / then lags" feel. When the sim is
// at/past this target we feed the native step dt=0: the function still runs fully (no accumulator starvation,
// so none of the historic tick-off freezes) but advances no frame, so inputs arrive before their frame sims.
static volatile long      g_confirmedTarget   = -1;  // -1 = no hold (no active contested sim)
static volatile long long g_confirmedTargetAt = 0;   // when it was published; a STALE target must never hold
// [HOLD-AT-TARGET] DISABLED 2026-09-08 — opt-in via -HoldTarget. It FROZE the sim: the target is
// min(newest)-DELAY across players, but with staggered epochs the two clients sit ~1595 frames apart, so the
// shared sim frame (running at the LEADING player's epoch) is permanently "past" a target derived from the
// LAGGING one -> dt=0 on nearly every tick -> frameAdvViaStep/s collapsed 90 -> 1-12 and the ball stopped
// replicating (user: "it feels better because it isn't replicating at all"). The hold is only sound once all
// players share ONE epoch; until then min(newest) is not a valid target for the shared frame.
static bool               g_holdSimAtTarget   = false;// opt-in: -HoldTarget
// Per-player newest input frame (indexed by playerIdx) — so the step pump can hold the sim behind
// the SLOWEST player's confirmed inputs (min), not the fastest (global max). Holding behind the max
// makes every slower player's inputs perpetually stale = the residual ~15-35 MI.
// [2026-09-08 FRAME-REBASE] `offset`/`offsetSet`/`rawNewest` support the epoch rebase (below): each
// client free-runs its own rollback command-frame from ITS join, so two players staggered by ~3min sit
// ~17300 frames apart in ONE shared sim (which has ONE frame field). The gate rejects |Δ|>100, so our
// resync yanks the shared frame to each submitter -> it ping-pongs between the two epochs every ingest;
// that thrash is the residual MI/MIB "roof" on LAN (not latency). Fix: add a per-player `offset` that maps
// every player's input frames onto ONE canonical wall-clock 90Hz epoch (first player = offset 0), applied
// at ingest and undone on the results-send so each client still sees its own epoch. See [[a2-22284-snap-mi]].
struct PlayerFrame { int simIdx; int newest; ULONGLONG seen; int offset; bool offsetSet; int rawNewest;
                     ULONGLONG yankWinStart; int yankCount; };   // [YANK-BUDGET] see IngestInput_Hook
static PlayerFrame g_playerFrames[256] = {};
// [FRAME-REBASE] DISABLED 2026-09-08 — opt-in only via -FrameRebase. Rewriting the input frame in place
// (inputData+4) FEEDS BACK: that buffer is re-read on later ingests, so our own canonical value returns as
// the next "raw" (proven live: a rejoining player logged `raw 1 -> canon 71202` then `raw 71245` = our own
// canon, flipping its offset +71203 -> -22). The jittering offset lands that player's inputs at inconsistent
// frames so its disc never matches the sim -> "rejoined and cannot touch the ball" (client looks bricked),
// and [MI] spread jumps back to ~20. It is also UNNECESSARY: the clients self-converge their command frames
// after the join window (measured: a 2652-frame gap decayed to ~0), so the staggered-join epoch gap closes on
// its own. Any future epoch alignment must NOT mutate the shared input buffer. See [[a2-22284-snap-mi]].
static bool g_frameRebase = false;  // opt-in: -FrameRebase
// Canonical epoch = a WALL-CLOCK 90Hz timeline anchored on the first rebased ingest. A player's offset is
// derived ONCE (canonicalNow - its raw frame) so the first/only player maps to ~0 and a late joiner maps
// onto the same wall clock. CRITICAL: the offset depends ONLY on wall-clock + this player's own raw frame —
// NEVER on another player's rebased value (the earlier leader-max version did, which mutually bootstrapped
// the offsets to INT_MAX — see [[a2-22284-snap-mi]]). VR runs a hard 90Hz so client clocks track wall-clock
// closely; RB_MAX_OFFSET clamps any pathological derivation so a bad read can never run the frame away again.
static bool      g_rebaseAnchorSet = false;
static ULONGLONG g_rebaseWall0     = 0;
static int       g_rebaseFrame0    = 0;
static constexpr int RB_MAX_OFFSET = 200000;   // ~37 min of frames; any |offset| beyond this is bogus -> 0
static int RebaseCanonicalNow()
{
    const ULONGLONG dtms = GetTickCount64() - g_rebaseWall0;
    return g_rebaseFrame0 + (int)((dtms * 90ULL) / 1000ULL);
}
static SDK::UClass* g_goalCls           = nullptr; // UGoalComponent class (for goal-overlap trace)
static ULONGLONG g_lastBallBuildTick   = 0;
static ULONGLONG g_lastBallStepTick    = 0;
static ULONGLONG g_lastBallOverlapTick = 0;
static ULONGLONG g_lastTeamColor       = 0;   // team-color init throttle
static ULONGLONG g_lastNetTune         = 0;   // net-driver send-rate re-apply throttle
static ULONGLONG g_lastNetRateTune     = 0;   // [NETRATE] per-connection bandwidth-cap re-apply throttle
static ULONGLONG g_lastFreqProbe       = 0;   // frequent-data timestamp probe throttle
static ULONGLONG g_lastBallPos         = 0;   // ball-position probe throttle
static ULONGLONG g_lastBallWire        = 0;   // sim-wire probe throttle
static void*     g_probeEntity         = nullptr; // cached active-remote UA2PlayerEntity for rate sampling
static int       g_probePidx           = -1;
// PING-STAMP FIX: cached {entity, pingMs} for every connected player with a real ping. The server
// knows each player's RTT (PlayerState::GetPingInMilliseconds) but the headless relay never copies it
// into FrequentData.Ping (stays 0), which collapses client-side interpolation -> remote players snap.
// We refresh this list at 1Hz (reflected ping call) and write the ping into both freq-data copies at
// fast-path rate so it survives the relay/replication. Ping@+0x8 within FrequentData; FrequentData is
// at localData@0xF0 and VRPlayerRepData@0x308 on UA2PlayerEntity.
struct FPingTarget { void* entity; float pingMs; };
static FPingTarget g_pingTargets[128];          // up to 115 players/station
static int         g_pingTargetCount = 0;
static bool        g_pingStampEnabled = true;   // console-toggleable A/B switch
// Last non-zero ping per entity. GetPingInMilliseconds() momentarily reads 0 on any network blip;
// without this hold, that transient drops the player from the stamp list for a second -> FrequentData.Ping
// goes 0 -> the other client's interpolation buffer COLLAPSES and latches -> persistent mid-match snap
// (only a grab re-inits it). Holding the last good ping (and flooring to 100 if never seen) means we
// never emit 0, so the collapse can't happen.
static std::unordered_map<void*, float> g_lastGoodPing;
static constexpr float kPingFloorMs = 100.0f;
static volatile long g_ingestPingFixes = 0;   // [SNAPFIX] # of incoming pose RPCs whose Ping we corrected 0->ping (1Hz census)
// [DORM] how many VRPawns we had to pull out of net dormancy, and how many needed an explicit flush.
static long g_dormFixed   = 0;
static long g_dormFlushed = 0;

// TEAM-COLOR STAMP FIX (mirrors the ping fix). Server_SetCurrentColor writes the Mass fragment, but
// the fragment->replicated-copy sync only carries pose (FrequentData), NOT the color, so the entity's
// replicated VRPlayerRepData@0x308 CurrentTeamColor@0x500 / TeamIndex@0x418 stay default and clients
// never see team colors. We write them directly on the entity (same struct that already replicates
// for pose) at fast-path rate. Registered at admit with the TicketManager palette color + team.
// Offsets within the entity object: VRPlayerRepData CurrentTeamColor@0x500 (0x14) / TeamIndex@0x418;
// localData CurrentTeamColor@0x2E8 / TeamIndex@0x200.
struct FColorTarget { void* entity; unsigned char color[0x14]; signed char teamIndex; };
static FColorTarget g_colorTargets[32];
static int          g_colorTargetCount = 0;
static bool         g_colorStampEnabled = true;
static void RegisterColorTarget(void* entity, const unsigned char* color, int teamIndex)
{
    if (!entity || teamIndex < 0) return;
    for (int i = 0; i < g_colorTargetCount; ++i)          // update existing
        if (g_colorTargets[i].entity == entity)
        { memcpy(g_colorTargets[i].color, color, 0x14); g_colorTargets[i].teamIndex = (signed char)teamIndex; return; }
    if (g_colorTargetCount >= 32) return;                 // else append
    g_colorTargets[g_colorTargetCount].entity = entity;
    memcpy(g_colorTargets[g_colorTargetCount].color, color, 0x14);
    g_colorTargets[g_colorTargetCount].teamIndex = (signed char)teamIndex;
    ++g_colorTargetCount;
}
// [PERF] Master switch for the TEMP debug probes/dumps. [PROF] measured them at 250-400ms/s on the
// VPS core - ProbeBallPositions alone peaked at 286ms/s - i.e. a quarter of the server's entire
// capacity spent on instrumentation nobody reads in production. Off by default; -HalcyonDiag re-arms.
static bool g_diag = false;
// [DIAG 2026-09-09] -HalcyonMinimal skips our OPTIONAL per-tick detectors (goals/golf/volleyfall/
// overlaps/physics-sync). Purely to answer one question: are the residual 400ms frames OURS or the
// engine's? If maxDt spikes persist with these off, further DLL optimisation is wasted effort.
// NOT for production - it disables scoring/golf/spleef detection.
static bool g_minimal = false;
// [PERF 2026-09-09] -ParallelGC re-enables gc.AllowParallelGC while KEEPING gc.CreateGCClusters
// OFF. Rationale: the IDA-confirmed crash this guard exists for is in the parallel GC's CLUSTER
// reference pass (range-sorting refs against a cluster's [base,size)). With clustering disabled
// there are no clusters, so that path should never run - while GC reference collection still gets
// to use the 5 otherwise-idle cores instead of stalling the game thread. Measured: >200ms stalls
// occur ONLY inside the game process (a separate spin loop on the same box never exceeded 155ms),
// which points at single-threaded GC. OFF by default - this is the crash guard, so opt in only.
static bool g_parallelGC = false;
// [MI 2026-09-09] Rollback input delay, in frames at 90Hz. The pump confirms up to
// (newest input - g_simDelay). DELAY=2 is only ~22ms of buffer, but a real client sits ~100ms away
// and the server still takes occasional 200ms+ stalls - so inputs routinely arrive later than the
// sim has already confirmed, which is exactly what becomes MissedInputs. Classic rollback tuning:
// input delay must cover network jitter. Raising it trades a little input latency for fewer
// mispredictions (the corrections that make players 'lose the ball'). -SimDelay=N to tune.
static int g_simDelay = 2;
// [HOLD 2026-09-09] Continuous client authority for a ball a client is actively streaming.
// Symptom: while a player HOLDS the ball it teleports behind them. Cause: the existing hit
// injection is ONE-SHOT (g_pendingActive is cleared after a single write), so each streamed
// Server_SendPhysicsPropData snaps the ball to a position that is already ~1 RTT (~100ms) old,
// and between injections the server sim integrates it away again. The ball therefore alternates
// between 'where the sim thinks it is' and 'where the hand was 100ms ago' = the teleport.
// With this on, any ball streamed within HOLD_FRESH_MS is pinned to the stream EVERY frame, so
// the sim cannot diverge and the ball simply trails by latency instead of snapping.
// [ORPHAN-REAP 2026-09-09] Destroy VRPawns whose Controller has been gone for a while. Measured:
// 7 orphaned pawns after only 3 join/disconnect cycles - every disconnect leaks its pawn, and the
// engine keeps replicating each one, so every subsequent join costs more (this compounds into the
// 'ping goes to 1k when players join' report). A pawn can legitimately have a null Controller for a
// moment while possession completes, so only reap after ORPHAN_GRACE_MS of continuous orphanhood.
// OFF by default: destroying actors in a live match is the riskiest thing in this payload.
static bool g_reapOrphans = false;   // -ReapOrphans
static const unsigned long long ORPHAN_GRACE_MS = 30000;
static volatile long g_orphansReaped = 0;   // [DIAG] cumulative reaped orphan pawns
// File-scope (not function-local): an unordered_map with a destructor inside a function that uses
// __try is C2712. Destroy is isolated in its own helper for the same reason.
static std::unordered_map<void*, unsigned long long>* g_orphanSince = nullptr;
static void SafeDestroyOrphan(SDK::UObject* o)
{
    __try {
        auto* fn = o->Class ? o->Class->GetFunction("Actor", "K2_DestroyActor") : nullptr;
        if (fn) { o->ProcessEvent(fn, nullptr); InterlockedIncrement(&g_orphansReaped); }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static bool g_holdAuthority = false;   // -HoldAuthority
static const unsigned long long HOLD_FRESH_MS = 150;
// Matching the sim's ball entries by RootComponent/prim alone is a guess about WHICH component the
// sim stores. Keep the owning ACTOR as well and match by walking each sim entry's Outer chain up to
// it, so any component of that ball matches. (pins/s stayed 0 with component-only matching.)
struct HeldBall { void* actor; void* root; void* prim; SDK::FVector pos, vel; unsigned long long seen; };
static HeldBall g_held[8];
static int      g_heldN = 0;
// [2026-09-09] Is this a plausible HEAP UObject pointer? The [DISC] seat gate was reporting
// PASS on values like 0x00007FF72F92D530 -- those are inside the EXECUTABLE IMAGE, not the heap
// (a real UObject in this process looks like 0x0000018B90766DC0). So pawn+0x328 is NOT a disc
// component on this build and the gate was validating garbage. Reject image-range and misaligned
// values so the diagnostic reports FAIL(bad-ptr) instead of a false PASS.
static bool PlausHeapObj(void* p)
{
    const uintptr_t v = reinterpret_cast<uintptr_t>(p);
    if (v < 0x10000ull || v >= 0x7FFFFFFFFFFFull || (v & 7) != 0) return false;
    const uintptr_t b = GetBase();
    if (v >= b && v < b + 0x20000000ull) return false;   // inside the module image
    return true;
}

// [2026-09-09 ***] BALL OWNERSHIP ARBITRATION.
// SendPhys_Hook used to stamp physicsSync->owningActor(+0xC8) = sender on EVERY incoming
// Server_SendPhysicsPropData, which force-accepted the stream. With ONE player that just
// makes hits land. With TWO OR MORE it makes the ball LAST-WRITER-WINS: every client streams
// its own local prediction of where the ball is, each RPC hands ownership to whoever spoke
// last, and the authoritative ball snaps between their disagreeing predictions. That is
// exactly the reported "it teleports behind players when they are holding it", "it spikes
// out of my hand", and the join-time ping storm (N clients x ~30Hz of accepted, conflicting
// physics writes).
// Grant ownership only when the ball is genuinely free: unowned, already owned by this same
// pawn, or the current owner has gone quiet for BALL_OWN_GRACE_MS. Otherwise leave +0xC8
// alone -- the native Server_SendPhysicsPropData_Implementation then sees owningActor != sender
// and drops the stream by itself, which is the engine's own correct behaviour.
static const unsigned long long BALL_OWN_GRACE_MS = 250;
struct BallOwn { void* sync; void* owner; unsigned long long seen; };
static BallOwn g_ballOwn[16];
static int     g_ballOwnN   = 0;
static bool    g_ballOwnArb = true;         // -NoBallOwnArb restores the old force-accept
static volatile long g_ownGrant = 0;        // [DIAG] streams accepted this second
static volatile long g_ownDeny  = 0;        // [DIAG] streams left for the engine to drop
static volatile long g_ownSteal = 0;        // [DIAG] of the grants, how many took over a stale owner
static volatile long g_holdPins = 0;   // [DIAG] per-frame pins applied, reported in [SEND]
static volatile long g_holdMiss = 0;   // [DIAG] fresh stream existed but NO sim ball matched it.
                                       // pins=0 & miss=0 -> the ball is not in any sim at all;
                                       // pins=0 & miss>0 -> it is, but our root/prim key is wrong.
static bool        g_freqDebug        = true;   // [2026-09-08] ON to measure pose replication rate: A2 pose rides the
                                                // Mass "frequent data" system (NOT vanilla actor replication — the net
                                                // driver is already 90 and was ruled out), so [FREQRATE] localHz (client
                                                // ->server pose rate) vs repHz (server's replicated-copy update rate) is
                                                // the meter for "replication feels laggy". Turn off once measured.
static ULONGLONG g_lastQuestInit       = 0;   // player quest-init throttle
static ULONGLONG g_lastTraining        = 0;   // shooting/goalie practice tick throttle

// ---------------------------------------------------------------------------
// PrintString capture — read a UE FString and echo it to our console.
//
// `fstr` points at an FString = { wchar_t* Data @0x00; int32 Num @0x08; int32 Max }.
// Num counts the null terminator, so an empty string has Num <= 1.
//
// Reads raw memory that may be a bad pointer for a half-built object, so it is
// SEH-guarded — and therefore holds no C++ unwinding objects (MSVC C2712).
// ---------------------------------------------------------------------------
static void PrintCapturedString(void* fstr)
{
    __try
    {
        const uintptr_t p = reinterpret_cast<uintptr_t>(fstr);
        wchar_t* data = *reinterpret_cast<wchar_t**>(p);
        int32_t  num  = *reinterpret_cast<int32_t*>(p + 8);
        if (data && num > 1)
            printf("[HalcyonA2][Print] %ls\n", data);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Pump A2PhysicsSync::SendLatestData() on every physics-sync component. The ball's
// owner (the server, for free balls) simulates it and must call SendLatestData to
// replicate PhysicsSyncRepData to clients. On our client-as-server the engine's
// normal send path doesn't run, so the server sims the ball (it falls) but never
// sends the state -> clients see no free physics. Pump it ourselves (CLAUDE.md §4.3).
// -QuietSims (A/B test for the player-lag flood): when set, ALL our per-tick ball network work is
// skipped — the mgr+0x412 results-send pin, PumpBallSimStep (which emits Client_SendServerSimResults),
// and PumpPhysicsSync (SendLatestData). Balls stop syncing, but if players go smooth in every district
// with this on, our ball sends were the flood crowding out player pose. If still laggy, it's the
// game's own running-gamemode netcode, not our pumps.
static bool g_quietSims = false;
static bool g_pinSimResults = false;  // [2026-09-07] OFF: pinning mgr+0x412 every frame caused the ball snap-back stutter (see the pin site). -PinSimResults re-enables.

static void PumpPhysicsSync()
{
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("A2PhysicsSync");
    if (!cls)
        return;
    static SDK::UFunction* fnSend = nullptr;

    // CACHED sync-component + player-position lists, rebuilt ~1s. The old per-call full GObjects walk
    // (120k+) at 30Hz was a game-thread hog AND it SendLatestData'd every moving ball in EVERY running
    // gamemode to every client (station-wide flood that snapped player pose — "running gamemodes fry
    // it"). Now: iterate the small cached list, and only send a ball that's within ~100m of a player
    // (a ball nobody is near is invisible to everyone -> no reason to replicate it -> the 7 unwatched
    // arenas stop flooding). Proximity IS the "occupied arena" gate, no slot bookkeeping needed.
    static SDK::UObject* syncs[512]; static int nSync = 0;
    static SDK::FVector players[64]; static int nPlayers = 0;
    static ULONGLONG lastRebuild = 0;
    const ULONGLONG now = GetTickCount64();
        // NOTE: this guard used to read "|| nSync == 0", which meant that whenever the scan found
        // NOTHING it re-ran a FULL GObjects walk (a virtual IsA per object, over the entire
        // object array) EVERY TICK, forever - e.g. Station_Prime simply has no goals, so the
        // goal cache never populated and never stopped scanning. A fast desktop core absorbed
        // it; a VPS vCPU did not: it pegged the game thread (client ping ~1s, STEP calls/s 90->25).
        // Still retry while empty, just on a timer instead of every single tick.
    // [BACKOFF] A full GObjects walk costs ~50ms on the VPS core. A fixed 500ms empty-retry
    // meant 3 always-empty caches each scanned 2x/s = ~300ms/s = 30% of wall (measured by
    // [PROF]). Level geometry that is absent stays absent, so back off to 8s; any non-empty
    // result snaps back to 3s (was 1s: [PROF] showed 5 populated caches each doing a ~100ms full
    // walk every second = ~550ms/s. Only LIST DISCOVERY is expensive - positions are read from
    // cached pointers at 10Hz regardless - so a new ball joins its detector within 3s and is
    // then tracked at full rate). s_bo is read before the rebuild resets the count,
    // so it reflects the PREVIOUS scan's result.
    static ULONGLONG s_bo = 500;
    static long s_epoch = -1;
    // STAGGERED: re-arming all six caches to the same interval made a JOIN rebuild every
    // one of them in the same frame - six full 167k walks at once, on top of EnableGoals.
    // The user saw ping spike to ~1s exactly when a second client joined. Spread them out.
    // [JOINDIAG-FIX 2026-09-09] Measured: every join produced rebuilds/s=5-8 and walks/s
    // 8->16, with a single DetectGoals call at 131ms - a ~300ms stall, which is the ~1s
    // client ping spike the user sees whenever someone joins. The previous 260ms stagger
    // still packed all six full 167k walks into 1.3s. Spread them 1.2s apart so at most
    // ONE cache rebuilds per second. The re-arm only buys ~2.6s faster goal pickup at
    // match start (normal cadence is 3s anyway) - not worth a visible spike.
    if (s_epoch != g_cacheEpoch) { s_epoch = g_cacheEpoch; s_bo = 600; }   // re-arm, widely staggered
    if (now - lastRebuild > s_bo)
    {
        s_bo = (nSync == 0) ? ((s_bo < 8000) ? s_bo * 2 : 8000) : 3000;
        InterlockedIncrement(&g_rebuilds);   // [PROF] how often caches actually rebuild
        lastRebuild = now; nSync = 0; nPlayers = 0;
        static SDK::UClass* pawnCls = nullptr;   // [PERF] cached class lookup
        if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
        if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("BP_VRPawn_C");
        const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (nSync < 512 && o->IsA(cls)) { syncs[nSync++] = o; }
            else if (pawnCls && nPlayers < 64 && o->IsA(pawnCls))
            {
                auto* a = static_cast<SDK::AActor*>(o);
                if (a->RootComponent) players[nPlayers++] = a->RootComponent->K2_GetComponentLocation();
            }
        }
    }
    if (nPlayers == 0)   // nobody connected -> nothing to send
        return;

    const double THRESH2 = 1.0e8;   // (10000uu = 100m)^2 — only sync balls a player could see
    // [PUMP] instrumentation: prove the frozen-skip gate is actually engaging. Counts, per ~1s,
    // how many in-range balls we SENT vs SKIPPED-because-frozen. If skipped>0 for a resting
    // heartball, the gate works and any remaining teleport is native client-driven networking.
    static ULONGLONG pumpLastLog = 0; static int pumpSent = 0, pumpFrozen = 0, pumpFirst = 0;
    for (int i = 0; i < nSync; ++i)
    {
        auto* o = syncs[i];
        // SendLatestData derefs PropMovement(@0xA8) + its UpdatedComponent(@0xA8); both null on
        // pooled/inactive components (the null+0x252 crash). Guard, and reuse UpdatedComponent to
        // get the ball's world position for the proximity gate.
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        void* propMovement = *reinterpret_cast<void**>(p + 0xA8);
        if (!propMovement) continue;
        void* updated = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(propMovement) + 0xA8);
        if (!updated) continue;

        SDK::FVector loc = static_cast<SDK::USceneComponent*>(updated)->K2_GetComponentLocation();
        bool inRange = false;
        for (int j = 0; j < nPlayers && !inRange; ++j)
        {
            const double dx = loc.X - players[j].X, dy = loc.Y - players[j].Y, dz = loc.Z - players[j].Z;
            if (dx * dx + dy * dy + dz * dz < THRESH2) inRange = true;
        }
        if (!inRange) continue;   // no player nearby -> don't blast this ball to everyone

        // ONLY send balls the SERVER is actually moving. Client-driven grabbables (heartballs) are
        // FROZEN server-side; SendLatestData'ing their stale pos at 30Hz stomps the client's real
        // networked motion -> the ball snaps between the frozen server pos and the client pos (the
        // "teleport between two points"). If the server pos hasn't changed since last tick, the server
        // isn't simulating this ball -> skip it and let its native replication drive it.
        static std::unordered_map<void*, SDK::FVector> lastPos;
        auto it = lastPos.find(o);
        if (it != lastPos.end())
        {
            const double dx = loc.X - it->second.X, dy = loc.Y - it->second.Y, dz = loc.Z - it->second.Z;
            const bool movedServerSide = (dx * dx + dy * dy + dz * dz) > 0.25;   // >0.5uu since last tick
            lastPos[o] = loc;
            if (!movedServerSide) { ++pumpFrozen; continue; }   // frozen server-side (client-driven) -> don't override
        }
        else { lastPos[o] = loc; ++pumpFirst; continue; }  // first sight -> record, don't send yet

        if (!fnSend) fnSend = o->Class->GetFunction("A2PhysicsSync", "SendLatestData");
        if (!fnSend) return;
        o->ProcessEvent(fnSend, nullptr);
        ++pumpSent;
    }
    if (now - pumpLastLog > 1000)
    {
        pumpLastLog = now;
        printf("[HalcyonA2][PUMP] sent=%d frozen-skip=%d first-skip=%d (nSync=%d nPlayers=%d)\n",
               pumpSent, pumpFrozen, pumpFirst, nSync, nPlayers);
        pumpSent = pumpFrozen = pumpFirst = 0;
    }
}

// AVRPawn::Server_SubmitInputs (sub_5496E10) drops every client input unless
// VRPawn->BallSimManager (@0x1B38) is non-null. On our server the pawn's ref is
// likely never set (the arena/sim-join server path that would set it doesn't run),
// so the inputs we see arriving get discarded before reaching the sim. Point every
// VRPawn at our manager. Also logs the prior value so we learn whether it was null.
static void WireVRPawnBallSimManagers()
{
    if (!g_ballSimMgr)
        return;
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("VRPawn"); // native AVRPawn (catches BP_VRPawn_C)
    if (!cls)
        cls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    if (!cls)
    {
        printf("[HalcyonA2][RB] WireVRPawns: VRPawn class not found\n");
        return;
    }
    int found = 0, wired = 0, orphans = 0;
    // [2026-09-04] Force the [DISC] seat-gate line to re-log ~every 2s (not only on verdict change) so it's
    // catchable in any log window while diagnosing why the player isn't seated.
    static uint64_t s_lastDiscLog = 0;
    const bool forceDisc = (GetTickCount64() - s_lastDiscLog > 2000);
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls))
            continue;
        // [ORPHAN-PAWN 2026-09-09] Skip pawns whose Controller is gone (APawn::Controller @0x2D0).
        // Measured across four join/disconnect cycles: pawnCount only ever CLIMBED (1 -> 6) - a
        // disconnecting player leaves its VRPawn behind. Those orphans inflated the count, fired a
        // g_cacheEpoch bump each time (the rebuild storm behind the join ping spike), kept SEATDRIVE
        // at pawns=6/seated=0, and coincided exactly with MI inverting (MI 0/MIB 44 -> MI 41/MIB 0).
        // Older logs show the same tell as "[QUEST] uninit comp ... pc <null>".
        {
            void* ctrl = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x2D0);
            if (!ctrl)
            {
                ++orphans;
                // Remember when this pawn first appeared controller-less; reap only after the grace window.
                if (!g_orphanSince) g_orphanSince = new std::unordered_map<void*, unsigned long long>();
                const unsigned long long nowO = GetTickCount64();
                auto it = g_orphanSince->find(o);
                if (it == g_orphanSince->end()) { (*g_orphanSince)[o] = nowO; }
                else if (g_reapOrphans && nowO - it->second > ORPHAN_GRACE_MS)
                {
                    g_orphanSince->erase(it);
                    SafeDestroyOrphan(o);
                }
                continue;
            }
        }
        ++found;

        // NET TUNING (player-movement snap fix). Outside the tackleball arenas (where pose rides the
        // forced-90Hz ball sim), remote player bodies snap: player pawns compete with the thousands of
        // static gamemode actors we load across every district for the per-connection actor-replication
        // budget, and get updated rarely. Pin each VRPawn to a high, non-adaptive update rate + high
        // priority so it always wins that budget, and never cull it by distance. AActor net fields:
        // NetCullDistanceSquared@0x170, NetUpdateFrequency@0x178, MinNetUpdateFrequency@0x17C, NetPriority@0x180.
        {
            const uintptr_t p = reinterpret_cast<uintptr_t>(o);
            *reinterpret_cast<uint8_t*>(p + 0x60) |= 0x08;    // bAlwaysRelevant — relevant to every connection regardless
                                                              // of distance from the (single, TKB-parked) relevancy viewer
            *reinterpret_cast<float*>(p + 0x170) = 1.0e12f;   // NetCullDistanceSquared — never distance-cull players
            *reinterpret_cast<float*>(p + 0x178) = 100.0f;    // NetUpdateFrequency — target 100Hz
            *reinterpret_cast<float*>(p + 0x17C) = 100.0f;    // MinNetUpdateFrequency — pin it (defeat adaptive down-throttle)
            *reinterpret_cast<float*>(p + 0x180) = 10.0f;     // NetPriority — win the actor budget over static objects

            // [2026-09-09 *** THE "1 POS UPDATE/SEC" LAG STATE] AActor::NetDormancy @0x159
            // (ENetDormancy, SDK 22284 Engine_classes.hpp:1121 / Engine_structs.hpp:1848:
            //  DORM_Never=0, DORM_Awake=1, DORM_DormantAll=2, DORM_DormantPartial=3, DORM_Initial=4).
            //
            // A DORMANT actor is skipped by the replication driver entirely until something dirties it
            // and calls FlushNetDormancy. A player pawn that goes dormant therefore stops sending pose
            // updates and only trickles out the occasional forced update -- which is exactly the reported
            // "player is only sending 1 pos upd/s". It also explains the reported WORKAROUND precisely:
            // GRABBING the stuck player writes a replicated property on their pawn, which flushes
            // dormancy and wakes replication back up. That is a dormancy symptom, not a bandwidth one
            // (bandwidth was already measured and cleared: [NETRATE] showed caps at 500000, raised=0).
            //
            // We set every other net knob on these pawns but never touched this one. Pin it to DORM_Never
            // so a player pawn can never be put to sleep in the first place.
            unsigned char& dorm = *reinterpret_cast<unsigned char*>(p + 0x159);
            if (dorm != 0)
            {
                const unsigned char was = dorm;
                dorm = 0;                                     // DORM_Never
                ++g_dormFixed;
                // Writing the field does NOT by itself wake an actor the connection has already filed
                // as dormant -- that needs FlushNetDormancy(). Call it so a player already stuck in the
                // laggy state recovers without needing someone to grab them.
                if (was == 2 || was == 3 || was == 4)
                {
                    static SDK::UFunction* fnFlush = nullptr;
                    if (!fnFlush)
                        if (SDK::UClass* ac = SDK::UObject::FindClassFast("Actor"))
                            fnFlush = ac->GetFunction("Actor", "FlushNetDormancy");
                    if (fnFlush)
                    {
                        o->ProcessEvent(fnFlush, nullptr);
                        ++g_dormFlushed;
                    }
                    HxLog("[HalcyonA2][DORM] %s NetDormancy %u -> DORM_Never + FlushNetDormancy "
                          "(this pawn was the 1-update/s lag state)\n", o->GetName().c_str(), was);
                }
            }
        }

        void** ref = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x1B38);
        if (*ref != g_ballSimMgr)
        {
            printf("[HalcyonA2][RB] VRPawn %s BallSimManager@0x1B38 was %p -> wiring to %p\n",
                   o->GetName().c_str(), *ref, g_ballSimMgr);
            *ref = g_ballSimMgr;
            ++wired;
        }

        // DISC-INTO-SIM check: sub_540D970 only adds a player to a ball sim if
        // *(pawn+0x328) != null AND that comp's +0x440 != null (+0x2FA is a byte flag).
        // Log per-pawn, once + on verdict change, so a joining REMOTE pawn always prints
        // (a global cap gets eaten by the local pawn re-logging every second).
        {
            // The reconcile (sub_7FF6774BBD80) seats a world player ONLY if ALL THREE hold (verified in IDA
            // for 22284): pawn+0x328 (disc comp) != null, comp+0x440 != null, comp+0x2FA (byte flag) != 0.
            // The old check tested only 0x328+0x440 -> could log IN-SIM while the player still fails 0x2FA and
            // is skipped -> never seated (outline=0). Report all three so we see the exact gate that fails.
            const uintptr_t pw = reinterpret_cast<uintptr_t>(o);
            void* comp = *reinterpret_cast<void**>(pw + 0x328);
            // [2026-09-09 FIX] Validate before trusting. Previously ANY non-null value at +0x328
            // counted, so an image-range pointer scored PASS while the sim reported participants=0
            // forever -- the diagnostic actively hid the failure.
            const bool compOk = PlausHeapObj(comp);
            void* f440 = compOk ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(comp) + 0x440) : nullptr;
            const bool f440Ok = PlausHeapObj(f440);
            const int  f2fa = compOk ? *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(comp) + 0x2FA) : 0;
            const int verdict = (compOk && f440Ok && f2fa) ? 1 : 0;
            auto it = g_discSeen.find(o);
            if (it == g_discSeen.end() || it->second != verdict || forceDisc)
            {
                // [DISCARRAY 2026-09-07] Passing the three-way gate is NOT enough. Right after it the
                // seater (sub_14545BD80 @0x14545c034..0x14545c108) walks the array at (comp+0x440)+0x2A0
                // with count at +0x2A8, keeps the entries whose virtual [222] returns non-null, and then
                // requires EXACTLY ONE survivor:
                //     if ( (_DWORD)v156 != 1 ) goto LABEL_52;   // -> player is NOT seated
                // So a player that passes the gate but has 0 (or 2+) entries in that array is silently
                // skipped, which is precisely the observed "every sim has participants=0 while [DISC] says
                // PASS". Log the count so the two cases can be told apart.
                int arrN = -1; void* arrD = nullptr;
                if (f440Ok) ReadEntityArray(f440, arrD, arrN);
                HxLog("[HalcyonA2][DISC] %s comp@0x328=%p(%s) +0x440=%p(%s) +0x2FA=%d entities@+0x2A0=%p n=%d => %s (seat needs n-surviving==1)\n",
                      o->GetName().c_str(), comp, compOk ? "heap" : "BAD-PTR",
                      f440, f440Ok ? "heap" : "BAD-PTR", f2fa, arrD, arrN, verdict ? "PASS" : "FAIL");
                printf("[HalcyonA2][DISC] %s comp@0x328=%p(%s) +0x440=%p(%s) +0x2FA=%d entities=%d => %s (reconcile seat-gate)\n",
                       o->GetName().c_str(), comp, compOk ? "heap" : "BAD-PTR",
                       f440, f440Ok ? "heap" : "BAD-PTR", f2fa, arrN, verdict ? "PASS" : "FAIL");
                g_discSeen[o] = verdict;
            }
        }
    }
    if (found != g_vrPawnCount)
    {
        // [JOINDIAG-FIX2] Rate-limit the epoch bump itself. Pawn count climbs in quick succession on
        // a join (0->1->2->3->4), and EVERY bump re-armed all six caches, so the 1.2s staggers collided
        // and rebuilds/s still hit 6 in a single second. One re-arm per 5s is plenty: its only job is to
        // notice geometry that appeared at match start, and the normal cadence is 3s regardless.
        {
            static ULONGLONG s_lastEpochBump = 0;
            const ULONGLONG nowEp = GetTickCount64();
            if (s_lastEpochBump && nowEp - s_lastEpochBump < 5000)
            {
                g_vrPawnCount = found;   // still track the count; just don't storm the caches
                return;
            }
            s_lastEpochBump = nowEp;
        }
        ++g_cacheEpoch;   // [PERF/SAFETY] re-arm cache backoffs on join/leave
        // [JOINDIAG] Mark the exact second a player joins/leaves. The user reports ping spiking to
        // ~1s whenever another player joins, so the join second has to be findable in the log to
        // correlate it against [STEP] maxDt and the [PROF] per-call peaks.
        HxLog("[HalcyonA2][JOINDIAG] pawnCount %d -> %d (epoch=%ld) orphanPawns=%d reaped=%ld <<< JOIN/LEAVE\n",
              g_vrPawnCount, found, (long)g_cacheEpoch, orphans, (long)g_orphansReaped);
    }
    g_vrPawnCount = found;
}

// ---------------------------------------------------------------------------
// GamemodesTracker spawn.
//
// A2.GamemodesTracker::GlobalGetGamemodesInfo reads a global singleton weak-ptr
// (unk_9BD06B0) that a tracker sets on its own BeginPlay; with no tracker in the
// world it logs "No GamemodesTracker in scene". The dedicated-server path that
// would spawn BP_GamemodesTracker never ran on our client-as-server, so we spawn
// it ourselves. It replicates (Net trackedModes), so spawning on the server (we
// hold authority) reaches the whole lobby; each side's BeginPlay sets its own
// local singleton. SpawnActor must run on the game thread, so this is driven from
// the ProcessEvent hook as a latched one-shot.
// ---------------------------------------------------------------------------
static void SpawnGamemodesTrackerIfReady()
{
    if (g_trackerDone)
        return;

    auto* world = SDK::UWorld::GetWorld();
    if (!world)
        return;

    // UWorld::AuthorityGameMode @ 0x158 — non-null once a game world is up.
    auto* gameMode = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(world) + 0x158);
    if (!gameMode)
        return;

    // Only present once Station_Prime / arena content is loaded (not the frontend).
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("BP_GamemodesTracker_C");
    if (!cls)
        return;

    g_trackerDone = true; // latch before spawning (spawn re-enters ProcessEvent)

    SDK::FTransform xform{};
    xform.Rotation.X = 0.0; xform.Rotation.Y = 0.0; xform.Rotation.Z = 0.0; xform.Rotation.W = 1.0;
    xform.Scale3D.X = 1.0; xform.Scale3D.Y = 1.0; xform.Scale3D.Z = 1.0;

    auto* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        world, cls, xform,
        SDK::ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn,
        nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);

    if (actor)
    {
        SDK::UGameplayStatics::FinishSpawningActor(actor, xform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
        printf("[HalcyonA2] spawned BP_GamemodesTracker -> 0x%llX\n", (unsigned long long)actor);
    }
    else
    {
        // [SPAWNRETRY] same as the BallSimManager: UE refuses SpawnActor while a Blueprint
        // construction script is running, and g_trackerDone was already latched, so the tracker was
        // never spawned again ("No GamemodesTracker in scene, cannot return GamemodesInfo!" forever).
        static volatile LONG s_tfails = 0;
        const LONG f = InterlockedIncrement(&s_tfails);
        if (f == 1 || (f % 50) == 0)
            HxLog("[HalcyonA2][SPAWNRETRY] BP_GamemodesTracker spawn refused (attempt %ld) - retrying\n", f);
        g_trackerDone = false;   // <- retry on the next tick
    }

    // [2026-09-01 ★ GC MARK NEUTER — applied HERE, not in Main] The EntryLevel->Station travel GC MUST run
    // first: UEngine::LoadMap verifies the old world was collected and hard-FATALs ("Fatal world leaks
    // detected") if it wasn't. By the time this tracker spawns, that travel + its world-leak verify have
    // already passed. The only GCs left are the sublevel-streaming full collects that flood (systemic
    // half-loaded-object corruption). Neuter CollectGarbageInternal (sub_7FF6732A94D0, RVA 0x12494D0)
    // now -> `xor eax,eax; ret`: no more full mark/sweep -> no flood; old worlds already cleaned; memory
    // just leaks from here (fine for a match-session server). Async-loader RemoveUnreachableObjects is a
    // separate fn, untouched, so streaming keeps working.
    static bool s_gcNeutered = false;
    if (g_neuterGC && !s_gcNeutered)
    {
        s_gcNeutered = true;
        const uintptr_t gcbase = GetBase();
        // Neuter BOTH the blocking-GC entry AND the shared reachability+sweep driver. A94D0
        // (CollectGarbageInternal) is only ONE caller of BF7B0; the sublevel-streaming GC that floods
        // reaches BF7B0 through a DIFFERENT caller, so neutering A94D0 alone left the flood. BF7B0
        // (sub_7FF6732BF7B0, RVA 0x125F7B0) is the mark driver that EVERY GC path funnels through -> ret
        // it and no reachability mark ever runs (the incremental sweep then flags nothing, so nothing is
        // wrongly freed). Deferred to here so the EntryLevel->Station travel's real GC already ran.
        WriteByte(gcbase + 0x12494D0 + 0, 0x31); WriteByte(gcbase + 0x12494D0 + 1, 0xC0); WriteByte(gcbase + 0x12494D0 + 2, 0xC3); // A94D0: xor eax,eax; ret
        WriteByte(gcbase + 0x125F7B0 + 0, 0x31); WriteByte(gcbase + 0x125F7B0 + 1, 0xC0); WriteByte(gcbase + 0x125F7B0 + 2, 0xC3); // BF7B0: xor eax,eax; ret
        printf("[HalcyonA2] ★ FULL GC NEUTERED (post-travel): CollectGarbageInternal (0x12494D0) + reachability driver BF7B0 (0x125F7B0) -> ret (no flood; memory leaks by design)\n");
    }
}

// The ball sim is the ABallSimManager actor; offline the OfflineBallSimSubsystem
// spawns it, but on our forced-dedicated server that spawn never fired (managers=0)
// even though the subsystem already holds all 70 props + a local player. Spawn the
// manager ourselves on authority and wire it back to the subsystem (deferred spawn,
// CLAUDE.md §5 R6: set the back-ref BEFORE BeginPlay). It has Net properties
// (SimulationsOutline, Stats) so once it ticks its state replicates to clients.
static void SpawnBallSimManagerIfNeeded()
{
    if (g_ballSimDone)
        return;
    auto* world = SDK::UWorld::GetWorld();
    if (!world)
        return;
    static SDK::UClass* mgrCls = nullptr;   // [PERF] cached class lookup
    if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
    static SDK::UClass* subCls = nullptr;   // [PERF] cached class lookup
    if (!subCls) subCls = SDK::UObject::FindClassFast("OfflineBallSimSubsystem");
    if (!mgrCls || !subCls)
        return;

    // Find the already-populated offline subsystem instance.
    SDK::UObject* sub = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(subCls)) { sub = o; break; }
    }
    if (!sub)
        return;

    // If a manager already exists (weakptr @0x3C set), don't double-spawn.
    int32_t mgrIdx = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(sub) + 0x3C);
    if (mgrIdx != 0)
    {
        g_ballSimDone = true;
        return;
    }

    g_ballSimDone = true;    // latch before spawning (spawn re-enters ProcessEvent)
    g_ballSimSub  = sub;

    SDK::FTransform xform{};
    xform.Rotation.W = 1.0;
    xform.Scale3D.X = 1.0; xform.Scale3D.Y = 1.0; xform.Scale3D.Z = 1.0;

    auto* actor = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        world, mgrCls, xform,
        SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!actor)
    {
        // [SPAWNRETRY 2026-09-07] g_ballSimDone was latched BEFORE the spawn, so a single failure
        // permanently disabled the ball sim for the whole server lifetime. And the spawn DOES fail,
        // intermittently: this runs off the ProcessEvent hook, which frequently lands while the engine
        // is inside a Blueprint construction script, and UE refuses SpawnActor there --
        //   "LogSpawn: Warning: SpawnActor failed because we are running a ConstructionScript (BallSimManager)"
        // Every run with that warning had managers=0 forever, no BallSimManager, no ball simulation at
        // all; runs that happened to miss the construction script worked. Un-latch and retry next tick.
        static volatile LONG s_fails = 0;
        const LONG f = InterlockedIncrement(&s_fails);
        if (f == 1 || (f % 50) == 0)
            HxLog("[HalcyonA2][SPAWNRETRY] BallSimManager spawn refused (attempt %ld; usually 'running a ConstructionScript') - retrying\n", f);
        g_ballSimDone = false;   // <- retry on the next tick
        return;
    }

    // R6: set ABallSimManager::OfflineBallSimSubsystem @0x2F0 before BeginPlay so the
    // manager wires itself to the subsystem (and, we hope, registers back into it).
    // [2026-09-01 BISECT] OfflineBallSimSubsystem is the CLIENT-side offline sim; wiring the manager to
    // it on a headless server is the suspected flood source. Gate the wire to test spawn-WITHOUT-wire.
    static bool g_wireOfflineSubsystem = true;   // [2026-09-04] 20996 parity: wire mgr+0x2F0 BEFORE BeginPlay
    if (g_wireOfflineSubsystem)
        *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(actor) + 0x2F0) = sub;
    else
        printf("[HalcyonA2] [BISECT] SKIPPED OfflineBallSimSubsystem wire (mgr+0x2F0) - testing spawn-without-wire\n");

    SDK::UGameplayStatics::FinishSpawningActor(actor, xform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    g_ballSimMgr = actor;

    // [2026-09-01 OFFLINE-MATCH] The offline client (balls WORK) spawns this manager via the game's own
    // sub_7FF67743C730 and lets it TICK NATIVELY — its per-frame tick reconciles+PRUNES so the sim set
    // stays at 1 (census: simset@0x308=1, no flood). Our forced-dedicated netmode(1) makes the game skip
    // that spawner (it only spawns on netmode 2/ListenServer or 0/Standalone), so we spawn our own — but
    // we were DISABLING its tick, so nothing pruned the accumulating -2 sims -> the GC walks millions of
    // stale/null refs = the flood. FIX: leave the tick ENABLED (match offline) and set the ballsim world
    // global qword_7FF67BCBC880 that the game spawner sets (some sim paths key off it). Our own reconcile/
    // step pumps stay OFF (g_ballPumpsEnabled=false) so we don't double-drive the native tick; a tiny
    // always-on arm (mgr+0x412=1) keeps the results-send firing to clients.
    actor->SetActorTickEnabled(true);   // tick ENABLED — native reconcile/step/prune, exactly like offline
    // qword_7FF67BCBC880 = ballsim owning-world global the game's spawner (sub_7FF67743C730) sets.
    *reinterpret_cast<void**>(GetBase() + 0x9C5C880) = reinterpret_cast<void*>(SDK::UWorld::GetWorld());
    printf("[HalcyonA2] spawned ABallSimManager -> 0x%llX (tick ENABLED, offline-match; world-global set)\n",
           (unsigned long long)actor);
    // NOTE: this hand-spawn path is now DISABLED (g_ballsimEnabled=false); the game spawns the manager
    // itself via the spawn-gate patch. Kept as a fallback only.
}

// [2026-09-02 ★ EXPERIMENT — DELAYED NATIVE BALLSIM SPAWN] Hypothesis (user): the GC flood is because the
// ball sim spawns/runs at world-init WHILE other objects (half-built UA2HeartCoreComponent etc.) are still
// being constructed, so its first reference-gather/GC walk chokes on their garbage. Test: keep the game's
// own spawner gate CLOSED at boot (so its one-shot world-init call no-ops), then AFTER g_trackerDone + a
// delay (components fully built by then), open the gate and CALL the game's own spawner ourselves. The
// spawner (sub_7FF67743C730) is a UOfflineBallSimSubsystem method (vtable-only, one-shot) that checks
// GetNetMode==2||==0 then SpawnActor(ABallSimManager) + sets the ballsim world global. We invoke it with
// the subsystem instance as `this`. If the flood is gone -> early-spawn-on-half-built was the trigger.
static bool     g_delayBallSimSpawn   = false;   // EXPERIMENT OFF (manual out-of-band spawner call fatals: "Failed to find ReceiveBeginPlay in OfflineBallSimSubsystem" — the subsystem lifecycle method can't be relocated). Native spawn at boot instead.
static int      g_ballSimSpawnDelayMs = 20000;   // spawn 20s after the tracker/world is up
static bool     g_ballSimGatePatched  = false;   // latch: delayed spawn fired
static void DelayedNativeBallSimSpawn()
{
    if (g_ballSimGatePatched) return;
    static SDK::UClass* subCls = nullptr;   // [PERF] cached class lookup
    if (!subCls) subCls = SDK::UObject::FindClassFast("OfflineBallSimSubsystem");
    if (!subCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    void* subInst = nullptr;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(subCls)) { subInst = o; break; }
    }
    if (!subInst) { printf("[HalcyonA2][DELAY-EXP] no OfflineBallSimSubsystem instance yet; will retry\n"); return; }
    const uintptr_t base = GetBase();
    WriteByte(base + 0x53DC76A, 0x01);   // open the spawn gate (cmp eax,2 -> cmp eax,1) so our call passes
    reinterpret_cast<__int64(__fastcall*)(void*)>(base + 0x53DC730)(subInst);   // the game's own spawner, NOW
    g_ballSimGatePatched = true;
    printf("[HalcyonA2][DELAY-EXP] LATE native BallSimManager spawn fired (delay=%dms) via sub_7FF67743C730(subsystem=%p)\n",
           g_ballSimSpawnDelayMs, subInst);
}
static void SafeDelayedNativeBallSimSpawn() { __try { DelayedNativeBallSimSpawn(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// The manager now exists + is wired, but SimulationsOutline stays 0 — nothing calls
// the "reconcile sims against current players" path (BallSimManager::sub_540D970:
// destroys stale sims, then walks the world player list and builds a sim per valid
// player via sub_540E8A0). On a real server a player-join event drives it; our
// headless join never fires it. Drive it ourselves so sims get built for connected
// VRPawns, then the manager's own tick steps them.
static void PumpBallSimBuild()
{
    if (!g_ballSimMgr)
        return;
    static auto reconcileSims = reinterpret_cast<void (*)(void*)>(GetBase() + 0x545BD80);
    reconcileSims(g_ballSimMgr);
}

// The manager's per-frame stepper (BallSimManager tick, takes deltaSeconds). It runs
// the fixed 90Hz step loop that actually advances the balls. Offline the actor ticks
// itself; on our headless server it doesn't step (balls frozen), so drive it here.
static void PumpBallSimStep()
{
    if (!g_ballSimMgr)
        return;
    static auto stepSim = reinterpret_cast<void (*)(void*, float)>(GetBase() + 0x54863C0);
    const uintptr_t gate = reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x412;
    const float FIXED_DT = 1.0f / 90.0f;   // rollback sim runs at a fixed 90Hz timestep
    const int   DELAY    = g_simDelay;   // [MI] tunable via -SimDelay=N (see g_simDelay)

    // [2026-09-07 STUTTER-SAFE] DO NOT arm the results-send phase unconditionally any more. Arming the
    // gate (mgr+0x412=1) every tick is exactly what g_pinSimResults did — and the user confirmed that
    // pin caused the +894/-894 snap-back stutter, because it force-sends a result for a sim frame the
    // clients have NOT confirmed yet, so their prediction gets stomped. The gate is now armed ONLY inside
    // the catch-up loop below, i.e. only when we step the sim toward a frame the SLOWEST active player
    // has already confirmed (target = newest-input - DELAY). That result matches what clients predicted
    // -> shared state with NO stutter. With no participants/inputs (idle, or a ball nobody is driving)
    // the sim just ticks natively at wall-clock and we send NOTHING extra -> identical to the smooth
    // pump-off baseline, never the pin stutter. This makes -BallPump safe to leave on by default.

    // INPUT-DELAY + CATCH-UP (anti-MI + anti-lag). Target frame = newest confirmed input - DELAY.
    // - If the sim is BEHIND the target (it fell behind at 1 step/tick while inputs arrive faster),
    //   step MULTIPLE fixed frames this tick to CATCH UP (capped) — otherwise it lags forever,
    //   inputs pile up and get dropped as stale (the MI + rubber-band the user saw).
    // - If the sim is AT/PAST the target, HOLD — never run ahead of confirmed inputs.
    // (SEH from the SafePumpBallSimStep wrapper — a stale frame ptr just skips this tick.)
    if (g_activeSimFramePtr && g_newestInFrame >= 0)
    {
        // Hold behind the SLOWEST active player in the active sim (min of their newest frames), so
        // every player's inputs for a frame have arrived before the sim steps it -> no missed inputs.
        // (Holding behind the global max/fastest player left the slower player's inputs stale = MI.)
        const ULONGLONG now = GetTickCount64();
        int minNewest = 0x7FFFFFFF, maxNewest = -1, activeN = 0;
        for (int p = 0; p < 256; ++p)
        {
            const PlayerFrame& pf = g_playerFrames[p];
            if (pf.newest < 0 || pf.simIdx != g_activeSimIdx) continue;
            if (now - pf.seen > 500) continue;   // player stopped submitting -> drop from the min
            if (pf.newest < minNewest) minNewest = pf.newest;
            if (pf.newest > maxNewest) maxNewest = pf.newest;
            ++activeN;
        }
        // [2026-09-08 ORPHAN-RELEASE] The active-sim binding (g_activeSimFramePtr/g_activeSimIdx) is set by
        // the IN hook to whatever sim last received a client input, and is otherwise sticky. When every player
        // leaves that sim (or a player disconnects), NO new input re-points the binding, yet the sim's own
        // frame keeps climbing on the native tick (an entity lingers) -> the binding stays pinned to a DEAD
        // sim whose `target` (= last newest input - DELAY) is frozen while its frame runs away thousands of
        // frames ahead (live: sim=4 simFrame=57875 target=2701 players=0 for 1200+ ticks). A pawn that
        // rejoins then cannot anchor a clean sim, and if it re-seats INTO that runaway sim every input lands
        // tens-of-thousands of frames behind -> instant MI/MIB roof. So: if the active sim has had 0 active
        // participants for a sustained window, RELEASE the binding. The next real input (IN hook) re-anchors
        // a live sim from scratch, and g_newestInFrame resets so the fresh epoch isn't compared to the corpse.
        {
            static ULONGLONG s_staleSince = 0;
            if (activeN == 0)
            {
                if (!s_staleSince) s_staleSince = now;
                if (now - s_staleSince > 2000)   // 2s of nobody feeding this sim = orphaned
                {
                    HxLog("[HalcyonA2][ORPHAN] releasing stale active sim=%d (frame=%d, no participants %llums) "
                          "-> next input re-anchors\n",
                          g_activeSimIdx, *g_activeSimFramePtr, (unsigned long long)(now - s_staleSince));
                    g_activeSimFramePtr = nullptr;
                    g_activeSimIdx      = -1;
                    g_newestInFrame     = -1;
                    s_staleSince        = 0;
                    // fall through to the native wall-clock tick below (no results arming)
                    goto native_tick;
                }
            }
            else
            {
                s_staleSince = 0;
            }
        }

        const int newestForTarget = (activeN > 0) ? minNewest : g_newestInFrame;
        const int target = newestForTarget - DELAY;
        // [HOLD-AT-TARGET] Publish the confirmed frame so StepSim_Hook can stop the NATIVE tick advancing
        // past it (it advances by wall-clock and ignored this hold, leaving the sim ahead of the slowest
        // player's confirmed input -> predicted inputs -> ball reacts early then corrects). Only meaningful
        // with real contested players; -1 disables the hold so idle balls tick normally.
        g_confirmedTarget   = (activeN > 0) ? target : -1;
        g_confirmedTargetAt = (long long)now;

        int budget = 12;   // cap frames/tick so a big backlog can't cause one giant hitch
        int stepped = 0;
        while (*g_activeSimFramePtr < target && budget-- > 0)
        {
            *reinterpret_cast<uint8_t*>(gate) = 1;   // arm the results/send phase (sub_543F2D0)
            stepSim(g_ballSimMgr, FIXED_DT);
            ++stepped;
        }

        static ULONGLONG lastLog = 0;
        if (now - lastLog > 1000)
        {
            lastLog = now;
            HxLog("[HalcyonA2][MI] sim=%d simFrame=%d target=%d players=%d spread=%d (min=%d max=%d) stepped=%d DELAY=%d\n",
                  g_activeSimIdx, *g_activeSimFramePtr, target, activeN,
                  (maxNewest >= 0 ? maxNewest - minNewest : 0), minNewest, maxNewest, stepped, DELAY);
        }
        return;
    }

    // No active sim yet (pre-join / idle) — keep the manager ticking at wall-clock, but do NOT arm the
    // results-send (see STUTTER-SAFE note above): with nobody submitting inputs there is nothing to
    // confirm, and force-sending here is what re-introduced the pin stutter. Native tick only.
    // (ORPHAN-RELEASE above jumps here after dropping a dead active-sim binding.)
native_tick:
    g_confirmedTarget = -1;   // no active/contested sim -> never hold the native tick (idle balls tick normally)
    static ULONGLONG last = 0;
    const ULONGLONG now = GetTickCount64();
    float dt = last ? static_cast<float>(now - last) / 1000.0f : FIXED_DT;
    last = now;
    if (dt > 0.1f) dt = 0.1f;
    stepSim(g_ballSimMgr, dt);
}

// [22284] BALLSIM GARBAGE FIX — drop phantom-only sim builds.
// reconcileSims (0x545BD80) enumerates the WORLD's players and builds a sim per player via the build
// worker sub_7FF6774BCCB0 (0x545CCB0), passing a4 = a TArray<uint8> of PARTICIPANT PLAYER-INDEX bytes
// (each = a pawn's PlayerIndex@0x1C22). The build resolves each byte via AVRPawn::GetPlayerByID(world,
// idx) and, when it returns INVALID (the server's phantom/local-host player, whose index maps to no
// real player), builds the sim entry ANYWAY with a null/invalid player ref — that dangling ref is what
// the parallel GC cluster pass AVs on = the millions-of-null-refs flood. FIX: intercept the build; if
// EVERY participant index resolves invalid (a pure phantom sim), skip it (don't build the garbage
// entry). Real players (>=1 valid index) build unchanged. No array surgery — safe for mixed sims.
using BuildSim_t     = double(__fastcall*)(void*, unsigned int, int, void*, __int64, __int64, char);
using GetPlayerById_t = __int64(__fastcall*)(void*, unsigned int);
using GetSimWorld_t   = __int64(__fastcall*)(void*);
static BuildSim_t g_buildSimOrig = nullptr;
static bool       g_ballFilterPhantom = true;   // drop phantom-only sim builds (the GC-flood source)
static bool       g_skipEmptyPhantomBuild = false;  // [PORT-AUDIT] OFF: tried 2026-09-06 - skipping the empty -2 build faults at 0x547AE4B (the tick derefs the missing -2 entry, RDX=-2, RAX=0)

// [2026-09-03 ★ WHY-FRIES DIAG] Log every distinct BallSim BUILD's participants server-side, so we can
// diff the SERVER against the offline capture. Offline the -2 prediction sim resolves to a real, locally
// controlled pawn: `1->OK(BP_VRPawn_C ctrl=1 loc=1)`. On the headless server the local seat is a phantom,
// so we expect INVALID, or `ctrl=0 loc=0` (no controller / not locally controlled), or empty (count=0)
// builds getting stepped — whichever shows is the exact root of "fine offline, fries on server". Mirrors
// clientdiag.cpp's DescribeBuild. All reads SEH-guarded (pl may not be a pawn). Gate off with g_logBuildSim.
static bool g_logBuildSim = true;
// [2026-09-03] TICK-HANG WATCHDOG shared state (declared here because BuildSim_Hook, below, stamps it; the
// watchdog thread + StepSim_Hook that consume it live further down near g_ballSimNoTick). See TickWatchdog().
static volatile long long g_stepEnterTick = 0;   // GetTickCount64 at StepSim entry
static volatile long long g_stepExitTick  = 0;   // GetTickCount64 at StepSim exit
static volatile long      g_stepSeq       = 0;   // increments each StepSim entry
static volatile int       g_lastOpPhase   = 0;   // last ball-sim sub-op phase (1=reconcile 2=advance 3=buildsim)
static volatile int       g_lastOpSim     = 0;   // simId of that sub-op
static volatile long      g_lastOpSeq     = 0;   // increments each sub-op (progress detector)
static bool BS_FillName(void* p, char* buf, int cap)
{
    auto* o = reinterpret_cast<SDK::UObject*>(p);
    std::string n = o->GetName();
    int k = (int)n.size(); if (k >= cap) k = cap - 1;
    for (int i = 0; i < k; ++i) buf[i] = n[i];
    buf[k] = 0; return true;
}
static bool BS_ReadName(void* p, char* buf, int cap)
{
    __try { return BS_FillName(p, buf, cap); } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void LogBuildSimServer(void* mgr, unsigned int simId, void* a4)
{
    static int s_lastSim = 0x7fffffff, s_lastCount = -1;
    static uint64_t s_lastT = 0;
    int count = 0;
    __try { count = a4 ? *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(a4) + 8) : 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { count = -1; }
    // [2026-09-04] Log distinct (simId,count) IMMEDIATELY, but ALSO re-log every 2s so a steady-state build
    // (the joined player's sim) stays visible instead of scrolling past once at join — we need to see whether
    // the player is a participant and whether GetPlayerByID resolves them (OK vs INVALID) = the seating gate.
    const uint64_t now = GetTickCount64();
    if ((int)simId == s_lastSim && count == s_lastCount && now - s_lastT < 2000) return;
    s_lastSim = (int)simId; s_lastCount = count; s_lastT = now;
    __try
    {
        auto getWorld      = reinterpret_cast<GetSimWorld_t>(GetBase() + 0x3509AB0);
        auto getPlayerById = reinterpret_cast<GetPlayerById_t>(GetBase() + 0x54EB5E0);
        void* world = reinterpret_cast<void*>(getWorld(mgr));
        unsigned char* data = a4 ? *reinterpret_cast<unsigned char**>(a4) : nullptr;
        char line[512];
        int off = _snprintf_s(line, sizeof(line), _TRUNCATE,
            "[DiagBUILD] Index=%d world=%p participants=%d [", (int)simId, world, count);
        if (data && count > 0 && count < 64)
        {
            for (int i = 0; i < count && off < (int)sizeof(line) - 120; ++i)
            {
                __int64 pl = world ? getPlayerById(world, data[i]) : 0;
                char pn[72] = "-"; int hasCtrl = -1, localCtl = -1;
                if (pl)
                {
                    if (!BS_ReadName(reinterpret_cast<void*>(pl), pn, sizeof(pn))) strcpy(pn, "(bad)");
                    __try
                    {
                        void* ctrl = *reinterpret_cast<void**>(static_cast<uintptr_t>(pl) + 0x2D0);   // AVRPawn.Controller
                        hasCtrl = ctrl ? 1 : 0;
                        if (ctrl) localCtl = *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(ctrl) + 0x6C4) ? 1 : 0; // bIsLocalPlayerController
                    }
                    __except (EXCEPTION_EXECUTE_HANDLER) {}
                }
                off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE,
                    "%s%u->%s(%s ctrl=%d loc=%d)", (i ? " " : ""), (unsigned)data[i],
                    pl ? "OK" : "INVALID", pn, hasCtrl, localCtl);
            }
        }
        _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "]");
        HxLog("%s\n", line);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[DiagBUILD] Index=%d (SEH fault reading build args)\n", (int)simId); }
}

static double __fastcall BuildSim_Hook(void* mgr, unsigned int simId, int a3, void* a4,
                                       __int64 a5, __int64 a6, char a7)
{
    g_lastOpPhase = 3; g_lastOpSim = (int)simId; ++g_lastOpSeq;   // watchdog stamp
    if (g_logBuildSim) LogBuildSimServer(mgr, simId, a4);

    // [2026-09-01] NOTE: the Index=-2 IsServer=0 "local" sim is CORRECT — the offline client (where balls
    // work) and the 20996 server BOTH build only -2/IsServer:0 sims and pump results to the network. Do
    // NOT skip or empty negative-index builds; that was a wrong turn (it also crashed callers that look
    // the entry up right after). The flood is a manager-SETUP difference vs the offline subsystem-created
    // manager, not the -2 sim itself. Participant-compaction below still runs for any build with players.
    if (g_ballFilterPhantom && a4)
    {
        __try
        {
            unsigned char* data   = *reinterpret_cast<unsigned char**>(a4);           // TArray<uint8> data
            int*           pcount = reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(a4) + 8); // &.Num
            int            count  = pcount ? *pcount : 0;
            if (g_skipEmptyPhantomBuild && (int)simId < 0 && count == 0)
            {
                // [PORT-AUDIT] headless run 2026-09-06: the first native tick builds the local-player prediction sim
                // ("[DiagBUILD] Index=-2 participants=0 []") holding EVERY ball ("[SIMPART] sim=-2 balls=51"). The
                // phantom filter below only covered count>0, so this empty build always went through on 22284's
                // restructured build worker (0x545CCB0, 0.06 alignment to 20996). Skip it like the phantom-only case.
                static uint64_t s_lastE = 0; const uint64_t nowE = GetTickCount64();
                if (nowE - s_lastE > 5000) { s_lastE = nowE; printf("[HalcyonA2][BALLFILTER] skipped EMPTY phantom sim build (simId=%d)\n", (int)simId); }
                return 0.0;
            }
            if (data && count > 0 && count < 4096)
            {
                // Every participant byte is a pawn PlayerIndex@0x1C22. The build worker resolves each
                // via AVRPawn::GetPlayerByID(world, idx) against the global player registry populated at
                // pawn BeginPlay (sub_7FF67753DE50). For any index that does NOT resolve (a phantom/
                // unregistered player) it STILL builds a sim entry, but with a NULL player ref -> the
                // parallel GC ref-collector AVs on those millions of null refs = the flood. FIX: compact
                // the participant list in place to only indices that resolve to a real player, so every
                // entry the worker builds is valid. If NONE resolve (a pure-phantom arena), skip the whole
                // build. Never drops a build that has >=1 real player -> a genuinely-registered player is
                // never denied a sim; worst case an unregistered player's ball just doesn't sim (benign,
                // not a flood). a4 is a per-call temp array in reconcile/stepSim, freed by ptr after, so
                // shrinking .Num is safe.
                auto getWorld      = reinterpret_cast<GetSimWorld_t>(GetBase() + 0x3509AB0);
                auto getPlayerById = reinterpret_cast<GetPlayerById_t>(GetBase() + 0x54EB5E0);
                void* world = reinterpret_cast<void*>(getWorld(mgr));
                if (world)
                {
                    int valid = 0;
                    for (int i = 0; i < count; ++i)
                        if (getPlayerById(world, data[i]) != 0)
                            data[valid++] = data[i];   // keep only resolvable participants

                    static uint64_t s_last = 0; uint64_t now = GetTickCount64();
                    if (valid == 0)
                    {
                        if (now - s_last > 2000)
                        {
                            s_last = now;
                            printf("[HalcyonA2][BALLFILTER] skipped phantom-only sim build (simId=%u, %d unresolved participant(s))\n",
                                   simId, count);
                        }
                        return 0.0;   // no real players -> don't build any garbage entry
                    }
                    if (valid != count)
                    {
                        *pcount = valid;   // drop the unresolved (null-ref/garbage) participants
                        if (now - s_last > 2000)
                        {
                            s_last = now;
                            printf("[HalcyonA2][BALLFILTER] compacted sim build simId=%u: %d valid / %d total (dropped %d garbage)\n",
                                   simId, valid, count, count - valid);
                        }
                    }
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    return g_buildSimOrig(mgr, simId, a3, a4, a5, a6, a7);
}

// ─────────────────────────────────────────────────────────────────────────────
// [2026-09-02] REMOTE-JOIN CRASH FIX. Once the handshake started succeeding, a real
// VR player joining got all the way to pawn BeginPlay ("Enabling cursor ... this is
// VR and windows") then the server STALLED ~33s and crashed:
//   EXCEPTION_ACCESS_VIOLATION reading 0x0.
// Decoded the crash callstack (recovered VPS ASLR base 0x7FF69F250000, 29/29 frames
// resolved): the fault is in sub_7FF6734963A0 (RVA 0x14363A0) — a Slate widget
// GEOMETRY HIT-TEST. It walks a widget parent chain (`while(w){ ... w=*(w+72) }`) and
// derefs a null widget on the -nullrhi headless server. A corrupt/cyclic widget chain
// explains the 33s stall (spinning the walk) then the null deref. Same family as the
// already-fixed texture-streaming + arena-leave Slate headless crashes.
// Also prominent on the stack: AVRPawn::Server_AttemptTakeOwnershipOfLevelEditor
// (RVA 0x5502070) — the joining player's Blueprint fires the "take ownership of the
// Level Editor" RPC; the server SPAWNS + POSSESSES an ALevelEditorPawn (SpawnActor
// sub_...B1C570 then AController::Possess), whose headless UI construction hits the
// null Slate walk. (Stack frame-to-frame linkage is partly stale — the UE scan walker
// picked up dead slots — so we guard by the reliable crash RIP, not the chain.)
//
// TWO independent, log-confirmable guards so joins work regardless of the true trigger:
//   (1) SEH-wrap the Slate hit-test: on fault return its own empty result {0,0}.
//   (2) Block the LevelEditor-ownership RPC server-side: a joining jakeball player
//       should stay a normal pawn, not spawn a headless editor pawn (also removes the
//       33s stall). Toggle via g_blockLevelEditor if we later want a headless-safe editor.

// (1) Slate widget hit-test SEH guard (crash site sub_7FF6734963A0).
using SlateHitTest_t = __int64*(__fastcall*)(__int64, __int64*, __int64, __int64, char, int);
static SlateHitTest_t g_slateHitTestOrig   = nullptr;
static bool           g_guardSlateHitTest  = false;  // [2026-09-02] DISABLED: wrong target. Crash-callstack base was mis-solved (0x7FF69F250000); the real boot base is 0x7FF69F360000, so sub_7FF6734963A0 was NEVER the crash site (it is a real, safely-running Slate fn). Real crash = sub_7FF673386AD0 @ RVA 0x1326B07, guarded by PawnSpawnCrashVeh.
static uint64_t       g_slateGuardHits     = 0;
static uint64_t       g_slateGuardLastLog  = 0;
// [2026-09-02 v2] SEH-wrapping this does NOT work: UE installs a vectored exception handler
// (the same mechanism as our GC-GUARD/PE-GUARD) which fires BEFORE frame-level __except, so it
// grabs the AV and terminates before our handler runs. And the 33s stall = a huge/corrupt widget
// walk we don't want to run at all headless. So SKIP the original entirely: on a -nullrhi server a
// widget-geometry hit-test is meaningless, and the function's own "nothing found" result is {0,0}.
// No walk -> no stall, no AV, nothing for any handler to fight over. Deterministic.
static bool g_skipSlateHitTest = true;
static __int64* __fastcall SlateHitTest_Hook(__int64 a1, __int64* a2, __int64 a3, __int64 a4, char a5, int a6)
{
    if (g_skipSlateHitTest)
    {
        if (a2) { a2[0] = 0; a2[1] = 0; }   // empty FArrangedWidget array = "no widgets under point"
        ++g_slateGuardHits;
        uint64_t now = GetTickCount64();
        if (now - g_slateGuardLastLog > 3000)
        {
            g_slateGuardLastLog = now;
            printf("[HalcyonA2] ★ Slate hit-test SKIPPED headless (no walk, no crash) sub_7FF6734963A0; total=%llu\n",
                   (unsigned long long)g_slateGuardHits);
        }
        return a2;
    }
    __try { return g_slateHitTestOrig(a1, a2, a3, a4, a5, a6); }
    __except (EXCEPTION_EXECUTE_HANDLER) { if (a2) { a2[0] = 0; a2[1] = 0; } return a2; }
}

// (2) Block the server-side "take ownership of Level Editor" RPC.
using TakeLevelEditor_t = void(__fastcall*)(__int64, void*);
static TakeLevelEditor_t g_takeLevelEditorOrig = nullptr;
static bool              g_blockLevelEditor    = false;  // [2026-09-02] DISABLED: not the trigger (never fired). See PawnSpawnCrashVeh for the real fix.
static void __fastcall TakeLevelEditor_Hook(__int64 a1, void* a2)
{
    if (g_blockLevelEditor)
    {
        printf("[HalcyonA2] ★ BLOCKED Server_AttemptTakeOwnershipOfLevelEditor (headless Slate/editor crash guard) — joining player stays a normal pawn\n");
        return;   // do NOT spawn/possess a headless ALevelEditorPawn
    }
    g_takeLevelEditorOrig(a1, a2);
}

// TEMP diagnostic: HeartBalls replicate/sync between clients but arena JakeBalls don't.
// Are there multiple BallSimManagers (per-arena) we're NOT driving, or one manager whose
// arena sims we don't reconcile? Log every live BallSimManager + its SimulationsOutline
// count (Num@0x358), and the role/repl of jakeball vs heartball actors. Latched to a few
// prints so it isn't spam. Remove once diagnosed.
static int      g_ballDbg     = 0;
static ULONGLONG g_lastBallDbg = 0;
static bool     g_disableMgrRepl = false; // [2026-09-07 BALLSIM] WAS true to kill an Iris GC flood, but the
                                          // BallSimManager carries the SimulationsOutline NET property -
                                          // making it non-replicating means clients never learn which sim a
                                          // ball belongs to, so they can't predict/hit it and the raw result
                                          // stream stutters an unpredicted ball. The flood is now contained by
                                          // InstallGcNumClamp + BuildSim participant-compaction + the GC
                                          // ref-batch clamp, so let it replicate again. -KillMgrRepl forces the
                                          // old behaviour back if the flood returns.

// ★ [2026-09-01] THE GC-FLOOD SOURCE FIX. The flood is UE's GC token interpreter walking a TArray<UObject*>
// with Data<4GB (null/garbage) and a CORRUPT Num=millions — the huge Num drives the million-iteration walk
// (sanitizing the batched pointer VALUES can't stop a COUNT-driven loop). So find that array on its owning
// UObject and clamp Num to 0 before the next GC runs. Scan every non-CDO UObject's first 0x2000 bytes for a
// TArray {void* Data; int32 Num; int32 Max} where Num is absurd (>500k) AND Data is sub-4GB (null/garbage on
// this multi-TB heap) — an unmistakable corrupt-array signature that no legitimate array matches. Clamp Num
// (and Max) to 0 so GC skips it entirely. Runs ~1s (well under the 60s GC interval). Logs the owner once.
static ULONGLONG g_lastArrScan = 0;
static int       g_arrClampLogs = 0;
// Raw scan of one object's memory for a corrupt {Data<4GB, Num>500k} TArray; clamps Num/Max to 0 and
// returns the byte offset found (or -1). No C++ objects here so __try is legal (C2712 otherwise).
// Scan ONE UBallSpawnerComponent's memory for a corrupt object-array token: {Data, Num, Max} where Num is
// absurd (>0x40000) AND Data is NOT a valid heap pointer (0 with Num>0, or non-canonical >0x7FFF_FFFFFFFF,
// or <0x10000). A live component's real TArray/TMap has a valid heap Data + sane Num; a freed/garbage one
// has this signature. CLAMP Num/Max to 0 so the GC token walk terminates immediately. No C++ objects -> __try ok.
static int ScanClampSpawner(uintptr_t p, uintptr_t* outData, unsigned* outNum, unsigned* outMax)
{
    __try
    {
        for (int off = 0x40; off < 0x1000; off += 8)
        {
            uintptr_t data = *reinterpret_cast<uintptr_t*>(p + off);
            unsigned  n    = *reinterpret_cast<unsigned*>(p + off + 8);
            unsigned  mx   = *reinterpret_cast<unsigned*>(p + off + 12);
            // ★ TIGHT signature: a null-Data TArray with Num == 0xFFFFFFFF (-1) — the uninitialized/corrupt
            // array the GC walks (4 billion entries from a null base = the flood). This is UNAMBIGUOUS: no
            // float, count, or valid array is Data==0 && Num==0xFFFFFFFF. (The earlier loose "Num>0x40000 &&
            // badData" also matched FLOAT fields like 0x3F800000=1.0f and zeroed real config -> froze the
            // component. Do NOT loosen this.)
            if (data == 0 && n == 0xFFFFFFFFu)
            {
                *outData = data; *outNum = n; *outMax = mx;
                *reinterpret_cast<unsigned*>(p + off + 8)  = 0;   // Num = 0
                if (mx == 0xFFFFFFFFu) *reinterpret_cast<unsigned*>(p + off + 12) = 0;   // Max = 0 (only if also corrupt)
                return off;
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return -1;
}
static void ClampCorruptArrays()
{
    static SDK::UClass* spawnerCls = nullptr;
    if (!spawnerCls) spawnerCls = SDK::UObject::FindClassFast("BallSpawnerComponent");
    if (!spawnerCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int clamped = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || !o->IsA(spawnerCls)) continue;   // ONLY BallSpawnerComponents — no UClass/UPackage false positives
        uintptr_t data = 0; unsigned n = 0, mx = 0;
        int off = ScanClampSpawner(reinterpret_cast<uintptr_t>(o), &data, &n, &mx);
        while (off >= 0)   // there may be several corrupt fields on the same freed object
        {
            ++clamped;
            if (g_arrClampLogs < 60)
            {
                ++g_arrClampLogs;
                printf("[HalcyonA2][ARRCLAMP] %s (%s)%s +0x%X: Data=0x%llX Num=%u Max=%u -> clamped\n",
                       o->GetName().c_str(), o->Class ? o->Class->GetName().c_str() : "?",
                       o->IsDefaultObject() ? " [CDO]" : "", off, (unsigned long long)data, n, mx);
            }
            off = ScanClampSpawner(reinterpret_cast<uintptr_t>(o), &data, &n, &mx);
        }
    }
    if (clamped && g_arrClampLogs < 60)
        printf("[HalcyonA2][ARRCLAMP] clamped %d corrupt array field(s) on BallSpawnerComponents this pass\n", clamped);
}

static void DumpBallStructure()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    auto* mgrCls   = SDK::UObject::FindClassFast("BallSimManager");
    static SDK::UClass* actorCls = nullptr;   // [PERF] cached class lookup
    if (!actorCls) actorCls = SDK::UObject::FindClassFast("Actor");
    printf("[HalcyonA2][BallDbg] --- ball structure ---\n");
    int mgrCount = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !mgrCls || !o->IsA(mgrCls)) continue;
        int32_t simNum  = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x358); // SimulationsOutline
        int32_t simSet  = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x308); // internal sim TSet count (offline=1, bounded)
        auto* ma = static_cast<SDK::AActor*>(o);
        // [2026-09-01 FLOOD TEST] Offline the manager runs Standalone (no net driver, nothing replicates).
        // On our Iris server it replicates (repl=1) -> Iris builds a reference descriptor for its sim state
        // and the GC then walks a corrupt huge pointer array (millions of null refs) = the flood. Make it
        // NON-replicating (like offline); ball actors still replicate on their own + we pump via pawn RPCs.
        if (g_disableMgrRepl && ma->bReplicates)
        {
            if (SDK::UFunction* fn = o->Class ? o->Class->GetFunction("Actor", "SetReplicates") : nullptr)
            {
                struct { bool b; } parm{ false };
                o->ProcessEvent(fn, &parm);
                printf("[HalcyonA2][BallDbg]   -> called SetReplicates(false) on %s (offline-match, kill Iris descriptor)\n",
                       o->GetName().c_str());
            }
        }
        printf("[HalcyonA2][BallDbg]   BallSimManager %s outline@0x358=%d simset@0x308=%d role=%d remote=%d repl=%d%s\n",
               o->GetName().c_str(), simNum, simSet, (int)ma->Role, (int)ma->RemoteRole, (int)ma->bReplicates,
               (o == g_ballSimMgr) ? " (ours)" : " (native)");
        ++mgrCount;
    }
    // [2026-09-01] Also report the OfflineBallSimSubsystem's BallSimManager weakptr@0x3C (TWeakObjectPtr
    // = int32 ObjectIndex). If the game populated it, that's the NATIVE sim to wire to instead of ours.
    if (auto* subCls = SDK::UObject::FindClassFast("OfflineBallSimSubsystem"))
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject() || !o->IsA(subCls)) continue;
            int32_t mgrIdx = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x3C);
            SDK::UObject* mgrObj = (mgrIdx > 0 && mgrIdx < num) ? SDK::UObject::GObjects->GetByIndex(mgrIdx) : nullptr;
            printf("[HalcyonA2][BallDbg]   OfflineBallSimSubsystem %s -> mgr idx@0x3C=%d obj=%s\n",
                   o->GetName().c_str(), mgrIdx, mgrObj ? mgrObj->GetName().c_str() : "(none)");
        }
    int jake = 0, heart = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !actorCls || !o->IsA(actorCls)) continue;
        std::string n = o->GetName();
        bool isJake  = n.rfind("BP_JakeBall_C", 0) == 0;
        bool isHeart = (n.rfind("BP_HeartBall_C", 0) == 0 || n.rfind("BP_HeartBall_small_C", 0) == 0);
        if (!isJake && !isHeart) continue;
        auto* a = static_cast<SDK::AActor*>(o);
        if (isJake && jake < 8)
        {
            SDK::FVector wl{};
            if (a->RootComponent) wl = a->RootComponent->K2_GetComponentLocation();
            printf("[HalcyonA2][BallDbg]   JAKE  %-26s role=%d remote=%d repl=%d world=(%.0f,%.0f,%.0f)\n",
                   n.c_str(), (int)a->Role, (int)a->RemoteRole, (int)a->bReplicates,
                   (double)wl.X, (double)wl.Y, (double)wl.Z);
        }
        if (isHeart && heart < 4)
            printf("[HalcyonA2][BallDbg]   HEART %-26s role=%d remote=%d repl=%d\n",
                   n.c_str(), (int)a->Role, (int)a->RemoteRole, (int)a->bReplicates);
        if (isJake) ++jake; if (isHeart) ++heart;
    }
    printf("[HalcyonA2][BallDbg]   managers=%d jakeballs=%d heartballs=%d (role: 3=Auth 2=AutoProxy 1=SimProxy)\n",
           mgrCount, jake, heart);
}

// SEH guards for the per-tick sim driving. The step/build/send paths advance rollback
// renderers whose skeletal meshes can be half-initialized (null mesh -> AV in native
// bone-transform code, sub_1B7BC80). Guard so a bad object skips a frame instead of
// killing the process (these wrappers hold no C++ unwinding objects, so __try is legal).
static void SafePumpBallSimBuild()  { __try { PumpBallSimBuild(); }  __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SafePumpBallSimStep()   { __try { PumpBallSimStep(); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SafePumpPhysicsSync()   { __try { PumpPhysicsSync(); }   __except (EXCEPTION_EXECUTE_HANDLER) {} }

// PLAYER-MOVEMENT SMOOTHNESS. Outside the tackleball arenas, player pose rides NORMAL actor
// replication, whose send rate is capped by the net driver's NetServerMaxTickRate. If that's the UE
// default (~30) it's too coarse for VR head/hands -> remote players snap. In TKB the pose instead
// rides the ball rollback sim's Client_SendServerSimResults at 90Hz, bypassing the cap -> smooth.
// That's why per-pawn NetUpdateFrequency=100 did nothing (the DRIVER cap overrides it). Raise the
// driver's rate to 90 so normal replication matches the arena rate everywhere. UNetDriver fields:
// NetServerMaxTickRate@0x84, MaxNetTickRate@0xA0.
static void TuneNetDriver()
{
    auto* world = SDK::UWorld::GetWorld();
    static int dbg0 = 0;
    if (!world || !world->NetDriver)
    {
        if (dbg0 < 20) { ++dbg0; printf("[HalcyonA2][NET][dbg] world=%p NetDriver=%p (null -> waiting)\n",
                                        (void*)world, (void*)(world ? world->NetDriver : nullptr)); }
        return;
    }
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    // VALIDITY GATE — this runs on a 2s ticker that can fire mid-travel, when GWorld is the half-built
    // new world and world->NetDriver is stale/garbage (during LoadMap it briefly pointed at a NON-
    // UNetDriver object; writing NetServerMaxTickRate into it corrupted a shader-map struct -> crash in
    // the material registry reading null+0x10). So prove it's a real, LISTENING GameNetDriver first:
    //   (a) its vtable pointer lands inside the exe image (a genuine UNetDriver vtable is in .rdata),
    //   (b) ClientConnections.Num() (@0xD8) > 0 -> InitListen finished and a client is actually bound.
    // Both reads are cheap ints; the caller (SafeTuneNetDriver) SEH-wraps us so a faulting read on a
    // garbage pointer just aborts this pass instead of crashing.
    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase)
    {
        imgBase = GetBase();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
        imgSize   = nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);       // UNetDriver vtable ptr
    const int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8);   // ClientConnections.Num()
    const int32_t cur      = *reinterpret_cast<int32_t*>(nd + 0x84);   // NetServerMaxTickRate

    // DIAGNOSTIC (throttled ~20x): show what we actually see so we know which gate is failing when the
    // "-> 90" line never prints. Remove once tuning confirmed working.
    static int dbg = 0;
    if (dbg < 20)
    {
        ++dbg;
        printf("[HalcyonA2][NET][dbg] NetDriver=%p vt=%p inImg=%d numConns=%d curRate=%d\n",
               (void*)nd, (void*)vt, (vt >= imgBase && vt < imgBase + imgSize) ? 1 : 0, numConns, cur);
    }

    if (vt < imgBase || vt >= imgBase + imgSize) return;          // not a real in-image object -> bail
    if (numConns <= 0) return;                                    // ClientConnections.Num() == 0 -> not listening yet

    if (cur != 90)
    {
        printf("[HalcyonA2][NET] NetServerMaxTickRate %d -> 90 (MaxNetTickRate -> 90) — un-cap non-arena player pose\n", cur);
        *reinterpret_cast<int32_t*>(nd + 0x84) = 90;   // NetServerMaxTickRate
        *reinterpret_cast<int32_t*>(nd + 0xA0) = 90;   // MaxNetTickRate
    }
}
static void SafeTuneNetDriver() { __try { TuneNetDriver(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-08 ★ POSE-RATE / BANDWIDTH CAP] Measured with two independent probes ([POSERATE] counting real
// Server_SetFrequentData arrivals, and [FREQRATE] X-deltas) that each client's pose stream arrives at only
// ~31-46/s, against a 90Hz sim and a VR client that should push ~72-90/s. The sim is healthy
// (frameAdvViaStep~91, 0 hitches), so this is a NETWORK rate limit, not CPU. FReplicatedFrequentData is
// 0x110 = 272 BYTES; with packet overhead ~300B, UE's stock connection caps (MaxInternetClientRate=10000,
// MaxClientRate=15000 bytes/s) allow only ~33-50 poses/s — which brackets the measured 31 and 46 almost
// exactly, and explains why the two clients differ (per-connection cap, not a fixed server throttle).
// Raise the driver caps AND each live connection's CurrentNetSpeed so pose RPCs are not rate-limited.
// UNetDriver: MaxNetTickRate@0xA0, MaxInternetClientRate@0xA4, MaxClientRate@0xA8,
//             ClientConnections{Data@0xD0, Num@0xD8};  UNetConnection: CurrentNetSpeed@0x38.
static int  g_netSpeedTarget = 200000;   // bytes/s per connection (~660 poses/s of headroom); -NetSpeed=N
static bool g_tuneNetRates   = true;     // -NoNetRateTune to A/B
static void TuneNetRates()
{
    if (!g_tuneNetRates) return;
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->NetDriver) return;
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    // Same validity gate as TuneNetDriver: a real in-image UNetDriver that is actually listening.
    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase)
    {
        imgBase = GetBase();
        auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
        auto* nt  = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
        imgSize   = nt->OptionalHeader.SizeOfImage;
    }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);
    if (vt < imgBase || vt >= imgBase + imgSize) return;
    const int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8);
    if (numConns <= 0 || numConns > 256) return;

    int32_t& maxInet = *reinterpret_cast<int32_t*>(nd + 0xA4);
    int32_t& maxCli  = *reinterpret_cast<int32_t*>(nd + 0xA8);
    const int32_t oldInet = maxInet, oldCli = maxCli;
    if (maxInet < g_netSpeedTarget) maxInet = g_netSpeedTarget;
    if (maxCli  < g_netSpeedTarget) maxCli  = g_netSpeedTarget;

    void** conns = *reinterpret_cast<void***>(nd + 0xD0);
    int raised = 0; int32_t firstOld = -1;
    for (int32_t i = 0; conns && i < numConns; ++i)
    {
        void* c = conns[i];
        if (!c) continue;
        int32_t& speed = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(c) + 0x38);  // CurrentNetSpeed
        if (firstOld < 0) firstOld = speed;
        if (speed < g_netSpeedTarget) { speed = g_netSpeedTarget; ++raised; }
    }
    // Log the ACTUAL values (not the target) and re-log when the connection count changes, so a second
    // client joining is visible. MEASURED 2026-09-08: caps were ALREADY 500000 and raised=0 — the stock
    // 10000/15000 defaults do NOT apply here, so bandwidth is NOT what limits the pose rate. Theory dead.
    static int s_lastConns = -1;
    if (s_lastConns != numConns || raised)
    {
        s_lastConns = numConns;
        const int32_t nowSpeed = (conns && numConns > 0 && conns[0])
            ? *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(conns[0]) + 0x38) : -1;
        printf("[HalcyonA2][NETRATE] MaxClientRate %d (now %d) MaxInternetClientRate %d (now %d) | conns=%d "
               "CurrentNetSpeed(first) %d (now %d) raised=%d\n",
               oldCli, maxCli, oldInet, maxInet, numConns, firstOld, nowSpeed, raised);
    }
}
static void SafeTuneNetRates() { __try { TuneNetRates(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SNAP DIAGNOSIS. Remote players snap because the client can't interpolate their pose: each pose
// snapshot (FReplicatedFrequentData) carries FTimestamp{Seconds@0,fractional@4} (server clock), and
// the client lerps buffered snapshots against it. This probe reads, per VRPawn on the SERVER, that
// timestamp from LocalFreqData (AVRPawn+0xF20 -> Timestamp@+0xF20/.frac@+0xF24, Ping@+0xF28) once/sec.
// If Seconds/frac advance smoothly ~real-time, the server stream is fine and the bug is client
// interp / server-time sync; if frozen/zero/sparse, the server isn't producing/stamping it and we fix
// it server-side. PlayerIndex@0x1C22 identifies the pawn.
static void ProbeFreqTimestamps()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("VRPawn");
    if (!cls) cls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    if (!cls) return;
    // Server world-time sync: clients sync their clock to AGameState.ReplicatedWorldTimeSecondsDouble
    // (@0x2C8, Net). If it's frozen/zero the server isn't advancing/replicating world time -> clients
    // can't place the pose Timestamps on their timeline -> snap. Compare its value to the [FREQ] ts.
    if (g_freqDebug) if (auto* w = SDK::UWorld::GetWorld())
    {
        void* gs = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(w) + 0x160);   // UWorld.GameState
        if (gs)
        {
            const double rwtsD = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(gs) + 0x2C8);
            const float  rwtsF = *reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(gs) + 0x2C4);
            printf("[HalcyonA2][WTIME] ReplicatedWorldTimeSecondsDouble=%.3f float=%.3f\n", rwtsD, rwtsF);
        }
        else printf("[HalcyonA2][WTIME] GameState=null\n");
    }

    // [PARR] TEMP diagnostic: dump every PlayerState (GameState.PlayerArray@0x2B0) with its
    // CompressedPing@0x2A0, bIsSpectator@0x2A2 bit1, and reflected GetPingInMilliseconds. Goal: does
    // the SPECTATOR's PlayerState have a real ping, or is CompressedPing 0 (so its client thinks its
    // own ping is 0 -> snaps everyone it watches)? VR players' PlayerState ping worked (62ms).
    if (g_freqDebug) if (auto* w2 = SDK::UWorld::GetWorld())
    {
        void* gs = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(w2) + 0x160);
        if (gs)
        {
            void** arr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(gs) + 0x2B0);
            const int pnum = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(gs) + 0x2B8);
            for (int k = 0; arr && k < pnum && k < 12; ++k)
            {
                auto* ps = reinterpret_cast<SDK::UObject*>(arr[k]);
                if (!ps) continue;
                const uintptr_t psa = reinterpret_cast<uintptr_t>(ps);
                const uint8_t cping = *reinterpret_cast<uint8_t*>(psa + 0x2A0);
                const bool isSpec = (*reinterpret_cast<uint8_t*>(psa + 0x2A2) >> 1) & 1;
                float pingMs = -1.0f;
                if (ps->Class) { auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
                                 if (fn) { char pb[8] = {}; ps->ProcessEvent(fn, pb); pingMs = *reinterpret_cast<float*>(pb); } }
                const wchar_t* nm = *reinterpret_cast<wchar_t**>(psa + 0x330);   // PlayerNamePrivate.Data
                printf("[HalcyonA2][PARR] ps[%d]=%s name='%ls' compressedPing=%u spec=%d GetPingMs=%.1f\n",
                       k, ps->GetName().c_str(), nm ? nm : L"", cping, isSpec ? 1 : 0, pingMs);
            }
        }
    }

    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int shown = 0;           // gates per-pawn debug prints to the first few (spam guard at scale)
    g_pingTargetCount = 0;   // rebuild the ping-stamp target list each pass
    for (int32_t i = 0; i < num; ++i)   // process EVERY player (up to 115/station), not just the first 8
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const int      pidx = *reinterpret_cast<unsigned char*>(p + 0x1C22);

        // [2026-09-08 STALE-OFFSET FIX] AVRPawn.Entity is @0x928 on 22284 (SDK A2_classes.hpp: `class
        // AVRPawn` -> `class UA2PlayerEntity* Entity; // 0x0928`). The port had the 20996 value 0x840, so
        // EVERY pawn->Entity read returned null and silently bailed: [FREQ] logged `Entity=null` for all
        // pawns, g_probeEntity was never cached (so [FREQRATE] never printed), team-colour replication
        // no-opped, and the spleef finish detector never fired. Diagnostics were blind because of this.
        // Real player pose is on the Entity (AVRPawn.Entity@0x928 -> UA2PlayerEntity), NOT the pawn's
        // LocalFreqData (that's a client-only mirror, zero on the server). Two copies of
        // FReplicatedVRPlayerData, whose FrequentData{Timestamp@0,Ping@8,Root.pos@10} is at offset 0:
        //   localData@0xF0        = server's working copy from the incoming Server_SetFrequentData RPC
        //   VRPlayerRepData@0x308 = the Net-replicated copy actually sent to other clients
        void* entity = *reinterpret_cast<void**>(p + 0x928);
        if (!entity) { if (g_freqDebug && shown < 8) printf("[HalcyonA2][FREQ] pawn=%s pidx=%d Entity=null\n", o->GetName().c_str(), pidx); ++shown; continue; }
        const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
        auto rd = [&](uintptr_t off, int32_t& s, unsigned& f, float& pg, double& x) {
            s  = *reinterpret_cast<int32_t*>(e + off + 0x0);
            f  = *reinterpret_cast<uint16_t*>(e + off + 0x4);
            pg = *reinterpret_cast<float*>(e + off + 0x8);
            x  = *reinterpret_cast<double*>(e + off + 0x10);   // Root.position.X
        };
        int32_t ls, rs = 0; unsigned lf, rf = 0; float lp, rp = 0.0f; double lx, rx = 0.0;
        rd(0xF0,  ls, lf, lp, lx);   // FrequentDataReplicationOnly — on 22284 this IS the replicated copy
        // NOTE: the old `rd(0x308, ...)` read discGrabChainCheckedPlayers (a TArray) as if it were a second
        // pose copy, so it always printed ts=0/X=0 and made a healthy stream look like a dead sync.

        // Does the server actually KNOW this player's ping? Call APlayerState::GetPingInMilliseconds
        // (reflected, returns ExactPing-ms or compressedPing*4). PlayerState @ pawn+0x2B8. If nonzero,
        // the fix is to feed it into FrequentData.Ping; if zero, the server isn't measuring RTT at all.
        float psPingMs = -1.0f;
        auto* ps = *reinterpret_cast<SDK::UObject**>(p + 0x2B8);
        if (ps && ps->Class)
        {
            auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
            if (fn) { char pb[8] = {}; ps->ProcessEvent(fn, pb); psPingMs = *reinterpret_cast<float*>(pb); }
        }
        // REP columns dropped: there is no second copy on 22284 (0x308 is a TArray), so printing it only
        // ever showed zeros and looked like a dead sync. 0xF0 IS the replicated pose.
        if (g_freqDebug && shown < 8)
            printf("[HalcyonA2][FREQ] pidx=%d pose ts=%d+%u ping=%.1f X=%.0f | PS.ping=%.1fms%s\n",
                   pidx, ls, lf, lp, lx, psPingMs, (psPingMs < 0.0f) ? "  (phantom/host - not a client)" : "");
        (void)rs; (void)rf; (void)rp; (void)rx;
        // Cache an ACTIVE REMOTE CLIENT for the fast-path rate sampler. Gate on the REAL (0xF0) pose — the
        // old gate used the bogus 0x308 read, which was always 0, so g_probeEntity was NEVER cached.
        // ALSO require a genuine PlayerState ping: the server's own phantom/host pawn has a frozen pose
        // (constant ts/X, ping=100 default) and PS.ping=-1, and being last in GObjects it kept winning the
        // cache -> [FREQRATE] reported localHz=0 for a pawn that simply never moves.
        if ((ls != 0 || lx != 0.0) && psPingMs >= 0.0f) { g_probeEntity = entity; g_probePidx = pidx; }
        // NOTE: ping-stamp targets are now built by the UA2PlayerEntity walk BELOW (class-agnostic), so
        // SPECTATOR pawns (BP_SpectatorPawn / camera pawn — NOT a VRPawn) are covered too. This VR-pawn
        // loop stays only for the [FREQ]/[TCOL2] debug + the active-remote cache above.

        // [TCOL2] Watch the entity's OBJECT-SIDE color copies over time. Server_SetCurrentColor writes
        // the Mass FRAGMENT; a Mass processor is supposed to sync fragment -> VRPlayerRepData@0x308
        // (the replicated copy) -> clients. localData CurrentTeamColor@0x2E8/TeamIdx@0x200;
        // VRPlayerRepData CurrentTeamColor@0x500/TeamIdx@0x418. If 0x500 never becomes the real color,
        // the fragment->replicated sync is dropping it (the bug); clients only ever see the default.
        // [2026-09-08] [TCOL2] DISABLED — all four reads use the 20996 layout: 0x500/0x418 are past the end
        // of UA2PlayerEntity (~0x338) and 0x2E8/0x200 hit lastGrabbedPlayer / CosmeticsFragment. It reported
        // garbage, which is what made the colour sync look "dropped". Colour is in
        // CosmeticsFragmentReplicationOnly@0x200(0xF0) on this build — map that struct before restoring.
        ++shown;
    }

    // Build the ping-stamp targets from UA2PlayerEntity DIRECTLY (class-agnostic). The entity is what
    // carries FrequentData (Ping@0xF0+8 / 0x308+8); it exists for VR players AND spectators, whereas the
    // VR-pawn loop above only sees VRPawns. Walking entities is why the SPECTATOR now gets ping-stamped
    // too — before, its FrequentData.Ping stayed 0 so it snapped both ways (others saw it snap, and its
    // own 0-ping entity collapsed its interp of everyone else). Ping comes from the entity's Pawn
    // (UA2PlayerEntity.Pawn@0xE8 -> APawn.PlayerState@0x2B8 -> GetPingInMilliseconds), floored + held.
    static SDK::UClass* entCls = nullptr;   // [PERF] cached class lookup
    if (!entCls) entCls = SDK::UObject::FindClassFast("A2PlayerEntity");
    if (entCls)
    {
        g_pingTargetCount = 0;
        int withPawn = 0;
        for (int32_t i = 0; i < num && g_pingTargetCount < 128; ++i)
        {
            auto* eo = SDK::UObject::GObjects->GetByIndex(i);
            if (!eo || eo->IsDefaultObject() || !eo->IsA(entCls)) continue;
            const uintptr_t e2 = reinterpret_cast<uintptr_t>(eo);
            void* pawn = *reinterpret_cast<void**>(e2 + 0xE8);          // UA2PlayerEntity.Pawn
            float psPing = -1.0f;
            if (pawn)
            {
                ++withPawn;
                auto* ps = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(pawn) + 0x2B8);   // APawn.PlayerState
                if (ps && ps->Class)
                {
                    auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
                    if (fn) { char pb[8] = {}; ps->ProcessEvent(fn, pb); psPing = *reinterpret_cast<float*>(pb); }
                }
            }
            float stamp = psPing;
            if (stamp > 0.0f) g_lastGoodPing[eo] = stamp;                // hold last-good so a transient 0 read never propagates
            else { auto it = g_lastGoodPing.find(eo); stamp = (it != g_lastGoodPing.end()) ? it->second : kPingFloorMs; }
            g_pingTargets[g_pingTargetCount].entity = eo;
            g_pingTargets[g_pingTargetCount].pingMs = stamp;
            ++g_pingTargetCount;
        }
        const long fixes = InterlockedExchange(&g_ingestPingFixes, 0);   // ingest 0->ping corrections since last census
        printf("[HalcyonA2][PINGSTAMP] entities=%d withPawn=%d ingestFixes/s=%ld (VR + spectators)\n",
               g_pingTargetCount, withPawn, fixes);
    }
}
static void SafeProbeFreqTimestamps() { __try { ProbeFreqTimestamps(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// BALL-SYNC DIAGNOSIS. Golf/heart balls "teleport between spawn and hit point" for remote clients =
// the server only has 2 states (spawn, post-hit rest) because the ball's flight isn't simulated
// server-side. Confirm by logging any DiscEntity whose SERVER position moved >10u since the last
// ~200ms sample: a hit ball that STREAMS (many [BALLPOS] lines tracing the arc) = server simulates it
// (then it's a SendLatestData rate/interp fix); a single JUMP = no server simulation (the root).
struct FBallPos { double x, y, z; };
static void ProbeBallPositions()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    static std::unordered_map<void*, FBallPos> lastPos;
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("DiscEntity");
    if (!cls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls) || !o->Class) continue;
        auto* fn = o->Class->GetFunction("Actor", "K2_GetActorLocation");
        if (!fn) continue;
        double loc[3] = {};
        o->ProcessEvent(fn, loc);
        auto it = lastPos.find(o);
        if (it != lastPos.end())
        {
            const double dx = loc[0]-it->second.x, dy = loc[1]-it->second.y, dz = loc[2]-it->second.z;
            const double d2 = dx*dx + dy*dy + dz*dz;
            if (d2 > 100.0)
                printf("[HalcyonA2][BALLPOS] %s -> (%.0f,%.0f,%.0f) moved=%.0f\n",
                       o->GetName().c_str(), loc[0], loc[1], loc[2], sqrt(d2));
        }
        lastPos[o] = FBallPos{ loc[0], loc[1], loc[2] };
    }
}
static void SafeProbeBallPositions() { __try { ProbeBallPositions(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SIM-WIRE probe (golf + heartball fix). reconcile sub_540D970 adds a player+ball to the ball sim
// only if the pawn's disc component (pawn+0x328) has a ball at +0x440 AND the flag at +0x2FA(762) is
// set. Jakeball admission wires this; golf/heart balls (server-spawned via BallSpawner, not admission)
// likely don't → reconcile skips them → frozen server-side. Dump the wire state per pawn so we confirm
// the gap and see which ball (class) is/should be wired. Read-only (no sim mutation).
// Is p a plausible live UObject (vtable ptr lands in the exe image)? SEH-guarded read.
static bool WirePtrLooksReal(void* p)
{
    if (!p) return false;
    static uintptr_t ib = 0, is = 0;
    if (!ib) { ib = GetBase(); auto* d = reinterpret_cast<IMAGE_DOS_HEADER*>(ib);
               is = reinterpret_cast<IMAGE_NT_HEADERS*>(ib + d->e_lfanew)->OptionalHeader.SizeOfImage; }
    __try { const uintptr_t vt = *reinterpret_cast<uintptr_t*>(p); return vt >= ib && vt < ib + is; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Read an actor's world position (K2_GetActorLocation) into out[3]; false if not resolvable.
static bool WireActorLoc(void* actor, double out[3])
{
    if (!WirePtrLooksReal(actor)) return false;
    auto* o = reinterpret_cast<SDK::UObject*>(actor);
    if (!o->Class) return false;
    auto* fn = o->Class->GetFunction("Actor", "K2_GetActorLocation");
    if (!fn) return false;
    o->ProcessEvent(fn, out);
    return true;
}
// One pawn's real ball refs (correct AVRPawn offsets): currentDiscEntity@0x1130, HeartBall@0x1DC0,
// Entity@0x928, BallSimManager@0x1B38, PersonalBallSpawner@0x1B40. SEH lives in the Safe wrapper.
static void ProbeOnePawnWire(SDK::UObject* o)
{
    const uintptr_t p = reinterpret_cast<uintptr_t>(o);
    const int pidx = *reinterpret_cast<unsigned char*>(p + 0x1C22);
    void* curDisc = *reinterpret_cast<void**>(p + 0x1210);  // [22284] currentDiscEntity (was stale 0x1130)
    void* heart   = *reinterpret_cast<void**>(p + 0x1E60);  // [22284] HeartBall (was stale 0x1DC0)
    void* simMgr  = *reinterpret_cast<void**>(p + 0x1B38);
    auto nm = [](void* x) -> std::string {
        return WirePtrLooksReal(x) ? reinterpret_cast<SDK::UObject*>(x)->GetName() : std::string(x ? "(bad)" : "(null)");
    };
    double dloc[3] = {}, hloc[3] = {};
    const bool haveD = WireActorLoc(curDisc, dloc);
    const bool haveH = WireActorLoc(heart, hloc);
    printf("[HalcyonA2][BALLWIRE] pidx=%d curDisc=%s%s heartBall=%s%s simMgr=%p\n",
           pidx, nm(curDisc).c_str(), haveD ? "" : " (noloc)",
           nm(heart).c_str(), haveH ? "" : " (noloc)", simMgr);
    if (haveD) printf("[HalcyonA2][BALLWIRE]   curDisc pos=(%.0f,%.0f,%.0f)\n", dloc[0], dloc[1], dloc[2]);
    if (haveH) printf("[HalcyonA2][BALLWIRE]   heart   pos=(%.0f,%.0f,%.0f)\n", hloc[0], hloc[1], hloc[2]);
}
static void SafeProbeOnePawnWire(SDK::UObject* o)
{ __try { ProbeOnePawnWire(o); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[HalcyonA2][BALLWIRE] (pawn read faulted)\n"); } }
static void ProbeOneBallOwner(SDK::UObject* o)
{
    const std::string nm = o->GetName();
    if (nm.find("Golf") == std::string::npos && nm.find("Heart") == std::string::npos) return;
    void* spawner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x408);
    printf("[HalcyonA2][BALLWIRE] ball=%s spawnerPawn=%p\n", nm.c_str(), spawner);
}
static void SafeProbeOneBallOwner(SDK::UObject* o)
{ __try { ProbeOneBallOwner(o); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void ProbeBallWire()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("VRPawn");
    if (!cls) cls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    const auto* dcls = SDK::UObject::FindClassFast("DiscEntity");
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int shown = 0, dshown = 0;
    for (int32_t i = 0; i < num && (shown < 8 || dshown < 8); ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        if (cls && shown < 8 && o->IsA(cls)) { SafeProbeOnePawnWire(o); ++shown; continue; }
        if (dcls && dshown < 8 && o->IsA(dcls)) { SafeProbeOneBallOwner(o); ++dshown; }
    }
}
static void SafeProbeBallWire() { __try { ProbeBallWire(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
// [2026-09-09 *** BALL "TOO EXCITED"] ABallSimManager::PropHitHandSpeedRestitutionCurve @0x0380
// (SDK 22284, A2_classes.hpp) is the UCurveFloat the native contact solver samples to turn HAND SPEED
// into restitution -- it is what makes a gentle tap yield a gentle ball and a hard swing yield a fast
// one. If it is NULL on this headless server the solver has no speed-dependent damping left and every
// contact resolves at whatever constant it falls back to, i.e. a tap launches the ball exactly as hard
// as a smash. That matches the reported "the ball flies with even a tap" precisely.
//
// This is DIAGNOSTIC-FIRST: report the live manager's curve and the class-default-object's curve. Only
// if the live one is null AND the CDO has a real one do we copy it across (g_fixRestCurve, on by
// default; -NoRestCurveFix disables). Copying a UObject* the CDO already holds adds no new object and
// no lifetime concern -- it is the value the instance was supposed to be constructed with.
static bool g_fixRestCurve = true;
static int  g_restCurveLogged = 0;
static void CheckBallSimRestitutionCurve()
{
    if (!g_ballSimMgr) return;
    void** live = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x380);
    void* cur = *live;

    void* cdoCurve = nullptr;
    SDK::UClass* bsm = SDK::UObject::FindClassFast("BallSimManager");
    if (bsm)
    {
        SDK::UObject* cdo = bsm->ClassDefaultObject;
        if (cdo) cdoCurve = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(cdo) + 0x380);
    }

    if (g_restCurveLogged < 6)
    {
        ++g_restCurveLogged;
        HxLog("[HalcyonA2][RESTCURVE] mgr=%p PropHitHandSpeedRestitutionCurve=%p (%s) cdoCurve=%p (%s)\n",
              g_ballSimMgr, cur,
              cur ? static_cast<SDK::UObject*>(cur)->GetName().c_str() : "NULL <-- no speed->restitution damping",
              cdoCurve,
              cdoCurve ? static_cast<SDK::UObject*>(cdoCurve)->GetName().c_str() : "NULL");
    }

    if (g_fixRestCurve && !cur && cdoCurve)
    {
        *live = cdoCurve;
        HxLog("[HalcyonA2][RESTCURVE] *** live manager had NO restitution curve; copied the CDO's %s in. "
              "Taps should now damp instead of launching.\n",
              static_cast<SDK::UObject*>(cdoCurve)->GetName().c_str());
    }
}
static void SafeCheckRestCurve() { __try { CheckBallSimRestitutionCurve(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-09 *** THE 1-POS-UPDATE/SEC LAG STATE, REAL LAYER] This build ships UA2ReplicationGraph
// (SDK 22284 A2_classes.hpp:4243), a UReplicationDriver. When a replication GRAPH drives replication,
// the per-actor knobs we pin in WireVRPawnBallSimManagers (bAlwaysRelevant, NetUpdateFrequency,
// MinNetUpdateFrequency, NetPriority, NetCullDistanceSquared) are largely IGNORED -- the graph's nodes
// decide relevancy and rate instead, and an actor is routed to a node BY CLASS when it registers:
//     SpatializedClasses    @0x0570 -> GridNode3D, update frequency falls off with DISTANCE
//     NonSpatializedClasses @0x0580
//     AlwaysRelevantClasses @0x0590 -> AlwaysRelevantNode, full rate to everyone
// If the VR pawn class is SPATIALIZED, a player whose grid cell is stale or far gets throttled to a
// trickle -- which is exactly "only sending 1 pos upd/s" -- and GRABBING them fixes it because the grab
// dirties the actor and forces an update. That also explains why setting bAlwaysRelevant on the actor
// changed nothing: the routing decision was already made, by class, at registration.
// DIAGNOSTIC FIRST: print the three class lists and say which bucket the VR pawn lands in. TArray is
// {Data@0x00, Num@0x08}.
static int g_repGraphLogged = 0;
static int s_noGraphTries = 0;
static const int kNoGraphGiveUp = 10;
static void DumpReplicationGraph()
{
    if (g_repGraphLogged >= 2 || s_noGraphTries >= kNoGraphGiveUp) return;
    SDK::UClass* rgCls = SDK::UObject::FindClassFast("A2ReplicationGraph");
    if (!rgCls) return;
    SDK::UObject* graph = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
    InterlockedIncrement(&g_walks); g_objN = num;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(rgCls)) { graph = o; break; }
    }
    if (!graph)
    {
        // MEASURED 2026-09-09: the class ships in this build but NO live instance is ever created on
        // this server -- replication uses the classic UNetDriver path, so the per-actor net flags we
        // pin (including NetDormancy) ARE the operative layer after all. Give up after a bounded number
        // of tries: this scan is a full ~166k-object walk and cost 12ms EVERY SECOND while it kept
        // looking for something that is never going to appear.
        if (++s_noGraphTries == kNoGraphGiveUp)
            HxLog("[HalcyonA2][REPGRAPH] no live A2ReplicationGraph after %d scans -> classic UNetDriver "
                  "replication (per-actor net flags are authoritative). Scan disabled.\n",
                  kNoGraphGiveUp);
        return;
    }
    ++g_repGraphLogged;
    const uintptr_t g = reinterpret_cast<uintptr_t>(graph);
    void* grid  = *reinterpret_cast<void**>(g + 0x5A0);
    void* arNode = *reinterpret_cast<void**>(g + 0x5B0);
    HxLog("[HalcyonA2][REPGRAPH] LIVE A2ReplicationGraph=%p GridNode3D=%p AlwaysRelevantNode=%p"
          " -- a graph IS driving replication, so per-actor net flags are advisory only\n",
          graph, grid, arNode);
    const char* kName[3] = { "Spatialized", "NonSpatialized", "AlwaysRelevant" };
    const uintptr_t kOff[3] = { 0x570, 0x580, 0x590 };
    for (int L = 0; L < 3; ++L)
    {
        void** data = *reinterpret_cast<void***>(g + kOff[L]);
        const int n = *reinterpret_cast<int*>(g + kOff[L] + 0x08);
        HxLog("[HalcyonA2][REPGRAPH] %sClasses n=%d\n", kName[L], n);
        for (int i = 0; data && i < n && i < 40; ++i)
        {
            auto* c = static_cast<SDK::UObject*>(data[i]);
            if (!c) continue;
            const std::string cn = c->GetName();
            const bool isPawn = cn.find("VRPawn") != std::string::npos;
            HxLog("[HalcyonA2][REPGRAPH]     %s%s\n", cn.c_str(),
                  isPawn ? "   <<<<< THE PLAYER PAWN CLASS IS IN THIS BUCKET" : "");
        }
    }
}
static void SafeDumpReplicationGraph() { __try { DumpReplicationGraph(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static void SafeWireVRPawns()       { __try { WireVRPawnBallSimManagers(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// SPECTATOR SMOOTHNESS (direction B: VR players see the spectator pawn snap). The spectator pose rides
// UA2SpectatorEntity.RepData (a bare FReplicatedTransformData — NO Timestamp/Ping/interp, unlike VR's
// FReplicatedFrequentData), so receivers can only snap to the latest replicated transform at whatever
// cadence it arrives. We can't add client-side interp (clients are Android, no injection), so the only
// server-side lever is REPLICATION DENSITY: pin the spectator pawn's net fields high + always-relevant
// so its transform reaches VR viewers as often as possible → raw motion looks smooth. (This is the
// OPPOSITE of VR pawns, where rate did nothing because interp was the lever; here there IS no interp.)
// ASpectatorCameraManagerPawn net fields (SDK struct): NetCullDistanceSquared@0x170, NetUpdateFrequency
// @0x178, MinNetUpdateFrequency@0x17C, NetPriority@0x180, bAlwaysRelevant@0x60 bit3.
static void TuneSpectatorPawns()
{
    static SDK::UClass* cls = nullptr;
    static const char* clsName = nullptr;
    if (!cls) { cls = SDK::UObject::FindClassFast("SpectatorCameraManagerPawn"); if (cls) clsName = "SpectatorCameraManagerPawn"; }
    if (!cls) { cls = SDK::UObject::FindClassFast("BP_SpectatorPawn_C");         if (cls) clsName = "BP_SpectatorPawn_C"; }
    if (!cls)
    {
        static bool warned = false;
        if (!warned) { warned = true; printf("[SPEC] NO spectator class resolved (SpectatorCameraManagerPawn / BP_SpectatorPawn_C both null) — tune is a NO-OP\n"); }
        return;
    }
    int tuned = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        *reinterpret_cast<uint8_t*>(p + 0x60) |= 0x08;    // bAlwaysRelevant — always relevant to every connection
        *reinterpret_cast<float*>(p + 0x170) = 1.0e12f;   // NetCullDistanceSquared — never distance-cull
        *reinterpret_cast<float*>(p + 0x178) = 100.0f;    // NetUpdateFrequency — target 100Hz
        *reinterpret_cast<float*>(p + 0x17C) = 100.0f;    // MinNetUpdateFrequency — pin (defeat adaptive down-throttle)
        *reinterpret_cast<float*>(p + 0x180) = 10.0f;     // NetPriority — win the actor budget
        ++tuned;
    }
    static int lastTuned = -1;
    if (tuned != lastTuned) { lastTuned = tuned; printf("[SPEC] tuned %d spectator pawn(s) (cls=%s)\n", tuned, clsName ? clsName : "?"); }
}
static void SafeTuneSpectatorPawns() { __try { TuneSpectatorPawns(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Watch each jakeball's physics-sync owningActor (physicsSync@ball+0x4F0, owningActor@+0xC8
// — the field Server_HitProp/SendPhysicsPropData gate on). Log only on change. When a player
// hits the ball, if ownership transfers we'll see `-> <VRPawn>`; if it never leaves null/server,
// the Server_HitProp ownership handshake is being rejected (the snap-back cause).
static std::unordered_map<void*, SDK::FVector> g_ballPos;   // last server-side world pos per jakeball
static int g_ownLogged = 0;
static void WatchBallOwnership()
{
    static SDK::UClass* actorCls = nullptr;   // [PERF] cached class lookup
    if (!actorCls) actorCls = SDK::UObject::FindClassFast("Actor");
    if (!actorCls || g_ownLogged >= 100000)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(actorCls))
            continue;
        std::string n = o->GetName();
        if (n.rfind("BP_JakeBall_C", 0) != 0)
            continue;
        auto* a = static_cast<SDK::AActor*>(o);
        if (!a->RootComponent)
            continue;
        SDK::FVector p = a->RootComponent->K2_GetComponentLocation();
        SDK::FVector& last = g_ballPos[o];
        const float dx = p.X - last.X, dy = p.Y - last.Y, dz = p.Z - last.Z;
        if (dx * dx + dy * dy + dz * dz > 10000.0f)   // server ball moved > 100 units (skip settle-bounce, catch hits)
        {
            void* physSync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x4F0);
            void* owner = physSync ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physSync) + 0xC8) : nullptr;
            printf("[HalcyonA2][OWN] %s SERVER-moved (%.0f,%.0f,%.0f) owner=%s\n",
                   n.c_str(), (double)p.X, (double)p.Y, (double)p.Z,
                   owner ? static_cast<SDK::UObject*>(owner)->GetName().c_str() : "null");
            ++g_ownLogged;
        }
        last = p;
    }
}
static void SafeWatchBallOwnership() { __try { WatchBallOwnership(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Dump arena-admission state: are there spawned ATicketManagers, and do our VRPawns have a
// GamemodeSlot / CurrentTicketManager / ticket? A pawn with ticketMgr=null / slot=null is
// never admitted -> lands in the -2 template sim -> no inputs, no team. Offsets (SDK map):
//   ATicketManager: SlotID@0x318 (Num@0x320), VerifiedTicketHolders num @0x390.
//   AVRPawn: CurrentTicketManager@0x1128, PlayerIndex@0x1C22, GamemodeSlot@0x1C40,
//            SlotID Num@0x1CB0, PawnCurrentTicket@0x1CB8.
static int      g_admDump = 0;
static ULONGLONG g_lastAdmDump = 0;
static void DumpAdmissionState()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    auto* tmCls   = SDK::UObject::FindClassFast("TicketManager");
    static SDK::UClass* pawnCls = nullptr;   // [PERF] cached class lookup
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("BP_VRPawn_C");
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int tmCount = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject())
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        if (tmCls && o->IsA(tmCls))
        {
            const int slotLen = *reinterpret_cast<int*>(p + 0x320);
            const int th      = *reinterpret_cast<int*>(p + 0x380);   // TicketHolders num
            const int vth     = *reinterpret_cast<int*>(p + 0x390);   // VerifiedTicketHolders num
            const float cur   = *reinterpret_cast<float*>(p + 0x36C); // CurrentTicket
            const float req   = *reinterpret_cast<float*>(p + 0x370); // RequiredTicket
            const int   tcNum = *reinterpret_cast<int*>(p + 0x2F0);    // TeamColors.Num (@0x2E8+8)
            auto* tcData = *reinterpret_cast<unsigned char**>(p + 0x2E8);  // TeamColors.Data
            int c0r = -1, c0g = -1, c0b = -1;   // first entry's Primary FColor (mem = B,G,R,A)
            if (tcNum > 0 && tcData) { c0b = tcData[0]; c0g = tcData[1]; c0r = tcData[2]; }
            if (th || vth || tcNum)
                printf("[HalcyonA2][ADM] TicketManager %s slotLen=%d holders=%d verified=%d cur=%.1f req=%.1f teamColors=%d primary0=(%d,%d,%d)\n",
                       o->GetName().c_str(), slotLen, th, vth, cur, req, tcNum, c0r, c0g, c0b);
            ++tmCount;
        }
        else if (pawnCls && o->IsA(pawnCls))
        {
            void* ctm  = *reinterpret_cast<void**>(p + 0x1128);
            void* slot = *reinterpret_cast<void**>(p + 0x1C40);
            const int sidLen = *reinterpret_cast<int*>(p + 0x1CB0);
            const int ticket = *reinterpret_cast<int*>(p + 0x1CB8);
            const int pidx   = *reinterpret_cast<unsigned char*>(p + 0x1C22);
            printf("[HalcyonA2][ADM] VRPawn %s ticketMgr=%p slot=%p slotIDlen=%d ticket=%d playerIdx=%d\n",
                   o->GetName().c_str(), ctm, slot, sidLen, ticket, pidx);
        }
    }
    printf("[HalcyonA2][ADM] ticketManagers=%d\n", tmCount);
}
static void SafeDumpAdmissionState() { __try { DumpAdmissionState(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Dump each ABallSimManager's SimulationsOutline (@0x350, num@0x358): one
// FBallSimulationOutline{SimIndex@0, TicketMan@0x8, PlayerIndexes@0x10 (TArray<uint8>),
// Balls@0x20} per TicketManager (stride 0x30). This is the seat map the rollback pump
// reads: if an admitted pawn's PlayerIndex shows under a real SimIndex here, admission
// -> sim seat works and the -2 problem is downstream; if it's absent, the seat/rebuild
// isn't picking up VerifiedTicketHolders on our manager.
static volatile LONG g_seatedPlayers = 0;   // [SEATFIX] total PlayerIndexes across every sim outline
static void DumpSimSeats()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    LONG seated = 0;
    static SDK::UClass* mgrCls = nullptr;   // [PERF] cached class lookup
    if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
    if (!mgrCls)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(mgrCls))
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const uintptr_t olData = *reinterpret_cast<uintptr_t*>(p + 0x350);
        const int       olNum  = *reinterpret_cast<int*>(p + 0x358);
        printf("[HalcyonA2][SEAT] mgr=%p outlines=%d\n", reinterpret_cast<void*>(p), olNum);
        if (!olData || olNum <= 0 || olNum > 64)
            continue;
        for (int k = 0; k < olNum; ++k)
        {
            const uintptr_t ol = olData + static_cast<uintptr_t>(k) * 0x30;
            const int    simIdx = *reinterpret_cast<int*>(ol + 0x0);
            void*        tm     = *reinterpret_cast<void**>(ol + 0x8);
            uint8_t*     piD    = *reinterpret_cast<uint8_t**>(ol + 0x10);
            const int    piN    = *reinterpret_cast<int*>(ol + 0x18);
            void*        ball0  = *reinterpret_cast<void**>(ol + 0x20) ?
                                  *reinterpret_cast<void**>(*reinterpret_cast<uintptr_t*>(ol + 0x20)) : nullptr;
            const int    ballN  = *reinterpret_cast<int*>(ol + 0x28);   // Balls TArray num
            char buf[160]; int bl = 0; buf[0] = 0;
            if (piD && piN > 0 && piN <= 32)
                for (int j = 0; j < piN; ++j)
                    bl += sprintf_s(buf + bl, sizeof(buf) - bl, "%d ", piD[j]);
            if (piN > 0 && piN <= 32) seated += piN;
            if (simIdx != -2 || piN > 0 || ballN > 0)
                printf("[HalcyonA2][SEAT]   sim=%d tm=%p players=[%s] n=%d balls=%d ball0=%p\n",
                       simIdx, tm, buf, piN, ballN, ball0);
        }
    }
    InterlockedExchange(&g_seatedPlayers, seated);
}
static void SafeDumpSimSeats() { __try { DumpSimSeats(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-03 ★ BALL-HIT REGRESSION FIX] g_ballSimMgr is assigned ONLY inside the now-disabled hand-spawn
// path (g_ballsimEnabled=false). Since the game spawns the BallSimManager NATIVELY now, g_ballSimMgr stayed
// NULL — so the results-send pin in the PE hook (`if (g_ballSimMgr) *(mgr+0x412)=1`) NEVER fired. That flag
// is what makes the native manager tick emit Client_SendServerSimResults; with it never set, the server sims
// a player's hit but never confirms the new ball state back to clients -> "balls can't be hit by players".
// Fix: latch g_ballSimMgr onto the live native manager (there is exactly one; BallDbg reports "managers=1").
// Throttled ~1s GObjects walk, re-finds periodically so a map change / respawn re-latches. Cheap: the
// s_last gate makes it a no-op on all but 1 of the thousands of PE calls per second.
// [BALLTUNE 2026-09-07] The ball is server-authoritative and SHARED (both clients agree at the same
// repTs), but each client's rendered actorPos trails the replicated repPos - the "laggy" feel. How
// fresh repPos is on clients depends on the ball ACTOR's net settings, so raise them server-side:
//   NetUpdateFrequency @0x178, MinNetUpdateFrequency @0x17C, NetPriority @0x180, NetDormancy @0x159,
//   bReplicateMovement bit @0x60 bit4.
// A dormant or low-frequency ball pushes its physics-sync rep data slowly -> the client interpolates
// over a big gap -> lag. This logs the current values once, then pins them high on every jakeball.
// -NoBallTune disables it.
static bool g_tuneBallNet = true;
static void TuneBallNet()
{
    if (!g_tuneBallNet) return;
    // Self-throttle: this walks GObjects, and the PE hook fires thousands of times per frame. Without
    // this it walked every call and stalled the game thread so hard that clients could not finish
    // joining. ~2 Hz is plenty to keep net settings pinned.
    static uint64_t s_gate = 0; const uint64_t nowg = GetTickCount64();
    if (nowg - s_gate < 500) return;
    s_gate = nowg;
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!cls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    static bool loggedOne = false;
    int tuned = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || o->Class != cls) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        float& nuf  = *reinterpret_cast<float*>(p + 0x178);
        float& mnuf = *reinterpret_cast<float*>(p + 0x17C);
        uint8_t& dorm = *reinterpret_cast<uint8_t*>(p + 0x159);
        uint8_t& repFlags = *reinterpret_cast<uint8_t*>(p + 0x60);
        if (!loggedOne)
        {
            loggedOne = true;
            HxLog("[HalcyonA2][BALLTUNE] before: %s NetUpdateFreq=%.0f MinNetUpdateFreq=%.0f NetDormancy=%d bReplicateMovement=%d\n",
                  o->GetName().c_str(), nuf, mnuf, (int)dorm, (int)((repFlags >> 4) & 1));
        }
        // 60 Hz push, always awake, movement replicated. NetDormancy 0 = DORM_Never (never sleep).
        if (nuf < 60.0f)  nuf  = 60.0f;
        if (mnuf < 30.0f) mnuf = 30.0f;
        if (dorm != 0) dorm = 0;   // DORM_Never - never sleep, always replicate fresh
        // NOTE: deliberately do NOT touch bReplicateMovement. It is 0 by design - the ball's position
        // rides the UA2PhysicsSync repData channel, not native movement replication. Adding native
        // movement replication would create a second, competing position channel (the old stutter).
        (void)repFlags;
        ++tuned;
    }
    static uint64_t s_last = 0; const uint64_t nowt = GetTickCount64();
    if (nowt - s_last > 5000) { s_last = nowt; HxLog("[HalcyonA2][BALLTUNE] pinned net settings on %d jakeball(s)\n", tuned); }
}
static void SafeTuneBallNet() { __try { TuneBallNet(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static void EnsureNativeBallSimMgr()
{
    static uint64_t s_last = 0;
    const uint64_t now = GetTickCount64();
    if (now - s_last < 1000) return;
    s_last = now;
    static SDK::UClass* mgrCls = nullptr;   // [PERF] cached class lookup
    if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
    if (!mgrCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(mgrCls)) continue;
        if (g_ballSimMgr != o)
        {
            g_ballSimMgr = o;
            printf("[HalcyonA2][BALLMGR] latched native BallSimManager -> %p (results-send pin now armed)\n", o);
        }
        return;   // exactly one manager — first non-CDO instance is it
    }
}
static void SafeEnsureNativeBallSimMgr() { __try { EnsureNativeBallSimMgr(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-04 ★ 20996 PARITY TEST] 20996 (WORKING) hand-spawned the manager and, before BeginPlay, set
// ABallSimManager::OfflineBallSimSubsystem @0x2F0 so the manager registered back into the subsystem. Native
// spawn (22284) is supposed to do this itself — this proves it. One-shot: find the OfflineBallSimSubsystem,
// log the native manager's CURRENT mgr+0x2F0 (was it already wired?), and set it if null. If `was=` already
// equals the subsystem, native == offline and hand-spawn wouldn't change anything (look elsewhere). If it
// was null and this unblocks input/hits, the back-wire was the missing 20996 piece.
static bool g_wireNativeSubsystem   = true;
static bool g_nativeSubsystemWired  = false;
static void WireNativeManagerSubsystem()
{
    if (!g_wireNativeSubsystem || g_nativeSubsystemWired || !g_ballSimMgr) return;
    static SDK::UClass* subCls = nullptr;   // [PERF] cached class lookup
    if (!subCls) subCls = SDK::UObject::FindClassFast("OfflineBallSimSubsystem");
    if (!subCls) return;
    void* sub = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(subCls)) { sub = o; break; }
    }
    if (!sub) return;
    const uintptr_t slot = reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x2F0;
    void* before = *reinterpret_cast<void**>(slot);
    if (before != sub) *reinterpret_cast<void**>(slot) = sub;
    g_nativeSubsystemWired = true;
    HxLog("[HalcyonA2][SUBWIRE] native mgr+0x2F0 was %p -> %p OfflineBallSimSubsystem (%s)\n",
          before, sub, (before == sub) ? "ALREADY WIRED (native==offline)" : "was null/other -> set (20996 back-wire)");
}
static void SafeWireNativeManagerSubsystem() { __try { WireNativeManagerSubsystem(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-04 ★ SEAT DIAG] The reconcile/seater sub_7FF6774BBD80 (RVA 0x545BD80) is the function that
// enumerates world players, checks the three disc gates (pawn+0x328 / comp+0x440 / comp+0x2FA), and calls
// the build worker (sub_7FF6774BCCB0 -> [DiagBUILD]) to SEAT a player in a sim. Since NO seating log ever
// fires, hook it and report: is it even CALLED on the headless server? If [RECON] never prints, the tick
// doesn't run the seater at all (client-perspective path skipped under forced dedicated netmode) = the root
// of "not being added to the sim" — then the fix is to drive this ourselves from a ticker. If it DOES print
// but [DiagBUILD] still doesn't, the disc gates / already-seated checks bail (the [DISC] +0x2FA line says
// which). Pure pass-through + throttled log; harmless.
static constexpr uintptr_t Reconcile_RVA = 0x545BD80;
using Reconcile_t = double(__fastcall*)(__int64);
static Reconcile_t Reconcile_Orig = nullptr;
// [2026-09-05 ★ GATE THE RECONCILE — physics not jumping] The reconcile RE-BASES each sim every pass; running
// it every tick (the game now does, once g_forceAuthSim made the sim authoritative) stomps the ball transforms
// so they JUMP/rubber-band instead of physically simulating (user: "they jump, look like the transform is being
// moved, not actually physically moving"; 20996: "reconcile continuous -> rubber-band; gated-on-join -> smooth
// + hits"). So SKIP the reconcile except (a) a startup window where the arena sims must first build, and (b) when
// armed by an admit / player-count change (g_reconcileWanted). Between rebuilds the sim runs physics untouched.
static volatile long g_reconcileWanted = 0;   // set >0 (a small burst) on admit / pawn-count change
// [2026-09-05] SEH-guard the game's reconcile/build call. When a 2nd player joins, our armed reconcile burst
// runs the game's seater (sub_7FF6774BBD80 -> build worker) DURING join streaming; it can read a half-built
// pawn/disc/ticket object and AV (crash seen: read 0x26a...f6328, HalcyonA2.dll trampoline frames on top). A
// game-function fault here is caught -> we skip that pass, the next one succeeds, server survives.
static double SafeReconcileOrig(__int64 mgr) { __try { return Reconcile_Orig(mgr); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0.0; } }
static double __fastcall Reconcile_Hook(__int64 mgr)
{
    static uint64_t s_first = 0, s_last = 0; static long s_ran = 0, s_skip = 0;
    const uint64_t now = GetTickCount64();
    if (!s_first) s_first = now;
    const bool bootWindow = (now - s_first) < 20000;   // first ~20s: let arena sims build
    const bool run = bootWindow || (g_reconcileWanted > 0);
    if (run && g_reconcileWanted > 0) InterlockedDecrement(&g_reconcileWanted);
    if (run) ++s_ran; else ++s_skip;
    if (now - s_last > 1000)
    {
        s_last = now;
        HxLog("[HalcyonA2][RECON] sub_545BD80 ran=%ld skipped=%ld wanted=%ld boot=%d\n",
              s_ran, s_skip, (long)g_reconcileWanted, bootWindow ? 1 : 0);
        s_ran = s_skip = 0;
    }
    if (run) return SafeReconcileOrig(mgr);
    return 0.0;   // gated out -> sim keeps its state, physics runs uninterrupted between rebuilds
}

// [2026-09-04 ★★ SEAT FIX] CONFIRMED: on the forced-dedicated headless server the game NEVER calls the
// reconcile/seater (sub_7FF6774BBD80) — [RECON] never fired — because it's client-perspective code gated off
// under netmode==2. So no world player is ever seated (outline@0x358 stays 0), no inputs ingest, no hits.
// The joined player PASSES all three disc seat-gates ([DISC] +0x2FA=106 => PASS), so they're eligible — the
// ONLY thing missing is that nobody runs the seater. Drive it ourselves ~4Hz on the game thread (the PE-hook
// ticker). It's the game's own fn: it enumerates world players, re-checks the disc gates + phantom filter,
// prunes leavers, and calls the build worker to seat eligible players -> sim entry -> the client is told it's
// in sim N -> it starts streaming inputs (ingest fires) and hits land. We call Reconcile_Orig (the trampoline
// original) so it does the real work without re-entering our log hook. Throttled outline readback proves it.
static bool     g_driveSeater     = true;
static uint64_t g_lastSeaterTick  = 0;
// [DISCCENSUS] Both the seater and Server_HitProp key on the player's ADiscEntity: the seater
// (sub_14545BD80) keeps only entity-array entries whose virtual [+1776] resolves to
// ADiscEntity::StaticClass and demands EXACTLY ONE survivor, and Server_HitProp_Implementation
// resolves the ball's UA2PhysicsSync through the same virtual index 222. If our headless mock client
// never gets a disc (it has no hands and no input), both would fail for the same benign reason and
// there would be no server bug to chase. Count them and say who owns each.
static void DiscCensus()
{
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("DiscEntity");
    if (!cls) { HxLog("[HalcyonA2][DISCCENSUS] class DiscEntity not found\n"); return; }
    // Every ball in the game derives from ADiscEntity (BP_JakeBall_C included), and the pooled
    // training balls alone are ~40 actors, so print ONLY the ones a player owns - those are the
    // "player's disc" the seater and the hit path actually care about.
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int total = 0, pooled = 0, pawnOwned = 0, unowned = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        auto* a = static_cast<SDK::AActor*>(o);
        ++total;
        SDK::AActor* own = a->Owner;
        if (!own) { ++unowned; continue; }
        const std::string on = own->GetName();
        if (on.find("PoolingManager") != std::string::npos) { ++pooled; continue; }
        if (on.find("VRPawn") != std::string::npos)
        {
            ++pawnOwned;
            const SDK::FVector p = a->K2_GetActorLocation();
            HxLog("[HalcyonA2][DISCCENSUS]   PLAYER DISC %s owner=%s pos=(%.0f, %.0f, %.0f) role=%d\n",
                  a->GetName().c_str(), on.c_str(), p.X, p.Y, p.Z, (int)a->GetLocalRole());
        }
    }
    HxLog("[HalcyonA2][DISCCENSUS] ADiscEntity total=%d pooled=%d unowned=%d PLAYER-OWNED=%d\n",
          total, pooled, unowned, pawnOwned);
}
static void SafeDiscCensus() { __try { DiscCensus(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static void DriveSeater()
{
    if (!g_driveSeater || !g_ballSimMgr) return;
    const uint64_t now = GetTickCount64();
    if (now - g_lastSeaterTick < 1000) return;
    g_lastSeaterTick = now;
    // [2026-09-05] Do NOT drive the reconcile every tick anymore (that re-bases the sim -> balls jump). The
    // native tick calls it (gated in Reconcile_Hook). Here we only ARM a short reconcile burst when the pawn
    // count changes (someone joined/left) so new seats get built — like 20996's join-gated reconcile.
    static int s_seatFails = 0;                 // [MIFIX] consecutive SEATFIX arms with nobody seated
    if (g_seatedPlayers > 0) s_seatFails = 0;    // seating works -> reset the backoff
    if (g_vrPawnCount != g_lastReconcileCount)
    {
        g_lastReconcileCount = g_vrPawnCount;
        g_reconcileWanted = 3;   // was 8 — a join/leave still (re)builds seats, with a smaller hitch
    }
    const int outline = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x358);
    HxLog("[HalcyonA2][SEATDRIVE] outline@0x358=%d pawns=%d wanted=%ld seated=%ld (reconcile gated -> physics runs)\n",
          outline, g_vrPawnCount, (long)g_reconcileWanted, (long)g_seatedPlayers);
    SafeDumpSimSeats();
    { static uint64_t s_lastDisc = 0;
      if (now - s_lastDisc > 5000) { s_lastDisc = now; SafeDiscCensus(); } }

    // [SEATFIX 2026-09-07] The pawn-count delta above is the ONLY thing that re-armed the seater, and it
    // does not fire for a joining network client: the server already owns a VR pawn of its own, so
    // g_vrPawnCount frequently does not change when someone joins, and any change that does happen inside
    // the 20s boot window is consumed while there is still nobody to seat. The measured result was that
    // after boot [RECON] read "ran=0 skipped=1 wanted=0" forever, every sim reported players=[] n=0, and
    // [SIMPART] reported participants=0 on all 7 sims even though the joined player's disc PASSES the seat
    // gate ([DISC] +0x2FA=102 => PASS). With no participants the sims integrate balls under gravity only and
    // a player can never touch a ball -- which is exactly what "ball sim is broken" looks like in game.
    // So drive it off the symptom instead of off a pawn-count guess: if somebody is eligible but nobody is
    // seated anywhere, let a reconcile burst through. Rate-limited so it cannot turn into the every-tick
    // reconcile that made balls jump.
    if (g_vrPawnCount > 0 && g_seatedPlayers == 0 && g_reconcileWanted == 0)
    {
        // [MIFIX 2026-09-07] BACK OFF the re-arm. Measured: re-arming wanted=8 every 3s while seated
        // stays 0 makes the EXPENSIVE reconcile (sub_545BD80) run in bursts, and each burst is a
        // >55ms game-thread hitch. StepSim drops every frame accumulated past 55ms (spiral-of-death
        // guard at dt>0.05556), and every dropped frame is a MissedInput -> this thrash is a direct
        // MI source that scales with pawn count (= "MI high with >1 player"). Since seating keeps
        // failing here anyway, hammering it only produces hitches. So: small burst (3, not 8), and
        // grow the retry interval the longer it fails (3s -> 6 -> 12 -> 24 -> cap 30s). If seating
        // ever succeeds (seated>0, checked at the top-level guard), s_fails resets and normal cadence
        // returns, so a real client that CAN seat is unaffected.
        static uint64_t s_lastArm = 0;
        const uint64_t interval = (uint64_t)3000 << (s_seatFails < 4 ? s_seatFails : 4);   // 3s,6,12,24,48->cap
        const uint64_t capped   = interval > 30000 ? 30000 : interval;
        if (now - s_lastArm > capped)
        {
            s_lastArm = now;
            if (s_seatFails < 8) ++s_seatFails;
            g_reconcileWanted = 3;   // was 8 — fewer expensive passes per burst = smaller game-thread hitch
            HxLog("[HalcyonA2][SEATFIX] %d eligible pawn(s) but 0 seated - arming a reconcile burst (fails=%d nextIn=%llums)\n",
                  g_vrPawnCount, s_seatFails, (unsigned long long)capped);
        }
    }
}
static void SafeDriveSeater() { __try { DriveSeater(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// AVRPawn::Server_SendPhysicsPropData_Implementation (sub_5496650) applies the client's
// streamed ball state ONLY IF physicsSync->owningActor(@0xC8) == the sending pawn. On our
// server ownership never transfers (stays null), so every hit-stream is dropped -> the
// authoritative ball never moves from a hit -> snap-back. Force it: stamp owningActor to
// the sender right before the apply so the server accepts the stream (last-writer-wins).
static constexpr uintptr_t SendPhysImpl_RVA = 0x5503050;
using SendPhys_t = void(__fastcall*)(void*, void*, void*, void*);
static SendPhys_t SendPhys_Orig = nullptr;
// Pending hit velocity to inject into the sim's ball state (set by SendPhys_Hook,
// consumed by SimIntegrate_Hook).
static SDK::FVector g_pendingVel{};
static SDK::FVector g_pendingPos{};              // client's streamed ball position (data+0x18)
// Latest client-streamed position per ball. For golf balls the actor RootComponent is FROZEN
// server-side (no sim moves it); the live position only exists in this Server_SendPhysicsPropData
// stream (data+0x18). GolfSinkDetect reads THIS for stroke/sink detection. Game-thread only (RPC
// handler + PE ticker are the same thread) so no lock needed.
struct BallStreamPos { SDK::FVector pos; unsigned long long seen; };
static std::unordered_map<void*, BallStreamPos> g_ballStreamPos;
static void*        g_pendingRoot   = nullptr;   // ball RootComponent
static void*        g_pendingPrim   = nullptr;   // physicsSync primitiveComp (@physicsSync+0xB0)
static bool         g_pendingActive = false;
static int          g_injLog        = 0;
static volatile long g_sendPhysCnt = 0;      // [HIT DIAG] # of Server_SendPhysicsPropData RPCs this second
static volatile long g_sendPhysHitCnt = 0;   // ...of which carried a real (moving) velocity
static void __fastcall SendPhys_Hook(void* pawn, void* ball, void* data, void* a4)
{
    // [2026-09-03 ★ HIT DIAG] Prove whether the client's hit-stream RPC even REACHES the server. If this
    // stays 0 while a player is clearly hitting a ball, the client isn't sending it — i.e. the client
    // doesn't locally own/drive the ball (no autonomous proxy), which is upstream of anything the sim does.
    InterlockedIncrement(&g_sendPhysCnt);
    __try {
        if (ball)
        {
            void* physSync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ball) + 0x4F0);
            if (physSync && !g_ballOwnArb)
            {
                // legacy force-accept (-NoBallOwnArb): last writer always wins
                *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physSync) + 0xC8) = pawn;
            }
            else if (physSync)
            {
                // [BALLOWN] arbitrate instead of force-accepting. POD-only: this whole body lives
                // inside the enclosing __try, so no C++ objects / no RAII (C2712).
                const unsigned long long nowO = GetTickCount64();
                int slot = -1;
                for (int b = 0; b < g_ballOwnN; ++b)
                    if (g_ballOwn[b].sync == physSync) { slot = b; break; }
                if (slot < 0)
                {
                    if (g_ballOwnN < 16) slot = g_ballOwnN++;
                    else
                    {
                        unsigned long long oldest = ~0ull;
                        slot = 0;
                        for (int b = 0; b < 16; ++b)
                            if (g_ballOwn[b].seen < oldest) { oldest = g_ballOwn[b].seen; slot = b; }
                    }
                    g_ballOwn[slot].sync  = physSync;
                    g_ballOwn[slot].owner = nullptr;
                    g_ballOwn[slot].seen  = 0;
                }
                void* cur = g_ballOwn[slot].owner;
                const bool stale = (g_ballOwn[slot].seen == 0) || ((nowO - g_ballOwn[slot].seen) > BALL_OWN_GRACE_MS);
                if (!cur || cur == pawn || stale)
                {
                    if (cur && cur != pawn) InterlockedIncrement(&g_ownSteal);
                    g_ballOwn[slot].owner = pawn;
                    g_ballOwn[slot].seen  = nowO;
                    *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physSync) + 0xC8) = pawn;
                    InterlockedIncrement(&g_ownGrant);
                }
                else
                {
                    // NOT the owner. Deliberately do not touch +0xC8 -- the native implementation
                    // compares owningActor against the sender and drops this stream on its own.
                    InterlockedIncrement(&g_ownDeny);
                }
            }

            // OPTION 1: relay the client's streamed velocity into the sim buffer so the step
            // carries the hit instead of stomping it. FReplicatedPhysicsObjectData.Velocity
            // is @0x48 (FVector_NetQuantize10 = 3 doubles). Server_SetVelocity writes it into
            // the sim's FBallSimBallState.QueuedSetVelocity — the store the sim step reads.
            // Stash the client's post-hit velocity + the ball's candidate sim components,
            // for the sub_5434CE0 hook to write into FBallSimBallState.QueuedSetVelocity.
            if (data)
            {
                // Record the live streamed position for EVERY ball (data+0x18) — this is the moving
                // golf-ball position the frozen RootComponent doesn't have.
                g_ballStreamPos[ball] = { *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x18), GetTickCount64() };
                // [HOLD] Track EVERY stream (not just fast ones): a held ball moves slowly with the
                // hand and would fail the sp2>100 hit filter, yet it is exactly the case that needs
                // continuous authority. Keyed by the sim-side components so SimIntegrate can match.
                {
                    void* hroot = static_cast<SDK::AActor*>(ball)->RootComponent;
                    void* hprim = physSync ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(physSync) + 0xB0) : nullptr;
                    const SDK::FVector hpos = *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x18);
                    const SDK::FVector hvel = *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x48);
                    int slot = -1;
                    for (int h = 0; h < g_heldN; ++h)
                        if (g_held[h].root == hroot && g_held[h].prim == hprim) { slot = h; break; }
                    if (slot < 0)
                    {
                        if (g_heldN < 8) slot = g_heldN++;
                        else { unsigned long long oldest = ~0ull; for (int h = 0; h < 8; ++h) if (g_held[h].seen < oldest) { oldest = g_held[h].seen; slot = h; } }
                    }
                    g_held[slot] = { ball, hroot, hprim, hpos, hvel, GetTickCount64() };
                }

                SDK::FVector vel = *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x48);
                const double sp2 = vel.X * vel.X + vel.Y * vel.Y + vel.Z * vel.Z;
                if (sp2 > 100.0)   // only inject a meaningful velocity (skip ~rest)
                {
                    InterlockedIncrement(&g_sendPhysHitCnt);   // [HIT DIAG] a real moving-ball stream
                    g_pendingVel  = vel;
                    g_pendingPos  = *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(data) + 0x18);
                    g_pendingRoot = static_cast<SDK::AActor*>(ball)->RootComponent;
                    void* pssync  = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ball) + 0x4F0);
                    g_pendingPrim = pssync ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pssync) + 0xB0) : nullptr;
                    g_pendingActive = true;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    SendPhys_Orig(pawn, ball, data, a4);
    // [HIT DIAG] ~1Hz census of the hit-stream RPC. all = every stream (incl. rest), hits = moving.
    {
        static uint64_t s_last = 0;
        const uint64_t now = GetTickCount64();
        if (now - s_last > 1000)
        {
            s_last = now;
            const long all  = InterlockedExchange(&g_sendPhysCnt, 0);
            const long hits = InterlockedExchange(&g_sendPhysHitCnt, 0);
            const long gr = InterlockedExchange(&g_ownGrant, 0);
            const long dn = InterlockedExchange(&g_ownDeny,  0);
            const long st = InterlockedExchange(&g_ownSteal, 0);
            HxLog("[HalcyonA2][SENDPHYS] streams/s=%ld (moving=%ld) lastPawn=%p lastBall=%p | [BALLOWN] arb=%d grant/s=%ld deny/s=%ld steal/s=%ld tracked=%d\n",
                  all, hits, pawn, ball, (int)g_ballOwnArb, gr, dn, st, g_ballOwnN);
        }
    }
}

// sub_5434CE0(manager, simIndex, FBallSimulation, dt, a5, a6) — the per-sim integrator.
// a3 = FBallSimulation: Balls@0x48 (TArray<UPrimitiveComponent*>), States@0x58
// (TArray<FBallSimState>, 0x28 stride; BallStates@0x18, FBallSimBallState 0xD0 stride,
// QueuedSetVelocity@0x88). On a pending hit, find the ball's index in Balls and write the
// client's velocity into the latest state's ball entry, so the integrator applies it.
static constexpr uintptr_t SimIntegrate_RVA = 0x547F7A0;
using SimIntegrate_t = __int64(__fastcall*)(void*, unsigned int, void*, double, char, char);
static SimIntegrate_t SimIntegrate_Orig = nullptr;
// [2026-09-05 ★ IDA-CONFIRMED] This injection is now OFF by default. The integrator decompile proves it wrote the
// POST-integrate results/history snapshot (fsim+0x58), NOT the live mover (ballEntity+0x4C8 -> +0xC0) that the next
// substep integrates — so it never drove physics, and stamping the streamed POSITION each frame WAS the "jump /
// looks-set-not-moving" artifact. Now that g_forceAuthSim makes the sim authoritative, the native contact solver
// (sub_7FF6774D5160) lands the hit itself IF the remote's disc is an ingested participant. Re-enable ONLY as the
// A4 mover-inject fallback, and if so target the MOVER, not this snapshot.
static bool g_injectHitVel = false;
static __int64 __fastcall SimIntegrate_Hook(void* mgr, unsigned int simIndex, void* fsim, double dt, char a5, char a6)
{
    const __int64 r = SimIntegrate_Orig(mgr, simIndex, fsim, dt, a5, a6);
    // [2026-09-05 ★ A1 DIAGNOSTIC — the decisive question] Is the remote player's disc a PARTICIPANT of a sim that
    // holds a BALL? If pidx 2 never appears here for a ball-holding sim, no injection can EVER land a hit — it is a
    // seat/build-worker sim-membership bug, not physics. Log per sim ~1s: ball count (fsim+0x50), participant ids
    // (fsim+0x38 base, stride 48, id@+0, count fsim+0x40). Cross-ref [IN] added>0 for the remote pidx.
    __try {
        // [2026-09-07] The throttle used to be a SINGLE timestamp shared by every sim, so with more than
        // one simulation alive only whichever sim happened to be integrated on the second boundary was ever
        // printed -- in practice always the -2 phantom, which is why the log read "participants=0" forever
        // and looked like nobody was ever a participant. Throttle PER SIM INDEX instead so every live
        // simulation reports, and we can actually see whether each connected player's sim has them in it.
        static uint64_t s_partLast[17] = {0};   // [0..15] = sim index, [16] = the -2 phantom / anything else
        const uint64_t now = GetTickCount64();
        const int slot = (simIndex < 16u) ? (int)simIndex : 16;
        if (fsim && now - s_partLast[slot] > 1000)
        {
            s_partLast[slot] = now;
            const uintptr_t s = reinterpret_cast<uintptr_t>(fsim);
            const int nBalls = *reinterpret_cast<int*>(s + 0x50);
            void* partD = *reinterpret_cast<void**>(s + 0x38);
            const int nPart = *reinterpret_cast<int*>(s + 0x40);
            char ids[160]; int il = 0; ids[0] = 0;
            if (partD && nPart > 0 && nPart <= 32)
                for (int i = 0; i < nPart && il < 140; ++i)
                    il += sprintf_s(ids + il, sizeof(ids) - il, "%d ",
                                    *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(partD) + (size_t)i * 48));
            HxLog("[HalcyonA2][SIMPART] sim=%u balls=%d participants=%d ids=[%s]\n", simIndex, nBalls, nPart, ids);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // [HOLD] Continuous client authority. Runs BEFORE the one-shot hit injection so a genuine hit
    // still overrides. Only balls streamed within HOLD_FRESH_MS are pinned; once the player lets go
    // the stream stops and the sim resumes owning the ball ~150ms later.
    if (g_holdAuthority) __try {
        if (fsim && g_heldN > 0)
        {
            const unsigned long long nowH = GetTickCount64();
            const uintptr_t s = reinterpret_cast<uintptr_t>(fsim);
            void** balls    = *reinterpret_cast<void***>(s + 0x48);
            const int nBalls = *reinterpret_cast<int*>(s + 0x50);
            void* statesD   = *reinterpret_cast<void**>(s + 0x58);
            const int nStates = *reinterpret_cast<int*>(s + 0x60);
            if (balls && nBalls > 0 && statesD && nStates > 0)
            {
                const uintptr_t st = reinterpret_cast<uintptr_t>(statesD) + (size_t)(nStates - 1) * 0x28;
                void* bsD  = *reinterpret_cast<void**>(st + 0x18);
                const int nBs = *reinterpret_cast<int*>(st + 0x20);
                if (bsD)
                {
                    bool anyFresh = false, anyHit = false;
                    for (int h = 0; h < g_heldN; ++h)
                        if (nowH - g_held[h].seen <= HOLD_FRESH_MS) { anyFresh = true; break; }
                    for (int i = 0; i < nBalls && i < nBs; ++i)
                    {
                        for (int h = 0; h < g_heldN; ++h)
                        {
                            if (nowH - g_held[h].seen > HOLD_FRESH_MS) continue;
                            bool match = (balls[i] == g_held[h].root || balls[i] == g_held[h].prim);
                            if (!match && balls[i] && g_held[h].actor)
                            {
                                // Walk the sim entry's Outer chain to its owning actor (components are
                                // Outer'd to their actor). Bounded to 6 hops like the GolfSink cup match.
                                SDK::UObject* ow = static_cast<SDK::UObject*>(balls[i]);
                                for (int g = 0; g < 6 && ow; ++g)
                                {
                                    if (ow == g_held[h].actor) { match = true; break; }
                                    ow = ow->Outer;
                                }
                            }
                            if (!match) continue;
                            const uintptr_t bs = reinterpret_cast<uintptr_t>(bsD) + (size_t)i * 0xD0;
                            *reinterpret_cast<double*>(bs + 0x00) = g_held[h].pos.X;
                            *reinterpret_cast<double*>(bs + 0x08) = g_held[h].pos.Y;
                            *reinterpret_cast<double*>(bs + 0x10) = g_held[h].pos.Z;
                            *reinterpret_cast<double*>(bs + 0x20) = g_held[h].vel.X;
                            *reinterpret_cast<double*>(bs + 0x28) = g_held[h].vel.Y;
                            *reinterpret_cast<double*>(bs + 0x30) = g_held[h].vel.Z;
                            InterlockedIncrement(&g_holdPins);
                            anyHit = true;
                            break;
                        }
                    }
                    if (anyFresh && !anyHit) InterlockedIncrement(&g_holdMiss);
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // [2026-09-09 *** "THE BALL IS TOO EXCITED" MEASUREMENT]
    // The restitution curve was measured present ([RESTCURVE] -> C_PropHitHandSpeedRestitution-
    // Curve, identical to the CDO), so a missing curve is NOT the cause. That curve is sampled on
    // HAND SPEED, which makes the hand velocity the server actually feeds it the next suspect:
    // if the server sees a much larger hand speed than the client did, a tap solves like a smash.
    //
    // Measure it directly instead of guessing. Layout (SDK 22284 A2_structs.hpp):
    //   FBallSimState      0x28: PlayerStates TArray@0x08, BallStates TArray@0x18
    //   FBallSimPlayerState 0x18: PlayerIndex@0x00, ContactStates TArray@0x08
    //   FBallSimContactState 0x90: position@0x08, Velocity@0x28, InstantVelocity@0x40,
    //                              ParentHMDVelocity@0x58, PawnVelocity@0x70, Colliding@0x88
    //   FBallSimBallState   0xD0: pos@0x00, Velocity@0x20
    // On any COLLIDING contact, print hand |Velocity| and |InstantVelocity| next to the ball's
    // |Velocity|. The ratio ball/hand is the effective restitution -- if a light tap shows a ratio
    // far above 1, the solver is amplifying, and by how much. POD-only inside the __try (C2712).
    __try {
        if (fsim)
        {
            const uintptr_t s = reinterpret_cast<uintptr_t>(fsim);
            void* statesD  = *reinterpret_cast<void**>(s + 0x58);
            const int nStates = *reinterpret_cast<int*>(s + 0x60);
            if (statesD && nStates > 0)
            {
                const uintptr_t st = reinterpret_cast<uintptr_t>(statesD) + (size_t)(nStates - 1) * 0x28;
                void* psD = *reinterpret_cast<void**>(st + 0x08);   // PlayerStates.Data
                const int nPs = *reinterpret_cast<int*>(st + 0x10); // PlayerStates.Num
                void* bsD = *reinterpret_cast<void**>(st + 0x18);   // BallStates.Data
                const int nBs = *reinterpret_cast<int*>(st + 0x20);

                // ball 0's speed this state (the one being hit in a 1-ball jakeball sim)
                double bspd = -1.0;
                if (bsD && nBs > 0)
                {
                    const uintptr_t b0 = reinterpret_cast<uintptr_t>(bsD);
                    const double bvx = *reinterpret_cast<double*>(b0 + 0x20);
                    const double bvy = *reinterpret_cast<double*>(b0 + 0x28);
                    const double bvz = *reinterpret_cast<double*>(b0 + 0x30);
                    bspd = sqrt(bvx*bvx + bvy*bvy + bvz*bvz);
                }

                for (int pi = 0; psD && pi < nPs && pi < 8; ++pi)
                {
                    const uintptr_t ps = reinterpret_cast<uintptr_t>(psD) + (size_t)pi * 0x18;
                    void* csD = *reinterpret_cast<void**>(ps + 0x08);
                    const int nCs = *reinterpret_cast<int*>(ps + 0x10);
                    for (int ci = 0; csD && ci < nCs && ci < 8; ++ci)
                    {
                        const uintptr_t cs = reinterpret_cast<uintptr_t>(csD) + (size_t)ci * 0x90;
                        if (!*reinterpret_cast<unsigned char*>(cs + 0x88)) continue;   // Colliding
                        const double vx = *reinterpret_cast<double*>(cs + 0x28);
                        const double vy = *reinterpret_cast<double*>(cs + 0x30);
                        const double vz = *reinterpret_cast<double*>(cs + 0x38);
                        const double ix = *reinterpret_cast<double*>(cs + 0x40);
                        const double iy = *reinterpret_cast<double*>(cs + 0x48);
                        const double iz = *reinterpret_cast<double*>(cs + 0x50);
                        const double hs = sqrt(vx*vx + vy*vy + vz*vz);
                        const double is = sqrt(ix*ix + iy*iy + iz*iz);
                        // throttle: at most ~5 lines/s, and only for contacts worth seeing
                        static ULONGLONG s_lastC = 0;
                        const ULONGLONG nowC = GetTickCount64();
                        if (nowC - s_lastC < 200) continue;
                        s_lastC = nowC;
                        HxLog("[HalcyonA2][CONTACT] sim=%u p=%d c=%d handV=%.0f instV=%.0f ballV=%.0f "
                              "ratio(ball/hand)=%.2f\n",
                              simIndex, pi, ci, hs, is, bspd,
                              (hs > 1.0) ? (bspd / hs) : -1.0);
                    }
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    if (g_injectHitVel) __try {
        if (g_pendingActive && fsim && (g_pendingRoot || g_pendingPrim))
        {
            const uintptr_t s = reinterpret_cast<uintptr_t>(fsim);
            void** balls   = *reinterpret_cast<void***>(s + 0x48);
            const int nBalls = *reinterpret_cast<int*>(s + 0x50);
            void* statesD  = *reinterpret_cast<void**>(s + 0x58);
            const int nStates = *reinterpret_cast<int*>(s + 0x60);
            if (balls && nBalls > 0 && statesD && nStates > 0)
            {
                for (int i = 0; i < nBalls; ++i)
                {
                    if (balls[i] != g_pendingRoot && balls[i] != g_pendingPrim)
                        continue;
                    // latest (just-appended) state — the one sent to clients
                    const uintptr_t st = reinterpret_cast<uintptr_t>(statesD) + (size_t)(nStates - 1) * 0x28;
                    void* bsD  = *reinterpret_cast<void**>(st + 0x18);
                    const int nBs = *reinterpret_cast<int*>(st + 0x20);
                    if (bsD && i < nBs)
                    {
                        const uintptr_t bs = reinterpret_cast<uintptr_t>(bsD) + (size_t)i * 0xD0;
                        *reinterpret_cast<double*>(bs + 0x00) = g_pendingPos.X;   // position
                        *reinterpret_cast<double*>(bs + 0x08) = g_pendingPos.Y;
                        *reinterpret_cast<double*>(bs + 0x10) = g_pendingPos.Z;
                        *reinterpret_cast<double*>(bs + 0x20) = g_pendingVel.X;   // Velocity
                        *reinterpret_cast<double*>(bs + 0x28) = g_pendingVel.Y;
                        *reinterpret_cast<double*>(bs + 0x30) = g_pendingVel.Z;
                    }
                    (void)simIndex; (void)nBalls; (void)nStates;   // [INJ] logging silenced
                    g_pendingActive = false;
                    break;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// sub_540B010 = the input-ingest called by Server_SubmitInputs. Returns 1 if the input
// (disc/hand poses) was ADDED to the sim's player buffer, 0 if SKIPPED (frame delta >100
// vs the sim frame, or player lookup miss). If our remote hits are SKIPPED, the disc never
// enters the sim -> no disc->ball collision -> frozen ball. Log ADDED/SKIPPED + frames.
static constexpr uintptr_t IngestInput_RVA = 0x54599B0;
using IngestInput_t = char(__fastcall*)(void*, int, int, void*, int, void*);
static IngestInput_t IngestInput_Orig = nullptr;
static int g_ingestLog = 0;
// Replicate sub_540B010's sim-entry hash lookup to reach a sim's frame counter (entry+0xC).
// mgr+0x300 = entry array (0xF8 stride, next-link @+0xF0); hash @mgr+0x340 (or mgr+0x338 if
// null), cap @mgr+0x348; empty when mgr+0x308==mgr+0x334.
static int* GetSimFramePtr(uintptr_t mgr, int simIdx)
{
    if (*reinterpret_cast<int*>(mgr + 0x308) == *reinterpret_cast<int*>(mgr + 0x334))
        return nullptr;
    uintptr_t hashBase = *reinterpret_cast<uintptr_t*>(mgr + 0x340);
    if (!hashBase) hashBase = mgr + 0x338;
    const int cap = *reinterpret_cast<int*>(mgr + 0x348);
    if (cap <= 0) return nullptr;
    int idx = *reinterpret_cast<int*>(hashBase + 4LL * (simIdx & (cap - 1)));
    if (idx == -1) return nullptr;
    const uintptr_t simArray = *reinterpret_cast<uintptr_t*>(mgr + 0x300);
    if (!simArray) return nullptr;
    for (int guard = 0; guard < 4096; ++guard)
    {
        const uintptr_t entry = simArray + 248LL * idx;
        if (*reinterpret_cast<int*>(entry) == simIdx)
            return reinterpret_cast<int*>(entry + 0xC);
        idx = *reinterpret_cast<int*>(entry + 0xF0);
        if (idx == -1) return nullptr;
    }
    return nullptr;
}

// a5 of sub_540B010 = "apply/overwrite even if this frame is already buffered" — i.e. this is an
// authoritative CORRECTION for a predicted frame. Headless it comes in a5=0, so late corrective
// inputs are dropped (skipRecent) instead of overwriting the prediction -> missedCaught stays 0 ->
// rubber-band. Force it to 1 so corrections land + trigger the rollback catch. Safe: for an
// already-confirmed frame it overwrites with the same value (no re-sim); only predicted frames
// (byte@+1==0) actually correct + set the caller's re-sim signal. Flag so we can A/B it.
// [TEMP-YANK] Make the abs>100 resync a TEMPORARY, call-scoped swap of the shared sim frame instead of a
// permanent write, so two players on different epochs stop ping-ponging it (the measured MI/MIB source).
// A/B with -NoTempYank to restore the old permanent yank.
static bool g_tempYank = true;
// [YANK-BUDGET] max resync yanks allowed per player per second. A real epoch re-sync needs only a couple;
// a stalled client dumping stale frames produced ~49/s and forced a huge rollback re-sim ("ball goes
// flying"). Tune with -YankBudget=N; 0 disables the limit (old flood-through behaviour).
static int  g_yankBudget  = 3;
static long g_yankDropped = 0;   // stale out-of-window inputs left for the native gate to reject
static bool g_forceInputOverwrite = true;

static char __fastcall IngestInput_Hook(void* mgr, int simIdx, int playerIdx, void* inputData, int a5, void* outByte)
{
    int inFrame = -1, simFrame = -1;
    bool resynced = false;
    int* fpSwap = nullptr; int fpSaved = 0;   // [TEMP-YANK] see below
    __try {
        if (inputData) inFrame = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(inputData) + 4);
        // [FRAME-REBASE] Map this player's raw command-frame onto the shared leader epoch BEFORE anything
        // downstream (newest tracking, the abs>100 resync, and the native ingest gate) sees it — so all
        // players occupy one epoch in the single shared sim frame and the resync yank never thrashes it.
        // The frame is rewritten IN the input packet (+4) so the native buffer stores the canonical frame;
        // SendResults_Hook undoes it per-player so each client still gets its own epoch back.
        if (g_frameRebase && inputData && inFrame >= 0 && playerIdx >= 0 && playerIdx < 256)
        {
            PlayerFrame& pf = g_playerFrames[playerIdx];
            // Anchor the canonical wall clock on the very first rebased input (first player -> offset ~0).
            if (!g_rebaseAnchorSet) { g_rebaseAnchorSet = true; g_rebaseWall0 = GetTickCount64(); g_rebaseFrame0 = inFrame; }
            // (re)derive offset ONCE on first sight, or on a genuine large backward jump (rejoin/fresh
            // session). Offset comes only from the wall clock + this player's raw frame — never another
            // player's rebased value — so there is no cross-player feedback.
            if (!pf.offsetSet || inFrame < pf.rawNewest - 500)
            {
                int off = RebaseCanonicalNow() - inFrame;
                if (off > RB_MAX_OFFSET || off < -RB_MAX_OFFSET)   // bogus (garbage frame / overflow) -> don't rebase
                {
                    HxLog("[HalcyonA2][REBASE] player %d BOGUS offset=%d (raw %d) -> clamped to 0\n", playerIdx, off, inFrame);
                    off = 0;
                }
                pf.offset    = off;
                pf.offsetSet = true;
                HxLog("[HalcyonA2][REBASE] player %d offset=%d (raw %d -> canon %d)\n",
                      playerIdx, pf.offset, inFrame, inFrame + pf.offset);
            }
            pf.rawNewest = inFrame;
            inFrame += pf.offset;   // canonical frame; rewrite the packet so native ingest stores it
            *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(inputData) + 4) = inFrame;
        }
        int* fp = GetSimFramePtr(reinterpret_cast<uintptr_t>(mgr), simIdx);
        if (fp)
        {
            simFrame = *fp;
            // Track the sim a player occupies + the newest confirmed input frame, for the
            // step pump's input-delay gate. A big backward jump = a fresh session -> reset.
            g_activeSimFramePtr = fp;
            g_activeSimIdx = simIdx;
            if (inFrame > g_newestInFrame || inFrame < g_newestInFrame - 200)
                g_newestInFrame = inFrame;
            // Track per-player newest frame (session-reset on a big backward jump) so the step pump
            // can hold behind the slowest player.
            if (playerIdx >= 0 && playerIdx < 256 && inFrame >= 0)
            {
                PlayerFrame& pf = g_playerFrames[playerIdx];
                if (inFrame > pf.newest || inFrame < pf.newest - 200) pf.newest = inFrame;
                pf.simIdx = simIdx;
                pf.seen   = GetTickCount64();
            }
            // The gate (sub_540B010) rejects inputs where abs(input.frame - sim.frame) > 100.
            // Our sim free-ran far ahead of the client's fresh frame counter, so every input
            // is dropped as stale. When out of window, yank the sim's frame to the client's so
            // the input lands at the live frame and the next step applies it. Fires only when
            // drifted; normal ingestion holds it in sync afterward (both advance ~90Hz).
            if (inFrame >= 0)
            {
                int d = inFrame - simFrame; if (d < 0) d = -d;
                if (d > 100)
                {
                    // [2026-09-08 TEMP-YANK] Was a PERMANENT write (`*fp = inFrame`). With two players on
                    // different command-frame epochs sharing ONE sim frame, each ingest yanked that shared
                    // frame to whoever just submitted, so it ping-ponged between the epochs every single
                    // input (measured [IN] resync=90-107/s). The MI loop compares each participant's
                    // buffered inputs against this same shared frame, so whichever player did NOT just yank
                    // had its entire buffer counted as behind => the MI/MIB roof.
                    // The native gate (sub_1454599B0) only READS this field (`v22 = *(v21+12) - a4.frame`,
                    // 0x145459a88) and never writes it, so we only need it correct FOR THE DURATION OF THE
                    // CALL. Swap it in just around IngestInput_Orig and restore immediately after: every
                    // player's input is still accepted (gate delta 0), but the shared frame is left
                    // untouched, so it no longer thrashes and the other player's inputs stay valid.
                    // Unlike the rebase this does NOT mutate the input packet (no feedback / rejoin brick),
                    // and unlike the hold it never stops the sim advancing (no freeze).
                    // [2026-09-08 YANK-BUDGET — "the spikes make the ball go flying"] A genuine epoch
                    // re-sync (fresh client vs a free-running sim) needs only a FEW yanks. But a stalled
                    // client that then dumps a batch of stale frames produces a FLOOD of out-of-window
                    // inputs (measured: resync=49 in ONE second, while it is 0 the rest of the time), and
                    // forcing all of those into the sim makes it re-simulate a ~0.5s stale batch — which is
                    // what launches the ball and pins MI at the 44 buffer cap. The native gate's own intent
                    // is to REJECT inputs this far out of window; only our resync overrides it. So keep a
                    // small per-player budget: enough to re-sync a real epoch change, not enough to force a
                    // stale flood through. Over budget => don't yank => the gate drops the stale input.
                    bool allowYank = true;
                    if (playerIdx >= 0 && playerIdx < 256)
                    {
                        PlayerFrame& pfy = g_playerFrames[playerIdx];
                        const ULONGLONG nowY = GetTickCount64();
                        if (nowY - pfy.yankWinStart > 1000) { pfy.yankWinStart = nowY; pfy.yankCount = 0; }
                        if (g_yankBudget > 0 && pfy.yankCount >= g_yankBudget) allowYank = false;
                        else ++pfy.yankCount;
                    }
                    if (allowYank)
                    {
                        if (g_tempYank) { fpSwap = fp; }
                        else            { *fp = inFrame; simFrame = inFrame; }
                        resynced = true;
                    }
                    else ++g_yankDropped;   // stale flood -> let the native gate reject it (no rollback burst)
                }
            }
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    const int applyFlag = g_forceInputOverwrite ? 1 : a5;   // force overwrite so late corrections catch
    // [TEMP-YANK] Present this player's frame to the gate for the duration of the native call only, then
    // put the shared sim frame back exactly as it was (see the note at the resync above).
    if (fpSwap) __try { fpSaved = *fpSwap; *fpSwap = inFrame; } __except (EXCEPTION_EXECUTE_HANDLER) { fpSwap = nullptr; }
    char r = IngestInput_Orig(mgr, simIdx, playerIdx, inputData, applyFlag, outByte);
    if (fpSwap) __try { *fpSwap = fpSaved; } __except (EXCEPTION_EXECUTE_HANDLER) {}
    // Per-second census answering "why are inputs missed — is the server SKIPPING ones that arrived?"
    //   added       = ingest accepted it (r != 0)
    //   skipRecent  = REJECTED an input for a RECENT frame (< 60 behind newest) — THE smoking gun:
    //                 if high, the game/gate is throwing away recent/rollback inputs -> misprediction
    //                 never corrected -> the jitter/snap. If ~0, missed = pure network loss (frames
    //                 that never arrived at all).
    //   skipOld     = REJECTED an ancient redundant re-send (>= 60 behind) — harmless.
    //   resync      = our abs>100 yank fired (a big frame jump = a snap source) — should be ~0 mid-play.
    static int cAdd = 0, cSkipRecent = 0, cSkipOld = 0, cResync = 0;
    static ULONGLONG lastInLog = 0;
    if (r) ++cAdd;
    else if (inFrame >= 0 && (g_newestInFrame - inFrame) < 60) ++cSkipRecent;
    else ++cSkipOld;
    if (resynced) ++cResync;
    const ULONGLONG now = GetTickCount64();
    if (now - lastInLog > 1000)
    {
        lastInLog = now;
        HxLog("[HalcyonA2][IN] sim=%d added=%d skipRecent=%d skipOld=%d resync=%d staleDropped=%ld simFrame=%d newest=%d\n",
              simIdx, cAdd, cSkipRecent, cSkipOld, cResync, g_yankDropped, simFrame, g_newestInFrame);
        cAdd = cSkipRecent = cSkipOld = cResync = 0; g_yankDropped = 0;
    }
    return r;
}

// Read the ACTUAL missedInputs/missedCaught the server computes and ships to the client each step
// (Client_SendServerSimResults = sub_5309C40; args: pawn, state, &players, &inputs, missedInputs,
// missedCaught). The client's HUD "MI/MIB" = these. Logging them tells us whether MI is server-side
// (a real miss count we can attack) or client-side prediction/clock.
// sub_5309C40 signature (confirmed from IDA): the last two args are CHAR (single bytes), not int —
// missedInputs/missedCaught are bytes (0-255), which is why the client HUD tops out ~44. Reading them
// as int grabbed 3 garbage stack bytes -> the 2-billion/negative junk we saw. Read as char, treat as
// unsigned count.
static constexpr uintptr_t SendResults_RVA = 0x534A7F0;   // [PORT-AUDIT] was 0x54E35B0 (= the pawn-side results RECEIVER); 0x534A7F0 is the sender called from StepSim (IDA-aligned 1.00 to 20996 0x5309C40)
using SendResults_t = void(__fastcall*)(void*, void*, void*, void*, char, char);
static SendResults_t SendResults_Orig = nullptr;
static ULONGLONG g_lastSendLog = 0;
static int g_sendMax = 0;   // peak missedInputs seen since last log
static void __fastcall SendResults_Hook(void* pawn, void* state, void* players, void* inputs,
                                        char missedInputsRaw, char missedCaughtRaw)
{
    const int missedInputs  = static_cast<unsigned char>(missedInputsRaw);
    const int missedCaught  = static_cast<unsigned char>(missedCaughtRaw);
    if (missedInputs > g_sendMax) g_sendMax = missedInputs;
    // [2026-09-08] Count sends/sec. IN AN ARENA the player pose rides THIS path (Client_SendServerSimResults),
    // not the ~30-46Hz Server_SetFrequentData stream — so this rate, not [POSERATE], is what actually drives a
    // remote player's smoothness while seated. ~90/s => in-arena replication is fine and the lag is client-side
    // interpolation; ~30/s => this is the in-arena bottleneck and it IS fixable server-side (arming cadence).
    static int s_sendCalls = 0;
    ++s_sendCalls;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastSendLog > 1000)
    {
        g_lastSendLog = now;
        HxLog("[HalcyonA2][SEND] sends/s=%d missedInputs=%d (peak=%d) missedCaught=%d simFrame=%d newest=%d streams/s=%ld hits/s=%ld pins/s=%ld miss/s=%ld | gtcTicked=%ld gtcSkipStopped=%ld\n",
              s_sendCalls, missedInputs, g_sendMax, missedCaught,
              g_activeSimFramePtr ? *g_activeSimFramePtr : -1, g_newestInFrame,
              g_sendPhysCnt, g_sendPhysHitCnt,             // READ ONLY: [SENDPHYS] owns/resets these.
                                                           // Exchanging here consumed them first and
                                                           // made [SEND] always print streams/s=0.
              InterlockedExchange(&g_holdPins, 0),         // [HOLD] per-frame authority pins applied
              InterlockedExchange(&g_holdMiss, 0),       // [HOLD] fresh stream, no sim-ball match
              // [SCRAPRUN] gate counters: skipStopped>0 proves stale/stopped GameTimeComponents
              // WERE being driven before the fix, i.e. the spurious-OnCountdownEnd mechanism is real.
              g_gtcTicked, g_gtcSkipStopped);
        g_sendMax = 0; s_sendCalls = 0;
    }
    // [FRAME-REBASE] Undo the ingest rebase on the way OUT: the input records shipped back to the client
    // (`inputs` = TArray of 328-byte records, frame @+4) were stored in the CANONICAL/leader epoch, but this
    // client predicts in ITS OWN epoch and reconciles by frame number — hand it back frames it recognises.
    // Target player = the entity `pawn`'s player-index byte @+7202 (same field the seater/StepSim use). The
    // `inputs` array here is a per-send scratch copy (built in StepSim), so rewriting it in place is safe;
    // we restore afterward anyway as belt-and-braces. Everything SEH-guarded (a stray ptr just sends as-is).
    int rbOff = 0; int rbN = 0; uintptr_t rbData = 0;
    if (g_frameRebase)
    {
        __try {
            const int pidx = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(pawn) + 7202);
            if (pidx >= 0 && pidx < 256 && g_playerFrames[pidx].offsetSet)
                rbOff = g_playerFrames[pidx].offset;
            if (rbOff != 0 && inputs)
            {
                rbData = *reinterpret_cast<uintptr_t*>(inputs);
                rbN    = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(inputs) + 8);
                if (rbData && rbN > 0 && rbN < 4096)
                    for (int i = 0; i < rbN; ++i)
                        *reinterpret_cast<int*>(rbData + (size_t)i * 328 + 4) -= rbOff;
                else { rbData = 0; rbN = 0; }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) { rbData = 0; rbN = 0; }
    }
    SendResults_Orig(pawn, state, players, inputs, missedInputsRaw, missedCaughtRaw);
    if (rbData && rbN > 0)   // restore the scratch copy's frames (Orig already copied+sent the translated ones)
        __try { for (int i = 0; i < rbN; ++i) *reinterpret_cast<int*>(rbData + (size_t)i * 328 + 4) += rbOff; }
        __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// DIAGNOSTIC: who advances the sim frame? Our pump logs stepped=0, yet simFrame climbs ~90/s and
// the server predicts ~30 frames (missedInputs). Hook the step (sub_543F2D0) and measure how many
// times/sec it's called + how much the active sim's frame advances PER call. If frameAdv/call≈1 the
// STEP drives the frame (so enforcing the input-delay hold by throttling the step would shrink the
// prediction window/MI); if frameAdv≈0 the frame is advanced by INGEST instead (throttling the step
// would just stutter physics, not lower MI).
// SNAP DIAGNOSIS rate sampler — the 1Hz probe can't see the true pose update rate. StepSim runs
// ~800/s, so sample the cached active-remote entity here and count how often localData (@0xF0,
// incoming RPC) and VRPlayerRepData (@0x308, replicated copy) actually change per second. localHz =
// how fast the client's pose reaches the server; repHz = how fast the server updates the copy it
// replicates. If either is ~10-20 that sparse rate IS the snap; if both ~72-90 the send-to-client
// cadence / client interp is the culprit instead. Root.position.X (@+0x10) is the change witness.
static double g_lastLocalX = 1e300, g_lastRepX = 1e300;
static int g_localChanges = 0, g_repChanges = 0;
static ULONGLONG g_lastFreqRateLog = 0;
static void SampleFreqRate()
{
    // [2026-09-08 ★ CRITICAL LAYOUT FIX — these writes were CORRUPTING MEMORY on 22284]
    // The port carried the 20996 UA2PlayerEntity layout (two FReplicatedVRPlayerData copies 0x218 apart:
    // localData@0xF0 + VRPlayerRepData@0x308). On 22284 the SDK (gamesdk/22284/SDK/A2_classes.hpp) shows a
    // COMPLETELY different, SMALLER object — there is only ONE frequent-data struct and it is already the
    // replicated one, and the class ENDS at ~0x338:
    //     0x00F0 (0x0110) FReplicatedFrequentData    FrequentDataReplicationOnly;  // Net  <- the pose
    //     0x0200 (0x00F0) FA2PlayerCosmeticsFragment CosmeticsFragmentReplicationOnly; // Net, RepNotify
    //     0x02F0          AActor*                    lastGrabbedPlayer;
    //     0x02F8 (0x0010) TArray<UA2PlayerEntity*>   playersGrabbingPlayer;
    //     0x0308 (0x0010) TArray<UA2PlayerEntity*>   discGrabChainCheckedPlayers;   // <- NOT a pose copy
    //     0x0330 (0x0008) Pad  -> object ends ~0x338
    // So the old writes landed as: (e+0x308+8) = discGrabChainCheckedPlayers.ArrayNum <- a FLOAT ping bit
    // pattern (20.9f => ArrayNum = 1,101,086,003!), (e+0x2E8) = over lastGrabbedPlayer (a live AActor*),
    // and (e+0x418)/(e+0x500) = 0xE0/0x1C8 bytes PAST THE END of the object (heap corruption). This ran at
    // StepSim rate (~85Hz) PER PLAYER with both stamps ON by default — a TArray advertising 1.1 billion
    // elements is almost certainly the long-hunted GC flood / hang / crash source in this port.
    // FrequentData layout is confirmed unchanged: Timestamp@0x0, Ping@0x8, Root(pos)@0x10.
    if (g_pingStampEnabled)
    {
        for (int i = 0; i < g_pingTargetCount; ++i)
        {
            const uintptr_t e = reinterpret_cast<uintptr_t>(g_pingTargets[i].entity);
            // ONLY the real replicated struct. (The old second write corrupted a TArray count.)
            *reinterpret_cast<float*>(e + 0xF0 + 0x8) = g_pingTargets[i].pingMs;   // FrequentDataReplicationOnly.Ping
        }
    }

    // TEAM-COLOR stamp — DISABLED on 22284: every one of its four writes used the 20996 layout and either
    // overwrote `lastGrabbedPlayer` or wrote past the end of the object. The colour lives in
    // CosmeticsFragmentReplicationOnly@0x200 on this build; re-enable only after mapping that struct.
    if (g_colorStampEnabled && false)
    {
        for (int i = 0; i < g_colorTargetCount; ++i) { (void)g_colorTargets[i]; }
    }

    if (!g_probeEntity) return;
    const uintptr_t e = reinterpret_cast<uintptr_t>(g_probeEntity);
    // Only one copy exists on 22284, so localHz IS the replicated pose rate; repHz is retired (it used to
    // read the discGrabChainCheckedPlayers TArray and always reported 0, which looked like a broken sync).
    const double lx = *reinterpret_cast<double*>(e + 0xF0 + 0x10);
    if (lx != g_lastLocalX) { g_lastLocalX = lx; ++g_localChanges; }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastFreqRateLog > 1000)
    {
        g_lastFreqRateLog = now;
        if (g_freqDebug)
            printf("[HalcyonA2][FREQRATE] pidx=%d localHz=%d repHz=%d\n", g_probePidx, g_localChanges, g_repChanges);
        g_localChanges = 0; g_repChanges = 0;
    }
}
static void SafeSampleFreqRate() { __try { SampleFreqRate(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static constexpr uintptr_t StepSim_RVA = 0x54863C0;
using StepSim_t = void(__fastcall*)(void*, float);
static StepSim_t StepSim_Orig = nullptr;
static ULONGLONG g_lastStepLog = 0;
static int g_stepCalls = 0, g_stepFrameAdv = 0;
// [MIDIAG] Track the sim's frame-drop trigger. StepSim (sub_1454863C0) drops (advances without
// simulating) any accumulated dt beyond ~0.05556s (5 frames) — and every dropped frame is a
// MissedInput. So dt>0.0556 per StepSim call is a literal MI-generating hitch. Count them + the max.
static int    g_stepHitches = 0;      // # of StepSim calls this second with dt > 55ms (the drop threshold)
static double g_stepMaxDt    = 0.0;   // worst dt this second

// [2026-09-01] THE -2 GC-FLOOD ROOT CAUSE (found by reading stepSim 0x54863C0 in IDA).
// stepSim (the manager's native ActorTick) contains a CLIENT-PERSPECTIVE REBUILD loop gated on
//   `BallNetPersp(world) == 3 (NM_Client) && AVRPawn::GetLocalInstanceWithWorld(world) != 0`.
// It matches the LOCAL player's index byte (localInstance+0x1C22) and REBUILDS that player's prediction
// sim (index -2) via sub_7FF6774BCCB0 EVERY tick. On our headless server (a client process acting as a
// dedicated server, with a phantom local player) this runs and rebuilds -2 hundreds of times/sec ->
// each rebuild spawns+frees sim/renderer objects the parallel GC cluster pass chokes on = the millions-
// of-null-refs flood that pauses the process so nobody can join. It ALSO resets the sim's frame every
// tick so it never accumulates enough dt to advance -> frameAdvViaStep stays 0 (the sim never STEPS,
// only REBUILDS). BallNetPersp = sub_7FF6760A0990(world) returns ENetMode (2=NM_ListenServer/authority,
// 3=NM_Client); the sim BUILD (0x545CCB0) reads it as `==2 -> IsServer`. forceAuth's scanner only
// patches the OTHER GetNetMode getters' `cmp eax,2`, never this one and never the `==3` gate, so it
// never stopped the client rebuild. FIX: during the ball tick only, remap this getter's client(3) ->
// ListenServer(2). The `==3` client-rebuild gates then fail (Block 1 init + Block 2 -2 rebuild skipped),
// the `==2` authority checks pass, and the UNCONDITIONAL fixed-step advance loop (later in stepSim)
// still steps the real arena sims -> frameAdvViaStep goes >0 and the -2 flood stops. Scoped to stepSim
// via g_inBallStep (thread-local) so nothing outside the tick changes -- removing the local player
// globally crashes LoadMap (see Main), which is why this is scoped instead.
static thread_local bool g_inBallStep = false;
static bool g_killLocalSimRebuild = false;  // [2026-09-01] OFF — BallNetPersp_RVA 0x60A0990 is a BAD RVA:
                                            // it's MID(+0x40) into unrelated sub_7FF678100950, so MH_CreateHook
                                            // wrote a trampoline into the middle of that function = live memory
                                            // corruption (a prime GC-flood suspect). The intended target
                                            // sub_7FF6760A0990 is actually RVA 0x4040990 = the WorldGetNetMode
                                            // hook (already installed), so this hook was redundant AND corrupting.

// [2026-09-02 ★ EXPERIMENT — NO-TICK] user test: let the manager spawn natively but NEVER step. Our manual
// pumps are already OFF (g_ballPumpsEnabled=false); this no-ops the manager's native tick BODY (sub_7FF6774E63C0)
// AND we call SetActorTickEnabled(false) on the actor (see DisableNativeBallSimTick). If the flood is gone ->
// the sim's stepping/building on half-built objects was the trigger. WARNING (dllmain:1452): a prior test
// disabling the tick made the flood WORSE (the tick's reconcile PRUNES stale -2 sims; no prune -> they pile
// up -> GC walks more stale refs). With NO stepping at all, ideally no sims are built either -> nothing to walk.
static bool g_ballSimNoTick = false;   // [2026-09-03] TICK ON — real fix in. Watchdog proved every prior tick-on freeze was OUR advance-skip: skipping sub_7FF6774C4630 for simId<0 starved the Phase-2 fixed-step accumulator (`for(j=v48[3]; j>=1/90; j=v48[3]) advance()`), so it never decremented -> infinite loop (`stuck in ADVANCE simId=-2 opSeq=1.4e9`). BOTH skips now OFF (g_skipPhantomSimStep / g_skipPhantomReconcile = false): let -2 advance+reconcile natively so the loop terminates, and let the GC Num-clamp (14 sites) contain any garbage the headless -2 state feeds the GC. Watchdog stays armed to catch any NEW hang. If the GC floods instead of hanging, that's a different problem (clamp gap), not the loop. Prior dead-end: tried tick ON + advance-skip + reconcile-skip (sub_7FF6774EFC40) + advance-skip + NEW reconcile-skip (sub_7FF6774EFC40) for the phantom -2 -> STILL froze at ball streaming (balls 1->27->51, hung at BP_VolleyJakeball), [STEP] calls/s=1 frameAdvViaStep=0 — same failure as the 2026-09-02 tick-on attempt. Root-cause REFUTED: once the host player (Index 1) logs in, the server -2 sim = `participants=1 [1->OK(BP_VRPawn_C ctrl=1 loc=1)]`, IDENTICAL to offline (the earlier `participants=0 []` was only the transient pre-login streaming rebuilds). So "empty -2" is NOT the fryer; the divergence is the headless local pawn's null/stale VR state, OR the Phase-3 results-build (mgr+1042) / authoritative ball sims during streaming — NOT covered by the -2 advance/reconcile skips. Reconcile+advance hooks stay installed (inert while tick off). See a2-ballsim-manager.
// [2026-09-03 ★ TICK-HANG WATCHDOG] The manager tick (sub_7FF6774E63C0) freezes the GAME THREAD during ball
// streaming when re-enabled. Because it hangs, no game-thread logger ever prints WHERE. This watchdog runs on
// its OWN thread: each ball-sim sub-op (reconcile / advance / buildsim) stamps g_lastOp{Phase,Sim,Seq}; StepSim
// stamps enter/exit ticks. If the game thread is inside StepSim for >3s (enter newer than exit, stale >3s) the
// watchdog dumps the exact phase + simId it died in — that pinpoints the freeze in ONE run. Purely diagnostic;
// g_tickWatchdog gates it. Phases: 1=reconcile(sub_7FF6774EFC40) 2=advance(sub_7FF6774C4630) 3=buildsim(sub_7FF6774BCCB0).
// (watchdog shared state g_stepEnterTick/g_stepExitTick/g_stepSeq/g_lastOp* is declared up by g_logBuildSim.)
static bool               g_tickWatchdog  = true;
static void TickWatchdog()
{
    long reported = -1;
    for (;;)
    {
        Sleep(1000);
        if (g_ballSimNoTick) continue;                      // only meaningful with the tick on
        const long long en = g_stepEnterTick, ex = g_stepExitTick;
        if (en > ex && (long long)GetTickCount64() - en > 3000 && g_stepSeq != reported)
        {
            reported = g_stepSeq;
            const char* ph = g_lastOpPhase == 1 ? "RECONCILE(sub_..EFC40)"
                           : g_lastOpPhase == 2 ? "ADVANCE(sub_..C4630)"
                           : g_lastOpPhase == 3 ? "BUILDSIM(sub_..BCCB0)"
                           : "PHASE3-TAIL(or between ops)";
            HxLog("[HalcyonA2][TICKHANG] game thread stuck in StepSim seq=%ld for %llums | lastOp phase=%d %s simId=%d opSeq=%ld\n",
                  g_stepSeq, (long long)GetTickCount64() - en, g_lastOpPhase, ph, g_lastOpSim, g_lastOpSeq);
        }
    }
}
// [MIFIX 2026-09-08 — DEFAULT ON] Root cause of the "MI/MIB out the roof with 2 seated players",
// found with the first-ever 2-REAL-client seat (both BP_VRPawn ctrl=1 loc=0, participants=2):
// the results-send was fully OFF (`[PUMP] sent=0`), so clients got NO authoritative frame and each
// client's rollback clock free-ran on its own join-epoch. Live [MI] showed the two clients ~5532
// frames apart (min=16381 max=21915) => target = min(newest)-DELAY = 16379 while the sim sat at
// 21914, so the pump's catch-up loop (while simFrame<target) NEVER ran => sent=0 => no resync =>
// the gap persisted and the native sim reported it as MissedInputs. hitches>55ms/s=0 the whole
// time, so the old "MI = game-thread hitch frame-drop" theory does NOT apply here.
// Arming the send on each CONTESTED advance (below) feeds clients an authoritative frame ~90/s
// independent of the broken min-target, so their clocks converge and the spread collapses. Scoped
// to contested advancing sims + input-within-500ms, so idle balls are never pinned (no +894/-894
// idle stutter). -NoArmResultsOnStep restores the old opt-in-off behaviour for A/B.
static bool g_armResultsOnStep = true;   // [MIFIX] default ON (was opt-in -ArmResultsOnStep)
static void __fastcall StepSim_Hook(void* mgr, float dt)
{
    if (g_ballSimNoTick) return;   // fallback: fully disable the tick (proven flood-free)
    SafeSampleFreqRate();
    const int before = g_activeSimFramePtr ? *g_activeSimFramePtr : -1;
    // [HOLD-AT-TARGET] Enforce the input-delay buffer the pump intends. The native actor tick advances the
    // sim by wall-clock and outran the pump's hold, so the sim sat ~2-4 frames AHEAD of the slowest player's
    // confirmed input: it simulated each frame before that player's input for it arrived -> predicted the hit
    // (ball reacts BEFORE the player visibly hits it) then corrected when the real input landed (ball lags) —
    // the residual MI (~30 under active play). Feeding dt=0 holds the frame WITHOUT skipping the call, so the
    // fixed-step accumulator is neither starved nor grown (the old skip-the-advance approach caused the
    // documented infinite-loop freezes). The pump still advances the sim up to the target with a real dt.
    const long holdTarget = g_confirmedTarget;
    const bool targetFresh = (GetTickCount64() - (unsigned long long)g_confirmedTargetAt) < 250;  // stale -> never hold (no frozen ball)
    if (g_holdSimAtTarget && targetFresh && holdTarget > 0 && before >= 0 && before >= (int)holdTarget)
        dt = 0.0f;   // at/past the confirmed frame -> hold this frame, let the late input arrive first
    const bool prevInStep = g_inBallStep;
    g_inBallStep = true;
    g_stepEnterTick = (long long)GetTickCount64(); ++g_stepSeq;   // watchdog: mark tick entry
    StepSim_Orig(mgr, dt);
    g_stepExitTick = (long long)GetTickCount64();                 // watchdog: mark clean exit
    g_inBallStep = prevInStep;
    const int after = g_activeSimFramePtr ? *g_activeSimFramePtr : -1;
    ++g_stepCalls;
    if (before >= 0 && after >= before) g_stepFrameAdv += (after - before);
    if (dt > 0.0556) ++g_stepHitches;              // [MIDIAG] this call dropped frames -> generated MI
    if (dt > g_stepMaxDt) g_stepMaxDt = dt;

    // [MIFIX] Arm the results-send for a contested sim that actually advanced a frame this step, so
    // clients get a confirmation per real step (bounds MI) — never for an idle ball (which doesn't
    // advance without inputs, so the +894/-894 idle-ball stutter source is never touched). Opt-in.
    if (g_armResultsOnStep && mgr && after > before && g_activeSimIdx >= 0)
    {
        const ULONGLONG nowStep = GetTickCount64();
        bool contested = false;
        for (int p = 0; p < 256; ++p)   // any participant that submitted input to this sim recently?
        {
            const PlayerFrame& pf = g_playerFrames[p];
            if (pf.newest >= 0 && pf.simIdx == g_activeSimIdx && nowStep - pf.seen <= 500) { contested = true; break; }
        }
        if (contested)
            __try { *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(mgr) + 0x412) = 1; }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
    }
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastStepLog > 1000)
    {
        g_lastStepLog = now;
        HxLog("[HalcyonA2][STEP] calls/s=%d frameAdvViaStep/s=%d hitches>55ms/s=%d maxDt=%.1fms lastDt=%.4f\n",
              g_stepCalls, g_stepFrameAdv, g_stepHitches, g_stepMaxDt * 1000.0, dt);
        g_stepHitches = 0; g_stepMaxDt = 0.0;
        g_stepCalls = 0; g_stepFrameAdv = 0;
    }
}

// [EXPERIMENT — NO-TICK] find the game's natively-spawned BallSimManager and disable its actor tick, so the
// manager exists (in GObjects, wired) but its per-frame reconcile/step/build never runs. Latched, one-shot.
static bool g_disabledNativeTick = false;
static void DisableNativeBallSimTick()
{
    if (g_disabledNativeTick || !g_ballSimNoTick) return;
    static SDK::UClass* mgrCls = nullptr;   // [PERF] cached class lookup
    if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
    if (!mgrCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(mgrCls)) continue;
        reinterpret_cast<SDK::AActor*>(o)->SetActorTickEnabled(false);
        g_disabledNativeTick = true;
        printf("[HalcyonA2][NOTICK-EXP] SetActorTickEnabled(false) on native %s (manager spawned, tick OFF)\n",
               o->GetName().c_str());
        break;
    }
}
static void SafeDisableNativeBallSimTick() { __try { DisableNativeBallSimTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// [2026-09-02 ★ TICK FLOOD FIX] sub_7FF6774C4630 (RVA 0x5464630) = the per-sim advance the manager tick
// calls from its unconditional fixed-step loop (stepSim LABEL_48), once per active sim per 1/90s substep.
// The GC flood is this stepping the PHANTOM local-host player's client-prediction sim (index -2): its
// FBallSimState float arrays get written each step and the GC ref-walker later reads a float as a TArray
// Num (~1 billion) -> flood. The dedicated server should NEVER run client-perspective prediction; it only
// owns AUTHORITATIVE sims (index >= 0, built per real arena/match). FIX: skip the advance for any negative
// simId -> the phantom -2 (and any -1) never steps (no garbage state, no flood), while real index>=0 arena
// sims step normally so balls simulate for actual matches. If a match's balls freeze, it means no
// authoritative sim was built for it (admission/seat gap) — that's the jakeball-campaign work, not this.
// [2026-09-03 ★★ CRITICAL: DO NOT SKIP THE ADVANCE] g_skipPhantomSimStep=true was the CAUSE of every tick-on
// freeze, not a fix. The manager tick's Phase-2 fixed-step loop is `for (j=v48[3]; j>=0.011112; j=v48[3])
// advance(simId)` — the advance is what DECREMENTS the accumulator v48[3]. Returning early for simId<0 without
// calling the original leaves v48[3] un-consumed, so j never drops below the step and the loop spins forever
// (watchdog caught it: `stuck in ADVANCE simId=-2 opSeq=1.39e9` = 1.4 BILLION calls in 4s). You CANNOT skip the
// advance without breaking the loop. Correct approach: let -2 advance natively (loop terminates) and let the
// already-installed GC Num-clamp contain any garbage the headless -2 state feeds the GC ref-walker. Flag kept
// (default OFF) only so the old behaviour can be toggled back for A/B testing — never enable it with tick on.
static constexpr uintptr_t BallAdvance_RVA = 0x5464630;
using BallAdvance_t = __int64(__fastcall*)(void*, unsigned int, char);
static BallAdvance_t BallAdvance_Orig = nullptr;
static bool g_skipPhantomSimStep = false;   // ★ MUST be false with tick on — see note above (skipping = infinite loop)
static __int64 __fastcall BallAdvance_Hook(void* mgr, unsigned int simId, char flag)
{
    g_lastOpPhase = 2; g_lastOpSim = (int)simId; ++g_lastOpSeq;   // watchdog stamp
    if (g_skipPhantomSimStep && (int)simId < 0)
        return 0;   // (DISABLED) skipping the advance breaks the fixed-step accumulator -> infinite loop
    return BallAdvance_Orig(mgr, simId, flag);
}

// [2026-09-03 ★ THE RESULTS-BUILD TAIL] sub_7FF6774EFC40 (RVA 0x548FC40) = the per-sim RECONCILE/CORRECTION
// build the manager tick (sub_7FF6774E63C0) runs in Phase 2 for every dirty sim, RIGHT BEFORE the advance
// (sub_7FF6774C4630). Signature: (mgr, simId, FBallSimState* corrections). It diffs the predicted vs
// authoritative FBallSimState and copies/writes correction montages into the sim's 208-byte-stride arrays.
// The advance was already skipped for the phantom -2 (BallAdvance_Hook) but THIS was not — and it is the
// "RESULTS-BUILD tail" the g_ballSimNoTick note said the advance-skip alone was missing. Server-side the -2
// sim has ZERO participants (confirmed: `[DiagBUILD] Index=-2 participants=0 []`), so reconciling it copies
// uninitialised state -> the GC ref-walker later reads a garbage float as a TArray Num = the flood. Offline
// the -2 sim carries a real local VR participant (`participants=1 [1->OK(BP_VRPawn_C ctrl=1 loc=1)]`) so
// reconcile has valid state and never garbages — THIS is why it ticks fine offline but fries on the server.
// Skipping it for negative simId is safe: the caller ignores the return and only clears the per-sim dirty
// flag (`*((_DWORD*)v48+50) = -1`) afterward. Together with BallAdvance_Hook this excludes the phantom -2
// from ALL Phase-2 tick paths (Phase 1 needs a real local instance = null on the server; Phase 3's
// per-participant work is guarded by count>0 so the 0-participant -2 falls straight through).
static constexpr uintptr_t BallReconcile_RVA = 0x548FC40;
using BallReconcile_t = __int64(__fastcall*)(void*, unsigned int, unsigned int*);
static BallReconcile_t BallReconcile_Orig = nullptr;
static bool g_skipPhantomReconcile = false;   // [2026-09-03] OFF — let -2 reconcile natively too (it's a one-shot guarded by the dirty flag, NOT a fixed-step loop, so it never caused the hang; run the native tick unmodified and rely on the GC Num-clamp). Kept for A/B toggling.
static __int64 __fastcall BallReconcile_Hook(void* mgr, unsigned int simId, unsigned int* corrections)
{
    g_lastOpPhase = 1; g_lastOpSim = (int)simId; ++g_lastOpSeq;   // watchdog stamp
    if (g_skipPhantomReconcile && (int)simId < 0)
        return 1;   // (DISABLED) 1 = "matched / no correction"
    return BallReconcile_Orig(mgr, simId, corrections);
}

// P2 — push the team color to the joining client. Admission (SetColorAndTeamIndex on the server
// pawn) doesn't recolor the owning client's view, so send the AVRPawn NetClient RPC
// Client_UpdateTeamColors(FTeamColor, int8) explicitly: UE marshals it to the owning connection
// (the VR player), whose client runs SetColorAndTeamIndex locally. Color comes from the TM's now-
// populated TeamColors[teamIndex] (P1). No SEH here (GetFunction uses std::string); the ProcessEvent
// is isolated in SafeProcessEvent (defined later — forward-declared).
static bool SafeProcessEvent(SDK::UObject* o, SDK::UFunction* fn, void* parms);
static void PushTeamColorToClient(void* pawnV, uintptr_t tm, int teamIndex)
{
    auto* pawn = reinterpret_cast<SDK::UObject*>(pawnV);
    if (!pawn || !pawn->Class || teamIndex < 0) { printf("[HalcyonA2][TCOL] Client bail: pawn/class/team (team=%d)\n", teamIndex); return; }
    static SDK::UFunction* fn = nullptr;
    if (!fn) fn = pawn->Class->GetFunction("VRPawn", "Client_UpdateTeamColors");
    if (!fn) { printf("[HalcyonA2][TCOL] Client bail: Client_UpdateTeamColors fn not found\n"); return; }
    auto* tcData    = *reinterpret_cast<unsigned char**>(tm + 0x2E8);   // TeamColors.Data
    const int tcNum = *reinterpret_cast<int*>(tm + 0x2F0);              // TeamColors.Num
    if (!tcData || teamIndex >= tcNum) { printf("[HalcyonA2][TCOL] Client bail: TeamColors empty/oob (data=%p num=%d team=%d)\n", tcData, tcNum, teamIndex); return; }
    struct { unsigned char TeamColor[0x14]; signed char TeamIndex; unsigned char pad[7]; } parms{};
    memcpy(parms.TeamColor, tcData + teamIndex * 0x14, 0x14);           // FTeamColor is 0x14 bytes
    parms.TeamIndex = static_cast<signed char>(teamIndex);
    const bool ok = SafeProcessEvent(pawn, fn, &parms);
    printf("[HalcyonA2][TCOL] Client_UpdateTeamColors SENT pawn=%s team=%d color=(%u,%u,%u,%u) pe_ok=%d\n",
           pawn->GetName().c_str(), teamIndex, parms.TeamColor[0], parms.TeamColor[1], parms.TeamColor[2], parms.TeamColor[3], ok);
}

// P0 — write the REPLICATED per-player team+color so it sticks and every client sees it. The
// client-only Client_UpdateTeamColors above doesn't write UA2PlayerEntity.VRPlayerRepData, so the
// default gray OnReps back over the local paint. Server_SetCurrentColor(FTeamColor,int8) is NetServer;
// on our authority ProcessEvent runs the impl locally and writes replicated TeamIndex@0x418 +
// CurrentTeamColor@0x500 on the entity (AVRPawn.Entity@0x928) -> replicates to all.
static void PushReplicatedTeamColor(void* pawnV, uintptr_t tm, int teamIndex)
{
    auto* pawn = reinterpret_cast<SDK::UObject*>(pawnV);
    if (!pawn || teamIndex < 0) { printf("[HalcyonA2][TCOL] Rep bail: pawn/team (team=%d)\n", teamIndex); return; }
    void* entityV = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + 0x928);  // AVRPawn.Entity
    if (!entityV) { printf("[HalcyonA2][TCOL] Rep bail: Entity@0x928 null\n"); return; }
    auto* entity = reinterpret_cast<SDK::UObject*>(entityV);
    if (!entity->Class) { printf("[HalcyonA2][TCOL] Rep bail: entity->Class null\n"); return; }
    static SDK::UFunction* fn = nullptr;
    if (!fn) fn = entity->Class->GetFunction("A2PlayerEntity", "Server_SetCurrentColor");
    if (!fn) { printf("[HalcyonA2][TCOL] Rep bail: Server_SetCurrentColor fn not found\n"); return; }
    auto* tcData    = *reinterpret_cast<unsigned char**>(tm + 0x2E8);   // TeamColors.Data
    const int tcNum = *reinterpret_cast<int*>(tm + 0x2F0);              // TeamColors.Num
    if (!tcData || teamIndex >= tcNum) { printf("[HalcyonA2][TCOL] Rep bail: TeamColors empty/oob (data=%p num=%d team=%d)\n", tcData, tcNum, teamIndex); return; }
    struct { unsigned char TeamColor[0x14]; signed char NewTeamIndex; unsigned char pad[3]; } parms{};
    memcpy(parms.TeamColor, tcData + teamIndex * 0x14, 0x14);
    parms.NewTeamIndex = static_cast<signed char>(teamIndex);
    // The Server_SetCurrentColor RPC writes only the Mass fragment, whose color never syncs to the
    // replicated copy -> register this player so the fast path stamps the color into the entity's
    // replicated VRPlayerRepData directly (the actual fix; the RPC below is now just belt-and-braces).
    RegisterColorTarget(entityV, parms.TeamColor, teamIndex);
    // The impl gates on sub_53AB0C0(entity) = (entity+0xC8 != 0 && entity+0xCC != 0) = the Mass entity
    // handle {Index@0xC8, Serial@0xCC}. If either is 0 the color write silently no-ops. Log it so we
    // see whether the gate passes on our headless server.
    const uintptr_t e = reinterpret_cast<uintptr_t>(entityV);
    const uint32_t massIdx = *reinterpret_cast<uint32_t*>(e + 0xC8);
    const uint32_t massSer = *reinterpret_cast<uint32_t*>(e + 0xCC);
    const bool ok = SafeProcessEvent(entity, fn, &parms);
    printf("[HalcyonA2][TCOL] Server_SetCurrentColor SENT entity=%s team=%d color=(%u,%u,%u,%u) pe_ok=%d | MassHandle idx=%u ser=%u gate=%s\n",
           entity->GetName().c_str(), teamIndex, parms.TeamColor[0], parms.TeamColor[1], parms.TeamColor[2], parms.TeamColor[3], ok,
           massIdx, massSer, (massIdx != 0 && massSer != 0) ? "PASS" : "FAIL(no write)");
}

// P1 — set the replicated player-state team so the arena roster / per-player display resolves the
// team (AAxPlayerState.TeamIndex@0x380, Net/RepNotify; default -1 renders "None"). Reached via
// APawn.PlayerState@0x2B8.
static void SetPlayerStateTeam(void* pawnV, int teamIndex)
{
    auto* pawn = reinterpret_cast<SDK::UObject*>(pawnV);
    if (!pawn || teamIndex < 0) { printf("[HalcyonA2][TCOL] PSTeam bail: pawn/team (team=%d)\n", teamIndex); return; }
    void* psV = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + 0x2B8);  // APawn.PlayerState
    if (!psV) { printf("[HalcyonA2][TCOL] PSTeam bail: PlayerState@0x2B8 null\n"); return; }
    auto* ps = reinterpret_cast<SDK::UObject*>(psV);
    if (!ps->Class) { printf("[HalcyonA2][TCOL] PSTeam bail: ps->Class null\n"); return; }
    static SDK::UFunction* fn = nullptr;
    if (!fn) fn = ps->Class->GetFunction("AxPlayerState", "SetTeamIndex");
    if (!fn) { printf("[HalcyonA2][TCOL] PSTeam bail: SetTeamIndex fn not found\n"); return; }
    struct { int32_t NewTeamIndex; } parms{ teamIndex };
    const bool ok = SafeProcessEvent(ps, fn, &parms);
    printf("[HalcyonA2][TCOL] SetTeamIndex SENT ps=%s team=%d pe_ok=%d\n", ps->GetName().c_str(), teamIndex, ok);
}

// AVRPawn::Server_NotifyPlayerLeftArena_Implementation (sub @ 0x5502B00, a1=pawn). Fires when a
// player leaves the arena. Our fast-path color stamp keeps re-writing the arena team color every
// tick, so leaving never visually resets — we must drop the entity from g_colorTargets here (stop
// stamping) and reset its color/team to the hub default so it actually clears.
static constexpr uintptr_t NotifyLeftArena_RVA = 0x5502B00;
using NotifyLeftArena_t = __int64(__fastcall*)(__int64, __int64);
static NotifyLeftArena_t NotifyLeftArena_Orig = nullptr;
static __int64 __fastcall NotifyLeftArena_Hook(__int64 pawn, __int64 a2)
{
    const __int64 r = NotifyLeftArena_Orig(pawn, a2);
    __try {
        void* entity = *reinterpret_cast<void**>(pawn + 0x928);
        if (entity)
        {
            for (int i = 0; i < g_colorTargetCount; ++i)          // stop stamping this entity
                if (g_colorTargets[i].entity == entity)
                { g_colorTargets[i] = g_colorTargets[--g_colorTargetCount]; break; }
            // [2026-09-08] Colour/team writes REMOVED — they used the 20996 layout: 0x500 and 0x418 are PAST
            // the end of UA2PlayerEntity (~0x338) and 0x2E8 overwrites `lastGrabbedPlayer` (a live AActor*).
            // Harmless before only because Entity@0x840 always read null; fixing that offset to 0x928 would
            // have turned this into live heap corruption on every arena exit. Colour lives in
            // CosmeticsFragmentReplicationOnly@0x200 on this build — map it before re-implementing.
        }
        *reinterpret_cast<signed char*>(pawn + 0x1CA0) = -1;      // pawn TeamIndex
        *reinterpret_cast<void**>(pawn + 0x1128) = nullptr;       // CurrentTicketManager backptr
        printf("[HalcyonA2][TCOL] player left arena -> color/team reset (pawn=%p)\n", reinterpret_cast<void*>(pawn));
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return r;
}

// GOLF-CUP server-side detection probe. Does the server natively detect the ball sinking? If so we
// can synthesize the BallInCup event to the (running) conductor ourselves instead of relying on the
// client send that isn't arriving. AGolfCup::BallInCup_Impl@0x53DC340 (cup, Score) fires OnBallInCup;
// OnGoalBeginOverlap_Impl@0x53F0410 (cup, overlappedComp, ball=OtherActor) is the trigger overlap.
static void LogGolfBallInCup(__int64 cup, unsigned int score)
{
    auto* c = reinterpret_cast<SDK::UObject*>(cup);
    printf("[HalcyonA2][GOLFCUP] BallInCup cup=%s Score=%u\n", (c && c->Class) ? c->GetName().c_str() : "?", score);
}
static void LogGolfOverlap(__int64 cup, __int64 ball)
{
    auto* c = reinterpret_cast<SDK::UObject*>(cup);
    auto* b = reinterpret_cast<SDK::UObject*>(ball);
    printf("[HalcyonA2][GOLFCUP] Overlap cup=%s ball=%s\n",
           (c && c->Class) ? c->GetName().c_str() : "?", (b && b->Class) ? b->GetName().c_str() : "(non-ball)");
}
static constexpr uintptr_t GolfBallInCup_RVA = 0x53DC320;   // [PORT-AUDIT] was 0x53DC340 (3-arg inner fn, hooked with a 2-arg thunk); 0x53DC320 = 20996 0x53986A0 equivalent
using GolfBallInCup_t = __int64(__fastcall*)(__int64, unsigned int);
static GolfBallInCup_t GolfBallInCup_Orig = nullptr;
static __int64 __fastcall GolfBallInCup_Hook(__int64 cup, unsigned int score)
{
    __try { LogGolfBallInCup(cup, score); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return GolfBallInCup_Orig(cup, score);
}
static constexpr uintptr_t GolfOverlap_RVA = 0x53F03B0;   // [PORT-AUDIT] was 0x53F0410 (unrelated 10-byte fn); 0x53F03B0 = 20996 0x53AD660 equivalent (align 1.00)
using GolfOverlap_t = void(__fastcall*)(__int64, __int64, __int64);
static GolfOverlap_t GolfOverlap_Orig = nullptr;
static void __fastcall GolfOverlap_Hook(__int64 cup, __int64 comp, __int64 ball)
{
    __try { LogGolfOverlap(cup, ball); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    GolfOverlap_Orig(cup, comp, ball);
}

// CRASH-STOPPER: FName->string resolver (sub_114E210 @ 0x114E210). On arena-leave the ball-sim
// teardown resolves an INVALID FName (id low-word 0xFFFF, null pool chunk) -> reads 0x1FFFE -> AV.
// SEH-wrap it: an invalid FName returns an empty FString instead of crashing. Zero cost on the normal
// path (x64 __try is table-based); only fires on the bad id. a2 = out FString {Data@0, Num/Max@8}.
static constexpr uintptr_t FNameResolve_RVA = 0x114D690;   // [PORT-AUDIT] 22284 (align 1.00 to 20996 0x114E210)
using FNameResolve_t = __int64*(__fastcall*)(unsigned int*, __int64*);
static FNameResolve_t FNameResolve_Orig = nullptr;
static __int64* __fastcall FNameResolve_Hook(unsigned int* a1, __int64* a2)
{
    __try { return FNameResolve_Orig(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { if (a2) { a2[0] = 0; a2[1] = 0; } return a2; }
}

// CRASH-STOPPER: Slate/text-run layout leaf (sub_1EC13E0 @ 0x1EC13E0). On arena-leave a UI text
// block (scoreboard/leaderboard) lays out a run whose FName key is INVALID (id low-word 0xFFFF,
// null pool chunk). The FName->entry lookup INLINED here (sub_1142D30) returns 0 + 2*0xFFFF =
// 0x1FFFE, and the next `movzx r10,byte[rax]` (0x1EC148C) AVs reading 0x1FFFE. The caller
// sub_1EC1BE0 is the recursive run-tree walker (the two 0x1EC1C5C frames). This inlined resolver
// is NOT sub_114E210, which is why the existing FName guard never caught it. Headless (-nullrhi)
// renders nothing, so a failed glyph layout is cosmetically irrelevant: swallow the AV and return
// 0 -> the walker's per-run loop just advances to the next run. Zero cost on the happy path
// (table-based x64 SEH). Narrowest possible wrap = the exact crash-site function only.
static constexpr uintptr_t TextLayoutLeaf_RVA = 0x1EC07A0;   // [PORT-AUDIT] 22284 (align 1.00 to 20996 0x1EC13E0)
using TextLayoutLeaf_t = __int64(__fastcall*)(__int64, __int64);
static TextLayoutLeaf_t TextLayoutLeaf_Orig = nullptr;
static __int64 __fastcall TextLayoutLeaf_Hook(__int64 a1, __int64 a2)
{
    __try { return TextLayoutLeaf_Orig(a1, a2); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// GOLF (and all A2 events) DIAGNOSTIC. sub_465F820(ctx, channel, payload) is the UNetEventsBridge
// server-side dispatch that BOTH the small-payload (TriggerEventOnServer_Param_Implementation) and the
// fractured path funnel into. payload a3 = {bytes@0, int len@8}. The bytes are a self-describing tagged
// tree (tag1=bool/4, tag3=number/8-byte double, tag5=string len-prefixed, tag6=table of key/value).
// Golf fires {EventType="BallInCup"|"OnPlayerStartHole", Cup/Score/PlayerID/HoleIndex as doubles}. Hook
// it, hexdump small payloads so we can see the REAL bytes and write the exact parser. Capped; temporary.
static constexpr uintptr_t EvtDispatch_RVA = 0x466A240;   // [PORT-AUDIT] 22284 (align 1.00 to 20996 0x465F820); hook still not installed (diagnostic only)
using EvtDispatch_t = __int64(__fastcall*)(__int64, __int64, __int64);
static EvtDispatch_t EvtDispatch_Orig = nullptr;
static int g_golfDumpCount = 0;
// True if the byte buffer contains `needle` as either UTF-8 (stride 1) or UTF-16LE (stride 2) chars.
static bool BytesContainAscii(const uint8_t* p, int len, const char* needle)
{
    const int nl = (int)strlen(needle);
    for (int stride = 1; stride <= 2; ++stride)
        for (int i = 0; i + (nl - 1) * stride < len; ++i)
        {
            int k = 0;
            for (; k < nl; ++k) if (p[i + k * stride] != (uint8_t)needle[k]) break;
            if (k == nl) return true;
        }
    return false;
}
static __int64 __fastcall EvtDispatch_Hook(__int64 ctx, __int64 channel, __int64 payload)
{
    __try {
        const uint8_t* p = payload ? *reinterpret_cast<uint8_t**>(payload) : nullptr;
        const int plen  = payload ? *reinterpret_cast<int*>(payload + 8) : 0;
        // Golf-only filter: skip everything unless the payload mentions a golf event/key. Kills the
        // boot spam (this dispatch fires for ALL A2 events) and targets BallInCup/OnPlayerStartHole.
        const bool isGolf = p && plen > 0 &&
            (BytesContainAscii(p, plen, "Cup") || BytesContainAscii(p, plen, "Hole") ||
             BytesContainAscii(p, plen, "Golf") || BytesContainAscii(p, plen, "StartHole"));
        if (isGolf && plen < 512 && g_golfDumpCount < 120)
        {
            // ascii view first (cheap) so we can eyeball which events are golf; skip pure-spam later.
            char asc[513];
            const int n = plen < 512 ? plen : 512;
            for (int i = 0; i < n; ++i) asc[i] = (p[i] >= 32 && p[i] < 127) ? (char)p[i] : '.';
            asc[n] = 0;
            // channel name (FString {wchar*@0,int@8}); guard — may be a different struct.
            const wchar_t* ch = channel ? *reinterpret_cast<wchar_t**>(channel) : nullptr;
            ++g_golfDumpCount;
            printf("[HalcyonA2][EVT] chan='%ls' len=%d ascii=\"%s\"\n", ch ? ch : L"?", plen, asc);
            // hex of the first 96 bytes for the parser.
            char hex[96 * 3 + 1]; const int hn = plen < 96 ? plen : 96;
            for (int i = 0; i < hn; ++i) sprintf(hex + i * 3, "%02X ", p[i]);
            printf("[HalcyonA2][EVT] hex=%s\n", hex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
    return EvtDispatch_Orig(ctx, channel, payload);
}

// ATicketManager::GiveAndCheckTicket impl (sub_46F6180). The arena-admission gate: gives
// the ticket (roster name) then verifies -> AddToVerifiedTicketHolders + SetColorAndTeamIndex.
// On our headless server players show on the roster but never reach VerifiedTicketHolders
// (the list the ball-sim rebuild + team-apply BOTH read) -> no team, no real sim seat (-2).
// Log every decision input + the verified-count promotion so we see WHICH bail fires:
//   team-full (teamSize>=max), CurrentTicket>RequiredTicket, whitelist, or the *(this+0x148)
//   world/slot gate that the admit branch requires non-null.
static constexpr uintptr_t GiveCheck_RVA = 0x4723AA0;
using GiveCheck_t = char(__fastcall*)(uintptr_t, uint64_t*, int64_t, char, char);
static GiveCheck_t GiveCheck_Orig = nullptr;
static int g_gacLog = 0;
static char __fastcall GiveCheck_Hook(uintptr_t tm, uint64_t* pawnIface, int64_t teamIndex,
                                      char silent, char checkOverlap)
{
    int   vBefore = -1, tBefore = -1, ts = -1, mts = -1, ovr = 0;
    float cur = 0.f, req = 0.f;
    void* gate = nullptr; void* pawn = nullptr;
    const int team = static_cast<int>(teamIndex);
    __try {
        vBefore = *reinterpret_cast<int*>(tm + 0x390);   // VerifiedTicketHolders num
        tBefore = *reinterpret_cast<int*>(tm + 0x380);   // TicketHolders num
        cur     = *reinterpret_cast<float*>(tm + 0x36C);
        req     = *reinterpret_cast<float*>(tm + 0x370);
        ovr     = *reinterpret_cast<uint8_t*>(tm + 0x368);
        int* tsD = *reinterpret_cast<int**>(tm + 0x3E0); int tsN = *reinterpret_cast<int*>(tm + 0x3E8);
        int* mD  = *reinterpret_cast<int**>(tm + 0x3F0); int mN  = *reinterpret_cast<int*>(tm + 0x3F8);
        if (tsD && team >= 0 && team < tsN) ts  = tsD[team];
        if (mD  && team >= 0 && team < mN)  mts = mD[team];
        gate = *reinterpret_cast<void**>(tm + 0x148);    // the admit-branch world/slot gate
        pawn = pawnIface ? reinterpret_cast<void*>(pawnIface[0]) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {}

    char r = GiveCheck_Orig(tm, pawnIface, teamIndex, silent, checkOverlap);

    // On a successful admit, do the two things the headless path leaves undone:
    // (1) stamp the pawn->manager backptr — GiveAndCheckTicket sets slot+ticket+team but
    //     leaves CurrentTicketManager@0x1128 null, and the seat-rebuild (sub_540E8A0)
    //     likely maps pawn->sim through it; a null => dumped to the -2 template.
    // (2) force a sim reconcile — reconcile (sub_540D970) only runs on VRPawn-count change,
    //     which does NOT fire on admission, so the freshly-verified holder never gets
    //     copied into SimulationsOutline.PlayerIndexes without a nudge.
    printf("[HalcyonA2][TCOL] GiveAndCheckTicket -> ret=%d pawn=%p team=%d (colors push %s)\n",
           (int)r, pawn, team, (r && pawn) ? "YES" : "NO");
    if (r && pawn)
    {
        // [22284 PORT] The 20996 pawn->manager backptr stamp (CurrentTicketManager@0x1128) stays REMOVED —
        // correct: the reconcile (sub_7FF6774BBD80) builds each sim's participant list from the sim's
        // TicketManager VerifiedTicketHolders@0x388 (count @0x390), resolving each holder's PlayerIndex@0x1C22.
        // It never reads pawn+0x1128.
        //
        // [2026-09-04 ★★ SEAT-ROSTER FIX] BUT that rebuild stage is GATED on a DIRTY BYTE at tm+0x398 (IDA:
        // `if (*(BYTE*)(tm+0x398)==0) skip; else rebuild + clear it`). The native AddToVerifiedTicketHolders
        // bumps VerifiedTicketHolders (verified 0->1 in [GAC]) but on our headless path nothing marks the
        // manager dirty, so the driven reconcile keeps skipping this sim's rebuild -> the verified holder is
        // never copied into SimulationsOutline.PlayerIndexes -> players=[] forever (what the [SEAT] dump shows).
        // Set the dirty byte here so the very next reconcile pass rebuilds THIS arena's sim with the new holder.
        __try { *reinterpret_cast<uint8_t*>(tm + 0x398) = 1; } __except (EXCEPTION_EXECUTE_HANDLER) {}
        g_lastReconcileCount = -1;   // belt: also force the count-change reconcile path next tick
        g_reconcileWanted = 8;       // [2026-09-05] let the gated reconcile through for a burst so this
                                     // freshly-verified holder gets seated, then it re-gates (no rubber-band)

        // (3) team color/membership. P0 writes the REPLICATED per-player color+team (sticks + all
        //     clients see it); P1 sets the player-state team so the roster resolves it (was "None");
        //     PushTeamColorToClient stays for the instant owning-client local paint.
        PushReplicatedTeamColor(pawn, tm, team);   // P0 (authoritative, replicated)
        SetPlayerStateTeam(pawn, team);            // P1 (roster / per-player team)
        PushTeamColorToClient(pawn, tm, team);     // local paint (belt-and-suspenders)
    }

    int vAfter = -1;
    __try { vAfter = *reinterpret_cast<int*>(tm + 0x390); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    if (g_gacLog < 200)
    {
        printf("[HalcyonA2][GAC] tm=%p pawn=%p team=%d silent=%d ovr=%d cur=%.1f req=%.1f "
               "teamSize=%d/%d gate(0x148)=%p verified %d->%d ticketHolders=%d ret=%d\n",
               (void*)tm, pawn, team, (int)silent, ovr, cur, req, ts, mts, gate,
               vBefore, vAfter, tBefore, (int)r);
        ++g_gacLog;
    }
    return r;
}

static void SafeDumpBallStructure() { __try { DumpBallStructure(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Match start requires a disc to physically overlap the CenterDiscCylinder start
// trigger (a UPhysicalComponent whose Collider is set to "OverlapAll"), which fires
// OverlapBegin -> OnOverlapByDisc -> the Luau OnBallHitsStartTrigger. Our sim writes
// the ball transform raw each step, so UE never re-runs overlap detection and the
// disc slides through without tripping the trigger. Force the recheck by calling the
// engine's UPrimitiveComponent::UpdateOverlaps directly on each disc's root primitive
// (it recomputes overlaps and fires OverlapBegin/End) -> the already-armed trigger
// fires naturally.
//   sub_37A7960 = UPrimitiveComponent::UpdateOverlaps(this, const TOverlapArrayView* PendingOverlaps,
//                 bool bDoNotifyActors, const TOverlapArrayView* OverlapsAtEndLocation)
//   (identified by the "UpdateOverlaps" stat string it registers)
// UpdateOverlaps can null-deref on a half-constructed / unregistered component (a
// disc renderer being spawned in a ConstructionScript, a pooled collider with no
// world yet). Guard each call with SEH so a bad component is skipped, not fatal.
// SEH function must hold no C++ unwinding objects (MSVC C2712) — so this is isolated.
static char(*g_UpdateOverlaps)(void*, void*, unsigned char, void*) = nullptr;
static char SafeUpdateOverlaps(void* comp)
{
    __try { return g_UpdateOverlaps(comp, nullptr, /*bDoNotifyActors=*/1, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static void PumpBallOverlaps()
{
    static SDK::UClass* primCls = nullptr;   // [PERF] cached class lookup
    if (!primCls) primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
    if (!primCls)
        return;
    if (!g_UpdateOverlaps)
        g_UpdateOverlaps = reinterpret_cast<char(*)(void*, void*, unsigned char, void*)>(GetBase() + 0x37B02C0);

    // CACHED disc/physical/goal lists, rebuilt ~1s. The old per-call full GObjects walk (120k+ objects
    // now that multiple gamemodes load) ran 10-20Hz ON THE GAME THREAD and starved actor replication ->
    // remote players teleported/snapped. DetectGoals hit + fixed the exact same thing; same pattern here.
    // Iterating the small cached lists (~70 discs + ~220 phys + ~32 goals) instead of 120k objects is a
    // ~300x cut. Stale entries (GC between rebuilds) are caught by the SafePumpBallOverlaps SEH wrapper.
    static SDK::UObject* discs[128]; static int nDisc = 0;
    static SDK::UObject* phys[512];  static int nPhys = 0;
    static SDK::UObject* goals[64];  static int nGoal = 0;
    static ULONGLONG lastRebuild = 0;
    const ULONGLONG now = GetTickCount64();
        // Same per-tick full-scan trap as the other caches: an empty result must not mean
        // "re-walk the whole GObjects array every tick, forever". Retry on a timer.
    // [BACKOFF] A full GObjects walk costs ~50ms on the VPS core. A fixed 500ms empty-retry
    // meant 3 always-empty caches each scanned 2x/s = ~300ms/s = 30% of wall (measured by
    // [PROF]). Level geometry that is absent stays absent, so back off to 8s; any non-empty
    // result snaps back to 3s (was 1s: [PROF] showed 5 populated caches each doing a ~100ms full
    // walk every second = ~550ms/s. Only LIST DISCOVERY is expensive - positions are read from
    // cached pointers at 10Hz regardless - so a new ball joins its detector within 3s and is
    // then tracked at full rate). s_bo is read before the rebuild resets the count,
    // so it reflects the PREVIOUS scan's result.
    static ULONGLONG s_bo = 500;
    static long s_epoch = -1;
    // STAGGERED: re-arming all six caches to the same interval made a JOIN rebuild every
    // one of them in the same frame - six full 167k walks at once, on top of EnableGoals.
    // The user saw ping spike to ~1s exactly when a second client joined. Spread them out.
    if (s_epoch != g_cacheEpoch) { s_epoch = g_cacheEpoch; s_bo = 1800; }   // re-arm, widely staggered
    if (now - lastRebuild > s_bo)
    {
        s_bo = ((nDisc == 0 && nPhys == 0 && nGoal == 0)) ? ((s_bo < 8000) ? s_bo * 2 : 8000) : 3000;
        InterlockedIncrement(&g_rebuilds);   // [PROF] how often caches actually rebuild
        lastRebuild = now;
        nDisc = nPhys = nGoal = 0;
        static SDK::UClass* discCls = nullptr;   // [PERF] cached class lookup
        if (!discCls) discCls = SDK::UObject::FindClassFast("DiscEntity");
        static SDK::UClass* physCls = nullptr;   // [PERF] cached class lookup
        if (!physCls) physCls = SDK::UObject::FindClassFast("PhysicalComponent");
        static SDK::UClass* goalCls = nullptr;   // [PERF] cached class lookup
        if (!goalCls) goalCls = SDK::UObject::FindClassFast("GoalComponent");
        const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (discCls && nDisc < 128 && o->IsA(discCls))      discs[nDisc++] = o;
            else if (physCls && nPhys < 512 && o->IsA(physCls)) phys[nPhys++]  = o;
            else if (goalCls && nGoal < 64 && o->IsA(goalCls))  goals[nGoal++] = o;
        }
    }

    // [PERF 2026-09-09] AMORTIZED. This used to run every list in full on ONE call: up to
    // 128 discs x2 + 512 physical colliders + 64 goals = ~830 UpdateOverlaps, and each one is a
    // physics query. [PROF] caught a single call at 306ms = 27 frames at 90Hz. That one stall is
    // what shows up as MIB (the client runs ~30 frames ahead of server confirmation) and makes
    // the client's ping jump 72 -> 112 and back. The AVERAGE cost was never the problem.
    //
    // This is a periodic RE-CHECK (a fallback so a disc already sitting inside a trigger still
    // trips OnOverlapByDisc), so it does not have to finish within one frame. Process a fixed
    // budget per call and carry a cursor across the three lists, treating them as one sequence.
    // Full coverage now takes ~1.3s at 10Hz instead of one 300ms stall.
    // A COUNT budget is not a bound here: UpdateOverlaps cost varies per component, and 64 items
    // still peaked at 204ms. Budget by TIME instead, so a single call can never blow the frame no
    // matter how expensive the individual queries turn out to be.
    static int s_ovCursor = 0;
    const double OV_BUDGET_MS = 4.0;
    const int totalItems = nDisc + nPhys + nGoal;
    if (totalItems <= 0) return;
    if (s_ovCursor >= totalItems) s_ovCursor = 0;
    LARGE_INTEGER _ovQpf, _ovT0, _ovT1;
    QueryPerformanceFrequency(&_ovQpf);
    QueryPerformanceCounter(&_ovT0);
    for (int done = 0; done < totalItems; ++done)
    {
        if (done)
        {
            QueryPerformanceCounter(&_ovT1);
            if (((double)(_ovT1.QuadPart - _ovT0.QuadPart) * 1000.0 / (double)_ovQpf.QuadPart) >= OV_BUDGET_MS)
                break;
        }
        const int idx = s_ovCursor;
        s_ovCursor = (idx + 1 >= totalItems) ? 0 : (idx + 1);
        if (idx < nDisc)
        {
            // Disc side: the ball's root primitive AND its SphereComponent@0x4D0 (the real
            // collision body; the root may be a non-colliding scene component).
            auto* o = discs[idx];
            auto* a = static_cast<SDK::AActor*>(o);
            auto* root = a->RootComponent;
            if (root && root->IsA(primCls))
                SafeUpdateOverlaps(root);
            void* sphere = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x4D0);
            if (sphere && static_cast<SDK::UObject*>(sphere)->IsA(primCls))
            {
                static_cast<SDK::UPrimitiveComponent*>(sphere)->SetGenerateOverlapEvents(true);
                SafeUpdateOverlaps(sphere);
            }
        }
        else if (idx < nDisc + nPhys)
        {
            // Trigger side: force each PhysicalComponent's Collider@0x4F0 to generate events and
            // re-check, so both sides of the overlap generate events.
            void* collider = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(phys[idx - nDisc]) + 0x4F0);
            if (collider && static_cast<SDK::UObject*>(collider)->IsA(primCls))
            {
                static_cast<SDK::UPrimitiveComponent*>(collider)->SetGenerateOverlapEvents(true);
                SafeUpdateOverlaps(collider);
            }
        }
        else
        {
            // Goal side: UGoalComponent's trigger is a UStaticMeshComponent@0x5A0.
            void* collider = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(goals[idx - nDisc - nPhys]) + 0x5A0);
            if (collider && static_cast<SDK::UObject*>(collider)->IsA(primCls))
            {
                static_cast<SDK::UPrimitiveComponent*>(collider)->SetGenerateOverlapEvents(true);
                SafeUpdateOverlaps(collider);
            }
        }
    }
}

// The pump touches many game objects (SetGenerateOverlapEvents, UpdateOverlaps) that
// can be half-constructed while an arena is streaming in -> null deref. Wrap the whole
// thing in SEH (this wrapper holds no C++ unwinding objects, so __try is legal) so a
// transient bad object skips a frame instead of killing the process.
static void SafePumpBallOverlaps()
{
    __try { PumpBallOverlaps(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// [2026-09-03 ★ DEATHRUN FINISH -> OVERTIME/SPLEEF]
// When a Runner reaches the finish, the Luau `EndTrigger.onOverlapByPlayerServer.Listen(CheckWhoHitEndTrigger)`
// switches the match to OVERTIME_COUNTDOWN (the spleef finale). That event is broadcast by
// UPhysicalComponent::OverlapBegin_Implementation (RVA 0x53B2B20) when a player pawn overlaps the trigger
// server-side — self=trigger, arg2=overlapped comp (UNUSED by the impl), arg3=OtherActor. But our VR
// pawns' server collision doesn't track the player (the real pose lives on AVRPawn.Entity@0x928, not the
// pawn actor root), so the engine never detects the overlap and the finish does nothing (dying/reset still
// works because that path is client-side / IsLocallyControlled). FIX: geometrically detect a Runner at the
// EndTrigger using the entity's server-side pose, then call OverlapBegin_Implementation(endTrigger, 0, pawn)
// directly to run the real Luau path -> SwitchState(OVERTIME_COUNTDOWN); its 15s countdown is then advanced
// to OVERTIME_RUNNING by the GameTimeComponent tick driver. The Luau itself re-checks team==0 && RUNNING, so
// a mis-timed fire is harmless — but we only latch on a genuine team-0 Runner so a Killer can't consume it.
// The EndTrigger is the level.json prefab named "PlayerOverlapAtEnd" (PrimitiveCubePurpleTrigger).
using OverlapBegin_t = void(__fastcall*)(void*, __int64, void*);
static void*     g_endTrigger        = nullptr;   // cached EndTrigger UPhysicalComponent
static double    g_endTriggerLoc[3]  = {};        // its world center
static ULONGLONG g_lastEndScan       = 0;         // EndTrigger (re)locate throttle
static ULONGLONG g_lastFinishTick    = 0;         // detector run throttle
static ULONGLONG g_lastFinishNearLog = 0;         // "runner near finish" log throttle
static double    g_spleefRadius      = 2000.0;    // fire radius around the finish (tunable via -SpleefRadius=N)

static void DetectRunnerAtFinish()
{
    // [PERF] Was called on every ProcessEvent dispatch pass (~150/s) and cost 42-92ms/s even on the
    // early-return path. The geometric finish check only needs 10Hz.
    {
        static ULONGLONG s_last = 0;
        const ULONGLONG nowR = GetTickCount64();
        if (nowR - s_last < 100) return;
        s_last = nowR;
    }
    if (g_endTriggerFired) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_lastFinishTick < 200) return;     // ~5Hz
    g_lastFinishTick = now;

    // (Re)locate the EndTrigger PhysicalComponent by prefab name "PlayerOverlapAtEnd" (throttled).
    if (!g_endTrigger || now - g_lastEndScan > 3000)
    {
        g_lastEndScan = now;
        static SDK::UClass* physCls = nullptr;   // [PERF] cached class lookup
        if (!physCls) physCls = SDK::UObject::FindClassFast("PhysicalComponent");
        if (physCls)
        {
            const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
            for (int32_t i = 0; i < num; ++i)
            {
                auto* o = SDK::UObject::GObjects->GetByIndex(i);
                if (!o || o->IsDefaultObject() || !o->IsA(physCls)) continue;
                SDK::UObject* owner = o->Outer;
                bool hit = o->GetName().find("PlayerOverlapAtEnd") != std::string::npos
                        || (owner && owner->GetName().find("PlayerOverlapAtEnd") != std::string::npos);
                if (hit)
                {
                    if (g_endTrigger != o)
                        printf("[HalcyonA2][SPLEEF] cached EndTrigger comp=%p (%s) owner=%s\n",
                               o, o->GetName().c_str(), owner ? owner->GetName().c_str() : "?");
                    g_endTrigger = o;
                    if (owner) WireActorLoc(owner, g_endTriggerLoc);   // world center
                    break;
                }
            }
        }
    }
    if (!g_endTrigger) return;

    // Cached VRPawn list (rebuilt ~1s) so we don't walk GObjects at detector rate (starves replication).
    static SDK::UObject* pawns[128]; static int nPawn = 0; static ULONGLONG lastPawnRebuild = 0;
        // Same per-tick full-scan trap as the other caches: an empty result must not mean
        // "re-walk the whole GObjects array every tick, forever". Retry on a timer.
    // [BACKOFF] A full GObjects walk costs ~50ms on the VPS core. A fixed 500ms empty-retry
    // meant 3 always-empty caches each scanned 2x/s = ~300ms/s = 30% of wall (measured by
    // [PROF]). Level geometry that is absent stays absent, so back off to 8s; any non-empty
    // result snaps back to 3s (was 1s: [PROF] showed 5 populated caches each doing a ~100ms full
    // walk every second = ~550ms/s. Only LIST DISCOVERY is expensive - positions are read from
    // cached pointers at 10Hz regardless - so a new ball joins its detector within 3s and is
    // then tracked at full rate). s_bo is read before the rebuild resets the count,
    // so it reflects the PREVIOUS scan's result.
    static ULONGLONG s_bo = 500;
    static long s_epoch = -1;
    // STAGGERED: re-arming all six caches to the same interval made a JOIN rebuild every
    // one of them in the same frame - six full 167k walks at once, on top of EnableGoals.
    // The user saw ping spike to ~1s exactly when a second client joined. Spread them out.
    if (s_epoch != g_cacheEpoch) { s_epoch = g_cacheEpoch; s_bo = 3000; }   // re-arm, widely staggered
    if (now - lastPawnRebuild > s_bo)
    {
        s_bo = (nPawn == 0) ? ((s_bo < 8000) ? s_bo * 2 : 8000) : 3000;
        InterlockedIncrement(&g_rebuilds);   // [PROF] how often caches actually rebuild
        lastPawnRebuild = now; nPawn = 0;
        static SDK::UClass* pawnCls = nullptr;   // [PERF] cached class lookup
        if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
        if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("BP_VRPawn_C");
        if (pawnCls)
        {
            const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
            for (int32_t i = 0; i < num && nPawn < 128; ++i)
            {
                auto* o = SDK::UObject::GObjects->GetByIndex(i);
                if (o && !o->IsDefaultObject() && o->IsA(pawnCls)) pawns[nPawn++] = o;
            }
        }
    }

    const double R2 = g_spleefRadius * g_spleefRadius;
    for (int i = 0; i < nPawn; ++i)
    {
        auto* o = pawns[i];
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        void* entity = *reinterpret_cast<void**>(p + 0x928);   // AVRPawn.Entity
        if (!entity) continue;
        const uintptr_t e = reinterpret_cast<uintptr_t>(entity);
        // [2026-09-08] The old `team = *(signed char*)(e + 0x418)` is an OUT-OF-BOUNDS read on 22284
        // (UA2PlayerEntity ends ~0x338; 0x418 is a 20996 VRPlayerRepData offset). This whole detector was
        // dormant until now only because Entity@0x840 always read null — fixing that to 0x928 would have
        // turned it into a live OOB read. Keep it dormant until the real TeamIndex is mapped for this build
        // (SDK shows TeamIndex on other classes at 0x368 / 0x380 Net+RepNotify, not on UA2PlayerEntity).
        continue;
        double px = *reinterpret_cast<double*>(e + 0x100);               // localData FrequentData.Root.pos (server working copy)
        double py = *reinterpret_cast<double*>(e + 0x108);
        double pz = *reinterpret_cast<double*>(e + 0x110);
        if (px == 0.0 && py == 0.0 && pz == 0.0) continue;               // no pose yet
        double dx = px - g_endTriggerLoc[0], dy = py - g_endTriggerLoc[1], dz = pz - g_endTriggerLoc[2];
        double d2 = dx * dx + dy * dy + dz * dz;
        if (d2 <= R2)
        {
            unsigned char pawnID = *reinterpret_cast<unsigned char*>(p + 0x1C22);   // PlayerIndex
            printf("[HalcyonA2][SPLEEF] Runner pawn=%s id=%u at finish d=%.0f (r=%.0f) -> firing onOverlapByPlayerServer\n",
                   o->GetName().c_str(), pawnID, sqrt(d2), g_spleefRadius);
            auto ob = reinterpret_cast<OverlapBegin_t>(GetBase() + 0x53B2B20);
            ob(g_endTrigger, 0, o);          // -> Luau CheckWhoHitEndTrigger -> SwitchState(OVERTIME_COUNTDOWN)
            g_endTriggerFired = true;
            break;
        }
        else if (d2 <= R2 * 4.0 && now - g_lastFinishNearLog > 1000)     // diagnostic: near but not firing
        {
            g_lastFinishNearLog = now;
            printf("[HalcyonA2][SPLEEF] Runner pawn=%s near finish d=%.0f (fire<%.0f) at (%.0f,%.0f,%.0f) trig(%.0f,%.0f,%.0f)\n",
                   o->GetName().c_str(), sqrt(d2), g_spleefRadius, px, py, pz,
                   g_endTriggerLoc[0], g_endTriggerLoc[1], g_endTriggerLoc[2]);
        }
    }
}
static void SafeDetectRunnerAtFinish() { __try { DetectRunnerAtFinish(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Goal-collider state: is UGoalComponent::Collider@0x5A0 a valid PrimitiveComponent, is the
// goal enabled (bGoalEnabled@0x5A9), and where is it vs the ball? If OverlapBegin never fires
// we need to know whether the collider is valid/enabled (setup) or the overlap just isn't
// registering (profile/binding).
static void DumpGoals()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    static SDK::UClass* goalCls = nullptr;   // [PERF] cached class lookup
    if (!goalCls) goalCls = SDK::UObject::FindClassFast("GoalComponent");
    static SDK::UClass* primCls = nullptr;   // [PERF] cached class lookup
    if (!primCls) primCls = SDK::UObject::FindClassFast("PrimitiveComponent");
    if (!goalCls || !primCls)
        return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    int shown = 0;
    for (int32_t i = 0; i < num && shown < 8; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(goalCls))
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        void* col        = *reinterpret_cast<void**>(p + 0x5A0);
        const int  team  = *reinterpret_cast<int*>(p + 0x590);
        const int  gti   = *reinterpret_cast<int*>(p + 0x598);
        const int  en    = *reinterpret_cast<uint8_t*>(p + 0x5A9);
        const int  ig    = *reinterpret_cast<uint8_t*>(p + 0x5A8);
        const bool cPrim = col && static_cast<SDK::UObject*>(col)->IsA(primCls);
        SDK::FVector loc{};
        if (cPrim) loc = static_cast<SDK::USceneComponent*>(col)->K2_GetComponentLocation();
        printf("[HalcyonA2][GOALDBG] %s col=%p prim=%d enabled=%d inGoal=%d team=%d gti=%d pos=(%.0f,%.0f,%.0f)\n",
               o->GetName().c_str(), col, (int)cPrim, en, ig, team, gti,
               (double)loc.X, (double)loc.Y, (double)loc.Z);
        ++shown;
    }
}
static void SafeDumpGoals() { __try { DumpGoals(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Every goal ships with bGoalEnabled@0x5A9 == 0 on our server (the serverOnly Luau that
// calls EnableGoal(true) at match start never ran) -> UGoalComponent::OverlapBegin
// short-circuits and no goal is ever detected. Call the game's own EnableGoal(true) on each
// real goal (team@0x590 >= 0 filters out the -1 CDO/_GEN_VARIABLE templates). Re-run
// periodically so a goal that disables itself after scoring re-arms for the next round.
static void EnableGoals()
{
    // [PERF 2026-09-09] This walked the FULL ~167k object array on EVERY call (1/s, ungated) and
    // [PROF] caught it at 280ms - a join-time stall big enough to spike a client to ~1s ping.
    // The pass is idempotent (already-enabled goals are skipped), so it only needs to run when
    // goals may have appeared: periodically, and immediately on a player-count change (match
    // forming), which is what g_cacheEpoch signals.
    {
        static ULONGLONG s_lastEnable = 0;
        static long      s_enEpoch    = -1;
        const ULONGLONG  nowEn = GetTickCount64();
        if (s_enEpoch != g_cacheEpoch) { s_enEpoch = g_cacheEpoch; s_lastEnable = 0; }
        if (s_lastEnable && nowEn - s_lastEnable < 3000) return;
        s_lastEnable = nowEn;
    }
    static SDK::UClass* goalCls = nullptr;   // [PERF] cached class lookup
    if (!goalCls) goalCls = SDK::UObject::FindClassFast("GoalComponent");
    if (!goalCls)
        return;
    static SDK::UFunction* fnEnable = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(goalCls))
            continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        if (*reinterpret_cast<int*>(p + 0x590) < 0)          // template / unassigned goal
            continue;
        if (*reinterpret_cast<uint8_t*>(p + 0x5A9) != 0)     // already enabled
            continue;
        if (!fnEnable) fnEnable = o->Class->GetFunction("GoalComponent", "EnableGoal");
        if (!fnEnable) return;
        struct { bool bEnable; } params{ true };
        o->ProcessEvent(fnEnable, &params);
        // Belt-and-suspenders: force the flag too, in case EnableGoal is a headless no-op.
        // Our scoring is geometric (DetectGoals), so all we need is this byte set for the gate.
        *reinterpret_cast<uint8_t*>(p + 0x5A9) = 1;
    }
}
static void SafeEnableGoals() { __try { EnableGoals(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// GEOMETRIC goal detection. The UE overlap chain (ball collider vs UGoalComponent::Collider
// -> OverlapBegin) never fires on our server even with goals enabled + overlaps pumped
// (collision profile / Luau-driven binding missing). So detect the ball entering a goal by
// distance and drive the game's own score path directly. SimulateGoalOnTeam0() runs the full
// native chain (IncrementScore + freeze ball + advance round via the GameStateManager that
// BP already listens to). Per-goal 3s cooldown prevents double-counting while the ball sits
// in. (Team-correct scoring is a follow-up; this proves the pipeline.)
struct FBoundsParams { SDK::USceneComponent* Component; SDK::FVector Origin; SDK::FVector BoxExtent; float SphereRadius; char Pad[4]; };

// Team-correct goal scoring, extracted from UGoalComponent::SimulateGoalOnTeam0. That function
// just fakes up an FGoalInfo and calls sub_5371710(goal, &info) @ RVA 0x53AF7C0 — the real scorer,
// which resolves the arena's ScoreComponent, IncrementScore()s, freezes the ball, and advances the
// round via the GameStateManager the BP already listens to. SimulateGoalOnTeam0 hardcodes
// ScoringTeam=1 (ball in team-0's net -> team 1 scores) + sets goal+0x5A8=1 first. We set
// ScoringTeam from the goal actually hit so the correct team gets the point. Other fields mirror
// Simulate (Distance=100, GoalPoints=1, PlayerScoredID=-1, Timestamp=*(*(goal+0xA0)+0x6C0)); the
// rest are cosmetic stats.
// Fill an FString (16 bytes at outFStr) with the scorer's display name via the game's own
// GetPlayerName, which returns a GAME-ALLOCATED FString. This is REQUIRED for safety: the score
// component's FString ops (assign/realloc/free) must operate on a game-heap block — handing the
// game a pointer into our DLL's memory (a static buffer) trips FMallocBinned2 "realloc an
// unrecognized block" and crashes the process. The out FString leaks its buffer (SDK FString has no
// destructor), which is a tiny, per-goal leak — acceptable, and safe.
static void FillScorerNameFString(int playerIdx, void* outFStr)
{
    memset(outFStr, 0, 0x10);
    if (playerIdx < 0) return;
    static SDK::UClass* pawnCls = nullptr;
    if (!pawnCls) pawnCls = SDK::UObject::FindClassFast("VRPawn");
    if (!pawnCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(pawnCls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        if (*reinterpret_cast<int*>(p + 0x1C22) != playerIdx) continue;
        auto* ps = *reinterpret_cast<SDK::UObject**>(p + 0x2B8);   // APawn.PlayerState
        if (!ps || !ps->Class) return;
        static SDK::UFunction* fn = nullptr;
        if (!fn) fn = ps->Class->GetFunction("PlayerState", "GetPlayerName");
        if (!fn) fn = ps->Class->GetFunction("AxPlayerState", "GetPlayerName");
        if (!fn) return;
        SafeProcessEvent(ps, fn, outFStr);   // GetPlayerName params = { FString ReturnValue@0 }
        return;
    }
}

// SEH-isolate the name resolution — it scans GObjects + ProcessEvents GetPlayerName, and it runs
// BEFORE the score call, so a fault here must NOT abort ScoreGoal (that would silently swallow the
// goal). On failure just leave the name empty; the score still lands.
static void SafeFillScorerName(int id, void* out) { __try { FillScorerNameFString(id, out); } __except (EXCEPTION_EXECUTE_HANDLER) { memset(out, 0, 0x10); } }

using GoalScoreFn = void(__fastcall*)(void* goal, SDK::FGoalInfo* info);
static bool g_goalUseOverlap = true;   // prefer driving the game's own OverlapBegin (FX + score)
static void ScoreGoal(void* goal, int scoringTeam, int scorerId, void* ball)
{
    const uintptr_t g = reinterpret_cast<uintptr_t>(goal);

    // PREFERRED PATH — drive the game's OWN UGoalComponent::OverlapBegin with the ball. Our direct
    // sub_5371710 call only publishes an EMPTY "PostGoalWithDisc" bridge event (conductor scores, but
    // clients get no goal data -> no FX). OverlapBegin builds the full payload (velocity/position/scorer/
    // bounces @goal+0x1160), fires the effect virtual + sound, and arms the timer that publishes the
    // populated event -> clients get the goal FX AND the conductor still scores off the same publish.
    // Guarded: OverlapBegin sets goal+0x480 = 257 (bInGoal|pending) when it runs the full path; if its
    // arena/disc guards reject our synthetic overlap it won't, so we fall back to the raw scorer.
    if (g_goalUseOverlap && ball)
    {
        auto* gc = static_cast<SDK::UObject*>(goal);
        void* collider = *reinterpret_cast<void**>(g + 0x5A0);   // UGoalComponent.Collider (== a1+1440 in OverlapBegin)
        static SDK::UFunction* fnOverlap = nullptr;
        if (!fnOverlap && gc->Class) fnOverlap = gc->Class->GetFunction("GoalComponent", "OverlapBegin");
        HxLog("[HalcyonA2][GOALFX] attempt fn=%p collider=%p ball=%p\n", (void*)fnOverlap, collider, ball);
        if (fnOverlap && collider)
        {
            *reinterpret_cast<uint16_t*>(g + 0x480) = 0;   // clear so we can detect a fresh "took"
            struct {
                void* Overlapped; void* OtherActor; void* OtherComp;
                int32_t OtherBodyIndex; bool FromSweep; char pad[3];
                unsigned char SweepResult[0x120];
            } parm{};
            parm.Overlapped = collider;
            parm.OtherActor = ball;
            gc->ProcessEvent(fnOverlap, &parm);
            if (*reinterpret_cast<uint16_t*>(g + 0x480) == 257)
            {
                HxLog("[HalcyonA2][GOALFX] OverlapBegin took -> FX + publish (goal=%p ball=%p)\n", goal, ball);
                return;   // OverlapBegin ran the full path -> it owns the FX + the (timer) publish
            }
            HxLog("[HalcyonA2][GOALFX] OverlapBegin rejected (guards) -> raw scorer fallback (no FX)\n");
        }
    }

    // FALLBACK — raw scorer (no FX). Only reached if OverlapBegin's guards rejected our overlap, so
    // scoring never regresses.
    *reinterpret_cast<uint8_t*>(g + 0x5A8) = 1;   // mark scored (Simulate sets this before scoring)

    double timestamp = 0.0;                        // FGoalInfo.Timestamp (cosmetic goal-time stat)
    if (void* tsrc = *reinterpret_cast<void**>(g + 0xA0))
        timestamp = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(tsrc) + 0x6C0);

    SDK::FGoalInfo info{};
    info.ScoringTeam      = scoringTeam;
    info.Distance         = 100.0f;
    info.GoalPoints       = 1;
    // Attribute the goal to the last player to hit the ball (ADiscEntity.RollbackRecentPlayerHit)
    // so "you scored a goal" quests credit the actual scorer + the scoreboard shows "Scored by".
    info.PlayerScoredTeam = scoringTeam;
    info.PlayerScoredID   = scorerId;
    info.Timestamp        = timestamp;
    // scorer NAME -> game-allocated FString (see FillScorerNameFString; static buffer here crashed).
    // SEH-isolated so name resolution can never prevent the score from landing.
    SafeFillScorerName(scorerId, reinterpret_cast<char*>(&info) + 0x40);

    reinterpret_cast<GoalScoreFn>(GetBase() + 0x53AF7C0)(goal, &info);
}

// [PERF 2026-09-09] Bounds memo. GetComponentBounds is a reflected ProcessEvent; recomputing it
// for every cached goal / golf cup / floor panel on EVERY rebuild was the dominant peak -
// [PROF] caught VolleyfallTick at 256ms with up to 128 panels. A single 256ms stall is ~23 frames
// at 90Hz, which is what lands in MIB and makes a client's ping spike then recover.
//
// These are STATIC level geometry: their bounds do not move. So measure each component once and
// reuse it. Keyed by COMPONENT pointer, and the whole memo is dropped whenever g_cacheEpoch
// changes (player count changed = match/level transition), so a destroyed-and-recycled address
// can never hand back stale bounds across a match.
struct BoundsMemoEntry { void* comp; SDK::FVector org, ext; };
static BoundsMemoEntry g_bmemo[512];
static int  g_bmemoN     = 0;
static long g_bmemoEpoch = -1;
static bool BoundsMemoGet(void* comp, SDK::FVector& org, SDK::FVector& ext)
{
    if (g_bmemoEpoch != g_cacheEpoch) { g_bmemoEpoch = g_cacheEpoch; g_bmemoN = 0; }
    for (int i = 0; i < g_bmemoN; ++i)
        if (g_bmemo[i].comp == comp) { org = g_bmemo[i].org; ext = g_bmemo[i].ext; return true; }
    return false;
}
static void BoundsMemoPut(void* comp, const SDK::FVector& org, const SDK::FVector& ext)
{
    if (g_bmemoN >= 512) return;
    g_bmemo[g_bmemoN].comp = comp; g_bmemo[g_bmemoN].org = org; g_bmemo[g_bmemoN].ext = ext;
    ++g_bmemoN;
}

static void DetectGoals()
{
    static SDK::UClass* goalCls = nullptr;   // [PERF] cached class lookup
    if (!goalCls) goalCls = SDK::UObject::FindClassFast("GoalComponent");
    static SDK::UClass* ballCls = nullptr;   // [PERF] cached class lookup
    if (!ballCls) ballCls = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!goalCls || !ballCls)
        return;
    static SDK::UFunction* fnBounds = nullptr;
    static SDK::UObject*    kslCDO  = nullptr;
    if (!kslCDO)  kslCDO = static_cast<SDK::UObject*>(SDK::UKismetSystemLibrary::GetDefaultObj());
    if (!fnBounds && kslCDO) fnBounds = kslCDO->Class->GetFunction("KismetSystemLibrary", "GetComponentBounds");
    if (!kslCDO || !fnBounds)
        return;
    // Edge-triggered debounce: a ball scores ONCE when it enters a goal, and only re-arms after it
    // has left every goal. (The old 3s time cooldown re-scored a ball that just sat frozen in the net.)
    static std::unordered_set<void*> ballInGoal;
    const double MARGIN = 40.0;   // small slack so the ball (radius) counts as "in"

    // CACHED goal + ball lists, rebuilt ~1Hz. A full GObjects walk (twice) + per-goal
    // GetComponentBounds every 100ms stuttered the game thread on a weak/contended VPS core, which
    // made the rollback sim burst-step and inflated MI. At 10Hz we now only read cached ball
    // positions vs cached goal AABBs (no walk, no ProcessEvent). Stale cache entries (GC) are caught
    // by the SafeDetectGoals SEH wrapper + refreshed on the next rebuild.
    static SDK::UObject* gObj[64]; static SDK::FVector gOrg[64]; static SDK::FVector gExt[64]; static int gN = 0;
    static SDK::UObject* bObj[256]; static int bN = 0;
    static ULONGLONG lastRebuild = 0;
    const ULONGLONG now = GetTickCount64();
        // NOTE: this guard used to read "|| gN == 0", which meant that whenever the scan found
        // NOTHING it re-ran a FULL GObjects walk (a virtual IsA per object, over the entire
        // object array) EVERY TICK, forever - e.g. Station_Prime simply has no goals, so the
        // goal cache never populated and never stopped scanning. A fast desktop core absorbed
        // it; a VPS vCPU did not: it pegged the game thread (client ping ~1s, STEP calls/s 90->25).
        // Still retry while empty, just on a timer instead of every single tick.
    // [BACKOFF] A full GObjects walk costs ~50ms on the VPS core. A fixed 500ms empty-retry
    // meant 3 always-empty caches each scanned 2x/s = ~300ms/s = 30% of wall (measured by
    // [PROF]). Level geometry that is absent stays absent, so back off to 8s; any non-empty
    // result snaps back to 3s (was 1s: [PROF] showed 5 populated caches each doing a ~100ms full
    // walk every second = ~550ms/s. Only LIST DISCOVERY is expensive - positions are read from
    // cached pointers at 10Hz regardless - so a new ball joins its detector within 3s and is
    // then tracked at full rate). s_bo is read before the rebuild resets the count,
    // so it reflects the PREVIOUS scan's result.
    static ULONGLONG s_bo = 500;
    static long s_epoch = -1;
    // STAGGERED: re-arming all six caches to the same interval made a JOIN rebuild every
    // one of them in the same frame - six full 167k walks at once, on top of EnableGoals.
    // The user saw ping spike to ~1s exactly when a second client joined. Spread them out.
    if (s_epoch != g_cacheEpoch) { s_epoch = g_cacheEpoch; s_bo = 4200; }   // re-arm, widely staggered
    if (now - lastRebuild > s_bo)
    {
        s_bo = (gN == 0) ? ((s_bo < 8000) ? s_bo * 2 : 8000) : 3000;
        InterlockedIncrement(&g_rebuilds);   // [PROF] how often caches actually rebuild
        lastRebuild = now;
        gN = 0; bN = 0;
        const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (gN < 64 && o->IsA(goalCls))
            {
                const uintptr_t p = reinterpret_cast<uintptr_t>(o);
                if (*reinterpret_cast<int*>(p + 0x590) < 0)
                    continue;   // template / unassigned team only — cache ALL real goals regardless
                                // of enabled-state (a goal is briefly DISABLED right after a score;
                                // excluding it from the cache for up to 1s = "can't goal". We check
                                // bGoalEnabled LIVE at score time instead.)
                auto* col = *reinterpret_cast<SDK::USceneComponent**>(p + 0x5A0);
                if (!col) continue;
                FBoundsParams bp{}; bp.Component = col;
                if (!BoundsMemoGet(bp.Component, bp.Origin, bp.BoxExtent))   // [PERF] static geometry: measure once
                {
                    kslCDO->ProcessEvent(fnBounds, &bp);
                    BoundsMemoPut(bp.Component, bp.Origin, bp.BoxExtent);
                }
                gObj[gN] = o; gOrg[gN] = bp.Origin; gExt[gN] = bp.BoxExtent; ++gN;
            }
            else if (bN < 256 && o->IsA(ballCls))
            {
                bObj[bN++] = o;
            }
        }
        int gEnabled = 0;
        for (int g = 0; g < gN; ++g)
            if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(gObj[g]) + 0x5A9)) ++gEnabled;
        // Log only on change: HxLog is a file write, on the game thread.
        static int s_lgN = -1, s_lgE = -1, s_lbN = -1;
        if (gN != s_lgN || gEnabled != s_lgE || bN != s_lbN)
        {
            s_lgN = gN; s_lgE = gEnabled; s_lbN = bN;
            HxLog("[HalcyonA2][GOALDBG2] rebuilt cache: goals=%d enabled=%d balls=%d\n", gN, gEnabled, bN);
        }
    }
    if (gN == 0)
        return;

    for (int b = 0; b < bN; ++b)
    {
        auto* o = bObj[b];
        auto* a = static_cast<SDK::AActor*>(o);
        if (!a || !a->RootComponent)
            continue;
        SDK::FVector bp = a->RootComponent->K2_GetComponentLocation();
        int hit = -1;
        for (int g = 0; g < gN; ++g)
        {
            // goal must be ENABLED right now (checked live, not at cache time)
            if (*reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(gObj[g]) + 0x5A9) == 0) continue;
            // inside the goal's world AABB (+ margin)?
            if (fabs(bp.X - gOrg[g].X) > gExt[g].X + MARGIN) continue;
            if (fabs(bp.Y - gOrg[g].Y) > gExt[g].Y + MARGIN) continue;
            if (fabs(bp.Z - gOrg[g].Z) > gExt[g].Z + MARGIN) continue;
            hit = g; break;
        }
        void* ballKey = o;
        if (hit < 0) { ballInGoal.erase(ballKey); continue; }   // outside all goals -> re-arm this ball
        if (ballInGoal.count(ballKey)) continue;                // already scored on this entry
        ballInGoal.insert(ballKey);

        const int goalTeam    = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(gObj[hit]) + 0x590);
        const int scoringTeam = (goalTeam == 0) ? 1 : 0;        // ball in team T's net -> opponent scores
        const int scorer      = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(o) + 0x318);  // RollbackRecentPlayerHit
        ScoreGoal(gObj[hit], scoringTeam, scorer, o);   // pass the ball so OverlapBegin can build FX payload
        HxLog("[HalcyonA2][GOALHIT] %s in goal team=%d -> ScoringTeam=%d scorer(player)=%d\n",
              o->GetName().c_str(), goalTeam, scoringTeam, scorer);
    }
}
static void SafeDetectGoals() { __try { DetectGoals(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// GOLF SINK DETECTION (geometric, mirrors DetectGoals). The golf conductor (ServerCourseLogic.luau)
// RUNS and receives OnPlayerStartHole, but BallInCup never reaches it: the client's relay is gated on
// the native Cup.onBallInCup_Multicast, which never fires headless (the kinematically-replicated ball
// generates no server physics overlap in the cup's trigger volume). But the server DOES have the
// ball's transform (BP_GolfBallDisplay_C : BP_JakeBall_C -> RootComponent location updates; remotes see
// it move + sink). So detect the sink ourselves: ball RootComponent pos inside AGolfCup's
// GoalTriggerVolume(@0x2B8) AABB -> call the cup's reflected BallInCup(Score) -> broadcasts
// onBallInCup_Multicast -> client relays {BallInCup,...} -> conductor scores. Score (real stroke count)
// source is still unknown (native passes hardcoded 3; client-side counter) -> log candidate ball fields
// + fire a placeholder for now to PROVE the detect->fire->conductor pipeline; refine Score next.
static bool g_golfSinkDetect = true;
// Per-ball stroke tracking. NewHitEvent only fires on the FIRST hit server-side (confirmed: dribble
// ball works off one hit + a timer; stroke ball needs one per swing and freezes at 1; and the ball
// DOES move server-side — both VR + spectators see it). So count strokes ourselves: a golf stroke =
// the ball going from AT-REST -> suddenly MOVING (you only ever hit a stopped ball). Edge-detect that
// per ball at 10Hz off the same RootComponent position the sink detector reads.
struct GolfBallState {
    SDK::FVector last;         // motion-detect: last streamed pos
    bool         moving;
    int          strokes;
    ULONGLONG    seen;
    SDK::FVector lastHit;      // NewHitEvent: location of the last COUNTED strike
    bool         wasHit;       // NewHitEvent: first hit seen for this ball
    bool         hitDriven;    // NewHitEvent has fired for this ball -> motion counting backs off
};
static std::unordered_map<void*, GolfBallState> g_golfState;

// FName ComparisonIndex of "NewHitEvent", resolved once (0 = not yet resolved). Lets ProcessEvent_Hook
// recognize the event with a cheap integer compare instead of a per-call GetName() string alloc.
static int32_t g_nheIdx = 0;
static int32_t g_applyDataIdx = 0;  // [22284] FName idx of Server_ApplyData (spectator transform RPC) -> DROP in PE hook
static int32_t g_takeOwnIdx  = 0;   // [22284] FName idx of Server_AttemptTakeOwnershipOfSpectatorManager -> DROP (ROOT)

// [22284] QUEST/OCULUS-PLATFORM NEUTER. Only VR clients (not spectators) get an in-world kiosk
// initialized on join; that kiosk calls the OVRPlatform BP libraries (e.g. OvrRequestsBlueprintLibrary
// ::User_GetOrgScopedID -> the "quest kiosk org id" console line) which reach into the Oculus Platform
// SDK. On the headless Windows server there's no real Oculus runtime/entitlement, so the native
// platform call HARD-EXITS the process (no UE crash banner, no dump) the instant a VR player finishes
// joining. Drop every OVRPlatform BP-library call at ProcessEvent so the platform call never runs.
static bool         g_neuterQuestPlatform = false;  // [2026-09-01] user: re-enable kiosk/OVR init (test w/ cleared DB)
static const char*  kOvrLibNames[4] = { "OvrRequestsBlueprintLibrary", "OvrPageRequestsBlueprintLibrary",
                                        "OvrFunctionsBlueprintLibrary", "OvrPlatformUtilsLibrary" };
static SDK::UObject* g_ovrLibs[4] = { nullptr, nullptr, nullptr, nullptr };

// [22284] Per-player quest-progression seeding (InitPlayerQuests -> native register worker 0x4680E50
// with our hand-built FServerQuestProgression struct). This is THE crash when a VR player joins: the
// 22284 struct offsets differ from 20996, so feeding the native worker a malformed prog corrupts the
// heap -> the process fast-fails/exits with no dump (SEH can't catch a fast-fail). OFF until the 22284
// quest-progression offsets are re-verified; stability first (same call as ballsim-off). Quests just
// won't seed server-side (client quest UI stays uninit) — cosmetic vs. a crash.
static bool         g_questSeedEnabled = true;   // [2026-09-01] user: re-enable seeding after clearing all player_quests from the DB
// BP_GolfBallDisplay_C class (the stroke ball), resolved once. Used to guard the LastHitLocation poison
// so we only touch that field on balls that actually have it (dribble balls have a different layout).
static SDK::UClass* g_strokeBallCls = nullptr;
// BP_GolfBallDisplay_C property offsets (from the SDK dump):
//   StrokeCount       int32   @0x650 (Net, RepNotify -> OnRep_StrokeCount -> SetBallNumber)
//   LastHitLocation   FVector @0x658 (the anti-double-count anchor the game's gate measures against)
static constexpr uintptr_t GolfBall_LastHitLocation_Off = 0x658;

// The GAME's own StrokeCount++ lives in NewHitEvent, gated on
// Vector_Distance(GetActorLocation(), LastHitLocation) >= StrokeDistanceThreshold(100). Headless the
// ball actor's location is FROZEN, so that gate can never pass after the first hit -> count stuck at 1.
// NewHitEvent itself DOES fire per hit (server-auth, dispatched by FName via ProcessEvent), so we run
// the game's exact logic here but feed it the client-streamed position (g_ballStreamPos) instead of the
// frozen GetActorLocation(). This is authoritative per-hit — no motion/gap heuristics needed.
static void GolfHitEvent(SDK::UObject* ball)
{
    if (!ball) return;

    // Poison LastHitLocation to a far point so the GAME's own NewHitEvent gate
    // (Vector_Distance(GetActorLocation(), LastHitLocation) >= 100) PASSES — its GetActorLocation is
    // frozen headless, so without this the native StrokeCount++ never runs and the on-ball number
    // (SetBallNumber, driven by the replicated StrokeCount) stays stuck at 1. We run BEFORE the native
    // handler (ProcessEvent_Orig), so the game then increments StrokeCount itself and drives
    // SetBallNumber + push-replication + ReportStrokeCountToTee natively. Guarded to the stroke-ball
    // class so we never write 0x658 on a ball with a different layout. First hit (WasHit=false) skips
    // the gate anyway and overwrites LastHitLocation, so poisoning every time is harmless.
    if (g_strokeBallCls && ball->IsA(g_strokeBallCls))
    {
        SDK::FVector* lhl = reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(ball) + GolfBall_LastHitLocation_Off);
        lhl->X = 1.0e9; lhl->Y = 1.0e9; lhl->Z = 1.0e9;
    }

    auto sp = g_ballStreamPos.find(ball);
    SDK::FVector pos = (sp != g_ballStreamPos.end()) ? sp->second.pos : SDK::FVector{ 0, 0, 0 };
    GolfBallState& st = g_golfState[ball];   // default-constructs if absent
    st.hitDriven = true;                     // authoritative source is live -> motion path backs off
    if (!st.wasHit)
    {
        st.wasHit = true; st.lastHit = pos; st.strokes = 1;
        HxLog("[HalcyonA2][GOLFHIT] ball=%s FIRST stroke #1\n", ball->GetName().c_str());
        return;
    }
    const double dx = pos.X - st.lastHit.X, dy = pos.Y - st.lastHit.Y, dz = pos.Z - st.lastHit.Z;
    const double d = sqrt(dx*dx + dy*dy + dz*dz);
    if (d >= 100.0)   // StrokeDistanceThreshold — same anti-double-count gate the game uses
    {
        st.lastHit = pos; st.strokes++;
        HxLog("[HalcyonA2][GOLFHIT] ball=%s stroke #%d (dist=%.0f)\n", ball->GetName().c_str(), st.strokes, d);
    }
    else
        HxLog("[HalcyonA2][GOLFHIT] ball=%s ignored (dist=%.0f < 100)\n", ball->GetName().c_str(), d);
}
static void GolfSinkDetect()
{
    auto* cupCls  = SDK::UObject::FindClassFast("GolfCup");
    static SDK::UClass* ballCls = nullptr;   // [PERF] cached class lookup
    if (!ballCls) ballCls = SDK::UObject::FindClassFast("BP_JakeBall_C"); // golf balls are subclasses
    if (!cupCls || !ballCls) return;                                // not a golf level -> nothing to do
    static SDK::UObject* kslCDO = nullptr; static SDK::UFunction* fnBounds = nullptr;
    if (!kslCDO) kslCDO = static_cast<SDK::UObject*>(SDK::UKismetSystemLibrary::GetDefaultObj());
    if (!fnBounds && kslCDO) fnBounds = kslCDO->Class->GetFunction("KismetSystemLibrary", "GetComponentBounds");
    if (!kslCDO || !fnBounds) return;

    static std::unordered_set<void*> ballInCup;   // edge-trigger: score once per entry, re-arm on exit
    const double MARGIN = 30.0;

    static SDK::UObject* cObj[64]; static SDK::FVector cOrg[64]; static SDK::FVector cExt[64];
    static SDK::UObject* cComp[64]; static int cN = 0;   // cComp = the cup's GolfCupComponent (networked BallInCup)
    static SDK::UObject* bObj[256]; static int bNg = 0;
    static ULONGLONG lastRb = 0;
    const ULONGLONG now = GetTickCount64();
        // NOTE: this guard used to read "|| cN == 0", which meant that whenever the scan found
        // NOTHING it re-ran a FULL GObjects walk (a virtual IsA per object, over the entire
        // object array) EVERY TICK, forever - e.g. Station_Prime simply has no goals, so the
        // goal cache never populated and never stopped scanning. A fast desktop core absorbed
        // it; a VPS vCPU did not: it pegged the game thread (client ping ~1s, STEP calls/s 90->25).
        // Still retry while empty, just on a timer instead of every single tick.
    // [BACKOFF] A full GObjects walk costs ~50ms on the VPS core. A fixed 500ms empty-retry
    // meant 3 always-empty caches each scanned 2x/s = ~300ms/s = 30% of wall (measured by
    // [PROF]). Level geometry that is absent stays absent, so back off to 8s; any non-empty
    // result snaps back to 3s (was 1s: [PROF] showed 5 populated caches each doing a ~100ms full
    // walk every second = ~550ms/s. Only LIST DISCOVERY is expensive - positions are read from
    // cached pointers at 10Hz regardless - so a new ball joins its detector within 3s and is
    // then tracked at full rate). s_bo is read before the rebuild resets the count,
    // so it reflects the PREVIOUS scan's result.
    static ULONGLONG s_bo = 500;
    static long s_epoch = -1;
    // STAGGERED: re-arming all six caches to the same interval made a JOIN rebuild every
    // one of them in the same frame - six full 167k walks at once, on top of EnableGoals.
    // The user saw ping spike to ~1s exactly when a second client joined. Spread them out.
    if (s_epoch != g_cacheEpoch) { s_epoch = g_cacheEpoch; s_bo = 5400; }   // re-arm, widely staggered
    if (now - lastRb > s_bo)
    {
        s_bo = (cN == 0) ? ((s_bo < 8000) ? s_bo * 2 : 8000) : 3000;
        InterlockedIncrement(&g_rebuilds);   // [PROF] how often caches actually rebuild
        lastRb = now; cN = 0; bNg = 0;
        static SDK::UClass* compCls = nullptr;   // [PERF] cached class lookup
        if (!compCls) compCls = SDK::UObject::FindClassFast("GolfCupComponent");
        SDK::UObject* compTmp[128]; int nComp = 0;
        const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (cN < 64 && o->IsA(cupCls))
            {
                auto* vol = *reinterpret_cast<SDK::USceneComponent**>(reinterpret_cast<uintptr_t>(o) + 0x2B8); // GoalTriggerVolume
                if (!vol) continue;
                FBoundsParams bp{}; bp.Component = vol;
                if (!BoundsMemoGet(bp.Component, bp.Origin, bp.BoxExtent))   // [PERF] static geometry: measure once
                {
                    kslCDO->ProcessEvent(fnBounds, &bp);
                    BoundsMemoPut(bp.Component, bp.Origin, bp.BoxExtent);
                }
                cObj[cN] = o; cOrg[cN] = bp.Origin; cExt[cN] = bp.BoxExtent; cComp[cN] = nullptr; ++cN;
            }
            else if (compCls && nComp < 128 && o->IsA(compCls))
                compTmp[nComp++] = o;
            else if (bNg < 256 && o->IsA(ballCls))
                bObj[bNg++] = o;
        }
        // Match each cup to its GolfCupComponent by walking the component's Outer chain up to the cup —
        // that component's BallInCup PUBLISHES over the bridge (networked); the cup actor's doesn't.
        int matched = 0;
        for (int c = 0; c < cN; ++c)
            for (int k = 0; k < nComp; ++k)
            {
                SDK::UObject* ow = compTmp[k]->Outer; int guard = 0;
                while (ow && guard++ < 6) { if (ow == cObj[c]) break; ow = ow->Outer; }
                if (ow == cObj[c]) { cComp[c] = compTmp[k]; ++matched; break; }
            }
        // Log only on change: HxLog is a file write, on the game thread.
        static int s_lcN = -1, s_lmt = -1, s_lbg = -1;
        if (cN != s_lcN || matched != s_lmt || bNg != s_lbg)
        {
            s_lcN = cN; s_lmt = matched; s_lbg = bNg;
            HxLog("[HalcyonA2][GOLFSINK] cache: cups=%d comps=%d balls=%d\n", cN, matched, bNg);
        }

        // Resolve the "NewHitEvent" FName index once so ProcessEvent_Hook can count strokes off the
        // authoritative per-hit event (cheap integer compare, no per-call string work).
        if (!g_nheIdx && bNg > 0 && bObj[0]->Class)
        {
            SDK::UFunction* f = bObj[0]->Class->GetFunction("BP_GolfBallDisplay_C", "NewHitEvent");
            if (!f) f = bObj[0]->Class->GetFunction("BP_DribbleGolfBall_C", "NewHitEvent");
            if (!f) f = bObj[0]->Class->GetFunction("BP_JakeBall_C", "NewHitEvent");
            if (!f) f = bObj[0]->Class->GetFunction("DiscEntity", "NewHitEvent");
            if (f) { g_nheIdx = f->Name.ComparisonIndex; HxLog("[HalcyonA2][GOLFSINK] NewHitEvent FName idx=%d\n", g_nheIdx); }
        }
        if (!g_strokeBallCls)
            g_strokeBallCls = SDK::UObject::FindClassFast("BP_GolfBallDisplay_C");
    }
    if (cN == 0) return;

    const double MOVE_THRESH = 60.0;   // per-100ms displacement that counts as "hit into motion"
    const double REST_THRESH = 15.0;   // below this = at rest -> re-arm for the next stroke
    std::unordered_set<void*> live;

    for (int b = 0; b < bNg; ++b)
    {
        void* key = bObj[b];
        // Golf balls' RootComponent is frozen; use the client-streamed position instead. Only balls
        // being actively streamed (the player's live golf ball) have a recent entry -> this also
        // naturally filters the 71 cached balls down to the one in play.
        auto sp = g_ballStreamPos.find(key);
        if (sp == g_ballStreamPos.end() || (now - sp->second.seen) > 2000) continue;
        SDK::FVector p = sp->second.pos;
        live.insert(key);

        // --- stroke count ---
        // Primary source is the authoritative NewHitEvent (GolfHitEvent, driven from ProcessEvent_Hook).
        // The motion edge-detect below is a FALLBACK for balls that never see a NewHitEvent — once one
        // has (st.hitDriven), we stop motion-counting to avoid double-counting.
        auto it = g_golfState.find(key);
        if (it == g_golfState.end()) { g_golfState[key] = { p, false, 0, now, {0,0,0}, false, false }; continue; }   // baseline only
        GolfBallState& st = it->second;

        if (!st.hitDriven)
        {
            // Stream-gap re-arm: the client only streams the ball WHILE it's in motion, so a gap since we
            // last saw it means it came to REST between strokes. Re-arm + re-baseline without counting —
            // otherwise if the stream cuts out mid-decel (moving latched true), the next hit never counts.
            if (now - st.seen > 300)
            {
                st.moving = false; st.last = p; st.seen = now;
            }
            else
            {
                const double dx = p.X - st.last.X, dy = p.Y - st.last.Y, dz = p.Z - st.last.Z;
                const double disp = sqrt(dx*dx + dy*dy + dz*dz);
                if (!st.moving && disp > MOVE_THRESH)
                {
                    st.moving = true; st.strokes++;
                    HxLog("[HalcyonA2][GOLFSTROKE] ball=%s stroke #%d (disp=%.0f)\n", bObj[b]->GetName().c_str(), st.strokes, disp);
                }
                else if (disp < REST_THRESH) st.moving = false;   // rested -> arm for the next hit
                st.last = p; st.seen = now;
            }
        }
        else { st.last = p; st.seen = now; }   // keep motion baseline fresh but don't count

        // --- geometric cup overlap ---
        int hit = -1;
        for (int c = 0; c < cN; ++c)
        {
            if (fabs(p.X - cOrg[c].X) > cExt[c].X + MARGIN) continue;
            if (fabs(p.Y - cOrg[c].Y) > cExt[c].Y + MARGIN) continue;
            if (fabs(p.Z - cOrg[c].Z) > cExt[c].Z + MARGIN) continue;
            hit = c; break;
        }
        if (hit < 0) { ballInCup.erase(key); continue; }
        if (ballInCup.count(key)) continue;
        ballInCup.insert(key);

        const int score = st.strokes > 0 ? st.strokes : 1;   // our tracked stroke count
        auto* cup  = cObj[hit];
        auto* comp = cComp[hit];
        int firedWhat = 0;
        // The NETWORKED BallInCup lives on the GolfCupComponent (publishes over the UNetEventsBridge —
        // reaches clients + the ServerCourseLogic conductor). The cup actor's BallInCup is a local
        // multicast delegate that reaches nobody. Prefer the component; fall back to the actor.
        SDK::UFunction* fnComp = (g_golfSinkDetect && comp && comp->Class)
            ? comp->Class->GetFunction("GolfCupComponent", "BallInCup") : nullptr;
        if (fnComp)
        {
            struct { int32_t Score; int32_t _pad; void* Disc; } parm{ score, 0, bObj[b] };
            comp->ProcessEvent(fnComp, &parm);
            firedWhat = 2;
        }
        else
        {
            SDK::UFunction* fnBIC = (g_golfSinkDetect && cup->Class) ? cup->Class->GetFunction("GolfCup", "BallInCup") : nullptr;
            if (fnBIC) { struct { int32_t Score; } parm{ score }; cup->ProcessEvent(fnBIC, &parm); firedWhat = 1; }
        }
        HxLog("[HalcyonA2][GOLFSINK] SUNK ball=%s cup=%s comp=%p pos=(%.0f,%.0f,%.0f) strokes=%d fired=%d\n",
              bObj[b]->GetName().c_str(), cup->GetName().c_str(), (void*)comp, p.X, p.Y, p.Z, score, firedWhat);
    }

    // drop tracking for balls that are gone (GC / new hole) so counts don't leak across balls
    if (g_golfState.size() > live.size())
        for (auto it = g_golfState.begin(); it != g_golfState.end(); )
            (live.count(it->first)) ? (void)++it : (void)(it = g_golfState.erase(it));
}
static void SafeGolfSinkDetect() { __try { GolfSinkDetect(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Volleyfall "spleef" floor. Each tile is an ABP_FloorPanelB_LE_C. Its
// ReceiveActorBeginOverlap(disc) destroys the tile: Cast<DiscEntity>(OtherActor),
// 0.2s delay, then Mesh collision off + Mesh hidden + IsActive=false (Net, RepIndex
// 10, @0x2C0) MarkPropertyDirty + tileDestroyed.Broadcast(). ResetPanel restores it.
// That overlap is a physics touch between the disc (ball) and the panel, which never
// fires on our headless server (same reason jakeball goals / the golf cup don't).
// So detect the ball-in-panel overlap GEOMETRICALLY (exactly like DetectGoals: read the
// jakeball RootComponent world location vs each panel's cached AABB) and call the game's
// OWN ReceiveActorBeginOverlap with the real disc so its authentic destroy path (incl.
// the IsActive replication clients watch) runs. Volleyfall balls are jakeballs
// (JakeBallSpawner), whose RootComponent is live server-side (golf balls' isn't).
// ---------------------------------------------------------------------------
static bool g_volleyfall = true;
static void VolleyfallTick()
{
    if (!g_volleyfall) return;
    static SDK::UClass* panelCls = nullptr;   // [PERF] cached class lookup
    if (!panelCls) panelCls = SDK::UObject::FindClassFast("BP_FloorPanelB_LE_C");
    if (!panelCls) return;   // not a volleyfall level -> nothing to do
    auto* ballCls  = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!ballCls) return;

    static SDK::UObject* kslCDO = nullptr; static SDK::UFunction* fnBounds = nullptr;
    if (!kslCDO) kslCDO = static_cast<SDK::UObject*>(SDK::UKismetSystemLibrary::GetDefaultObj());
    if (!fnBounds && kslCDO) fnBounds = kslCDO->Class->GetFunction("KismetSystemLibrary", "GetComponentBounds");
    if (!kslCDO || !fnBounds) return;

    static SDK::UClass* spawnCls = nullptr;   // [PERF] cached class lookup
    if (!spawnCls) spawnCls = SDK::UObject::FindClassFast("BallSpawnerComponent");

    constexpr uintptr_t Panel_IsActive_Off = 0x2C0;   // bool IsActive (Net, RepIndex 10)
    constexpr uintptr_t Panel_Mesh_Off     = 0x2E0;   // UInstancedStaticMeshComponent* Mesh
    constexpr uintptr_t Spawner_Disc_Off   = 0x580;   // ADiscEntity* Disc (the spawner's current ball)
    const double MARGIN = 40.0;
    const double DROP   = 120.0;   // ball this far below the floor plane = fell through -> reset

    static SDK::UObject* pObj[128]; static SDK::FVector pOrg[128]; static SDK::FVector pExt[128]; static int pN = 0;
    static SDK::UObject* bObj[128]; static int bN = 0;
    static SDK::UObject* sObj[16]; static int sN = 0;   // BallSpawnerComponents
    static SDK::UFunction* fnOverlap = nullptr;
    static SDK::UFunction* fnReset = nullptr;           // BP_FloorPanelB_LE_C::ResetPanel
    static SDK::UFunction* fnResetBall = nullptr;       // BallSpawnerComponent::ResetBall
    static double floorZ = 0.0;
    static ULONGLONG lastRb = 0;
    const ULONGLONG now = GetTickCount64();
        // NOTE: this guard used to read "|| pN == 0", which meant that whenever the scan found
        // NOTHING it re-ran a FULL GObjects walk (a virtual IsA per object, over the entire
        // object array) EVERY TICK, forever - e.g. Station_Prime simply has no goals, so the
        // goal cache never populated and never stopped scanning. A fast desktop core absorbed
        // it; a VPS vCPU did not: it pegged the game thread (client ping ~1s, STEP calls/s 90->25).
        // Still retry while empty, just on a timer instead of every single tick.
    // [BACKOFF] A full GObjects walk costs ~50ms on the VPS core. A fixed 500ms empty-retry
    // meant 3 always-empty caches each scanned 2x/s = ~300ms/s = 30% of wall (measured by
    // [PROF]). Level geometry that is absent stays absent, so back off to 8s; any non-empty
    // result snaps back to 3s (was 1s: [PROF] showed 5 populated caches each doing a ~100ms full
    // walk every second = ~550ms/s. Only LIST DISCOVERY is expensive - positions are read from
    // cached pointers at 10Hz regardless - so a new ball joins its detector within 3s and is
    // then tracked at full rate). s_bo is read before the rebuild resets the count,
    // so it reflects the PREVIOUS scan's result.
    static ULONGLONG s_bo = 500;
    static long s_epoch = -1;
    // STAGGERED: re-arming all six caches to the same interval made a JOIN rebuild every
    // one of them in the same frame - six full 167k walks at once, on top of EnableGoals.
    // The user saw ping spike to ~1s exactly when a second client joined. Spread them out.
    if (s_epoch != g_cacheEpoch) { s_epoch = g_cacheEpoch; s_bo = 6600; }   // re-arm, widely staggered
    if (now - lastRb > s_bo)
    {
        s_bo = (pN == 0) ? ((s_bo < 8000) ? s_bo * 2 : 8000) : 3000;
        InterlockedIncrement(&g_rebuilds);   // [PROF] how often caches actually rebuild
        lastRb = now; pN = 0; bN = 0; sN = 0;
        double zSum = 0.0;
        const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject()) continue;
            if (pN < 128 && o->IsA(panelCls))
            {
                auto* mesh = *reinterpret_cast<SDK::USceneComponent**>(reinterpret_cast<uintptr_t>(o) + Panel_Mesh_Off);
                if (!mesh) continue;
                FBoundsParams bp{}; bp.Component = mesh;
                if (!BoundsMemoGet(bp.Component, bp.Origin, bp.BoxExtent))   // [PERF] static geometry: measure once
                {
                    kslCDO->ProcessEvent(fnBounds, &bp);
                    BoundsMemoPut(bp.Component, bp.Origin, bp.BoxExtent);
                }
                pObj[pN] = o; pOrg[pN] = bp.Origin; pExt[pN] = bp.BoxExtent; zSum += bp.Origin.Z; ++pN;
            }
            else if (bN < 128 && o->IsA(ballCls))
                bObj[bN++] = o;
            else if (spawnCls && sN < 16 && o->IsA(spawnCls))
                sObj[sN++] = o;
        }
        if (pN > 0) floorZ = zSum / pN;
        if (!fnOverlap && pN > 0 && pObj[0]->Class)
            fnOverlap = pObj[0]->Class->GetFunction("BP_FloorPanelB_LE_C", "ReceiveActorBeginOverlap");
        if (!fnReset && pN > 0 && pObj[0]->Class)
            fnReset = pObj[0]->Class->GetFunction("BP_FloorPanelB_LE_C", "ResetPanel");
        if (!fnResetBall && sN > 0 && sObj[0]->Class)
            fnResetBall = sObj[0]->Class->GetFunction("BallSpawnerComponent", "ResetBall");
        if (pN > 0)
            HxLog("[HalcyonA2][VOLLEY] cache: panels=%d balls=%d spawners=%d floorZ=%.0f fnOverlap=%d fnReset=%d fnResetBall=%d\n",
                  pN, bN, sN, floorZ, fnOverlap ? 1 : 0, fnReset ? 1 : 0, fnResetBall ? 1 : 0);
    }
    if (pN == 0 || !fnOverlap) return;

    // Latch: don't re-fire a panel's destroy during its 0.2s delay window (IsActive is still true
    // until the delay resolves). Once IsActive flips false (destroyed) or true again (reset) the
    // active-check below re-arms it.
    static std::unordered_set<void*> pending;

    for (int b = 0; b < bN; ++b)
    {
        auto* a = static_cast<SDK::AActor*>(bObj[b]);
        if (!a || !a->RootComponent) continue;
        SDK::FVector p = a->RootComponent->K2_GetComponentLocation();
        for (int c = 0; c < pN; ++c)
        {
            auto* panel = pObj[c];
            const bool active = *reinterpret_cast<bool*>(reinterpret_cast<uintptr_t>(panel) + Panel_IsActive_Off);
            if (!active) { pending.erase(panel); continue; }   // broken (or mid-reset) -> nothing to do / re-arm
            if (pending.count(panel)) continue;                // destroy already in flight (0.2s delay)
            if (fabs(p.X - pOrg[c].X) > pExt[c].X + MARGIN) continue;
            if (fabs(p.Y - pOrg[c].Y) > pExt[c].Y + MARGIN) continue;
            if (fabs(p.Z - pOrg[c].Z) > pExt[c].Z + MARGIN) continue;
            // ball is on this active tile -> run the game's own overlap/destroy with the real disc
            struct { SDK::AActor* OtherActor; } parm{ a };
            panel->ProcessEvent(fnOverlap, &parm);
            pending.insert(panel);
            HxLog("[HalcyonA2][VOLLEY] break panel=%s (ball=%s) pos=(%.0f,%.0f,%.0f)\n",
                  panel->GetName().c_str(), bObj[b]->GetName().c_str(), p.X, p.Y, p.Z);
        }

        // --- reset: ball fell through the floor (below the panel plane) ---
        // The designed reset (Red Reset Trigger OnOverlapByDiscSimple -> ResetPanel on every tile +
        // BallReseter.luau respawning the ball) fires from the same server-side disc overlap that
        // never happens headless. Detect the fall geometrically and drive it: restore all panels +
        // reset the ball via its spawner. Latched per-ball so it fires once per fall.
        static std::unordered_set<void*> fell;
        if (p.Z < floorZ - DROP)
        {
            if (!fell.count(bObj[b]))
            {
                fell.insert(bObj[b]);
                if (fnReset)
                    for (int c = 0; c < pN; ++c) pObj[c]->ProcessEvent(fnReset, nullptr);   // restore floor
                // reset the ball via the spawner that owns it (match by Disc@0x580; else first spawner)
                SDK::UObject* spawner = nullptr;
                for (int s = 0; s < sN; ++s)
                    if (*reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(sObj[s]) + Spawner_Disc_Off) == bObj[b]) { spawner = sObj[s]; break; }
                if (!spawner && sN > 0) spawner = sObj[0];
                if (spawner && fnResetBall) spawner->ProcessEvent(fnResetBall, nullptr);
                HxLog("[HalcyonA2][VOLLEY] RESET (ball=%s fell to z=%.0f, floorZ=%.0f) panels=%d spawner=%p\n",
                      bObj[b]->GetName().c_str(), p.Z, floorZ, pN, (void*)spawner);
            }
        }
        else fell.erase(bObj[b]);   // back above the floor -> re-arm
    }
}
static void SafeVolleyfallTick() { __try { VolleyfallTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Server dashboard AUTH — seed the api-key global, then run the native server login.
// The server login (sub_54AB570 -> sub_5339790 -> POST /users/log_in_with_key) sends
// x-api-key = qword_9BD4460, which is EMPTY on our hacked boot (nothing seeds it from the
// -DashboardApiKey cmdline), so the login carries NO key -> "No dashboard api key" forever and the
// authenticated chain (register + /v1/deployments station-config fetch -> boards) never proceeds.
// Cold-start chicken-egg: the login reads the token global, but the token global is only filled by
// a successful login (sub_5425100 stores the returned api_key there). So we seed it ourselves.
// The seed MUST be game-allocated: sub_5425100 later overwrites qword_9BD4460 via sub_FC0890 (which
// frees the old buffer), so a static literal would be freed and crash -> copy via sub_FBCD70 from a
// static view. (This build uses log_in_with_key; the /users/log_in_server route is in the backend
// openapi but that string isn't in the binary, so it's unused.) FString layout at 0x9BD4460:
// Data@0x9BD4460, Num@0x9BD4468, Max@0x9BD446C.
static const wchar_t kDashApiKey[] = L"halcyon-server-key";
using FStringAssign2Fn = void(__fastcall*)(void* dest, const void* src);   // sub_FBCD70 (FString copy)
using SessionLoginFn   = __int64(__fastcall*)(void* sessionSubsystem);     // sub_54AB570 (server login)
static bool      g_dashKeySeeded = false;
static void SeedDashboardApiKey()
{
    // [2026-09-03 ★ CRASH FIX] DISABLED — this whole seed path is built on STALE old-build RVAs. Our
    // crash reporter caught it: base+0xFBCD70 (the intended FString-copy) is now MID-WAY into an unrelated
    // function (sub_7FF67301CD50, the mesh-type array filter) on this build. Calling it jumps past the
    // prologue -> corrupts the stack frame -> write-AV to 0x...cd70, and the frame corruption defeats even
    // the outer SafeTriggerServerDashboardLogin SEH (unhandled crash on server boot). The api-key global
    // (0x9BD4460/68) and the login fn (0x54B5660) are equally stale. Per this path's own note it is
    // "belt-and-suspenders only — the native login already authenticates off the -DashboardApiKey cmdline",
    // so skipping it is safe and stops the crash. TODO to re-enable: re-find (a) the FString-assign RVA,
    // (b) the api-key FString global, (c) the session-login fn for this build, then restore the seed.
    g_dashKeySeeded = true;
    static bool s_once = false;
    if (!s_once) { s_once = true; printf("[HalcyonA2] dashboard api-key seed SKIPPED (stale RVAs; native cmdline key used) — crash-fix\n"); }
}
static void SafeSeedDashboardApiKey() { __try { SeedDashboardApiKey(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static bool      g_dashLoginDone = false;
static ULONGLONG g_lastDashLogin = 0;
static int       g_dashLoginTries = 0;
static void TriggerServerDashboardLogin()
{
    SeedDashboardApiKey();   // ensure the key global is populated before we log in
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("A2SessionSubsystem");
    if (!cls) return;
    SDK::UObject* ss = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(cls)) { ss = o; break; }
    }
    if (!ss) { printf("[HalcyonA2] server-login: A2SessionSubsystem not live yet\n"); return; }
    printf("[HalcyonA2] server-login: sub_54AB570(session=%p) with seeded key\n", ss);
    g_dashLoginDone = true;   // latch BEFORE the call: sub_54AB570 on this instance faults in our context
                              // (SEH-swallowed) so it never reached this line -> endless 3s retries. The
                              // native sub_540CC30 login already authenticates off our seeded key (log
                              // shows the token), so our explicit call is belt-and-suspenders only.
    reinterpret_cast<SessionLoginFn>(GetBase() + 0x54B5660)(ss);   // native login -> POST log_in_with_key (x-api-key seeded)
}
static void SafeTriggerServerDashboardLogin() { __try { TriggerServerDashboardLogin(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Station dashboard FULL NATIVE INIT. Root cause of everything dashboard-side being dead (no
// server_events, no periodic deployment fetch, no server_launched): sub_541D8A0 is the init that
// registers ALL the reporting delegates (heartbeat sub_54480B0 @client+184, server_events, config
// fetch, etc.) and fires "server_launched" -- but at boot it BAILED at its netmode gate
// (sub_4037D80/GetNetMode != 2) before registering anything, only running its first line
// `qword_9BD44F0 = a1`. Now that gate passes (we hook GetNetMode -> 1 and patched the cmp byte 2->1),
// we call it ourselves post-injection to run the whole init: it re-reads the -Dashboard* cmdline
// args, registers every reporting delegate, and fires server_launched -- the native cascade. One
// shot (it unregisters existing delegates at the slots before re-adding, so a single call is enough;
// don't loop it). client = qword_9BD44F0 (set at boot on sub_541D8A0's first line).
// Full native init sub_541D8A0(client): registers all reporting delegates + fires server_launched.
// It bailed with nothing registered when we called it. Gate B (netmode) is confirmed satisfied
// (GetNetMode hooked ->1, cmp byte patched 2->1), so it's gate A `(*(clientVtable+392))() == 0`
// (client's world/context null post-travel) OR a fault mid-registration. DIAGNOSE FIRST: replicate
// both gate checks and log them, so the next run pinpoints the failure instead of us guessing.
// vtable slot for gate A = 392/8 = 49.  GetNetMode = sub_4037D80 (RVA 0x4040990).
using CtxVirtFn       = void*(__fastcall*)(void*);
using GetNetModeFn    = int(__fastcall*)(void*);
using DashboardInitFn = __int64(__fastcall*)(void* dashboardClient);   // sub_541D8A0 (full init)
static bool      g_deployFetchDone = false;
static ULONGLONG g_lastDeployFetch = 0;
static int       g_deployFetchTries = 0;
static void TriggerDeploymentFetch()
{
    uint64_t base = GetBase();
    void* client = *reinterpret_cast<void**>(base + 0x9C5D0A0);   // qword_9BD44F0 (dashboard client)
    if (!client) { printf("[HalcyonA2] dashboard-init: client (qword_9BD44F0) null, waiting\n"); return; }

    // Diagnose sub_541D8A0's two early gates before calling it.
    void** vtbl = *reinterpret_cast<void***>(client);
    void*  ctx  = reinterpret_cast<CtxVirtFn>(vtbl[49])(client);                                   // gate A: (*(v1+392))()
    int    nm   = ctx ? reinterpret_cast<GetNetModeFn>(base + 0x4040990)(ctx) : -999;              // gate B: sub_4037D80(ctx)
    printf("[HalcyonA2] dashboard-init: gateA ctx=%p  gateB netmode=%d (need ==1)\n", ctx, nm);

    printf("[HalcyonA2] dashboard-init: sub_541D8A0(client=%p) -> full native init\n", client);
    reinterpret_cast<DashboardInitFn>(base + 0x54B13D0)(client);
    g_deployFetchDone = true;
}
static void SafeTriggerDeploymentFetch() { __try { TriggerDeploymentFetch(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Diagnostic: enumerate live module (gamemode) slots + confirm UGamemodesManager. A gamemode loads
// via a module slot whose DefaultGamemodePath (e.g. "deathrun", "arena_jakeball") resolves through
// ProjectMapping.locationsToProject -> ProjectID -> baked LevelDefinitions/{name}_a2level project
// (level.json objects + gamemode.luau). This dump tells us which slots exist, their gamemode path,
// and whether they're loaded (LoadedGameMode!=null) -- the template for spawning a "deathrun" slot.
// AModuleSlot offsets: bShouldLoadOnStart@0x2F8, SlotID@0x390(FString), VisibleName@0x3A0,
// DefaultGamemodePath@0x418(FString), LoadedGameMode@0x440(null=unloaded).
static bool      g_slotDumpDone = false;
static ULONGLONG g_lastSlotDump = 0;
static int       g_slotDumpTries = 0;
static void DumpModuleSlots()
{
    auto rdFStr = [](uintptr_t a) -> const wchar_t* { auto d = *reinterpret_cast<const wchar_t**>(a); return d ? d : L""; };
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("ModuleSlot");
    if (!cls) { printf("[HalcyonA2][SLOTS] ModuleSlot class not found yet\n"); return; }
    static SDK::UClass* gmCls = nullptr;   // [PERF] cached class lookup
    if (!gmCls) gmCls = SDK::UObject::FindClassFast("GamemodesManager");
    void* gmMgr = nullptr;
    int count = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        if (gmCls && !gmMgr && o->IsA(gmCls)) gmMgr = o;
        if (!o->IsA(cls)) continue;
        uintptr_t p = reinterpret_cast<uintptr_t>(o);
        double loc[3] = {};   // K2_GetActorLocation -> FVector (world position of the slot)
        if (auto* gl = o->Class ? o->Class->GetFunction("Actor", "K2_GetActorLocation") : nullptr)
            SafeProcessEvent(o, gl, loc);
        printf("[HalcyonA2][SLOTS] %s (%s) SlotID='%ls' Path='%ls' loaded=%p pos=(%.0f, %.0f, %.0f)\n",
               o->GetName().c_str(),
               o->Class ? o->Class->GetName().c_str() : "?",
               rdFStr(p + 0x390), rdFStr(p + 0x418),
               *reinterpret_cast<void**>(p + 0x440),
               loc[0], loc[1], loc[2]);
        ++count;
    }
    printf("[HalcyonA2][SLOTS] total module slots = %d ; GamemodesManager = %p\n", count, gmMgr);
}
static void SafeDumpModuleSlots() { __try { DumpModuleSlots(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Find the Scraprun module-slot marker(s) at runtime and print their WORLD position. The scraprun
// slot was "removed for perf" leaving a bare AActor marker ("ModuleSlotLocationBeforeItWasRemoved-
// ForPerf") in the streamed PKR_Scraprun level; its engine-computed world transform is the target
// for -GamemodePos (no fragile nested-transform math). Match bare AActor (exact class "Actor") whose
// full name contains "Scraprun".
static void DumpScraprunMarkers()
{
    int n = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num && n < 40; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->Class) continue;
        if (o->Class->GetName() != "Actor") continue;          // bare AActor only
        std::string fn = o->GetFullName();
        if (fn.find("Scraprun") == std::string::npos && fn.find("scraprun") == std::string::npos) continue;
        double loc[3] = {};
        if (auto* gl = o->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(o, gl, loc);
        printf("[HalcyonA2][MARKER] %s pos=(%.0f, %.0f, %.0f)\n", fn.c_str(), loc[0], loc[1], loc[2]);
        ++n;
    }
    printf("[HalcyonA2][MARKER] bare-Actor-in-Scraprun count=%d\n", n);
}
static void SafeDumpScraprunMarkers() { __try { DumpScraprunMarkers(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Ground-truth: after a deathrun load, dump the WORLD positions of the actors it spawned, so we can
// see EXACTLY where the coord rewrite landed the course (vs. the corridor marker -33850,11775,4000).
// PRECISE isolation: g_gmObjBase is GObjects->Num() captured just before AddSlot, so any object at an
// index >= that is one this load created — no keyword guessing, no other gamemodes' actors.
static int32_t g_gmObjBase = 0;
static void DumpDeathrunActors()
{
    if (!g_diag) return;   // [PERF] diagnostic-only; see g_diag
    // deathrun2-UNIQUE class tokens (none appear in golf/jakeball/tackleball). Index-range alone is
    // useless here — the whole world (~28k objects) streams in after AddSlot.
    static const char* kTok[] = { "BP_Trap_", "Deathrun", "GameStateManager", "ScoreboardA", "Cube_Shield" };
    int n = 0, actors = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    printf("[HalcyonA2][DRPOS] scanning %d objects for deathrun2 actors (corridor marker = -33850,11775,4000)\n", num);
    for (int32_t i = 0; i < num && n < 40; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->Class) continue;
        std::string cn = o->Class->GetName();
        bool hit = false; for (auto* t : kTok) if (cn.find(t) != std::string::npos) { hit = true; break; }
        if (!hit) continue;
        auto* locFn = o->Class->GetFunction("Actor", "K2_GetActorLocation");
        if (!locFn) continue;
        ++actors;
        double loc[3] = {};
        SafeProcessEvent(o, locFn, loc);
        printf("[HalcyonA2][DRPOS] %s (%s) pos=(%.0f, %.0f, %.0f)\n",
               o->GetName().c_str(), cn.c_str(), loc[0], loc[1], loc[2]);
        ++n;
    }
    printf("[HalcyonA2][DRPOS] deathrun actor total=%d (shown %d) | corridor marker = -33850,11775,4000\n", actors, n);
}
static void SafeDumpDeathrunActors() { __try { DumpDeathrunActors(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---------------------------------------------------------------------------
// Load a gamemode into a module slot (OPT-IN via env GS_LOAD_GAMEMODE, e.g. "deathrun";
// GS_GAMEMODE_SLOT selects the target slot, default "PKR_Custom_Full" = the Parkour custom slot).
// UGamemodesManager::AddSlot(mgr, slot) async-loads the slot's DefaultGamemodePath when it's set
// (slot+0x418.Num > 1) and we're the server (GetNetMode != NM_Client): it kicks sub_46A7750 which
// resolves the path via ProjectMapping -> loads LevelDefinitions/{name}_a2level (level.json objects
// + gamemode.luau) -> binds to the slot -> replicates. The empty custom/UGC slots (PKR_Custom_Full,
// Station_Full_1, ...) are registered at boot with an EMPTY path so nothing loads; we set the path
// then re-call AddSlot to trigger the load. AModuleSlot: SlotID@0x390, DefaultGamemodePath@0x418.
static char      g_loadGmPath[64] = {};
static char      g_loadGmSlot[64] = "PKR_Custom_Full";
static bool      g_gmPosSet = false;
static double    g_gmPos[3] = {};   // -GamemodePos=X,Y,Z : world position to move the slot to before load
static char      g_snapMarker[96] = {};   // -SnapToMarker=<fullname substr> : copy that actor's full transform to the slot
static bool      g_gmLoadDone = false;
static ULONGLONG g_lastGmLoad = 0;
static int       g_gmLoadTries = 0;
static void SetSlotFString(void* slotObj, uint32_t off, const wchar_t* val)
{
    // [2026-09-03 ★ CRASH FIX] base+0xFBCD70 is a STALE RVA (now mid-way into sub_7FF67301CD50) — calling it
    // corrupts the stack and crashes (see SeedDashboardApiKey). Fall back to the manual set (point Data at
    // the literal + set Num/Max), matching SetFStringMember. Safe as long as `val` is a persistent buffer
    // (the gamemode-path callers pass a static/long-lived wpath). If the game later frees this FString the
    // literal-free could fault — but that's the -LoadGamemode path only, and not crashing on the SET is the
    // priority. Re-find the FString-assign RVA to restore a true game-owned copy.
    int wl = static_cast<int>(wcslen(val)) + 1;
    const uintptr_t p = reinterpret_cast<uintptr_t>(slotObj) + off;
    *reinterpret_cast<const wchar_t**>(p) = val;   // Data
    *reinterpret_cast<int32_t*>(p + 8)    = wl;    // ArrayNum
    *reinterpret_cast<int32_t*>(p + 12)   = wl;    // ArrayMax
}

// ---------------------------------------------------------------------------
// PLACEMENT via the object-builder hook (the proper fix — see below).
//
// A loaded gamemode's placed prefabs store their transform as LOCAL (slot-space) coords in a
// REPLICATED netvar: sub_46D77A0(RVA 0x46D77A0) is the per-ObjectPrefab netvar builder, called once
// per object during the async load with a3 = the deserialized prefab. It copies a3's transform
// VERBATIM into UVector/URotator netvars (position@a3+48 = FVector f64 x/y/z@48/56/64; rotation@a3+72
// = FRotator f64 pitch/yaw/roll@72/80/88 — NO slot transform applied server-side). The CLIENT applies
// the slot's world transform when it spawns each prefab, i.e. worldPos = slotWorld o localPos. So to
// relocate the whole course into the Scraprun corridor without moving the (Static, non-replicating)
// slot, we pre-multiply every object's local transform by a constant delta D = slotWorld^-1 o marker-
// World before it's copied into the replicated netvar. Then the client computes
// slotWorld o (D o local) = markerWorld o local  ->  course lands at the marker. Nothing to move,
// no NetVarSystem wiring, and the rewritten value replicates exactly like the original did.
//
// D is captured in LoadGamemodeIntoSlot() (from the target slot's + marker's live transforms) and the
// hook applies it while g_objRewrite is set. Scale is assumed 1 on both slot and marker (ignored).
struct HQuat { double x, y, z, w; };
struct HVec  { double x, y, z; };
static bool   g_objRewrite = false;   // set true just before AddSlot; gates the sub_46D77A0 rewrite
static bool   g_rewriteRot = false;   // also rewrite rotation (full-transform mode via -SnapToMarker)
static HQuat  g_dRot   = { 0,0,0,1 }; // per-object rotation delta B, as quat (for object spin rewrite)
static HVec   g_dTrans = { 0,0,0 };   // per-object translation C
static int    g_objRewriteCount = 0;
static bool   g_useMatPos = false;    // position via matrix g_Bmat (bypass quat round-trip)
static double g_Bmat[3][3] = { {1,0,0},{0,1,0},{0,0,1} };   // per-object position rotation (B)
static double g_gmNudge[3] = {};      // -GmNudge=X,Y,Z : world-space nudge of the authored origin
static bool   g_forceRot = false;     // -GmRewriteRot : also rewrite object rotation (couples into pos)

// PKR_Custom_Full's client-map intercept T1: the client places a loaded object at world = A*fed + T1,
// where fed is the value we write into the position netvar (a3+48) and A = UERot(pkrActorRot)^T. T1 was
// measured by regressing 40 deathrun2 actor positions (residual < 10u) from a known-fed run. A fixed
// slot property. Placement: worldWant(local) = Rs*local + P (Rs = marker applied rot, P = marker loc +
// nudge); feed fed = B*local + C, B = A^-1*Rs, C = A^-1*(P - T1). (Earlier bug: used the raw-local
// intercept Tp = T1 + A*Dold here, which put C off by Dold -> course in space on the wrong side.)
static const HVec kPkrTp = { -33590.0, 23550.0, 40314.0 };   // = T1 (fed-intercept), NOT Tp

static HQuat HRotToQuat(double pitch, double yaw, double roll)   // UE FRotator::Quaternion()
{
    const double H = 3.14159265358979323846 / 360.0;   // deg -> half-radian
    double SP = sin(pitch*H), CP = cos(pitch*H), SY = sin(yaw*H), CY = cos(yaw*H), SR = sin(roll*H), CR = cos(roll*H);
    HQuat q;
    q.x = CR*SP*SY - SR*CP*CY;
    q.y = -CR*SP*CY - SR*CP*SY;
    q.z = CR*CP*SY - SR*SP*CY;
    q.w = CR*CP*CY + SR*SP*SY;
    return q;
}
static void HQuatToRot(HQuat q, double& pitch, double& yaw, double& roll)   // UE FQuat::Rotator()
{
    const double RAD2DEG = 180.0 / 3.14159265358979323846;
    const double kSingThresh = 0.4999995;
    double sing = q.z*q.x - q.w*q.y;
    double yawY = 2.0*(q.w*q.z + q.x*q.y);
    double yawX = 1.0 - 2.0*(q.y*q.y + q.z*q.z);
    if (sing < -kSingThresh)      { pitch = -90.0; yaw = atan2(yawY, yawX)*RAD2DEG; roll = -yaw - 2.0*atan2(q.x, q.w)*RAD2DEG; }
    else if (sing > kSingThresh)  { pitch =  90.0; yaw = atan2(yawY, yawX)*RAD2DEG; roll =  yaw - 2.0*atan2(q.x, q.w)*RAD2DEG; }
    else                   { pitch = asin(2.0*sing)*RAD2DEG; yaw = atan2(yawY, yawX)*RAD2DEG;
                             roll = atan2(-2.0*(q.w*q.x + q.y*q.z), 1.0 - 2.0*(q.x*q.x + q.y*q.y))*RAD2DEG; }
}
static HQuat HQuatMul(HQuat a, HQuat b)   // a * b (UE order)
{
    return {
        a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y,
        a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x,
        a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w,
        a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z };
}
static HQuat HQuatConj(HQuat q) { return { -q.x, -q.y, -q.z, q.w }; }
static HVec  HQuatRotate(HQuat q, HVec v)   // v' = q * v * q^-1  (UE FQuat::RotateVector)
{
    HVec u = { q.x, q.y, q.z };
    HVec t = { 2.0*(u.y*v.z - u.z*v.y), 2.0*(u.z*v.x - u.x*v.z), 2.0*(u.x*v.y - u.y*v.x) };   // 2*cross(u,v)
    return { v.x + q.w*t.x + (u.y*t.z - u.z*t.y),
             v.y + q.w*t.y + (u.z*t.x - u.x*t.z),
             v.z + q.w*t.z + (u.x*t.y - u.y*t.x) };
}

// --- 3x3 matrix helpers (mirror solve2.ps1 exactly; quaternion path had a handedness bug) ---
struct HMat { double m[3][3]; };
static HMat HUERot(double P, double Y, double R)   // UE FRotationMatrix (row-vector convention), degrees
{
    const double d = 3.14159265358979323846 / 180.0;
    double sp=sin(P*d),cp=cos(P*d),sy=sin(Y*d),cy=cos(Y*d),sr=sin(R*d),cr=cos(R*d);
    HMat o;
    o.m[0][0]=cp*cy;            o.m[0][1]=cp*sy;            o.m[0][2]=sp;
    o.m[1][0]=sr*sp*cy-cr*sy;   o.m[1][1]=sr*sp*sy+cr*cy;   o.m[1][2]=-sr*cp;
    o.m[2][0]=-(cr*sp*cy+sr*sy);o.m[2][1]=cy*sr-cr*sp*sy;   o.m[2][2]=cr*cp;
    return o;
}
static HMat HTranspose(HMat a){ HMat o; for(int i=0;i<3;i++)for(int j=0;j<3;j++)o.m[i][j]=a.m[j][i]; return o; }
static HMat HMatMul(HMat a, HMat b){ HMat o; for(int i=0;i<3;i++)for(int j=0;j<3;j++){double s=0;for(int k=0;k<3;k++)s+=a.m[i][k]*b.m[k][j];o.m[i][j]=s;} return o; }
static HVec HMatVec(HMat a, HVec v){ return { a.m[0][0]*v.x+a.m[0][1]*v.y+a.m[0][2]*v.z,
                                              a.m[1][0]*v.x+a.m[1][1]*v.y+a.m[1][2]*v.z,
                                              a.m[2][0]*v.x+a.m[2][1]*v.y+a.m[2][2]*v.z }; }
static HQuat HMatToQuat(HMat mm)   // column-vector rotation matrix -> quat (x,y,z,w)
{
    double(*m)[3] = mm.m; double t = m[0][0]+m[1][1]+m[2][2], s, x, y, z, w;
    if (t > 0)                                   { s=sqrt(t+1.0)*2; w=0.25*s; x=(m[2][1]-m[1][2])/s; y=(m[0][2]-m[2][0])/s; z=(m[1][0]-m[0][1])/s; }
    else if (m[0][0]>m[1][1] && m[0][0]>m[2][2]) { s=sqrt(1.0+m[0][0]-m[1][1]-m[2][2])*2; w=(m[2][1]-m[1][2])/s; x=0.25*s; y=(m[0][1]+m[1][0])/s; z=(m[0][2]+m[2][0])/s; }
    else if (m[1][1]>m[2][2])                    { s=sqrt(1.0+m[1][1]-m[0][0]-m[2][2])*2; w=(m[0][2]-m[2][0])/s; x=(m[0][1]+m[1][0])/s; y=0.25*s; z=(m[1][2]+m[2][1])/s; }
    else                                         { s=sqrt(1.0+m[2][2]-m[0][0]-m[1][1])*2; w=(m[1][0]-m[0][1])/s; x=(m[0][2]+m[2][0])/s; y=(m[1][2]+m[2][1])/s; z=0.25*s; }
    return { x, y, z, w };
}

// sub_46D77A0(a1=new obj, a2, a3=deserialized prefab). Rewrite a3's local transform in place before
// the original copies it into the replicated netvars. One-shot server loads exactly one gamemode, so
// leaving g_objRewrite latched after our AddSlot is safe (nothing else builds prefab objects after).
static __int64(__fastcall* ObjBuild_Orig)(__int64, __int64, __int64) = nullptr;
static __int64 __fastcall ObjBuild_Hook(__int64 a1, __int64 a2, __int64 a3)
{
    __try
    {
        if (g_objRewrite && a3)
        {
            auto* pos = reinterpret_cast<double*>(a3 + 48);   // FVector {x,y,z}
            auto* rot = reinterpret_cast<double*>(a3 + 72);   // FRotator {pitch,yaw,roll}
            HVec  oldPos = { pos[0], pos[1], pos[2] };
            HQuat oldRot = HRotToQuat(rot[0], rot[1], rot[2]);
            HVec  nPos;
            if (g_useMatPos)   // position via matrix B (mirrors the offline solver exactly)
                nPos = { g_Bmat[0][0]*oldPos.x + g_Bmat[0][1]*oldPos.y + g_Bmat[0][2]*oldPos.z,
                         g_Bmat[1][0]*oldPos.x + g_Bmat[1][1]*oldPos.y + g_Bmat[1][2]*oldPos.z,
                         g_Bmat[2][0]*oldPos.x + g_Bmat[2][1]*oldPos.y + g_Bmat[2][2]*oldPos.z };
            else
                nPos = HQuatRotate(g_dRot, oldPos);   // identity when g_dRot is unit -> exact oldPos
            nPos.x += g_dTrans.x; nPos.y += g_dTrans.y; nPos.z += g_dTrans.z;   // B*local + C
            pos[0] = nPos.x; pos[1] = nPos.y; pos[2] = nPos.z;
            if (g_rewriteRot)   // leave rotation alone in translation-only mode (avoids round-trip drift)
            {
                HQuat nRot = HQuatMul(g_dRot, oldRot);
                double np, ny, nr; HQuatToRot(nRot, np, ny, nr);
                rot[0] = np; rot[1] = ny; rot[2] = nr;
            }
            (void)oldRot;
            ++g_objRewriteCount;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    return ObjBuild_Orig(a1, a2, a3);
}

static void LoadGamemodeIntoSlot()
{
    uint64_t base = GetBase();
    static SDK::UClass* slotCls = nullptr;   // [PERF] cached class lookup
    if (!slotCls) slotCls = SDK::UObject::FindClassFast("ModuleSlot");
    auto* gmCls   = SDK::UObject::FindClassFast("GamemodesManager");
    if (!slotCls || !gmCls) { printf("[HalcyonA2][GM] classes not ready\n"); return; }
    wchar_t wpath[64] = {}; size_t cvt = 0; mbstowcs_s(&cvt, wpath, g_loadGmPath, _TRUNCATE);
    wchar_t wantSlot[64] = {}; mbstowcs_s(&cvt, wantSlot, g_loadGmSlot, _TRUNCATE);

    // Find the GamemodesManager + the target (real, fully-initialized) slot.
    SDK::UObject* mgr = nullptr; SDK::UObject* slot = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        if (!mgr && o->IsA(gmCls)) mgr = o;
        if (!slot && o->IsA(slotCls))
        {
            auto* sid = *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(o) + 0x390);
            if (sid && wcsstr(sid, wantSlot)) slot = o;
        }
    }
    if (!mgr || !slot) { printf("[HalcyonA2][GM] mgr=%p slot('%s')=%p not ready yet\n", mgr, g_loadGmSlot, slot); return; }
    auto* addFn = mgr->Class->GetFunction("GamemodesManager", "AddSlot");
    if (!addFn) { printf("[HalcyonA2][GM] AddSlot UFunction not found\n"); return; }

    // Target world transform for the course. -SnapToMarker uses a marker actor's full loc+rot;
    // -GamemodePos is loc-only (rotation kept = slot's, so the course isn't re-rotated). We DON'T move
    // the slot (it's Static, non-replicating — moving it never worked). Instead capture D = slotWorld^-1
    // o markerWorld and arm the sub_46D77A0 hook, which rewrites each object's local coords so the
    // client's slotWorld o (D o local) lands at markerWorld o local. Skip if neither target was given
    // (loads at the slot's baked spot, old behavior).
    double tgtLoc[3] = {}, tgtRot[3] = {}; bool haveLoc = false, haveRot = false;
    if (g_snapMarker[0])
    {
        SDK::UObject* marker = nullptr;
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject() || !o->Class || o->Class->GetName() != "Actor") continue;
            if (o->GetFullName().find(g_snapMarker) == std::string::npos) continue;
            marker = o; break;
        }
        if (!marker) { printf("[HalcyonA2][GM] snap-to-marker '%s' NOT FOUND (waiting)\n", g_snapMarker); return; }
        if (auto* gl = marker->Class->GetFunction("Actor", "K2_GetActorLocation")) { SafeProcessEvent(marker, gl, tgtLoc); haveLoc = true; }
        if (auto* gr = marker->Class->GetFunction("Actor", "K2_GetActorRotation")) { SafeProcessEvent(marker, gr, tgtRot); haveRot = true; }
        printf("[HalcyonA2][GM] marker loc=(%.0f,%.0f,%.0f) rot=(P%.1f,Y%.1f,R%.1f)\n",
               tgtLoc[0], tgtLoc[1], tgtLoc[2], tgtRot[0], tgtRot[1], tgtRot[2]);
        // The PKR_Scraprun level-instance transform isn't always applied when we read: if the marker
        // is still at origin, it's not positioned yet — bail and retry (don't latch, don't snap to 0,0,0).
        if (fabs(tgtLoc[0]) < 1.0 && fabs(tgtLoc[1]) < 1.0 && fabs(tgtLoc[2]) < 1.0)
        { printf("[HalcyonA2][GM] marker not positioned yet (0,0,0) -- waiting\n"); return; }
    }
    else if (g_gmPosSet) { tgtLoc[0] = g_gmPos[0]; tgtLoc[1] = g_gmPos[1]; tgtLoc[2] = g_gmPos[2]; haveLoc = true; }

    if (haveLoc)
    {
        double slotLoc[3] = {}, slotRot[3] = {};
        if (auto* gl = slot->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(slot, gl, slotLoc);
        if (auto* gr = slot->Class->GetFunction("Actor", "K2_GetActorRotation")) SafeProcessEvent(slot, gr, slotRot);
        g_objRewriteCount = 0;

        if (haveRot)
        {
            // FULL transform (via -SnapToMarker): place the course as the removed scraprun slot would,
            // i.e. worldWant = Rs*local + P (Rs = marker's applied rotation, P = marker loc + nudge).
            // Client applies world = A*fed + Tp, A = UERot(pkrActorRot)^T (confirmed exactly). So feed
            // fed = B*local + C, B = A^-1*Rs = UERot(pkrRot)*UERot(markerRot)^T, C = A^-1*(P - kPkrTp).
            // Matrices (not quaternions) so this mirrors solve2.ps1, which correctly predicted the
            // corridor; only the final B is converted to a quat for the hook.
            //
            // The actor rotations are read live for logging only — K2_GetActorRotation is FLAKY (returns
            // (0,0,0) on some load-timing paths, same late-stream issue the marker loc had), which throws
            // the whole transform off. PKR_Custom_Full and the scraprun marker are fixed, so use the
            // known-good constants for the actual math.
            const double pkrRot[3]    = { -60.0, 180.0, 180.0 };   // PKR_Custom_Full actor rotation (fixed)
            const double markerRotC[3]= { -90.0, 180.0, 180.0 };   // scraprun marker actor rotation (fixed)
            printf("[HalcyonA2][GM] live-read rot (diagnostic): slot=(P%.1f,Y%.1f,R%.1f) marker=(P%.1f,Y%.1f,R%.1f)\n",
                   slotRot[0],slotRot[1],slotRot[2], tgtRot[0],tgtRot[1],tgtRot[2]);
            HMat Ainv = HUERot(pkrRot[0], pkrRot[1], pkrRot[2]);            // A^-1 = UERot(pkrRot)
            HMat Rs   = HTranspose(HUERot(markerRotC[0], markerRotC[1], markerRotC[2])); // scraprun applied rotation
            HMat B    = HMatMul(Ainv, Rs);
            for (int i=0;i<3;i++) for (int j=0;j<3;j++) g_Bmat[i][j] = B.m[i][j];
            g_useMatPos = true;
            g_dRot = HMatToQuat(B);
            HVec P  = { tgtLoc[0]+g_gmNudge[0], tgtLoc[1]+g_gmNudge[1], tgtLoc[2]+g_gmNudge[2] };
            HVec dW = { P.x - kPkrTp.x, P.y - kPkrTp.y, P.z - kPkrTp.z };
            g_dTrans = HMatVec(Ainv, dW);                                    // C
            // The client's spawn COUPLES object rotation into position (rewriting a3+72 moves the actor),
            // so rewriting rotation breaks the clean world=A*fedPos+Tp mapping and flings the course off.
            // With rotation left alone, fedPos=B*local+C yields world = Rs*local + marker -> the course
            // LAYOUT lands correctly in the corridor (each mesh spun ~30deg off, PKR slot rot vs corridor).
            // Opt into rotation rewrite only for experiments via -GmRewriteRot.
            g_rewriteRot = g_forceRot;
            printf("[HalcyonA2][GM] ARMED full-transform (baked pkrRot=-60,180,180 markerRot=-90,180,180) "
                   "P=(%.0f,%.0f,%.0f) nudge=(%.0f,%.0f,%.0f) rewriteRot=%d | B=(%.4f,%.4f,%.4f,%.4f) C=(%.0f,%.0f,%.0f)\n",
                   P.x,P.y,P.z, g_gmNudge[0],g_gmNudge[1],g_gmNudge[2], g_rewriteRot?1:0,
                   g_dRot.x,g_dRot.y,g_dRot.z,g_dRot.w, g_dTrans.x,g_dTrans.y,g_dTrans.z);
        }
        else
        {
            // Loc-only (-GamemodePos): translation shift, no rotation change.
            g_dRot = { 0,0,0,1 };
            g_rewriteRot = false;
            g_dTrans = { tgtLoc[0]-slotLoc[0]+g_gmNudge[0], tgtLoc[1]-slotLoc[1]+g_gmNudge[1], tgtLoc[2]-slotLoc[2]+g_gmNudge[2] };
            printf("[HalcyonA2][GM] ARMED translation-only: D.trans=(%.0f,%.0f,%.0f)\n",
                   g_dTrans.x,g_dTrans.y,g_dTrans.z);
        }
        g_objRewrite = true;
    }

    SetSlotFString(slot, 0x418, wpath);   // DefaultGamemodePath
    void* parms = slot;
    printf("[HalcyonA2][GM] loading '%s' into slot '%ls' via AddSlot\n",
           g_loadGmPath, *reinterpret_cast<const wchar_t**>(reinterpret_cast<uintptr_t>(slot) + 0x390));
    g_gmObjBase = SDK::UObject::GObjects->Num();   // snapshot: everything after this index is ours
    SafeProcessEvent(mgr, addFn, &parms);
    g_gmLoadDone = true;
    printf("[HalcyonA2][GM] AddSlot fired for gamemode '%s'\n", g_loadGmPath);
}
static void SafeLoadGamemodeIntoSlot() { __try { LoadGamemodeIntoSlot(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[HalcyonA2][GM] load faulted (SEH)\n"); } }

// ---------------------------------------------------------------------------
// SLOT-SPAWN placement (-SpawnSlotAtMarker) — the correct approach. Coordinate-rewrite is dead (the
// client transform isn't affine). Instead spawn a REAL AModuleSlot at the marker (Movable -> its
// transform replicates) and initialize it the way a level-placed slot is, using the reflected
// UFunctions from the SDK (SandboxEngine.ModuleSlot): PushNetVars() builds the netvar container that
// a raw SpawnActor slot lacks (the sub_465A320 null-table crash). Then AddSlot loads deathrun2 into
// it and the client places every object at slotTransform*local = marker*local = the corridor, natively.
static bool g_spawnSlot = false;
static double g_slotRotAdj[3] = {};   // -SlotRot=P,Y,R : extra rotation added to the marker rotation
static SDK::UObject* g_spawnedSlot = nullptr;   // the runtime slot, for the post-load netvar push
static bool g_pushedAfterLoad = false;
// After the async load binds LoadedGameMode, push the slot's + gamemode's netvars so the state
// replicates to clients (a runtime slot never gets the client-side auto-load a level slot does, so
// the client only learns the gamemode via these netvar pushes). Fired from the ticker once bound.
static void PushSpawnedSlotNetVars()
{
    if (!g_spawnedSlot || g_pushedAfterLoad) return;
    uintptr_t s = reinterpret_cast<uintptr_t>(g_spawnedSlot);
    void* lgm = *reinterpret_cast<void**>(s + 0x440);   // AModuleSlot::LoadedGameMode
    if (!lgm) { printf("[HalcyonA2][SPAWN] waiting for LoadedGameMode to bind...\n"); return; }
    g_pushedAfterLoad = true;
    if (auto* fn = g_spawnedSlot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(g_spawnedSlot, fn, nullptr);
    auto* lgmO = reinterpret_cast<SDK::UObject*>(lgm);
    if (lgmO->Class)
        if (auto* fn = lgmO->Class->GetFunction("LoadedGameMode", "PushNetVars")) SafeProcessEvent(lgmO, fn, nullptr);
    if (auto* fnu = g_spawnedSlot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(g_spawnedSlot, fnu, nullptr);
    printf("[HalcyonA2][SPAWN] pushed slot+gamemode netvars post-load (LoadedGameMode=%p)\n", lgm);
}
static void SafePushSpawnedSlotNetVars() { __try { PushSpawnedSlotNetVars(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
static void SpawnSlotAndLoad()
{
    uint64_t base = GetBase();
    auto* world = SDK::UWorld::GetWorld();
    if (!world) { printf("[HalcyonA2][SPAWN] no world\n"); return; }
    static SDK::UClass* gmCls = nullptr;   // [PERF] cached class lookup
    if (!gmCls) gmCls = SDK::UObject::FindClassFast("GamemodesManager");
    // Use the importance-volume variant (like real gamemode slots). Its ImportanceVolume drives
    // A2's mobile LOD/streaming: when a local pawn overlaps it the client calls
    // LoadedGameMode->EnterImportanceVolume() which makes the meshes render. Plain BP_GamemodeSlot_C
    // has no volume, so on Android objects load as collision-only (invisible). Same "defaultslot"
    // CDO default, so the client match still holds.
    static SDK::UClass* slotBPCls = nullptr;   // [PERF] cached class lookup
    if (!slotBPCls) slotBPCls = SDK::UObject::FindClassFast("BP_ModuleSlotWithImportanceVolume_C");
    if (!slotBPCls) slotBPCls = SDK::UObject::FindClassFast("BP_GamemodeSlot_C");
    if (!gmCls || !slotBPCls) { printf("[HalcyonA2][SPAWN] classes not ready (gm=%p slotBP=%p)\n", gmCls, slotBPCls); return; }

    // GamemodesManager instance
    SDK::UObject* mgr = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    { auto* o = SDK::UObject::GObjects->GetByIndex(i); if (o && !o->IsDefaultObject() && o->IsA(gmCls)) { mgr = o; break; } }
    if (!mgr) { printf("[HalcyonA2][SPAWN] GamemodesManager not ready\n"); return; }

    // Marker transform (live, guarded non-zero — level-instance applies it late)
    double mLoc[3] = {}, mRot[3] = {};
    { SDK::UObject* marker = nullptr;
      for (int32_t i = 0; i < num; ++i)
      { auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->Class || o->Class->GetName() != "Actor") continue;
        if (o->GetFullName().find(g_snapMarker) == std::string::npos) continue; marker = o; break; }
      if (!marker) { printf("[HalcyonA2][SPAWN] marker '%s' not found (waiting)\n", g_snapMarker); return; }
      if (auto* gl = marker->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(marker, gl, mLoc);
      if (auto* gr = marker->Class->GetFunction("Actor", "K2_GetActorRotation")) SafeProcessEvent(marker, gr, mRot);
      if (fabs(mLoc[0]) < 1.0 && fabs(mLoc[1]) < 1.0 && fabs(mLoc[2]) < 1.0)
      { printf("[HalcyonA2][SPAWN] marker not positioned yet -- waiting\n"); return; } }
    printf("[HalcyonA2][SPAWN] marker loc=(%.0f,%.0f,%.0f) rot=(P%.1f,Y%.1f,R%.1f)\n",
           mLoc[0],mLoc[1],mLoc[2], mRot[0],mRot[1],mRot[2]);

    // Build the spawn transform at the marker. Apply -SlotRot as a WORLD-space quaternion delta
    // (pre-multiply) rather than adding Euler angles: the marker is pitched -90 so Euler yaw/roll are
    // gimbal-locked (they collapse to the same axis). As a quat delta, -SlotRot Yaw = world Z spin,
    // Roll = world X, Pitch = world Y — all distinct.
    HQuat qMarker = HRotToQuat(mRot[0], mRot[1], mRot[2]);
    HQuat qDelta  = HRotToQuat(g_slotRotAdj[0], g_slotRotAdj[1], g_slotRotAdj[2]);
    HQuat q       = HQuatMul(qMarker, qDelta);   // LOCAL delta: -SlotRot Yaw spins the course about its OWN up
    printf("[HalcyonA2][SPAWN] marker rot=(P%.1f,Y%.1f,R%.1f) + world adj=(P%.0f,Y%.0f,R%.0f) -> quat=(%.3f,%.3f,%.3f,%.3f)\n",
           mRot[0],mRot[1],mRot[2], g_slotRotAdj[0],g_slotRotAdj[1],g_slotRotAdj[2], q.x,q.y,q.z,q.w);
    SDK::FTransform xform{};
    xform.Translation.X = mLoc[0]+g_gmNudge[0]; xform.Translation.Y = mLoc[1]+g_gmNudge[1]; xform.Translation.Z = mLoc[2]+g_gmNudge[2];
    printf("[HalcyonA2][SPAWN] spawn loc=(%.0f,%.0f,%.0f) (marker + nudge %.0f,%.0f,%.0f)\n",
           xform.Translation.X, xform.Translation.Y, xform.Translation.Z, g_gmNudge[0],g_gmNudge[1],g_gmNudge[2]);
    xform.Rotation.X = q.x; xform.Rotation.Y = q.y; xform.Rotation.Z = q.z; xform.Rotation.W = q.w;
    xform.Scale3D.X = 1.0; xform.Scale3D.Y = 1.0; xform.Scale3D.Z = 1.0;

    auto* slot = SDK::UGameplayStatics::BeginDeferredActorSpawnFromClass(
        world, slotBPCls, xform, SDK::ESpawnActorCollisionHandlingMethod::AlwaysSpawn,
        nullptr, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    if (!slot) { printf("[HalcyonA2][SPAWN] BeginDeferredActorSpawnFromClass returned null\n"); return; }
    uintptr_t s = reinterpret_cast<uintptr_t>(slot);
    g_spawnedSlot = slot; g_pushedAfterLoad = false;

    // Leave SlotID at the CDO default "defaultslot". SlotID does NOT replicate, so the client's copy
    // of this slot keeps the CDO default; if we rename it server-side the client can't match it to the
    // gamemode and never spawns the course (confirmed via client census). Keeping "defaultslot" on the
    // server means the manager keys deathrun2 under "defaultslot" and the client's "defaultslot" slot
    // matches -> loads the course at the corridor, exactly like a baked slot does.

    // NOTE: bAllObjectsVisibleAtAllTimes@0x2FC and the ImportanceVolume box extent are CLIENT-LOCAL,
    // non-replicated — the Android client reads its own slot copy's CDO defaults, so setting them
    // server-side does nothing (same wall as SlotID). The LOD pop-in on the long course is fixed
    // instead via the replicated DefaultLODSettings netvar — see FixDeathrunLOD().

    SDK::UGameplayStatics::FinishSpawningActor(slot, xform, SDK::ESpawnActorScaleMethod::MultiplyWithRoot);
    printf("[HalcyonA2][SPAWN] spawned slot 0x%llX (class %s)\n",
           (unsigned long long)slot, slot->Class ? slot->Class->GetName().c_str() : "?");

    // A runtime-spawned actor doesn't replicate to clients by default (a level-placed slot does). The
    // client needs the slot to appear so it can place the gamemode objects relative to it. Force it:
    // SetReplicates(true) + bAlwaysRelevant + bReplicateMovement + huge net-cull distance.
    if (auto* sr = slot->Class->GetFunction("Actor", "SetReplicates"))
    { struct { bool b; } rp{ true }; SafeProcessEvent(slot, sr, &rp); }
    *reinterpret_cast<uint8_t*>(s + 0x60) |= 0x08 | 0x10;                 // bAlwaysRelevant | bReplicateMovement
    *reinterpret_cast<float*>(s + 0x170) = 1.0e12f;                        // NetCullDistanceSquared (never cull)
    if (auto* fnu = slot->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(slot, fnu, nullptr);

    // Initialize like a level-placed slot: push its netvars (the container a fresh spawn otherwise
    // lacks -> the loader's null-deref). Do NOT call UpdateSlotIDFromPosition/GenerateUniqueSlotID:
    // those rename SlotID, which the client can't match (SlotID doesn't replicate).
    if (auto* fn = slot->Class->GetFunction("ModuleSlot", "PushNetVars")) SafeProcessEvent(slot, fn, nullptr);

    printf("[HalcyonA2][SPAWN] init done: SlotID='%ls' ModuleState=%p LoadedGameMode=%p\n",
           *reinterpret_cast<const wchar_t**>(s + 0x390),
           *reinterpret_cast<void**>(s + 0x300), *reinterpret_cast<void**>(s + 0x440));

    // Load deathrun2 into it.
    wchar_t wpath[64] = {}; size_t cvt = 0; mbstowcs_s(&cvt, wpath, g_loadGmPath, _TRUNCATE);
    SetSlotFString(slot, 0x418, wpath);   // DefaultGamemodePath
    auto* addFn = mgr->Class->GetFunction("GamemodesManager", "AddSlot");
    if (!addFn) { printf("[HalcyonA2][SPAWN] AddSlot not found\n"); return; }
    void* parms = slot;
    g_gmObjBase = SDK::UObject::GObjects->Num();
    printf("[HalcyonA2][SPAWN] AddSlot('%s') on fresh slot...\n", g_loadGmPath);
    SafeProcessEvent(mgr, addFn, &parms);
    g_gmLoadDone = true;
    double sLoc[3] = {};
    if (auto* gl = slot->Class->GetFunction("Actor", "K2_GetActorLocation")) SafeProcessEvent(slot, gl, sLoc);
    uintptr_t m = reinterpret_cast<uintptr_t>(mgr);
    int32_t mgrLoadedGm = *reinterpret_cast<int32_t*>(m + 0x2B8 + 8);   // GamemodesManager.loadedGamemodes.Num
    auto* eng = *reinterpret_cast<void**>(m + 0x60);                    // GamemodesManager.SandboxEngine
    int32_t engLoadedGm = eng ? *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(eng) + 0x118 + 8) : -1;
    printf("[HalcyonA2][SPAWN] AddSlot fired | slot worldpos=(%.0f,%.0f,%.0f) RemoteRole=%d bRepBits=0x%02X "
           "LoadedGameMode=%p | mgr.loadedGamemodes=%d eng.loadedGamemodes=%d\n",
           sLoc[0], sLoc[1], sLoc[2],
           *reinterpret_cast<uint8_t*>(s + 0x68), *reinterpret_cast<uint8_t*>(s + 0x60),
           *reinterpret_cast<void**>(s + 0x440), mgrLoadedGm, engLoadedGm);
}
static void SafeSpawnSlotAndLoad() { __try { SpawnSlotAndLoad(); } __except (EXCEPTION_EXECUTE_HANDLER) { printf("[HalcyonA2][SPAWN] faulted (SEH)\n"); } }

// Deathrun's death balls (BP_SmallDeathBall/BP_BigDeathBall) are broken in the client's cooked APK
// (missing package dependency -> broken mesh/material) and CRASH the Android render thread (SIGSEGV)
// the instant one is drawn. We can't re-cook the client, so suppress them server-side: null out any
// BallSpawnerComponent whose DiscClass (@0x498) is a death ball so SpawnBall no-ops (the engine just
// warns "no class specified"), and destroy any death balls that already spawned. Deathrun then plays
// minus the death-ball traps. Runs periodically (spawners are (re)created on gamemode load).
static char g_ballClass[96] = {};   // -BallClass=<ClassName> : replacement for broken death balls (else suppress)
static int  g_deathBallsSuppressed = 0;
static void SuppressDeathBalls()
{
    static SDK::UClass* spawnerCls = nullptr;   // [PERF] cached class lookup
    if (!spawnerCls) spawnerCls = SDK::UObject::FindClassFast("BallSpawnerComponent");
    if (!spawnerCls) return;
    // Optional replacement class (a ball the client CAN render, pulled from the OBB paks). If it
    // resolves we swap DiscClass to it (keeps the trap producing a real, renderable ball); otherwise
    // we null DiscClass so SpawnBall no-ops.
    SDK::UClass* repl = g_ballClass[0] ? SDK::UObject::FindClassFast(g_ballClass) : nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(spawnerCls)) continue;
        auto** discCls = reinterpret_cast<SDK::UClass**>(reinterpret_cast<uintptr_t>(o) + 0x498);   // DiscClass
        if (*discCls && (*discCls)->GetName().find("DeathBall") != std::string::npos)
        {
            printf("[HalcyonA2][NODB] spawner %s: DiscClass=%s -> %s\n",
                   o->GetName().c_str(), (*discCls)->GetName().c_str(), repl ? repl->GetName().c_str() : "(null/suppressed)");
            *discCls = repl;   // nullptr = suppress, else the safe replacement ball
            ++g_deathBallsSuppressed;
        }
    }
    // Destroy any broken death balls already in the world (they'd crash a client that renders them).
    auto destroyClass = [&](const char* cn)
    {
        auto* c = SDK::UObject::FindClassFast(cn);
        if (!c) return;
        for (int32_t i = 0; i < num; ++i)
        {
            auto* o = SDK::UObject::GObjects->GetByIndex(i);
            if (!o || o->IsDefaultObject() || !o->IsA(c)) continue;
            if (auto* fn = o->Class->GetFunction("Actor", "K2_DestroyActor")) SafeProcessEvent(o, fn, nullptr);
        }
    };
    destroyClass("BP_SmallDeathBall_C");
    destroyClass("BP_BigDeathBall_C");
}
static void SafeSuppressDeathBalls() { __try { SuppressDeathBalls(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ANDROID LOD POP-IN FIX. deathrun2's project carries a replicated `DefaultLODSettings`
// (type NetworkedRawData in level.json settings) = the ONE server->client lever for object
// visibility (bAllObjectsVisibleAtAllTimes + the ImportanceVolume box are client-local and
// non-replicated, so setting them does nothing — that was the failed fix).
//
// Triangulated across all shipped levels, the blob is a serialized FPrefabLODSettings:
//   * open courses (club_golf, quests) = 37 bytes: 0x01 + zeros = NO importance-volume hide,
//     everything always visible.
//   * deathrun2 == arena_jakeball == TackleballTraining = 45 bytes: 0x01 + 28 zeros + four
//     trailing int32=1 = ShowImportanceVolume/HideOutsideImportanceVolume ON = hide objects
//     outside the box. Jakeball/tackleball fit their box so don't pop; deathrun's long corridor
//     exceeds it -> far traps hide = the pop-in.
//
// Fix = turn deathrun2's blob into the golf "no-hide" form by zeroing the four trailing int32=1.
// The value lives in a replicated TArray<uint8>; we can't reach it by a typed netvar API (no
// raw-data UNetVar class in the SDK), so locate it by its exact 45-byte signature (a TArray
// {ptr,Num=45,Max} whose buffer matches) among the loaded objects and patch it in place. The
// signature is exact enough that false positives are impossible.
static bool g_fixLod = true;          // -NoFixLOD disables
static int  g_lodPatched = 0;
static bool LODSigMatches(const uint8_t* b)
{
    // 0x01, then 28 zero bytes, then 01 00 00 00 x4
    if (b[0] != 0x01) return false;
    for (int i = 1; i <= 28; ++i) if (b[i] != 0x00) return false;
    for (int g = 0; g < 4; ++g)
    {
        const uint8_t* w = b + 29 + g * 4;
        if (w[0] != 0x01 || w[1] || w[2] || w[3]) return false;
    }
    return true;
}
// Raw, SEH-safe scan of one object's member region for the DefaultLODSettings TArray<uint8>
// {void* Data; int32 Num=45; int32 Max}. Patches every match in place (zeros the trailing flags)
// and returns how many it patched. No C++ objects here (SEH requires no unwinding).
static int ScanObjForLOD(uintptr_t base, uintptr_t* firstOff)
{
    int hits = 0;
    __try
    {
        for (uintptr_t off = 0; off <= 0x800; off += 8)
        {
            int32_t n = *reinterpret_cast<int32_t*>(base + off + 8);
            if (n != 45) continue;                                   // cheap pre-filter (in-object read)
            int32_t mx = *reinterpret_cast<int32_t*>(base + off + 12);
            if (mx < 45 || mx > 0x10000) continue;
            auto* data = *reinterpret_cast<uint8_t**>(base + off);
            uintptr_t dp = reinterpret_cast<uintptr_t>(data);
            if (dp < 0x10000 || dp > 0x7FFFFFFFFFFFULL || (dp & 1)) continue;
            if (!LODSigMatches(data)) continue;
            // Match: zero the four trailing int32=1 -> golf "no-hide" profile.
            DWORD oldp;
            VirtualProtect(data + 29, 16, PAGE_READWRITE, &oldp);
            for (int k = 29; k < 45; ++k) data[k] = 0;
            VirtualProtect(data + 29, 16, oldp, &oldp);
            if (hits == 0 && firstOff) *firstOff = off;
            ++hits;
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { /* object smaller than scan window / bad read — skip */ }
    return hits;
}
static void FixDeathrunLOD()
{
    if (!g_fixLod) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o) continue;
        uintptr_t off = 0;
        int hits = ScanObjForLOD(reinterpret_cast<uintptr_t>(o), &off);   // raw scan/patch (SEH)
        if (hits > 0)
        {
            g_lodPatched += hits;   // logging (std::string) OUTSIDE the SEH scope
            printf("[HalcyonA2][LOD] patched DefaultLODSettings on %s (+0x%llX, %d) -> no-hide (all objects visible)\n",
                   o->GetName().c_str(), (unsigned long long)off, hits);
        }
    }
}
static void SafeFixDeathrunLOD() { __try { FixDeathrunLOD(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// The blob scan above only ever found a TRANSIENT copy (SacrificialUClass) — the live replicated
// value isn't a bare 45-byte TArray at runtime. The effective per-object LOD is the FPrefabLODSettings
// on each placed object: UPrefabComponent.Settings@0x258 -> LODSettings@0xF8 (and the shared
// prefabDefinition@0x278 -> Settings -> LODSettings). Settings is an InstancedReference/
// PersistentInstance subobject of the server-spawned (replicating) prefab actor, so patching it +
// ForceNetUpdate is the direct, testable lever. Force each to "always visible, no distance/volume
// hide" = the golf profile. FPrefabLODSettings (SandboxEngine_structs): VisibleNever@0,
// VisibleAlways@1, MaxDrawDistance@4, MinDrawDistance@8, VisibleWhenPlaying@0xC,
// ShowDistanceToGamemode@0x10, HideDistanceToGamemode@0x14, ShowImportanceVolume@0x18,
// HideOutsideImportanceVolume@0x19.
static int g_lodObjPatched = 0;
static void ApplyAlwaysVisible(uintptr_t lod)   // lod = &FPrefabLODSettings
{
    *reinterpret_cast<uint8_t*>(lod + 0x00) = 0;           // VisibleNever = false
    *reinterpret_cast<uint8_t*>(lod + 0x01) = 1;           // VisibleAlways = true
    *reinterpret_cast<float*>  (lod + 0x04) = 1.0e12f;     // MaxDrawDistance = effectively infinite
    *reinterpret_cast<float*>  (lod + 0x08) = 0.0f;        // MinDrawDistance = 0
    *reinterpret_cast<float*>  (lod + 0x10) = 1.0e12f;     // ShowDistanceToGamemode = always
    *reinterpret_cast<float*>  (lod + 0x14) = 1.0e12f;     // HideDistanceToGamemode = never
    *reinterpret_cast<uint8_t*>(lod + 0x18) = 0;           // ShowImportanceVolume = false
    *reinterpret_cast<uint8_t*>(lod + 0x19) = 0;           // HideOutsideImportanceVolume = false
}
static void ForceTrapLOD()
{
    if (!g_fixLod) return;
    static SDK::UClass* prefabCls = nullptr;   // [PERF] cached class lookup
    if (!prefabCls) prefabCls = SDK::UObject::FindClassFast("PrefabComponent");
    if (!prefabCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(prefabCls)) continue;
        uintptr_t pc = reinterpret_cast<uintptr_t>(o);
        // per-instance Settings@0x258 -> LODSettings@0xF8
        if (auto* settings = *reinterpret_cast<void**>(pc + 0x258))
        { ApplyAlwaysVisible(reinterpret_cast<uintptr_t>(settings) + 0xF8); ++g_lodObjPatched; }
        // shared definition@0x278 -> Settings@0x38 -> LODSettings@0xF8
        if (auto* def = *reinterpret_cast<void**>(pc + 0x278))
            if (auto* dset = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(def) + 0x38))
                ApplyAlwaysVisible(reinterpret_cast<uintptr_t>(dset) + 0xF8);
        // re-replicate the owning prefab actor so the change goes out
        if (auto* actor = *reinterpret_cast<SDK::UObject**>(pc + 0x3E8))
            if (auto* fnu = actor->Class->GetFunction("Actor", "ForceNetUpdate")) SafeProcessEvent(actor, fnu, nullptr);
    }
}
static void SafeForceTrapLOD() { __try { ForceTrapLOD(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ★ THE ACTUAL FIX. sub_46B5E70 (project deserializer) creates the replicated "DefaultLODSettings"
// as a RawData netvar (vtable off_7FE6540 @ RVA 0x7FE6540, allocated by sub_531F860(80) — NOT a
// UObject, so it never appeared in the GObjects scan). Layout: TArray<uint8> Data@0x38, Num@0x40.
// It calls sub_46A0DF0(netvar) once, right after the bytes are memcpy'd in — so hooking that gives
// us the netvar with its bytes in place, BEFORE it replicates. We filter for the DefaultLODSettings
// blob (vtable match + Num==45 + exact signature) and zero the trailing hide-flags -> golf "no-hide"
// (all objects always visible), so the client receives the no-hide profile from the first send.
using NetVarReg_t = __int64(__fastcall*)(void*);
static NetVarReg_t g_NetVarReg_Orig = nullptr;
static int g_lodNetvarPatched = 0;
static void NetVarRegFilter(void* netvar)
{
    int didPatch = 0;
    if (g_fixLod && netvar)
    {
        __try
        {
            uintptr_t vt = *reinterpret_cast<uintptr_t*>(netvar);
            if (vt == GetBase() + 0x804BDA0)                                   // RawData netvar vtable [PORT-AUDIT] 22284 (was 20996 0x7FE6540; mapped via 3 aligned referrers)
            {
                int num = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(netvar) + 0x40);
                auto* data = *reinterpret_cast<uint8_t**>(reinterpret_cast<uint8_t*>(netvar) + 0x38);
                if (num == 45 && data && LODSigMatches(data))
                {
                    DWORD op; VirtualProtect(data + 29, 16, PAGE_READWRITE, &op);
                    for (int k = 29; k < 45; ++k) data[k] = 0;               // -> golf no-hide profile
                    VirtualProtect(data + 29, 16, op, &op);
                    didPatch = 1;
                }
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER) { didPatch = 0; }
    }
    if (didPatch) { ++g_lodNetvarPatched; printf("[HalcyonA2][LOD] rewrote DefaultLODSettings netvar -> no-hide (#%d)\n", g_lodNetvarPatched); }
}
// [PORT-AUDIT] 20996 sub_46A0DF0 exists as two identical copies on 22284: 0x46BC830 (called from the old-style callers)
// and 0x47138D0 (called from the new ObjBuild). Hook both; the port never installed this hook at all.
static NetVarReg_t g_NetVarReg_Orig2 = nullptr;
static __int64 __fastcall NetVarReg_Hook(void* netvar)  { NetVarRegFilter(netvar); return g_NetVarReg_Orig(netvar); }
static __int64 __fastcall NetVarReg_Hook2(void* netvar) { NetVarRegFilter(netvar); return g_NetVarReg_Orig2(netvar); }

// P1 — arena team colors. Each arena's TicketManager holds TeamColors[2] (Home/Away), which drives
// the arena decor (BP_GamemodeColorCoordinator reads TicketManager->TeamColors[i].Primary) AND the
// color admission stamps on a joining pawn. Offline the module slot's InitializeRandomColors() fills
// it from DT_CosmeticMaterialMetaData; on our headless server the TicketManager instead inits
// TeamColors to 2x the "NoPreference" gray (0xFF63697B = RGB 99,105,123) from the empty level.json
// TeamColors:{}, and calling InitializeRandomColors didn't stick (reset at TM init). So set the
// colors ourselves, directly on each TicketManager, using the exact DT palette (InitializeRandomColors
// draws rows 1..6 = these 6, skipping NoPreference[0] and Storm[7]). Deterministic 2-color pick per TM
// (stable across re-applies -> no flicker); re-apply whenever a TM shows empty/NoPreference so a late
// TM re-init can't leave it gray. FColor memory order is B,G,R,A.
struct HTeamColor { unsigned char pr, pg, pb, sr, sg, sb, logo; const wchar_t* name; };
static const HTeamColor kTeamPalette[6] = {
    {   0, 114, 255, 101,   0,  99, 1, L"Excelsior"    },
    { 216, 155,   0,  38,  38,  39, 2, L"RustRunners"  },
    { 221,  42,   0, 255, 202, 149, 3, L"BetelJuicers" },
    {   0,  31, 216, 199, 192,   0, 4, L"Crowns"       },
    {   0, 163,   0, 191, 186,   0, 5, L"Cobs"         },
    { 224,  73, 246,   0, 101, 134, 6, L"Cuttlefish"   },
};
static void FillFColor(void* fc, unsigned char r, unsigned char g, unsigned char b)
{
    auto* p = reinterpret_cast<unsigned char*>(fc);   // FColor = { B, G, R, A }
    p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
}
static bool SafeProcessEvent(SDK::UObject* o, SDK::UFunction* fn, void* parms)
{
    __try { o->ProcessEvent(fn, parms); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static void InitTeamColors()
{
    static SDK::UClass* tmCls = nullptr;   // [PERF] cached class lookup
    if (!tmCls) tmCls = SDK::UObject::FindClassFast("TicketManager");
    if (!tmCls) return;
    static SDK::UFunction* fnSet = nullptr;
    static SDK::UFunction* fnRep = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(tmCls)) continue;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const int tcNum = *reinterpret_cast<int*>(p + 0x2F0);            // TeamColors.Num
        auto* tcData    = *reinterpret_cast<unsigned char**>(p + 0x2E8); // TeamColors.Data
        // already has real colors? (not empty, first entry isn't the NoPreference gray)
        const bool isDefault = tcNum < 2 || !tcData ||
            (tcData[2] == 99 && tcData[1] == 105 && tcData[0] == 123);
        if (!isDefault) continue;

        if (!fnSet) fnSet = o->Class->GetFunction("TicketManager", "SetTeamColors");
        if (!fnSet) return;

        // deterministic 2-of-6 pick keyed on the TM pointer (stable -> re-apply is idempotent)
        const size_t h  = reinterpret_cast<uintptr_t>(o) >> 4;
        const int    ia = static_cast<int>(h % 6);
        const int    ib = static_cast<int>((ia + 1 + (h / 6) % 5) % 6);
        const HTeamColor& A = kTeamPalette[ia];
        const HTeamColor& B = kTeamPalette[ib];

        struct { SDK::FTeamColor Home; SDK::FTeamColor Away; } parms{};
        FillFColor(&parms.Home.Primary,   A.pr, A.pg, A.pb);
        FillFColor(&parms.Home.Secondary, A.sr, A.sg, A.sb);
        parms.Home.TeamLogoIndex = A.logo;
        FillFColor(&parms.Away.Primary,   B.pr, B.pg, B.pb);
        FillFColor(&parms.Away.Secondary, B.sr, B.sg, B.sb);
        parms.Away.TeamLogoIndex = B.logo;
        // P2 — set the team NAME (FTeamColor.TeamName@0xC, FName). Without it the arena team-name
        // label + per-player roster resolve to "None" even with colors/logos set. Conv_StringToName
        // creates/resolves the FName (A2 FNames aren't encrypted).
        parms.Home.TeamName = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(A.name));
        parms.Away.TeamName = SDK::UKismetStringLibrary::Conv_StringToName(SDK::FString(B.name));

        if (SafeProcessEvent(o, fnSet, &parms))
        {
            // Fire the RepNotify server-side so the local BP_GamemodeColorCoordinator repaints (the
            // server doesn't run its own OnReps; remote clients also get it via replication).
            if (!fnRep) fnRep = o->Class->GetFunction("TicketManager", "OnRep_TeamColors");
            if (fnRep) SafeProcessEvent(o, fnRep, nullptr);
            printf("[HalcyonA2][TEAMCOLOR] set %s home=%d away=%d\n", o->GetName().c_str(), ia, ib);
        }
    }
}
static void SafeInitTeamColors() { __try { InitTeamColors(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---- Tackleball/Driftball TRAINING: shooting & goalie practice ---------------
// Reimplements TackleballTraining_a2level/gamemode.luau (serverOnly -> never runs on our headless
// server), whose only job is: when a practice mode is enabled, spawn the next training ball at a
// random location and fire EventCompleteWithDiscArgument so the BP's server-side shooting/goalie
// logic (aim/velocity/scoring, all present + authority-gated) actually runs. Same "serverOnly luau
// conductor is missing" gap solved for jakeball goals. Kiosk = ABP_TackleballTraining_C:
//   ShootingTraining@0x350, GoalieTraining@0x358, Event@0x360, ActivePawn@0x430, ActiveBalls@0x438.
//   UShootingTrainingComponent.bIsEnabled@0x478, UGoalieTrainingComponent.bIsEnabled@0x498.
// Locations/rotations are the hardcoded arrays from gamemode.luau (FVector = 3 doubles).
struct TVec { double x, y, z; };
static const TVec kGoalieLocs[7] = {
    {2.998538,119.090794,207.935547}, {-281.232418,175.940410,207.935547},
    {-426.630364,289.948226,207.935547}, {-574.910145,478.843127,207.935547},
    {281.862065,176.994229,207.935547}, {427.270759,290.490197,207.935547},
    {576.825207,479.800985,207.935547},
};
static const TVec kShootLocs[7] = {
    {551.825222,1104.800985,157.935547}, {-550.999973,1104.801012,157.935547},
    {-648.174793,479.801015,132.935547}, {651.825207,479.800983,132.935547},
    {551.825220,1025.999960,81.560547}, {-551.825170,1025.999987,81.560547},
    {576.825207,479.800985,207.935547},
};
static const TVec kShootRots[7] = {
    {0,120,0}, {0,-60,0}, {0,-60,0}, {30,180,0}, {-30,180,0}, {0,-60,0}, {0,120,0},
};
static int g_trainSpawnCtr = 0;
static int g_trainDbg = 0;
static std::unordered_map<void*, ULONGLONG> g_trainLastSpawn;   // per-kiosk spawn cooldown
// DIAG: watch each spawned training ball for ~12s — did the native EventComplete handler fling it
// (GetDiscVelocity != 0) and does it actually MOVE server-side (RootComponent pos changes)?
//   vel==0 after spawn      -> the EventCompleteWithDisc handler never flung it (candidate A)
//   vel!=0 but pos static    -> flung but the free ball doesn't simulate server-side (candidate B, golf-class)
struct TrainWatch { ULONGLONG spawnedMs; SDK::FVector lastPos; };
static std::unordered_map<void*, TrainWatch> g_trainWatch;
static void TrainingTick()
{
    static SDK::UClass* kioskCls = nullptr;
    if (!kioskCls) kioskCls = SDK::UObject::FindClassFast("BP_TackleballTraining_C");
    if (!kioskCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    const ULONGLONG now = GetTickCount64();

    // Report on watched balls (vel + moved) then drop stale ones.
    for (auto it = g_trainWatch.begin(); it != g_trainWatch.end(); )
    {
        auto* ballO = static_cast<SDK::UObject*>(it->first);
        auto* ballA = static_cast<SDK::AActor*>(it->first);
        SDK::FVector vel{}; SDK::FVector pos{};
        if (ballO->Class) { auto* fnV = ballO->Class->GetFunction("DiscEntity", "GetDiscVelocity");
                            if (fnV) SafeProcessEvent(ballO, fnV, &vel); }
        if (ballA->RootComponent) pos = ballA->RootComponent->K2_GetComponentLocation();
        const double dx = pos.X - it->second.lastPos.X, dy = pos.Y - it->second.lastPos.Y, dz = pos.Z - it->second.lastPos.Z;
        HxLog("[HalcyonA2][TRAINBALL] ball=%p vel=(%.0f,%.0f,%.0f) pos=(%.0f,%.0f,%.0f) moved=%.0f\n",
              it->first, vel.X, vel.Y, vel.Z, pos.X, pos.Y, pos.Z, sqrt(dx*dx+dy*dy+dz*dz));
        it->second.lastPos = pos;
        if (now - it->second.spawnedMs > 12000) it = g_trainWatch.erase(it); else ++it;
    }
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(kioskCls)) continue;
        const uintptr_t k = reinterpret_cast<uintptr_t>(o);
        void* shooting = *reinterpret_cast<void**>(k + 0x350);
        void* goalie   = *reinterpret_cast<void**>(k + 0x358);
        void* event    = *reinterpret_cast<void**>(k + 0x360);
        void* pawn     = *reinterpret_cast<void**>(k + 0x430);            // ActivePawn (may be client-only)
        const int ballsNum = *reinterpret_cast<int*>(k + 0x440);         // ActiveBalls.Num
        const bool sEnabled = shooting && *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(shooting) + 0x478);
        const bool gEnabled = goalie   && *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(goalie)   + 0x498);

        if (g_trainDbg < 80)
        {
            HxLog("[HalcyonA2][TRAIN] %s shoot=%d goalie=%d pawn=%p balls=%d\n",
                  o->GetName().c_str(), (int)sEnabled, (int)gEnabled, pawn, ballsNum);
            ++g_trainDbg;
        }

        if (!(sEnabled || gEnabled) || !event) continue;
        if (ballsNum > 0) continue;                       // ball in flight — wait for it to resolve
        if (now - g_trainLastSpawn[o] < 2000) continue;   // per-kiosk cooldown (avoid flooding)
        g_trainLastSpawn[o] = now;

        const int idx = (g_trainSpawnCtr++) % 7;
        const TVec loc = gEnabled ? kGoalieLocs[idx] : kShootLocs[idx];
        const TVec rot = gEnabled ? TVec{0,0,0}       : kShootRots[idx];
        auto* compO = reinterpret_cast<SDK::UObject*>(gEnabled ? goalie : shooting);
        if (!compO || !compO->Class) continue;

        // RequestTrainingBallAtLocation(FVector Location, FVector Rotation) -> ADiscEntity* (return
        // @0x30). Called via ProcessEvent — the SDK .cpp wrappers aren't linked in this project.
        static SDK::UFunction* fnShootReq  = nullptr;
        static SDK::UFunction* fnGoalieReq = nullptr;
        SDK::UFunction* fnReq = nullptr;
        if (gEnabled) { if (!fnGoalieReq) fnGoalieReq = compO->Class->GetFunction("GoalieTrainingComponent",  "RequestTrainingBallAtLocation"); fnReq = fnGoalieReq; }
        else          { if (!fnShootReq ) fnShootReq  = compO->Class->GetFunction("ShootingTrainingComponent", "RequestTrainingBallAtLocation"); fnReq = fnShootReq;  }
        if (!fnReq) continue;
        struct { double lx, ly, lz, rx, ry, rz; void* ret; } rp{};
        rp.lx = loc.x; rp.ly = loc.y; rp.lz = loc.z;
        rp.rx = rot.x; rp.ry = rot.y; rp.rz = rot.z;
        if (!SafeProcessEvent(compO, fnReq, &rp)) continue;
        void* ball = rp.ret;
        HxLog("[HalcyonA2][TRAIN] spawn %s idx=%d ball=%p on %s\n",
              gEnabled ? "goalie" : "shoot", idx, ball, o->GetName().c_str());
        if (!ball) continue;

        // Fire EventCompleteWithDiscArgument so the BP's server shooting/goalie logic runs.
        auto* evtO = reinterpret_cast<SDK::UObject*>(event);
        if (!evtO->Class) continue;
        static SDK::UFunction* fnEvtDone = nullptr;
        if (!fnEvtDone) fnEvtDone = evtO->Class->GetFunction("EventComponent", "EventCompleteWithDiscArgument");
        if (fnEvtDone) { struct { void* Disc; } ep{ ball }; SafeProcessEvent(evtO, fnEvtDone, &ep); }

        // Watch this ball to see if the native handler flings it + whether it moves server-side.
        SDK::FVector p0{};
        if (auto* ra = static_cast<SDK::AActor*>(ball)->RootComponent) p0 = ra->K2_GetComponentLocation();
        g_trainWatch[ball] = { now, p0 };
    }
}
static void SafeTrainingTick() { __try { TrainingTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// QUESTS — register each player with the server progression system. On our headless server a
// player's UA2PlayerQuestComponent never gets initialized (IsInitialized@0x140 stays 0) and is
// never inserted into UServerProgression.PlayerInstances, so quest completion/progress fails
// ("PlayerInstances does not contain Component") and the kiosk shows the placeholder subquest.
// Server_SetQuestProgressionAndInitializeQuests(FA2QuestProgression) = the register+init: it
// stores the progression, sets IsInitialized=1, and inserts the component into PlayerInstances
// (confirmed in sub_4680E50). It does NOT need a pre-loaded quest table to register. We call it
// with an EMPTY FA2QuestProgression (no saved progress) on each not-yet-initialized component.
// It's a NetServer RPC but our pawn has authority, so ProcessEvent runs the impl locally.
// The Server RPC wrapper (sub_46851C0) bails unless it can resolve the world + UServerProgression
// at call time, which is flaky on our server (real players stayed IsInitialized=0). So skip the
// RPC and call the register worker directly: sub_4680E50(UServerProgression, component,
// FA2QuestProgression*) unconditionally sets IsInitialized=1 and inserts the component into
// PlayerInstances@0x2B0. We resolve UServerProgression ourselves and pass an empty progression.
// [22284] The register worker MOVED: 20996 sub_4680E50 is USandboxStatsStore::StaticClass() on 22284
// (calling it = the VR-join hard-exit/fast-fail). Re-verified in IDA via the reflected RPC:
// execServer_SetQuestProgressionAndInitializeQuests (RVA 0x4695EC0) -> _Impl (RVA 0x4690260, vtable
// idx 150) -> resolves UServerProgression -> calls the register worker **sub_7FF6766EBE60 = RVA
// 0x468BE60**. Confirmed it stores prog->comp+0x120, sets IsInitialized (comp+0x140)=1, and inserts
// the component into PlayerInstances (sp+0x2B0, 0x180-stride) — same behavior/offsets as 20996.
static void SafeRegisterQuest(void* sp, void* comp, void* prog)
{
    __try { reinterpret_cast<void(__fastcall*)(void*, void*, void*)>(GetBase() + 0x468BE60)(sp, comp, prog); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// TEMP PROOF: seed the register with drychee's 3 real completed quests (decoded from the Mothership
// player_quests blob) instead of an empty progression, so registering doesn't CLOBBER the client's
// saved progress. Hardcoded for now — generalize to a per-player Mothership fetch once this proves
// the seed fixes the clobber + lights the kiosk. FA2QuestProgression{LastUpdate@0, Quests TArray@0x8,
// Version@0x18}; FA2QuestStorage(0x20){FGuid ID@0, uint8 Progress@0x10, int CompletedVersion@0x14,
// FDateTime CompletedTime@0x18}. UE struct-assign (the register's copy) deep-copies the TArray, so a
// static source buffer is safe + reusable across players.
static unsigned char g_seedQuests[3 * 0x20];
static unsigned char g_seedProg[0x20];
static bool g_seedBuilt = false;
static void ParseGuid32(const char* h, unsigned char* out16)
{
    auto hx = [](char c) -> unsigned { c |= 0x20; return (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : 0; };
    for (int g = 0; g < 4; ++g)
    {
        unsigned v = 0;
        for (int k = 0; k < 8; ++k) v = (v << 4) | hx(h[g * 8 + k]);
        *reinterpret_cast<unsigned*>(out16 + g * 4) = v;   // FGuid A,B,C,D as uint32
    }
}
static void BuildSeedProgression()
{
    static const char* kGuids[3] = {
        "7BB295444C8A86C232F1709A8FA6A34B",
        "418A44014B5FBF10071D49B7DA319A25",
        "59259FE944A3EEB3123D5EBA6DBEEB71",
    };
    memset(g_seedQuests, 0, sizeof(g_seedQuests));
    for (int i = 0; i < 3; ++i)
    {
        unsigned char* q = g_seedQuests + i * 0x20;
        ParseGuid32(kGuids[i], q + 0);               // FGuid ID
        q[0x10] = 255;                               // Progress
        *reinterpret_cast<int*>(q + 0x14) = 1;       // CompletedVersion
    }
    memset(g_seedProg, 0, sizeof(g_seedProg));
    *reinterpret_cast<void**>(g_seedProg + 0x8)  = g_seedQuests;   // Quests.Data
    *reinterpret_cast<int*>  (g_seedProg + 0x10) = 3;              // Quests.Num
    *reinterpret_cast<int*>  (g_seedProg + 0x14) = 3;              // Quests.Max
    *reinterpret_cast<int*>  (g_seedProg + 0x18) = 3;              // SavedQuestProgressionVersion
    g_seedBuilt = true;
}

// ---- REAL per-player progression, fetched from Mothership -------------------
// Feeding a player anything other than their real progression makes the client refuse to
// init the quest UI (it compares its own Mothership fetch against what the server feeds and
// bails on mismatch). So we fetch each joiner's real player_quests and register with THAT.
struct FetchedQuest { unsigned char guid[16]; unsigned char progress; int completedVersion; };

static std::mutex g_qMx;
static std::unordered_map<std::string, std::vector<FetchedQuest>> g_qReady;  // orgId -> quests
static std::unordered_set<std::string> g_qInflight;
static std::vector<std::string> g_qQueue;   // FIFO of ready orgIds not yet assigned to a component
                                            // (interim correlation; exact per-player mapping via the
                                            // FetchUserRoles caller is a TODO — fine for single-join)

static std::vector<FetchedQuest> ParseQuestsJson(const std::string& json)
{
    std::vector<FetchedQuest> out;
    size_t pos = 0;
    const std::string idKey = "\"iD\"";
    while (true)
    {
        size_t k = json.find(idKey, pos);
        if (k == std::string::npos) break;
        size_t colon = json.find(':', k + idKey.size());
        size_t s1 = (colon == std::string::npos) ? std::string::npos : json.find('"', colon);
        size_t s2 = (s1 == std::string::npos) ? std::string::npos : json.find('"', s1 + 1);
        if (s2 == std::string::npos) break;
        std::string idStr = json.substr(s1 + 1, s2 - s1 - 1);
        pos = s2 + 1;
        if (idStr.size() != 32) continue;
        FetchedQuest fq{};
        ParseGuid32(idStr.c_str(), fq.guid);
        fq.progress = (unsigned char)ExtractJsonInt(json, "progress", s2);
        fq.completedVersion = ExtractJsonInt(json, "completedVersion", s2);
        out.push_back(fq);
    }
    return out;
}

// Kick a one-shot background fetch for an org id (HTTP is blocking; never do it on the game thread).
static void KickQuestFetch(const std::string& orgId)
{
    if (orgId.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_qMx);
        if (g_qInflight.count(orgId) || g_qReady.count(orgId)) return;
        g_qInflight.insert(orgId);
    }
    std::thread([orgId]() {
        std::string json = FetchPlayerQuestsJson(orgId);
        std::vector<FetchedQuest> quests = ParseQuestsJson(json);
        {
            std::lock_guard<std::mutex> lk(g_qMx);
            g_qReady[orgId] = quests;
            g_qInflight.erase(orgId);
            g_qQueue.push_back(orgId);
        }
        HxLog("[HalcyonA2][QUEST] fetched %zu real quests for org %s\n", quests.size(), orgId.c_str());
    }).detach();
}

// Build an FA2QuestProgression in a reusable static buffer (rebuilt per call, so a move-assign in
// the register worker can't poison later registers).
static unsigned char g_realQBuf[4096 * 0x20];
static unsigned char g_realQProg[0x20];
static void* BuildProgFromFetched(const std::vector<FetchedQuest>& qs)
{
    int n = (int)qs.size();
    if (n > 4096) n = 4096;
    memset(g_realQBuf, 0, (size_t)n * 0x20);
    for (int i = 0; i < n; ++i)
    {
        unsigned char* q = g_realQBuf + (size_t)i * 0x20;
        memcpy(q + 0x00, qs[i].guid, 16);      // FGuid ID
        q[0x10] = qs[i].progress;              // Progress
        *reinterpret_cast<int*>(q + 0x14) = qs[i].completedVersion;
    }
    memset(g_realQProg, 0, sizeof(g_realQProg));
    *reinterpret_cast<void**>(g_realQProg + 0x08) = g_realQBuf;   // Quests.Data
    *reinterpret_cast<int*>  (g_realQProg + 0x10) = n;            // Num
    *reinterpret_cast<int*>  (g_realQProg + 0x14) = n;            // Max
    *reinterpret_cast<int*>  (g_realQProg + 0x18) = 3;            // SavedQuestProgressionVersion
    return g_realQProg;
}

// ===== Backend auth join-gate (Tier 1) =======================================================
// A connecting player must correspond to a live authed session in OUR backend, or we kick them.
// The backend (MothershipServer._activeSessions) marks a session active ONLY on a real client
// login (Quest attestation / RIFT); a pirate/standalone client that skipped our backend isn't
// active -> kicked. Design rules:
//   * FAIL-OPEN — any network/parse failure leaves the org "unknown" and never kicks (a backend
//     blip must not mass-kick legit players).
//   * REMOTE-ONLY — only controllers with a NetConnection and !bIsLocalPlayerController; never the
//     client-as-server's own host controller.
//   * LOG-ONLY until g_gateEnforce is set true, so we can watch the [GATE] lines confirm real
//     players resolve authorized=1 before any kick goes live.
// Kick primitive = sub_5333610(pc) (the controller's return-to-menu virtual; see IDA notes).
// [PORT-AUDIT] The 20996 kick helper (0x5333610 = pc->vtbl[77](pc, FText("Kick"), 0), reached from Server_KickPlayer) does not
// exist on 22284 (0x5333610 is now +0x10 inside an unrelated 0x2F-byte fn; the Kick UFunctions are gone from the SDK).
// Calling it corrupted the stack on the first enforced kick. Kick through the engine RPC instead: the client returns to
// the main menu and drops the connection.
static void KickPcViaRpc(uintptr_t pc)
{
    auto* p = reinterpret_cast<SDK::APlayerController*>(pc);
    SDK::FString reason(L"Kicked by server");
    p->ClientReturnToMainMenuWithTextReason(SDK::UKismetTextLibrary::Conv_StringToText(reason));
}
static void SafeKickPc(uintptr_t pc) { __try { KickPcViaRpc(pc); } __except (EXCEPTION_EXECUTE_HANDLER) { HxLog("[HalcyonA2][GATE] kick RPC faulted pc=%p\n", (void*)pc); } }

// [TESTING] -NoAuthGate drops this to log-only. A client that never completes the dashboard/
// mothership auth handshake (e.g. the -HalcyonClient mock client, or any direct IP join used for
// testing) is otherwise kicked by AuthGateTick with "KICK no-auth-timeout" ~30s after joining,
// which is exactly what ended the first end-to-end ball-sim/VOIP test run.
static bool g_gateEnforce = true;    // PROD: validated — legit players resolve authorized=1 within ~11s; a never-authed IP-join never populates org and is kicked after the grace window
static const ULONGLONG kGateGraceMs = 30000;   // a joining client gets this long to complete dashboard login + backend authorize before an empty org == kick (real logins land ~11s)
static ULONGLONG g_lastAuthGate = 0;
static std::mutex g_authMx;
static std::unordered_map<std::string, int> g_authState;   // org -> 0 unknown, 1 authorized, 2 unauthorized
static std::unordered_map<std::string, ULONGLONG> g_authCheckedAt;   // org -> last authorized-check completion (ms); lets a cached state==2 be re-checked (first-login race)
static std::unordered_set<std::string> g_authInflight;
static std::unordered_set<std::string> g_gateKicked;       // orgs already actioned (no repeat)
static std::unordered_map<void*, ULONGLONG> g_connFirstSeen;   // connection ptr -> first tick we saw it (grace clock)
static std::unordered_set<void*> g_connKicked;                 // connections already actioned (no repeat)

// Fire a one-shot background check of an org id against /v1/server/authorized (HTTP blocks; keep it
// off the game thread, same as KickQuestFetch). Leaves state 0 (unknown) on any failure = fail-open.
static void CheckAuthorized(const std::string& orgId)
{
    if (orgId.empty()) return;
    {
        std::lock_guard<std::mutex> lk(g_authMx);
        if (g_authInflight.count(orgId)) return;
        auto it = g_authState.find(orgId);
        if (it != g_authState.end() && it->second != 0) return;   // already resolved
        g_authInflight.insert(orgId);
    }
    std::thread([orgId]() {
        const std::wstring hdr = L"x-server-api-key: " + std::wstring(kServerApiKey) + L"\r\n";
        const std::wstring wid(orgId.begin(), orgId.end());   // org id is ASCII (Meta id / GUID)
        const std::wstring path = L"/v1/server/authorized?id=" + wid;
        DWORD code = 0;
        const std::string resp = HttpReq(kMotherHost, kMotherPort, L"GET", path.c_str(), "", hdr, &code);
        int state = 0;   // unknown -> fail-open, retried next pass
        if (code == 200)
        {
            if (resp.find("\"authorized\":true")  != std::string::npos ||
                resp.find("\"authorized\": true") != std::string::npos)  state = 1;
            else if (resp.find("\"authorized\":false")  != std::string::npos ||
                     resp.find("\"authorized\": false") != std::string::npos) state = 2;
        }
        {
            std::lock_guard<std::mutex> lk(g_authMx);
            g_authInflight.erase(orgId);
            if (state != 0) { g_authState[orgId] = state; g_authCheckedAt[orgId] = GetTickCount64(); }   // stamp so a cached 2 can be refreshed
        }
        HxLog("[HalcyonA2][GATE] authorized check org=%s http=%lu state=%d\n",
              orgId.c_str(), (unsigned long)code, state);
    }).detach();
}

// Turn up to N raw bytes into "aabbcc.." for identity-format discovery in the log.
static std::string HexBytes(const uint8_t* p, int n)
{
    static const char* H = "0123456789abcdef";
    std::string s; s.reserve(n * 2);
    for (int i = 0; i < n && i < 64; ++i) { s.push_back(H[p[i] >> 4]); s.push_back(H[p[i] & 0xF]); }
    return s;
}

// Game-thread pass (~1s): enumerate the netdriver's ClientConnections — the AUTHORITATIVE list of
// connected clients (VR AND spectators, class-agnostic; conn+0x30 = PlayerController). Each remote
// client is gated on org@PC+0xA30, which is populated ONLY from the dashboard backend's validated
// login response (LoginWithKeyResponseHandler) — NOT the client's raw JWT — so it is not forgeable
// and a never-authed IP-join never gets one. Empirically (logs 2026-08-20): a legit Quest/RIFT player
// completes the dashboard login ~11s after connecting -> org populates -> /v1/server/authorized
// returns state=1; a client that opened the game, skipped auth, and joined by IP keeps org=='' the
// whole session (its dashboard login is rejected server-side). Decision, FAIL-CLOSED after a grace
// window:
//   org present + state==1 -> keep (authed)
//   org present + state==2 -> kick (id is not an authed backend session)
//   org empty  + age > kGateGraceMs -> kick (never authenticated through us — the IP-bypass case)
//   otherwise -> wait (within grace / backend still resolving) = fail-open, no false kicks.
// Kick primitive = sub_5333610(pc); once-per-connection via g_connKicked. Set g_gateDiscover to dump
// the raw connection id bytes for debugging.
static bool g_gateDiscover = false;  // optional per-connection id dump (org@0xA30 is the anchor; flip on only to debug)
// The dashboard/game player count comes from the EOS session we return, whose PublicPlayers list is
// only mutated by the CLIENT (join = POST, leave = DELETE). A HARD disconnect (quit/crash/network
// drop) never sends the leave, so ghosts accumulate and the count never drops. The AUTHORITY on who's
// actually connected is this gameserver's netdriver — ClientConnections.Num() drops the instant UE
// tears down a closed connection. So periodically report the real count to the backend, which
// reconciles the session's player count down to it. ~3-minute cadence (the count is not time-critical).
static void PlayerCountReportTick()
{
    if (g_deploymentId.empty()) return;   // not registered yet / manual launch without an id

    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->NetDriver) return;
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase) { imgBase = GetBase(); auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
                    auto* nth = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                    imgSize = nth->OptionalHeader.SizeOfImage; }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);
    if (vt < imgBase || vt >= imgBase + imgSize) return;          // not a real in-image netdriver yet
    int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8);    // ClientConnections.Num() (may be 0)
    if (numConns < 0) numConns = 0;

    // Fire-and-forget so a remote POST never stalls the game thread. Capture by value.
    const std::string dep  = g_deploymentId;
    const int         cnt  = numConns;
    std::thread([dep, cnt]() {
        std::string body = "{\"deployment_id\":\"" + dep + "\",\"player_count\":" + std::to_string(cnt) + "}";
        HttpPostLocal(kBackendHost, kBackendPort, L"/update_player_count", body);
    }).detach();
    printf("[HalcyonA2] player-count report: deployment=%s count=%d\n", g_deploymentId.c_str(), numConns);
}
static void SafePlayerCountReportTick() { __try { PlayerCountReportTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

static void AuthGateTick()
{
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->NetDriver) return;
    const uintptr_t nd = reinterpret_cast<uintptr_t>(world->NetDriver);

    // same validity gate as TuneNetDriver: prove it's a real in-image UNetDriver before dereferencing.
    static uintptr_t imgBase = 0, imgSize = 0;
    if (!imgBase) { imgBase = GetBase(); auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imgBase);
                    auto* nth = reinterpret_cast<IMAGE_NT_HEADERS*>(imgBase + dos->e_lfanew);
                    imgSize = nth->OptionalHeader.SizeOfImage; }
    const uintptr_t vt = *reinterpret_cast<uintptr_t*>(nd);
    if (vt < imgBase || vt >= imgBase + imgSize) return;
    void** conns = *reinterpret_cast<void***>(nd + 0xD0);            // ClientConnections.Data
    const int32_t numConns = *reinterpret_cast<int32_t*>(nd + 0xD8); // ClientConnections.Num()
    if (!conns || numConns <= 0) return;

    const ULONGLONG nowMs = GetTickCount64();
    std::unordered_set<void*> live;   // connections seen this pass, for stale-entry cleanup

    // action a connection: kick (once) if enforcing, else log the would-kick. `why` names the reason.
    auto doKick = [&](void* conn, uintptr_t pc, const char* why, const std::string& org)
    {
        if (g_connKicked.count(conn)) return;   // already actioned this connection
        g_connKicked.insert(conn);
        if (g_gateEnforce)
        {
            HxLog("[HalcyonA2][GATE] KICK %s conn=%p pc=%p org='%s'\n", why, conn, (void*)pc, org.c_str());
            SafeKickPc(pc);   // [PORT-AUDIT] was a raw call into 0x5333610 (mid-function on 22284)
        }
        else
            HxLog("[HalcyonA2][GATE] would-kick (log-only) %s conn=%p pc=%p org='%s'\n", why, conn, (void*)pc, org.c_str());
    };

    for (int32_t c = 0; c < numConns && c < 64; ++c)
    {
        const uintptr_t conn = reinterpret_cast<uintptr_t>(conns[c]);
        if (!conn) continue;
        const uintptr_t pc = *reinterpret_cast<uintptr_t*>(conn + 0x30);   // UNetConnection.PlayerController
        if (!pc) continue;
        if (*reinterpret_cast<bool*>(pc + 0x6C4)) continue;                // bIsLocalPlayerController -> host, never touch
        live.insert(reinterpret_cast<void*>(conn));

        // grace clock: first tick we saw this connection (covers VR + spectators identically).
        auto fs = g_connFirstSeen.find(reinterpret_cast<void*>(conn));
        if (fs == g_connFirstSeen.end()) fs = g_connFirstSeen.emplace(reinterpret_cast<void*>(conn), nowMs).first;
        const ULONGLONG ageMs = nowMs - fs->second;

        // org@PC+0xA30 is populated ONLY from the dashboard backend's validated login response — it is
        // not client-forgeable, and a never-authed IP-join never gets one. So it's the gate anchor.
        std::string org = FStringToNarrow(reinterpret_cast<void*>(pc + 0xA30));

        if (g_gateDiscover)   // optional raw-id dump (off by default; org@0xA30 is the real anchor)
        {
            uint8_t* rb   = *reinterpret_cast<uint8_t**>(conn + 0x160 + 0x20);    // PlayerId.ReplicationBytes.Data
            const int32_t rbn = *reinterpret_cast<int32_t*>(conn + 0x160 + 0x28); // .Num
            HxLog("[HalcyonA2][GATE-ID] conn=%p pc=%p org='%s' age=%llums replBytes(n=%d)=%s\n",
                  (void*)conn, (void*)pc, org.c_str(), (unsigned long long)ageMs, rbn,
                  (rb && rbn > 0) ? HexBytes(rb, rbn).c_str() : "<none>");
        }

        if (org.empty())
        {
            // legit clients populate org within ~11s of connecting (dashboard login round-trip). If it's
            // still empty past the grace window, this player never authenticated through us -> kick.
            if (ageMs > kGateGraceMs)
                doKick(reinterpret_cast<void*>(conn), pc, "no-auth-timeout", org);
            continue;                                            // otherwise still within grace -> wait
        }

        int state = 0;
        {
            std::lock_guard<std::mutex> lk(g_authMx);
            auto it = g_authState.find(org);
            if (it != g_authState.end()) state = it->second;
        }
        if (state == 1) continue;                                // authorized backend session -> keep
        if (state == 0) { CheckAuthorized(org); continue; }      // resolving -> fail-open, (re)check next pass
        // state == 2: NOT sticky. Mothership persists a first-login player's session slightly AFTER the
        // dashboard login that populated `org`, so our first check can race to `false`. Re-check on a
        // short TTL (so a now-authed player flips to state=1 and is kept) and only kick if STILL
        // unauthorized past the grace window — never on a single early false.
        {
            ULONGLONG lastChk = 0;
            { std::lock_guard<std::mutex> lk(g_authMx); auto ca = g_authCheckedAt.find(org); if (ca != g_authCheckedAt.end()) lastChk = ca->second; }
            if (nowMs - lastChk > 5000) CheckAuthorized(org);    // refresh the cached 2
        }
        if (ageMs > kGateGraceMs)
            doKick(reinterpret_cast<void*>(conn), pc, "unauthorized", org);   // still unauthorized after 30s + re-checks
    }

    // drop tracking for connections that have gone away (also lets a reused conn ptr re-arm cleanly).
    if (g_connFirstSeen.size() > live.size())
        for (auto it = g_connFirstSeen.begin(); it != g_connFirstSeen.end(); )
            if (!live.count(it->first)) { g_connKicked.erase(it->first); it = g_connFirstSeen.erase(it); }
            else ++it;
}
static void SafeAuthGateTick() { __try { AuthGateTick(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// Hook A2Station__FetchUserRoles (RVA 0x54B04E0): the game calls it for every joining player with
// a1 = that player's Oculus org-scoped id (FString). We use it to warm the quest fetch AND to kick
// off the backend authorized-check for the join gate.
using FetchRoles_t = __int64(__fastcall*)(__int64, __int64);
static FetchRoles_t FetchRoles_Orig = nullptr;
static __int64 __fastcall FetchRoles_Hook(__int64 a1, __int64 a2)
{
    std::string orgId = FStringToNarrow(reinterpret_cast<void*>(a1));
    if (!orgId.empty())
    {
        HxLog("[HalcyonA2][QUEST] roles fetch for org %s -> warming quest fetch\n", orgId.c_str());
        KickQuestFetch(orgId);
        CheckAuthorized(orgId);     // join-gate: resolve whether this player authed with our backend
    }
    return FetchRoles_Orig ? FetchRoles_Orig(a1, a2) : 0;
}
static constexpr uintptr_t FetchRoles_RVA = 0x54B04E0;

static void InitPlayerQuests()
{
    static SDK::UClass* qcCls = nullptr;
    static SDK::UClass* spCls = nullptr;
    if (!qcCls) qcCls = SDK::UObject::FindClassFast("A2PlayerQuestComponent");
    if (!spCls) spCls = SDK::UObject::FindClassFast("ServerProgression");
    if (!qcCls || !spCls) return;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting

    // resolve the live UServerProgression subsystem
    void* sp = nullptr;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (o && !o->IsDefaultObject() && o->IsA(spCls)) { sp = o; break; }
    }
    if (!sp) return;   // subsystem not up yet — retry next pass

    int seen = 0, registered = 0, skippedInit = 0;
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(qcCls)) continue;
        // skip template/placeholder components (owned by a class-default pawn)
        if (!o->Outer || o->Outer->IsDefaultObject()) continue;
        ++seen;
        const uintptr_t p = reinterpret_cast<uintptr_t>(o);
        const unsigned char init0 = *reinterpret_cast<unsigned char*>(p + 0x140);
        if (init0) { ++skippedInit; continue; }   // already IsInitialized

        // Exact per-player correlation: read the org-scoped id straight off the player.
        //   component -> Outer(pawn) -> Controller(APawn+0x2D0) -> org id FString(VRPC+0xA30)
        // The org is stored there by AVRPlayerController::Server_LoginToStationDashboard -> sub_54AE2E0.
        // The local host's PC never gets an org stored, so it's naturally skipped (org == "").
        void* pawn = o->Outer;
        void* pc   = pawn ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(pawn) + 0x2D0) : nullptr;
        std::string org = pc ? FStringToNarrow(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(pc) + 0xA30)) : "";
        HxLog("[HalcyonA2][QUEST] uninit comp %s pawn %s pc %s org '%s'\n",
              o->GetName().c_str(),
              pawn ? o->Outer->GetName().c_str() : "<null>",
              pc   ? reinterpret_cast<SDK::UObject*>(pc)->GetName().c_str() : "<null>",
              org.c_str());
        if (org.empty()) continue;   // dashboard login not applied yet (or local host) — wait/skip

        // Fetch this player's real progression once; register only when it's ready (feeding
        // wrong/empty data makes the client refuse to init the quest UI).
        std::vector<FetchedQuest> quests; bool ready = false;
        {
            std::lock_guard<std::mutex> lk(g_qMx);
            auto it = g_qReady.find(org);
            if (it != g_qReady.end()) { quests = it->second; ready = true; }
        }
        if (!ready) { KickQuestFetch(org); continue; }   // kick (once) then register next pass

        void* prog = BuildProgFromFetched(quests);
        SafeRegisterQuest(sp, o, prog);
        ++registered;
        HxLog("[HalcyonA2][QUEST] register %s (pc %s) with %zu REAL quests IsInitialized ->%d\n",
               o->GetName().c_str(),
               reinterpret_cast<SDK::UObject*>(pc)->GetName().c_str(), quests.size(),
               *reinterpret_cast<unsigned char*>(p + 0x140));
    }
    if (registered)
        HxLog("[HalcyonA2][QUEST] pass: %d qcomp(s), %d registered, %d already-init\n",
               seen, registered, skippedInit);
}
static void SafeInitPlayerQuests() { __try { InitPlayerQuests(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }

// ---- QUEST TRACE ----------------------------------------------------------
// Does the GAME itself call the quest-init/progress paths on join (just gated/failing), or never?
// Hook the impls + workers and log every call (with our own poking disabled, any hit = the game).
//   0x4690260 UA2PlayerQuestComponent::Server_SetQuestProgressionAndInitializeQuests_Impl(comp, prog)
//   0x46902F0 UA2PlayerQuestComponent::Server_SetQuests_Impl(comp, bundle, bool removing)
//   0x4680E50 register+init worker(sp, comp, prog)  [the guts of ...InitializeQuests]
//   0x4685C30 UServerProgression::SetProgress(sp, pawn, FGuid*, uint8)
using QInit_t = __int64(__fastcall*)(void*, void*);
using QSetQ_t = __int64(__fastcall*)(void*, void*, unsigned char);
using QReg_t  = __int64(__fastcall*)(void*, void*, void*);
using QSetP_t = __int64(__fastcall*)(void*, void*, void*, unsigned char);
static QInit_t QInit_Orig = nullptr;
static QSetQ_t QSetQ_Orig = nullptr;
static QReg_t  QReg_Orig  = nullptr;
static QSetP_t QSetP_Orig = nullptr;
static __int64 __fastcall QInit_Hook(void* comp, void* prog)
{
    printf("[HalcyonA2][QTRACE] Server_SetQuestProgressionAndInitializeQuests comp=%p prog=%p\n", comp, prog);
    return QInit_Orig(comp, prog);
}
static __int64 __fastcall QSetQ_Hook(void* comp, void* bundle, unsigned char rem)
{
    printf("[HalcyonA2][QTRACE] Server_SetQuests comp=%p bundle=%p removing=%d\n", comp, bundle, (int)rem);
    return QSetQ_Orig(comp, bundle, rem);
}
static __int64 __fastcall QReg_Hook(void* sp, void* comp, void* prog)
{
    printf("[HalcyonA2][QTRACE] RegisterWorker(4680E50) sp=%p comp=%p prog=%p\n", sp, comp, prog);
    return QReg_Orig(sp, comp, prog);
}
static __int64 __fastcall QSetP_Hook(void* sp, void* pawn, void* guid, unsigned char prog)
{
    printf("[HalcyonA2][QTRACE] ServerProgression::SetProgress sp=%p pawn=%p progress=%d\n", sp, pawn, (int)prog);
    return QSetP_Orig(sp, pawn, guid, prog);
}

// Texture-streaming thunk sub_54ABCC0(a1): this = *(a1+0x890); return (*this->vtable[0x600/8])(this).
// On -nullrhi that render/streaming resource is NULL => original null-derefs (the long-standing
// random crash). Guard it: skip when null, otherwise pass through.
using StreamThunk_t = __int64(__fastcall*)(__int64);
static StreamThunk_t StreamThunk_Orig = nullptr;
static __int64 __fastcall StreamThunk_Hook(__int64 a1)
{
    if (!a1 || *reinterpret_cast<void**>(a1 + 0x890) == nullptr) return 0;
    return StreamThunk_Orig(a1);
}

// [PORT 22284] Boot-spine gate: when true, ProcessEvent_Hook is a pure pass-through so NONE of the
// ballsim pumps / subsystem one-shots / tickers run (they use offsets not yet runtime-verified for
// 22284). Net mode + station self-init don't need the PE handler. Flip to false to re-arm them.
static bool g_bootSpineOnly = false;  // [22284] boot validated -> PE handler + tickers re-armed
// [PORT 22284] The jakeball rollback ballsim (authority hook + pump + velocity inject) uses 20996
// struct offsets (mgr+0x412, VRPawn+0x1B38, ball-state strides) that need re-verification, AND making
// the sim IsServer:1 triggers the native server-build which poisons on our headless -2 template sim.
// Keep it OFF until the ballsim offsets are ported; everything else (Vivox/roles/quests/tickets/golf)
// runs fine without it.
static bool g_ballsimEnabled = true;  // [2026-09-04 ★ SWITCH BACK TO HAND-SPAWN] User request / 20996 parity:
                                      // hand-spawn the manager ourselves + wire mgr+0x2F0 (OfflineBallSimSubsystem)
                                      // BEFORE BeginPlay, exactly like the working 20996 build. When true, the native
                                      // spawn-gate patch (0x53DC76A) is NOT applied (see boot), so the game does NOT
                                      // also spawn one -> exactly one (ours). Flood contained by sweep+batch; seater
                                      // drive + dirty-flag seat fix still apply to whichever manager g_ballSimMgr is.
                                      // (prior native-spawn rationale kept below for revert:)
                                      // OFF — PATCH the game's own spawner gate (0x53DC768 cmp eax,2->1) so the game
                                      // spawns+wires+ticks the manager ITSELF on dedicated, like offline (netmode 0).
static bool g_ballsimEnabled_unused = true;  // [2026-09-01] ON: manager spawns; BuildSim_Hook drops phantom-only
                                      // sim builds (the GC-flood garbage source), so enabling is now safe.
                                      // [22284] STABLE server. Jakeball needs the 20996 admission/seat
                                      // redo (stepSim is client-local; phantom player has no valid sim ->
                                      // GC flood). Everything else works: net-mode, stations, Vivox,
                                      // roles, teams, tickets, golf. PlayerIndex fix (0x1B48->0x1C22) is
                                      // in and helps goals/team-colors regardless.
static bool g_ballPumpsEnabled = true;  // [2026-09-07 ON by default] The helper's pointer ("the sim isn't
                                      // pumping — needs manually ticked, make sure sim pump is turned on") is right:
                                      // PumpBallSimStep is the ONLY thing that steps the rollback sim in sync with
                                      // clients (input-delay -> results-send). Enabling it now stays STABLE for 90s+
                                      // with clients joined (the old -1-deref/GC-flood crash reason no longer applies:
                                      // BuildSim_Hook drops phantom-only builds and the GC Num-clamps contain the rest).
                                      // The pump is now STUTTER-SAFE (PumpBallSimStep only arms the results-send while
                                      // stepping toward a CONFIRMED-input frame; idle/no-participant balls tick natively
                                      // with no force-send), so the worst case with 0 participants is the old smooth
                                      // pump-off behaviour, never the pin stutter. -NoBallPump forces it back off.
                                      // NOTE (measured 2026-09-07): arena sims report participants=0 for a mock client
                                      // even though its disc PASSES the seat gate, so frameAdvViaStep only climbs once a
                                      // player is a real sim PARTICIPANT (watch [SIMPART] participants>0 + [MI] stepped>0).
                                      // ---- (rationale for wanting it ON kept below:)
                                      // [2026-09-03 ★ RE-ENABLED] The native tick reconciles/prunes but does
                                      // NOT actually advance the active rollback sim on our headless setup:
                                      // [STEP] frameAdvViaStep stays 0 and balls never react to hits. The
                                      // premise for OFF ("don't double-drive the native tick") was moot anyway —
                                      // every pump early-returns on !g_ballSimMgr (dllmain ~2054/6601), and
                                      // g_ballSimMgr was NULL the whole time (hand-spawn that set it is disabled),
                                      // so the pumps have been dead no-ops regardless. Now that EnsureNativeBallSimMgr
                                      // latches g_ballSimMgr onto the live native manager, turn the pumps ON so
                                      // SafePumpBallSimStep (the ONLY thing that moves the server ball actor / steps
                                      // the sim ~90Hz) runs and player hits get processed. Flood risk is now
                                      // contained by the GC sweep + batch-hook; BuildSim_Hook still drops phantom-only
                                      // builds. If it re-floods, back off SafePumpBallSimBuild first (keep the step).
// [MIFIX 2026-09-07 — opt-in, DEFAULT OFF] The high MI (user reports MIB~44 with >1 player) is sparse
// sim-result confirmations: with the mgr+0x412 results-send pin OFF (it was disabled because pinning it
// every frame made an IDLE ball's authoritative snapshot fight bReplicateMovement = the +894/-894 stutter
// the user later confirmed fixed), the GAME advances the sim on input ingest but nothing arms the send,
// so clients get confirmations far below the step rate and over-predict -> MI balloons past the real RTT.
// This lever arms the send in StepSim_Hook ONLY when a contested sim actually ADVANCED a frame this step
// (after>before) AND a participant submitted input to that sim within 500ms. An IDLE ball's sim does not
// advance without inputs, so it is never armed -> the stutter source stays untouched. Left OFF by default
// so it can't regress the confirmed-fixed stutter unverified; the user enables -ArmResultsOnStep to test
// with real players and watch MI fall. (mgr+0x412 is manager-global, so if a contested arena and a
// separate idle ball coexist, arming for the arena step also flushes the idle ball's results once per
// contested step — real-player testing is needed to confirm that cadence doesn't reintroduce any stutter,
// which is exactly why this is opt-in rather than on.)  (the flag itself is declared up by StepSim_Hook.)

static bool g_neuterStepSim  = false; // [22284] TEST DONE: neuter -> flood gone but balls FROZE. stepSim
                                      // moves balls AND builds garbage, and is entirely local-player-
                                      // centric (no separable physics step). Jakeball needs the 20996
                                      // admission/seat redo on 22284, not a stepSim patch. Left off.
static bool g_forceAuthSim   = true;  // [2026-09-04 ★★★ RE-ENABLED — THE 20996 HITTABLE-BALLS FIX] Memory
                                      // a2-match-state-and-ball-physics: A2's rollback sim decides authority
                                      // via GetNetMode()==NM_ListenServer(2), NOT ==1. We force netmode 1, so
                                      // EVERY ==2 ballsim authority check FAILS -> the sim runs NON-authoritative
                                      // (IsServer:0 client-prediction only) -> remote client hits NEVER reach the
                                      // server sim -> balls not hittable. PatchBallSimNetModeChecks (==2->==1 across
                                      // the ballsim cluster) + BallSimGetWorld_Hook are the exact 20996 fix. It was
                                      // turned OFF only for the GC-flood bisection ("does spawning alone flood?");
                                      // the flood is now contained by the reflection sweep + batch-hook, so turn the
                                      // authority forcing back ON. This is what makes the server APPLY the hit.
                                      // If the flood returns, it's contained (GC-GUARD short), not fatal.

// Resolve the FName idxs of the RPCs we DROP in ProcessEvent. Separate fn: GetFunction() builds
// std::string temporaries that C++-unwind, which cannot live in ProcessEvent_Hook's __try scope (C2712).
static void ResolveDropIdxs()
{
    // ROOT fix (user's call): block AVRPawn::Server_AttemptTakeOwnershipOfSpectatorManager so the
    // server's phantom local player never takes over the spectator manager -> the whole spectator view
    // path (spectator entity + its Server_ApplyData transform RPC + the spectator camera-manager
    // BlueprintModifyCamera/PostProcess update) never runs, so none of those -1-chain dispatches AV.
    // Same function the A2SpecBlock client DLL blocks to keep the client in VR instead of spectator.
    if (!g_takeOwnIdx)
        if (SDK::UClass* vc = SDK::UObject::FindClassFast("VRPawn"))
            if (SDK::UFunction* f = vc->GetFunction("VRPawn", "Server_AttemptTakeOwnershipOfSpectatorManager"))
                g_takeOwnIdx = f->Name.ComparisonIndex;
    if (!g_applyDataIdx)
        if (SDK::UClass* sc = SDK::UObject::FindClassFast("A2SpectatorEntity"))
            if (SDK::UFunction* f = sc->GetFunction("A2SpectatorEntity", "Server_ApplyData"))
                g_applyDataIdx = f->Name.ComparisonIndex;
}

// Cache the OVRPlatform BP-library UClasses once so ProcessEvent_Hook can drop their (client-only)
// platform calls with a few pointer compares (no per-PE string work). Separate fn: FindClassFast builds
// std::string temporaries that cannot live in ProcessEvent_Hook's __try scope (C2712).
static void ResolveQuestNeuter()
{
    for (int i = 0; i < 4; ++i)
        if (!g_ovrLibs[i])
            g_ovrLibs[i] = reinterpret_cast<SDK::UObject*>(SDK::UObject::FindClassFast(kOvrLibNames[i]));
}

// SEH safety net for the dispatch: a UFunction with a broken property chain (failed BP loads on the
// headless client-as-server, e.g. BP_VolleyJakeball) AVs inside the engine's ProcessEvent at +0xB9.
// Never let that kill the server -> catch, drop, and log the FName idx (throttled) so it can be added
// to the cheap pre-drop set above. No C++ objects here (SEH cannot coexist with C++ unwinding, C2712).
static void SafeProcessEventOrig(SDK::UObject* Context, SDK::UFunction* Function, void* Parms)
{
    __try
    {
        ProcessEvent_Orig(Context, Function, Parms);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        static uint64_t s_lastPeGuard = 0;
        uint64_t nowG = GetTickCount64();
        if (nowG - s_lastPeGuard > 1000)
        {
            s_lastPeGuard = nowG;
            printf("[HalcyonA2][PE-GUARD] dropped AV in ProcessEvent dispatch, FName idx=%d\n",
                   Function ? Function->Name.ComparisonIndex : -1);
        }
    }
}

// =============================================================================================
// [RPCTRACE] Server-side visibility for the two-client ball test. The server had NO log of whether a
// client's Server_HitProp / Server_SpawnBall / Server_NotifyPlayerEnteredArena RPC ever arrived, so
// "client A hit the ball and nothing happened" could not be told apart from "the RPC never landed".
// Recognised by cached FName ComparisonIndex (integer compare per ProcessEvent - never GetName()
// here, that costs the game thread dearly), then the params are read straight off the Parms block
// using the layouts in A2_parameters.hpp.
// =============================================================================================
static int32_t g_idxHitProp     = 0;
static int32_t g_idxSpawnBall   = 0;
static int32_t g_idxEnterArena  = 0;
static int32_t g_idxHitResponse = 0;
static int32_t g_idxSendPhys    = 0;
static int32_t g_idxSetFreqData = 0;   // A2PlayerEntity::Server_SetFrequentData — ingest ping-correction site
// [2026-09-09] AActor::SetNetDormancy / FlushNetDormancy are REFLECTED UFunctions (SDK 22284
// Engine_classes.hpp:1236 / :1172), so any call to them passes through ProcessEvent and we can just
// watch for it. This settles the dormancy hypothesis for the 1-update/s lag state WITHOUT needing to
// reproduce the state: if nothing ever calls SetNetDormancy on a VRPawn, dormancy cannot be the
// mechanism and the pin added in 27c4cf9 is treating the wrong thing.
static int32_t g_idxSetDormancy   = 0;
static int32_t g_idxFlushDormancy = 0;
static bool    g_rpcIdxDone     = false;

static void ResolveRpcIndices()
{
    if (g_rpcIdxDone) return;
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("VRPawn");
    if (!cls) return;
    auto grab = [&](const char* fn) -> int32_t {
        auto* f = cls->GetFunction("VRPawn", fn);
        return f ? f->Name.ComparisonIndex : 0;
    };
    g_idxHitProp     = grab("Server_HitProp");
    g_idxSpawnBall   = grab("Server_SpawnBall");
    g_idxEnterArena  = grab("Server_NotifyPlayerEnteredArena");
    g_idxHitResponse = grab("Client_HitProp_Response");
    g_idxSendPhys    = grab("Server_SendPhysicsPropData");
    // Server_SetFrequentData lives on A2PlayerEntity (the pose-ingest RPC), not VRPawn.
    if (auto* peCls = SDK::UObject::FindClassFast("A2PlayerEntity"))
        if (auto* f = peCls->GetFunction("A2PlayerEntity", "Server_SetFrequentData"))
            g_idxSetFreqData = f->Name.ComparisonIndex;
    if (auto* aCls = SDK::UObject::FindClassFast("Actor"))
    {
        if (auto* f = aCls->GetFunction("Actor", "SetNetDormancy"))   g_idxSetDormancy   = f->Name.ComparisonIndex;
        if (auto* f = aCls->GetFunction("Actor", "FlushNetDormancy")) g_idxFlushDormancy = f->Name.ComparisonIndex;
    }
    HxLog("[HalcyonA2][DORMWATCH] watching SetNetDormancy(idx=%d) / FlushNetDormancy(idx=%d) -- if neither "
          "ever fires on a VRPawn, dormancy is NOT the 1-update/s lag mechanism\n",
          g_idxSetDormancy, g_idxFlushDormancy);
    g_rpcIdxDone = true;
    HxLog("[HalcyonA2][RPCTRACE] name idx: HitProp=%d SpawnBall=%d EnterArena=%d HitResponse=%d\n",
          g_idxHitProp, g_idxSpawnBall, g_idxEnterArena, g_idxHitResponse);
    // Report the native exec RVA of each RPC (UFunction::ExecFunction @0xD8) so the actual
    // _Implementation can be pulled up in IDA -- that is where the server decides to accept or drop
    // a hit, and nothing else tells us the RVA on this build.
    auto rva = [&](const char* fn) -> unsigned long long {
        auto* f = cls->GetFunction("VRPawn", fn);
        if (!f) return 0ull;
        const uintptr_t p = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(f) + 0xD8);
        return p ? (unsigned long long)(p - GetBase()) : 0ull;
    };
    HxLog("[HalcyonA2][RPCTRACE] exec RVA: Server_HitProp=0x%llX Server_SpawnBall=0x%llX Server_NotifyPlayerEnteredArena=0x%llX\n",
          rva("Server_HitProp"), rva("Server_SpawnBall"), rva("Server_NotifyPlayerEnteredArena"));
}

static void TraceBallRpc(SDK::UObject* Context, SDK::UFunction* Function, void* Parms)
{
    if (!Function || !Parms) return;
    const int32_t idx = Function->Name.ComparisonIndex;
    if (idx == g_idxHitProp)
    {
        struct HP { SDK::AActor* Actor; SDK::AActor* prev; double lastSeen; double ts;
                    SDK::FVector pos; SDK::FVector force; };
        auto* p = reinterpret_cast<HP*>(Parms);
        // Also report the gate the port already identified: the ball's physics-sync owningActor
        // (physicsSync @ball+0x4F0, owningActor @+0xC8). Server_HitProp/SendPhysicsPropData key off it.
        void* psync = nullptr; void* powner = nullptr;
        if (p->Actor)
        {
            psync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(p->Actor) + 0x4F0);
            if (psync) powner = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(psync) + 0xC8);
        }
        const SDK::FVector ap = p->Actor ? p->Actor->K2_GetActorLocation() : SDK::FVector{};
        // execServer_HitProp (0x53536C0) ends in `vtbl[2664](this, Actor, previousOwner)`, i.e. the
        // _Implementation is virtual slot 2664/8 = 333 on the pawn. Report its RVA once so the actual
        // accept/reject logic can be decompiled.
        static bool s_implLogged = false;
        if (!s_implLogged && Context)
        {
            s_implLogged = true;
            const uintptr_t vt = *reinterpret_cast<uintptr_t*>(Context);
            const uintptr_t impl = *reinterpret_cast<uintptr_t*>(vt + 2664);
            HxLog("[HalcyonA2][RPCTRACE] Server_HitProp_Implementation = RVA 0x%llX (IDB 0x%llX)\n",
                  (unsigned long long)(impl - GetBase()),
                  (unsigned long long)(0x140000000ull + (impl - GetBase())));
        }
        // [BALLSIM] also read the sync's own timestamp (sync+0x160) that HitProp compares ts against,
        // and the +0xBC gate byte, so we can see WHY ownership is or isn't taken.
        double syncTs = 0.0; int gateBC = -1;
        if (psync) { syncTs = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(psync) + 0x160);
                     gateBC = *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(psync) + 0xBC); }
        HxLog("[HalcyonA2][RPCTRACE] Server_HitProp from %s: actor=%s pos=(%.0f, %.0f, %.0f) force=(%.0f, %.0f, %.0f) ts=%.1f syncTs=%.1f gate0xBC=%d physSync=%p owner=%s\n",
              Context ? Context->GetName().c_str() : "<null>",
              p->Actor ? p->Actor->GetName().c_str() : "<null>",
              p->pos.X, p->pos.Y, p->pos.Z,
              p->force.X, p->force.Y, p->force.Z, p->ts, syncTs, gateBC, psync,
              powner ? static_cast<SDK::UObject*>(powner)->GetName().c_str() : "<null>");
        // and the owner AFTER the implementation runs (the real trace hook fires pre-call; schedule a
        // one-tick-later read via a static so we see whether ownership was actually granted).
        {
            static void* s_watchSync = nullptr; static uint64_t s_watchAt = 0;
            if (s_watchSync && GetTickCount64() >= s_watchAt)
            {
                auto* ow = *reinterpret_cast<SDK::UObject**>(reinterpret_cast<uintptr_t>(s_watchSync) + 0xC8);
                HxLog("[HalcyonA2][BALLSIM] post-hit owner on sync=%p is %s\n", s_watchSync,
                      ow ? ow->GetName().c_str() : "<null>");
                s_watchSync = nullptr;
            }
            if (psync) { s_watchSync = psync; s_watchAt = GetTickCount64() + 50; }
        }
    }
    else if (idx == g_idxSpawnBall)
    {
        struct SB { SDK::UClass* cls; SDK::FVector loc; SDK::FVector vel; SDK::FVector ang; };
        auto* p = reinterpret_cast<SB*>(Parms);
        HxLog("[HalcyonA2][RPCTRACE] Server_SpawnBall from %s: class=%s loc=(%.0f, %.0f, %.0f) vel=(%.0f, %.0f, %.0f)\n",
              Context ? Context->GetName().c_str() : "<null>",
              p->cls ? p->cls->GetName().c_str() : "<null>",
              p->loc.X, p->loc.Y, p->loc.Z, p->vel.X, p->vel.Y, p->vel.Z);
    }
    else if (idx == g_idxEnterArena)
    {
        auto* p = reinterpret_cast<SDK::FString*>(Parms);
        HxLog("[HalcyonA2][RPCTRACE] Server_NotifyPlayerEnteredArena from %s: slot='%s'\n",
              Context ? Context->GetName().c_str() : "<null>", FStringToNarrow(p).c_str());
    }
    else if (idx == g_idxSendPhys)
    {
        // Is the server still treating the sender as the ball's owner when the stream arrives? If
        // ownership has been reclaimed (UA2PhysicsSync::ServerReclaimOwnership) the implementation
        // silently drops every streamed update, which looks exactly like "the hit did nothing".
        struct SP { SDK::AActor* Actor; uint8_t data[0x88]; };
        auto* p = reinterpret_cast<SP*>(Parms);
        void* psync = p->Actor ? *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(p->Actor) + 0x4F0) : nullptr;
        auto* owner = psync ? *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(psync) + 0xC8) : nullptr;
        const SDK::FVector inPos = *reinterpret_cast<SDK::FVector*>(p->data + 0x18);
        const SDK::FVector cur   = psync ? *reinterpret_cast<SDK::FVector*>(reinterpret_cast<uintptr_t>(psync) + 0xD8) : SDK::FVector{};
        static uint64_t s_last = 0; const uint64_t now = GetTickCount64();
        if (now - s_last > 900)
        {
            s_last = now;
            HxLog("[HalcyonA2][RPCTRACE] Server_SendPhysicsPropData from %s: actor=%s owner=%s inPos=(%.0f, %.0f, %.0f) syncRepPos=(%.0f, %.0f, %.0f) accepted=%d@@",
                  Context ? Context->GetName().c_str() : "<null>",
                  p->Actor ? p->Actor->GetName().c_str() : "<null>",
                  owner ? owner->GetName().c_str() : "<null>",
                  inPos.X, inPos.Y, inPos.Z, cur.X, cur.Y, cur.Z,
                  (int)(owner == static_cast<SDK::AActor*>(Context)));
        }
    }
    else if (idx == g_idxHitResponse)
    {
        struct HR { SDK::AActor* Actor; bool Success; };
        auto* p = reinterpret_cast<HR*>(Parms);
        HxLog("[HalcyonA2][RPCTRACE] Client_HitProp_Response -> %s: actor=%s SUCCESS=%d\n",
              Context ? Context->GetName().c_str() : "<null>",
              p->Actor ? p->Actor->GetName().c_str() : "<null>", (int)p->Success);
    }
}
// [DORMWATCH] Separate function: GetName() builds a std::string, which cannot live inside
// ProcessEvent_Hook's __try scope (C2712) -- same split as SafeTraceBallRpc.
static void DormWatch(SDK::UObject* Context, bool isSet, void* Parms)
{
    const int arg = (isSet && Parms) ? (int)*reinterpret_cast<unsigned char*>(Parms) : -1;
    const unsigned char cur = *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(Context) + 0x159);
    HxLog("[HalcyonA2][DORMWATCH] %s on %s  newValue=%d currentNetDormancy=%d\n",
          isSet ? "SetNetDormancy" : "FlushNetDormancy",
          Context->GetName().c_str(), arg, (int)cur);
}
static void SafeDormWatch(SDK::UObject* c, bool isSet, void* p)
{
    __try { DormWatch(c, isSet, p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

static void SafeTraceBallRpc(SDK::UObject* c, SDK::UFunction* f, void* p)
{
    __try { TraceBallRpc(c, f, p); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// [SNAPFIX 2026-09-07] Correct FrequentData.Ping at INGEST — the robust half of the ping fix.
// SampleFreqRate() stamps the ping into localData/rep at StepSim rate, but the incoming pose RPC
// Server_SetFrequentData writes localData.Ping from the CLIENT's report (the client never measures
// its own RTT, so it sends 0) at ~80Hz per player. Whenever the fragment->replicated sync happens
// to run between an incoming RPC (Ping=0) and our next stamp, the replicated copy latches 0 and the
// receiving client's interpolation buffer COLLAPSES -> the remote player snaps "as if 500 ping",
// and only a grab (which re-inits the buffer) clears it. Correcting the Ping in the incoming Parms
// BEFORE the handler stores it means localData.Ping is never 0 to begin with, so the sync can never
// propagate a 0 -> the collapse cannot happen. Two functions: the raw param write is SEH-only (no
// C++ unwinding objects allowed in a __try scope, C2712); the map lookup lives in the outer one.
static void WriteIngestPing(void* parms, float pm)
{
    __try {
        float* p = reinterpret_cast<float*>(reinterpret_cast<uintptr_t>(parms) + 0x8);  // FReplicatedFrequentData.Ping@8
        if (*p <= 0.0f) { *p = pm; InterlockedIncrement(&g_ingestPingFixes); }   // only fill a client-sent 0/garbage; never lower a real value
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void CorrectIngestPing(SDK::UObject* ent, void* parms)
{
    if (!ent || !parms) return;
    float pm = kPingFloorMs;                                   // floor if we have never measured this player
    auto it = g_lastGoodPing.find(ent);                        // server-measured ping, refreshed ~1Hz
    if (it != g_lastGoodPing.end() && it->second > 0.0f) pm = it->second;
    WriteIngestPing(parms, pm);
}

// ===== [PROF] per-function cost attribution =========================================
// The VPS game thread is pegged at 100% of one (5.2x slower) core, so the question is how
// much of the frame OUR hook costs vs the base game. Wrap each Safe*() dispatch in a QPC
// timer, accumulate, and dump once a second as microseconds + % of wall time. Cheap:
// two QueryPerformanceCounter calls per dispatch, and the dispatches are already throttled.
static bool  g_prof = true;
static const char* g_profName[48];
static long long   g_profTicks[48];
static long long   g_profCalls[48];
static long long   g_profMax[48];     // [PROF] worst SINGLE call - MIB is driven by the worst
                                      // frame, not the average, so peaks matter more than totals
static int         g_profN = 0;
static long long   g_profQpf = 0;
static ULONGLONG   g_profLastDump = 0;
// No RAII here: ProcessEvent_Hook uses __try, and any object with a destructor in that scope
// is C2712 'cannot use __try in functions that require object unwinding'. LARGE_INTEGER is POD.
static int ProfSlot(const char* n) {
    for (int i = 0; i < g_profN; ++i) if (g_profName[i] == n) return i;
    if (g_profN >= 48) return 0;
    g_profName[g_profN] = n; g_profTicks[g_profN] = 0; g_profCalls[g_profN] = 0; g_profMax[g_profN] = 0;
    return g_profN++;
}
#define PROF(fn) do {                                                              \
        int _s = ProfSlot(#fn);       /* no function-local static: its thread-safe init guard emits EH -> C2712 */                                             \
        LARGE_INTEGER _a, _b;                                                      \
        if (g_prof) QueryPerformanceCounter(&_a);                                  \
        fn();                                                                      \
        if (g_prof) { QueryPerformanceCounter(&_b);                                \
                      const long long _d = _b.QuadPart - _a.QuadPart;                  \
                      g_profTicks[_s] += _d; ++g_profCalls[_s];                        \
                      if (_d > g_profMax[_s]) g_profMax[_s] = _d; }                    \
    } while (0)
static void ProfDump()
{
    if (!g_prof) return;
    const ULONGLONG now = GetTickCount64();
    if (!g_profLastDump) { g_profLastDump = now; return; }
    if (now - g_profLastDump < 1000) return;
    const double wallMs = double(now - g_profLastDump);
    g_profLastDump = now;
    if (!g_profQpf) { LARGE_INTEGER f; QueryPerformanceFrequency(&f); g_profQpf = f.QuadPart; }
    char buf[1400]; int off = 0; double totMs = 0.0;
    for (int i = 0; i < g_profN; ++i)
    {
        const double ms = (double(g_profTicks[i]) * 1000.0) / double(g_profQpf);
        totMs += ms;
        if (ms >= 0.5 && off < 1200)
        {
            const double mx = (double(g_profMax[i]) * 1000.0) / double(g_profQpf ? g_profQpf : 1);
            off += _snprintf_s(buf + off, sizeof(buf) - off, _TRUNCATE, "%s%s=%.1fms/%lld(pk%.0f)",
                               off ? " " : "", g_profName[i], ms, g_profCalls[i], mx);
        }
        g_profTicks[i] = 0; g_profCalls[i] = 0; g_profMax[i] = 0;
    }
    buf[off] = 0;
    const long wk = InterlockedExchange(&g_walks, 0);
    HxLog("[HalcyonA2][PROF] dll=%.1fms/s (%.1f%% of wall) walks/s=%ld rebuilds/s=%ld epoch=%ld objN=%ld gtc[live=%d ticked=%ld skipStopped=%ld] [%s]\n",
          totMs, (totMs / wallMs) * 100.0, wk, InterlockedExchange(&g_rebuilds, 0), (long)g_cacheEpoch, (long)g_objN,
          // [SCRAPRUN EVIDENCE] skipStopped counts GameTimeComponents our ticker DECLINED to drive
          // because ClockStartedAt <= 0. If it climbs, stale/stopped timers were being driven every
          // frame before the gate -- the spurious-OnCountdownEnd mechanism, observable while IDLE.
          (int)g_gameTimers.size(), g_gtcTicked, g_gtcSkipStopped, buf);
}
// ====================================================================================

static void ProcessEvent_Hook(SDK::UObject* Context, SDK::UFunction* Function, void* Parms)
{
    // [PORT 22284] DROP A2SpectatorEntity::Server_ApplyData. The server's phantom local player rides
    // an A2SpectatorEntity that fires Server_ApplyData(FReplicatedTransformData) every tick; on the
    // headless server that Net Server RPC's UFunction chain is incomplete (the Iris "Rejected RPC
    // ProtocolNotFound" spam), so the engine's ProcessEvent walks a -1 child ptr at +0xB9 (test
    // [ [Function+0xC8] +0xB0 ]) and AVs reading 0xffffffff`ffffffff. A headless server has no reason
    // to apply a spectator's transform -> drop it. Server-side twin of the A2SpecBlock client DLL.
    // (This was the real boot crash the whole time -- NOT the ballsim.) Resolve the FName idx once
    // (throttled; the class CDO exists from A2-module init, well before any spectator entity spawns),
    // then it's a cheap integer compare per call.
    // [PORT 22284 / user call] We no longer touch the spectator RPCs at all — no Server_ApplyData
    // drop, no Server_AttemptTakeOwnershipOfSpectatorManager block. The real root of the whole
    // spectator -> camera -> GC crash cascade was TuneSpectatorPawns force-pinning the spectator pawn
    // always-relevant + max-net-frequency, which kept a broken/incomplete spectator permanently
    // replicated and retained (its dangling ref = the null the parallel GC AV'd on). That tuning is
    // now disabled (SafeTuneSpectatorPawns call removed). Let the spectator behave normally; the
    // SafeProcessEventOrig SEH net below still stops any stray dispatch AV from killing the server.

    if (g_bootSpineOnly) { SafeProcessEventOrig(Context, Function, Parms); return; }

    // [22284] Drop OVRPlatform BP-library calls on the headless server (see g_neuterQuestPlatform). A VR
    // player's kiosk init dispatches User_GetOrgScopedID etc. into the Oculus Platform SDK, which
    // HARD-EXITS the process (no dump). Cheap: compare the UFunction's owning class (Outer) to the
    // cached OVR library ptrs. Dropping = the native platform call never runs; the kiosk BP just stalls
    // at that latent node (cosmetic server-side), and the VR player stays in.
    if (g_neuterQuestPlatform && Function)
    {
        SDK::UObject* fout = Function->Outer;
        if (fout && (fout == g_ovrLibs[0] || fout == g_ovrLibs[1] || fout == g_ovrLibs[2] || fout == g_ovrLibs[3]))
        {
            static uint64_t s_lastQ = 0; uint64_t nq = GetTickCount64();
            if (nq - s_lastQ > 2000) { s_lastQ = nq; printf("[HalcyonA2][QUEST-NEUTER] dropped OVR platform call, idx=%d\n", Function->Name.ComparisonIndex); }
            return;
        }
    }
    // TEMP rollback-pipeline trace: the jakeball syncs via the rollback channel
    // (client Server_SubmitInputs/Server_HitProp -> server -> Client_SendServerSimResults
    // back to all clients). If our offline-wired BallSimManager never SENDS
    // Client_SendServerSimResults, clients never converge. Log the relevant RPCs (both
    // directions) so we can see exactly where the pipeline breaks. Capped; remove after.
    // NOTE: the per-ProcessEvent [GOAL] trace that lived here is DISABLED — it called
    // Function->GetName() (FName->string + heap alloc) on EVERY ProcessEvent (thousands/frame),
    // which taxed the game thread down to ~40fps with 185ms hitches. That made the rollback sim
    // burst-step and predict ~30 frames = the high MI + ball jumping. Never do per-PE string work.
    // (Goal scoring is geometric via DetectGoals; this trace isn't needed.)

    // Authoritative golf stroke count: NewHitEvent fires per hit (server-auth) but the game's own
    // StrokeCount++ is gated on the FROZEN GetActorLocation() so it sticks at 1. Recognize the event by
    // cached FName index (cheap integer compare — NEVER GetName() here) and run the count ourselves.
    if (g_nheIdx && Function && Function->Name.ComparisonIndex == g_nheIdx)
    {
        __try { GolfHitEvent(Context); } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    // [RPCTRACE] log the ball RPCs the two-client test drives (cheap integer compares).
    if (!g_rpcIdxDone) { __try { ResolveRpcIndices(); } __except (EXCEPTION_EXECUTE_HANDLER) { g_rpcIdxDone = true; } }
    if (Function && (Function->Name.ComparisonIndex == g_idxHitProp ||
                     Function->Name.ComparisonIndex == g_idxSpawnBall ||
                     Function->Name.ComparisonIndex == g_idxEnterArena ||
                     Function->Name.ComparisonIndex == g_idxSendPhys ||
                     Function->Name.ComparisonIndex == g_idxHitResponse))
        SafeTraceBallRpc(Context, Function, Parms);

    // [DORMWATCH] Log every dormancy call, with the target actor, so we can see whether the game ever
    // puts a player pawn to sleep. POD-only compare; the log itself is rare by nature.
    if (Function && Context && (g_idxSetDormancy || g_idxFlushDormancy) &&
        (Function->Name.ComparisonIndex == g_idxSetDormancy ||
         Function->Name.ComparisonIndex == g_idxFlushDormancy))
        SafeDormWatch(Context, Function->Name.ComparisonIndex == g_idxSetDormancy, Parms);

    // [SNAPFIX] Correct the incoming pose RPC's Ping the instant it arrives, before the handler stores
    // it into localData -> the fragment->rep sync can never carry a 0 -> no interpolation collapse/snap.
    // Cheap cached-index integer compare; the correction itself is a single guarded float write.
    if (g_pingStampEnabled && g_idxSetFreqData && Function &&
        Function->Name.ComparisonIndex == g_idxSetFreqData)
        CorrectIngestPing(Context, Parms);

    // [2026-09-08 POSERATE] Count the ACTUAL incoming pose RPCs per player per second. The [FREQRATE]
    // X-delta probe is indirect (a stationary player reads 0, and it can alias), yet it showed only ~33-36
    // changes/s while moving — well under the ~80Hz the client is believed to send and the 90Hz sim. This
    // counts Server_SetFrequentData arrivals directly, per PlayerIndex, so we get the true client->server
    // pose rate with no sampling artefacts. If this reads ~80-90 the ingest is fine and the lag is in
    // client interpolation; if it reads ~30-40 the pose stream itself is throttled/dropping.
    if (g_freqDebug && g_idxSetFreqData && Function &&
        Function->Name.ComparisonIndex == g_idxSetFreqData)
    {
        __try {
            static int      s_poseCount[256] = {};
            static ULONGLONG s_lastPoseLog   = 0;
            int pidx = -1;
            // Context is the UA2PlayerEntity; its owning AVRPawn is at +0xE8, PlayerIndex at pawn+0x1C22.
            if (Context)
            {
                void* pw = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(Context) + 0xE8);
                if (pw) pidx = *reinterpret_cast<unsigned char*>(reinterpret_cast<uintptr_t>(pw) + 0x1C22);
            }
            if (pidx >= 0 && pidx < 256) ++s_poseCount[pidx];
            const ULONGLONG nowP = GetTickCount64();
            if (nowP - s_lastPoseLog > 1000)
            {
                s_lastPoseLog = nowP;
                char buf[256]; int n = 0;
                for (int q = 0; q < 256 && n < 200; ++q)
                    if (s_poseCount[q]) { n += snprintf(buf + n, sizeof(buf) - n, "p%d=%d/s ", q, s_poseCount[q]); s_poseCount[q] = 0; }
                if (n) printf("[HalcyonA2][POSERATE] %s\n", buf);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {}
    }

    static thread_local bool inHook = false;
    if (!inHook)
    {
        inHook = true;

        // Make sure g_ballSimMgr points at the live native manager (the hand-spawn that used to set it
        // is disabled). Without this the pin below is dead and no hit ever confirms to clients.
        SafeEnsureNativeBallSimMgr();
        SafeTuneBallNet();  // [BALLTUNE] keep jakeball net settings fresh so clients don't lag behind repPos

        // [2026-09-04 ★★ SEAT FIX] Drive the reconcile/seater ~4Hz — the game never calls it on our
        // dedicated server, so without this no player is seated (outline=0) and hits can't work. See DriveSeater.
        SafeDriveSeater();

        // [2026-09-04 20996 PARITY] one-shot: report/apply the mgr+0x2F0 OfflineBallSimSubsystem back-wire.
        SafeWireNativeManagerSubsystem();

        // Keep the ball-sim results-send flag (mgr+0x412) pinned to 1 on EVERY ProcessEvent call —
        // the GAME advances the sim itself (our step loop is usually inert), and sub_543F2D0 only
        // emits Client_SendServerSimResults when this flag is set. Arming it only at ~90Hz can miss
        // game-driven steps -> clients get sparse confirmations -> they over-predict -> MI balloons
        // far past the real RTT (85ms ping was showing MI ~44). Pinning it here makes every step send.
        // [2026-09-07 BALLSIM ★ STUTTER FIX] This per-frame results-send pin is DISABLED by default.
        // Isolated with a 2-client watcher: with it ON, a ball nobody touched snapped +894 then exactly
        // -894 in X every second (the "dragged in editor" stutter) - the forced authoritative sim
        // snapshot fighting the ball actor's own bReplicateMovement. With it OFF, the same balls
        // replicate SMOOTHLY (small continuous deltas, zero snaps) while the SimulationsOutline still
        // reaches clients (that rides on manager replication, not this flag). Client hits ride the
        // ownership -> Server_SendPhysicsPropData -> native ball replication path, which does not need
        // this pin. -PinSimResults restores the old behaviour for comparison.
        if (g_pinSimResults && !g_quietSims && g_ballSimMgr)
            *reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(g_ballSimMgr) + 0x412) = 1;

        // [2026-09-03 ★ ROOT FLOOD FIX] Keep component-archetype TArray headers valid so the GC never walks
        // a {Data=0,Max=0,Num=huge} corrupt array (the flood/hang). Self-gated to ~2 Hz; archetypes only.
        GcCorruptArraySweep();

        // Populate the Vivox VOIP config on the live subsystem ASAP (before any client
        // requests voice tokens). One-shot, retries until the subsystem exists.
        if (!g_voipConfigDone)
            ApplyVoipConfigIfNeeded();

        // Cache the OVRPlatform BP-library classes so the PE hook can drop their (client-only) platform
        // calls that hard-exit the headless server when a VR player's kiosk initializes. Cheap; runs
        // until all four resolve.
        if (g_neuterQuestPlatform)
            ResolveQuestNeuter();

        // Station dashboard, step 1: seed the api key + run the native server login so the server
        // authenticates. Retry ~3s until the A2SessionSubsystem is live, then latch.
        if (g_trackerDone && !g_dashLoginDone && GetTickCount64() - g_lastDashLogin > 3000)
        {
            g_lastDashLogin = GetTickCount64();
            if (++g_dashLoginTries <= 10)
                SafeTriggerServerDashboardLogin();
        }

        // Station dashboard, step 2: fetch the deployment/station config ourselves (sub_54198E0 ->
        // /v1/deployments?include_station_config -> sub_5409770 applies -> boards). This uses the
        // cmdline -DashboardApiKey directly (sub_5413CC0) and does NOT depend on the login, so gate it
        // on the tracker being up (station loaded + netvar/HTTP plumbing ready), NOT on g_dashLoginDone
        // (which the login latch controls). Fire a few times ~3s apart, then latch.
        if (g_trackerDone && !g_deployFetchDone && GetTickCount64() - g_lastDeployFetch > 3000)
        {
            g_lastDeployFetch = GetTickCount64();
            SafeTriggerDeploymentFetch();
            if (++g_deployFetchTries >= 3) g_deployFetchDone = true;
        }

        // Diagnostic: dump module (gamemode) slots a few times ~5s apart (slots stream in with the
        // districts), then latch. Shows which gamemodes have slots + load state + confirms
        // GamemodesManager -- the template for spawning a "deathrun" slot.
        if (g_trackerDone && !g_slotDumpDone && GetTickCount64() - g_lastSlotDump > 5000)
        {
            g_lastSlotDump = GetTickCount64();
            SafeDumpModuleSlots();
            SafeDumpScraprunMarkers();
            if (++g_slotDumpTries >= 3) g_slotDumpDone = true;
        }

        // Opt-in (GS_LOAD_GAMEMODE): load the requested gamemode into its slot once the world +
        // slots are up. Retry ~5s apart until the manager + target slot are found, then latch.
        if (g_loadGmPath[0] && g_trackerDone && !g_gmLoadDone && GetTickCount64() - g_lastGmLoad > 5000)
        {
            g_lastGmLoad = GetTickCount64();
            if (g_spawnSlot) SafeSpawnSlotAndLoad();   // spawn a real slot at the marker + init it
            else             SafeLoadGamemodeIntoSlot();
            if (++g_gmLoadTries >= 30) g_gmLoadDone = true;   // wait out the marker's level-instance transform
        }
        // Spawn path: once the async load binds LoadedGameMode, push its netvars so clients render it.
        if (g_spawnSlot && g_gmLoadDone && !g_pushedAfterLoad && GetTickCount64() - g_lastGmLoad > 2000)
        {
            static ULONGLONG lastPush = 0;
            if (GetTickCount64() - lastPush > 2000) { lastPush = GetTickCount64(); SafePushSpawnedSlotNetVars(); }
        }
        // Suppress deathrun's death balls (they crash the Android render thread — broken client cook).
        if (g_spawnSlot && g_gmLoadDone)
        {
            static ULONGLONG lastNoDb = 0;
            if (GetTickCount64() - lastNoDb > 1000) { lastNoDb = GetTickCount64(); SafeSuppressDeathBalls(); }
        }
        // LOD/visibility fix is done at the source: the NetVarReg_Hook on sub_46A0DF0 rewrites the
        // replicated "DefaultLODSettings" netvar to golf's no-hide profile the moment it's created
        // during gamemode load (before it replicates). The per-object ForceTrapLOD/FixDeathrunLOD
        // passes proved ineffective (per-object Settings don't replicate; the raw blob only existed
        // on a transient) — no longer invoked.
        // CRITICAL: sub_46D77A0 builds EVERY gamemode's objects, so the rewrite must be armed ONLY for
        // deathrun2's load. Disarm as soon as the load settles (no new objects for 3s, or a 25s hard
        // cap) — otherwise later stream-ins of golf/jakeball/etc. get shifted by our delta too. Then
        // dump where deathrun2's actors actually landed (ground truth vs. the corridor marker).
        static int       g_lastRwCount = -1;
        static ULONGLONG g_lastRwChange = 0;
        static bool      g_rwDone = false;
        if (!g_rwDone && g_gmLoadDone && (g_objRewrite || g_spawnSlot))
        {
            ULONGLONG now = GetTickCount64();
            if (g_objRewriteCount != g_lastRwCount) { g_lastRwCount = g_objRewriteCount; g_lastRwChange = now; }
            // spawn path: no rewrite counter, just wait ~10s after load for actors to stream in.
            bool settled = g_objRewrite ? (g_objRewriteCount > 0 && now - g_lastRwChange > 3000)
                                        : (now - g_lastGmLoad > 10000);
            bool timeout = (now - g_lastGmLoad > 25000);
            if (settled || timeout)
            {
                if (g_objRewrite)
                {
                    g_objRewrite = false;   // DISARM before anything else can load through the hook
                    printf("[HalcyonA2][GM] placement hook rewrote %d object transform(s); DISARMED\n", g_objRewriteCount);
                }
                g_rwDone = true;
                PROF(SafeDumpDeathrunActors);
            }
        }

        // One-shot world setup: spawn the GamemodesTracker once Station_Prime + its
        // class are live.
        if (!g_trackerDone)
            SpawnGamemodesTrackerIfReady();

        // No ABallSimManager is spawned on our server, so the 70 balls never join a
        // sim. Spawn+wire one (once, latched).
        if (g_ballsimEnabled && g_trackerDone && !g_ballSimDone)
            SpawnBallSimManagerIfNeeded();

        // [EXPERIMENT] delayed native ballsim spawn: gate kept closed at boot; once the tracker/world is up
        // and g_ballSimSpawnDelayMs has elapsed, open the gate + call the game's own spawner so the manager
        // spawns AFTER half-built components finish (test: does the GC flood stop?).
        if (g_delayBallSimSpawn && g_trackerDone && !g_ballSimGatePatched)
        {
            static uint64_t s_trackerDoneTick = 0;
            if (s_trackerDoneTick == 0) s_trackerDoneTick = GetTickCount64();
            if (GetTickCount64() - s_trackerDoneTick >= (uint64_t)g_ballSimSpawnDelayMs)
                PROF(SafeDelayedNativeBallSimSpawn);
        }

        // [EXPERIMENT NO-TICK] manager spawns natively (early), but we disable its actor tick (one-shot) so
        // it never reconciles/steps/builds. StepSim_Hook also no-ops the tick body as a belt.
        if (g_ballSimNoTick && !g_disabledNativeTick && g_trackerDone)
            PROF(SafeDisableNativeBallSimTick);

        // Once the manager exists, reconcile/build sims from the player list ~1s so
        // connected VRPawns get a sim...
        if (g_ballSimMgr && GetTickCount64() - g_lastBallBuildTick > 1000)
        {
            g_lastBallBuildTick = GetTickCount64();
            PROF(SafeWireVRPawns);   // point every VRPawn@0x1B38 at our manager; updates g_vrPawnCount
            PROF(SafeCheckRestCurve);   // [RESTCURVE] hand-speed->restitution curve present?
            PROF(SafeDumpReplicationGraph);   // [REPGRAPH] which node owns the player pawn class?
            // Reconcile only on player-count change: continuous reconcile re-bases the sim
            // frame counter every second (MI ~= all inputs) and rubber-bands the ball to
            // spawn. Gating it gives MI=0 and no reset. (Hits register in NEITHER mode, so
            // reconcile is not the hit gate — the server-side disc->ball collision is.)
            if (g_ballPumpsEnabled && g_vrPawnCount != g_lastReconcileCount)
            {
                g_lastReconcileCount = g_vrPawnCount;
                PROF(SafePumpBallSimBuild);
            }
        }
        // Step the sim ~90Hz (the only thing that moves the server ball actor).
        if (g_ballPumpsEnabled && !g_quietSims && g_ballSimMgr && GetTickCount64() - g_lastBallStepTick >= 11)
        {
            g_lastBallStepTick = GetTickCount64();
            PROF(SafePumpBallSimStep);
        }
        // Re-run overlap detection on the discs (~20Hz) so a ball rolling into a
        // start trigger actually fires OnOverlapByDisc and the match begins.
        if (g_ballPumpsEnabled && g_ballSimMgr && GetTickCount64() - g_lastBallOverlapTick >= 100)   // 10Hz (was 20Hz); cached lists
        {
            g_lastBallOverlapTick = GetTickCount64();
            if (!g_minimal) PROF(SafePumpBallOverlaps);
        }
        // [2026-09-03] Drive each learned GameTimeComponent's native TickComponent so its countdowns
        // elapse and broadcast onCountdownEnd/onTimerStopped -> the Luau runs SwitchState with its real
        // onEnter (DisableForcefields etc). This is the primary fix. Cheap: no-op until a timer is learned.
        PROF(SafeTickGameTimers);
        // [2026-09-03] Deathrun finish: a Runner reaching the EndTrigger starts overtime/spleef. The
        // server-side onOverlapByPlayerServer never fires (pawn collision doesn't track the player), so
        // detect it geometrically and drive the real Luau path. Self-gates (latched + throttled). See
        // DetectRunnerAtFinish.
        PROF(SafeDetectRunnerAtFinish);
        // Fallback only: if the tick above somehow doesn't advance a GSM out of GAME_BEGIN within ~11s,
        // force it to RUNNING via the NetVar (skips the Luau onEnter). Disarmed automatically the moment
        // the tick's onCountdownEnd drives updateGameState(5). See WatchdogGameBegin.
        PROF(SafeWatchdogGameBegin);
        // TEMP: census the ball-sim structure (jakeball vs heartball) a few times.
        // [2026-09-01] Run regardless of g_ballSimMgr — we NO LONGER spawn one; this hunts for a NATIVE
        // BallSimManager already in the level (the game runs its own sim; stepSim ticks ~39/s) to wire to.
        // [2026-09-01] LOG-ONLY corrupt-array scan (no writes -> safe). Identifies the object/class/offset
        // whose TArray goes {Data=0, Num=millions} on the dedicated server (the GC-flood source) so we can
        // compare it to the healthy offline state and find WHY it corrupts. Runs ~3s.
        // ★ Clamp the corrupt array on freed/garbage UBallSpawnerComponents (the confirmed GC-flood culprit),
        // targeted to that class only (safe), ~1s so it runs before the next GC.
        // [2026-09-01] DISABLED: superseded by InstallGcNumClamp (the interpreter Num-clamp fixes the flood
        // at the source for ALL classes without touching object memory). Per-class scanning was whack-a-mole
        // (BallSpawnerComponent fixed -> UA2SoundComponent surfaced) and wrote into live component fields.
        if (false && g_trackerDone && GetTickCount64() - g_lastArrScan > 1000)
        {
            g_lastArrScan = GetTickCount64();
            ClampCorruptArrays();
        }

        if (g_trackerDone && g_ballDbg < 10000 && GetTickCount64() - g_lastBallDbg > 2000)  // [2026-09-01] continuous: watch native mgr simset over time
        {
            g_lastBallDbg = GetTickCount64();
            PROF(SafeDumpBallStructure);
            ++g_ballDbg;
        }

        // Arena-admission dump ([ADM]/[SEAT]/[GOALDBG]) DISABLED again 2026-09-03 — re-enabling it (raw-
        // walks sim structs with 20996 offsets) coincided with a -1-deref crash during native multi-arena
        // sim builds. Keep off; [SENDPHYS] answers the hit question without touching sim internals.
        if (false && g_trackerDone && g_admDump < 40 && GetTickCount64() - g_lastAdmDump > 3000)
        {
            g_lastAdmDump = GetTickCount64();
            PROF(SafeDumpAdmissionState);
            PROF(SafeDumpSimSeats);
            if (g_admDump < 8) SafeDumpGoals();
            ++g_admDump;
        }

        // Keep goals armed (bGoalEnabled=0 on our server; re-arm after scoring) ~every 2s.
        if (g_trackerDone && GetTickCount64() - g_lastEnableGoals > 2000)
        {
            g_lastEnableGoals = GetTickCount64();
            PROF(SafeEnableGoals);
        }

        // NET-DRIVER TUNING DISABLED — proven a NO-OP: diagnostic showed NetServerMaxTickRate is
        // ALREADY 90 (curRate=90). The driver was never capped, so the non-arena snapping is NOT a
        // net-rate problem. A2 player pose rides the Mass "frequent data" system, not vanilla actor
        // replication -> the throttle is there. See a2-match-state-and-ball-physics.
        if (false && g_trackerDone && GetTickCount64() - g_lastNetTune > 2000)
        {
            g_lastNetTune = GetTickCount64();
            PROF(SafeTuneNetDriver);
        }

        // [NETRATE] Per-connection bandwidth caps DO still apply (unlike the tick rate above, which was
        // already 90). Re-apply every 2s so connections that join later are covered too.
        if (g_trackerDone && GetTickCount64() - g_lastNetRateTune > 2000)
        {
            g_lastNetRateTune = GetTickCount64();
            PROF(SafeTuneNetRates);
        }

        // SNAP DIAGNOSIS probe — log each VRPawn's frequent-data server timestamp once/sec so we can
        // see if the pose stream's timestamps are advancing. Remove once the snap cause is found.
        if (g_trackerDone && GetTickCount64() - g_lastFreqProbe > 1000)
        {
            g_lastFreqProbe = GetTickCount64();
            PROF(SafeProbeFreqTimestamps);
        }

        // BALL-SYNC probe — log DiscEntity server positions that moved, ~5Hz, to see if hit balls
        // stream server-side or only jump (spawn->hit). Remove once the ball teleport cause is found.
        if (g_trackerDone && GetTickCount64() - g_lastBallPos > 200)
        {
            g_lastBallPos = GetTickCount64();
            PROF(SafeProbeBallPositions);
        }

        // SIM-WIRE probe — per-pawn disc-comp/ball/flag state + owned golf/heart balls, ~1s. Confirms
        // whether golf/heart balls are wired for reconcile (the sim fix). Read-only.
        if (g_trackerDone && GetTickCount64() - g_lastBallWire > 1000)
        {
            g_lastBallWire = GetTickCount64();
            PROF(SafeProbeBallWire);
        }

        // Populate each arena's TicketManager.TeamColors (2 random rows from
        // DT_CosmeticMaterialMetaData) via the module slot's own InitializeRandomColors — this
        // colors the arena decor AND makes admission color joining players (else all gray). Retry
        // ~2s; each slot is colored once, as its TicketManager comes online.
        if (g_trackerDone && GetTickCount64() - g_lastTeamColor > 2000)
        {
            g_lastTeamColor = GetTickCount64();
            PROF(SafeInitTeamColors);
        }

        // Register each player's quest component with the server progression system ~every 2s.
        // QTRACE confirmed: the game fires ServerProgression::SetProgress on its own but NEVER
        // the init RPCs (no orchestrator on our headless server), so SetProgress errors with
        // "PlayerInstances does not contain Component". We register the pawn ourselves; then the
        // game's own SetProgress completions land. (QTRACE hooks left in to watch the error vanish.)
        if (g_questSeedEnabled && g_trackerDone && GetTickCount64() - g_lastQuestInit > 2000)
        {
            g_lastQuestInit = GetTickCount64();
            PROF(SafeInitPlayerQuests);
        }

        // Backend auth join-gate (~1s): kick remote players who didn't authenticate through our
        // backend. Log-only until g_gateEnforce is set. See AuthGateTick.
        if (g_trackerDone && GetTickCount64() - g_lastAuthGate > 1000)
        {
            g_lastAuthGate = GetTickCount64();
            PROF(SafeAuthGateTick);
            // [PORT 22284 / user call] DISABLED: TuneSpectatorPawns force-pinned the spectator pawn
            // always-relevant + max-net-frequency. On 22284 that kept a broken/incomplete spectator
            // entity permanently replicated and retained -> its dangling reference was the null that
            // the parallel GC reference pass AV'd on (worker-thread crash), and it also kept the
            // spectator flooding its -1-chain Server_ApplyData. Root of the whole cascade -> off.
            // (The 20996 smoothness reason no longer applies now that we don't force the spectator.)
            // SafeTuneSpectatorPawns();
        }

        // Report the real connected-player count to the backend every ~3 min so the EOS session's
        // count gets reconciled past client-missed leaves (hard disconnects). See PlayerCountReportTick.
        static ULONGLONG s_lastPlayerReport = 0;
        if (g_trackerDone && GetTickCount64() - s_lastPlayerReport > 180000)
        {
            s_lastPlayerReport = GetTickCount64();
            PROF(SafePlayerCountReportTick);
        }

        // Shooting/goalie practice conductor (Tackleball/Driftball). The serverOnly gamemode.luau
        // that spawns each training ball + kicks the BP shooting logic doesn't run headless, so we
        // do it ourselves. Was 500ms; a full GObjects walk 2Hz for a kiosk that only exists in the
        // training arena is wasted game-thread time in a jakeball match -> now 2s.
        if (g_trackerDone && GetTickCount64() - g_lastTraining > 2000)
        {
            g_lastTraining = GetTickCount64();
            PROF(SafeTrainingTick);
        }

        // Geometric goal detection -> drive the score (~10Hz).
        if (g_trackerDone && GetTickCount64() - g_lastDetectGoals > 100)
        {
            g_lastDetectGoals = GetTickCount64();
            if (!g_minimal) PROF(SafeDetectGoals);
            if (!g_minimal) PROF(SafeGolfSinkDetect);   // same 10Hz cadence: geometric golf-cup sink -> fire BallInCup
            if (!g_minimal) PROF(SafeVolleyfallTick);   // geometric spleef-floor break -> fire the panel's ReceiveActorBeginOverlap
        }

        // Pump the physics-sync send so free-ball (heartball/grabbable) physics reaches
        // clients (~30Hz — the jakeball uses the rollback channel, not this; halved from
        // 66Hz to free game-thread time for the rollback step so it doesn't fall behind).
        if (!g_quietSims && g_trackerDone && GetTickCount64() - g_lastPhysTick >= 30)
        {
            g_lastPhysTick = GetTickCount64();
            if (!g_minimal) PROF(SafePumpPhysicsSync);
            ProfDump();   // [PROF] 1Hz cost attribution for the DLL's own tick work
        }

        // (SafeWatchBallOwnership diagnostic disabled — it walked GObjects + read positions
        // at 10Hz, stealing game-thread time from the rollback step. Re-enable if needed.)

        inHook = false;
    }
    SafeProcessEventOrig(Context, Function, Parms);
}

// UGameplayUtilityStatics::IsRunningSimulate(UWorld*) — the gate for A2's Mass
// physics processor (UA2PhysicsSyncProcessor). On our headless client-as-server it
// returns false, so the ball/disc Mass simulation never advances and everything
// floats. Native impl RVA (from IDA: "impl: 0x52DC906"). Force it true so the
// processor runs. Full replace — we never need the original result.
static constexpr uintptr_t IsRunningSimulate_RVA = 0x531EF96;  // [22284] was 0x52DC906
static bool __fastcall IsRunningSimulate_Hook(void* /*World*/)
{
    return true;
}

// UA2NetworkUtilityBPFL::GetNetMode -> EA2NetMode. A2's OWN net-mode abstraction
// that gameplay/physics key off (EA2NetMode: Standalone=0, DedicatedServer=1,
// ListenServer=2, Client=3). Our client-as-server auto-listens, so this reports
// ListenServer/Standalone and every dedicated-server-gated path (incl. the Mass
// physics processor that moves the balls) is skipped. The exec thunk's core impl
// is sub_5473BC0 (from IDA). Force it to DedicatedServer(1).
static constexpr uintptr_t A2GetNetMode_RVA = 0x54AF360;  // [22284] was 0x54AF360
static int __fastcall A2GetNetMode_Hook(void* /*Context*/)
{
    return 1; // EA2NetMode::DedicatedServer — int (not char) so callers reading full EAX get clean 1
}

// UWorld::GetNetMode (found via UKismetSystemLibrary::IsDedicatedServer, which is
// `GetNetMode(World) == 1`). Force NM_DedicatedServer(1) so every net-mode query —
// including the ones during world init, before the net driver attaches (UE-174595)
// — reports a real dedicated server. Installed BEFORE open so it covers init.
static constexpr uintptr_t WorldGetNetMode_RVA = 0x4040990;  // [22284] was 0x4040990
// [22284] MUST return int, not char: InternalGetNetMode returns ENetMode (32-bit) and callers like
// sub_54B13D0 read the full EAX (`if ((_DWORD)result != 2)`). A `char` return sets only AL=1 and
// leaves the upper EAX bits garbage -> the gate compare fails. `int` gives a clean EAX=1.
// [2026-09-02 ★ BALLSIM VERDICT — do NOT re-add a g_inBallStep net-mode remap here; traced dead in IDA.]
// This getter (sub_7FF6760A0990, RVA 0x4040990) IS the ball sim's net check, but forcing NM_ListenServer(2)
// during the tick is INERT: (1) the sim build (0x545CCB0) uses `IsServer=(GetNetMode==2)` ONLY to feed a
// log line — no logic branches on it; (2) the per-step advance (0x54C4630) has NO net-mode check at all;
// (3) the client-perspective rebuild (stepSim Block 1/2) is gated `==3 NM_Client`, which ALREADY fails on
// the server (we return 1, not 3) — so that rebuild is already skipped. Offline is ALSO IsServer:0
// (NM_Standalone), so server == offline on this axis. The real offline-vs-server divergence is NOT net
// mode: it's that the -2 sim's participant advance (sub_7FF677548DA0) dereferences the resolved PLAYER's
// VR/local state, which is real offline but null/stale for the headless server's phantom local player ->
// the GC pass walks those null refs = the flood. Contained (not crashed) by BuildSim_Hook participant-
// compaction + InstallGcNumClamp. So keep returning 1 unconditionally (the dashboard gate sub_54B13D0 and
// IsDedicatedServer require it).
static int __fastcall WorldGetNetMode_Hook(void* /*World*/)
{
    return 1; // NM_DedicatedServer
}

// sub_3500950 — the BallSimManager's guarded GetWorld. It returns null for our hand-
// spawned manager (a world-resolution guard fails), so the ball sim can't determine
// net mode (logs "IsServer: 0") or resolve players into the sim -> it builds a local,
// non-authoritative sim on every machine -> the jakeball never converges between
// clients (each runs its own). Fix: when this returns null for a BallSimManager, hand
// back the real UWorld so GetNetMode (our hook) reports DedicatedServer and the sim goes
// authoritative. Gated on the null-return + class so it never affects other callers.
static constexpr uintptr_t BallSimGetWorld_RVA = 0x3509AB0;
using BallSimGetWorld_t = __int64 (__fastcall*)(void*);
static BallSimGetWorld_t BallSimGetWorld_Orig = nullptr;
static __int64 __fastcall BallSimGetWorld_Hook(void* a1)
{
    __int64 r = BallSimGetWorld_Orig(a1);
    if (r == 0 && a1)
    {
        static SDK::UClass* mgrCls = nullptr;
        if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
        if (mgrCls && static_cast<SDK::UObject*>(a1)->IsA(mgrCls))
            return reinterpret_cast<__int64>(SDK::UWorld::GetWorld());
    }
    return r;
}

// [2026-09-01] REMOVE THE SERVER'S LOCAL PLAYER FROM THE SIM (user's fix). The rollback ball sim +
// physics-sync are CLIENT-perspective: they find "the local player" via AVRPawn::GetLocalInstanceWith
// World (RVA 0x54EAC60) and, when it's non-null, build/rebuild THAT player's prediction sim (index -2)
// and reference its client-only objects (renderers etc.) -> the null-ref GC flood that pauses the box.
// Our server is a client process with a PHANTOM local player, so this returns non-null and the whole
// client path runs. A real dedicated server has no local player -> this returns null and none of it
// runs. So make it return null — but ONLY after the world/station is up (g_trackerDone, which is also
// when the sim manager spawns), so LoadMap still has its local player (removing it during load crashes
// BP_VolleyJakeball CosmeticLoadout). Callers null-check the result (the sim's gate is `!= 0`), so this
// makes them cleanly skip the local-player work = dedicated-server behavior. Gated for instant revert.
static bool g_nullLocalInstanceInSim = false;  // had ZERO effect on the flood (rip identical) -> off; local player wasn't it
static constexpr uintptr_t GetLocalInst_RVA = 0x54EAC60;
using GetLocalInst_t = __int64(__fastcall*)(void*);
static GetLocalInst_t GetLocalInst_Orig = nullptr;
static __int64 __fastcall GetLocalInst_Hook(void* world)
{
    if (g_nullLocalInstanceInSim && g_trackerDone)
        return 0;   // no local player -> the client-perspective sim/physics path skips the -2 rebuild
    return GetLocalInst_Orig(world);
}

// UGameplayStatics::GetPlayerPawn_Implementation (RVA 0x3D24C40). Reads the controller's Pawn at
// a1+0x2D8 with NO null-check on a1. After we drop the server's local player (LocalPlayers.Remove(0)),
// the local-player resolvers (e.g. sub_40329F0) return null, so client-side per-frame ticks that do
// GetPlayerPawn(localController) deref null+0x2D8 and crash. Every such caller checks the RESULT for
// null (e.g. sub_53B7340: `if (result != 0)`), so returning 0 on a null controller makes them cleanly
// skip — a targeted guard, not a NOP (remote players' pawns still resolve normally).
static constexpr uintptr_t GetPlayerPawn_RVA = 0x3D24C40;
using GetPlayerPawn_t = __int64(__fastcall*)(void*);
static GetPlayerPawn_t GetPlayerPawn_Orig = nullptr;
static __int64 __fastcall GetPlayerPawn_Hook(void* a1)
{
    if (!a1) return 0;   // no local player -> null controller; skip instead of deref-crashing
    return GetPlayerPawn_Orig(a1);
}

// [2026-09-01] Ball-sim net-perspective getter (sub_7FF6760A0990) -> ENetMode (2=NM_ListenServer,
// 3=NM_Client). See the big note at StepSim_Hook: during the ball tick we remap client(3)->ListenServer
// (2) so stepSim's `==3` client-rebuild gates fail and the -2 local-player prediction sim stops being
// rebuilt every tick (the GC flood). Scoped to g_inBallStep -> only affects calls made from inside the
// manager's tick; every other caller sees the real value unchanged.
static constexpr uintptr_t BallNetPersp_RVA = 0x60A0990;
using BallNetPersp_t = int(__fastcall*)(void*);
static BallNetPersp_t BallNetPersp_Orig = nullptr;
static int __fastcall BallNetPersp_Hook(void* world)
{
    int r = BallNetPersp_Orig(world);
    if (g_killLocalSimRebuild && g_inBallStep && r == 3)
        return 2;   // ball tick: client -> ListenServer authority (skip the -2 client rebuild)
    return r;
}

// UKismetSystemLibrary::PrintString — the real native impl (the body the
// execPrintString exec thunk calls after unpacking the FFrame:
// sub_3A21120(WCO, InString, bPrintToScreen, bPrintToLog, &TextColor, Duration, Key)).
// Hooking the impl here catches EVERY PrintString — including the EX_CallMath fast-path
// calls that never route through ProcessEvent — so PrintDebugString's "Gamemode Tests:
// ..." narration (which calls PrintString internally) lands here too. rdx = the second
// arg = const FString& InString, so read Data@0x00 / Num@0x08 straight off it.
static constexpr uintptr_t PrintString_RVA = 0x3A29B20;
using PrintString_t = void(__fastcall*)(void*, void*, bool, bool, void*, float, void*);
static PrintString_t PrintString_Orig = nullptr;
static void __fastcall PrintString_Hook(void* wco, void* inString, bool toScreen,
                                        bool toLog, void* color, float duration, void* key)
{
    PrintCapturedString(inString);
    PrintString_Orig(wco, inString, toScreen, toLog, color, duration, key);
}

// A2's rollback ball sim gates authority on `GetNetMode() == NM_ListenServer(2)`
// (e.g. sub_540E8A0: the "IsServer" it logs is `GetNetMode == 2`). A2 has no listen
// servers — we run a dedicated server (net mode 1) — so those checks fail and the sim
// runs OFFLINE (non-authoritative), which is why the jakeball never converges between
// clients. GetNetMode is called out-of-line (`call 0x4040990`/`0x54AF360`) and the check
// is `cmp eax, 2` a few bytes later. Scan the BallSimManager code cluster for that shape
// and patch the imm `02` -> `01` so our dedicated server is recognized as the authority.
static void PatchBallSimNetModeChecks()
{
    const uintptr_t base = GetBase();
    // [PORT-AUDIT] 20996 range 0x5400000-0x5480000 patched exactly six sites (seater-binder, build worker, sub_5414850,
    // DashboardInit, sub_547AE70, sub_547B3C0). Their 22284 equivalents are 0x545B446/0x545CD31/0x5461A1B/0x54B141A/
    // 0x54B3351/0x54B358C, all inside [0x5450000,0x54B4000). The previous 0x5400000-0x5500000 ALSO patched six unrelated
    // sites - five in the VOIP/OnlineCommunications region (0x542DAC0, 0x5436A00, 0x5436BC0, 0x54379E0, 0x54380E0: the
    // ListenServer-only positional-participant list paths) and 0x54BA8C0 - which 20996 never touched.
    const uintptr_t lo   = base + 0x5450000;
    const uintptr_t hi   = base + 0x54B4000;
    const uintptr_t gnmA = base + 0x4040990;   // UWorld::GetNetMode
    const uintptr_t gnmB = base + 0x54AF360;   // UA2NetworkUtilityBPFL::GetNetMode
    int patched = 0;
    for (uintptr_t p = lo; p < hi - 8; ++p)
    {
        if (*reinterpret_cast<uint8_t*>(p) != 0xE8)   // call rel32
            continue;
        const int32_t rel = *reinterpret_cast<int32_t*>(p + 1);
        const uintptr_t target = p + 5 + rel;
        if (target != gnmA && target != gnmB)
            continue;
        // within the next few instructions, find `cmp eax, 2` (83 F8 02) and patch to 01.
        for (uintptr_t q = p + 5; q < p + 5 + 20; ++q)
        {
            if (*reinterpret_cast<uint8_t*>(q) == 0x83 &&
                *reinterpret_cast<uint8_t*>(q + 1) == 0xF8 &&
                *reinterpret_cast<uint8_t*>(q + 2) == 0x02)
            {
                WriteByte(q + 2, 0x01);
                ++patched;
                break;
            }
        }
    }
    printf("[HalcyonA2] patched %d ball-sim net-mode checks (==2 -> ==1)\n", patched);
}


// =============================================================================================
// [CLIENTMODE] Mock client. Injected into a SECOND copy of the same shipping exe with
// -HalcyonClient -HalcyonConnect=<ip:port>, this turns that process into a headless test client:
//   1. keep GIsClient/GIsServer alone (this really is a client) and patch the entitlement self-exit
//   2. "open <ip>:<port>" to join the HalcyonA2 server
//   3. once the join lands the client is parked on ASpectatorCameraManagerPawn, so call its
//      Server_ExitSpectator(PlayerController) RPC to be given a real VR pawn - that is what makes
//      the client a ball-sim PARTICIPANT instead of a spectator
//   4. log, every second: our pawn, the ball actors' positions/velocities (does ballsim actually
//      simulate for us?) and the Vivox channel URI the SERVER handed us (is voice positional?)
// This exists so ball-sim and VOIP can be verified end to end without a headset.
// =============================================================================================
static std::wstring g_clientConnect;          // "ip:port" from -HalcyonConnect=
static bool         g_clientExited  = false;  // Server_ExitSpectator already sent?

static SDK::UObject* FindFirstOfClass(const char* className, bool skipDefaults = true)
{
    auto* cls = SDK::UObject::FindClassFast(className);
    if (!cls) return nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o) continue;
        if (skipDefaults && o->IsDefaultObject()) continue;
        if (o->IsA(cls)) return o;
    }
    return nullptr;
}

// [RELATCH 2026-09-07] This used to latch g_clientExited the first time a VR pawn appeared and then
// return immediately forever. In the two-client run the server put client A BACK into a spectator
// pawn a couple of minutes after the join, and because of the latch nothing ever tried to leave
// spectator again -- so the driver never reached a VR pawn and never hit anything. Recompute the
// state every tick instead of latching, and re-send Server_ExitSpectator (rate-limited) whenever we
// are holding a spectator pawn.
static bool     g_clientInArena   = false;   // pawn is a real VR pawn right now
static uint64_t g_lastExitSend    = 0;
static std::string g_lastPawnName;

static void ClientTryExitSpectator()
{
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->OwningGameInstance) return;
    auto& lps = world->OwningGameInstance->LocalPlayers;
    if (lps.Num() <= 0 || !lps[0] || !lps[0]->PlayerController) return;
    SDK::APlayerController* pc = lps[0]->PlayerController;

    static SDK::UClass* specCls = nullptr;   // [PERF] cached class lookup
    if (!specCls) specCls = SDK::UObject::FindClassFast("SpectatorCameraManagerPawn");
    if (!specCls) return;

    const std::string pawnName = pc->Pawn ? pc->Pawn->GetName() : std::string("<none>");
    if (pawnName != g_lastPawnName)
    {
        HxLog("[HalcyonA2][CLIENT] pawn changed: %s -> %s\n",
              g_lastPawnName.empty() ? "<start>" : g_lastPawnName.c_str(), pawnName.c_str());
        g_lastPawnName = pawnName;
    }

    // "In arena" means specifically a VR pawn: before the join completes the local pawn is
    // BP_EntryPawn_C, which is not a spectator either but is not in the game world.
    g_clientInArena = (pawnName.find("VRPawn") != std::string::npos);
    g_clientExited  = g_clientInArena;
    if (g_clientInArena) return;

    if (!pc->Pawn || !pc->Pawn->IsA(specCls)) return;   // entry pawn / nothing to do yet

    const uint64_t now = GetTickCount64();
    if (now - g_lastExitSend < 3000) return;
    g_lastExitSend = now;

    // The RPC must go to the pawn THIS PlayerController owns -- a Server_ RPC on an actor the local
    // connection does not own is dropped by the net driver.
    SDK::UObject* spec = pc->Pawn;
    auto* fn = spec->Class ? spec->Class->GetFunction("SpectatorCameraManagerPawn", "Server_ExitSpectator") : nullptr;
    if (!fn)
    {
        static bool once = false;
        if (!once) { once = true; HxLog("[HalcyonA2][CLIENT] Server_ExitSpectator not found on %s\n", spec->GetName().c_str()); }
        return;
    }
    struct { SDK::APlayerController* PlayerController; } params{ pc };
    spec->ProcessEvent(fn, &params);
    static int tries = 0;
    ++tries;
    HxLog("[HalcyonA2][CLIENT] Server_ExitSpectator #%d on %s\n", tries, spec->GetName().c_str());
}

// [CLIENTMODE] Drive the Vivox handshake by hand. A real client only calls
// UA2OnlineCommunicationsComponent::RequestChannelJoinTokens after its mothership/Vivox login has
// completed; the mock client never logs in, so the server's JoinBuild/SendJoin hooks were never hit
// and there was nothing to check. Calling the two Server_ RPCs directly exercises exactly the path a
// real client takes, and the URI the server hands back is what decides positional vs group voice.
static bool g_voipAsked = false;
static void ClientRequestVoipTokens()
{
    if (g_voipAsked) return;
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->OwningGameInstance) return;
    auto& lps = world->OwningGameInstance->LocalPlayers;
    if (lps.Num() <= 0 || !lps[0] || !lps[0]->PlayerController) return;
    SDK::APlayerController* pc = lps[0]->PlayerController;
    if (!pc->Pawn) return;

    static SDK::UClass* commCls = nullptr;   // [PERF] cached class lookup
    if (!commCls) commCls = SDK::UObject::FindClassFast("A2OnlineCommunicationsComponent");
    if (!commCls)
    {
        static bool once = false;
        if (!once) { once = true; HxLog("[HalcyonA2][CLIENT] class A2OnlineCommunicationsComponent not found\n"); }
        return;
    }
    // The component hangs off the VR pawn (BP_VRPawn onlineCommunicationsComponent @0x8B0), but find
    // it by outer instead of by offset so a layout change cannot silently break the test.
    SDK::UObject* comm = nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(commCls)) continue;
        if (o->Outer == static_cast<SDK::UObject*>(pc->Pawn)) { comm = o; break; }
        if (!comm) comm = o;   // fall back to any instance
    }
    if (!comm)
    {
        static bool once = false;
        if (!once) { once = true; HxLog("[HalcyonA2][CLIENT] no A2OnlineCommunicationsComponent instance found\n"); }
        return;
    }

    const std::string acct = "halcyontest";
    auto* fnLogin = comm->Class->GetFunction("A2OnlineCommunicationsComponent", "RequestVivoxLoginToken");
    auto* fnJoin  = comm->Class->GetFunction("A2OnlineCommunicationsComponent", "RequestChannelJoinTokens");
    if (!fnLogin || !fnJoin)
    {
        HxLog("[HalcyonA2][CLIENT] VOIP RPCs not found on %s\n", comm->GetName().c_str());
        g_voipAsked = true;
        return;
    }
    // NOTE: calling these from this worker thread killed the client process outright (the Vivox login
    // path is game-thread affine). Off by default; -HalcyonVoipRpc opts in for a manual experiment.
    if (!wcsstr(GetCommandLineW(), L"-HalcyonVoipRpc"))
    {
        g_voipAsked = true;
        HxLog("[HalcyonA2][CLIENT] VOIP RPCs found on %s but NOT sent (game-thread affine; use -HalcyonVoipRpc to force). "
              "Check the server's [VOIPFIX] resolved-channel line instead.\n", comm->GetName().c_str());
        return;
    }
    {
        struct { SDK::FString AccountId; } p{ SDK::FString(L"halcyontest") };
        HxLog("[HalcyonA2][CLIENT] -> RequestVivoxLoginToken\n");
        comm->ProcessEvent(fnLogin, &p);
    }
    {
        struct { SDK::FString AccountId; } p{ SDK::FString(L"halcyontest") };
        HxLog("[HalcyonA2][CLIENT] -> RequestChannelJoinTokens\n");
        comm->ProcessEvent(fnJoin, &p);
    }
    g_voipAsked = true;
    HxLog("[HalcyonA2][CLIENT] requested Vivox login + channel-join tokens on %s (outer=%s) acct='%s'\n",
          comm->GetName().c_str(), comm->Outer ? comm->Outer->GetName().c_str() : "<null>", acct.c_str());
}

// =============================================================================================
// [BALLTEST] Two-client ball test. One client runs with -HalcyonDrive and actually acts on a ball
// (enter arena -> teleport onto it -> AVRPawn::Server_HitProp, plus an independent
// AVRPawn::Server_SpawnBall probe); the other only watches. BOTH sample every BP_JakeBall_C once a
// second and print a per-ball delta with a wall clock, so "the driver moved it" and "the watcher saw
// it move" can be lined up by timestamp instead of inferred.
// =============================================================================================
static bool         g_clientDrive = false;   // -HalcyonDrive
static std::wstring g_arenaSlot;             // -HalcyonArena=<slotID> (default TKB_Prime)

struct BallSample { SDK::FVector pos{}; bool seen = false; int stillFor = 0; };
static std::unordered_map<void*, BallSample> g_ballPrev;
static std::unordered_map<void*, double> g_repSeen;   // [REPDATA] last seen rep Timestamp per ball

// [HITTEST] The ball the driver is currently hitting, so BOTH clients can print it every second by
// world position regardless of the per-client actor name (network actors get different local names).
static void*         g_hitTarget      = nullptr;
static SDK::UClass*  g_hitTargetClass = nullptr;
static SDK::FVector  g_hitTargetPos{};
static uint64_t      g_hitAtMs        = 0;

static const char* NowStamp()
{
    static char buf[32];
    SYSTEMTIME st; GetLocalTime(&st);
    sprintf_s(buf, "%02d:%02d:%02d.%03d", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buf;
}

// Every BP_JakeBall_C we can see, with how far it moved since the previous sample.
static void ClientReportState()
{
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->OwningGameInstance) { HxLog("[HalcyonA2][CLIENT] no world\n"); return; }
    auto& lps = world->OwningGameInstance->LocalPlayers;
    SDK::APlayerController* pc = (lps.Num() > 0 && lps[0]) ? lps[0]->PlayerController : nullptr;
    SDK::APawn* pawn = pc ? pc->Pawn : nullptr;

    // [BALLSIM] Did the server's BallSimManager replicate to us, and did its SimulationsOutline arrive?
    // outline@0x358 populated on the CLIENT means the ball<->sim association reached us - the thing
    // that makes a ball predictable and hittable. 0 means the manager isn't replicating its sim state.
    {
        static SDK::UClass* mgrCls = nullptr;   // [PERF] cached class lookup
        if (!mgrCls) mgrCls = SDK::UObject::FindClassFast("BallSimManager");
        if (mgrCls)
        {
            const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
            for (int32_t i = 0; i < num; ++i)
            {
                auto* o = SDK::UObject::GObjects->GetByIndex(i);
                if (!o || o->IsDefaultObject() || !o->IsA(mgrCls)) continue;
                auto* ma = static_cast<SDK::AActor*>(o);
                const int32_t simNum = *reinterpret_cast<int32_t*>(reinterpret_cast<uintptr_t>(o) + 0x358);
                HxLog("[HalcyonA2][CLIENT][MGR] %s outline@0x358=%d role=%d remote=%d repl=%d\n",
                      o->GetName().c_str(), simNum, (int)ma->Role, (int)ma->RemoteRole, (int)ma->bReplicates);
            }
        }
    }

    // Sample EVERY ball actor (the old 12-entry cap sampled GObjects in index order and routinely
    // missed the very ball the driver was hitting), but only PRINT the ones that actually moved, so
    // the log stays readable and any movement anywhere is impossible to miss.
    static const char* kBallClasses[] = { "BP_JakeBall_C", "BP_VolleyJakeball_C", "BP_TestBall_C" };
    int n = 0, moved = 0;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    SDK::UClass* clsCache[3] = { nullptr, nullptr, nullptr };
    for (int c = 0; c < 3; ++c) clsCache[c] = SDK::UObject::FindClassFast(kBallClasses[c]);
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        bool isBall = false;
        for (int c = 0; c < 3 && !isBall; ++c) if (clsCache[c] && o->IsA(clsCache[c])) isBall = true;
        if (!isBall) continue;
        auto* a = static_cast<SDK::AActor*>(o);
        const SDK::FVector p = a->K2_GetActorLocation();
        auto& prev = g_ballPrev[o];
        double d = 0.0;
        if (prev.seen)
        {
            const double dx = p.X - prev.pos.X, dy = p.Y - prev.pos.Y, dz = p.Z - prev.pos.Z;
            d = sqrt(dx * dx + dy * dy + dz * dz);
        }
        const bool isNew = !prev.seen;
        const SDK::FVector prevPos = prev.pos;
        prev.stillFor = (prev.seen && d <= 0.5) ? prev.stillFor + 1 : 0;
        prev.pos = p; prev.seen = true;
        ++n;
        if (d > 0.5 || isNew)
        {
            if (d > 0.5) ++moved;
            // per-axis delta too: the hit test pushes purely along +X, and a scalar distance
            // cannot be told apart from the arena's ambient bobbing.
            HxLog("[HalcyonA2][BALLTEST] %s %-30s pos=(%.0f, %.0f, %.0f) d=(%+.0f, %+.0f, %+.0f) moved=%.1f role=%d%s\n",
                  NowStamp(), a->GetName().c_str(), p.X, p.Y, p.Z,
                  isNew ? 0.0 : p.X - prevPos.X, isNew ? 0.0 : p.Y - prevPos.Y, isNew ? 0.0 : p.Z - prevPos.Z,
                  d, (int)a->GetLocalRole(), isNew ? "  <-- NEW ACTOR" : "");
        }
        // [REPDATA] The whole client-authored ball path ends in UA2PhysicsSync::PhysicsSyncRepData
        // (@sync+0xC0, Net + RepNotify): the owning client streams it up, the server stores it and
        // marks it dirty, and every OTHER client is supposed to receive it and move its ball in
        // OnRep_Data. The ball ACTOR on the server never moves in this design, so watching actor
        // positions cannot tell "the data never replicated" apart from "the data arrived and was
        // ignored". Print the replicated struct itself: owningActor tells us whether the ownership
        // handshake reached this client at all, and Timestamp/position tell us whether the stream is
        // arriving. UA2PhysicsSync lives at ball+0x4F0.
        {
            void* psync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(o) + 0x4F0);
            if (psync)
            {
                const uintptr_t rd = reinterpret_cast<uintptr_t>(psync) + 0xC0;
                const double     rts = *reinterpret_cast<double*>(rd + 0x00);
                auto*            rown = *reinterpret_cast<SDK::AActor**>(rd + 0x08);
                const SDK::FVector rp = *reinterpret_cast<SDK::FVector*>(rd + 0x18);
                auto& seen = g_repSeen[o];
                if (rown || (rts != seen))
                {
                    seen = rts;
                    HxLog("[HalcyonA2][REPDATA] %s %-28s repOwner=%s repTs=%.2f repPos=(%.0f, %.0f, %.0f) actorPos=(%.0f, %.0f, %.0f)\n",
                          NowStamp(), a->GetName().c_str(),
                          rown ? rown->GetName().c_str() : "<null>", rts, rp.X, rp.Y, rp.Z, p.X, p.Y, p.Z);
                }
            }
        }
        // [HITTEST] For the 12s after a hit, print the target unconditionally (even at rest) so a
        // "did it move?" answer never depends on the mover filter. On the WATCHER the target is
        // identified by world position, because the actor name differs per client.
        if (g_hitAtMs && GetTickCount64() - g_hitAtMs < 30000)
        {
            const double tdx = p.X - g_hitTargetPos.X, tdy = p.Y - g_hitTargetPos.Y, tdz = p.Z - g_hitTargetPos.Z;
            const bool isTarget = (o == g_hitTarget) ||
                                  (a->Class == g_hitTargetClass && sqrt(tdx * tdx + tdy * tdy + tdz * tdz) < 400.0);
            if (isTarget)
                HxLog("[HalcyonA2][HITTEST] %s TARGET %-28s pos=(%.0f, %.0f, %.0f) dFromHitPos=(%.0f, %.0f, %.0f) step=%.1f\n",
                      NowStamp(), a->GetName().c_str(), p.X, p.Y, p.Z, tdx, tdy, tdz, d);
        }
    }
    // [PINGTEST] Per-PlayerState ping for a HEADLESS mock client. Reporting only the MAX was
    // ambiguous - a remote player on cellular hid our own wired figure - so list each one.
    // Path matches [PARR]: GameState(UWorld+0x160).PlayerArray@0x2B0, count@0x2B8, reflected
    // PlayerState::GetPingInMilliseconds.
    float myPingMs = -1.0f;
    int   pingN    = 0;
    char  pingList[512]; pingList[0] = 0;
    if (auto* wPing = SDK::UWorld::GetWorld())
    {
        void* gsP = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(wPing) + 0x160);
        if (gsP)
        {
            void** parr = *reinterpret_cast<void***>(reinterpret_cast<uintptr_t>(gsP) + 0x2B0);
            const int pn = *reinterpret_cast<int*>(reinterpret_cast<uintptr_t>(gsP) + 0x2B8);
            for (int k = 0; parr && k < pn && k < 12; ++k)
            {
                auto* ps = reinterpret_cast<SDK::UObject*>(parr[k]);
                if (!ps || !ps->Class) continue;
                auto* fn = ps->Class->GetFunction("PlayerState", "GetPingInMilliseconds");
                if (!fn) continue;
                char pb[8] = {}; ps->ProcessEvent(fn, pb);
                const float v = *reinterpret_cast<float*>(pb);
                char one[96];
                _snprintf_s(one, sizeof(one), _TRUNCATE, "%s%s=%.0f",
                            pingN ? " " : "", ps->GetName().c_str(), v);
                strncat_s(pingList, sizeof(pingList), one, _TRUNCATE);
                ++pingN;
                if (v > myPingMs) myPingMs = v;
            }
        }
    }
    HxLog("[HalcyonA2][BALLTEST] %s SUMMARY pawn=%s balls=%d movedThisSecond=%d maxPing=%.1f ps=%d [%s]\n",
          NowStamp(), pawn ? pawn->GetName().c_str() : "<none>", n, moved, myPingMs, pingN, pingList);
}

// [ARENABALL] Where the arena we joined actually is. The driver used to target the ball nearest to
// its own pawn, which spawns at the station entry -- so it kept hitting a ball belonging to a
// DIFFERENT arena (e.g. it joined TKB_Prime at (0, 27250, -28275) but hit a ball at the PlazaEast
// arena, (-4425, 18550, -29875)). The ball must belong to the arena the player is actually in.
// Slot centres from the server's own [SLOTS] dump; -HalcyonArenaPos=x,y,z overrides.
static SDK::FVector g_arenaCenter{ 0.0, 27250.0, -28275.0 };   // TKB_Prime

// [HITTEST] The nearest BP_JakeBall_C that has been AT REST for at least `minStill` samples. Hitting
// an already-bouncing ball made the result unreadable (the arena has plenty of ambient motion), so
// the test picks a stationary ball: any movement afterwards is attributable to the hit.
static SDK::AActor* NearestRestingJakeBall(const SDK::FVector& from, double& outDist, int minStill)
{
    static SDK::UClass* ballCls = nullptr;   // [PERF] cached class lookup
    if (!ballCls) ballCls = SDK::UObject::FindClassFast("BP_JakeBall_C");
    if (!ballCls) return nullptr;
    SDK::AActor* best = nullptr; double bestD = 1e30;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject()) continue;
        // EXACT class, not IsA: BP_TackleballTrainingBall_C and the death balls derive from
        // BP_JakeBall_C, and the training balls sit as a parked pool of ~40 actors stacked at one
        // coordinate (-5, 16000, -33425). Being permanently at rest they always won the "resting"
        // filter, so the hit test kept firing at an inactive spawn pool instead of an arena ball.
        if (o->Class != ballCls) continue;
        // [ARENABALL] skip the pooled/inactive balls: BP_PoolingManager_C owns ~50 of them, parked
        // off-arena. Only live arena balls (owner null) are legitimate targets.
        auto* aa = static_cast<SDK::AActor*>(o);
        if (aa->Owner && aa->Owner->GetName().find("PoolingManager") != std::string::npos) continue;
        auto it = g_ballPrev.find(o);
        if (minStill > 0 && (it == g_ballPrev.end() || it->second.stillFor < minStill)) continue;
        auto* a = static_cast<SDK::AActor*>(o);
        const SDK::FVector p = a->K2_GetActorLocation();
        const double dx = p.X - from.X, dy = p.Y - from.Y, dz = p.Z - from.Z;
        const double d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) { bestD = d; best = a; }
    }
    outDist = (best ? sqrt(bestD) : 0.0);
    return best;
}
static SDK::AActor* NearestJakeBall(const SDK::FVector& /*fromPawn*/, double& outDist)
{
    // [ARENABALL] always measured from the ARENA we joined, never from the pawn.
    SDK::AActor* a = NearestRestingJakeBall(g_arenaCenter, outDist, 3);   // prefer one standing still
    if (!a) a = NearestRestingJakeBall(g_arenaCenter, outDist, 0);        // else any arena ball
    return a;
}

// [FREQDATA] Move the player ON THE SERVER.
//
// The first hit attempts failed because the driver only teleported its pawn LOCALLY
// (AActor::K2_SetActorLocation on a client is cosmetic - the server owns the pawn), so the server
// still saw the player hundreds of units away and rejected Server_HitProp: only the one attempt that
// happened to land within ~60 units ever produced Client_HitProp_Response SUCCESS=1.
//
// A real VR client feeds its transform to the server through
// UA2PlayerEntity::Server_SetFrequentData(FReplicatedFrequentData) ~every tick. Our headless client
// has no HMD/controllers so it never sends one. Do it by hand: take the entity's own
// FrequentDataReplicationOnly (@0xF0, 0x110 bytes) as a template so every field we do not understand
// keeps a sane value, overwrite the root + both hand positions (FReplicatedTransformData.position is
// at +0x00 of each; Root @0x10, leftHand @0x58, rightHand @0x88), and send it. Then the server's copy
// of the player really is standing on the ball and the hit passes its proximity check.
static SDK::UObject* FindMyPlayerEntity(SDK::APawn* pawn)
{
    static SDK::UClass* cls = nullptr;   // [PERF] cached class lookup
    if (!cls) cls = SDK::UObject::FindClassFast("A2PlayerEntity");
    if (!cls || !pawn) return nullptr;
    const int32_t num = SDK::UObject::GObjects->Num();
        InterlockedIncrement(&g_walks); g_objN = num;   // [PROF] full-walk accounting
    for (int32_t i = 0; i < num; ++i)
    {
        auto* o = SDK::UObject::GObjects->GetByIndex(i);
        if (!o || o->IsDefaultObject() || !o->IsA(cls)) continue;
        auto* owner = *reinterpret_cast<SDK::APawn**>(reinterpret_cast<uintptr_t>(o) + 0xE8);
        if (owner == pawn) return o;
    }
    return nullptr;
}

// Same channel also carries the player's MOTION: baseVelocity @0xB8 and the thruster/boost flags at
// 0xD1/0xD2/0xD6. A real player hits a ball by flying into it, so pushing a velocity as well as a
// position is much closer to the real input than teleporting - and if the server validates a hit by
// the player's motion rather than (or as well as) proximity, position alone would never pass.
static bool PushServerPosition(SDK::APawn* pawn, const SDK::FVector& where, const SDK::FVector& vel)
{
    SDK::UObject* ent = FindMyPlayerEntity(pawn);
    if (!ent) return false;
    auto* fn = ent->Class ? ent->Class->GetFunction("A2PlayerEntity", "Server_SetFrequentData") : nullptr;
    if (!fn) return false;

    alignas(16) uint8_t buf[0x110];
    memcpy(buf, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(ent) + 0xF0), sizeof(buf));
    auto put = [&](size_t off, const SDK::FVector& v) {
        auto* d = reinterpret_cast<SDK::FVector*>(buf + off);
        d->X = v.X; d->Y = v.Y; d->Z = v.Z;
    };
    put(0x10, where);   // Root.position
    put(0x58, where);   // leftHand.position
    put(0x88, where);   // rightHand.position
    put(0xB8, vel);     // baseVelocity  <- the "force"/motion the player is carrying
    buf[0xD1] = 1;      // leftThrusterActive
    buf[0xD2] = 1;      // rightThrusterActive
    buf[0xD6] = 1;      // isBigBoosting
    ent->ProcessEvent(fn, buf);
    return true;
}

static int g_driveStep = 0;

static void ClientDriveActions()
{
    auto* world = SDK::UWorld::GetWorld();
    if (!world || !world->OwningGameInstance) return;
    auto& lps = world->OwningGameInstance->LocalPlayers;
    if (lps.Num() <= 0 || !lps[0] || !lps[0]->PlayerController) return;
    SDK::APlayerController* pc = lps[0]->PlayerController;
    SDK::APawn* pawn = pc->Pawn;
    if (!pawn || pawn->GetName().find("VRPawn") == std::string::npos) return;
    auto* cls = pawn->Class;
    if (!cls) return;

    switch (g_driveStep)
    {
    case 0:   // put ourselves in an arena so the arena-scoped ball-sim paths consider us
    {
        auto* fn = cls->GetFunction("VRPawn", "Server_NotifyPlayerEnteredArena");
        if (!fn) { HxLog("[HalcyonA2][BALLTEST] Server_NotifyPlayerEnteredArena missing\n"); ++g_driveStep; return; }
        const std::wstring slot = g_arenaSlot.empty() ? std::wstring(L"TKB_Prime") : g_arenaSlot;
        struct { SDK::FString newSlotID; } p{ SDK::FString(slot.c_str()) };
        pawn->ProcessEvent(fn, &p);
        HxLog("[HalcyonA2][BALLTEST] %s -> Server_NotifyPlayerEnteredArena(%ls)\n", NowStamp(), slot.c_str());
        ++g_driveStep;
        return;
    }
    case 1: case 2:   // give the server a couple of seconds to admit/seat us
        ++g_driveStep;
        return;
    case 3: case 4: case 5:   // [FREQDATA] put the player ON the ball, server-side, for 3 ticks
    {
        double dist = 0.0;
        SDK::AActor* ball = NearestJakeBall(pawn->K2_GetActorLocation(), dist);
        if (!ball) { HxLog("[HalcyonA2][BALLTEST] no BP_JakeBall_C to target\n"); return; }
        const SDK::FVector bp = ball->K2_GetActorLocation();
        SDK::FHitResult hit{};
        pawn->K2_SetActorLocation(bp, false, &hit, true);          // local, cosmetic
        // approach the ball carrying real velocity, thrusters lit, rather than just appearing on it
        const SDK::FVector approach{ 900.0, 0.0, 150.0 };
        const bool ok = PushServerPosition(pawn, bp, approach);    // the one that actually counts
        HxLog("[HalcyonA2][BALLTEST] %s step%d Server_SetFrequentData(%s) pos=(%.0f, %.0f, %.0f) vel=(900,0,150) target=%s (%.0f from arena centre (%.0f, %.0f, %.0f))\n",
              NowStamp(), g_driveStep, ok ? "ok" : "NO A2PlayerEntity/RPC", bp.X, bp.Y, bp.Z,
              ball->GetName().c_str(), dist, g_arenaCenter.X, g_arenaCenter.Y, g_arenaCenter.Z);
        ++g_driveStep;
        return;
    }
    // [HITFIX] Decompiled AVRPawn::Server_HitProp_Implementation (RVA 0x55027F0) to find out why the
    // server kept silently dropping the hit:
    //
    //   sync = Actor->vtbl[222](Actor, key);              // the ball's UA2PhysicsSync
    //   if (sync && !sync[0xBC]) {
    //       owner = sync[0xC8];                           // owningActor
    //       if (owner == this)      { if (|Force|^2 > 40000) applyHit(sync); return; }
    //       if (owner == prevOwner) { if (Timestamp >= sync[0x160]) { takeOwnership(); claim(sync, this, pos); return; }
    //                                 else log("outdated ts"); }
    //       else                    { log("diff owner"); }
    //       denyOwnership(this, Actor, 0);
    //   }
    //
    // Two things were wrong with the test, both on our side:
    //  1. Timestamp was UGameplayStatics::GetTimeSeconds() from the CLIENT's world, which starts at 0
    //     when the client loads. The server compares it against the sync's own clock (sync+0x160), and
    //     the server had been up far longer, so every hit failed the `Timestamp >= sync[0x160]` test
    //     and took the "outdated ts" branch. Read the ball's replicated sync timestamp and beat it.
    //  2. The FIRST accepted call only CLAIMS OWNERSHIP; the force is applied on a LATER call, once
    //     owner == this. Sending exactly one HitProp per cycle could therefore never move anything.
    //     Send it repeatedly: claim on the first, hit on the ones after.
    case 6: case 7: case 8: case 9:
    {
        double dist = 0.0;
        SDK::AActor* ball = (g_driveStep == 6 || !g_hitTarget)
                          ? NearestJakeBall(pawn->K2_GetActorLocation(), dist)
                          : static_cast<SDK::AActor*>(g_hitTarget);
        if (!ball) { ++g_driveStep; return; }
        auto* fn = cls->GetFunction("VRPawn", "Server_HitProp");
        if (!fn) { HxLog("[HalcyonA2][BALLTEST] Server_HitProp missing\n"); ++g_driveStep; return; }
        const SDK::FVector bp = ball->K2_GetActorLocation();

        // Beat the ball's own physics-sync clock (UA2PhysicsSync @ball+0x4F0, timestamp @+0x160).
        double syncTs = 0.0;
        void* psync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ball) + 0x4F0);
        if (psync) syncTs = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(psync) + 0x160);
        const double localT = SDK::UGameplayStatics::GetTimeSeconds(world);
        const double t = (syncTs > localT ? syncTs : localT) + 1.0;

        struct HitParams {
            SDK::AActor* Actor; SDK::AActor* previousOwner;
            double lastSeenTimestamp; double Timestamp;
            SDK::FVector position; SDK::FVector Force;
        } p{ ball, nullptr, t, t, bp, SDK::FVector{ 400000.0, 0.0, 60000.0 } };

        if (g_driveStep == 6)
        {
            g_hitTarget = ball; g_hitTargetClass = ball->Class; g_hitTargetPos = bp;
            g_hitAtMs = GetTickCount64();
        }
        // keep the player standing on the ball, moving, for every attempt
        PushServerPosition(pawn, bp, SDK::FVector{ 900.0, 0.0, 150.0 });
        pawn->ProcessEvent(fn, &p);
        HxLog("[HalcyonA2][HITTEST] %s HIT#%d %s at (%.0f, %.0f, %.0f) force=(400000,0,60000) ts=%.2f (syncTs=%.2f localT=%.2f) dist=%.0f\n",
              NowStamp(), g_driveStep - 5, ball->GetName().c_str(), bp.X, bp.Y, bp.Z, t, syncTs, localT, dist);
        ++g_driveStep;
        return;
    }
    // [STREAMBALL] Taking ownership is only half of it. Server_HitProp's `owner == this` branch does
    // NOT integrate physics for us -- once a client owns a ball it becomes the authority for it and
    // STREAMS the ball's state up, which the server applies and replicates to everyone else. That is
    // AVRPawn::Server_SendPhysicsPropData(Actor, FReplicatedPhysicsObjectData), and the server-side
    // implementation only accepts it while physicsSync->owningActor == the sending pawn -- which the
    // hits above have now arranged. So: hold ownership, and stream the ball along +X for a few
    // seconds. Template the struct from the ball's own PhysicsSyncRepData (UA2PhysicsSync @ball+0x4F0,
    // rep data @+0xC0, same 0x88 layout as the RPC parameter) so fields we do not model keep sane
    // values; then overwrite Timestamp/owningActor/position/Velocity and the update flags.
    // [BURSTSTREAM] Ownership is not sticky. The server hands the ball over on Server_HitProp, but
    // reclaims it again almost immediately (UA2PhysicsSync::ServerReclaimOwnership) unless the owner
    // keeps asserting itself. Streaming once a second was far too slow: by the first update the
    // server had already taken the ball back, and every frame came back
    // `owner=<null> ... accepted=0`. A real client streams its owned ball every tick (~90 Hz), so do
    // the same -- claim, then push position + physics state in a tight burst for a few seconds.
    case 10:
    {
        SDK::AActor* ball = static_cast<SDK::AActor*>(g_hitTarget);
        if (!ball) { ++g_driveStep; return; }
        auto* fnHit  = cls->GetFunction("VRPawn", "Server_HitProp");
        auto* fnSend = cls->GetFunction("VRPawn", "Server_SendPhysicsPropData");
        if (!fnHit || !fnSend) { HxLog("[HalcyonA2][BALLTEST] hit/send RPC missing\n"); ++g_driveStep; return; }
        void* psync = *reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(ball) + 0x4F0);
        if (!psync) { HxLog("[HalcyonA2][BALLTEST] ball has no UA2PhysicsSync\n"); ++g_driveStep; return; }

        const SDK::FVector p0 = g_hitTargetPos;
        const SDK::FVector vel{ 1200.0, 0.0, 200.0 };
        HxLog("[HalcyonA2][BURSTSTREAM] %s begin: %s from (%.0f, %.0f, %.0f), 90 Hz for ~4s\n",
              NowStamp(), ball->GetName().c_str(), p0.X, p0.Y, p0.Z);

        int accepted = 0, sent = 0;
        for (int k = 0; k < 360; ++k)          // ~4s at 90 Hz
        {
            const double syncTs = *reinterpret_cast<double*>(reinterpret_cast<uintptr_t>(psync) + 0x160);
            const double localT = SDK::UGameplayStatics::GetTimeSeconds(world);
            const double ts = (syncTs > localT ? syncTs : localT) + 0.5;
            const double f = k / 90.0;         // seconds into the burst
            SDK::FVector np{ p0.X + vel.X * f, p0.Y, p0.Z + vel.Z * f };

            // hold the player on the ball and moving, exactly like a hand carrying it
            PushServerPosition(pawn, np, vel);

            // re-assert ownership every ~0.25s so a reclaim cannot outlive one quarter second
            if ((k % 22) == 0)
            {
                struct HitParams {
                    SDK::AActor* Actor; SDK::AActor* previousOwner;
                    double lastSeenTimestamp; double Timestamp;
                    SDK::FVector position; SDK::FVector Force;
                } hp{ ball, nullptr, ts, ts, np, SDK::FVector{ 400000.0, 0.0, 60000.0 } };
                pawn->ProcessEvent(fnHit, &hp);
            }

            struct SendParams { SDK::AActor* Actor; uint8_t data[0x88]; } sp{};
            sp.Actor = ball;
            memcpy(sp.data, reinterpret_cast<const void*>(reinterpret_cast<uintptr_t>(psync) + 0xC0), sizeof(sp.data));
            *reinterpret_cast<double*>(sp.data + 0x00) = ts;
            *reinterpret_cast<SDK::AActor**>(sp.data + 0x08) = pawn;
            sp.data[0x10] = 0;
            *reinterpret_cast<SDK::FVector*>(sp.data + 0x18) = np;
            *reinterpret_cast<SDK::FVector*>(sp.data + 0x48) = vel;
            sp.data[0x80] = 1;
            sp.data[0x81] = 1;
            pawn->ProcessEvent(fnSend, &sp);
            ++sent;

            auto* own = *reinterpret_cast<SDK::AActor**>(reinterpret_cast<uintptr_t>(psync) + 0xC8);
            if (own == static_cast<SDK::AActor*>(pawn)) ++accepted;

            if ((k % 90) == 0)
            {
                const SDK::FVector cur = ball->K2_GetActorLocation();
                HxLog("[HalcyonA2][BURSTSTREAM] %s k=%d want=(%.0f, %.0f, %.0f) owner=%s ball=(%.0f, %.0f, %.0f)\n",
                      NowStamp(), k, np.X, np.Y, np.Z, own ? own->GetName().c_str() : "<null>",
                      cur.X, cur.Y, cur.Z);
            }
            Sleep(11);
        }
        const SDK::FVector fin = ball->K2_GetActorLocation();
        HxLog("[HalcyonA2][BURSTSTREAM] %s done: sent=%d ownedFrames=%d final ball=(%.0f, %.0f, %.0f) (started (%.0f, %.0f, %.0f))\n",
              NowStamp(), sent, accepted, fin.X, fin.Y, fin.Z, p0.X, p0.Y, p0.Z);
        g_driveStep = 22;
        return;
    }
    case 22: case 23: case 24: case 25:   // let the result replicate before the second probe
        ++g_driveStep;
        return;
    case 26:  // independent probe: ask the server to spawn a NEW ball that is already moving
    {
        auto* fn = cls->GetFunction("VRPawn", "Server_SpawnBall");
        if (!fn) { HxLog("[HalcyonA2][BALLTEST] Server_SpawnBall missing\n"); ++g_driveStep; return; }
        double dist = 0.0;
        SDK::AActor* ref = NearestJakeBall(pawn->K2_GetActorLocation(), dist);
        SDK::UClass* ballCls = ref ? ref->Class : SDK::UObject::FindClassFast("BP_JakeBall_C");
        if (!ballCls) { ++g_driveStep; return; }
        SDK::FVector loc = pawn->K2_GetActorLocation(); loc.Z += 150.0;
        struct SpawnParams {
            SDK::UClass* BallClass; SDK::FVector Location;
            SDK::FVector StartingVelocity; SDK::FVector AngularVelocity;
        } p{ ballCls, loc, SDK::FVector{ 0.0, 700.0, 300.0 }, SDK::FVector{ 0.0, 0.0, 2.0 } };
        pawn->ProcessEvent(fn, &p);
        HxLog("[HalcyonA2][BALLTEST] %s -> Server_SpawnBall(%s at (%.0f, %.0f, %.0f) vel=(0,700,300))\n",
              NowStamp(), ballCls->GetName().c_str(), loc.X, loc.Y, loc.Z);
        ++g_driveStep;
        return;
    }
    default:
        // re-run the whole approach -> claim -> stream cycle every ~45s
        if (g_driveStep < 45) { ++g_driveStep; return; }
        g_driveStep = 3;
        return;
    }
}

static void ClientMain(uintptr_t base)
{
    HxLog("[HalcyonA2][CLIENT] mock client mode, base=0x%llX\n", (unsigned long long)base);

    // Entitlement self-exit: same signature-guarded patch the server uses (this host has no Oculus
    // entitlement, so without it the client process quits ~30s after boot too).
    {
        static const uint8_t entSig[] = { 0x40, 0x84, 0xED, 0x75, 0x59, 0x80, 0x3D };
        const uint8_t* ep = reinterpret_cast<const uint8_t*>(base + 0x5429424);
        if (memcmp(ep, entSig, sizeof(entSig)) == 0)
        {
            WriteByte(base + 0x5429427, 0xEB);
            HxLog("[HalcyonA2][CLIENT] entitlement self-exit patched\n");
        }
        else HxLog("[HalcyonA2][CLIENT] WARNING: entitlement signature mismatch - NOT patched\n");
    }

    SDK::UWorld* world = nullptr;
    while (true)
    {
        world = SDK::UWorld::GetWorld();
        if (world && world->OwningGameInstance) break;
        Sleep(100);
    }
    HxLog("[HalcyonA2][CLIENT] world ready: %p\n", (void*)world);

    if (g_clientConnect.empty()) g_clientConnect = L"127.0.0.1:7777";
    const std::wstring cmd = L"open " + g_clientConnect;
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(cmd.c_str()), nullptr);
    HxLog("[HalcyonA2][CLIENT] issued: %ls\n", cmd.c_str());

    for (int tick = 0;; ++tick)
    {
        Sleep(1000);
        ClientTryExitSpectator();
        if (g_clientExited && tick > 3) ClientRequestVoipTokens();   // only once we hold a real pawn
        if (g_clientDrive && g_clientInArena && tick > 6) ClientDriveActions();
        ClientReportState();   // [BALLTEST] every second on BOTH clients, timestamped for correlation
    }
}

static void Main(HMODULE)
{
    if (wcsstr(GetCommandLineW(), L"-HalcyonClient"))
    {
        wcscpy_s(g_hxLogSuffix, L"client");
        // [BALLTEST] -HalcyonName=<n> gives each client its own log so two can run side by side.
        if (const wchar_t* n = wcsstr(GetCommandLineW(), L"-HalcyonName="))
        {
            n += wcslen(L"-HalcyonName=");
            wchar_t nm[24]{}; int k = 0;
            while (*n && *n != L' ' && *n != L'"' && k < 20) nm[k++] = *n++;
            if (k) swprintf_s(g_hxLogSuffix, L"client%s", nm);
        }
    }

    // [CONSOLELOG 2026-09-07] stdout/stderr used to be re-opened on CONOUT$ of a freshly allocated
    // console. On a headless server nobody ever sees that console, so EVERY printf in this payload
    // (hook install results, the VOIP channel line, "issued: open ...") plus the whole engine log
    // after injection was written to a window that is thrown away -- which is why
    // logs\server-*.out.log always stopped at "Engine is initialized". Send both streams to
    // %TEMP%\HalcyonA2-console.log instead (same folder as HalcyonA2.log), so a headless run is
    // actually diagnosable. -HxConsole restores the old on-screen console.
    FILE* dummy;
    if (wcsstr(GetCommandLineW(), L"-HxConsole"))
    {
        AllocConsole();
        freopen_s(&dummy, "CONOUT$", "w", stdout);
        freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
    else
    {
        wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
        wchar_t cpath[MAX_PATH]{};
        swprintf_s(cpath, g_hxLogSuffix[0] ? L"%sHalcyonA2-%s-console.log" : L"%sHalcyonA2-console.log",
                   tmp, g_hxLogSuffix);
        // Open with FILE_SHARE_READ so the log can be tailed WHILE the process runs (the old
        // _wfreopen_s handle was exclusive, so every check had to wait for the server to die first).
        HANDLE hf = CreateFileW(cpath, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (hf != INVALID_HANDLE_VALUE)
        {
            const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(hf), _O_WRONLY | _O_APPEND);
            if (fd != -1) { _dup2(fd, _fileno(stdout)); _dup2(fd, _fileno(stderr)); _close(fd); }
            else CloseHandle(hf);
        }
        else { _wfreopen_s(&dummy, cpath, L"w", stdout); _wfreopen_s(&dummy, cpath, L"a", stderr); }
        setvbuf(stdout, nullptr, _IOLBF, 4096);
        setvbuf(stderr, nullptr, _IOLBF, 4096);
        HxLog("[HalcyonA2][CONSOLELOG] stdout/stderr -> %ls\n", cpath);
    }

    const uintptr_t base = GetBase();
    printf("[HalcyonA2] attached. base = 0x%llX\n", (unsigned long long)base);

    // [CLIENTMODE] -HalcyonClient turns this injection into a headless test CLIENT instead of a
    // server: no GIsServer flip, no net-mode forcing, no map open, no ball-sim hooks. See ClientMain.
    if (wcsstr(GetCommandLineW(), L"-HalcyonClient"))
    {
        if (wcsstr(GetCommandLineW(), L"-HalcyonDrive")) g_clientDrive = true;
        if (const wchar_t* ap = wcsstr(GetCommandLineW(), L"-HalcyonArenaPos="))
        {
            ap += wcslen(L"-HalcyonArenaPos=");
            double xyz[3] = { g_arenaCenter.X, g_arenaCenter.Y, g_arenaCenter.Z };
            for (int k = 0; k < 3 && *ap; ++k)
            {
                wchar_t* e = nullptr;
                xyz[k] = wcstod(ap, &e);
                if (e == ap) break;
                ap = (*e == L',') ? e + 1 : e;
            }
            g_arenaCenter.X = xyz[0]; g_arenaCenter.Y = xyz[1]; g_arenaCenter.Z = xyz[2];
        }
        if (const wchar_t* a = wcsstr(GetCommandLineW(), L"-HalcyonArena="))
        {
            a += wcslen(L"-HalcyonArena=");
            while (*a && *a != L' ' && *a != L'"') { g_arenaSlot += *a; ++a; }
        }
        if (const wchar_t* c = wcsstr(GetCommandLineW(), L"-HalcyonConnect="))
        {
            c += wcslen(L"-HalcyonConnect=");
            while (*c && *c != L' ' && *c != L'"') { g_clientConnect += *c; ++c; }
        }
        ClientMain(base);
        return;   // never falls through to the server path
    }

    // Wait for a live world + game instance (we may be injected in the frontend).
    SDK::UWorld* world = nullptr;
    while (true)
    {
        world = SDK::UWorld::GetWorld();
        if (world && world->OwningGameInstance)
            break;
        Sleep(100);
    }
    printf("[HalcyonA2] world ready: 0x%llX\n", (unsigned long long)world);

    // 1. Flip into server mode BEFORE travel. If we flip after open, the new world
    //    runs its whole init (subsystem Initialize, physics-step callback arming,
    //    package export loading) while the process still looks like a client, so the
    //    server-authoritative paths (incl. the Mass physics plumbing) never arm.
    WriteByte(base + GIsClient_RVA, 0); // GIsClient = false
    WriteByte(base + GIsServer_RVA, 1); // GIsServer = true
    printf("[HalcyonA2] GIsClient=false, GIsServer=true (before open)\n");

    // [PORT-AUDIT] Entitlement self-exit guard for the SERVER process. When the Oculus entitlement check fails on the
    // host (err log: "GetSignatureToken get_signature error: Missing entitlement"), LogA2MothershipAuthStateMachine
    // requests an engine exit ("Could not verify entitlement status ... Closing by request") ~30s after boot and the
    // process dies (exit code 3, crash in the object-teardown loop). Same patch as the A2EntitlementPatch UE4SS mod:
    // flip the jne at 0x5429427 (20996: 0x53DFCC7) to jmp. Signature-guarded.
    {
        static const uint8_t entSig[] = { 0x40, 0x84, 0xED, 0x75, 0x59, 0x80, 0x3D };
        const uint8_t* ep = reinterpret_cast<const uint8_t*>(base + 0x5429424);
        if (memcmp(ep, entSig, sizeof(entSig)) == 0 && ep[11] == 0x02 && ep[12] == 0x72 && ep[13] == 0x25 && ep[14] == 0x83 && ep[15] == 0x7F)
        {
            WriteByte(base + 0x5429427, 0xEB);
            printf("[HalcyonA2] entitlement self-exit patched (0x5429427 jne->jmp)\n");
            HxLog("[HalcyonA2] entitlement self-exit patched (0x5429427 jne->jmp)\n");
        }
        else { printf("[HalcyonA2] WARNING: entitlement patch signature mismatch @0x5429424 - NOT patched\n"); HxLog("[HalcyonA2] WARNING: entitlement patch signature mismatch @0x5429424 - NOT patched\n"); }
    }

    // [PORT 22284] CRASH GUARD #1: render/RHI vtable thunk sub_550F130 null-derefs on -nullrhi headless
    // (mov rcx,[rcx+0x890]; mov rax,[rcx]; jmp [rax+0x600]) — render subobject @this+0x890 is NULL, so
    // [null] faults during map-load/GC teardown. Neuter -> ret (same as the 29932 render-thunk fix).
    WriteByte(base + 0x550F130, 0xC3);
    printf("[HalcyonA2] [PORT 22284] patched render-thunk sub_550F130 -> ret (nullrhi crash guard #1)\n");

    // [22284 TEST] Neuter stepSim (the manager's engine-registered tick = the garbage-building
    // client-perspective SimulationsOutline walk). If balls still move (Mass) + flood stops, we win.
    if (g_neuterStepSim)
    {
        WriteByte(base + 0x54863C0, 0xC3);
        printf("[HalcyonA2] [TEST] stepSim (0x54863C0) neutered -> ret\n");
    }

    // [PORT 22284] CRASH GUARD #2 (worker-thread GC null-ref): install the scoped GC-GUARD VEH. The
    // engine's parallel GC cluster-reference pass bulk-derefs batched UObject refs with no null check
    // and AVs on a Background/Foreground Worker when a headless -nullrhi reference is null. Bisected
    // independent of spectator + ballsim. VEH skips the null (repoints base reg -> zero page). Ranges
    // = sub_124b9d0 (+0x145), sub_124db00 (+0x8e8), sub_124ece0 (+0x7d8).
    g_gcZeroPage = VirtualAlloc(nullptr, 0x20000, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE); // 128KB > 64KB null zone
    // [2026-09-01] Widened to the WHOLE GC reference-collector cluster (all 15 TFastReferenceCollector
    // template instantiations span ~0x1245000..0x1269000; verified in IDA as the callers of sub_124ECE0).
    // A crash at RVA 0x124F5F1 sat just past the old range-3 end (0x124F4B8) -> hard crash. One wide range
    // catches every null-ref in the collector regardless of which sibling fn it lands in.
    g_gcRanges[0] = base + 0x1245000; g_gcRanges[1] = base + 0x1269000;   // whole collector cluster
    g_gcRanges[2] = base + 0x124DB00; g_gcRanges[3] = base + 0x124DB00 + 0x8E8;
    g_gcRanges[4] = base + 0x124ECE0; g_gcRanges[5] = base + 0x124ECE0 + 0x7D8;
    if (g_gcZeroPage) { AddVectoredExceptionHandler(1, &GcNullRefVeh);
        printf("[HalcyonA2] [PORT 22284] GC-GUARD VEH installed (null-ref skip in parallel GC cluster pass)\n"); }
    else printf("[HalcyonA2] [PORT 22284] GC-GUARD VEH NOT installed (zero-page alloc failed)\n");

    // [2026-09-03] CUSTOM CRASH REPORTER — registered LAST (FirstHandler=0 => back of the VEH chain) so our
    // handled GC/pawn faults are consumed before it; it only sees the genuinely-fatal fault, logs rip(RVA)+
    // regs+stack to %TEMP%\HalcyonA2.log, then lets the process die normally. See CrashReporterVeh.
    AddVectoredExceptionHandler(0, &CrashReporterVeh);
    printf("[HalcyonA2] custom crash reporter VEH installed (logs rip/regs/stack on fatal fault)\n");

    // [2026-09-02 ★ ROOT FIX] ArrCopyDrv_Hook install is DEFERRED to after MH_Initialize() below (MinHook
    // must be initialized first, or MH_CreateHook returns NOT_INITIALIZED and the hook silently no-ops).
    if (g_pawnSpawnGuard)   // default false — kept as an emergency fallback only
    {
        g_pawnSpawnCrashRip = base + 0x1326B07;   // `mov rdi,[r14+rbx]` in sub_7FF673386AD0
        AddVectoredExceptionHandler(1, &PawnSpawnCrashVeh);
        printf("[HalcyonA2] PAWN-GUARD VEH installed [FALLBACK] (bad-archetype pawn-spawn skip @ +0x1326B07 = 0x%llX)\n",
               (unsigned long long)g_pawnSpawnCrashRip);
    }

    // [2026-09-01 ★ ROOT FIX] Clamp the unchecked Num read in the GC reference-token interpreter so a
    // corrupt {Data=0, Num=0xFFFFFFFF} field never enqueues billions of refs (the flood). Class-agnostic,
    // touches no object memory -> replaces the per-class UBallSpawnerComponent ARRCLAMP (now disabled).
    InstallGcNumClamp(base);
    InstallJakeballLookupNullGuard(base);   // [2026-09-02] fix 2nd-player-join AV @ 0x1D54 in ballsim lookup
    // NOTE: the FULL GC NEUTER (CollectGarbageInternal 0x12494D0 -> ret) is applied LATER, in the
    // GamemodesTracker spawn, NOT here — the EntryLevel->Station travel GC must run first or LoadMap
    // hard-FATALs with "Fatal world leaks detected". See that site.

    // Station Dashboard reporting gate: sub_541D8A0 (the reporting init that fetches
    // deployment/station config -> board netvars) bails unless GetNetMode() == 2 (NM_ListenServer).
    // We force GetNetMode to 1 (NM_DedicatedServer), so patch the compare `cmp eax, 2` (0541D8EF:
    // 83 F8 02) -> `cmp eax, 1` so our net mode passes the gate. Then, given the three
    // -Dashboard* cmdline args, the game natively registers + fetches config + reports.
    // sub_541D8A0 gate B: `cmp eax, 2 ; j..` on GetNetMode(ctx). Our GetNetMode hook returns garbage
    // for the ctx this init passes (`(*(clientVtable+392))()` isn't a plain UWorld), so comparing
    // against ANY constant fails and the init bails registering nothing. Neutralize the compare so it
    // ALWAYS passes: overwrite `cmp eax, 2` (83 F8 02 @ 0x541D8EF) with `xor eax,eax ; nop` (31 C0 90)
    // -> ZF=1 -> the following j.. takes the "equal" (pass) path into the init body. eax is reassigned
    // right after the gate, so clobbering it is safe.
    // [22284] DashboardInit sub_1454B13D0 gate: `cmp eax, 2` (83 F8 02) @ 0x54B141F on GetNetMode(ctx).
    // We run dedicated (net mode 1), so patch the imm 02 -> 01 (byte @ 0x54B1421) so the gate passes.
    WriteByte(base + 0x54B1421, 0x01);
    printf("[HalcyonA2] patched dashboard-init net-mode gate (54B141F: cmp eax,2 -> cmp eax,1)\n");

    // [2026-09-01 ★ BALLSIM SPAWN GATE] sub_7FF67743C730 (RVA 0x53DC730) is the GAME'S OWN BallSimManager
    // spawner (SpawnActor of ABallSimManager + sets the ballsim world global qword_7FF67BCBC880). It only
    // spawns when GetNetMode==2 (ListenServer) or ==0 (Standalone/offline) — NEVER on dedicated(1). That's
    // why offline (netmode 0) has a working, natively-ticking+pruning manager and our forced-dedicated(1)
    // server has managers=0 (so we hand-spawned a half-wired, tick-disabled one that floods). FIX: patch
    // the `cmp eax, 2` (83 F8 02 @ 0x53DC768) imm 02 -> 01 (byte @ 0x53DC76A) so netmode 1 matches -> the
    // game spawns + wires + ticks the manager ITSELF exactly like offline. No custom spawn, no double-drive,
    // native reconcile prunes the sim set (stays at 1) -> no GC flood.
    if (g_ballsimEnabled)
    {
        // [2026-09-04] HAND-SPAWN mode: do NOT patch the native gate, or the game would ALSO spawn a manager
        // (two managers). SpawnBallSimManagerIfNeeded creates + subsystem-wires + ticks ours instead.
        printf("[HalcyonA2] hand-spawn mode: native BallSimManager spawn gate LEFT CLOSED (we spawn our own, 20996-style)\n");
    }
    else if (!g_delayBallSimSpawn)
    {
        WriteByte(base + 0x53DC76A, 0x01);
        printf("[HalcyonA2] patched BallSimManager spawn gate (53DC768: cmp eax,2 -> cmp eax,1) - native spawn on dedicated\n");
    }
    else
        printf("[HalcyonA2] [DELAY-EXP] BallSimManager spawn gate LEFT CLOSED at boot; late spawn scheduled +%dms after tracker\n",
               g_ballSimSpawnDelayMs);

    // Seed the server dashboard api-key global BEFORE the native login runs, so the server's own
    // sub_54AB570 -> log_in_with_key carries x-api-key = our key and authenticates. (Also re-seeded +
    // login re-triggered from the ticker as a fallback if the native login already ran.)
    // [PORT 22284] api-key seed skipped: qword_9BD4460 + sub_FBCD70 not re-found; pass
    // -DashboardApiKey=halcyon-server-key on the cmdline instead (DashboardInit reads it natively).
    // SafeSeedDashboardApiKey();

    // Force the station-dashboard log categories to VeryVerbose(7) so the SILENT skip-branches print
    // (sub_5409770's config-apply gates only log at >=5). byte_9BD4450 = LogA2StationDashboard (config
    // apply + login-response/roles), byte_9BD4170 = LogA2SessionSubsystem (auth). The category's first
    // byte IS its runtime verbosity (that's what the `byte >= N` guards read).
    // [PORT 22284] log-verbosity globals (0x9BD4450/0x9BD4170) not re-found — skip (diagnostic only).

    // Opt-in gamemode load via CMDLINE (rides in through agent.cfg game_args like the -Dashboard*
    // args; env vars don't reach the game process under the allocator): -LoadGamemode=<path>
    // (e.g. deathrun) loads it into a module slot -GamemodeSlot=<id> (default PKR_Custom_Full) once
    // the world is up. Unset = no-op.
    {
        const wchar_t* cl = GetCommandLineW();
        auto grab = [cl](const wchar_t* key, char* out, size_t sz) {
            const wchar_t* p = wcsstr(cl, key);
            if (!p) return;
            p += wcslen(key);
            wchar_t quote = 0;
            if (*p == L'"' || *p == L'\'') { quote = *p; ++p; }   // tolerate a quoted value
            wchar_t val[96] = {}; int i = 0;
            while (*p && i < 95 && (quote ? (*p != quote) : (*p != L' '))) val[i++] = *p++;
            size_t cvt = 0; wcstombs_s(&cvt, out, sz, val, _TRUNCATE);
        };
        grab(L"-LoadGamemode=", g_loadGmPath, sizeof(g_loadGmPath));
        grab(L"-GamemodeSlot=", g_loadGmSlot, sizeof(g_loadGmSlot));
        char posbuf[96] = {};
        grab(L"-GamemodePos=", posbuf, sizeof(posbuf));   // "X,Y,Z"
        if (posbuf[0] && sscanf_s(posbuf, "%lf,%lf,%lf", &g_gmPos[0], &g_gmPos[1], &g_gmPos[2]) == 3)
            g_gmPosSet = true;
        grab(L"-SnapToMarker=", g_snapMarker, sizeof(g_snapMarker));   // fullname substr of a marker actor
        char nudgebuf[96] = {};
        grab(L"-GmNudge=", nudgebuf, sizeof(nudgebuf));   // "X,Y,Z" world-space nudge of the course origin
        if (nudgebuf[0]) sscanf_s(nudgebuf, "%lf,%lf,%lf", &g_gmNudge[0], &g_gmNudge[1], &g_gmNudge[2]);
        if (wcsstr(GetCommandLineW(), L"-GmRewriteRot")) g_forceRot = true;
        if (wcsstr(GetCommandLineW(), L"-SpawnSlotAtMarker")) g_spawnSlot = true;
        char rotbuf[96] = {};
        grab(L"-SlotRot=", rotbuf, sizeof(rotbuf));   // "P,Y,R" extra rotation on the spawned slot
        if (rotbuf[0]) sscanf_s(rotbuf, "%lf,%lf,%lf", &g_slotRotAdj[0], &g_slotRotAdj[1], &g_slotRotAdj[2]);
        grab(L"-BallClass=", g_ballClass, sizeof(g_ballClass));   // renderable ball to replace broken death balls
        if (wcsstr(GetCommandLineW(), L"-KillMgrRepl")) { g_disableMgrRepl = true; HxLog("[HalcyonA2] -KillMgrRepl: BallSimManager set non-replicating (old flood-suppression behaviour)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoAuthGate")) { g_gateEnforce = false; HxLog("[HalcyonA2][GATE] -NoAuthGate: auth gate is LOG-ONLY (no kicks) - testing only\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoFixLOD")) g_fixLod = false;   // disable the DefaultLODSettings pop-in fix
        if (wcsstr(GetCommandLineW(), L"-BallPump"))   { g_ballPumpsEnabled = true;  HxLog("[HalcyonA2] -BallPump: manual ball-sim pump ENABLED\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoBallPump")) { g_ballPumpsEnabled = false; HxLog("[HalcyonA2] -NoBallPump: manual ball-sim pump DISABLED\n"); }
        if (wcsstr(GetCommandLineW(), L"-ArmResultsOnStep")) { g_armResultsOnStep = true; HxLog("[HalcyonA2] -ArmResultsOnStep: arm results-send per contested sim step (MI-reduction lever, test for stutter)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoArmResultsOnStep")) { g_armResultsOnStep = false; HxLog("[HalcyonA2] -NoArmResultsOnStep: results-send arming OFF (A/B: reverts the MI fix)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoTempYank")) { g_tempYank = false; HxLog("[HalcyonA2] -NoTempYank: resync writes the shared sim frame PERMANENTLY again (A/B: reverts the MI thrash fix)\n"); }
        // [DEPLOYABILITY] backend location + shared secret, so moving the VPS is a config change not a rebuild.
        {
            auto grabW = [](const wchar_t* key, std::wstring& out) {
                const wchar_t* p = wcsstr(GetCommandLineW(), key);
                if (!p) return false;
                p += wcslen(key);
                std::wstring v;
                if (*p == L'"') { ++p; while (*p && *p != L'"') v += *p++; }
                else            { while (*p && *p != L' ' && *p != L'\t') v += *p++; }
                if (v.empty()) return false;
                out = v; return true;
            };
            if (grabW(L"-MothershipHost=", g_motherHost))
                HxLog("[HalcyonA2] -MothershipHost=%ls\n", g_motherHost.c_str());
            if (grabW(L"-BackendHost=", g_backendHost))
                HxLog("[HalcyonA2] -BackendHost=%ls (register_server / player-count)\n", g_backendHost.c_str());
            if (const wchar_t* bp = wcsstr(GetCommandLineW(), L"-BackendPort="))
            {
                const int v = _wtoi(bp + 13);
                if (v > 0 && v < 65536) { g_backendPort = v; HxLog("[HalcyonA2] -BackendPort=%d\n", v); }
            }
            if (const wchar_t* mp = wcsstr(GetCommandLineW(), L"-MothershipPort="))
            {
                const int v = _wtoi(mp + 16);
                if (v > 0 && v < 65536) { g_motherPort = v; HxLog("[HalcyonA2] -MothershipPort=%d\n", v); }
            }
            std::wstring keyTmp;
            if (grabW(L"-ServerApiKey=", keyTmp))
            {
                g_serverApiKey = keyTmp;
                HxLog("[HalcyonA2] -ServerApiKey set from cmdline (%zu chars)\n", g_serverApiKey.size());
            }
        }
        if (const wchar_t* yb = wcsstr(GetCommandLineW(), L"-YankBudget="))   // 0 = unlimited (old behaviour)
        {
            const int v = _wtoi(yb + 13);
            if (v >= 0 && v <= 1000) { g_yankBudget = v; HxLog("[HalcyonA2] -YankBudget=%d resync yanks/player/sec (0=unlimited)\n", v); }
        }
        if (wcsstr(GetCommandLineW(), L"-NoNetRateTune")) { g_tuneNetRates = false; HxLog("[HalcyonA2] -NoNetRateTune: per-connection bandwidth cap left at engine default (A/B the pose-rate fix)\n"); }
        if (wcsstr(GetCommandLineW(), L"-ReapOrphans")) { g_reapOrphans = true; HxLog("[HalcyonA2] -ReapOrphans: destroy VRPawns orphaned >30s (leak fix; destroys actors)\n"); }
        if (wcsstr(GetCommandLineW(), L"-HoldAuthority")) { g_holdAuthority = true; HxLog("[HalcyonA2] -HoldAuthority: held/streamed balls pinned to the client stream every frame (anti-teleport)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoBallOwnArb")) { g_ballOwnArb = false; HxLog("[HalcyonA2] -NoBallOwnArb: ball ownership arbitration OFF (legacy force-accept, last writer wins)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoRestCurveFix")) { g_fixRestCurve = false; HxLog("[HalcyonA2] -NoRestCurveFix: will NOT restore a missing hand-speed restitution curve\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoTimerGate")) { g_gateTimers = false; HxLog("[HalcyonA2] -NoTimerGate: driving ALL learned game timers again (pre-ScrapRun-fix behaviour)\n"); }
        if (const wchar_t* sd = wcsstr(GetCommandLineW(), L"-SimDelay="))
        {
            const int v = _wtoi(sd + wcslen(L"-SimDelay="));
            if (v >= 0 && v <= 30) { g_simDelay = v; HxLog("[HalcyonA2] -SimDelay=%d frames (~%dms of input buffer at 90Hz)\n", v, (v * 1000) / 90); }
        }
        if (wcsstr(GetCommandLineW(), L"-ParallelGC")) { g_parallelGC = true; HxLog("[HalcyonA2] -ParallelGC: parallel GC ON, clusters still OFF (crash-guard experiment)\n"); }
        if (wcsstr(GetCommandLineW(), L"-HalcyonMinimal")) { g_minimal = true; HxLog("[HalcyonA2] -HalcyonMinimal: optional detectors OFF (diagnostic only)\n"); }
        if (wcsstr(GetCommandLineW(), L"-HalcyonDiag")) { g_diag = true; HxLog("[HalcyonA2] -HalcyonDiag: TEMP probe/dump diagnostics ENABLED (costs 250-400ms/s on a weak core)\n"); }
        if (const wchar_t* ns = wcsstr(GetCommandLineW(), L"-NetSpeed="))   // bytes/s per connection
        {
            const int v = _wtoi(ns + 10);
            if (v >= 10000 && v <= 10000000) { g_netSpeedTarget = v; HxLog("[HalcyonA2] -NetSpeed=%d bytes/s per connection\n", v); }
        }
        if (wcsstr(GetCommandLineW(), L"-NoFrameRebase")) { g_frameRebase = false; HxLog("[HalcyonA2] -NoFrameRebase: staggered-join epoch rebase OFF (default)\n"); }
        if (wcsstr(GetCommandLineW(), L"-FrameRebase"))   { g_frameRebase = true;  HxLog("[HalcyonA2] -FrameRebase: epoch rebase ON (WARNING: in-place frame rewrite feeds back -> rejoin brick)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoHoldTarget"))  { g_holdSimAtTarget = false; HxLog("[HalcyonA2] -NoHoldTarget: input-delay hold OFF (default)\n"); }
        if (wcsstr(GetCommandLineW(), L"-HoldTarget"))    { g_holdSimAtTarget = true;  HxLog("[HalcyonA2] -HoldTarget: input-delay hold ON (WARNING: freezes the sim unless all players share one epoch)\n"); }
        if (wcsstr(GetCommandLineW(), L"-NoBallTune")) g_tuneBallNet = false;
        if (wcsstr(GetCommandLineW(), L"-PinSimResults")) g_pinSimResults = true;
        if (wcsstr(GetCommandLineW(), L"-QuietSims")) g_quietSims = true; // A/B: kill all our ball net sends (player-lag test)
        char spleefbuf[32] = {};
        grab(L"-SpleefRadius=", spleefbuf, sizeof(spleefbuf));   // deathrun finish->overtime fire radius (default 2000)
        if (spleefbuf[0]) { double r = atof(spleefbuf); if (r > 0.0) g_spleefRadius = r; }
    }
    printf("[HalcyonA2] deathrun finish->spleef fire radius = %.0f (-SpleefRadius=N to tune)\n", g_spleefRadius);
    if (g_quietSims) printf("[HalcyonA2] -QuietSims: ball sim step + results-send + physics-sync DISABLED (player-lag A/B)\n");
    if (g_snapMarker[0]) printf("[HalcyonA2] -SnapToMarker='%s'\n", g_snapMarker);
    if (g_gmNudge[0]||g_gmNudge[1]||g_gmNudge[2]) printf("[HalcyonA2] -GmNudge=(%.0f,%.0f,%.0f)\n", g_gmNudge[0],g_gmNudge[1],g_gmNudge[2]);
    if (g_loadGmPath[0])
        printf("[HalcyonA2] -LoadGamemode='%s' -> slot '%s'%s (will load after world up)\n",
               g_loadGmPath, g_loadGmSlot,
               g_gmPosSet ? " @ custom pos" : "");

    // Hook UWorld::GetNetMode -> DedicatedServer BEFORE open so world init sees a
    // real server (the physics-step arming happens during init).
    MH_Initialize();

    // [EXITTRACE] install the deliberate-exit tracer first, so it catches an exit from ANY later hook.
    {
        HMODULE k32 = GetModuleHandleA("kernel32.dll");
        void* tp = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "TerminateProcess")) : nullptr;
        void* xp = k32 ? reinterpret_cast<void*>(GetProcAddress(k32, "ExitProcess")) : nullptr;
        int ct = tp ? (int)MH_CreateHook(tp, &TerminateProcess_Hook, reinterpret_cast<void**>(&TerminateProcess_Orig)) : -1;
        int et = tp ? (int)MH_EnableHook(tp) : -1;
        int cx = xp ? (int)MH_CreateHook(xp, &ExitProcess_Hook, reinterpret_cast<void**>(&ExitProcess_Orig)) : -1;
        int ex = xp ? (int)MH_EnableHook(xp) : -1;
        printf("[HalcyonA2] EXITTRACE TerminateProcess create=%d enable=%d  ExitProcess create=%d enable=%d\n", ct, et, cx, ex);
        HxLog("[HalcyonA2] EXITTRACE armed (TerminateProcess %d/%d, ExitProcess %d/%d)\n", ct, et, cx, ex);
    }

    // [2026-09-02 ★ ROOT FIX] Remote VR player join spawns a pawn from a half-loaded BP archetype whose
    // property SOURCE array is corrupt { Data=0, Num=huge } -> AV-at-0 + multi-minute grind in the copy
    // driver sub_7FF673350CB0. Sanitize the corrupt Num at the driver that reads it (see ArrCopyDrv_Hook).
    // MUST be after MH_Initialize() (was failing NOT_INITIALIZED when placed earlier). Supersedes the old
    // PAWN-GUARD VEH (which only skipped the leaf fault and could not stop the outer Num loop).
    {
        void* drvAddr = reinterpret_cast<void*>(base + 0x12F0CB0);   // sub_7FF673350CB0 FArrayProperty copy driver
        MH_STATUS sArr  = MH_CreateHook(drvAddr, &ArrCopyDrv_Hook, reinterpret_cast<void**>(&ArrCopyDrv_Orig));
        MH_STATUS sArrE = MH_EnableHook(drvAddr);
        printf("[HalcyonA2] ARRFIX hook @ %p create=%d enable=%d [BUILD v7 GC-ON] (corrupt-archetype array-copy Num sanitizer)\n",
               drvAddr, (int)sArr, (int)sArrE);
    }

    // [2026-09-02 ★ COSMETIC JOIN-CRASH FIX] Guard sub_7FF67743A880 so a half-loaded cosmetic archetype's
    // garbage FName never reaches FName::ToString (AV reading 0x82 on VR-pawn join). See CosmeticCmp_Hook.
    {
        void* cosAddr = reinterpret_cast<void*>(base + CosmeticCmp_RVA);   // sub_7FF67743A880
        MH_STATUS sCos  = MH_CreateHook(cosAddr, &CosmeticCmp_Hook, reinterpret_cast<void**>(&CosmeticCmp_Orig));
        MH_STATUS sCosE = MH_EnableHook(cosAddr);
        printf("[HalcyonA2] COSFIX hook @ %p create=%d enable=%d (cosmetic-mesh corrupt-FName join-crash guard)\n",
               cosAddr, (int)sCos, (int)sCosE);
    }

    // [2026-09-03 ★ SCRAPRUN GATE] Force getBoolConfigVariable("bScraprunOpen")=true so the deathrun2/
    // ScraprunPrime gamemode.luau drops the ScraprunBlocker and enables the start button. See GetBoolCfg_Hook.
    {
        void* scrAddr = reinterpret_cast<void*>(base + GetBoolCfg_RVA);   // sub_7FF67673A020
        MH_STATUS sScr  = MH_CreateHook(scrAddr, &GetBoolCfg_Hook, reinterpret_cast<void**>(&GetBoolCfg_Orig));
        MH_STATUS sScrE = MH_EnableHook(scrAddr);
        printf("[HalcyonA2] SCRAPRUN gate hook @ %p create=%d enable=%d (force bScraprunOpen=true)\n",
               scrAddr, (int)sScr, (int)sScrE);
    }

    // [2026-09-03 DEATHRUN STATE DIAG] Log every match-state transition (see UpdGameState_Hook).
    {
        void* gsAddr = reinterpret_cast<void*>(base + UpdGameState_RVA);   // UGameStateManagerComponent::UpdateGameState_Implementation
        MH_STATUS sGs  = MH_CreateHook(gsAddr, &UpdGameState_Hook, reinterpret_cast<void**>(&UpdGameState_Orig));
        MH_STATUS sGsE = MH_EnableHook(gsAddr);
        printf("[HalcyonA2] GSMSTATE diag hook @ %p create=%d enable=%d (log match-state transitions)\n",
               gsAddr, (int)sGs, (int)sGsE);

        // Capture the deathrun GameTimeComponent so the ticker can drive its TickComponent (see
        // StartTimerCd_Hook / TickGameTimers) — the real fix that broadcasts onCountdownEnd headless.
        void* stcAddr = reinterpret_cast<void*>(base + StartTimerCd_RVA);
        MH_STATUS sStc  = MH_CreateHook(stcAddr, &StartTimerCd_Hook, reinterpret_cast<void**>(&StartTimerCd_Orig));
        MH_STATUS sStcE = MH_EnableHook(stcAddr);
        printf("[HalcyonA2] GTC startTimerWithCountdown hook @ %p create=%d enable=%d (drive GameTimeComponent tick)\n",
               stcAddr, (int)sStc, (int)sStcE);
    }

    // [REPLAYGUARD] install before the map travel below: the crash is in the GameState tick task.
    if (wcsstr(GetCommandLineW(), L"-AllowReplaySnapshot")) g_skipReplaySnapshot = false;
    if (wcsstr(GetCommandLineW(), L"-NoReplayGuard"))       g_guardReplay = false;
    if (g_guardReplay)
    {
        void* rsAddr = reinterpret_cast<void*>(base + ReplaySnapshot_RVA);
        MH_STATUS cRs = MH_CreateHook(rsAddr, &ReplaySnapshot_Hook, reinterpret_cast<void**>(&ReplaySnapshot_Orig));
        MH_STATUS eRs = MH_EnableHook(rsAddr);
        HxLog("[HalcyonA2] REPLAYGUARD snapshot hook @ %p create=%d enable=%d (skip=%d)\n",
              rsAddr, (int)cRs, (int)eRs, (int)g_skipReplaySnapshot);
    }
    if (g_guardReplay)
    {
        void* rpAddr = reinterpret_cast<void*>(base + ReplayPostStep_RVA);
        MH_STATUS cRp = MH_CreateHook(rpAddr, &ReplayPostStep_Hook, reinterpret_cast<void**>(&ReplayPostStep_Orig));
        MH_STATUS eRp = MH_EnableHook(rpAddr);
        HxLog("[HalcyonA2] REPLAYGUARD hook @ %p create=%d enable=%d (skip dead UA2ReplaySystem post-step)\n",
              rpAddr, (int)cRp, (int)eRp);
    }

    void* gnmAddr = reinterpret_cast<void*>(base + WorldGetNetMode_RVA);
    MH_CreateHook(gnmAddr, &WorldGetNetMode_Hook, nullptr);
    MH_EnableHook(gnmAddr);
    printf("[HalcyonA2] UWorld::GetNetMode hook @ 0x%llX (before open)\n", (unsigned long long)gnmAddr);

    // Netvar-register hook: rewrites the replicated "DefaultLODSettings" netvar to golf's no-hide
    // profile at creation, so deathrun course objects stop proximity-hiding on Android (§ LOD fix).
    // [PORT 22284] NetVarReg hook (DefaultLODSettings no-hide, 0x46A0DF0) DISABLED — RVA not re-found.
    // if (g_fixLod) { ... }

    // 2. Travel to the real map — now it initializes as a server.
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(
        world,
        SDK::FString(L"open /Game/A2/Maps/Station_Prime/Station_Prime_P.umap"),
        nullptr);
    printf("[HalcyonA2] issued: open Station_Prime_P\n");

    // NOTE: we do NOT remove the server's local player. Tried it (LocalPlayers.Remove(0)) as a fix for
    // the spawn-centered movement lag on the theory it was the streaming/relevancy origin — it made NO
    // difference to the lag and it crashed LoadMap: with no local player, post-load init (e.g. loading
    // BP_VolleyJakeball's CosmeticLoadout) walks the local player and derefs null+0x10 (sub_3EF1F60).
    // Not worth it. See a2-match-state-and-ball-physics.

    // Kill texture streaming — dead weight on our -nullrhi server, and its manager periodically
    // virtual-calls a NULL render resource (obj@this+0x890) => the long-standing random null-deref
    // crash (sub_54ABCC0, in the streaming-manager update fed by ClientAddTextureStreamingLoc).
    // No renderer == nothing to stream. (Belt-and-suspenders: sub_54ABCC0 is also null-guarded.)
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"r.TextureStreaming 0"), nullptr);
    printf("[HalcyonA2] issued: r.TextureStreaming 0\n");

    // Widen the GC purge window (CLAUDE.md §3). UE's default incremental pending-kill purge runs
    // often and stalls the game thread — on a weak/contended VPS core those stalls show up as the
    // [STEP] dt spikes during play (0.05-0.4s), which burst-step the rollback sim = residual MI +
    // ball jitter. Pushing the purge interval way out removes that periodic hitch (and the old
    // GC-after-purge crash window). Also raise the GC object headroom so it doesn't purge under load.
    // [PORT 22284] ROOT of the worker-thread GC crash: the old 9999 purge delay = effectively NEVER
    // purge pending-kill objects. On the churn-heavy headless server (district streaming loads/unloads
    // thousands of actors) dead objects PILE UP and GC keeps ref-scanning them via their clusters ->
    // millions of dead/null refs (the GC-GUARD counted 5M+ and climbing) and the long-standing
    // null-deref. Purge regularly (60s) so dead objects are actually reclaimed and never accumulate.
    // Dropped gc.MaxObjectsNotConsideredByGC 1000000 (runtime permanent-pool override, doesn't help).
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"gc.TimeBetweenPurgingPendingKillObjects 60"), nullptr);
    printf("[HalcyonA2] issued: gc.TimeBetweenPurgingPendingKillObjects 60 (purge dead objects regularly)\n");

    // [PORT 22284] CRASH GUARD (IDA-confirmed, NOT log-guessed): a Background Worker thread AVs in the
    // parallel GC cluster-reference pass. Callstack: task DoWork sub_124c2570 (iterates GUObjectArray,
    // fields qword_97C7C00/dword_97C7BF8 adjacent to GObjects@0x97C7EA0) -> ref-collector visitor
    // sub_1245b70 batches 32 refs -> flush sub_124ece0 range-sorts them (checks which point into a
    // cluster's [base,size) memory) and derefs each unguarded -> one batched reference is null ->
    // `mov rcx,[rax]` with rax=0. This is object clustering + parallel GC racing the ballsim's rapid
    // sim churn (Building Simulation Index -2/4/5/6 built+torn down in <10ms). Disable both so
    // reference collection is single-threaded and the cluster-verify path never runs.
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world, SDK::FString(L"gc.CreateGCClusters 0"), nullptr);
    SDK::UKismetSystemLibrary::ExecuteConsoleCommand(world,
        SDK::FString(g_parallelGC ? L"gc.AllowParallelGC 1" : L"gc.AllowParallelGC 0"), nullptr);
    printf("[HalcyonA2] issued: gc.CreateGCClusters 0 + gc.AllowParallelGC 0 (parallel-GC cluster crash guard)\n");

    // [2026-09-01] The console commands above may not stick on 22284 (that's the difference vs 20996).
    // Write the cvar BACKING INTS directly — found in IDA via the RegisterConsoleVariableRef sites:
    //   gc.AllowParallelGC  -> dword @ RVA 0x94C76B8 (sub_7FF672D3CFA0)
    //   gc.CreateGCClusters -> dword @ RVA 0x94C8444 (sub_7FF672D49240)
    // Setting these 0 = parallel GC + object clustering truly off (the flood is the parallel cluster
    // reference pass). .data is writable so a plain store is fine; re-asserted here after travel.
    {
        const uintptr_t b = GetBase();
        const int wasP = *reinterpret_cast<int*>(b + 0x94C76B8);
        const int wasC = *reinterpret_cast<int*>(b + 0x94C8444);
        *reinterpret_cast<int*>(b + 0x94C76B8) = g_parallelGC ? 1 : 0;   // gc.AllowParallelGC
        *reinterpret_cast<int*>(b + 0x94C8444) = 0;   // gc.CreateGCClusters = 0
        printf("[HalcyonA2] gc cvar backing vars set DIRECTLY: AllowParallelGC %d->0, CreateGCClusters %d->0\n",
               wasP, wasC);
    }

    // 3. Register this server with the backend, then set the station-id FString to
    //    the unique id it hands back (no writer for this global exists in the client).
    //    Detect the ACTUAL bound game port first (UE starts at 7777 and auto-increments if taken),
    //    so multiple instances on this box each register their own port instead of all claiming
    //    7777. Poll ~12s while the net driver binds after the open above; fall back to 7777.
    int gamePort = 0;
    for (int i = 0; i < 120 && gamePort == 0; ++i) { gamePort = GetOurListenPort(); if (gamePort) break; Sleep(100); }
    if (gamePort == 0) { gamePort = 7777; printf("[HalcyonA2] listen port not detected in 12s; defaulting to 7777\n"); }
    // Station name shown in the in-game browser. Instance index comes from the port offset so multiple
    // instances on one box get distinct names: base, base_1, base_2...
    // [2026-09-08] Was hardcoded to "HalcyonA2", which is why a station called that kept reappearing in
    // the browser after being deleted. Set it with -ServerName= (StationName in server.config.psd1).
    const int  inst = gamePort - 7777;
    char nameStr[64];
    {
        char baseName[48] = "Rigel";
        const wchar_t* sn = wcsstr(GetCommandLineW(), L"-ServerName=");
        if (sn)
        {
            sn += wcslen(L"-ServerName=");
            wchar_t w[48] = {}; int i = 0;
            const wchar_t q = (*sn == L'"') ? (++sn, L'"') : L'\0';
            while (*sn && i < 47 && (q ? (*sn != q) : (*sn != L' ' && *sn != L'\t'))) w[i++] = *sn++;
            if (i) { size_t cvt = 0; wcstombs_s(&cvt, baseName, sizeof(baseName), w, _TRUNCATE); }
        }
        if (inst <= 0) sprintf_s(nameStr, "%s", baseName);
        else           sprintf_s(nameStr, "%s_%d", baseName, inst);
    }
    printf("[HalcyonA2] detected game listen port = %d -> server_name = %s\n", gamePort, nameStr);
    char portStr[16]; sprintf_s(portStr, "%d", gamePort);

    // Deployment id chosen by the allocator/socket BEFORE launch and handed to the game as
    // -DashboardDeploymentId (the same arg the game's native config fetch reads). Register under it
    // so our DB row and the game's config GET agree — instead of the old fixed "halcyon" reused
    // everywhere. Absent (manual launch) -> register_server mints a fresh id as before.
    std::string deploymentId;
    {
        const wchar_t* cl = GetCommandLineW();
        const wchar_t* p = wcsstr(cl, L"-DashboardDeploymentId=");
        if (p)
        {
            p += wcslen(L"-DashboardDeploymentId=");
            wchar_t q = 0;
            if (*p == L'"' || *p == L'\'') { q = *p; ++p; }
            wchar_t val[128] = {}; int i = 0;
            while (*p && i < 127 && (q ? (*p != q) : (*p != L' '))) val[i++] = *p++;
            char buf[128] = {}; size_t cvt = 0; wcstombs_s(&cvt, buf, sizeof(buf), val, _TRUNCATE);
            deploymentId = buf;
        }
    }
    printf("[HalcyonA2] deployment id (from -DashboardDeploymentId) = %s\n",
           deploymentId.empty() ? "(none; backend will mint)" : deploymentId.c_str());
    g_deploymentIdW.assign(deploymentId.begin(), deploymentId.end());   // for per-server Vivox channel scoping

    // Register under this box's real address. Priority: explicit -RegisterIp=<ip> launch arg (ops
    // override), else the detected PUBLIC IP, else a last-resort fallback so registration still sends.
    std::string registerIp;
    {
        const wchar_t* cl = GetCommandLineW();
        const wchar_t* p = wcsstr(cl, L"-RegisterIp=");
        if (p)
        {
            p += wcslen(L"-RegisterIp=");
            wchar_t q = 0;
            if (*p == L'"' || *p == L'\'') { q = *p; ++p; }
            wchar_t val[64] = {}; int i = 0;
            while (*p && i < 63 && (q ? (*p != q) : (*p != L' '))) val[i++] = *p++;
            char buf[64] = {}; size_t cvt = 0; wcstombs_s(&cvt, buf, sizeof(buf), val, _TRUNCATE);
            registerIp = buf;
        }
    }
    if (registerIp.empty()) registerIp = GetPublicIp();
    // [2026-09-08] Was a hardcoded fallback to 34.239.141.19 — a stale AWS address that is NOT this
    // project's. Publishing it made every client try to connect to a stranger's box. Fail loudly instead:
    // registering with a known-wrong IP is worse than not registering, and -RegisterIp= is the real fix.
    if (registerIp.empty())
    {
        printf("[HalcyonA2] public IP detect FAILED and no -RegisterIp= given -> NOT registering.\n"
               "[HalcyonA2]   Set RegisterIp in tools\\server.config.psd1 to this box's PUBLIC ip.\n");
        HxLog("[HalcyonA2][REGISTER] aborted: no public IP (set -RegisterIp=)\n");
        return;
    }
    else printf("[HalcyonA2] register IP = %s\n", registerIp.c_str());

    std::string reqBody =
        std::string("{\"ip\":\"") + registerIp + "\",\"port\":\"" + portStr +
        "\",\"server_name\":\"" + nameStr + "\",\"max_players\":10";
    reqBody += std::string(",\"version\":\"") + kBuildVersion + "\"";
    if (!deploymentId.empty()) reqBody += ",\"deployment_id\":\"" + deploymentId + "\"";
    reqBody += "}";
    const std::string resp = HttpPostLocal(kBackendHost, kBackendPort, L"/register_server", reqBody);
    printf("[HalcyonA2] register_server -> %s\n", resp.empty() ? "(no response)" : resp.c_str());

    // Remember the effective deployment id (ours, or the one the backend minted) so the
    // player-count heartbeat can key its updates to this server's EOS session.
    g_deploymentId = !deploymentId.empty() ? deploymentId : ExtractJsonString(resp, "deployment_id");

    const std::string sid = ExtractJsonString(resp, "station_id");
    if (sid.empty())
    {
        printf("[HalcyonA2] registration failed; using fallback station id\n");
        g_stationId = kStationFallback;
    }
    else
    {
        g_stationId.assign(sid.begin(), sid.end());
    }

    // CRASH FIX (2026-08-20): do NOT write GA2StationId (0x95DF980 Data / C8 Num / CC Max) at all.
    // It is a game FString that BOTH A2Station__FindRoleForStation / FetchUserRoles read AND the
    // native station-dashboard path writes: `GA2StationId = <parsed station_id>` is an FString
    // operator= -> realloc of the existing Data. We used to point Data at g_stationId's std::wstring
    // (CRT-heap) buffer, so that native realloc hit FMallocBinned2 "realloc an unrecognized block,
    // canary 0x44" and killed the process. Now that the net-mode gate is patched AND the backend
    // returns a real station_id, the native path populates GA2StationId itself with a game-heap
    // buffer it fully owns (alloc/realloc/free) — so we leave it untouched. Roles read GA2StationId,
    // so confirm they still resolve after deploy; if not, the native populate isn't firing.
    printf("[HalcyonA2] station id (registered) = %ls -> NOT touching GA2StationId; native dashboard populates it\n",
           g_stationId.c_str());

    // [VOIPFIX] Resolve and LOG the Vivox channel now, at boot, instead of only on the first client
    // join. The URI is what decides positional-vs-group voice: the client parses "confctl-d" (positional)
    // vs "confctl-g" (group, 2D) and the "!p-<audible>-<conversational>-<fade>-<model>" spec straight out
    // of the string it is handed, so printing it here is a direct, checkable answer to "is voice 3D?"
    // without needing a client to be online.
    EnsureVoipChannel();
    {
        const bool positional = g_voipChannel.Data && wcsstr(g_voipChannel.Data, L"confctl-d") != nullptr;
        HxLog("[HalcyonA2][VOIPFIX] resolved channel = %ls  -> %s\n",
              g_voipChannel.Data ? g_voipChannel.Data : L"(null)",
              positional ? "POSITIONAL (3D)" : "GROUP (2D!)");
    }

    // 4. Hook ProcessEvent (game thread) for one-shot world setup + the ball sim
    //    pumps + PrintString/PrintDebugString capture.
    void* peAddr = reinterpret_cast<void*>(base + SDK::Offsets::ProcessEvent);
    MH_CreateHook(peAddr, &ProcessEvent_Hook, reinterpret_cast<void**>(&ProcessEvent_Orig));

    // [PORT 22284] IsRunningSimulate hook DISABLED for boot spine: 0x531EF96 is a MID-exec address
    // (logic inlined in execIsRunningSimulate 0x531EF40), so a 5-byte detour there is risky. Only
    // needed for ball physics (post-boot). Re-enable/verify the hook point when wiring ballsim.
    // void* isRunAddr = reinterpret_cast<void*>(base + IsRunningSimulate_RVA);
    // MH_STATUS s1 = MH_CreateHook(isRunAddr, &IsRunningSimulate_Hook, nullptr);

    // Force A2's net mode to DedicatedServer so server-gated physics/logic runs.
    void* a2NetAddr = reinterpret_cast<void*>(base + A2GetNetMode_RVA);
    MH_STATUS s2 = MH_CreateHook(a2NetAddr, &A2GetNetMode_Hook, nullptr);
    printf("[HalcyonA2] A2 GetNetMode hook @ 0x%llX create=%s\n",
           (unsigned long long)a2NetAddr, MH_StatusToString(s2));

    // Echo every UKismetSystemLibrary::PrintString to our console.
    void* psAddr = reinterpret_cast<void*>(base + PrintString_RVA);
    MH_STATUS s3 = MH_CreateHook(psAddr, &PrintString_Hook, reinterpret_cast<void**>(&PrintString_Orig));
    printf("[HalcyonA2] PrintString hook @ 0x%llX create=%s\n",
           (unsigned long long)psAddr, MH_StatusToString(s3));

    // [PORT 22284] Boot spine validated -> subsystems re-enabled below. The hooks whose 22284 RVA
    // is still unfound stay DISABLED (EvtDispatch, FNameResolve/TextLayoutLeaf/StreamThunk crash
    // guards, ObjBuild, QReg/QSetP) plus the stale 0x547380A NOP. EnableHook(ALL) + the station
    // self-init thread run at the end of Main.

    // Vivox login-token "f" claim fix (RequestVivoxLoginToken impl + JWT assembler).
    void* reqAddr = reinterpret_cast<void*>(base + ReqLogin_RVA);
    MH_STATUS s4 = MH_CreateHook(reqAddr, &ReqLogin_Hook, reinterpret_cast<void**>(&ReqLogin_Orig));
    void* jwtAddr = reinterpret_cast<void*>(base + JwtBuild_RVA);
    MH_STATUS s5 = MH_CreateHook(jwtAddr, &JwtBuild_Hook, reinterpret_cast<void**>(&JwtBuild_Orig));
    printf("[HalcyonA2] VOIP login-token hooks: ReqLogin=%s JwtBuild=%s\n",
           MH_StatusToString(s4), MH_StatusToString(s5));

    // Vivox channel-join fix (join-token builder + ReceiveChannelJoinTokens sender).
    void* joinAddr = reinterpret_cast<void*>(base + JoinBuild_RVA);
    MH_STATUS s6 = MH_CreateHook(joinAddr, &JoinBuild_Hook, reinterpret_cast<void**>(&JoinBuild_Orig));
    void* sendAddr = reinterpret_cast<void*>(base + SendJoin_RVA);
    MH_STATUS s7 = MH_CreateHook(sendAddr, &SendJoin_Hook, reinterpret_cast<void**>(&SendJoin_Orig));
    printf("[HalcyonA2] VOIP channel-join hooks: JoinBuild=%s SendJoin=%s\n",
           MH_StatusToString(s6), MH_StatusToString(s7));

    // [22284 DIAGNOSTIC] Ballsim AUTHORITATIVE forcing — gated to test the GC null-ref crash source.
    if (g_forceAuthSim)
    {
        // BallSimManager GetWorld fix — makes the sim go authoritative (IsServer:1).
        void* bgwAddr = reinterpret_cast<void*>(base + BallSimGetWorld_RVA);
        MH_STATUS s8 = MH_CreateHook(bgwAddr, &BallSimGetWorld_Hook, reinterpret_cast<void**>(&BallSimGetWorld_Orig));
        printf("[HalcyonA2] BallSim GetWorld hook @ 0x%llX create=%s\n",
               (unsigned long long)bgwAddr, MH_StatusToString(s8));

        // Make the ball sim treat our dedicated server (net mode 1) as the authority.
        PatchBallSimNetModeChecks();
    }
    else
    {
        printf("[HalcyonA2] [DIAG] ballsim authoritative forcing DISABLED (g_forceAuthSim=false)\n");
    }

    // Force-accept client ball-state streams (bypass the owningActor gate) so hits land.
    void* spAddr = reinterpret_cast<void*>(base + SendPhysImpl_RVA);
    MH_STATUS s9 = MH_CreateHook(spAddr, &SendPhys_Hook, reinterpret_cast<void**>(&SendPhys_Orig));
    printf("[HalcyonA2] SendPhysicsPropData hook @ 0x%llX create=%s\n",
           (unsigned long long)spAddr, MH_StatusToString(s9));

    // Log whether client inputs are ADDED to the sim or SKIPPED (frame gate / player miss).
    void* inAddr = reinterpret_cast<void*>(base + IngestInput_RVA);
    MH_STATUS s10 = MH_CreateHook(inAddr, &IngestInput_Hook, reinterpret_cast<void**>(&IngestInput_Orig));
    printf("[HalcyonA2] input-ingest hook @ 0x%llX create=%s\n",
           (unsigned long long)inAddr, MH_StatusToString(s10));

    // Read the server's computed missedInputs/missedCaught (the client HUD MI/MIB) each send.
    void* srAddr = reinterpret_cast<void*>(base + SendResults_RVA);
    MH_STATUS sSR = MH_CreateHook(srAddr, &SendResults_Hook, reinterpret_cast<void**>(&SendResults_Orig));
    printf("[HalcyonA2] SendServerSimResults hook @ 0x%llX create=%s\n",
           (unsigned long long)srAddr, MH_StatusToString(sSR));

    // DIAGNOSTIC: measure who advances the sim frame (step vs ingest).
    void* stAddr = reinterpret_cast<void*>(base + StepSim_RVA);
    MH_STATUS sST = MH_CreateHook(stAddr, &StepSim_Hook, reinterpret_cast<void**>(&StepSim_Orig));
    printf("[HalcyonA2] StepSim probe hook @ 0x%llX create=%s\n",
           (unsigned long long)stAddr, MH_StatusToString(sST));

    // [2026-09-02 ★ TICK FLOOD FIX] skip stepping the phantom -2 sim (see BallAdvance_Hook) so the tick can
    // run (balls simulate for real matches) without the phantom's client-prediction sim flooding GC.
    void* advAddr = reinterpret_cast<void*>(base + BallAdvance_RVA);
    MH_STATUS sADV = MH_CreateHook(advAddr, &BallAdvance_Hook, reinterpret_cast<void**>(&BallAdvance_Orig));
    MH_STATUS sADVe = MH_EnableHook(advAddr);
    printf("[HalcyonA2] ball-advance phantom-skip hook @ 0x%llX create=%s enable=%s (tick ON, -2 skipped)\n",
           (unsigned long long)advAddr, MH_StatusToString(sADV), MH_StatusToString(sADVe));

    // [2026-09-04 ★ SEAT DIAG] hook the reconcile/seater (sub_7FF6774BBD80) to prove whether it even runs on
    // the headless server. If [RECON] never prints, the seater isn't being ticked here = root of "not added
    // to sim". See Reconcile_Hook.
    void* rcAddr = reinterpret_cast<void*>(base + Reconcile_RVA);
    MH_STATUS sRC  = MH_CreateHook(rcAddr, &Reconcile_Hook, reinterpret_cast<void**>(&Reconcile_Orig));
    MH_STATUS sRCe = MH_EnableHook(rcAddr);
    printf("[HalcyonA2] ball-reconcile(seater) hook @ 0x%llX create=%s enable=%s\n",
           (unsigned long long)rcAddr, MH_StatusToString(sRC), MH_StatusToString(sRCe));

    // [2026-09-03 ★ RESULTS-BUILD TAIL FIX] skip the Phase-2 reconcile/correction build for the phantom -2
    // sim too (see BallReconcile_Hook) — the piece the advance-skip alone was missing. With both installed
    // the empty -2 sim is excluded from every tick path, so the manager tick can run (g_ballSimNoTick=false)
    // for real matches without the phantom's uninitialised state flooding GC.
    void* recAddr = reinterpret_cast<void*>(base + BallReconcile_RVA);
    MH_STATUS sREC = MH_CreateHook(recAddr, &BallReconcile_Hook, reinterpret_cast<void**>(&BallReconcile_Orig));
    MH_STATUS sRECe = MH_EnableHook(recAddr);
    printf("[HalcyonA2] ball-reconcile phantom-skip hook @ 0x%llX create=%s enable=%s (tick ON, -2 reconcile skipped)\n",
           (unsigned long long)recAddr, MH_StatusToString(sREC), MH_StatusToString(sRECe));

    // [2026-09-03] TICK-HANG WATCHDOG: separate thread that dumps the exact ball-sim sub-op the game thread
    // died in when the manager tick freezes during streaming. Diagnostic only. See TickWatchdog().
    if (g_tickWatchdog)
    {
        std::thread(TickWatchdog).detach();
        printf("[HalcyonA2] tick-hang watchdog thread started (dumps stuck sub-op if StepSim stalls >3s)\n");
    }

    // [2026-09-01] -2 flood fix: remap client->ListenServer perspective during the ball tick so the
    // manager's client-rebuild loop (the -2 local prediction sim that floods the GC) is skipped. Must be
    // created AFTER StepSim_Hook so g_inBallStep is set around the original tick. See StepSim_Hook note.
    // [2026-09-01] DO NOT INSTALL unless the flag is on — BallNetPersp_RVA 0x60A0990 is a BAD RVA
    // (MID-function of unrelated sub_7FF678100950); MH_CreateHook here writes a trampoline into the
    // middle of that function = memory corruption. Gated off.
    if (g_killLocalSimRebuild)
    {
        void* bnpAddr = reinterpret_cast<void*>(base + BallNetPersp_RVA);
        MH_STATUS sBNP = MH_CreateHook(bnpAddr, &BallNetPersp_Hook, reinterpret_cast<void**>(&BallNetPersp_Orig));
        printf("[HalcyonA2] BallNetPersp (-2 rebuild kill) hook @ 0x%llX create=%s\n",
               (unsigned long long)bnpAddr, MH_StatusToString(sBNP));
    }

    // [22284] Ballsim garbage fix: filter phantom-only sim builds (see BuildSim_Hook).
    void* bsAddr = reinterpret_cast<void*>(base + 0x545CCB0);
    MH_STATUS sBS = MH_CreateHook(bsAddr, &BuildSim_Hook, reinterpret_cast<void**>(&g_buildSimOrig));
    printf("[HalcyonA2] BuildSim phantom-filter hook @ 0x%llX create=%s\n",
           (unsigned long long)bsAddr, MH_StatusToString(sBS));

    // ★ [2026-09-01] THE GC-FLOOD FIX: clamp the corrupt GC ref-batch count on entry (see GcBatchCompact_Hook).
    void* gcbAddr = reinterpret_cast<void*>(base + 0x124F4C0);
    MH_STATUS sGCB = MH_CreateHook(gcbAddr, &GcBatchCompact_Hook, reinterpret_cast<void**>(&GcBatchCompact_Orig));
    printf("[HalcyonA2] GC ref-batch clamp hook @ 0x%llX create=%s\n",
           (unsigned long long)gcbAddr, MH_StatusToString(sGCB));

    // [2026-09-01] Remove the server's local player from the sim (see GetLocalInst_Hook): make
    // AVRPawn::GetLocalInstanceWithWorld return null post-boot so the client-perspective sim/physics
    // path (the -2 rebuild + client-only object refs = the GC flood) never runs. THE ballsim fix.
    void* gliAddr = reinterpret_cast<void*>(base + GetLocalInst_RVA);
    MH_STATUS sGLI = MH_CreateHook(gliAddr, &GetLocalInst_Hook, reinterpret_cast<void**>(&GetLocalInst_Orig));
    printf("[HalcyonA2] GetLocalInstanceWithWorld null-in-sim hook @ 0x%llX create=%s\n",
           (unsigned long long)gliAddr, MH_StatusToString(sGLI));

    // Inject the hit velocity into the sim's ball state each integrate.
    void* siAddr = reinterpret_cast<void*>(base + SimIntegrate_RVA);
    MH_STATUS s11 = MH_CreateHook(siAddr, &SimIntegrate_Hook, reinterpret_cast<void**>(&SimIntegrate_Orig));
    printf("[HalcyonA2] sim-integrate hook @ 0x%llX create=%s\n",
           (unsigned long long)siAddr, MH_StatusToString(s11));

    // Trace ATicketManager::GiveAndCheckTicket (arena admission) to find the verify-bail
    // that keeps players out of VerifiedTicketHolders (no team, no sim seat).
    void* gacAddr = reinterpret_cast<void*>(base + GiveCheck_RVA);
    MH_STATUS s12 = MH_CreateHook(gacAddr, &GiveCheck_Hook, reinterpret_cast<void**>(&GiveCheck_Orig));
    printf("[HalcyonA2] GiveAndCheckTicket hook @ 0x%llX create=%s\n",
           (unsigned long long)gacAddr, MH_StatusToString(s12));

    // Leave-arena reset: drop the player from the color stamp + clear color/team (else our fast-path
    // stamp keeps re-painting the arena color forever after they leave).
    void* nlaAddr = reinterpret_cast<void*>(base + NotifyLeftArena_RVA);
    MH_STATUS s13 = MH_CreateHook(nlaAddr, &NotifyLeftArena_Hook, reinterpret_cast<void**>(&NotifyLeftArena_Orig));
    printf("[HalcyonA2] Server_NotifyPlayerLeftArena hook @ 0x%llX create=%s\n",
           (unsigned long long)nlaAddr, MH_StatusToString(s13));

    // [2026-09-02] REMOTE-JOIN CRASH FIX (see SlateHitTest_Hook / TakeLevelEditor_Hook above).
    if (g_guardSlateHitTest)
    {
        void* shtAddr = reinterpret_cast<void*>(base + 0x14363A0);   // sub_7FF6734963A0 (Slate widget hit-test / crash site)
        MH_STATUS sSHT = MH_CreateHook(shtAddr, &SlateHitTest_Hook, reinterpret_cast<void**>(&g_slateHitTestOrig));
        printf("[HalcyonA2] Slate hit-test null-guard hook @ 0x%llX create=%s\n",
               (unsigned long long)shtAddr, MH_StatusToString(sSHT));
    }
    if (g_blockLevelEditor)
    {
        void* tleAddr = reinterpret_cast<void*>(base + 0x5502070);   // AVRPawn::Server_AttemptTakeOwnershipOfLevelEditor_Implementation
        MH_STATUS sTLE = MH_CreateHook(tleAddr, &TakeLevelEditor_Hook, reinterpret_cast<void**>(&g_takeLevelEditorOrig));
        printf("[HalcyonA2] Server_AttemptTakeOwnershipOfLevelEditor block hook @ 0x%llX create=%s\n",
               (unsigned long long)tleAddr, MH_StatusToString(sTLE));
    }

    // [PORT 22284] EvtDispatch hook (0x465F820) DISABLED — RVA unfound (golf-payload diagnostic only).

    // [PORT-AUDIT] FNameResolve / TextLayoutLeaf crash guards re-enabled with the verified 22284 RVAs.
    {
        MH_STATUS sFN = MH_CreateHook(reinterpret_cast<void*>(base + FNameResolve_RVA), &FNameResolve_Hook, reinterpret_cast<void**>(&FNameResolve_Orig));
        MH_STATUS sTL = MH_CreateHook(reinterpret_cast<void*>(base + TextLayoutLeaf_RVA), &TextLayoutLeaf_Hook, reinterpret_cast<void**>(&TextLayoutLeaf_Orig));
        printf("[HalcyonA2] crash guards FNameResolve=%s TextLayoutLeaf=%s\n", MH_StatusToString(sFN), MH_StatusToString(sTL));
        MH_STATUS sNV1 = MH_CreateHook(reinterpret_cast<void*>(base + 0x46BC830), &NetVarReg_Hook,  reinterpret_cast<void**>(&g_NetVarReg_Orig));
        MH_STATUS sNV2 = MH_CreateHook(reinterpret_cast<void*>(base + 0x47138D0), &NetVarReg_Hook2, reinterpret_cast<void**>(&g_NetVarReg_Orig2));
        printf("[HalcyonA2] NetVarReg (LOD no-hide) hooks %s / %s\n", MH_StatusToString(sNV1), MH_StatusToString(sNV2));
    }

    // Golf-cup server-side detection probe (does the sink fire on the server?).
    void* gbicAddr = reinterpret_cast<void*>(base + GolfBallInCup_RVA);
    MH_STATUS s16 = MH_CreateHook(gbicAddr, &GolfBallInCup_Hook, reinterpret_cast<void**>(&GolfBallInCup_Orig));
    void* govAddr = reinterpret_cast<void*>(base + GolfOverlap_RVA);
    MH_STATUS s17 = MH_CreateHook(govAddr, &GolfOverlap_Hook, reinterpret_cast<void**>(&GolfOverlap_Orig));
    printf("[HalcyonA2] GolfCup hooks BallInCup=%s Overlap=%s\n", MH_StatusToString(s16), MH_StatusToString(s17));

    // [PORT 22284] ObjBuild hook (0x46D77A0) DISABLED — not needed on this build (no gamemode-slot
    // spawn); 22284 RVA unfound anyway (strings 'prefabType'/'serverOnly' lead if ever needed).

    // QUEST TRACE hooks — see if the game drives quest init/progress itself.
    MH_STATUS qa = MH_CreateHook(reinterpret_cast<void*>(base + 0x4690260), &QInit_Hook, reinterpret_cast<void**>(&QInit_Orig));
    MH_STATUS qb = MH_CreateHook(reinterpret_cast<void*>(base + 0x46902F0), &QSetQ_Hook, reinterpret_cast<void**>(&QSetQ_Orig));
    // [PORT 22284] QReg(0x4680E50)/QSetP(0x4685C30) DISABLED — RVAs unfound (trace-only).
    printf("[HalcyonA2] QTRACE hooks: Init=%s SetQuests=%s\n", MH_StatusToString(qa), MH_StatusToString(qb));

    // [PORT 22284] StreamThunk null-guard (0x54ABCC0) DISABLED — RVA unfound (tiny indirect-jmp thunk);
    // runtime crash-driven. Note: r.TextureStreaming 0 already kills the streaming path on -nullrhi.

    // Quests: capture each joiner's org-scoped id from the roles fetch to warm their Mothership
    // quest fetch (A2Station__FetchUserRoles, a1 = org id FString).
    void* frAddr = reinterpret_cast<void*>(base + FetchRoles_RVA);
    MH_STATUS sFetchRoles = MH_CreateHook(frAddr, &FetchRoles_Hook, reinterpret_cast<void**>(&FetchRoles_Orig));
    printf("[HalcyonA2] FetchUserRoles hook @ 0x%llX create=%s\n",
           (unsigned long long)frAddr, MH_StatusToString(sFetchRoles));

    // Null-guard GetPlayerPawn so client ticks that resolve the (removed) local player skip instead
    // of dereferencing a null controller — the first fault after LocalPlayers.Remove(0).
    void* gppAddr = reinterpret_cast<void*>(base + GetPlayerPawn_RVA);
    MH_STATUS sGpp = MH_CreateHook(gppAddr, &GetPlayerPawn_Hook, reinterpret_cast<void**>(&GetPlayerPawn_Orig));
    printf("[HalcyonA2] GetPlayerPawn null-guard hook @ 0x%llX create=%s\n",
           (unsigned long long)gppAddr, MH_StatusToString(sGpp));

    // Silence AVRPawn::GetLocalPlayer_Implementation's per-pawn "GetLocalPlayer -- Controller was
    // invalid" LogTemp::Error spam. With the local player removed, every pawn whose controller resolves
    // null hits that branch each frame; the RETURN (0) is correct — only the FMsg::Logf floods the log
    // (real game-thread format+I/O cost). NOP the 5-byte `call FMsg__Logf` @0x547380A; behavior is
    // otherwise identical (still returns 0), the log just doesn't fire. Nothing else in the func changes.
    // [PORT 22284] 0x547380A 5-byte NOP (GetLocalPlayer log-spam silencer) DISABLED — that 20996
    // address is a DIFFERENT function on 22284; writing 5 NOPs there would corrupt it. Cosmetic only.

    MH_STATUS es = MH_EnableHook(MH_ALL_HOOKS);
    printf("[HalcyonA2] EnableHook(ALL)=%s | ProcessEvent @ 0x%llX\n",
           MH_StatusToString(es), (unsigned long long)peAddr);

    // [PORT 22284] Native station self-init (sub_54B13D0): registers the reporting delegates +
    // server_launched. Kept alongside the ProcessEvent ticker (g_deployFetchDone latch dedups).
    std::thread([]() {
        for (int i = 0; i < 600 && !g_deployFetchDone; ++i) { SafeTriggerDeploymentFetch(); Sleep(200); }
    }).detach();
    printf("[HalcyonA2] [PORT 22284] station self-init thread started (sub_54B13D0)\n");
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        // Never do work in DllMain (loader lock). Spin up a worker thread.
        std::thread(Main, hModule).detach();
    }
    return TRUE;
}
