#include "core/model.h"
#include "core/ultrakill_features.h"
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
#include <vector>

namespace fs = std::filesystem;

namespace {

HMODULE self = nullptr;
std::wstring dir;
core::Model model;
std::mutex modelLock;
std::unique_ptr<ui::Menu> menu;
std::mutex backupMutex;
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

bool CopyShared(const fs::path& from, const fs::path& to) {
    HANDLE in = CreateFileW(from.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(in, &size) != 0;
    HANDLE out = ok ? CreateFileW(to.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr) : INVALID_HANDLE_VALUE;
    ok = ok && out != INVALID_HANDLE_VALUE;
    long long copied = 0;
    std::vector<char> buf(1 << 16);
    while (ok) {
        DWORD got = 0, put = 0;
        if (!ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr)) ok = false;
        if (!ok || got == 0) break;
        if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got) ok = false;
        copied += got;
    }
    if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
    CloseHandle(in);
    return ok && copied == size.QuadPart;
}

int CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    if (ec) return -1;
    int failed = 0;
    for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path target = to / it->path().lexically_relative(from);
        std::error_code fe;
        if (it->is_directory(fe)) fs::create_directories(target, fe);
        else if (it->is_regular_file(fe) && !CopyShared(it->path(), target)) ++failed;
        if (fe) ++failed;
    }
    return ec ? -1 : failed;
}

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[40];
    swprintf_s(s, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

fs::path FreeFolder(const fs::path& root, const std::wstring& stamp) {
    std::error_code ec;
    fs::path p = root / stamp;
    for (int n = 2; fs::exists(p, ec) && n < 100; ++n) p = root / (stamp + L"_" + std::to_wstring(n));
    return fs::exists(p, ec) || ec ? fs::path() : p;
}

void WriteReadme(const fs::path& folder, const std::wstring& stamp) {
    std::ofstream f(folder / L"README.txt", std::ios::binary | std::ios::trunc);
    f << "ULTRAKILL save backup made by Fathom on " << Narrow(stamp) << "\r\n\r\n"
      << "To restore it, close ULTRAKILL first. Then copy the Saves and Preferences folders from here\r\n"
      << "back into the ULTRAKILL game folder and replace the files there.\r\n";
}

std::string BackupSaves() {
    std::lock_guard<std::mutex> g(backupMutex);
    try {
        fs::path game = fs::path(dir).parent_path();
        fs::path saves = game / L"Saves";
        std::error_code ec;
        if (!fs::is_directory(saves, ec)) {
            LOG("save backup failed: the Saves folder was not found");
            return {};
        }
        std::wstring stamp = Stamp();
        fs::path dst = FreeFolder(fs::path(dir) / L"save_backup", stamp);
        if (dst.empty()) {
            LOG("save backup failed: no free backup folder name");
            return {};
        }
        int bad = CopyTree(saves, dst / L"Saves");
        if (bad != 0) {
            if (bad < 0) LOG("save backup failed: the Saves folder could not be read");
            else LOG("save backup failed: %d save files could not be copied", bad);
            fs::remove_all(dst, ec);
            return {};
        }
        fs::path prefs = game / L"Preferences";
        if (fs::is_directory(prefs, ec) && CopyTree(prefs, dst / L"Preferences") != 0)
            LOG("save backup: some preference files could not be copied");
        WriteReadme(dst, stamp);
        std::string path = Narrow(dst.wstring());
        ukgame::SetLastBackup(path);
        LOG("saves backed up to save_backup\\%s", Narrow(dst.filename().wstring()).c_str());
        return path;
    } catch (...) {
        LOG("save backup failed");
        return {};
    }
}

void Frame() {
    std::lock_guard<std::mutex> g(modelLock);
    if (!greeted) {
        greeted = true;
        menu->Notify(std::string("Fathom loaded. Press ") + core::KeyName(model.keyOpen) + " to open", ICON_CROSSHAIR);
    }
    for (const std::string& n : ukgame::TakeNotes()) menu->Notify(n, ICON_INFO);
    menu->Frame();
    model.menuOpen = menu->open;
    ukgame::SetMenu(menu->open, menu->focused);
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
            if (!monoThread) err = "could not join the game's script runtime";
            else if (ukgame::Prepare(err)) return true;
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
    ukgame::SetMenu(false, false);
    ukgame::RequestStop();
    ULONGLONG start = GetTickCount64(), told = 0;
    while (!ukgame::Stopped()) {
        Sleep(20);
        ULONGLONG waited = GetTickCount64() - start;
        if (waited >= 3000 && waited - told >= 10000) {
            told = waited;
            LOG("waiting for the game to draw a frame so the cheats can be switched off");
        }
    }
    bool safe = ukgame::SafeToUnload();
    if (!safe) LOG("the menu file stays loaded but does nothing until the game closes");
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::SaveSettings(model);
    }
    overlay::Stop();
    if (!ukgame::HoldsHooks()) {
        MH_DisableHook(MH_ALL_HOOKS);
        Sleep(150);
        MH_Uninitialize();
    }
    menu.reset();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    Sleep(300);
    if (monoThread) mono::Detach(monoThread);
    monoThread = nullptr;
    LOG("unloaded");
    slog::Close();
    return safe;
}

DWORD WINAPI MainThread(void*) {
    slog::Open(dir + L"\\fathom.log");
    LOG("Fathom for ULTRAKILL starting");
    MH_Initialize();
    if (!WaitForGame()) {
        LOG("gave up waiting for the game");
        MH_Uninitialize();
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
        uk::BuildFeatures(model);
        model.settingsPath = Narrow(dir + L"\\settings.json");
        model.backupSaves = [] { return BackupSaves(); };
        model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        model.openUrl = [](const std::string& url) {
            std::wstring w(url.begin(), url.end());
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        ukgame::SetFolder(dir);
        ukgame::SetBackup([] { return BackupSaves(); });
        ukgame::Wire(model, modelLock);
        core::LoadSettings(model);
        menu = std::make_unique<ui::Menu>(model);
        menu->SyncFromModel();
        menu->open = false;
    }

    overlay::Callbacks cb;
    cb.frame = [] { Frame(); };
    cb.wantsInput = [] { return menu && menu->open && menu->focused; };
    cb.wantsKeyboard = [] { return true; };
    cb.logoReady = [](uint64_t tex) {
        std::lock_guard<std::mutex> g(modelLock);
        model.logoTexture = tex;
    };
    cb.window = [](void* h) { ukgame::SetGameWindow(h); };
    bool overlayOk = overlay::Start(cb);
    if (!overlayOk) {
        LOG("overlay failed to start");
        unloadRequested = true;
    }
    std::string err;
    if (overlayOk && !ukgame::Start(err)) {
        LOG("could not connect: %s", err.c_str());
        std::lock_guard<std::mutex> g(modelLock);
        for (core::Feature& f : model.features) {
            f.available = false;
            f.missing = "Could not connect to the game";
        }
    }
    if (monoThread) {
        mono::Detach(monoThread);
        monoThread = nullptr;
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
