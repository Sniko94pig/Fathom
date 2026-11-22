#include "core/ultrakill_features.h"
#include "imgui.h"
#include "ui/icons.h"

namespace uk {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

const Tab Player = Tab::Survival;
const Tab Movement = Tab::Crafting;
const Tab Weapons = Tab::World;
const Tab Level = Tab::Biomods;

Feature& Add(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    Feature f;
    f.tab = tab;
    f.group = group;
    f.id = id;
    f.label = label;
    f.hint = hint;
    m.features.push_back(std::move(f));
    return m.features.back();
}

Feature& Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    return Add(m, tab, group, id, label, hint);
}

Feature& Slider(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, float mn, float mx,
                float def, float step, bool counts = true, const char* fmt = "%.1fx") {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Slider;
    f.min = mn;
    f.max = mx;
    f.def = def;
    f.value = def;
    f.step = step;
    f.fmt = fmt;
    f.counts = counts;
    return f;
}

Feature& Action(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, const char* button,
                bool confirm = false) {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Action;
    f.actionLabel = button;
    f.confirm = confirm;
    return f;
}

}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(32);
    m.tabs = {Player, Movement, Weapons, Level, Tab::Settings};
    m.plainTabs = {Level};
    m.tabLabels = {{Player, "Player", ICON_HEART_PULSE},
                   {Movement, "Movement", ICON_NAVIGATION},
                   {Weapons, "Weapons", ICON_CROSSHAIR},
                   {Level, "Level", ICON_MAP}};

    Toggle(m, Player, "Health", "godmode", "God mode", "Nothing can hurt or kill you");
    Toggle(m, Player, "Health", "unlimited_health", "Unlimited health", "Health refills as soon as it drops, but instant kills still kill");
    Action(m, Player, "Health", "refill_health", "Refill health", "Health back to full once", "Refill");
    Slider(m, Player, "Time", "game_speed", "Game speed", "Speeds up or slows down the whole game", 0.1f, 3.0f, 1.0f, 0.1f);

    Toggle(m, Movement, "Dash & jumps", "unlimited_dash", "Unlimited dash", "Dash as often as you want, stamina never runs out");
    Toggle(m, Movement, "Dash & jumps", "unlimited_jumps", "Unlimited jumps", "Jump again in mid-air as often as you want");
    Toggle(m, Movement, "Dash & jumps", "unlimited_walljumps", "Unlimited wall jumps", "Wall jump as many times as you want before you land");
    Slider(m, Movement, "Speed", "player_speed", "Player speed", "How fast you move", 1.0f, 5.0f, 1.0f, 0.1f);
    Toggle(m, Movement, "Flying", "fly", "Fly", "Fly in any direction, set how fast with Fly speed");
    Slider(m, Movement, "Flying", "fly_speed", "Fly speed", "Fly and noclip speed", 1.0f, 10.0f, 1.0f, 0.5f, false);
    Toggle(m, Movement, "Flying", "noclip", "Noclip", "Move through walls, floors and ceilings");

    Toggle(m, Weapons, "Shooting", "unlimited_shots", "Unlimited shots", "Every gun keeps firing with no ammo limit or cooldown, as fast as Fire rate");
    Slider(m, Weapons, "Shooting", "fire_rate", "Fire rate", "How fast every gun shoots while Unlimited shots is on", 1.0f, 20.0f, 5.0f, 0.5f, false);
    Toggle(m, Weapons, "Shooting", "instant_charge", "Instant charge", "Charge attacks are ready right away, on every weapon that has one");
    Toggle(m, Weapons, "Shooting", "no_reload", "No reload", "Weapons never reload or recharge, like coins and the Railcannon");
    Action(m, Weapons, "Unlocks", "unlock_all_weapons", "Unlock all weapons", "Unlocks every weapon and every weapon variant in your save", "Unlock", true);
    Toggle(m, Weapons, "Unlocks", "free_shop", "Free shop", "Weapons and variants in the shop cost 0 P");
    Toggle(m, Weapons, "Aiming", "softaim", "Softaim", "While you hold Fire or Alt fire, your aim pulls to the head of the visible enemy closest to your crosshair");
    Slider(m, Weapons, "Aiming", "aim_strength", "Aim strength", "How hard Softaim pulls your aim, 10 locks on", 1.0f, 10.0f, 10.0f, 1.0f, false, "%.0f");
    Slider(m, Weapons, "Aiming", "aim_area", "Aim area", "How far from the crosshair Softaim grabs an enemy, in degrees", 5.0f, 90.0f, 30.0f, 5.0f, false, "%.0f");

    Toggle(m, Level, "Orbs", "orb_esp", "Orb ESP", "Shows secret orbs and red soul orbs through walls, with their distance");
    Action(m, Level, "Orbs", "collect_all_orbs", "Collect all orbs", "Collects every secret orb still to find in this level (red soul orbs stay), your saves are backed up first", "Collect", true);
    Action(m, Level, "Enemies", "kill_enemies_in_range", "Kill all enemies in range", "Kills every enemy within Kill range", "Kill");
    Slider(m, Level, "Enemies", "kill_range", "Kill range", "How far Kill all enemies in range reaches", 5.0f, 1000.0f, 50.0f, 5.0f, false, "%.0f m");
    Toggle(m, Level, "Challenge", "challenge_shower", "Challenge shower", "Shows every unlocked level's challenge in the level select before you beat it, and as a note when the level starts");

    m.gameName = "ULTRAKILL";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
