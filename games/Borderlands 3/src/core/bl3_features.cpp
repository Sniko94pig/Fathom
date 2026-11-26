#include "core/bl3_features.h"
#include "imgui.h"
#include "ui/icons.h"
#include <algorithm>

namespace bl3 {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

const Tab Player = Tab::Survival;
const Tab Weapons = Tab::Crafting;
const Tab Aim = Tab::World;
const Tab Skills = Tab::Biomods;
const Tab Movement = Tab::BaseHub;
const Tab LootMap = Tab::Spawner;
const Tab Vehicle = Tab::Legit;

const char* kAlone[] = {"no_self_damage", "invisibility", "ignore_level_req", "silent_aim", "aim_assist", "enemy_esp", "loot_esp",
                        "kill_nearby", "game_speed", "legendary_drops", "drop_filter", "free_spins", "mayhem_reroll"};

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

Feature& Option(Model& m, Tab tab, const char* group, const char* id, const char* label, bool on) {
    Feature& f = Add(m, tab, group, id, label, "");
    f.counts = false;
    f.on = on;
    return f;
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

Feature& Page(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, const char* page) {
    Feature& f = Action(m, tab, group, id, label, hint, "Open");
    f.page = page;
    return f;
}

Feature& Input(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, const char* placeholder) {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Input;
    f.inputs = {""};
    f.inputHints = {placeholder};
    f.actionLabel = "Set";
    return f;
}

}

bool AloneOnly(const std::string& id) {
    return std::any_of(std::begin(kAlone), std::end(kAlone), [&](const char* a) { return id == a; });
}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(96);
    m.tabs = {Player, Weapons, Aim, Skills, Movement, LootMap, Vehicle, Tab::Settings};
    m.plainTabs = {Skills, Movement, LootMap, Vehicle};
    m.tabLabels = {{Player, "Player", ICON_HEART_PULSE},   {Weapons, "Weapons", ICON_CROSSHAIR}, {Aim, "Aim & ESP", ICON_EYE},
                   {Skills, "Skills", ICON_ZAP},           {Movement, "Movement", ICON_NAVIGATION}, {LootMap, "Loot & Map", ICON_PACKAGE},
                   {Vehicle, "Vehicle", ICON_GAUGE}};
    m.categories = {"Pistols", "SMGs", "Assault rifles", "Shotguns", "Sniper rifles", "Heavy weapons", "Shields", "Grenades", "Class mods",
                    "Artifacts", "Ammo"};

    Toggle(m, Player, "Health", "unlimited_health", "Unlimited health", "Health refills as soon as it drops and you can't die");
    Toggle(m, Player, "Health", "god_mode", "God mode", "Nothing can hurt you, using the game's own god mode");
    Toggle(m, Player, "Health", "unlimited_shield", "Unlimited shield", "Your shield refills as soon as it drops");
    Action(m, Player, "Health", "heal_health", "Heal health", "Health back to full once", "Heal");
    Action(m, Player, "Health", "restore_shield", "Restore shield", "Shield back to full once", "Restore");
    Toggle(m, Player, "Health", "no_self_damage", "No self-damage",
           "Your own explosions, splash and skills don't hurt you. Only when you play alone");
    Toggle(m, Player, "Stealth", "invisibility", "Invisibility", "Enemies can't see or target you. Only when you play alone");
    Slider(m, Player, "Level & XP", "mega_xp", "Mega XP", "Every bit of XP you earn is multiplied", 1.0f, 100.0f, 1.0f, 1.0f, true, "%.0fx");
    Slider(m, Player, "Level & XP", "give_level", "Level to give", "Pick a level, then press Give. It only goes up", 2.0f, 72.0f, 72.0f, 1.0f,
           false, "%.0f");
    Action(m, Player, "Level & XP", "give_level_apply", "Give level", "Raises your level to the one on the slider, your saves are backed up first",
           "Give", true);
    Input(m, Player, "Currency", "money", "Money", "Type the amount and press Set, your saves are backed up first", "Amount");
    Input(m, Player, "Currency", "eridium", "Eridium", "Type the amount and press Set, your saves are backed up first", "Amount");
    Input(m, Player, "Currency", "golden_keys", "Golden keys", "Type the amount and press Set, your saves are backed up first", "Amount");

    Toggle(m, Weapons, "Ammo", "unlimited_ammo", "Unlimited ammo", "Your guns never run out of ammo");
    Toggle(m, Weapons, "Ammo", "no_reload", "No reload", "The magazine stays full while your spare ammo still goes down");
    Toggle(m, Weapons, "Ammo", "instant_reload", "Instant reload", "Reloading is done right away");
    Slider(m, Weapons, "Handling", "fire_rate", "Fire rate", "How fast your guns shoot", 1.0f, 10.0f, 1.0f, 0.5f);
    Toggle(m, Weapons, "Handling", "no_recoil", "No recoil", "No recoil, spread or sway");
    Toggle(m, Weapons, "Handling", "no_charge", "No charge time", "Guns that charge up fire right away");
    Toggle(m, Weapons, "Handling", "no_overheat", "No overheat", "Guns and vehicle guns never overheat");
    Slider(m, Weapons, "Damage", "damage_mult", "Damage multiplier", "How much damage your guns do", 1.0f, 100.0f, 1.0f, 1.0f, true, "%.0fx");
    Toggle(m, Weapons, "Damage", "one_hit_kill", "One-hit kills", "Your guns kill any enemy in one hit");
    Toggle(m, Weapons, "Damage", "always_crit", "Always crits", "Every hit counts as a critical hit");
    Toggle(m, Weapons, "Gear", "ignore_level_req", "Ignore level requirements",
           "Use and equip gear above your level. Only when you play alone");

    Toggle(m, Aim, "Silent aim", "silent_aim", "Silent aim",
           "Your bullets fly to the head of the enemy closest to your crosshair without moving your camera. Only when you play alone");
    Option(m, Aim, "Silent aim", "silent_walls", "Shoot through walls", false);
    Option(m, Aim, "Silent aim", "silent_headshot", "Every shot is a headshot", true);
    Slider(m, Aim, "Silent aim", "silent_area", "Silent aim area", "How far from the crosshair silent aim picks an enemy, in degrees", 5.0f,
           180.0f, 30.0f, 5.0f, false, "%.0f");
    Toggle(m, Aim, "Aim assist", "aim_assist", "Aim assist",
           "While you aim or shoot, your aim pulls to the enemy closest to your crosshair. Only when you play alone");
    Slider(m, Aim, "Aim assist", "aim_strength", "Aim strength", "How hard aim assist pulls, 10 locks on", 1.0f, 10.0f, 5.0f, 1.0f, false,
           "%.0f");
    Slider(m, Aim, "Aim assist", "aim_area", "Aim area", "How far from the crosshair aim assist grabs an enemy, in degrees", 5.0f, 90.0f,
           20.0f, 5.0f, false, "%.0f");
    Toggle(m, Aim, "Enemy ESP", "enemy_esp", "Enemy ESP", "Shows enemies through walls. Only when you play alone");
    Option(m, Aim, "Enemy ESP", "esp_name", "Show name", true);
    Option(m, Aim, "Enemy ESP", "esp_health", "Show health", true);
    Option(m, Aim, "Enemy ESP", "esp_distance", "Show distance", true);
    Option(m, Aim, "Enemy ESP", "esp_box", "Show box", true);
    Toggle(m, Aim, "Loot ESP", "loot_esp", "Loot ESP", "Shows loot and chests through walls in their rarity colour. Only when you play alone");
    Option(m, Aim, "Loot ESP", "lootesp_common", "Show white (common)", false);
    Option(m, Aim, "Loot ESP", "lootesp_uncommon", "Show green (uncommon)", true);
    Option(m, Aim, "Loot ESP", "lootesp_rare", "Show blue (rare)", true);
    Option(m, Aim, "Loot ESP", "lootesp_epic", "Show purple (epic)", true);
    Option(m, Aim, "Loot ESP", "lootesp_legendary", "Show orange (legendary)", true);
    Option(m, Aim, "Loot ESP", "lootesp_chests", "Show chests", true);
    Option(m, Aim, "Loot ESP", "lootesp_name", "Show name", true);
    Option(m, Aim, "Loot ESP", "lootesp_distance", "Show distance", true);
    Action(m, Aim, "Enemies", "kill_nearby", "Kill all nearby enemies", "Kills every enemy within Kill range. Only when you play alone", "Kill");
    Slider(m, Aim, "Enemies", "kill_range", "Kill range", "How far Kill all nearby enemies reaches", 5.0f, 300.0f, 50.0f, 5.0f, false,
           "%.0f m");

    Toggle(m, Skills, "Cooldown", "instant_cooldown", "Instant cooldown", "Your action skill is ready again right after you use it");
    Action(m, Skills, "Cooldown", "reset_cooldown", "Reset cooldown", "Your action skill is ready right now", "Reset");
    Toggle(m, Skills, "Duration", "infinite_duration", "Infinite duration", "Action skills that last a while never run out");
    Action(m, Skills, "Duration", "restore_duration", "Restore duration", "Fills the running action skill's time back up", "Restore");

    Slider(m, Movement, "Speed", "player_speed", "Player speed", "How fast you walk and sprint", 1.0f, 5.0f, 1.0f, 0.1f);
    Slider(m, Movement, "Speed", "jump_height", "Jump height", "How high you jump", 1.0f, 10.0f, 1.0f, 0.5f);
    Toggle(m, Movement, "Flying", "fly", "Fly", "Fly in any direction, walls still stop you. Space goes up, Ctrl goes down");
    Toggle(m, Movement, "Flying", "noclip", "Noclip", "Walk through walls and objects while you stay on the ground");
    Slider(m, Movement, "Flying", "fly_speed", "Fly speed", "Speed for fly, noclip and freecam", 1.0f, 10.0f, 2.0f, 0.5f, false);
    Toggle(m, Movement, "Camera", "freecam", "Freecam", "Move the camera freely with WASD, Space and Ctrl while your character stays put");
    Slider(m, Movement, "Time", "game_speed", "Game speed", "Speeds up or slows down the whole game. Only when you play alone", 0.1f, 5.0f,
           1.0f, 0.1f);

    Page(m, LootMap, "Giver", "giver", "Giver", "Give yourself the best guns, shields, grenades, class mods, artifacts and ammo", "add_items");
    Action(m, LootMap, "Giver", "refill_ammo", "Refill all ammo", "Fills every ammo type and your grenades", "Refill");
    Page(m, LootMap, "Map", "map", "Map", "Map of where you are with every location, click to teleport. Also lists every map in the game",
         "map");
    Toggle(m, LootMap, "Drops", "legendary_drops", "Legendary drops", "Every gun and gear that drops is Legendary. Only when you play alone");
    Toggle(m, LootMap, "Drops", "drop_filter", "Rarity drop filter", "Only the rarities you tick below drop. Only when you play alone");
    Option(m, LootMap, "Drops", "drop_common", "Drop white (common)", false);
    Option(m, LootMap, "Drops", "drop_uncommon", "Drop green (uncommon)", false);
    Option(m, LootMap, "Drops", "drop_rare", "Drop blue (rare)", true);
    Option(m, LootMap, "Drops", "drop_epic", "Drop purple (epic)", true);
    Option(m, LootMap, "Drops", "drop_legendary", "Drop orange (legendary)", true);
    Toggle(m, LootMap, "Fun", "free_spins", "Free slot machine spins", "Moxxi's slot machines cost nothing. Only when you play alone");
    Action(m, LootMap, "Fun", "mayhem_reroll", "Mayhem modifier reroller",
           "Picks new random Mayhem modifiers, the map reloads like changing Mayhem at the console. Only when you play alone", "Reroll",
           true);

    Toggle(m, Vehicle, "Vehicle", "vehicle_boost", "Unlimited vehicle boost", "Boost never runs out in the car you drive");
    Toggle(m, Vehicle, "Vehicle", "vehicle_god", "Invincible vehicle", "The car you drive can't be damaged");
    Toggle(m, Vehicle, "Vehicle", "vehicle_fly", "Vehicle fly", "Your car flies where you look, Space goes up, Ctrl goes down");
    Toggle(m, Vehicle, "Vehicle", "vehicle_noclip", "Vehicle noclip", "Your car drives through everything, use with care");
    Action(m, Vehicle, "Teleport", "bring_vehicle", "Bring my car", "Moves the last car you drove next to you. The map teleport also takes your car with you",
           "Bring");

    m.gameName = "Borderlands 3";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
