#include "launcher/maps.h"
#include "ui/icons.h"

namespace launcher::maps {

namespace {

const Kind kSn1Kinds[] = {
    {"Lifepods", ICON_LIFE_BUOY, 255, 152, 64},      {"Wrecks", ICON_SHIP, 232, 196, 120},        {"Degasi bases", ICON_HOUSE, 120, 220, 140},
    {"Alien sites", ICON_PYRAMID, 80, 226, 206},     {"Alien arches", ICON_ORBIT, 176, 142, 255}, {"Landmarks", ICON_MAP_PIN, 255, 104, 124},
};

const Kind kSotfKinds[] = {
    {"Caves", ICON_MOUNTAIN, 222, 180, 120},        {"Snow caves", ICON_MOUNTAIN_SNOW, 190, 226, 255}, {"Bunkers", ICON_WAREHOUSE, 90, 196, 255},
    {"Abandoned villages", ICON_TENT, 236, 222, 140}, {"Cannibal villages", ICON_SKULL, 255, 100, 100},
};

}

const char* Title(int game) {
    static const char* names[Count] = {"Subnautica", "Subnautica 2", "Sons of the Forest"};
    return game >= 0 && game < Count ? names[game] : "";
}

const Kind* Kinds(int game, int& count) {
    if (game == Subnautica) {
        count = (int)(sizeof(kSn1Kinds) / sizeof(kSn1Kinds[0]));
        return kSn1Kinds;
    }
    if (game == SonsOfTheForest) {
        count = (int)(sizeof(kSotfKinds) / sizeof(kSotfKinds[0]));
        return kSotfKinds;
    }
    count = 0;
    return nullptr;
}

bool World(int game, float& x0, float& z0, float& x1, float& z1) {
    float e = game == Subnautica ? 2048.0f : game == SonsOfTheForest ? 2000.0f : 0.0f;
    if (e <= 0) return false;
    x0 = z0 = -e;
    x1 = z1 = e;
    return true;
}

}
