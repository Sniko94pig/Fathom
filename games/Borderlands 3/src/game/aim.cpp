#include "game/internal.h"
#include "ingame/log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace bl3 {

namespace {

struct Enemy {
    void* actor = nullptr;
    ue::FVector feet;
    ue::FVector head;
    float height = 0;
    float dist = 0;
    float health = -1;
    bool badass = false;
};

std::vector<Enemy> enemies;
double enemiesAt = 0;

struct HeadCache {
    ue::FWeak actor;
    ue::FName bone;
    bool tried = false;
};
std::unordered_map<void*, HeadCache> heads;

void* HitRegionDataClass() {
    static void* c = nullptr;
    if (!c) c = Cls("HitRegionData");
    return c;
}

ue::FName CritBone(void* actor) {
    auto it = heads.find(actor);
    if (it != heads.end() && ue::WeakGet(it->second.actor) == actor && it->second.tried) return it->second.bone;
    HeadCache hc;
    hc.actor = ue::MakeWeak(actor);
    hc.tried = true;
    void* d = DamageComp(actor);
    void* hrc = d ? ue::Ptr(d, "CachedHitRegionComponent") : nullptr;
    if (!hrc) hrc = ComponentOf(actor, "HitRegionComponent");
    if (hrc) {
        ue::Prop p = ue::FindProp(ue::ClassOf(hrc), "HitRegions");
        auto list = ue::Read<ue::TArray<uint8_t>>(hrc, p);
        for (int i = 0; p && list.data && i < list.num && i < 64; ++i) {
            void* data = ue::At<void*>(list.data + (size_t)i * 0x70, 0);
            if (!ue::Valid(data) || !ue::IsA(data, HitRegionDataClass())) continue;
            if (!ue::GetBool(data, "bCriticalHit")) continue;
            ue::Prop bp = ue::FindProp(ue::ClassOf(data), "BoneNames");
            auto bones = ue::Read<ue::TArray<ue::FName>>(data, bp);
            if (bones.num > 0 && bones.data) {
                hc.bone = ue::At<ue::FName>(bones.data, 0);
                break;
            }
        }
    }
    if (hc.bone.index == 0) hc.bone = ue::MakeName(L"Head");
    if (heads.size() > 512) heads.clear();
    heads[actor] = hc;
    return hc.bone;
}

bool HeadOf(void* actor, const ue::FVector& feet, float height, ue::FVector& out) {
    void* mesh = ue::Ptr(actor, "Mesh");
    if (mesh) {
        ue::FName bone = CritBone(actor);
        if (bone.index != 0) {
            ue::Call c(mesh, "GetSocketLocation");
            c.Set("InSocketName", bone);
            if (c.Run()) {
                ue::FVector v = c.Get<ue::FVector>("ReturnValue");
                ue::Call ml(mesh, "K2_GetComponentLocation");
                ue::FVector m = ml.Run() ? ml.Get<ue::FVector>("ReturnValue") : ue::FVector{};
                if (Dist(v, m) > 5.0f && Dist(v, feet) < 3000.0f) {
                    out = v;
                    return true;
                }
            }
        }
    }
    out = {feet.x, feet.y, feet.z + height * 0.9f};
    return false;
}

void RefreshEnemies(const ue::FVector& from) {
    if (st.time - enemiesAt < 0.05) return;
    enemiesAt = st.time;
    enemies.clear();
    for (void* a : Objects(Scan::Characters)) {
        if (!ue::Alive(a) || a == st.pawn) continue;
        ue::FVector center = Location(a);
        if (Dist(from, center) > 30000.0f) continue;
        if (Dead(a) || !IsHostile(a)) continue;
        Enemy e;
        e.actor = a;
        float half = 90.0f;
        void* cap = ue::Ptr(a, "CapsuleComponent");
        if (cap) {
            float h = ue::GetFloat(cap, "CapsuleHalfHeight", 90.0f);
            if (h > 5.0f && h < 2000.0f) half = h;
        }
        e.feet = {center.x, center.y, center.z - half};
        e.height = half * 2.0f;
        HeadOf(a, e.feet, e.height, e.head);
        e.dist = Dist(from, center) / 100.0f;
        float mh = MaxHealth(a);
        e.health = mh > 0 ? std::clamp(Health(a) / mh, 0.0f, 1.0f) : -1.0f;
        std::string cn = ue::Name(ue::ClassOf(a));
        e.badass = cn.find("Badass") != std::string::npos || cn.find("Boss") != std::string::npos;
        enemies.push_back(e);
        if (enemies.size() >= 128) break;
    }
}

bool Visible(void* actor, const ue::FVector& from) {
    ue::Call c(st.pc, "LineOfSightTo");
    c.Set<void*>("Other", actor).Set("ViewPoint", from).SetBool("bAlternateChecks", false);
    return c.Run() && c.GetBool("ReturnValue");
}

const Enemy* PickTarget(const ue::FVector& from, const ue::FRotator& view, float area, bool throughWalls) {
    const Enemy* best = nullptr;
    float bestAngle = area;
    for (const Enemy& e : enemies) {
        if (!ue::Alive(e.actor)) continue;
        float ang = AngleBetween(view, LookAt(from, e.head));
        if (ang > bestAngle) continue;
        if (!throughWalls && !Visible(e.actor, from)) continue;
        bestAngle = ang;
        best = &e;
    }
    return best;
}

float Norm180(float a) {
    while (a > 180.0f) a -= 360.0f;
    while (a < -180.0f) a += 360.0f;
    return a;
}

void AimAssist(const ue::FVector& from, const ue::FRotator& view) {
    if (!KeyDown(VK_RBUTTON) && !KeyDown(VK_LBUTTON)) return;
    const Enemy* t = PickTarget(from, view, std::clamp(Value("aim_area"), 5.0f, 90.0f), false);
    if (!t) return;
    ue::FRotator want = LookAt(from, t->head);
    ue::FRotator cur = ControlRotation();
    float strength = std::clamp(Value("aim_strength"), 1.0f, 10.0f);
    float alpha = strength >= 9.99f ? 1.0f : 1.0f - std::exp(-strength * strength * 1.5f * st.dt);
    float dp = Norm180(want.pitch - Norm180(cur.pitch)), dy = Norm180(want.yaw - cur.yaw);
    ue::FRotator next{Norm180(cur.pitch) + dp * alpha, cur.yaw + dy * alpha, 0.0f};
    SetControlRotation(next);
}

struct Bullet {
    ue::FWeak obj;
    double seen = 0;
};
std::vector<Bullet> handled;

struct WallSave {
    ue::FWeak data;
    ue::Prop prop;
    bool value = false;
};
std::vector<WallSave> walls;

void* Manager() {
    static ue::FWeak w;
    static double nextLook = 0;
    void* m = ue::WeakGet(w);
    if (m) return m;
    if (st.time < nextLook) return nullptr;
    nextLook = st.time + 2.0;
    m = ue::FindFirstOf(Cls("LightProjectileManager"));
    if (m) w = ue::MakeWeak(m);
    return m;
}

bool Handled(void* p) {
    for (Bullet& b : handled)
        if (ue::WeakGet(b.obj) == p) return true;
    return false;
}

void MakeWallPiercing(void* bullet) {
    void* data = ue::Ptr(bullet, "Data");
    if (!data) return;
    for (WallSave& w : walls)
        if (ue::WeakGet(w.data) == data) return;
    ue::Prop p = ue::FindProp(ue::ClassOf(data), "bPenetratesWorld");
    if (!p) return;
    walls.push_back({ue::MakeWeak(data), p, ue::ReadBool(data, p)});
    ue::WriteBool(data, p, true);
}

void RestoreWalls() {
    for (WallSave& w : walls) {
        void* d = ue::WeakGet(w.data);
        if (d) ue::WriteBool(d, w.prop, w.value);
    }
    walls.clear();
}

void SilentAim(const ue::FVector& from, const ue::FRotator& view) {
    void* mgr = Manager();
    if (!mgr) return;
    bool throughWalls = On("silent_walls");
    if (!throughWalls && !walls.empty()) RestoreWalls();
    float area = std::clamp(Value("silent_area"), 5.0f, 180.0f);
    handled.erase(std::remove_if(handled.begin(), handled.end(), [](const Bullet& b) { return st.time - b.seen > 3.0 || !ue::WeakGet(b.obj); }),
                  handled.end());
    for (const char* listName : {"ActiveProjectiles", "ProjectilesWaitingBatch"}) {
        ue::Prop lp = ue::FindProp(ue::ClassOf(mgr), listName);
        auto list = ue::Read<ue::TArray<void*>>(mgr, lp);
        for (int i = 0; lp && list.data && i < list.num && i < 512; ++i) {
            void* p = ue::At<void*>(list.data + i, 0);
            if (!ue::Alive(p) || Handled(p)) continue;
            ue::Call ins(p, "GetInstigator");
            if (!ins.Run() || ins.Get<void*>("ReturnValue") != st.pawn) continue;
            handled.push_back({ue::MakeWeak(p), st.time});
            if (throughWalls) MakeWallPiercing(p);
            const Enemy* t = PickTarget(from, view, area, throughWalls);
            if (!t) continue;
            ue::Prop locP = ue::FindProp(ue::ClassOf(p), "Location");
            ue::Prop velP = ue::FindProp(ue::ClassOf(p), "Velocity");
            if (locP && velP) {
                ue::FVector loc = ue::Read<ue::FVector>(p, locP);
                ue::FVector vel = ue::Read<ue::FVector>(p, velP);
                float speed = Len(vel);
                ue::FVector dir = Sub(t->head, loc);
                float len = Len(dir);
                if (speed > 1.0f && len > 1.0f) ue::Write<ue::FVector>(p, velP, Mul(dir, speed / len));
            }
            ue::Call home(p, "SetHomingTarget");
            home.Set<void*>("HomingTarget", t->actor).Set("TargetOffset", Sub(t->head, Location(t->actor)));
            if (home.Run()) ue::Call(p, "EnableHoming").Run();
            ue::SetFloat(p, "GravityScale", 0.0f);
        }
    }
}

std::string RarityKindName(int k) {
    switch (k) {
    case 1: return "Common";
    case 2: return "Uncommon";
    case 3: return "Rare";
    case 4: return "Epic";
    case 5: return "Legendary";
    default: return "Item";
    }
}

int RarityKind(void* pickup) {
    void* r = ue::Ptr(pickup, "AssociatedInventoryRarityData");
    if (!r) return 0;
    std::string n = ue::Name(r);
    if (n.find("Legendary") != std::string::npos || n.find("_05") != std::string::npos || n.find("_06") != std::string::npos ||
        n.find("Effervescent") != std::string::npos || n.find("Pearl") != std::string::npos)
        return 5;
    if (n.find("VeryRare") != std::string::npos || n.find("Epic") != std::string::npos || n.find("_04") != std::string::npos) return 4;
    if (n.find("Uncommon") != std::string::npos || n.find("_02") != std::string::npos) return 2;
    if (n.find("Rare") != std::string::npos || n.find("_03") != std::string::npos) return 3;
    if (n.find("Common") != std::string::npos || n.find("_01") != std::string::npos) return 1;
    return 0;
}

std::string PickupName(void* pickup, int kind) {
    void* data = ue::Ptr(pickup, "AssociatedInventoryData");
    std::string n = data ? ue::Name(data) : std::string();
    for (const char* pre : {"InvData_", "InventoryData_", "InvBalD_"})
        if (n.rfind(pre, 0) == 0) n = n.substr(std::strlen(pre));
    for (char& c : n)
        if (c == '_') c = ' ';
    if (n.empty()) n = RarityKindName(kind);
    return n;
}

double espAt = 0;

void PublishEsp(bool enemyOn, bool lootOn, const ue::FVector& camLoc, const ue::FRotator& camRot, float fov, bool camOk) {
    if (st.time - espAt < 0.05) return;
    espAt = st.time;
    std::vector<core::EspDot> dots, items;
    if (enemyOn) {
        for (const Enemy& e : enemies) {
            core::EspDot d;
            d.x = e.feet.x;
            d.y = e.feet.y;
            d.z = e.feet.z;
            d.top = e.feet.z + e.height;
            d.box = true;
            d.name = DisplayName(e.actor);
            d.dist = e.dist;
            d.health = e.health;
            d.kind = e.badass ? 1 : 0;
            dots.push_back(std::move(d));
        }
    }
    if (lootOn) {
        bool want[6] = {false, On("lootesp_common"), On("lootesp_uncommon"), On("lootesp_rare"), On("lootesp_epic"), On("lootesp_legendary")};
        for (void* p : Objects(Scan::Pickups)) {
            if (!ue::Alive(p) || items.size() >= 300) continue;
            int k = RarityKind(p);
            if (k > 0 && !want[k]) continue;
            if (k == 0 && !want[1]) continue;
            ue::FVector l = Location(p);
            float dist = Dist(camLoc, l) / 100.0f;
            if (dist > 400.0f) continue;
            core::EspDot d;
            d.x = l.x;
            d.y = l.y;
            d.z = l.z;
            d.top = l.z + 40.0f;
            d.name = PickupName(p, k);
            d.dist = dist;
            d.kind = k ? k : 1;
            items.push_back(std::move(d));
        }
        if (On("lootesp_chests")) {
            for (void* c : Objects(Scan::Lootables)) {
                if (!ue::Alive(c) || items.size() >= 400) continue;
                ue::Prop sp = ue::FindProp(ue::ClassOf(c), "CurrentInteractiveState");
                if (sp && ue::At<uint8_t>(c, sp.offset) != 1) continue;
                ue::FVector l = Location(c);
                float dist = Dist(camLoc, l) / 100.0f;
                if (dist > 300.0f) continue;
                core::EspDot d;
                d.x = l.x;
                d.y = l.y;
                d.z = l.z;
                d.top = l.z + 60.0f;
                d.name = DisplayName(c);
                d.dist = dist;
                d.kind = 6;
                items.push_back(std::move(d));
            }
        }
    }
    WithModel([&](core::Model& m) {
        m.esp = std::move(dots);
        m.itemEsp = std::move(items);
        m.camera.valid = camOk;
        m.camera.x = camLoc.x;
        m.camera.y = camLoc.y;
        m.camera.z = camLoc.z;
        m.camera.pitch = camRot.pitch;
        m.camera.yaw = camRot.yaw;
        m.camera.roll = camRot.roll;
        m.camera.fov = fov;
    });
}

bool espShown = false;

void ClearEsp() {
    if (!espShown) return;
    espShown = false;
    WithModel([](core::Model& m) {
        m.esp.clear();
        m.itemEsp.clear();
    });
}

void KillNearby(const ue::FVector& from) {
    float range = std::clamp(Value("kill_range"), 5.0f, 300.0f);
    void* lib = Lib("DamageStatics");
    void* dt = Cls("GbxDamageType");
    void* ds = Cls("DamageSource");
    int hit = 0;
    std::vector<void*> targets;
    for (void* a : Objects(Scan::Characters)) {
        if (!ue::Alive(a) || a == st.pawn || Dead(a) || !IsHostile(a)) continue;
        if (Dist(from, Location(a)) / 100.0f > range) continue;
        targets.push_back(a);
    }
    for (void* a : targets) {
        float h = Health(a);
        ue::Call c(lib, "ApplyDamage");
        c.Set<void*>("DamageReceiver", a).Set<float>("BaseDamage", std::max(1.0e9f, h * 100.0f)).Set<void*>("DamageType", dt);
        c.Set<void*>("DamageSource", ds).Set<void*>("EventInstigator", st.pc).Set<void*>("DamageCauser", st.pawn);
        if (c.Run()) ++hit;
    }
    int dead = 0;
    for (void* a : targets)
        if (!ue::Alive(a) || Dead(a)) ++dead;
    LOG("kill nearby: %zu targets, %d hits, %d dead", targets.size(), hit, dead);
    if (targets.empty()) Note("No enemies within range");
    else Note("Killed " + std::to_string(dead) + " of " + std::to_string(targets.size()) + " enemies nearby");
}

}

void AimInit() {
    bool chars = Cls("OakCharacter") && HasFunc("OakBlueprintLibrary", "GetAttitudeTowardPlayer");
    bool camera = HasFunc("PlayerCameraManager", "GetCameraLocation") && HasFunc("PlayerCameraManager", "GetFOVAngle");
    Ready("silent_aim", chars && camera && HasFunc("LightProjectile", "SetHomingTarget") && Has("LightProjectileManager", "ActiveProjectiles"));
    Ready("aim_assist", chars && camera && HasFunc("Controller", "SetControlRotation") && HasFunc("Controller", "LineOfSightTo"));
    Ready("enemy_esp", chars && camera);
    Ready("loot_esp", camera && Cls("InventoryItemPickup"));
    Ready("kill_nearby", chars && HasFunc("DamageStatics", "ApplyDamage"));
}

void AimTick() {
    bool silent = On("silent_aim") && st.alone;
    bool assist = On("aim_assist") && st.alone;
    bool enemyEsp = On("enemy_esp") && st.alone;
    bool lootEsp = On("loot_esp") && st.alone;
    bool kill = Pressed("kill_nearby");
    if (kill && !st.alone) {
        Note("Kill all nearby enemies only works when you play alone");
        kill = false;
    }
    if (!silent && !walls.empty()) RestoreWalls();
    if (!silent && !assist && !enemyEsp && !lootEsp && !kill) {
        ClearEsp();
        return;
    }
    ue::FVector camLoc;
    ue::FRotator camRot;
    float fov = 90.0f;
    bool camOk = CameraView(camLoc, camRot, fov);
    if (!camOk) {
        camLoc = Location(st.pawn);
        camRot = ControlRotation();
    }
    if (silent || assist || enemyEsp) RefreshEnemies(camLoc);
    if (kill) KillNearby(Location(st.pawn));
    if (assist && !st.menuOpen) AimAssist(camLoc, camRot);
    if (silent) SilentAim(camLoc, camRot);
    if (enemyEsp || lootEsp) {
        espShown = true;
        PublishEsp(enemyEsp, lootEsp, camLoc, camRot, fov, camOk);
    } else {
        ClearEsp();
    }
}

void AimOff() {
    RestoreWalls();
    handled.clear();
    ClearEsp();
}

}
