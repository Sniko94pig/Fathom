#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/patch.h"
#include "ingame/unity.h"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sotf {

namespace {

using il2::Class;
using il2::Field;
using il2::Method;
using il2::Object;
using un::Fld;
using un::Vec3;

const char* kSons = "Sons";
const char* kVail = "Sons.Ai.Vail";
const char* kItems = "Sons.Item";
const char* kBallistics = "BulletBallistics";

const float kPi = 3.14159265f;
const float kAimRange = 150.0f;
const float kAimMin = 0.5f;
const float kAimCone = 30.0f;
const float kRecoilLimit = 45.0f;
const float kKillMult = 1000.0f;
const float kKillCone = 10.0f;
const float kMeleeCone = 15.0f;
const float kMeleeReach = 4.0f;
const float kPlayerHead = 1.6f;
const double kScanEvery = 0.05;
const double kKillHold = 0.25;
const double kBusyWindow = 2.0;
const double kRigRetry = 2.0;

enum HeldKind { K_RANGED = 1, K_BOW = 2, K_THROWN = 4, K_ROPE = 8, K_MELEE = 16, K_FLASH = 32 };
enum HookId { H_SPENT, H_BOW_SPENT, H_SHOOT, H_ARMOUR, H_COUNT };
enum TargetKind { T_ACTOR, T_PLAYER };

struct Refs {
    Class *localPlayer = nullptr, *inventory = nullptr, *itemInstance = nullptr, *held = nullptr, *ranged = nullptr, *bow = nullptr,
          *melee = nullptr, *flashlight = nullptr, *ropeGun = nullptr, *rangedWeapon = nullptr, *ammo = nullptr, *weapon = nullptr,
          *ammoProps = nullptr, *bulletInfo = nullptr, *projectileInfo = nullptr, *ballistics = nullptr, *bulletHandler = nullptr,
          *ballisticSettings = nullptr, *itemData = nullptr, *meleeData = nullptr, *weaponInfo = nullptr, *armourSystem = nullptr,
          *slotData = nullptr, *playerStats = nullptr, *actorManager = nullptr, *actor = nullptr, *vailTypes = nullptr,
          *tracker = nullptr, *mpUtil = nullptr, *boltEntity = nullptr, *remoteSetup = nullptr;
    std::vector<Class*> thrown;
    Fld lpInventory, lpCamTr, lpStats;
    Fld invSlots, invView;
    Fld iiData, iiObject;
    Fld rcWeapon, mcInfo, flDrain;
    Fld rcFireDelay, rcNextFire, rcAttack, rcCharging, rcWeaponAnim, hcAnimator;
    Fld rwAmmo, rwNpc, rwScreen, rwLocal, rwItemData, rwSpawn, rwController, wSpawn;
    Fld amInternal, amMax, amCur, amProps;
    Fld apScatter, apProjectile;
    Fld biSpeed, biMask, piType;
    Fld bhInstance, bhSettings, bsDrop, bsDrag, bsAir;
    Fld idAmmoMult, idMelee, mdCharge, mdSmash;
    Fld wiDamage, wiRemote, wiMelee;
    Fld asSlots, sdPiece, sdPoints;
    Fld psArmour, psDying;
    Fld mgrInstance, mgrActive;
    Fld vaId, vaDead, vaDamage, vaHead, vaCapHeight, vaCapCenter, vaScale, vaCtrl;
    Fld trInstance, trEntities, mpEntities, rsAnimator;
    Method *rcGetWeapon = nullptr, *rcReload = nullptr, *rwLaser = nullptr, *amAvailable = nullptr, *zeroing = nullptr,
           *vaDying = nullptr, *vtClass = nullptr, *vtFriendly = nullptr, *colBounds = nullptr;
    Method *animGetSpeed = nullptr, *animSetSpeed = nullptr, *animBone = nullptr, *beAttached = nullptr, *beControl = nullptr;
    Method *spent = nullptr, *bowSpent = nullptr, *shoot = nullptr, *armourCalc = nullptr;
    int viewWorld = 2, aliveState = 0;
    int clsCannibal = 1, clsCreepy = 2, clsAnimal = 3;
    int friends[4] = {8, 9, 10, 100};
    int headBone = 10;
    std::vector<int> shotTypes;
} R;

struct Support {
    bool ammo = false, reload = false, recoil = false, damage = false, aim = false, armour = false, flash = false, rate = false,
         players = false;
} support;

using SpentFn = void (*)(Object*, const void*);
using ShootFn = void (*)(Object*, const Vec3*, float, int32_t, const void*);
using ArmourFn = float (*)(Object*, float, bool, const void*);

SpentFn oSpent = nullptr;
SpentFn oBowSpent = nullptr;
ShootFn oShoot = nullptr;
ArmourFn oArmour = nullptr;

std::atomic<bool> ammoOn{false}, recoilOn{false}, aimOn{false}, armourOn{false};
std::atomic<int> hooked{0}, blocked{0}, broken{0};
Object* armourSelf = nullptr;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
int published = 0;

struct Hand {
    Object* item = nullptr;
    Object* go = nullptr;
    uint32_t ctrl = 0;
    uint32_t info = 0;
    bool infoTried = false;
    int kind = 0;
    double retry = 0;
};
Hand hands[2];

struct Target {
    uint32_t h = 0;
    Object* raw = nullptr;
    int kind = T_ACTOR;
    int cls = 0;
    bool body = false;
    Vec3 head, vel;
    bool hasHead = false;
};
Target target;

struct AimFilter {
    bool enemies = true, animals = true, players = false;
};
AimFilter filter;

struct Rig {
    uint32_t ent = 0;
    uint32_t head = 0;
    double retry = 0;
    bool seen = false;
};
std::unordered_map<Object*, Rig> rigs;

struct Speed {
    patch::Hold obj;
    float base = 1.0f, written = 1.0f;
    bool have = false;
};
Speed sSpeed[2];
patch::Scaled sFire;
Object* fireCtrl = nullptr;
float lastNext = 0;
double busyUntil = 0;
std::unordered_map<Class*, Method*> idles;

struct Burst {
    Object* weapon = nullptr;
    LONGLONG t = 0;
    int laser = -1;
    bool haveAim = false;
    Vec3 aim;
    Object* zeroInfo = nullptr;
    Object* zeroActor = nullptr;
    float zeroDist = -100.0f, zeroDeg = 0;
};
Burst burst;

struct TypeInfo {
    bool known = false;
    bool friendly = false;
    int cls = 0;
};
TypeInfo types[128];

struct ListShape {
    Class* cls = nullptr;
    int items = -1, size = -1;
};
ListShape shapes[4];

struct Cand {
    Object* actor = nullptr;
    int kind = T_ACTOR;
    int cls = 0;
    Vec3 head, body;
    float dist = 0, cosine = 0;
};

std::unordered_map<Class*, int> kinds;
std::unordered_map<Class*, Method*> reloads;

patch::Scaled sAmmo, sMelee, sCharge, sSmash;
patch::Forced<float> sFlash[2];

struct Box {
    Vec3 center, extents;
};

double nextScan = 0, reloadWait = 0, killNext = 0, killUntil = 0;
Object* lastPlayer = nullptr;
LONGLONG tickRate = 0;

LONGLONG Ticks() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

Vec3 Cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }

bool VecOf(Method* m, Object* compOrGo, Vec3& out) {
    Object* t = m ? un::TransformOf(compOrGo) : nullptr;
    if (!t) return false;
    bool ok = false;
    Object* r = un::Call(m, t, {}, &ok);
    if (!ok || !r) return false;
    out = il2::Result<Vec3>(r, Vec3{});
    return Finite(out);
}

bool Pos(Object* o, Vec3& out) { return VecOf(un::E.trGetPosition, o, out); }

bool Fwd(Object* o, Vec3& out) { return VecOf(un::E.trForward, o, out); }

bool ListRead(Object* list, Object**& items, int& count) {
    items = nullptr;
    count = 0;
    Class* c = list ? il2::ClassOf(list) : nullptr;
    if (!c) return false;
    ListShape* s = nullptr;
    for (ListShape& e : shapes)
        if (e.cls == c) {
            s = &e;
            break;
        }
    if (!s) {
        Field* fi = il2::FindField(c, "_items");
        Field* fs = il2::FindField(c, "_size");
        if (!fi || !fs) return false;
        for (ListShape& e : shapes)
            if (!e.cls) {
                s = &e;
                break;
            }
        if (!s) s = &shapes[0];
        s->cls = c;
        s->items = il2::Offset(fi);
        s->size = il2::Offset(fs);
    }
    Object* arr = il2::Get<Object*>(list, s->items);
    int n = il2::Get<int>(list, s->size);
    if (!arr) return false;
    size_t len = il2::Length(arr);
    if (n < 0) n = 0;
    if ((size_t)n > len) n = (int)len;
    items = (Object**)il2::Data(arr);
    count = n;
    return true;
}

int KindOfClass(Class* c) {
    if (!c) return 0;
    auto it = kinds.find(c);
    if (it != kinds.end()) return it->second;
    int k = 0;
    if (R.ranged && il2::IsSubclass(c, R.ranged)) k |= K_RANGED;
    if (R.bow && il2::IsSubclass(c, R.bow)) k |= K_BOW;
    if (R.ropeGun && il2::IsSubclass(c, R.ropeGun)) k |= K_ROPE;
    for (Class* t : R.thrown)
        if (il2::IsSubclass(c, t)) k |= K_THROWN;
    if (R.melee && il2::IsSubclass(c, R.melee)) k |= K_MELEE;
    if (R.flashlight && il2::IsSubclass(c, R.flashlight)) k |= K_FLASH;
    kinds[c] = k;
    return k;
}

int KindOf(Object* o) { return o ? KindOfClass(il2::ClassOf(o)) : 0; }

void ClearHand(Hand& h) {
    if (h.ctrl) il2::Unpin(h.ctrl);
    if (h.info) il2::Unpin(h.info);
    h = Hand{};
}

Object* HandCtrl(int index, Object* inv, double now) {
    Hand& h = hands[index];
    Object* slots = un::Ref(inv, R.invSlots);
    Object* item = slots && il2::Length(slots) > (size_t)index ? il2::At(slots, (size_t)index) : nullptr;
    Object* go = item ? un::Ref(item, R.iiObject) : nullptr;
    if (!un::Alive(go)) {
        if (h.go || h.ctrl) ClearHand(h);
        return nullptr;
    }
    if (go == h.go && item == h.item) {
        Object* c = h.ctrl ? il2::Pinned(h.ctrl) : nullptr;
        if (c && un::Alive(c)) return c;
        if (now < h.retry) return nullptr;
    }
    ClearHand(h);
    h.go = go;
    h.item = item;
    Object* c = un::CompInChildren(go, R.held, false);
    if (!c) {
        h.retry = now + 0.25;
        return nullptr;
    }
    h.ctrl = il2::Weak(c);
    h.kind = KindOf(c);
    return c;
}

Object* RangedOf(Object* ctrl) {
    if (!ctrl) return nullptr;
    Object* rw = un::Ref(ctrl, R.rcWeapon);
    if (!un::Alive(rw) && R.rcGetWeapon) rw = un::Call(R.rcGetWeapon, ctrl);
    return un::Alive(rw) ? rw : nullptr;
}

Object* ShotInfo(Object* rw, bool* scatter) {
    if (scatter) *scatter = false;
    Object* ammo = rw ? un::Ref(rw, R.rwAmmo) : nullptr;
    Object* props = ammo ? un::Ref(ammo, R.amProps) : nullptr;
    if (!un::Alive(props)) return nullptr;
    if (scatter) *scatter = un::Val<bool>(props, R.apScatter);
    return un::Ref(props, R.apProjectile);
}

Object* SpawnOf(Object* rw) {
    Object* s = un::Ref(rw, R.wSpawn);
    if (!un::Alive(s)) s = un::Ref(rw, R.rwSpawn);
    return un::Alive(s) ? s : nullptr;
}

bool EnemyClass(int cls) { return cls == R.clsCannibal || cls == R.clsCreepy; }

bool Targetable(int id, int& cls) {
    cls = 0;
    if (id <= 0 || id >= 128) return false;
    for (int f : R.friends)
        if (id == f) return false;
    TypeInfo& t = types[id];
    if (!t.known) {
        if (!R.vtClass) return false;
        int v = id;
        bool ok = false;
        Object* r = un::Call(R.vtClass, nullptr, {&v}, &ok);
        if (!ok || !r) return false;
        t.cls = il2::Result<int>(r, 0);
        t.friendly = R.vtFriendly && un::CallBool(R.vtFriendly, nullptr, {&v}, false);
        t.known = true;
    }
    cls = t.cls;
    return !t.friendly && (EnemyClass(cls) || cls == R.clsAnimal);
}

bool Allowed(int kind, int cls) {
    if (kind == T_PLAYER) return filter.players;
    if (cls == R.clsAnimal) return filter.animals;
    return filter.enemies && EnemyClass(cls);
}

bool Capsule(Object* actor, Vec3 root, Vec3& top, Vec3& mid, float& reach) {
    Object* ctl = R.colBounds ? un::Ref(actor, R.vaCtrl) : nullptr;
    if (ctl && un::Alive(ctl)) {
        bool ok = false;
        Object* r = un::Call(R.colBounds, ctl, {}, &ok);
        Box b = ok ? il2::Result<Box>(r, Box{}) : Box{};
        float hy = b.extents.y;
        if (ok && Finite(b.center) && Finite(b.extents) && hy > 0.025f && hy < 10.0f && un::Dist(b.center, root) < hy + 3.0f) {
            top = un::Add(b.center, Vec3{0, hy * 0.8f, 0});
            mid = b.center;
            reach = hy + 0.75f;
            return true;
        }
    }
    if (!R.vaCapHeight || !R.vaCapCenter) return false;
    float h = un::Val<float>(actor, R.vaCapHeight);
    Vec3 cc = un::Val<Vec3>(actor, R.vaCapCenter);
    float s = un::Val<float>(actor, R.vaScale, 1.0f);
    if (!(s > 0.05f && s < 20.0f)) s = 1.0f;
    if (!(h > 0.05f && h < 20.0f) || !Finite(cc) || std::fabs(cc.y) > 20.0f) return false;
    top = un::Add(root, Vec3{0, (cc.y + h * 0.4f) * s, 0});
    mid = un::Add(root, Vec3{0, cc.y * s, 0});
    reach = h * s * 0.5f + 0.75f;
    return Finite(top) && Finite(mid);
}

bool ActorPoints(Object* actor, int cls, Vec3& head, Vec3& body) {
    Vec3 root, top, mid, j;
    float reach = 0;
    bool haveRoot = Pos(actor, root);
    bool cap = haveRoot && Capsule(actor, root, top, mid, reach);
    Object* joint = un::Ref(actor, R.vaHead);
    if (un::Alive(joint) && Pos(joint, j) && (!cap || un::Dist(j, mid) <= reach)) {
        head = j;
        body = cap ? mid : j;
        return true;
    }
    if (cap) {
        head = top;
        body = mid;
        return true;
    }
    if (!haveRoot) return false;
    bool animal = cls == R.clsAnimal;
    head = un::Add(root, Vec3{0, animal ? 0.5f : 1.6f, 0});
    body = un::Add(root, Vec3{0, animal ? 0.3f : 1.0f, 0});
    return true;
}

void UnpinRig(Rig& r) {
    if (r.ent) il2::Unpin(r.ent);
    if (r.head) il2::Unpin(r.head);
    r = Rig{};
}

void ClearRigs() {
    for (auto& e : rigs) UnpinRig(e.second);
    rigs.clear();
}

Object* FindHead(Object* ent) {
    if (!R.animBone) return nullptr;
    Object* anim = nullptr;
    Object* setup = R.remoteSetup ? un::CompInChildren(ent, R.remoteSetup, false) : nullptr;
    if (setup) anim = un::Ref(setup, R.rsAnimator);
    if (!un::Alive(anim) && un::E.animator) anim = un::CompInChildren(ent, un::E.animator, false);
    if (!un::Alive(anim)) return nullptr;
    int bone = R.headBone;
    Object* t = un::Call(R.animBone, anim, {&bone});
    return un::Alive(t) ? t : nullptr;
}

Object* RigHead(Object* ent, double now) {
    auto it = rigs.find(ent);
    if (it != rigs.end() && il2::Pinned(it->second.ent) != ent) {
        UnpinRig(it->second);
        rigs.erase(it);
        it = rigs.end();
    }
    if (it == rigs.end()) {
        if (now < 0) return nullptr;
        Rig r;
        r.ent = il2::Weak(ent);
        it = rigs.emplace(ent, r).first;
    }
    Rig& r = it->second;
    r.seen = true;
    if (r.head) {
        Object* h = il2::Pinned(r.head);
        if (h && un::Alive(h)) return h;
        il2::Unpin(r.head);
        r.head = 0;
    }
    if (now < 0 || now < r.retry) return nullptr;
    r.retry = now + kRigRetry;
    Object* t = FindHead(ent);
    if (t) r.head = il2::Weak(t);
    return t;
}

bool PlayerPoints(Object* ent, double now, Vec3& head, Vec3& body) {
    Vec3 root, p;
    if (!Pos(ent, root)) return false;
    Object* h = RigHead(ent, now);
    if (h && Pos(h, p) && un::Dist(p, root) < 3.0f) {
        head = p;
        body = un::Mul(un::Add(p, root), 0.5f);
        if (un::Dist(head, body) < 0.2f) body = head;
        return true;
    }
    head = un::Add(root, Vec3{0, kPlayerHead, 0});
    body = un::Add(root, Vec3{0, 1.0f, 0});
    return true;
}

void DropTarget() {
    if (target.h) il2::Unpin(target.h);
    target = Target{};
}

Object* TargetObj() {
    if (!target.h) return nullptr;
    Object* a = il2::Pinned(target.h);
    if (!a || a != target.raw || !un::Alive(a)) return nullptr;
    if (target.kind == T_ACTOR) return un::Val<bool>(a, R.vaDead) ? nullptr : a;
    if (R.beAttached && !un::CallBool(R.beAttached, a, {}, false)) return nullptr;
    return a;
}

bool Fresh(Object* o, Vec3& out) {
    Vec3 head, body;
    bool ok = target.kind == T_PLAYER ? PlayerPoints(o, -1.0, head, body) : ActorPoints(o, target.cls, head, body);
    if (!ok) return false;
    out = target.body ? body : head;
    return Finite(out);
}

void SetTarget(const Cand& k, Vec3 point, bool body) {
    if (target.h && target.raw == k.actor && il2::Pinned(target.h) == k.actor) {
        if (target.body != body) {
            target.body = body;
            target.head = point;
            target.hasHead = true;
        }
        return;
    }
    DropTarget();
    target.h = il2::Weak(k.actor);
    target.raw = k.actor;
    target.kind = k.kind;
    target.cls = k.cls;
    target.body = body;
    target.head = point;
    target.hasHead = true;
}

void TrackTarget(float dt) {
    if (!target.h) return;
    Object* a = TargetObj();
    if (!a || !Allowed(target.kind, target.cls)) {
        DropTarget();
        return;
    }
    Vec3 head;
    if (!Fresh(a, head)) return;
    if (dt > 0.0005f && target.hasHead) {
        Vec3 v = un::Mul(un::Sub(head, target.head), 1.0f / dt);
        if (!Finite(v) || un::Len(v) > 40.0f) v = Vec3{};
        target.vel = un::Add(un::Mul(target.vel, 0.6f), un::Mul(v, 0.4f));
    }
    target.head = head;
    target.hasHead = true;
}

bool Visible(Vec3 from, Vec3 to, float dist, int mask) {
    if (dist < 0.01f) return true;
    un::RayHit hit;
    Vec3 dir = un::Mul(un::Sub(to, from), 1.0f / dist);
    if (!un::Raycast(from, dir, dist, hit, mask)) return true;
    return hit.distance >= dist - 1.0f;
}

bool Consider(Cand& k, Vec3 pos, Vec3 fwd, float minCos) {
    Vec3 to = un::Sub(k.head, pos);
    float d = un::Len(to);
    if (!(d >= kAimMin && d <= kAimRange)) return false;
    float cs = un::Dot(to, fwd) / d;
    if (!(cs >= minCos)) return false;
    k.dist = d;
    k.cosine = cs;
    return true;
}

void ActorCands(Vec3 pos, Vec3 fwd, float minCos, std::vector<Cand>& cands) {
    Object* mgr = un::Static(R.mgrInstance);
    Object** items = nullptr;
    int count = 0;
    if (!un::Alive(mgr) || !ListRead(un::Ref(mgr, R.mgrActive), items, count) || !items) return;
    std::vector<Object*> actors(items, items + count);
    for (Object* a : actors) {
        if (!a || !un::Alive(a) || un::Val<bool>(a, R.vaDead)) continue;
        if (!un::Val<bool>(a, R.vaDamage, true)) continue;
        int cls = 0;
        if (!Targetable(un::Val<int>(a, R.vaId), cls) || !Allowed(T_ACTOR, cls)) continue;
        Cand k;
        k.actor = a;
        k.kind = T_ACTOR;
        k.cls = cls;
        if (!ActorPoints(a, cls, k.head, k.body)) continue;
        if (Consider(k, pos, fwd, minCos)) cands.push_back(k);
    }
}

void AddEntities(Object* list, std::vector<Object*>& out) {
    Object** items = nullptr;
    int count = 0;
    if (!list || !ListRead(list, items, count) || !items) return;
    for (int i = 0; i < count; ++i)
        if (items[i] && std::find(out.begin(), out.end(), items[i]) == out.end()) out.push_back(items[i]);
}

void Remotes(Vec3 pos, float minDist, std::vector<Object*>& out) {
    std::vector<Object*> ents;
    Object* tracker = un::Static(R.trInstance);
    if (un::Alive(tracker)) AddEntities(un::Ref(tracker, R.trEntities), ents);
    AddEntities(un::Static(R.mpEntities), ents);
    for (Object* e : ents) {
        if (!un::Alive(e) || (R.boltEntity && !il2::IsSubclass(il2::ClassOf(e), R.boltEntity))) continue;
        if (!un::CallBool(R.beAttached, e, {}, false)) continue;
        if (R.beControl && un::CallBool(R.beControl, e, {}, true)) continue;
        Vec3 root;
        if (!Pos(e, root) || un::Dist(root, pos) < minDist) continue;
        out.push_back(e);
    }
}

void PlayerCands(Vec3 pos, Vec3 fwd, float minCos, double now, std::vector<Cand>& cands) {
    std::vector<Object*> ents;
    Remotes(pos, 1.5f, ents);
    for (auto& e : rigs) e.second.seen = false;
    for (Object* e : ents) {
        Cand k;
        k.actor = e;
        k.kind = T_PLAYER;
        if (!PlayerPoints(e, now, k.head, k.body)) continue;
        if (Consider(k, pos, fwd, minCos)) cands.push_back(k);
    }
    for (auto it = rigs.begin(); it != rigs.end();) {
        if (it->second.seen) {
            ++it;
            continue;
        }
        UnpinRig(it->second);
        it = rigs.erase(it);
    }
}

void Scan(Object* rw, double now) {
    Object* cam = un::Static(R.lpCamTr);
    Vec3 pos, fwd;
    if (!un::Alive(cam) || !Pos(cam, pos) || !Fwd(cam, fwd)) {
        DropTarget();
        return;
    }
    fwd = un::Norm(fwd);
    Object* info = ShotInfo(rw, nullptr);
    int mask = info ? un::Val<int>(info, R.biMask) : 0;
    if (mask == 0) mask = ~0;
    float minCos = std::cos(kAimCone * kPi / 180.0f);
    std::vector<Cand> cands;
    if (filter.enemies || filter.animals) ActorCands(pos, fwd, minCos, cands);
    if (filter.players && support.players) PlayerCands(pos, fwd, minCos, now, cands);
    else if (!rigs.empty()) ClearRigs();
    std::sort(cands.begin(), cands.end(), [](const Cand& x, const Cand& y) {
        if (x.cosine != y.cosine) return x.cosine > y.cosine;
        return x.dist < y.dist;
    });
    for (size_t i = 0; i < cands.size() && i < 10; ++i) {
        const Cand& k = cands[i];
        bool body = false;
        if (!Visible(pos, k.head, k.dist, mask)) {
            float bd = un::Dist(k.body, pos);
            if (un::Dist(k.body, k.head) < 0.05f || !(bd >= kAimMin) || !Visible(pos, k.body, bd, mask)) continue;
            body = true;
        }
        if (k.kind == T_ACTOR && R.vaDying && un::CallBool(R.vaDying, k.actor)) continue;
        SetTarget(k, body ? k.body : k.head, body);
        return;
    }
    DropTarget();
}

void Aim(const Ctx& c, bool on, Object* ctrl, int kind) {
    Object* rw = on && ctrl && (kind & K_RANGED) && !(kind & (K_THROWN | K_ROPE)) ? RangedOf(ctrl) : nullptr;
    if (!rw) {
        if (target.h) DropTarget();
        if (!rigs.empty()) ClearRigs();
        return;
    }
    TrackTarget(c.dt);
    if (c.now < nextScan) return;
    nextScan = c.now + kScanEvery;
    Scan(rw, c.now);
}

struct Sighted {
    bool enemy = false;
    Vec3 point;
    float dist = 0, cosine = 0;
};

bool Sight(Vec3 pos, Vec3 fwd, Vec3 head, Vec3 body, float reach, float minCos, Sighted& s) {
    float best = -2.0f;
    for (int i = 0; i <= 4; ++i) {
        Vec3 p = un::Add(body, un::Mul(un::Sub(head, body), i * 0.25f));
        Vec3 to = un::Sub(p, pos);
        float d = un::Len(to);
        if (!(d >= 0.05f)) continue;
        float cs = un::Dot(to, fwd) / d;
        if (cs > best) {
            best = cs;
            s.point = p;
            s.dist = d;
        }
    }
    s.cosine = best;
    return best >= minCos && s.dist <= reach;
}

int KillScan(Object* ctrl, int kind) {
    if (!ctrl || !(kind & (K_RANGED | K_MELEE))) return -1;
    Object* cam = un::Static(R.lpCamTr);
    Vec3 pos, fwd;
    if (!un::Alive(cam) || !Pos(cam, pos) || !Fwd(cam, fwd)) return -1;
    fwd = un::Norm(fwd);
    bool ranged = (kind & K_RANGED) != 0;
    float reach = ranged ? kAimRange : kMeleeReach;
    float minCos = std::cos((ranged ? kKillCone : kMeleeCone) * kPi / 180.0f);
    int mask = 0;
    Object* rw = ranged ? RangedOf(ctrl) : nullptr;
    if (rw) {
        Object* info = ShotInfo(rw, nullptr);
        mask = info ? un::Val<int>(info, R.biMask) : 0;
    }
    if (mask == 0) mask = ~0;
    bool aimed = false;
    if (ranged && aimOn.load() && target.h && TargetObj()) {
        if (target.kind != T_ACTOR || !EnemyClass(target.cls)) return -1;
        aimed = true;
    }
    std::vector<Sighted> seen;
    Object* mgr = un::Static(R.mgrInstance);
    Object** items = nullptr;
    int count = 0;
    if (un::Alive(mgr) && ListRead(un::Ref(mgr, R.mgrActive), items, count) && items) {
        std::vector<Object*> actors(items, items + count);
        for (Object* a : actors) {
            if (!a || !un::Alive(a) || un::Val<bool>(a, R.vaDead)) continue;
            int cls = 0;
            bool enemy = Targetable(un::Val<int>(a, R.vaId), cls) && EnemyClass(cls);
            if (enemy && !un::Val<bool>(a, R.vaDamage, true)) continue;
            Vec3 head, body;
            if (!ActorPoints(a, cls, head, body)) continue;
            Sighted s;
            s.enemy = enemy;
            if (Sight(pos, fwd, head, body, reach, minCos, s)) seen.push_back(s);
        }
    }
    if (support.players) {
        std::vector<Object*> ents;
        Remotes(pos, R.beControl ? 0.0f : 1.5f, ents);
        for (Object* e : ents) {
            Vec3 head, body;
            if (!PlayerPoints(e, -1.0, head, body)) continue;
            Sighted s;
            if (Sight(pos, fwd, head, body, reach, minCos, s)) seen.push_back(s);
        }
    }
    std::sort(seen.begin(), seen.end(), [](const Sighted& x, const Sighted& y) {
        if (x.cosine != y.cosine) return x.cosine > y.cosine;
        return x.dist < y.dist;
    });
    const Sighted* hit = nullptr;
    int checked = 0;
    for (const Sighted& s : seen) {
        if (++checked > 12) break;
        if (!Visible(pos, s.point, s.dist, mask)) continue;
        if (!s.enemy) return -1;
        hit = &s;
        break;
    }
    if (hit) {
        for (const Sighted& s : seen)
            if (!s.enemy && s.dist <= hit->dist) return -1;
        return 1;
    }
    return aimed ? 1 : 0;
}

bool Killing(const Ctx& c, Object* ctrl, int kind) {
    if (c.now < killNext) return c.now < killUntil;
    killNext = c.now + kScanEvery;
    int r = KillScan(ctrl, kind);
    bool ranged = (kind & K_RANGED) != 0;
    if (r > 0) killUntil = c.now + (ranged ? kKillHold : kScanEvery * 2.0);
    else if (r < 0 || !ranged) killUntil = 0;
    return c.now < killUntil;
}

bool AimableShot(int type) {
    for (int t : R.shotTypes)
        if (t == type) return true;
    return false;
}

float VacuumAngle(float dist, float speed) {
    if (speed <= 1.0f) return 0;
    float k = 9.81f * dist / (speed * speed);
    if (k >= 1.0f) return 45.0f;
    return 0.5f * std::asin(k) * 180.0f / kPi;
}

float Zeroing(Object* info, Object* actor, float dist, float speed) {
    if (burst.zeroInfo == info && burst.zeroActor == actor && std::fabs(burst.zeroDist - dist) < 1.0f) return burst.zeroDeg;
    bool drop = true, drag = true;
    float air = 1.22f;
    Object* handler = un::Static(R.bhInstance);
    Object* settings = un::Alive(handler) ? un::Ref(handler, R.bhSettings) : nullptr;
    if (un::Alive(settings)) {
        drop = un::Val<bool>(settings, R.bsDrop, true);
        drag = un::Val<bool>(settings, R.bsDrag, true);
        float a = un::Val<float>(settings, R.bsAir, 1.22f);
        if (a > 0.01f && a < 10.0f) air = a;
    }
    float deg = 0;
    if (drop) {
        deg = -1.0f;
        if (R.zeroing) {
            Object* ref = info;
            float distance = dist, delta = 1.0f, step = 0.015f;
            int iterations = 4, steps = 1000;
            bool ok = false;
            Object* r = un::Call(R.zeroing, nullptr, {&distance, &ref, &drag, &air, &iterations, &delta, &step, &steps}, &ok);
            if (ok && r) {
                float v = std::fabs(il2::Result<float>(r, 1000.0f));
                if (std::isfinite(v) && v <= 45.0f) deg = v;
            }
        }
        if (deg < 0) deg = VacuumAngle(dist, speed);
    }
    burst.zeroInfo = info;
    burst.zeroActor = actor;
    burst.zeroDist = dist;
    burst.zeroDeg = deg;
    return deg;
}

Vec3 Tilt(Vec3 dir, float deg) {
    if (!(deg > 0.0001f)) return dir;
    Vec3 up{0, 1, 0};
    Vec3 perp = un::Sub(up, un::Mul(dir, un::Dot(dir, up)));
    float pl = un::Len(perp);
    if (pl < 1e-3f) return dir;
    perp = un::Mul(perp, 1.0f / pl);
    float r = deg * std::min(pl, 1.0f) * kPi / 180.0f;
    return un::Norm(un::Add(un::Mul(dir, std::cos(r)), un::Mul(perp, std::sin(r))));
}

Vec3 RotateLike(Vec3 from, Vec3 to, Vec3 v) {
    float c = un::Dot(from, to);
    if (c <= -0.999f) return v;
    Vec3 k = Cross(from, to);
    float f = un::Dot(k, v) / (1.0f + c);
    return un::Norm(un::Add(un::Add(un::Mul(v, c), Cross(k, v)), un::Mul(k, f)));
}

bool CrosshairPoint(Object* info, Vec3& point) {
    if (burst.haveAim) {
        point = burst.aim;
        return true;
    }
    Object* cam = un::Static(R.lpCamTr);
    Vec3 pos, fwd;
    if (!un::Alive(cam) || !Pos(cam, pos) || !Fwd(cam, fwd)) return false;
    fwd = un::Norm(fwd);
    if (un::Len(fwd) < 0.5f) return false;
    int mask = info ? un::Val<int>(info, R.biMask) : 0;
    if (mask == 0) mask = ~0;
    un::RayHit hit;
    if (un::Raycast(pos, fwd, 1000.0f, hit, mask) && hit.distance > 1.5f && Finite(hit.point)) point = hit.point;
    else point = un::Add(pos, un::Mul(fwd, 300.0f));
    burst.aim = point;
    burst.haveAim = true;
    return true;
}

bool AimAt(Object* info, Vec3 origin, Vec3& out) {
    if (!info || !target.h || !AimableShot(un::Val<int>(info, R.piType, -1))) return false;
    Object* actor = TargetObj();
    if (!actor) return false;
    Vec3 head = target.head;
    Vec3 fresh;
    if (Fresh(actor, fresh)) head = fresh;
    else if (!target.hasHead) return false;
    float dist = un::Dist(head, origin);
    if (!(dist >= 0.3f && dist <= kAimRange + 25.0f)) return false;
    float speed = un::Val<float>(info, R.biSpeed);
    if (speed > 1.0f) head = un::Add(head, un::Mul(target.vel, dist / speed));
    Vec3 to = un::Sub(head, origin);
    dist = un::Len(to);
    if (!(dist >= 0.3f)) return false;
    Vec3 dir = un::Mul(to, 1.0f / dist);
    out = Tilt(dir, Zeroing(info, actor, dist, speed));
    return Finite(out) && un::Len(out) > 0.5f;
}

bool Straighten(Object* self, Object* info, bool scatter, Object* spawn, Vec3 origin, Vec3 in, Vec3& out) {
    if (un::Val<bool>(self, R.rwScreen)) {
        if (burst.laser < 0) burst.laser = R.rwLaser && un::CallBool(R.rwLaser, self) ? 1 : 0;
        if (!burst.laser) return false;
    }
    Vec3 point;
    if (!CrosshairPoint(info, point)) return false;
    Vec3 want = un::Norm(un::Sub(point, origin));
    Vec3 shot = un::Norm(in);
    if (un::Len(want) < 0.5f || un::Len(shot) < 0.5f) return false;
    Vec3 base = shot;
    if (scatter) {
        Vec3 f;
        if (!Fwd(spawn, f)) return false;
        base = un::Norm(f);
        if (un::Len(base) < 0.5f) return false;
    }
    if (un::Dot(base, want) < std::cos(kRecoilLimit * kPi / 180.0f)) return false;
    out = scatter ? RotateLike(base, shot, want) : want;
    return Finite(out) && un::Len(out) > 0.5f;
}

bool LocalWeapon(Object* self, Object* ctrl) {
    if (un::Val<bool>(self, R.rwLocal)) return true;
    Object* held = hands[0].ctrl ? il2::Pinned(hands[0].ctrl) : nullptr;
    return ctrl && held && ctrl == held;
}

bool Steer(Object* self, Vec3 in, Vec3& out) {
    bool aim = aimOn.load(std::memory_order_relaxed) && target.h != 0, recoil = recoilOn.load(std::memory_order_relaxed);
    if (!aim && !recoil) return false;
    if (!un::Alive(self) || un::Val<bool>(self, R.rwNpc)) return false;
    Object* ctrl = un::Ref(self, R.rwController);
    if (!LocalWeapon(self, ctrl) || (KindOf(ctrl) & (K_ROPE | K_THROWN))) return false;
    LONGLONG now = Ticks();
    if (burst.weapon != self || now - burst.t > tickRate / 60) {
        burst = Burst{};
        burst.weapon = self;
        burst.t = now;
    }
    bool scatter = false;
    Object* info = ShotInfo(self, &scatter);
    Object* spawn = SpawnOf(self);
    Vec3 origin;
    if (!spawn || !Pos(spawn, origin)) return false;
    if (aim && AimAt(info, origin, out)) return true;
    return recoil && Straighten(self, info, scatter, spawn, origin, in, out);
}

bool SteerSafe(Object* self, const Vec3* in, Vec3* out) {
    __try {
        return Steer(self, *in, *out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HkShoot(Object* self, const Vec3* direction, float zeroAngle, int32_t uid, const void* mi) {
    Vec3 d;
    if (direction && self && (aimOn.load(std::memory_order_relaxed) || recoilOn.load(std::memory_order_relaxed)) &&
        SteerSafe(self, direction, &d)) {
        oShoot(self, &d, zeroAngle, uid, mi);
        return;
    }
    oShoot(self, direction, zeroAngle, uid, mi);
}

bool SkipSpent(Object* self) {
    __try {
        return self != nullptr && ammoOn.load(std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void HkSpent(Object* self, const void* mi) {
    if (SkipSpent(self)) return;
    oSpent(self, mi);
}

void HkBowSpent(Object* self, const void* mi) {
    if (SkipSpent(self)) return;
    oBowSpent(self, mi);
}

struct Boost {
    Object* slot[16];
    float saved[16];
    int n = 0;
    ~Boost();
};

void Unboost(Boost* b) {
    __try {
        for (int i = 0; i < b->n; ++i) un::SetVal<float>(b->slot[i], R.sdPoints, b->saved[i]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    b->n = 0;
}

Boost::~Boost() {
    if (n > 0) Unboost(this);
}

void BoostArmour(Object* self, float dmg, Boost* b) {
    Object** items = nullptr;
    int count = 0;
    if (!ListRead(un::Ref(self, R.asSlots), items, count) || !items) return;
    float want = dmg * 2.0f + 1.0f;
    for (int i = 0; i < count && b->n < 16; ++i) {
        Object* s = items[i];
        if (!s || !un::Alive(un::Ref(s, R.sdPiece))) continue;
        float pts = un::Val<float>(s, R.sdPoints);
        if (pts >= want) continue;
        b->slot[b->n] = s;
        b->saved[b->n] = pts;
        ++b->n;
        un::SetVal<float>(s, R.sdPoints, want);
    }
}

void BoostSafe(Object* self, float dmg, Boost* b) {
    __try {
        BoostArmour(self, dmg, b);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

bool ArmourWanted(Object* self, float dmg) {
    __try {
        return armourOn.load(std::memory_order_relaxed) && self && self == armourSelf && std::isfinite(dmg) && dmg > 0.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float HkArmour(Object* self, float dmg, bool demonic, const void* mi) {
    if (!ArmourWanted(self, dmg)) return oArmour(self, dmg, demonic, mi);
    Boost b;
    BoostSafe(self, dmg, &b);
    return oArmour(self, dmg, demonic, mi);
}

Object* ArmourOf() {
    Object* stats = un::Static(R.lpStats);
    if (!un::Alive(stats)) return nullptr;
    Object* a = un::Ref(stats, R.psArmour);
    return un::Alive(a) ? a : nullptr;
}

Method* ReloadOf(Class* c) {
    if (!c) return nullptr;
    auto it = reloads.find(c);
    if (it != reloads.end()) return it->second;
    Method* m = il2::FindMethod(c, "ReloadAmmo", 2);
    if (!m) m = R.rcReload;
    reloads[c] = m;
    return m;
}

void NoReload(const Ctx& c, Object* ctrl, int kind) {
    if (!ctrl || !(kind & K_RANGED) || (kind & (K_BOW | K_THROWN)) || c.now < reloadWait) return;
    Object* stats = un::Static(R.lpStats);
    if (un::Alive(stats) && un::Val<int>(stats, R.psDying, R.aliveState) != R.aliveState) return;
    Object* rw = RangedOf(ctrl);
    Object* ammo = rw ? un::Ref(rw, R.rwAmmo) : nullptr;
    if (!ammo || !un::Val<bool>(ammo, R.amInternal)) return;
    int max = un::Val<int>(ammo, R.amMax), cur = un::Val<int>(ammo, R.amCur);
    if (max <= 0 || cur < 0 || cur >= max) return;
    if (R.amAvailable && !un::CallBool(R.amAvailable, ammo)) {
        reloadWait = c.now + 0.5;
        return;
    }
    Method* m = ReloadOf(il2::ClassOf(ctrl));
    if (!m) return;
    bool single = false;
    un::Call(m, ctrl, {rw, &single});
    if (un::Val<int>(ammo, R.amCur) <= cur) reloadWait = c.now + 0.5;
}

void UnscalePlain(patch::Scaled& s, const Fld& f) {
    Object* o = s.obj.get();
    if (s.have && o && f && std::fabs(un::Val<float>(o, f) - s.written) < 1e-4f) un::SetVal<float>(o, f, s.base);
    s.obj.reset();
    s.have = false;
}

void ScalePlain(Object* o, const Fld& f, float mult, patch::Scaled& s) {
    if (!f) return;
    if (s.have && s.obj.get() != o) UnscalePlain(s, f);
    if (!o || std::fabs(mult - 1.0f) < 1e-4f) {
        if (s.have) UnscalePlain(s, f);
        return;
    }
    float cur = un::Val<float>(o, f);
    if (!s.have) {
        s.obj.set(o);
        s.base = cur;
        s.have = true;
    } else if (std::fabs(cur - s.written) > 1e-4f) {
        s.base = cur;
    }
    float want = s.base * mult;
    if (cur != want) un::SetVal<float>(o, f, want);
    s.written = want;
}

Object* MeleeInfo(Hand& h, Object* ctrl) {
    Object* info = un::Ref(ctrl, R.mcInfo);
    if (un::Alive(info)) return info;
    if (h.info) {
        Object* cached = il2::Pinned(h.info);
        if (un::Alive(cached)) return cached;
    }
    if (h.infoTried || !R.weaponInfo || !un::Alive(h.go)) return nullptr;
    h.infoTried = true;
    Object* found = un::CompInChildren(h.go, R.weaponInfo, false);
    if (!found) return nullptr;
    h.info = il2::Weak(found);
    return found;
}

void Damage(float mult, Object* ctrl, int kind, Hand& h) {
    Object *data = nullptr, *info = nullptr, *melee = nullptr;
    if (mult > 1.0001f && ctrl && (kind & (K_RANGED | K_MELEE))) {
        if (kind & K_RANGED) {
            Object* rw = RangedOf(ctrl);
            data = rw ? un::Ref(rw, R.rwItemData) : nullptr;
        }
        if (!un::Alive(data)) data = h.item ? un::Ref(h.item, R.iiData) : nullptr;
        if (!un::Alive(data)) data = nullptr;
        if (kind & K_MELEE) {
            info = MeleeInfo(h, ctrl);
            if (info && un::Val<bool>(info, R.wiRemote)) info = nullptr;
            melee = info ? un::Ref(info, R.wiMelee) : nullptr;
            if (!melee && data) melee = un::Ref(data, R.idMelee);
        }
    }
    patch::Scale(data, R.idAmmoMult, mult, sAmmo);
    patch::Scale(info, R.wiDamage, mult, sMelee);
    ScalePlain(melee, R.mdCharge, mult, sCharge);
    ScalePlain(melee, R.mdSmash, mult, sSmash);
}

void Flashlight(bool on, Object* right, int rightKind, Object* left, int leftKind) {
    Object* a = on && right && (rightKind & K_FLASH) ? right : nullptr;
    Object* b = on && left && (leftKind & K_FLASH) ? left : nullptr;
    patch::Force<float>(a, R.flDrain, a != nullptr, 0.0f, sFlash[0]);
    patch::Force<float>(b, R.flDrain, b != nullptr, 0.0f, sFlash[1]);
}

void SlowDown(Speed& s) {
    Object* o = s.obj.get();
    if (s.have && o && un::Alive(o) && R.animGetSpeed && R.animSetSpeed) {
        float cur = un::CallFloat(R.animGetSpeed, o, {}, -1.0f);
        if (std::fabs(cur - s.written) < 1e-4f) un::Call(R.animSetSpeed, o, {&s.base});
    }
    s.obj.reset();
    s.have = false;
}

void SpeedUp(Object* o, float mult, Speed& s) {
    if (s.have && s.obj.get() != o) SlowDown(s);
    if (!o || !un::Alive(o) || mult <= 1.0001f || !R.animGetSpeed || !R.animSetSpeed) {
        if (s.have) SlowDown(s);
        return;
    }
    float cur = un::CallFloat(R.animGetSpeed, o, {}, -1.0f);
    if (!std::isfinite(cur) || cur < 0.0f) return;
    if (!s.have) {
        s.obj.set(o);
        s.base = cur;
        s.have = true;
    } else if (std::fabs(cur - s.written) > 1e-4f) {
        s.base = cur;
    }
    float want = s.base * mult;
    if (cur != want) un::Call(R.animSetSpeed, o, {&want});
    s.written = want;
}

Method* IdleOf(Class* c) {
    if (!c) return nullptr;
    auto it = idles.find(c);
    if (it != idles.end()) return it->second;
    Method* m = il2::FindMethod(c, "IsItemIdle", 0);
    if (m && !il2::MethodPointer(m)) m = nullptr;
    idles[c] = m;
    return m;
}

void FireRate(const Ctx& c, float rate, Object* ctrl, int kind) {
    Object* f = rate > 1.0001f && ctrl && (kind & K_RANGED) && !(kind & (K_THROWN | K_ROPE)) ? ctrl : nullptr;
    patch::Scale(f, R.rcFireDelay, f ? 1.0f / rate : 1.0f, sFire);
    bool busy = false;
    if (f) {
        float next = un::Val<float>(f, R.rcNextFire);
        bool shot = f == fireCtrl && next + 1e-4f < lastNext;
        if (shot || (R.rcAttack && un::Val<int>(f, R.rcAttack) != 0)) busyUntil = c.now + kBusyWindow;
        fireCtrl = f;
        lastNext = next;
        if (R.rcCharging && un::Val<bool>(f, R.rcCharging)) {
            busy = true;
        } else if (c.now < busyUntil) {
            Method* idle = IdleOf(il2::ClassOf(f));
            busy = !idle || !un::CallBool(idle, f, {}, true);
        }
    } else {
        fireCtrl = nullptr;
        lastNext = 0;
        busyUntil = 0;
    }
    Object* pa = busy ? un::Ref(f, R.hcAnimator) : nullptr;
    Object* wa = busy ? un::Ref(f, R.rcWeaponAnim) : nullptr;
    SpeedUp(un::Alive(pa) ? pa : nullptr, rate, sSpeed[0]);
    SpeedUp(un::Alive(wa) && wa != pa ? wa : nullptr, rate, sSpeed[1]);
}

void ReleaseAll() {
    ammoOn.store(false);
    recoilOn.store(false);
    aimOn.store(false);
    armourOn.store(false);
    armourSelf = nullptr;
    patch::Unscale(sAmmo, R.idAmmoMult);
    patch::Unscale(sMelee, R.wiDamage);
    UnscalePlain(sCharge, R.mdCharge);
    UnscalePlain(sSmash, R.mdSmash);
    patch::Release(sFlash[0], R.flDrain);
    patch::Release(sFlash[1], R.flDrain);
    patch::Unscale(sFire, R.rcFireDelay);
    SlowDown(sSpeed[0]);
    SlowDown(sSpeed[1]);
    fireCtrl = nullptr;
    lastNext = 0;
    busyUntil = 0;
    ClearRigs();
    DropTarget();
    ClearHand(hands[0]);
    ClearHand(hands[1]);
    burst = Burst{};
    nextScan = 0;
    reloadWait = 0;
    killNext = 0;
    killUntil = 0;
    lastPlayer = nullptr;
}

void Publish() {
    int bad = (blocked.load() | broken.load()) & ~(1 << H_BOW_SPENT);
    if (bad == published || !model || !modelLock) return;
    std::unique_lock<std::mutex> g(*modelLock, std::try_to_lock);
    if (!g.owns_lock()) return;
    int fresh = bad & ~published;
    int other = blocked.load();
    for (core::Feature& f : model->features) {
        int bit = 0;
        if (f.id == "unlimited_ammo") bit = 1 << H_SPENT;
        else if (f.id == "no_recoil" || f.id == "silent_aim" || f.id == "aim_enemies" || f.id == "aim_animals" || f.id == "aim_players")
            bit = 1 << H_SHOOT;
        else if (f.id == "unbreakable_armor") bit = 1 << H_ARMOUR;
        if (!(fresh & bit)) continue;
        f.available = false;
        f.missing = (other & bit) ? "Another mod already changes this part of the game" : "Could not attach to this part of the game";
    }
    published = bad;
}

bool Detoured(void* p) {
    __try {
        const unsigned char* b = (const unsigned char*)p;
        if (b[0] == 0xE9) return true;
        if (b[0] == 0xFF && b[1] == 0x25) return true;
        if (b[0] == 0x48 && b[1] == 0xB8 && b[10] == 0xFF && b[11] == 0xE0) return true;
        if (b[0] == 0x49 && b[1] == 0xBB) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

bool Attach(int id, Method* m, void* detour, void** orig, const char* what) {
    void* target = il2::MethodPointer(m);
    if (!target) return true;
    if (Detoured(target)) {
        blocked.fetch_or(1 << id);
        LOG("%s is already changed by another mod, left alone", what);
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, detour, orig);
    if (st == MH_OK) {
        st = MH_EnableHook(target);
        if (st != MH_OK) MH_RemoveHook(target);
    }
    if (st != MH_OK) {
        broken.fetch_or(1 << id);
        LOG("could not attach to %s: %s", what, MH_StatusToString(st));
        return false;
    }
    hooked.fetch_or(1 << id);
    return true;
}

bool Resolve(std::string& err) {
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    tickRate = freq.QuadPart;
    R.thrown.clear();
    R.shotTypes.clear();

    R.localPlayer = un::C(kSons, "TheForest.Utils", "LocalPlayer");
    if (!R.localPlayer) {
        err = "the game's player was not found";
        return false;
    }
    R.lpInventory = un::F(R.localPlayer, "<Inventory>k__BackingField");
    R.lpCamTr = un::F(R.localPlayer, "<MainCamTr>k__BackingField");
    R.lpStats = un::F(R.localPlayer, "<Stats>k__BackingField");

    R.inventory = un::C(kSons, "TheForest.Items.Inventory", "PlayerInventory");
    R.invSlots = un::F(R.inventory, "_equipmentSlots");
    R.invView = un::F(R.inventory, "_currentView");
    R.viewWorld = (int)il2::EnumValue(il2::Nested(R.inventory, "PlayerViews"), "World", 2);

    R.itemInstance = un::C(kSons, "Sons.Inventory", "ItemInstance");
    R.iiData = un::F(R.itemInstance, "_data");
    R.iiObject = un::F(R.itemInstance, "<ItemObject>k__BackingField");

    R.held = un::C(kSons, "Sons.Weapon", "HeldControllerBase");
    R.ranged = un::C(kSons, "Sons.Weapon", "RangedWeaponController");
    R.bow = un::C(kSons, "Sons.Weapon", "BowWeaponController");
    R.melee = un::C(kSons, "Sons.Weapon", "MeleeWeaponController");
    R.flashlight = un::C(kSons, "Sons.Weapon", "FlashlightController");
    R.ropeGun = un::C(kSons, "Sons.Weapon", "RopeGunController");
    for (const char* n : {"GrenadeWeaponController", "MolotovWeaponController", "FlareWeaponController", "SmallRockWeaponController",
                          "TimeBombWeaponController"})
        if (Class* t = un::C(kSons, "Sons.Weapon", n)) R.thrown.push_back(t);

    R.rcWeapon = un::F(R.ranged, "_rangedWeapon");
    R.rcGetWeapon = un::M(R.ranged, "GetRangedWeapon", 0);
    R.rcReload = un::M(R.ranged, "ReloadAmmo", 2);
    R.spent = un::M(R.ranged, "OnAmmoSpent", 0);
    R.bowSpent = un::M(R.bow, "OnAmmoSpent", 0);
    if (R.bowSpent && il2::MethodPointer(R.bowSpent) == il2::MethodPointer(R.spent)) R.bowSpent = nullptr;
    R.mcInfo = un::F(R.melee, "_weaponInfo");
    R.flDrain = un::F(R.flashlight, "_powerDrainRate");
    R.rcFireDelay = un::F(R.ranged, "_fireDelay");
    R.rcNextFire = un::F(R.ranged, "_nextFireDelay");
    R.rcAttack = un::F(R.ranged, "_attackState");
    R.rcCharging = un::F(R.ranged, "_isCharging");
    R.rcWeaponAnim = un::F(R.ranged, "_weaponAnimator");
    R.hcAnimator = un::F(R.held, "_playerAnimator");
    R.animGetSpeed = un::M(un::E.animator, "get_speed", 0);
    R.animSetSpeed = un::M(un::E.animator, "set_speed", std::vector<std::string>{"System.Single"});
    R.animBone = un::M(un::E.animator, "GetBoneTransform", 1);
    R.headBone = (int)il2::EnumValue(un::C("UnityEngine.AnimationModule", "UnityEngine", "HumanBodyBones"), "Head", 10);

    R.rangedWeapon = un::C(kSons, "Sons.Weapon", "RangedWeapon");
    R.rwAmmo = un::F(R.rangedWeapon, "_ammo");
    R.rwNpc = un::F(R.rangedWeapon, "_controlledByAI");
    R.rwScreen = un::F(R.rangedWeapon, "_useModifiedScreenPostion");
    R.rwLocal = un::F(R.rangedWeapon, "_isLocalPlayer");
    R.rwItemData = un::F(R.rangedWeapon, "_itemData");
    R.rwSpawn = un::F(R.rangedWeapon, "_projectilePhysicalSpawnPoint");
    R.rwController = un::F(R.rangedWeapon, "_rangedWeaponController");
    R.rwLaser = un::M(R.rangedWeapon, "HasLaserSight", 0);
    R.wSpawn = un::F(R.rangedWeapon, "physicalSpawnPoint");
    std::vector<std::string> shootArgs{"UnityEngine.Vector3", "System.Single", "System.Int32"};
    R.shoot = un::M(R.rangedWeapon, "Shoot", shootArgs);
    R.weapon = un::C(kBallistics, "Ballistics", "Weapon");
    Method* baseShoot = R.weapon ? il2::FindMethod(R.weapon, "Shoot", shootArgs) : nullptr;
    if (R.shoot && baseShoot && il2::MethodPointer(R.shoot) == il2::MethodPointer(baseShoot)) R.shoot = nullptr;

    R.ammo = il2::Nested(R.rangedWeapon, "Ammo");
    if (!R.ammo && R.rangedWeapon) un::Missing("RangedWeapon.Ammo");
    R.amInternal = un::F(R.ammo, "_usesInternalAmmoStorage");
    R.amMax = un::F(R.ammo, "_maxCount");
    R.amCur = un::F(R.ammo, "_currentCount");
    R.amProps = un::F(R.ammo, "_properties");
    R.amAvailable = un::M(R.ammo, "IsAvailableInInventory", 0);

    R.ammoProps = un::C(kItems, "", "AmmoProperties");
    R.apScatter = un::F(R.ammoProps, "ScatterShot");
    R.apProjectile = un::F(R.ammoProps, "ProjectileInfo");
    R.bulletInfo = un::C(kBallistics, "Ballistics", "BulletInfo");
    R.biSpeed = un::F(R.bulletInfo, "maxSpeed");
    R.biMask = un::F(R.bulletInfo, "hitMask");
    R.projectileInfo = un::C(kItems, "", "ProjectileInfo");
    R.piType = un::F(R.projectileInfo, "_projectileType");
    Class* projectileType = un::C(kItems, "", "ImpactProjectileType");
    for (const char* n : {"Bullet", "Arrow", "ShotgunBuckshot", "NonLethal", "HeavyBullet", "ShotgunSlug"}) {
        long long v = il2::EnumValue(projectileType, n, -1);
        if (v >= 0) R.shotTypes.push_back((int)v);
    }
    if (R.shotTypes.empty()) R.shotTypes = {1, 2, 3, 4, 6, 7};
    R.ballistics = un::C(kBallistics, "Ballistics", "BulletBallistics");
    R.zeroing = un::M(R.ballistics, "calculateZeroingAngle", 8);
    R.bulletHandler = un::C(kBallistics, "Ballistics", "BulletHandler");
    R.bhInstance = un::F(R.bulletHandler, "_instance");
    R.bhSettings = un::F(R.bulletHandler, "ballisticSettings");
    R.ballisticSettings = un::C(kBallistics, "Ballistics", "BallisticSettings");
    R.bsDrop = un::F(R.ballisticSettings, "BulletDrop");
    R.bsDrag = un::F(R.ballisticSettings, "BulletDrag");
    R.bsAir = un::F(R.ballisticSettings, "AirDensity");

    R.itemData = un::C(kItems, "Sons.Items.Core", "ItemData");
    R.idAmmoMult = un::F(R.itemData, "AmmoDamageMult");
    R.idMelee = un::F(R.itemData, "MeleeWeaponData");
    R.meleeData = un::C(kItems, "Sons.Items.Core", "MeleeWeaponData");
    R.mdCharge = un::F(R.meleeData, "ChargeAttackDamage");
    R.mdSmash = un::F(R.meleeData, "GroundSmashDamage");
    R.weaponInfo = un::C(kSons, "", "weaponInfo");
    R.wiDamage = un::F(R.weaponInfo, "_weaponDamage");
    R.wiRemote = un::F(R.weaponInfo, "remotePlayer");
    R.wiMelee = un::F(R.weaponInfo, "_meleeWeaponData");

    R.armourSystem = un::C(kSons, "Sons.Wearable.Armour", "PlayerArmourSystem");
    R.asSlots = un::F(R.armourSystem, "_armourSlotData");
    R.armourCalc = un::M(R.armourSystem, "CalculateRemainingDamageAfterArmourHit", 2);
    R.slotData = il2::Nested(R.armourSystem, "ArmourSlotData");
    if (!R.slotData && R.armourSystem) un::Missing("PlayerArmourSystem.ArmourSlotData");
    R.sdPiece = un::F(R.slotData, "ArmourPiece");
    R.sdPoints = un::F(R.slotData, "RemainingArmourpoints");
    R.playerStats = un::C(kSons, "", "PlayerStats");
    R.psArmour = un::F(R.playerStats, "_armourSystem");
    R.psDying = un::F(R.playerStats, "_dyingState");
    R.aliveState = (int)il2::EnumValue(il2::Nested(R.playerStats, "DyingState"), "Alive", 0);

    R.actorManager = un::C(kVail, "Sons.Ai.Vail", "VailActorManager");
    R.mgrInstance = un::F(R.actorManager, "_instance");
    R.mgrActive = un::F(R.actorManager, "_activeActors");
    R.actor = un::C(kVail, "Sons.Ai.Vail", "VailActor");
    R.vaId = un::F(R.actor, "_id");
    R.vaDead = un::F(R.actor, "_isDead");
    R.vaDamage = un::F(R.actor, "_damageEnabled");
    R.vaHead = un::F(R.actor, "_visionHeadJoint");
    R.vaCapHeight = un::F(R.actor, "_capsuleHeight");
    R.vaCapCenter = un::F(R.actor, "_capsuleCenter");
    R.vaScale = un::F(R.actor, "_actorScaleMult");
    R.vaCtrl = un::F(R.actor, "_characterController");
    R.colBounds = un::M(un::E.collider, "get_bounds", 0);
    R.vaDying = un::M(R.actor, "IsDying", 0);
    R.vailTypes = un::C(kVail, "Sons.Ai.Vail", "VailTypes");
    R.vtClass = un::M(R.vailTypes, "GetActorClass", 1);
    R.vtFriendly = un::M(R.vailTypes, "IsPlayerFriendly", 1);
    Class* classId = un::C(kVail, "Sons.Ai.Vail", "VailActorClassId");
    R.clsCannibal = (int)il2::EnumValue(classId, "Cannibal", 1);
    R.clsCreepy = (int)il2::EnumValue(classId, "Creepy", 2);
    R.clsAnimal = (int)il2::EnumValue(classId, "Animal", 3);
    Class* typeId = un::C(kVail, "Sons.Ai.Vail", "VailActorTypeId");
    const char* friendNames[4] = {"Timmy", "Robby", "Virginia", "PlayerRobby"};
    for (int i = 0; i < 4; ++i) R.friends[i] = (int)il2::EnumValue(typeId, friendNames[i], R.friends[i]);

    R.boltEntity = un::C("bolt", "", "BoltEntity");
    R.beAttached = R.boltEntity ? il2::Getter(R.boltEntity, "isAttached") : nullptr;
    R.beControl = R.boltEntity ? il2::Getter(R.boltEntity, "hasControl") : nullptr;
    R.tracker = un::C(kSons, "", "PlayerTracker");
    R.trInstance = un::F(R.tracker, "_instance");
    R.trEntities = un::F(R.tracker, "AllPlayerEntities");
    R.mpUtil = un::C("Sons.Multiplayer", "Sons.Multiplayer", "MultiplayerUtilities");
    R.mpEntities = un::F(R.mpUtil, "_playerEntities");
    R.remoteSetup = un::C(kSons, "Sons.Multiplayer.Client", "CoopPlayerRemoteSetup");
    R.rsAnimator = un::F(R.remoteSetup, "_playerAnimator");

    bool hands = R.lpInventory && R.invSlots && R.iiObject && R.held && un::E.compGetComponentInChildren;
    bool spawn = R.wSpawn || R.rwSpawn;
    bool shot = il2::MethodPointer(R.shoot) && R.rwLocal && spawn && R.lpCamTr && un::E.raycast && un::E.trForward;
    support.ammo = il2::MethodPointer(R.spent) != nullptr;
    support.reload = hands && R.ranged && R.rcWeapon && R.rwAmmo && R.amInternal && R.amMax && R.amCur && R.rcReload;
    support.recoil = shot && R.rwScreen;
    support.damage = hands && R.idAmmoMult && (R.ranged || R.melee);
    support.aim = shot && hands && R.ranged && R.rcWeapon && R.mgrInstance && R.mgrActive && R.vaId && R.vaDead && R.vtClass &&
                  R.rwAmmo && R.amProps && R.apProjectile && R.piType;
    support.armour = il2::MethodPointer(R.armourCalc) && R.asSlots && R.sdPiece && R.sdPoints && R.lpStats && R.psArmour;
    support.flash = hands && R.flashlight && R.flDrain;
    support.rate = hands && R.ranged && R.rcFireDelay && R.rcNextFire;
    support.players = R.beAttached && R.trInstance && R.trEntities;
    if (!support.players) support.players = R.beAttached && R.mpEntities;

    std::string off;
    const std::pair<const char*, bool> parts[] = {{"unlimited ammo", support.ammo}, {"no reload", support.reload},
                                                  {"no recoil", support.recoil},    {"weapon damage", support.damage},
                                                  {"silent aim", support.aim},      {"unbreakable armor", support.armour},
                                                  {"flashlight", support.flash},    {"fire rate", support.rate},
                                                  {"silent aim on players", support.aim && support.players}};
    for (const auto& p : parts)
        if (!p.second) off += std::string(off.empty() ? "" : ", ") + p.first;
    if (!off.empty()) LOG("Combat cheats not available: %s", off.c_str());
    return true;
}

bool Supported(const std::string& id) {
    if (id == "unlimited_ammo") return support.ammo;
    if (id == "no_reload") return support.reload;
    if (id == "no_recoil") return support.recoil;
    if (id == "weapon_damage") return support.damage;
    if (id == "silent_aim") return support.aim;
    if (id == "aim_enemies" || id == "aim_animals" || id == "aim_players") return support.aim;
    if (id == "fire_rate") return support.rate;
    if (id == "unbreakable_armor") return support.armour;
    if (id == "unlimited_flashlight") return support.flash;
    return false;
}

void Tick(const Ctx& c) {
    Publish();
    if (!c.inWorld || !c.player) {
        ReleaseAll();
        return;
    }
    if (c.player != lastPlayer) {
        ReleaseAll();
        lastPlayer = c.player;
    }
    int live = hooked.load();
    bool ammo = support.ammo && (live & (1 << H_SPENT)) && c.s.On("unlimited_ammo");
    bool reload = support.reload && c.s.On("no_reload");
    bool recoil = support.recoil && (live & (1 << H_SHOOT)) && c.s.On("no_recoil");
    bool aim = support.aim && (live & (1 << H_SHOOT)) && c.s.On("silent_aim");
    bool armour = support.armour && (live & (1 << H_ARMOUR)) && c.s.On("unbreakable_armor");
    bool flash = support.flash && c.s.On("unlimited_flashlight");
    float mult = support.damage ? std::clamp(c.s.Value("weapon_damage", 1.0f), 1.0f, 10.0f) : 1.0f;
    bool kill = support.damage && c.session == core::Session::Client && c.cheats && c.s.On("one_hit_kill");
    if (kill) WarnAchievementsOnce();
    else killNext = killUntil = 0;
    bool damage = mult > 1.0001f || kill;
    float rate = support.rate ? std::clamp(c.s.Value("fire_rate", 1.0f), 1.0f, 10.0f) : 1.0f;
    bool fire = rate > 1.0001f;
    filter.enemies = c.s.On("aim_enemies");
    filter.animals = c.s.On("aim_animals");
    filter.players = c.s.On("aim_players");

    ammoOn.store(ammo);
    recoilOn.store(recoil);
    armourSelf = armour ? ArmourOf() : nullptr;
    armourOn.store(armour && armourSelf != nullptr);

    Object* inv = nullptr;
    if (reload || damage || flash || aim || recoil || fire) {
        inv = un::Static(R.lpInventory);
        if (!un::Alive(inv)) inv = nullptr;
    }
    Object* right = inv ? HandCtrl(0, inv, c.now) : nullptr;
    Object* left = inv && flash ? HandCtrl(1, inv, c.now) : nullptr;
    if (!inv) ClearHand(hands[0]);
    if (!inv || !flash) ClearHand(hands[1]);
    int rightKind = right ? hands[0].kind : 0;
    int leftKind = left ? hands[1].kind : 0;
    bool world = inv && (!R.invView || un::Val<int>(inv, R.invView) == R.viewWorld);

    if (reload && world) NoReload(c, right, rightKind);
    if (kill && world && Killing(c, right, rightKind)) mult = std::max(mult, kKillMult);
    else if (kill && !world) killNext = killUntil = 0;
    Damage(mult, right, rightKind, hands[0]);
    Flashlight(flash, right, rightKind, left, leftKind);
    FireRate(c, fire && world ? rate : 1.0f, right, rightKind);
    aimOn.store(aim);
    Aim(c, aim && world, right, rightKind);
}

bool Action(const std::string&, const Ctx&) { return false; }

void Release(const Ctx&) { ReleaseAll(); }

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
}

bool Start(std::string& err) {
    bool good = true;
    if (support.ammo && !Attach(H_SPENT, R.spent, (void*)&HkSpent, (void**)&oSpent, "RangedWeaponController.OnAmmoSpent")) good = false;
    if (support.ammo && R.bowSpent &&
        !Attach(H_BOW_SPENT, R.bowSpent, (void*)&HkBowSpent, (void**)&oBowSpent, "BowWeaponController.OnAmmoSpent"))
        good = false;
    if ((support.recoil || support.aim) && !Attach(H_SHOOT, R.shoot, (void*)&HkShoot, (void**)&oShoot, "RangedWeapon.Shoot")) good = false;
    if (support.armour &&
        !Attach(H_ARMOUR, R.armourCalc, (void*)&HkArmour, (void**)&oArmour, "PlayerArmourSystem.CalculateRemainingDamageAfterArmourHit"))
        good = false;
    if (!good) err = "some weapon and armor parts are already changed by another mod or could not be attached";
    return good;
}

}

const Group& WeaponsGroup() {
    static Group g{"Combat", &Resolve, &Supported, &Tick, &Action, &Release, &Wire, &Start};
    return g;
}

}
