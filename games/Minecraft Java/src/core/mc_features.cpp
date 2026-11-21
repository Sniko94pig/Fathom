#include "core/mc_features.h"
#include "imgui.h"
#include "ui/icons.h"
#include <cctype>
#include <initializer_list>

namespace mc {

using core::Feature;
using core::Kind;
using core::Model;
using core::Tab;

namespace {

const Tab Combat = Tab::Biomods;
const Tab Movement = Tab::Survival;
const Tab Render = Tab::World;
const Tab Blocks = Tab::Crafting;
const Tab Items = Tab::Spawner;
const Tab Other = Tab::Legit;
const Tab Baritone = Tab::BaseHub;

Feature& Add(Model& m, Tab tab, const char* group, const std::string& id, const char* label, const char* hint) {
    Feature f;
    f.tab = tab;
    f.group = group;
    f.id = id;
    f.label = label;
    f.hint = hint;
    f.missing = "Start Minecraft from Fathom to use this";
    m.features.push_back(std::move(f));
    return m.features.back();
}

void Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    Add(m, tab, group, id, label, hint);
}

void Action(Model& m, const char* group, const char* id, const char* label, const char* hint, const char* button) {
    Feature& f = Add(m, Baritone, group, id, label, hint);
    f.kind = Kind::Action;
    f.actionLabel = button;
    f.counts = false;
}

void Input(Model& m, const char* group, const char* id, const char* label, const char* hint, const char* button,
           std::initializer_list<const char*> boxes, std::initializer_list<float> sizes = {}) {
    Feature& f = Add(m, Baritone, group, id, label, hint);
    f.kind = Kind::Input;
    f.actionLabel = button;
    f.counts = false;
    for (const char* b : boxes) {
        f.inputHints.push_back(b);
        f.inputs.emplace_back();
    }
    f.inputSizes.assign(sizes.begin(), sizes.end());
}

void Setting(Model& m, const char* group, const char* name, const char* label, const char* hint, bool def) {
    Feature& f = Add(m, Baritone, group, std::string("bs_") + name, label, hint);
    f.counts = false;
    f.on = def;
}

void Number(Model& m, const char* group, const char* name, const char* label, float mn, float mx, float def, float step,
            const char* fmt) {
    Feature& f = Add(m, Baritone, group, std::string("bs_") + name, label, "");
    f.kind = Kind::Slider;
    f.counts = false;
    f.min = mn;
    f.max = mx;
    f.def = def;
    f.value = def;
    f.step = step;
    f.fmt = fmt;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool Coord(const std::string& s) {
    if (s.empty()) return false;
    size_t i = 0;
    if (s[0] == '~') {
        if (s.size() == 1) return true;
        i = 1;
    }
    if (s[i] == '-' || s[i] == '+') ++i;
    if (i >= s.size()) return false;
    for (; i < s.size(); ++i)
        if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

bool Count(const std::string& s) {
    if (s.empty() || s.size() > 7) return false;
    for (char c : s)
        if (!std::isdigit((unsigned char)c)) return false;
    return true;
}

std::string Names(const std::string& s) {
    std::string out;
    bool space = false;
    for (char c : s) {
        if (c == ',' || c == ' ' || c == '\t' || c == ';') {
            space = !out.empty();
            continue;
        }
        if (!std::isalnum((unsigned char)c) && c != '_' && c != ':' && c != '.' && c != '-' && c != '/') return {};
        if (space) out.push_back(' ');
        space = false;
        out.push_back(c);
    }
    return out;
}

std::string Place(const std::string& x, const std::string& y, const std::string& z, std::string& error) {
    if (!x.empty() && !z.empty()) {
        if (!Coord(x) || !Coord(z) || (!y.empty() && !Coord(y))) {
            error = "Use whole numbers, like 120 64 -300";
            return {};
        }
        return y.empty() ? x + " " + z : x + " " + y + " " + z;
    }
    if (x.empty() && z.empty() && !y.empty()) {
        if (!Coord(y)) {
            error = "Use a whole number for the height";
            return {};
        }
        return y;
    }
    error = "Type X and Z, or only Y";
    return {};
}

}

bool IsHack(const Feature& f) { return f.id.rfind("b_", 0) != 0 && f.id.rfind("bs_", 0) != 0; }

const char* BaritoneSetting(const Feature& f) { return f.id.rfind("bs_", 0) == 0 ? f.id.c_str() + 3 : nullptr; }

std::string BaritoneCommand(const Feature& f, std::string& error) {
    auto in = [&](size_t i) { return i < f.inputs.size() ? Trim(f.inputs[i]) : std::string(); };
    auto names = [&](size_t i, const char* what) {
        std::string n = Names(in(i));
        if (n.empty()) error = in(i).empty() ? std::string("Type ") + what + " first" : std::string("That does not look like ") + what;
        return n;
    };
    const std::string& id = f.id;
    if (id == "b_stop") return "stop";
    if (id == "b_pause") return "pause";
    if (id == "b_resume") return "resume";
    if (id == "b_forcecancel") return "forcecancel";
    if (id == "b_goto") {
        std::string p = Place(in(0), in(1), in(2), error);
        return p.empty() ? std::string() : "goto " + p;
    }
    if (id == "b_gotoblock") {
        std::string n = names(0, "a block");
        return n.empty() ? std::string() : "goto " + n;
    }
    if (id == "b_come") return "come";
    if (id == "b_surface") return "surface";
    if (id == "b_thisway") {
        if (!Count(in(0))) {
            error = "Type how many blocks, like 1000";
            return {};
        }
        return "thisway " + in(0);
    }
    if (id == "b_explore") {
        if (in(0).empty() && in(1).empty()) return "explore";
        if (!Coord(in(0)) || !Coord(in(1))) {
            error = "Type X and Z, or leave both empty";
            return {};
        }
        return "explore " + in(0) + " " + in(1);
    }
    if (id == "b_elytra") return "elytra";
    if (id == "b_goal") {
        std::string p = Place(in(0), in(1), in(2), error);
        return p.empty() ? std::string() : "goal " + p;
    }
    if (id == "b_path") return "path";
    if (id == "b_invert") return "invert";
    if (id == "b_axis") return "axis";
    if (id == "b_mine") {
        std::string n = names(0, "a block");
        if (n.empty()) return {};
        if (in(1).empty()) return "mine " + n;
        if (!Count(in(1))) {
            error = "The amount must be a whole number";
            return {};
        }
        return "mine " + in(1) + " " + n;
    }
    if (id == "b_tunnel") {
        if (in(0).empty() && in(1).empty() && in(2).empty()) return "tunnel";
        if (!Count(in(0)) || !Count(in(1)) || !Count(in(2))) {
            error = "Type height, width and depth, or leave all three empty";
            return {};
        }
        return "tunnel " + in(0) + " " + in(1) + " " + in(2);
    }
    if (id == "b_find") {
        std::string n = names(0, "a block");
        return n.empty() ? std::string() : "find " + n;
    }
    if (id == "b_farm") {
        if (in(0).empty()) return "farm";
        if (!Count(in(0))) {
            error = "The range must be a whole number";
            return {};
        }
        return "farm " + in(0);
    }
    if (id == "b_follow") {
        if (in(0).empty()) return "follow players";
        std::string n = names(0, "a player name");
        return n.empty() ? std::string() : "follow player " + n;
    }
    if (id == "b_followmob") {
        if (in(0).empty()) return "follow entities";
        std::string n = names(0, "a mob");
        return n.empty() ? std::string() : "follow entity " + n;
    }
    if (id == "b_pickup") return "pickup";
    if (id == "b_build") {
        std::string file = Trim(in(0));
        if (file.empty() || file.find_first_of("\"<>|*?") != std::string::npos) {
            error = file.empty() ? "Type the schematic file name first" : "That file name has odd characters";
            return {};
        }
        if (in(1).empty() && in(2).empty() && in(3).empty()) return "build " + file;
        if (!Coord(in(1)) || !Coord(in(2)) || !Coord(in(3))) {
            error = "Type X, Y and Z, or leave all three empty";
            return {};
        }
        return "build " + file + " " + in(1) + " " + in(2) + " " + in(3);
    }
    if (id == "b_litematica") return "litematica";
    if (id == "b_schematica") return "schematica";
    if (id == "b_sel1") return "sel 1";
    if (id == "b_sel2") return "sel 2";
    if (id == "b_selclear") return "sel clear";
    if (id == "b_selcleararea") return "sel cleararea";
    if (id == "b_selfill") {
        std::string n = names(0, "a block");
        return n.empty() ? std::string() : "sel fill " + n;
    }
    if (id == "b_selwalls") {
        std::string n = names(0, "a block");
        return n.empty() ? std::string() : "sel walls " + n;
    }
    if (id == "b_selshell") {
        std::string n = names(0, "a block");
        return n.empty() ? std::string() : "sel shell " + n;
    }
    if (id == "b_selreplace") {
        std::string from = names(0, "the block to replace");
        if (from.empty()) return {};
        std::string to = names(1, "the new block");
        return to.empty() ? std::string() : "sel replace " + from + " " + to;
    }
    if (id == "b_wpsave") {
        std::string n = names(0, "a name");
        return n.empty() ? std::string() : "wp save user " + n;
    }
    if (id == "b_wpgoto") {
        std::string n = names(0, "a name");
        return n.empty() ? std::string() : "wp goto " + n;
    }
    if (id == "b_wpdel") {
        std::string n = names(0, "a name");
        return n.empty() ? std::string() : "wp delete " + n;
    }
    if (id == "b_wplist") return "wp list";
    if (id == "b_sethome") return "sethome";
    if (id == "b_home") return "home";
    if (id == "b_click") return "click";
    if (id == "b_eta") return "eta";
    if (id == "b_proc") return "proc";
    if (id == "b_gc") return "gc";
    if (id == "b_repack") return "repack";
    if (id == "b_render") return "render";
    if (id == "b_blacklist") return "blacklist";
    if (id == "b_custom") {
        std::string c = in(0);
        while (!c.empty() && (c[0] == '#' || c[0] == '.' || c[0] == '/')) c.erase(0, 1);
        c = Trim(c);
        if (c.empty()) error = "Type a command first";
        return c;
    }
    error = "Unknown command";
    return {};
}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(256);
    m.tabs = {Combat, Movement, Render, Blocks, Items, Other, Baritone, Tab::Settings};
    m.plainTabs = {Combat, Items, Other, Baritone};
    m.tabLabels = {{Combat, "Combat", ICON_CROSSHAIR},
                   {Movement, "Movement", ICON_NAVIGATION},
                   {Render, "Render", ICON_EYE},
                   {Blocks, "Blocks", ICON_HAMMER},
                   {Items, "Items", ICON_PACKAGE},
                   {Other, "Other", ICON_SLIDERS_HORIZONTAL},
                   {Baritone, "Baritone", ICON_MAP}};

    Toggle(m, Combat, "Auras", "killaura", "Kill aura", "Automatically attacks entities around you");
    Toggle(m, Combat, "Auras", "killauralegit", "Kill aura legit", "Slower Killaura that is harder to detect");
    Toggle(m, Combat, "Auras", "multiaura", "Multi aura", "Faster Killaura that attacks multiple entities at once");
    Toggle(m, Combat, "Auras", "clickaura", "Click aura", "Automatically attacks the closest valid entity whenever you click");
    Toggle(m, Combat, "Auras", "tpaura", "TP aura", "Teleports around enemies and hits them");
    Toggle(m, Combat, "Auras", "triggerbot", "Trigger bot", "Automatically attacks the entity you're looking at");
    Toggle(m, Combat, "Auras", "crystalaura", "Crystal aura", "Automatically places (optional) and detonates end crystals to kill entities around you");
    Toggle(m, Combat, "Auras", "anchoraura", "Anchor aura", "Automatically places (optional), charges, and detonates respawn anchors to kill entities around you");
    Toggle(m, Combat, "Aim", "aimassist", "Aim assist", "Helps you aim at nearby entities");
    Toggle(m, Combat, "Aim", "bowaimbot", "Bow aimbot", "Automatically aims your bow or crossbow");
    Toggle(m, Combat, "Damage", "criticals", "Criticals", "Changes all your hits to critical hits");
    Toggle(m, Combat, "Damage", "arrowdmg", "Arrow damage", "Massively increases arrow damage, but also consumes a lot of hunger and reduces accuracy");
    Toggle(m, Combat, "Damage", "macedmg", "Mace damage", "Increases your damage when attacking with a mace");
    Toggle(m, Combat, "Damage", "attributeswap", "Attribute swap", "Swaps main-hand item attributes with the target slot");
    Toggle(m, Combat, "Damage", "autosword", "Auto sword", "Automatically uses the best weapon in your hotbar to attack entities");
    Toggle(m, Combat, "Bots", "fightbot", "Fight bot", "A bot that automatically walks around and kills everything");
    Toggle(m, Combat, "Bots", "protect", "Protect", "A bot that follows the closest entity and protects it from other entities");
    Toggle(m, Combat, "Stay alive", "antiknockback", "Anti knockback", "Prevents you from taking knockback from players and mobs");
    Toggle(m, Combat, "Stay alive", "autoarmor", "Auto armor", "Manages your armor automatically");
    Toggle(m, Combat, "Stay alive", "autototem", "Auto totem", "Automatically moves totems of undying to your off-hand");
    Toggle(m, Combat, "Stay alive", "autopotion", "Auto potion", "Automatically throws splash potions of instant health when your health is low");
    Toggle(m, Combat, "Stay alive", "autosoup", "Auto soup", "Automatically eats soup when your health is low");
    Toggle(m, Combat, "Stay alive", "autorespawn", "Auto respawn", "Automatically respawns you whenever you die");
    Toggle(m, Combat, "Stay alive", "autoleave", "Auto leave", "Automatically leaves the server when your health is low");

    Toggle(m, Movement, "Flying", "flight", "Flight", "Fly freely, turn on No fall to avoid fall damage");
    Toggle(m, Movement, "Flying", "creativeflight", "Creative flight", "Fly like in Creative Mode");
    Toggle(m, Movement, "Flying", "jetpack", "Jetpack", "Fly as if you had a jetpack");
    Toggle(m, Movement, "Flying", "glide", "Glide", "Makes you glide down slowly when falling");
    Toggle(m, Movement, "Flying", "boatfly", "Boat fly", "Fly with boats and other vehicles");
    Toggle(m, Movement, "Flying", "extraelytra", "Extra elytra", "Makes the Elytra easier to use");
    Toggle(m, Movement, "Flying", "nolevitation", "No levitation", "Disables the levitation effect when you get hit by a Shulker");
    Toggle(m, Movement, "Speed", "speedhack", "Speed", "Run ~2.5x faster than you would by sprinting and jumping");
    Toggle(m, Movement, "Speed", "autosprint", "Auto sprint", "Makes you sprint automatically");
    Toggle(m, Movement, "Speed", "bunnyhop", "Bunny hop", "Makes you jump automatically");
    Toggle(m, Movement, "Speed", "highjump", "High jump", "Jump higher");
    Toggle(m, Movement, "Speed", "step", "Step", "Step up full blocks");
    Toggle(m, Movement, "Speed", "spider", "Spider", "Climb up walls like a spider");
    Toggle(m, Movement, "Speed", "fastladder", "Fast ladder", "Climb up ladders faster");
    Toggle(m, Movement, "Speed", "parkour", "Parkour", "Makes you jump automatically when reaching the edge of a block");
    Toggle(m, Movement, "Speed", "noclip", "No-clip", "Teleport through up to 21 blocks (or 37 with elytra equipped)");
    Toggle(m, Movement, "Speed", "blink", "Blink", "Suspends all motion updates while enabled");
    Toggle(m, Movement, "Water", "jesus", "Walk on water", "Walk on top of water like it is solid");
    Toggle(m, Movement, "Water", "dolphin", "Dolphin", "Makes you bob up in water automatically");
    Toggle(m, Movement, "Water", "fish", "Swim like a fish", "Disables underwater gravity so you can swim like a fish");
    Toggle(m, Movement, "Water", "autoswim", "Auto swim", "Triggers the swimming animation automatically");
    Toggle(m, Movement, "Water", "antiwaterpush", "Anti water push", "Prevents you from getting pushed by water");
    Toggle(m, Movement, "Protection", "nofall", "No fall", "Protects you from fall damage");
    Toggle(m, Movement, "Protection", "noslowdown", "No slowdown", "Cancels slowness effects caused by honey, soul sand and using items");
    Toggle(m, Movement, "Protection", "noweb", "No web", "Prevents you from getting slowed down by cobwebs");
    Toggle(m, Movement, "Protection", "snowshoe", "Snow shoe", "Walk on powder snow without freezing");
    Toggle(m, Movement, "Protection", "safewalk", "Safe walk", "Prevents you from falling off edges");
    Toggle(m, Movement, "Protection", "antientitypush", "Anti entity push", "Prevents you from getting pushed by players and mobs");
    Toggle(m, Movement, "Protection", "antihunger", "Anti hunger", "Slows down your hunger when you are walking");
    Toggle(m, Movement, "Auto", "autowalk", "Auto walk", "Makes you walk automatically");
    Toggle(m, Movement, "Auto", "sneak", "Sneak", "Makes you sneak automatically");
    Toggle(m, Movement, "Auto", "invwalk", "Inventory walk", "Move around while the inventory is open");
    Toggle(m, Movement, "Auto", "follow", "Follow", "A bot that follows the closest entity");

    Toggle(m, Render, "ESP", "playeresp", "Player ESP", "Highlights nearby players, including mannequins");
    Toggle(m, Render, "ESP", "mobesp", "Mob ESP", "Highlights nearby mobs");
    Toggle(m, Render, "ESP", "itemesp", "Item ESP", "Highlights nearby items");
    Toggle(m, Render, "ESP", "chestesp", "Chest ESP", "Highlights nearby chests");
    Toggle(m, Render, "ESP", "portalesp", "Portal ESP", "Highlights nearby portals");
    Toggle(m, Render, "ESP", "barrieresp", "Barrier ESP", "See nearby barrier blocks");
    Toggle(m, Render, "ESP", "mobspawnesp", "Mob spawn ESP", "Highlights areas where mobs can spawn");
    Toggle(m, Render, "ESP", "openwateresp", "Open water ESP", "Shows if your fishing spot counts as open water");
    Toggle(m, Render, "ESP", "prophuntesp", "Prophunt ESP", "See fake blocks in Prophunt");
    Toggle(m, Render, "ESP", "truesight", "True sight", "See invisible entities");
    Toggle(m, Render, "ESP", "radar", "Radar", "Shows the location of nearby entities");
    Toggle(m, Render, "ESP", "healthtags", "Health tags", "Shows the health of players in their nametags");
    Toggle(m, Render, "ESP", "nametags", "Name tags", "Changes the scale of the nametags so you can always read them");
    Toggle(m, Render, "ESP", "trajectories", "Trajectories", "Predicts the flight path of arrows and throwable items");
    Toggle(m, Render, "Find", "xray", "X-ray", "See ores through other blocks");
    Toggle(m, Render, "Find", "search", "Search", "Helps you to find specific blocks by highlighting them in rainbow color");
    Toggle(m, Render, "Find", "cavefinder", "Cave finder", "Helps you to find caves by highlighting them in the selected color");
    Toggle(m, Render, "Find", "basefinder", "Base finder", "Finds player bases by searching for man-made blocks");
    Toggle(m, Render, "Find", "newchunks", "New chunks", "Highlights newly generated chunks around you");
    Toggle(m, Render, "Find", "logoffspot", "Logoff spot", "Marks the spot where a nearby player logged out");
    Toggle(m, Render, "Find", "strongholdfinder", "Stronghold finder", "Finds the stronghold by triangulating your Eye of Ender throws");
    Toggle(m, Render, "Camera", "freecam", "Freecam", "Move the camera without moving your character");
    Toggle(m, Render, "Camera", "cameradistance", "Camera distance", "Change the camera distance in 3rd person");
    Toggle(m, Render, "Camera", "cameranoclip", "Camera no-clip", "The 3rd person camera goes through walls");
    Toggle(m, Render, "Camera", "remoteview", "Remote view", "See the world as someone else");
    Toggle(m, Render, "Camera", "nohurtcam", "No hurtcam", "Disables the shaking effect when you get hurt");
    Toggle(m, Render, "Camera", "antiwobble", "Anti wobble", "Disables the wobble effect caused by nausea and portals");
    Toggle(m, Render, "Screen", "fullbright", "Fullbright", "See in the dark");
    Toggle(m, Render, "Screen", "antiblind", "Anti blind", "Prevents blindness and darkness effects");
    Toggle(m, Render, "Screen", "nofog", "No fog", "Removes distance fog from the world");
    Toggle(m, Render, "Screen", "noweather", "No weather", "Alter the client-side weather, time and moon phase");
    Toggle(m, Render, "Screen", "nooverlay", "No overlay", "Blocks the overlays of water, lava, and powder snow");
    Toggle(m, Render, "Screen", "nofireoverlay", "No fire overlay", "Blocks the overlay when you are on fire");
    Toggle(m, Render, "Screen", "nopumpkin", "No pumpkin", "Blocks the overlay when wearing a pumpkin on your head");
    Toggle(m, Render, "Screen", "noshieldoverlay", "No shield overlay", "Lowers the shield so it covers less of your screen");
    Toggle(m, Render, "Screen", "novignette", "No vignette", "Removes the vignette effect (darkening of the screen edges)");
    Toggle(m, Render, "Screen", "nobackground", "No inventory background", "Removes the dark background behind inventories");
    Toggle(m, Render, "Screen", "particleremover", "Particle remover", "Removes every particle for a cleaner view and more FPS");
    Toggle(m, Render, "Screen", "tnttimer", "TNT timer", "Shows a countdown above every primed TNT");
    Toggle(m, Render, "Screen", "overlay", "Mining overlay", "Renders the Nuker animation whenever you mine a block");
    Toggle(m, Render, "Screen", "nameprotect", "Name protect", "Hides all player names");

    Toggle(m, Blocks, "Mining", "nuker", "Nuker", "Automatically breaks blocks around you");
    Toggle(m, Blocks, "Mining", "nukerlegit", "Nuker legit", "Slower Nuker that's much harder for anti-cheat plugins to detect");
    Toggle(m, Blocks, "Mining", "speednuker", "Speed nuker", "Faster version of Nuker that cannot bypass NoCheat+");
    Toggle(m, Blocks, "Mining", "kaboom", "Kaboom", "Breaks blocks around you like an explosion");
    Toggle(m, Blocks, "Mining", "fastbreak", "Fast break", "Break blocks faster");
    Toggle(m, Blocks, "Mining", "automine", "Auto mine", "Automatically mines any block that you look at");
    Toggle(m, Blocks, "Mining", "veinminer", "Vein miner", "Automatically mines ore veins for you");
    Toggle(m, Blocks, "Mining", "tunneller", "Tunneller", "Automatically digs a tunnel");
    Toggle(m, Blocks, "Mining", "excavator", "Excavator", "Automatically breaks all blocks in the selected area");
    Toggle(m, Blocks, "Mining", "autotool", "Auto tool", "Automatically equips the fastest applicable tool in your hotbar when you try to break a block");
    Toggle(m, Blocks, "Building", "autobuild", "Auto build", "Builds things automatically");
    Toggle(m, Blocks, "Building", "instabuild", "Insta build", "Similar to AutoBuild, but tries to build the entire template in one tick");
    Toggle(m, Blocks, "Building", "templatetool", "Template tool", "Create custom templates for AutoBuild by scanning existing buildings");
    Toggle(m, Blocks, "Building", "scaffoldwalk", "Scaffold walk", "Automatically places blocks below your feet");
    Toggle(m, Blocks, "Building", "airplace", "Air place", "Place blocks in mid-air");
    Toggle(m, Blocks, "Building", "fastplace", "Fast place", "Place blocks 5 times faster");
    Toggle(m, Blocks, "Building", "liquids", "Liquids", "Place blocks in liquids");
    Toggle(m, Blocks, "Building", "buildrandom", "Build random", "Randomly places blocks around you");
    Toggle(m, Blocks, "Building", "instantbunker", "Instant bunker", "Builds a small bunker around you");
    Toggle(m, Blocks, "Building", "handnoclip", "Hand no-clip", "Reach specific blocks through walls");
    Toggle(m, Blocks, "Building", "autosign", "Auto sign", "Instantly writes whatever text you want on every sign you place or edit");
    Toggle(m, Blocks, "Building", "anticactus", "Anti cactus", "Protects you from cactus damage");
    Toggle(m, Blocks, "Farming", "autofarm", "Auto farm", "Harvests and replants crops automatically");
    Toggle(m, Blocks, "Farming", "bonemealaura", "Bonemeal aura", "Automatically uses bone meal on specific types of plants");
    Toggle(m, Blocks, "Farming", "tillaura", "Till aura", "Turns dirt and grass around you into farmland");
    Toggle(m, Blocks, "Farming", "treebot", "Tree bot", "An experimental bot that automatically walks around and chops down trees");

    Toggle(m, Items, "Inventory", "autodrop", "Auto drop", "Automatically drops unwanted items");
    Toggle(m, Items, "Inventory", "autoeat", "Auto eat", "Automatically eats food when necessary");
    Toggle(m, Items, "Inventory", "autosteal", "Auto steal", "Automatically steals everything from all chests and shulker boxes that you open");
    Toggle(m, Items, "Inventory", "autoswitch", "Auto switch", "Switches the item in your hand all the time");
    Toggle(m, Items, "Inventory", "restock", "Restock", "Automatically restocks your hand with the selected items from your inventory");
    Toggle(m, Items, "Creative mode", "itemgenerator", "Item generator", "Generates random items and drops them on the ground");
    Toggle(m, Items, "Creative mode", "killpotion", "Kill potion", "Generates a potion that can kill almost anything, including players in Creative mode");
    Toggle(m, Items, "Creative mode", "trollpotion", "Troll potion", "Generates a potion with many annoying effects on it");
    Toggle(m, Items, "Creative mode", "opitems", "OP items", "Make OP tools, weapons and armor with any enchantment level");

    Toggle(m, Other, "Chat", "antispam", "Anti spam", "Blocks chat spam by adding a counter to repeated messages");
    Toggle(m, Other, "Chat", "autocomplete", "Chat auto-complete", "Auto-completes your chat messages using large language models");
    Toggle(m, Other, "Chat", "chattranslator", "Chat translator", "Translates chat messages using Google Translate");
    Toggle(m, Other, "Chat", "fancychat", "Fancy chat", "Replaces ASCII characters in sent chat messages with fancier unicode characters");
    Toggle(m, Other, "Chat", "infinichat", "Infinite chat", "Removes the 256 character limit from the chat");
    Toggle(m, Other, "Chat", "masstpa", "Mass TPA", "Sends a TPA request to all players");
    Toggle(m, Other, "Fun", "derp", "Derp", "Randomly moves your head around");
    Toggle(m, Other, "Fun", "headroll", "Head roll", "Makes you nod all the time");
    Toggle(m, Other, "Fun", "lsd", "LSD", "Causes hallucinations");
    Toggle(m, Other, "Fun", "mileycyrus", "Twerk", "Makes you twerk");
    Toggle(m, Other, "Fun", "rainbowui", "Rainbow UI", "Makes everything colorful");
    Toggle(m, Other, "Fun", "skinderp", "Skin derp", "Randomly toggles parts of your skin");
    Toggle(m, Other, "Fun", "tired", "Tired", "Makes you look tired to other players");
    Toggle(m, Other, "Helpers", "autofish", "Auto fish", "Automatically catches fish using your best fishing rod");
    Toggle(m, Other, "Helpers", "autolibrarian", "Auto librarian", "Automatically trains a villager to become a librarian that sells a specific enchanted book");
    Toggle(m, Other, "Helpers", "feedaura", "Feed aura", "Automatically feeds animals around you");
    Toggle(m, Other, "Helpers", "antiafk", "Anti AFK", "Walks around randomly to hide you from AFK detectors");
    Toggle(m, Other, "Helpers", "potionsaver", "Potion saver", "Freezes all potion effects while you are standing still");
    Toggle(m, Other, "Server", "autoreconnect", "Auto reconnect", "Automatically reconnects when you get kicked from the server");
    Toggle(m, Other, "Server", "portalgui", "Portal GUI", "Open GUIs in portals");
    Toggle(m, Other, "Server", "reach", "Reach", "Reach further");
    Toggle(m, Other, "Server", "throw", "Throw", "Uses an item multiple times");
    Toggle(m, Other, "Server", "timer", "Timer", "Changes the speed of almost everything");
    Toggle(m, Other, "Safety", "panic", "Panic", "Instantly turns off all enabled hacks");
    Toggle(m, Other, "Safety", "toomanyhax", "Too many hax", "Blocks any features that you don't want");

    Action(m, "Control", "b_stop", "Stop", "Baritone is idle", "Stop");
    Action(m, "Control", "b_pause", "Pause", "Pauses what Baritone is doing", "Pause");
    Action(m, "Control", "b_resume", "Resume", "Carries on after a pause", "Resume");
    Action(m, "Control", "b_forcecancel", "Force stop", "Stops everything, even when Baritone is stuck", "Force");
    Toggle(m, Baritone, "Control", "baritonepanel", "Baritone panel", "Opens the Baritone panel in the game, Right Ctrl also opens it");

    Input(m, "Travel", "b_goto", "Go to coordinates", "Leave Y empty to go to X and Z, or type only Y", "Go", {"X", "Y", "Z"});
    Input(m, "Travel", "b_gotoblock", "Go to a block", "Walks to the closest block of that kind", "Go", {"Block, like crafting_table"});
    Input(m, "Travel", "b_explore", "Explore", "Explores new chunks, from here or from X Z", "Explore", {"X (optional)", "Z (optional)"});
    Input(m, "Travel", "b_thisway", "Walk this way", "Walks that many blocks the way you look", "Go", {"Blocks, like 1000"});
    Action(m, "Travel", "b_come", "Come to the camera", "Walks to where your camera is, handy with Freecam", "Come");
    Action(m, "Travel", "b_surface", "Go to the surface", "Finds the way up to open air", "Go");
    Action(m, "Travel", "b_elytra", "Fly with an elytra", "Flies to the goal in the Nether, set a goal first", "Fly");

    Input(m, "Goal", "b_goal", "Set a goal", "Sets where to go without walking yet", "Set", {"X", "Y", "Z"});
    Action(m, "Goal", "b_path", "Walk to the goal", "Starts walking to the goal you set", "Go");
    Action(m, "Goal", "b_invert", "Run from the goal", "Walks away from the goal instead", "Run");
    Action(m, "Goal", "b_axis", "Go to a highway", "Walks to the closest axis or diagonal highway", "Go");
    Action(m, "Goal", "b_click", "Click a goal", "Opens a screen where you click the spot to go to", "Open");

    Input(m, "Mining", "b_mine", "Mine blocks", "Finds and mines blocks, stops at the amount if you set one", "Mine",
          {"Blocks, like diamond_ore iron_ore", "Amount"}, {3.0f, 1.0f});
    Input(m, "Mining", "b_tunnel", "Dig a tunnel", "Leave the boxes empty for a 1x2 tunnel", "Dig", {"Height", "Width", "Depth"});
    Input(m, "Mining", "b_find", "Find a block", "Lists where Baritone has seen that block, in chat", "Find", {"Block, like spawner"});

    Input(m, "Bots", "b_farm", "Farm", "Harvests and replants crops around you", "Farm", {"Range (optional)"});
    Input(m, "Bots", "b_follow", "Follow a player", "Leave the name empty to follow every player", "Follow", {"Player name (optional)"});
    Input(m, "Bots", "b_followmob", "Follow mobs", "Leave it empty to follow every mob", "Follow", {"Mob, like cow (optional)"});
    Action(m, "Bots", "b_pickup", "Pick up items", "Collects dropped items around you", "Pick up");

    Input(m, "Building", "b_build", "Build a schematic", "Files from the .minecraft/schematics folder, X Y Z is optional", "Build",
          {"File name", "X", "Y", "Z"}, {2.2f, 1.0f, 1.0f, 1.0f});
    Action(m, "Building", "b_litematica", "Build the Litematica placement", "Builds what you placed with Litematica", "Build");
    Action(m, "Building", "b_schematica", "Build the Schematica placement", "Builds what you placed with Schematica", "Build");

    Action(m, "Selection", "b_sel1", "Corner 1 here", "Marks the first corner where you stand", "Set");
    Action(m, "Selection", "b_sel2", "Corner 2 here", "Marks the second corner where you stand", "Set");
    Input(m, "Selection", "b_selfill", "Fill the selection", "Fills the box with one block", "Fill", {"Block, like stone"});
    Input(m, "Selection", "b_selreplace", "Replace blocks", "Swaps one block for another in the box", "Replace", {"Old block", "New block"});
    Input(m, "Selection", "b_selwalls", "Build walls", "Builds the four walls of the box", "Build", {"Block"});
    Input(m, "Selection", "b_selshell", "Build a shell", "Builds walls, floor and roof of the box", "Build", {"Block"});
    Action(m, "Selection", "b_selcleararea", "Clear the area", "Mines out everything in the box", "Clear");
    Action(m, "Selection", "b_selclear", "Forget the selection", "Removes the box", "Forget");

    Input(m, "Waypoints", "b_wpsave", "Save a waypoint here", "Remembers where you stand", "Save", {"Name"});
    Input(m, "Waypoints", "b_wpgoto", "Go to a waypoint", "Walks to a saved waypoint", "Go", {"Name"});
    Input(m, "Waypoints", "b_wpdel", "Delete a waypoint", "Removes a saved waypoint", "Delete", {"Name"});
    Action(m, "Waypoints", "b_wplist", "List waypoints", "Shows your waypoints in chat", "List");
    Action(m, "Waypoints", "b_sethome", "Set home here", "Saves this spot as home", "Set");
    Action(m, "Waypoints", "b_home", "Go home", "Walks back to your home waypoint", "Go");

    Action(m, "More", "b_proc", "What is it doing", "Shows Baritone's current task in chat", "Show");
    Action(m, "More", "b_eta", "Time left", "Shows how long until the goal, in chat", "Show");
    Action(m, "More", "b_blacklist", "Skip this target", "Stops going for the block it is going to now", "Skip");
    Action(m, "More", "b_render", "Redraw the path", "Fixes the path drawing when it looks wrong", "Redraw");
    Action(m, "More", "b_repack", "Rescan chunks", "Rebuilds Baritone's map of the loaded chunks", "Rescan");
    Action(m, "More", "b_gc", "Free memory", "Asks Java to clean up memory", "Free");
    Input(m, "More", "b_custom", "Any Baritone command", "Runs it like typing # in chat", "Run", {"Command, like goto 0 64 0"});

    Setting(m, "Moving rules", "allowBreak", "Break blocks", "Lets Baritone mine through blocks", true);
    Setting(m, "Moving rules", "allowPlace", "Place blocks", "Lets Baritone bridge and pillar up", true);
    Setting(m, "Moving rules", "allowSprint", "Sprint", "Sprints while walking", true);
    Setting(m, "Moving rules", "allowParkour", "Parkour jumps", "Jumps over gaps", false);
    Setting(m, "Moving rules", "allowParkourPlace", "Parkour with blocks", "Places a block while jumping a gap", false);
    Setting(m, "Moving rules", "allowDownward", "Dig down", "Mines straight down", true);
    Setting(m, "Moving rules", "allowDiagonalAscend", "Climb diagonally", "Goes up a block diagonally", false);
    Setting(m, "Moving rules", "allowDiagonalDescend", "Drop diagonally", "Goes down a block diagonally", false);
    Setting(m, "Moving rules", "allowWaterBucketFall", "Water bucket falls", "Uses a water bucket to fall safely", true);
    Setting(m, "Moving rules", "assumeWalkOnWater", "Walk on water", "Plans paths over water, use it with Walk on water", false);
    Setting(m, "Moving rules", "sprintInWater", "Sprint in water", "Swims faster", true);
    Setting(m, "Moving rules", "avoidance", "Avoid mobs", "Keeps away from mobs and spawners", false);
    Number(m, "Moving rules", "followRadius", "Follow distance", 1, 16, 3, 1, "%.0f blocks");
    Number(m, "Moving rules", "maxFallHeightNoWater", "Safe fall height", 2, 20, 3, 1, "%.0f blocks");

    Setting(m, "Work rules", "autoTool", "Best tool", "Switches to the best tool for each block", true);
    Setting(m, "Work rules", "allowInventory", "Use the inventory", "Moves tools and blocks from the inventory to the hotbar", false);
    Setting(m, "Work rules", "legitMine", "Mine only seen ores", "Only mines ores it can see, like a player", false);
    Setting(m, "Work rules", "mineScanDroppedItems", "Pick up mined items", "Goes back for the drops while mining", true);
    Setting(m, "Work rules", "walkWhileBreaking", "Walk while breaking", "Keeps moving while it mines", true);
    Setting(m, "Work rules", "replantCrops", "Replant crops", "Replants after farming", true);
    Setting(m, "Work rules", "buildInLayers", "Build in layers", "Builds schematics one layer at a time", false);
    Setting(m, "Work rules", "antiCheatCompatibility", "Server friendly", "Moves the camera like a player for servers", true);
    Number(m, "Work rules", "blockReachDistance", "Reach", 1, 6, 4.5f, 0.5f, "%.1f blocks");

    Setting(m, "Display", "freeLook", "Free look", "Lets you look around while Baritone walks", true);
    Setting(m, "Display", "renderPath", "Show the path", "Draws the path in the world", true);
    Setting(m, "Display", "renderGoal", "Show the goal", "Draws the goal in the world", true);
    Setting(m, "Display", "chatControl", "Chat commands", "Type # commands in the chat", true);
    Setting(m, "Display", "desktopNotifications", "Windows notifications", "Tells you when a task is done", false);

    m.gameName = "Minecraft";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
