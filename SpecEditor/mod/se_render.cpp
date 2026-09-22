// se_render.cpp - the render half: get ImGui on screen inside the game's D3D12 swapchain.
//
// The game ships its own Dear ImGui (UImGuiManager), but it exposes no UFunctions and its context is
// private native state, so we cannot draw into it. We therefore bring our own ImGui and hook the
// swapchain, which is the standard overlay approach and keeps our UI completely independent of the
// game's debug UI.
//
// Hooking D3D12 needs the vtables of IDXGISwapChain3 and ID3D12CommandQueue, and you cannot get those
// without a device. Creating a throwaway device + swapchain on a hidden window to read the vtable
// layout is the usual trick and is safe: it touches nothing the game owns.
//
// Everything here runs on the RENDER thread. It may read the snapshot and push commands. It must never
// touch a UObject - see se_core.h.

#include "se_core.h"

#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include "MinHook.h"
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx12.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

namespace se {

#ifdef RIGEL_EOS
bool g_uiVisible = false;   // published Rift build: hidden until INSERT, so ordinary spectators never see it
#else
bool g_uiVisible = true;
#endif

namespace {

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain3*, UINT, UINT);
typedef void(__stdcall* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
typedef HRESULT(__stdcall* ResizeBuffers_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
typedef HRESULT(__stdcall* ResizeBuffers1_t)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
ResizeBuffers_t        g_resizeOrig  = nullptr;
ResizeBuffers1_t       g_resize1Orig = nullptr;

Present_t              g_presentOrig = nullptr;
ExecuteCommandLists_t  g_execOrig    = nullptr;
WNDPROC                g_wndProcOrig = nullptr;

ID3D12Device*              g_device    = nullptr;
ID3D12CommandQueue*        g_cmdQueue  = nullptr;   // learned from ExecuteCommandLists
ID3D12DescriptorHeap*      g_srvHeap   = nullptr;
ID3D12DescriptorHeap*      g_rtvHeap   = nullptr;
ID3D12GraphicsCommandList* g_cmdList   = nullptr;
HWND                       g_hwnd      = nullptr;
bool                       g_imguiReady = false;

struct FrameCtx
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12Resource*         backbuffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
};
std::vector<FrameCtx> g_frames;

// UE-style dark theme. Deliberately close to the editor: near-black panels, thin square-ish corners,
// a blue-grey accent, and tight padding so dense property rows read well.
void ApplyUnrealStyle()
{
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 2.0f;
    s.ChildRounding     = 2.0f;
    s.FrameRounding     = 2.0f;
    s.PopupRounding     = 2.0f;
    s.ScrollbarRounding = 2.0f;
    s.TabRounding       = 2.0f;
    s.GrabRounding      = 2.0f;
    s.WindowPadding     = ImVec2(6, 6);
    s.FramePadding      = ImVec2(6, 3);
    s.ItemSpacing       = ImVec2(6, 4);
    s.IndentSpacing     = 16.0f;
    s.ScrollbarSize     = 12.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.WindowMenuButtonPosition = ImGuiDir_None;

    // UE5's stock dark palette (FStyleColors): Background #151515, Panel #242424, Header #2F2F2F,
    // Recessed #1A1A1A, Input #0F0F0F, InputOutline/Dropdown #383838, Hover #575757, Foreground #C0C0C0,
    // Primary/Select #0070E0. Panels are #242424 with #151515 gutters between them, inputs are recessed
    // near-black wells, and the one saturated colour is the selection blue.
    auto hex = [](unsigned v, float a = 1.0f) {
        return ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f, (v & 0xFF) / 255.0f, a);
    };
    ImVec4* c = s.Colors;
    const ImVec4 background = hex(0x151515), panel = hex(0x242424), header = hex(0x2F2F2F);
    const ImVec4 recessed = hex(0x1A1A1A), input = hex(0x0F0F0F), outline = hex(0x383838);
    const ImVec4 hover = hex(0x575757), fg = hex(0xC0C0C0), primary = hex(0x0070E0);

    c[ImGuiCol_Text]                 = fg;
    c[ImGuiCol_TextDisabled]         = hex(0x808080);
    c[ImGuiCol_WindowBg]             = panel;
    c[ImGuiCol_ChildBg]              = panel;
    c[ImGuiCol_PopupBg]              = hex(0x1A1A1A, 0.98f);
    c[ImGuiCol_Border]               = background;
    c[ImGuiCol_BorderShadow]         = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg]              = input;
    c[ImGuiCol_FrameBgHovered]       = recessed;
    c[ImGuiCol_FrameBgActive]        = input;
    c[ImGuiCol_TitleBg]              = background;
    c[ImGuiCol_TitleBgActive]        = background;
    c[ImGuiCol_TitleBgCollapsed]     = background;
    c[ImGuiCol_MenuBarBg]            = background;
    c[ImGuiCol_ScrollbarBg]          = panel;
    c[ImGuiCol_ScrollbarGrab]        = outline;
    c[ImGuiCol_ScrollbarGrabHovered] = hover;
    c[ImGuiCol_ScrollbarGrabActive]  = hex(0x808080);
    c[ImGuiCol_CheckMark]            = primary;
    c[ImGuiCol_SliderGrab]           = primary;
    c[ImGuiCol_SliderGrabActive]     = hex(0x1C8AFF);
    c[ImGuiCol_Button]               = outline;
    c[ImGuiCol_ButtonHovered]        = hover;
    c[ImGuiCol_ButtonActive]         = primary;
    c[ImGuiCol_Header]               = header;           // collapsible category bars
    c[ImGuiCol_HeaderHovered]        = hex(0x3A3A3A);
    c[ImGuiCol_HeaderActive]         = primary;
    c[ImGuiCol_Separator]            = background;
    c[ImGuiCol_SeparatorHovered]     = primary;
    c[ImGuiCol_ResizeGrip]           = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab]                  = background;       // inactive tab blends into the gutter
    c[ImGuiCol_TabHovered]           = header;
    c[ImGuiCol_TabSelected]          = panel;            // active tab is the panel colour, as in UE5
    c[ImGuiCol_TabSelectedOverline]  = primary;
    c[ImGuiCol_TabDimmed]            = background;
    c[ImGuiCol_TabDimmedSelected]    = panel;
    c[ImGuiCol_TableHeaderBg]        = header;
    c[ImGuiCol_TableBorderLight]     = background;
    c[ImGuiCol_TableBorderStrong]    = background;
    c[ImGuiCol_TableRowBg]           = panel;
    c[ImGuiCol_TableRowBgAlt]        = hex(0x282828);
    c[ImGuiCol_TextSelectedBg]       = hex(0x0070E0, 0.45f);
    c[ImGuiCol_NavCursor]            = primary;

    s.WindowRounding = 0.0f;       // UE5 panels are square; only small controls are rounded
    s.WindowBorderSize = 0.0f;
    s.FrameBorderSize  = 0.0f;
    s.TabRounding      = 3.0f;
    s.TabBorderSize    = 0.0f;
    s.TabBarOverlineSize = 2.0f;
}

// ── who owns the mouse ────────────────────────────────────────────────────────────────────────
// A2 is a mouse-look game: it hides the cursor, clips it to the window, re-centres it every frame and
// reads raw mouse deltas. With the overlay passing everything through, the cursor vanished the moment it
// entered the window and the editor could not be clicked. So the editor now behaves like Unreal's own
// viewport: while the UI is up the mouse and keyboard are the EDITOR's -- cursor visible and free --
// and holding RIGHT mouse hands both back to the game, so RMB + drag looks and RMB + WASD flies.
typedef BOOL(WINAPI* ClipCursor_t)(const RECT*);
typedef BOOL(WINAPI* SetCursorPos_t)(int, int);
typedef int(WINAPI* ShowCursor_t)(BOOL);
typedef HCURSOR(WINAPI* SetCursor_t)(HCURSOR);
ClipCursor_t   g_clipOrig   = nullptr;
SetCursorPos_t g_setPosOrig = nullptr;
ShowCursor_t   g_showOrig   = nullptr;
SetCursor_t    g_setCurOrig = nullptr;

bool EditorOwnsInput()
{
    // With the editor camera flying the view, the editor owns everything, right mouse included (it is the
    // camera's look button). Otherwise holding RMB hands input to the game, as before.
    return g_uiVisible && g_imguiReady && (Cam().active || !(GetAsyncKeyState(VK_RBUTTON) & 0x8000));
}

POINT g_lockPt{};   // where the cursor is pinned while looking

void BeginLook(HWND hwnd)
{
    GetCursorPos(&g_lockPt);
    SetCapture(hwnd);
    if (g_showOrig) while (g_showOrig(FALSE) >= 0) {}
    Cam().looking = true;
}
void EndLook()
{
    Cam().looking = false;
    ReleaseCapture();
    if (g_setPosOrig) g_setPosOrig(g_lockPt.x, g_lockPt.y);
    if (g_showOrig) while (g_showOrig(TRUE) < 0) {}
}

// While the editor owns input, the game may not confine, re-centre or hide the cursor.
BOOL WINAPI Hook_ClipCursor(const RECT* r)       { return g_clipOrig(EditorOwnsInput() ? nullptr : r); }
BOOL WINAPI Hook_SetCursorPos(int x, int y)      { return EditorOwnsInput() ? TRUE : g_setPosOrig(x, y); }
int  WINAPI Hook_ShowCursor(BOOL show)           { return (EditorOwnsInput() && !show) ? 0 : g_showOrig(show); }
HCURSOR WINAPI Hook_SetCursor(HCURSOR c)
{
    if (EditorOwnsInput() && !c) c = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    return g_setCurOrig(c);
}

bool IsMouseMsg(UINT m)
{
    return m == WM_INPUT || (m >= WM_MOUSEFIRST && m <= WM_MOUSELAST) || m == WM_MOUSEHOVER || m == WM_MOUSELEAVE;
}
bool IsKeyDownMsg(UINT m) { return m == WM_KEYDOWN || m == WM_SYSKEYDOWN || m == WM_CHAR || m == WM_SYSCHAR; }

LRESULT __stdcall WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_INSERT) { g_uiVisible = !g_uiVisible; return 0; }
    if (!g_uiVisible || !g_imguiReady)
    {
        if (Cam().looking) EndLook();
        return CallWindowProc(g_wndProcOrig, hwnd, msg, wp, lp);
    }

    // Unreal-style viewport camera. RMB over the bare viewport starts looking: the cursor is hidden and
    // pinned, and its movement becomes look deltas for the game thread, which also reads WASD/QE/Shift
    // directly. While looking nothing reaches ImGui or the game.
    CameraInput& cam = Cam();
    if (cam.active)
    {
        if (msg == WM_RBUTTONDOWN && !ImGui::GetIO().WantCaptureMouse) { BeginLook(hwnd); return 0; }
        if (cam.looking)
        {
            if (msg == WM_RBUTTONUP || msg == WM_CAPTURECHANGED || msg == WM_KILLFOCUS)
            {
                EndLook();
                return msg == WM_KILLFOCUS ? CallWindowProc(g_wndProcOrig, hwnd, msg, wp, lp) : 0;
            }
            if (msg == WM_MOUSEMOVE)
            {
                POINT p;
                GetCursorPos(&p);
                if (p.x != g_lockPt.x || p.y != g_lockPt.y)
                {
                    cam.dx = cam.dx + static_cast<float>(p.x - g_lockPt.x);
                    cam.dy = cam.dy + static_cast<float>(p.y - g_lockPt.y);
                    if (g_setPosOrig) g_setPosOrig(g_lockPt.x, g_lockPt.y);
                }
                return 0;
            }
            if (msg == WM_MOUSEWHEEL) { cam.wheel += GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1; return 0; }
            if (msg == WM_SETCURSOR)  { if (g_setCurOrig) g_setCurOrig(nullptr); return TRUE; }
            if (msg == WM_INPUT)      return DefWindowProcW(hwnd, msg, wp, lp);
            if (IsMouseMsg(msg) || IsKeyDownMsg(msg) || msg == WM_KEYUP || msg == WM_SYSKEYUP) return 0;
        }
    }

    ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp);   // ImGui always sees input while it is up

    if (!EditorOwnsInput())                              // RMB held: the game drives (look / fly)
        return CallWindowProc(g_wndProcOrig, hwnd, msg, wp, lp);

    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT)
    {
        // Undo whatever hiding the game already did (the display counter can sit below zero), and show
        // an arrow unless ImGui has asked for a resize or text cursor this frame.
        if (g_showOrig) while (g_showOrig(TRUE) < 0) {}
        if (ImGui::GetMouseCursor() == ImGuiMouseCursor_Arrow) SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
        return TRUE;
    }
    if (IsMouseMsg(msg))
    {
        if (g_clipOrig) g_clipOrig(nullptr);
        // WM_INPUT must still be completed through DefWindowProc, or the raw-input queue backs up.
        return msg == WM_INPUT ? DefWindowProcW(hwnd, msg, wp, lp) : 0;
    }
    // Keys go to the editor (W/E/R switch the gizmo, typing goes into search boxes) instead of moving
    // the player. Key-UPs still pass, so nothing held from before RMB was released gets stuck down.
    if (IsKeyDownMsg(msg)) return 0;
    return CallWindowProc(g_wndProcOrig, hwnd, msg, wp, lp);
}

// The swapchain hands us the device; the command queue has to be learned from the game's own
// ExecuteCommandLists, because ImGui's DX12 backend needs the queue the game actually presents with.
void __stdcall Hook_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (!g_cmdQueue && q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_cmdQueue = q;
    g_execOrig(q, n, lists);
}

// The overlay holds a reference to every swapchain back buffer (GetBuffer). DXGI requires ALL of them to be
// released before ResizeBuffers -- holding them made the game's resize fail the moment the window was
// resized, which crashed the client. Release before the resize, rebuild after.
void ReleaseRenderTargets()
{
    for (auto& f : g_frames)
        if (f.backbuffer) { f.backbuffer->Release(); f.backbuffer = nullptr; }
}

void CreateRenderTargets(IDXGISwapChain3* sc)
{
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    const UINT count = desc.BufferCount;
    if (count != g_frames.size() || !g_rtvHeap)
    {
        // Buffer count changed: new RTV heap sized for it, and an allocator per buffer.
        for (auto& f : g_frames) if (f.allocator) { f.allocator->Release(); f.allocator = nullptr; }
        if (g_rtvHeap) { g_rtvHeap->Release(); g_rtvHeap = nullptr; }
        D3D12_DESCRIPTOR_HEAP_DESC rtv{};
        rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv.NumDescriptors = count;
        if (FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap)))) { g_frames.clear(); return; }
        g_frames.assign(count, FrameCtx{});
        for (auto& f : g_frames) g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator));
    }
    const UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < count; ++i)
    {
        g_frames[i].rtv = h;
        if (SUCCEEDED(sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backbuffer))) && g_frames[i].backbuffer)
            g_device->CreateRenderTargetView(g_frames[i].backbuffer, nullptr, h);
        h.ptr += rtvSize;
    }
}

HRESULT __stdcall Hook_ResizeBuffers(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags)
{
    if (g_imguiReady) ReleaseRenderTargets();
    const HRESULT hr = g_resizeOrig(sc, n, w, h, fmt, flags);
    if (g_imguiReady) CreateRenderTargets(sc);
    Log("[render] ResizeBuffers %ux%u buffers=%u -> 0x%08X", w, h, n, static_cast<unsigned>(hr));
    return hr;
}

HRESULT __stdcall Hook_ResizeBuffers1(IDXGISwapChain3* sc, UINT n, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags,
                                      const UINT* masks, IUnknown* const* queues)
{
    if (g_imguiReady) ReleaseRenderTargets();
    const HRESULT hr = g_resize1Orig(sc, n, w, h, fmt, flags, masks, queues);
    if (g_imguiReady) CreateRenderTargets(sc);
    Log("[render] ResizeBuffers1 %ux%u buffers=%u -> 0x%08X", w, h, n, static_cast<unsigned>(hr));
    return hr;
}

bool InitImGui(IDXGISwapChain3* sc)
{
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&g_device)))) { Log("[render] GetDevice failed"); return false; }

    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    g_hwnd = desc.OutputWindow;
    const UINT count = desc.BufferCount;

    D3D12_DESCRIPTOR_HEAP_DESC srv{};
    srv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv.NumDescriptors = 8;
    srv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(g_device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&g_srvHeap)))) return false;

    D3D12_DESCRIPTOR_HEAP_DESC rtv{};
    rtv.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv.NumDescriptors = count;
    rtv.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(g_device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&g_rtvHeap)))) return false;

    const UINT rtvSize = g_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = g_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    g_frames.resize(count);
    for (UINT i = 0; i < count; ++i)
    {
        g_frames[i].rtv = h;
        sc->GetBuffer(i, IID_PPV_ARGS(&g_frames[i].backbuffer));
        if (g_frames[i].backbuffer) g_device->CreateRenderTargetView(g_frames[i].backbuffer, nullptr, h);
        g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frames[i].allocator));
        h.ptr += rtvSize;
    }
    if (FAILED(g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frames[0].allocator,
                                           nullptr, IID_PPV_ARGS(&g_cmdList)))) return false;
    g_cmdList->Close();

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = nullptr;                       // do not litter the game folder
    ApplyUnrealStyle();
    ImGui_ImplWin32_Init(g_hwnd);
    ImGui_ImplDX12_Init(g_device, static_cast<int>(count), DXGI_FORMAT_R8G8B8A8_UNORM, g_srvHeap,
                        g_srvHeap->GetCPUDescriptorHandleForHeapStart(),
                        g_srvHeap->GetGPUDescriptorHandleForHeapStart());

    g_wndProcOrig = reinterpret_cast<WNDPROC>(SetWindowLongPtr(g_hwnd, GWLP_WNDPROC,
                                              reinterpret_cast<LONG_PTR>(WndProc)));
    Log("[render] ImGui up: hwnd=%p buffers=%u", g_hwnd, count);
    return true;
}

HRESULT __stdcall Hook_Present(IDXGISwapChain3* sc, UINT interval, UINT flags)
{
    if (!g_imguiReady)
    {
        if (!InitImGui(sc)) return g_presentOrig(sc, interval, flags);
        g_imguiReady = true;
    }
    if (!g_uiVisible || !g_cmdQueue) return g_presentOrig(sc, interval, flags);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    DrawEditorUI();
    ImGui::Render();

    const UINT idx = sc->GetCurrentBackBufferIndex();
    if (idx < g_frames.size() && g_frames[idx].allocator && g_frames[idx].backbuffer)
    {
        FrameCtx& f = g_frames[idx];
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = f.backbuffer;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        f.allocator->Reset();
        g_cmdList->Reset(f.allocator, nullptr);
        g_cmdList->ResourceBarrier(1, &b);
        g_cmdList->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
        g_cmdList->SetDescriptorHeaps(1, &g_srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_cmdList);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        g_cmdList->ResourceBarrier(1, &b);
        g_cmdList->Close();
        ID3D12CommandList* lists[] = { g_cmdList };
        g_cmdQueue->ExecuteCommandLists(1, lists);
    }
    return g_presentOrig(sc, interval, flags);
}

// Build a throwaway device + swapchain purely to read the vtable layout. Nothing the game owns is
// touched; the objects are released immediately.
void* g_resizeSlot = nullptr;
void* g_resize1Slot = nullptr;
bool GrabVTables(void** presentSlot, void** execSlot)
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"SpecEditorProbe";
    RegisterClassExW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 8, 8, nullptr, nullptr,
                             wc.hInstance, nullptr);
    if (!wnd) { UnregisterClassW(wc.lpszClassName, wc.hInstance); return false; }

    ID3D12Device* dev = nullptr;
    IDXGIFactory4* factory = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    IDXGISwapChain1* sc1 = nullptr;
    bool ok = false;

    if (SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev))) &&
        SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        D3D12_COMMAND_QUEUE_DESC qd{};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
        {
            DXGI_SWAP_CHAIN_DESC1 sd{};
            sd.BufferCount = 2;
            sd.Width = sd.Height = 8;
            sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd.SampleDesc.Count = 1;
            if (SUCCEEDED(factory->CreateSwapChainForHwnd(queue, wnd, &sd, nullptr, nullptr, &sc1)))
            {
                *presentSlot = (*reinterpret_cast<void***>(sc1))[8];    // IDXGISwapChain::Present
                g_resizeSlot = (*reinterpret_cast<void***>(sc1))[13];   // IDXGISwapChain::ResizeBuffers
                IDXGISwapChain3* sc3 = nullptr;
                if (SUCCEEDED(sc1->QueryInterface(IID_PPV_ARGS(&sc3))) && sc3)
                {
                    g_resize1Slot = (*reinterpret_cast<void***>(sc3))[39];  // IDXGISwapChain3::ResizeBuffers1
                    sc3->Release();
                }
                *execSlot    = (*reinterpret_cast<void***>(queue))[10]; // ExecuteCommandLists
                ok = true;
            }
        }
    }
    if (sc1) sc1->Release();
    if (queue) queue->Release();
    if (factory) factory->Release();
    if (dev) dev->Release();
    DestroyWindow(wnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}  // namespace

bool InstallRenderHook()
{
    void* present = nullptr;
    void* exec = nullptr;
    if (!GrabVTables(&present, &exec)) { Log("[render] could not read D3D12 vtables"); return false; }

    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) return false;
    if (MH_CreateHook(present, &Hook_Present, reinterpret_cast<void**>(&g_presentOrig)) != MH_OK ||
        MH_EnableHook(present) != MH_OK) { Log("[render] Present hook failed"); return false; }
    if (MH_CreateHook(exec, &Hook_ExecuteCommandLists, reinterpret_cast<void**>(&g_execOrig)) != MH_OK ||
        MH_EnableHook(exec) != MH_OK) { Log("[render] ExecuteCommandLists hook failed"); return false; }

    if (g_resizeSlot && MH_CreateHook(g_resizeSlot, &Hook_ResizeBuffers, reinterpret_cast<void**>(&g_resizeOrig)) == MH_OK)
        MH_EnableHook(g_resizeSlot);
    else Log("[render] ResizeBuffers hook failed -- resizing the window may crash");
    if (g_resize1Slot && MH_CreateHook(g_resize1Slot, &Hook_ResizeBuffers1, reinterpret_cast<void**>(&g_resize1Orig)) == MH_OK)
        MH_EnableHook(g_resize1Slot);
    Log("[render] hooks installed (Present=%p ExecuteCommandLists=%p ResizeBuffers=%p ResizeBuffers1=%p)", present, exec,
        g_resizeSlot, g_resize1Slot);

    // Cursor ownership (see EditorOwnsInput). Failing any of these only costs cursor behaviour, not the UI.
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    struct { const char* name; void* hook; void** orig; } cur[] = {
        { "ClipCursor",   reinterpret_cast<void*>(&Hook_ClipCursor),   reinterpret_cast<void**>(&g_clipOrig) },
        { "SetCursorPos", reinterpret_cast<void*>(&Hook_SetCursorPos), reinterpret_cast<void**>(&g_setPosOrig) },
        { "ShowCursor",   reinterpret_cast<void*>(&Hook_ShowCursor),   reinterpret_cast<void**>(&g_showOrig) },
        { "SetCursor",    reinterpret_cast<void*>(&Hook_SetCursor),    reinterpret_cast<void**>(&g_setCurOrig) },
    };
    int ok = 0;
    for (auto& h : cur)
    {
        void* target = u32 ? reinterpret_cast<void*>(GetProcAddress(u32, h.name)) : nullptr;
        if (target && MH_CreateHook(target, h.hook, h.orig) == MH_OK && MH_EnableHook(target) == MH_OK) ++ok;
        else Log("[render] cursor hook %s failed", h.name);
    }
    Log("[render] cursor ownership hooks: %d/4 (RMB hands the mouse to the game)", ok);
    return true;
}

}  // namespace se
