#include "game/lists.h"

namespace gtav::lists {

const Weapon kWeapons[] = {
    {"WEAPON_PISTOL", "Pistol", "Pistols"},
    {"WEAPON_COMBATPISTOL", "Combat Pistol", "Pistols"},
    {"WEAPON_APPISTOL", "AP Pistol", "Pistols"},
    {"WEAPON_PISTOL50", "Pistol .50", "Pistols"},
    {"WEAPON_STUNGUN", "Stun Gun", "Pistols"},
    {"WEAPON_MICROSMG", "Micro SMG", "SMGs"},
    {"WEAPON_SMG", "SMG", "SMGs"},
    {"WEAPON_ASSAULTSMG", "Assault SMG", "SMGs"},
    {"WEAPON_PUMPSHOTGUN", "Pump Shotgun", "Shotguns"},
    {"WEAPON_SAWNOFFSHOTGUN", "Sawed-Off Shotgun", "Shotguns"},
    {"WEAPON_ASSAULTSHOTGUN", "Assault Shotgun", "Shotguns"},
    {"WEAPON_BULLPUPSHOTGUN", "Bullpup Shotgun", "Shotguns"},
    {"WEAPON_ASSAULTRIFLE", "Assault Rifle", "Assault rifles"},
    {"WEAPON_CARBINERIFLE", "Carbine Rifle", "Assault rifles"},
    {"WEAPON_ADVANCEDRIFLE", "Advanced Rifle", "Assault rifles"},
    {"WEAPON_MG", "MG", "Machine guns"},
    {"WEAPON_COMBATMG", "Combat MG", "Machine guns"},
    {"WEAPON_SNIPERRIFLE", "Sniper Rifle", "Sniper rifles"},
    {"WEAPON_HEAVYSNIPER", "Heavy Sniper", "Sniper rifles"},
    {"WEAPON_GRENADELAUNCHER", "Grenade Launcher", "Heavy weapons"},
    {"WEAPON_RPG", "RPG", "Heavy weapons"},
    {"WEAPON_MINIGUN", "Minigun", "Heavy weapons"},
    {"WEAPON_GRENADE", "Grenade", "Throwables"},
    {"WEAPON_STICKYBOMB", "Sticky Bomb", "Throwables"},
    {"WEAPON_SMOKEGRENADE", "Tear Gas", "Throwables"},
    {"WEAPON_BZGAS", "BZ Gas", "Throwables"},
    {"WEAPON_MOLOTOV", "Molotov", "Throwables"},
    {"WEAPON_KNIFE", "Knife", "Melee"},
    {"WEAPON_NIGHTSTICK", "Nightstick", "Melee"},
    {"WEAPON_HAMMER", "Hammer", "Melee"},
    {"WEAPON_BAT", "Baseball Bat", "Melee"},
    {"WEAPON_CROWBAR", "Crowbar", "Melee"},
    {"WEAPON_GOLFCLUB", "Golf Club", "Melee"},
    {"WEAPON_PETROLCAN", "Jerry Can", "Armor and items"},
    {"WEAPON_FIREEXTINGUISHER", "Fire Extinguisher", "Armor and items"},
    {"GADGET_PARACHUTE", "Parachute", "Armor and items"},
};
const size_t kWeaponCount = sizeof(kWeapons) / sizeof(kWeapons[0]);

const Ammo kAmmo[] = {
    {"AMMO_PISTOL", "Pistol ammo, 50 rounds", 50},
    {"AMMO_SMG", "SMG ammo, 100 rounds", 100},
    {"AMMO_SHOTGUN", "Shotgun shells, 25", 25},
    {"AMMO_RIFLE", "Rifle ammo, 100 rounds", 100},
    {"AMMO_MG", "MG ammo, 200 rounds", 200},
    {"AMMO_SNIPER", "Sniper rounds, 20", 20},
    {"AMMO_GRENADELAUNCHER", "Launcher grenades, 10", 10},
    {"AMMO_RPG", "Rockets, 5", 5},
    {"AMMO_MINIGUN", "Minigun ammo, 500 rounds", 500},
};
const size_t kAmmoCount = sizeof(kAmmo) / sizeof(kAmmo[0]);

const Armor kArmor[] = {
    {"Super light armor", 20},
    {"Light armor", 40},
    {"Standard armor", 60},
    {"Heavy armor", 80},
    {"Super heavy armor", 100},
};
const size_t kArmorCount = sizeof(kArmor) / sizeof(kArmor[0]);

const Snack kSnacks[] = {
    {"P's & Q's", 10},
    {"EgoChaser", 25},
    {"Meteorite", 40},
    {"eCola", 10},
    {"Sprunk", 10},
    {"First aid kit", 100},
};
const size_t kSnackCount = sizeof(kSnacks) / sizeof(kSnacks[0]);

const char* const kVehicles[] = {
    "blista", "dilettante", "dilettante2", "issi2", "panto", "prairie", "rhapsody", "brioso",
    "asea", "asea2", "asterope", "cog55", "cognoscenti", "emperor", "emperor2", "emperor3", "fugitive", "glendale", "ingot", "intruder",
    "premier", "primo", "primo2", "regina", "romero", "schafter2", "stanier", "stratum", "stretch", "superd", "surge", "tailgater",
    "warrener", "washington",
    "baller", "baller2", "bjxl", "cavalcade", "cavalcade2", "dubsta", "dubsta2", "fq2", "granger", "gresley", "habanero", "huntley",
    "landstalker", "mesa", "mesa2", "patriot", "radi", "rocoto", "seminole", "serrano",
    "cogcabrio", "exemplar", "f620", "felon", "felon2", "jackal", "oracle", "oracle2", "sentinel", "sentinel2", "zion", "zion2",
    "blade", "buccaneer", "chino", "dominator", "dukes", "dukes2", "gauntlet", "hotknife", "phoenix", "picador", "ratloader",
    "ratloader2", "ruiner", "sabregt", "slamvan", "stalion", "vigero", "voodoo2", "virgo",
    "btype", "casco", "coquette2", "jb700", "manana", "monroe", "peyote", "pigalle", "stinger", "stingergt", "tornado", "tornado2",
    "tornado3", "tornado4", "ztype", "feltzer3",
    "alpha", "banshee", "bestiagts", "blista2", "blista3", "buffalo", "buffalo2", "buffalo3", "carbonizzare", "comet2", "coquette",
    "elegy2", "feltzer2", "furoregt", "fusilade", "futo", "jester", "jester2", "khamelion", "kuruma", "kuruma2", "massacro",
    "massacro2", "ninef", "ninef2", "penumbra", "rapidgt", "rapidgt2", "schwarzer", "sultan", "surano",
    "adder", "bullet", "cheetah", "entityxf", "infernus", "osiris", "t20", "turismor", "vacca", "voltic", "zentorno",
    "akuma", "bagger", "bati", "bati2", "carbonrs", "daemon", "double", "enduro", "faggio2", "hakuchou", "hexer", "innovation",
    "lectro", "nemesis", "pcj", "ruffian", "sanchez", "sanchez2", "sovereign", "thrust", "vader", "vindicator",
    "bfinjection", "bifta", "blazer", "blazer2", "blazer3", "bodhi2", "brawler", "dloader", "dubsta3", "dune", "dune2", "insurgent",
    "insurgent2", "kalahari", "marshall", "mesa3", "monster", "rancherxl", "rancherxl2", "rebel", "rebel2", "sandking", "sandking2",
    "technical", "guardian",
    "bulldozer", "cutter", "dump", "flatbed", "handler", "mixer", "mixer2", "rubble", "tiptruck", "tiptruck2",
    "airtug", "caddy", "caddy2", "docktug", "forklift", "mower", "ripley", "sadler", "sadler2", "scrap", "towtruck", "towtruck2",
    "tractor", "tractor2", "tractor3", "utillitruck", "utillitruck2", "utillitruck3",
    "bison", "bison2", "bison3", "bobcatxl", "boxville", "boxville2", "boxville3", "burrito", "burrito2", "burrito3", "burrito4",
    "burrito5", "camper", "gburrito", "gburrito2", "journey", "minivan", "paradise", "pony", "pony2", "rumpo", "rumpo2", "speedo",
    "speedo2", "surfer", "surfer2", "taco", "youga",
    "bmx", "cruiser", "fixter", "scorcher", "tribike", "tribike2", "tribike3",
    "dinghy", "dinghy2", "dinghy3", "jetmax", "marquis", "predator", "seashark", "seashark2", "speeder", "squalo", "submersible",
    "submersible2", "suntrap", "toro", "tropic", "tug",
    "annihilator", "buzzard", "buzzard2", "cargobob", "cargobob2", "cargobob3", "frogger", "frogger2", "maverick", "polmav",
    "savage", "skylift", "swift", "swift2", "valkyrie",
    "besra", "blimp", "blimp2", "cargoplane", "cuban800", "dodo", "duster", "hydra", "jet", "lazer", "luxor", "luxor2", "mammatus",
    "miljet", "shamal", "stunt", "titan", "velum", "velum2", "vestra",
    "airbus", "bus", "coach", "rentalbus", "taxi", "tourbus", "trash", "trash2",
    "ambulance", "fbi", "fbi2", "firetruk", "lguard", "pbus", "police", "police2", "police3", "police4", "policeb", "policeold1",
    "policeold2", "policet", "pranger", "riot", "sheriff", "sheriff2",
    "barracks", "barracks2", "barracks3", "crusader", "rhino",
    "benson", "biff", "hauler", "mule", "mule2", "mule3", "packer", "phantom", "pounder", "stockade", "stockade3",
};
const size_t kVehicleCount = sizeof(kVehicles) / sizeof(kVehicles[0]);

const char* const kVehicleClasses[] = {
    "Compacts", "Sedans", "SUVs", "Coupes", "Muscle", "Sports classics", "Sports", "Super", "Motorcycles", "Off-road", "Industrial",
    "Utility", "Vans", "Cycles", "Boats", "Helicopters", "Planes", "Service", "Emergency", "Military", "Commercial", "Trains",
    "Open wheel",
};
const size_t kVehicleClassCount = sizeof(kVehicleClasses) / sizeof(kVehicleClasses[0]);

}
