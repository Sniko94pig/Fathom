#include "game/internal.h"
#include "game/lists.h"
#include "ingame/log.h"
#include <algorithm>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace gtav {

namespace {

std::vector<uint32_t> known;
std::unordered_map<uint32_t, bool> gunCache;
std::unordered_map<uint32_t, std::string> weaponNames;

std::string Text(const char* label) {
    if (!label || !*label) return {};
    if (!n::DOES_TEXT_LABEL_EXIST(label)) return {};
    const char* t = n::GET_FILENAME_FOR_AUDIO_CONVERSATION(label);
    if (!t || !*t || std::string(t) == "NULL") return {};
    return t;
}

const char* Category(uint32_t group) {
    static const uint32_t pistol = Joaat("GROUP_PISTOL"), stun = Joaat("GROUP_STUNGUN"), smg = Joaat("GROUP_SMG"),
                          shotgun = Joaat("GROUP_SHOTGUN"), rifle = Joaat("GROUP_RIFLE"), mg = Joaat("GROUP_MG"),
                          sniper = Joaat("GROUP_SNIPER"), heavy = Joaat("GROUP_HEAVY"), thrown = Joaat("GROUP_THROWN"),
                          melee = Joaat("GROUP_MELEE"), petrol = Joaat("GROUP_PETROLCAN"), fire = Joaat("GROUP_FIREEXTINGUISHER"),
                          chute = Joaat("GROUP_PARACHUTE");
    if (group == pistol || group == stun) return "Pistols";
    if (group == smg) return "SMGs";
    if (group == shotgun) return "Shotguns";
    if (group == rifle) return "Assault rifles";
    if (group == mg) return "Machine guns";
    if (group == sniper) return "Sniper rifles";
    if (group == heavy) return "Heavy weapons";
    if (group == thrown) return "Throwables";
    if (group == melee) return "Melee";
    if (group == petrol || group == fire || group == chute) return "Armor and items";
    return nullptr;
}

void BuildList() {
    std::vector<core::Item> items;
    std::unordered_set<uint32_t> seen;
    for (size_t i = 0; i < lists::kWeaponCount; ++i) {
        const lists::Weapon& w = lists::kWeapons[i];
        uint32_t h = Joaat(w.model);
        if (!n::IS_WEAPON_VALID(h) || !seen.insert(h).second) continue;
        known.push_back(h);
        weaponNames[h] = w.name;
        items.push_back({"w:" + std::to_string(h), w.name, w.category});
    }
    int dlc = n::GET_NUM_DLC_WEAPONS_SP();
    int added = 0;
    for (int i = 0; i < dlc && i < 1024; ++i) {
        alignas(8) uint8_t data[512] = {};
        if (!n::GET_DLC_WEAPON_DATA_SP(i, data)) continue;
        uint32_t h = 0;
        std::memcpy(&h, data + 8, 4);
        if (!h || !n::IS_WEAPON_VALID(h) || !seen.insert(h).second) continue;
        char label[65] = {};
        std::memcpy(label, data + 56, 64);
        std::string name = Text(label);
        const char* cat = Category(n::GET_WEAPONTYPE_GROUP(h));
        if (name.empty() || !cat) continue;
        known.push_back(h);
        weaponNames[h] = name;
        items.push_back({"w:" + std::to_string(h), name, cat});
        ++added;
    }
    for (size_t i = 0; i < lists::kAmmoCount; ++i) items.push_back({std::string("ammo:") + lists::kAmmo[i].type, lists::kAmmo[i].name, "Ammo"});
    for (size_t i = 0; i < lists::kArmorCount; ++i)
        items.push_back({"armor:" + std::to_string(lists::kArmor[i].amount), lists::kArmor[i].name, "Armor and items"});
    for (size_t i = 0; i < lists::kSnackCount; ++i) items.push_back({"snack:" + std::to_string(i), lists::kSnacks[i].name, "Snacks"});
    LOG("item list: %zu items, %d weapons from add-on packs", items.size(), added);
    WithModel([&](core::Model& m) {
        m.items = std::move(items);
        m.categories = {"Pistols", "SMGs", "Shotguns", "Assault rifles", "Machine guns", "Sniper rifles", "Heavy weapons", "Throwables",
                        "Melee", "Ammo", "Armor and items", "Snacks"};
        m.itemNote = false;
        m.itemIds = false;
        m.duplicateButton = false;
        m.itemMax = 25;
    });
}

void GiveWeapon(uint32_t h, int count) {
    static const uint32_t thrown = Joaat("GROUP_THROWN");
    bool throwable = n::GET_WEAPONTYPE_GROUP(h) == thrown;
    bool had = n::HAS_PED_GOT_WEAPON(st.ped, h, 0) != 0;
    if (!had) n::GIVE_WEAPON_TO_PED(st.ped, h, throwable ? count : 9999, 0, 0);
    int max = 0;
    if (throwable) {
        if (had) n::ADD_AMMO_TO_PED(st.ped, h, count);
    } else if (n::GET_MAX_AMMO(st.ped, h, &max) && max > 0) {
        n::SET_PED_AMMO(st.ped, h, max, 0);
    }
    if (!n::HAS_PED_GOT_WEAPON(st.ped, h, 0)) {
        Note("The game did not give " + weaponNames[h]);
        return;
    }
    std::string name = weaponNames.count(h) ? weaponNames[h] : std::string("weapon");
    Note(throwable ? "Added " + std::to_string(count) + " x " + name : "Added " + name + " with full ammo");
}

void Give(const std::string& id, int count) {
    if (!st.ped || !st.alive) {
        Note("Items can be added when your character is alive");
        return;
    }
    count = std::clamp(count, 1, 25);
    try {
        if (id.rfind("w:", 0) == 0) {
            GiveWeapon((uint32_t)std::stoul(id.substr(2)), count);
        } else if (id.rfind("ammo:", 0) == 0) {
            std::string type = id.substr(5);
            for (size_t i = 0; i < lists::kAmmoCount; ++i) {
                if (type != lists::kAmmo[i].type) continue;
                n::ADD_PED_AMMO_BY_TYPE(st.ped, Joaat(lists::kAmmo[i].type), lists::kAmmo[i].rounds * count);
                Note("Added " + std::to_string(count) + " x " + lists::kAmmo[i].name);
            }
        } else if (id.rfind("armor:", 0) == 0) {
            int amount = std::clamp(std::stoi(id.substr(6)), 0, 100);
            int cap = n::GET_PLAYER_MAX_ARMOUR(st.player);
            if (cap > 0) amount = std::min(amount, cap);
            int now = n::GET_PED_ARMOUR(st.ped);
            n::SET_PED_ARMOUR(st.ped, std::max(now, amount));
            Note("Armor set to " + std::to_string(std::max(now, amount)));
        } else if (id.rfind("snack:", 0) == 0) {
            size_t i = (size_t)std::stoul(id.substr(6));
            if (i >= lists::kSnackCount) return;
            int hp = n::GET_ENTITY_HEALTH(st.ped), max = n::GET_ENTITY_MAX_HEALTH(st.ped);
            int room = std::max(0, max - 100);
            int heal = room * lists::kSnacks[i].percent * count / 100;
            n::SET_ENTITY_HEALTH(st.ped, std::min(max, hp + std::max(1, heal)), 0, 0);
            Note(std::string("You ate ") + lists::kSnacks[i].name + (count > 1 ? " x" + std::to_string(count) : std::string()));
        }
    } catch (...) {
        LOG("bad item id %s", id.c_str());
    }
}

}

const std::vector<uint32_t>& KnownWeapons() { return known; }

bool IsGun(uint32_t w) {
    if (!w) return false;
    auto it = gunCache.find(w);
    if (it != gunCache.end()) return it->second;
    static const uint32_t pistol = Joaat("GROUP_PISTOL"), stun = Joaat("GROUP_STUNGUN"), smg = Joaat("GROUP_SMG"),
                          shotgun = Joaat("GROUP_SHOTGUN"), rifle = Joaat("GROUP_RIFLE"), mg = Joaat("GROUP_MG"),
                          sniper = Joaat("GROUP_SNIPER"), heavy = Joaat("GROUP_HEAVY");
    uint32_t g = n::GET_WEAPONTYPE_GROUP(w);
    bool gun = g == pistol || g == stun || g == smg || g == shotgun || g == rifle || g == mg || g == sniper || g == heavy;
    gunCache[w] = gun;
    return gun;
}

void GiverInit(core::Model&) {
    BuildList();
    WithModel([](core::Model& m) {
        m.spawn = [](const core::Item& it, int count) {
            std::string id = it.id;
            Post([id, count] { Give(id, count); });
        };
    });
    Ready("item_giver", true);
}

void GiverTick() {}

void GiverOff() {}

}
