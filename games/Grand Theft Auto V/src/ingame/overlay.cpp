#include "ingame/overlay.h"
#include "ingame/log.h"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_dx12.h"
#include "ui/atlas.h"
#include "MinHook.h"
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstring>
#include <future>
#include <mutex>
#include <vector>

namespace overlay {

namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(__stdcall*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using Resize1Fn = HRESULT(__stdcall*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
using ExecFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn oPresent = nullptr;
Present1Fn oPresent1 = nullptr;
ResizeFn oResize = nullptr;
Resize1Fn oResize1 = nullptr;
ExecFn oExec = nullptr;
void* tPresent = nullptr;
void* tPresent1 = nullptr;
void* tResize = nullptr;
void* tResize1 = nullptr;
void* tExec = nullptr;

enum class Api { None, Dx11, Dx12 };

Callbacks cbs;
std::atomic<int> inflight{0};
std::atomic<int> execInflight{0};
std::atomic<bool> stopping{false};
std::atomic<bool> running{false};
std::atomic<bool> failed{false};
std::atomic<uint64_t> presents{0};
std::mutex renderLock;
thread_local bool inPresent = false;

struct Guard {
    Guard() { ++inflight; }
    ~Guard() { --inflight; }
};

struct ExecGuard {
    ExecGuard() { ++execInflight; }
    ~ExecGuard() { --execInflight; }
};

Api api = Api::None;
HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
IDXGISwapChain* lastChain = nullptr;
UINT bufW = 0, bufH = 0;
bool ready = false;
LARGE_INTEGER freq{}, last{};

ID3D11Device* device11 = nullptr;
ID3D11DeviceContext* context11 = nullptr;
ID3D11RenderTargetView* rtv11 = nullptr;

ID3D12Device* device12 = nullptr;
std::atomic<ID3D12CommandQueue*> queue{nullptr};
std::atomic<ID3D12CommandQueue*> uploadQueue{nullptr};
std::mutex queueLock;
std::vector<ID3D12CommandQueue*> seenQueues;
int searchFrames = 0;
ID3D12DescriptorHeap* rtvHeap = nullptr;
ID3D12DescriptorHeap* srvHeap = nullptr;
ID3D12GraphicsCommandList* list = nullptr;
ID3D12Fence* fence = nullptr;
HANDLE fenceEvent = nullptr;
UINT64 fenceValue = 0;
DXGI_FORMAT rtvFormat = DXGI_FORMAT_UNKNOWN;
bool backend12 = false;
UINT64 drawCalls = 0;

struct FrameCtx {
    ID3D12CommandAllocator* alloc = nullptr;
    UINT64 fence = 0;
};
std::vector<FrameCtx> frames;

constexpr int kSrvCount = 256;

struct SrvAlloc {
    UINT inc = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    std::vector<int> free;
} srv;

struct Tex {
    ID3D11ShaderResourceView* view = nullptr;
    ID3D12Resource* res = nullptr;
    int slot = -1;
    uint64_t id = 0;
};

struct Retired {
    ID3D12Resource* res;
    int slot;
    UINT64 fence;
};
std::vector<Retired> retired;

Tex logoTex, mapPicTex, atlasTex, mapTex;
bool logoTried = false;
bool mapTried = false;
std::mutex atlasLock;
std::wstring atlasRequest;
std::future<ui::Atlas> atlasJob;
std::mutex mapLock;
std::vector<uint8_t> mapPending;
int mapW = 0, mapH = 0;

std::atomic<bool> shotWanted{false};
std::atomic<uint32_t> shotGen{0};
uint32_t copyGen = 0;
bool shotPending = false;
UINT64 shotFence = 0;
ID3D12Resource* shotBuf = nullptr;
UINT64 shotBufSize = 0;
D3D12_PLACED_SUBRESOURCE_FOOTPRINT shotPrint{};
DXGI_FORMAT shotFormat = DXGI_FORMAT_UNKNOWN;
UINT shotW = 0, shotH = 0;
std::mutex shotLock;
std::vector<uint8_t> shotRgba;
int shotRgbaW = 0, shotRgbaH = 0;
bool shotReady = false;

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
std::atomic<float> lookDx{0}, lookDy{0};
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
    if (stopping) return CallWindowProcW(oWndProc, h, msg, w, l);
    bool want = !stopping && !failed && WantsInput();
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
        RAWINPUT ri{};
        UINT size = sizeof(ri);
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (ri.header.dwType == RIM_TYPEMOUSE) {
                rawSeen = true;
                if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    rawDx.fetch_add((float)ri.data.mouse.lLastX);
                    rawDy.fetch_add((float)ri.data.mouse.lLastY);
                    if (!want) {
                        lookDx.fetch_add((float)ri.data.mouse.lLastX);
                        lookDy.fetch_add((float)ri.data.mouse.lLastY);
                    }
                }
                input = true;
            } else if (ri.header.dwType == RIM_TYPEKEYBOARD) {
                input = keys && !(ri.data.keyboard.Flags & RI_KEY_BREAK);
            }
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

bool ReadPtr(const void* p, void*& out) {
    __try {
        out = *(void* const*)p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WaitFence(UINT64 v, DWORD ms) {
    if (!fence) return false;
    ULONGLONG end = GetTickCount64() + ms;
    while (fence->GetCompletedValue() < v) {
        ULONGLONG now = GetTickCount64();
        if (now >= end || !fenceEvent || FAILED(fence->SetEventOnCompletion(v, fenceEvent))) return false;
        WaitForSingleObject(fenceEvent, (DWORD)(end - now));
    }
    return true;
}

void WaitGpu() {
    ID3D12CommandQueue* q = queue.load();
    if (!q || !fence) return;
    q->Signal(fence, ++fenceValue);
    WaitFence(fenceValue, 2000);
}

float HalfToFloat(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1F, man = h & 0x3FF;
    uint32_t bits;
    if (exp == 0) {
        if (man == 0) {
            bits = sign;
        } else {
            exp = 127 - 15 + 1;
            while (!(man & 0x400)) {
                man <<= 1;
                --exp;
            }
            man &= 0x3FF;
            bits = sign | (exp << 23) | (man << 13);
        }
    } else if (exp == 31) {
        bits = sign | 0x7F800000 | (man << 13);
    } else {
        bits = sign | ((exp + 127 - 15) << 23) | (man << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint8_t LinearTo8(float v) {
    if (!(v > 0.0f)) return 0;
    if (v >= 1.0f) return 255;
    float s = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.0f / 2.4f) - 0.055f;
    return (uint8_t)std::clamp((int)(s * 255.0f + 0.5f), 0, 255);
}

const uint8_t* HalfTable() {
    static std::vector<uint8_t> table;
    if (table.empty()) {
        table.resize(65536);
        for (uint32_t h = 0; h < 65536; ++h) table[h] = LinearTo8(HalfToFloat((uint16_t)h));
    }
    return table.data();
}

bool ConvertShot(const uint8_t* src, UINT pitch, UINT w, UINT h, DXGI_FORMAT f, std::vector<uint8_t>& out) {
    out.assign((size_t)w * h * 4, 255);
    for (UINT y = 0; y < h; ++y) {
        const uint8_t* row = src + (size_t)y * pitch;
        uint8_t* o = out.data() + (size_t)y * w * 4;
        for (UINT x = 0; x < w; ++x, o += 4) {
            switch (f) {
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
            case DXGI_FORMAT_R8G8B8A8_TYPELESS:
                o[0] = row[x * 4];
                o[1] = row[x * 4 + 1];
                o[2] = row[x * 4 + 2];
                break;
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
            case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            case DXGI_FORMAT_B8G8R8X8_UNORM:
                o[0] = row[x * 4 + 2];
                o[1] = row[x * 4 + 1];
                o[2] = row[x * 4];
                break;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_R10G10B10A2_TYPELESS: {
                uint32_t v;
                std::memcpy(&v, row + x * 4, 4);
                o[0] = (uint8_t)(((v >> 0) & 0x3FF) >> 2);
                o[1] = (uint8_t)(((v >> 10) & 0x3FF) >> 2);
                o[2] = (uint8_t)(((v >> 20) & 0x3FF) >> 2);
                break;
            }
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
            case DXGI_FORMAT_R16G16B16A16_TYPELESS: {
                static const uint8_t* lut = HalfTable();
                uint16_t c[3];
                std::memcpy(c, row + x * 8, 6);
                o[0] = lut[c[0]];
                o[1] = lut[c[1]];
                o[2] = lut[c[2]];
                break;
            }
            default:
                return false;
            }
        }
    }
    return true;
}

bool EnsureShotBuffer(ID3D12Resource* bb) {
    D3D12_RESOURCE_DESC d = bb->GetDesc();
    if (shotBuf && shotW == (UINT)d.Width && shotH == d.Height && shotFormat == d.Format) return true;
    if (shotBuf) {
        shotBuf->Release();
        shotBuf = nullptr;
    }
    UINT64 total = 0;
    device12->GetCopyableFootprints(&d, 0, 1, 0, &shotPrint, nullptr, nullptr, &total);
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = total;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device12->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&shotBuf))) ||
        !shotBuf) {
        LOG("picture buffer could not be created");
        shotBuf = nullptr;
        return false;
    }
    shotBufSize = total;
    shotW = (UINT)d.Width;
    shotH = d.Height;
    shotFormat = d.Format;
    LOG("picture buffer ready: %ux%u format %d", shotW, shotH, (int)shotFormat);
    return true;
}

void CollectShot() {
    if (!shotPending || !fence || fence->GetCompletedValue() < shotFence) return;
    shotPending = false;
    if (copyGen != shotGen.load()) return;
    void* p = nullptr;
    D3D12_RANGE r{0, (SIZE_T)shotBufSize};
    if (!shotBuf || FAILED(shotBuf->Map(0, &r, &p)) || !p) return;
    std::vector<uint8_t> rgba;
    bool ok = ConvertShot((const uint8_t*)p + shotPrint.Offset, shotPrint.Footprint.RowPitch, shotW, shotH, shotFormat, rgba);
    D3D12_RANGE none{0, 0};
    shotBuf->Unmap(0, &none);
    if (!ok) {
        static bool told = false;
        if (!told) {
            told = true;
            LOG("pictures are not supported for back buffer format %d", (int)shotFormat);
        }
        return;
    }
    std::lock_guard<std::mutex> g(shotLock);
    shotRgba.swap(rgba);
    shotRgbaW = (int)shotW;
    shotRgbaH = (int)shotH;
    shotReady = true;
}

ID3D11ShaderResourceView* MakeTexture11(const uint8_t* rgba, int w, int h) {
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
    if (FAILED(device11->CreateTexture2D(&d, &s, &tex)) || !tex) return nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    device11->CreateShaderResourceView(tex, nullptr, &view);
    tex->Release();
    return view;
}

ID3D12Resource* MakeTexture12(const uint8_t* rgba, int w, int h, D3D12_CPU_DESCRIPTOR_HANDLE cpu) {
    ID3D12CommandQueue* q = uploadQueue.load();
    if (!q || !device12) return nullptr;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = (UINT64)w;
    desc.Height = (UINT)h;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    ID3D12Resource* tex = nullptr;
    if (FAILED(device12->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                 IID_PPV_ARGS(&tex))) ||
        !tex)
        return nullptr;

    UINT pitch = ((UINT)w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES up{};
    up.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)pitch * (UINT64)h;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* stage = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* done = nullptr;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    void* mapped = nullptr;
    bool ok = false;
    if (ev &&
        SUCCEEDED(device12->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&stage))) &&
        SUCCEEDED(stage->Map(0, nullptr, &mapped)) &&
        SUCCEEDED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) &&
        SUCCEEDED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl))) &&
        SUCCEEDED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done)))) {
        for (int y = 0; y < h; ++y)
            std::memcpy((uint8_t*)mapped + (size_t)y * pitch, rgba + (size_t)y * (size_t)w * 4, (size_t)w * 4);
        stage->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = stage;
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = (UINT)w;
        src.PlacedFootprint.Footprint.Height = (UINT)h;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = pitch;
        dst.pResource = tex;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        cl->ResourceBarrier(1, &b);
        cl->Close();
        ID3D12CommandList* lists[] = {cl};
        q->ExecuteCommandLists(1, lists);
        q->Signal(done, 1);
        done->SetEventOnCompletion(1, ev);
        ok = WaitForSingleObject(ev, 5000) == WAIT_OBJECT_0;
    }
    if (ev) CloseHandle(ev);
    if (done) done->Release();
    if (cl) cl->Release();
    if (alloc) alloc->Release();
    if (stage) stage->Release();
    if (!ok) {
        tex->Release();
        return nullptr;
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Texture2D.MipLevels = 1;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    device12->CreateShaderResourceView(tex, &sd, cpu);
    return tex;
}

bool MakeTex(const uint8_t* rgba, int w, int h, Tex& t) {
    if (!rgba || w <= 0 || h <= 0) return false;
    if (api == Api::Dx11) {
        t.view = MakeTexture11(rgba, w, h);
        t.id = (uint64_t)t.view;
        return t.view != nullptr;
    }
    if (srv.free.empty()) return false;
    int slot = srv.free.back();
    srv.free.pop_back();
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{srv.cpu.ptr + (SIZE_T)slot * srv.inc};
    ID3D12Resource* r = MakeTexture12(rgba, w, h, cpu);
    if (!r) {
        srv.free.push_back(slot);
        return false;
    }
    t.res = r;
    t.slot = slot;
    t.id = (uint64_t)(srv.gpu.ptr + (UINT64)slot * srv.inc);
    return true;
}

void DropTex(Tex& t) {
    if (t.view) t.view->Release();
    if (t.res) retired.push_back({t.res, t.slot, fenceValue});
    t = Tex{};
}

void FreeTex(Tex& t) {
    if (t.view) t.view->Release();
    if (t.res) t.res->Release();
    if (t.slot >= 0) srv.free.push_back(t.slot);
    t = Tex{};
}

void PumpRetired() {
    if (retired.empty() || !fence) return;
    UINT64 done = fence->GetCompletedValue();
    for (auto it = retired.begin(); it != retired.end();) {
        if (done < it->fence) {
            ++it;
            continue;
        }
        it->res->Release();
        srv.free.push_back(it->slot);
        it = retired.erase(it);
    }
}

void LoadStatic() {
    if (!logoTried) {
        logoTried = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (ui::LogoPixels(px, w, h) && MakeTex(px.data(), w, h, logoTex) && cbs.logoReady) cbs.logoReady(logoTex.id);
    }
    if (!mapTried) {
        mapTried = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (!cbs.folder.empty())
            for (const wchar_t* name : {L"\\map.png", L"\\map.jpg", L"\\map.jpeg"})
                if (ui::LoadImageFile(cbs.folder + name, px, w, h)) break;
        if (!px.empty()) {
            bool ok = MakeTex(px.data(), w, h, mapPicTex);
            if (ok && cbs.mapImageReady) cbs.mapImageReady(mapPicTex.id, w, h);
            LOG("map picture %s: %dx%d", ok ? "loaded" : "failed", w, h);
        }
    }
}

void PumpAtlas() {
    {
        std::lock_guard<std::mutex> g(atlasLock);
        if (!atlasRequest.empty() && !atlasJob.valid()) {
            std::wstring folder = atlasRequest;
            atlasRequest.clear();
            atlasJob = std::async(std::launch::async, [folder] { return ui::BuildAtlas(folder, 64); });
        }
    }
    if (!atlasJob.valid() || atlasJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    ui::Atlas a = atlasJob.get();
    if (a.width <= 0 || a.height <= 0 || a.rects.empty() || a.rgba.size() < (size_t)a.width * (size_t)a.height * 4) return;
    Tex t;
    if (!MakeTex(a.rgba.data(), a.width, a.height, t)) {
        LOG("item icons could not be loaded");
        return;
    }
    if (cbs.atlasReady) cbs.atlasReady(a, t.id);
    DropTex(atlasTex);
    atlasTex = t;
    LOG("item icons loaded: %d", (int)a.rects.size());
}

void PumpMap() {
    std::vector<uint8_t> img;
    int w = 0, h = 0;
    {
        std::lock_guard<std::mutex> g(mapLock);
        if (mapPending.empty()) return;
        img.swap(mapPending);
        w = mapW;
        h = mapH;
    }
    if (w <= 0 || h <= 0 || img.size() < (size_t)w * (size_t)h * 4) return;
    Tex t;
    if (!MakeTex(img.data(), w, h, t)) return;
    if (cbs.mapReady) cbs.mapReady(t.id);
    DropTex(mapTex);
    mapTex = t;
}

void DrawMenu() {
    try {
        if (cbs.frame) cbs.frame();
    } catch (...) {
        static int reported = 0;
        if (reported++ < 5) LOG("error while drawing the menu");
    }
}

void AttachWindow() {
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    if (cbs.window) cbs.window(hwnd);
}

bool OwnChain(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || desc.OutputWindow != hwnd) return false;
    IUnknown* mine = api == Api::Dx12 ? (IUnknown*)device12 : (IUnknown*)device11;
    IUnknown* dev = nullptr;
    if (FAILED(sc->GetDevice(api == Api::Dx12 ? __uuidof(ID3D12Device) : __uuidof(ID3D11Device), (void**)&dev)) || !dev)
        return false;
    bool same = dev == mine;
    dev->Release();
    return same;
}

void ReleaseTarget11() {
    if (rtv11) rtv11->Release();
    rtv11 = nullptr;
}

bool CreateTarget11(IDXGISwapChain* sc) {
    ID3D11Texture2D* bb = nullptr;
    if (FAILED(sc->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&bb)) || !bb) return false;
    D3D11_TEXTURE2D_DESC td{};
    bb->GetDesc(&td);
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = ViewFormat(td.Format);
    rd.ViewDimension = td.SampleDesc.Count > 1 ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    HRESULT hr = device11->CreateRenderTargetView(bb, &rd, &rtv11);
    if (FAILED(hr)) hr = device11->CreateRenderTargetView(bb, nullptr, &rtv11);
    bufW = td.Width;
    bufH = td.Height;
    bb->Release();
    return SUCCEEDED(hr) && rtv11;
}

bool Init11(IDXGISwapChain* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    hwnd = desc.OutputWindow;
    device11->GetImmediateContext(&context11);
    if (!hwnd || !context11 || !ImGui_ImplDX11_Init(device11, context11)) {
        LOG("menu renderer failed to start");
        failed = true;
        return false;
    }
    ready = true;
    AttachWindow();
    LOG("overlay ready on DirectX 11: %ux%u, format %d, window %p", desc.BufferDesc.Width, desc.BufferDesc.Height,
        (int)desc.BufferDesc.Format, hwnd);
    return true;
}

void Render11(IDXGISwapChain* sc) {
    if (!ready && !Init11(sc)) return;
    if (sc != lastChain) {
        if (!OwnChain(sc)) return;
        ReleaseTarget11();
        lastChain = sc;
    }
    if (!rtv11 && !CreateTarget11(sc)) return;
    LoadStatic();
    PumpAtlas();
    PumpMap();
    FeedInput();
    ImGui_ImplDX11_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (!dd || dd->TotalVtxCount <= 0) return;
    ID3D11RenderTargetView* oldRtv[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT] = {};
    ID3D11DepthStencilView* oldDsv = nullptr;
    context11->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, &oldDsv);
    context11->OMSetRenderTargets(1, &rtv11, nullptr);
    ImGui_ImplDX11_RenderDrawData(dd);
    context11->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, oldRtv, oldDsv);
    for (ID3D11RenderTargetView* v : oldRtv)
        if (v) v->Release();
    if (oldDsv) oldDsv->Release();
}

void __stdcall hkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    ExecGuard guard;
    if (!queue.load() && q != uploadQueue.load() && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard<std::mutex> g(queueLock);
        if (seenQueues.size() < 32 && std::find(seenQueues.begin(), seenQueues.end(), q) == seenQueues.end()) {
            seenQueues.push_back(q);
            LOG("direct queue seen: %p", q);
        }
    }
    oExec(q, n, lists);
}

bool Prepare12() {
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr;
    if (FAILED(device12->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))) || !q) {
        LOG("could not create the menu command queue");
        return false;
    }
    uploadQueue = q;
    void* target = (*(void***)q)[10];
    if (stopping || MH_CreateHook(target, (void*)&hkExec, (void**)&oExec) != MH_OK) {
        LOG("command queue hook could not be created");
        return false;
    }
    if (MH_EnableHook(target) != MH_OK) {
        MH_RemoveHook(target);
        LOG("command queue hook could not be enabled");
        return false;
    }
    tExec = target;
    return true;
}

ID3D12CommandQueue* ScanChain(IDXGISwapChain* sc, const std::vector<ID3D12CommandQueue*>& qs) {
    const uint8_t* base = (const uint8_t*)sc;
    for (int o = 0; o < 0x1000; o += 8) {
        void* v = nullptr;
        if (!ReadPtr(base + o, v)) break;
        for (ID3D12CommandQueue* q : qs)
            if (v == q) {
                LOG("present queue %p found at swap chain offset 0x%X (%zu queues seen)", q, o, qs.size());
                return q;
            }
    }
    return nullptr;
}

ID3D12CommandQueue* PickQueue(IDXGISwapChain* sc) {
    std::vector<ID3D12CommandQueue*> qs;
    {
        std::lock_guard<std::mutex> g(queueLock);
        qs = seenQueues;
    }
    if (qs.empty()) return nullptr;
    if (ID3D12CommandQueue* q = ScanChain(sc, qs)) return q;
    if (searchFrames >= 120 && qs.size() == 1) {
        LOG("present queue not found in the swap chain, using the only direct queue %p", qs[0]);
        return qs[0];
    }
    return nullptr;
}

bool StartBackend12(DXGI_FORMAT fmt) {
    ImGui_ImplDX12_InitInfo info{};
    info.Device = device12;
    info.CommandQueue = uploadQueue.load();
    info.NumFramesInFlight = (int)frames.size();
    info.RTVFormat = fmt;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = srvHeap;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) {
        int i = 0;
        if (!srv.free.empty()) {
            i = srv.free.back();
            srv.free.pop_back();
        }
        c->ptr = srv.cpu.ptr + (SIZE_T)i * srv.inc;
        g->ptr = srv.gpu.ptr + (UINT64)i * srv.inc;
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) {
        srv.free.push_back((int)((c.ptr - srv.cpu.ptr) / srv.inc));
    };
    if (!ImGui_ImplDX12_Init(&info)) return false;
    rtvFormat = fmt;
    backend12 = true;
    drawCalls = 0;
    return true;
}

bool Init12(IDXGISwapChain3* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc)) || !desc.OutputWindow) return false;
    hwnd = desc.OutputWindow;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kSrvCount;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device12->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)))) return false;
    srv.inc = device12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv.cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srv.gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
    srv.free.clear();
    for (int i = kSrvCount - 1; i >= 0; --i) srv.free.push_back(i);

    D3D12_DESCRIPTOR_HEAP_DESC rd{};
    rd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rd.NumDescriptors = 1;
    if (FAILED(device12->CreateDescriptorHeap(&rd, IID_PPV_ARGS(&rtvHeap)))) return false;

    frames.assign(std::clamp<UINT>(desc.BufferCount, 2u, 8u), FrameCtx{});
    for (FrameCtx& f : frames)
        if (FAILED(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
    if (FAILED(device12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].alloc, nullptr, IID_PPV_ARGS(&list))))
        return false;
    list->Close();
    if (FAILED(device12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!fenceEvent || !StartBackend12(ViewFormat(desc.BufferDesc.Format))) return false;

    ready = true;
    AttachWindow();
    LOG("overlay ready on DirectX 12: %ux%u, %u buffers, format %d, window %p", desc.BufferDesc.Width, desc.BufferDesc.Height,
        desc.BufferCount, (int)desc.BufferDesc.Format, hwnd);
    return true;
}

bool TexturesBusy(ImDrawData* dd) {
    if (!dd->Textures) return false;
    for (ImTextureData* t : *dd->Textures)
        if (t->Status == ImTextureStatus_WantUpdates ||
            (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames >= (int)frames.size()))
            return true;
    return false;
}

void Draw12(IDXGISwapChain* raw, IDXGISwapChain3* sc) {
    if (!queue.load()) {
        ++searchFrames;
        ID3D12CommandQueue* q = PickQueue(raw);
        if (!q) {
            if (searchFrames == 600) {
                std::lock_guard<std::mutex> g(queueLock);
                LOG("present queue not found yet (%zu direct queues seen), the menu stays hidden", seenQueues.size());
            }
            return;
        }
        q->AddRef();
        queue = q;
    }
    if (!ready && !Init12(sc)) {
        LOG("menu renderer failed to start");
        failed = true;
        return;
    }
    if (raw != lastChain) {
        if (!OwnChain(raw)) return;
        lastChain = raw;
    }
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(sc->GetDesc(&desc))) return;
    DXGI_FORMAT fmt = ViewFormat(desc.BufferDesc.Format);
    if (fmt != rtvFormat) {
        WaitGpu();
        if (backend12) ImGui_ImplDX12_Shutdown();
        backend12 = false;
        if (!StartBackend12(fmt)) {
            LOG("menu renderer could not restart for back buffer format %d", (int)fmt);
            failed = true;
            return;
        }
        LOG("back buffer format changed to %d, menu renderer restarted", (int)fmt);
    }
    bufW = desc.BufferDesc.Width;
    bufH = desc.BufferDesc.Height;

    PumpRetired();
    LoadStatic();
    PumpAtlas();
    PumpMap();
    CollectShot();
    FeedInput();
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    DrawMenu();
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    bool menu = dd && dd->TotalVtxCount > 0 && dd->DisplaySize.x > 0 && dd->DisplaySize.y > 0;
    bool shot = shotWanted.load() && !shotPending;
    if (!menu && !shot) return;

    FrameCtx& f = frames[drawCalls % frames.size()];
    if (!WaitFence(f.fence, 250)) return;
    if (menu && TexturesBusy(dd) && !WaitFence(fenceValue, 1000)) {
        if (!shot) return;
        menu = false;
    }
    ID3D12Resource* bb = nullptr;
    if (FAILED(sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb))) || !bb) return;
    if (shot && !EnsureShotBuffer(bb)) {
        shot = false;
        shotWanted = false;
    }
    if (!menu && !shot) {
        bb->Release();
        return;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
    if (menu) {
        D3D12_RENDER_TARGET_VIEW_DESC rd{};
        rd.Format = rtvFormat;
        rd.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device12->CreateRenderTargetView(bb, &rd, rtv);
    }

    f.alloc->Reset();
    list->Reset(f.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = bb;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_PRESENT;
    if (shot) {
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = shotBuf;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = shotPrint;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = bb;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    if (menu) {
        b.Transition.StateBefore = state;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->ResourceBarrier(1, &b);
        state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        list->SetDescriptorHeaps(1, &srvHeap);
        ImGui_ImplDX12_RenderDrawData(dd, list);
    }
    b.Transition.StateBefore = state;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    ID3D12CommandQueue* q = queue.load();
    oExec(q, 1, lists);
    q->Signal(fence, ++fenceValue);
    f.fence = fenceValue;
    ++drawCalls;
    if (shot) {
        shotFence = fenceValue;
        shotPending = true;
        shotWanted = false;
        copyGen = shotGen.load();
    }
    bb->Release();
}

void Render12(IDXGISwapChain* raw) {
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(raw->QueryInterface(IID_PPV_ARGS(&sc))) || !sc) return;
    Draw12(raw, sc);
    sc->Release();
}

bool Detect(IDXGISwapChain* sc) {
    ID3D12Device* d12 = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D12Device), (void**)&d12)) && d12) {
        device12 = d12;
        api = Api::Dx12;
        LOG("the game is drawing with DirectX 12");
        return Prepare12();
    }
    ID3D11Device* d11 = nullptr;
    if (SUCCEEDED(sc->GetDevice(__uuidof(ID3D11Device), (void**)&d11)) && d11) {
        device11 = d11;
        api = Api::Dx11;
        LOG("the game is drawing with DirectX 11");
        return true;
    }
    LOG("the game is not using DirectX 11 or 12, the menu cannot be shown");
    return false;
}

void OnPresent(IDXGISwapChain* sc, UINT flags) {
    if (inPresent || (flags & DXGI_PRESENT_TEST)) return;
    inPresent = true;
    ++presents;
    if (!stopping && !failed) {
        std::unique_lock<std::mutex> lock(renderLock, std::try_to_lock);
        if (lock.owns_lock()) {
            if (api == Api::None && !Detect(sc)) failed = true;
            if (!failed && api == Api::Dx11) Render11(sc);
            if (!failed && api == Api::Dx12) Render12(sc);
        }
    }
    inPresent = false;
}

HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    Guard guard;
    OnPresent(sc, flags);
    return oPresent(sc, sync, flags);
}

HRESULT __stdcall hkPresent1(IDXGISwapChain1* sc, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* p) {
    Guard guard;
    OnPresent(sc, flags);
    return oPresent1(sc, sync, flags, p);
}

void BeforeResize() {
    std::lock_guard<std::mutex> lock(renderLock);
    if (api == Api::Dx11) ReleaseTarget11();
    else if (api == Api::Dx12 && ready) WaitGpu();
}

void AfterResize1(IDXGISwapChain* sc, IUnknown* present) {
    std::lock_guard<std::mutex> lock(renderLock);
    if (api != Api::Dx12 || !ready || sc != lastChain) return;
    ID3D12CommandQueue* next = nullptr;
    if (FAILED(present->QueryInterface(IID_PPV_ARGS(&next))) || !next) return;
    ID3D12CommandQueue* old = queue.exchange(next);
    if (old != next) LOG("present queue changed to %p", next);
    if (old) old->Release();
}

HRESULT __stdcall hkResize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    Guard guard;
    BeforeResize();
    return oResize(sc, count, w, h, fmt, flags);
}

HRESULT __stdcall hkResize1(IDXGISwapChain3* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags, const UINT* mask,
                            IUnknown* const* queues) {
    Guard guard;
    BeforeResize();
    HRESULT r = oResize1(sc, count, w, h, fmt, flags, mask, queues);
    if (SUCCEEDED(r) && queues && queues[0]) AfterResize1(sc, queues[0]);
    return r;
}

bool FindTargets() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FathomGtavProbe";
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
        IDXGISwapChain3* sc3 = nullptr;
        if (SUCCEEDED(sc->QueryInterface(__uuidof(IDXGISwapChain3), (void**)&sc3)) && sc3) {
            tResize1 = (*(void***)sc3)[39];
            sc3->Release();
        }
    }
    if (sc) sc->Release();
    if (ctx) ctx->Release();
    if (dev) dev->Release();
    DestroyWindow(w);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return ok;
}

void Shutdown11() {
    if (ready) ImGui_ImplDX11_Shutdown();
    ReleaseTarget11();
    FreeTex(logoTex);
    FreeTex(mapPicTex);
    FreeTex(atlasTex);
    FreeTex(mapTex);
    if (context11) context11->Release();
    context11 = nullptr;
    if (device11) device11->Release();
    device11 = nullptr;
    ready = false;
}

void Shutdown12() {
    WaitGpu();
    if (backend12) ImGui_ImplDX12_Shutdown();
    backend12 = false;
    FreeTex(logoTex);
    FreeTex(mapPicTex);
    FreeTex(atlasTex);
    FreeTex(mapTex);
    for (Retired& r : retired) r.res->Release();
    retired.clear();
    if (shotBuf) shotBuf->Release();
    shotBuf = nullptr;
    shotPending = false;
    for (FrameCtx& f : frames)
        if (f.alloc) f.alloc->Release();
    frames.clear();
    if (list) list->Release();
    list = nullptr;
    if (fence) fence->Release();
    fence = nullptr;
    if (fenceEvent) CloseHandle(fenceEvent);
    fenceEvent = nullptr;
    if (rtvHeap) rtvHeap->Release();
    rtvHeap = nullptr;
    if (srvHeap) srvHeap->Release();
    srvHeap = nullptr;
    if (ID3D12CommandQueue* q = uploadQueue.exchange(nullptr)) q->Release();
    if (ID3D12CommandQueue* q = queue.exchange(nullptr)) q->Release();
    {
        std::lock_guard<std::mutex> g(queueLock);
        seenQueues.clear();
    }
    if (device12) device12->Release();
    device12 = nullptr;
    ready = false;
}

}

bool Start(Callbacks cb) {
    cbs = std::move(cb);
    QueryPerformanceFrequency(&freq);
    if (!FindTargets()) {
        LOG("could not find the DirectX functions");
        return false;
    }
    if (MH_CreateHook(tPresent, (void*)&hkPresent, (void**)&oPresent) != MH_OK ||
        MH_CreateHook(tResize, (void*)&hkResize, (void**)&oResize) != MH_OK) {
        LOG("render hooks could not be created");
        return false;
    }
    if (tPresent1 && MH_CreateHook(tPresent1, (void*)&hkPresent1, (void**)&oPresent1) != MH_OK) tPresent1 = nullptr;
    if (tResize1 && MH_CreateHook(tResize1, (void*)&hkResize1, (void**)&oResize1) != MH_OK) tResize1 = nullptr;
    MH_EnableHook(tPresent);
    MH_EnableHook(tResize);
    if (tPresent1) MH_EnableHook(tPresent1);
    if (tResize1) MH_EnableHook(tResize1);
    running = true;
    LOG("render hooks installed");
    return true;
}

bool Stop() {
    stopping = true;
    bool safe = true;
    if (tPresent) MH_DisableHook(tPresent);
    if (tPresent1) MH_DisableHook(tPresent1);
    if (tResize) MH_DisableHook(tResize);
    if (tResize1) MH_DisableHook(tResize1);
    for (int i = 0; i < 200 && inflight.load() > 0; ++i) Sleep(10);
    if (hwnd && oWndProc) {
        if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == (LONG_PTR)&WndProc) {
            SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
        } else {
            safe = false;
            LOG("another program hooked the game window after the menu, the menu file stays loaded");
        }
    }
    if (tExec) MH_DisableHook(tExec);
    for (int i = 0; i < 200 && (inflight.load() > 0 || execInflight.load() > 0); ++i) Sleep(10);
    Sleep(100);
    if (atlasJob.valid()) atlasJob.wait();
    if (api == Api::Dx11) Shutdown11();
    if (api == Api::Dx12) Shutdown12();
    running = false;
    if (inflight.load() > 0 || execInflight.load() > 0) safe = false;
    return safe;
}

bool Running() { return running; }

void SetMapImage(std::vector<uint8_t>&& rgba, int width, int height) {
    std::lock_guard<std::mutex> g(mapLock);
    mapPending = std::move(rgba);
    mapW = width;
    mapH = height;
}

void LoadAtlas(const std::wstring& folder) {
    std::lock_guard<std::mutex> g(atlasLock);
    atlasRequest = folder;
}

uint64_t FrameCount() { return presents.load(); }

void RequestShot() {
    {
        std::lock_guard<std::mutex> g(shotLock);
        shotReady = false;
        shotRgba.clear();
    }
    ++shotGen;
    shotWanted = true;
}

bool WindowActive() { return hwnd && GetForegroundWindow() == hwnd; }

bool TakeShot(std::vector<uint8_t>& rgba, int& w, int& h) {
    std::lock_guard<std::mutex> g(shotLock);
    if (!shotReady) return false;
    rgba.swap(shotRgba);
    w = shotRgbaW;
    h = shotRgbaH;
    shotReady = false;
    shotRgba.clear();
    return true;
}

void CancelShot() {
    shotWanted = false;
    ++shotGen;
    std::lock_guard<std::mutex> g(shotLock);
    shotReady = false;
    shotRgba.clear();
}

void TakeLook(float& dx, float& dy) {
    dx = lookDx.exchange(0);
    dy = lookDy.exchange(0);
}

bool GameActive() { return hwnd && GetForegroundWindow() == hwnd && !menuCapture.load() && !WantsInput(); }

int VirtualKeyFor(int imguiKey) {
    for (int vk = 1; vk < 256; ++vk)
        if (ToKey(vk) == (ImGuiKey)imguiKey) return vk;
    return 0;
}

}
