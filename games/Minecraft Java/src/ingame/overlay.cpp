#include "ingame/overlay.h"
#include "ingame/log.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "MinHook.h"
#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <mutex>
#include <vector>

namespace overlay {

namespace {

using SwapFn = BOOL(WINAPI*)(HDC);
using BindFn = void(APIENTRY*)(GLenum, GLuint);
using DpiFn = UINT(WINAPI*)(HWND);

SwapFn oSwap = nullptr;
void* tSwap = nullptr;
BindFn bindFramebuffer = nullptr;
DpiFn dpiForWindow = nullptr;

Callbacks cbs;
std::atomic<int> inflight{0};
std::atomic<bool> stopping{false};
std::atomic<bool> drawn{false};
thread_local bool inSwap = false;

struct Guard {
    Guard() { ++inflight; }
    ~Guard() { --inflight; }
};

HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
HGLRC context = nullptr;
bool ready = false;
bool failed = false;
bool captured = false;
LARGE_INTEGER freq{}, last{};

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

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Guard guard;
    if (stopping) return CallWindowProcW(oWndProc, h, msg, w, l);
    bool want = WantsInput();
    bool input = false;
    switch (msg) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        int vk = Resolve(w, l);
        bool allowed = want || !cbs.keysAllowed || cbs.keysAllowed();
        if (!down || allowed) Push({EvType::Key, vk, down, 0});
        if (allowed && cbs.eatKey && cbs.eatKey(vk)) return 0;
        input = down;
        if (vk == VK_F4 && msg == WM_SYSKEYDOWN) input = false;
        break;
    }
    case WM_CHAR:
        if (want) Push({EvType::Char, (int)w, true, 0});
        input = true;
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
        if (GetRawInputData((HRAWINPUT)l, RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != (UINT)-1) {
            if (ri.header.dwType == RIM_TYPEMOUSE) {
                rawSeen = true;
                if (!(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE)) {
                    rawDx.fetch_add((float)ri.data.mouse.lLastX);
                    rawDy.fetch_add((float)ri.data.mouse.lLastY);
                }
                input = true;
            } else if (ri.header.dwType == RIM_TYPEKEYBOARD) {
                input = !(ri.data.keyboard.Flags & RI_KEY_BREAK);
            }
        }
        if (want && input) return DefWindowProcW(h, msg, w, l);
        break;
    }
    }
    if (want && input) return 0;
    return CallWindowProcW(oWndProc, h, msg, w, l);
}

void FeedInput() {
    ImGuiIO& io = ImGui::GetIO();
    RECT rc{};
    GetClientRect(hwnd, &rc);
    io.DisplaySize = ImVec2((float)std::max<LONG>(1, rc.right - rc.left), (float)std::max<LONG>(1, rc.bottom - rc.top));
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

bool Init(HDC hdc, HGLRC cur) {
    HWND w = WindowFromDC(hdc);
    if (!w || !cur) return false;
    if (!ImGui_ImplOpenGL3_Init("#version 150")) {
        LOG("the menu could not start on OpenGL");
        failed = true;
        return false;
    }
    bindFramebuffer = (BindFn)wglGetProcAddress("glBindFramebuffer");
    hwnd = w;
    context = cur;
    QueryPerformanceFrequency(&freq);
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    char cls[64] = {};
    GetClassNameA(hwnd, cls, sizeof(cls));
    const char* version = (const char*)glGetString(GL_VERSION);
    LOG("menu ready on window %s, OpenGL %s", cls, version ? version : "unknown");
    ready = true;
    return true;
}

void Render(HDC hdc) {
    if (failed || inSwap || stopping) return;
    inSwap = true;
    HGLRC cur = wglGetCurrentContext();
    if (!ready) {
        if (!Init(hdc, cur)) {
            inSwap = false;
            return;
        }
    } else if (cur != context || WindowFromDC(hdc) != hwnd) {
        inSwap = false;
        return;
    }
    bool want = WantsInput();
    if (want != captured) {
        captured = want;
        if (cbs.capture) cbs.capture(want);
    }
    FeedInput();
    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    try {
        if (cbs.frame) cbs.frame();
    } catch (...) {
        static int reported = 0;
        if (reported++ < 5) LOG("error while drawing the menu");
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    bool work = dd && dd->TotalVtxCount > 0;
    if (dd && dd->Textures)
        for (ImTextureData* t : *dd->Textures)
            if (t->Status != ImTextureStatus_OK) work = true;
    if (work) {
        GLint fb = 0;
        glGetIntegerv(0x8CA6, &fb);
        if (fb != 0 && bindFramebuffer) bindFramebuffer(0x8CA9, 0);
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        if (fb != 0 && bindFramebuffer) bindFramebuffer(0x8CA9, (GLuint)fb);
    }
    drawn = true;
    inSwap = false;
}

BOOL WINAPI hkSwap(HDC hdc) {
    Guard guard;
    if (!stopping) Render(hdc);
    return oSwap(hdc);
}

}

bool Start(Callbacks cb) {
    cbs = std::move(cb);
    HMODULE gl = GetModuleHandleW(L"opengl32.dll");
    if (!gl) gl = LoadLibraryW(L"opengl32.dll");
    tSwap = gl ? (void*)GetProcAddress(gl, "wglSwapBuffers") : nullptr;
    if (!tSwap) {
        LOG("could not find wglSwapBuffers");
        return false;
    }
    if (MH_CreateHook(tSwap, (void*)&hkSwap, (void**)&oSwap) != MH_OK || MH_EnableHook(tSwap) != MH_OK) {
        LOG("the render hook could not be installed");
        return false;
    }
    dpiForWindow = (DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow");
    LOG("render hook installed");
    return true;
}

void Stop() {
    if (stopping.exchange(true)) return;
    if (hwnd && oWndProc) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    if (tSwap) MH_DisableHook(tSwap);
    LOG("menu unloaded");
}

bool Rendering() { return drawn.load() && !stopping.load(); }

float Scale() {
    UINT dpi = dpiForWindow && hwnd ? dpiForWindow(hwnd) : 96;
    return std::clamp((float)dpi / 96.0f, 1.0f, 4.0f);
}

int VirtualKeyFor(int imguiKey) {
    for (int vk = 1; vk < 256; ++vk)
        if (ToKey(vk) == (ImGuiKey)imguiKey) return vk;
    return 0;
}

}
