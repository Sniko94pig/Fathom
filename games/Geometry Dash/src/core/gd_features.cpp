#include "core/gd_features.h"
#include "imgui.h"
#include "ui/icons.h"

namespace gd {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

const Tab Player = Tab::Survival;
const Tab Level = Tab::Crafting;
const Tab Troll = Tab::World;

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
}

void Action(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint, const char* button,
            bool confirm = false) {
    Feature& f = Add(m, tab, group, id, label, hint);
    f.kind = Kind::Action;
    f.actionLabel = button;
    f.confirm = confirm;
}

}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(16);
    m.tabs = {Player, Level, Troll, Tab::Settings};
    m.tabLabels = {{Player, "Player", ICON_GAUGE},
                   {Level, "Level", ICON_PLAY},
                   {Troll, "Troll", ICON_SKULL}};

    Slider(m, Player, "Speed", "game_speed", "Game speed", "Whole game and music. Not 1x: runs don't count", 0.1f, 3.0f, 1.0f, 0.05f, "%.2fx");
    Toggle(m, Player, "Cheats", "noclip", "Noclip", "Nothing can kill you. Runs don't count");
    Toggle(m, Player, "Cheats", "unlimited_jumps", "Unlimited jumps", "Jump in mid-air again and again. Runs don't count");
    Toggle(m, Player, "Cheats", "hitboxes", "Show hitboxes", "Draws the hitboxes of you and every object");
    Toggle(m, Player, "Unlocks", "unlock_icons", "Unlock all icons", "Pick any icon, only you see it");

    Action(m, Level, "Level", "complete_level", "Complete level", "Finishes the level now. It won't count", "Apply");
    Toggle(m, Level, "Macro", "macro_record", "Record macro", "Your winning run is saved as a macro");
    Toggle(m, Level, "Macro", "macro_play", "Play macro", "The bot plays your macro. Runs don't count");

    Toggle(m, Troll, "Colors", "rgb", "RGB", "Rainbow colors on your icon and the level");
    Slider(m, Troll, "Colors", "rgb_speed", "RGB speed", "How fast the colors change", 0.1f, 5.0f, 1.0f, 0.1f, "%.1fx", false);
    Toggle(m, Troll, "Jumpscares", "jumpscares", "Random jumpscares", "A scary face and loud sound at random times");
    Slider(m, Troll, "Jumpscares", "jumpscare_every", "Jumpscare every", "About how often, a bit random each time", 1.0f, 30.0f, 5.0f, 1.0f, "%.0f min", false);
    Action(m, Troll, "Jumpscares", "jumpscare_now", "Jumpscare now", "Shows one jumpscare right away", "Scare");

    m.gameName = "Geometry Dash";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
