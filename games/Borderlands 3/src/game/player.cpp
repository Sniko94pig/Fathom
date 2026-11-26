#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <chrono>
#include <cmath>
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

int CategoryMax(void* inv, void* cat) {
    ue::Call c(inv, "GetSelectedItemQuantity");
    c.Set<void*>("ChildTypeToGet", cat);
    return c.Run() ? c.Get<int32_t>("MaxQuantity") : 0;
}

bool UpdateCategory(void* inv, void* cat, int quantity) {
    ue::Call c(inv, "AddOrUpdateCategoryOnly");
    c.Set<void*>("ItemCategory", cat).Set<int32_t>("Quantity", quantity);
    return c.Run();
}

void SyncProfile(void* cat, int oldValue, int value) {
    void* profile = st.pc ? ue::Ptr(st.pc, "CurrentOakProfile") : nullptr;
    if (profile) {
        ue::Prop p = ue::FindProp(ue::ClassOf(profile), "BankInventoryCategoryList");
        auto list = ue::Read<ue::TArray<uint8_t>>(profile, p);
        for (int i = 0; p && list.data && i < list.num && i < 512; ++i) {
            uint8_t* e = list.data + (size_t)i * 0x10;
            if (ue::At<void*>(e, 0) != cat) continue;
            ue::Put<int32_t>(e, 8, value);
            break;
        }
    }
    if (st.pc) {
        ue::Prop p = ue::FindProp(ue::ClassOf(st.pc), "PremiumCurrencyManager");
        if (p) {
            auto list = ue::At<ue::TArray<uint8_t>>(st.pc, p.offset);
            for (int i = 0; list.data && i < list.num && i < 64; ++i) {
                uint8_t* e = list.data + (size_t)i * 0x10;
                if (ue::At<void*>(e, 0) != cat) continue;
                if (ue::At<int32_t>(e, 8) == oldValue) ue::Put<int32_t>(e, 8, value);
                break;
            }
        }
    }
}

struct CurrencyJob {
    const char* catProp;
    const char* label;
    bool profile;
    long long cap;
    long long want;
    std::future<std::string> backup;
};
std::vector<CurrencyJob> currencyJobs;

void ApplyCurrency(const char* catProp, const char* label, bool profile, long long cap, long long want) {
    void* gg = GameplayGlobals();
    void* cat = gg ? ue::Ptr(gg, catProp) : nullptr;
    void* inv = Inventory();
    if (!cat || !inv) {
        Note(std::string(label) + ": not found in the game");
        return;
    }
    int mx = CategoryMax(inv, cat);
    long long limit = mx > 0 ? mx : cap;
    want = std::clamp(want, 0ll, std::min(limit, 2000000000ll));
    int before = CategoryQuantity(inv, cat);
    if (!UpdateCategory(inv, cat, (int)want)) {
        Note(std::string(label) + ": the game refused the change");
        return;
    }
    int after = CategoryQuantity(inv, cat);
    if (after != want && before >= 0 && after == before + want) {
        UpdateCategory(inv, cat, (int)(want - after));
        after = CategoryQuantity(inv, cat);
    }
    if (profile) SyncProfile(cat, before, (int)want);
    LOG("%s: %d -> %d (asked %lld, max %d)", label, before, after, want, mx);
    if (after == want) Note(std::string(label) + " set to " + std::to_string(want));
    else Note(std::string(label) + " is now " + std::to_string(after));
}

void SubmitCurrency(const char* id, const char* catProp, const char* label, bool profile, long long cap) {
    std::vector<std::string> in;
    if (!Submitted(id, in)) return;
    long long want = 0;
    if (!ParseAmount(in, want)) {
        Note(std::string("Type a number for ") + label);
        return;
    }
    for (CurrencyJob& j : currencyJobs)
        if (j.label == label) {
            j.want = want;
            return;
        }
    currencyJobs.push_back({catProp, label, profile, cap, want, std::async(std::launch::async, [] { return BackupNow(); })});
    Note("Backing up your saves first...");
}

void PumpCurrency() {
    for (auto it = currencyJobs.begin(); it != currencyJobs.end();) {
        if (it->backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        std::string path = it->backup.get();
        if (path.empty()) Note(std::string("Save backup failed, ") + it->label + " was not changed");
        else ApplyCurrency(it->catProp, it->label, it->profile, it->cap, it->want);
        it = currencyJobs.erase(it);
    }
}

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
    Ready("golden_keys", cur && Has("GameplayGlobals", "GoldenKeyInventoryType"));
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
    SubmitCurrency("money", "CreditsInventoryType", "Money", false, 2000000000ll);
    SubmitCurrency("eridium", "EridiumInventoryType", "Eridium", false, 2000000000ll);
    SubmitCurrency("golden_keys", "GoldenKeyInventoryType", "Golden keys", true, 999ll);
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
