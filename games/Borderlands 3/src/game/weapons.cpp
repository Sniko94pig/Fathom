#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

namespace bl3 {

namespace {

const wchar_t* kReason = L"FathomMenu";

struct PoolRef {
    uint8_t raw[0x18] = {};
};

bool ReadPool(void* comp, const char* prop, PoolRef& out) {
    ue::Prop p = comp ? ue::FindProp(ue::ClassOf(comp), prop) : ue::Prop{};
    if (!p || p.size < (int)sizeof(out.raw)) return false;
    return ue::SafeCopy(out.raw, (uint8_t*)comp + p.offset, sizeof(out.raw));
}

void* PoolLib() { return Lib("GameResourcePoolFunctionLibrary"); }

bool PoolValues(const PoolRef& ref, float& cur, float& mx) {
    ue::Call c(PoolLib(), "BreakResourcePoolReference");
    c.SetRaw("InPool", ref.raw, sizeof(ref.raw));
    if (!c.Run() || !c.GetBool("bValid")) return false;
    cur = c.Get<float>("CurrentValue");
    mx = c.Get<float>("MaxValue");
    return true;
}

void SetPool(const PoolRef& ref, float v) {
    ue::Call c(PoolLib(), "SetResourcePoolValue");
    c.SetRaw("InPool", ref.raw, sizeof(ref.raw)).Set("InValue", v);
    c.Run();
}

void RefillPool(const PoolRef& ref) {
    ue::Call c(PoolLib(), "RefillResourcePoolByPercentage");
    c.SetRaw("InPool", ref.raw, sizeof(ref.raw)).Set("Percentage", 1.0f).Set("MaxPercentage", 1.0f);
    c.Run();
}

bool PoolFor(void* resource, PoolRef& out) {
    void* mgr = st.pawn ? ue::Ptr(st.pawn, "ResourcePoolComponent") : nullptr;
    if (!mgr || !resource) return false;
    ue::Call c(mgr, "GetResourcePoolByResource");
    c.Set<void*>("Resource", resource);
    if (!c.Run()) return false;
    return c.GetRaw("ReturnValue", out.raw, sizeof(out.raw));
}

void RefillGrenades() {
    void* res = st.pawn ? ue::Ptr(st.pawn, "GrenadeResource") : nullptr;
    PoolRef ref;
    if (PoolFor(res, ref)) RefillPool(ref);
}

struct Applied {
    ue::FWeak pawn;
    bool active = false;
};

Applied infinite;
std::vector<Tracked> reloadAttrs, fireAttrs, recoilAttrs, chargeAttrs, heatAttrs;

struct Modes {
    ue::FWeak weapon;
    std::vector<ue::FWeak> fire, ammo;
    double at = -100;
};
std::vector<Modes> modeCache;

void AddUnique(std::vector<ue::FWeak>& list, void* p) {
    for (const ue::FWeak& w : list)
        if (ue::WeakGet(w) == p) return;
    list.push_back(ue::MakeWeak(p));
}

void Collect(void* weapon, Modes& m) {
    m.at = st.time;
    m.fire.clear();
    m.ammo.clear();
    void* fireCls = Cls("WeaponFireComponent");
    void* ammoCls = Cls("WeaponAmmoComponent");
    int modes = 2;
    ue::Prop ms = ue::FindProp(ue::ClassOf(weapon), "UseModeState");
    if (ms) {
        auto a = ue::Read<ue::TArray<uint8_t>>(weapon, ms);
        if (a.num > 0 && a.num <= 8) modes = a.num;
    }
    for (int i = 0; i < modes; ++i)
        for (int k = 0; k < 2; ++k) {
            void* cls = k == 0 ? fireCls : ammoCls;
            if (!cls) continue;
            ue::Call c(weapon, "GetUseModeComponentByClass");
            c.Set<uint8_t>("UseModeIndex", (uint8_t)i).Set<void*>("ComponentClass", cls);
            if (!c.Run()) continue;
            void* p = c.Get<void*>("ReturnValue");
            if (ue::Alive(p) && ue::IsA(p, cls)) AddUnique(k == 0 ? m.fire : m.ammo, p);
        }
    void* cur = ue::Ptr(weapon, "CurrentFireComponent");
    if (ue::Alive(cur) && fireCls && ue::IsA(cur, fireCls)) AddUnique(m.fire, cur);
    if (m.fire.empty()) {
        void* p = ComponentOf(weapon, "WeaponFireComponent");
        if (p) AddUnique(m.fire, p);
    }
    if (m.ammo.empty()) {
        void* p = ComponentOf(weapon, "WeaponAmmoComponent");
        if (p) AddUnique(m.ammo, p);
    }
}

std::vector<void*> Parts(void* weapon, bool fire) {
    std::vector<void*> out;
    if (!weapon) return out;
    Modes* m = nullptr;
    for (Modes& x : modeCache)
        if (ue::WeakGet(x.weapon) == weapon) m = &x;
    if (!m) {
        modeCache.erase(std::remove_if(modeCache.begin(), modeCache.end(), [](const Modes& x) { return !ue::WeakGet(x.weapon); }),
                        modeCache.end());
        if (modeCache.size() >= 24) modeCache.erase(modeCache.begin());
        modeCache.push_back({});
        m = &modeCache.back();
        m->weapon = ue::MakeWeak(weapon);
        Collect(weapon, *m);
    } else if (st.time - m->at > 3.0) {
        Collect(weapon, *m);
    }
    for (const ue::FWeak& w : fire ? m->fire : m->ammo)
        if (void* p = ue::WeakGet(w)) out.push_back(p);
    return out;
}

struct AmmoLayout {
    void* cls = nullptr;
    ue::Prop loaded, max, synced, infinite, pool;
};

AmmoLayout layouts[4];

const AmmoLayout& LayoutOf(void* comp) {
    void* cls = ue::ClassOf(comp);
    for (AmmoLayout& l : layouts)
        if (l.cls == cls) return l;
    static int next = 0;
    AmmoLayout& l = layouts[next++ % 4];
    l.cls = cls;
    l.loaded = ue::FindProp(cls, "LoadedAmmo");
    l.max = ue::FindProp(cls, "MaxLoadedAmmo");
    l.synced = ue::FindProp(cls, "ServerSyncedLoadedAmmo");
    l.infinite = ue::FindProp(cls, "bInfiniteAmmo");
    l.pool = ue::FindProp(cls, "ResourcePool");
    return l;
}

void FillMagazine(void* comp, bool fromReserve) {
    const AmmoLayout& l = LayoutOf(comp);
    if (!l.loaded || !l.max) return;
    int loaded = ue::At<int32_t>(comp, l.loaded.offset);
    int mx = ue::At<int32_t>(comp, l.max.offset + 4);
    if (mx <= 0 || mx > 100000 || loaded < 0 || loaded >= mx) return;
    int add = mx - loaded;
    if (fromReserve) {
        PoolRef ref;
        float cur = 0, top = 0;
        if (!l.pool || !ReadPool(comp, "ResourcePool", ref) || !PoolValues(ref, cur, top)) return;
        int spare = (int)std::floor(cur + 0.001f);
        add = std::min(add, spare);
        if (add <= 0) return;
        SetPool(ref, cur - (float)add);
    }
    ue::Put<int32_t>(comp, l.loaded.offset, loaded + add);
    if (l.synced) ue::Put<int32_t>(comp, l.synced.offset, loaded + add);
}

void LogMagazine(const char* what, void* weapon) {
    std::vector<void*> parts = Parts(weapon, false);
    if (parts.empty()) {
        LOG("%s: no magazine found on %s", what, ue::Name(ue::ClassOf(weapon)).c_str());
        return;
    }
    void* a = parts[0];
    const AmmoLayout& l = LayoutOf(a);
    int loaded = l.loaded ? ue::At<int32_t>(a, l.loaded.offset) : -1;
    int mx = l.max ? ue::At<int32_t>(a, l.max.offset + 4) : -1;
    LOG("%s: %s magazine %d of %d, %zu ammo parts, infinite flag %s", what, ue::Name(ue::ClassOf(a)).c_str(), loaded, mx, parts.size(),
        l.infinite ? "found" : "missing");
}

void TopUpReserve(void* comp) {
    const AmmoLayout& l = LayoutOf(comp);
    PoolRef ref;
    float cur = 0, mx = 0;
    if (!l.pool || !ReadPool(comp, "ResourcePool", ref) || !PoolValues(ref, cur, mx)) return;
    if (mx > 0 && cur < mx) RefillPool(ref);
}

struct FlagSave {
    ue::FWeak obj;
    ue::Prop prop;
    bool value = false;
};
std::vector<FlagSave> infiniteFlags;

void KeepInfiniteFlag(void* comp) {
    const AmmoLayout& l = LayoutOf(comp);
    if (!l.infinite) return;
    for (FlagSave& f : infiniteFlags)
        if (ue::WeakGet(f.obj) == comp) {
            if (!ue::ReadBool(comp, f.prop)) ue::WriteBool(comp, f.prop, true);
            return;
        }
    if (infiniteFlags.size() > 64)
        infiniteFlags.erase(std::remove_if(infiniteFlags.begin(), infiniteFlags.end(), [](const FlagSave& f) { return !ue::WeakGet(f.obj); }),
                            infiniteFlags.end());
    infiniteFlags.push_back({ue::MakeWeak(comp), l.infinite, ue::ReadBool(comp, l.infinite)});
    ue::WriteBool(comp, l.infinite, true);
}

void RestoreInfiniteFlags() {
    for (FlagSave& f : infiniteFlags) {
        void* o = ue::WeakGet(f.obj);
        if (o) ue::WriteBool(o, f.prop, f.value);
    }
    infiniteFlags.clear();
}

bool InfiniteAmmo(bool want) {
    void* lib = Lib("OakBlueprintLibrary");
    void* cur = ue::WeakGet(infinite.pawn);
    if (infinite.active && (!want || cur != st.pawn)) {
        if (cur) {
            ue::Call c(lib, "ResourceUnlockPlayerInfiniteAmmo");
            c.Set("Reason", ue::MakeName(kReason)).Set<void*>("Actor", cur);
            c.Run();
        }
        infinite = {};
    }
    if (want && !infinite.active && st.pawn) {
        ue::Call c(lib, "ResourceLockPlayerInfiniteAmmo");
        c.Set("Reason", ue::MakeName(kReason)).Set<void*>("Actor", st.pawn);
        if (c.Run()) {
            infinite.active = true;
            infinite.pawn = ue::MakeWeak(st.pawn);
        }
    }
    return infinite.active;
}

struct DamageTrack {
    ue::FWeak comp;
    int offset = -1;
    float baseNatural = 0;
    float baseWritten = 0;
    float valueNatural = 0;
    float valueWritten = 0;
    float mult = 1.0f;
    bool viaBase = false;
};
std::vector<DamageTrack> damage;
bool damageLogged = false;

void ScaleDamage(void* fire, float mult) {
    ue::Prop p = ue::FindProp(ue::ClassOf(fire), "Damage");
    if (!p) return;
    int vo = p.offset + 4, bo = p.offset + 8;
    DamageTrack* t = nullptr;
    for (DamageTrack& d : damage)
        if (d.offset == p.offset && ue::WeakGet(d.comp) == fire) t = &d;
    float base = ue::At<float>(fire, bo), value = ue::At<float>(fire, vo);
    if (!std::isfinite(base) || !std::isfinite(value)) return;
    if (!t) {
        if (damage.size() > 48)
            damage.erase(std::remove_if(damage.begin(), damage.end(), [](const DamageTrack& d) { return !ue::WeakGet(d.comp); }), damage.end());
        DamageTrack d;
        d.comp = ue::MakeWeak(fire);
        d.offset = p.offset;
        d.baseWritten = std::nanf("");
        d.valueWritten = std::nanf("");
        damage.push_back(d);
        t = &damage.back();
    }
    t->viaBase = std::fabs(base) > 0.0001f;
    if (t->viaBase) {
        if (!(base == t->baseWritten)) {
            t->baseNatural = base;
            t->valueNatural = (value == t->valueWritten && t->mult > 0) ? value / t->mult : value;
            t->baseWritten = base * mult;
            t->valueWritten = t->valueNatural * mult;
            t->mult = mult;
            ue::Put<float>(fire, bo, t->baseWritten);
            ue::Put<float>(fire, vo, t->valueWritten);
        } else if (t->mult != mult) {
            float now = t->mult > 0 ? value / t->mult : value;
            t->baseWritten = t->baseNatural * mult;
            t->valueWritten = now * mult;
            t->mult = mult;
            ue::Put<float>(fire, bo, t->baseWritten);
            ue::Put<float>(fire, vo, t->valueWritten);
        }
    } else {
        if (!(value == t->valueWritten)) t->valueNatural = value;
        float want = t->valueNatural * mult;
        if (value != want) ue::Put<float>(fire, vo, want);
        t->valueWritten = want;
        t->mult = mult;
    }
    if (!damageLogged) {
        damageLogged = true;
        LOG("damage: weapon damage %.1f -> %.1f (x%.0f)", t->valueNatural, ue::At<float>(fire, vo), mult);
    }
}

void RestoreDamage() {
    for (DamageTrack& d : damage) {
        void* o = ue::WeakGet(d.comp);
        if (!o) continue;
        int vo = d.offset + 4, bo = d.offset + 8;
        float value = ue::At<float>(o, vo);
        if (d.viaBase) {
            if (ue::At<float>(o, bo) == d.baseWritten) {
                ue::Put<float>(o, bo, d.baseNatural);
                if (d.mult > 0) ue::Put<float>(o, vo, value / d.mult);
            }
        } else if (value == d.valueWritten) {
            ue::Put<float>(o, vo, d.valueNatural);
        }
    }
    damage.clear();
    damageLogged = false;
}

bool Listed(void* comp, const std::vector<void*>& weapons) {
    if (!comp) return false;
    void* w = ue::OuterOf(comp);
    if (w && std::find(weapons.begin(), weapons.end(), w) != weapons.end()) return true;
    void* p = ue::Ptr(comp, "WeaponPrivate");
    return p && std::find(weapons.begin(), weapons.end(), p) != weapons.end();
}

void PruneTracked(std::vector<Tracked>& list, const std::vector<void*>& weapons) {
    for (auto it = list.begin(); it != list.end();) {
        void* o = ue::WeakGet(it->obj);
        if (o && Listed(o, weapons)) {
            ++it;
            continue;
        }
        if (o && ue::At<float>(o, it->offset) == it->written) ue::Put<float>(o, it->offset, it->natural);
        it = list.erase(it);
    }
}

void PruneFlags(const std::vector<void*>& weapons) {
    for (auto it = infiniteFlags.begin(); it != infiniteFlags.end();) {
        void* o = ue::WeakGet(it->obj);
        if (o && Listed(o, weapons)) {
            ++it;
            continue;
        }
        if (o) ue::WriteBool(o, it->prop, it->value);
        it = infiniteFlags.erase(it);
    }
}

void PruneDamage(const std::vector<void*>& weapons) {
    for (auto it = damage.begin(); it != damage.end();) {
        void* o = ue::WeakGet(it->comp);
        if (o && Listed(o, weapons)) {
            ++it;
            continue;
        }
        if (o) {
            int vo = it->offset + 4, bo = it->offset + 8;
            float value = ue::At<float>(o, vo);
            if (it->viaBase) {
                if (ue::At<float>(o, bo) == it->baseWritten) {
                    ue::Put<float>(o, bo, it->baseNatural);
                    if (it->mult > 0) ue::Put<float>(o, vo, value / it->mult);
                }
            } else if (value == it->valueWritten) {
                ue::Put<float>(o, vo, it->valueNatural);
            }
        }
        it = damage.erase(it);
    }
}

struct ShotMod {
    ue::FWeak mod;
    ue::FWeak pawn;
    bool registered = false;
    bool failed = false;
    double failedAt = 0;
} shot;

void* SpawnShotModifier() {
    ue::Call c(Lib("GameplayStatics"), "SpawnObject");
    c.Set<void*>("ObjectClass", Cls("WeaponShotModifier")).Set<void*>("Outer", st.pawn);
    if (!c.Run()) return nullptr;
    void* o = c.Get<void*>("ReturnValue");
    return ue::Alive(o) ? o : nullptr;
}

void ConfigureShot(void* mod) {
    void* cls = ue::ClassOf(mod);
    ue::Prop critP = ue::FindProp(cls, "CriticalHitModifier");
    if (critP) ue::Put<uint8_t>(mod, critP.offset, 1);
    ue::Prop useDef = ue::FindProp(cls, "bUseDefaultCriticalHitMultiplier");
    if (useDef) ue::WriteBool(mod, useDef, true);
    ue::Prop md = ue::FindProp(cls, "bModifyDamage");
    if (md) ue::WriteBool(mod, md, false);
    ue::Prop every = ue::FindProp(cls, "bTriggerEveryXShots");
    if (every) ue::WriteBool(mod, every, false);
}

bool RegisterShot(void* pawn, void* mod, bool reg) {
    void* causer = pawn ? ue::Ptr(pawn, "OakDamageCauserComponent") : nullptr;
    if (!causer) return false;
    ue::Call c(causer, reg ? "RegisterWeaponShotModifier" : "UnregisterWeaponShotModifier");
    c.Set<void*>("ModifierOwner", pawn).Set<void*>("Modifier", mod);
    if (reg) c.Set<void*>("ContextOverride", nullptr);
    return c.Run();
}

void DropShot() {
    void* mod = ue::WeakGet(shot.mod);
    void* pawn = ue::WeakGet(shot.pawn);
    if (shot.registered && mod && pawn) RegisterShot(pawn, mod, false);
    bool failed = shot.failed;
    double failedAt = shot.failedAt;
    shot = {};
    shot.failed = failed;
    shot.failedAt = failedAt;
}

void UpdateShot(bool crit) {
    if (shot.registered && (!crit || ue::WeakGet(shot.pawn) != st.pawn || !ue::WeakGet(shot.mod))) DropShot();
    if (shot.failed && st.time - shot.failedAt > 10.0) shot.failed = false;
    if (!crit || shot.failed || shot.registered || !st.pawn) return;
    void* mod = SpawnShotModifier();
    if (!mod) {
        shot.failed = true;
        shot.failedAt = st.time;
        LOG("could not create the critical hit modifier");
        return;
    }
    ConfigureShot(mod);
    if (!RegisterShot(st.pawn, mod, true)) {
        shot.failed = true;
        shot.failedAt = st.time;
        LOG("could not register the critical hit modifier");
        return;
    }
    shot.mod = ue::MakeWeak(mod);
    shot.pawn = ue::MakeWeak(st.pawn);
    shot.registered = true;
}

struct RegionSave {
    ue::FWeak data;
    ue::Prop prop;
};
std::vector<RegionSave> critRegions;
std::unordered_set<int64_t> critKnown;
struct KindCache {
    const void* ptr = nullptr;
    int kind = 0;
    double at = -100;
};
std::unordered_map<int64_t, KindCache> actorKind;
double critAt = -100;
bool critLogged = false;

int64_t Key(const ue::FWeak& w) { return ((int64_t)w.index << 32) | (uint32_t)w.serial; }

void RegionsOf(void* actor, std::vector<void*>& out) {
    void* d = DamageComp(actor);
    void* hrc = d ? ue::Ptr(d, "CachedHitRegionComponent") : nullptr;
    if (!ue::Alive(hrc)) hrc = ComponentOf(actor, "HitRegionComponent");
    if (!hrc) return;
    void* def = ue::Ptr(hrc, "DefaultHitRegion");
    if (ue::Valid(def)) out.push_back(def);
    ue::Prop p = ue::FindProp(ue::ClassOf(hrc), "HitRegions");
    if (!p) return;
    static int stride = 0;
    if (!stride) {
        stride = ue::StructSize(Cls("HitRegionState"));
        if (stride < 0x10 || stride > 0x200) stride = 0x70;
    }
    auto list = ue::Read<ue::TArray<uint8_t>>(hrc, p);
    for (int i = 0; list.data && i < list.num && i < 64; ++i) {
        void* data = ue::At<void*>(list.data + (size_t)i * stride, 0);
        if (ue::Valid(data)) out.push_back(data);
    }
}

void MarkCritRegions() {
    if (st.time - critAt < 0.5 || !st.pawn) return;
    critAt = st.time;
    void* hrd = Cls("HitRegionData");
    if (!hrd) return;
    if (actorKind.size() > 4096) actorKind.clear();
    std::unordered_set<void*> skip;
    {
        std::vector<void*> mine;
        RegionsOf(st.pawn, mine);
        skip.insert(mine.begin(), mine.end());
    }
    ue::FVector me = QuickLocation(st.pawn);
    std::vector<void*> hostile;
    for (void* a : Objects(Scan::Characters)) {
        if (!ue::Alive(a) || a == st.pawn) continue;
        if (!(Dist(me, QuickLocation(a)) < 10000.0f)) continue;
        int64_t key = Key(ue::MakeWeak(a));
        auto it = actorKind.find(key);
        int kind = 0;
        if (it != actorKind.end() && it->second.ptr == a && st.time - it->second.at < 3.0) {
            kind = it->second.kind;
        } else {
            if (IsPlayerPawn(a)) {
                kind = 2;
            } else {
                bool strict = false, loose = false;
                int att = Hostility(a, strict, loose);
                kind = loose ? 1 : (att == 0 ? 2 : 0);
            }
            actorKind[key] = {a, kind, st.time};
        }
        if (kind == 1) {
            hostile.push_back(a);
            continue;
        }
        if (kind != 2) continue;
        std::vector<void*> friendly;
        RegionsOf(a, friendly);
        skip.insert(friendly.begin(), friendly.end());
    }
    for (auto it = critRegions.begin(); it != critRegions.end();) {
        void* o = ue::WeakGet(it->data);
        if (o && skip.count(o)) {
            ue::WriteBool(o, it->prop, false);
            critKnown.erase(Key(it->data));
            it = critRegions.erase(it);
            continue;
        }
        ++it;
    }
    int added = 0;
    for (void* a : hostile) {
        std::vector<void*> regs;
        RegionsOf(a, regs);
        for (void* r : regs) {
            if (skip.count(r) || !ue::IsA(r, hrd)) continue;
            ue::Prop cp = ue::FindProp(ue::ClassOf(r), "bCriticalHit");
            if (!cp || ue::ReadBool(r, cp)) continue;
            ue::FWeak w = ue::MakeWeak(r);
            if (!critKnown.insert(Key(w)).second) continue;
            critRegions.push_back({w, cp});
            ue::WriteBool(r, cp, true);
            ++added;
        }
    }
    if (added && !critLogged) {
        critLogged = true;
        LOG("always crits: %d enemy body parts now count as weak spots", added);
    }
}

void RestoreCritRegions() {
    for (RegionSave& r : critRegions) {
        void* o = ue::WeakGet(r.data);
        if (o) ue::WriteBool(o, r.prop, false);
    }
    critRegions.clear();
    critKnown.clear();
    actorKind.clear();
    critLogged = false;
}

struct LevelSave {
    ue::FWeak obj;
    ue::Prop prop;
    bool value = false;
};
std::vector<LevelSave> levelFlags;
std::unordered_set<int64_t> levelKnown;
bool levelReqActive = false;
size_t levelScanned = 0;

void LevelRequirements(bool want) {
    if (!want) {
        if (levelReqActive) {
            for (LevelSave& f : levelFlags) {
                void* o = ue::WeakGet(f.obj);
                if (o) ue::WriteBool(o, f.prop, f.value);
            }
            levelFlags.clear();
            levelKnown.clear();
            levelReqActive = false;
            levelScanned = 0;
        }
        return;
    }
    levelReqActive = true;
    const std::vector<void*>& data = Objects(Scan::InventoryData);
    if (levelScanned > data.size()) levelScanned = 0;
    size_t end = std::min(data.size(), levelScanned + 400);
    for (size_t i = levelScanned; i < end; ++i) {
        void* d = data[i];
        if (!ue::Alive(d)) continue;
        ue::Prop p = ue::FindProp(ue::ClassOf(d), "bUsesPlayerLevelRequirement");
        if (!p || !ue::ReadBool(d, p)) continue;
        ue::FWeak w = ue::MakeWeak(d);
        if (!levelKnown.insert(((int64_t)w.index << 32) | (uint32_t)w.serial).second) continue;
        levelFlags.push_back({w, p, true});
        ue::WriteBool(d, p, false);
    }
    levelScanned = end >= data.size() ? 0 : end;
}

void RefillAllAmmo() {
    void* ammoCls = Cls("GbxAmmoTypeData");
    int filled = 0;
    std::vector<void*> resources;
    ue::ForEachOf(ammoCls, [&](void* t) {
        void* r = ue::Ptr(t, "ResourceData");
        if (r && std::find(resources.begin(), resources.end(), r) == resources.end()) resources.push_back(r);
        return true;
    });
    for (void* r : resources) {
        PoolRef ref;
        float cur = 0, mx = 0;
        if (!PoolFor(r, ref) || !PoolValues(ref, cur, mx)) continue;
        RefillPool(ref);
        ++filled;
    }
    RefillGrenades();
    Note(filled ? "Ammo and grenades refilled" : "No ammo found to refill");
}

}

void RefillAllAmmoNow() { RefillAllAmmo(); }

bool CritMarked(void* region) { return region && critKnown.count(Key(ue::MakeWeak(region))) > 0; }

void WeaponsInit() {
    bool ammoLib = HasFunc("OakBlueprintLibrary", "ResourceLockPlayerInfiniteAmmo") && HasFunc("OakBlueprintLibrary", "ResourceUnlockPlayerInfiniteAmmo");
    bool pools = HasFunc("GameResourcePoolFunctionLibrary", "BreakResourcePoolReference") &&
                 HasFunc("GameResourcePoolFunctionLibrary", "SetResourcePoolValue") && HasFunc("GameResourcePoolManagerComponent", "GetResourcePoolByResource");
    bool mag = Has("WeaponAmmoPoolComponent", "LoadedAmmo") && Has("WeaponAmmoPoolComponent", "MaxLoadedAmmo");
    Ready("unlimited_ammo", mag || ammoLib);
    Ready("no_reload", pools && mag && Has("WeaponAmmoPoolComponent", "ResourcePool"));
    Ready("instant_reload", Has("WeaponReloadComponent", "ReloadTime"));
    Ready("fire_rate", Has("WeaponFireComponent", "FireRate"));
    Ready("no_recoil", Has("WeaponRecoilPatternComponent", "PatternHeightScale") && Has("WeaponFireComponent", "Spread"));
    Ready("no_charge", Has("WeaponChargeComponent", "ChargeTime"));
    Ready("no_overheat", Has("WeaponHeatComponent", "Heat") && Has("WeaponHeatComponent", "UseHeatImpulse"));
    Ready("damage_mult", Has("WeaponFireComponent", "Damage"));
    Ready("one_hit_kill", Has("WeaponFireComponent", "Damage"));
    bool shots = HasFunc("OakDamageCauserComponent", "RegisterWeaponShotModifier") && HasFunc("GameplayStatics", "SpawnObject") &&
                 Has("WeaponShotModifier", "CriticalHitModifier");
    Ready("always_crit", shots || Has("HitRegionData", "bCriticalHit"));
    Ready("ignore_level_req", Has("InventoryData", "bUsesPlayerLevelRequirement"));
    Ready("refill_ammo", pools && Has("GbxAmmoTypeData", "ResourceData"));
}

void WeaponsTick() {
    bool unlimited = On("unlimited_ammo");
    InfiniteAmmo(unlimited);
    std::vector<void*> weapons = PlayerWeapons();
    void* held = HeldWeapon();
    static bool ammoLogged = false, reloadLogged = false;
    if (unlimited && held && !ammoLogged) {
        ammoLogged = true;
        LogMagazine("unlimited ammo", held);
    }
    if (!unlimited) ammoLogged = false;
    if (On("no_reload") && held && !reloadLogged) {
        reloadLogged = true;
        LogMagazine("no reload", held);
    }
    if (!On("no_reload")) reloadLogged = false;
    if (unlimited) {
        for (void* w : weapons)
            for (void* a : Parts(w, false)) {
                KeepInfiniteFlag(a);
                FillMagazine(a, false);
                if ((st.tick % 20) == 0) TopUpReserve(a);
            }
        if ((st.tick % 30) == 0) RefillGrenades();
    } else if (!infiniteFlags.empty()) {
        RestoreInfiniteFlags();
    }
    if (On("no_reload") && !unlimited && held)
        for (void* a : Parts(held, false)) FillMagazine(a, true);

    bool instant = On("instant_reload"), recoil = On("no_recoil"), charge = On("no_charge"), heat = On("no_overheat");
    float rate = Value("fire_rate");
    float mult = On("one_hit_kill") ? 1000000.0f : std::max(1.0f, Value("damage_mult"));
    bool boost = mult > 1.001f;
    for (void* w : weapons) {
        if (instant) {
            void* r = ComponentOf(w, "WeaponReloadComponent");
            if (r) {
                SetAttr(reloadAttrs, r, "ReloadTime", 0.05f);
                if (ue::FindProp(ue::ClassOf(r), "TapedReloadTime")) SetAttr(reloadAttrs, r, "TapedReloadTime", 0.05f);
            }
        }
        if (rate > 1.001f || recoil || boost) {
            for (void* fire : Parts(w, true)) {
                if (rate > 1.001f) ScaleAttr(fireAttrs, fire, "FireRate", rate);
                if (boost) ScaleDamage(fire, mult);
                if (recoil) {
                    SetAttr(recoilAttrs, fire, "Spread", 0.0f);
                    SetAttr(recoilAttrs, fire, "AccuracyImpulse", 0.0f);
                    SetAttr(recoilAttrs, fire, "BurstAccuracyImpulseScale", 0.0f);
                }
            }
        }
        if (recoil) {
            void* pat = ComponentOf(w, "WeaponRecoilPatternComponent");
            if (pat) {
                SetAttr(recoilAttrs, pat, "PatternWidthScale", 0.0f);
                SetAttr(recoilAttrs, pat, "PatternHeightScale", 0.0f);
                SetAttr(recoilAttrs, pat, "PatternZoomScale", 0.0f);
            }
            void* sway = ComponentOf(w, "WeaponSwayComponent");
            if (sway) {
                SetAttr(recoilAttrs, sway, "Scale", 0.0f);
                SetAttr(recoilAttrs, sway, "AccuracyScale", 0.0f);
                SetAttr(recoilAttrs, sway, "ZoomScale", 0.0f);
                SetAttr(recoilAttrs, sway, "ZoomAccuracyScale", 0.0f);
            }
        }
        if (charge) {
            void* c = ComponentOf(w, "WeaponChargeComponent");
            if (c) SetAttr(chargeAttrs, c, "ChargeTime", 0.01f);
        }
        if (heat) {
            void* h = ComponentOf(w, "WeaponHeatComponent");
            if (h) {
                SetAttr(heatAttrs, h, "UseHeatImpulse", 0.0f);
                ue::SetFloat(h, "Heat", 0.0f);
            }
        }
    }
    if (!instant && !reloadAttrs.empty()) RestoreAttrs(reloadAttrs);
    if (rate <= 1.001f && !fireAttrs.empty()) RestoreAttrs(fireAttrs);
    if (!recoil && !recoilAttrs.empty()) RestoreAttrs(recoilAttrs);
    if (!charge && !chargeAttrs.empty()) RestoreAttrs(chargeAttrs);
    if (!heat && !heatAttrs.empty()) RestoreAttrs(heatAttrs);
    if (!boost && !damage.empty()) RestoreDamage();
    static double pruneAt = -100;
    if (st.time - pruneAt > 0.5) {
        pruneAt = st.time;
        for (std::vector<Tracked>* l : {&reloadAttrs, &fireAttrs, &recoilAttrs, &chargeAttrs, &heatAttrs})
            if (!l->empty()) PruneTracked(*l, weapons);
        if (!infiniteFlags.empty()) PruneFlags(weapons);
        if (!damage.empty()) PruneDamage(weapons);
    }

    bool always = On("always_crit");
    UpdateShot(always || (On("silent_aim") && On("silent_headshot") && st.alone));
    if (always && st.alone) MarkCritRegions();
    else if (!critRegions.empty()) RestoreCritRegions();

    LevelRequirements(On("ignore_level_req") && st.alone);
    if (Pressed("refill_ammo")) RefillAllAmmo();
}

void WeaponsOff() {
    InfiniteAmmo(false);
    RestoreInfiniteFlags();
    RestoreAttrs(reloadAttrs);
    RestoreAttrs(fireAttrs);
    RestoreAttrs(recoilAttrs);
    RestoreAttrs(chargeAttrs);
    RestoreAttrs(heatAttrs);
    RestoreDamage();
    if (shot.registered) DropShot();
    RestoreCritRegions();
    LevelRequirements(false);
}

}
