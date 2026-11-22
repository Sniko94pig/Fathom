#include "core/gd_features.h"
#include "core/model.h"
#include "ingame/game.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include "ingame/modules.h"
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

std::wstring Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t s[40];
    swprintf_s(s, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return s;
}

fs::path SaveFolder() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return {};
    return fs::path(buf) / L"GeometryDash";
}

std::string BackupSaves() {
    std::lock_guard<std::mutex> g(backupMutex);
    try {
        fs::path saves = SaveFolder();
        std::error_code ec;
        if (saves.empty() || !fs::is_directory(saves, ec)) {
            LOG("save backup failed: the save folder was not found");
            return {};
        }
        std::vector<fs::path> files;
        for (const auto& e : fs::directory_iterator(saves, ec)) {
            std::wstring name = e.path().filename().wstring();
            bool dat = e.path().extension() == L".dat";
            bool ours = name.rfind(L"CCGameManager", 0) == 0 || name.rfind(L"CCLocalLevels", 0) == 0;
            if (dat && ours && e.is_regular_file(ec)) files.push_back(e.path());
        }
        if (files.empty()) {
            LOG("save backup failed: no save files were found");
            return {};
        }
        std::wstring stamp = Stamp();
        fs::path root = fs::path(dir) / L"save_backup";
        fs::path dst = root / stamp;
        for (int n = 2; fs::exists(dst, ec) && n < 100; ++n) dst = root / (stamp + L"_" + std::to_wstring(n));
        fs::create_directories(dst, ec);
        if (ec) {
            LOG("save backup failed: the backup folder could not be made");
            return {};
        }
        int bad = 0;
        for (const fs::path& f : files)
            if (!CopyShared(f, dst / f.filename())) ++bad;
        if (bad) {
            LOG("save backup failed: %d save files could not be copied", bad);
            fs::remove_all(dst, ec);
            return {};
        }
        std::ofstream readme(dst / L"README.txt", std::ios::binary | std::ios::trunc);
        readme << "Geometry Dash save backup made by Fathom on " << gd::Narrow(stamp) << "\r\n\r\n"
               << "To restore it, close Geometry Dash first. Then copy these files back into\r\n"
               << "%LOCALAPPDATA%\\GeometryDash and replace the files there.\r\n";
        LOG("saves backed up to save_backup\\%s", gd::Narrow(dst.filename().wstring()).c_str());
        return gd::Narrow(dst.wstring());
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
    for (const std::string& n : gd::TakeNotes()) menu->Notify(n, ICON_INFO);
    game::BeforeMenu(*menu);
    menu->Frame();
    model.menuOpen = menu->open;
    gd::C().menuOpen = menu->open;
    game::AfterMenu();
    if (model.requestUnload) {
        model.requestUnload = false;
        unloadRequested = true;
    }
}

bool GameWindowUp() {
    struct Find {
        DWORD pid;
        bool found;
    } f{GetCurrentProcessId(), false};
    EnumWindows(
        [](HWND w, LPARAM lp) -> BOOL {
            Find* s = (Find*)lp;
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            if (pid != s->pid || !IsWindowVisible(w)) return TRUE;
            wchar_t cls[64] = {};
            GetClassNameW(w, cls, 64);
            if (wcscmp(cls, L"GLFW30") != 0) return TRUE;
            s->found = true;
            return FALSE;
        },
        (LPARAM)&f);
    return f.found;
}

bool WaitForGame() {
    std::string err, lastErr;
    for (int i = 0; i < 2400 && !unloadRequested; ++i) {
        err.clear();
        if (gd::LoadModules(err) && gd::cc::Resolve(err)) {
            if (GameWindowUp()) return true;
            err = "the game window is not open yet";
        }
        if (err != lastErr) {
            LOG("waiting for the game: %s", err.c_str());
            lastErr = err;
        }
        Sleep(500);
    }
    return false;
}

void Unload() {
    LOG("unloading");
    {
        std::lock_guard<std::mutex> g(modelLock);
        if (menu) menu->open = false;
    }
    game::RequestStop();
    ULONGLONG start = GetTickCount64(), told = 0;
    while (!game::Stopped()) {
        Sleep(20);
        ULONGLONG waited = GetTickCount64() - start;
        if (waited >= 3000 && waited - told >= 10000) {
            told = waited;
            LOG("waiting for the game to run a frame so the cheats can be switched off");
        }
    }
    bool told2 = false;
    while (game::Holding()) {
        if (!told2) {
            told2 = true;
            gd::Note("Fathom unloads when you leave this level");
            LOG("waiting for the level to close so safe mode can finish");
        }
        Sleep(100);
    }
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::SaveSettings(model);
    }
    game::SavePrefs(true);
    overlay::Stop();
    MH_DisableHook(MH_ALL_HOOKS);
    for (int i = 0; i < 300 && gd::BusyCount() > 0; ++i) Sleep(10);
    Sleep(200);
    MH_Uninitialize();
    menu.reset();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    Sleep(200);
    LOG("unloaded");
    slog::Close();
}

DWORD WINAPI MainThread(void*) {
    slog::Open(dir + L"\\fathom.log");
    LOG("Fathom for Geometry Dash starting");
    if (!WaitForGame()) {
        LOG("gave up waiting for the game");
        slog::Close();
        FreeLibraryAndExitThread(self, 0);
    }
    LOG("game found");
    gd::SetFolder(dir);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ui::LoadFonts();

    {
        std::lock_guard<std::mutex> g(modelLock);
        gd::BuildFeatures(model);
        model.settingsPath = gd::Narrow(dir + L"\\settings.json");
        model.backupSaves = [] { return BackupSaves(); };
        model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
        model.openUrl = [](const std::string& url) {
            std::wstring w = gd::Widen(url);
            ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        };
        model.inWorld = true;
        game::Wire(model, modelLock);
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
    cb.cleanup = [] { scare::Cleanup(); };
    bool overlayOk = overlay::Start(gd::cc::SwapBuffersTarget(), cb);
    if (!overlayOk) {
        LOG("overlay failed to start");
        unloadRequested = true;
    }
    std::string err;
    if (overlayOk && !game::Start(err)) {
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
        game::SavePrefs(false);
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
    Unload();
    FreeLibraryAndExitThread(self, 0);
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
