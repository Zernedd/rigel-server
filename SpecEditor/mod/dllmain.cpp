// dllmain.cpp - Spec Editor entry point.
//
// Shipped as dsound.dll next to A2-Win64-Shipping.exe, the same proxy seam the Rigel EOS redirect uses:
// the exe statically imports dsound by ordinal, so the loader picks ours up first and we get to run at
// process start without an injector. Every ordinal is forwarded to the real system dsound, so audio is
// untouched.
//
// Install order matters. The render hook can go in immediately, because it only needs D3D12. The game
// hook must wait until the engine image is actually up - ProcessEvent is patched in place, and doing
// that before UE has finished its own start-up is how you get a mysterious early crash.

#include "se_core.h"
#include "MinHook.h"
#include <cstdio>
#include <share.h>
#include <cstring>

extern "C" void* g_real1;  extern "C" void* g_real2;  extern "C" void* g_real3;
extern "C" void* g_real4;  extern "C" void* g_real5;  extern "C" void* g_real6;
extern "C" void* g_real7;  extern "C" void* g_real8;  extern "C" void* g_real9;
extern "C" void* g_real10; extern "C" void* g_real11; extern "C" void* g_real12;

namespace se {

EditorState& State()
{
    static EditorState s;
    return s;
}

CameraInput& Cam()
{
    static CameraInput c;
    return c;
}

LiveDrag& Drag()
{
    static LiveDrag d;
    return d;
}

PickState& Pick()
{
    static PickState p;
    return p;
}

Notice& Notes()
{
    static Notice n;
    return n;
}

ProblemBox& Problems()
{
    static ProblemBox b;
    return b;
}

DWORD g_mainThread = 0;

void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) strcpy_s(path, MAX_PATH, ".\\");
    strcat_s(path, MAX_PATH, "spec_editor.log");
    // _fsopen with _SH_DENYNO, NOT fopen_s: fopen_s opens the file non-shareable, so while anything else
    // held it open -- a `tail -f`, an editor, a log viewer -- every later line was silently dropped. The
    // first live test lost its whole log that way after line one and looked exactly like a hang.
    FILE* f = _fsopen(path, "a", _SH_DENYNO);
    if (!f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "[%02d:%02d:%02d] ", st.wHour, st.wMinute, st.wSecond);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

}  // namespace se

namespace {

void ResolveRealDsound()
{
    char sys[MAX_PATH];
    GetSystemDirectoryA(sys, MAX_PATH);
    strcat_s(sys, MAX_PATH, "\\dsound.dll");
    HMODULE real = LoadLibraryA(sys);
    if (!real) { se::Log("[boot] could not load the real dsound at %s", sys); return; }
    g_real1  = GetProcAddress(real, MAKEINTRESOURCEA(1));
    g_real2  = GetProcAddress(real, MAKEINTRESOURCEA(2));
    g_real3  = GetProcAddress(real, MAKEINTRESOURCEA(3));
    g_real4  = GetProcAddress(real, MAKEINTRESOURCEA(4));
    g_real5  = GetProcAddress(real, MAKEINTRESOURCEA(5));
    g_real6  = GetProcAddress(real, MAKEINTRESOURCEA(6));
    g_real7  = GetProcAddress(real, MAKEINTRESOURCEA(7));
    g_real8  = GetProcAddress(real, MAKEINTRESOURCEA(8));
    g_real9  = GetProcAddress(real, MAKEINTRESOURCEA(9));
    g_real10 = GetProcAddress(real, MAKEINTRESOURCEA(10));
    g_real11 = GetProcAddress(real, MAKEINTRESOURCEA(11));
    g_real12 = GetProcAddress(real, MAKEINTRESOURCEA(12));
}

// The game's own window, once it exists and is showing. Its appearance is the signal that the game has
// finished creating its D3D12 device and swapchain.
BOOL CALLBACK FindGameWindow(HWND w, LPARAM out)
{
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(w)) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(w, cls, 64);
    if (wcscmp(cls, L"UnrealWindow") != 0) return TRUE;
    *reinterpret_cast<HWND*>(out) = w;
    return FALSE;
}

// The build's Oculus entitlement self-exit ("Could not verify entitlement status ... A Shipping build would
// exit at this point"). On a PC whose Meta sign-in cannot produce an entitlement it lands right after
// engine init and closes the client. Same signature-guarded jne->jmp flip as the HalcyonA2 payload and the
// A2EntitlementPatch UE4SS mod; we load at process start, so we always get there first.
void PatchEntitlementExit()
{
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    auto* ep = reinterpret_cast<uint8_t*>(base + 0x5429424);
    static const uint8_t sig[] = { 0x40, 0x84, 0xED, 0x75, 0x59, 0x80, 0x3D };
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(ep, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) { se::Log("[boot] entitlement patch: address not mapped"); return; }
    if (ep[3] == 0xEB) { se::Log("[boot] entitlement exit already patched"); return; }
    if (memcmp(ep, sig, sizeof(sig)) != 0) { se::Log("[boot] entitlement patch: signature mismatch - not patched"); return; }
    DWORD old = 0;
    if (!VirtualProtect(ep + 3, 1, PAGE_EXECUTE_READWRITE, &old)) return;
    ep[3] = 0xEB;
    VirtualProtect(ep + 3, 1, old, &old);
    FlushInstructionCache(GetCurrentProcess(), ep + 3, 1);
    se::Log("[boot] entitlement self-exit patched");
}

DWORD WINAPI Worker(LPVOID)
{
    se::Log("=== Spec Editor starting (pid=%lu) ===", GetCurrentProcessId());
#ifndef RIGEL_EOS
    PatchEntitlementExit();   // not in the published Rift build: there entitlement passes for real (RiftAppId)
#endif

    // Do NOT probe D3D12 yet. This thread starts from dsound's DllMain, i.e. at process start, and
    // building our throwaway device + swapchain then races the game's own device creation inside the GPU
    // driver: the first live run deadlocked right here, with the game frozen at ~200 MB and this log
    // stopped at the line above. Wait until the game's window is up -- its renderer exists by then --
    // and a little longer, so nothing is initialising concurrently.
    HWND game = nullptr;
    for (int i = 0; i < 1200 && !game; ++i)            // up to 2 minutes
    {
        EnumWindows(FindGameWindow, reinterpret_cast<LPARAM>(&game));
        if (!game) Sleep(100);
    }
    if (!game) se::Log("[boot] game window never appeared - probing anyway");
    else       se::Log("[boot] game window up (%p); settling before the render probe", (void*)game);
    Sleep(4000);

    if (!se::InstallRenderHook())
        se::Log("[boot] render hook failed - no UI this run");

    // Wait for the engine to be up before patching ProcessEvent. A live UWorld is the signal that the
    // object system and the game thread are both real.
    for (int i = 0; i < 2400; ++i)
    {
        if (GetModuleHandleW(nullptr) && se::State().ReadSnapshot().worldReady) break;
        Sleep(50);
        if (i == 60 && !se::InstallGameHook()) continue;   // first attempt once the image settles
    }
    if (!se::InstallGameHook())
        se::Log("[boot] game hook not installed");

    se::Log("[boot] ready. F12 (or Insert) toggles the editor UI.");
    return 0;
}

}  // namespace

#ifdef RIGEL_EOS
void RigelEos_Attach();   // RigelRift\eosredirect\dllmain.cpp: app-id config + EOS redirect + glyph fix
#endif

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);
        // dsound is a static import, so this runs during process start-up on the main thread -- which in
        // UE is the game thread. Everything UObject in the mod is gated on it.
        se::g_mainThread = GetCurrentThreadId();
        ResolveRealDsound();
#ifdef RIGEL_EOS
        RigelEos_Attach();   // the published Rift build: it is ALSO the station-browser redirect
#endif
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
