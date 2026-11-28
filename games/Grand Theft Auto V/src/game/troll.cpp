#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <random>
#include <unordered_map>

namespace gtav {

namespace {

std::mt19937 rng{12345u};

float Rand(float a, float b) { return std::uniform_real_distribution<float>(a, b)(rng); }

std::unordered_map<int, double> flung;
double nextFling = 0;

struct MobMember {
    uint32_t group = 0;
    uint32_t gave = 0;
    double tasked = 0;
};

struct Mob {
    bool active = false;
    uint32_t group = 0;
    std::unordered_map<int, MobMember> members;
    double nextScan = 0;
} mob;

std::unordered_map<int, double> ragdolled;
std::unordered_map<int, double> knocked;

struct Frozen {
    bool wasMission = false;
};
std::unordered_map<int, Frozen> frozen;

struct Hidden {
    int vehicle = 0;
} hidden;

const char* kMobWeapons[] = {"WEAPON_PISTOL", "WEAPON_MICROSMG", "WEAPON_PUMPSHOTGUN", "WEAPON_BAT", "WEAPON_KNIFE", "WEAPON_CROWBAR",
                             "WEAPON_COMBATPISTOL", "WEAPON_HAMMER"};

bool Usable(int ped) {
    return ped && ped != st.ped && n::DOES_ENTITY_EXIST(ped) && !n::IS_PED_A_PLAYER(ped) && n::IS_PED_HUMAN(ped) &&
           !n::IS_PED_DEAD_OR_DYING(ped, 1);
}

bool InMyVehicle(int ped) { return st.vehicle && n::IS_PED_IN_ANY_VEHICLE(ped, 0) && n::GET_VEHICLE_PED_IS_IN(ped, 0) == st.vehicle; }

bool Touching(int ped) {
    if (n::IS_ENTITY_TOUCHING_ENTITY(ped, st.ped)) return true;
    if (st.vehicle && n::IS_ENTITY_TOUCHING_ENTITY(ped, st.vehicle)) return true;
    if (!st.vehicle && !n::IS_PED_IN_ANY_VEHICLE(ped, 0)) {
        V3 p = n::GET_ENTITY_COORDS(ped, false);
        float dz = std::fabs(p.z - st.pos.z);
        float dx = p.x - st.pos.x, dy = p.y - st.pos.y;
        if (dz < 1.2f && dx * dx + dy * dy < 0.95f * 0.95f) return true;
    }
    return false;
}

void FlingTick() {
    if (!On("fling_cars") || !st.ped) {
        flung.clear();
        return;
    }
    if (st.time < nextFling) return;
    nextFling = st.time + 0.2;
    for (auto it = flung.begin(); it != flung.end();) it = st.time - it->second > 30.0 ? flung.erase(it) : std::next(it);
    for (int v : AllVehicles()) {
        if (!v || v == st.vehicle || !n::DOES_ENTITY_EXIST(v)) continue;
        if (n::GET_PED_IN_VEHICLE_SEAT(v, -1, false) == st.ped) continue;
        V3 p = n::GET_ENTITY_COORDS(v, false);
        if (Dist(p, st.pos) > 35.0f) continue;
        auto f = flung.find(v);
        if (f != flung.end() && st.time - f->second < 3.0) continue;
        if (n::IS_ENTITY_IN_AIR(v)) continue;
        V3 away = Norm(V3{p.x - st.pos.x, p.y - st.pos.y, 0});
        float out = Rand(4.0f, 9.0f);
        n::SET_ENTITY_VELOCITY(v, away.x * out + Rand(-2, 2), away.y * out + Rand(-2, 2), Rand(18.0f, 26.0f));
        n::APPLY_FORCE_TO_ENTITY(v, 1, 0.0f, 0.0f, Rand(0.5f, 1.5f), Rand(-1.5f, 1.5f), Rand(-1.5f, 1.5f), 0.0f, 0, 0, 1, 1, 0, 1);
        flung[v] = st.time;
    }
}

void MobCalm(int ped, const MobMember& m) {
    if (!n::DOES_ENTITY_EXIST(ped) || n::IS_PED_DEAD_OR_DYING(ped, 1)) return;
    n::SET_PED_RELATIONSHIP_GROUP_HASH(ped, m.group);
    if (m.gave) n::REMOVE_WEAPON_FROM_PED(ped, m.gave);
    n::SET_PED_COMBAT_ATTRIBUTES(ped, 46, 0);
    n::SET_PED_COMBAT_ATTRIBUTES(ped, 5, 0);
    n::SET_PED_KEEP_TASK(ped, 0);
    n::CLEAR_PED_TASKS(ped);
    n::TASK_WANDER_STANDARD(ped, 10.0f, 10);
}

void MobRelease() {
    if (!mob.active) return;
    for (auto& [ped, m] : mob.members) MobCalm(ped, m);
    uint32_t player = Joaat("PLAYER"), male = Joaat("CIVMALE"), female = Joaat("CIVFEMALE");
    for (uint32_t other : {mob.group, player, male, female}) {
        n::CLEAR_RELATIONSHIP_BETWEEN_GROUPS(5, mob.group, other);
        n::CLEAR_RELATIONSHIP_BETWEEN_GROUPS(5, other, mob.group);
    }
    n::REMOVE_RELATIONSHIP_GROUP(mob.group);
    LOG("mob calmed: %zu people", mob.members.size());
    mob = Mob{};
}

void MobTick() {
    if (!On("angry_mob") || !st.ped) {
        MobRelease();
        return;
    }
    if (!mob.active) {
        uint32_t g = 0;
        if (!n::ADD_RELATIONSHIP_GROUP("FATHOM_MOB", &g) || !g) g = Joaat("FATHOM_MOB");
        mob.active = true;
        mob.group = g;
        uint32_t player = Joaat("PLAYER"), male = Joaat("CIVMALE"), female = Joaat("CIVFEMALE");
        for (uint32_t other : {g, player, male, female}) n::SET_RELATIONSHIP_BETWEEN_GROUPS(5, g, other);
        LOG("mob started");
    }
    if (st.time < mob.nextScan) return;
    mob.nextScan = st.time + 0.5;
    for (auto it = mob.members.begin(); it != mob.members.end();) {
        if (!n::DOES_ENTITY_EXIST(it->first) || n::IS_PED_DEAD_OR_DYING(it->first, 1)) {
            it = mob.members.erase(it);
            continue;
        }
        if (st.time - it->second.tasked > 4.0 && !n::IS_PED_IN_COMBAT(it->first, 0)) {
            n::TASK_COMBAT_HATED_TARGETS_AROUND_PED(it->first, 60.0f, 0);
            it->second.tasked = st.time;
        }
        ++it;
    }
    if (mob.members.size() >= 40) return;
    for (int ped : AllPeds()) {
        if (mob.members.size() >= 40) break;
        if (mob.members.count(ped) || !Usable(ped) || InMyVehicle(ped)) continue;
        if (n::IS_ENTITY_A_MISSION_ENTITY(ped)) continue;
        int type = n::GET_PED_TYPE(ped);
        if (type == 6 || type == 27 || type == 29) continue;
        if (Dist(n::GET_ENTITY_COORDS(ped, false), st.pos) > 40.0f) continue;
        MobMember m;
        m.group = n::GET_PED_RELATIONSHIP_GROUP_HASH(ped);
        uint32_t w = Joaat(kMobWeapons[std::uniform_int_distribution<int>(0, 7)(rng)]);
        if (!n::HAS_PED_GOT_WEAPON(ped, w, 0)) {
            n::GIVE_WEAPON_TO_PED(ped, w, 250, 0, 1);
            m.gave = w;
        } else {
            n::SET_CURRENT_PED_WEAPON(ped, w, 1);
        }
        n::SET_PED_RELATIONSHIP_GROUP_HASH(ped, mob.group);
        n::SET_PED_COMBAT_ATTRIBUTES(ped, 46, 1);
        n::SET_PED_COMBAT_ATTRIBUTES(ped, 5, 1);
        n::SET_PED_FLEE_ATTRIBUTES(ped, 0, 0);
        n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, 0);
        n::TASK_COMBAT_HATED_TARGETS_AROUND_PED(ped, 60.0f, 0);
        n::SET_PED_KEEP_TASK(ped, 1);
        m.tasked = st.time;
        mob.members[ped] = m;
    }
}

void KnockOut(int v) {
    int seats = n::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(v);
    for (int s = -1; s < seats && s < 16; ++s) {
        int occ = n::GET_PED_IN_VEHICLE_SEAT(v, s, false);
        if (!occ || occ == st.ped || !n::DOES_ENTITY_EXIST(occ)) continue;
        n::CLEAR_PED_TASKS_IMMEDIATELY(occ);
        n::SET_PED_CAN_RAGDOLL(occ, 1);
        n::SET_PED_TO_RAGDOLL(occ, 5000, 5000, 0, 0, 0, 0);
    }
    n::APPLY_FORCE_TO_ENTITY(v, 1, Rand(-3, 3), Rand(-3, 3), Rand(5.0f, 8.0f), Rand(-1, 1), Rand(-1, 1), 0.0f, 0, 0, 1, 1, 0, 1);
}

void TouchTick() {
    bool rag = On("ragdoll_touch") && st.ped && st.alive;
    bool freeze = On("freeze_touch") && st.ped && st.alive;
    if (!rag) {
        ragdolled.clear();
        knocked.clear();
    }
    if (!rag && !freeze) return;
    for (int ped : AllPeds()) {
        if (!Usable(ped) || InMyVehicle(ped)) continue;
        V3 p = n::GET_ENTITY_COORDS(ped, false);
        if (Dist(p, st.pos) > 9.0f) continue;
        if (!Touching(ped)) continue;
        if (freeze) {
            if (frozen.count(ped) || n::IS_PED_IN_ANY_VEHICLE(ped, 0)) continue;
            Frozen f;
            f.wasMission = n::IS_ENTITY_A_MISSION_ENTITY(ped) != 0;
            if (!f.wasMission) n::SET_ENTITY_AS_MISSION_ENTITY(ped, 1, 1);
            n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, 1);
            n::TASK_STAND_STILL(ped, -1);
            n::FREEZE_ENTITY_POSITION(ped, 1);
            frozen[ped] = f;
            continue;
        }
        auto r = ragdolled.find(ped);
        if (r != ragdolled.end() && st.time - r->second < 1.5) continue;
        if (n::IS_PED_RAGDOLL(ped)) continue;
        n::SET_PED_CAN_RAGDOLL(ped, 1);
        n::SET_PED_TO_RAGDOLL(ped, 4000, 4000, 0, 0, 0, 0);
        ragdolled[ped] = st.time;
    }
    if (!rag) return;
    for (int v : AllVehicles()) {
        if (!v || v == st.vehicle || !n::DOES_ENTITY_EXIST(v)) continue;
        if (Dist(n::GET_ENTITY_COORDS(v, false), st.pos) > 15.0f) continue;
        bool touch = n::IS_ENTITY_TOUCHING_ENTITY(v, st.ped) || (st.vehicle && n::IS_ENTITY_TOUCHING_ENTITY(v, st.vehicle));
        if (!touch) continue;
        auto k = knocked.find(v);
        if (k != knocked.end() && st.time - k->second < 3.0) continue;
        KnockOut(v);
        knocked[v] = st.time;
    }
}

void FreezeHold() {
    for (auto it = frozen.begin(); it != frozen.end();) {
        if (!n::DOES_ENTITY_EXIST(it->first)) {
            it = frozen.erase(it);
            continue;
        }
        n::FREEZE_ENTITY_POSITION(it->first, 1);
        ++it;
    }
}

void FreezeRelease() {
    for (auto& [ped, f] : frozen) {
        if (!n::DOES_ENTITY_EXIST(ped)) continue;
        n::FREEZE_ENTITY_POSITION(ped, 0);
        n::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(ped, 0);
        n::CLEAR_PED_TASKS(ped);
        if (!f.wasMission) {
            int p = ped;
            n::SET_PED_AS_NO_LONGER_NEEDED(&p);
        }
    }
    if (!frozen.empty()) LOG("unfroze %zu people", frozen.size());
    frozen.clear();
}

void HiddenRelease() {
    if (hidden.vehicle && n::DOES_ENTITY_EXIST(hidden.vehicle)) n::SET_ENTITY_VISIBLE(hidden.vehicle, 1, 0);
    hidden = Hidden{};
}

void HiddenTick() {
    int v = On("invisible_car") ? st.vehicle : 0;
    if (!v) {
        HiddenRelease();
        return;
    }
    if (hidden.vehicle && hidden.vehicle != v) HiddenRelease();
    hidden.vehicle = v;
    n::SET_ENTITY_VISIBLE(v, 0, 0);
    if (st.ped && !n::IS_ENTITY_VISIBLE(st.ped)) n::SET_ENTITY_VISIBLE(st.ped, 1, 0);
}

}

void TrollInit() {
    Ready("fling_cars", true);
    Ready("angry_mob", true);
    Ready("ragdoll_touch", true);
    Ready("freeze_touch", true);
    Ready("invisible_car", true);
}

void TrollTick() {
    FlingTick();
    MobTick();
    TouchTick();
    if (On("freeze_touch")) FreezeHold();
    else if (!frozen.empty()) FreezeRelease();
    HiddenTick();
}

void TrollOff() {
    flung.clear();
    MobRelease();
    ragdolled.clear();
    knocked.clear();
    FreezeRelease();
    HiddenRelease();
}

}
