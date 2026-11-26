#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cctype>
#include <deque>
#include <mutex>
#include <unordered_set>

namespace bl3 {

namespace {

struct GiverItem {
    std::vector<uint8_t> asset;
    std::string name, category, path, assetName;
    bool top = false;
};

std::vector<GiverItem> list;
bool built = false, building = false, failed = false;
double worldSince = 0;
void* lastWorld = nullptr;
std::mutex queueLock;
std::vector<std::pair<int, int>> queue;
std::deque<std::vector<uint8_t>> keepAlive;
core::Model* modelPtr = nullptr;

const char* kTop[] = {"smartgun",   "monarch",     "lucian",      "hellwalker", "sandhawk",     "plaguebearer", "kaoson",      "flakker",
                      "freeradical", "hellshock",  "recursion",   "reflux",     "kybsworth",    "ioncannon",    "backburner",  "unkempt",
                      "maggie",      "hyperfocus", "crader",      "dictator",   "anarchy",      "crossroad",    "thelob",      "sellout",
                      "conference",  "trevonator", "shredifier",  "kenulox",    "yellowcake",   "linoge",       "monocle",     "clairvoyance",
                      "transformer", "recharger",  "frontloader", "stopgap",    "roughrider",   "bigboom",      "whiskey",     "hex",
                      "ghastcall",   "fastball",   "stormfront",  "quasar",     "hunterseeker", "bloodletter",  "greenmonster", "spiritualdriver",
                      "phasezerker", "executor",   "seeindead",   "rakkcommander", "bountyhunter", "redfang",   "victoryrush", "deathless",
                      "pearl",       "commanderplanetoid", "elementalprojector", "snowshoe", "ottoidol"};

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Squash(const std::string& s) {
    std::string out;
    for (char c : s)
        if (std::isalnum((unsigned char)c)) out.push_back((char)std::tolower((unsigned char)c));
    return out;
}

bool Generic(const std::string& lower) {
    for (const char* t : {"_01_common", "_02_uncommon", "_03_rare", "_04_veryrare", "_05_legendary", "_ai_", "useonly", "test", "debug", "dev_",
                          "_default", "mission", "quest", "showcase", "tutorial", "_base"})
        if (lower.find(t) != std::string::npos) return true;
    return false;
}

std::string Pretty(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '_') {
            out.push_back(' ');
            continue;
        }
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)raw[i - 1])) out.push_back(' ');
        out.push_back(c);
    }
    return out;
}

std::string TailName(const std::string& name, int skip) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i) {
        if (i == name.size() || name[i] == '_') {
            parts.push_back(name.substr(start, i - start));
            start = i + 1;
        }
    }
    if ((int)parts.size() <= skip) return name;
    std::string out;
    for (size_t i = (size_t)skip; i < parts.size(); ++i) {
        if (parts[i].empty()) continue;
        std::string l = Lower(parts[i]);
        if (l == "lgd" || l == "legendary" || l == "unique" || l == "balance") continue;
        if (!out.empty()) out += "_";
        out += parts[i];
    }
    return out.empty() ? name : out;
}

bool Classify(const std::string& name, const std::string& path, const std::string& classToken, GiverItem& it) {
    std::string l = Lower(name), lp = Lower(path);
    if (Generic(l)) return false;
    bool unique = lp.find("/_unique/") != std::string::npos || lp.find("/unique/") != std::string::npos;
    if (l.rfind("balance_", 0) == 0) {
        static const std::pair<const char*, const char*> types[] = {{"_ps_", "Pistols"},        {"_sm_", "SMGs"},
                                                                    {"_ar_", "Assault rifles"}, {"_sg_", "Shotguns"},
                                                                    {"_sr_", "Sniper rifles"},  {"_hw_", "Heavy weapons"}};
        for (auto& t : types)
            if (l.find(t.first) != std::string::npos) {
                if (!unique) return false;
                it.category = t.second;
                it.name = Pretty(TailName(name, 3));
                return true;
            }
        return false;
    }
    if (l.rfind("invbald_shield_", 0) == 0) {
        it.category = "Shields";
        it.name = Pretty(TailName(name, 2));
        return true;
    }
    if (l.rfind("invbald_gm_", 0) == 0 || l.rfind("invbald_grenade", 0) == 0) {
        it.category = "Grenades";
        it.name = Pretty(TailName(name, 2));
        return true;
    }
    if (l.rfind("invbald_cm_", 0) == 0 || l.rfind("invbald_classmod", 0) == 0) {
        static const char* classes[] = {"siren", "beastmaster", "gunner", "operative"};
        bool any = false, mine = false;
        for (const char* c : classes)
            if (l.find(c) != std::string::npos) {
                any = true;
                if (classToken.find(c) != std::string::npos) mine = true;
            }
        if (any && !mine) return false;
        it.category = "Class mods";
        it.name = Pretty(TailName(name, 2));
        return true;
    }
    if (l.rfind("invbald_artifact", 0) == 0) {
        it.category = "Artifacts";
        it.name = Pretty(TailName(name, 2));
        return true;
    }
    return false;
}

void* Registry() {
    ue::Call g(Lib("AssetRegistryHelpers"), "GetAssetRegistry");
    if (!g.Run()) return nullptr;
    const uint8_t* raw = g.Raw("ReturnValue");
    void* reg = raw ? ue::At<void*>(raw, 0) : nullptr;
    return ue::Alive(reg) ? reg : nullptr;
}

void Publish() {
    std::vector<core::Item> items;
    for (size_t i = 0; i < list.size(); ++i) {
        core::Item it;
        it.id = std::to_string(i);
        it.name = list[i].name;
        it.category = list[i].top ? "Top picks" : list[i].category;
        items.push_back(it);
    }
    core::Item ammo;
    ammo.id = "ammo";
    ammo.name = "Refill all ammo and grenades";
    ammo.category = "Ammo";
    items.push_back(ammo);
    WithModel([&](core::Model& m) { m.items = std::move(items); });
}

void Build() {
    building = true;
    void* reg = Registry();
    if (!reg) {
        failed = true;
        building = false;
        LOG("giver: asset registry not found");
        Note("The Giver could not read the game's item list");
        return;
    }
    void* fn = ue::FindFunction(ue::ClassOf(reg), "GetAssetsByClass");
    if (!fn) fn = ue::FindFunction(Cls("AssetRegistry"), "GetAssetsByClass");
    int assetSize = ue::StructSize(Cls("AssetData"));
    if (!fn || assetSize < 0x28 || assetSize > 0x100) {
        failed = true;
        building = false;
        LOG("giver: GetAssetsByClass missing (size %d)", assetSize);
        Note("The Giver could not read the game's item list");
        return;
    }
    std::string classToken;
    void* pcClass = st.pc ? ue::Ptr(st.pc, "CurrentPlayerClass") : nullptr;
    if (pcClass) classToken = Lower(ue::Name(pcClass));
    std::unordered_set<int32_t> seen;
    list.clear();
    for (const char* cls : {"InventoryBalanceData", "OakInventoryBalanceData"}) {
        ue::Call c(reg, fn, 0);
        c.Set("ClassName", ue::MakeName(ue::Wide(cls))).SetBool("bSearchSubClasses", true);
        if (!c.Run()) continue;
        auto out = c.Get<ue::TArray<uint8_t>>("OutAssetData");
        if (!out.data || out.num <= 0 || out.num > 200000) continue;
        LOG("giver: %d assets of %s", out.num, cls);
        for (int i = 0; i < out.num; ++i) {
            const uint8_t* e = out.data + (size_t)i * assetSize;
            ue::FName objPath = ue::At<ue::FName>(e, 0);
            if (!seen.insert(objPath.index).second) continue;
            std::string name = ue::NameToString(ue::At<ue::FName>(e, 0x18));
            std::string path = ue::NameToString(ue::At<ue::FName>(e, 0x10));
            GiverItem it;
            if (!Classify(name, path, classToken, it)) continue;
            if (!DlcAllowed(path)) continue;
            it.asset.assign(e, e + assetSize);
            it.path = path;
            it.assetName = name;
            std::string sq = Squash(name);
            for (const char* t : kTop)
                if (sq.find(t) != std::string::npos) it.top = true;
            list.push_back(std::move(it));
        }
    }
    std::sort(list.begin(), list.end(), [](const GiverItem& a, const GiverItem& b) {
        if (a.top != b.top) return a.top;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });
    LOG("giver: %zu items in the list", list.size());
    built = true;
    building = false;
    if (list.empty()) {
        failed = true;
        Note("The Giver found no items in this version of the game");
    }
    Publish();
}

int PlayerLevel() {
    void* b = st.pawn ? ue::Ptr(st.pawn, "PlayerBalanceComponent") : nullptr;
    ue::Call c(b, "GetExperienceLevel");
    int l = c.Run() ? c.Get<int32_t>("ReturnValue") : 1;
    return std::clamp(l, 1, 80);
}

void SpawnItem(const GiverItem& it, int count) {
    ue::Call get(Lib("AssetRegistryHelpers"), "GetAsset");
    get.SetRaw("InAssetData", it.asset.data(), (int)it.asset.size());
    void* bal = get.Run() ? get.Get<void*>("ReturnValue") : nullptr;
    if (!ue::Alive(bal)) {
        Note("Could not load " + it.name);
        return;
    }
    void* data = ue::Ptr(bal, "InventoryData");
    void* manufacturer = nullptr;
    ue::Prop mp = ue::FindProp(ue::ClassOf(bal), "Manufacturers");
    auto mans = ue::Read<ue::TArray<uint8_t>>(bal, mp);
    if (mp && mans.data && mans.num > 0) {
        void* m = ue::At<void*>(mans.data, 0);
        if (ue::Valid(m)) manufacturer = m;
    }
    void* selStruct = Cls("SelectedInventoryInfo");
    void* reqStruct = Cls("SpawnDroppedPickupLootRequest");
    int selSize = ue::StructSize(selStruct), reqSize = ue::StructSize(reqStruct);
    if (!data || selSize <= 0 || reqSize <= 0 || selSize > 0x400 || reqSize > 0x400) {
        Note("Could not create " + it.name);
        return;
    }
    count = std::clamp(count, 1, 10);
    int level = PlayerLevel();
    keepAlive.emplace_back((size_t)selSize * count, 0);
    std::vector<uint8_t>& sel = keepAlive.back();
    for (int i = 0; i < count; ++i) {
        uint8_t* e = sel.data() + (size_t)i * selSize;
        ue::Put<void*>(e, ue::FindProp(selStruct, "InventoryData").offset, data);
        ue::Put<void*>(e, ue::FindProp(selStruct, "InventoryBalanceData").offset, bal);
        if (manufacturer) ue::Put<void*>(e, ue::FindProp(selStruct, "ManufacturerData").offset, manufacturer);
        ue::Put<int32_t>(e, ue::FindProp(selStruct, "GameStage").offset, level);
    }
    std::vector<uint8_t> req((size_t)reqSize, 0);
    ue::Prop ctx = ue::FindProp(reqStruct, "ContextActor");
    ue::Prop infos = ue::FindProp(reqStruct, "SelectedInventoryInfos");
    ue::Prop policy = ue::FindProp(reqStruct, "InstancingPolicy");
    ue::Prop player = ue::FindProp(reqStruct, "PlayerToInstanceFor");
    if (!infos) {
        Note("Could not create " + it.name);
        return;
    }
    if (ctx) ue::Put<void*>(req.data(), ctx.offset, st.pawn);
    ue::TArray<uint8_t> arr{sel.data(), count, count};
    ue::Put(req.data(), infos.offset, arr);
    if (policy && player) {
        ue::Put<uint8_t>(req.data(), policy.offset, 2);
        ue::Put<void*>(req.data(), player.offset, st.pc);
    }
    ue::Call c(Lib("OakBlueprintLibrary"), "SpawnLootAsync");
    c.Set<void*>("ContextActor", st.pawn).SetRaw("Request", req.data(), reqSize);
    if (c.Run()) {
        Note("Dropped " + std::to_string(count) + " x " + it.name + " at your feet");
        LOG("giver: spawned %d x %s (%s)", count, it.assetName.c_str(), it.path.c_str());
    } else {
        Note("The game refused to create " + it.name);
    }
    while (keepAlive.size() > 64) keepAlive.pop_front();
}

}

void GiverInit(core::Model& m) {
    modelPtr = &m;
    bool ok = HasFunc("AssetRegistryHelpers", "GetAssetRegistry") && HasFunc("AssetRegistryHelpers", "GetAsset") &&
              HasFunc("OakBlueprintLibrary", "SpawnLootAsync") && Has("SpawnDroppedPickupLootRequest", "SelectedInventoryInfos") &&
              Has("SelectedInventoryInfo", "InventoryBalanceData");
    Ready("giver", ok);
    {
        std::lock_guard<std::mutex> g(queueLock);
        queue.clear();
    }
    WithModel([](core::Model& mm) {
        mm.categories = {"Top picks", "Pistols", "SMGs", "Assault rifles", "Shotguns", "Sniper rifles", "Heavy weapons", "Shields", "Grenades",
                         "Class mods", "Artifacts", "Ammo"};
        mm.spawn = [](const core::Item& it, int n) {
            int idx = it.id == "ammo" ? -1 : std::atoi(it.id.c_str());
            std::lock_guard<std::mutex> g(queueLock);
            queue.push_back({idx, n});
        };
    });
}

void GiverTick() {
    if (st.world != lastWorld) {
        lastWorld = st.world;
        worldSince = st.time;
    }
    if (!built && !building && !failed && st.time - worldSince > 6.0 && (DlcKnown() || st.time - worldSince > 20.0)) Build();
    std::vector<std::pair<int, int>> todo;
    {
        std::lock_guard<std::mutex> g(queueLock);
        todo.swap(queue);
    }
    for (auto& [idx, n] : todo) {
        if (idx < 0) {
            RefillAllAmmoNow();
            continue;
        }
        if (idx >= (int)list.size()) continue;
        SpawnItem(list[(size_t)idx], n);
    }
}

void GiverOff() {
    std::lock_guard<std::mutex> g(queueLock);
    queue.clear();
}

}
