#include "game/giver_icons.h"
#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace bl3 {

namespace {

namespace gi = giver_icons;

constexpr int kMaxPerRequest = 50;
constexpr double kPickupWait = 2.5;
constexpr double kPackWait = 0.4;
constexpr double kWatchLimit = 6.0;
constexpr double kScanEvery = 0.1;
constexpr float kNearFlat = 220.0f;
constexpr float kTightFlat = 80.0f;

struct GiverItem {
    std::vector<uint8_t> asset;
    std::string id, name, derived, learned, category, path, assetName;
    int type = gi::Other;
    int tier = 0;
    bool tierTried = false;
    bool top = false;
};

struct Created {
    void* ret = nullptr;
    void* pickup = nullptr;
    void* inv = nullptr;
    void* comp = nullptr;
};

struct Job {
    size_t item = 0;
    int asked = 1, want = 1, next = 0, dropped = 0, packed = 0, unsure = 0;
    bool started = false, over = false, quiet = false, car = false, waiting = false, ghost = false, deepUsed = false, others = false;
    std::vector<ue::FVector> points;
    std::vector<void*> before;
    std::string shown, why;
    unsigned tried = 0, dead = 0;
    int method = -1;
    double deadline = 0, scanAt = -1;
    ue::FWeak ret, bal, category;
};

struct Watch {
    ue::FWeak pickup;
    ue::FVector at;
    double since = 0;
};

struct Ctx {
    void* bal = nullptr;
    void* data = nullptr;
    std::vector<uint8_t> init;
};

enum class Check { Wait, Good, Broken, NoPickup, Nothing, Gone };
enum class Pack { Added, Unsure, Full, NoPack, NoKind, Refused, Fault };
enum class Fire { Made, Empty, Failed };

std::vector<GiverItem> list;
std::unordered_map<std::string, size_t> byId;
bool built = false, building = false, failed = false;
double worldSince = 0;
ue::FWeak lastWorld;
std::mutex queueLock;
std::vector<std::pair<std::string, int>> queue;
std::deque<Job> jobs;
std::vector<Watch> watches;
std::vector<ue::FWeak> made;
int bestDrop = -1, bestPack = -1;
unsigned sessionDead = 0;
size_t rarityCursor = 0;
double rarityScanAt = -100;
std::unordered_map<std::string, size_t> byAsset;
int rarityFresh = 0;
bool namesFresh = false;
double picturesAt = -100;

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

int TypeFor(const std::string& category) {
    static const std::pair<const char*, int> types[] = {{"Pistols", gi::Pistol},        {"SMGs", gi::Smg},
                                                        {"Assault rifles", gi::Rifle},  {"Shotguns", gi::Shotgun},
                                                        {"Sniper rifles", gi::Sniper},  {"Heavy weapons", gi::Launcher},
                                                        {"Shields", gi::Shield},        {"Grenades", gi::Grenade},
                                                        {"Class mods", gi::ClassMod},   {"Artifacts", gi::Artifact}};
    for (const auto& t : types)
        if (category == t.first) return t.second;
    return gi::Other;
}

std::string SafeId(const std::string& s) {
    std::string out;
    for (char c : s) out.push_back(std::isalnum((unsigned char)c) || c == '_' || c == '-' ? c : '_');
    return out.empty() ? std::string("item") : out;
}

uint32_t Hash(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= (uint8_t)std::tolower((unsigned char)c);
        h *= 16777619u;
    }
    return h;
}

void AssignIds() {
    std::unordered_map<std::string, int> count;
    for (const GiverItem& g : list) ++count[SafeId(g.assetName)];
    std::unordered_set<std::string> used;
    for (GiverItem& g : list) {
        std::string id = SafeId(g.assetName);
        if (count[id] > 1) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "_%08x", (unsigned)Hash(g.path));
            id += buf;
        }
        while (used.count(id) || id == "ammo") id += "_";
        used.insert(id);
        g.id = id;
    }
}

void Index() {
    byId.clear();
    for (size_t i = 0; i < list.size(); ++i) byId[list[i].id] = i;
}

std::vector<std::string> Words(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == ' ') {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::string Join(const std::vector<std::string>& w, size_t from) {
    std::string out;
    for (size_t i = from; i < w.size(); ++i) out += (out.empty() ? "" : " ") + w[i];
    return out;
}

std::string CleanName(const std::string& shown, const std::string& derived) {
    std::vector<std::string> w = Words(shown);
    std::string all = Squash(shown), key = Squash(derived);
    size_t at = key.size() >= 3 ? all.find(key) : std::string::npos;
    if (w.empty() || at == std::string::npos || at == 0) return Join(w, 0);
    size_t pos = 0, keep = 0;
    for (size_t i = 0; i < w.size(); ++i) {
        size_t len = Squash(w[i]).size();
        if (pos + len > at) break;
        pos += len;
        keep = i + 1;
    }
    return keep < w.size() ? Join(w, keep) : Join(w, 0);
}

std::string Merge(const std::string& old, const std::string& now) {
    if (old.empty() || Lower(old) == Lower(now)) return now;
    std::vector<std::string> a = Words(old), b = Words(now);
    size_t n = 0;
    while (n < a.size() && n < b.size() && Lower(a[a.size() - 1 - n]) == Lower(b[b.size() - 1 - n])) ++n;
    if (n == 0) return now;
    return Join(b, b.size() - n);
}

void* Registry() {
    ue::Call g(Lib("AssetRegistryHelpers"), "GetAssetRegistry");
    if (!g.Run()) return nullptr;
    const uint8_t* raw = g.Raw("ReturnValue");
    void* reg = raw ? ue::At<void*>(raw, 0) : nullptr;
    return ue::Alive(reg) ? reg : nullptr;
}

void* LoadBalance(const GiverItem& it) {
    ue::Call get(Lib("AssetRegistryHelpers"), "GetAsset");
    get.SetRaw("InAssetData", it.asset.data(), (int)it.asset.size());
    void* bal = get.Run() ? get.Get<void*>("ReturnValue") : nullptr;
    return ue::Alive(bal) ? bal : nullptr;
}

int TierOf(void* rarity) {
    if (!rarity) return 0;
    if (void* gg = GameplayGlobals()) {
        static const char* fields[] = {"InventoryRarityCommon", "InventoryRarityUncommon", "InventoryRarityRare", "InventoryRarityVeryRare",
                                       "InventoryRarityLegendary"};
        for (int i = 0; i < 5; ++i)
            if (ue::Ptr(gg, fields[i]) == rarity) return i + 1;
    }
    std::string n = Lower(ue::Name(rarity));
    if (n.find("legendary") != std::string::npos || n.find("_05") != std::string::npos) return 5;
    if (n.find("veryrare") != std::string::npos || n.find("epic") != std::string::npos || n.find("_04") != std::string::npos) return 4;
    if (n.find("uncommon") != std::string::npos || n.find("_02") != std::string::npos) return 2;
    if (n.find("rare") != std::string::npos || n.find("_03") != std::string::npos) return 3;
    if (n.find("common") != std::string::npos || n.find("_01") != std::string::npos) return 1;
    return 0;
}

void LearnTier(GiverItem& it, void* bal) {
    it.tierTried = true;
    int t = TierOf(ue::Ptr(bal, "RarityData"));
    if (t > 0 && t != it.tier) {
        it.tier = t;
        ++rarityFresh;
    }
}

void AskPictures() {
    std::vector<gi::Spec> specs;
    specs.reserve(list.size() + 1);
    for (const GiverItem& g : list) specs.push_back({g.id, g.type, g.tier, g.learned});
    specs.push_back({"ammo", gi::Ammo, 0, std::string()});
    gi::Request(std::move(specs), gi::Dir());
    picturesAt = st.time;
    rarityFresh = 0;
    namesFresh = false;
}

void ResolveRarities() {
    if (!built || list.empty()) return;
    const std::vector<void*>& loaded = Objects(Scan::Balances);
    if (st.time - rarityScanAt > 5.0 && loaded.size() != rarityCursor) {
        rarityScanAt = st.time;
        rarityCursor = loaded.size();
        if (byAsset.size() != list.size()) {
            byAsset.clear();
            for (size_t i = 0; i < list.size(); ++i) byAsset[list[i].assetName] = i;
        }
        for (void* b : loaded) {
            if (!ue::Alive(b)) continue;
            auto f = byAsset.find(ue::Name(b));
            if (f == byAsset.end() || f->second >= list.size()) continue;
            GiverItem& it = list[f->second];
            if (!it.tierTried) LearnTier(it, b);
        }
    }
    if ((rarityFresh <= 0 && !namesFresh) || st.time - picturesAt < 4.0) return;
    AskPictures();
}

void Publish() {
    std::vector<core::Item> items;
    items.reserve(list.size() + 1);
    for (const GiverItem& g : list) {
        core::Item it;
        it.id = g.id;
        it.name = g.name;
        it.category = g.top ? "Top picks" : g.category;
        items.push_back(std::move(it));
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
            it.derived = it.name;
            it.type = TypeFor(it.category);
            std::string sq = Squash(name);
            for (const char* t : kTop)
                if (sq.find(t) != std::string::npos) it.top = true;
            list.push_back(std::move(it));
        }
    }
    AssignIds();
    std::unordered_map<std::string, gi::Known> known = gi::Remembered(gi::Dir());
    int knownTiers = 0, knownNames = 0;
    for (GiverItem& g : list) {
        auto k = known.find(g.id);
        if (k == known.end()) continue;
        if (k->second.tier > 0) {
            g.tier = k->second.tier;
            ++knownTiers;
        }
        if (!k->second.name.empty()) {
            g.learned = k->second.name;
            g.name = g.learned;
            ++knownNames;
        }
    }
    for (GiverItem& g : list)
        if (g.top && g.tier == 0) g.tier = 5;
    std::sort(list.begin(), list.end(), [](const GiverItem& a, const GiverItem& b) {
        if (a.top != b.top) return a.top;
        if (a.category != b.category) return a.category < b.category;
        return a.name < b.name;
    });
    Index();
    rarityCursor = 0;
    rarityFresh = 0;
    namesFresh = false;
    byAsset.clear();
    LOG("giver: %zu items in the list, %d with a known rarity, %d with a known name", list.size(), knownTiers, knownNames);
    built = true;
    building = false;
    if (list.empty()) {
        failed = true;
        Note("The Giver found no items in this version of the game");
    }
    Publish();
    if (!list.empty()) AskPictures();
}

int PlayerLevel() {
    void* b = st.pawn ? ue::Ptr(st.pawn, "PlayerBalanceComponent") : nullptr;
    ue::Call c(b, "GetExperienceLevel");
    int l = c.Run() ? c.Get<int32_t>("ReturnValue") : 1;
    return std::clamp(l, 1, 80);
}

ue::FVector Flat(float yawDeg) {
    float y = yawDeg * 3.14159265f / 180.0f;
    return {std::cos(y), std::sin(y), 0.0f};
}

ue::FVector Toward(const ue::FVector& from, const ue::FVector& to, float back) {
    ue::FVector d = Sub(to, from);
    float l = Len(d);
    if (l < 1.0f) return from;
    return Add(from, Mul(d, std::max(0.0f, l - back) / l));
}

bool Trace(const ue::FVector& from, const ue::FVector& to, ue::FVector& hit) {
    static void* hitStruct = nullptr;
    if (!hitStruct) hitStruct = Cls("HitResult");
    ue::Prop impact = hitStruct ? ue::FindProp(hitStruct, "ImpactPoint") : ue::Prop{};
    if (!impact) return false;
    void* ignore[2] = {st.pawn, st.vehicle};
    ue::TArray<void*> ignoreList{ignore, st.vehicle ? 2 : 1, 2};
    ue::Call c(Lib("KismetSystemLibrary"), "LineTraceSingle");
    c.Set<void*>("WorldContextObject", st.world).Set("Start", from).Set("End", to).Set<uint8_t>("TraceChannel", 0).SetBool("bTraceComplex", false);
    c.Set("ActorsToIgnore", ignoreList).Set<uint8_t>("DrawDebugType", 0).SetBool("bIgnoreSelf", true);
    if (!c.Run() || !c.GetBool("ReturnValue")) return false;
    const uint8_t* out = c.Raw("OutHit");
    if (!out) return false;
    std::memcpy(&hit, out + impact.offset, sizeof(hit));
    return true;
}

std::vector<ue::FVector> DropPoints(int n, bool& car) {
    car = st.vehicle != nullptr;
    ue::FVector origin, anchor, rows, cols;
    float baseZ = 0, half = 0, feet = 0;
    if (car) {
        origin = Location(st.vehicle);
        ue::FVector extent{180.0f, 180.0f, 80.0f};
        ue::Call b(st.vehicle, "GetActorBounds");
        b.SetBool("bOnlyCollidingComponents", true);
        if (b.Run()) {
            ue::FVector o = b.Get<ue::FVector>("Origin"), e = b.Get<ue::FVector>("BoxExtent");
            if (Len(e) > 20.0f && Len(e) < 3000.0f) {
                origin = o;
                extent = e;
            }
        }
        float yaw = Rotation(st.vehicle).yaw;
        half = std::max(extent.x, extent.y);
        rows = Flat(yaw + 90.0f);
        cols = Flat(yaw);
        ue::FVector hit;
        if (Trace(origin, Add(origin, Mul(rows, half + 160.0f)), hit)) rows = Mul(rows, -1.0f);
        anchor = Add(origin, Mul(rows, half + 110.0f));
        baseZ = origin.z + extent.z;
        feet = origin.z - extent.z;
    } else {
        origin = Location(st.pawn);
        float yaw = ControlRotation().yaw;
        rows = Flat(yaw);
        cols = Flat(yaw + 90.0f);
        anchor = Add(origin, Mul(rows, 175.0f));
        baseZ = origin.z + 40.0f;
        feet = origin.z - 90.0f;
    }
    ue::FVector from{origin.x, origin.y, baseZ};
    int perRow = std::clamp(n, 1, 5);
    std::vector<ue::FVector> out;
    for (int k = 0; k < n; ++k) {
        int row = k / perRow, col = k % perRow;
        int inRow = std::min(perRow, n - row * perRow);
        float lateral = ((float)col - (float)(inRow - 1) * 0.5f) * 45.0f;
        ue::FVector p = Add(Add(anchor, Mul(rows, (float)row * 45.0f)), Mul(cols, lateral));
        p.z = baseZ;
        ue::FVector hit, ground;
        if (Trace(from, p, hit)) p = Toward(from, hit, 35.0f);
        bool floor = TraceDown(Add(p, ue::FVector{0.0f, 0.0f, 60.0f}), 1500.0f, ground) && ground.z > feet - 350.0f;
        if (!floor) {
            p = Add(from, Mul(rows, car ? half + 60.0f : 60.0f));
            p.z = baseZ;
        } else if (car) {
            p.z = ground.z + 120.0f;
        } else if (ground.z > p.z - 50.0f) {
            p.z = ground.z + 60.0f;
        }
        out.push_back(p);
    }
    return out;
}

void* PickupClass() {
    static void* cls = nullptr;
    if (!cls) cls = Cls("InventoryItemPickup");
    return cls;
}

void* BalanceStateOf(void* actor) {
    if (!actor) return nullptr;
    ue::Call c(Lib("InventoryBlueprintLibrary"), "GetInventoryBalanceState");
    c.Set<void*>("Inventory", actor);
    void* r = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    return ue::Alive(r) ? r : nullptr;
}

void* PickupComp(void* pickup) {
    ue::Call c(pickup, "GetInventoryBalanceStateComponent");
    void* comp = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    if (ue::Alive(comp)) return comp;
    comp = ue::Ptr(pickup, "CachedInventoryBalanceComponent");
    return comp ? comp : BalanceStateOf(ue::Ptr(pickup, "PickupActor"));
}

bool PickupReady(void* pickup) {
    ue::Call c(pickup, "IsPickupInitialized");
    return c.Run() && c.GetBool("ReturnValue");
}

bool Holds(void* pickup, void* inv) {
    return ue::Alive(pickup) && (ue::Ptr(pickup, "PickupActor") == inv || ue::Ptr(pickup, "ReplicatedPickupActor") == inv);
}

bool IsMade(void* pickup) {
    for (const ue::FWeak& w : made)
        if (ue::WeakGet(w) == pickup) return true;
    return false;
}

void EachPickup(Job& j, bool deep, const std::function<bool(void*)>& fn) {
    std::unordered_set<void*> seen;
    auto visit = [&](void* p) { return !p || !seen.insert(p).second || !ue::Alive(p) || fn(p); };
    for (void* p : Objects(Scan::Pickups))
        if (!visit(p)) return;
    ue::Prop pl = st.gameState ? ue::FindProp(ue::ClassOf(st.gameState), "PickupList") : ue::Prop{};
    if (pl) {
        auto arr = ue::Read<ue::TArray<void*>>(st.gameState, pl);
        if (arr.data && arr.num > 0 && arr.num < 50000)
            for (int i = arr.num - 1; i >= 0; --i) {
                void* p = nullptr;
                ue::SafeRead(arr.data + i, p);
                if (!visit(p)) return;
            }
    }
    if (!deep || j.deepUsed || !PickupClass()) return;
    j.deepUsed = true;
    ue::ForEachOf(PickupClass(), [&](void* p) { return visit(p); });
}

bool Near(void* pickup, const ue::FVector& at) {
    ue::FVector l = Location(pickup);
    float dx = l.x - at.x, dy = l.y - at.y;
    return dx * dx + dy * dy <= kNearFlat * kNearFlat && l.z > at.z - 700.0f && l.z < at.z + 150.0f;
}

bool Ours(void* pickup, void* bal, void* category, const ue::FVector& at) {
    void* comp = PickupComp(pickup);
    void* owner = comp ? ue::Ptr(comp, "InventoryBalanceData") : nullptr;
    if (owner) return bal && owner == bal;
    void* cat = ue::Ptr(pickup, "PickupCategory");
    if (!cat || !category || cat != category) return false;
    ue::FVector l = Location(pickup);
    float dx = l.x - at.x, dy = l.y - at.y;
    return dx * dx + dy * dy <= kTightFlat * kTightFlat;
}

bool ScanDue(Job& j, bool late) {
    if (!late && st.time - j.scanAt < kScanEvery) return false;
    j.scanAt = st.time;
    return true;
}

bool Fresh(const Job& j, void* pickup) {
    return std::find(j.before.begin(), j.before.end(), pickup) == j.before.end() && !IsMade(pickup);
}

std::vector<void*> Snapshot(Job& j, const ue::FVector& at) {
    std::vector<void*> out;
    EachPickup(j, false, [&](void* p) {
        if (Near(p, at)) out.push_back(p);
        return true;
    });
    return out;
}

void* PickupHolding(void* inv, Job& j, bool deep, bool scan) {
    if (!inv || !PickupClass()) return nullptr;
    void* owner = ue::Ptr(inv, "Owner");
    if (owner && ue::IsA(owner, PickupClass()) && Holds(owner, inv)) return owner;
    if (!scan) return nullptr;
    void* found = nullptr;
    EachPickup(j, deep, [&](void* p) {
        if (!Holds(p, inv)) return true;
        found = p;
        return false;
    });
    return found;
}

Created Resolve(void* ret, Job& j, bool deep, bool scan) {
    Created k;
    k.ret = ret;
    if (!ret) return k;
    if (PickupClass() && ue::IsA(ret, PickupClass())) {
        k.pickup = ret;
        k.inv = ue::Ptr(ret, "PickupActor");
        k.comp = PickupComp(ret);
    } else {
        k.inv = ret;
        k.comp = BalanceStateOf(ret);
        k.pickup = PickupHolding(ret, j, deep, scan);
    }
    return k;
}

int PartCount(void* comp) {
    if (!comp) return 0;
    ue::Prop p = ue::FindProp(ue::ClassOf(comp), "PartList");
    if (!p) return 0;
    auto a = ue::Read<ue::TArray<void*>>(comp, p);
    return a.data && a.num > 0 && a.num < 1000 ? a.num : 0;
}

std::string ItemName(void* comp) {
    if (!comp) return {};
    ue::Prop p = ue::FindProp(ue::ClassOf(comp), "DisplayName");
    std::string s = p ? ue::TextToString((const uint8_t*)comp + p.offset) : std::string();
    if (s.empty()) {
        ue::Call c(comp, "GetDisplayName");
        if (c.Run()) s = ue::TextToString(c.Raw("ReturnValue"));
    }
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (!s.empty() && std::isspace((unsigned char)s.back())) s.pop_back();
    while (!s.empty() && std::isspace((unsigned char)s.front())) s.erase(s.begin());
    return s.size() > 80 ? std::string() : s;
}

void Learn(GiverItem& it, const std::string& shown) {
    std::string clean = Merge(it.learned, CleanName(shown, it.derived));
    if (clean.empty() || clean == it.learned) return;
    it.learned = clean;
    namesFresh = true;
    if (it.name == clean) return;
    LOG("giver: %s is called %s in the game", it.assetName.c_str(), clean.c_str());
    it.name = clean;
    Publish();
}

void Destroy(const Created& k) {
    for (void* a : {k.pickup, k.inv, k.ret})
        if (a && ue::Alive(a)) DestroyActor(a);
}

void Sweep(Job& j, void* keep) {
    if (j.next >= (int)j.points.size()) return;
    void* bal = ue::WeakGet(j.bal);
    void* category = ue::WeakGet(j.category);
    ue::FVector at = j.points[(size_t)j.next];
    std::vector<void*> doomed;
    EachPickup(j, true, [&](void* p) {
        if (p != keep && Near(p, at) && Ours(p, bal, category, at) && Fresh(j, p)) doomed.push_back(p);
        return true;
    });
    for (void* p : doomed) {
        void* inv = ue::Ptr(p, "PickupActor");
        DestroyActor(p);
        if (inv && ue::Alive(inv)) DestroyActor(inv);
    }
    if (!doomed.empty()) LOG("giver: removed %zu leftover pickups near the drop point", doomed.size());
}

void* BackpackOf() { return st.pawn ? ue::Ptr(st.pawn, "OakCharacterInventory") : nullptr; }

bool BackpackFull() {
    ue::Call c(BackpackOf(), "IsInventoryFull");
    return c.Run() && c.GetBool("ReturnValue");
}

Pack Backpack(void* inv, void* data) {
    void* pack = BackpackOf();
    if (!pack) return Pack::NoPack;
    void* category = data ? ue::Ptr(data, "InventoryCategory") : nullptr;
    if (!category) return Pack::NoKind;
    if (!ue::Alive(inv)) return Pack::Refused;
    if (BackpackFull()) return Pack::Full;
    ue::Call add(pack, "AddExternalItem");
    if (!add.Ok()) return Pack::Refused;
    add.Set<void*>("ItemCategory", category).Set<void*>("ActorToAdd", inv).Set<int32_t>("Quantity", 1);
    add.SetBool("AddedByPickup", false).SetBool("bAutoEquip", false).SetBool("bConditionalDestroyActor", false);
    add.Set<int32_t>("DesiredSlot", -1).SetBool("bBypassInventoryFull", false);
    bool added = add.Run();
    int handle = added ? add.Get<int32_t>("ReturnValue") : 0;
    ue::Call has(pack, "HasActorInList");
    has.Set<void*>("ItemActor", inv);
    bool checked = has.Run();
    bool in = checked && has.GetBool("ReturnValue");
    if (!added) {
        LOG("giver: adding to the backpack failed inside the game, %s",
            in ? "but the item is in it" : checked ? "the item is not in it, removing it" : "the backpack could not be checked, left the item alone");
        return in ? Pack::Added : checked ? Pack::Refused : Pack::Fault;
    }
    LOG("giver: backpack add handle %d, %s", handle, !checked ? "could not confirm it, kept the item" : in ? "in the list" : "not in the list, kept the item");
    return in ? Pack::Added : Pack::Unsure;
}

const char* PackWhy(Pack r) {
    switch (r) {
    case Pack::Full: return "your backpack is full";
    case Pack::NoPack: return "your backpack could not be found";
    case Pack::NoKind: return "the game could not tell what kind of item it is";
    default: return "the game would not put it in your backpack";
    }
}

bool WantsPickup(int m) { return m < 2; }

const char* FuncOf(int m) { return m % 2 == 0 ? "CreateInventory" : "BuildInventory"; }

int NextMethod(Job& j) {
    if (!st.alone) j.others = true;
    int order[6];
    int n = 0;
    auto push = [&](int m) {
        if (m >= 0 && std::find(order, order + n, m) == order + n) order[n++] = m;
    };
    if (st.alone) {
        push(bestDrop);
        push(0);
        push(1);
    }
    push(bestPack);
    push(2);
    push(3);
    for (int i = 0; i < n; ++i)
        if (!((j.tried | j.dead | sessionDead) & (1u << order[i]))) return order[i];
    return -1;
}

bool MakeInit(const GiverItem& it, Ctx& c, std::string& why) {
    c.bal = LoadBalance(it);
    if (!c.bal) {
        why = "the game could not load it";
        return false;
    }
    c.data = ue::Ptr(c.bal, "InventoryData");
    if (!c.data) {
        why = "the game could not read this item";
        return false;
    }
    void* initStruct = Cls("InventoryBalanceStateInitializationData");
    int size = ue::StructSize(initStruct);
    ue::Prop stage = ue::FindProp(initStruct, "GameStage");
    ue::Prop data = ue::FindProp(initStruct, "InventoryData");
    ue::Prop bal = ue::FindProp(initStruct, "InventoryBalanceData");
    ue::Prop man = ue::FindProp(initStruct, "ManufacturerData");
    if (size < 0x40 || size > 0x200 || !stage || !data || !bal || bal.offset + 8 > size || data.offset + 8 > size) {
        why = "this version of the game has no way to build it";
        return false;
    }
    void* manufacturer = nullptr;
    void* manStruct = Cls("InventoryManufacturerBalanceData");
    ue::Prop mp = ue::FindProp(ue::ClassOf(c.bal), "Manufacturers");
    ue::Prop md = manStruct ? ue::FindProp(manStruct, "ManufacturerData") : ue::Prop{};
    auto mans = ue::Read<ue::TArray<uint8_t>>(c.bal, mp);
    if (mp && mans.data && mans.num > 0 && mans.num < 64) {
        void* m = ue::At<void*>(mans.data, md ? md.offset : 0);
        if (ue::Valid(m)) manufacturer = m;
    }
    c.init.assign((size_t)size, 0);
    ue::Put<int32_t>(c.init.data(), stage.offset, PlayerLevel());
    ue::Put<void*>(c.init.data(), data.offset, c.data);
    ue::Put<void*>(c.init.data(), bal.offset, c.bal);
    if (manufacturer && man && man.offset + 8 <= size) ue::Put<void*>(c.init.data(), man.offset, manufacturer);
    return true;
}

Fire Launch(Job& j, int m, const Ctx& c) {
    const GiverItem& it = list[j.item];
    ue::Call call(Lib("InventoryBlueprintLibrary"), FuncOf(m));
    if (!call.Ok()) return Fire::Failed;
    ue::FVector at = j.points[(size_t)j.next];
    call.Set<void*>("WorldContextObject", st.world).SetBool("bCreatePickup", WantsPickup(m)).Set("SpawnLocation", at);
    call.SetRaw("InitData", c.init.data(), (int)c.init.size());
    bool ran = call.Run();
    void* ret = ran ? call.Get<void*>("ReturnValue") : nullptr;
    j.method = m;
    if (!ue::Alive(ret)) {
        LOG("giver: %s%s %s for %s", FuncOf(m), WantsPickup(m) ? "" : " without a pickup", ran ? "gave nothing" : "failed inside the game",
            it.assetName.c_str());
        return Fire::Empty;
    }
    LOG("giver: %s%s made a %s for %s", FuncOf(m), WantsPickup(m) ? "" : " without a pickup", ue::Name(ue::ClassOf(ret)).c_str(),
        it.assetName.c_str());
    j.ret = ue::MakeWeak(ret);
    j.waiting = true;
    j.ghost = false;
    j.deadline = st.time + (WantsPickup(m) ? kPickupWait : kPackWait);
    return Fire::Made;
}

Check Evaluate(Job& j, Created& k) {
    bool late = st.time >= j.deadline;
    void* ret = ue::WeakGet(j.ret);
    if (!ret) return Check::Gone;
    bool pick = WantsPickup(j.method);
    k = Resolve(ret, j, late && pick, ScanDue(j, late));
    int parts = PartCount(k.comp);
    if (!pick) return parts > 0 ? Check::Good : late ? Check::Broken : Check::Wait;
    if (k.pickup && parts > 0) return Check::Good;
    if (!late) return k.pickup && PickupReady(k.pickup) ? Check::Broken : Check::Wait;
    return parts > 0 ? Check::NoPickup : Check::Broken;
}

void* FindOurs(Job& j, bool deep) {
    void* bal = ue::WeakGet(j.bal);
    if (!bal || j.next >= (int)j.points.size()) return nullptr;
    ue::FVector at = j.points[(size_t)j.next];
    void* found = nullptr;
    EachPickup(j, deep, [&](void* p) {
        if (!Near(p, at)) return true;
        void* comp = PickupComp(p);
        if (!comp || ue::Ptr(comp, "InventoryBalanceData") != bal || PartCount(comp) <= 0 || !Fresh(j, p)) return true;
        found = p;
        return false;
    });
    return found;
}

Check Ghost(Job& j, Created& k) {
    bool late = st.time >= j.deadline;
    if (!ScanDue(j, late)) return Check::Wait;
    if (void* found = FindOurs(j, late)) {
        k = Resolve(found, j, false, false);
        return Check::Good;
    }
    return late ? Check::Nothing : Check::Wait;
}

void Finish(Job& j, const Created& k) {
    GiverItem& it = list[j.item];
    std::string shown = ItemName(k.comp);
    if (!shown.empty()) {
        j.shown = shown;
        Learn(it, shown);
    }
    ue::FVector at = j.points[(size_t)j.next];
    if (k.pickup && st.alone) {
        ++j.dropped;
        if (WantsPickup(j.method)) bestDrop = j.method;
        watches.push_back({ue::MakeWeak(k.pickup), at, st.time});
        if (made.size() > 200)
            made.erase(std::remove_if(made.begin(), made.end(), [](const ue::FWeak& w) { return !ue::WeakGet(w); }), made.end());
        made.push_back(ue::MakeWeak(k.pickup));
        LOG("giver: dropped %s with %d parts at %.0f %.0f %.0f", it.assetName.c_str(), PartCount(k.comp), at.x, at.y, at.z);
    } else {
        if (k.pickup) {
            j.others = true;
            Destroy(k);
            LOG("giver: the game dropped %s on the ground while other players are in the game, removed it", it.assetName.c_str());
            j.why = "the game dropped it on the ground while other players are in your game, so it was removed";
            j.over = true;
            return;
        }
        void* inv = k.inv ? k.inv : k.ret;
        void* data = k.comp ? ue::Ptr(k.comp, "InventoryData") : nullptr;
        if (!data) {
            void* bal = LoadBalance(it);
            data = bal ? ue::Ptr(bal, "InventoryData") : nullptr;
        }
        Pack r = Backpack(inv, data);
        if (r != Pack::Added && r != Pack::Unsure) {
            if (r != Pack::Fault) Destroy(k);
            j.why = PackWhy(r);
            j.over = true;
            return;
        }
        ++j.packed;
        if (r == Pack::Unsure) ++j.unsure;
        if (!WantsPickup(j.method)) bestPack = j.method;
        LOG("giver: put %s with %d parts in the backpack", it.assetName.c_str(), PartCount(k.comp));
    }
    ++j.next;
    j.tried = 0;
    j.before.clear();
    if (j.next >= j.want) j.over = true;
}

bool Settle(Job& j, Check r, const Created& k) {
    if (r == Check::Wait) return false;
    bool ghost = j.ghost;
    j.waiting = false;
    j.ghost = false;
    const GiverItem& it = list[j.item];
    if (r == Check::Good) {
        if (ghost) {
            LOG("giver: a pickup for %s showed up after %s gave nothing, kept it", it.assetName.c_str(), FuncOf(j.method));
            Sweep(j, k.pickup);
        }
        Finish(j, k);
        return false;
    }
    if (r == Check::Gone) {
        if (WantsPickup(j.method)) {
            if (void* found = FindOurs(j, true)) {
                LOG("giver: the item made by %s for %s moved into another pickup, kept that one", FuncOf(j.method), it.assetName.c_str());
                Created g = Resolve(found, j, false, false);
                Sweep(j, found);
                Finish(j, g);
                return false;
            }
            Sweep(j, nullptr);
        }
        LOG("giver: the item made by %s for %s vanished right away", FuncOf(j.method), it.assetName.c_str());
        j.why = "the game removed it as soon as it appeared";
        j.over = true;
        return false;
    }
    if (r == Check::Nothing) LOG("giver: no pickup appeared after %s for %s", FuncOf(j.method), it.assetName.c_str());
    else if (r == Check::NoPickup) LOG("giver: %s made %s but no pickup for it, removed it", FuncOf(j.method), it.assetName.c_str());
    else LOG("giver: %s made %s without any parts, removed it", FuncOf(j.method), it.assetName.c_str());
    j.dead |= 1u << j.method;
    if (WantsPickup(j.method) && bestDrop != j.method && r != Check::Broken) sessionDead |= 1u << j.method;
    Destroy(k);
    if (WantsPickup(j.method)) Sweep(j, nullptr);
    return true;
}

bool StartJob(Job& j) {
    GiverItem& it = list[j.item];
    j.shown = it.name;
    if (!st.canCheat || !st.pawn || !st.world) return false;
    Ctx c;
    std::string why;
    if (!MakeInit(it, c, why)) {
        Note("Could not give " + it.name + ": " + why);
        LOG("giver: %s not given, %s", it.assetName.c_str(), why.c_str());
        return false;
    }
    void* dlcSet = ue::Ptr(c.bal, "DlcInventorySetData");
    if (dlcSet && !DlcOwnedPath(ue::Path(dlcSet))) {
        Note(it.name + " comes from paid content the menu can't confirm you own, so it was skipped");
        LOG("giver: skipped %s, DLC set %s", it.assetName.c_str(), ue::Path(dlcSet).c_str());
        return false;
    }
    if (!it.tierTried) LearnTier(it, c.bal);
    j.others = !st.alone;
    j.points = DropPoints(j.want, j.car);
    j.started = true;
    return !j.points.empty();
}

void Step(Job& j) {
    if (!j.started && !StartJob(j)) {
        j.over = true;
        j.quiet = true;
        return;
    }
    if (j.waiting) {
        Created k;
        Check r = j.ghost ? Ghost(j, k) : Evaluate(j, k);
        if (!Settle(j, r, k)) return;
    }
    const GiverItem& it = list[j.item];
    for (;;) {
        int m = NextMethod(j);
        if (m < 0) {
            if (j.why.empty()) j.why = "the game would not build it";
            j.over = true;
            return;
        }
        j.tried |= 1u << m;
        if (!WantsPickup(m) && BackpackFull()) {
            j.why = PackWhy(Pack::Full);
            j.over = true;
            return;
        }
        Ctx c;
        if (!MakeInit(it, c, j.why)) {
            j.over = true;
            return;
        }
        j.bal = ue::MakeWeak(c.bal);
        j.category = ue::MakeWeak(ue::Ptr(c.data, "InventoryCategory"));
        j.before = WantsPickup(m) ? Snapshot(j, j.points[(size_t)j.next]) : std::vector<void*>();
        Fire f = Launch(j, m, c);
        if (f == Fire::Failed) {
            j.dead |= 1u << m;
            continue;
        }
        if (f == Fire::Empty) {
            if (!WantsPickup(m)) {
                j.dead |= 1u << m;
                continue;
            }
            j.ret = {};
            j.waiting = true;
            j.ghost = true;
            j.deadline = st.time + kPickupWait;
            return;
        }
        Created k;
        Check r = Evaluate(j, k);
        if (!Settle(j, r, k)) return;
    }
}

void Report(const Job& j) {
    if (j.quiet) return;
    const GiverItem& it = list[j.item];
    std::string name = j.shown.empty() ? it.name : j.shown;
    int got = j.dropped + j.packed;
    LOG("giver: %s finished, %d dropped, %d in the backpack, %d asked%s%s", it.assetName.c_str(), j.dropped, j.packed, j.asked,
        j.why.empty() ? "" : ", ", j.why.c_str());
    if (got == 0) {
        Note("Could not give " + name + ": " + (j.why.empty() ? std::string("the game would not build it") : j.why));
        return;
    }
    std::string msg;
    if (j.dropped > 0) msg = "Dropped " + std::to_string(j.dropped) + " x " + name + (j.car ? " next to your car" : " in front of you");
    if (j.packed > 0)
        msg += j.dropped > 0 ? " and put " + std::to_string(j.packed) + " in your backpack"
                             : "Put " + std::to_string(j.packed) + " x " + name + " in your backpack";
    std::vector<std::string> extra;
    if (j.asked > j.want) extra.push_back(std::to_string(kMaxPerRequest) + " at a time");
    if (j.packed > 0) extra.push_back(j.others ? "other players are in your game, so nothing is dropped" : "the game would not drop it here");
    if (j.unsure > 0) extra.push_back("the game did not confirm " + (j.unsure == 1 ? std::string("it") : std::to_string(j.unsure) + " of them"));
    if (!extra.empty()) {
        msg += " (";
        for (size_t i = 0; i < extra.size(); ++i) msg += (i ? "; " : "") + extra[i];
        msg += ")";
    }
    if (got < j.want) msg += ". " + std::to_string(j.want - got) + " more could not be made" + (j.why.empty() ? std::string() : ": " + j.why);
    Note(msg);
}

void Abandon(Job& j) {
    if (!j.waiting || j.ghost) return;
    void* ret = ue::WeakGet(j.ret);
    if (!ret) return;
    Created k = Resolve(ret, j, false, false);
    if (PartCount(k.comp) == 0) Destroy(k);
}

void CheckWatches() {
    for (size_t i = 0; i < watches.size();) {
        Watch& w = watches[i];
        double age = st.time - w.since;
        void* p = ue::WeakGet(w.pickup);
        bool done = true;
        if (!p) {
            LOG("giver: a dropped item was picked up or removed");
        } else if (age < 0.5) {
            done = false;
        } else {
            bool ready = PickupReady(p);
            if (!ready && age < kWatchLimit) {
                done = false;
            } else {
                ue::FVector loc = Location(p);
                ue::Prop ap = ue::FindProp(ue::ClassOf(p), "bIsActive");
                ue::Prop sp = ue::FindProp(ue::ClassOf(p), "bPickupSpawnPaused");
                bool active = ap && ue::ReadBool(p, ap);
                bool paused = sp && ue::ReadBool(p, sp);
                float off = Dist(loc, w.at);
                if (off > 1500.0f) {
                    Teleport(p, w.at, Rotation(p));
                    LOG("giver: a dropped item ended up %.0f m from where it was aimed, moved it back", off / 100.0f);
                }
                if (ready && ap && !active) {
                    ue::Call(p, "ActivatePickup").Run();
                    LOG("giver: a dropped item was still inactive, switched it on");
                }
                LOG("giver: dropped item at %.0f %.0f %.0f (aimed %.0f %.0f %.0f) after %.1f s, ready %d, active %d, paused %d", loc.x, loc.y, loc.z,
                    w.at.x, w.at.y, w.at.z, age, ready ? 1 : 0, active ? 1 : 0, paused ? 1 : 0);
            }
        }
        if (done) watches.erase(watches.begin() + (std::ptrdiff_t)i);
        else ++i;
    }
}

}

void GiverInit(core::Model& m) {
    (void)m;
    bool make = HasFunc("InventoryBlueprintLibrary", "CreateInventory") || HasFunc("InventoryBlueprintLibrary", "BuildInventory");
    bool ok = HasFunc("AssetRegistryHelpers", "GetAssetRegistry") && HasFunc("AssetRegistryHelpers", "GetAsset") && make &&
              HasFunc("InventoryBlueprintLibrary", "GetInventoryBalanceState") && Has("InventoryBalanceStateInitializationData", "InventoryBalanceData") &&
              Has("InventoryBalanceStateInitializationData", "GameStage") && Has("InventoryBalanceStateComponent", "PartList");
    Ready("giver", ok);
    {
        std::lock_guard<std::mutex> g(queueLock);
        queue.clear();
    }
    WithModel([](core::Model& mm) {
        mm.categories = {"Top picks", "Pistols", "SMGs", "Assault rifles", "Shotguns", "Sniper rifles", "Heavy weapons", "Shields", "Grenades",
                         "Class mods", "Artifacts", "Ammo"};
        mm.spawn = [](const core::Item& it, int n) {
            std::lock_guard<std::mutex> g(queueLock);
            queue.push_back({it.id, n});
        };
    });
}

void GiverTick() {
    ue::FWeak w = ue::MakeWeak(st.world);
    if (w.index != lastWorld.index || w.serial != lastWorld.serial) {
        if (!jobs.empty()) Note("The Giver request was cancelled");
        lastWorld = w;
        worldSince = st.time;
        jobs.clear();
        watches.clear();
        made.clear();
        sessionDead = 0;
    }
    if (!built && !building && !failed && st.time - worldSince > 6.0 && (DlcKnown() || st.time - worldSince > 20.0)) Build();
    gi::Pump();
    std::vector<std::pair<std::string, int>> todo;
    {
        std::lock_guard<std::mutex> g(queueLock);
        todo.swap(queue);
    }
    for (auto& [id, n] : todo) {
        if (id == "ammo") {
            RefillAllAmmoNow();
            continue;
        }
        auto f = byId.find(id);
        if (f == byId.end() || f->second >= list.size()) {
            LOG("giver: unknown item %s", id.c_str());
            continue;
        }
        if (jobs.size() >= 32) {
            Note("The Giver is still busy, try again in a moment");
            continue;
        }
        Job j;
        j.item = f->second;
        j.asked = std::max(n, 1);
        j.want = std::clamp(n, 1, kMaxPerRequest);
        jobs.push_back(std::move(j));
    }
    for (int steps = 0; steps < 3 && !jobs.empty(); ++steps) {
        Job& j = jobs.front();
        Step(j);
        if (j.over) {
            Report(j);
            jobs.pop_front();
            continue;
        }
        if (j.waiting) break;
    }
    CheckWatches();
    ResolveRarities();
}

void GiverOff() {
    bool cancelled = false;
    {
        std::lock_guard<std::mutex> g(queueLock);
        cancelled = !queue.empty();
        queue.clear();
    }
    if (!jobs.empty()) cancelled = true;
    for (Job& j : jobs) Abandon(j);
    jobs.clear();
    watches.clear();
    made.clear();
    gi::Stop();
    if (cancelled) Note("The Giver request was cancelled");
}

}
