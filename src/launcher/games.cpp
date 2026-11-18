#include "launcher/games.h"
#include <windows.h>
#include <cstring>
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
        {"fallout4", "Fallout 4", 377160, "FO4", false},
        {"falloutnv", "Fallout: New Vegas", 22380, "FNV", false},
        {"outlast", "Outlast", 238320, "OL", false},
        {"satisfactory", "Satisfactory", 526870, "SAT", false},
        {"sonsoftheforest", "Sons of the Forest", 1326470, "SOF", false},
        {"subnautica", "Subnautica", 264710, "SN", false},
        {"belowzero", "Subnautica: Below Zero", 848450, "BZ", false},
        {"theforest", "The Forest", 242760, "TF", false},
        {"tlou1", "The Last of Us Part I", 1888930, "TLOU", false},
        {"tlou2", "The Last of Us Part II", 2531310, "TLOU2", false},
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

bool SteamInstalled(int appId) {
    static std::vector<std::wstring> libs = SteamLibraries();
    std::error_code ec;
    for (const std::wstring& lib : libs)
        if (fs::exists(fs::path(lib) / L"steamapps" / (L"appmanifest_" + std::to_wstring(appId) + L".acf"), ec)) return true;
    return false;
}

}
