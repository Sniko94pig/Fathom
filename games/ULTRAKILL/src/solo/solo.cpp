#include <windows.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

using U = uintptr_t;

HMODULE self = nullptr;
HMODULE real = nullptr;

const int kPollMs = 250;
const int kMaxPolls = 4800;
const int kStablePolls = 12;
const int kMinAssemblies = 50;

FARPROC Real(const char* name) { return real ? GetProcAddress(real, name) : nullptr; }

std::wstring ModulePath(HMODULE m) {
    std::wstring p(MAX_PATH, L'\0');
    for (int i = 0; i < 8; ++i) {
        DWORD n = GetModuleFileNameW(m, p.data(), (DWORD)p.size());
        if (n == 0) return L"";
        if (n < p.size()) {
            p.resize(n);
            return p;
        }
        p.resize(p.size() * 2);
    }
    return L"";
}

std::wstring Parent(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? L"" : p.substr(0, i);
}

std::wstring Leaf(const std::wstring& p) {
    size_t i = p.find_last_of(L"\\/");
    return i == std::wstring::npos ? p : p.substr(i + 1);
}

bool IsGame() { return _wcsicmp(Leaf(ModulePath(nullptr)).c_str(), L"ULTRAKILL.exe") == 0; }

std::wstring DataFolder() {
    std::wstring here = Parent(ModulePath(self));
    if (here.empty()) return L"";
    if (_wcsicmp(Leaf(here).c_str(), L"fathom") == 0) return here;
    return here + L"\\fathom";
}

bool Payload(const void** data, DWORD* size) {
    HRSRC r = FindResourceW(self, MAKEINTRESOURCEW(1), MAKEINTRESOURCEW(10));
    HGLOBAL g = r ? LoadResource(self, r) : nullptr;
    *data = g ? LockResource(g) : nullptr;
    *size = r ? SizeofResource(self, r) : 0;
    return *data && *size;
}

bool SameFile(const std::wstring& path, const void* data, DWORD size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER len{};
    bool same = GetFileSizeEx(h, &len) && len.QuadPart == (LONGLONG)size;
    if (same) {
        std::vector<char> buf(size);
        DWORD got = 0;
        same = ReadFile(h, buf.data(), size, &got, nullptr) && got == size && std::memcmp(buf.data(), data, size) == 0;
    }
    CloseHandle(h);
    return same;
}

bool WriteMenu(const std::wstring& path, const void* data, DWORD size) {
    if (SameFile(path, data, size)) return true;
    std::wstring tmp = path + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD put = 0;
    bool ok = WriteFile(h, data, size, &put, nullptr) && put == size;
    CloseHandle(h);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}

HMODULE Mono() {
    HMODULE m = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
    if (!m) m = GetModuleHandleW(L"mono-2.0-sgen.dll");
    if (!m) m = GetModuleHandleW(L"mono.dll");
    return m;
}

void CountOne(void*, void* n) { ++*(int*)n; }

bool ScriptsLoaded() {
    using RootDomain = void* (*)();
    using Foreach = void (*)(void (*)(void*, void*), void*);
    int last = -1, stable = 0;
    for (int i = 0; i < kMaxPolls; ++i) {
        Sleep(kPollMs);
        HMODULE m = Mono();
        auto root = m ? (RootDomain)GetProcAddress(m, "mono_get_root_domain") : nullptr;
        auto each = m ? (Foreach)GetProcAddress(m, "mono_assembly_foreach") : nullptr;
        if (!root || !each || !root()) {
            last = -1;
            stable = 0;
            continue;
        }
        int n = 0;
        each(&CountOne, &n);
        if (n != last) {
            last = n;
            stable = 0;
            continue;
        }
        if (n >= kMinAssemblies && ++stable >= kStablePolls) return true;
    }
    return false;
}

DWORD WINAPI Loader(void*) {
    std::wstring data = DataFolder();
    if (data.empty()) return 0;
    CreateDirectoryW(data.c_str(), nullptr);
    std::wstring menu = data + L"\\fathom_ultrakill.dll";
    const void* bytes = nullptr;
    DWORD size = 0;
    bool have = Payload(&bytes, &size) && WriteMenu(menu, bytes, size);
    if (!have && GetFileAttributesW(menu.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    if (!ScriptsLoaded()) return 0;
    HMODULE m = LoadLibraryW(menu.c_str());
    auto inject = m ? (DWORD(WINAPI*)(LPVOID))GetProcAddress(m, "FathomInject") : nullptr;
    if (!inject) {
        if (m) FreeLibrary(m);
        return 0;
    }
    inject(nullptr);
    return 1;
}

}

extern "C" {

#define FWD(n, ...)                                  \
    static auto f = (U(WINAPI*)(__VA_ARGS__))Real(n); \
    if (!f) return 0;

U WINAPI Solo_GetFileVersionInfoA(U a, U b, U c, U d) { FWD("GetFileVersionInfoA", U, U, U, U) return f(a, b, c, d); }
U WINAPI Solo_GetFileVersionInfoW(U a, U b, U c, U d) { FWD("GetFileVersionInfoW", U, U, U, U) return f(a, b, c, d); }
U WINAPI Solo_GetFileVersionInfoByHandle(U a, U b, U c, U d) { FWD("GetFileVersionInfoByHandle", U, U, U, U) return f(a, b, c, d); }
U WINAPI Solo_GetFileVersionInfoExA(U a, U b, U c, U d, U e) { FWD("GetFileVersionInfoExA", U, U, U, U, U) return f(a, b, c, d, e); }
U WINAPI Solo_GetFileVersionInfoExW(U a, U b, U c, U d, U e) { FWD("GetFileVersionInfoExW", U, U, U, U, U) return f(a, b, c, d, e); }
U WINAPI Solo_GetFileVersionInfoSizeA(U a, U b) { FWD("GetFileVersionInfoSizeA", U, U) return f(a, b); }
U WINAPI Solo_GetFileVersionInfoSizeW(U a, U b) { FWD("GetFileVersionInfoSizeW", U, U) return f(a, b); }
U WINAPI Solo_GetFileVersionInfoSizeExA(U a, U b, U c) { FWD("GetFileVersionInfoSizeExA", U, U, U) return f(a, b, c); }
U WINAPI Solo_GetFileVersionInfoSizeExW(U a, U b, U c) { FWD("GetFileVersionInfoSizeExW", U, U, U) return f(a, b, c); }
U WINAPI Solo_VerFindFileA(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerFindFileA", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Solo_VerFindFileW(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerFindFileW", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Solo_VerInstallFileA(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerInstallFileA", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Solo_VerInstallFileW(U a, U b, U c, U d, U e, U g, U h, U i) { FWD("VerInstallFileW", U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i); }
U WINAPI Solo_VerLanguageNameA(U a, U b, U c) { FWD("VerLanguageNameA", U, U, U) return f(a, b, c); }
U WINAPI Solo_VerLanguageNameW(U a, U b, U c) { FWD("VerLanguageNameW", U, U, U) return f(a, b, c); }
U WINAPI Solo_VerQueryValueA(U a, U b, U c, U d) { FWD("VerQueryValueA", U, U, U, U) return f(a, b, c, d); }
U WINAPI Solo_VerQueryValueW(U a, U b, U c, U d) { FWD("VerQueryValueW", U, U, U, U) return f(a, b, c, d); }

}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    self = inst;
    DisableThreadLibraryCalls(inst);
    wchar_t sys[MAX_PATH];
    UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    if (n > 0 && n < MAX_PATH) real = LoadLibraryW((std::wstring(sys, n) + L"\\version.dll").c_str());
    if (!IsGame()) return TRUE;
    HANDLE t = CreateThread(nullptr, 0, Loader, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return TRUE;
}
