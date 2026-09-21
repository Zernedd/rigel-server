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

void Log(const char* fmt, ...)
{
    char path[MAX_PATH];
    DWORD n = GetTempPathA(MAX_PATH, path);
    if (n == 0 || n > MAX_PATH) strcpy_s(path, MAX_PATH, ".\\");
    strcat_s(path, MAX_PATH, "spec_editor.log");
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

DWORD WINAPI Worker(LPVOID)
{
    se::Log("=== Spec Editor starting (pid=%lu) ===", GetCurrentProcessId());

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

    se::Log("[boot] ready. INSERT toggles the editor UI.");
    return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE mod, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(mod);
        ResolveRealDsound();
        CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    }
    return TRUE;
}
