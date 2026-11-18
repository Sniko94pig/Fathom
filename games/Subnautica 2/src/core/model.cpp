#include "core/model.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>

namespace core {

namespace {

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

void Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, bool counts = true) {
    Add(m, tab, group, id, label, hint).counts = counts;
}

void Slider(Model& m, Tab tab, const char* group, const char* id, const char* label, float mn, float mx, float def,
            float c0, float c1, float c2, float c3) {
    Feature& f = Add(m, tab, group, id, label, "");
    f.kind = Kind::Slider;
    f.min = mn;
    f.max = mx;
    f.def = def;
    f.value = def;
    f.caps[0] = c0;
    f.caps[1] = c1;
    f.caps[2] = c2;
    f.caps[3] = c3;
}

void Action(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, const char* button,
            bool confirm) {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Action;
    f.actionLabel = button;
    f.confirm = confirm;
}

}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(64);

    Toggle(m, Tab::Survival, "Vitals", "godmode", "Godmode", "Take no damage");
    Toggle(m, Tab::Survival, "Vitals", "infinite_oxygen", "Infinite oxygen", "Oxygen never runs out");
    Toggle(m, Tab::Survival, "Vitals", "unlimited_food", "Unlimited food", "Hunger stays full");
    Toggle(m, Tab::Survival, "Vitals", "unlimited_water", "Unlimited water", "Thirst stays full");
    Toggle(m, Tab::Survival, "Vitals", "no_radiation", "No radiation", "Ignore radiation zones");
    Toggle(m, Tab::Survival, "Vitals", "stable_temperature", "Stable body temperature", "Never too hot or too cold");
    Action(m, Tab::Survival, "Vitals", "refill_health", "Refill health", "Back to full health once", "Refill", false);
    Action(m, Tab::Survival, "Vitals", "refill_food", "Refill hunger", "Food bar back to full", "Refill", false);
    Action(m, Tab::Survival, "Vitals", "refill_water", "Refill thirst", "Water bar back to full", "Refill", false);
    Action(m, Tab::Survival, "Vitals", "refill_oxygen", "Refill oxygen", "Oxygen back to full", "Refill", false);
    Slider(m, Tab::Survival, "Movement", "player_speed", "Player speed", 1.0f, 10.0f, 1.0f, 1.2f, 1.5f, 2.0f, 2.5f);
    Slider(m, Tab::Survival, "Movement", "jump_height", "Jump height", 1.0f, 10.0f, 1.0f, 1.0f, 1.2f, 1.5f, 2.0f);
    Toggle(m, Tab::Survival, "Movement", "noclip", "No-clip", "Swim through walls and rocks like they are not there");
    Toggle(m, Tab::Survival, "Movement", "fly", "Fly", "Fly with WASD, Space and Ctrl, walls still stop you");
    Slider(m, Tab::Survival, "Movement", "fly_speed", "Fly speed", 0.25f, 10.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    Toggle(m, Tab::Survival, "Items", "unlimited_battery", "Unlimited tool battery", "Tools never drain");
    Toggle(m, Tab::Survival, "Items", "unlimited_use", "Unlimited use", "Food, medkits and tools come back after use");

    Toggle(m, Tab::Biomods, "Biomods", "zero_cooldowns", "Zero cooldowns", "Biomods are ready right away");
    Toggle(m, Tab::Biomods, "Biomods", "unlimited_biomod_energy", "Unlimited energy", "Biomod energy never drops");
    Toggle(m, Tab::Biomods, "Scanning", "instant_biosampler", "Instant biosampler scan", "Samples finish at once");
    Toggle(m, Tab::Biomods, "Scanning", "instant_scan", "Instant scan", "Scanner finishes at once");
    Action(m, Tab::Biomods, "Unlocks", "unlock_scans", "Unlock all scans", "Every scan in the game, save is backed up first",
           "Unlock", true);
    Toggle(m, Tab::Biomods, "Unlocks", "unlock_dna", "Unlock all DNA", "All bio abilities and biomod slots, turn off to go back");

    Toggle(m, Tab::Crafting, "Crafting", "instant_craft", "Instant crafting", "Fabricators finish at once");
    Toggle(m, Tab::Crafting, "Crafting", "instant_build", "Instant build", "Base pieces complete at once");
    Toggle(m, Tab::Crafting, "Crafting", "instant_deconstruct", "Instant deconstruct", "Base pieces come apart at once");
    Toggle(m, Tab::Crafting, "Crafting", "free_craft", "Free crafting and building", "No ingredients needed");
    Toggle(m, Tab::Crafting, "Base", "unlimited_power", "Unlimited base power", "Bases never run out of power");
    Toggle(m, Tab::Crafting, "Unlocks", "unlock_blueprints", "Unlock all blueprints", "Turn off before saving to go back to the ones you had");
    Toggle(m, Tab::Crafting, "Unlocks", "unlock_databank", "Unlock all databank entries", "Turn off before saving to go back to what you had");

    Slider(m, Tab::World, "World", "game_speed", "Game speed", 0.1f, 5.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    Toggle(m, Tab::World, "World", "hide_water_fog", "Hide water fog", "See far underwater, the world edge stays hidden");
    Toggle(m, Tab::World, "World", "free_camera", "Free camera", "Mouse to look, WASD, Space and Ctrl to fly");
    Slider(m, Tab::World, "World", "free_camera_speed", "Free camera speed", 0.25f, 10.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f);
    Toggle(m, Tab::World, "World", "performance_mode", "Performance mode", "All graphics on lowest for the most FPS");
    Toggle(m, Tab::World, "Vehicles", "vehicle_hull", "Infinite vehicle hull", "Vehicles take no damage");
    Slider(m, Tab::World, "Vehicles", "vehicle_speed", "Vehicle speed", 1.0f, 20.0f, 1.0f, 1.0f, 1.3f, 1.6f, 2.0f);
    Toggle(m, Tab::World, "Creature ESP", "esp", "Creature ESP", "Show creatures through walls");
    Toggle(m, Tab::World, "Creature ESP", "esp_hostile", "Hostile", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_passive", "Passive", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_leviathan", "Leviathan", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_name", "Show name", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_distance", "Show distance", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_health", "Show health", "", false);
    Toggle(m, Tab::World, "Creature ESP", "esp_box", "Show box", "", false);

    Toggle(m, Tab::World, "Item ESP", "item_esp", "Item ESP", "Show items, data boxes and crates nearby");
    Toggle(m, Tab::World, "Item ESP", "itemesp_items", "Items", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_data", "Data boxes, logs and black boxes", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_crates", "Supply crates", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_name", "Show name", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_distance", "Show distance", "", false);
    Toggle(m, Tab::World, "Item ESP", "itemesp_box", "Show box", "", false);

    for (const char* id : {"esp_hostile", "esp_passive", "esp_leviathan", "esp_name", "esp_distance", "esp_health", "esp_box",
                           "itemesp_items", "itemesp_data", "itemesp_crates", "itemesp_name", "itemesp_distance", "itemesp_box"})
        m.find(id)->on = true;


    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

bool Implemented(const std::string&) { return true; }

const char* NotYetReason(const std::string&) { return "Not available yet"; }

Feature* Model::find(const std::string& id) {
    for (Feature& f : features)
        if (f.id == id) return &f;
    return nullptr;
}

int Model::activeCount() const {
    int n = 0;
    for (const Feature& f : features) {
        if (!f.counts || !f.available || f.building) continue;
        if (f.kind == Kind::Toggle && f.on) ++n;
        if (f.kind == Kind::Slider && std::fabs(f.value - f.def) > 0.001f) ++n;
    }
    return n;
}

float Model::sliderMax(const Feature& f) const {
    if (!legit) return f.max;
    int t = std::clamp(legitTier, 0, 3);
    float cap = f.caps[t];
    if (cap <= 0) return f.max;
    return std::clamp(cap, f.min, f.max);
}

const Marker* Model::player() const {
    for (const Marker& k : markers)
        if (k.type == MarkerType::Player) return &k;
    return nullptr;
}

void SetFeature(Model& m, Feature& f, bool on) {
    if (f.kind != Kind::Toggle || f.on == on) return;
    if (on && (!f.available || f.building || !m.cheatsAllowed())) return;
    f.on = on;
    if (f.apply) f.apply(f);
}

void SetValue(Model& m, Feature& f, float v) {
    if (f.kind != Kind::Slider) return;
    float mx = m.sliderMax(f);
    v = std::clamp(v, std::min(f.min, f.def), mx);
    if (std::fabs(v - f.value) < 0.0001f) return;
    if (!f.available || f.building || !m.cheatsAllowed()) v = f.def;
    f.value = v;
    if (f.apply) f.apply(f);
}

void DisableAll(Model& m) {
    for (Feature& f : m.features) {
        if (!f.counts) continue;
        if (f.kind == Kind::Toggle && f.on) {
            f.on = false;
            if (f.apply) f.apply(f);
        }
        if (f.kind == Kind::Slider && f.value != f.def) {
            f.value = f.def;
            if (f.apply) f.apply(f);
        }
    }
}

}
