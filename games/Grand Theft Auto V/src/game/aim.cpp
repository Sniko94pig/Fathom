#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cmath>

namespace gtav {

namespace {

const int kHeadBone = 31086;

int Pick(float range) {
    int best = 0;
    float bestD = 1e9f;
    V3 cam = n::GET_FINAL_RENDERED_CAM_COORD();
    for (int ped : AllPeds()) {
        if (!ped || ped == st.ped || !n::DOES_ENTITY_EXIST(ped)) continue;
        if (!n::IS_PED_HUMAN(ped) || n::IS_PED_DEAD_OR_DYING(ped, 1)) continue;
        if (st.vehicle && n::IS_PED_IN_ANY_VEHICLE(ped, 0) && n::GET_VEHICLE_PED_IS_IN(ped, 0) == st.vehicle) continue;
        V3 head = n::GET_PED_BONE_COORDS(ped, kHeadBone, 0, 0, 0);
        if (Dist(head, cam) > range) continue;
        float sx = 0, sy = 0;
        if (!n::GET_SCREEN_COORD_FROM_WORLD_COORD(head.x, head.y, head.z, &sx, &sy)) continue;
        float dx = (sx - 0.5f) * 1.7778f, dy = sy - 0.5f;
        float d = dx * dx + dy * dy;
        if (d < bestD) {
            bestD = d;
            best = ped;
        }
    }
    return best;
}

}

void AimInit() { Ready("silent_aim", true); }

void AimTick() {}

void AimOff() {}

void SilentShots(uint32_t weapon, int shots) {
    if (!On("silent_aim") || !st.alive || shots <= 0) return;
    static const uint32_t sniper = Joaat("GROUP_SNIPER");
    float range = n::GET_WEAPONTYPE_GROUP(weapon) == sniper ? 1500.0f : 300.0f;
    int target = Pick(range);
    if (!target) return;
    V3 head = n::GET_PED_BONE_COORDS(target, kHeadBone, 0, 0, 0);
    V3 from = n::GET_FINAL_RENDERED_CAM_COORD();
    V3 dir = Norm(Sub(head, from));
    V3 start = Sub(head, Mul(dir, 0.6f));
    int damage = std::max(1, (int)std::lround(n::GET_WEAPON_DAMAGE(weapon, 0)));
    for (int i = 0; i < shots; ++i)
        n::SHOOT_SINGLE_BULLET_BETWEEN_COORDS_IGNORE_ENTITY(start.x, start.y, start.z, head.x, head.y, head.z, damage, 1, weapon, st.ped, 0, 1, -1.0f,
                                                           st.ped, 0);
}

}
