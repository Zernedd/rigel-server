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

bool g_uiVisible = true;

namespace {

typedef HRESULT(__stdcall* Present_t)(IDXGISwapChain3*, UINT, UINT);
typedef void(__stdcall* ExecuteCommandLists_t)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

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

    ImVec4* c = s.Colors;
    const ImVec4 bg0(0.055f, 0.055f, 0.055f, 1.00f);   // window
    const ImVec4 bg1(0.086f, 0.086f, 0.086f, 1.00f);   // child / frame
    const ImVec4 bg2(0.133f, 0.133f, 0.133f, 1.00f);   // hovered
    const ImVec4 accent(0.157f, 0.471f, 0.784f, 1.00f);
    const ImVec4 text(0.851f, 0.851f, 0.851f, 1.00f);

    c[ImGuiCol_Text]            = text;
    c[ImGuiCol_TextDisabled]    = ImVec4(0.45f, 0.45f, 0.45f, 1.00f);
    c[ImGuiCol_WindowBg]        = bg0;
    c[ImGuiCol_ChildBg]         = bg0;
    c[ImGuiCol_PopupBg]         = ImVec4(0.07f, 0.07f, 0.07f, 0.98f);
    c[ImGuiCol_Border]          = ImVec4(0.02f, 0.02f, 0.02f, 1.00f);
    c[ImGuiCol_FrameBg]         = bg1;
    c[ImGuiCol_FrameBgHovered]  = bg2;
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.18f, 0.18f, 0.18f, 1.00f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.04f, 0.04f, 0.04f, 1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.06f, 0.06f, 0.06f, 1.00f);
    c[ImGuiCol_MenuBarBg]       = ImVec4(0.07f, 0.07f, 0.07f, 1.00f);
    c[ImGuiCol_ScrollbarBg]     = bg0;
    c[ImGuiCol_ScrollbarGrab]   = ImVec4(0.22f, 0.22f, 0.22f, 1.00f);
    c[ImGuiCol_CheckMark]       = accent;
    c[ImGuiCol_SliderGrab]      = accent;
    c[ImGuiCol_SliderGrabActive]= ImVec4(0.21f, 0.55f, 0.88f, 1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.21f, 0.21f, 0.21f, 1.00f);
    c[ImGuiCol_ButtonActive]    = accent;
    c[ImGuiCol_Header]          = ImVec4(0.14f, 0.14f, 0.14f, 1.00f);
    c[ImGuiCol_HeaderHovered]   = ImVec4(0.19f, 0.19f, 0.19f, 1.00f);
    c[ImGuiCol_HeaderActive]    = accent;
    c[ImGuiCol_Separator]       = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_Tab]             = ImVec4(0.09f, 0.09f, 0.09f, 1.00f);
    c[ImGuiCol_TabHovered]      = ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_TabSelected]     = ImVec4(0.13f, 0.13f, 0.13f, 1.00f);
    c[ImGuiCol_TableHeaderBg]   = ImVec4(0.10f, 0.10f, 0.10f, 1.00f);
    c[ImGuiCol_TableBorderLight]= ImVec4(0.16f, 0.16f, 0.16f, 1.00f);
    c[ImGuiCol_TableRowBgAlt]   = ImVec4(0.075f, 0.075f, 0.075f, 1.00f);
}

LRESULT __stdcall WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == WM_KEYDOWN && wp == VK_INSERT) g_uiVisible = !g_uiVisible;
    if (g_uiVisible && g_imguiReady && ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return 1;
    return CallWindowProc(g_wndProcOrig, hwnd, msg, wp, lp);
}

// The swapchain hands us the device; the command queue has to be learned from the game's own
// ExecuteCommandLists, because ImGui's DX12 backend needs the queue the game actually presents with.
void __stdcall Hook_ExecuteCommandLists(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists)
{
    if (!g_cmdQueue && q && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) g_cmdQueue = q;
    g_execOrig(q, n, lists);
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

    Log("[render] hooks installed (Present=%p ExecuteCommandLists=%p)", present, exec);
    return true;
}

}  // namespace se
