#include "ingame/overlay.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include "imgui.h"
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
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER 0x8D40
#endif
#ifndef GL_DRAW_FRAMEBUFFER
#define GL_DRAW_FRAMEBUFFER 0x8CA9
#endif
#ifndef GL_READ_FRAMEBUFFER
#define GL_READ_FRAMEBUFFER 0x8CA8
#endif
#ifndef GL_DRAW_FRAMEBUFFER_BINDING
#define GL_DRAW_FRAMEBUFFER_BINDING 0x8CA6
#endif
#ifndef GL_READ_FRAMEBUFFER_BINDING
#define GL_READ_FRAMEBUFFER_BINDING 0x8CAA
#endif

namespace overlay {

namespace {

using SwapFn = void(__fastcall*)(void*);
using KeyFn = bool(__fastcall*)(void*, int, bool, bool, double);
using TouchFn = void(__fastcall*)(void*, int, int*, float*, float*, double);
using RightClickFn = void(__fastcall*)(void*, float, float);
using InsertFn = void(__fastcall*)(void*, const char*, int, int);
using PlainFn = void(__fastcall*)(void*);
using FullscreenFn = void(__fastcall*)(void*, bool, bool, bool);
using LockFn = void(__fastcall*)(void*, bool);
using LockedFn = bool(__fastcall*)(void*);
using ViewFn = void* (*)();
using BindFramebufferFn = void(APIENTRY*)(GLenum, GLuint);

SwapFn oSwap = nullptr;
KeyFn oKey = nullptr;
TouchFn oTouch = nullptr;
RightClickFn oRightClick = nullptr;
InsertFn oInsert = nullptr;
PlainFn oDeleteBack = nullptr;
PlainFn oDeleteForward = nullptr;
PlainFn oPaste = nullptr;
FullscreenFn oFullscreen = nullptr;
LockFn oLock = nullptr;
LockedFn getLocked = nullptr;
ViewFn sharedView = nullptr;
BindFramebufferFn bindFramebuffer = nullptr;
void* tSwap = nullptr;

Callbacks cbs;
std::atomic<int> inflight{0};
std::atomic<bool> stopping{false};
std::atomic<bool> running{false};
std::atomic<bool> drawn{false};
std::atomic<bool> cleaned{false};
std::atomic<bool> capture{false};
std::atomic<bool> stuck{false};
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
LARGE_INTEGER freq{}, last{};
GLuint logoTex = 0;
bool logoTried = false;
std::atomic<float> shownW{0}, shownH{0};
bool cursorFreed = false;
bool gameWantsLock = false;
bool captured = false;

enum class EvType { Key, Char, Button, Wheel, Focus };
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

bool WantsInput() { return !stopping.load() && cbs.wantsInput && cbs.wantsInput(); }
bool WantsKeyboard() { return cbs.wantsKeyboard && cbs.wantsKeyboard(); }

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    Guard guard;
    bool want = WantsInput();
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
    case WM_SETFOCUS:
        Push({EvType::Focus, 1, true, 0});
        break;
    case WM_KILLFOCUS:
        Push({EvType::Focus, 0, false, 0});
        break;
    case WM_SETCURSOR:
        if (want && LOWORD(l) == HTCLIENT) {
            SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
            return TRUE;
        }
        break;
    }
    if (want && input) {
        bool isDown = msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDOWN || msg == WM_RBUTTONDBLCLK ||
                      msg == WM_MBUTTONDOWN || msg == WM_MBUTTONDBLCLK;
        if (isDown && !captured) {
            SetCapture(h);
            captured = true;
        }
        return 0;
    }
    if (captured && (msg == WM_LBUTTONUP || msg == WM_RBUTTONUP || msg == WM_MBUTTONUP) && !(GetKeyState(VK_LBUTTON) & 0x8000) &&
        !(GetKeyState(VK_RBUTTON) & 0x8000) && !(GetKeyState(VK_MBUTTON) & 0x8000)) {
        captured = false;
        if (GetCapture() == h) ReleaseCapture();
    }
    return CallWindowProcW(oWndProc, h, msg, w, l);
}

bool __fastcall hkKey(void* self, int key, bool down, bool repeat, double ts) {
    gd::Busy busy;
    if (down && capture.load()) return false;
    return oKey(self, key, down, repeat, ts);
}

void __fastcall hkTouch(void* self, int num, int* ids, float* xs, float* ys, double ts) {
    gd::Busy busy;
    if (capture.load()) return;
    oTouch(self, num, ids, xs, ys, ts);
}

void __fastcall hkRightClick(void* self, float x, float y) {
    gd::Busy busy;
    if (capture.load()) return;
    oRightClick(self, x, y);
}

void __fastcall hkInsert(void* self, const char* text, int len, int key) {
    gd::Busy busy;
    if (capture.load()) return;
    oInsert(self, text, len, key);
}

void __fastcall hkDeleteBack(void* self) {
    gd::Busy busy;
    if (capture.load()) return;
    oDeleteBack(self);
}

void __fastcall hkDeleteForward(void* self) {
    gd::Busy busy;
    if (capture.load()) return;
    oDeleteForward(self);
}

void __fastcall hkPaste(void* self) {
    gd::Busy busy;
    if (capture.load()) return;
    oPaste(self);
}

void __fastcall hkLock(void* view, bool lock) {
    gd::Busy busy;
    gameWantsLock = lock;
    if (cursorFreed) return;
    oLock(view, lock);
}

void UpdateCursorLock(bool want) {
    void* view = sharedView ? sharedView() : nullptr;
    if (!view || !oLock || !getLocked) return;
    if (want && !cursorFreed) {
        gameWantsLock = getLocked(view);
        if (gameWantsLock) oLock(view, false);
        cursorFreed = true;
    } else if (!want && cursorFreed) {
        cursorFreed = false;
        if (gameWantsLock) oLock(view, true);
    }
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
        case EvType::Focus:
            io.AddFocusEvent(e.down);
            break;
        }
    }

    bool want = WantsInput();
    capture = want;
    UpdateCursorLock(want);
    CURSORINFO ci{sizeof(ci)};
    GetCursorInfo(&ci);
    bool osCursor = (ci.flags & CURSOR_SHOWING) != 0 && ci.hCursor != nullptr;
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
            if (wcscmp(cls, L"GLFW30") != 0) return TRUE;
            s->found = w;
            return FALSE;
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

void Fail(const char* why) {
    LOG("%s", why);
    failed = true;
    if (cbs.fatal) cbs.fatal();
}

bool Init() {
    HDC dc = wglGetCurrentDC();
    HWND w = dc ? WindowFromDC(dc) : nullptr;
    if (!w) w = FindGameWindow();
    if (!w) return false;
    const char* ver = (const char*)glGetString(GL_VERSION);
    int major = ver ? std::atoi(ver) : 0;
    if (major < 3) {
        Fail("the game's OpenGL version is too old for the menu");
        return false;
    }
    if (!ImGui_ImplOpenGL3_Init(nullptr)) {
        Fail("menu renderer failed to start");
        return false;
    }
    bindFramebuffer = (BindFramebufferFn)wglGetProcAddress("glBindFramebuffer");
    hwnd = w;
    context = wglGetCurrentContext();
    QueryPerformanceFrequency(&freq);
    oWndProc = (WNDPROC)SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)&WndProc);
    if (cbs.window) cbs.window(hwnd);
    LOG("overlay ready: OpenGL %s, window %p", ver ? ver : "unknown", hwnd);
    ready = true;
    return true;
}

void RestoreWndProc(bool final) {
    if (!hwnd || !oWndProc) return;
    if (GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == (LONG_PTR)&WndProc) SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    else if (final && IsWindow(hwnd)) stuck = true;
    if (captured && GetCapture() == hwnd) ReleaseCapture();
    captured = false;
    hwnd = nullptr;
    oWndProc = nullptr;
}

void Teardown(bool final) {
    if (!ready) return;
    RestoreWndProc(final);
    if (cbs.cleanup) {
        try {
            cbs.cleanup();
        } catch (...) {
        }
    }
    if (logoTex) glDeleteTextures(1, &logoTex);
    logoTex = 0;
    logoTried = false;
    ImGui_ImplOpenGL3_Shutdown();
    context = nullptr;
    ready = false;
}

void Forget() {
    ImGuiIO& io = ImGui::GetIO();
    io.BackendRendererUserData = nullptr;
    io.BackendRendererName = nullptr;
    io.BackendFlags &= ~(ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures);
    for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
    }
    if (cbs.lost) {
        try {
            cbs.lost();
        } catch (...) {
        }
    }
    logoTex = 0;
    logoTried = false;
    if (hwnd && IsWindow(hwnd) && oWndProc && GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == (LONG_PTR)&WndProc)
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, (LONG_PTR)oWndProc);
    hwnd = nullptr;
    oWndProc = nullptr;
    captured = false;
    context = nullptr;
    ready = false;
    LOG("the game made a new window, the menu starts again");
}

void Cleanup() {
    UpdateCursorLock(false);
    capture = false;
    Teardown(true);
    cleaned = true;
}

void Render() {
    if (failed || inSwap) return;
    inSwap = true;
    if (ready && wglGetCurrentContext() != context) Forget();
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
    if (dd) {
        GLint drawFb = 0, readFb = 0;
        if (bindFramebuffer) {
            glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawFb);
            glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &readFb);
            if (drawFb != 0) bindFramebuffer(GL_FRAMEBUFFER, 0);
        }
        ImGui_ImplOpenGL3_RenderDrawData(dd);
        if (bindFramebuffer && (drawFb != 0 || readFb != 0)) {
            bindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)drawFb);
            bindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)readFb);
        }
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

void __fastcall hkFullscreen(void* view, bool full, bool borderless, bool fix) {
    Guard guard;
    if (!inSwap) {
        UpdateCursorLock(false);
        Teardown(false);
    }
    oFullscreen(view, full, borderless, fix);
}

template <class F>
bool HookExport(const char* name, F detour, F* orig) {
    return gd::Hook(gd::CocosFn(name), (void*)detour, (void**)orig, name);
}

}

bool Start(Callbacks cb) {
    cbs = std::move(cb);
    tSwap = gd::cc::SwapBuffersTarget();
    sharedView = (ViewFn)gd::CocosFn("?sharedOpenGLView@CCEGLView@cocos2d@@SAPEAV12@XZ");
    getLocked = (LockedFn)gd::CocosFn("?getCursorLocked@CCEGLView@cocos2d@@QEBA_NXZ");
    if (!tSwap) {
        LOG("could not find the game's frame function");
        return false;
    }
    bool inputOk = true;
    inputOk &= HookExport("?dispatchKeyboardMSG@CCKeyboardDispatcher@cocos2d@@QEAA_NW4enumKeyCodes@2@_N1N@Z", &hkKey, &oKey);
    inputOk &= HookExport("?handleTouchesBegin@CCEGLViewProtocol@cocos2d@@UEAAXHQEAHQEAM1N@Z", &hkTouch, &oTouch);
    inputOk &= HookExport("?handleRightClick@CCEGLViewProtocol@cocos2d@@QEAAXMM@Z", &hkRightClick, &oRightClick);
    inputOk &= HookExport("?dispatchInsertText@CCIMEDispatcher@cocos2d@@QEAAXPEBDHW4enumKeyCodes@2@@Z", &hkInsert, &oInsert);
    inputOk &= HookExport("?dispatchDeleteBackward@CCIMEDispatcher@cocos2d@@QEAAXXZ", &hkDeleteBack, &oDeleteBack);
    inputOk &= HookExport("?dispatchDeleteForward@CCIMEDispatcher@cocos2d@@QEAAXXZ", &hkDeleteForward, &oDeleteForward);
    inputOk &= HookExport("?performSafeClipboardPaste@CCEGLView@cocos2d@@QEAAXXZ", &hkPaste, &oPaste);
    if (!inputOk) LOG("some game input could not be held back while the menu is open");
    if (sharedView && getLocked) HookExport("?toggleLockCursor@CCEGLView@cocos2d@@QEAAX_N@Z", &hkLock, &oLock);
    if (!HookExport("?toggleFullScreen@CCEGLView@cocos2d@@QEAAX_N00@Z", &hkFullscreen, &oFullscreen))
        LOG("switching fullscreen will turn the menu off");
    if (MH_CreateHook(tSwap, (void*)&hkSwap, (void**)&oSwap) != MH_OK) {
        LOG("render hook could not be created");
        return false;
    }
    if (MH_EnableHook(tSwap) != MH_OK) {
        MH_RemoveHook(tSwap);
        LOG("render hook could not be switched on");
        return false;
    }
    running = true;
    LOG("render hook installed");
    return true;
}

bool Stop() {
    stopping = true;
    capture = false;
    if (running.load()) {
        ULONGLONG start = GetTickCount64(), told = 0;
        while (!cleaned.load()) {
            Sleep(10);
            ULONGLONG waited = GetTickCount64() - start;
            if (waited >= 3000 && waited - told >= 10000) {
                told = waited;
                LOG("waiting for the game to draw a frame so the menu can close");
            }
        }
    }
    MH_DisableHook(MH_ALL_HOOKS);
    ULONGLONG start = GetTickCount64(), told = 0;
    while (inflight.load() > 0) {
        Sleep(10);
        ULONGLONG waited = GetTickCount64() - start;
        if (waited >= 3000 && waited - told >= 10000) {
            told = waited;
            LOG("waiting for the game window to finish a message");
        }
    }
    running = false;
    return !stuck.load();
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
