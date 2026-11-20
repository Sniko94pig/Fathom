#include "launcher/games.h"
#include <windows.h>
#include <shellapi.h>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <regex>

namespace fs = std::filesystem;

namespace launcher {

const std::vector<GameDef>& Games() {
    static const std::vector<GameDef> list = {
        {"subnautica2", "Subnautica 2", 1962700, "SN2", true},
        {"borderlands", "Borderlands", 729040, "BL", false},
        {"borderlands2", "Borderlands 2", 49520, "BL2", false},
        {"borderlands3", "Borderlands 3", 397540, "BL3", false},
        {"borderlands4", "Borderlands 4", 1285190, "BL4", false},
        {"cyberpunk2077", "Cyberpunk 2077", 1091500, "CP", false},
        {"doom", "DOOM + DOOM II", 2280, "DOOM", false},
        {"dunea", "Dune: Awakening", 1172710, "DUNE", false},
        {"dyinglight", "Dying Light", 239140, "DL", false},
        {"backrooms", "Escape the Backrooms", 1943950, "ETB", false},
        {"fallout4", "Fallout 4", 377160, "FO4", false},
        {"falloutnv", "Fallout: New Vegas", 22380, "FNV", false},
        {"geometrydash", "Geometry Dash", 322170, "GD", false},
        {"helloneighbor", "Hello Neighbor", 521890, "HN", false},
        {"icarus", "Icarus", 1149460, "ICA", false},
        {"minecraft", "Minecraft: Java Edition", -1, "MC", false, true, "https://www.minecraft.net/"},
        {"outlast", "Outlast", 238320, "OL", false},
        {"poppyplaytime", "Poppy Playtime", 1721470, "PP", false},
        {"raft", "Raft", 648800, "RAFT", false},
        {"satisfactory", "Satisfactory", 526870, "SAT", false},
        {"schedule1", "Schedule I", 3164500, "S1", false},
        {"sonsoftheforest", "Sons of the Forest", 1326470, "SOF", true},
        {"subnautica", "Subnautica", 264710, "SN", true},
        {"belowzero", "Subnautica: Below Zero", 848450, "BZ", false},
        {"theforest", "The Forest", 242760, "TF", false},
        {"tlou1", "The Last of Us Part I", 1888930, "TLOU", false},
        {"tlou2", "The Last of Us Part II", 2531310, "TLOU2", false},
        {"ultrakill", "ULTRAKILL", 1229490, "UK", false},
    };
    return list;
}

int FindGame(const char* id) {
    const auto& list = Games();
    for (size_t i = 0; i < list.size(); ++i)
        if (std::strcmp(list[i].id, id) == 0) return (int)i;
    return -1;
}

std::wstring SteamPath() {
    HKEY key;
    wchar_t steam[MAX_PATH] = {};
    DWORD size = sizeof(steam);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", 0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"SteamPath", nullptr, nullptr, (BYTE*)steam, &size);
        RegCloseKey(key);
    }
    return steam;
}

std::vector<std::wstring> SteamLibraries() {
    std::vector<std::wstring> libs;
    std::wstring steam = SteamPath();
    if (steam.empty()) return libs;
    libs.push_back(fs::path(steam).wstring());
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
    return libs;
}

namespace {

std::string Squash(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s)
        if (c < 128 && std::iswalnum(c)) out.push_back((char)std::towlower(c));
    return out;
}

std::vector<fs::path> XboxRoots() {
    std::vector<fs::path> out;
    DWORD mask = GetLogicalDrives();
    for (int d = 0; d < 26; ++d) {
        if (!(mask & (1u << d))) continue;
        wchar_t root[] = {wchar_t(L'A' + d), L':', L'\\', 0};
        if (GetDriveTypeW(root) != DRIVE_FIXED) continue;
        std::error_code ec;
        fs::path dir = fs::path(root) / L"XboxGames";
        if (fs::is_directory(dir, ec)) out.push_back(dir);
    }
    return out;
}

std::vector<std::string> XboxFolders() {
    std::vector<std::string> out;
    for (const fs::path& dir : XboxRoots()) {
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(dir, ec))
            if (e.is_directory(ec)) out.push_back(Squash(e.path().filename().wstring()));
    }
    return out;
}

const char* kMinecraftApp = "shell:AppsFolder\\Microsoft.4297127D64EC6_8wekyb3d8bbwe!Minecraft";

}

std::wstring LocalArt(int appId) {
    if (appId != -1) return {};
    std::error_code ec;
    for (const fs::path& dir : XboxRoots()) {
        fs::path file = dir / L"Minecraft Launcher" / L"Content" / L"SplashScreen.png";
        if (fs::exists(file, ec)) return file.wstring();
    }
    return {};
}

void StartGame(const GameDef& g) {
    std::wstring url;
    if (g.appId > 0) url = L"steam://rungameid/" + std::to_wstring(g.appId);
    else if (std::strcmp(g.id, "minecraft") == 0) url = std::wstring(kMinecraftApp, kMinecraftApp + std::strlen(kMinecraftApp));
    else if (g.site) url = std::wstring(g.site, g.site + std::strlen(g.site));
    if (!url.empty()) ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

const char* InstalledWith(const GameDef& g) {
    if (g.appId > 0 && SteamInstalled(g.appId)) return "Installed with Steam";
    if (std::strcmp(g.id, "minecraft") == 0) {
        wchar_t buf[MAX_PATH];
        DWORD n = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
        std::error_code ec;
        if (n && n < MAX_PATH && fs::is_directory(fs::path(buf) / L".minecraft" / L"versions", ec)) return "Installed on this PC";
        return nullptr;
    }
    static const std::vector<std::string> xbox = XboxFolders();
    std::string name = Squash(std::wstring(g.name, g.name + std::strlen(g.name)));
    for (const std::string& f : xbox)
        if (f == name) return "Installed with the Xbox app";
    return nullptr;
}

bool SteamInstalled(int appId) {
    static std::vector<std::wstring> libs = SteamLibraries();
    std::error_code ec;
    for (const std::wstring& lib : libs)
        if (fs::exists(fs::path(lib) / L"steamapps" / (L"appmanifest_" + std::to_wstring(appId) + L".acf"), ec)) return true;
    return false;
}

}
