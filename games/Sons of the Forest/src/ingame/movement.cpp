#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/patch.h"
#include "ingame/unity.h"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sotf {

namespace {

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;
using un::Vec3;

const char* kSons = "Sons";
const float kFlyBase = 10.0f;
const float kSkin = 0.05f;
const double kGuardLimit = 20.0;
const double kCompRetry = 2.0;

const char* kIds[] = {"noclip", "flight", "flight_speed", "unlimited_jumps", "walk_speed", "run_speed", "jump_height"};

struct MoveRefs {
    Class *localPlayer = nullptr, *fpc = nullptr, *anim = nullptr, *stats = nullptr, *vitals = nullptr, *inventory = nullptr,
          *dyingState = nullptr, *views = nullptr, *cutscenes = nullptr, *chat = nullptr, *pause = nullptr, *knight = nullptr,
          *zip = nullptr, *caves = nullptr, *killBox = nullptr, *terrain = nullptr, *saves = nullptr, *trigger = nullptr;
    Fld lpInstance, lpFpc, lpRb, lpTr, lpGo, lpCam, lpAnim, lpStats, lpVitals, lpInv, lpSpecial, lpGliding, lpGolf, lpRaft;
    Fld walk, run, crouch, storeMax, slowActive, walkBefore, runBefore, jumpMult, jumpAfter, jumpFrame, canJump, crouchBlocked,
        jumpBlock, locked, moveLocked, prevGrounded, layers, fpcRb;
    Fld rootMotion, onRope, logSled, swimming, sledding;
    Fld hasDied, dying, downed, view;
    Fld inCaves, killInstance, killDepth;
    Method *superJump = nullptr, *clearFall = nullptr, *grounded = nullptr, *setGrounded = nullptr, *isJumping = nullptr,
           *setJumping = nullptr, *wasGrounded = nullptr, *jumpInput = nullptr, *onJump = nullptr, *fpcEnable = nullptr, *bounds = nullptr;
    Method *teleport = nullptr, *caveCheck = nullptr, *cutscene = nullptr, *chatOpen = nullptr, *pauseActive = nullptr,
           *knightActive = nullptr, *onZip = nullptr, *cavesGet = nullptr, *terrainHeight = nullptr, *capsuleCast = nullptr,
           *checkCapsule = nullptr, *layer = nullptr, *save = nullptr;
    int alive = 0, world = 2, ignore = 1;
    std::vector<int> busyViews;
} R;

struct Avail {
    bool player = false, walk = false, run = false, jump = false, jumps = false, fly = false, flight = false, noclip = false;
} A;

struct Body {
    Object *lp = nullptr, *fpc = nullptr, *rb = nullptr, *tr = nullptr, *cam = nullptr, *anim = nullptr, *stats = nullptr,
           *vit = nullptr, *inv = nullptr, *go = nullptr, *special = nullptr;
};

struct Box {
    Vec3 c, e;
};

struct Capsule {
    Vec3 a, b;
    float r = 0.3f;
    float feet = 0;
};

struct Cap {
    patch::Hold obj;
    float base = 0, written = 0;
    bool have = false;
};

struct Jump {
    patch::Hold obj;
    float origMult = 0, origAfter = 0, written = 0;
    bool have = false;
    int lastFrame = 0;
    bool guard = false;
    double since = 0;
    int groundedFrames = 0;
};

struct Fly {
    patch::Hold player, fpcTok, rbTok;
    bool active = false;
    bool clipped = false;
    bool wasNoclip = false;
};

patch::Scaled walkS, crouchS, runS;
Cap cap;
Jump jump;
Fly fly;
patch::Hold compOwner, knightComp, zipComp;
double compRetry = 0;

std::atomic<bool> landReady{false};
std::atomic<DWORD> mainThread{0};
std::atomic<bool> landNoted{false};
using SaveFn = void (*)(Object*, Object*, bool, const void*);
SaveFn oSave = nullptr;
bool saveHooked = false;

bool Inst(const Fld& f) { return f && !f.isStatic; }

bool Stat(const Fld& f) { return f && f.isStatic; }

bool Flag(Object* o, const Fld& f) { return o && Inst(f) && un::Val<uint8_t>(o, f) != 0; }

Object* Live(Object* o) { return un::Alive(o) ? o : nullptr; }

bool LiveCtx(const Ctx& c) { return c.inWorld && c.player && c.cheats; }

bool FindBody(Object* lp, Body& b) {
    b = Body{};
    if (!A.player || !un::Alive(lp)) return false;
    b.lp = lp;
    b.fpc = Live(un::Ref(lp, R.lpFpc));
    if (!b.fpc) return false;
    b.rb = Live(un::Ref(lp, R.lpRb));
    if (!b.rb) b.rb = Live(un::Ref(b.fpc, R.fpcRb));
    b.tr = Live(un::Ref(lp, R.lpTr));
    if (!b.tr) b.tr = un::TransformOf(b.fpc);
    b.cam = Live(un::Ref(lp, R.lpCam));
    b.anim = Live(un::Ref(lp, R.lpAnim));
    b.stats = Live(un::Ref(lp, R.lpStats));
    b.vit = Live(un::Ref(lp, R.lpVitals));
    b.inv = Live(un::Ref(lp, R.lpInv));
    b.go = Live(un::Ref(lp, R.lpGo));
    b.special = Live(un::Ref(lp, R.lpSpecial));
    return true;
}

float Mult(const Ctx& c, const char* id, float lo, float hi) {
    float v = c.s.Value(id, 1.0f);
    if (!std::isfinite(v)) v = 1.0f;
    return std::clamp(v, lo, hi);
}

bool Down(const Body& b) {
    if (Flag(b.vit, R.hasDied)) return true;
    if (b.stats && Inst(R.dying) && un::Val<int>(b.stats, R.dying, R.alive) != R.alive) return true;
    return Flag(b.stats, R.downed);
}

int View(const Body& b) { return b.inv && Inst(R.view) ? un::Val<int>(b.inv, R.view, R.world) : R.world; }

bool BusyView(int v) { return std::find(R.busyViews.begin(), R.busyViews.end(), v) != R.busyViews.end(); }

bool Cutscene() { return R.cutscene && un::CallBool(R.cutscene, nullptr); }

bool ChatOpen() { return R.chatOpen && un::CallBool(R.chatOpen, nullptr); }

bool Paused() { return R.pauseActive && un::CallBool(R.pauseActive, nullptr); }

bool InCaves() {
    if (Stat(R.inCaves)) return un::StaticVal<bool>(R.inCaves, false);
    return R.cavesGet && un::CallBool(R.cavesGet, nullptr);
}

bool Kinematic(Object* rb) { return rb && un::E.rbGetKinematic && un::CallBool(un::E.rbGetKinematic, rb); }

void SetKinematic(Object* rb, bool on) {
    if (rb && un::E.rbSetKinematic) un::Call(un::E.rbSetKinematic, rb, {&on});
}

void StopBody(Object* rb) {
    if (!rb || !un::E.rbSetVelocity) return;
    Vec3 zero{};
    un::Call(un::E.rbSetVelocity, rb, {&zero});
}

void ClearFall(Object* fpc) {
    if (fpc && R.clearFall) un::Call(R.clearFall, fpc);
}

void SetFpc(Object* fpc, bool on) {
    if (!fpc) return;
    if (R.fpcEnable) un::Call(R.fpcEnable, fpc, {&on});
    else un::SetEnabled(fpc, on);
}

void Teleport(Object* tr, Vec3 p) {
    if (R.teleport) un::Call(R.teleport, nullptr, {&p});
    else if (tr) un::SetPosition(tr, p);
}

void MoveTo(const Body& b, Vec3 p) {
    if (!b.tr) return;
    Vec3 cur = un::Position(b.tr);
    if (un::Dist(cur, p) < 0.01f) return;
    if (R.caveCheck) {
        bool canExit = true;
        un::Call(R.caveCheck, nullptr, {&p, &canExit});
    }
    Teleport(b.tr, p);
}

void Comps(const Body& b, double now) {
    if (compOwner.get() != b.fpc) {
        compOwner.set(b.fpc);
        knightComp.reset();
        zipComp.reset();
        compRetry = 0;
    }
    if (!b.special) return;
    bool needKnight = R.knight && R.knightActive && !un::Alive(knightComp.get());
    bool needZip = R.zip && R.onZip && !un::Alive(zipComp.get());
    if ((!needKnight && !needZip) || now < compRetry) return;
    compRetry = now + kCompRetry;
    if (needKnight) knightComp.set(un::CompInChildren(b.special, R.knight, true));
    if (needZip) zipComp.set(un::CompInChildren(b.special, R.zip, true));
}

bool Vehicle(const Body& b) {
    if (Stat(R.lpGliding) && un::StaticVal<bool>(R.lpGliding, false)) return true;
    if (Stat(R.lpGolf) && un::StaticVal<bool>(R.lpGolf, false)) return true;
    if (Stat(R.lpRaft) && Live(un::Static(R.lpRaft))) return true;
    if (Flag(b.anim, R.onRope) || Flag(b.anim, R.sledding) || Flag(b.anim, R.logSled)) return true;
    Object* k = Live(knightComp.get());
    if (k && un::CallBool(R.knightActive, k)) return true;
    Object* z = Live(zipComp.get());
    if (z && un::CallBool(R.onZip, z)) return true;
    return false;
}

bool GameOwns(const Body& b) {
    if (Down(b)) return true;
    if (BusyView(View(b))) return true;
    if (Cutscene()) return true;
    return Vehicle(b);
}

int Mask(const Body& b) {
    int m = Inst(R.layers) ? un::Val<int>(b.fpc, R.layers) : 0;
    if (m == 0) m = ~(1 << 2);
    int l = b.go && R.layer ? un::CallInt(R.layer, b.go, {}, -1) : -1;
    if (l >= 0 && l < 32) m &= ~(1 << l);
    return m;
}

Capsule Shell(const Body& b, Vec3 pos) {
    Box bx{};
    bool ok = false;
    if (R.bounds) {
        Object* r = un::Call(R.bounds, b.fpc, {}, &ok);
        if (ok) bx = il2::Result<Box>(r, Box{});
    }
    bool good = ok && std::isfinite(bx.c.x) && std::isfinite(bx.c.y) && std::isfinite(bx.c.z) && std::isfinite(bx.e.x) &&
                std::isfinite(bx.e.y) && bx.e.y > 0.3f && bx.e.y < 5.0f && bx.e.x > 0.05f && bx.e.x < 3.0f &&
                un::Dist(bx.c, pos) < 10.0f;
    if (!good) {
        bx.c = un::Add(pos, Vec3{0, 0.9f, 0});
        bx.e = Vec3{0.35f, 0.9f, 0.35f};
    }
    Capsule k;
    k.r = std::max(bx.e.x - kSkin, 0.1f);
    float half = std::max(bx.e.y - k.r - kSkin, 0.0f);
    k.a = un::Add(bx.c, Vec3{0, half, 0});
    k.b = un::Sub(bx.c, Vec3{0, half, 0});
    k.feet = pos.y - (bx.c.y - bx.e.y);
    return k;
}

Capsule Shift(const Capsule& k, Vec3 d) {
    Capsule o = k;
    o.a = un::Add(k.a, d);
    o.b = un::Add(k.b, d);
    return o;
}

bool Cast(const Capsule& k, Vec3 dir, float dist, int mask, un::RayHit& hit) {
    hit = un::RayHit{};
    if (!R.capsuleCast || !(dist > 0.0f)) return false;
    Vec3 a = k.a, b = k.b;
    float r = k.r;
    int q = R.ignore;
    return un::CallBool(R.capsuleCast, nullptr, {&a, &b, &r, &dir, &hit, &dist, &mask, &q});
}

bool Blocked(const Capsule& k, int mask) {
    if (!R.checkCapsule) return false;
    Vec3 a = k.a, b = k.b;
    float r = k.r;
    int q = R.ignore;
    return un::CallBool(R.checkCapsule, nullptr, {&a, &b, &r, &mask, &q});
}

float TerrainAt(Vec3 p) {
    if (!R.terrainHeight) return NAN;
    float t = un::CallFloat(R.terrainHeight, nullptr, {&p}, NAN);
    return std::isfinite(t) && std::fabs(t) < 100000.0f ? t : NAN;
}

float KillFloor() {
    if (!Stat(R.killInstance) || !Inst(R.killDepth)) return NAN;
    Object* kb = Live(un::Static(R.killInstance));
    if (!kb) return NAN;
    float d = un::Val<float>(kb, R.killDepth, NAN);
    return std::isfinite(d) ? d : NAN;
}

Vec3 Sweep(const Body& b, Vec3 pos, Vec3 move) {
    float dist = un::Len(move);
    if (dist < 1e-5f) return pos;
    Vec3 n = un::Mul(move, 1.0f / dist);
    int mask = Mask(b);
    Capsule k = Shell(b, pos);
    un::RayHit hit;
    if (!Cast(k, n, dist + kSkin, mask, hit)) return un::Add(pos, move);
    float allowed = std::clamp(hit.distance - kSkin, 0.0f, dist);
    Vec3 done = un::Mul(n, allowed);
    Vec3 rest = un::Sub(move, done);
    Vec3 nrm = un::Norm(hit.normal);
    Vec3 slide = un::Sub(rest, un::Mul(nrm, un::Dot(rest, nrm)));
    float sl = un::Len(slide);
    if (sl < 1e-4f) return un::Add(pos, done);
    Vec3 n2 = un::Mul(slide, 1.0f / sl);
    Capsule k2 = Shift(k, done);
    if (!Cast(k2, n2, sl + kSkin, mask, hit)) return un::Add(un::Add(pos, done), slide);
    float a2 = std::clamp(hit.distance - kSkin, 0.0f, sl);
    return un::Add(un::Add(pos, done), un::Mul(n2, a2));
}

Vec3 SafePlace(const Body& b, Vec3 pos) {
    int mask = Mask(b);
    Capsule k = Shell(b, pos);
    bool caves = InCaves();
    Vec3 p = pos;
    float t = caves ? NAN : TerrainAt(pos);
    if (std::isfinite(t)) {
        float bottom = p.y - k.feet;
        if (t > bottom + 0.05f) {
            float lift = t - bottom + 0.1f;
            p.y += lift;
            k = Shift(k, Vec3{0, lift, 0});
        }
    }
    if (Blocked(k, mask)) {
        for (float dy = 0.5f; dy <= 30.0f; dy += 0.5f)
            if (!Blocked(Shift(k, Vec3{0, dy, 0}), mask)) return un::Add(p, Vec3{0, dy, 0});
        if (!caves) {
            un::RayHit hit;
            Vec3 from = un::Add(p, Vec3{0, 300.0f, 0});
            if (un::Raycast(from, Vec3{0, -1.0f, 0}, 400.0f, hit, mask, R.ignore)) return Vec3{p.x, hit.point.y + k.feet + 0.1f, p.z};
            if (std::isfinite(t)) return Vec3{p.x, t + k.feet + 0.1f, p.z};
        }
        for (float dy = 0.5f; dy <= 10.0f; dy += 0.5f)
            if (!Blocked(Shift(k, Vec3{0, -dy, 0}), mask)) return un::Sub(p, Vec3{0, dy, 0});
        return p;
    }
    float floor = caves ? NAN : KillFloor();
    if (std::isfinite(floor) && p.y < floor + 2.0f && std::isfinite(t) && t + k.feet + 0.1f > p.y) return Vec3{p.x, t + k.feet + 0.1f, p.z};
    return p;
}

void Disarm(Object* fpc) {
    if (!fpc) return;
    ClearFall(fpc);
    bool no = false, yes = true;
    if (R.setGrounded) un::Call(R.setGrounded, fpc, {&no});
    if (Inst(R.prevGrounded)) un::SetVal<uint8_t>(fpc, R.prevGrounded, 0);
    if (R.setJumping) un::Call(R.setJumping, fpc, {&yes});
}

bool Owned() { return fly.fpcTok.held() || fly.rbTok.held(); }

void ForgetChanged() {
    Object* f = fly.fpcTok.get();
    if (fly.fpcTok.held() && (!un::Alive(f) || un::Enabled(f))) fly.fpcTok.reset();
    Object* r = fly.rbTok.get();
    if (fly.rbTok.held() && (!un::Alive(r) || !Kinematic(r))) fly.rbTok.reset();
}

void RawRestore() {
    Object* r = fly.rbTok.get();
    if (fly.rbTok.held() && un::Alive(r) && Kinematic(r)) {
        SetKinematic(r, false);
        StopBody(r);
    }
    fly.rbTok.reset();
    Object* f = fly.fpcTok.get();
    bool had = fly.fpcTok.held() && un::Alive(f);
    if (had && !un::Enabled(f)) SetFpc(f, true);
    fly.fpcTok.reset();
    if (had) Disarm(f);
}

void DropTokens() {
    fly.fpcTok.reset();
    fly.rbTok.reset();
}

void SafeRelease(const Body& b) {
    if (fly.clipped && b.tr && fly.player.get() == b.fpc) MoveTo(b, SafePlace(b, un::Position(b.tr)));
    bool mine = fly.player.get() == b.fpc;
    RawRestore();
    if (mine) Disarm(b.fpc);
    fly.active = false;
    fly.clipped = false;
    fly.wasNoclip = false;
}

bool CanTake(const Body& b) {
    if (!b.rb || !b.tr) return false;
    bool fpcOk = fly.fpcTok.get() == b.fpc || (un::Enabled(b.fpc) && !Flag(b.fpc, R.moveLocked));
    if (!fpcOk) return false;
    if (fly.rbTok.get() != b.rb && Kinematic(b.rb)) return false;
    if (Flag(b.fpc, R.locked)) return false;
    if (View(b) != R.world) return false;
    if (Flag(b.anim, R.rootMotion)) return false;
    return true;
}

void TakeOver(const Body& b) {
    ClearFall(b.fpc);
    if (!Kinematic(b.rb)) {
        StopBody(b.rb);
        SetKinematic(b.rb, true);
        fly.rbTok.set(b.rb);
    } else if (!fly.rbTok.held()) {
        fly.rbTok.set(b.rb);
    }
    if (un::Enabled(b.fpc)) {
        SetFpc(b.fpc, false);
        fly.fpcTok.set(b.fpc);
    } else if (!fly.fpcTok.held()) {
        fly.fpcTok.set(b.fpc);
    }
    fly.active = true;
}

void Reassert(const Body& b) {
    if (un::Enabled(b.fpc)) {
        SetFpc(b.fpc, false);
        if (fly.fpcTok.get() != b.fpc) fly.fpcTok.set(b.fpc);
    }
    if (!Kinematic(b.rb)) {
        StopBody(b.rb);
        SetKinematic(b.rb, true);
        if (fly.rbTok.get() != b.rb) fly.rbTok.set(b.rb);
    }
}

void Steer(const Ctx& c, const Body& b, bool noclip) {
    if (!(c.dt > 0.0f)) return;
    int fw = (KeyDown('W') ? 1 : 0) - (KeyDown('S') ? 1 : 0);
    int rt = (KeyDown('D') ? 1 : 0) - (KeyDown('A') ? 1 : 0);
    int up = (KeyDown(VK_SPACE) ? 1 : 0) - (KeyDown(VK_CONTROL) ? 1 : 0);
    if (fw == 0 && rt == 0 && up == 0) return;
    if (View(b) != R.world || Flag(b.fpc, R.locked) || ChatOpen() || Paused()) return;
    Object* view = b.cam ? b.cam : b.tr;
    Vec3 f = un::Forward(view);
    Vec3 r = un::Right(view);
    Vec3 dir = un::Norm(un::Add(un::Add(un::Mul(f, (float)fw), un::Mul(r, (float)rt)), Vec3{0, (float)up, 0}));
    if (un::Len(dir) < 0.5f) return;
    float speed = kFlyBase * Mult(c, "flight_speed", 0.25f, 10.0f);
    Vec3 move = un::Mul(dir, speed * c.dt);
    Vec3 pos = un::Position(b.tr);
    if (!std::isfinite(pos.x) || !std::isfinite(pos.y) || !std::isfinite(pos.z)) return;
    Vec3 next = noclip ? un::Add(pos, move) : Sweep(b, pos, move);
    if (noclip && !InCaves()) {
        float floor = KillFloor();
        if (std::isfinite(floor) && next.y < floor + 5.0f) next.y = std::max(pos.y, floor + 5.0f);
    }
    if (!std::isfinite(next.x) || !std::isfinite(next.y) || !std::isfinite(next.z)) return;
    Teleport(b.tr, next);
}

void FlyTick(const Ctx& c, const Body* b) {
    if (!A.fly) return;
    bool flightOn = A.flight && c.s.On("flight");
    bool noclipOn = A.noclip && c.s.On("noclip");
    bool on = !c.stopping && (flightOn || noclipOn);
    if (!b) {
        if (Owned()) RawRestore();
        fly.player.reset();
        fly.active = false;
        fly.clipped = false;
        fly.wasNoclip = false;
        landReady = false;
        return;
    }
    if (fly.player.get() != b->fpc) {
        if (Owned()) RawRestore();
        fly.player.set(b->fpc);
        fly.active = false;
        fly.clipped = false;
        fly.wasNoclip = false;
    }
    if (!on && !Owned() && !fly.active) {
        landReady = false;
        return;
    }
    Comps(*b, c.now);
    if (GameOwns(*b)) {
        ForgetChanged();
        fly.active = false;
        landReady = false;
        return;
    }
    if (!on) {
        SafeRelease(*b);
        landReady = false;
        return;
    }
    if (!fly.active) {
        if (!CanTake(*b)) {
            landReady = false;
            return;
        }
        TakeOver(*b);
    } else {
        Reassert(*b);
    }
    if (noclipOn) fly.clipped = true;
    if (fly.wasNoclip && !noclipOn && b->tr) MoveTo(*b, SafePlace(*b, un::Position(b->tr)));
    fly.wasNoclip = noclipOn;
    landReady = true;
    Steer(c, *b, noclipOn);
}

void FlyRelease(const Body* b) {
    if (b && fly.player.get() == b->fpc && (Owned() || fly.active)) {
        Comps(*b, 0);
        if (Cutscene() || Vehicle(*b)) DropTokens();
        else SafeRelease(*b);
    } else {
        RawRestore();
    }
    fly.player.reset();
    fly.active = false;
    fly.clipped = false;
    fly.wasNoclip = false;
    landReady = false;
}

void LandNow() {
    if (!landReady.load() || GetCurrentThreadId() != mainThread.load()) return;
    if (!fly.active || !Stat(R.lpInstance)) return;
    Body b;
    if (!FindBody(un::Static(R.lpInstance), b) || !b.tr || fly.player.get() != b.fpc) return;
    Vec3 pos = un::Position(b.tr);
    Vec3 p = fly.clipped ? SafePlace(b, pos) : pos;
    Capsule k = Shell(b, pos);
    k = Shift(k, un::Sub(p, pos));
    un::RayHit hit;
    if (Cast(k, Vec3{0, -1.0f, 0}, 1000.0f, Mask(b), hit)) {
        p.y -= std::max(hit.distance - kSkin - 0.02f, 0.0f);
    } else if (!InCaves()) {
        float t = TerrainAt(p);
        if (std::isfinite(t) && t + k.feet + 0.05f < p.y) p.y = t + k.feet + 0.05f;
    }
    if (un::Dist(p, pos) < 0.05f) return;
    MoveTo(b, p);
    LOG("player put on the ground before the game saved");
    if (!landNoted.exchange(true)) Note("You were put on the ground before the game saved");
}

void SafeLand() {
    __try {
        LandNow();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void HkSave(Object* dir, Object* name, bool playerOnly, const void* mi) {
    if (landReady.load()) SafeLand();
    oSave(dir, name, playerOnly, mi);
}

void Unspeed(patch::Scaled& s, const Fld& f, const Fld& before) {
    Object* o = s.obj.get();
    if (s.have && o && un::Alive(o) && Inst(before) && Flag(o, R.slowActive) && std::fabs(un::Val<float>(o, before) - s.written) < 1e-4f)
        un::SetVal<float>(o, before, s.base);
    patch::Unscale(s, f);
}

void Speed(Object* fpc, const Fld& f, const Fld& before, float mult, patch::Scaled& s) {
    if (!Inst(f)) return;
    bool want = fpc && std::fabs(mult - 1.0f) > 1e-4f;
    if (s.have && (!want || s.obj.get() != fpc)) Unspeed(s, f, before);
    if (!want) return;
    if (Inst(before) && Flag(fpc, R.slowActive)) return;
    patch::Scale(fpc, f, mult, s);
}

void ReleaseCap() {
    Object* o = cap.obj.get();
    if (cap.have && o && un::Alive(o) && std::fabs(un::Val<float>(o, R.storeMax) - cap.written) < 1e-3f)
        un::SetVal<float>(o, R.storeMax, cap.base);
    cap.obj.reset();
    cap.have = false;
}

float BaseOf(const patch::Scaled& s, Object* fpc, const Fld& f) {
    if (s.have && s.obj.get() == fpc) return s.base;
    return Inst(f) ? un::Val<float>(fpc, f) : 0.0f;
}

void CapTick(Object* fpc, float wm, float rm) {
    if (!Inst(R.storeMax)) return;
    bool want = fpc && (wm > 1.0001f || rm > 1.0001f);
    if (cap.have && (!want || cap.obj.get() != fpc)) ReleaseCap();
    if (!want) return;
    float cur = un::Val<float>(fpc, R.storeMax);
    if (!std::isfinite(cur)) return;
    if (!cap.have) {
        cap.obj.set(fpc);
        cap.base = cur;
        cap.have = true;
    } else if (std::fabs(cur - cap.written) > 1e-3f) {
        cap.base = cur;
    }
    float top = std::max({BaseOf(walkS, fpc, R.walk) * wm, BaseOf(crouchS, fpc, R.crouch) * wm * 1.75f, BaseOf(runS, fpc, R.run) * rm});
    float need = std::isfinite(top) ? 1.1f * top : 0.0f;
    float value = std::max(cap.base, need);
    if (cur != value) un::SetVal<float>(fpc, R.storeMax, value);
    cap.written = value;
}

void ReleaseJump() {
    Object* o = jump.obj.get();
    if (jump.have && o && un::Alive(o)) {
        if (un::Val<float>(o, R.jumpMult) == jump.written && un::Val<float>(o, R.jumpAfter) == jump.written) {
            un::SetVal<float>(o, R.jumpMult, jump.origMult);
            un::SetVal<float>(o, R.jumpAfter, jump.origAfter);
        }
        if (jump.guard) ClearFall(o);
    }
    jump.obj.reset();
    jump.have = false;
    jump.guard = false;
    jump.groundedFrames = 0;
}

void JumpTick(const Ctx& c, const Body* b, float mult) {
    Object* fpc = b ? b->fpc : nullptr;
    bool want = fpc && mult > 1.0001f;
    if (jump.have && (!want || jump.obj.get() != fpc)) ReleaseJump();
    if (!want) return;
    if (!jump.have) {
        jump.obj.set(fpc);
        jump.origMult = un::Val<float>(fpc, R.jumpMult);
        jump.origAfter = un::Val<float>(fpc, R.jumpAfter);
        jump.have = true;
        jump.lastFrame = un::Val<int>(fpc, R.jumpFrame);
        jump.guard = false;
        jump.groundedFrames = 0;
    }
    if (un::Val<float>(fpc, R.jumpMult) != mult || un::Val<float>(fpc, R.jumpAfter) != mult) {
        float v = mult;
        un::Call(R.superJump, fpc, {&v});
    }
    jump.written = mult;
    int jf = un::Val<int>(fpc, R.jumpFrame);
    if (jf != jump.lastFrame) {
        jump.lastFrame = jf;
        jump.guard = true;
        jump.since = c.now;
        jump.groundedFrames = 0;
    }
    if (!jump.guard) return;
    ClearFall(fpc);
    bool landed = un::CallBool(R.grounded, fpc) && !(R.isJumping && un::CallBool(R.isJumping, fpc));
    jump.groundedFrames = landed ? jump.groundedFrames + 1 : 0;
    if (jump.groundedFrames >= 2 || Flag(b->anim, R.swimming) || c.now - jump.since > kGuardLimit) {
        jump.guard = false;
        jump.groundedFrames = 0;
    }
}

void ExtraJump(const Ctx& c, const Body& b) {
    if (!c.s.On("unlimited_jumps") || fly.active || Owned() || c.s.menuFocused) return;
    if (!un::Enabled(b.fpc)) return;
    if (Flag(b.fpc, R.moveLocked) || Flag(b.fpc, R.locked) || Flag(b.fpc, R.jumpBlock) || Flag(b.fpc, R.crouchBlocked)) return;
    if (Inst(R.canJump) && !Flag(b.fpc, R.canJump)) return;
    if (!un::CallBool(R.jumpInput, nullptr)) return;
    if (un::CallBool(R.grounded, b.fpc) || un::CallBool(R.wasGrounded, b.fpc)) return;
    if (!b.anim || Flag(b.anim, R.rootMotion) || Flag(b.anim, R.onRope) || Flag(b.anim, R.logSled) || Flag(b.anim, R.swimming) ||
        Flag(b.anim, R.sledding))
        return;
    if (View(b) != R.world || Down(b)) return;
    if (Stat(R.lpGliding) && un::StaticVal<bool>(R.lpGliding, false)) return;
    if (Stat(R.lpGolf) && un::StaticVal<bool>(R.lpGolf, false)) return;
    if (Stat(R.lpRaft) && Live(un::Static(R.lpRaft))) return;
    if (ChatOpen() || Paused()) return;
    bool apply = true;
    un::Call(R.onJump, b.fpc, {&apply});
}

bool Supported(const std::string& id) {
    if (id == "walk_speed") return A.walk;
    if (id == "run_speed") return A.run;
    if (id == "jump_height") return A.jump;
    if (id == "unlimited_jumps") return A.jumps;
    if (id == "flight") return A.flight;
    if (id == "noclip") return A.noclip;
    if (id == "flight_speed") return A.flight || A.noclip;
    return false;
}

bool Resolve(std::string& err) {
    const std::vector<std::string> vec{"UnityEngine.Vector3"};
    R.localPlayer = un::C(kSons, "TheForest.Utils", "LocalPlayer");
    R.fpc = un::C(kSons, "", "FirstPersonCharacter");
    R.anim = un::C(kSons, "", "PlayerAnimatorControl");
    R.stats = un::C(kSons, "", "PlayerStats");
    R.vitals = un::C(kSons, "", "Vitals");
    R.inventory = un::C(kSons, "TheForest.Items.Inventory", "PlayerInventory");
    R.dyingState = il2::Nested(R.stats, "DyingState");
    R.views = il2::Nested(R.inventory, "PlayerViews");
    R.cutscenes = un::C(kSons, "Sons.Cutscenes", "CutsceneManager");
    R.chat = un::C(kSons, "TheForest.UI.Multiplayer", "ChatBox");
    R.pause = un::C("Sons.Gui", "Sons.Gui", "PauseMenu");
    R.knight = un::C(kSons, "Sons.Gameplay", "PlayerKnightVAction");
    R.zip = un::C(kSons, "Sons.Gameplay.ZipLines", "PlayerZipLineAction");
    R.caves = un::C("Sons.Areas", "Sons.Areas", "CaveEntranceManager");
    R.killBox = un::C("Endnight", "Endnight.Physics", "KillBox");
    R.terrain = un::C("Endnight", "Endnight.Environment", "TerrainUtilities");
    R.saves = un::C("Sons.Save", "Sons.Save", "SaveGameManager");
    R.trigger = un::C("UnityEngine.PhysicsModule", "UnityEngine", "QueryTriggerInteraction");

    R.lpInstance = un::F(R.localPlayer, "_instance");
    R.lpFpc = un::F(R.localPlayer, "_fpc");
    R.lpRb = un::F(R.localPlayer, "_rigidbody");
    R.lpTr = un::F(R.localPlayer, "_transform");
    R.lpGo = un::F(R.localPlayer, "_playerGO");
    R.lpCam = un::F(R.localPlayer, "_mainCamTr");
    R.lpAnim = un::F(R.localPlayer, "_animControl");
    R.lpStats = un::F(R.localPlayer, "_stats");
    R.lpVitals = un::F(R.localPlayer, "_vitals");
    R.lpInv = un::F(R.localPlayer, "_inventory");
    R.lpSpecial = un::F(R.localPlayer, "_specialActions");
    R.lpGliding = un::F(R.localPlayer, "<IsGliding>k__BackingField");
    R.lpGolf = un::F(R.localPlayer, "<IsInGolfCart>k__BackingField");
    R.lpRaft = un::F(R.localPlayer, "<AttachedRaft>k__BackingField");
    R.teleport = R.localPlayer ? il2::FindMethod(R.localPlayer, "TeleportTo", vec) : nullptr;
    if (R.localPlayer && !R.teleport) un::Missing("LocalPlayer.TeleportTo");
    R.caveCheck = un::M(R.localPlayer, "CheckCaveForcedEnter", std::vector<std::string>{"UnityEngine.Vector3", "System.Boolean"});

    R.walk = un::F(R.fpc, "_walkSpeed");
    R.run = un::F(R.fpc, "_runSpeed");
    R.crouch = un::F(R.fpc, "_crouchSpeed");
    R.storeMax = un::F(R.fpc, "storeMaxVelocity");
    R.slowActive = un::F(R.fpc, "_movementSlowIsActive");
    R.walkBefore = un::F(R.fpc, "_walkSpeedBeforeSlow");
    R.runBefore = un::F(R.fpc, "_runSpeedBeforeSlow");
    R.jumpMult = un::F(R.fpc, "_jumpMultiplier");
    R.jumpAfter = un::F(R.fpc, "_setJumpMultiplierAfterJump");
    R.jumpFrame = un::F(R.fpc, "_jumpPressFixedFrame");
    R.canJump = un::F(R.fpc, "_canJump");
    R.crouchBlocked = un::F(R.fpc, "_crouchHeightBlocked");
    R.jumpBlock = un::F(R.fpc, "jumpInputBlock");
    R.locked = un::F(R.fpc, "Locked");
    R.moveLocked = un::F(R.fpc, "_movementLocked");
    R.prevGrounded = un::F(R.fpc, "_prevGrounded");
    R.layers = un::F(R.fpc, "CollisionLayers");
    R.fpcRb = un::F(R.fpc, "_rigidbody");

    R.superJump = un::M(R.fpc, "SetSuperJump", 1);
    R.clearFall = un::M(R.fpc, "ClearFallDamageFlag", 0);
    R.grounded = un::M(R.fpc, "get_Grounded", 0);
    R.setGrounded = un::M(R.fpc, "set_Grounded", 1);
    R.isJumping = un::M(R.fpc, "get_IsJumping", 0);
    R.setJumping = un::M(R.fpc, "set_IsJumping", 1);
    R.wasGrounded = un::M(R.fpc, "WasGrounded", 0);
    R.jumpInput = un::M(R.fpc, "GetJumpInput", 0);
    R.onJump = un::M(R.fpc, "OnJumpInput", 1);
    R.fpcEnable = un::M(R.fpc, "SetEnabled", 1);
    R.bounds = un::M(R.fpc, "GetPrimaryColliderBounds", 0);

    R.rootMotion = un::F(R.anim, "useRootMotion");
    R.onRope = un::F(R.anim, "onRope");
    R.logSled = un::F(R.anim, "_isConnectedToLogSled");
    R.swimming = un::F(R.anim, "_isSwimming");
    R.sledding = un::F(R.anim, "<IsSledding>k__BackingField");

    R.hasDied = un::F(R.vitals, "_hasDied");
    R.dying = un::F(R.stats, "_dyingState");
    R.downed = un::F(R.stats, "_playerIsDowned");
    R.view = un::F(R.inventory, "_currentView");

    R.cutscene = un::M(R.cutscenes, "IsCutscenePlaying", 0);
    R.chatOpen = un::M(R.chat, "get_IsChatOpen", 0);
    R.pauseActive = un::M(R.pause, "get_IsActive", 0);
    R.knightActive = un::M(R.knight, "IsActive", 0);
    R.onZip = un::M(R.zip, "get_IsOnZipLine", 0);
    R.inCaves = un::F(R.caves, "_isInCaves");
    R.cavesGet = un::M(R.caves, "get_IsInCaves", 0);
    R.killInstance = un::F(R.killBox, "_instance");
    R.killDepth = un::F(R.killBox, "_killDepth");
    R.terrainHeight = un::M(R.terrain, "GetTerrainHeight", vec);
    R.capsuleCast = un::M(un::E.physics, "CapsuleCast", 8);
    R.checkCapsule = un::M(un::E.physics, "CheckCapsule", 5);
    R.layer = un::M(un::E.gameObject, "get_layer", 0);
    R.save = un::M(R.saves, "Save", std::vector<std::string>{"System.String", "System.String", "System.Boolean"});

    R.alive = R.dyingState ? (int)il2::EnumValue(R.dyingState, "Alive", 0) : 0;
    R.world = R.views ? (int)il2::EnumValue(R.views, "World", 2) : 2;
    R.ignore = R.trigger ? (int)il2::EnumValue(R.trigger, "Ignore", 1) : 1;
    R.busyViews.clear();
    if (R.views) {
        const char* busy[] = {"Death", "Sleep", "WakingUp", "Loading", "PlaneCrash", "EndDeactivate"};
        const long long fallback[] = {7, 9, 1, 0, -1, 12};
        for (int i = 0; i < 6; ++i) R.busyViews.push_back((int)il2::EnumValue(R.views, busy[i], fallback[i]));
    }

    A.player = Inst(R.lpFpc);
    A.walk = A.player && Inst(R.walk);
    A.run = A.player && Inst(R.run);
    A.jump = A.player && R.superJump && Inst(R.jumpMult) && Inst(R.jumpAfter) && Inst(R.jumpFrame) && R.clearFall && R.grounded;
    A.jumps = A.player && R.onJump && R.jumpInput && R.grounded && R.wasGrounded && Inst(R.jumpBlock) && Inst(R.locked) &&
              Inst(R.lpAnim) && Inst(R.lpInv) && Inst(R.view);
    A.fly = A.player && Inst(R.lpTr) && (Inst(R.lpRb) || Inst(R.fpcRb)) && un::E.rbGetKinematic && un::E.rbSetKinematic &&
            (R.fpcEnable || un::E.setEnabled) && R.clearFall && R.teleport && Inst(R.locked) && Inst(R.lpInv) && Inst(R.view) &&
            Inst(R.lpStats) && Inst(R.lpVitals) && Inst(R.hasDied) && Inst(R.downed) && R.cutscene;
    A.flight = A.fly && R.capsuleCast;
    A.noclip = A.fly;

    bool any = false;
    for (const char* id : kIds) {
        if (Supported(id)) any = true;
        else LOG("Movement: %s is not available", id);
    }
    if (!any) err = "the player's movement was not found";
    return any;
}

void Tick(const Ctx& c) {
    mainThread = GetCurrentThreadId();
    Body b;
    bool have = !c.stopping && LiveCtx(c) && FindBody(c.player, b);
    FlyTick(c, have ? &b : nullptr);
    Object* fpc = have ? b.fpc : nullptr;
    float wm = A.walk ? Mult(c, "walk_speed", 1.0f, 10.0f) : 1.0f;
    float rm = A.run ? Mult(c, "run_speed", 1.0f, 10.0f) : 1.0f;
    if (A.walk) {
        Speed(fpc, R.walk, R.walkBefore, wm, walkS);
        Speed(fpc, R.crouch, Fld{}, wm, crouchS);
    }
    if (A.run) Speed(fpc, R.run, R.runBefore, rm, runS);
    if (A.walk || A.run) CapTick(fpc, wm, rm);
    if (A.jump) JumpTick(c, have ? &b : nullptr, Mult(c, "jump_height", 1.0f, 10.0f));
    if (A.jumps && have) ExtraJump(c, b);
}

bool Action(const std::string&, const Ctx&) { return false; }

void Release(const Ctx& c) {
    Body b;
    bool have = c.inWorld && c.player && FindBody(c.player, b);
    FlyRelease(have ? &b : nullptr);
    Unspeed(walkS, R.walk, R.walkBefore);
    Unspeed(crouchS, R.crouch, Fld{});
    Unspeed(runS, R.run, R.runBefore);
    ReleaseCap();
    ReleaseJump();
    compOwner.reset();
    knightComp.reset();
    zipComp.reset();
    compRetry = 0;
    landReady = false;
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

bool Start(std::string& err) {
    if (saveHooked || !(A.flight || A.noclip) || !R.save) return true;
    void* target = il2::MethodPointer(R.save);
    if (!target) return true;
    if (Detoured(target)) {
        LOG("SaveGameManager.Save is already changed by another mod, left alone");
        err = "landing before a save is not available";
        return false;
    }
    MH_STATUS st = MH_CreateHook(target, (void*)&HkSave, (void**)&oSave);
    if (st == MH_OK) {
        st = MH_EnableHook(target);
        if (st != MH_OK) MH_RemoveHook(target);
    }
    if (st != MH_OK) {
        LOG("could not attach to SaveGameManager.Save: %s", MH_StatusToString(st));
        err = "landing before a save is not available";
        return false;
    }
    saveHooked = true;
    return true;
}

}

const Group& MovementGroup() {
    static Group g{"Movement", &Resolve, &Supported, &Tick, &Action, &Release, nullptr, &Start};
    return g;
}

}
