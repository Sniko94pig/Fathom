#include "ingame/overlay.h"
#include "ingame/log.h"
#include "imgui.h"
#include "imgui_impl_opengl2.h"
#include "imgui_impl_opengl3.h"
#include "ui/atlas.h"
#include "MinHook.h"
#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cstdlib>
#include <mutex>
#include <vector>

#ifndef GL_UNPACK_ROW_LENGTH
#define GL_UNPACK_ROW_LENGTH 0x0CF2
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE 0x812F
#endif

namespace overlay {

namespace {

using SwapFn = void(__fastcall*)(void*);

SwapFn oSwap = nullptr;
void* tSwap = nullptr;

Callbacks cbs;
std::atomic<int> inflight{0};
std::atomic<bool> stopping{false};
std::atomic<bool> running{false};
std::atomic<bool> drawn{false};
std::atomic<bool> cleaned{false};
thread_local bool inSwap = false;

struct Guard {
    Guard() { ++inflight; }
    ~Guard() { --inflight; }
};

HWND hwnd = nullptr;
WNDPROC oWndProc = nullptr;
bool ready = false;
bool failed = false;
bool modern = false;
LARGE_INTEGER freq{}, last{};
GLuint logoTex = 0;
bool logoTried = false;
std::atomic<float> shownW{0}, shownH{0};

enum class EvType { Key, Char, Button, Wheel };
struct Ev {
    EvType type;
    int a;
    bool down;
    float f;
};
std::mutex evLock;
std::vector<Ev> events;

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
    case WM_SETCURSOR:
        if (want && LOWORD(l) == HTCLIENT) input = true;
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
    io.DisplaySize = ImVec2(cw, ch);
    shownW = cw;
    shownH = ch;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    io.DeltaTime = last.QuadPart ? (float)(now.QuadPart - last.QuadPart) / (float)freq.QuadPart : 1.0f / 60.0f;
    if (io.DeltaTime <= 0) io.DeltaTime = 1.0f / 60.0f;
    if (io.DeltaTime > 0.5f) io.DeltaTime = 0.5f;
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
    POINT p;
    float mx = -FLT_MAX, my = -FLT_MAX;
    if (GetCursorPos(&p) && ScreenToClient(hwnd, &p)) {
        mx = (float)p.x;
        my = (float)p.y;
    }
    io.AddMousePosEvent(want ? mx : -FLT_MAX, want ? my : -FLT_MAX);
    io.MouseDrawCursor = want && !osCursor;
}

HWND FindGameWindow() {
    struct Find {
        DWORD pid;
        HWND found;
    } f{GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            Find* s = (Find*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != s->pid || !IsWindowVisible(w)) return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(w, cls, 64);
            if (wcscmp(cls, L"GLFW30") == 0) {
                s->found = w;
                return FALSE;
            }
            if (!s->found) s->found = w;
            return TRUE;
        },
        (LPARAM)&f);
    return f.found;
}

GLuint Upload(const uint8_t* rgba, int w, int h) {
    GLint lastTex = 0, lastAlign = 4, lastRow = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &lastTex);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &lastAlign);
    glGetIntegerv(GL_UNPACK_ROW_LENGTH, &lastRow);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    glPixelStorei(GL_UNPACK_ALIGNMENT, lastAlign);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, lastRow);
    glBindTexture(GL_TEXTURE_2D, (GLuint)lastTex);
    return tex;
}

bool Init() {
    HDC dc = wglGetCurrentDC();
    hwnd = dc ? WindowFromDC(dc) : nullptr;
    if (!hwnd) hwnd = FindGameWindow();
    if (!hwnd) return false;
    const char* ver = (const char*)glGetString(GL_VERSION);
    int major = ver ? std::atoi(ver) : 0;
    modern = major >= 3;
    bool ok = modern ? ImGui_ImplOpenGL3_Init(nullptr) : ImGui_ImplOpenGL2_Init();
    if (!ok) {
        LOG("menu renderer failed to start, OpenGL %s", ver ? ver : "unknown");
        failed = true;
        return false;
    }
    QueryPerformanceFrequency(&freq);
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    if (cbs.window) cbs.window(hwnd);
    LOG("overlay ready: OpenGL %s, window %p", ver ? ver : "unknown", hwnd);
    ready = true;
    return true;
}

void Cleanup() {
    if (ready) {
        if (cbs.cleanup) {
            try {
                cbs.cleanup();
            } catch (...) {
            }
        }
        if (logoTex) glDeleteTextures(1, &logoTex);
        logoTex = 0;
        if (modern) ImGui_ImplOpenGL3_Shutdown();
        else ImGui_ImplOpenGL2_Shutdown();
        ready = false;
    }
    cleaned = true;
}

void Render() {
    if (failed || inSwap) return;
    inSwap = true;
    if (!ready && !Init()) {
        inSwap = false;
        return;
    }
    if (!logoTried) {
        logoTried = true;
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        if (ui::LogoPixels(px, w, h)) logoTex = Upload(px.data(), w, h);
        if (logoTex && cbs.logoReady) cbs.logoReady((uint64_t)logoTex);
    }
    FeedInput();
    if (modern) ImGui_ImplOpenGL3_NewFrame();
    else ImGui_ImplOpenGL2_NewFrame();
    ImGui::NewFrame();
    try {
        if (cbs.frame) cbs.frame();
    } catch (...) {
        static int reported = 0;
        if (reported++ < 5) LOG("error while drawing the menu");
    }
    ImGui::Render();
    ImDrawData* dd = ImGui::GetDrawData();
    if (dd) {
        if (modern) ImGui_ImplOpenGL3_RenderDrawData(dd);
        else ImGui_ImplOpenGL2_RenderDrawData(dd);
    }
    drawn = true;
    inSwap = false;
}

void __fastcall hkSwap(void* view) {
    Guard guard;
    if (!stopping) Render();
    else if (!cleaned && !inSwap) Cleanup();
    oSwap(view);
}

}

bool Start(void* swapTarget, Callbacks cb) {
    cbs = std::move(cb);
    tSwap = swapTarget;
    if (!tSwap) {
        LOG("could not find the game's frame function");
        return false;
    }
    if (MH_CreateHook(tSwap, (void*)&hkSwap, (void**)&oSwap) != MH_OK || MH_EnableHook(tSwap) != MH_OK) {
        LOG("render hook could not be created");
        return false;
    }
    running = true;
    LOG("render hook installed");
    return true;
}

void Stop() {
    stopping = true;
    for (int i = 0; i < 300 && !cleaned.load(); ++i) Sleep(10);
    if (!cleaned.load()) LOG("the game drew no frame, the menu drawing could not be cleaned up");
    if (hwnd && oWndProc) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    if (tSwap) MH_DisableHook(tSwap);
    for (int i = 0; i < 200 && inflight.load() > 0; ++i) Sleep(10);
    Sleep(100);
    running = false;
}

bool Rendering() { return drawn.load(); }

bool Cleaned() { return cleaned.load(); }

void* Window() { return hwnd; }

void DisplaySize(float& w, float& h) {
    w = shownW.load();
    h = shownH.load();
}

int VirtualKeyFor(int imguiKey) {
    for (int vk = 1; vk < 256; ++vk)
        if (ToKey(vk) == (ImGuiKey)imguiKey) return vk;
    return 0;
}

uint64_t MakeTexture(const uint8_t* rgba, int w, int h) {
    if (!ready || !rgba || w <= 0 || h <= 0) return 0;
    return (uint64_t)Upload(rgba, w, h);
}

void FreeTexture(uint64_t tex) {
    if (!tex || !ready) return;
    GLuint t = (GLuint)tex;
    glDeleteTextures(1, &t);
}

}
