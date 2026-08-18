/* Minimal in-game Dear ImGui proof-of-concept for Grim Dawn.
 *
 * Grim Dawn can run through Direct3D 9 (primarily the x86 executable) or
 * Direct3D 11 (normally the x64 executable), so hooks for both render paths
 * are installed. The first backend that renders a real game frame owns the
 * single ImGui context. Press F10 to show or hide the test window.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d9.h>
#include <d3d11.h>
#include <dxgi.h>

#include <MinHook.h>
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx9.h"
#include "backends/imgui_impl_dx11.h"
#include "imgui_overlay.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

namespace {

enum RenderBackend : LONG {
    BACKEND_NONE = 0,
    BACKEND_DX9 = 9,
    BACKEND_DX11 = 11
};

using D3D9EndSceneFn = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *);
using D3D9ResetFn = HRESULT (STDMETHODCALLTYPE *)(IDirect3DDevice9 *,
    D3DPRESENT_PARAMETERS *);
using DXGIPresentFn = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *, UINT,
    UINT);
using DXGIResizeBuffersFn = HRESULT (STDMETHODCALLTYPE *)(IDXGISwapChain *,
    UINT, UINT, UINT, DXGI_FORMAT, UINT);

D3D9EndSceneFn g_original_d3d9_end_scene = nullptr;
D3D9ResetFn g_original_d3d9_reset = nullptr;
DXGIPresentFn g_original_dxgi_present = nullptr;
DXGIResizeBuffersFn g_original_dxgi_resize_buffers = nullptr;

volatile LONG g_backend = BACKEND_NONE;
HWND g_game_window = nullptr;
WNDPROC g_original_wndproc = nullptr;
bool g_show_window = true;

IDirect3DDevice9 *g_d3d9_device = nullptr;
ID3D11Device *g_d3d11_device = nullptr;
ID3D11DeviceContext *g_d3d11_context = nullptr;
ID3D11RenderTargetView *g_d3d11_rtv = nullptr;

void debug_log(const char *message) {
    OutputDebugStringA("GDEXT IMGUI: ");
    OutputDebugStringA(message);
    OutputDebugStringA("\n");
}

LRESULT CALLBACK overlay_wndproc(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam) {
    if (ImGui::GetCurrentContext()) {
        ImGui_ImplWin32_WndProcHandler(hwnd, message, wparam, lparam);
        ImGuiIO &io = ImGui::GetIO();
        if (g_show_window &&
            ((io.WantCaptureMouse && message >= WM_MOUSEFIRST &&
              message <= WM_MOUSELAST) ||
             (io.WantCaptureKeyboard && (message == WM_KEYDOWN ||
              message == WM_KEYUP || message == WM_SYSKEYDOWN ||
              message == WM_SYSKEYUP || message == WM_CHAR)))) {
            return 1;
        }
    }
    return g_original_wndproc
        ? CallWindowProcW(g_original_wndproc, hwnd, message, wparam, lparam)
        : DefWindowProcW(hwnd, message, wparam, lparam);
}

bool attach_window(HWND hwnd) {
    if (!hwnd)
        return false;
    g_game_window = hwnd;
    SetLastError(0);
    LONG_PTR old_proc = SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(overlay_wndproc));
    if (!old_proc && GetLastError() != 0) {
        g_game_window = nullptr;
        return false;
    }
    g_original_wndproc = reinterpret_cast<WNDPROC>(old_proc);
    return true;
}

void create_imgui_context(void) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    ImGui::StyleColorsDark();
}

void draw_test_window(const char *renderer_name) {
    if (GetAsyncKeyState(VK_F10) & 1)
        g_show_window = !g_show_window;
    if (!g_show_window)
        return;

    ImGui::SetNextWindowSize(ImVec2(390.0f, 170.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(24.0f, 24.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("dpyes-ext in-game UI test", &g_show_window,
        ImGuiWindowFlags_NoCollapse)) {
        ImGui::TextUnformatted("Dear ImGui is rendering inside Grim Dawn.");
        ImGui::Separator();
        ImGui::Text("Renderer: %s", renderer_name);
        ImGui::Text("Architecture: %s", sizeof(void *) == 8 ? "x64" : "x86");
        ImGui::Text("Frame rate: %.1f FPS", ImGui::GetIO().Framerate);
        ImGui::Spacing();
        ImGui::TextDisabled("Press F10 to show or hide this test window.");
    }
    ImGui::End();
}

bool create_d3d11_render_target(IDXGISwapChain *swap_chain) {
    ID3D11Texture2D *back_buffer = nullptr;
    if (FAILED(swap_chain->GetBuffer(0, IID_PPV_ARGS(&back_buffer))))
        return false;
    HRESULT hr = g_d3d11_device->CreateRenderTargetView(back_buffer, nullptr,
        &g_d3d11_rtv);
    back_buffer->Release();
    return SUCCEEDED(hr);
}

bool initialize_d3d11(IDXGISwapChain *swap_chain) {
    if (InterlockedCompareExchange(&g_backend, BACKEND_NONE, BACKEND_NONE) !=
        BACKEND_NONE)
        return g_backend == BACKEND_DX11;

    ID3D11Device *device = nullptr;
    if (FAILED(swap_chain->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    ID3D11DeviceContext *context = nullptr;
    device->GetImmediateContext(&context);

    DXGI_SWAP_CHAIN_DESC desc = {};
    if (FAILED(swap_chain->GetDesc(&desc)) || !desc.OutputWindow) {
        context->Release();
        device->Release();
        return false;
    }

    create_imgui_context();
    if (!attach_window(desc.OutputWindow) ||
        !ImGui_ImplWin32_Init(desc.OutputWindow) ||
        !ImGui_ImplDX11_Init(device, context)) {
        debug_log("failed to initialize Direct3D 11 ImGui backend");
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        context->Release();
        device->Release();
        return false;
    }

    g_d3d11_device = device;
    g_d3d11_context = context;
    create_d3d11_render_target(swap_chain);
    InterlockedExchange(&g_backend, BACKEND_DX11);
    debug_log("Direct3D 11 ImGui backend initialized");
    return true;
}

bool initialize_d3d9(IDirect3DDevice9 *device) {
    if (InterlockedCompareExchange(&g_backend, BACKEND_NONE, BACKEND_NONE) !=
        BACKEND_NONE)
        return g_backend == BACKEND_DX9;

    D3DDEVICE_CREATION_PARAMETERS params = {};
    if (FAILED(device->GetCreationParameters(&params)) || !params.hFocusWindow)
        return false;

    create_imgui_context();
    if (!attach_window(params.hFocusWindow) ||
        !ImGui_ImplWin32_Init(params.hFocusWindow) ||
        !ImGui_ImplDX9_Init(device)) {
        debug_log("failed to initialize Direct3D 9 ImGui backend");
        if (ImGui::GetCurrentContext())
            ImGui::DestroyContext();
        return false;
    }

    device->AddRef();
    g_d3d9_device = device;
    InterlockedExchange(&g_backend, BACKEND_DX9);
    debug_log("Direct3D 9 ImGui backend initialized");
    return true;
}

HRESULT STDMETHODCALLTYPE hook_dxgi_present(IDXGISwapChain *swap_chain,
    UINT sync_interval, UINT flags) {
    if (initialize_d3d11(swap_chain) && g_backend == BACKEND_DX11) {
        if (!g_d3d11_rtv)
            create_d3d11_render_target(swap_chain);
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_test_window("Direct3D 11");
        ImGui::Render();
        if (g_d3d11_rtv) {
            ID3D11RenderTargetView *old_rtv = nullptr;
            ID3D11DepthStencilView *old_dsv = nullptr;
            g_d3d11_context->OMGetRenderTargets(1, &old_rtv, &old_dsv);
            g_d3d11_context->OMSetRenderTargets(1, &g_d3d11_rtv, nullptr);
            ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
            g_d3d11_context->OMSetRenderTargets(1, &old_rtv, old_dsv);
            if (old_dsv)
                old_dsv->Release();
            if (old_rtv)
                old_rtv->Release();
        }
    }
    return g_original_dxgi_present(swap_chain, sync_interval, flags);
}

HRESULT STDMETHODCALLTYPE hook_dxgi_resize_buffers(IDXGISwapChain *swap_chain,
    UINT buffer_count, UINT width, UINT height, DXGI_FORMAT format,
    UINT swap_chain_flags) {
    if (g_backend == BACKEND_DX11 && g_d3d11_rtv) {
        g_d3d11_rtv->Release();
        g_d3d11_rtv = nullptr;
    }
    HRESULT hr = g_original_dxgi_resize_buffers(swap_chain, buffer_count,
        width, height, format, swap_chain_flags);
    if (SUCCEEDED(hr) && g_backend == BACKEND_DX11)
        create_d3d11_render_target(swap_chain);
    return hr;
}

HRESULT STDMETHODCALLTYPE hook_d3d9_end_scene(IDirect3DDevice9 *device) {
    if (initialize_d3d9(device) && g_backend == BACKEND_DX9) {
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        draw_test_window("Direct3D 9");
        ImGui::Render();
        ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
    }
    return g_original_d3d9_end_scene(device);
}

HRESULT STDMETHODCALLTYPE hook_d3d9_reset(IDirect3DDevice9 *device,
    D3DPRESENT_PARAMETERS *params) {
    if (g_backend == BACKEND_DX9)
        ImGui_ImplDX9_InvalidateDeviceObjects();
    HRESULT hr = g_original_d3d9_reset(device, params);
    if (SUCCEEDED(hr) && g_backend == BACKEND_DX9)
        ImGui_ImplDX9_CreateDeviceObjects();
    return hr;
}

LRESULT CALLBACK dummy_wndproc(HWND hwnd, UINT message, WPARAM wparam,
    LPARAM lparam) {
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HWND create_dummy_window(const wchar_t *class_name) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = dummy_wndproc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return nullptr;
    return CreateWindowExW(0, class_name, L"dpyes-ext dummy", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, wc.hInstance, nullptr);
}

bool hook_function(void *target, void *detour, void **original) {
    if (!target)
        return false;
    MH_STATUS status = MH_CreateHook(target, detour, original);
    if (status != MH_OK && status != MH_ERROR_ALREADY_CREATED)
        return false;
    status = MH_EnableHook(target);
    return status == MH_OK || status == MH_ERROR_ENABLED;
}

bool install_d3d11_hooks(void) {
    HWND window = create_dummy_window(L"DPYesExtDummyDX11");
    if (!window)
        return false;

    DXGI_SWAP_CHAIN_DESC desc = {};
    desc.BufferCount = 2;
    desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow = window;
    desc.SampleDesc.Count = 1;
    desc.Windowed = TRUE;
    desc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    IDXGISwapChain *swap_chain = nullptr;
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr,
        D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &desc, &swap_chain, &device, &level, &context);
    if (FAILED(hr)) {
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP,
            nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc, &swap_chain,
            &device, &level, &context);
    }

    bool hooked = false;
    if (SUCCEEDED(hr)) {
        void **vtable = *reinterpret_cast<void ***>(swap_chain);
        bool present = hook_function(vtable[8],
            reinterpret_cast<void *>(hook_dxgi_present),
            reinterpret_cast<void **>(&g_original_dxgi_present));
        bool resize = hook_function(vtable[13],
            reinterpret_cast<void *>(hook_dxgi_resize_buffers),
            reinterpret_cast<void **>(&g_original_dxgi_resize_buffers));
        hooked = present;
        if (present && !resize)
            debug_log("warning: DXGI ResizeBuffers hook was not installed");
    }

    if (context)
        context->Release();
    if (device)
        device->Release();
    if (swap_chain)
        swap_chain->Release();
    DestroyWindow(window);
    UnregisterClassW(L"DPYesExtDummyDX11", GetModuleHandleW(nullptr));
    return hooked;
}

bool install_d3d9_hooks(void) {
    HWND window = create_dummy_window(L"DPYesExtDummyDX9");
    if (!window)
        return false;

    IDirect3D9 *d3d = Direct3DCreate9(D3D_SDK_VERSION);
    IDirect3DDevice9 *device = nullptr;
    if (d3d) {
        D3DPRESENT_PARAMETERS params = {};
        params.Windowed = TRUE;
        params.SwapEffect = D3DSWAPEFFECT_DISCARD;
        params.hDeviceWindow = window;
        params.BackBufferFormat = D3DFMT_UNKNOWN;
        d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window,
            D3DCREATE_SOFTWARE_VERTEXPROCESSING, &params, &device);
    }

    bool hooked = false;
    if (device) {
        void **vtable = *reinterpret_cast<void ***>(device);
        bool reset = hook_function(vtable[16],
            reinterpret_cast<void *>(hook_d3d9_reset),
            reinterpret_cast<void **>(&g_original_d3d9_reset));
        bool end_scene = hook_function(vtable[42],
            reinterpret_cast<void *>(hook_d3d9_end_scene),
            reinterpret_cast<void **>(&g_original_d3d9_end_scene));
        hooked = end_scene;
        if (end_scene && !reset)
            debug_log("warning: Direct3D 9 Reset hook was not installed");
    }

    if (device)
        device->Release();
    if (d3d)
        d3d->Release();
    DestroyWindow(window);
    UnregisterClassW(L"DPYesExtDummyDX9", GetModuleHandleW(nullptr));
    return hooked;
}

} // namespace

extern "C" int imgui_overlay_install_hooks(void) {
    bool dx11 = install_d3d11_hooks();
    bool dx9 = install_d3d9_hooks();
    if (dx11)
        debug_log("Direct3D 11 hooks installed");
    if (dx9)
        debug_log("Direct3D 9 hooks installed");
    if (!dx11 && !dx9)
        debug_log("no Direct3D render hook could be installed");
    return dx11 || dx9;
}
