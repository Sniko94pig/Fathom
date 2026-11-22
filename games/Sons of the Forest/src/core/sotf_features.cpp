#include "core/sotf_features.h"
#include "imgui.h"
#include "ui/icons.h"

namespace sotf {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

const Tab Player = Tab::Survival;
const Tab Combat = Tab::Biomods;
const Tab Building = Tab::Crafting;
const Tab World = Tab::World;
const Tab Companions = Tab::BaseHub;
const Tab Spawner = Tab::Spawner;

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

void Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    Add(m, tab, group, id, label, hint);
}

void Slider(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, float mn, float mx,
            float def, const char* fmt = "%.1fx") {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Slider;
    f.min = mn;
    f.max = mx;
    f.def = def;
    f.value = def;
    f.fmt = fmt;
}

void Action(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, const char* button) {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Action;
    f.actionLabel = button;
}

}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(96);
    m.tabs = {Player, Combat, Building, World, Companions, Spawner, Tab::Settings};
    m.plainTabs = {Combat, Companions, Spawner};
    m.tabLabels = {{Player, "Player", ICON_HEART_PULSE},
                   {Combat, "Combat", ICON_CROSSHAIR},
                   {Building, "Building", ICON_HAMMER},
                   {World, "World", ICON_GLOBE},
                   {Companions, "Kelvin & Virginia", ICON_USERS},
                   {Spawner, "Spawner & Map", ICON_PACKAGE}};

    Toggle(m, Player, "Vitals", "godmode", "Godmode", "Nothing can kill you");
    Toggle(m, Player, "Vitals", "unlimited_health", "Unlimited health", "Health refills as soon as it drops");
    Toggle(m, Player, "Vitals", "unlimited_stamina", "Unlimited stamina", "Stamina never runs out");
    Toggle(m, Player, "Vitals", "unlimited_hydration", "Unlimited hydration", "Thirst stays full");
    Toggle(m, Player, "Vitals", "unlimited_fullness", "Unlimited fullness", "Hunger stays full");
    Toggle(m, Player, "Vitals", "unlimited_temperature", "Unlimited temperature", "You never get too cold");
    Toggle(m, Player, "Vitals", "unlimited_lung", "Unlimited lung capacity", "Hold your breath underwater forever");
    Toggle(m, Player, "Vitals", "fully_rested", "Fully rested", "You never get tired");
    Action(m, Player, "Restore", "restore_health", "Restore health", "Health back to full once", "Apply");
    Action(m, Player, "Restore", "restore_hunger", "Restore hunger", "Fullness back to full once", "Apply");
    Action(m, Player, "Restore", "restore_hydration", "Restore hydration", "Hydration back to full once", "Apply");
    Action(m, Player, "Restore", "restore_stamina", "Restore stamina", "Stamina back to full once", "Apply");
    Action(m, Player, "Restore", "restore_temperature", "Restore temperature", "Warms you back up once", "Apply");
    Action(m, Player, "Restore", "restore_lung", "Restore lung capacity", "Breath back to full once", "Apply");
    Action(m, Player, "Restore", "restore_rest", "Rest", "If you are sleepy, your rest goes back to full", "Apply");
    Toggle(m, Player, "Protection", "no_sickness", "No sickness", "Bad food and dirty water do not make you sick");
    Toggle(m, Player, "Protection", "no_fall_damage", "No fall damage", "Falling from any height does not hurt");
    Toggle(m, Player, "Gear", "unbreakable_armor", "Unbreakable armor", "The armor you wear never breaks");
    Toggle(m, Player, "Gear", "unlimited_flashlight", "Unlimited flashlight battery", "Your flashlight never runs out");
    Toggle(m, Player, "Movement", "noclip", "No-clip", "Move through walls, rocks and the ground");
    Toggle(m, Player, "Movement", "flight", "Flight", "Fly freely around the map");
    Slider(m, Player, "Movement", "flight_speed", "Flight speed", "", 0.25f, 10.0f, 1.0f);
    Toggle(m, Player, "Movement", "unlimited_jumps", "Unlimited jumps", "Jump again in mid-air, as often as you want");
    Slider(m, Player, "Movement", "walk_speed", "Walking speed", "", 1.0f, 10.0f, 1.0f);
    Slider(m, Player, "Movement", "run_speed", "Running speed", "", 1.0f, 10.0f, 1.0f);
    Slider(m, Player, "Movement", "jump_height", "Jump height", "", 1.0f, 10.0f, 1.0f);
    Slider(m, Player, "Strength", "strength_levels", "Strength levels to add", "How many levels the button adds", 1.0f, 10.0f, 1.0f,
           "%.0f");
    Action(m, Player, "Strength", "add_strength", "Add strength", "Adds the strength levels set above", "Add");

    Toggle(m, Combat, "Weapons", "unlimited_ammo", "Unlimited ammo", "Ammo never runs out");
    Toggle(m, Combat, "Weapons", "no_reload", "No reload", "Keep shooting without reloading, shots still use ammo");
    Toggle(m, Combat, "Weapons", "no_recoil", "No weapon recoil", "Every bullet goes exactly where you aim");
    Slider(m, Combat, "Weapons", "weapon_damage", "Weapon damage", "", 1.0f, 10.0f, 1.0f);
    Slider(m, Combat, "Weapons", "fire_rate", "Fire rate", "", 1.0f, 10.0f, 1.0f);
    Toggle(m, Combat, "Weapons", "silent_aim", "Silent aim", "Every shot hits the head of a target picked below");
    Toggle(m, Combat, "Silent aim targets", "aim_enemies", "Enemies", "Cannibals and mutants, babies too");
    Toggle(m, Combat, "Silent aim targets", "aim_animals", "Animals", "Deer, rabbits, sharks and other wildlife");
    Toggle(m, Combat, "Silent aim targets", "aim_players", "Other players", "Only in multiplayer");
    Toggle(m, Combat, "Enemies", "one_hit_kill", "One hit enemies", "Enemies die from one hit");
    Slider(m, Combat, "Enemies", "enemy_range", "Range", "For the two buttons below", 5.0f, 200.0f, 50.0f, "%.0f m");
    Action(m, Combat, "Enemies", "kill_in_range", "Kill enemies in range", "Kills every enemy within the range", "Kill");
    Action(m, Combat, "Enemies", "dismember_in_range", "Dismember enemies in range", "Cuts the limbs off enemies within the range",
           "Apply");
    Toggle(m, Combat, "Stealth", "invisibility", "Invisibility", "Enemies cannot see you");
    Toggle(m, Combat, "Stealth", "ghost", "Ghost mode", "Enemies and animals ignore you, even when you attack");

    Toggle(m, Building, "Building", "no_build_requirements", "No building requirements", "Build without logs, stones or other materials");
    Toggle(m, Building, "Building", "build_anywhere", "Build anywhere", "Place structures where the game normally says no");
    Action(m, Building, "Blueprints", "finish_blueprints", "Instant blueprints", "Finishes every blueprint you have placed", "Apply");
    Toggle(m, Building, "Blueprints", "always_fill_blueprints", "Always fill blueprints", "New blueprints finish as soon as you place them");
    Toggle(m, Building, "Crafting", "no_craft_requirements", "No crafting requirements", "Your materials come back after every craft");
    Toggle(m, Building, "Crafting", "no_item_limit", "No item limit", "Carry as many of each item as you want");
    Toggle(m, Building, "3D printer", "unlimited_resin", "Unlimited printer resin", "The 3D printer never runs out of resin");
    Toggle(m, Building, "3D printer", "fast_printing", "Fast printing", "The 3D printer finishes at once");
    Toggle(m, Building, "Building", "log_hack", "Endless held logs", "A log you hold and place is not used up");
    Toggle(m, Building, "Building", "stone_hack", "Endless held stones", "A stone you hold and place is not used up");
    Toggle(m, Building, "Trees", "one_hit_trees", "One hit cut trees", "One swing cuts a tree");
    Toggle(m, Building, "Trees", "instant_tree_fall", "Instant tree falling", "Cut trees fall right away");

    Toggle(m, World, "Time", "stop_time", "Stop time", "The time of day stays where it is");
    Slider(m, World, "Screen", "brightness", "Brightness", "", 0.5f, 3.0f, 1.0f);
    Slider(m, World, "Screen", "fov", "Field of view", "", 60.0f, 120.0f, 75.0f, "%.0f");
    Toggle(m, World, "Game", "enable_console", "Enable console", "Turns on the game's own developer console, press F1 to open it");
    Action(m, World, "Game", "instant_save", "Instant save", "Saves the game wherever you are", "Save");

    Action(m, Companions, "Kelvin", "kelvin_heal", "Heal Kelvin", "Kelvin back to full health", "Apply");
    Action(m, Companions, "Kelvin", "kelvin_revive", "Revive Kelvin", "Brings Kelvin back to life", "Apply");
    Action(m, Companions, "Kelvin", "kelvin_bring", "Bring Kelvin to you", "Teleports Kelvin right next to you", "Apply");
    Toggle(m, Companions, "Kelvin", "kelvin_freeze", "Freeze Kelvin", "Kelvin stops moving and doing tasks");
    Toggle(m, Companions, "Kelvin", "kelvin_godmode", "Kelvin cannot die", "Nothing can kill Kelvin");
    Action(m, Companions, "Virginia", "virginia_heal", "Heal Virginia", "Virginia back to full health", "Apply");
    Action(m, Companions, "Virginia", "virginia_revive", "Revive Virginia", "Brings Virginia back to life", "Apply");
    Action(m, Companions, "Virginia", "virginia_bring", "Bring Virginia to you", "Teleports Virginia right next to you", "Apply");
    Toggle(m, Companions, "Virginia", "virginia_freeze", "Freeze Virginia", "Virginia stops moving and doing tasks");
    Toggle(m, Companions, "Virginia", "virginia_godmode", "Virginia cannot die", "Nothing can kill Virginia");

    Action(m, Spawner, "Items", "add_items", "Add items", "Put items in your inventory", "Open");
    Action(m, Spawner, "Items", "spawner", "Spawn items", "Drop items in front of you", "Open");
    Action(m, Spawner, "Items", "instant_loadout", "Loadout loader", "Guns, ammo, GPS and more in one press", "Open");
    Action(m, Spawner, "Items", "add_all_items", "Add all items", "Every item in the game goes in your inventory", "Apply");
    Action(m, Spawner, "Items", "add_blueprints", "Add blueprints", "Pick the Blueprints chip to see every blueprint you don't have", "Open");
    Action(m, Spawner, "Spawn", "vehicle_spawner", "Spawn vehicle", "Spawn a vehicle in front of you", "Open");
    Action(m, Spawner, "Spawn", "creature_spawner", "Spawn enemies and creatures", "Spawn enemies and animals near you", "Open");
    Action(m, Spawner, "Spawn", "remove_spawned_creatures", "Remove creatures I spawned", "Works until you load a save again", "Remove");
    Action(m, Spawner, "Map", "map", "Map with teleport", "See the whole map and teleport anywhere", "Open");

    for (const char* id : {"add_items", "spawner", "creature_spawner", "instant_loadout", "map"})
        if (Feature* f = m.find(id)) f->page = id;
    if (Feature* f = m.find("vehicle_spawner")) f->page = "spawner";
    if (Feature* f = m.find("add_blueprints")) f->page = "add_items";
    for (const char* id : {"strength_levels", "enemy_range", "aim_enemies", "aim_animals", "aim_players"})
        if (Feature* f = m.find(id)) f->counts = false;
    for (const char* id : {"aim_enemies", "aim_animals"})
        if (Feature* f = m.find(id)) f->on = true;
    m.mapDrawn = true;

    m.gameName = "Sons of the Forest";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
