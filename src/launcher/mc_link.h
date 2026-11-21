#pragma once
#include "core/mc_features.h"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace launcher::mclink {

using State = mc::Live;

struct Install {
    std::string name;
    std::filesystem::path mods;
    bool night = false;
    bool fathom = false;
};

struct Profile {
    bool ok = false;
    bool needFabric = false;
    bool launcherOpen = false;
    std::string error;
    std::string mc;
    std::string loader;
    std::string api;
    std::filesystem::path mods;
};

std::filesystem::path Dir();
std::filesystem::path LogFile();
bool Read(State& out);
long long Send(nlohmann::json cmd);
const std::string& Session();
bool GameOpen();
bool LauncherOpen();
std::vector<Install> FabricInstalls();
int RemoveOldMod();
Profile Current();
Profile Prepare(const std::filesystem::path& jar, const std::string& agent, const std::function<void(const std::string&)>& step);

}
