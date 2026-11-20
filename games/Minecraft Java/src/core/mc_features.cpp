#include "core/mc_features.h"
#include "imgui.h"
#include "ui/icons.h"

namespace mc {

using core::Feature;
using core::Model;
using core::Tab;

namespace {

const Tab Combat = Tab::Biomods;
const Tab Movement = Tab::Survival;
const Tab Render = Tab::World;
const Tab Blocks = Tab::Crafting;
const Tab Items = Tab::Spawner;
const Tab ChatFun = Tab::BaseHub;
const Tab Other = Tab::Legit;

void Toggle(Model& m, Tab tab, const char* group, const char* id, const char* label, const char* hint) {
    Feature f;
    f.tab = tab;
    f.group = group;
    f.id = id;
    f.label = label;
    f.hint = hint;
    f.building = true;
    m.features.push_back(std::move(f));
}

}

void BuildFeatures(Model& m) {
    m.features.clear();
    m.features.reserve(160);
    m.tabs = {Combat, Movement, Render, Blocks, Items, ChatFun, Other, Tab::Settings};
    m.plainTabs = {Combat, Items, ChatFun, Other};
    m.tabLabels = {{Combat, "Combat", ICON_CROSSHAIR},
                   {Movement, "Movement", ICON_NAVIGATION},
                   {Render, "Render", ICON_EYE},
                   {Blocks, "Blocks", ICON_HAMMER},
                   {Items, "Items", ICON_PACKAGE},
                   {ChatFun, "Chat & Fun", ICON_SPARKLES},
                   {Other, "Other", ICON_SLIDERS_HORIZONTAL}};

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

    Toggle(m, ChatFun, "Chat", "antispam", "Anti spam", "Blocks chat spam by adding a counter to repeated messages");
    Toggle(m, ChatFun, "Chat", "autocomplete", "Chat auto-complete", "Auto-completes your chat messages using large language models");
    Toggle(m, ChatFun, "Chat", "chattranslator", "Chat translator", "Translates chat messages using Google Translate");
    Toggle(m, ChatFun, "Chat", "fancychat", "Fancy chat", "Replaces ASCII characters in sent chat messages with fancier unicode characters");
    Toggle(m, ChatFun, "Chat", "infinichat", "Infinite chat", "Removes the 256 character limit from the chat");
    Toggle(m, ChatFun, "Chat", "masstpa", "Mass TPA", "Sends a TPA request to all players");
    Toggle(m, ChatFun, "Fun", "derp", "Derp", "Randomly moves your head around");
    Toggle(m, ChatFun, "Fun", "headroll", "Head roll", "Makes you nod all the time");
    Toggle(m, ChatFun, "Fun", "lsd", "LSD", "Causes hallucinations");
    Toggle(m, ChatFun, "Fun", "mileycyrus", "Twerk", "Makes you twerk");
    Toggle(m, ChatFun, "Fun", "rainbowui", "Rainbow UI", "Makes everything colorful");
    Toggle(m, ChatFun, "Fun", "skinderp", "Skin derp", "Randomly toggles parts of your skin");
    Toggle(m, ChatFun, "Fun", "tired", "Tired", "Makes you look tired to other players");

    Toggle(m, Other, "Helpers", "autofish", "Auto fish", "Automatically catches fish using your best fishing rod");
    Toggle(m, Other, "Helpers", "autolibrarian", "Auto librarian", "Automatically trains a villager to become a librarian that sells a specific enchanted book");
    Toggle(m, Other, "Helpers", "feedaura", "Feed aura", "Automatically feeds animals around you");
    Toggle(m, Other, "Helpers", "antiafk", "Anti AFK", "Walks around randomly to hide you from AFK detectors");
    Toggle(m, Other, "Server", "autoreconnect", "Auto reconnect", "Automatically reconnects when you get kicked from the server");
    Toggle(m, Other, "Server", "portalgui", "Portal GUI", "Open GUIs in portals");
    Toggle(m, Other, "Server", "reach", "Reach", "Reach further");
    Toggle(m, Other, "Server", "throw", "Throw", "Uses an item multiple times");
    Toggle(m, Other, "Server", "timer", "Timer", "Changes the speed of almost everything");
    Toggle(m, Other, "Safety", "panic", "Panic", "Instantly turns off all enabled hacks");
    Toggle(m, Other, "Safety", "toomanyhax", "Too many hax", "Blocks any features that you don't want");

    m.gameName = "Minecraft";
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
