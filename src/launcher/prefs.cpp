#include "launcher/prefs.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <wininet.h>
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using json = nlohmann::json;

namespace launcher {

namespace {

const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t* kRunName = L"Fathom";

std::wstring ExePath() {
    wchar_t path[MAX_PATH];
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

json ToJson(const ui::LoaderSettings& s) {
    return json{{"startWithWindows", s.startWithWindows}, {"closeAfterInject", s.closeAfterInject},
                {"autoInject", s.autoInject},             {"onTop", s.onTop},
                {"scale", s.scale},                       {"accent", s.accent},
                {"showSoon", s.showSoon},                 {"installedFirst", s.installedFirst},
                {"webArt", s.webArt},                     {"checkUpdates", s.checkUpdates},
                {"backupOnInject", s.backupOnInject}};
}

template <typename T>
void Read(const json& j, const char* key, T& v) {
    auto it = j.find(key);
    if (it == j.end()) return;
    try {
        v = it->get<T>();
    } catch (...) {
    }
}

}

bool LoadPrefs(ui::LoaderSettings& s, const std::wstring& file) {
    std::ifstream in{fs::path(file)};
    if (!in) return false;
    json j = json::parse(in, nullptr, false);
    if (!j.is_object()) return false;
    ui::LoaderSettings d;
    Read(j, "startWithWindows", d.startWithWindows);
    Read(j, "closeAfterInject", d.closeAfterInject);
    Read(j, "autoInject", d.autoInject);
    Read(j, "onTop", d.onTop);
    Read(j, "scale", d.scale);
    Read(j, "accent", d.accent);
    Read(j, "showSoon", d.showSoon);
    Read(j, "installedFirst", d.installedFirst);
    Read(j, "webArt", d.webArt);
    Read(j, "checkUpdates", d.checkUpdates);
    Read(j, "backupOnInject", d.backupOnInject);
    d.scale = std::clamp(d.scale, 0.7f, 2.0f);
    d.accent = std::clamp(d.accent, 0, ui::AccentCount() - 1);
    s = d;
    return true;
}

bool SavePrefs(const ui::LoaderSettings& s, const std::wstring& file) {
    fs::path tmp = fs::path(file);
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        out << ToJson(s).dump(2);
        if (!out) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

std::string PrefsText(const ui::LoaderSettings& s) { return ToJson(s).dump(); }

bool StartupEnabled() {
    wchar_t buf[1024];
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunName, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return false;
    return Lower(buf).find(Lower(ExePath())) != std::wstring::npos;
}

bool SetStartup(bool on) {
    HKEY key;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    LONG r;
    if (on) {
        std::wstring cmd = L"\"" + ExePath() + L"\" --minimized";
        r = RegSetValueExW(key, kRunName, 0, REG_SZ, (const BYTE*)cmd.c_str(), (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        r = RegDeleteValueW(key, kRunName);
        if (r == ERROR_FILE_NOT_FOUND) r = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

namespace update {

namespace {

std::mutex lock;
Status status;
std::thread job;

bool Fetch(const wchar_t* url, const wchar_t* headers, std::string& out) {
    HINTERNET net = InternetOpenW(L"Fathom", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return false;
    DWORD ms = 20000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof(ms));
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof(ms));
    bool ok = false;
    HINTERNET h = InternetOpenUrlW(net, url, headers, headers ? (DWORD)-1 : 0,
                                   INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE | INTERNET_FLAG_NO_UI, 0);
    if (h) {
        DWORD code = 0, len = sizeof(code);
        HttpQueryInfoW(h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &code, &len, nullptr);
        char buf[16384];
        DWORD got = 0;
        while (InternetReadFile(h, buf, sizeof(buf), &got) && got) out.append(buf, got);
        ok = code == 200 && !out.empty();
        InternetCloseHandle(h);
    }
    InternetCloseHandle(net);
    return ok;
}

std::vector<int> Parts(const std::string& v) {
    std::vector<int> out;
    int cur = -1;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur = (cur < 0 ? 0 : cur * 10) + (c - '0');
        } else if (cur >= 0) {
            out.push_back(cur);
            cur = -1;
        }
    }
    if (cur >= 0) out.push_back(cur);
    while (out.size() < 3) out.push_back(0);
    return out;
}

void Begin(void (*fn)()) {
    {
        std::lock_guard<std::mutex> g(lock);
        if (status.busy) return;
        status.busy = true;
    }
    if (job.joinable()) job.join();
    job = std::thread(fn);
}

void CheckJob() {
    std::string body;
    bool ok = Fetch(L"https://api.github.com/repos/Sniko94pig/Fathom/releases/latest", L"Accept: application/vnd.github+json\r\n", body);
    std::string tag;
    if (ok) {
        json j = json::parse(body, nullptr, false);
        if (j.is_object() && j.contains("tag_name") && j["tag_name"].is_string()) tag = j["tag_name"].get<std::string>();
    }
    std::lock_guard<std::mutex> g(lock);
    status.busy = false;
    status.checked = !tag.empty();
    status.failed = tag.empty();
    status.latest = tag;
    status.newer = !tag.empty() && Newer(tag, FATHOM_VERSION);
}

void DownloadJob() {
    std::string body;
    bool ok = Fetch(L"https://github.com/Sniko94pig/Fathom/releases/latest/download/FathomSetup.exe", nullptr, body);
    std::wstring file;
    if (ok && body.size() > 100000 && body[0] == 'M' && body[1] == 'Z') {
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        fs::path dir = fs::path(tmp) / L"FathomUpdate";
        std::error_code ec;
        fs::create_directories(dir, ec);
        fs::path path = dir / L"FathomSetup.exe";
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(body.data(), (std::streamsize)body.size());
        out.close();
        if (out) file = path.wstring();
    }
    std::lock_guard<std::mutex> g(lock);
    status.busy = false;
    status.installerReady = !file.empty();
    status.failed = file.empty();
    status.installer = file;
}

}

bool Newer(const std::string& latest, const std::string& current) {
    std::vector<int> a = Parts(latest), b = Parts(current);
    return std::lexicographical_compare(b.begin(), b.end(), a.begin(), a.end());
}

void Check() { Begin(CheckJob); }
void Download() { Begin(DownloadJob); }

Status Get() {
    std::lock_guard<std::mutex> g(lock);
    return status;
}

void Wait() {
    if (job.joinable()) job.join();
}

}

}
