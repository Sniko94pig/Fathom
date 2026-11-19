#include "ingame/overlay.h"
#include "ingame/log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "ui/atlas.h"
#include "MinHook.h"
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <mutex>
#include <vector>

namespace overlay {

namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn oPresent = nullptr;
Present1Fn oPresent1 = nullptr;
ResizeFn oResize = nullptr;
void* tPresent = nullptr;
void* tPresent1 = nullptr;
void* tResize = nullptr;

Callbacks cbs;
std::atomic<int> inflight{0};
std::atomic<bool> stopping{false};
std::atomic<bool> running{false};
std::atomic<bool> drawn{false};
thread_local bool inPresent = false;

struct Guard {
    Guard() { ++inflight; }
    ~Guard() { --inflight; }
};

HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
ID3D11Device* device = nullptr;
ID3D11DeviceContext* context = nullptr;
ID3D11RenderTargetView* rtv = nullptr;
IDXGISwapChain* lastChain = nullptr;
UINT bufW = 0, bufH = 0;
bool ready = false;
bool failed = false;
LARGE_INTEGER freq{}, last{};
ID3D11ShaderResourceView* logoSrv = nullptr;
bool logoTried = false;

enum class EvType { Key, Char, Button, Wheel };
struct Ev {
    EvType type;
    int a;
    bool down;
    float f;
};
std::mutex evLock;
std::vector<Ev> events;
std::atomic<float> rawDx{0}, rawDy{0};
std::atomic<bool> menuCapture{false};
float vx = -1, vy = -1;
bool rawSeen = false;

struct KeyMap {
    int vk;
    ImGuiKey key;
};

const KeyMap kKeys[] = {
    {VK_TAB, ImGuiKey_Tab}, {VK_LEFT, ImGuiKey_LeftArrow}, {VK_RIGHT, ImGuiKey_RightArrow}, {VK_UP, ImGuiKey_UpArrow},
    {VK_DOWN, ImGuiKey_DownArrow}, {VK_PRIOR, ImGuiKey_PageUp}, {VK_NEXT, ImGuiKey_PageDown}, {VK_HOME, ImGuiKey_Home},
    {VK_END, ImGuiKey_End}, {VK_INSERT, ImGuiKey_Insert}, {VK_DELETE, ImGuiKey_Delete}, {VK_BACK, ImGuiKey_Backspace},
    {VK_SPACE, ImGuiKey_Space}, {VK_RETURN, ImGuiKey_Enter}, {VK_ESCAPE, ImGuiKey_Escape}, {VK_OEM_7, ImGuiKey_Apostrophe},
    {VK_OEM_COMMA, ImGuiKey_Comma}, {VK_OEM_MINUS, ImGuiKey_Minus}, {VK_OEM_PERIOD, ImGuiKey_Period},
    {VK_OEM_2, ImGuiKey_Slash}, {VK_OEM_1, ImGuiKey_Semicolon}, {VK_OEM_PLUS, ImGuiKey_Equal},
    {VK_OEM_4, ImGuiKey_LeftBracket}, {VK_OEM_5, ImGuiKey_Backslash}, {VK_OEM_6, ImGuiKey_RightBracket},
    {VK_OEM_3, ImGuiKey_GraveAccent}, {VK_CAPITAL, ImGuiKey_CapsLock}, {VK_SCROLL, ImGuiKey_ScrollLock},
    {VK_NUMLOCK, ImGuiKey_NumLock}, {VK_SNAPSHOT, ImGuiKey_PrintScreen}, {VK_PAUSE, ImGuiKey_Pause},
    {VK_NUMPAD0, ImGuiKey_Keypad0}, {VK_NUMPAD1, ImGuiKey_Keypad1}, {VK_NUMPAD2, ImGuiKey_Keypad2},
    {VK_NUMPAD3, ImGuiKey_Keypad3}, {VK_NUMPAD4, ImGuiKey_Keypad4}, {VK_NUMPAD5, ImGuiKey_Keypad5},
    {VK_NUMPAD6, ImGuiKey_Keypad6}, {VK_NUMPAD7, ImGuiKey_Keypad7}, {VK_NUMPAD8, ImGuiKey_Keypad8},
    {VK_NUMPAD9, ImGuiKey_Keypad9}, {VK_DECIMAL, ImGuiKey_KeypadDecimal}, {VK_DIVIDE, ImGuiKey_KeypadDivide},
    {VK_MULTIPLY, ImGuiKey_KeypadMultiply}, {VK_SUBTRACT, ImGuiKey_KeypadSubtract}, {VK_ADD, ImGuiKey_KeypadAdd},
    {VK_LSHIFT, ImGuiKey_LeftShift}, {VK_RSHIFT, ImGuiKey_RightShift}, {VK_LCONTROL, ImGuiKey_LeftCtrl},
    {VK_RCONTROL, ImGuiKey_RightCtrl}, {VK_LMENU, ImGuiKey_LeftAlt}, {VK_RMENU, ImGuiKey_RightAlt},
    {VK_LWIN, ImGuiKey_LeftSuper}, {VK_RWIN, ImGuiKey_RightSuper}, {VK_APPS, ImGuiKey_Menu},
    {VK_XBUTTON1, ImGuiKey_MouseX1}, {VK_XBUTTON2, ImGuiKey_MouseX2}, {VK_MBUTTON, ImGuiKey_MouseMiddle},
};

ImGuiKey ToKey(int vk) {
    if (vk >= '0' && vk <= '9') return (ImGuiKey)(ImGuiKey_0 + (vk - '0'));
    if (vk >= 'A' && vk <= 'Z') return (ImGuiKey)(ImGuiKey_A + (vk - 'A'));
    if (vk >= VK_F1 && vk <= VK_F12) return (ImGuiKey)(ImGuiKey_F1 + (vk - VK_F1));
    for (const KeyMap& k : kKeys)
        if (k.vk == vk) return k.key;
    return ImGuiKey_None;
}

int Resolve(WPARAM w, LPARAM l) {
    int vk = (int)w;
    bool ext = (l & 0x01000000) != 0;
    if (vk == VK_SHIFT) return (int)MapVirtualKeyW((UINT)((l >> 16) & 0xFF), MAPVK_VSC_TO_VK_EX);
    if (vk == VK_CONTROL) return ext ? VK_RCONTROL : VK_LCONTROL;
    if (vk == VK_MENU) return ext ? VK_RMENU : VK_LMENU;
    return vk;
}

void Push(const Ev& e) {
    std::lock_guard<std::mutex> g(evLock);
    if (events.size() < 512) events.push_back(e);
}

bool WantsInput() { return cbs.wantsInput && cbs.wantsInput(); }
bool WantsKeyboard() { return cbs.wantsKeyboard && cbs.wantsKeyboard(); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Guard guard;
    bool want = WantsInput() && !stopping;
    menuCapture = want;
    bool keys = want && WantsKeyboard();
    bool input = false;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        int vk = Resolve(w, l);
        Push({EvType::Key, vk, down, 0});
        input = down && (keys || vk == VK_ESCAPE);
        if (vk == VK_F4 && msg == WM_SYSKEYDOWN) input = false;
        break;
    }
    case WM_CHAR:
        Push({EvType::Char, (int)w, true, 0});
        input = keys;
        break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        Push({EvType::Button, 0, true, 0});
        input = true;
        break;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONDBLCLK:
        Push({EvType::Button, 1, true, 0});
        input = true;
        break;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONDBLCLK:
        Push({EvType::Button, 2, true, 0});
        input = true;
        break;
    case WM_LBUTTONUP:
        Push({EvType::Button, 0, false, 0});
        break;
    case WM_RBUTTONUP:
        Push({EvType::Button, 1, false, 0});
        break;
    case WM_MBUTTONUP:
        Push({EvType::Button, 2, false, 0});
        break;
    case WM_MOUSEWHEEL:
        Push({EvType::Wheel, 0, true, (float)GET_WHEEL_DELTA_WPARAM(w) / WHEEL_DELTA});
        input = true;
        break;
    case WM_MOUSEMOVE:
        input = true;
        break;
    case WM_INPUT: {
        RAWINPUT ri;
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1 &&
            ri.header.dwType == RIM_TYPEMOUSE) {
            rawSeen = true;
            if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                rawDx.fetch_add((float)ri.data.mouse.lLastX);
                rawDy.fetch_add((float)ri.data.mouse.lLastY);
            }
            input = true;
        } else if (ri.header.dwType == RIM_TYPEKEYBOARD) {
            input = keys && !(ri.data.keyboard.Flags & RI_KEY_BREAK);
        }
        if (want && input) return DefWindowProcW(h, msg, w, l);
        break;
    }
    case WM_SETCURSOR:
        if (want) input = true;
        break;
    }
    if (want && input) return msg == WM_SETCURSOR ? TRUE : 0;
    return CallWindowProcW(oWndProc, h, msg, w, l);
}

void FeedInput() {
    ImGuiIO& io = ImGui::GetIO();
    RECT rc{};
    GetClientRect(hwnd, &rc);
    float cw = (float)std::max<LONG>(1, rc.right - rc.left), ch = (float)std::max<LONG>(1, rc.bottom - rc.top);
    float dw = bufW ? (float)bufW : cw, dh = bufH ? (float)bufH : ch;
    io.DisplaySize = ImVec2(dw, dh);
    float sx = dw / cw, sy = dh / ch;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 1.0f / 60.0f;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60.0f;
    last = now;

    std::vector<Ev> evs;
    {
        std::lock_guard<std::mutex> g(evLock);
        evs.swap(events);
    }
    for (const Ev& e : evs) {
        switch (e.type) {
        case EvType::Key: {
            ImGuiKey k = ToKey(e.a);
            if (k != ImGuiKey_None) io.AddKeyEvent(k, e.down);
            io.AddKeyEvent(ImGuiMod_Ctrl, (GetKeyState(VK_CONTROL) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Shift, (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            io.AddKeyEvent(ImGuiMod_Alt, (GetKeyState(VK_MENU) & 0x8000) != 0);
            break;
        }
        case EvType::Char:
            if (e.a > 0 && e.a < 0x10000) io.AddInputCharacterUTF16((unsigned short)e.a);
            break;
        case EvType::Button:
            io.AddMouseButtonEvent(e.a, e.down);
            break;
        case EvType::Wheel:
            io.AddMouseWheelEvent(0, e.f);
            break;
        }
    }

    bool want = WantsInput();
    CURSORINFO ci{sizeof(ci)};
    GetCursorInfo(&ci);
    bool osCursor = (ci.flags & CURSOR_SHOWING) != 0;
    float dx = rawDx.exchange(0), dy = rawDy.exchange(0);
    if (osCursor || !rawSeen) {
        POINT p;
        if (GetCursorPos(&p) && ScreenToClient(hwnd, &p)) {
            vx = (float)p.x * sx;
            vy = (float)p.y * sy;
        }
    } else {
        if (vx < 0) {
            vx = io.DisplaySize.x * 0.5f;
            vy = io.DisplaySize.y * 0.5f;
        }
        if (want) {
            vx = std::clamp(vx + dx * sx, 0.0f, io.DisplaySize.x - 1);
            vy = std::clamp(vy + dy * sy, 0.0f, io.DisplaySize.y - 1);
        }
    }
    io.AddMousePosEvent(want ? vx : -FLT_MAX, want ? vy : -FLT_MAX);
    io.MouseDrawCursor = want && !osCursor;
}

DXGI_FORMAT ViewFormat(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

void ReleaseTarget() {
    if (rtv) rtv->Release();
    rtv = nullptr;
}

bool CreateTarget(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = ViewFormat(td.Format);
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr = device->CreateRenderTargetView(bb, &rd, &rtv);
    if (FAILED(hr)) hr = device->CreateRenderTargetView(bb, nullptr, &rtv);
    bufW = td.Width;
    bufH = td.Height;
    bb->Release();
    return SUCCEEDED(hr) && rtv;
}

ID3D11ShaderResourceView* MakeTexture(const uint8_t* rgba, int w, int h) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA s{rgba, (UINT)w * 4, 0};
    ID3D11Texture2D* tex = nullptr;
    if (FAILED(device->CreateTexture2D(&d, &s, &tex)) || !tex) return nullptr;
    ID3D11ShaderResourceView* srv = nullptr;
    device->CreateShaderResourceView(tex, nullptr, &srv);
    tex->Release();
    return srv;
}

bool Init(IDXGISwapChain* sc) {
    if (FAILED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&device)) || !device) {
        LOG("the game is not using DirectX 11, the menu cannot be shown");
        failed = true;
        return false;
    }
    device->GetImmediateContext(&context);
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    hwnd = desc.OutputWindow;
    if (!ImGui_ImplDX11_Init(device, context)) {
        LOG("menu renderer failed to start");
        failed = true;
        return false;
    }
    QueryPerformanceFrequency(&freq);
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    if (cbs.window) cbs.window(hwnd);
    LOG("overlay ready: %ux%u, format %d, window %p", desc.BufferDesc.Width, desc.BufferDesc.Height, (int)desc.BufferDesc.Format, hwnd);
    ready = true;
    return true;
}

void Render(IDXGISwapChain* sc) {
    if (failed || inPresent) return;
    inPresent = true;
    if (!ready && !Init(sc)) {
        inPresent = false;
        return;
    }
    if (sc != lastChain) {
        ReleaseTarget();
        lastChain = sc;
    }
    if (!rtv && !CreateTarget(sc)) {
        inPresent = false;
        return;
    }
    if (!logoTried) {
        logoTried = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (ui::LogoPixels(px, w, h)) logoSrv = MakeTexture(px.data(), w, h);
        if (logoSrv && cbs.logoReady) cbs.logoReady((uint64_t)logoSrv);
    }
    FeedInput();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    try {
        if (cbs.frame) cbs.frame();
    } catch (...) {
        static int reported = 0;
        if (reported++ < 5) LOG("error while drawing the menu");
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd && dd->TotalVtxCount > 0) {
        ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
        ID3D11DepthStencilView* oldDsv = nullptr;
        context->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
        context->OMSetRenderTargets(1, &rtv, nullptr);
        ImGui_ImplDX11_RenderDrawData(dd);
        context->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
        for (ID3D11RenderTargetView* v : oldRtv)
            if (v) v->Release();
        if (oldDsv) oldDsv->Release();
    }
    drawn = true;
    inPresent = false;
}

HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    Guard guard;
    if (!stopping && !(flags & DXGI_PRESENT_TEST)) Render(sc);
    return oPresent(sc, sync, flags);
}

HRESULT __stdcall hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    Guard guard;
    if (!stopping && !(flags & DXGI_PRESENT_TEST)) Render(sc);
    return oPresent1(sc, sync, flags, p);
}

HRESULT __stdcall hkResize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    Guard guard;
    ReleaseTarget();
    return oResize(sc, count, w, h, fmt, flags);
}

bool FindTargets() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FathomSn1Probe";
    RegisterClassExW(&wc);
    HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    if (!w) return false;
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 1;
    sd.BufferDesc.Width = 64;
    sd.BufferDesc.Height = 64;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = w;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    IDXGISwapChain* sc = nullptr;
    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* ctx = nullptr;
    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    if (FAILED(hr))
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &fl, &ctx);
    bool ok = SUCCEEDED(hr) && sc;
    if (ok) {
        void** vt = *(void***)sc;
        tPresent = vt[8];
        tResize = vt[13];
        IDXGISwapChain1* sc1 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain1), (void**)&sc1)) && sc1) {
            tPresent1 = (*(void***)sc1)[22];
            sc1->Release();
        }
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

}

bool Start(Callbacks cb) {
    cbs = std::move(cb);
    if (!FindTargets()) {
        LOG("could not find the DirectX 11 functions");
        return false;
    }
    if (MH_CreateHook(tPresent, (void*)&hkPresent, (void**)&oPresent) != MH_OK ||
        MH_CreateHook(tResize, (void*)&hkResize, (void**)&oResize) != MH_OK) {
        LOG("render hooks could not be created");
        return false;
    }
    if (tPresent1 && MH_CreateHook(tPresent1, (void*)&hkPresent1, (void**)&oPresent1) != MH_OK) tPresent1 = nullptr;
    MH_EnableHook(tPresent);
    MH_EnableHook(tResize);
    if (tPresent1) MH_EnableHook(tPresent1);
    running = true;
    LOG("render hooks installed");
    return true;
}

void Stop() {
    stopping = true;
    if (hwnd && oWndProc) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    if (tPresent) MH_DisableHook(tPresent);
    if (tPresent1) MH_DisableHook(tPresent1);
    if (tResize) MH_DisableHook(tResize);
    for (int i = 0; i < 200 && inflight.load() > 0; ++i) Sleep(10);
    Sleep(100);
    if (ready) {
        ImGui_ImplDX11_Shutdown();
        ReleaseTarget();
        if (logoSrv) logoSrv->Release();
        logoSrv = nullptr;
        if (context) context->Release();
        if (device) device->Release();
        context = nullptr;
        device = nullptr;
        ready = false;
    }
    running = false;
}

bool Rendering() { return drawn.load(); }

int VirtualKeyFor(int imguiKey) {
    for (int vk = 1; vk < 256; ++vk)
        if (ToKey(vk) == (ImGuiKey)imguiKey) return vk;
    return 0;
}

}
