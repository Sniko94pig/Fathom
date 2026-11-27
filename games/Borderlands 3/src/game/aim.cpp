#include "game/internal.h"
#include "ingame/esp.h"
#include "ingame/log.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace bl3 {

namespace {

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

std::string Pretty(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '_') {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
            continue;
        }
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)raw[i - 1]) && !out.empty() && out.back() != ' ') out.push_back(' ');
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool Finite(const ue::FVector& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

struct Enemy {
    ue::FWeak w;
    void* actor = nullptr;
    bool strict = false;
    void* root = nullptr;
    ue::FVector loc, center, extent, head;
    float dist = 0;
    float health = -1;
    bool badass = false;
    bool visible = true;
    std::string name;
};

std::vector<Enemy> enemies;
double enemiesAt = -100;

struct Known {
    int32_t serial = -1;
    const void* ptr = nullptr;
    bool strict = false;
    std::string name;
    bool badass = false;
    ue::FName bone;
    bool boneTried = false;
    bool hostile = false;
    double hostileAt = -100;
    bool visible = true;
    double visibleAt = -100;
    float maxHealth = 0;
    double maxAt = -100;
    ue::FVector centerOff, extent, headOff;
    double boundsAt = -100;
    double headAt = -100;
};
std::unordered_map<int32_t, Known> known;

void* HitRegionDataClass() {
    static void* c = nullptr;
    if (!c) c = Cls("HitRegionData");
    return c;
}

int SceneOffset(const char* prop) {
    ue::Prop p = ue::FindProp(Cls("SceneComponent"), prop);
    return p ? p.offset : -1;
}

bool FastLocation(void* root, ue::FVector& out) {
    static int lo = -2, ao = -2;
    if (lo == -2) {
        lo = SceneOffset("RelativeLocation");
        ao = SceneOffset("AttachParent");
    }
    if (!root || lo < 0 || ao < 0) return false;
    if (ue::At<void*>(root, ao)) return false;
    out = ue::At<ue::FVector>(root, lo);
    return Finite(out);
}

ue::FName CritBone(void* actor) {
    void* d = DamageComp(actor);
    void* hrc = d ? ue::Ptr(d, "CachedHitRegionComponent") : nullptr;
    if (!ue::Alive(hrc)) hrc = ComponentOf(actor, "HitRegionComponent");
    ue::FName first, named;
    if (hrc) {
        ue::Prop p = ue::FindProp(ue::ClassOf(hrc), "HitRegions");
        static int stride = 0;
        if (!stride) {
            stride = ue::StructSize(Cls("HitRegionState"));
            if (stride < 0x10 || stride > 0x200) stride = 0x70;
        }
        auto list = ue::Read<ue::TArray<uint8_t>>(hrc, p);
        for (int i = 0; p && list.data && i < list.num && i < 64; ++i) {
            void* data = ue::At<void*>(list.data + (size_t)i * stride, 0);
            if (!ue::Valid(data) || !ue::IsA(data, HitRegionDataClass())) continue;
            if (!ue::GetBool(data, "bCriticalHit") || CritMarked(data)) continue;
            ue::Prop bp = ue::FindProp(ue::ClassOf(data), "BoneNames");
            auto bones = ue::Read<ue::TArray<ue::FName>>(data, bp);
            if (bones.num <= 0 || !bones.data) continue;
            ue::FName b = ue::At<ue::FName>(bones.data, 0);
            if (first.index == 0) first = b;
            std::string n = Lower(ue::Name(data) + " " + ue::NameToString(b));
            if (named.index == 0 && (n.find("head") != std::string::npos || n.find("crit") != std::string::npos || n.find("weak") != std::string::npos))
                named = b;
        }
    }
    ue::FName pick = named.index ? named : first;
    if (pick.index == 0) pick = ue::MakeName(L"Head");
    void* mesh = ue::Ptr(actor, "Mesh");
    if (mesh) {
        ue::Call c(mesh, "DoesSocketExist");
        c.Set("InSocketName", pick);
        if (c.Run() && !c.GetBool("ReturnValue")) pick = ue::FName{};
    }
    return pick;
}

Known& Info(void* a) {
    if (known.size() > 4096) known.clear();
    ue::FWeak w = ue::MakeWeak(a);
    Known& k = known[w.index];
    if (k.serial == w.serial && k.ptr == a) return k;
    k = Known{};
    k.serial = w.serial;
    k.ptr = a;
    void* tc = ue::Ptr(a, "TargetableComponent");
    if (ue::Alive(tc)) {
        ue::Call c(tc, "GetTargetNameString");
        if (c.Run()) k.name = ue::FStringToUtf8(c.Get<ue::FString>("ReturnValue"));
    }
    while (!k.name.empty() && (k.name.back() == ' ' || k.name.back() == '\n')) k.name.pop_back();
    if (k.name.empty() || k.name.size() > 48) k.name = DisplayName(a);
    std::string low = Lower(k.name + " " + ue::Name(ue::ClassOf(a)));
    for (const char* t : {"badass", "anointed", "boss", "elite", "chupacabratch"})
        if (low.find(t) != std::string::npos) k.badass = true;
    return k;
}

void Bounds(void* a, const ue::FVector& loc, ue::FVector& center, ue::FVector& extent) {
    void* mesh = ue::Ptr(a, "Mesh");
    if (mesh) {
        ue::Call b(Lib("KismetSystemLibrary"), "GetComponentBounds");
        b.Set<void*>("Component", mesh);
        if (b.Run()) {
            ue::FVector o = b.Get<ue::FVector>("Origin"), x = b.Get<ue::FVector>("BoxExtent");
            if (Finite(o) && Finite(x) && x.x > 5 && x.y > 5 && x.z > 5 && x.x < 2500 && x.y < 2500 && x.z < 2500 && Dist(o, loc) < 2500) {
                center = o;
                extent = x;
                return;
            }
        }
    }
    float half = 90.0f, r = 40.0f;
    void* cap = ue::Ptr(a, "CapsuleComponent");
    if (cap) {
        float h = ue::GetFloat(cap, "CapsuleHalfHeight", 90.0f), rr = ue::GetFloat(cap, "CapsuleRadius", 40.0f);
        if (h > 5.0f && h < 2000.0f) half = h;
        if (rr > 5.0f && rr < 2000.0f) r = rr;
    }
    center = loc;
    extent = {r, r, half};
}

ue::FVector HeadOf(void* a, Known& k, const ue::FVector& center, const ue::FVector& extent) {
    void* mesh = ue::Ptr(a, "Mesh");
    if (mesh) {
        if (!k.boneTried) {
            k.boneTried = true;
            k.bone = CritBone(a);
        }
        if (k.bone.index != 0) {
            ue::Call c(mesh, "GetSocketLocation");
            c.Set("InSocketName", k.bone);
            if (c.Run()) {
                ue::FVector v = c.Get<ue::FVector>("ReturnValue");
                if (Finite(v) && std::fabs(v.x - center.x) < extent.x + 150 && std::fabs(v.y - center.y) < extent.y + 150 &&
                    std::fabs(v.z - center.z) < extent.z + 150)
                    return v;
            }
        }
    }
    return {center.x, center.y, center.z + extent.z * 0.75f};
}

bool Visible(void* actor, const ue::FVector& from) {
    ue::Call c(st.pc, "LineOfSightTo");
    c.Set<void*>("Other", actor).Set("ViewPoint", from).SetBool("bAlternateChecks", false);
    return c.Run() && c.GetBool("ReturnValue");
}

void RefreshEnemies(const ue::FVector& from, bool precise) {
    if (st.time - enemiesAt >= 0.1) {
        enemiesAt = st.time;
        std::vector<Enemy> next;
        next.reserve(64);
        for (void* a : Objects(Scan::Characters)) {
            if (next.size() >= 128) break;
            if (!ue::Alive(a) || a == st.pawn) continue;
            ue::FVector loc = QuickLocation(a);
            float d = Dist(from, loc);
            if (!(d <= 30000.0f)) continue;
            Known& k = Info(a);
            if (st.time - k.hostileAt > 1.0) {
                Hostility(a, k.strict, k.hostile);
                k.hostileAt = st.time;
            }
            if (!k.hostile) continue;
            void* dc = DamageComp(a);
            if (!dc || ue::GetBool(dc, "bCurrentlyDead")) continue;
            ue::Call hc(dc, "GetCurrentHealth");
            float hp = hc.Run() ? hc.Get<float>("ReturnValue") : 0.0f;
            if (!(hp > 0.0f)) continue;
            if (st.time - k.maxAt > 2.0) {
                ue::Call mc(dc, "GetMaxHealth");
                k.maxHealth = mc.Run() ? mc.Get<float>("ReturnValue") : 0.0f;
                k.maxAt = st.time;
            }
            if (st.time - k.boundsAt > 0.5) {
                ue::FVector c, x;
                Bounds(a, loc, c, x);
                k.centerOff = Sub(c, loc);
                k.extent = x;
                k.boundsAt = st.time;
            }
            Enemy e;
            e.w = ue::MakeWeak(a);
            e.actor = a;
            e.root = ue::Ptr(a, "RootComponent");
            e.loc = loc;
            e.center = Add(loc, k.centerOff);
            e.extent = k.extent;
            if (st.time - k.headAt > (precise ? 0.1 : 0.5)) {
                k.headOff = Sub(HeadOf(a, k, e.center, e.extent), loc);
                k.headAt = st.time;
            }
            e.head = Add(loc, k.headOff);
            e.dist = d / 100.0f;
            e.health = k.maxHealth > 0 ? std::clamp(hp / k.maxHealth, 0.0f, 1.0f) : -1.0f;
            e.badass = k.badass;
            e.strict = k.strict;
            e.name = k.name;
            if (st.time - k.visibleAt > (precise ? 0.25 : 0.6)) {
                k.visible = d < 15000.0f && Visible(a, from);
                k.visibleAt = st.time;
            }
            e.visible = k.visible;
            next.push_back(std::move(e));
        }
        enemies.swap(next);
        return;
    }
    for (Enemy& e : enemies) {
        ue::FVector now;
        if (!ue::WeakGet(e.w) || !FastLocation(e.root, now)) continue;
        ue::FVector delta = Sub(now, e.loc);
        if (Len(delta) > 2000.0f) continue;
        e.loc = now;
        e.center = Add(e.center, delta);
        e.head = Add(e.head, delta);
        e.dist = Dist(from, now) / 100.0f;
    }
}

const Enemy* PickTarget(const ue::FVector& from, const ue::FRotator& view, float area, bool throughWalls) {
    const Enemy* best = nullptr;
    float bestAngle = area;
    for (const Enemy& e : enemies) {
        if (!e.strict) continue;
        if (!throughWalls && !e.visible) continue;
        if (!ue::WeakGet(e.w)) continue;
        float ang = AngleBetween(view, LookAt(from, e.head));
        if (!(ang <= bestAngle)) continue;
        bestAngle = ang;
        best = &e;
    }
    return best;
}

float Norm180(float a) {
    if (!std::isfinite(a)) return 0.0f;
    return std::remainder(a, 360.0f);
}

void AimAssist(const ue::FVector& from, const ue::FRotator& view) {
    if (!KeyDown(VK_RBUTTON) && !KeyDown(VK_LBUTTON)) return;
    const Enemy* t = PickTarget(from, view, std::clamp(Value("aim_area"), 5.0f, 90.0f), false);
    if (!t) return;
    ue::FRotator want = LookAt(from, t->head);
    ue::FRotator cur = ControlRotation();
    if (!std::isfinite(want.pitch) || !std::isfinite(want.yaw) || !std::isfinite(cur.pitch) || !std::isfinite(cur.yaw)) return;
    float strength = std::clamp(Value("aim_strength"), 1.0f, 10.0f);
    float alpha = strength >= 9.99f ? 1.0f : 1.0f - std::exp(-strength * strength * 1.5f * st.dt);
    float dp = Norm180(want.pitch - Norm180(cur.pitch)), dy = Norm180(want.yaw - cur.yaw);
    ue::FRotator next{Norm180(cur.pitch) + dp * alpha, cur.yaw + dy * alpha, 0.0f};
    SetControlRotation(next);
}

struct Bullet {
    int32_t serial = 0;
    void* ptr = nullptr;
    float start = 0;
    bool mine = false;
    bool changed = false;
    int tries = 0;
    float gravity = 1.0f;
    ue::FVector aim;
};
std::unordered_map<int32_t, Bullet> handled;

struct BulletProps {
    void* cls = nullptr;
    ue::Prop start, loc, vel, gravity, comp, damage, dtype, dsource;
} bp;

void BulletLayout(void* p) {
    void* cls = ue::ClassOf(p);
    if (cls == bp.cls) return;
    bp.cls = cls;
    bp.start = ue::FindProp(cls, "StartTime");
    bp.loc = ue::FindProp(cls, "Location");
    bp.vel = ue::FindProp(cls, "Velocity");
    bp.gravity = ue::FindProp(cls, "GravityScale");
    bp.comp = ue::FindProp(cls, "ComponentLocation");
    bp.damage = ue::FindProp(cls, "Damage");
    bp.dtype = ue::FindProp(cls, "DamageType");
    bp.dsource = ue::FindProp(cls, "DamageSource");
}

void UndoAllBullets() {
    for (auto& entry : handled) {
        Bullet& b = entry.second;
        if (!b.changed) continue;
        void* p = ue::WeakGet(ue::FWeak{entry.first, b.serial, b.ptr});
        if (p && bp.start && bp.gravity && ue::ClassOf(p) == bp.cls && ue::Read<float>(p, bp.start) == b.start) ue::Write<float>(p, bp.gravity, b.gravity);
        b.changed = false;
    }
    handled.clear();
}

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

Bullet* TrackBullet(void* p, bool& isNew) {
    ue::FWeak w = ue::MakeWeak(p);
    float start = bp.start ? ue::Read<float>(p, bp.start) : 0.0f;
    auto it = handled.find(w.index);
    if (it != handled.end() && it->second.serial == w.serial && it->second.start == start) {
        isNew = false;
        return &it->second;
    }
    Bullet& b = handled[w.index];
    b = Bullet{};
    b.serial = w.serial;
    b.ptr = p;
    b.start = start;
    isNew = true;
    return &b;
}

bool Mine(void* p, void* held) {
    ue::Call i(p, "GetInstigator");
    void* ins = i.Run() ? i.Get<void*>("ReturnValue") : nullptr;
    if (ins) return ins == st.pawn || (st.vehicle && ins == st.vehicle && DrivingAlone());
    ue::Call c(p, "GetDamageCauser");
    void* dc = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    return dc && (dc == st.pawn || dc == held);
}

void Redirect(void* p, Bullet& b, float lead) {
    if (!bp.loc || !bp.vel) return;
    ue::FVector loc = ue::Read<ue::FVector>(p, bp.loc);
    ue::FVector vel = ue::Read<ue::FVector>(p, bp.vel);
    if (!Finite(loc) || !Finite(vel)) return;
    ue::FVector dir = Sub(b.aim, loc);
    float len = Len(dir);
    if (!(len > 1.0f)) return;
    ue::FVector unit = Mul(dir, 1.0f / len);
    if (len > lead) {
        ue::FVector closeTo = Sub(b.aim, Mul(unit, lead));
        ue::Write<ue::FVector>(p, bp.loc, closeTo);
        if (bp.comp) ue::Write<ue::FVector>(p, bp.comp, closeTo);
    }
    ue::Write<ue::FVector>(p, bp.vel, Mul(unit, std::max(Len(vel), 4000.0f)));
    if (!b.changed) b.gravity = bp.gravity ? ue::Read<float>(p, bp.gravity) : 1.0f;
    if (bp.gravity) ue::Write<float>(p, bp.gravity, 0.0f);
    b.changed = true;
}

bool OffCourse(void* p, const Bullet& b) {
    ue::FVector loc = ue::Read<ue::FVector>(p, bp.loc);
    ue::FVector vel = ue::Read<ue::FVector>(p, bp.vel);
    ue::FVector dir = Sub(b.aim, loc);
    float dl = Len(dir), vl = Len(vel);
    if (!(dl > 30.0f) || !(vl > 1.0f)) return false;
    float cosv = (dir.x * vel.x + dir.y * vel.y + dir.z * vel.z) / (dl * vl);
    return cosv < 0.995f;
}

void Retire(void* p) {
    void* cls = ue::ClassOf(p);
    ue::Prop dmg = ue::FindProp(cls, "Damage"), radius = ue::FindProp(cls, "DamageRadius");
    ue::Prop life = ue::FindProp(cls, "Lifetime"), elapsed = ue::FindProp(cls, "ElapsedTime");
    if (dmg) ue::Write<float>(p, dmg, 0.0f);
    if (radius) ue::Write<float>(p, radius, 0.0f);
    if (bp.vel) {
        ue::FVector v = ue::Read<ue::FVector>(p, bp.vel);
        float len = Len(v);
        ue::Write<ue::FVector>(p, bp.vel, len > 1.0f ? Mul(v, 1.0f / len) : ue::FVector{});
    }
    if (bp.gravity) ue::Write<float>(p, bp.gravity, 0.0f);
    if (life && elapsed) {
        float e = ue::Read<float>(p, elapsed);
        if (std::isfinite(e) && e >= 0.0f) ue::Write<float>(p, life, e + 0.02f);
    }
}

struct WeakRaw {
    int32_t index = 0;
    int32_t serial = 0;
};

bool DirectHit(void* p, const Enemy& t, bool head, const ue::FVector& aim) {
    static void* hitStruct = Cls("HitResult");
    static void* inputStruct = Cls("PipelineDamageInput");
    static void* critStruct = Cls("CriticalHitDamageOverrides");
    int hsize = ue::StructSize(hitStruct), isize = ue::StructSize(inputStruct);
    if (hsize < 0x40 || hsize > 0x200 || isize < 0x100 || isize > 0x600 || !bp.damage) return false;
    float dmg = ue::Read<float>(p, bp.damage);
    if (!(dmg > 0.0f) || !std::isfinite(dmg)) return false;
    void* type = bp.dtype ? ue::Read<void*>(p, bp.dtype) : nullptr;
    void* source = bp.dsource ? ue::Read<void*>(p, bp.dsource) : nullptr;
    ue::FVector from = bp.loc ? ue::Read<ue::FVector>(p, bp.loc) : aim;
    if (!Finite(from)) from = aim;
    ue::FVector dir = Sub(aim, from);
    float len = Len(dir);
    ue::FVector unit = len > 1.0f ? Mul(dir, 1.0f / len) : ue::FVector{1.0f, 0.0f, 0.0f};
    ue::FVector normal = Mul(unit, -1.0f);
    std::vector<uint8_t> hit((size_t)hsize, 0);
    auto put = [&](const char* name, const void* value, int size) {
        ue::Prop q = ue::FindProp(hitStruct, name);
        if (q && q.offset + size <= hsize) std::memcpy(hit.data() + q.offset, value, (size_t)size);
    };
    int32_t none = -1;
    float one = 1.0f;
    put("Location", &aim, sizeof(aim));
    put("ImpactPoint", &aim, sizeof(aim));
    put("Normal", &normal, sizeof(normal));
    put("ImpactNormal", &normal, sizeof(normal));
    put("TraceStart", &from, sizeof(from));
    put("TraceEnd", &aim, sizeof(aim));
    put("Distance", &len, sizeof(len));
    put("Time", &one, sizeof(one));
    put("FaceIndex", &none, sizeof(none));
    put("Item", &none, sizeof(none));
    ue::Prop blocking = ue::FindProp(hitStruct, "bBlockingHit");
    if (blocking) ue::WriteBool(hit.data(), blocking, true);
    ue::FWeak aw = ue::MakeWeak(t.actor);
    if (aw.serial) {
        WeakRaw r{aw.index, aw.serial};
        put("Actor", &r, sizeof(r));
    }
    void* mesh = ue::Ptr(t.actor, "Mesh");
    ue::FWeak mw = ue::MakeWeak(mesh);
    if (mesh && mw.serial) {
        WeakRaw r{mw.index, mw.serial};
        put("Component", &r, sizeof(r));
    }
    if (head) {
        ue::FName bone = Info(t.actor).bone;
        if (bone.index) put("BoneName", &bone, sizeof(bone));
    }
    ue::Call mk(Lib("DamageStatics"), "MakePipelineDamageInput");
    if (!mk.Ok()) return false;
    mk.Set<void*>("DamageCauser", st.pawn).Set<void*>("DamageReceiver", t.actor).Set<void*>("DamageType", type).Set<void*>("DamageSource", source);
    mk.SetRaw("HitInfo", hit.data(), hsize).Set("HitLocation", aim).Set("HitDirection", unit).Set("HitForceDirection", unit);
    if (!mk.Run()) return false;
    std::vector<uint8_t> input((size_t)isize, 0);
    if (!mk.GetRaw("ReturnValue", input.data(), isize)) return false;
    ue::Prop recv = ue::FindProp(inputStruct, "DamageReceiverComp");
    ue::Prop able = ue::FindProp(inputStruct, "Damageable");
    bool target = (recv && ue::At<void*>(input.data(), recv.offset)) || (able && ue::At<void*>(input.data(), able.offset));
    if (!target) return false;
    ue::Prop ic = ue::FindProp(inputStruct, "InstigatorController");
    ue::Prop ip = ue::FindProp(inputStruct, "InstigatorPawn");
    if (ic && !ue::At<void*>(input.data(), ic.offset)) ue::Put<void*>(input.data(), ic.offset, st.pc);
    if (ip && !ue::At<void*>(input.data(), ip.offset)) ue::Put<void*>(input.data(), ip.offset, st.pawn);
    if (head && critStruct) {
        ue::Prop ch = ue::FindProp(inputStruct, "CriticalHitOverrides");
        ue::Prop ct = ue::FindProp(critStruct, "Type");
        ue::Prop cd = ue::FindProp(critStruct, "bUseDefaultCriticalHitMultiplier");
        if (ch && ct) ue::Put<uint8_t>(input.data(), ch.offset + ct.offset, 1);
        if (ch && cd) ue::WriteBool(input.data() + ch.offset, cd, true);
    }
    ue::Call ap(Lib("DamageStatics"), "ApplyDamageEx");
    if (!ap.Ok()) return false;
    ap.SetRaw("PipelineInput", input.data(), isize).Set<float>("BaseDamage", dmg);
    if (!ap.Run()) return false;
    Retire(p);
    return true;
}

int redirected = 0, retried = 0, direct = 0;
double redirectLogAt = -100;

void SilentAim(const ue::FVector& from, const ue::FRotator& view) {
    void* mgr = Manager();
    if (!mgr) return;
    bool throughWalls = On("silent_walls");
    bool head = On("silent_headshot");
    float lead = throughWalls ? 60.0f : 120.0f;
    float area = std::clamp(Value("silent_area"), 5.0f, 180.0f);
    if (handled.size() > 4096) handled.clear();
    bool picked = false, checked = false, seen = true;
    const Enemy* t = nullptr;
    void* held = HeldWeapon();
    ue::Prop lp = ue::FindProp(ue::ClassOf(mgr), "ActiveProjectiles");
    auto list = ue::Read<ue::TArray<void*>>(mgr, lp);
    for (int i = 0; lp && list.data && i < list.num && i < 512; ++i) {
        void* p = ue::At<void*>(list.data + i, 0);
        if (!ue::Alive(p)) continue;
        BulletLayout(p);
        bool isNew = false;
        Bullet* b = TrackBullet(p, isNew);
        if (isNew) {
            b->mine = Mine(p, held);
            if (!b->mine) continue;
            if (!picked) {
                t = PickTarget(from, view, area, throughWalls);
                picked = true;
            }
            if (!t) {
                b->mine = false;
                continue;
            }
            b->aim = head ? ue::FVector{t->head.x, t->head.y, t->head.z + 3.0f} : ue::FVector{t->center.x, t->center.y, t->center.z + t->extent.z * 0.2f};
            if (throughWalls) {
                if (!checked) {
                    seen = Visible(t->actor, from);
                    checked = true;
                }
                if (!seen && DirectHit(p, *t, head, b->aim)) {
                    b->mine = false;
                    ++direct;
                    continue;
                }
            }
            Redirect(p, *b, lead);
            ++redirected;
        } else if (b->mine && b->changed && b->tries < 2 && OffCourse(p, *b)) {
            ++b->tries;
            Redirect(p, *b, lead);
            ++retried;
        }
    }
    if ((redirected || retried || direct) && st.time - redirectLogAt > 30.0) {
        redirectLogAt = st.time;
        LOG("silent aim: %d shots sent to targets, %d set back on course, %d hit through walls", redirected, retried, direct);
        redirected = retried = direct = 0;
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

std::string SimpleLootName(void* pickup) {
    void* data = ue::Ptr(pickup, "AssociatedInventoryData");
    std::string raw = data ? ue::Name(data) : ue::Name(ue::ClassOf(pickup));
    std::string s = Squash(raw);
    static const std::pair<const char*, const char*> ammo[] = {
        {"sniper", "Sniper ammo"}, {"shotgun", "Shotgun ammo"}, {"smg", "SMG ammo"}, {"pistol", "Pistol ammo"}, {"assault", "Rifle ammo"},
        {"rifle", "Rifle ammo"},   {"heavy", "Heavy ammo"},     {"launcher", "Heavy ammo"}, {"grenade", "Grenade"}, {"ar", "Rifle ammo"}};
    if (s.find("ammo") != std::string::npos) {
        std::string rest = s.substr(s.find("ammo") + 4);
        for (auto& a : ammo)
            if (rest.find(a.first) != std::string::npos || s.find(a.first) != std::string::npos) return a.second;
        return "Ammo";
    }
    static const std::pair<const char*, const char*> table[] = {
        {"eridium", "Eridium"},           {"money", "Money"},         {"cash", "Money"},         {"health", "Health"},
        {"roomdecoration", "Room decoration"}, {"trinket", "Weapon trinket"}, {"weaponskin", "Weapon skin"}, {"echolog", "Echo log"},
        {"emote", "Emote"},              {"skin", "Skin"},           {"head", "Head"},          {"grenade", "Grenade"},
        {"shield", "Shield"},            {"artifact", "Artifact"},   {"classmod", "Class mod"}, {"vaultcard", "Vault card key"},
        {"goldenkey", "Golden key"},     {"diamondkey", "Diamond key"}};
    for (auto& t : table)
        if (s.find(t.first) != std::string::npos) return t.second;
    for (const char* pre : {"InvData_", "InventoryData_", "InvBalD_", "BPInvPart_", "BP_", "BPIO_"})
        if (raw.rfind(pre, 0) == 0) raw = raw.substr(std::strlen(pre));
    if (raw.size() > 2 && raw.compare(raw.size() - 2, 2, "_C") == 0) raw.resize(raw.size() - 2);
    return Pretty(raw);
}

std::string ChestName(void* chest) {
    std::string raw = ue::Name(ue::ClassOf(chest));
    std::string s = Squash(raw);
    static const std::pair<const char*, const char*> table[] = {
        {"redchest", "Red chest"},     {"chestred", "Red chest"},       {"weaponchest", "Weapon chest"}, {"weaponcrate", "Weapon crate"},
        {"gunchest", "Weapon chest"},  {"typhon", "Typhon dead drop"}, {"lockbox", "Lockbox"},          {"strongbox", "Strongbox"},
        {"safe", "Safe"},              {"ammocrate", "Ammo crate"},     {"ammobox", "Ammo box"},         {"ammo", "Ammo box"},
        {"locker", "Locker"},          {"refrigerator", "Fridge"},      {"fridge", "Fridge"},            {"cooler", "Cooler"},
        {"toilet", "Toilet"},          {"dumpster", "Dumpster"},        {"trashcan", "Trash can"},       {"trash", "Trash can"},
        {"mailbox", "Mailbox"},        {"cashregister", "Cash register"}, {"register", "Cash register"}, {"cabinet", "Cabinet"},
        {"duffel", "Duffel bag"},      {"bag", "Bag"},                  {"crate", "Crate"},              {"chest", "Chest"},
        {"box", "Box"}};
    for (auto& t : table)
        if (s.find(t.first) != std::string::npos) return t.second;
    std::string last = raw;
    if (last.size() > 2 && last.compare(last.size() - 2, 2, "_C") == 0) last.resize(last.size() - 2);
    size_t u = last.find_last_of('_');
    if (u != std::string::npos && u + 1 < last.size()) last = last.substr(u + 1);
    return Pretty(last);
}

struct LootInfo {
    int32_t serial = -1;
    const void* ptr = nullptr;
    std::string name;
    int kind = 0;
};
std::unordered_map<int32_t, LootInfo> lootNames;

const LootInfo& Loot(void* p) {
    if (lootNames.size() > 8192) lootNames.clear();
    ue::FWeak w = ue::MakeWeak(p);
    LootInfo& l = lootNames[w.index];
    if (l.serial == w.serial && l.ptr == p) return l;
    l = LootInfo{};
    l.serial = w.serial;
    l.ptr = p;
    l.kind = RarityKind(p);
    void* bal = ue::Ptr(p, "CachedInventoryBalanceComponent");
    if (ue::Alive(bal)) {
        ue::Call c(bal, "GetDisplayName");
        if (c.Run()) l.name = ue::TextToString(c.Raw("ReturnValue"));
    }
    while (!l.name.empty() && (l.name.back() == ' ' || l.name.back() == '\n')) l.name.pop_back();
    if (l.name.empty() || l.name.size() > 48) l.name = SimpleLootName(p);
    return l;
}

struct PlaceCache {
    int32_t serial = -1;
    const void* ptr = nullptr;
    ue::FVector loc;
    std::string name;
    double at = -100;
};
std::unordered_map<int32_t, PlaceCache> places;

const PlaceCache& Place(void* actor, bool chest) {
    if (places.size() > 8192) places.clear();
    ue::FWeak w = ue::MakeWeak(actor);
    PlaceCache& c = places[w.index];
    bool same = c.serial == w.serial && c.ptr == actor;
    if (!same || st.time - c.at > 1.0) {
        if (!same) c.name = chest ? ChestName(actor) : std::string();
        c.serial = w.serial;
        c.ptr = actor;
        c.loc = Location(actor);
        c.at = st.time;
    }
    return c;
}

struct Calibration {
    float k = 1.0f;
    bool measured = false;
    bool logged = false;
    double at = -100;
    ue::FRotator lastRot;
    ue::FVector lastLoc;
    int pov = -1;
    int width = 0, height = 0;
} cal;

std::vector<int> PovCandidates() {
    static std::vector<int> list;
    static bool done = false;
    if (done) return list;
    done = true;
    void* camCls = Cls("PlayerCameraManager");
    ue::Prop pov = ue::FindProp(Cls("CameraCacheEntry"), "POV");
    for (const char* n : {"CameraCachePrivate", "CameraCache"}) {
        ue::Prop c = ue::FindProp(camCls, n);
        if (c && pov) list.push_back(c.offset + pov.offset);
    }
    return list;
}

bool ReadPov(void* cam, int off, ue::FVector& loc, ue::FRotator& rot, float& fov) {
    float raw[7];
    if (!cam || off < 0 || !ue::SafeCopy(raw, (const uint8_t*)cam + off, sizeof(raw))) return false;
    loc = {raw[0], raw[1], raw[2]};
    rot = {raw[3], raw[4], raw[5]};
    fov = raw[6];
    return Finite(loc) && std::isfinite(rot.pitch) && std::isfinite(rot.yaw) && std::isfinite(rot.roll) && fov > 5.0f && fov < 170.0f;
}

float BaseK(int w, int h) {
    if (w <= 0 || h <= 0) return 1.0f;
    void* lp = nullptr;
    ue::Prop lps = st.gameInstance ? ue::FindProp(ue::ClassOf(st.gameInstance), "LocalPlayers") : ue::Prop{};
    auto arr = ue::Read<ue::TArray<void*>>(st.gameInstance, lps);
    if (arr.data && arr.num > 0) ue::SafeRead(arr.data, lp);
    ue::Prop ar = lp ? ue::FindProp(ue::ClassOf(lp), "AspectRatioAxisConstraint") : ue::Prop{};
    uint8_t mode = ar ? ue::At<uint8_t>(lp, ar.offset) : 1;
    return mode == 0 ? (float)h / (float)w : 1.0f;
}

void Calibrate(const ue::FVector& camLoc, const ue::FRotator& camRot, float fov) {
    if (!st.camera || !st.pc || st.time - cal.at < 0.5) return;
    cal.at = st.time;
    int chosen = -1;
    for (int off : PovCandidates()) {
        ue::FVector l;
        ue::FRotator r;
        float f;
        if (ReadPov(st.camera, off, l, r, f) && Dist(l, camLoc) < 10.0f && std::fabs(Norm180(r.yaw - camRot.yaw)) < 1.0f &&
            std::fabs(f - fov) < 1.0f) {
            chosen = off;
            break;
        }
    }
    ue::Call vs(st.pc, "GetViewportSize");
    if (vs.Run()) {
        int w = vs.Get<int32_t>("SizeX"), h = vs.Get<int32_t>("SizeY");
        if (w > 100 && h > 100) {
            cal.width = w;
            cal.height = h;
        }
    }
    if (!cal.measured) cal.k = BaseK(cal.width, cal.height);
    bool still = Dist(camLoc, cal.lastLoc) < 2.0f && AngleBetween(camRot, cal.lastRot) < 0.2f;
    cal.lastLoc = camLoc;
    cal.lastRot = camRot;
    if (still && cal.width > 0) {
        double P = camRot.pitch * 3.14159265358979 / 180, Y = camRot.yaw * 3.14159265358979 / 180, R = camRot.roll * 3.14159265358979 / 180;
        double SP = std::sin(P), CP = std::cos(P), SY = std::sin(Y), CY = std::cos(Y), SR = std::sin(R), CR = std::cos(R);
        double fx = CP * CY, fy = CP * SY, fz = SP;
        double rx = SR * SP * CY - CR * SY, ry = SR * SP * SY + CR * CY, rz = -SR * CP;
        double ux = -(CR * SP * CY + SR * SY), uy = CY * SR - CR * SP * SY, uz = CR * CP;
        ue::FVector test{(float)(camLoc.x + fx * 1000 + rx * 300 + ux * 200), (float)(camLoc.y + fy * 1000 + ry * 300 + uy * 200),
                         (float)(camLoc.z + fz * 1000 + rz * 300 + uz * 200)};
        ue::Call c(st.pc, "ProjectWorldLocationToScreen");
        c.Set("WorldLocation", test).SetBool("bPlayerViewportRelative", false).SetBool("bUseForegroundProjection", false);
        if (c.Run() && c.GetBool("ReturnValue")) {
            float s[2] = {};
            c.GetRaw("ScreenLocation", s, sizeof(s));
            double focal = (s[0] - cal.width * 0.5) * 1000.0 / 300.0;
            double focalY = (cal.height * 0.5 - s[1]) * 1000.0 / 200.0;
            double byFov = (cal.width * 0.5) / std::tan(fov * 0.5 * 3.14159265358979 / 180.0);
            double k = focal / byFov;
            if (std::isfinite(k) && k > 0.25 && k < 4.0 && std::fabs(focalY / focal - 1.0) < 0.1) {
                cal.k = cal.measured ? (float)(cal.k * 0.6 + k * 0.4) : (float)k;
                cal.measured = true;
                if (!cal.logged) {
                    cal.logged = true;
                    LOG("screen check: %dx%d, field of view %.1f, scale %.3f", cal.width, cal.height, fov, cal.k);
                }
            }
        }
    }
    cal.pov = chosen;
    if (chosen >= 0) esp::SetLiveCamera(st.camera, chosen, cal.k);
    else esp::ClearLiveCamera();
}

double espAt = -100, lootAt = -100;
bool espShown = false;

void PublishEsp(bool enemyOn, bool lootOn, const ue::FVector& camLoc, const ue::FRotator& camRot, float fov, bool camOk) {
    if (st.time - espAt < 0.016) return;
    espAt = st.time;
    std::vector<core::EspDot> dots, items;
    if (enemyOn) {
        for (const Enemy& e : enemies) {
            if (!ue::WeakGet(e.w)) continue;
            core::EspDot d;
            d.x = e.center.x;
            d.y = e.center.y;
            d.z = e.center.z - e.extent.z;
            d.cx = e.center.x;
            d.cy = e.center.y;
            d.cz = e.center.z;
            d.ex = e.extent.x;
            d.ey = e.extent.y;
            d.ez = e.extent.z;
            d.top = e.center.z + e.extent.z;
            d.box = true;
            d.name = e.name;
            d.dist = e.dist;
            d.health = e.health;
            d.kind = (e.badass ? 1 : 0) | (e.visible ? 0 : 2);
            dots.push_back(std::move(d));
        }
    }
    bool lootNow = lootOn && st.time - lootAt >= 0.125;
    if (lootNow) {
        lootAt = st.time;
        bool want[6] = {false, On("lootesp_common"), On("lootesp_uncommon"), On("lootesp_rare"), On("lootesp_epic"), On("lootesp_legendary")};
        static void* skip[3] = {};
        if (!skip[0]) {
            skip[0] = Cls("OakInventoryDisplayItem");
            skip[1] = Cls("GunRackDisplayItem");
            skip[2] = Cls("OakMissionPickup");
        }
        for (void* p : Objects(Scan::Pickups)) {
            if (items.size() >= 300) break;
            if (!ue::Alive(p)) continue;
            if (ue::IsA(p, skip[0]) || ue::IsA(p, skip[1]) || ue::IsA(p, skip[2])) continue;
            if (!ue::GetBool(p, "bIsActive") || ue::Ptr(p, "PickedUpBy")) continue;
            const LootInfo& info = Loot(p);
            int k = info.kind;
            if (k > 0 && !want[k]) continue;
            if (k == 0 && !want[1]) continue;
            const PlaceCache& pc = Place(p, false);
            float dist = Dist(camLoc, pc.loc) / 100.0f;
            if (dist > 400.0f) continue;
            core::EspDot d;
            d.x = pc.loc.x;
            d.y = pc.loc.y;
            d.z = pc.loc.z;
            d.top = pc.loc.z + 30.0f;
            d.name = info.name;
            d.dist = dist;
            d.kind = k ? k : 1;
            items.push_back(std::move(d));
        }
        if (On("lootesp_chests")) {
            for (void* c : Objects(Scan::Lootables)) {
                if (items.size() >= 400) break;
                if (!ue::Alive(c)) continue;
                ue::Prop sp = ue::FindProp(ue::ClassOf(c), "CurrentInteractiveState");
                if (sp && ue::At<uint8_t>(c, sp.offset) != 1) continue;
                const PlaceCache& pc = Place(c, true);
                float dist = Dist(camLoc, pc.loc) / 100.0f;
                if (dist > 300.0f) continue;
                core::EspDot d;
                d.x = pc.loc.x;
                d.y = pc.loc.y;
                d.z = pc.loc.z;
                d.top = pc.loc.z + 50.0f;
                d.name = pc.name;
                d.dist = dist;
                d.kind = 6;
                items.push_back(std::move(d));
            }
        }
    }
    if (lootNow) {
        std::sort(items.begin(), items.end(), [](const core::EspDot& x, const core::EspDot& y) { return x.dist < y.dist; });
        if (items.size() > 220) items.resize(220);
        std::reverse(items.begin(), items.end());
    }
    std::sort(dots.begin(), dots.end(), [](const core::EspDot& x, const core::EspDot& y) { return x.dist > y.dist; });
    float k = cal.k;
    int w = cal.width, h = cal.height;
    WithModel([&](core::Model& m) {
        m.esp = std::move(dots);
        if (lootNow) m.itemEsp = std::move(items);
        else if (!lootOn) m.itemEsp.clear();
        m.camera.valid = camOk;
        m.camera.x = camLoc.x;
        m.camera.y = camLoc.y;
        m.camera.z = camLoc.z;
        m.camera.pitch = camRot.pitch;
        m.camera.yaw = camRot.yaw;
        m.camera.roll = camRot.roll;
        m.camera.fov = fov;
        m.camera.width = w;
        m.camera.height = h;
        m.camera.focal = (w > 0 && fov > 1.0f) ? k * (w * 0.5) / std::tan(fov * 0.5 * 3.14159265358979 / 180.0) : 0.0;
    });
}

void ClearEsp() {
    esp::ClearLiveCamera();
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
    Ready("silent_aim", chars && camera && Has("LightProjectile", "Velocity") && Has("LightProjectile", "Location") &&
                            Has("LightProjectileManager", "ActiveProjectiles"));
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
    if (!silent && !handled.empty()) UndoAllBullets();
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
    if (silent || assist || enemyEsp) RefreshEnemies(camLoc, silent || assist);
    static double listLogAt = -100;
    if ((silent || assist || enemyEsp) && enemiesAt > 0 && st.time - listLogAt > 60.0) {
        listLogAt = st.time;
        int hidden = 0;
        for (const Enemy& e : enemies)
            if (!e.visible) ++hidden;
        LOG("enemy list: %zu enemies within 300 m, %d behind cover, %zu characters scanned", enemies.size(), hidden, Objects(Scan::Characters).size());
    }
    if (kill) KillNearby(Location(st.pawn));
    if (assist && !st.menuOpen) AimAssist(camLoc, camRot);
    if (silent) SilentAim(camLoc, camRot);
    if (camOk) Calibrate(camLoc, camRot, fov);
    if (enemyEsp || lootEsp || silent || assist) {
        espShown = true;
        PublishEsp(enemyEsp, lootEsp, camLoc, camRot, fov, camOk);
    } else {
        ClearEsp();
    }
}

void AimOff() {
    UndoAllBullets();
    ClearEsp();
}

}
