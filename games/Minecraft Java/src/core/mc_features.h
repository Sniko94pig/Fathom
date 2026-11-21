#pragma once
#include "core/model.h"
#include <nlohmann/json.hpp>
#include <functional>
#include <string>
#include <unordered_map>

namespace mc {

struct Live {
    bool night = false;
    bool nightOn = true;
    bool world = false;
    bool baritone = false;
    bool pathing = false;
    bool hasPos = false;
    bool menu = false;
    long long ack = 0;
    long long eta = -1;
    int pos[3] = {0, 0, 0};
    std::string session, mc, client, task, goal;
    std::unordered_map<std::string, bool> hacks;
    std::unordered_map<std::string, bool> flags;
    std::unordered_map<std::string, double> numbers;
};

void BuildFeatures(core::Model& m);
bool IsHack(const core::Feature& f);
const char* BaritoneSetting(const core::Feature& f);
std::string BaritoneCommand(const core::Feature& f, std::string& error);

bool ParseLive(const std::string& text, Live& out);
nlohmann::json Command(core::Feature& f);
nlohmann::json SliderCommand(const core::Feature& f);
void ApplyLive(core::Model& m, bool linked, const Live& st, const std::function<bool(const std::string&)>& hold, const std::string& gotoHint);
std::string TaskText(bool linked, const Live& st);

}
