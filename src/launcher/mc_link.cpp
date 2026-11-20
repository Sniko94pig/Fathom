#include "launcher/mc_link.h"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwctype>
#include <deque>
#include <fstream>
#include <iterator>
#include <random>
#include <set>

namespace fs = std::filesystem;
using nlohmann::json;

namespace launcher::mclink {

namespace {

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

json ReadJson(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return json();
    return json::parse(in, nullptr, false);
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::string Str(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool Bool(const json& o, const char* key, bool def) {
    auto it = o.find(key);
    return it != o.end() && it->is_boolean() ? it->get<bool>() : def;
}

long long Int(const json& o, const char* key, long long def) {
    auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<long long>() : def;
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
    return false;
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

BOOL CALLBACK FindGame(HWND h, LPARAM found) {
    wchar_t cls[32];
    if (!GetClassNameW(h, cls, 32) || wcscmp(cls, L"GLFW30") != 0) return TRUE;
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
    json j = ReadJson(p);
    if (!j.is_object()) {
        if (haveLast) out = last;
        return haveLast;
    }
    out.session = Str(j, "session");
    out.ack = Int(j, "ack", 0);
    out.mc = Str(j, "mc");
    out.client = Str(j, "client");
    out.night = Bool(j, "night", false);
    out.nightOn = Bool(j, "nightOn", true);
    out.world = Bool(j, "world", false);
    out.baritone = Bool(j, "baritone", false);
    out.pathing = Bool(j, "pathing", false);
    out.task = Str(j, "task");
    out.goal = Str(j, "goal");
    out.eta = Int(j, "eta", -1);
    auto hacks = j.find("hacks");
    if (hacks != j.end() && hacks->is_object())
        for (const auto& e : hacks->items())
            if (e.value().is_boolean()) out.hacks[e.key()] = e.value().get<bool>();
    auto settings = j.find("settings");
    if (settings != j.end() && settings->is_object())
        for (const auto& e : settings->items()) {
            if (e.value().is_boolean()) out.flags[e.key()] = e.value().get<bool>();
            else if (e.value().is_number()) out.numbers[e.key()] = e.value().get<double>();
        }
    auto pos = j.find("pos");
    if (pos != j.end() && pos->is_array() && pos->size() == 3 && (*pos)[0].is_number() && (*pos)[1].is_number() && (*pos)[2].is_number()) {
        out.hasPos = true;
        for (int i = 0; i < 3; ++i) out.pos[i] = (*pos)[i].get<int>();
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
        std::wstring key = in.mods.lexically_normal().wstring();
        for (wchar_t& c : key) c = (wchar_t)std::towlower(c);
        while (!key.empty() && (key.back() == L'\\' || key.back() == L'/')) key.pop_back();
        if (!seen.insert(key).second) continue;
        Scan(in);
        out.push_back(std::move(in));
    }
    std::stable_sort(out.begin(), out.end(), [](const Install& a, const Install& b) { return a.night && !b.night; });
    return out;
}

int AddMod(const fs::path& jar, bool onlyUpdate) {
    std::error_code ec;
    if (!fs::exists(jar, ec)) return -1;
    std::string want = ReadAll(jar);
    if (want.empty()) return -1;
    int n = 0;
    for (const Install& in : FabricInstalls()) {
        if (onlyUpdate && !in.fathom) continue;
        fs::path dest = in.mods / L"fathom_mc.jar";
        if (in.fathom && ReadAll(dest) == want) {
            ++n;
            continue;
        }
        fs::create_directories(in.mods, ec);
        if (fs::copy_file(jar, dest, fs::copy_options::overwrite_existing, ec)) ++n;
    }
    return n;
}

}
