#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cmath>

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
std::vector<Tracked> reloadAttrs, fireAttrs, recoilAttrs, chargeAttrs, heatAttrs, damageAttrs;

struct ShotMod {
    ue::FWeak mod;
    ue::FWeak pawn;
    bool registered = false;
    bool failed = false;
    float mult = 1.0f;
    bool crit = false;
} shot;

struct FlagSave {
    ue::FWeak obj;
    ue::Prop prop;
    bool value = false;
};
std::vector<FlagSave> levelFlags;
bool levelReqActive = false;
size_t levelScanned = 0;

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

void KeepMagazineFull(void* weapon) {
    void* ammo = ComponentOf(weapon, "WeaponAmmoPoolComponent");
    if (!ammo) return;
    ue::Prop lp = ue::FindProp(ue::ClassOf(ammo), "LoadedAmmo");
    ue::Prop mp = ue::FindProp(ue::ClassOf(ammo), "MaxLoadedAmmo");
    if (!lp || !mp) return;
    int loaded = ue::At<int32_t>(ammo, lp.offset);
    int mx = ue::At<int32_t>(ammo, mp.offset + 4);
    if (mx <= 0 || mx > 100000 || loaded >= mx || loaded < 0) return;
    PoolRef ref;
    float cur = 0, top = 0;
    if (!ReadPool(ammo, "ResourcePool", ref) || !PoolValues(ref, cur, top)) return;
    int spare = (int)std::floor(cur + 0.001f);
    int move = std::min(mx - loaded, spare);
    if (move <= 0) return;
    ue::Put<int32_t>(ammo, lp.offset, loaded + move);
    SetPool(ref, cur - (float)move);
}

void* ShotModifierClass() { return Cls("WeaponShotModifier"); }

void* SpawnShotModifier() {
    void* gs = Lib("GameplayStatics");
    ue::Call c(gs, "SpawnObject");
    c.Set<void*>("ObjectClass", ShotModifierClass()).Set<void*>("Outer", st.pawn);
    if (!c.Run()) return nullptr;
    void* o = c.Get<void*>("ReturnValue");
    return ue::Alive(o) ? o : nullptr;
}

void ConfigureShot(void* mod, float mult, bool crit) {
    void* cls = ue::ClassOf(mod);
    ue::Prop critP = ue::FindProp(cls, "CriticalHitModifier");
    if (critP) ue::Put<uint8_t>(mod, critP.offset, crit ? 1 : 0);
    ue::Prop useDef = ue::FindProp(cls, "bUseDefaultCriticalHitMultiplier");
    if (useDef) ue::WriteBool(mod, useDef, true);
    ue::Prop md = ue::FindProp(cls, "bModifyDamage");
    if (md) ue::WriteBool(mod, md, mult > 1.001f);
    ue::Prop type = ue::FindProp(cls, "DamageModifierType");
    if (type) ue::Put<uint8_t>(mod, type.offset, 0);
    ue::Prop dm = ue::FindProp(cls, "DamageModifier");
    if (dm) {
        ue::Put<float>(mod, dm.offset, mult);
        ue::Put<float>(mod, dm.offset + 0x30, 1.0f);
    }
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
    shot = {};
    shot.failed = failed;
}

void UpdateShot(float mult, bool crit) {
    bool want = mult > 1.001f || crit;
    if (shot.registered && (!want || ue::WeakGet(shot.pawn) != st.pawn || !ue::WeakGet(shot.mod))) DropShot();
    if (!want || shot.failed) return;
    if (!shot.registered) {
        void* mod = SpawnShotModifier();
        if (!mod) {
            shot.failed = true;
            LOG("could not create the shot modifier, using weapon damage instead");
            return;
        }
        ConfigureShot(mod, mult, crit);
        if (!RegisterShot(st.pawn, mod, true)) {
            shot.failed = true;
            LOG("could not register the shot modifier, using weapon damage instead");
            return;
        }
        shot.mod = ue::MakeWeak(mod);
        shot.pawn = ue::MakeWeak(st.pawn);
        shot.registered = true;
        shot.mult = mult;
        shot.crit = crit;
        return;
    }
    if (shot.mult != mult || shot.crit != crit) {
        ConfigureShot(ue::WeakGet(shot.mod), mult, crit);
        shot.mult = mult;
        shot.crit = crit;
    }
}

void LevelRequirements(bool want) {
    if (!want) {
        if (levelReqActive) {
            for (FlagSave& f : levelFlags) {
                void* o = ue::WeakGet(f.obj);
                if (o) ue::WriteBool(o, f.prop, f.value);
            }
            levelFlags.clear();
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
        bool known = false;
        for (FlagSave& f : levelFlags)
            if (ue::WeakGet(f.obj) == d) known = true;
        if (known) continue;
        levelFlags.push_back({ue::MakeWeak(d), p, true});
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

void WeaponsInit() {
    bool ammoLib = HasFunc("OakBlueprintLibrary", "ResourceLockPlayerInfiniteAmmo") && HasFunc("OakBlueprintLibrary", "ResourceUnlockPlayerInfiniteAmmo");
    bool pools = HasFunc("GameResourcePoolFunctionLibrary", "BreakResourcePoolReference") &&
                 HasFunc("GameResourcePoolFunctionLibrary", "SetResourcePoolValue") && HasFunc("GameResourcePoolManagerComponent", "GetResourcePoolByResource");
    Ready("unlimited_ammo", ammoLib);
    Ready("no_reload", pools && Has("WeaponAmmoPoolComponent", "LoadedAmmo") && Has("WeaponAmmoPoolComponent", "ResourcePool"));
    Ready("instant_reload", Has("WeaponReloadComponent", "ReloadTime"));
    Ready("fire_rate", Has("WeaponFireComponent", "FireRate"));
    Ready("no_recoil", Has("WeaponRecoilPatternComponent", "PatternHeightScale") && Has("WeaponFireComponent", "Spread"));
    Ready("no_charge", Has("WeaponChargeComponent", "ChargeTime"));
    Ready("no_overheat", Has("WeaponHeatComponent", "Heat") && Has("WeaponHeatComponent", "UseHeatImpulse"));
    bool shots = HasFunc("OakDamageCauserComponent", "RegisterWeaponShotModifier") && HasFunc("GameplayStatics", "SpawnObject") &&
                 Has("WeaponShotModifier", "CriticalHitModifier");
    Ready("damage_mult", shots || Has("WeaponFireComponent", "Damage"));
    Ready("one_hit_kill", shots || Has("WeaponFireComponent", "Damage"));
    Ready("always_crit", shots);
    Ready("ignore_level_req", Has("InventoryData", "bUsesPlayerLevelRequirement"));
    Ready("refill_ammo", pools && Has("GbxAmmoTypeData", "ResourceData"));
}

void WeaponsTick() {
    bool unlimited = On("unlimited_ammo");
    InfiniteAmmo(unlimited);
    if (unlimited && (st.tick % 30) == 0) RefillGrenades();
    std::vector<void*> weapons = PlayerWeapons();
    void* held = HeldWeapon();
    if (On("no_reload") && !unlimited && held) KeepMagazineFull(held);

    bool instant = On("instant_reload"), recoil = On("no_recoil"), charge = On("no_charge"), heat = On("no_overheat");
    float rate = Value("fire_rate");
    for (void* w : weapons) {
        if (instant) {
            void* r = ComponentOf(w, "WeaponReloadComponent");
            if (r) {
                SetAttr(reloadAttrs, r, "ReloadTime", 0.05f);
                if (ue::FindProp(ue::ClassOf(r), "TapedReloadTime")) SetAttr(reloadAttrs, r, "TapedReloadTime", 0.05f);
            }
        }
        void* fire = (rate > 1.001f || recoil) ? ComponentOf(w, "WeaponFireComponent") : nullptr;
        if (fire && rate > 1.001f) ScaleAttr(fireAttrs, fire, "FireRate", rate);
        if (recoil) {
            if (fire) {
                SetAttr(recoilAttrs, fire, "Spread", 0.0f);
                SetAttr(recoilAttrs, fire, "AccuracyImpulse", 0.0f);
                SetAttr(recoilAttrs, fire, "BurstAccuracyImpulseScale", 0.0f);
            }
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

    float mult = On("one_hit_kill") ? 1000000.0f : std::max(1.0f, Value("damage_mult"));
    bool crit = On("always_crit") || (On("silent_aim") && On("silent_headshot") && st.alone);
    UpdateShot(mult, crit);
    if (shot.failed && mult > 1.001f && held) {
        void* fire = ComponentOf(held, "WeaponFireComponent");
        if (fire) ScaleAttr(damageAttrs, fire, "Damage", mult);
    } else if (!damageAttrs.empty()) {
        RestoreAttrs(damageAttrs);
    }

    LevelRequirements(On("ignore_level_req") && st.alone);
    if (Pressed("refill_ammo")) RefillAllAmmo();
}

void WeaponsOff() {
    InfiniteAmmo(false);
    RestoreAttrs(reloadAttrs);
    RestoreAttrs(fireAttrs);
    RestoreAttrs(recoilAttrs);
    RestoreAttrs(chargeAttrs);
    RestoreAttrs(heatAttrs);
    RestoreAttrs(damageAttrs);
    if (shot.registered) DropShot();
    LevelRequirements(false);
}

}
