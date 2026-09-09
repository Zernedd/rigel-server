// A2EntitlementPatch - a UE4SS native mod.
//
// Stops A2 from quitting itself when the Meta entitlement check fails.
//
// THE PROBLEM
// A client that joins a private server connects fine ("Welcomed by server", travel
// completes) and then exits a few seconds later:
//
//   LogA2MothershipAuthStateMachine: Error: Could not verify entitlement status:
//     Missing entitlement for 23916854551246326 (1971031).
//     A Shipping build would exit at this point
//   Closing by request
//
// That is not a crash. The Oculus platform runtime cannot prove the logged-in account
// owns app 23916854551246326 for this build, and the shipping build treats a failed
// entitlement as fatal.
//
// THE PATCH
// The entitlement result handler is in A2MothershipAuthStateMachine.cpp at RVA
// 0x53DFC70. Its second argument is the success flag:
//
//   0x053DFCC4  test bpl, bpl          ; 40 84 ED   bSuccess?
//   0x053DFCC7  jne  0x053DFD22        ; 75 59      -> entitled path, auth continues
//   0x053DFCC9  ...                                    not-entitled path: log, then exit
//
// Making that jump unconditional (75 -> EB) means the handler always takes the entitled
// path, so the auth state machine carries on exactly as it would for an entitled user
// and nothing requests an exit.
//
// One byte, written once. No hooks and no trampolines, so the patch itself cannot
// destabilise the process. It is guarded by a 16-byte signature that occurs exactly once
// in the A2-Win64-Shipping.exe this targets (UE 5.4.2, build 20996): on any other build
// the mod refuses and writes nothing.
//
// HOW UE4SS LOADS THIS
// UE4SS loads Mods\<ModName>\dlls\main.dll and then requires the exported lifecycle
// functions `start_mod` and `uninstall_mod`; without them it calls FreeLibrary and marks
// the mod uninstallable. We export both. `start_mod` does the patch and returns nullptr,
// which UE4SS records as "not started" - every callback it would otherwise make is
// guarded by `if (m_mod)`, so returning nullptr means UE4SS never calls into this DLL
// again. That keeps the mod entirely free of the CppUserModBase ABI: nothing here has to
// match the UE4SS build, so a UE4SS update cannot break it.
//
// The mod is harmless on the server, which never runs the Mothership auth state machine -
// it patches the same byte and nothing ever executes it.

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <share.h>

// ---------------------------------------------------------------------------
// Patch site. RVAs are for A2-Win64-Shipping.exe, UE 5.4.2 build 20996
// (166,396,928 bytes). Re-derive them for any other build.
// ---------------------------------------------------------------------------
// The handler moved between builds, so the site is FOUND rather than hardcoded:
//   20996 (Rift)  0x53DFCC4
//   22284 (Nov15) 0x5429424
// The 16 bytes are identical on both except bytes 7..10, which are the rip-relative
// displacement of the `cmp byte ptr [rip+disp32], 2` and therefore differ per build.
// Wildcard those four and the same pattern matches every build that has this check.
//
// test bpl,bpl / jne +0x59 / cmp byte ptr[rip+??],2 / jb .. / cmp dword ptr[rdi+8]..
static const uint8_t kSignature[16] = {
    0x40, 0x84, 0xED, 0x75, 0x59, 0x80, 0x3D, 0x00,
    0x00, 0x00, 0x00, 0x02, 0x72, 0x25, 0x83, 0x7F,
};
// 0 = must match, 1 = wildcard (the rip-relative displacement).
static const uint8_t kSigMask[16] = {
    0, 0, 0, 0, 0, 0, 0, 1,
    1, 1, 1, 0, 0, 0, 0, 0,
};
static constexpr size_t kJneOffsetInSig = 3;   // the `jne` opcode is the 4th byte of the pattern

// Known sites, tried first so the usual case costs nothing; the scan is the fallback.
static const uintptr_t kKnownSigRVAs[] = { 0x53DFCC4 /*20996*/, 0x5429424 /*22284*/ };

static bool SigMatches(const uint8_t* p)
{
    for (size_t i = 0; i < sizeof(kSignature); ++i)
        if (!kSigMask[i] && p[i] != kSignature[i]) return false;
    return true;
}

// Walk the main module's executable sections looking for the one place this pattern occurs.
// Returns the RVA of the pattern start, or 0 when it is not found / not unique.
static uintptr_t ScanForSite(uintptr_t base)
{
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;

    auto* sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t found = 0; int hits = 0;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t start = base + sec->VirtualAddress;
        const size_t    size  = sec->Misc.VirtualSize;
        if (size < sizeof(kSignature)) continue;
        auto* p = reinterpret_cast<const uint8_t*>(start);
        for (size_t off = 0; off + sizeof(kSignature) <= size; ++off)
        {
            if (p[off] != 0x40 || p[off + 1] != 0x84) continue;   // cheap prefilter
            if (SigMatches(p + off))
            {
                if (++hits > 1) return 0;                          // ambiguous: refuse
                found = (start + off) - base;
            }
        }
    }
    return (hits == 1) ? found : 0;
}

static constexpr uint8_t kOpJne = 0x75;   // jump if not equal  (original)
static constexpr uint8_t kOpJmp = 0xEB;   // jump always        (patched)

static FILE* g_log = nullptr;

// UE4SS owns the console, so printf lands in its window; the file copy is what survives.
static void Log(const char* fmt, ...)
{
    if (!g_log)
    {
        wchar_t tmp[MAX_PATH]{}; GetTempPathW(MAX_PATH, tmp);
        wchar_t path[MAX_PATH]{}; swprintf_s(path, L"%sA2EntitlementPatch.log", tmp);
        // _SH_DENYNO, unlike _wfopen_s, leaves the file readable while we hold it open.
        g_log = _wfsopen(path, L"a", _SH_DENYNO);
    }
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    vfprintf(stdout, fmt, ap); va_end(ap);
    if (g_log) { vfprintf(g_log, fmt, ap2); fflush(g_log); }
    va_end(ap2);
}

static void HexDump(const char* label, const uint8_t* p, size_t n)
{
    char line[128]; int used = 0;
    for (size_t i = 0; i < n && used < 100; ++i)
        used += sprintf_s(line + used, sizeof(line) - used, "%02X ", p[i]);
    Log("[A2EntitlementPatch] %s: %s\n", label, line);
}

static void ApplyPatch()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    Log("[A2EntitlementPatch] loaded. game base = 0x%llX\n", (unsigned long long)base);

    // Resolve the site: known RVAs first, then a full scan of the executable sections.
    uintptr_t sigRVA = 0;
    for (uintptr_t cand : kKnownSigRVAs)
    {
        auto* p = reinterpret_cast<uint8_t*>(base + cand);
        MEMORY_BASIC_INFORMATION m{};
        if (VirtualQuery(p, &m, sizeof(m)) == 0 || m.State != MEM_COMMIT) continue;
        if (SigMatches(p)) { sigRVA = cand; break; }
        if (p[kJneOffsetInSig] == kOpJmp && p[0] == 0x40 && p[1] == 0x84 && p[2] == 0xED)
        {
            Log("[A2EntitlementPatch] already patched at RVA 0x%llX (jne -> jmp); nothing to do\n",
                (unsigned long long)cand);
            return;
        }
    }
    if (!sigRVA)
    {
        sigRVA = ScanForSite(base);
        if (sigRVA)
            Log("[A2EntitlementPatch] site found by scan at RVA 0x%llX (unknown build)\n",
                (unsigned long long)sigRVA);
    }
    if (!sigRVA)
    {
        Log("[A2EntitlementPatch] ABORT: entitlement site not found at any known RVA and the\n"
            "[A2EntitlementPatch] pattern scan found no unique match. Nothing was written.\n");
        HexDump("expected (0=wildcards at 7..10)", kSignature, sizeof(kSignature));
        return;
    }

    uint8_t* const sig = reinterpret_cast<uint8_t*>(base + sigRVA);
    uint8_t* const jne = sig + kJneOffsetInSig;

    DWORD old = 0;
    if (!VirtualProtect(jne, 1, PAGE_EXECUTE_READWRITE, &old))
    {
        Log("[A2EntitlementPatch] ABORT: VirtualProtect failed (%lu)\n", GetLastError());
        return;
    }
    *jne = kOpJmp;
    VirtualProtect(jne, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), jne, 1);

    Log("[A2EntitlementPatch] patched RVA 0x%llX: jne -> jmp "
        "(a failed entitlement check no longer exits the game)\n",
        (unsigned long long)(sigRVA + kJneOffsetInSig));
    HexDump("now", sig, sizeof(kSignature));
    (void)kOpJne;
}

// ---------------------------------------------------------------------------
// UE4SS mod lifecycle. Both exports must exist or UE4SS unloads the DLL.
// start_mod returns nullptr on purpose - see the header comment.
// ---------------------------------------------------------------------------
extern "C" __declspec(dllexport) void* start_mod()
{
    ApplyPatch();
    return nullptr;
}

extern "C" __declspec(dllexport) void uninstall_mod(void*)
{
    // Nothing to undo: the patch is a single byte in the game image and the process is
    // going away. UE4SS never calls this anyway, since start_mod returned nullptr.
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
        DisableThreadLibraryCalls(hModule);
    return TRUE;
}
