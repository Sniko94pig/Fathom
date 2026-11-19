#include "core/sn1_features.h"
#include "imgui.h"

namespace sn1 {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

Feature& Add(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    Feature f;
    f.tab = tab;
    f.group = group;
    f.id = id;
    f.label = label;
    f.hint = hint;
    f.building = true;
    m.features.push_back(std::move(f));
    return m.features.back();
}

void Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, bool counts = true) {
    Add(m, tab, group, id, label, hint).counts = counts;
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
    m.features.reserve(64);
    m.tabs = {Tab::Survival, Tab::Crafting, Tab::World, Tab::Spawner, Tab::Settings};
    m.plainTabs = {Tab::Spawner};

    Toggle(m, Tab::Survival, "Vitals", "godmode", "Godmode", "Nothing can kill you");
    Toggle(m, Tab::Survival, "Vitals", "unlimited_health", "Unlimited health", "Health refills as soon as it drops");
    Toggle(m, Tab::Survival, "Vitals", "radiation_immunity", "Radiation immunity", "Radiation does not hurt you");
    Toggle(m, Tab::Survival, "Vitals", "unlimited_food", "Unlimited food", "Hunger stays full");
    Toggle(m, Tab::Survival, "Vitals", "unlimited_water", "Unlimited water", "Thirst stays full");
    Toggle(m, Tab::Survival, "Vitals", "infinite_oxygen", "Infinite oxygen", "Oxygen never goes down");
    Action(m, Tab::Survival, "Vitals", "refill_health", "Refill health", "Back to full health once", "Refill");
    Action(m, Tab::Survival, "Vitals", "refill_food", "Refill hunger", "Food bar back to full", "Refill");
    Action(m, Tab::Survival, "Vitals", "refill_water", "Refill thirst", "Water bar back to full", "Refill");
    Action(m, Tab::Survival, "Vitals", "refill_oxygen", "Refill oxygen", "Oxygen back to full", "Refill");
    Toggle(m, Tab::Survival, "Stealth", "ghost", "Ghost mode", "Creatures ignore you");
    Slider(m, Tab::Survival, "Movement", "player_speed", "Player speed", "", 1.0f, 10.0f, 1.0f);
    Slider(m, Tab::Survival, "Movement", "jump_height", "Jump height", "", 1.0f, 10.0f, 1.0f);
    Toggle(m, Tab::Survival, "Movement", "noclip", "No-clip", "Swim through walls and rocks underwater");
    Toggle(m, Tab::Survival, "Movement", "fly", "Fly", "WASD, Space and Ctrl, walls still stop you");
    Slider(m, Tab::Survival, "Movement", "fly_speed", "Fly speed", "", 0.25f, 10.0f, 1.0f);

    Toggle(m, Tab::Crafting, "Crafting", "instant_craft", "Instant crafting", "Fabricators finish at once");
    Toggle(m, Tab::Crafting, "Crafting", "instant_build", "Instant build", "Base pieces complete at once");
    Toggle(m, Tab::Crafting, "Crafting", "instant_deconstruct", "Instant deconstruct", "Habitat pieces come apart at once");
    Toggle(m, Tab::Crafting, "Crafting", "free_craft", "Free crafting and building", "No ingredients needed");
    Toggle(m, Tab::Crafting, "Base", "base_integrity", "Infinite base integrity", "Habitats never flood or break");
    Toggle(m, Tab::Crafting, "Base", "unlimited_power", "Infinite power", "Bases and habitats never run out of power");
    Toggle(m, Tab::Crafting, "Unlocks", "unlock_blueprints", "Unlock all blueprints", "Turn off to go back to the blueprints you had");
    Toggle(m, Tab::Crafting, "Unlocks", "instant_scan", "Instant scan", "Scanner finishes at once");

    Slider(m, Tab::World, "World", "game_speed", "Game speed", "", 0.1f, 5.0f, 1.0f);
    Toggle(m, Tab::World, "World", "remove_fog", "Remove fog", "Clear water, deep areas lit up");
    Toggle(m, Tab::World, "Vehicles", "vehicle_hull", "Infinite vehicle hull", "Your vehicle takes no damage");
    Toggle(m, Tab::World, "Vehicles", "vehicle_energy", "Infinite vehicle energy", "Your vehicle never runs out of power");
    Toggle(m, Tab::World, "Vehicles", "vehicle_invisible", "Vehicle invisibility", "Predators cannot see your vehicle");
    Slider(m, Tab::World, "Vehicles", "vehicle_speed", "Vehicle speed", "", 1.0f, 10.0f, 1.0f);
    Slider(m, Tab::World, "Creatures", "clear_radius", "Clear hostiles radius", "", 10.0f, 200.0f, 50.0f, "%.0f m");
    Action(m, Tab::World, "Creatures", "clear_hostiles", "Clear hostiles", "Kill hostile creatures nearby, not leviathans", "Clear");
    m.find("clear_hostiles")->confirm = true;
    Slider(m, Tab::World, "ESP", "esp_range", "ESP range", "For creature and item ESP", 10.0f, 500.0f, 150.0f, "%.0f m");
    Toggle(m, Tab::World, "Creature ESP", "esp", "Creature ESP", "Show creatures through walls");
    Toggle(m, Tab::World, "Creature ESP", "esp_hostile", "Hostile", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_passive", "Passive", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_leviathan", "Leviathan", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_name", "Show name", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_distance", "Show distance", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_health", "Show health", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_box", "Show box", "", false);
    Toggle(m, Tab::World, "Item ESP", "item_esp", "Item ESP", "Show resources, fragments, data boxes and crates");
    Toggle(m, Tab::World, "Item ESP", "itemesp_resources", "Resources", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_fragments", "Fragments", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_data", "Data boxes, PDAs and logs", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_crates", "Supply crates", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_name", "Show name", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_distance", "Show distance", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_box", "Show box", "", false);

    Action(m, Tab::Spawner, "Items", "add_items", "Add items", "Put items in your inventory, with icons, names and categories", "Open");
    Action(m, Tab::Spawner, "Items", "spawner", "Spawner", "Spawn items, vehicles and more in front of you", "Open");
    Action(m, Tab::Spawner, "Items", "creature_spawner", "Creature spawner", "Spawn creatures and other entities", "Open");
    Action(m, Tab::Spawner, "Items", "instant_loadout", "Instant loadout", "Pick a set of items once, get them all in one press", "Open");
    Action(m, Tab::Spawner, "Map", "map", "Map with teleport", "See the whole map and teleport anywhere", "Open");

    for (Feature& f : m.features)
        if (f.tab != Tab::Spawner) f.building = false;

    for (const char* id : {"esp_hostile", "esp_passive", "esp_leviathan", "esp_name", "esp_distance", "esp_health", "esp_box",
                           "itemesp_resources", "itemesp_fragments", "itemesp_data", "itemesp_crates", "itemesp_name",
                           "itemesp_distance", "itemesp_box"})
        m.find(id)->on = true;

    m.gameName = "Subnautica";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
