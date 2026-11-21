#include "ingame/groups.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ukshop {

using namespace uk;
using namespace un;

namespace {

struct Game {
    Class *saver = nullptr, *scene = nullptr, *prefs = nullptr, *variation = nullptr, *category = nullptr;
    Class *setter = nullptr, *guns = nullptr, *fists = nullptr, *hook = nullptr;
    Method *addGear = nullptr, *checkGear = nullptr, *getMoney = nullptr, *savePath = nullptr, *isCustom = nullptr;
    Fld slot;
    Method *getInt = nullptr, *setInt = nullptr;
    Fld cost, owned, name;
    Method* updateMoney = nullptr;
    Fld categoryName;
    Method* categoryCheck = nullptr;
    Method* resetWeapons = nullptr;
    Fld gunsActivated, gunsNone, gunsSlots;
    Method* yesWeapon = nullptr;
    Fld fistsShown;
    Method *resetFists = nullptr, *noFist = nullptr, *hookCancel = nullptr;
};

const char* const kKeys[] = {"rev0", "rev1", "rev2", "sho0", "sho1", "sho2", "nai0", "nai1", "nai2", "rai0",
                             "rai1", "rai2", "rock0", "rock1", "rock2", "revalt", "shoalt", "naialt", "arm1", "arm2"};

const double kPatchEvery = 0.25;

Game G;
bool canUnlock = false, canShop = false;
std::string usedBackup;

std::unordered_map<int, int> idCost;
std::unordered_map<std::string, int> nameCost;
bool patched = false;
bool backupTried = false;
double nextPatch = 0;
int patchPlayer = 0;

bool IsAlt(const char* key) {
    size_t n = std::strlen(key);
    return n > 3 && std::strcmp(key + n - 3, "alt") == 0;
}

bool IsArm(const std::string& key) { return key.rfind("arm", 0) == 0; }

int Gear(const char* key) { return CallInt(G.checkGear, nullptr, {mono::NewString(key)}, -1); }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Clean(std::wstring p) {
    for (wchar_t& ch : p)
        if (ch == L'/') ch = L'\\';
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

bool SlotPathOk(const std::string& path, int slot) {
    std::wstring dir = Clean(uk::Folder());
    size_t cut = dir.find_last_of(L'\\');
    if (cut == std::wstring::npos) return false;
    std::wstring want = dir.substr(0, cut) + L"\\Saves\\Slot" + std::to_wstring(slot + 1);
    std::wstring got = Clean(Widen(path));
    return !got.empty() && _wcsicmp(want.c_str(), got.c_str()) == 0;
}

std::string Short(const std::string& backup) {
    std::wstring p = Clean(Widen(backup));
    size_t cut = p.find_last_of(L'\\');
    std::wstring leaf = cut == std::wstring::npos ? p : p.substr(cut + 1);
    return "fathom\\save_backup\\" + Narrow(leaf);
}

void Equip(Object* prefs, const char* key) {
    Object* pref = mono::NewString((std::string("weapon.") + key).c_str());
    int fallback = 0, one = 1;
    if (CallInt(G.getInt, prefs, {pref, &fallback}, 1) != 1) Call(G.setInt, prefs, {pref, &one});
}

void Refresh(const Ctx& c, const std::vector<std::string>& done) {
    std::unordered_set<std::string> keys(done.begin(), done.end());
    bool guns = false, arms = false;
    for (const std::string& k : done) {
        if (IsArm(k)) arms = true;
        else guns = true;
    }
    if (G.variation && G.name && G.owned) {
        for (Object* vi : FindAll(G.variation, true)) {
            if (!keys.count(mono::Str(Ref(vi, G.name)))) continue;
            SetVal<uint8_t>(vi, G.owned, 1);
            if (G.updateMoney && ActiveAndEnabled(vi)) Call(G.updateMoney, vi);
        }
    }
    if (G.category && G.categoryName && G.categoryCheck) {
        for (Object* cat : FindAll(G.category, true))
            if (keys.count(mono::Str(Ref(cat, G.categoryName)))) Call(G.categoryCheck, cat);
    }
    if (!c.inLevel) return;
    if (guns && G.resetWeapons && G.gunsSlots) {
        Object* setter = Single(G.setter);
        Object* gc = Single(G.guns);
        if (setter && gc && Enabled(setter) && ListItems(Ref(gc, G.gunsSlots)).size() == 6 && Val<uint8_t>(gc, G.gunsNone, 1) == 0) {
            bool out = Val<uint8_t>(gc, G.gunsActivated) != 0 && c.activated && !c.dead;
            bool first = false;
            Call(G.resetWeapons, setter, {&first});
            if (out && G.yesWeapon) Call(G.yesWeapon, gc);
        }
    }
    if (arms && G.resetFists && G.hookCancel) {
        Object* fc = Single(G.fists);
        Object* hook = Single(G.hook);
        if (fc && hook) {
            bool shown = Val<uint8_t>(fc, G.fistsShown, 1) != 0;
            Call(G.hookCancel, hook);
            Call(G.resetFists, fc);
            if (!shown && G.noFist) Call(G.noFist, fc);
        }
    }
}

void Unlock(const Ctx& c) {
    if (c.loading) {
        c.Note("Wait until the level has loaded");
        return;
    }
    bool ok = false;
    Object* custom = Call(G.isCustom, nullptr, {}, &ok);
    if (!ok) {
        c.Note("Could not check the game, nothing was changed");
        return;
    }
    if (mono::Result<uint8_t>(custom, 1) != 0) {
        c.Note("Leave the custom level first");
        return;
    }
    Call(G.getMoney, nullptr, {}, &ok);
    if (!ok) {
        c.Note("Could not read your save, nothing was changed");
        return;
    }
    int slot = StaticVal<int>(G.slot, -1);
    std::string path = mono::Str(Call(G.savePath, nullptr, {}, &ok));
    if (!ok || slot < 0 || slot > 4 || !SlotPathOk(path, slot)) {
        LOG("shop: unlock refused, the save folder is \"%s\" (slot %d)", path.c_str(), slot + 1);
        c.Note("Your save folder is not where Fathom expects it, nothing was changed");
        return;
    }
    std::vector<const char*> todo;
    for (const char* key : kKeys) {
        int have = Gear(key);
        if (have < 0) {
            c.Note("Could not read your save, nothing was changed");
            return;
        }
        if (have == 0) todo.push_back(key);
    }
    if (todo.empty()) {
        c.Note("Every weapon is already unlocked");
        return;
    }
    std::string backup = uk::LastBackup();
    if (backup.empty() || backup == usedBackup) backup = uk::Backup();
    if (backup.empty()) {
        c.Note("Save backup failed, nothing was changed");
        return;
    }
    usedBackup = backup;
    Object* prefs = Single(G.prefs);
    std::vector<std::string> done;
    bool failed = false;
    for (const char* key : todo) {
        Call(G.addGear, nullptr, {mono::NewString(key)}, &ok);
        if (!ok || Gear(key) != 1) {
            failed = true;
            break;
        }
        done.push_back(key);
        if (!IsAlt(key) && prefs) Equip(prefs, key);
    }
    if (!done.empty()) Refresh(c, done);
    int owned = (int)(std::size(kKeys) - todo.size());
    std::string where = " in save slot " + std::to_string(slot + 1);
    LOG("shop: unlocked %d of %d missing items in slot %d%s", (int)done.size(), (int)todo.size(), slot + 1, failed ? ", then the save could not be written" : "");
    if (failed) {
        c.Note("Unlocked " + std::to_string(done.size()) + " of " + std::to_string(todo.size()) + " items" + where +
               ", then the save could not be written. Backup: " + Short(backup));
        return;
    }
    c.Note("Unlocked " + std::to_string(done.size()) + (done.size() == 1 ? " item" : " items") + where + " (" + std::to_string(owned) +
           " were already yours). Backup: " + Short(backup));
}

void Patch() {
    std::unordered_set<int> seen;
    for (Object* vi : FindAll(G.variation, true)) {
        int id = InstanceId(vi);
        if (!id) continue;
        seen.insert(id);
        int cost = Val<int>(vi, G.cost, 0);
        if (cost <= 0) continue;
        idCost.emplace(id, cost);
        std::string name = mono::Str(Ref(vi, G.name));
        if (!name.empty()) nameCost.emplace(name, cost);
        SetVal<int>(vi, G.cost, 0);
        patched = true;
        if (G.updateMoney && ActiveAndEnabled(vi)) Call(G.updateMoney, vi);
    }
    for (auto it = idCost.begin(); it != idCost.end();) {
        if (seen.count(it->first)) ++it;
        else it = idCost.erase(it);
    }
}

void Unpatch() {
    if (patched && canShop) {
        for (Object* vi : FindAll(G.variation, true)) {
            if (Val<int>(vi, G.cost, -1) != 0) continue;
            int cost = 0;
            auto it = idCost.find(InstanceId(vi));
            if (it != idCost.end()) {
                cost = it->second;
            } else {
                auto nt = nameCost.find(mono::Str(Ref(vi, G.name)));
                if (nt != nameCost.end()) cost = nt->second;
            }
            if (cost <= 0) continue;
            SetVal<int>(vi, G.cost, cost);
            if (G.updateMoney && ActiveAndEnabled(vi)) Call(G.updateMoney, vi);
        }
    }
    idCost.clear();
    patched = false;
}

}

bool Resolve() {
    G = Game{};
    G.saver = Cls("GameProgressSaver");
    G.addGear = MT(G.saver, "AddGear", {"System.String"});
    G.checkGear = MT(G.saver, "CheckGear", {"System.String"});
    G.getMoney = M(G.saver, "GetMoney", 0);
    G.savePath = M(G.saver, "get_SavePath", 0);
    G.slot = F(G.saver, "currentSlot");
    G.scene = Cls("SceneHelper");
    G.isCustom = M(G.scene, "get_IsPlayingCustom", 0);
    G.prefs = Cls("PrefsManager");
    G.getInt = MT(G.prefs, "GetInt", {"System.String", "System.Int32"});
    G.setInt = MT(G.prefs, "SetInt", {"System.String", "System.Int32"});

    G.variation = Cls("VariationInfo");
    G.cost = F(G.variation, "cost");
    G.owned = F(G.variation, "alreadyOwned");
    G.name = F(G.variation, "weaponName");
    G.updateMoney = M(G.variation, "UpdateMoney", 0);
    G.category = Cls("ShopCategory");
    G.categoryName = F(G.category, "weaponName");
    G.categoryCheck = M(G.category, "CheckGear", 0);

    G.setter = Cls("GunSetter");
    G.resetWeapons = MT(G.setter, "ResetWeapons", {"System.Boolean"});
    G.guns = Cls("GunControl");
    G.gunsActivated = F(G.guns, "activated");
    G.gunsNone = F(G.guns, "noWeapons");
    G.gunsSlots = F(G.guns, "slots");
    G.yesWeapon = M(G.guns, "YesWeapon", 0);
    G.fists = Cls("FistControl");
    G.fistsShown = F(G.fists, "_activated");
    G.resetFists = M(G.fists, "ResetFists", 0);
    G.noFist = M(G.fists, "NoFist", 0);
    G.hook = Cls("HookArm");
    G.hookCancel = M(G.hook, "Cancel", 0);

    canUnlock = G.addGear && G.checkGear && G.getMoney && G.savePath && G.slot && G.isCustom && G.prefs && G.getInt && G.setInt;
    canShop = G.variation && G.cost && G.name && R.findObjectsOfTypeInactive;

    if (!canUnlock) LOG("shop: unlock all weapons is off (save methods not found)");
    else if (!(G.resetWeapons && G.yesWeapon && G.resetFists && G.hookCancel && G.updateMoney))
        LOG("shop: unlocked weapons may only show up after the next level load");
    if (!canShop) LOG("shop: free shop is off (shop price fields not found)");
    else if (!G.updateMoney) LOG("shop: free shop works, but an open shop page shows the new price only when reopened");
    return canUnlock || canShop;
}

bool Supported(const std::string& id) {
    if (id == "unlock_all_weapons") return canUnlock;
    if (id == "free_shop") return canShop;
    return false;
}

void Tick(const Ctx& c) {
    bool want = canShop && c.inLevel && !c.dead && c.on("free_shop");
    if (!want) {
        if (patched || !idCost.empty()) Unpatch();
        return;
    }
    if (!backupTried) {
        backupTried = true;
        if (uk::LastBackup().empty() && uk::Backup().empty()) c.Note("Could not back up your saves before the free shop");
    }
    if (c.now < nextPatch && c.playerId == patchPlayer) return;
    nextPatch = c.now + kPatchEvery;
    patchPlayer = c.playerId;
    Patch();
}

void Release(const Ctx& c) {
    Unpatch();
    nextPatch = 0;
    patchPlayer = 0;
    if (c.stopping) nameCost.clear();
}

void Action(const std::string& id, const Ctx& c) {
    if (id == "unlock_all_weapons" && canUnlock) Unlock(c);
}

bool Busy() { return patched; }

}
