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

const Kind kSn2Kinds[] = {
    {"Signals", ICON_MAP_PIN, 255, 104, 124},    {"Lifepods", ICON_LIFE_BUOY, 255, 152, 64}, {"Black boxes", ICON_ARCHIVE, 236, 236, 240},
    {"Supply crates", ICON_BOX, 242, 200, 90},   {"Supply lockers", ICON_LOCK, 90, 196, 255}, {"Quartz chips", ICON_SPARKLES, 190, 140, 255},
    {"Terminals", ICON_ZAP, 120, 226, 150},
};

const Sheet kBl3Sheets[] = {
    {"pandora.png", "Pandora", "", false},
    {"sanctuary.png", "Sanctuary III", "", false},
    {"promethea.png", "Promethea", "", false},
    {"athenas.png", "Athenas", "", false},
    {"eden6.png", "Eden-6", "", false},
    {"nekrotafeyo.png", "Nekrotafeyo", "", false},
    {"dlc_moxxis_heist.png", "Moxxi's Heist of the Handsome Jackpot", "Also shows Midnight's Cairn from Takedown at the Maliwan Blacksite", true},
    {"dlc_guns_love_and_tentacles.png", "Guns, Love, and Tentacles", "", true},
    {"dlc_bounty_of_blood.png", "Bounty of Blood", "", true},
    {"dlc_psycho_krieg.png", "Psycho Krieg and the Fantastic Fustercluck", "", true},
    {"dlc_guardian_breach_and_villa_ultraviolet.png", "Takedown at the Guardian Breach, Villa Ultraviolet and The Heck Hole", "", true},
};

}

const Sheet* Sheets(int game, int& count) {
    if (game == Borderlands3) {
        count = (int)(sizeof(kBl3Sheets) / sizeof(kBl3Sheets[0]));
        return kBl3Sheets;
    }
    count = 0;
    return nullptr;
}

const char* Axis(int game) { return game == Subnautica2 ? "Y" : "Z"; }

const char* Title(int game) {
    static const char* names[Count] = {"Subnautica", "Subnautica 2", "Sons of the Forest", "Borderlands 3"};
    return game >= 0 && game < Count ? names[game] : "";
}

const Kind* Kinds(int game, int& count) {
    if (game == Subnautica) {
        count = (int)(sizeof(kSn1Kinds) / sizeof(kSn1Kinds[0]));
        return kSn1Kinds;
    }
    if (game == Subnautica2) {
        count = (int)(sizeof(kSn2Kinds) / sizeof(kSn2Kinds[0]));
        return kSn2Kinds;
    }
    if (game == SonsOfTheForest) {
        count = (int)(sizeof(kSotfKinds) / sizeof(kSotfKinds[0]));
        return kSotfKinds;
    }
    count = 0;
    return nullptr;
}

bool World(int game, float& x0, float& z0, float& x1, float& z1) {
    if (game == Subnautica2) {
        x0 = -3886.9f;
        x1 = -800.6f;
        z0 = 5124.3f;
        z1 = 3621.2f;
        return true;
    }
    float e = game == Subnautica ? 2048.0f : game == SonsOfTheForest ? 2000.0f : 0.0f;
    if (e <= 0) return false;
    x0 = z0 = -e;
    x1 = z1 = e;
    return true;
}

}
