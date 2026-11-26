#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <random>
#include <unordered_set>

namespace bl3 {

namespace {

struct WeightSave {
    ue::FWeak pool;
    uint8_t* addr = nullptr;
    float value = 0;
};

std::vector<WeightSave> weightSaves;
std::unordered_set<void*> processedPools;
int appliedMask = 0;
size_t poolCursor = 0, expCursor = 0;

int RarityOf(void* attr) {
    std::string n = ue::Name(attr);
    if (n.rfind("Att_RarityWeight_", 0) != 0) return 0;
    if (n.find("Legendary") != std::string::npos || n.find("_05") != std::string::npos) return 5;
    if (n.find("VeryRare") != std::string::npos || n.find("_04") != std::string::npos) return 4;
    if (n.find("Uncommon") != std::string::npos || n.find("_02") != std::string::npos) return 2;
    if (n.find("Rare") != std::string::npos || n.find("_03") != std::string::npos) return 3;
    if (n.find("Common") != std::string::npos || n.find("_01") != std::string::npos) return 1;
    return 0;
}

void ApplyPool(void* pool, int mask) {
    ue::Prop p = ue::FindProp(ue::ClassOf(pool), "BalancedItems");
    if (!p) return;
    auto arr = ue::Read<ue::TArray<uint8_t>>(pool, p);
    if (!arr.data || arr.num <= 0 || arr.num > 2000) return;
    struct Entry {
        uint8_t* e;
        int rarity;
    };
    std::vector<Entry> list;
    for (int i = 0; i < arr.num; ++i) {
        uint8_t* e = arr.data + (size_t)i * 0x70;
        void* attr = ue::At<void*>(e, 0x58);
        if (!attr || !ue::Valid(attr)) continue;
        int r = RarityOf(attr);
        if (r) list.push_back({e, r});
    }
    if (list.empty()) return;
    bool anyKept = std::any_of(list.begin(), list.end(), [&](const Entry& x) { return (mask & (1 << x.rarity)) != 0; });
    if (!anyKept) return;
    for (const Entry& x : list) {
        if (mask & (1 << x.rarity)) continue;
        uint8_t* addr = x.e + 0x68;
        float v = ue::At<float>(addr, 0);
        weightSaves.push_back({ue::MakeWeak(pool), addr, v});
        ue::Put<float>(addr, 0, 0.0f);
    }
}

void RestoreWeights() {
    for (WeightSave& w : weightSaves)
        if (ue::WeakGet(w.pool)) ue::Put<float>(w.addr, 0, w.value);
    weightSaves.clear();
    processedPools.clear();
    poolCursor = expCursor = 0;
    appliedMask = 0;
}

void StepPools(const std::vector<void*>& list, size_t& cursor, int mask, int budget) {
    if (cursor > list.size()) cursor = 0;
    size_t end = std::min(list.size(), cursor + (size_t)budget);
    for (size_t i = cursor; i < end; ++i) {
        void* pool = list[i];
        if (!ue::Alive(pool) || processedPools.count(pool)) continue;
        processedPools.insert(pool);
        ApplyPool(pool, mask);
    }
    cursor = end >= list.size() ? 0 : end;
}

void Drops(bool legendary, bool filter) {
    int mask = 0;
    if (legendary) mask = 1 << 5;
    else if (filter) {
        if (On("drop_common")) mask |= 1 << 1;
        if (On("drop_uncommon")) mask |= 1 << 2;
        if (On("drop_rare")) mask |= 1 << 3;
        if (On("drop_epic")) mask |= 1 << 4;
        if (On("drop_legendary")) mask |= 1 << 5;
        if (mask == 0) mask = 1 << 5;
    }
    if (mask != appliedMask && appliedMask != 0) RestoreWeights();
    if (mask == 0) return;
    appliedMask = mask;
    StepPools(Objects(Scan::ItemPools), poolCursor, mask, 500);
    StepPools(Objects(Scan::PoolExpansions), expCursor, mask, 200);
}

struct DefSave {
    ue::FWeak def;
    ue::Prop prop;
    bool value = false;
};
std::vector<DefSave> defSaves;

void RestoreDefs() {
    for (DefSave& d : defSaves) {
        void* o = ue::WeakGet(d.def);
        if (o) ue::WriteBool(o, d.prop, d.value);
    }
    defSaves.clear();
}

void FreeSpins(bool want) {
    if (!want) {
        if (!defSaves.empty()) RestoreDefs();
        return;
    }
    void* target = st.pc ? ue::Ptr(st.pc, "CachedUsableTarget") : nullptr;
    void* owner = target ? ue::OuterOf(target) : nullptr;
    if (!owner || !ue::NameContains(ue::ClassOf(owner), "SlotMachine")) return;
    ue::Prop info = ue::FindProp(ue::ClassOf(st.pc), "CachedUsabilityInfo");
    void* infoStruct = Cls("UsabilityInfo");
    if (!info || !infoStruct) return;
    for (const char* m : {"PrimaryUseDef", "PrimaryHoldUseDef", "SecondaryUseDef", "SecondaryHoldUseDef"}) {
        ue::Prop mp = ue::FindProp(infoStruct, m);
        if (!mp) continue;
        void* def = ue::At<void*>(st.pc, info.offset + mp.offset);
        if (!ue::Alive(def)) continue;
        ue::Prop cost = ue::FindProp(ue::ClassOf(def), "bCostsToUse");
        if (!cost || !ue::ReadBool(def, cost)) continue;
        bool known = false;
        for (DefSave& d : defSaves)
            if (ue::WeakGet(d.def) == def) known = true;
        if (known) continue;
        defSaves.push_back({ue::MakeWeak(def), cost, true});
        ue::WriteBool(def, cost, false);
        LOG("slot machine use made free: %s", ue::Name(def).c_str());
    }
}

void Reroll() {
    void* lib = Lib("MayhemModeFunctionLibrary");
    ue::Call lv(lib, "GetMayhemLevel");
    lv.Set<void*>("WorldContextObject", st.world);
    int level = lv.Run() ? lv.Get<int32_t>("ReturnValue") : -1;
    if (level <= 0) {
        Note(level == 0 ? "Turn on Mayhem mode first, then reroll" : "Could not read the Mayhem level");
        return;
    }
    std::random_device rd;
    int seed = (int)(rd() & 0x7FFFFFFF);
    ue::Call c(lib, "TryMapReloadForMayhemActivityOrLevelChange");
    c.Set<void*>("WorldContextObject", st.world).Set<void*>("OptionalInstigator", st.pc).Set<int32_t>("MayhemLevel", level).Set<int32_t>("RandomSeed", seed);
    if (!c.Run()) {
        Note("The game did not allow a reroll right now");
        return;
    }
    uint8_t r = c.Get<uint8_t>("ReturnValue");
    LOG("mayhem reroll: level %d seed %d result %u", level, seed, r);
    switch (r) {
    case 0: Note("Rerolling Mayhem modifiers, the map reloads"); break;
    case 2: Note("Close the game's menu first, then reroll"); break;
    case 3: Note("Wait until nobody is down, then reroll"); break;
    case 5: Note("Get out of your vehicle first, then reroll"); break;
    default: Note("The game did not allow a reroll right now"); break;
    }
}

}

void LootInit() {
    bool pools = Has("ItemPoolData", "BalancedItems") && Has("BalancedInventoryInfo", "Weight");
    Ready("legendary_drops", pools);
    Ready("drop_filter", pools);
    Ready("free_spins", Has("OakPlayerController", "CachedUsabilityInfo") && Has("UsableTypeDefinition", "bCostsToUse"));
    Ready("mayhem_reroll", HasFunc("MayhemModeFunctionLibrary", "TryMapReloadForMayhemActivityOrLevelChange") &&
                               HasFunc("MayhemModeFunctionLibrary", "GetMayhemLevel"));
}

void LootTick() {
    Drops(On("legendary_drops") && st.alone, On("drop_filter") && st.alone);
    FreeSpins(On("free_spins") && st.alone);
    if (Pressed("mayhem_reroll")) {
        if (st.alone) Reroll();
        else Note("The Mayhem reroller only works when you play alone");
    }
}

void LootOff() {
    if (!weightSaves.empty() || appliedMask) RestoreWeights();
    RestoreDefs();
}

}
