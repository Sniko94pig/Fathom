#include "launcher/inject.h"
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <regex>
#include <vector>

namespace fs = std::filesystem;

namespace inject {

namespace {

const wchar_t* kExe = L"Subnautica2-Win64-Shipping.exe";
const wchar_t* kRun = L"fathom.run.dll";

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Env(const wchar_t* name) {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(name, buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

DWORD FindGame() {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, kExe) == 0) {
            pid = pe.th32ProcessID;
            break;
        }
    CloseHandle(snap);
    return pid;
}

uintptr_t RemoteModule(DWORD pid, const wchar_t* name) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    uintptr_t base = 0;
    MODULEENTRY32W me{sizeof(me)};
    for (BOOL ok = Module32FirstW(snap, &me); ok; ok = Module32NextW(snap, &me))
        if (_wcsicmp(me.szModule, name) == 0) {
            base = (uintptr_t)me.modBaseAddr;
            break;
        }
    CloseHandle(snap);
    return base;
}

std::wstring ProcessPath(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    wchar_t buf[MAX_PATH * 2];
    DWORD n = MAX_PATH * 2;
    std::wstring out;
    if (QueryFullProcessImageNameW(h, 0, buf, &n)) out.assign(buf, n);
    CloseHandle(h);
    return out;
}

std::wstring SteamWin64() {
    HKEY key;
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof(steam);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, (BYTE*)steam, &size);
        RegCloseKey(key);
    }
    std::vector<fs::path> libs;
    if (steam[0]) {
        libs.push_back(steam);
        std::ifstream vdf(fs::path(steam) / L"steamapps" / L"libraryfolders.vdf");
        std::string line;
        std::regex re("\"path\"\\s+\"([^\"]+)\"");
        while (std::getline(vdf, line)) {
            std::smatch m;
            if (std::regex_search(line, m, re)) {
                std::string p = m[1].str();
                std::string clean;
                for (size_t i = 0; i < p.size(); ++i) {
                    if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                    clean.push_back(p[i]);
                }
                libs.push_back(fs::u8path(clean));
            }
        }
    }
    for (const fs::path& lib : libs) {
        fs::path w = lib / L"steamapps" / L"common" / L"Subnautica2" / L"Subnautica2" / L"Binaries" / L"Win64";
        std::error_code ec;
        if (fs::exists(w / kExe, ec)) return w.wstring();
    }
    return {};
}

std::wstring ExeFolder() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return fs::path(path).parent_path().wstring();
}

bool SameFile(const fs::path& a, const fs::path& b) {
    std::error_code ec;
    if (!fs::exists(a, ec) || !fs::exists(b, ec)) return false;
    if (fs::file_size(a, ec) != fs::file_size(b, ec)) return false;
    std::ifstream fa(a, std::ios::binary), fb(b, std::ios::binary);
    std::vector<char> x((std::istreambuf_iterator<char>(fa)), {}), y((std::istreambuf_iterator<char>(fb)), {});
    return x == y;
}

bool RunRemote(HANDLE proc, void* start, void* param, DWORD& code, DWORD wait) {
    HANDLE t = CreateRemoteThread(proc, nullptr, 0, (LPTHREAD_START_ROUTINE)start, param, 0, nullptr);
    if (!t) return false;
    bool done = WaitForSingleObject(t, wait) == WAIT_OBJECT_0;
    code = 0;
    if (done) GetExitCodeThread(t, &code);
    CloseHandle(t);
    return done;
}

}

Paths Locate() {
    Paths p;
    p.win64 = SteamWin64();
    if (p.win64.empty()) {
        DWORD pid = FindGame();
        if (pid) {
            std::wstring exe = ProcessPath(pid);
            if (!exe.empty()) p.win64 = fs::path(exe).parent_path().wstring();
        }
    }
    p.bundled = (fs::path(ExeFolder()) / L"fathom.dll").wstring();
    if (p.win64.empty()) return p;
    fs::path folder = fs::path(p.win64) / L"fathom";
    p.folder = folder.wstring();
    p.dll = (folder / L"fathom.dll").wstring();
    p.run = (folder / kRun).wstring();
    p.settings = (folder / L"settings.json").wstring();
    p.log = (folder / L"fathom.log").wstring();
    p.found = true;
    return p;
}

GameState State(const Paths&) {
    DWORD pid = FindGame();
    if (!pid) return GameState::NotRunning;
    return RemoteModule(pid, kRun) ? GameState::Injected : GameState::Running;
}

std::string Inject(const Paths& p, bool& ok) {
    ok = false;
    if (!p.found) return "Subnautica 2 was not found";
    DWORD pid = FindGame();
    if (!pid) return "Start the game first";
    if (RemoteModule(pid, kRun)) return "The menu is already running in the game";

    std::error_code ec;
    fs::create_directories(p.folder, ec);
    {
        std::wstring home = ExeFolder();
        std::ofstream h(fs::path(p.folder) / L"home.txt", std::ios::binary | std::ios::trunc);
        h.write((const char*)home.data(), (std::streamsize)(home.size() * sizeof(wchar_t)));
    }
    if (fs::exists(p.bundled, ec) && !SameFile(p.bundled, p.dll)) fs::copy_file(p.bundled, p.dll, fs::copy_options::overwrite_existing, ec);
    if (!fs::exists(p.dll, ec)) return "fathom.dll is missing";
    fs::copy_file(p.dll, p.run, fs::copy_options::overwrite_existing, ec);
    if (ec) return "Could not prepare the menu file";

    HMODULE local = LoadLibraryExW(p.run.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return "The menu file could not be read";
    uintptr_t rva = (uintptr_t)GetProcAddress(local, "FathomInject") - (uintptr_t)local;
    bool hasExport = GetProcAddress(local, "FathomInject") != nullptr;
    FreeLibrary(local);
    if (!hasExport) return "The menu file is outdated";

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                  PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return "No access to the game process";
    size_t bytes = (p.run.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(proc, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    std::string result;
    DWORD code = 0;
    if (!mem || !WriteProcessMemory(proc, mem, p.run.c_str(), bytes, nullptr)) {
        result = "Could not write into the game";
    } else {
        void* loadLib = (void*)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
        if (!RunRemote(proc, loadLib, mem, code, 15000)) {
            result = "The game did not respond";
        } else {
            uintptr_t base = RemoteModule(pid, kRun);
            if (!base) {
                result = "The game refused to load the menu";
            } else if (!RunRemote(proc, (void*)(base + rva), nullptr, code, 5000) || code == 0) {
                result = "The menu did not start";
            } else {
                ok = true;
                result = code == 2 ? "The menu was already running" : "Injected. Press Insert in the game";
            }
        }
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return result;
}

void StartGame() { ShellExecuteW(nullptr, L"open", L"steam://rungameid/1962700", nullptr, nullptr, SW_SHOWNORMAL); }

std::string BackupSaves() {
    try {
        fs::path src = fs::path(Env(L"LOCALAPPDATA")) / L"Subnautica2" / L"Saved" / L"SaveGames";
        if (!fs::exists(src)) return {};
        fs::path root = fs::path(ExeFolder()) / L"save-backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf_s(stamp, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path dst = root / stamp / L"SaveGames";
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return Narrow((root / stamp).wstring());
    } catch (...) {
        return {};
    }
}

}
