#include "game/overlay.h"
#include "common/dx12_texture.h"
#include "game/log.h"
#include <future>
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "kiero/kiero.h"
#include "minhook/include/MinHook.h"
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <mutex>
#include <vector>

namespace overlay {

namespace {

using PresentFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT);
using ResizeFn = HRESULT(__stdcall*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using ExecFn = void(__stdcall*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

PresentFn oPresent = nullptr;
ResizeFn oResize = nullptr;
ExecFn oExec = nullptr;

Callbacks cbs;
std::atomic<int> inflight{0};
std::atomic<bool> stopping{false};
std::atomic<bool> running{false};

struct Guard {
    Guard() { ++inflight; }
    ~Guard() { --inflight; }
};

HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
ID3D12Device* device = nullptr;
std::atomic<ID3D12CommandQueue*> queue{nullptr};
ID3D12CommandQueue* uploadQueue = nullptr;
std::mutex queueLock;
std::vector<ID3D12CommandQueue*> seenQueues;
int searchFrames = 0;
ID3D12DescriptorHeap* rtvHeap = nullptr;
ID3D12DescriptorHeap* srvHeap = nullptr;
ID3D12GraphicsCommandList* list = nullptr;
ID3D12Fence* fence = nullptr;
HANDLE fenceEvent = nullptr;
UINT64 fenceValue = 0;
DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
bool imguiReady = false;
bool failed = false;
LARGE_INTEGER freq{}, last{};

struct FrameCtx {
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12Resource* buffer = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
    UINT64 fence = 0;
};
std::vector<FrameCtx> frames;

struct SrvAlloc {
    UINT inc = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    std::vector<int> free;
} srv;

std::mutex atlasLock;
std::wstring atlasRequest;
std::future<ui::Atlas> atlasJob;
ID3D12Resource* atlasTex = nullptr;
std::vector<ID3D12Resource*> retired;
int atlasSlot = -1;

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
    if (a.rgba.empty() || !uploadQueue) return;
    if (atlasSlot < 0 && !srv.free.empty()) {
        atlasSlot = srv.free.back();
        srv.free.pop_back();
    }
    if (atlasSlot < 0) return;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{srv.cpu.ptr + (SIZE_T)atlasSlot * srv.inc};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{srv.gpu.ptr + (UINT64)atlasSlot * srv.inc};
    if (atlasTex) {
        if (!srv.free.empty()) {
            atlasSlot = srv.free.back();
            srv.free.pop_back();
            cpu.ptr = srv.cpu.ptr + (SIZE_T)atlasSlot * srv.inc;
            gpu.ptr = srv.gpu.ptr + (UINT64)atlasSlot * srv.inc;
        }
        retired.push_back(atlasTex);
        atlasTex = nullptr;
    }
    atlasTex = dx12::CreateTexture(device, uploadQueue, a.rgba.data(), a.width, a.height, cpu);
    if (!atlasTex) {
        LOG("icon texture upload failed");
        return;
    }
    LOG("icons loaded: %zu", a.rects.size());
    if (cbs.atlasReady) cbs.atlasReady(a, (uint64_t)gpu.ptr);
}

ID3D12Resource* logoTex = nullptr;
bool logoTried = false;
ID3D12Resource* mapImgTex = nullptr;
bool mapImgTried = false;
std::mutex mapLock;
std::vector<uint8_t> mapPending;
int mapW = 0, mapH = 0;
ID3D12Resource* mapTex[2] = {};
int mapSlot[2] = {-1, -1};
int mapCur = -1;
std::vector<std::pair<ID3D12Resource*, int>> mapRetired;
int frameNo = 0;

void PumpMap() {
    ++frameNo;
    for (auto it = mapRetired.begin(); it != mapRetired.end();) {
        if (frameNo - it->second > 8) {
            it->first->Release();
            it = mapRetired.erase(it);
        } else {
            ++it;
        }
    }
    std::vector<uint8_t> img;
    int w, h;
    {
        std::lock_guard<std::mutex> g(mapLock);
        if (mapPending.empty()) return;
        img.swap(mapPending);
        w = mapW;
        h = mapH;
    }
    if (!uploadQueue) return;
    int next = mapCur == 0 ? 1 : 0;
    if (mapSlot[next] < 0) {
        if (srv.free.empty()) return;
        mapSlot[next] = srv.free.back();
        srv.free.pop_back();
    }
    if (mapTex[next]) {
        mapRetired.push_back({mapTex[next], frameNo});
        mapTex[next] = nullptr;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{srv.cpu.ptr + (SIZE_T)mapSlot[next] * srv.inc};
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{srv.gpu.ptr + (UINT64)mapSlot[next] * srv.inc};
    mapTex[next] = dx12::CreateTexture(device, uploadQueue, img.data(), w, h, cpu);
    if (!mapTex[next]) return;
    mapCur = next;
    if (cbs.mapReady) cbs.mapReady((uint64_t)gpu.ptr);
}

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
std::atomic<uint64_t> presents{0};
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
    if (vk == VK_RETURN && ext) return VK_RETURN;
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
        if (vk == VK_F4 && (msg == WM_SYSKEYDOWN)) input = false;
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
                if (!want) {
                    lookDx.fetch_add((float)ri.data.mouse.lLastX);
                    lookDy.fetch_add((float)ri.data.mouse.lLastY);
                }
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
    io.DisplaySize = ImVec2((float)(rc.right - rc.left), (float)(rc.bottom - rc.top));
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
            vx = (float)p.x;
            vy = (float)p.y;
        }
    } else {
        if (vx < 0) {
            vx = io.DisplaySize.x * 0.5f;
            vy = io.DisplaySize.y * 0.5f;
        }
        if (want) {
            vx = std::clamp(vx + dx, 0.0f, io.DisplaySize.x - 1);
            vy = std::clamp(vy + dy, 0.0f, io.DisplaySize.y - 1);
        }
    }
    io.AddMousePosEvent(want ? vx : -FLT_MAX, want ? vy : -FLT_MAX);
    io.MouseDrawCursor = want && !osCursor;
}

void ReleaseTargets() {
    for (FrameCtx& f : frames)
        if (f.buffer) {
            f.buffer->Release();
            f.buffer = nullptr;
        }
}

void WaitGpu() {
    ID3D12CommandQueue* q = queue.load();
    if (!q || !fence) return;
    q->Signal(fence, ++fenceValue);
    if (fence->GetCompletedValue() < fenceValue) {
        fence->SetEventOnCompletion(fenceValue, fenceEvent);
        WaitForSingleObject(fenceEvent, 2000);
    }
}

bool CreateTargets(IDXGISwapChain3* sc) {
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    UINT count = desc.BufferCount;
    if (frames.size() != count) {
        for (FrameCtx& f : frames)
            if (f.alloc) f.alloc->Release();
        frames.assign(count, FrameCtx{});
        if (rtvHeap) rtvHeap->Release();
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hd.NumDescriptors = count;
        if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap)))) return false;
        UINT inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < count; ++i) {
            frames[i].rtv = h;
            h.ptr += inc;
            if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frames[i].alloc))))
                return false;
        }
    }
    for (UINT i = 0; i < count; ++i) {
        if (FAILED(sc->GetBuffer(i, IID_PPV_ARGS(&frames[i].buffer)))) return false;
        device->CreateRenderTargetView(frames[i].buffer, nullptr, frames[i].rtv);
    }
    return true;
}

bool ReadPtr(const void* p, void*& out) {
    __try {
        out = *(void* const*)p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

ID3D12CommandQueue* FindPresentQueue(IDXGISwapChain* sc) {
    std::vector<ID3D12CommandQueue*> qs;
    {
        std::lock_guard<std::mutex> g(queueLock);
        qs = seenQueues;
    }
    if (qs.empty()) return nullptr;
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

bool InitRender(IDXGISwapChain3* sc) {
    if (FAILED(sc->GetDevice(IID_PPV_ARGS(&device)))) return false;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&uploadQueue)))) return false;
    DXGI_SWAP_CHAIN_DESC desc{};
    sc->GetDesc(&desc);
    hwnd = desc.OutputWindow;
    format = desc.BufferDesc.Format;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 64;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap)))) return false;
    srv.inc = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv.cpu = srvHeap->GetCPUDescriptorHandleForHeapStart();
    srv.gpu = srvHeap->GetGPUDescriptorHandleForHeapStart();
    srv.free.clear();
    for (int i = 63; i >= 0; --i) srv.free.push_back(i);

    if (!CreateTargets(sc)) return false;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].alloc, nullptr, IID_PPV_ARGS(&list))))
        return false;
    list->Close();
    if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
    fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    ImGui_ImplDX12_InitInfo info{};
    info.Device = device;
    info.CommandQueue = uploadQueue;
    info.NumFramesInFlight = (int)frames.size();
    info.RTVFormat = format;
    info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    info.SrvDescriptorHeap = srvHeap;
    info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* c, D3D12_GPU_DESCRIPTOR_HANDLE* g) {
        int i = srv.free.empty() ? 0 : srv.free.back();
        if (!srv.free.empty()) srv.free.pop_back();
        c->ptr = srv.cpu.ptr + (SIZE_T)i * srv.inc;
        g->ptr = srv.gpu.ptr + (UINT64)i * srv.inc;
    };
    info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE c, D3D12_GPU_DESCRIPTOR_HANDLE) {
        srv.free.push_back((int)((c.ptr - srv.cpu.ptr) / srv.inc));
    };
    if (!ImGui_ImplDX12_Init(&info)) return false;

    QueryPerformanceFrequency(&freq);
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    LOG("overlay ready: %u buffers, format %d, window %p", desc.BufferCount, (int)format, hwnd);
    imguiReady = true;
    return true;
}

void Render(IDXGISwapChain* raw) {
    if (failed) return;
    IDXGISwapChain3* sc = nullptr;
    if (FAILED(raw->QueryInterface(IID_PPV_ARGS(&sc)))) return;
    if (!queue.load()) {
        ID3D12CommandQueue* q = FindPresentQueue(raw);
        if (!q) {
            if (++searchFrames == 600) {
                std::lock_guard<std::mutex> g(queueLock);
                LOG("present queue not found yet (%zu direct queues seen), overlay stays hidden", seenQueues.size());
            }
            sc->Release();
            return;
        }
        queue = q;
    }
    if (!imguiReady) {
        if (!InitRender(sc)) {
            LOG("overlay init failed");
            failed = true;
            sc->Release();
            return;
        }
    }
    if ((frames.empty() || !frames[0].buffer) && !CreateTargets(sc)) {
        sc->Release();
        return;
    }

    PumpAtlas();
    PumpMap();
    if (!mapImgTried && uploadQueue && !srv.free.empty() && !cbs.folder.empty()) {
        mapImgTried = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        for (const wchar_t* name : {L"\\map.png", L"\\map.jpg", L"\\map.jpeg"})
            if (ui::LoadImageFile(cbs.folder + name, px, w, h)) break;
        if (!px.empty()) {
            int slot = srv.free.back();
            srv.free.pop_back();
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{srv.cpu.ptr + (SIZE_T)slot * srv.inc};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{srv.gpu.ptr + (UINT64)slot * srv.inc};
            mapImgTex = dx12::CreateTexture(device, uploadQueue, px.data(), w, h, cpu);
            if (mapImgTex && cbs.mapImageReady) cbs.mapImageReady((uint64_t)gpu.ptr, w, h);
            LOG("map picture %s: %dx%d", mapImgTex ? "loaded" : "failed", w, h);
        }
    }
    if (!logoTex && !logoTried && uploadQueue && !srv.free.empty()) {
        logoTried = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (ui::LogoPixels(px, w, h)) {
            int slot = srv.free.back();
            srv.free.pop_back();
            D3D12_CPU_DESCRIPTOR_HANDLE cpu{srv.cpu.ptr + (SIZE_T)slot * srv.inc};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{srv.gpu.ptr + (UINT64)slot * srv.inc};
            logoTex = dx12::CreateTexture(device, uploadQueue, px.data(), w, h, cpu);
            if (logoTex && cbs.logoReady) cbs.logoReady((uint64_t)gpu.ptr);
        }
    }
    FeedInput();
    ImGui_ImplDX12_NewFrame();
    ImGui::NewFrame();
    try {
        if (cbs.frame) cbs.frame();
    } catch (...) {
        static int reported = 0;
        if (reported++ < 5) LOG("error while drawing the menu");
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();

    UINT idx = sc->GetCurrentBackBufferIndex();
    sc->Release();
    if (!dd || dd->TotalVtxCount == 0 || idx >= frames.size() || !frames[idx].buffer) return;
    FrameCtx& f = frames[idx];
    if (fence->GetCompletedValue() < f.fence) return;
    f.alloc->Reset();
    list->Reset(f.alloc, nullptr);
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = f.buffer;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    list->OMSetRenderTargets(1, &f.rtv, FALSE, nullptr);
    list->SetDescriptorHeaps(1, &srvHeap);
    ImGui_ImplDX12_RenderDrawData(dd, list);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = {list};
    ID3D12CommandQueue* q = queue.load();
    oExec(q, 1, lists);
    q->Signal(fence, ++fenceValue);
    f.fence = fenceValue;
}

HRESULT __stdcall hkPresent(IDXGISwapChain* sc, UINT sync, UINT flags) {
    Guard guard;
    ++presents;
    if (!stopping) Render(sc);
    return oPresent(sc, sync, flags);
}

HRESULT __stdcall hkResize(IDXGISwapChain* sc, UINT count, UINT w, UINT h, DXGI_FORMAT fmt, UINT flags) {
    Guard guard;
    if (imguiReady) {
        WaitGpu();
        ReleaseTargets();
    }
    HRESULT r = oResize(sc, count, w, h, fmt, flags);
    if (imguiReady && fmt != DXGI_FORMAT_UNKNOWN && fmt != format) {
        LOG("back buffer format changed, overlay disabled");
        failed = true;
    }
    return r;
}

void __stdcall hkExec(ID3D12CommandQueue* q, UINT n, ID3D12CommandList* const* lists) {
    Guard guard;
    if (!queue.load() && q != uploadQueue && q->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        std::lock_guard<std::mutex> g(queueLock);
        if (seenQueues.size() < 32 && std::find(seenQueues.begin(), seenQueues.end(), q) == seenQueues.end()) {
            seenQueues.push_back(q);
            LOG("direct queue seen: %p", q);
        }
    }
    oExec(q, n, lists);
}

}

bool Start(Callbacks cb) {
    cbs = std::move(cb);
    if (kiero::init(kiero::RenderType::D3D12) != kiero::Status::Success) {
        LOG("kiero init failed");
        return false;
    }
    if (kiero::bind(54, (void**)&oExec, (void*)&hkExec) != kiero::Status::Success ||
        kiero::bind(140, (void**)&oPresent, (void*)&hkPresent) != kiero::Status::Success ||
        kiero::bind(145, (void**)&oResize, (void*)&hkResize) != kiero::Status::Success) {
        LOG("kiero bind failed");
        return false;
    }
    running = true;
    LOG("render hooks installed");
    return true;
}

void Stop() {
    stopping = true;
    if (hwnd && oWndProc) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    kiero::unbind(140);
    kiero::unbind(145);
    kiero::unbind(54);
    for (int i = 0; i < 200 && inflight.load() > 0; ++i) Sleep(10);
    Sleep(100);
    if (imguiReady) {
        WaitGpu();
        ImGui_ImplDX12_Shutdown();
        ReleaseTargets();
        for (FrameCtx& f : frames)
            if (f.alloc) f.alloc->Release();
        frames.clear();
        if (list) list->Release();
        if (fence) fence->Release();
        if (fenceEvent) CloseHandle(fenceEvent);
        if (rtvHeap) rtvHeap->Release();
        if (atlasJob.valid()) atlasJob.wait();
        if (atlasTex) atlasTex->Release();
        for (ID3D12Resource* r : retired) r->Release();
        retired.clear();
        if (logoTex) logoTex->Release();
        logoTex = nullptr;
        if (mapImgTex) mapImgTex->Release();
        mapImgTex = nullptr;
        for (auto& r : mapRetired) r.first->Release();
        mapRetired.clear();
        for (ID3D12Resource*& t : mapTex)
            if (t) {
                t->Release();
                t = nullptr;
            }
        if (srvHeap) srvHeap->Release();
        if (uploadQueue) uploadQueue->Release();
        if (device) device->Release();
        imguiReady = false;
    }
    running = false;
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
