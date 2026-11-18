#include <windows.h>
#include <cstdint>
#include <string>

namespace {

using U = uintptr_t;

HMODULE real = nullptr;
HMODULE self = nullptr;
HMODULE menu = nullptr;
DWORD gameThread = 0;
volatile LONG reloadVk = VK_INSERT;
volatile LONG menuActive = 0;
std::wstring dir;

struct LoaderApi {
    DWORD gameThread;
    const wchar_t* dir;
    void (*setReloadKey)(int vk);
    void (*unloaded)();
};

void SetReloadKey(int vk) { InterlockedExchange(&reloadVk, vk); }
void Unloaded() { InterlockedExchange(&menuActive, 0); }

LoaderApi api{0, nullptr, &SetReloadKey, &Unloaded};

FARPROC Real(const char* name) { return real ? GetProcAddress(real, name) : nullptr; }

bool IsGame() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring p(path);
    return p.find(L"Subnautica2-Win64-Shipping") != std::wstring::npos;
}

std::wstring Folder() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/")) + L"\\fathom";
}

bool LoadMenu() {
    std::wstring src = dir + L"\\fathom.dll";
    std::wstring run = dir + L"\\fathom.run.dll";
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
    if (GetModuleHandleW(run.c_str())) return false;
    CopyFileW(src.c_str(), run.c_str(), FALSE);
    menu = LoadLibraryW(run.c_str());
    if (!menu) return false;
    auto start = (bool (*)(const LoaderApi*))GetProcAddress(menu, "FathomStart");
    if (!start) {
        FreeLibrary(menu);
        menu = nullptr;
        return false;
    }
    InterlockedExchange(&menuActive, 1);
    api.gameThread = gameThread;
    api.dir = dir.c_str();
    if (!start(&api)) {
        InterlockedExchange(&menuActive, 0);
        return false;
    }
    return true;
}

bool GameFocused() {
    HWND w = GetForegroundWindow();
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

DWORD WINAPI LoaderThread(void*) {
    LoadMenu();
    bool was = false;
    for (;;) {
        Sleep(50);
        int vk = (int)reloadVk;
        bool down = vk && (GetAsyncKeyState(vk) & 0x8000) != 0;
        if (down && !was && !menuActive && GameFocused()) {
            Sleep(300);
            LoadMenu();
        }
        was = down;
    }
}

}

extern "C" {

#define FWD(n, ...) static auto f = (U(WINAPI*)(__VA_ARGS__))Real(n); if (!f) return 0;

U WINAPI Proxy_GetFileVersionInfoA(U a, U b, U c, U d) { FWD("GetFileVersionInfoA", U, U, U, U) return f(a, b, c, d); }
U WINAPI Proxy_GetFileVersionInfoW(U a, U b, U c, U d) { FWD("GetFileVersionInfoW", U, U, U, U) return f(a, b, c, d); }
U WINAPI Proxy_GetFileVersionInfoByHandle(U a, U b, U c, U d) { FWD("GetFileVersionInfoByHandle", U, U, U, U) return f(a, b, c, d); }
U WINAPI Proxy_GetFileVersionInfoExA(U a, U b, U c, U d, U e) { FWD("GetFileVersionInfoExA", U, U, U, U, U) return f(a, b, c, d, e); }
U WINAPI Proxy_GetFileVersionInfoExW(U a, U b, U c, U d, U e) { FWD("GetFileVersionInfoExW", U, U, U, U, U) return f(a, b, c, d, e); }
U WINAPI Proxy_GetFileVersionInfoSizeA(U a, U b) { FWD("GetFileVersionInfoSizeA", U, U) return f(a, b); }
U WINAPI Proxy_GetFileVersionInfoSizeW(U a, U b) { FWD("GetFileVersionInfoSizeW", U, U) return f(a, b); }
U WINAPI Proxy_GetFileVersionInfoSizeExA(U a, U b, U c) { FWD("GetFileVersionInfoSizeExA", U, U, U) return f(a, b, c); }
U WINAPI Proxy_GetFileVersionInfoSizeExW(U a, U b, U c) { FWD("GetFileVersionInfoSizeExW", U, U, U) return f(a, b, c); }
U WINAPI Proxy_VerFindFileA(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerFindFileA", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Proxy_VerFindFileW(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerFindFileW", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Proxy_VerInstallFileA(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerInstallFileA", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Proxy_VerInstallFileW(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerInstallFileW", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Proxy_VerLanguageNameA(U a, U b, U c) { FWD("VerLanguageNameA", U, U, U) return f(a, b, c); }
U WINAPI Proxy_VerLanguageNameW(U a, U b, U c) { FWD("VerLanguageNameW", U, U, U) return f(a, b, c); }
U WINAPI Proxy_VerQueryValueA(U a, U b, U c, U d) { FWD("VerQueryValueA", U, U, U, U) return f(a, b, c, d); }
U WINAPI Proxy_VerQueryValueW(U a, U b, U c, U d) { FWD("VerQueryValueW", U, U, U, U) return f(a, b, c, d); }

}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    self = inst;
    DisableThreadLibraryCalls(inst);
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    real = LoadLibraryW((std::wstring(sys) + L"\\version.dll").c_str());
    if (!IsGame()) return TRUE;
    gameThread = GetCurrentThreadId();
    dir = Folder();
    HANDLE t = CreateThread(nullptr, 0, LoaderThread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
