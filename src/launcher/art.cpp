#include "launcher/art.h"
#include "launcher/games.h"
#include "stb_image.h"
#include <windows.h>
#include <urlmon.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace launcher::art {

namespace {

std::thread worker;
std::atomic<bool> stopping{false};
std::mutex lock;
std::deque<Image> ready;

class Abort : public IBindStatusCallback {
public:
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
    HRESULT STDMETHODCALLTYPE OnProgress(ULONG, ULONG, ULONG, LPCWSTR) override { return stopping ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE OnStopBinding(HRESULT, LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetBindInfo(DWORD*, BINDINFO*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDataAvailable(DWORD, DWORD, FORMATETC*, STGMEDIUM*) override { return stopping ? E_ABORT : S_OK; }
    HRESULT STDMETHODCALLTYPE OnObjectAvailable(REFIID, IUnknown*) override { return S_OK; }
};

const wchar_t* SteamName(int kind) { return kind == Hero ? L"library_hero.jpg" : L"library_600x900.jpg"; }

bool FromSteam(int appId, int kind, const fs::path& to) {
    std::wstring steam = SteamPath();
    if (steam.empty()) return false;
    fs::path cache = fs::path(steam) / L"appcache" / L"librarycache";
    std::error_code ec;
    fs::path flat = cache / (std::to_wstring(appId) + L"_" + SteamName(kind));
    if (fs::exists(flat, ec)) return fs::copy_file(flat, to, fs::copy_options::overwrite_existing, ec);
    fs::path dir = cache / std::to_wstring(appId);
    if (!fs::is_directory(dir, ec)) return false;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file(ec) && _wcsicmp(it->path().filename().c_str(), SteamName(kind)) == 0)
            return fs::copy_file(it->path(), to, fs::copy_options::overwrite_existing, ec);
    }
    return false;
}

bool FromWeb(int appId, int kind, const fs::path& to) {
    std::wstring url = L"https://cdn.cloudflare.steamstatic.com/steam/apps/" + std::to_wstring(appId) + L"/" + SteamName(kind);
    fs::path part = to;
    part += L".part";
    Abort cb;
    if (FAILED(URLDownloadToFileW(nullptr, url.c_str(), part.c_str(), 0, &cb))) return false;
    std::error_code ec;
    if (fs::file_size(part, ec) < 1000 || ec) {
        fs::remove(part, ec);
        return false;
    }
    fs::rename(part, to, ec);
    return !ec;
}

bool Load(const fs::path& file, Image& img) {
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() < 100) return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

void Shrink(Image& img, int maxW) {
    if (img.width <= maxW) return;
    int nw = maxW, nh = std::max(1, (int)((int64_t)img.height * maxW / img.width));
    std::vector<uint8_t> out((size_t)nw * nh * 4);
    for (int y = 0; y < nh; ++y) {
        int y0 = y * img.height / nh, y1 = std::max(y0 + 1, (y + 1) * img.height / nh);
        for (int x = 0; x < nw; ++x) {
            int x0 = x * img.width / nw, x1 = std::max(x0 + 1, (x + 1) * img.width / nw);
            unsigned sum[4] = {0, 0, 0, 0};
            unsigned n = 0;
            for (int yy = y0; yy < y1; ++yy)
                for (int xx = x0; xx < x1; ++xx) {
                    const uint8_t* p = img.rgba.data() + ((size_t)yy * img.width + xx) * 4;
                    for (int c = 0; c < 4; ++c) sum[c] += p[c];
                    ++n;
                }
            uint8_t* o = out.data() + ((size_t)y * nw + x) * 4;
            for (int c = 0; c < 4; ++c) o[c] = (uint8_t)(sum[c] / n);
        }
    }
    img.rgba.swap(out);
    img.width = nw;
    img.height = nh;
}

void Run(std::vector<std::pair<int, int>> wanted, std::wstring cacheDir, bool web) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::error_code ec;
    fs::create_directories(cacheDir, ec);
    for (const auto& [appId, kind] : wanted) {
        if (stopping) break;
        fs::path file = fs::path(cacheDir) / (std::to_wstring(appId) + (kind == Hero ? L"_hero.jpg" : L"_capsule.jpg"));
        if (!fs::exists(file, ec) && !FromSteam(appId, kind, file) && !(web && FromWeb(appId, kind, file))) continue;
        Image img;
        img.appId = appId;
        img.kind = kind;
        if (!Load(file, img)) {
            fs::remove(file, ec);
            continue;
        }
        Shrink(img, kind == Hero ? 1280 : 360);
        std::lock_guard<std::mutex> g(lock);
        ready.push_back(std::move(img));
    }
    CoUninitialize();
}

}

void Start(const std::vector<std::pair<int, int>>& wanted, const std::wstring& cacheDir, bool web) {
    Stop();
    {
        std::lock_guard<std::mutex> g(lock);
        ready.clear();
    }
    stopping = false;
    worker = std::thread(Run, wanted, cacheDir, web);
}

bool Take(Image& out) {
    std::lock_guard<std::mutex> g(lock);
    if (ready.empty()) return false;
    out = std::move(ready.front());
    ready.pop_front();
    return true;
}

void Stop() {
    stopping = true;
    if (worker.joinable()) worker.join();
}

}
