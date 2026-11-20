#pragma once
#include <nlohmann/json.hpp>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace launcher::mclink {

struct State {
    bool night = false;
    bool nightOn = true;
    bool world = false;
    bool baritone = false;
    bool pathing = false;
    bool hasPos = false;
    long long ack = 0;
    long long eta = -1;
    int pos[3] = {0, 0, 0};
    std::string session, mc, client, task, goal;
    std::unordered_map<std::string, bool> hacks;
    std::unordered_map<std::string, bool> flags;
    std::unordered_map<std::string, double> numbers;
};

struct Install {
    std::string name;
    std::filesystem::path mods;
    bool night = false;
    bool fathom = false;
};

std::filesystem::path Dir();
std::filesystem::path LogFile();
bool Read(State& out);
long long Send(nlohmann::json cmd);
const std::string& Session();
bool GameOpen();
std::vector<Install> FabricInstalls();
int AddMod(const std::filesystem::path& jar, bool onlyUpdate);

}
