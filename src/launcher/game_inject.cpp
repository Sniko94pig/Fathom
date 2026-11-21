#include "launcher/game_inject.h"
#include "launcher/games.h"
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace fs = std::filesystem;

namespace gameinject {

namespace {

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

DWORD FindGame(const Spec& s) {
    DWORD pid = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (_wcsicmp(pe.szExeFile, s.exe) == 0) {
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

std::wstring SteamFolder(const Spec& s) {
    for (const std::wstring& lib : launcher::SteamLibraries()) {
        fs::path dir = fs::path(lib) / L"steamapps" / L"common" / s.steamDir;
        std::error_code ec;
        if (fs::exists(dir / s.exe, ec)) return dir.wstring();
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

bool Is64Bit(HANDLE proc) {
    USHORT process = 0, machine = 0;
    if (IsWow64Process2(proc, &process, &machine)) return process == IMAGE_FILE_MACHINE_UNKNOWN;
    return true;
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

fs::path Sn1Saves(const inject::Paths& p) { return fs::path(p.win64) / L"SNAppData" / L"SavedGames"; }

fs::path SotfSaves(const inject::Paths&) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return {};
    return fs::path(buf) / L"AppData" / L"LocalLow" / L"Endnight" / L"SonsOfTheForest" / L"Saves";
}

fs::path UkSaves(const inject::Paths& p) { return fs::path(p.win64) / L"Saves"; }

}

const Spec kSubnautica{L"Subnautica.exe", L"Subnautica", L"fathom_sn1.dll", L"fathom_sn1.run.dll", "Subnautica", 264710, L"Subnautica", Sn1Saves};
const Spec kSonsOfTheForest{L"SonsOfTheForest.exe", L"Sons Of The Forest", L"fathom_sotf.dll", L"fathom_sotf.run.dll", "Sons of the Forest", 1326470,
                            L"SonsOfTheForest", SotfSaves};
const Spec kUltrakill{L"ULTRAKILL.exe", L"ULTRAKILL", L"fathom_ultrakill.dll", L"fathom_ultrakill.run.dll", "ULTRAKILL", 1229490, L"ULTRAKILL", UkSaves};

inject::Paths Locate(const Spec& s) {
    inject::Paths p;
    p.win64 = SteamFolder(s);
    if (p.win64.empty()) {
        DWORD pid = FindGame(s);
        if (pid) {
            std::wstring exe = ProcessPath(pid);
            if (!exe.empty()) p.win64 = fs::path(exe).parent_path().wstring();
        }
    }
    p.bundled = (fs::path(ExeFolder()) / s.dll).wstring();
    if (p.win64.empty()) return p;
    fs::path folder = fs::path(p.win64) / L"fathom";
    p.folder = folder.wstring();
    p.dll = (folder / s.dll).wstring();
    p.run = (folder / s.run).wstring();
    p.settings = (folder / L"settings.json").wstring();
    p.log = (folder / L"fathom.log").wstring();
    p.found = true;
    return p;
}

inject::GameState State(const Spec& s) {
    DWORD pid = FindGame(s);
    if (!pid) return inject::GameState::NotRunning;
    return RemoteModule(pid, s.run) ? inject::GameState::Injected : inject::GameState::Running;
}

std::string Inject(const Spec& s, const inject::Paths& p, bool& ok, const std::string& openKey) {
    ok = false;
    if (!p.found) return std::string(s.name) + " was not found";
    DWORD pid = FindGame(s);
    if (!pid) return "Start the game first";
    if (RemoteModule(pid, s.run)) return "The menu is already running in the game";

    std::error_code ec;
    fs::create_directories(p.folder, ec);
    {
        std::wstring home = ExeFolder();
        std::ofstream h(fs::path(p.folder) / L"home.txt", std::ios::binary | std::ios::trunc);
        h.write((const char*)home.data(), (std::streamsize)(home.size() * sizeof(wchar_t)));
    }
    if (fs::exists(p.bundled, ec) && !SameFile(p.bundled, p.dll)) fs::copy_file(p.bundled, p.dll, fs::copy_options::overwrite_existing, ec);
    if (!fs::exists(p.dll, ec)) return Narrow(s.dll) + " is missing";
    fs::copy_file(p.dll, p.run, fs::copy_options::overwrite_existing, ec);
    if (ec) return "Could not prepare the menu file";

    HMODULE local = LoadLibraryExW(p.run.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!local) return "The menu file could not be read";
    FARPROC entry = GetProcAddress(local, "FathomInject");
    uintptr_t rva = entry ? (uintptr_t)entry - (uintptr_t)local : 0;
    FreeLibrary(local);
    if (!entry) return "The menu file is outdated";

    HANDLE proc = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE |
                                  PROCESS_VM_READ,
                              FALSE, pid);
    if (!proc) return "No access to the game process";
    if (!Is64Bit(proc)) {
        CloseHandle(proc);
        return "Start the 64-bit " + std::string(s.name) + " from Steam";
    }
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
            uintptr_t base = RemoteModule(pid, s.run);
            if (!base) {
                result = "The game refused to load the menu";
            } else if (!RunRemote(proc, (void*)(base + rva), nullptr, code, 5000) || code == 0) {
                result = "The menu did not start";
            } else {
                ok = true;
                result = code == 2 ? "The menu was already running" : "Injected. Press " + openKey + " in the game";
            }
        }
    }
    if (mem) VirtualFreeEx(proc, mem, 0, MEM_RELEASE);
    CloseHandle(proc);
    return result;
}

void StartGame(const Spec& s) {
    std::wstring url = L"steam://rungameid/" + std::to_wstring(s.appId);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

std::string BackupSaves(const Spec& s, const inject::Paths& p) {
    try {
        if (!p.found) return {};
        fs::path src = s.saves(p);
        if (src.empty() || !fs::exists(src)) return {};
        fs::path root = fs::path(ExeFolder()) / L"save-backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf_s(stamp, L"%s_%04d-%02d-%02d_%02d-%02d-%02d", s.backupName, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path dst = root / stamp / src.filename();
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        return Narrow((root / stamp).wstring());
    } catch (...) {
        return {};
    }
}

}
