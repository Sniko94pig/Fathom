#include "core/model.h"
#include "launcher/art.h"
#include "launcher/games.h"
#include "launcher/home.h"
#include "launcher/logview.h"
#include "launcher/inject.h"
#include "launcher/game_inject.h"
#include "core/sn1_features.h"
#include "core/sotf_features.h"
#include "launcher/prefs.h"
#include "ui/icons.h"
#include "ui/atlas.h"
#include "common/dx12_texture.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx12.h"
#include <d3d12.h>
#include <dxgi1_5.h>
#include <tchar.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _DEBUG
#define DX12_ENABLE_DEBUG_LAYER
#endif

#ifdef DX12_ENABLE_DEBUG_LAYER
#include <dxgidebug.h>
#pragma comment(lib, "dxguid.lib")
#endif

static const int APP_NUM_FRAMES_IN_FLIGHT = 2;
static const int APP_NUM_BACK_BUFFERS = 2;
static const int APP_SRV_HEAP_SIZE = 256;

struct FrameContext
{
    ID3D12CommandAllocator*     CommandAllocator;
    UINT64                      FenceValue;
};

struct ExampleDescriptorHeapAllocator
{
    ID3D12DescriptorHeap*       Heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE  HeapType = D3D12_DESCRIPTOR_HEAP_TYPE_NUM_TYPES;
    D3D12_CPU_DESCRIPTOR_HANDLE HeapStartCpu;
    D3D12_GPU_DESCRIPTOR_HANDLE HeapStartGpu;
    UINT                        HeapHandleIncrement;
    ImVector<int>               FreeIndices;

    void Create(ID3D12Device* device, ID3D12DescriptorHeap* heap)
    {
        IM_ASSERT(Heap == nullptr && FreeIndices.empty());
        Heap = heap;
        D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
        HeapType = desc.Type;
        HeapStartCpu = Heap->GetCPUDescriptorHandleForHeapStart();
        HeapStartGpu = Heap->GetGPUDescriptorHandleForHeapStart();
        HeapHandleIncrement = device->GetDescriptorHandleIncrementSize(HeapType);
        FreeIndices.reserve((int)desc.NumDescriptors);
        for (int n = desc.NumDescriptors; n > 0; n--)
            FreeIndices.push_back(n - 1);
    }
    void Destroy()
    {
        Heap = nullptr;
        FreeIndices.clear();
    }
    void Alloc(D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_desc_handle)
    {
        IM_ASSERT(FreeIndices.Size > 0);
        int idx = FreeIndices.back();
        FreeIndices.pop_back();
        out_cpu_desc_handle->ptr = HeapStartCpu.ptr + (idx * HeapHandleIncrement);
        out_gpu_desc_handle->ptr = HeapStartGpu.ptr + (idx * HeapHandleIncrement);
    }
    void Free(D3D12_CPU_DESCRIPTOR_HANDLE out_cpu_desc_handle, D3D12_GPU_DESCRIPTOR_HANDLE out_gpu_desc_handle)
    {
        int cpu_idx = (int)((out_cpu_desc_handle.ptr - HeapStartCpu.ptr) / HeapHandleIncrement);
        int gpu_idx = (int)((out_gpu_desc_handle.ptr - HeapStartGpu.ptr) / HeapHandleIncrement);
        IM_ASSERT(cpu_idx == gpu_idx);
        FreeIndices.push_back(cpu_idx);
    }
};

static FrameContext                 g_frameContext[APP_NUM_FRAMES_IN_FLIGHT] = {};
static UINT                         g_frameIndex = 0;

static ID3D12Device*                g_pd3dDevice = nullptr;
static ID3D12DescriptorHeap*        g_pd3dRtvDescHeap = nullptr;
static ID3D12DescriptorHeap*        g_pd3dSrvDescHeap = nullptr;
static ExampleDescriptorHeapAllocator g_pd3dSrvDescHeapAlloc;
static ID3D12CommandQueue*          g_pd3dCommandQueue = nullptr;
static ID3D12GraphicsCommandList*   g_pd3dCommandList = nullptr;
static ID3D12Fence*                 g_fence = nullptr;
static HANDLE                       g_fenceEvent = nullptr;
static UINT64                       g_fenceLastSignaledValue = 0;
static IDXGISwapChain3*             g_pSwapChain = nullptr;
static bool                         g_SwapChainTearingSupport = false;
static bool                         g_SwapChainOccluded = false;
static HANDLE                       g_hSwapChainWaitableObject = nullptr;
static ID3D12Resource*              g_mainRenderTargetResource[APP_NUM_BACK_BUFFERS] = {};
static D3D12_CPU_DESCRIPTOR_HANDLE  g_mainRenderTargetDescriptor[APP_NUM_BACK_BUFFERS] = {};

bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
void WaitForPendingOperations();
FrameContext* WaitForNextFrameContext();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);


static std::string Narrow(const std::wstring& w)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

static std::wstring ExeDir()
{
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return std::filesystem::path(path).parent_path().wstring();
}

static std::filesystem::file_time_type FileTime(const std::wstring& path)
{
    std::error_code ec;
    auto t = std::filesystem::last_write_time(path, ec);
    return ec ? std::filesystem::file_time_type::min() : t;
}

static SIZE ClientSize(float scale)
{
    return SIZE{ (LONG)std::lround(960.0f * scale), (LONG)std::lround(620.0f * scale) };
}

static float g_uiScale = 1.0f;

static void FitWindow(HWND hwnd, float from, float to)
{
    if (IsZoomed(hwnd) || from <= 0.0f)
        return;
    RECT c;
    GetClientRect(hwnd, &c);
    float k = to / from;
    RECT r = { 0, 0, (LONG)std::lround((c.right - c.left) * k), (LONG)std::lround((c.bottom - c.top) * k) };
    DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
    AdjustWindowRectEx(&r, style, FALSE, 0);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR cmd, int)
{
    if (cmd && strstr(cmd, "--inject"))
    {
        bool ok = false;
        inject::Inject(inject::Locate(), ok);
        return ok ? 0 : 1;
    }
    HANDLE single = CreateMutexW(nullptr, TRUE, L"FathomLauncherSingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS)
    {
        HWND other = FindWindowW(L"FathomLauncher", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }

    ImGui_ImplWin32_EnableDpiAwareness();

    core::Model model;
    core::BuildFeatures(model);
    core::Model sn1Model;
    sn1::BuildFeatures(sn1Model);
    launcher::Home home;
    home.paths = inject::Locate();
    home.sn1Paths = gameinject::Locate(gameinject::kSubnautica);
    std::wstring sn1SettingsPath = home.sn1Paths.found ? home.sn1Paths.settings : (std::filesystem::path(ExeDir()) / L"subnautica-settings.json").wstring();
    if (home.sn1Paths.found) { std::error_code ec; std::filesystem::create_directories(home.sn1Paths.folder, ec); }
    sn1Model.settingsPath = Narrow(sn1SettingsPath);
    std::wstring settingsPath = home.paths.found ? home.paths.settings : L"settings.json";
    if (home.paths.found) { std::error_code ec; std::filesystem::create_directories(home.paths.folder, ec); }
    model.settingsPath = Narrow(settingsPath);
    for (core::Feature& f : model.features)
    {
        f.available = false;
        f.missing = "Turn this on inside the game. Cheats always start off.";
    }
    model.backupSaves = [] { return inject::BackupSaves(); };
    model.openUrl = [](const std::string& url) {
        std::wstring w(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    core::LoadSettings(model);
    std::string lastText = core::SettingsText(model);
    auto knownTime = FileTime(settingsPath);
    for (core::Feature& f : sn1Model.features)
    {
        f.available = false;
        f.missing = "Turn this on inside the game. Cheats always start off.";
    }
    sn1Model.backupSaves = [&home] { return gameinject::BackupSaves(gameinject::kSubnautica, home.sn1Paths); };
    sn1Model.openUrl = model.openUrl;
    core::LoadSettings(sn1Model);
    std::string sn1LastText = core::SettingsText(sn1Model);
    auto sn1KnownTime = FileTime(sn1SettingsPath);
    core::Model sotfModel;
    sotf::BuildFeatures(sotfModel);
    home.sotfPaths = gameinject::Locate(gameinject::kSonsOfTheForest);
    std::wstring sotfSettingsPath = home.sotfPaths.found ? home.sotfPaths.settings : (std::filesystem::path(ExeDir()) / L"sonsoftheforest-settings.json").wstring();
    if (home.sotfPaths.found) { std::error_code ec; std::filesystem::create_directories(home.sotfPaths.folder, ec); }
    sotfModel.settingsPath = Narrow(sotfSettingsPath);
    for (core::Feature& f : sotfModel.features)
    {
        f.available = false;
        f.missing = "Turn this on inside the game. Cheats always start off.";
    }
    sotfModel.backupSaves = [&home] { return gameinject::BackupSaves(gameinject::kSonsOfTheForest, home.sotfPaths); };
    sotfModel.openUrl = model.openUrl;
    core::LoadSettings(sotfModel);
    std::string sotfLastText = core::SettingsText(sotfModel);
    auto sotfKnownTime = FileTime(sotfSettingsPath);

    ui::Loader loader;
    loader.version = FATHOM_VERSION;
    loader.gameFound = home.paths.found;
    const std::wstring prefsPath = (std::filesystem::path(ExeDir()) / L"launcher.json").wstring();
    launcher::LoadPrefs(loader.s, prefsPath);
    loader.s.startWithWindows = launcher::StartupEnabled();
    ui::LoaderSettings applied = loader.s;
    std::string prefsText = launcher::PrefsText(loader.s);

    SIZE cs = ClientSize(loader.s.scale);
    DWORD style = WS_OVERLAPPEDWINDOW;
    g_uiScale = loader.s.scale;
    RECT wr = { 0, 0, cs.cx, cs.cy };
    AdjustWindowRectEx(&wr, style, FALSE, 0);
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, GetModuleHandle(nullptr), LoadIconW(GetModuleHandle(nullptr), MAKEINTRESOURCEW(1)), LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"FathomLauncher", nullptr };
    ::RegisterClassExW(&wc);
    HWND hwnd = ::CreateWindowW(wc.lpszClassName, L"Fathom", style, CW_USEDEFAULT, CW_USEDEFAULT, wr.right - wr.left, wr.bottom - wr.top, nullptr, nullptr, wc.hInstance, nullptr);
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &dark, sizeof(dark));

    if (!CreateDeviceD3D(hwnd))
    {
        CleanupDeviceD3D();
        ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
        MessageBoxW(nullptr, L"Your graphics card could not start DirectX 12.", L"Fathom", MB_ICONERROR);
        return 1;
    }

    if (loader.s.onTop)
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    ::ShowWindow(hwnd, cmd && strstr(cmd, "--minimized") ? SW_SHOWMINNOACTIVE : SW_SHOWDEFAULT);
    ::UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    ui::LoadFonts();
    ui::LoadMonoFont();
    launcher::LogView logView;
    ui::Menu mainMenu(model);
    ui::Menu sn1Menu(sn1Model);
    ui::Menu sotfMenu(sotfModel);
    for (ui::Menu* mm : { &mainMenu, &sn1Menu, &sotfMenu })
    {
        mm->embedded = true;
        mm->homePage = [&home, mm] {
            home.mine = mm->ActiveHome() == 1;
            home.Draw();
        };
        mm->homeName = "Games";
        mm->homeIcon = ICON_LAYERS;
        mm->onHome = true;
        mm->showGameTabs = false;
        mm->onBack = [&] { home.game = -1; mainMenu.onHome = true; sn1Menu.onHome = true; sotfMenu.onHome = true; };
        mm->loader = &loader;
        mm->uiScale = loader.s.scale;
    }
    ui::T().accent = ui::AccentColor(loader.s.accent);
    bool artRefresh = false, artWipe = false, downloading = false, updateAsked = false;
    ULONGLONG closeAt = 0;
    struct Watch
    {
        inject::GameState last = inject::GameState::NotRunning;
        ULONGLONG since = 0;
        bool tried = false;
    };
    Watch watch[3];
    auto openPath = [](const std::wstring& path) {
        std::error_code ec;
        std::filesystem::create_directories(path, ec);
        ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    loader.openHome = [&] { openPath(ExeDir()); };
    loader.openBackups = [&] { openPath((std::filesystem::path(ExeDir()) / L"save-backups").wstring()); };
    loader.openGame = [&] { openPath(home.paths.folder); };
    loader.backupSaves = [] { return inject::BackupSaves(); };
    loader.refreshArt = [&] { artRefresh = true; artWipe = true; };
    loader.checkUpdates = [] { launcher::update::Check(); };
    loader.installUpdate = [&] { downloading = true; launcher::update::Download(); };
    if (loader.s.checkUpdates)
        launcher::update::Check();
    const auto& games = launcher::Games();
    home.installed.resize(games.size());
    home.installedWith.resize(games.size());
    std::vector<std::pair<int, int>> wantedArt;
    for (size_t i = 0; i < games.size(); ++i)
    {
        home.installedWith[i] = launcher::InstalledWith(games[i]);
        home.installed[i] = home.installedWith[i] != nullptr;
        if (games[i].appId > 0)
            wantedArt.push_back({ games[i].appId, launcher::art::Capsule });
    }
    for (const auto& g : games)
        if (g.appId > 0)
            wantedArt.push_back({ g.appId, launcher::art::Hero });
    const std::wstring artDir = (std::filesystem::path(ExeDir()) / L"art").wstring();
    launcher::art::Start(wantedArt, artDir, loader.s.webArt);
    struct ArtTex { ID3D12Resource* tex; D3D12_CPU_DESCRIPTOR_HANDLE cpu; D3D12_GPU_DESCRIPTOR_HANDLE gpu; };
    std::vector<ArtTex> artTex;

    ImGui_ImplWin32_Init(hwnd);

    ImGui_ImplDX12_InitInfo init_info = {};
    init_info.Device = g_pd3dDevice;
    init_info.CommandQueue = g_pd3dCommandQueue;
    init_info.NumFramesInFlight = APP_NUM_FRAMES_IN_FLIGHT;
    init_info.RTVFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    init_info.DSVFormat = DXGI_FORMAT_UNKNOWN;
    init_info.SrvDescriptorHeap = g_pd3dSrvDescHeap;
    init_info.SrvDescriptorAllocFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu_handle) { return g_pd3dSrvDescHeapAlloc.Alloc(out_cpu_handle, out_gpu_handle); };
    init_info.SrvDescriptorFreeFn = [](ImGui_ImplDX12_InitInfo*, D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle, D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle)            { return g_pd3dSrvDescHeapAlloc.Free(cpu_handle, gpu_handle); };
    ImGui_ImplDX12_Init(&init_info);

    ID3D12Resource* iconTex = nullptr;
    ID3D12Resource* logoTex = nullptr;
    {
        std::vector<uint8_t> px;
        int lw = 0, lh = 0;
        if (ui::LogoPixels(px, lw, lh))
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu;
            g_pd3dSrvDescHeapAlloc.Alloc(&cpu, &gpu);
            logoTex = dx12::CreateTexture(g_pd3dDevice, g_pd3dCommandQueue, px.data(), lw, lh, cpu);
            if (logoTex)
                model.logoTexture = (uint64_t)gpu.ptr;
        }
    }
    if (home.paths.found)
    {
        core::LoadItems(model.items, Narrow(home.paths.folder + L"\\items.json"));
        std::error_code bec;
        for (const auto& e : std::filesystem::directory_iterator(home.paths.folder + L"\\bases", bec))
        {
            if (e.path().extension() != L".json")
                continue;
            core::BaseInfo b;
            b.name = e.path().stem().string();
            b.file = e.path().string();
            model.localBases.push_back(b);
        }
        ui::Atlas atlas = ui::BuildAtlas(home.paths.folder + L"\\icons", 64);
        if (!atlas.rgba.empty())
        {
            D3D12_CPU_DESCRIPTOR_HANDLE cpu;
            D3D12_GPU_DESCRIPTOR_HANDLE gpu;
            g_pd3dSrvDescHeapAlloc.Alloc(&cpu, &gpu);
            iconTex = dx12::CreateTexture(g_pd3dDevice, g_pd3dCommandQueue, atlas.rgba.data(), atlas.width, atlas.height, cpu);
            if (iconTex)
            {
                model.iconTexture = (uint64_t)gpu.ptr;
                for (auto& [id, r] : atlas.rects) model.icons[id] = { r.u0, r.v0, r.u1, r.v1 };
            }
        }
    }

    sn1Model.logoTexture = model.logoTexture;
    sotfModel.logoTexture = model.logoTexture;

    std::thread worker;
    std::atomic<bool> workerDone{ false };
    std::string workerResult;
    bool workerOk = false;
    ULONGLONG lastPoll = 0, lastSync = 0;
    float shownScale = loader.s.scale;

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (::PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            ::TranslateMessage(&msg);
            ::DispatchMessage(&msg);
            if (msg.message == WM_QUIT)
                done = true;
        }
        if (done)
            break;

        bool sn1Open = home.game >= 0 && home.game < (int)games.size() && strcmp(games[home.game].id, "subnautica") == 0;
        bool sotfOpen = home.game >= 0 && home.game < (int)games.size() && strcmp(games[home.game].id, "sonsoftheforest") == 0;
        ui::Menu& menu = sn1Open ? sn1Menu : sotfOpen ? sotfMenu : mainMenu;
        ULONGLONG now = GetTickCount64();
        if (now - lastPoll >= 1000)
        {
            lastPoll = now;
            home.state = inject::State(home.paths);
            home.sn1State = gameinject::State(gameinject::kSubnautica);
            home.sotfState = gameinject::State(gameinject::kSonsOfTheForest);
            for (int gi = 0; gi < 3; ++gi)
            {
                inject::GameState st = gi == 1 ? home.sn1State : gi == 2 ? home.sotfState : home.state;
                bool found = gi == 1 ? home.sn1Paths.found : gi == 2 ? home.sotfPaths.found : home.paths.found;
                Watch& wt = watch[gi];
                if (wt.last == inject::GameState::Injected && st == inject::GameState::Running)
                    menu.Notify("The menu was unloaded from the game", ICON_INFO);
                wt.last = st;
                if (st == inject::GameState::NotRunning)
                {
                    wt.since = 0;
                    wt.tried = false;
                }
                else if (wt.since == 0)
                    wt.since = now;
                if (st == inject::GameState::Injected)
                    wt.tried = true;
                if (loader.s.autoInject && !wt.tried && !home.busy && !home.injectNow && found && st == inject::GameState::Running && now - wt.since >= 30000)
                {
                    wt.tried = true;
                    home.injectNow = true;
                    home.injectGame = gi;
                    menu.Notify("Injecting the menu automatically", ICON_ZAP);
                }
            }
        }
        {
            bool inGame = home.game >= 0 && home.game < (int)games.size();
            bool sn2 = inGame && strcmp(games[home.game].id, "subnautica2") == 0;
            menu.showGameTabs = sn2 || sn1Open || sotfOpen;
            menu.homeName = inGame ? games[home.game].name : "Games";
            menu.homeIcon = inGame ? ICON_PLAY : ICON_LAYERS;
            menu.backName = inGame ? (mainMenu.homeIndex == 1 ? "My games" : "All games") : nullptr;
            if (inGame)
                mainMenu.homeTabs.clear();
            else if (mainMenu.homeTabs.empty())
                mainMenu.homeTabs.push_back({ "My games", ICON_GAMEPAD_2 });
            const core::Model& keyModel = sn1Open ? sn1Model : sotfOpen ? sotfModel : model;
            home.openKey = core::KeyName(keyModel.keyOpen);
            home.showSoon = loader.s.showSoon;
            home.installedFirst = loader.s.installedFirst;
            ui::T().accent = ui::AccentColor(loader.s.accent);
            launcher::update::Status us = launcher::update::Get();
            if (us.checked && us.newer && !updateAsked && !downloading)
            {
                updateAsked = true;
                menu.Ask("Update available",
                    "Fathom " + us.latest + " is out. You have " + loader.version + ". Update now? Fathom closes and the installer opens.",
                    "Update", [&] { downloading = true; launcher::update::Download(); });
            }
            loader.updateBusy = us.busy;
            loader.updateReady = us.checked && us.newer;
            if (downloading)
                loader.updateText = us.busy ? "Downloading the installer\xe2\x80\xa6" : us.installerReady ? "Opening the installer\xe2\x80\xa6" : "The download did not work. Try again later.";
            else if (us.busy)
                loader.updateText = "Checking for updates\xe2\x80\xa6";
            else if (us.failed)
                loader.updateText = "Could not reach GitHub. Check your internet.";
            else if (us.checked)
                loader.updateText = us.newer ? "Version " + us.latest + " is out" : "You have the newest version";
            if (downloading && !us.busy)
            {
                downloading = false;
                if (us.installerReady)
                {
                    ShellExecuteW(nullptr, L"open", us.installer.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    done = true;
                }
            }
            home.unloadKey = core::KeyName(keyModel.keyUnload);
            if (sn2)
            {
                menu.status = home.state == inject::GameState::Injected ? "Menu active in game" : home.state == inject::GameState::Running ? "Game running" : "Game not running";
                menu.statusGood = home.state != inject::GameState::NotRunning;
            }
            else if (sn1Open)
            {
                menu.status = home.sn1State == inject::GameState::Injected ? "Menu active in game" : home.sn1State == inject::GameState::Running ? "Game running" : "Game not running";
                menu.statusGood = home.sn1State != inject::GameState::NotRunning;
            }
            else if (sotfOpen)
            {
                menu.status = home.sotfState == inject::GameState::Injected ? "Menu active in game" : home.sotfState == inject::GameState::Running ? "Game running" : "Game not running";
                menu.statusGood = home.sotfState != inject::GameState::NotRunning;
            }
            else
            {
                menu.status = inGame ? "Coming soon" : "Pick a game";
                menu.statusGood = false;
            }
            launcher::art::Image img;
            for (int uploads = 0; uploads < 4 && launcher::art::Take(img); ++uploads)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE cpu;
                D3D12_GPU_DESCRIPTOR_HANDLE gpu;
                g_pd3dSrvDescHeapAlloc.Alloc(&cpu, &gpu);
                ID3D12Resource* tex = dx12::CreateTexture(g_pd3dDevice, g_pd3dCommandQueue, img.rgba.data(), img.width, img.height, cpu);
                if (!tex)
                {
                    g_pd3dSrvDescHeapAlloc.Free(cpu, gpu);
                    continue;
                }
                artTex.push_back({ tex, cpu, gpu });
                home.art[launcher::ArtKey(img.appId, img.kind)] = { (ImTextureID)gpu.ptr, (float)img.width, (float)img.height };
            }
        }
        if (!home.logPath.empty())
        {
            logView.Open(home.logPath, home.logTitle);
            home.logPath.clear();
        }
        if (home.startNow)
        {
            home.startNow = false;
            if (sn1Open)
                gameinject::StartGame(gameinject::kSubnautica);
            else if (sotfOpen)
                gameinject::StartGame(gameinject::kSonsOfTheForest);
            else
                inject::StartGame();
            menu.Notify(sn1Open ? "Starting Subnautica\xe2\x80\xa6" : sotfOpen ? "Starting Sons of the Forest\xe2\x80\xa6" : "Starting Subnautica 2\xe2\x80\xa6", ICON_PLAY);
        }
        if (home.injectNow && !home.busy)
        {
            home.injectNow = false;
            home.busy = true;
            int target = home.injectGame;
            if (target == 1)
            {
                core::SaveSettings(sn1Model);
                sn1LastText = core::SettingsText(sn1Model);
                sn1KnownTime = FileTime(sn1SettingsPath);
            }
            else if (target == 2)
            {
                core::SaveSettings(sotfModel);
                sotfLastText = core::SettingsText(sotfModel);
                sotfKnownTime = FileTime(sotfSettingsPath);
            }
            else
            {
                core::SaveSettings(model);
                lastText = core::SettingsText(model);
                knownTime = FileTime(settingsPath);
            }
            workerDone = false;
            inject::Paths paths = target == 1 ? home.sn1Paths : target == 2 ? home.sotfPaths : home.paths;
            std::string key = core::KeyName(target == 2 ? sotfModel.keyOpen : sn1Model.keyOpen);
            worker = std::thread([&, paths, target, key] {
                workerResult = target == 1 ? gameinject::Inject(gameinject::kSubnautica, paths, workerOk, key)
                             : target == 2 ? gameinject::Inject(gameinject::kSonsOfTheForest, paths, workerOk, key)
                                           : inject::Inject(paths, workerOk);
                workerDone = true;
            });
        }
        if (home.busy && workerDone)
        {
            worker.join();
            home.busy = false;
            menu.Notify(workerResult, workerOk ? ICON_CHECK : ICON_TRIANGLE_ALERT);
            lastPoll = 0;
            if (workerOk && loader.s.closeAfterInject)
                closeAt = now + 2000;
        }
        if (now - lastSync >= 500)
        {
            lastSync = now;
            auto sync = [](core::Model& m, ui::Menu& mm, const std::wstring& path, std::string& last, std::filesystem::file_time_type& known) {
                std::string text = core::SettingsText(m);
                auto t = FileTime(path);
                if (text != last)
                {
                    core::SaveSettings(m);
                    last = text;
                    known = FileTime(path);
                }
                else if (t != known && t != std::filesystem::file_time_type::min())
                {
                    core::LoadSettings(m);
                    mm.SyncFromModel();
                    last = core::SettingsText(m);
                    known = t;
                }
            };
            sync(model, mainMenu, settingsPath, lastText, knownTime);
            sync(sn1Model, sn1Menu, sn1SettingsPath, sn1LastText, sn1KnownTime);
            sync(sotfModel, sotfMenu, sotfSettingsPath, sotfLastText, sotfKnownTime);
        }
        if (closeAt && now >= closeAt)
            done = true;
        {
            std::string text = launcher::PrefsText(loader.s);
            if (text != prefsText)
            {
                prefsText = text;
                launcher::SavePrefs(loader.s, prefsPath);
                if (loader.s.startWithWindows != applied.startWithWindows && !launcher::SetStartup(loader.s.startWithWindows))
                {
                    loader.s.startWithWindows = applied.startWithWindows;
                    menu.Notify("Windows did not allow the startup change", ICON_TRIANGLE_ALERT);
                }
                if (loader.s.onTop != applied.onTop)
                    SetWindowPos(hwnd, loader.s.onTop ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                if (loader.s.webArt && !applied.webArt)
                    artRefresh = true;
                applied = loader.s;
            }
        }
        menu.uiScale = loader.s.scale;
        if (std::fabs(shownScale - loader.s.scale) > 0.001f)
        {
            FitWindow(hwnd, shownScale, loader.s.scale);
            shownScale = loader.s.scale;
            g_uiScale = shownScale;
        }
        if (artRefresh)
        {
            artRefresh = false;
            launcher::art::Stop();
            WaitForPendingOperations();
            for (const ArtTex& t : artTex)
            {
                t.tex->Release();
                g_pd3dSrvDescHeapAlloc.Free(t.cpu, t.gpu);
            }
            artTex.clear();
            home.art.clear();
            if (artWipe)
            {
                std::error_code ec;
                for (const auto& e : std::filesystem::directory_iterator(artDir, ec))
                    std::filesystem::remove(e.path(), ec);
            }
            artWipe = false;
            launcher::art::Start(wantedArt, artDir, loader.s.webArt);
        }
        if (done)
            break;

        if ((g_SwapChainOccluded && g_pSwapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) || ::IsIconic(hwnd))
        {
            ::Sleep(10);
            continue;
        }
        g_SwapChainOccluded = false;

        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        menu.Frame();
        logView.Draw();
        model.requestUnload = false;
        sn1Model.requestUnload = false;
        sotfModel.requestUnload = false;
        ImGui::Render();

        FrameContext* frameCtx = WaitForNextFrameContext();
        UINT backBufferIdx = g_pSwapChain->GetCurrentBackBufferIndex();
        frameCtx->CommandAllocator->Reset();

        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Flags                  = D3D12_RESOURCE_BARRIER_FLAG_NONE;
        barrier.Transition.pResource   = g_mainRenderTargetResource[backBufferIdx];
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
        g_pd3dCommandList->Reset(frameCtx->CommandAllocator, nullptr);
        g_pd3dCommandList->ResourceBarrier(1, &barrier);

        const float clear_color_with_alpha[4] = { 0.047f, 0.047f, 0.059f, 1.0f };
        g_pd3dCommandList->ClearRenderTargetView(g_mainRenderTargetDescriptor[backBufferIdx], clear_color_with_alpha, 0, nullptr);
        g_pd3dCommandList->OMSetRenderTargets(1, &g_mainRenderTargetDescriptor[backBufferIdx], FALSE, nullptr);
        g_pd3dCommandList->SetDescriptorHeaps(1, &g_pd3dSrvDescHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pd3dCommandList);
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
        g_pd3dCommandList->ResourceBarrier(1, &barrier);
        g_pd3dCommandList->Close();

        g_pd3dCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&g_pd3dCommandList);
        g_pd3dCommandQueue->Signal(g_fence, ++g_fenceLastSignaledValue);
        frameCtx->FenceValue = g_fenceLastSignaledValue;

        HRESULT hr = g_pSwapChain->Present(1, 0);
        g_SwapChainOccluded = (hr == DXGI_STATUS_OCCLUDED);
        g_frameIndex++;
    }

    if (worker.joinable())
        worker.join();
    if (core::SettingsText(model) != lastText)
        core::SaveSettings(model);
    if (core::SettingsText(sn1Model) != sn1LastText)
        core::SaveSettings(sn1Model);
    if (core::SettingsText(sotfModel) != sotfLastText)
        core::SaveSettings(sotfModel);

    launcher::art::Stop();
    launcher::update::Wait();
    WaitForPendingOperations();
    for (const ArtTex& t : artTex)
        t.tex->Release();
    if (iconTex)
        iconTex->Release();
    if (logoTex)
        logoTex->Release();

    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();

    CleanupDeviceD3D();
    ::DestroyWindow(hwnd);
    ::UnregisterClassW(wc.lpszClassName, wc.hInstance);
    if (single)
        CloseHandle(single);

    return 0;
}

bool CreateDeviceD3D(HWND hWnd)
{
    DXGI_SWAP_CHAIN_DESC1 sd;
    {
        ZeroMemory(&sd, sizeof(sd));
        sd.BufferCount = APP_NUM_BACK_BUFFERS;
        sd.Width = 0;
        sd.Height = 0;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.SampleDesc.Count = 1;
        sd.SampleDesc.Quality = 0;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sd.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
        sd.Scaling = DXGI_SCALING_STRETCH;
        sd.Stereo = FALSE;
    }

#ifdef DX12_ENABLE_DEBUG_LAYER
    ID3D12Debug* pdx12Debug = nullptr;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&pdx12Debug))))
        pdx12Debug->EnableDebugLayer();
#endif

    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    if (D3D12CreateDevice(nullptr, featureLevel, IID_PPV_ARGS(&g_pd3dDevice)) != S_OK)
        return false;

#ifdef DX12_ENABLE_DEBUG_LAYER
    if (pdx12Debug != nullptr)
    {
        ID3D12InfoQueue* pInfoQueue = nullptr;
        g_pd3dDevice->QueryInterface(IID_PPV_ARGS(&pInfoQueue));
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
        pInfoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, true);

        const int D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ = 1424;
        D3D12_MESSAGE_ID disabledMessages[] = { (D3D12_MESSAGE_ID)D3D12_MESSAGE_ID_FENCE_ZERO_WAIT_ };
        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = 1;
        filter.DenyList.pIDList = disabledMessages;
        pInfoQueue->AddStorageFilterEntries(&filter);

        pInfoQueue->Release();
        pdx12Debug->Release();
    }
#endif

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = APP_NUM_BACK_BUFFERS;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dRtvDescHeap)) != S_OK)
            return false;

        SIZE_T rtvDescriptorSize = g_pd3dDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pd3dRtvDescHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
        {
            g_mainRenderTargetDescriptor[i] = rtvHandle;
            rtvHandle.ptr += rtvDescriptorSize;
        }
    }

    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = APP_SRV_HEAP_SIZE;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (g_pd3dDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pd3dSrvDescHeap)) != S_OK)
            return false;
        g_pd3dSrvDescHeapAlloc.Create(g_pd3dDevice, g_pd3dSrvDescHeap);
    }

    {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        desc.NodeMask = 1;
        if (g_pd3dDevice->CreateCommandQueue(&desc, IID_PPV_ARGS(&g_pd3dCommandQueue)) != S_OK)
            return false;
    }

    for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
        if (g_pd3dDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_frameContext[i].CommandAllocator)) != S_OK)
            return false;

    if (g_pd3dDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_frameContext[0].CommandAllocator, nullptr, IID_PPV_ARGS(&g_pd3dCommandList)) != S_OK ||
        g_pd3dCommandList->Close() != S_OK)
        return false;

    if (g_pd3dDevice->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence)) != S_OK)
        return false;

    g_fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (g_fenceEvent == nullptr)
        return false;

    {
        IDXGIFactory5* dxgiFactory = nullptr;
        IDXGISwapChain1* swapChain1 = nullptr;
        if (CreateDXGIFactory1(IID_PPV_ARGS(&dxgiFactory)) != S_OK)
            return false;

        BOOL allow_tearing = FALSE;
        dxgiFactory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow_tearing, sizeof(allow_tearing));
        g_SwapChainTearingSupport = (allow_tearing == TRUE);
        if (g_SwapChainTearingSupport)
            sd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

        if (dxgiFactory->CreateSwapChainForHwnd(g_pd3dCommandQueue, hWnd, &sd, nullptr, nullptr, &swapChain1) != S_OK)
            return false;
        if (swapChain1->QueryInterface(IID_PPV_ARGS(&g_pSwapChain)) != S_OK)
            return false;
        if (g_SwapChainTearingSupport)
            dxgiFactory->MakeWindowAssociation(hWnd, DXGI_MWA_NO_ALT_ENTER);

        swapChain1->Release();
        dxgiFactory->Release();
        g_pSwapChain->SetMaximumFrameLatency(APP_NUM_BACK_BUFFERS);
        g_hSwapChainWaitableObject = g_pSwapChain->GetFrameLatencyWaitableObject();
    }

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D()
{
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->SetFullscreenState(false, nullptr); g_pSwapChain->Release(); g_pSwapChain = nullptr; }
    if (g_hSwapChainWaitableObject != nullptr) { CloseHandle(g_hSwapChainWaitableObject); }
    for (UINT i = 0; i < APP_NUM_FRAMES_IN_FLIGHT; i++)
        if (g_frameContext[i].CommandAllocator) { g_frameContext[i].CommandAllocator->Release(); g_frameContext[i].CommandAllocator = nullptr; }
    if (g_pd3dCommandQueue) { g_pd3dCommandQueue->Release(); g_pd3dCommandQueue = nullptr; }
    if (g_pd3dCommandList) { g_pd3dCommandList->Release(); g_pd3dCommandList = nullptr; }
    if (g_pd3dRtvDescHeap) { g_pd3dRtvDescHeap->Release(); g_pd3dRtvDescHeap = nullptr; }
    if (g_pd3dSrvDescHeap) { g_pd3dSrvDescHeap->Release(); g_pd3dSrvDescHeap = nullptr; }
    if (g_fence) { g_fence->Release(); g_fence = nullptr; }
    if (g_fenceEvent) { CloseHandle(g_fenceEvent); g_fenceEvent = nullptr; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = nullptr; }

#ifdef DX12_ENABLE_DEBUG_LAYER
    IDXGIDebug1* pDebug = nullptr;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&pDebug))))
    {
        pDebug->ReportLiveObjects(DXGI_DEBUG_ALL, DXGI_DEBUG_RLO_SUMMARY);
        pDebug->Release();
    }
#endif
}

void CreateRenderTarget()
{
    for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
    {
        ID3D12Resource* pBackBuffer = nullptr;
        g_pSwapChain->GetBuffer(i, IID_PPV_ARGS(&pBackBuffer));
        g_pd3dDevice->CreateRenderTargetView(pBackBuffer, nullptr, g_mainRenderTargetDescriptor[i]);
        g_mainRenderTargetResource[i] = pBackBuffer;
    }
}

void CleanupRenderTarget()
{
    WaitForPendingOperations();

    for (UINT i = 0; i < APP_NUM_BACK_BUFFERS; i++)
        if (g_mainRenderTargetResource[i]) { g_mainRenderTargetResource[i]->Release(); g_mainRenderTargetResource[i] = nullptr; }
}

void WaitForPendingOperations()
{
    g_pd3dCommandQueue->Signal(g_fence, ++g_fenceLastSignaledValue);

    g_fence->SetEventOnCompletion(g_fenceLastSignaledValue, g_fenceEvent);
    ::WaitForSingleObject(g_fenceEvent, INFINITE);
}

FrameContext* WaitForNextFrameContext()
{
    FrameContext* frame_context = &g_frameContext[g_frameIndex % APP_NUM_FRAMES_IN_FLIGHT];
    if (g_fence->GetCompletedValue() < frame_context->FenceValue)
    {
        g_fence->SetEventOnCompletion(frame_context->FenceValue, g_fenceEvent);
        HANDLE waitableObjects[] = { g_hSwapChainWaitableObject, g_fenceEvent };
        ::WaitForMultipleObjects(2, waitableObjects, TRUE, INFINITE);
    }
    else
        ::WaitForSingleObject(g_hSwapChainWaitableObject, INFINITE);

    return frame_context;
}

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_SIZE:
        if (g_pd3dDevice != nullptr && wParam != SIZE_MINIMIZED)
        {
            CleanupRenderTarget();
            DXGI_SWAP_CHAIN_DESC1 desc = {};
            g_pSwapChain->GetDesc1(&desc);
            HRESULT result = g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), desc.Format, desc.Flags);
            IM_ASSERT(SUCCEEDED(result) && "Failed to resize swapchain.");
            CreateRenderTarget();
        }
        return 0;
    case WM_GETMINMAXINFO:
    {
        RECT r = { 0, 0, (LONG)std::lround(820.0f * g_uiScale), (LONG)std::lround(540.0f * g_uiScale) };
        AdjustWindowRectEx(&r, (DWORD)GetWindowLongPtrW(hWnd, GWL_STYLE), FALSE, 0);
        MINMAXINFO* mm = (MINMAXINFO*)lParam;
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU)
            return 0;
        break;
    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;
    }
    return ::DefWindowProcW(hWnd, msg, wParam, lParam);
}


