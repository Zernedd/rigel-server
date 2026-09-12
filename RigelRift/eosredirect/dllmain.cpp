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
#include <cstdlib>
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

// Defined in the EOS section below, declared here: the GetProcAddress hook installs the curl redirect
// the moment EOS_Initialize is resolved (before EOS_Platform_Create fetches the endpoint table).
void HookEosCurl();
static bool          g_eosDone  = false;
static volatile long g_eosGuard = 0;

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
    // %TEMP%\rigel_eos.diag -- always writable, unlike the install dir under Program Files.
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) strcpy_s(path, MAX_PATH, ".\\");
    strcat_s(path, MAX_PATH, "rigel_eos.diag");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") || !f) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ── the app id, done at its real source: config ──────────────────────────────────────────────
// [2026-09-12] Confirmed in IDA: the game reads the app id straight from config --
//     GConfig->GetString(L"OnlineSubsystemOculus", L"RiftAppId", out, GEngineIni)  (sub_14506CA30)
//     -> atoll(out) -> ovr_PlatformInitializeWindows
// so the entitlement check is only ever for whatever RiftAppId resolves to. make_rift.py writes exactly
// that key, but into the BUILD folder's Engine.ini -- and an INSTALLED build reads user config from
// %LOCALAPPDATA%\A2\Saved\Config\Windows\ instead (the game's own log reads OculusXR.ini from there), so
// our override was ignored and the cooked STOCK id (23916854551246326) won.
//
// Fix it at the source and be done with the fragile function-pointer race: write the override into the
// user Engine.ini UE actually reads. This DLL is a static import, so its DllMain runs during process
// load -- before UE's config system initialises -- so the value is in place before GConfig reads it.
// User config layers OVER the cooked DefaultEngine.ini, so our RiftAppId wins. Idempotent
// (WritePrivateProfileString just sets the one key), and it applies for whatever account runs the game
// because %LOCALAPPDATA% resolves per-user.
void WriteAppIdConfig()
{
    const char* lad = getenv("LOCALAPPDATA");
    if (!lad || !*lad) { Note("[appid] no LOCALAPPDATA -- cannot write config override"); return; }

    char dir[MAX_PATH];
    if (_snprintf_s(dir, sizeof(dir), _TRUNCATE, "%s\\A2\\Saved\\Config\\Windows", lad) < 0) return;

    // Create A2\Saved\Config\Windows one segment at a time (dependency-free).
    char part[MAX_PATH];
    strcpy_s(part, sizeof(part), dir);
    for (char* p = part + 3; *p; ++p)                 // skip "C:\"
        if (*p == '\\') { *p = 0; CreateDirectoryA(part, nullptr); *p = '\\'; }
    CreateDirectoryA(part, nullptr);

    char ini[MAX_PATH];
    if (_snprintf_s(ini, sizeof(ini), _TRUNCATE, "%s\\Engine.ini", dir) < 0) return;

    // 1. App id -- so the entitlement check is for our app (already proven).
    BOOL a = WritePrivateProfileStringA("OnlineSubsystemOculus", "RiftAppId", kOurAppId, ini);
    Note("[appid] config %s: [OnlineSubsystemOculus] RiftAppId=%s -> %s", a ? "written" : "FAILED", kOurAppId, ini);

    // 2. Mothership BaseUrl -- login / user data. The PC build has the STOCK mothership host cooked into
    //    its pak, and A2 reads [OnlineSubsystemMothership] BaseUrl from Engine.ini via GConfig
    //    (confirmed in the exe: sub_145165FD0). make_rift wrote this into the BUILD folder, which an
    //    installed build ignores -- same trap as the app id -- so the PC client was authenticating
    //    against the stock mothership and being denied. Write it where the installed build reads it.
    //    The Quest build overrides only BaseUrl (TitleId/EnvironmentId/DeploymentId stay stock), so we do
    //    the same.
    BOOL m = WritePrivateProfileStringA("OnlineSubsystemMothership", "BaseUrl",
                                        "https://rigel-ms.wwiggles.org", ini);
    Note("[ms] config %s: [OnlineSubsystemMothership] BaseUrl=https://rigel-ms.wwiggles.org", m ? "written" : "FAILED");

    // 3. EOS BaseUrl -- harmless if the SDK ignores it (its host is fetched at runtime); set for parity.
    WritePrivateProfileStringA("OnlineSubsystemEOS", "BaseUrl", "https://rigel-eos.wwiggles.org", ini);
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
// [2026-09-12] The real curl_easy_setopt in EOSSDK-Win64 is NOT the generic varargs shim my first
// signature matched (that was a formatter -- the hook never fired). Found the true one by the CURLOPT_URL
// constant (0x2712): a 3-argument setter (handle=rcx, option=edx, value=r8) called 70 times with the full
// range of option constants, whose prologue carries curl's null-handle check (test rcx,rcx; lea eax,[rcx+0x2b]
// = return CURLE_BAD_FUNCTION_ARGUMENT). So the value is a plain register arg -- no va_list -- and for
// CURLOPT_URL r8 is the URL string. Hook it and swap that pointer for an Epic host.
using curl_setopt_t = int (__fastcall*)(void* handle, int option, void* value, void* a4);
curl_setopt_t g_setoptOrig = nullptr;
const int CURLOPT_URL = 10002;

// Replacement URL buffer; thread-local so concurrent handles don't clobber each other, and it lives long
// enough for the original call to copy the string into the handle.
thread_local char t_urlbuf[1024];

int __fastcall my_curl_setopt(void* handle, int option, void* value, void* a4)
{
    static volatile long s_calls = 0, s_urls = 0;
    if (InterlockedIncrement(&s_calls) == 1) Note("[eosdiag] setopt hook IS being called");
    if (option == CURLOPT_URL && value)
    {
        const char* url = reinterpret_cast<const char*>(value);
        long u = InterlockedIncrement(&s_urls);
        if (u <= 20) Note("[eosdiag] CURLOPT_URL: %s", url);
        if (strstr(url, "epicgames.dev") || strstr(url, "epicgames.com") || strstr(url, "epicgames.net"))
        {
            const char* p = strstr(url, "://");
            const char* path = p ? strchr(p + 3, '/') : nullptr;   // keep the path/query
            _snprintf_s(t_urlbuf, sizeof(t_urlbuf), _TRUNCATE, "%s%s", kEosGateway, path ? path : "");
            Note("[eos] %s -> %s", url, t_urlbuf);
            return g_setoptOrig(handle, option, reinterpret_cast<void*>(t_urlbuf), a4);
        }
    }
    return g_setoptOrig(handle, option, value, a4);
}

// Scan a module's .text for the 24-byte curl_easy_setopt prologue.
uint8_t* FindCurlSetopt(HMODULE mod)
{
    auto base = reinterpret_cast<uint8_t*>(mod);
    auto dos  = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    auto sec  = IMAGE_FIRST_SECTION(nt);
    // curl_easy_setopt @ 0xcf7b00: spill edx/r8/r9 to home, sub rsp,0x28, then curl's null-handle guard
    // (test rcx,rcx; jne +8; lea eax,[rcx+0x2b] = return CURLE_BAD_FUNCTION_ARGUMENT). Distinctive.
    const uint8_t sig[] = { 0x89,0x54,0x24,0x10, 0x4C,0x89,0x44,0x24,0x18, 0x4C,0x89,0x4C,0x24,0x20,
                            0x48,0x83,0xEC,0x28, 0x48,0x85,0xC9, 0x75,0x08, 0x8D };
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

// [2026-09-12] We first tried hooking the init export after the platform loader appeared, on a tight
// module-sweep poll. The diag proved that loses the race: "[appid] hooked platform init (Ex=0 W=1)"
// printed, but the substitution line never did -- the plugin had already CALLED
// ovr_PlatformInitializeWindows (with the stock app id) before our hook landed, ~9s into start-up.
//
// So intercept at RESOLUTION instead of racing the call: the plugin loads the loader DLL dynamically
// and looks its functions up with GetProcAddress (there is no static import -- confirmed: the exe
// imports nothing ovr/platform). Hook GetProcAddress and, for the two init names, hand back our own
// wrapper while stashing the real pointer to call through. This cannot be out-raced: the function has
// to be resolved before it can be called.
using GetProcAddress_t = FARPROC (WINAPI*)(HMODULE, LPCSTR);
GetProcAddress_t g_gpaOrig = nullptr;

FARPROC WINAPI my_GetProcAddress(HMODULE mod, LPCSTR name)
{
    FARPROC real = g_gpaOrig(mod, name);
    // name can be an ordinal (low word only); only compare real string pointers.
    if (real && reinterpret_cast<uintptr_t>(name) > 0xffff)
    {
        if (strcmp(name, "ovr_PlatformInitializeWindowsEx") == 0)
        {
            g_initExOrig = reinterpret_cast<ovrInitEx_t>(real);
            Note("[appid] intercepted GetProcAddress(ovr_PlatformInitializeWindowsEx)");
            return reinterpret_cast<FARPROC>(&my_ovr_initEx);
        }
        if (strcmp(name, "ovr_PlatformInitializeWindows") == 0)
        {
            g_initOrig = reinterpret_cast<ovrInit_t>(real);
            Note("[appid] intercepted GetProcAddress(ovr_PlatformInitializeWindows)");
            return reinterpret_cast<FARPROC>(&my_ovr_init);
        }
        // The SDK resolves EOS_Initialize before it does anything with EOS. That is our cue to get the
        // curl redirect in BEFORE EOS_Platform_Create fetches (and caches) the endpoint table -- which is
        // the request that decides, for the whole session, whether auth goes to our gateway or real EOS.
        if (!g_eosDone && strcmp(name, "EOS_Initialize") == 0)
        {
            Note("[eos] EOS_Initialize resolved -- installing curl redirect now");
            HookEosCurl();
        }
        // [eosdiag] If EOS resolves a WinHTTP transport dynamically, it is NOT using curl on Windows and
        // that is why the curl hook sees nothing -- surface it.
        if (strncmp(name, "WinHttp", 7) == 0)
            Note("[eosdiag] GetProcAddress(%s)  (WinHTTP transport in use?)", name);
    }
    return real;
}

// Install the GetProcAddress hook. Runs on the worker thread, which the loader starts only after our
// DllMain returns -- so we are NOT under the loader lock and MH_EnableHook's thread suspend is safe.
bool InstallAppIdHook()
{
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    void* gpa = reinterpret_cast<void*>(GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress"));
    if (!gpa) { Note("[appid] could not find GetProcAddress"); return false; }
    if (MH_CreateHook(gpa, reinterpret_cast<void*>(&my_GetProcAddress),
                      reinterpret_cast<void**>(&g_gpaOrig)) != MH_OK) { Note("[appid] hook create failed"); return false; }
    if (MH_EnableHook(gpa) != MH_OK) { Note("[appid] hook enable failed"); return false; }
    Note("[appid] GetProcAddress hooked; will substitute app id %s at resolution time", kOurAppId);
    return true;
}

// [2026-09-12] TIMING is everything here. A live run proved the redirect was installing too late: the
// EOS SDK fetches its endpoint table (/sdk/v1/product) at EOS_Platform_Create, caches whatever hosts it
// gets, and then does ALL auth against those. If our curl hook is not in place for that FIRST fetch, the
// SDK talks to the REAL epicgames.dev, caches real endpoints, and the whole session's auth goes there --
// the client saw HTTP 401 "Unauthorized Oculus user ID and nonce" from real EOS, and our gateway logged
// nothing at all for that run.
//
// So the curl hook MUST be in before EOS_Platform_Create. EOSSDK is loaded dynamically and the game
// resolves its exports with GetProcAddress, so the app-id GetProcAddress hook already sees EOS_Initialize
// (the very first EOS call) being looked up -- hook curl right there, before the SDK is used. Idempotent
// and serialised so the worker's fallback poll and the GetProcAddress path can't collide.
void HookEosCurl()
{
    if (g_eosDone) return;
    if (InterlockedCompareExchange(&g_eosGuard, 1, 0) != 0) return;   // someone else is doing it
    HMODULE eos = GetModuleHandleA("EOSSDK-Win64-Shipping.dll");
    if (eos)
    {
        uint8_t* target = FindCurlSetopt(eos);
        if (!target) Note("[eos] curl_easy_setopt signature not found -- redirect off");
        else if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED)
            Note("[eos] MH_Initialize failed");
        else if (MH_CreateHook(target, reinterpret_cast<void*>(&my_curl_setopt),
                               reinterpret_cast<void**>(&g_setoptOrig)) != MH_OK)
            Note("[eos] MH_CreateHook failed");
        else if (MH_EnableHook(target) != MH_OK)
            Note("[eos] MH_EnableHook failed");
        else
        {
            g_eosDone = true;
            Note("[eos] curl_easy_setopt hooked at +0x%llx; epicgames.dev -> %s",
                 (unsigned long long)(target - reinterpret_cast<uint8_t*>(eos)), kEosGateway);
        }
    }
    InterlockedExchange(&g_eosGuard, 0);
}

DWORD WINAPI Worker(LPVOID)
{
    // Install the GetProcAddress interceptor immediately. It does two time-critical jobs at resolution
    // time, both before the game uses the thing being resolved: substitutes our app id when the platform
    // init is looked up, and installs the EOS curl hook when EOS_Initialize is looked up.
    InstallAppIdHook();

    // Fallback only: if the GetProcAddress path ever misses EOS_Initialize, still get the curl hook in as
    // soon as the module is present. Tight poll early (the SDK loads within a few seconds), then relax.
    for (int i = 0; i < 4000 && !g_eosDone; ++i)
    {
        HookEosCurl();
        Sleep(g_eosDone ? 0 : (i < 400 ? 5 : 250));
    }
    if (!g_eosDone) Note("[eos] never hooked curl -- EOS traffic will hit the real backend");
    return 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);
        WriteAppIdConfig();                  // BEFORE UE reads config: point RiftAppId at our own app
        ResolveRealDsound();                 // must happen before the game calls any dsound ordinal
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);   // EOS DLL is not up yet; wait off-thread
    }
    return TRUE;
}
