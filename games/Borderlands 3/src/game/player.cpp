#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <array>
#include <cstdlib>
#include <future>

namespace bl3 {

namespace {

const wchar_t* kReason = L"FathomMenu";

void* CoreLib() { return Lib("GbxGameSystemCoreBlueprintLibrary"); }

bool LockCall(const char* func, void* actor) {
    ue::Call c(CoreLib(), func);
    c.Set("Reason", ue::MakeName(kReason)).Set<void*>("Actor", actor);
    return c.Run();
}

bool SetTargetable(void* actor, bool targetable) {
    ue::Call c(CoreLib(), "ResourceLockTargetable");
    c.Set("Reason", ue::MakeName(kReason)).Set<void*>("Actor", actor).SetBool("bTargetable", targetable);
    return c.Run();
}

struct Applied {
    ue::FWeak pawn;
    bool active = false;
    void* Get() const { return ue::WeakGet(pawn); }
};

Applied god, demigod, hidden;
std::vector<Tracked> xpScale;
bool selfDamageOff = false;
ue::FWeak selfGlobals;
float savedSelfConst = 0.0f, savedSelfScale = 1.0f;

struct LevelJob {
    bool active = false;
    int target = 0;
    int tries = 0;
    double next = 0;
} levelJob;

void* DamageGlobals() {
    static double retry = 0;
    void* d = ue::WeakGet(selfGlobals);
    if (d || st.time < retry) return d;
    d = ue::FindFirstOf(Cls("DamageGlobalsData"));
    if (d) selfGlobals = ue::MakeWeak(d);
    else retry = st.time + 5.0;
    return d;
}

void* Balance() { return st.pawn ? ue::Ptr(st.pawn, "PlayerBalanceComponent") : nullptr; }

void* Inventory() { return st.pawn ? ue::Ptr(st.pawn, "OakCharacterInventory") : nullptr; }

void ToggleLock(Applied& a, bool want, const char* lockFn, const char* unlockFn) {
    void* cur = a.Get();
    if (a.active && (!want || cur != st.pawn)) {
        if (cur) LockCall(unlockFn, cur);
        a.active = false;
        a.pawn = {};
    }
    if (want && !a.active && st.pawn) {
        if (LockCall(lockFn, st.pawn)) {
            a.active = true;
            a.pawn = ue::MakeWeak(st.pawn);
        }
    }
}

void ToggleHidden(bool want) {
    void* cur = hidden.Get();
    if (hidden.active && (!want || cur != st.pawn)) {
        if (cur) SetTargetable(cur, true);
        hidden.active = false;
        hidden.pawn = {};
    }
    if (want && !hidden.active && st.pawn) {
        if (SetTargetable(st.pawn, false)) {
            hidden.active = true;
            hidden.pawn = ue::MakeWeak(st.pawn);
        }
    }
}

void RefillHealth() {
    void* d = DamageComp(st.pawn);
    if (!d) return;
    ue::Call mx(d, "GetMaxHealth"), cur(d, "GetCurrentHealth");
    if (!mx.Run() || !cur.Run()) return;
    float m = mx.Get<float>("ReturnValue"), c = cur.Get<float>("ReturnValue");
    if (m > 0 && c < m) {
        ue::Call set(d, "SetCurrentHealth");
        set.Set("NewCurrentHealth", m);
        set.Run();
    }
}

void RefillShield() {
    void* d = DamageComp(st.pawn);
    if (!d) return;
    ue::Call mx(d, "GetMaxShield"), cur(d, "GetCurrentShield");
    if (!mx.Run() || !cur.Run()) return;
    float m = mx.Get<float>("ReturnValue"), c = cur.Get<float>("ReturnValue");
    if (m > 0 && c < m) {
        ue::Call set(d, "SetCurrentShield");
        set.Set("NewCurrentShield", m);
        set.Run();
    }
}

void SelfDamage(bool want) {
    if (want == selfDamageOff) return;
    void* g = want ? DamageGlobals() : ue::WeakGet(selfGlobals);
    if (!g && !want) {
        selfDamageOff = false;
        return;
    }
    ue::Prop p = g ? ue::FindProp(ue::ClassOf(g), "DefaultInstigatorSelfDamageScale") : ue::Prop{};
    if (!p) return;
    if (want && !selfDamageOff) {
        savedSelfConst = ue::At<float>(g, p.offset);
        savedSelfScale = ue::At<float>(g, p.offset + 0x30);
        ue::Put<float>(g, p.offset, 0.0f);
        ue::Put<float>(g, p.offset + 0x30, 0.0f);
        selfDamageOff = true;
    } else if (!want && selfDamageOff) {
        ue::Put<float>(g, p.offset, savedSelfConst);
        ue::Put<float>(g, p.offset + 0x30, savedSelfScale);
        selfDamageOff = false;
    }
}

int ExperienceFor(int level) {
    void* g = Globals();
    void* eg = g ? ue::Ptr(g, "ExperienceGlobals") : nullptr;
    ue::Prop p = eg ? ue::FindProp(ue::ClassOf(eg), "PlayerExperienceFormula") : ue::Prop{};
    if (!p) return -1;
    float mul = ue::At<float>(eg, p.offset), pw = ue::At<float>(eg, p.offset + 4), add = ue::At<float>(eg, p.offset + 8);
    if (mul <= 0 || pw <= 0 || pw > 10) return -1;
    double xp = (double)mul * std::pow((double)level, (double)pw) + add;
    return xp < 0 ? 0 : (int)std::ceil(xp);
}

int CurrentLevel() {
    void* b = Balance();
    ue::Call c(b, "GetExperienceLevel");
    return c.Run() ? c.Get<int32_t>("ReturnValue") : -1;
}

void AddXp(int amount) {
    void* b = Balance();
    if (!b || amount <= 0) return;
    ue::Call c(b, "AddExperience");
    c.Set<int32_t>("Amount", amount).Set<uint8_t>("Type", 0).Set<uint8_t>("Source", 0);
    c.Run();
}

void StepLevelJob() {
    if (!levelJob.active || st.time < levelJob.next) return;
    levelJob.next = st.time + 0.4;
    int lvl = CurrentLevel();
    if (lvl < 0) {
        levelJob.active = false;
        Note("Could not read your level");
        return;
    }
    if (lvl >= levelJob.target) {
        levelJob.active = false;
        Note("You are now level " + std::to_string(lvl));
        return;
    }
    if (++levelJob.tries > 12) {
        levelJob.active = false;
        Note("Stopped at level " + std::to_string(lvl));
        return;
    }
    void* b = Balance();
    int xp = ue::GetInt(b, "Experience", -1);
    int need = ExperienceFor(levelJob.target);
    if (xp < 0 || need < 0) {
        levelJob.active = false;
        Note("Could not read your XP");
        return;
    }
    float scale = 1.0f;
    ue::Prop sp = ue::FindProp(ue::ClassOf(b), "AllExperienceScale");
    if (sp) scale = ue::At<float>(b, sp.offset + 4);
    if (!(scale > 0.0001f) || scale > 1000.0f) scale = 1.0f;
    int add = need - xp;
    if (add <= 0) add = 2 + levelJob.tries;
    AddXp((int)std::ceil(add / scale));
}

bool ParseAmount(const std::vector<std::string>& in, long long& out) {
    if (in.empty()) return false;
    std::string s;
    for (char c : in[0])
        if (c >= '0' && c <= '9') s.push_back(c);
    if (s.empty() || s.size() > 12) return false;
    out = std::atoll(s.c_str());
    return true;
}

int CategoryQuantity(void* inv, void* cat) {
    ue::Call c(inv, "GetCategoryItemQuantity");
    c.Set<void*>("ItemCategory", cat);
    return c.Run() ? c.Get<int32_t>("ReturnValue") : -1;
}

bool AddToCategory(void* inv, void* cat, int amount) {
    ue::Call c(inv, "AddOrUpdateCategoryOnly");
    c.Set<void*>("ItemCategory", cat).Set<int32_t>("Quantity", amount);
    return c.Run();
}

void Report(const char* label, long long want, int after) {
    if (after == want) Note(std::string(label) + " set to " + std::to_string(want));
    else Note(std::string(label) + " is now " + std::to_string(after));
}

bool AboveCap(const char* label, int before, long long asked, long long cap) {
    if (asked <= cap || before < cap) return false;
    Note(std::string(label) + " is already " + std::to_string(before) + ", the menu sets at most " + std::to_string(cap));
    return true;
}

void ApplyBackpackCurrency(void* cat, const char* label, long long want, long long asked, long long cap) {
    void* inv = Inventory();
    if (!inv) {
        Note(std::string(label) + ": not found in the game");
        return;
    }
    int before = CategoryQuantity(inv, cat);
    if (before < 0) {
        Note(std::string(label) + ": could not read the amount");
        return;
    }
    if (AboveCap(label, before, asked, cap)) return;
    long long change = want - before;
    if (change != 0 && !AddToCategory(inv, cat, (int)change)) {
        Note(std::string(label) + ": the game refused the change");
        return;
    }
    int after = CategoryQuantity(inv, cat);
    LOG("%s: %d -> %d (asked %lld)", label, before, after, want);
    Report(label, want, after);
}

struct PremiumStore {
    int state = 0;
    std::future<void*> scan;
    void* add = nullptr;
    int list = -1;
};
PremiumStore premium;

bool CallPremiumAdd(void* fn, void* pc, int32_t amount, void* cat) {
    using AddFn = void(__fastcall*)(void*, int32_t, void*);
    __try {
        ((AddFn)fn)(pc, amount, cat);
        return true;
    } __except (1) {
        return false;
    }
}

void StartPremiumScan() {
    premium.add = nullptr;
    premium.list = -1;
    premium.state = 3;
    void* pcCls = Cls("OakPlayerController");
    ue::Prop mgr = pcCls ? ue::FindProp(pcCls, "PremiumCurrencyManager") : ue::Prop{};
    void* mgrStruct = Cls("PremiumCurrencyManager");
    void* dataStruct = Cls("PremiumCurrencyData");
    ue::Prop list = mgrStruct ? ue::FindProp(mgrStruct, "Currencies") : ue::Prop{};
    ue::Prop data = dataStruct ? ue::FindProp(dataStruct, "CategoryData") : ue::Prop{};
    void* catCls = Cls("GbxInventoryCategoryData");
    ue::Prop flag = catCls ? ue::FindProp(catCls, "bIsPremiumCurrencyCategory") : ue::Prop{};
    int size = dataStruct ? ue::StructSize(dataStruct) : 0;
    void* cdo = pcCls ? Lib("OakPlayerController") : nullptr;
    if (!mgr || !list || !data || !flag || !cdo || size != 0x10 || data.offset != 0 || flag.offset >= 0x80 || mgr.offset < 0x80) {
        LOG("keys: key store layout not as expected (manager %d, list %d, size 0x%X, flag %d)", mgr.offset, list.offset, size, flag.offset);
        return;
    }
    std::array<int16_t, 29> pat = {0x49, 0x8B, 0xC0, 0x4D, 0x85, 0xC0, 0x74, -1, 0x41, 0x80, 0x78, (int16_t)flag.offset, 0x00, 0x74, -1,
                                   0x44, 0x8B, 0xC2, 0x48, 0x81, 0xC1, 0,    0,    0,    0,    0x48, 0x8B, 0xD0, 0xE9};
    for (int i = 0; i < 4; ++i) pat[21 + i] = (int16_t)((mgr.offset >> (8 * i)) & 0xFF);
    premium.list = mgr.offset + list.offset;
    premium.state = 1;
    premium.scan = std::async(std::launch::async, [pat, cdo]() -> void* {
        void* fn = ue::FindCodeUnique(pat.data(), (int)pat.size());
        return fn && ue::VtableHas(cdo, fn, 1600) ? fn : nullptr;
    });
}

void PollPremium() {
    if (premium.state != 1 || premium.scan.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    premium.add = premium.scan.get();
    premium.state = premium.add ? 2 : 3;
    if (premium.add) {
        LOG("keys: key store found, add code at %p", premium.add);
        return;
    }
    LOG("keys: the game's own add-keys code was not found");
    Ready("golden_keys", false, "Not found in this version of the game");
    Ready("diamond_keys", false, "Not found in this version of the game");
}

uint8_t* PremiumEntry(void* cat) {
    auto arr = ue::At<ue::TArray<uint8_t>>(st.pc, premium.list);
    for (int i = 0; arr.data && i < arr.num && i < 64; ++i) {
        uint8_t* e = arr.data + (size_t)i * 0x10;
        if (ue::At<void*>(e, 0) == cat) return e;
    }
    return nullptr;
}

void ApplyKeys(void* cat, const char* label, long long want, long long asked, long long cap) {
    if (!st.pc || premium.state != 2) {
        Note(std::string(label) + ": could not find where the game keeps them");
        return;
    }
    uint8_t* e = PremiumEntry(cat);
    int before = e ? ue::At<int32_t>(e, 8) : 0;
    if (AboveCap(label, before, asked, cap)) return;
    if (before == want) {
        Note(std::string(label) + " already at " + std::to_string(want));
        return;
    }
    bool ok;
    if (want > before) {
        ok = CallPremiumAdd(premium.add, st.pc, (int32_t)(want - before), cat);
    } else {
        ue::Put<int32_t>(e, 8, (int32_t)want - 1);
        ok = CallPremiumAdd(premium.add, st.pc, 1, cat);
    }
    e = PremiumEntry(cat);
    int after = e ? ue::At<int32_t>(e, 8) : 0;
    if (!ok && e) {
        ue::Put<int32_t>(e, 8, before);
        after = before;
    }
    int shown = CategoryQuantity(Inventory(), cat);
    LOG("%s: %d -> %d (asked %lld, call %s, the game shows %d)", label, before, after, want, ok ? "ok" : "crashed", shown);
    if (!ok) {
        Note(std::string(label) + ": the game refused the change");
        return;
    }
    Report(label, want, after);
}

struct CurrencyJob {
    const char* catProp;
    const char* label;
    bool keys;
    long long want, asked, cap;
    std::shared_future<std::string> backup;
};
std::vector<CurrencyJob> currencyJobs;
std::shared_future<std::string> runningBackup;
std::string lastBackup;
double lastBackupAt = -100;

std::shared_future<std::string> SharedBackup() {
    if (runningBackup.valid() && runningBackup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return runningBackup;
    if (!lastBackup.empty() && st.time >= lastBackupAt && st.time - lastBackupAt < 10.0) {
        std::promise<std::string> done;
        done.set_value(lastBackup);
        return done.get_future().share();
    }
    runningBackup = std::async(std::launch::async, [] { return BackupNow(); }).share();
    return runningBackup;
}

void ApplyCurrency(const CurrencyJob& j) {
    void* gg = GameplayGlobals();
    void* cat = gg ? ue::Ptr(gg, j.catProp) : nullptr;
    if (!cat) {
        Note(std::string(j.label) + ": not found in the game");
        return;
    }
    if (j.keys) ApplyKeys(cat, j.label, j.want, j.asked, j.cap);
    else ApplyBackpackCurrency(cat, j.label, j.want, j.asked, j.cap);
}

void SubmitCurrency(const char* id, const char* catProp, const char* label, bool keys, long long cap) {
    std::vector<std::string> in;
    if (!Submitted(id, in)) return;
    long long asked = 0;
    if (!ParseAmount(in, asked)) {
        Note(std::string("Type a number for ") + label);
        return;
    }
    long long want = std::clamp(asked, 0ll, cap);
    for (CurrencyJob& j : currencyJobs)
        if (j.label == label) {
            j.want = want;
            j.asked = asked;
            return;
        }
    currencyJobs.push_back({catProp, label, keys, want, asked, cap, SharedBackup()});
    if (currencyJobs.back().backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) Note("Backing up your saves first...");
}

void PumpCurrency() {
    PollPremium();
    for (auto it = currencyJobs.begin(); it != currencyJobs.end();) {
        bool waiting = it->backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready || (it->keys && premium.state == 1);
        if (waiting) {
            ++it;
            continue;
        }
        std::string path = it->backup.get();
        if (path.empty()) {
            Note(std::string("Save backup failed, ") + it->label + " was not changed");
        } else {
            if (path != lastBackup) {
                lastBackup = path;
                lastBackupAt = st.time;
            }
            ApplyCurrency(*it);
        }
        it = currencyJobs.erase(it);
    }
}

}

bool PlayerBusy() {
    auto running = [](const auto& f) { return f.valid() && f.wait_for(std::chrono::seconds(0)) != std::future_status::ready; };
    return running(runningBackup) || running(premium.scan);
}

void PlayerInit() {
    bool lib = HasFunc("GbxGameSystemCoreBlueprintLibrary", "ResourceLockGod") && HasFunc("GbxGameSystemCoreBlueprintLibrary", "ResourceUnlockGod");
    bool demi = HasFunc("GbxGameSystemCoreBlueprintLibrary", "ResourceLockDemigod") && HasFunc("GbxGameSystemCoreBlueprintLibrary", "ResourceUnlockDemigod");
    bool health = HasFunc("DamageComponent", "SetCurrentHealth") && HasFunc("DamageComponent", "GetMaxHealth");
    bool shield = HasFunc("DamageComponent", "SetCurrentShield") && HasFunc("DamageComponent", "GetMaxShield");
    Ready("god_mode", lib);
    Ready("unlimited_health", demi && health);
    Ready("heal_health", health);
    Ready("unlimited_shield", shield);
    Ready("restore_shield", shield);
    Ready("no_self_damage", Has("DamageGlobalsData", "DefaultInstigatorSelfDamageScale"));
    Ready("invisibility", HasFunc("GbxGameSystemCoreBlueprintLibrary", "ResourceLockTargetable"));
    Ready("mega_xp", Has("PlayerBalanceStateComponent", "AllExperienceScale"));
    bool level = HasFunc("PlayerBalanceStateComponent", "AddExperience") && HasFunc("BalanceStateComponent", "GetExperienceLevel") &&
                 Has("ExperienceGlobals", "PlayerExperienceFormula") && Has("OakGlobalsData", "ExperienceGlobals");
    Ready("give_level", level);
    Ready("give_level_apply", level);
    bool cur = HasFunc("InventoryListComponent", "AddOrUpdateCategoryOnly") && HasFunc("InventoryListComponent", "GetCategoryItemQuantity");
    Ready("money", cur && Has("GameplayGlobals", "CreditsInventoryType"));
    Ready("eridium", cur && Has("GameplayGlobals", "EridiumInventoryType"));
    bool keys = HasFunc("InventoryListComponent", "GetCategoryItemQuantity") && Has("OakPlayerController", "PremiumCurrencyManager") &&
                Has("PremiumCurrencyManager", "Currencies") && Has("GbxInventoryCategoryData", "bIsPremiumCurrencyCategory");
    if (keys) StartPremiumScan();
    keys = keys && premium.state == 1;
    Ready("golden_keys", keys && Has("GameplayGlobals", "GoldenKeyInventoryType"));
    Ready("diamond_keys", keys && Has("GameplayGlobals", "DiamondKeyInventoryType"));
}

void PlayerTick() {
    ToggleLock(god, On("god_mode"), "ResourceLockGod", "ResourceUnlockGod");
    ToggleLock(demigod, On("unlimited_health"), "ResourceLockDemigod", "ResourceUnlockDemigod");
    if (On("unlimited_health")) RefillHealth();
    if (On("unlimited_shield")) RefillShield();
    if (Pressed("heal_health")) {
        RefillHealth();
        Note("Health restored");
    }
    if (Pressed("restore_shield")) {
        RefillShield();
        Note("Shield restored");
    }
    SelfDamage((On("no_self_damage") || On("one_hit_kill") || Value("damage_mult") > 1.001f) && st.alone);
    ToggleHidden(On("invisibility") && st.alone);
    float m = Value("mega_xp");
    void* b = Balance();
    if (m > 1.001f && b) ScaleAttr(xpScale, b, "AllExperienceScale", m);
    else if (!xpScale.empty()) RestoreAttrs(xpScale);
    if (Pressed("give_level_apply")) {
        int target = std::clamp((int)std::lround(Value("give_level")), 2, 72);
        int lvl = CurrentLevel();
        if (lvl < 0) {
            Note("Could not read your level");
        } else if (lvl >= target) {
            Note("You are already level " + std::to_string(lvl) + ", Give level only goes up");
        } else {
            levelJob = {};
            levelJob.active = true;
            levelJob.target = target;
            levelJob.next = st.time;
            Note("Raising your level to " + std::to_string(target));
        }
    }
    StepLevelJob();
    SubmitCurrency("money", "CreditsInventoryType", "Money", false, 1000000000ll);
    SubmitCurrency("eridium", "EridiumInventoryType", "Eridium", false, 1000000000ll);
    SubmitCurrency("golden_keys", "GoldenKeyInventoryType", "Golden keys", true, 999ll);
    SubmitCurrency("diamond_keys", "DiamondKeyInventoryType", "Diamond keys", true, 999ll);
    PumpCurrency();
}

void PlayerOff() {
    ToggleLock(god, false, "ResourceLockGod", "ResourceUnlockGod");
    ToggleLock(demigod, false, "ResourceLockDemigod", "ResourceUnlockDemigod");
    SelfDamage(false);
    ToggleHidden(false);
    if (!xpScale.empty()) RestoreAttrs(xpScale);
    levelJob.active = false;
}

}
