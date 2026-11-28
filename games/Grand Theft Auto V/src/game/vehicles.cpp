#include "game/internal.h"
#include "game/lists.h"
#include "ingame/log.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <unordered_set>

namespace gtav {

namespace {

struct SpawnJob {
    uint32_t model = 0;
    int count = 1;
    double started = 0;
    std::string name;
};

std::vector<SpawnJob> jobs;
std::unordered_map<uint32_t, std::string> names;

struct Armor {
    bool active = false;
    int vehicle = 0;
    bool canDamage = true;
    bool tyresBurst = true;
    bool haveProofs = false;
    int proofs[8] = {0, 0, 0, 0, 0, 0, 0, 0};
} armor;

std::string Text(const char* label) {
    if (!label || !*label) return {};
    if (!n::DOES_TEXT_LABEL_EXIST(label)) return {};
    const char* t = n::GET_FILENAME_FOR_AUDIO_CONVERSATION(label);
    if (!t || !*t || std::string(t) == "NULL") return {};
    return t;
}

std::string Title(std::string s) {
    bool up = true;
    for (char& c : s) {
        if (c == '_' || c == ' ') {
            c = ' ';
            up = true;
            continue;
        }
        c = up ? (char)std::toupper((unsigned char)c) : (char)std::tolower((unsigned char)c);
        up = false;
    }
    return s;
}

void BuildList(core::Model&) {
    std::vector<std::pair<uint32_t, std::string>> cands;
    std::unordered_set<uint32_t> seen;
    for (size_t i = 0; i < lists::kVehicleCount; ++i) {
        uint32_t h = Joaat(lists::kVehicles[i]);
        if (seen.insert(h).second) cands.push_back({h, lists::kVehicles[i]});
    }
    int dlc = n::GET_NUM_DLC_VEHICLES();
    for (int i = 0; i < dlc && i < 4096; ++i) {
        uint32_t h = n::GET_DLC_VEHICLE_MODEL(i);
        if (h && seen.insert(h).second) cands.push_back({h, std::string()});
    }
    std::vector<core::Item> items;
    std::unordered_map<std::string, int> nameCount;
    std::vector<std::pair<core::Item, std::string>> pending;
    bool used[32] = {};
    for (auto& [h, model] : cands) {
        if (!n::IS_MODEL_IN_CDIMAGE(h) || !n::IS_MODEL_A_VEHICLE(h) || n::IS_THIS_MODEL_A_TRAIN(h)) continue;
        if (n::GET_VEHICLE_MODEL_NUMBER_OF_SEATS(h) <= 0) continue;
        const char* label = n::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(h);
        std::string labelStr = label ? label : "";
        std::string name = Text(label);
        if (name.empty()) name = Title(!model.empty() ? model : labelStr);
        if (name.empty()) continue;
        int cls = n::GET_VEHICLE_CLASS_FROM_NAME(h);
        if (cls < 0 || cls >= (int)lists::kVehicleClassCount || cls == 21) continue;
        used[cls] = true;
        core::Item it;
        it.id = std::to_string(h);
        it.name = name;
        it.category = lists::kVehicleClasses[cls];
        ++nameCount[name];
        std::string tag = !model.empty() ? model : labelStr;
        for (char& c : tag) c = (char)std::tolower((unsigned char)c);
        pending.push_back({it, tag});
    }
    for (auto& [it, tag] : pending) {
        core::Item x = it;
        if (nameCount[x.name] > 1 && !tag.empty()) x.name += " (" + tag + ")";
        names[(uint32_t)std::stoul(x.id)] = x.name;
        items.push_back(std::move(x));
    }
    std::sort(items.begin(), items.end(), [](const core::Item& a, const core::Item& b) { return a.name < b.name; });
    std::vector<std::string> cats;
    for (size_t c = 0; c < lists::kVehicleClassCount; ++c)
        if (c < 32 && used[c]) cats.push_back(lists::kVehicleClasses[c]);
    LOG("vehicle list: %zu vehicles in %zu classes (%d from add-on packs checked)", items.size(), cats.size(), dlc);
    WithModel([&](core::Model& m) {
        m.worldItems = std::move(items);
        m.worldCategories = std::move(cats);
    });
}

void Spawn(SpawnJob& j) {
    bool heli = n::IS_THIS_MODEL_A_HELI(j.model) != 0, plane = n::IS_THIS_MODEL_A_PLANE(j.model) != 0;
    V3 pos = st.pos;
    float heading = st.heading;
    bool air = false;
    if ((heli || plane) && st.vehicle && n::IS_ENTITY_IN_AIR(st.vehicle)) air = true;
    if ((heli || plane) && !st.vehicle && n::IS_ENTITY_IN_AIR(st.ped)) air = true;
    int first = 0;
    for (int i = 0; i < j.count; ++i) {
        V3 at;
        if (air && i == 0) {
            at = pos;
        } else {
            float side = (float)((i + 1) / 2) * (i % 2 ? 1.0f : -1.0f) * 5.0f;
            float ahead = plane ? 18.0f : heli ? 12.0f : 6.0f;
            at = n::GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(st.vehicle ? st.vehicle : st.ped, side, ahead, 0.5f);
        }
        int veh = n::CREATE_VEHICLE(j.model, at.x, at.y, at.z, heading, 0, 0, 0);
        if (!veh) continue;
        if (!(air && i == 0)) n::SET_VEHICLE_ON_GROUND_PROPERLY(veh, 5.0f);
        n::SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER(veh, 1);
        n::SET_VEHICLE_ENGINE_ON(veh, 1, 1, 0);
        if (heli) n::SET_HELI_BLADES_FULL_SPEED(veh);
        if (!first) {
            first = veh;
            if (st.alive) n::SET_PED_INTO_VEHICLE(st.ped, veh, -1);
            if (air && plane) n::SET_VEHICLE_FORWARD_SPEED(veh, 60.0f);
        } else {
            int copy = veh;
            n::SET_VEHICLE_AS_NO_LONGER_NEEDED(&copy);
        }
    }
    n::SET_MODEL_AS_NO_LONGER_NEEDED(j.model);
    if (!first) {
        Note("Could not spawn " + j.name);
        return;
    }
    int keep = first;
    n::SET_VEHICLE_AS_NO_LONGER_NEEDED(&keep);
    Note("Spawned " + j.name + (j.count > 1 ? " x" + std::to_string(j.count) : std::string()));
    LOG("spawned %s (%u) x%d", j.name.c_str(), j.model, j.count);
}

void JobsTick() {
    if (jobs.empty()) return;
    SpawnJob& j = jobs.front();
    if (!st.ped || !st.alive || st.cutscene) {
        if (Now() - j.started > 10.0) {
            Note("Could not spawn " + j.name + " right now");
            jobs.erase(jobs.begin());
        }
        return;
    }
    if (!n::HAS_MODEL_LOADED(j.model)) {
        n::REQUEST_MODEL(j.model);
        if (Now() - j.started > 8.0) {
            Note("The game did not load " + j.name);
            jobs.erase(jobs.begin());
        }
        return;
    }
    SpawnJob job = j;
    jobs.erase(jobs.begin());
    Spawn(job);
}

void ArmorApply(int v) {
    n::SET_ENTITY_INVINCIBLE(v, 1, 0);
    n::SET_ENTITY_PROOFS(v, 1, 1, 1, 1, 1, 1, 0, 1);
    n::SET_ENTITY_CAN_BE_DAMAGED(v, 0);
    n::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(v, 0);
    n::SET_VEHICLE_TYRES_CAN_BURST(v, 0);
    n::SET_VEHICLE_WHEELS_CAN_BREAK(v, 0);
    n::SET_VEHICLE_ENGINE_CAN_DEGRADE(v, 0);
    n::SET_VEHICLE_CAN_BREAK(v, 0);
    n::SET_VEHICLE_EXPLODES_ON_HIGH_EXPLOSION_DAMAGE(v, 0);
    n::SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE(v, 1);
    n::SET_DISABLE_VEHICLE_PETROL_TANK_FIRES(v, 1);
}

void ArmorRelease() {
    if (!armor.active) return;
    int v = armor.vehicle;
    if (v && n::DOES_ENTITY_EXIST(v)) {
        n::SET_ENTITY_INVINCIBLE(v, 0, 0);
        if (armor.haveProofs)
            n::SET_ENTITY_PROOFS(v, armor.proofs[0], armor.proofs[1], armor.proofs[2], armor.proofs[3], armor.proofs[4], armor.proofs[5],
                                 armor.proofs[6], armor.proofs[7]);
        else
            n::SET_ENTITY_PROOFS(v, 0, 0, 0, 0, 0, 0, 0, 0);
        n::SET_ENTITY_CAN_BE_DAMAGED(v, armor.canDamage ? 1 : 0);
        n::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(v, 1);
        n::SET_VEHICLE_TYRES_CAN_BURST(v, armor.tyresBurst ? 1 : 0);
        n::SET_VEHICLE_WHEELS_CAN_BREAK(v, 1);
        n::SET_VEHICLE_ENGINE_CAN_DEGRADE(v, 1);
        n::SET_VEHICLE_CAN_BREAK(v, 1);
        n::SET_VEHICLE_EXPLODES_ON_HIGH_EXPLOSION_DAMAGE(v, 1);
        n::SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE(v, 0);
        n::SET_DISABLE_VEHICLE_PETROL_TANK_FIRES(v, 0);
    }
    armor = Armor{};
}

void ArmorTick() {
    int v = On("vehicle_god_mode") ? st.vehicle : 0;
    if (!v) {
        ArmorRelease();
        return;
    }
    if (armor.active && armor.vehicle != v) ArmorRelease();
    if (!armor.active) {
        armor.active = true;
        armor.vehicle = v;
        armor.canDamage = n::GET_ENTITY_CAN_BE_DAMAGED(v) != 0;
        armor.tyresBurst = n::GET_VEHICLE_TYRES_CAN_BURST(v) != 0;
        int* p = armor.proofs;
        armor.haveProofs = n::GET_ENTITY_PROOFS(v, &p[0], &p[1], &p[2], &p[3], &p[4], &p[5], &p[6], &p[7]) != 0;
        LOG("vehicle god mode on for vehicle %d", v);
    }
    ArmorApply(v);
}

void Repair() {
    int v = st.vehicle;
    if (!v) {
        Note("Get in a vehicle first");
        return;
    }
    n::SET_VEHICLE_FIXED(v);
    n::SET_VEHICLE_DEFORMATION_FIXED(v);
    n::SET_VEHICLE_UNDRIVEABLE(v, 0);
    n::SET_VEHICLE_ENGINE_HEALTH(v, 1000.0f);
    n::SET_VEHICLE_BODY_HEALTH(v, 1000.0f);
    n::SET_VEHICLE_PETROL_TANK_HEALTH(v, 1000.0f);
    for (int t = 0; t < 8; ++t) n::SET_VEHICLE_TYRE_FIXED(v, t);
    int max = n::GET_ENTITY_MAX_HEALTH(v);
    if (max > 0) n::SET_ENTITY_HEALTH(v, max, 0, 0);
    n::STOP_ENTITY_FIRE(v);
    n::SET_VEHICLE_DIRT_LEVEL(v, 0.0f);
    n::WASH_DECALS_FROM_VEHICLE(v, 1.0f);
    Note("Vehicle repaired and cleaned");
}

void Steal() {
    if (!st.ped || !st.alive) {
        Note("Steal nearest car works when your character is alive");
        return;
    }
    int best = 0;
    float bestD = 150.0f;
    for (int v : AllVehicles()) {
        if (!v || v == st.vehicle || !n::DOES_ENTITY_EXIST(v)) continue;
        if (!n::IS_VEHICLE_DRIVEABLE(v, 0)) continue;
        uint32_t model = (uint32_t)n::GET_ENTITY_MODEL(v);
        if (n::IS_THIS_MODEL_A_TRAIN(model) || n::GET_VEHICLE_MODEL_NUMBER_OF_SEATS(model) <= 0) continue;
        float d = Dist(n::GET_ENTITY_COORDS(v, false), st.pos);
        if (d < bestD) {
            bestD = d;
            best = v;
        }
    }
    if (!best) {
        Note("No car nearby to steal");
        return;
    }
    int driver = n::GET_PED_IN_VEHICLE_SEAT(best, -1, false);
    if (driver && driver != st.ped && n::DOES_ENTITY_EXIST(driver)) {
        n::CLEAR_PED_TASKS_IMMEDIATELY(driver);
        n::SET_PED_TO_RAGDOLL(driver, 3000, 3000, 0, 0, 0, 0);
        V3 away = Norm(Sub(n::GET_ENTITY_COORDS(driver, false), n::GET_ENTITY_COORDS(best, false)));
        n::APPLY_FORCE_TO_ENTITY(driver, 1, away.x * 6.0f, away.y * 6.0f, 3.0f, 0, 0, 0, 0, 0, 1, 1, 0, 1);
    }
    n::SET_VEHICLE_DOORS_LOCKED(best, 1);
    n::SET_VEHICLE_NEEDS_TO_BE_HOTWIRED(best, 0);
    n::SET_PED_INTO_VEHICLE(st.ped, best, -1);
    n::SET_VEHICLE_ENGINE_ON(best, 1, 1, 0);
    uint32_t model = (uint32_t)n::GET_ENTITY_MODEL(best);
    std::string name = Text(n::GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(model));
    Note(name.empty() ? "Car stolen" : "Stole the " + name);
}

}

void VehiclesInit(core::Model& m) {
    BuildList(m);
    WithModel([](core::Model& mm) {
        mm.spawnWorld = [](const core::Item& it, int count) {
            uint32_t model = 0;
            try {
                model = (uint32_t)std::stoul(it.id);
            } catch (...) {
                return;
            }
            std::string name = it.name;
            int copies = std::clamp(count, 1, 10);
            Post([model, name, copies] {
                SpawnJob j;
                j.model = model;
                j.count = copies;
                j.started = Now();
                j.name = name;
                jobs.push_back(j);
            });
        };
    });
    Ready("spawn_cars", true);
    Ready("vehicle_god_mode", true);
    Ready("car_repair", true);
    Ready("steal_car", true);
}

void VehiclesTick() {
    JobsTick();
    ArmorTick();
    if (Pressed("car_repair")) Repair();
    if (Pressed("steal_car")) Steal();
}

void VehiclesOff() {
    ArmorRelease();
    for (const SpawnJob& j : jobs) n::SET_MODEL_AS_NO_LONGER_NEEDED(j.model);
    jobs.clear();
}

}
