#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wininet.h>
#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}
#include <gdiplus.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

const wchar_t* kBase = L"https://github.com/Sniko94pig/Fathom/releases/latest/download/";
const wchar_t* kFiles[] = {L"Fathom.exe", L"fathom.dll", L"fathom_sn1.dll", L"fathom_sotf.dll"};
const wchar_t* kKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\Fathom";
const wchar_t* kUninstaller = L"Uninstall Fathom.exe";
const wchar_t* kSetup = L"FathomSetup.exe";
const wchar_t* kGameExe = L"Subnautica2-Win64-Shipping.exe";
const UINT WM_REFRESH = WM_APP + 1;
const int ID_MAIN = 101, ID_SECOND = 102, ID_CHECK = 103;

enum class Mode { Install, Uninstall };
enum Stage { Ready, Working, Done, Failed };

struct Options {
    Mode mode = Mode::Install;
    bool quiet = false, shortcut = true, registry = true, relaunched = false;
    std::wstring dir, log, snapshot;
} opt;

HWND wnd = nullptr, btnMain = nullptr, btnSecond = nullptr, btnCheck = nullptr;
HFONT fTitle = nullptr, fStatus = nullptr, fBody = nullptr, fButton = nullptr;
float scale = 1.0f;
std::atomic<int> stage{Ready};
std::atomic<float> progress{0.0f};
std::mutex textLock;
std::wstring status, detail;
bool removeGameData = false;
bool alreadyInstalled = false;
std::thread worker;

const COLORREF cBg = RGB(12, 12, 15), cText = RGB(246, 246, 248), cDim = RGB(143, 143, 156), cAccent = RGB(255, 70, 85),
               cAccentText = RGB(255, 255, 255), cGhost = RGB(29, 29, 36), cTrack = RGB(42, 42, 51), cLine = RGB(48, 48, 58);

int S(int v) { return (int)(v * scale + 0.5f); }

void Log(const std::wstring& line) {
    if (opt.log.empty()) return;
    std::ofstream out(fs::path(opt.log), std::ios::app | std::ios::binary);
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), s.data(), n, nullptr, nullptr);
    out << s << "\r\n";
}

void Show(int st, const std::wstring& s, const std::wstring& d) {
    {
        std::lock_guard<std::mutex> g(textLock);
        status = s;
        detail = d;
    }
    stage = st;
    Log(s + L" - " + d);
    if (wnd) PostMessageW(wnd, WM_REFRESH, 0, 0);
}

std::wstring Known(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) out = p;
    if (p) CoTaskMemFree(p);
    return out;
}

std::wstring SelfPath() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    return std::wstring(buf, n);
}

bool Running(const wchar_t* exe) {
    bool found = false;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{sizeof(pe)};
    for (BOOL ok = Process32FirstW(snap, &pe); ok && !found; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, exe) != 0 || pe.th32ProcessID == GetCurrentProcessId()) continue;
        found = true;
    }
    CloseHandle(snap);
    return found;
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
            if (!std::regex_search(line, m, re)) continue;
            std::string p = m[1].str(), clean;
            for (size_t i = 0; i < p.size(); ++i) {
                if (p[i] == '\\' && i + 1 < p.size() && p[i + 1] == '\\') ++i;
                clean.push_back(p[i]);
            }
            int n = MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), (int)clean.size(), nullptr, 0);
            std::wstring w(n, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, clean.c_str(), (int)clean.size(), w.data(), n);
            libs.push_back(w);
        }
    }
    for (const fs::path& lib : libs) {
        fs::path w = lib / L"steamapps" / L"common" / L"Subnautica2" / L"Subnautica2" / L"Binaries" / L"Win64";
        std::error_code ec;
        if (fs::exists(w / kGameExe, ec)) return w.wstring();
    }
    return {};
}

class Callback : public IBindStatusCallback {
public:
    int index = 0, count = 1;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (riid == IID_IUnknown || riid == IID_IBindStatusCallback) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE OnStartBinding(DWORD, IBinding*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetPriority(LONG*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE OnLowResource(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG done, ULONG total, ULONG, LPCWSTR) override {
        if (total > 0) {
            progress = (index + (float)done / (float)total) / (float)count;
            if (wnd) PostMessageW(wnd, WM_REFRESH, 0, 0);
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

bool LooksLikeProgram(const fs::path& file) {
    std::error_code ec;
    if (fs::file_size(file, ec) < 50000 || ec) return false;
    std::ifstream in(file, std::ios::binary);
    char mz[2] = {};
    in.read(mz, 2);
    return mz[0] == 'M' && mz[1] == 'Z';
}

bool Replace(const fs::path& from, const fs::path& to) {
    for (int i = 0; i < 30; ++i) {
        if (MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return true;
        Sleep(100);
    }
    return false;
}

bool RemoveFile(const fs::path& file) {
    std::error_code ec;
    for (int i = 0; i < 50; ++i) {
        if (!fs::exists(file, ec)) return true;
        if (DeleteFileW(file.c_str())) return true;
        Sleep(100);
    }
    return !fs::exists(file, ec);
}

bool MakeShortcut(const fs::path& target, const fs::path& link) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) || !sl) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(target.parent_path().c_str());
    sl->SetIconLocation(target.c_str(), 0);
    sl->SetDescription(L"Fathom mod menu for Subnautica 2");
    IPersistFile* pf = nullptr;
    bool ok = SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf && SUCCEEDED(pf->Save(link.c_str(), TRUE));
    if (pf) pf->Release();
    sl->Release();
    return ok;
}

void RefreshIcons(const fs::path& dir, const fs::path& link) {
    for (const fs::path& p : {dir / L"Fathom.exe", dir / kUninstaller, dir / kSetup, link})
        if (!p.empty()) SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, p.c_str(), nullptr);
    SHChangeNotify(SHCNE_UPDATEDIR, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, dir.c_str(), nullptr);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST | SHCNF_FLUSHNOWAIT, nullptr, nullptr);
}

std::wstring ShortcutTarget(const fs::path& link) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW, (void**)&sl)) || !sl) return {};
    std::wstring out;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_IPersistFile, (void**)&pf)) && pf && SUCCEEDED(pf->Load(link.c_str(), STGM_READ))) {
        wchar_t buf[MAX_PATH * 2] = {};
        if (SUCCEEDED(sl->GetPath(buf, MAX_PATH * 2, nullptr, 0))) out = buf;
    }
    if (pf) pf->Release();
    sl->Release();
    return out;
}

void SetValue(HKEY k, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}

void SetValue(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof(v)); }

bool Register(const fs::path& dir) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kKey, 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) != ERROR_SUCCESS) return false;
    DWORD kb = 0;
    std::error_code ec;
    for (const wchar_t* f : kFiles) kb += (DWORD)(fs::file_size(dir / f, ec) / 1024);
    SetValue(k, L"DisplayName", L"Fathom");
    SetValue(k, L"DisplayIcon", (dir / L"Fathom.exe").wstring());
    SetValue(k, L"UninstallString", L"\"" + (dir / kUninstaller).wstring() + L"\"");
    SetValue(k, L"InstallLocation", dir.wstring());
    SetValue(k, L"Publisher", L"Sniko94pig");
    SetValue(k, L"URLInfoAbout", L"https://github.com/Sniko94pig/Fathom");
    SetValue(k, L"NoModify", 1);
    SetValue(k, L"NoRepair", 1);
    SetValue(k, L"EstimatedSize", kb);
    RegCloseKey(k);
    return true;
}

bool Install() {
    fs::path dir = opt.dir;
    std::error_code ec;
    if (Running(L"Fathom.exe")) {
        Show(Failed, L"Close Fathom first", L"Fathom is open. Close it, then press Try again.");
        return false;
    }
    fs::create_directories(dir, ec);
    if (!fs::is_directory(dir, ec)) {
        Show(Failed, L"Could not make the folder", dir.wstring());
        return false;
    }
    Callback cb;
    cb.count = (int)(sizeof(kFiles) / sizeof(kFiles[0]));
    for (int i = 0; i < cb.count; ++i) {
        std::wstring url = std::wstring(kBase) + kFiles[i];
        fs::path part = dir / (std::wstring(kFiles[i]) + L".download");
        Show(Working, L"Downloading Fathom...", std::wstring(L"Getting ") + kFiles[i] + L" from GitHub");
        DeleteUrlCacheEntryW(url.c_str());
        cb.index = i;
        HRESULT hr = URLDownloadToFileW(nullptr, url.c_str(), part.c_str(), 0, &cb);
        if (FAILED(hr) || !LooksLikeProgram(part)) {
            fs::remove(part, ec);
            Show(Failed, L"The download did not work",
                 L"Check your internet connection and press Try again. If it keeps failing, GitHub may be down for a moment.");
            return false;
        }
    }
    progress = 1.0f;
    Show(Working, L"Setting up...", L"Putting the files in place");
    fs::create_directories(dir / L"save-backups", ec);
    fs::create_directories(dir / L"old-builds", ec);
    if (fs::exists(dir / L"Fathom.exe", ec)) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf_s(stamp, L"%04d-%02d-%02d_%02d-%02d-%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        fs::path keep = dir / L"old-builds" / stamp;
        fs::create_directories(keep, ec);
        for (const wchar_t* f : kFiles) CopyFileW((dir / f).c_str(), (keep / f).c_str(), FALSE);
    }
    for (const wchar_t* f : kFiles)
        if (!Replace(dir / (std::wstring(f) + L".download"), dir / f)) {
            Show(Failed, L"Could not replace the old files", L"Close Fathom and Subnautica 2, then press Try again.");
            return false;
        }
    fs::path self = SelfPath(), uninstaller = dir / kUninstaller;
    if (!fs::equivalent(self, uninstaller, ec)) CopyFileW(self.c_str(), uninstaller.c_str(), FALSE);
    fs::path setupCopy = dir / kSetup;
    if (!fs::equivalent(self, setupCopy, ec)) CopyFileW(self.c_str(), setupCopy.c_str(), FALSE);
    fs::path link;
    if (opt.shortcut) {
        std::wstring desktop = Known(FOLDERID_Desktop);
        if (!desktop.empty()) link = fs::path(desktop) / L"Fathom.lnk";
        if (link.empty() || !MakeShortcut(dir / L"Fathom.exe", link)) Log(L"could not make the desktop shortcut");
    }
    if (opt.registry) Register(dir);
    RefreshIcons(dir, link);
    Show(Done, L"Fathom is installed",
         L"Open it with the Fathom shortcut on your desktop. To remove it later, use Uninstall Fathom in the Fathom folder or in Windows Settings > Apps.");
    return true;
}

bool Uninstall() {
    fs::path dir = opt.dir;
    std::error_code ec;
    if (Running(kGameExe) || Running(L"Fathom.exe")) {
        Show(Failed, L"Close Subnautica 2 and Fathom first", L"Both have to be closed before Fathom can be removed. Then press Try again.");
        return false;
    }
    Show(Working, L"Removing Fathom...", dir.wstring());
    progress = 0.3f;
    bool ok = true;
    for (const wchar_t* f : kFiles) {
        ok = RemoveFile(dir / f) && ok;
        ok = RemoveFile(dir / (std::wstring(f) + L".download")) && ok;
    }
    fs::path uninstaller = dir / kUninstaller;
    if (!fs::equivalent(SelfPath(), uninstaller, ec)) ok = RemoveFile(uninstaller) && ok;
    fs::path setupCopy = dir / kSetup;
    if (fs::exists(setupCopy, ec) && !fs::equivalent(SelfPath(), setupCopy, ec)) ok = RemoveFile(setupCopy) && ok;
    fs::remove_all(dir / L"old-builds", ec);
    fs::remove_all(dir / L"art", ec);
    fs::remove(dir / L"launcher.json", ec);
    fs::remove(dir / L"launcher.json.tmp", ec);
    if (fs::is_directory(dir / L"save-backups", ec) && fs::is_empty(dir / L"save-backups", ec)) fs::remove(dir / L"save-backups", ec);
    if (removeGameData) {
        std::wstring win64 = SteamWin64();
        if (!win64.empty()) fs::remove_all(fs::path(win64) / L"fathom", ec);
    }
    progress = 0.7f;
    if (opt.shortcut) {
        std::wstring desktop = Known(FOLDERID_Desktop);
        fs::path link = fs::path(desktop) / L"Fathom.lnk";
        std::wstring target = ShortcutTarget(link);
        if (!desktop.empty() && !target.empty() && fs::path(target).parent_path() == dir) DeleteFileW(link.c_str());
    }
    if (opt.registry) {
        RegDeleteTreeW(HKEY_CURRENT_USER, kKey);
        HKEY run;
        if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &run) ==
            ERROR_SUCCESS) {
            wchar_t buf[1024];
            DWORD size = sizeof(buf), type = 0;
            if (RegQueryValueExW(run, L"Fathom", nullptr, &type, (BYTE*)buf, &size) == ERROR_SUCCESS && type == REG_SZ) {
                buf[std::min<size_t>(size / sizeof(wchar_t), 1023)] = 0;
                std::wstring cmd = buf, exe = (dir / L"Fathom.exe").wstring();
                if (_wcsnicmp(cmd.c_str() + (cmd[0] == L'"' ? 1 : 0), exe.c_str(), exe.size()) == 0) RegDeleteValueW(run, L"Fathom");
            }
            RegCloseKey(run);
        }
    }
    progress = 1.0f;
    std::wstring left;
    if (fs::is_directory(dir, ec)) {
        if (fs::is_empty(dir, ec)) fs::remove(dir, ec);
        else left = L" Your save backups and other files you added are still in " + dir.wstring() + L".";
    }
    if (!ok) {
        Show(Failed, L"Some files could not be removed", L"Close Fathom and Subnautica 2, then press Try again.");
        return false;
    }
    Show(Done, L"Fathom was removed", L"The program and the desktop shortcut are gone." + left);
    return true;
}

void Start() {
    if (worker.joinable()) worker.join();
    progress = 0.0f;
    stage = Working;
    if (wnd) PostMessageW(wnd, WM_REFRESH, 0, 0);
    worker = std::thread([] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (opt.mode == Mode::Install) Install();
        else Uninstall();
        CoUninitialize();
    });
}

void Fill(HDC dc, RECT r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void Para(HDC dc, RECT r, COLORREF fill, COLORREF line) {
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    float dx = (float)(r.bottom - r.top) * 0.18f;
    Gdiplus::PointF pts[4] = {Gdiplus::PointF(r.left + dx, (float)r.top), Gdiplus::PointF((float)r.right, (float)r.top),
                              Gdiplus::PointF(r.right - dx, (float)r.bottom), Gdiplus::PointF((float)r.left, (float)r.bottom)};
    Gdiplus::SolidBrush brush(Gdiplus::Color(255, GetRValue(fill), GetGValue(fill), GetBValue(fill)));
    g.FillPolygon(&brush, pts, 4);
    if (line != fill) {
        Gdiplus::PointF in[4] = {Gdiplus::PointF(r.left + dx + 0.5f, r.top + 0.5f), Gdiplus::PointF(r.right - 0.5f, r.top + 0.5f),
                                 Gdiplus::PointF(r.right - dx - 0.5f, r.bottom - 0.5f), Gdiplus::PointF(r.left + 0.5f, r.bottom - 0.5f)};
        Gdiplus::Pen pen(Gdiplus::Color(255, GetRValue(line), GetGValue(line), GetBValue(line)), 1.0f);
        g.DrawPolygon(&pen, in, 4);
    }
}

void Layout() {
    int st = stage;
    bool uninstall = opt.mode == Mode::Uninstall;
    const wchar_t* main = L"";
    const wchar_t* second = L"";
    if (st == Ready) {
        main = uninstall ? L"Remove" : alreadyInstalled ? L"Update" : L"Install";
        second = L"Cancel";
    } else if (st == Done) {
        main = uninstall ? L"Close" : L"Open Fathom";
        second = uninstall ? L"" : L"Close";
    } else if (st == Failed) {
        main = L"Try again";
        second = L"Close";
    }
    SetWindowTextW(btnMain, main);
    SetWindowTextW(btnSecond, second);
    ShowWindow(btnMain, st == Working ? SW_HIDE : SW_SHOW);
    ShowWindow(btnSecond, st == Working || second[0] == 0 ? SW_HIDE : SW_SHOW);
    ShowWindow(btnCheck, uninstall && st == Ready ? SW_SHOW : SW_HIDE);
    InvalidateRect(wnd, nullptr, FALSE);
    InvalidateRect(btnMain, nullptr, FALSE);
    InvalidateRect(btnSecond, nullptr, FALSE);
    InvalidateRect(btnCheck, nullptr, FALSE);
}

void Paint(HDC target, RECT rc) {
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bmp = CreateCompatibleBitmap(target, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    HBRUSH bg = CreateSolidBrush(cBg);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    RECT top{0, 0, rc.right, S(3)};
    Fill(dc, top, cAccent);
    RECT logo{S(32), S(30), S(32) + S(44), S(30) + S(44)};
    if (HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1), IMAGE_ICON, S(44), S(44), 0)) {
        DrawIconEx(dc, logo.left, logo.top, ico, S(44), S(44), 0, nullptr, DI_NORMAL);
        DestroyIcon(ico);
    }
    SelectObject(dc, fTitle);
    SetTextColor(dc, cText);
    RECT title{S(90), S(30), rc.right - S(32), S(58)};
    DrawTextW(dc, L"FATHOM", -1, &title, DT_LEFT | DT_SINGLELINE);
    SelectObject(dc, fBody);
    SetTextColor(dc, cDim);
    RECT sub{S(90), S(58), rc.right - S(32), S(78)};
    DrawTextW(dc, L"Mod loader for your games", -1, &sub, DT_LEFT | DT_SINGLELINE);
    std::wstring s, d;
    {
        std::lock_guard<std::mutex> g(textLock);
        s = status;
        d = detail;
    }
    SelectObject(dc, fStatus);
    SetTextColor(dc, cText);
    RECT sr{S(32), S(104), rc.right - S(32), S(130)};
    DrawTextW(dc, s.c_str(), -1, &sr, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, fBody);
    SetTextColor(dc, cDim);
    RECT dr{S(32), S(136), rc.right - S(32), S(206)};
    DrawTextW(dc, d.c_str(), -1, &dr, DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS);
    if (stage == Working) {
        RECT track{S(32), S(222), rc.right - S(32), S(222) + S(6)};
        Fill(dc, track, cTrack);
        float p = std::min(1.0f, std::max(0.0f, progress.load()));
        RECT fill = track;
        fill.right = track.left + (LONG)((track.right - track.left) * p);
        if (fill.right > fill.left) Fill(dc, fill, cAccent);
    }
    BitBlt(target, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
}

void DrawButton(const DRAWITEMSTRUCT* di) {
    HDC dc = di->hDC;
    RECT r = di->rcItem;
    HBRUSH bg = CreateSolidBrush(cBg);
    FillRect(dc, &r, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    wchar_t text[128] = {};
    GetWindowTextW(di->hwndItem, text, 128);
    if (di->CtlID == ID_CHECK) {
        RECT box{r.left, r.top + (r.bottom - r.top - S(18)) / 2, r.left + S(18), r.top + (r.bottom - r.top + S(18)) / 2};
        Fill(dc, box, removeGameData ? cAccent : cGhost);
        if (!removeGameData) {
            HBRUSH lb = CreateSolidBrush(cLine);
            FrameRect(dc, &box, lb);
            DeleteObject(lb);
        }
        if (removeGameData) {
            HPEN pen = CreatePen(PS_SOLID, std::max(1, S(2)), cAccentText);
            HGDIOBJ op = SelectObject(dc, pen);
            MoveToEx(dc, box.left + S(4), box.top + S(9), nullptr);
            LineTo(dc, box.left + S(8), box.top + S(13));
            LineTo(dc, box.left + S(14), box.top + S(5));
            SelectObject(dc, op);
            DeleteObject(pen);
        }
        SelectObject(dc, fBody);
        SetTextColor(dc, cText);
        RECT tr{box.right + S(10), r.top, r.right, r.bottom};
        DrawTextW(dc, text, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        return;
    }
    bool primary = di->CtlID == ID_MAIN;
    COLORREF fill = primary ? cAccent : cGhost;
    if (pressed) fill = primary ? RGB(214, 56, 70) : RGB(40, 40, 50);
    Para(dc, r, fill, primary ? fill : cLine);
    if (primary) CharUpperW(text);
    SelectObject(dc, fButton);
    SetTextColor(dc, primary ? cAccentText : cText);
    DrawTextW(dc, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

void Snapshot(const std::wstring& file) {
    RECT rc{0, 0, S(540), S(320)};
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP bmp = CreateCompatibleBitmap(screen, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(dc, bmp);
    Paint(dc, rc);
    for (HWND b : {btnMain, btnSecond, btnCheck}) {
        if (!IsWindowVisible(b) && !(GetWindowLongW(b, GWL_STYLE) & WS_VISIBLE)) continue;
        RECT br;
        GetWindowRect(b, &br);
        MapWindowPoints(HWND_DESKTOP, wnd, (POINT*)&br, 2);
        DRAWITEMSTRUCT di{};
        di.CtlType = ODT_BUTTON;
        di.CtlID = (UINT)GetDlgCtrlID(b);
        di.hwndItem = b;
        di.hDC = dc;
        di.rcItem = RECT{0, 0, br.right - br.left, br.bottom - br.top};
        SetViewportOrgEx(dc, br.left, br.top, nullptr);
        DrawButton(&di);
        SetViewportOrgEx(dc, 0, 0, nullptr);
    }
    BITMAPINFOHEADER bi{sizeof(bi), rc.right, -rc.bottom, 1, 32, BI_RGB};
    std::vector<uint8_t> px((size_t)rc.right * rc.bottom * 4);
    GetDIBits(dc, bmp, 0, rc.bottom, px.data(), (BITMAPINFO*)&bi, DIB_RGB_COLORS);
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(bi);
    fh.bfSize = fh.bfOffBits + (DWORD)px.size();
    std::ofstream out(fs::path(file), std::ios::binary);
    out.write((const char*)&fh, sizeof(fh));
    out.write((const char*)&bi, sizeof(bi));
    out.write((const char*)px.data(), (std::streamsize)px.size());
    SelectObject(dc, old);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
}

void CleanupSelf() {
    if (!opt.relaunched) return;
    std::wstring cmd = L"cmd.exe /c ping 127.0.0.1 -n 3 > nul & del /f /q \"" + SelfPath() + L"\"";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_REFRESH:
        Layout();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        Paint(dc, rc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_DRAWITEM:
        DrawButton((const DRAWITEMSTRUCT*)lp);
        return TRUE;
    case WM_CTLCOLORBTN:
        return (LRESULT)GetStockObject(NULL_BRUSH);
    case WM_COMMAND: {
        int id = LOWORD(wp);
        int st = stage;
        if (id == ID_CHECK) {
            removeGameData = !removeGameData;
            InvalidateRect(btnCheck, nullptr, FALSE);
        } else if (id == ID_MAIN) {
            if (st == Ready || st == Failed) {
                Start();
            } else if (st == Done) {
                if (opt.mode == Mode::Install)
                    ShellExecuteW(nullptr, L"open", (fs::path(opt.dir) / L"Fathom.exe").c_str(), nullptr, opt.dir.c_str(), SW_SHOWNORMAL);
                DestroyWindow(h);
            }
        } else if (id == ID_SECOND && st != Working) {
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_CLOSE:
        if (stage == Working) return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HFONT Font(int px, int weight) {
    return CreateFontW(-S(px), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

void ParseArgs() {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--uninstall") opt.mode = Mode::Uninstall;
        else if (a == L"--quiet") opt.quiet = true;
        else if (a == L"--no-shortcut") opt.shortcut = false;
        else if (a == L"--no-registry") opt.registry = false;
        else if (a == L"--relaunched") opt.relaunched = true;
        else if (a == L"--dir" && i + 1 < argc) opt.dir = argv[++i];
        else if (a == L"--log" && i + 1 < argc) opt.log = argv[++i];
        else if (a == L"--snapshot" && i + 1 < argc) opt.snapshot = argv[++i];
    }
    if (argv) LocalFree(argv);
    fs::path self = SelfPath();
    std::wstring name = self.filename().wstring();
    if (_wcsnicmp(name.c_str(), L"Uninstall", 9) == 0) opt.mode = Mode::Uninstall;
    if (opt.dir.empty()) opt.dir = opt.mode == Mode::Uninstall ? self.parent_path().wstring() : (fs::path(Known(FOLDERID_Profile)) / L"Fathom").wstring();
}

}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ParseArgs();
    if (opt.mode == Mode::Uninstall && !opt.quiet && !opt.relaunched) {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path copy = fs::path(tmp) / L"FathomUninstall.exe";
        if (CopyFileW(SelfPath().c_str(), copy.c_str(), FALSE)) {
            std::wstring args = L"--uninstall --relaunched --dir \"" + opt.dir + L"\"";
            if (!opt.shortcut) args += L" --no-shortcut";
            if (!opt.registry) args += L" --no-registry";
            ShellExecuteW(nullptr, L"open", copy.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL);
            return 0;
        }
    }
    if (opt.quiet) {
        bool ok = opt.mode == Mode::Install ? Install() : Uninstall();
        CoUninitialize();
        return ok ? 0 : 1;
    }

    Gdiplus::GdiplusStartupInput gdipInput;
    ULONG_PTR gdipToken = 0;
    Gdiplus::GdiplusStartup(&gdipToken, &gdipInput, nullptr);
    using DpiFn = BOOL(WINAPI*)(HANDLE);
    if (auto set = (DpiFn)GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext")) set((HANDLE)-4);
    HDC screen = GetDC(nullptr);
    scale = GetDeviceCaps(screen, LOGPIXELSX) / 96.0f;
    ReleaseDC(nullptr, screen);
    fTitle = Font(22, FW_SEMIBOLD);
    fStatus = Font(16, FW_SEMIBOLD);
    fBody = Font(13, FW_NORMAL);
    fButton = Font(14, FW_SEMIBOLD);

    std::error_code ec;
    alreadyInstalled = fs::exists(fs::path(opt.dir) / L"Fathom.exe", ec);
    if (opt.mode == Mode::Install)
        Show(Ready, alreadyInstalled ? L"Update Fathom" : L"Install Fathom",
             L"Fathom goes into " + opt.dir + L" and gets a shortcut on your desktop. The files come from github.com/Sniko94pig/Fathom.");
    else
        Show(Ready, L"Remove Fathom", L"This removes Fathom from " + opt.dir + L" and the desktop shortcut. Your save backups stay.");

    WNDCLASSW wc{};
    wc.lpfnWndProc = Proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"FathomSetup";
    RegisterClassW(&wc);
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT r{0, 0, S(540), S(320)};
    AdjustWindowRect(&r, style, FALSE);
    int w = r.right - r.left, hgt = r.bottom - r.top;
    int x = (GetSystemMetrics(SM_CXSCREEN) - w) / 2, y = (GetSystemMetrics(SM_CYSCREEN) - hgt) / 2;
    wnd = CreateWindowExW(0, wc.lpszClassName, opt.mode == Mode::Install ? L"Fathom Setup" : L"Uninstall Fathom", style, x, y, w, hgt, nullptr,
                          nullptr, inst, nullptr);
    using DarkFn = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        if (auto set = (DarkFn)GetProcAddress(dwm, "DwmSetWindowAttribute")) {
            BOOL dark = TRUE;
            set(wnd, 20, &dark, sizeof(dark));
        }
    }
    int bw = S(150), bh = S(42), by = S(320) - S(28) - bh;
    btnMain = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, S(540) - S(32) - bw, by, bw, bh, wnd, (HMENU)(INT_PTR)ID_MAIN, inst, nullptr);
    btnSecond = CreateWindowW(L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, S(540) - S(32) - bw - S(12) - S(120), by, S(120), bh, wnd,
                              (HMENU)(INT_PTR)ID_SECOND, inst, nullptr);
    btnCheck = CreateWindowW(L"BUTTON", L"Also remove settings, map pictures and logs from the game folder", WS_CHILD | BS_OWNERDRAW, S(32),
                             S(204), S(540) - S(64), S(28), wnd, (HMENU)(INT_PTR)ID_CHECK, inst, nullptr);
    Layout();
    if (!opt.snapshot.empty()) {
        Snapshot(opt.snapshot);
        DestroyWindow(wnd);
        Gdiplus::GdiplusShutdown(gdipToken);
        CoUninitialize();
        return 0;
    }
    ShowWindow(wnd, SW_SHOWNORMAL);
    UpdateWindow(wnd);
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (worker.joinable()) worker.join();
    if (opt.mode == Mode::Uninstall) CleanupSelf();
    Gdiplus::GdiplusShutdown(gdipToken);
    CoUninitialize();
    return 0;
}
