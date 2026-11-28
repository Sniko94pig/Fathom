#include "core/gtav_features.h"
#include "core/model.h"
#include "game/game.h"
#include "game/shv.h"
#include "ingame/guard.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include "ui/icons.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "imgui.h"
#include "MinHook.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <vector>

namespace fs = std::filesystem;

namespace {

HMODULE self = nullptr;
std::wstring dir;
core::Model model;
std::mutex modelLock;
std::unique_ptr<ui::Menu> menu;
std::atomic<bool> unloadRequested{false};
std::atomic<bool> started{false};
bool greeted = false;

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

fs::file_time_type FileTime(const std::wstring& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
}

fs::path Documents() {
    PWSTR p = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

fs::path HomeFolder() {
    std::ifstream h(fs::path(dir) / L"home.txt", std::ios::binary);
    std::string raw((std::istreambuf_iterator<char>(h)), std::istreambuf_iterator<char>());
    if (raw.empty() || raw.size() % 2 != 0) return {};
    std::wstring home((const wchar_t*)raw.data(), raw.size() / 2);
    std::error_code ec;
    return fs::is_directory(fs::path(home), ec) ? fs::path(home) : fs::path();
}

std::string BackupSaves() {
    try {
        fs::path src = Documents() / L"Rockstar Games" / L"GTAV Enhanced" / L"Profiles";
        if (src.empty() || !fs::exists(src)) {
            LOG("save backup skipped: no Profiles folder");
            return {};
        }
        fs::path home = HomeFolder();
        fs::path root = home.empty() ? fs::path(dir) / L"save-backups" : home / L"save-backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[80];
        swprintf_s(stamp, L"GTAV_%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path dst = root / stamp / L"Profiles";
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        LOG("saves backed up to %s", Utf8(dst.wstring()).c_str());
        return Utf8((root / stamp).wstring());
    } catch (...) {
        LOG("save backup failed");
        return {};
    }
}

void Frame() {
    std::lock_guard<std::mutex> g(modelLock);
    if (!greeted) {
        greeted = true;
        menu->Notify(std::string("Fathom loaded. Press ") + core::KeyName(model.keyOpen) + " to open", ICON_ZAP);
    }
    for (const std::string& n : game::TakeNotes()) menu->Notify(n, ICON_INFO);
    menu->Frame();
    model.menuOpen = menu->open;
    game::SetMenuState(menu->open, menu->focused, model.mapVisible);
    if (model.requestUnload) {
        model.requestUnload = false;
        unloadRequested = true;
    }
}

bool Unload() {
    LOG("unloading");
    {
        std::lock_guard<std::mutex> g(modelLock);
        if (menu) menu->open = false;
    }
    game::SetMenuState(false, false, false);
    bool safe = game::Shutdown();
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::SaveSettings(model);
    }
    bool overlaySafe = overlay::Stop();
    game::Detach(self);
    if (!safe) {
        {
            std::lock_guard<std::mutex> g(modelLock);
            menu.reset();
        }
        if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
        LOG("cheats switch off on the next game update, the menu file stays loaded until the game closes");
        glog::Close();
        return false;
    }
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(150);
    MH_Uninitialize();
    {
        std::lock_guard<std::mutex> g(modelLock);
        menu.reset();
    }
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (!overlaySafe) LOG("the menu file stays loaded but does nothing until the game closes");
    LOG("unloaded");
    glog::Close();
    return overlaySafe;
}

void Leave(const char* why) {
    LOG("%s", why);
    glog::Close();
    FreeLibraryAndExitThread(self, 0);
}

DWORD WINAPI MainThread(void*) {
    glog::Open(dir + L"\\fathom.log");
    LOG("Fathom for Grand Theft Auto V starting, game version %s", guard::GameVersion().c_str());
    guard::Report report = guard::Check();
    if (guard::Blocked(report)) {
        LOG("%s", guard::Describe(report).c_str());
        Leave("Fathom did not start: turn BattlEye off for Story Mode first");
    }
    LOG("%s", guard::Describe(report).c_str());
    std::string why;
    if (!shv::Load(dir, why)) {
        LOG("Script Hook V: %s", why.c_str());
        Leave("Fathom did not start: Script Hook V is needed");
    }
    LOG("Script Hook V ready: %s, game version id %d", Utf8(shv::Path()).c_str(), shv::Version());

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ui::LoadFonts();

    {
        std::lock_guard<std::mutex> g(modelLock);
        gtav::BuildFeatures(model);
        model.settingsPath = Utf8(dir + L"\\settings.json");
        model.backupSaves = [] { return BackupSaves(); };
        model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        model.openUrl = [](const std::string& url) {
            std::wstring w = Wide(url);
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        game::SetFolder(dir);
        game::SetBackup([] { return BackupSaves(); });
        game::Wire(model, modelLock);
        core::LoadSettings(model);
        menu = std::make_unique<ui::Menu>(model);
        menu->SyncFromModel();
        menu->open = false;
    }

    MH_Initialize();
    if (!game::Attach(self)) {
        LOG("could not connect to Script Hook V");
        std::lock_guard<std::mutex> g(modelLock);
        for (core::Feature& f : model.features) {
            f.available = false;
            f.missing = "Could not connect to the game";
        }
    }
    overlay::Callbacks cb;
    cb.frame = [] { Frame(); };
    cb.wantsInput = [] { return menu && menu->open && menu->focused; };
    cb.wantsKeyboard = [] { return true; };
    cb.atlasReady = [](const ui::Atlas& a, uint64_t tex) {
        std::lock_guard<std::mutex> g(modelLock);
        model.iconTexture = tex;
        model.icons.clear();
        for (const auto& [id, r] : a.rects) model.icons[id] = {r.u0, r.v0, r.u1, r.v1};
    };
    cb.mapImageReady = [](uint64_t tex, int w, int h) {
        std::lock_guard<std::mutex> g(modelLock);
        model.mapImageTexture = tex;
        model.mapImageW = w;
        model.mapImageH = h;
    };
    cb.logoReady = [](uint64_t tex) {
        std::lock_guard<std::mutex> g(modelLock);
        model.logoTexture = tex;
    };
    cb.mapReady = [](uint64_t tex) {
        std::lock_guard<std::mutex> g(modelLock);
        model.mapTexture = tex;
    };
    cb.folder = dir;
    if (!overlay::Start(cb)) {
        LOG("overlay failed to start");
        unloadRequested = true;
    }

    std::wstring settingsFile = dir + L"\\settings.json";
    std::string lastText;
    {
        std::lock_guard<std::mutex> g(modelLock);
        lastText = core::SettingsText(model);
    }
    auto knownTime = FileTime(settingsFile);
    ULONGLONG lastSync = 0, lastGuard = GetTickCount64();
    while (!unloadRequested) {
        Sleep(50);
        ULONGLONG now = GetTickCount64();
        std::string leave;
        if (game::WantsUnload(leave)) {
            LOG("%s", leave.c_str());
            unloadRequested = true;
            break;
        }
        if (now - lastGuard >= 2000) {
            lastGuard = now;
            guard::Report r = guard::Check();
            if (guard::Blocked(r)) {
                LOG("%s", guard::Describe(r).c_str());
                LOG("BattlEye showed up, switching everything off and unloading");
                unloadRequested = true;
                break;
            }
        }
        if (now - lastSync < 500) continue;
        lastSync = now;
        std::lock_guard<std::mutex> g(modelLock);
        std::string text = core::SettingsText(model);
        auto t = FileTime(settingsFile);
        if (text != lastText) {
            core::SaveSettings(model);
            lastText = text;
            knownTime = FileTime(settingsFile);
        } else if (t != knownTime && t != fs::file_time_type::min()) {
            core::LoadSettings(model);
            menu->SyncFromModel();
            lastText = core::SettingsText(model);
            knownTime = t;
            LOG("settings reloaded from the app");
        }
    }
    if (Unload()) FreeLibraryAndExitThread(self, 0);
    ExitThread(0);
}

std::wstring ModuleFolder() {
    wchar_t path[MAX_PATH * 2] = {};
    GetModuleFileNameW(self, path, MAX_PATH * 2);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/"));
}

DWORD WINAPI Release(void*) {
    Sleep(300);
    FreeLibraryAndExitThread(self, 0);
}

DWORD Refuse(DWORD code, const std::string& why) {
    glog::Open(dir + L"\\fathom.log");
    LOG("Fathom for Grand Theft Auto V refused to start: %s", why.c_str());
    glog::Close();
    started = false;
    HANDLE t = CreateThread(nullptr, 0, Release, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
    return code;
}

}

extern "C" __declspec(dllexport) DWORD WINAPI FathomInject(LPVOID) {
    if (started.exchange(true)) return 2;
    dir = ModuleFolder();
    guard::Report report = guard::Check();
    if (guard::Blocked(report)) return Refuse(3, guard::Describe(report));
    std::string version = guard::GameVersion();
    if (!guard::KnownVersion(version)) return Refuse(6, "game version " + version + " has not been checked with this menu yet");
    std::wstring shvPath;
    if (!shv::Locate(dir, shvPath)) return Refuse(5, "ScriptHookV.dll was not found next to the menu file or in the game folder");
    HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    if (!t) return Refuse(0, "could not start the menu thread");
    CloseHandle(t);
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
