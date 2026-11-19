#include "core/model.h"
#include "core/sn1_features.h"
#include "ingame/esp.h"
#include "ingame/game.h"
#include "ingame/log.h"
#include "ingame/mono.h"
#include "ingame/overlay.h"
#include "ui/icons.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "imgui.h"
#include "MinHook.h"
#include <windows.h>
#include <shellapi.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

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
mono::Thread* monoThread = nullptr;

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

fs::file_time_type FileTime(const std::wstring& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
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
        fs::path src = fs::path(dir).parent_path() / L"SNAppData" / L"SavedGames";
        if (!fs::exists(src)) return {};
        fs::path home = HomeFolder();
        fs::path root = home.empty() ? fs::path(dir) / L"save-backups" : home / L"save-backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[80];
        swprintf_s(stamp, L"Subnautica_%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path dst = root / stamp / L"SavedGames";
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        LOG("saves backed up");
        return Narrow((root / stamp).wstring());
    } catch (...) {
        return {};
    }
}

void Frame() {
    std::lock_guard<std::mutex> g(modelLock);
    if (!greeted) {
        greeted = true;
        menu->Notify(std::string("Fathom loaded. Press ") + core::KeyName(model.keyOpen) + " to open", ICON_WAVES);
    }
    for (const std::string& n : sn1game::TakeNotes()) menu->Notify(n, ICON_INFO);
    esp::Draw(model.menuScale);
    menu->Frame();
    model.menuOpen = menu->open;
    sn1game::SetMenu(menu->open, menu->focused);
    if (model.requestUnload) {
        model.requestUnload = false;
        unloadRequested = true;
    }
}

bool WaitForGame() {
    std::string err, lastErr;
    for (int i = 0; i < 2400 && !unloadRequested; ++i) {
        if (mono::Load(err)) {
            if (!monoThread) monoThread = mono::Attach();
            if (monoThread && mono::FindImage("Assembly-CSharp") && sn1game::Prepare(err)) return true;
        }
        if (err != lastErr) {
            LOG("waiting for the game: %s", err.c_str());
            lastErr = err;
        }
        Sleep(500);
    }
    return false;
}

bool Unload() {
    LOG("unloading");
    {
        std::lock_guard<std::mutex> g(modelLock);
        if (menu) menu->open = false;
    }
    sn1game::SetMenu(false, false);
    sn1game::RequestStop();
    ULONGLONG start = GetTickCount64(), told = 0;
    while (!sn1game::Stopped()) {
        Sleep(20);
        ULONGLONG waited = GetTickCount64() - start;
        if (waited >= 3000 && waited - told >= 10000) {
            told = waited;
            LOG("waiting for the game to draw a frame so the cheats can be switched off");
        }
    }
    bool safe = sn1game::SafeToUnload();
    if (!safe) LOG("the menu file stays loaded but does nothing until the game closes");
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::SaveSettings(model);
    }
    overlay::Stop();
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(150);
    MH_Uninitialize();
    menu.reset();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    esp::Clear();
    Sleep(300);
    if (monoThread) mono::Detach(monoThread);
    monoThread = nullptr;
    LOG("unloaded");
    slog::Close();
    return safe;
}

DWORD WINAPI MainThread(void*) {
    slog::Open(dir + L"\\fathom.log");
    LOG("Fathom for Subnautica starting");
    if (!WaitForGame()) {
        LOG("gave up waiting for the game");
        if (monoThread) mono::Detach(monoThread);
        slog::Close();
        FreeLibraryAndExitThread(self, 0);
    }
    LOG("game found");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ui::LoadFonts();

    {
        std::lock_guard<std::mutex> g(modelLock);
        sn1::BuildFeatures(model);
        model.settingsPath = Narrow(dir + L"\\settings.json");
        model.backupSaves = [] { return BackupSaves(); };
        model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        model.openUrl = [](const std::string& url) {
            std::wstring w(url.begin(), url.end());
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        sn1game::SetFolder(dir);
        sn1game::Wire(model, modelLock);
        core::LoadSettings(model);
        menu = std::make_unique<ui::Menu>(model);
        menu->SyncFromModel();
        menu->open = false;
    }

    MH_Initialize();
    overlay::Callbacks cb;
    cb.frame = [] { Frame(); };
    cb.wantsInput = [] { return menu && menu->open && menu->focused; };
    cb.wantsKeyboard = [] { return true; };
    cb.logoReady = [](uint64_t tex) {
        std::lock_guard<std::mutex> g(modelLock);
        model.logoTexture = tex;
    };
    cb.window = [](void* h) { sn1game::SetGameWindow(h); };
    bool overlayOk = overlay::Start(cb);
    if (!overlayOk) {
        LOG("overlay failed to start");
        unloadRequested = true;
    }
    std::string err;
    if (overlayOk && !sn1game::Start(err)) {
        LOG("could not connect: %s", err.c_str());
        std::lock_guard<std::mutex> g(modelLock);
        for (core::Feature& f : model.features) {
            f.available = false;
            f.missing = "Could not connect to the game";
        }
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
    if (Unload()) FreeLibraryAndExitThread(self, 0);
    ExitThread(0);
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
