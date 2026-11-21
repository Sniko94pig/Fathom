#include "launcher/mc_link.h"
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <wininet.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <random>
#include <set>

extern const unsigned char logo_png[];
extern const unsigned int logo_png_size;

namespace fs = std::filesystem;
using nlohmann::json;

namespace launcher::mclink {

namespace {

const char* kSupported[] = {"1.20.1", "1.20.2", "1.20.4", "1.20.6", "1.21.1", "1.21.3", "1.21.4",
                            "1.21.5", "1.21.8", "1.21.10", "1.21.11", "26.1.2", "26.2", "26.3"};

fs::path Game() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    return fs::path(buf) / L".minecraft";
}

fs::path Wide(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return fs::path(w);
}

std::string Utf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

json ReadJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return json();
    return json::parse(in, nullptr, false);
}

std::string Str(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool WriteFile(const fs::path& p, const std::string& text) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.good()) return false;
    }
    for (int i = 0; i < 5; ++i) {
        if (MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
        Sleep(10);
    }
    fs::remove(tmp, ec);
    return false;
}

std::string Base64(const unsigned char* data, size_t len) {
    static const char* t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((len + 2) / 3 * 4);
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned)data[i] << 16;
        if (i + 1 < len) v |= (unsigned)data[i + 1] << 8;
        if (i + 2 < len) v |= data[i + 2];
        out.push_back(t[(v >> 18) & 63]);
        out.push_back(t[(v >> 12) & 63]);
        out.push_back(i + 1 < len ? t[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < len ? t[v & 63] : '=');
    }
    return out;
}

std::string Sha512(const std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    unsigned char out[64] = {};
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA512_ALGORITHM, nullptr, 0) == 0 &&
              BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(h, (PUCHAR)data.data(), (ULONG)data.size(), 0) == 0 && BCryptFinishHash(h, out, sizeof(out), 0) == 0;
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return {};
    std::string hex;
    char b[3];
    for (unsigned char c : out) {
        std::snprintf(b, sizeof(b), "%02x", c);
        hex += b;
    }
    return hex;
}

bool HttpGet(const std::wstring& url, const std::string& agent, std::string& body, size_t limit) {
    body.clear();
    HINTERNET net = InternetOpenA(agent.c_str(), INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
    if (!net) return false;
    DWORD timeout = 20000;
    InternetSetOptionW(net, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof(timeout));
    InternetSetOptionW(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof(timeout));
    HINTERNET req = InternetOpenUrlW(net, url.c_str(), nullptr, 0, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES, 0);
    bool ok = false;
    if (req) {
        DWORD status = 0, size = sizeof(status);
        if (HttpQueryInfoW(req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr) && status == 200) {
            char buf[65536];
            DWORD got = 0;
            ok = true;
            while (InternetReadFile(req, buf, sizeof(buf), &got) && got > 0) {
                body.append(buf, got);
                if (body.size() > limit) {
                    ok = false;
                    break;
                }
            }
        }
        InternetCloseHandle(req);
    }
    InternetCloseHandle(net);
    return ok && !body.empty();
}

std::vector<int> Parts(const std::string& v) {
    std::vector<int> out;
    int cur = 0;
    bool any = false;
    for (char c : v) {
        if (c >= '0' && c <= '9') {
            cur = cur * 10 + (c - '0');
            any = true;
        } else if (c == '.') {
            out.push_back(cur);
            cur = 0;
            any = false;
        } else {
            break;
        }
    }
    if (any) out.push_back(cur);
    return out;
}

bool Older(const std::string& a, const std::string& b) {
    std::vector<int> x = Parts(a), y = Parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); ++i) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q;
    }
    return false;
}

bool Supported(const std::string& mc) {
    for (const char* s : kSupported)
        if (mc == s) return true;
    return false;
}

struct Loader {
    std::string id, mc, version;
};

Loader FindLoader(const fs::path& root) {
    Loader best;
    std::error_code ec;
    for (auto it = fs::directory_iterator(root / L"versions", ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_directory(ec)) continue;
        std::string id = Utf8(it->path().filename().wstring());
        const std::string prefix = "fabric-loader-";
        if (id.rfind(prefix, 0) != 0) continue;
        json v = ReadJson(it->path() / (it->path().filename().wstring() + L".json"));
        if (!v.is_object() || Str(v, "mainClass").find("KnotClient") == std::string::npos) continue;
        std::string mc = Str(v, "inheritsFrom");
        if (!Supported(mc)) continue;
        std::string version = id.substr(prefix.size());
        size_t dash = version.find('-');
        if (dash != std::string::npos) version = version.substr(0, dash);
        if (best.id.empty() || Older(best.mc, mc) || (best.mc == mc && Older(best.version, version))) best = {id, mc, version};
    }
    return best;
}

std::string FindApi(const fs::path& mods) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(mods, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string n = Utf8(it->path().filename().wstring());
        if (n.rfind("fabric-api-", 0) == 0 && n.size() > 4 && n.compare(n.size() - 4, 4, ".jar") == 0) return n;
    }
    return {};
}

std::string EnsureApi(const fs::path& mods, const std::string& mc, const std::string& agent, std::string& error) {
    std::string have = FindApi(mods);
    std::string list;
    std::wstring url = L"https://api.modrinth.com/v2/project/fabric-api/version?loaders=%5B%22fabric%22%5D&game_versions=%5B%22" +
                       Wide(mc).wstring() + L"%22%5D";
    if (!HttpGet(url, agent, list, 8 << 20)) {
        if (!have.empty()) return have;
        error = "Could not reach Modrinth to get Fabric API. Check your internet and try again.";
        return {};
    }
    json versions = json::parse(list, nullptr, false);
    const json* pick = nullptr;
    std::string pickDate;
    if (versions.is_array())
        for (int pass = 0; pass < 2 && !pick; ++pass)
            for (const json& v : versions) {
                if (!v.is_object() || (pass == 0 && Str(v, "version_type") != "release")) continue;
                std::string date = Str(v, "date_published");
                if (!pick || date > pickDate) {
                    pick = &v;
                    pickDate = date;
                }
            }
    const json* file = nullptr;
    if (pick) {
        auto files = pick->find("files");
        if (files != pick->end() && files->is_array())
            for (const json& f : *files)
                if (f.is_object() && (!file || f.value("primary", false))) file = &f;
    }
    std::string name = file ? Str(*file, "filename") : std::string();
    std::string link = file ? Str(*file, "url") : std::string();
    std::string sha;
    if (file && file->contains("hashes") && (*file)["hashes"].is_object()) sha = Str((*file)["hashes"], "sha512");
    if (name.empty() || link.rfind("https://", 0) != 0 || sha.empty() || name.find_first_of("/\\:") != std::string::npos ||
        name.rfind("fabric-api-", 0) != 0) {
        if (!have.empty()) return have;
        error = "Modrinth has no Fabric API for Minecraft " + mc + " yet.";
        return {};
    }
    fs::path dest = mods / Wide(name);
    std::error_code ec;
    bool good = fs::exists(dest, ec) && Sha512(ReadAll(dest)) == sha;
    if (!good) {
        std::string data;
        if (!HttpGet(Wide(link).wstring(), agent, data, 64 << 20) || Sha512(data) != sha) {
            if (!have.empty()) return have;
            error = "The Fabric API download did not work. Try again.";
            return {};
        }
        if (!WriteFile(dest, data)) {
            error = "Could not save Fabric API in the Fathom mods folder.";
            return {};
        }
    }
    for (auto it = fs::directory_iterator(mods, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::string n = Utf8(it->path().filename().wstring());
        if (n != name && n.rfind("fabric-api-", 0) == 0) fs::remove(it->path(), ec);
    }
    return name;
}

bool WriteVersion(const fs::path& root, const Loader& loader, const std::string& id, const fs::path& mods, std::string& error) {
    fs::path from = root / L"versions" / Wide(loader.id);
    json v = ReadJson(from / (Wide(loader.id).wstring() + L".json"));
    if (!v.is_object()) {
        error = "Could not read the Fabric files for Minecraft " + loader.mc + ".";
        return false;
    }
    v["id"] = id;
    if (!v.contains("arguments") || !v["arguments"].is_object()) v["arguments"] = json::object();
    json& args = v["arguments"];
    if (!args.contains("jvm") || !args["jvm"].is_array()) args["jvm"] = json::array();
    json jvm = json::array();
    for (const json& a : args["jvm"])
        if (!a.is_string() || a.get<std::string>().rfind("-Dfabric.modsFolder=", 0) != 0) jvm.push_back(a);
    jvm.push_back("-Dfabric.modsFolder=" + Utf8(mods.wstring()));
    args["jvm"] = jvm;
    fs::path dir = root / L"versions" / Wide(id);
    fs::path file = dir / (Wide(id).wstring() + L".json");
    std::string text = v.dump(2);
    if (ReadAll(file) != text && !WriteFile(file, text)) {
        error = "Could not write the Fathom install files.";
        return false;
    }
    std::error_code ec;
    fs::path jar = dir / (Wide(id).wstring() + L".jar");
    fs::path src = from / (Wide(loader.id).wstring() + L".jar");
    if (!fs::exists(src, ec)) src = root / L"versions" / Wide(loader.mc) / (Wide(loader.mc).wstring() + L".jar");
    if (fs::exists(src, ec) && (!fs::exists(jar, ec) || fs::file_size(jar, ec) != fs::file_size(src, ec)))
        fs::copy_file(src, jar, fs::copy_options::overwrite_existing, ec);
    return true;
}

std::string Now() {
    SYSTEMTIME t;
    GetSystemTime(&t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    return buf;
}

bool ProfileReady(const json& j, const std::string& id) {
    if (!j.is_object()) return false;
    auto profiles = j.find("profiles");
    if (profiles == j.end() || !profiles->is_object()) return false;
    auto p = profiles->find("fathom");
    return p != profiles->end() && p->is_object() && Str(*p, "lastVersionId") == id && Str(*p, "name") == "Fathom";
}

bool WriteProfile(const fs::path& root, const std::string& id, std::string& error) {
    fs::path file = root / L"launcher_profiles.json";
    std::error_code ec;
    if (!fs::exists(file, ec)) {
        error = "Open the Minecraft Launcher once, then press Start game again.";
        return false;
    }
    json j = ReadJson(file);
    if (!j.is_object()) {
        error = "Could not read the Minecraft Launcher's installations.";
        return false;
    }
    if (ProfileReady(j, id)) return true;
    fs::path backup = root / L"launcher_profiles.json.fathom-backup";
    if (!fs::exists(backup, ec)) fs::copy_file(file, backup, ec);
    if (!j.contains("profiles") || !j["profiles"].is_object()) j["profiles"] = json::object();
    json& p = j["profiles"]["fathom"];
    if (!p.is_object()) p = json::object();
    std::string now = Now();
    p["name"] = "Fathom";
    p["type"] = "custom";
    p["lastVersionId"] = id;
    p["icon"] = "data:image/png;base64," + Base64(logo_png, logo_png_size);
    if (!p.contains("created")) p["created"] = now;
    p["lastUsed"] = now;
    if (!WriteFile(file, j.dump(2))) {
        error = "Could not save the Fathom install in the Minecraft Launcher.";
        return false;
    }
    return true;
}

void Scan(Install& in) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(in.mods, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        std::wstring n = it->path().filename().wstring();
        for (wchar_t& c : n) c = (wchar_t)std::towlower(c);
        if (n.size() < 4 || n.compare(n.size() - 4, 4, L".jar") != 0) continue;
        if (n == L"fathom_mc.jar") in.fathom = true;
        else if (n.find(L"night") != std::wstring::npos) in.night = true;
    }
}

std::string ModsArg(const std::string& args) {
    const std::string key = "-Dfabric.modsFolder=";
    size_t at = args.find(key);
    if (at == std::string::npos) return {};
    size_t s = at + key.size();
    bool quoted = at > 0 && args[at - 1] == '"';
    if (!quoted && s < args.size() && args[s] == '"') {
        quoted = true;
        ++s;
    }
    size_t e = args.find(quoted ? '"' : ' ', s);
    return args.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

std::string VersionMods(const json& v) {
    auto a = v.find("arguments");
    if (a == v.end() || !a->is_object()) return {};
    auto jvm = a->find("jvm");
    if (jvm == a->end() || !jvm->is_array()) return {};
    for (const json& e : *jvm)
        if (e.is_string()) {
            std::string s = ModsArg(e.get<std::string>());
            if (!s.empty()) return s;
        }
    return {};
}

std::wstring Key(const fs::path& p) {
    std::wstring k = p.lexically_normal().wstring();
    for (wchar_t& c : k) c = (wchar_t)std::towlower(c);
    while (!k.empty() && (k.back() == L'\\' || k.back() == L'/')) k.pop_back();
    return k;
}

BOOL CALLBACK FindGame(HWND h, LPARAM found) {
    wchar_t cls[32];
    if (!GetClassNameW(h, cls, 32) || (wcscmp(cls, L"GLFW30") != 0 && wcscmp(cls, L"SDL_app") != 0)) return TRUE;
    wchar_t title[64];
    if (GetWindowTextW(h, title, 64) >= 9 && wcsncmp(title, L"Minecraft", 9) == 0) {
        *(bool*)found = true;
        return FALSE;
    }
    return TRUE;
}

}

fs::path Dir() {
    fs::path g = Game();
    return g.empty() ? fs::path() : g / L"fathom";
}

fs::path LogFile() { return Dir() / L"mod-log.txt"; }

const std::string& Session() {
    static const std::string s = [] {
        std::random_device rd;
        char buf[24];
        std::snprintf(buf, sizeof(buf), "%08x%08x", (unsigned)rd(), (unsigned)rd());
        return std::string(buf);
    }();
    return s;
}

bool Read(State& out) {
    static State last;
    static bool haveLast = false;
    out = State();
    fs::path p = Dir() / L"state.json";
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) return false;
    auto age = fs::file_time_type::clock::now() - t;
    if (age > std::chrono::seconds(3) || age < -std::chrono::seconds(60)) return false;
    if (!mc::ParseLive(ReadAll(p), out)) {
        if (haveLast) out = last;
        return haveLast;
    }
    last = out;
    haveLast = true;
    return true;
}

long long Send(json cmd) {
    static std::deque<json> recent;
    static long long seq = 0;
    cmd["seq"] = ++seq;
    recent.push_back(std::move(cmd));
    while (recent.size() > 64) recent.pop_front();
    json root = {{"session", Session()}, {"commands", json::array()}};
    for (const json& c : recent) root["commands"].push_back(c);
    WriteFile(Dir() / L"control.json", root.dump(1));
    return seq;
}

bool GameOpen() {
    bool found = false;
    EnumWindows(FindGame, (LPARAM)&found);
    return found;
}

bool LauncherOpen() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W e{sizeof(e)};
    bool found = false;
    for (BOOL ok = Process32FirstW(snap, &e); ok && !found; ok = Process32NextW(snap, &e))
        found = _wcsicmp(e.szExeFile, L"MinecraftLauncher.exe") == 0 || _wcsicmp(e.szExeFile, L"Minecraft.exe") == 0;
    CloseHandle(snap);
    return found;
}

std::vector<Install> FabricInstalls() {
    std::vector<Install> out;
    fs::path root = Game();
    if (root.empty()) return out;
    json j = ReadJson(root / L"launcher_profiles.json");
    if (!j.is_object()) return out;
    auto profiles = j.find("profiles");
    if (profiles == j.end() || !profiles->is_object()) return out;
    std::set<std::wstring> seen;
    for (const auto& item : profiles->items()) {
        const json& p = item.value();
        if (!p.is_object()) continue;
        std::string version = Str(p, "lastVersionId");
        if (version.empty() || version.find_first_of("/\\:") != std::string::npos || version.find("..") != std::string::npos) continue;
        fs::path id = Wide(version);
        json v = ReadJson(root / L"versions" / id / (id.wstring() + L".json"));
        if (!v.is_object() || Str(v, "mainClass").find("KnotClient") == std::string::npos) continue;
        std::string folder = ModsArg(Str(p, "javaArgs"));
        if (folder.empty()) folder = VersionMods(v);
        Install in;
        in.name = Str(p, "name");
        if (in.name.empty()) in.name = version;
        if (!folder.empty()) {
            in.mods = Wide(folder);
        } else {
            std::string dir = Str(p, "gameDir");
            in.mods = (dir.empty() ? root : Wide(dir)) / L"mods";
        }
        if (!seen.insert(Key(in.mods)).second) continue;
        Scan(in);
        out.push_back(std::move(in));
    }
    return out;
}

int RemoveOldMod() {
    std::wstring ours = Key(Dir());
    int n = 0;
    std::error_code ec;
    for (const Install& in : FabricInstalls()) {
        if (!in.fathom || Key(in.mods).rfind(ours, 0) == 0) continue;
        if (fs::remove(in.mods / L"fathom_mc.jar", ec)) ++n;
    }
    return n;
}

Profile Current() {
    Profile p;
    fs::path root = Game();
    if (root.empty()) return p;
    Loader loader = FindLoader(root);
    if (loader.id.empty()) {
        p.needFabric = true;
        return p;
    }
    p.mc = loader.mc;
    p.loader = loader.id;
    p.mods = Dir() / L"mods" / Wide(loader.mc);
    p.api = FindApi(p.mods);
    std::error_code ec;
    p.ok = !p.api.empty() && fs::exists(p.mods / L"fathom_mc.jar", ec) && ProfileReady(ReadJson(root / L"launcher_profiles.json"), "fathom-" + loader.mc);
    return p;
}

Profile Prepare(const fs::path& jar, const std::string& agent, const std::function<void(const std::string&)>& step) {
    Profile p;
    fs::path root = Game();
    std::error_code ec;
    if (root.empty() || !fs::is_directory(root / L"versions", ec)) {
        p.needFabric = true;
        return p;
    }
    Loader loader = FindLoader(root);
    if (loader.id.empty()) {
        p.needFabric = true;
        return p;
    }
    p.mc = loader.mc;
    p.loader = loader.id;
    p.mods = Dir() / L"mods" / Wide(loader.mc);
    fs::create_directories(p.mods, ec);
    if (!fs::exists(jar, ec)) {
        p.error = "fathom_mc.jar is missing. Update Fathom with the installer.";
        return p;
    }
    fs::path mod = p.mods / L"fathom_mc.jar";
    if (!fs::exists(mod, ec) || fs::file_size(mod, ec) != fs::file_size(jar, ec) || ReadAll(mod) != ReadAll(jar)) {
        if (!fs::copy_file(jar, mod, fs::copy_options::overwrite_existing, ec)) {
            p.error = "Could not copy the Fathom mod. Close Minecraft, then press Start game again.";
            return p;
        }
    }
    if (step) step("Getting Fabric API");
    p.api = EnsureApi(p.mods, loader.mc, agent, p.error);
    if (p.api.empty()) return p;
    if (step) step("Making the Fathom install");
    std::string id = "fathom-" + loader.mc;
    if (!WriteVersion(root, loader, id, p.mods, p.error)) return p;
    if (!ProfileReady(ReadJson(root / L"launcher_profiles.json"), id)) {
        if (LauncherOpen()) {
            p.launcherOpen = true;
            p.error = "Close the Minecraft Launcher, then press Start game again.";
            return p;
        }
        if (!WriteProfile(root, id, p.error)) return p;
    }
    RemoveOldMod();
    p.ok = true;
    return p;
}

}
