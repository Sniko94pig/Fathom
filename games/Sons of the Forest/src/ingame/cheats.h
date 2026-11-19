#pragma once
#include "core/model.h"
#include "ingame/il2cpp.h"
#include "ingame/unity.h"
#include <mutex>
#include <string>
#include <unordered_map>

namespace sotf {

struct Snap {
    std::unordered_map<std::string, bool> on;
    std::unordered_map<std::string, float> value;
    bool menuOpen = false;
    bool menuFocused = false;
    bool mapVisible = false;

    bool On(const char* id) const {
        auto it = on.find(id);
        return it != on.end() && it->second;
    }
    float Value(const char* id, float fallback = 1.0f) const {
        auto it = value.find(id);
        return it != value.end() ? it->second : fallback;
    }
};

struct Ctx {
    const Snap& s;
    const Snap& prev;
    il2::Object* player = nullptr;
    bool inWorld = false;
    bool stopping = false;
    core::Session session = core::Session::Solo;
    double now = 0;
    float dt = 0;
};

struct Group {
    const char* name = "";
    bool (*resolve)(std::string& err) = nullptr;
    bool (*supported)(const std::string& id) = nullptr;
    void (*tick)(const Ctx& c) = nullptr;
    bool (*action)(const std::string& id, const Ctx& c) = nullptr;
    void (*release)(const Ctx& c) = nullptr;
    void (*wire)(core::Model& m, std::mutex& lock) = nullptr;
};

void Note(const std::string& text);
bool GameFocused();
bool KeyDown(int vk);
const std::wstring& Folder();

const Group& VitalsGroup();
const Group& MovementGroup();
const Group& WeaponsGroup();
const Group& EnemiesGroup();
const Group& BuildingGroup();
const Group& WorldGroup();
const Group& CompanionsGroup();
const Group& SpawnGroup();

}
