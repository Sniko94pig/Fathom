#include "core/bl3_features.h"
#include "core/model.h"
#include "game/game.h"
#include "game/ue.h"
#include "ingame/esp.h"
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
#include <tlhelp32.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

namespace {

HMODULE self = nullptr;
std::wstring dir;
DWORD gameThread = 0;
core::Model model;
std::mutex modelLock;
std::unique_ptr<ui::Menu> menu;
std::atomic<bool> unloadRequested{false};
std::atomic<bool> started{false};
bool greeted = false;

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
        fs::path src = Documents() / L"My Games" / L"Borderlands 3" / L"Saved" / L"SaveGames";
        if (src.empty() || !fs::exists(src)) return {};
        fs::path home = HomeFolder();
        fs::path root = home.empty() ? fs::path(dir) / L"save-backups" : home / L"save-backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[80];
        swprintf_s(stamp, L"Borderlands3_%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path dst = root / stamp / L"SaveGames";
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        LOG("saves backed up to %s", ue::Utf8(dst.wstring()).c_str());
        return ue::Utf8((root / stamp).wstring());
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
    esp::Draw(model);
    menu->Frame();
    model.menuOpen = menu->open;
    game::SetMenuState(menu->open, menu->focused, model.mapVisible);
    if (model.requestUnload) {
        model.requestUnload = false;
        unloadRequested = true;
    }
}

bool WaitForEngine() {
    std::string err, lastErr;
    for (int i = 0; i < 2400 && !unloadRequested; ++i) {
        if (ue::Init(err)) {
            void* cls = ue::FindObject("/Script/Engine.GameEngine");
            void* engine = cls ? ue::FindFirstOf(cls) : nullptr;
            if (engine && ue::Ptr(engine, "GameViewport")) return true;
            err = "engine not ready";
        }
        if (err != lastErr) {
            LOG("waiting for the game: %s", err.c_str());
            lastErr = err;
        }
        Sleep(500);
    }
    return false;
}

bool Unload(bool hooked) {
    LOG("unloading");
    {
        std::lock_guard<std::mutex> g(modelLock);
        if (menu) menu->open = false;
    }
    bool safe = hooked ? game::Shutdown() : true;
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::SaveSettings(model);
    }
    overlay::Stop();
    game::Unhook();
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(150);
    MH_Uninitialize();
    menu.reset();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    if (!safe) LOG("the menu file stays loaded but does nothing until the game closes");
    LOG("unloaded");
    glog::Close();
    return safe;
}

DWORD WINAPI MainThread(void*) {
    glog::Open(dir + L"\\fathom.log");
    LOG("Fathom for Borderlands 3 starting, game thread %lu", gameThread);
    if (!WaitForEngine()) {
        LOG("gave up waiting for the game");
        glog::Close();
        FreeLibraryAndExitThread(self, 0);
    }
    LOG("engine found");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ui::LoadFonts();

    {
        std::lock_guard<std::mutex> g(modelLock);
        bl3::BuildFeatures(model);
        model.settingsPath = ue::Utf8(dir + L"\\settings.json");
        model.backupSaves = [] { return BackupSaves(); };
        model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        model.openUrl = [](const std::string& url) {
            std::wstring w = ue::Wide(url);
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        game::SetFolder(dir);
        game::SetBackup([] { return BackupSaves(); });
        game::Wire(model, modelLock, gameThread);
        core::LoadSettings(model);
        menu = std::make_unique<ui::Menu>(model);
        menu->SyncFromModel();
        menu->open = false;
    }

    MH_Initialize();
    bool hooked = game::Hook();
    if (!hooked) {
        LOG("could not connect to the game loop");
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
    ULONGLONG lastSync = 0;
    while (!unloadRequested) {
        Sleep(50);
        ULONGLONG now = GetTickCount64();
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
    if (Unload(hooked)) FreeLibraryAndExitThread(self, 0);
    ExitThread(0);
}

DWORD OldestThread() {
    DWORD best = 0;
    ULONGLONG bestTime = ~0ull;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    THREADENTRY32 te{sizeof(te)};
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
        HANDLE t = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!t) continue;
        FILETIME c, e, k, u;
        if (GetThreadTimes(t, &c, &e, &k, &u)) {
            ULONGLONG v = ((ULONGLONG)c.dwHighDateTime << 32) | c.dwLowDateTime;
            if (v < bestTime) {
                bestTime = v;
                best = te.th32ThreadID;
            }
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    return best;
}

std::wstring ModuleFolder() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(self, path, MAX_PATH);
    std::wstring p(path);
    return p.substr(0, p.find_last_of(L"\\/"));
}

}

extern "C" __declspec(dllexport) DWORD WINAPI FathomInject(LPVOID) {
    if (started.exchange(true)) return 2;
    dir = ModuleFolder();
    gameThread = OldestThread();
    HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    if (!t) return 0;
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
