#include "game/internal.h"
#include <algorithm>

namespace bl3 {

namespace {

std::vector<void*> MyAbilities() {
    std::vector<void*> out;
    if (!st.pawn) return out;
    void* mgr = ue::Ptr(st.pawn, "PlayerAbilityManagerComponent");
    for (void* a : Objects(Scan::Abilities)) {
        if (!ue::Alive(a)) continue;
        bool mine = false;
        int guard = 0;
        for (void* o = ue::OuterOf(a); o && guard < 8; o = ue::OuterOf(o), ++guard)
            if (o == st.pawn || o == st.pc || (mgr && o == mgr)) {
                mine = true;
                break;
            }
        if (mine) out.push_back(a);
    }
    return out;
}

float Percent(void* ability, const char* fn) {
    ue::Call c(ability, fn);
    return c.Run() ? c.Get<float>("ReturnValue") : -1.0f;
}

bool Active(void* ability) {
    ue::Call c(ability, "IsActive");
    return c.Run() && c.GetBool("ReturnValue");
}

bool Simple(void* ability, const char* fn) {
    ue::Call c(ability, fn);
    return c.Run();
}

}

void SkillsInit() {
    Ready("instant_cooldown", HasFunc("OakActionAbility", "ResetActionAbilityCooldown") && HasFunc("OakActionAbility", "GetCooldownPercent"));
    Ready("reset_cooldown", HasFunc("OakActionAbility", "ResetActionAbilityCooldown"));
    Ready("infinite_duration", HasFunc("OakActionAbility", "ResetDurationTimer") && HasFunc("OakActionAbility", "GetDurationPercent"));
    Ready("restore_duration", HasFunc("OakActionAbility", "ResetDurationTimer"));
}

void SkillsTick() {
    bool instant = On("instant_cooldown"), infinite = On("infinite_duration");
    bool reset = Pressed("reset_cooldown"), restore = Pressed("restore_duration");
    if (!instant && !infinite && !reset && !restore) return;
    if ((instant || infinite) && !reset && !restore && (st.tick % 6) != 0) return;
    std::vector<void*> abilities = MyAbilities();
    if ((reset || restore) && abilities.empty()) {
        Note(ScanComplete(Scan::Abilities) ? "No action skill found" : "Still looking for your action skill, try again in a second");
        return;
    }
    int resets = 0, restores = 0;
    for (void* a : abilities) {
        bool active = Active(a);
        float cool = (instant && !active) ? Percent(a, "GetCooldownPercent") : -1.0f;
        if (reset || (cool >= 0.0f && cool < 0.999f)) {
            if (Simple(a, "ResetActionAbilityCooldown")) ++resets;
        }
        float left = (infinite && active) ? Percent(a, "GetDurationPercent") : -1.0f;
        if (active && (restore || (left >= 0.0f && left < 0.95f))) {
            if (Simple(a, "ResetDurationTimer")) ++restores;
        }
    }
    if (reset) Note(resets ? "Action skill cooldown reset" : "Could not reset the cooldown");
    if (restore) Note(restores ? "Action skill duration restored" : "No action skill is running right now");
}

void SkillsOff() {}

}
