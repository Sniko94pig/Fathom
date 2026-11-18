#include "core/model.h"
#include "game/game.h"
#include "game/log.h"
#include "game/overlay.h"
#include "game/ue.h"
#include "ui/icons.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "imgui.h"
#include "minhook/include/MinHook.h"
#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace fs = std::filesystem;

struct LoaderApi {
    DWORD gameThread;
    const wchar_t* dir;
    void (*setReloadKey)(int vk);
    void (*unloaded)();
};

namespace {

HMODULE self = nullptr;
LoaderApi api{};
std::wstring dir;
core::Model model;
std::mutex modelLock;
std::unique_ptr<ui::Menu> menu;
std::atomic<bool> unloadRequested{false};
bool greeted = false;

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

fs::file_time_type FileTime(const std::wstring& path) {
    std::error_code ec;
    auto t = fs::last_write_time(path, ec);
    return ec ? fs::file_time_type::min() : t;
}

std::string BackupSaves() {
    try {
        fs::path src = fs::path(Env(L"LOCALAPPDATA")) / L"Subnautica2" / L"Saved" / L"SaveGames";
        if (!fs::exists(src)) return {};
        fs::path root;
        {
            std::ifstream h(fs::path(dir) / L"home.txt", std::ios::binary);
            std::string raw((std::istreambuf_iterator<char>(h)), std::istreambuf_iterator<char>());
            if (!raw.empty() && raw.size() % 2 == 0) {
                std::wstring home((const wchar_t*)raw.data(), raw.size() / 2);
                std::error_code ec;
                if (fs::is_directory(fs::path(home), ec)) root = fs::path(home) / L"save-backups";
            }
        }
        if (root.empty()) root = fs::path(Env(L"USERPROFILE")) / L"Desktop" / L"sbn2 Mod menu" / L"save-backups";
        if (!fs::exists(root.parent_path())) root = fs::path(dir) / L"save-backups";
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf_s(stamp, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path dst = root / stamp / L"SaveGames";
        fs::create_directories(dst);
        fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing);
        LOG("saves backed up to %s", Narrow(dst.wstring()).c_str());
        return Narrow((root / stamp).wstring());
    } catch (...) {
        return {};
    }
}

struct Projector {
    double cx, cy, cz, fx, fy, fz, rx, ry, rz, ux, uy, uz, focal, halfW, halfH, scale;
    bool ok = false;

    Projector(const core::CameraView& c, ImVec2 display) {
        if (!c.valid || c.focal <= 0 || c.width <= 0 || c.height <= 0) return;
        double P = c.pitch * 3.14159265358979 / 180, Y = c.yaw * 3.14159265358979 / 180, R = c.roll * 3.14159265358979 / 180;
        double SP = std::sin(P), CP = std::cos(P), SY = std::sin(Y), CY = std::cos(Y), SR = std::sin(R), CR = std::cos(R);
        fx = CP * CY;
        fy = CP * SY;
        fz = SP;
        rx = SR * SP * CY - CR * SY;
        ry = SR * SP * SY + CR * CY;
        rz = -SR * CP;
        ux = -(CR * SP * CY + SR * SY);
        uy = CY * SR - CR * SP * SY;
        uz = CR * CP;
        cx = c.x;
        cy = c.y;
        cz = c.z;
        scale = display.x / (double)c.width;
        focal = c.focal * scale;
        halfW = display.x * 0.5;
        halfH = display.y * 0.5;
        ok = true;
    }

    bool operator()(double x, double y, double z, ImVec2& out) const {
        double dx = x - cx, dy = y - cy, dz = z - cz;
        double vz = dx * fx + dy * fy + dz * fz;
        if (vz < 10) return false;
        double vx = dx * rx + dy * ry + dz * rz, vy = dx * ux + dy * uy + dz * uz;
        out = ImVec2((float)(halfW + vx * focal / vz), (float)(halfH - vy * focal / vz));
        return true;
    }
};

void DrawDots(const std::vector<core::EspDot>& dots, bool items) {
    if (dots.empty()) return;
    Projector proj(model.camera, ImGui::GetIO().DisplaySize);
    if (!proj.ok) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto on = [](const char* id) {
        core::Feature* f = model.find(id);
        return !f || f->on;
    };
    bool showName = on(items ? "itemesp_name" : "esp_name"), showDist = on(items ? "itemesp_distance" : "esp_distance");
    bool showHealth = !items && on("esp_health");
    ImFont* font = ui::T().medium;
    float size = (items ? 12.0f : 13.0f) * model.menuScale;
    for (const core::EspDot& d : dots) {
        ImU32 col;
        if (items) col = d.kind == 1 ? IM_COL32(255, 214, 102, 255) : d.kind == 2 ? IM_COL32(255, 160, 90, 255) : IM_COL32(220, 240, 255, 255);
        else col = d.kind == 2 ? ui::Danger() : d.kind == 1 ? IM_COL32(255, 176, 96, 255) : ui::Accent();
        float alpha = std::clamp(1.2f - d.dist / (items ? 150.0f : 300.0f), 0.45f, 1.0f);
        ImVec2 p;
        if (!proj(d.x, d.y, d.z, p)) continue;
        ImVec2 label;
        if (!proj(d.x, d.y, d.top, label)) label = ImVec2(p.x, p.y - 20);
        bool drewBox = false;
        if (d.box) {
            float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
            int n = 0;
            for (int k = 0; k < 8; ++k) {
                ImVec2 s;
                if (!proj(d.cx + ((k & 1) ? d.ex : -d.ex), d.cy + ((k & 2) ? d.ey : -d.ey), d.cz + ((k & 4) ? d.ez : -d.ez), s)) continue;
                ++n;
                x0 = std::min(x0, s.x);
                y0 = std::min(y0, s.y);
                x1 = std::max(x1, s.x);
                y1 = std::max(y1, s.y);
            }
            if (n == 8) {
                dl->AddRect(ImVec2(x0 + 1, y0 + 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(0, 0, 0, (int)(140 * alpha)), 3.0f, 1.5f);
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), (col & 0x00FFFFFF) | ((ImU32)(230 * alpha) << 24), 3.0f, 1.5f);
                label = ImVec2((x0 + x1) * 0.5f, y0);
                drewBox = true;
            }
        }
        if (!drewBox) dl->AddCircleFilled(p, 3.0f * model.menuScale, col, 12);
        std::string line;
        if (showName) line = d.name;
        if (showDist) line += (line.empty() ? "" : "  ") + std::to_string((int)d.dist) + " m";
        if (!line.empty()) {
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, line.c_str());
            ImVec2 tp(label.x - ts.x * 0.5f, label.y - ts.y - 4);
            dl->AddText(font, size, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, (int)(200 * alpha)), line.c_str());
            dl->AddText(font, size, tp, items ? ((col & 0x00FFFFFF) | ((ImU32)(255 * alpha) << 24)) : ui::White(alpha), line.c_str());
        }
        if (showHealth && d.health >= 0) {
            float w = 40.0f * model.menuScale, h = 3.0f * model.menuScale;
            ImVec2 a(label.x - w * 0.5f, label.y - 2), b(label.x + w * 0.5f, label.y - 2 + h);
            if (!drewBox) {
                a = ImVec2(p.x - w * 0.5f, p.y + 6);
                b = ImVec2(p.x + w * 0.5f, p.y + 6 + h);
            }
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, (int)(160 * alpha)), h);
            dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(d.health, 0.0f, 1.0f), b.y), col, h);
        }
    }
}
void DrawEsp() {
    DrawDots(model.esp, false);
    DrawDots(model.itemEsp, true);
}
void Frame() {
    std::lock_guard<std::mutex> g(modelLock);
    if (!greeted) {
        greeted = true;
        menu->Notify(std::string("Fathom loaded. Press ") + core::KeyName(model.keyOpen) + " to open", ICON_WAVES);
    }
    for (const std::string& n : game::TakeNotes()) menu->Notify(n, ICON_INFO);
    DrawEsp();
    menu->Frame();
    model.menuOpen = menu->open;
    game::SetMenuState(menu->open, model.mapVisible, model.legitVisible);
    if (model.requestUnload) {
        model.requestUnload = false;
        unloadRequested = true;
    }
}

bool WaitForEngine() {
    std::string err, lastErr;
    for (int i = 0; i < 1200; ++i) {
        if (ue::Init(err) && ue::FindObject("/Script/Engine.GameEngine")) {
            void* cls = ue::FindObject("/Script/Engine.GameEngine");
            void* engine = ue::FindFirstOf(cls);
            if (engine && ue::Ptr(engine, "GameViewport")) return true;
        }
        if (err != lastErr) {
            LOG("waiting for the game: %s", err.c_str());
            lastErr = err;
        }
        Sleep(500);
    }
    return false;
}

void Unload(bool hooked) {
    LOG("unloading");
    if (hooked) game::Shutdown();
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::SaveSettings(model);
    }
    if (api.setReloadKey) api.setReloadKey(overlay::VirtualKeyFor(model.keyOpen));
    overlay::Stop();
    game::Unhook();
    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(150);
    MH_Uninitialize();
    menu.reset();
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    LOG("unloaded");
    glog::Close();
}

DWORD WINAPI MainThread(void*) {
    glog::Open(dir + L"\\fathom.log");
    LOG("Fathom starting, game thread %lu", api.gameThread);
    if (!WaitForEngine()) {
        LOG("gave up waiting for the game");
        if (api.unloaded) api.unloaded();
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

    core::BuildFeatures(model);
    model.settingsPath = Narrow(dir + L"\\settings.json");
    model.backupSaves = [] { return BackupSaves(); };
    model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    model.openUrl = [](const std::string& url) {
        std::wstring w(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    game::SetFolder(dir);
    game::Wire(model, modelLock, api.gameThread);
    core::LoadSettings(model);
    menu = std::make_unique<ui::Menu>(model);
    menu->SyncFromModel();
    menu->open = false;

    MH_Initialize();
    bool hooked = game::Hook();
    if (!hooked) {
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
    cb.folder = dir;
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
    if (!overlay::Start(cb)) {
        LOG("overlay failed to start");
        unloadRequested = true;
    }
    {
        std::lock_guard<std::mutex> g(modelLock);
        core::LoadItems(model.items, Narrow(dir + L"\\items.json"));
    }
    std::error_code iec;
    if (fs::exists(dir + L"\\icons", iec)) overlay::LoadAtlas(dir + L"\\icons");
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
    Unload(hooked);
    if (api.unloaded) api.unloaded();
    FreeLibraryAndExitThread(self, 0);
}

}

std::atomic<bool> started{false};

DWORD MainThreadId() {
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

extern "C" __declspec(dllexport) DWORD WINAPI FathomInject(LPVOID) {
    if (started.exchange(true)) return 2;
    api = LoaderApi{};
    api.gameThread = MainThreadId();
    static std::wstring folder;
    folder = ModuleFolder();
    dir = folder;
    HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    if (!t) return 0;
    CloseHandle(t);
    return 1;
}

extern "C" __declspec(dllexport) bool FathomStart(const LoaderApi* a) {
    if (!a || started.exchange(true)) return false;
    api = *a;
    dir = a->dir ? a->dir : L".";
    HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    if (!t) return false;
    CloseHandle(t);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self = inst;
        DisableThreadLibraryCalls(inst);
    }
    return TRUE;
}
