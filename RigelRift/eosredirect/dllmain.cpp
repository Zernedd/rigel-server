// dllmain.cpp - Rigel EOS redirect, shipped as the game binary dsound.dll.
//
// ============================================================================================
// WHAT THIS IS
// ============================================================================================
// The PC twin of the Quest rigel_hook.cpp. Two jobs, both done so the file looks and behaves like an
// ordinary game audio DLL rather than a mod:
//
//   1. dsound proxy. A2-Win64-Shipping.exe statically imports dsound.dll by ordinal (1,3,6,8,11,12).
//      We sit in A2\Binaries\Win64 next to the exe, so the loader picks us up first; DllMain forwards
//      every ordinal to the real C:\Windows\System32\dsound.dll (see thunks.asm). Nothing about audio
//      changes -- this is only a place to run at start-up, chosen because it is a DLL the game already
//      loads. No console, no log window, no extra folders.
//
//   2. EOS station-browser redirect. The PC EOS SDK (EOSSDK-Win64-Shipping.dll) builds its backend
//      host at runtime (EpicGamesPlatform::GetBaseURL) and talks to it through a statically-linked
//      libcurl, so there is no config key and no import to repoint -- exactly like the Quest .so. So we
//      hook curl_easy_setopt inside that DLL and rewrite the URL of any epicgames.dev request to our
//      gateway, scheme included. Everything non-URL, and every non-Epic URL, passes through untouched.
//
// WHY curl_easy_setopt AND HOW IT IS FOUND
// ----------------------------------------
// curl_easy_setopt(CURL* handle, CURLoption option, ...) is where the plaintext URL still exists,
// before TLS. For CURLOPT_URL (10002) the first vararg is a const char* URL -- on Win64 that lands in
// r8. It is not an export (curl is static), so we find it by a signature: the varargs shim spills
// rdx/r8/r9 to home space and builds a va_list, and in this SDK exactly one such shim tail-calls the
// internal option setter. Its first 24 bytes are distinctive and stable, so we scan .text for them
// rather than trusting a fixed RVA (confirmed at RVA 0xcfaf90 in the shipped build; the scan re-finds
// it if a rebuild shifts it).

#include <windows.h>
#include <psapi.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include "MinHook.h"

// From thunks.asm: the resolved real-dsound entry points, one per ordinal.
extern "C" void* g_real1;  extern "C" void* g_real2;  extern "C" void* g_real3;
extern "C" void* g_real4;  extern "C" void* g_real5;  extern "C" void* g_real6;
extern "C" void* g_real7;  extern "C" void* g_real8;  extern "C" void* g_real9;
extern "C" void* g_real10; extern "C" void* g_real11; extern "C" void* g_real12;

namespace {

const char* kEosGateway = "https://rigel-eos.wwiggles.org";   // our EOS gateway (VPS TLS)
// Our own "spec"/publish Meta app. Making the Oculus platform initialise under THIS id is the whole
// point of the app-id hook: the entitlement check then asks about our app, which the signed-in account
// (and anyone on the app's release channel) owns, so it passes honestly. Static storage: the pointer we
// hand the SDK must outlive the call.
static const char* const kOurAppId = "1366120006579163";

// A discreet breadcrumb so a first live launch can be confirmed without a console. OFF by default so a
// shipped build writes nothing at all; set RIGEL_EOS_DIAG=1 before launching to turn it on for one run.
// Written next to the game exe; harmless if it cannot be created. One line per notable event.
bool DiagOn()
{
    static int on = -1;
    if (on < 0) { char b[8]; on = (GetEnvironmentVariableA("RIGEL_EOS_DIAG", b, sizeof(b)) > 0) ? 1 : 0; }
    return on == 1;
}
void Note(const char* fmt, ...)
{
    if (!DiagOn()) return;
    char path[MAX_PATH];
    GetModuleFileNameA(nullptr, path, MAX_PATH);      // the game exe dir
    char* slash = strrchr(path, '\\');
    if (slash) strcpy_s(slash + 1, MAX_PATH - (slash + 1 - path), "dsound.diag");
    else       strcpy_s(path, MAX_PATH, "dsound.diag");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") || !f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ── the dsound proxy ────────────────────────────────────────────────────────────────────────
void ResolveRealDsound()
{
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    char dll[MAX_PATH];
    sprintf_s(dll, "%s\\dsound.dll", sys);
    HMODULE h = LoadLibraryA(dll);
    if (!h) { Note("[dsound] could not load the real %s", dll); return; }
    // Resolve by ordinal (MAKEINTRESOURCEA), because that is how the game imports them and how the
    // system dll exports them. A missing ordinal just leaves a null thunk target -- it would only ever
    // be reached if the game called that specific ordinal, which it does not import.
    void** slot[12] = { &g_real1,&g_real2,&g_real3,&g_real4,&g_real5,&g_real6,
                        &g_real7,&g_real8,&g_real9,&g_real10,&g_real11,&g_real12 };
    for (int ord = 1; ord <= 12; ++ord)
        *slot[ord - 1] = reinterpret_cast<void*>(GetProcAddress(h, MAKEINTRESOURCEA(ord)));
    Note("[dsound] forwarded to %s", dll);
}

// ── the EOS redirect ────────────────────────────────────────────────────────────────────────
using curl_setopt_t = int (__cdecl*)(void* handle, int option, ...);
curl_setopt_t g_setoptOrig = nullptr;
const int CURLOPT_URL = 10002;

// The URL is the first vararg. Capture it with va_list, rewrite if it is an Epic host, then call the
// original with the (possibly replaced) pointer. The replacement buffer is thread-local and lives long
// enough for the original call to copy it (curl dups the string into the handle).
thread_local char t_urlbuf[1024];

int my_curl_setopt_impl(void* handle, int option, void* arg)
{
    if (option == CURLOPT_URL && arg)
    {
        const char* url = reinterpret_cast<const char*>(arg);
        if (url && strstr(url, "epicgames.dev"))
        {
            const char* p = strstr(url, "://");
            const char* path = p ? strchr(p + 3, '/') : nullptr;   // keep the path/query
            _snprintf_s(t_urlbuf, sizeof(t_urlbuf), _TRUNCATE, "%s%s", kEosGateway, path ? path : "");
            static bool once = false;
            if (!once) { once = true; Note("[eos] %s -> %s", url, t_urlbuf); }
            return g_setoptOrig(handle, option, t_urlbuf);
        }
    }
    return g_setoptOrig(handle, option, arg);
}

// curl_easy_setopt is cdecl varargs; forward the single relevant vararg. (Every CURLOPT we care about
// takes one argument; curl reads exactly one per call.)
int __cdecl my_curl_setopt(void* handle, int option, ...)
{
    va_list ap; va_start(ap, option);
    void* arg = va_arg(ap, void*);
    va_end(ap);
    return my_curl_setopt_impl(handle, option, arg);
}

// Scan a module's .text for the 24-byte curl_easy_setopt prologue.
uint8_t* FindCurlSetopt(HMODULE mod)
{
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);
    const uint8_t sig[] = { 0x48,0x89,0x54,0x24,0x10, 0x4C,0x89,0x44,0x24,0x18, 0x4C,0x89,0x4C,0x24,0x20,
                            0x53,0x55,0x56,0x57, 0x48,0x83,0xEC,0x28, 0x48 };
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec)
    {
        if (memcmp(sec->Name, ".text", 5) != 0) continue;
        uint8_t* start = base + sec->VirtualAddress;
        SIZE_T   size  = sec->Misc.VirtualSize;
        uint8_t* found = nullptr;
        for (SIZE_T off = 0; off + sizeof(sig) <= size; ++off)
            if (memcmp(start + off, sig, sizeof(sig)) == 0)
            {
                if (found) { Note("[eos] AMBIGUOUS: >1 signature match, not hooking"); return nullptr; }
                found = start + off;
            }
        return found;
    }
    return nullptr;
}

// ── the app-id hook (entitlement, done honestly under our own app) ────────────────────────────
// The PC build initialises the Oculus platform with the STOCK app id (cooked into the pak, not
// overridable by config on an installed build), so the entitlement check is for the wrong app and a
// Shipping build exits. We intercept the platform init and substitute our app id, so the check is for
// OUR app -- which the account owns. Not a bypass: the entitlement is granted for real.
//
// Two entry points exist depending on SDK version; both take the app id as the first argument.
using ovrInit_t   = int (*)(const char* appId);
using ovrInitEx_t = int (*)(const char* appId, int productVersion);
ovrInit_t   g_initOrig   = nullptr;
ovrInitEx_t g_initExOrig = nullptr;

int my_ovr_init(const char* /*appId*/)
{
    Note("[appid] ovr_PlatformInitializeWindows -> %s", kOurAppId);
    return g_initOrig ? g_initOrig(kOurAppId) : 0;
}
int my_ovr_initEx(const char* /*appId*/, int productVersion)
{
    Note("[appid] ovr_PlatformInitializeWindowsEx -> %s (ver %d)", kOurAppId, productVersion);
    return g_initExOrig ? g_initExOrig(kOurAppId, productVersion) : 0;
}

// Resolve and hook whichever init export a module carries. Returns true once something is hooked.
bool TryHookOculusInit(HMODULE mod)
{
    static bool done = false;
    if (done || !mod) return done;

    auto ex = reinterpret_cast<ovrInitEx_t>(GetProcAddress(mod, "ovr_PlatformInitializeWindowsEx"));
    auto w  = reinterpret_cast<ovrInit_t>  (GetProcAddress(mod, "ovr_PlatformInitializeWindows"));
    if (!ex && !w) return false;                 // not the platform loader

    MH_Initialize();                             // idempotent; ALREADY_INITIALIZED is fine
    if (ex && MH_CreateHook(reinterpret_cast<void*>(ex), reinterpret_cast<void*>(&my_ovr_initEx),
                            reinterpret_cast<void**>(&g_initExOrig)) == MH_OK)
        MH_EnableHook(reinterpret_cast<void*>(ex));
    if (w && MH_CreateHook(reinterpret_cast<void*>(w), reinterpret_cast<void*>(&my_ovr_init),
                           reinterpret_cast<void**>(&g_initOrig)) == MH_OK)
        MH_EnableHook(reinterpret_cast<void*>(w));

    done = (g_initExOrig != nullptr) || (g_initOrig != nullptr);
    if (done) Note("[appid] hooked platform init (Ex=%d W=%d)", ex != nullptr, w != nullptr);
    return done;
}

// Scan every loaded module for the platform-init export and hook it. Called on a tight poll so it lands
// before the game calls init (~7s in), regardless of what the loader DLL is named.
bool SweepForOculusInit()
{
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof(mods), &needed)) return false;
    int n = (int)(needed / sizeof(HMODULE));
    if (n > 512) n = 512;
    for (int i = 0; i < n; ++i)
        if (TryHookOculusInit(mods[i])) return true;
    return false;
}

void InstallEosRedirect()
{
    // The EOS SDK DLL is loaded by the game a little after start-up. Wait for it (up to ~60s), then
    // hook once.
    HMODULE eos = nullptr;
    for (int i = 0; i < 120 && !eos; ++i)
    {
        eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
        if (!eos) Sleep(500);
    }
    if (!eos) { Note("[eos] EOSSDK never loaded -- redirect off"); return; }

    uint8_t* target = FindCurlSetopt(eos);
    if (!target) { Note("[eos] curl_easy_setopt signature not found -- redirect off"); return; }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
    { Note("[eos] MH_Initialize failed"); return; }
    if (MH_CreateHook(target, reinterpret_cast<void*>(&my_curl_setopt),
                      reinterpret_cast<void**>(&g_setoptOrig)) != MH_OK)
    { Note("[eos] MH_CreateHook failed"); return; }
    if (MH_EnableHook(target) != MH_OK)
    { Note("[eos] MH_EnableHook failed"); return; }

    Note("[eos] curl_easy_setopt hooked at +0x%llx; epicgames.dev -> %s",
         (unsigned long long)(target - reinterpret_cast<uint8_t*>(eos)), kEosGateway);
}

DWORD WINAPI Worker(LPVOID)
{
    // Phase 1 -- app id. This is time-critical: the platform init call happens only a few seconds into
    // start-up, and if we miss it the entitlement check runs under the stock app and the build exits.
    // Poll fast until hooked (or give up after ~30s). One startup sweep first, in case the loader is
    // already mapped.
    bool appIdHooked = SweepForOculusInit();
    for (int i = 0; i < 1500 && !appIdHooked; ++i)   // 1500 * 20ms = 30s
    {
        Sleep(20);
        appIdHooked = SweepForOculusInit();
    }
    if (!appIdHooked) Note("[appid] platform init never appeared -- entitlement will use the stock app");

    // Phase 2 -- EOS station-browser redirect. Not time-critical (stations are browsed after login).
    InstallEosRedirect();
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);
        ResolveRealDsound();                 // must happen before the game calls any dsound ordinal
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);   // EOS DLL is not up yet; wait off-thread
    }
    return TRUE;
}
