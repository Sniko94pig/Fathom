#include "core/gtav_features.h"
#include "imgui.h"
#include "ui/icons.h"

namespace gtav {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

const Tab Player = Tab::Survival;
const Tab Weapons = Tab::Crafting;
const Tab Vehicles = Tab::BaseHub;
const Tab World = Tab::World;
const Tab Troll = Tab::Legit;

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

Feature& Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    return Add(m, tab, group, id, label, hint);
}

Feature& Slider(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, float mn, float mx,
                float def, float step, const char* fmt, bool counts = true) {
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

}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(48);
    m.tabs = {Player, Weapons, Vehicles, World, Troll, Tab::Settings};
    m.plainTabs = {Vehicles, Troll};
    m.tabLabels = {{Player, "Player", ICON_HEART_PULSE},
                   {Weapons, "Weapons", ICON_CROSSHAIR},
                   {Vehicles, "Vehicles", ICON_GAUGE},
                   {World, "World", ICON_GLOBE},
                   {Troll, "Troll", ICON_SKULL}};
    m.categories = {"Pistols", "SMGs", "Shotguns", "Assault rifles", "Machine guns", "Sniper rifles", "Heavy weapons", "Throwables",
                    "Melee", "Ammo", "Armor and items"};

    Toggle(m, Player, "Health", "god_mode", "God mode", "Nothing can hurt or kill you: bullets, explosions, falling, fire or drowning");
    Toggle(m, Player, "Health", "unlimited_health", "Unlimited health", "You still get hit, but your health fills back up right away");
    Toggle(m, Player, "Health", "unlimited_stamina", "Unlimited stamina", "Sprint, swim and cycle forever");
    Action(m, Player, "Health", "heal", "Heal", "Full health and full armor right away", "Apply");
    Slider(m, Player, "Police", "wanted_level", "Stars", "Pick how many wanted stars you want, then press Apply. 0 clears them", 0.0f, 5.0f, 1.0f,
           1.0f, "%.0f", false);
    Action(m, Player, "Police", "give_stars", "Give stars", "Sets your wanted level to the Stars slider right away", "Apply");
    Toggle(m, Player, "Police", "police_blind", "Invisible to police", "The police can't see or chase you, even with stars");
    Slider(m, Player, "Money", "money_amount", "Money amount", "How much Story Mode money Give money adds", 10000.0f, 10000000.0f, 1000000.0f,
           10000.0f, "$%.0f", false);
    Action(m, Player, "Money", "give_money", "Give money", "Adds the Money amount to the character you are playing", "Apply");
    Toggle(m, Player, "Money", "unlimited_money", "Unlimited money", "Your Story Mode money never goes down when you buy things");
    Toggle(m, Player, "Movement", "fly", "Fly", "Fly like a superhero with WASD, Space and Ctrl, but still bump into walls");
    Slider(m, Player, "Movement", "fly_speed", "Fly speed", "How fast Fly moves you", 1.0f, 10.0f, 3.0f, 0.5f, "%.1fx", false);
    Toggle(m, Player, "Movement", "noclip", "Noclip", "Fly through walls and the ground with WASD, Space and Ctrl");
    Slider(m, Player, "Movement", "noclip_speed", "Noclip speed", "How fast Noclip moves you", 1.0f, 10.0f, 3.0f, 0.5f, "%.1fx", false);

    Page(m, Weapons, "Items", "item_giver", "Item giver", "Pick guns, ammo, armor, parachutes, snacks and more from a list", "add_items");
    Action(m, Weapons, "Ammo", "restore_ammo", "Restore ammo", "Refills the ammo of every weapon you have", "Apply");
    Toggle(m, Weapons, "Ammo", "unlimited_ammo", "Unlimited ammo", "Your ammo never runs out");
    Toggle(m, Weapons, "Ammo", "no_reload", "No reload", "You never have to reload, but shots still use ammo");
    Slider(m, Weapons, "Handling", "fire_rate", "Fire rate", "How fast your guns shoot", 1.0f, 10.0f, 1.0f, 0.5f, "%.1fx");
    Toggle(m, Weapons, "Handling", "no_recoil", "No recoil", "Your aim doesn't kick up when you shoot");
    Toggle(m, Weapons, "Handling", "no_spread", "No spread", "Every bullet goes exactly where you aim");
    Toggle(m, Weapons, "Aim", "silent_aim", "Silent aim", "Every shot hits the head of the person closest to your crosshair, even when your aim is off");

    Page(m, Vehicles, "Spawn", "spawn_cars", "Spawn cars", "Pick a vehicle from a list and it appears in front of you, with you in the driver's seat", "spawner");
    Toggle(m, Vehicles, "Vehicle", "vehicle_god_mode", "God mode vehicle", "The vehicle you are in can't be damaged");
    Action(m, Vehicles, "Vehicle", "car_repair", "Instant car repair", "Fixes and cleans the vehicle you are in right away", "Apply");
    Action(m, Vehicles, "Vehicle", "steal_car", "Steal nearest car", "Throws the driver of the nearest car out and puts you in the driver's seat", "Apply");

    Page(m, World, "Map", "map", "Map", "Click any spot on the map to teleport there, with your vehicle if you are in one", "map");
    Slider(m, World, "Time", "game_speed", "Game speed", "Slows down or speeds up the whole game", 0.1f, 2.0f, 1.0f, 0.05f, "%.2fx");
    Toggle(m, World, "Time", "stop_time", "Stop time", "The clock stops, so it stays the same time of day");
    Toggle(m, World, "Camera", "freecam", "Freecam", "The camera flies free with WASD, Space and Ctrl while your character stays put");
    Slider(m, World, "Camera", "freecam_speed", "Freecam speed", "How fast the free camera moves", 1.0f, 10.0f, 3.0f, 0.5f, "%.1fx", false);
    Slider(m, World, "Camera", "fov", "FOV", "How wide the camera sees", 30.0f, 120.0f, 50.0f, 1.0f, "%.0f");

    Toggle(m, Troll, "Cars", "fling_cars", "Fling cars aura", "Every car near you is flung into the air");
    Toggle(m, Troll, "Cars", "invisible_car", "Invisible car", "The vehicle you are in turns invisible");
    Toggle(m, Troll, "People", "angry_mob", "Angry pedestrian mob", "People near you grab weapons and fight each other and you. Turning it off calms them down");
    Toggle(m, Troll, "Touch", "ragdoll_touch", "Ragdoll touch", "Any person or car that touches you or your vehicle goes full ragdoll");
    Toggle(m, Troll, "Touch", "freeze_touch", "Freeze character touch", "Anyone you touch freezes in place. Turning it off unfreezes everyone");

    m.gameName = "Grand Theft Auto V";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
