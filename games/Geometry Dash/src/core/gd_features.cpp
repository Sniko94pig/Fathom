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
    f.building = true;
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

    Slider(m, Player, "Speed", "game_speed", "Game speed", "Speeds up or slows down the whole game, music too", 0.1f, 3.0f, 1.0f, 0.05f, "%.2fx");
    Toggle(m, Player, "Cheats", "noclip", "Noclip", "Spikes, blocks and saws can't kill you");
    Toggle(m, Player, "Cheats", "unlimited_jumps", "Unlimited jumps", "Jump again in mid-air as often as you want");
    Toggle(m, Player, "Cheats", "hitboxes", "Show hitboxes", "Draws the hitboxes of you, blocks, spikes and orbs");
    Toggle(m, Player, "Unlocks", "unlock_icons", "Unlock all icons", "Every icon, color and trail can be picked, only on your screen, other players don't see it");

    Action(m, Level, "Level", "complete_level", "Complete level", "Finishes the level you are playing right away", "Apply", true);
    Toggle(m, Level, "Macro", "macro_record", "Record macro", "Play normally; when you beat the level, your run is saved as a macro for it");
    Toggle(m, Level, "Macro", "macro_play", "Play macro", "The bot replays your saved macro for this level");

    Toggle(m, Troll, "Colors", "rgb", "RGB", "Your icon and the level cycle through rainbow colors");
    Slider(m, Troll, "Colors", "rgb_speed", "RGB speed", "How fast the colors change", 0.1f, 5.0f, 1.0f, 0.1f, "%.1fx", false);
    Toggle(m, Troll, "Jumpscares", "jumpscares", "Random jumpscares", "A scary face and a loud sound pop up at random moments");
    Slider(m, Troll, "Jumpscares", "jumpscare_every", "Jumpscare every", "About how often a jumpscare comes, a bit random each time", 1.0f, 30.0f, 5.0f, 1.0f, "%.0f min", false);
    Action(m, Troll, "Jumpscares", "jumpscare_now", "Jumpscare now", "Shows one jumpscare right away", "Scare");

    m.gameName = "Geometry Dash";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
