#pragma once
#include <string>
#include <vector>

namespace launcher {

struct GameDef {
    const char* id;
    const char* name;
    int appId;
    const char* tag;
    bool ready;
    bool building = false;
};

const std::vector<GameDef>& Games();
int FindGame(const char* id);

std::wstring SteamPath();
std::vector<std::wstring> SteamLibraries();
bool SteamInstalled(int appId);

}
