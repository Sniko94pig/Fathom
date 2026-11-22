#include "ingame/groups.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ukaim {

using namespace uk;
using namespace un;

namespace {

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Box {
    Vec3 center, extents;
};

struct Game {
    Class *nm = nullptr, *cc = nullptr, *im = nullptr, *pad = nullptr, *tracker = nullptr, *eid = nullptr, *part = nullptr;
    Class *collider = nullptr, *player = nullptr, *targeter = nullptr;
    Fld nmCc, nmRb, ccActivated, ccRotX, ccRotY, ccTransZ, ccTiltZ, ccOffset, ccFreeze, ccPlatformer;
    Fld imSource, imDevice, piFire1, piFire2, iasPressed;
    Fld eidDead, eidWeak, eidCenter, partEid, playerType, autoOn, autoAimed;
    Method *ccApply = nullptr, *imCombo = nullptr, *enemies = nullptr, *bounds = nullptr, *compareTag = nullptr;
    Method *linecast = nullptr, *maskGet = nullptr, *trParent = nullptr, *trRotation = nullptr;
    int envLmd = -1, ignore = -1, fps = -1;
};

struct Foe {
    Hold eid, body, weak, weakCol;
    std::vector<Hold> heads;
    bool scanned = false;
};

struct Cand {
    Vec3 p;
    float cosv = -1.0f;
};

const std::string kSoft = "softaim";
const std::string kStrength = "aim_strength";
const std::string kArea = "aim_area";
const double kRefreshEvery = 0.5;
const size_t kMaxFoes = 256;
const size_t kMaxHeads = 4;
const size_t kMaxParts = 128;
const int kScanPerTick = 6;
const int kMaxRays = 8;
const float kRange = 1000.0f;
const int kWallsFallback = 0x010001C0;
const float kDeg = 57.29578f;

Game G;
bool ok = false, canHeads = false, canAuto = false;
std::vector<Foe> foes;
std::vector<Cand> cands;
std::vector<Vec3> points;
double nextRefresh = 0;
int refreshPlayer = 0;
int brokenPlayer = 0;
Hold headTag;
int walls = 0;
bool wallsRead = false;
bool forced = false, toldForced = false;

bool Bit(Object* o, const Fld& f) { return Val<uint8_t>(o, f) != 0; }

bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float Flat(Vec3 v) { return std::sqrt(v.x * v.x + v.z * v.z); }

float Delta(float from, float to) {
    float d = std::fmod(to - from, 360.0f);
    if (d > 180.0f) d -= 360.0f;
    else if (d < -180.0f) d += 360.0f;
    return d;
}

Vec3 Rotate(const Quat& q, Vec3 p) {
    float x2 = q.x * 2.0f, y2 = q.y * 2.0f, z2 = q.z * 2.0f;
    float xx = q.x * x2, yy = q.y * y2, zz = q.z * z2;
    float xy = q.x * y2, xz = q.x * z2, yz = q.y * z2;
    float wx = q.w * x2, wy = q.w * y2, wz = q.w * z2;
    return {(1.0f - (yy + zz)) * p.x + (xy - wz) * p.y + (xz + wy) * p.z, (xy + wz) * p.x + (1.0f - (xx + zz)) * p.y + (yz - wx) * p.z,
            (xz - wy) * p.x + (yz + wx) * p.y + (1.0f - (xx + yy)) * p.z};
}

Fld Typed(Class* owner, const char* name, Class* type) {
    Fld f = F(owner, name);
    if (!f || !type) return {};
    if (f.isStatic || !mono::IsSubclass(mono::FieldClass(f.f), type)) {
        Missing(std::string(mono::ClassName(owner)) + "." + name + " has an unexpected type");
        return {};
    }
    return f;
}

void Free(Foe& f) {
    f.eid.reset();
    f.body.reset();
    f.weak.reset();
    f.weakCol.reset();
    for (Hold& h : f.heads) h.reset();
    f.heads.clear();
}

void DropFoes() {
    for (Foe& f : foes) Free(f);
    foes.clear();
    nextRefresh = 0;
    refreshPlayer = 0;
}

void Unforce() {
    if (!forced) return;
    forced = false;
    if (StaticVal<uint8_t>(G.autoOn, 1) == 0) SetStaticVal<uint8_t>(G.autoOn, 1);
}

void Force() {
    if (!canAuto || StaticVal<uint8_t>(G.autoOn, 0) == 0) return;
    SetStaticVal<uint8_t>(G.autoOn, 0);
    forced = true;
    Object* t = Single(G.targeter);
    if (t && Bit(t, G.autoAimed)) SetVal<uint8_t>(t, G.autoAimed, 0);
    if (toldForced) return;
    toldForced = true;
    LOG("aim: the game's own auto-aim waits while Softaim holds a target");
}

int Walls() {
    if (wallsRead) return walls;
    wallsRead = true;
    walls = kWallsFallback;
    if (G.maskGet && G.envLmd >= 0) {
        int lmd = G.envLmd;
        int m = CallInt(G.maskGet, nullptr, {&lmd}, 0);
        if (m != 0) walls = m;
    }
    LOG("aim: walls block Softaim on the layer mask 0x%08X", (unsigned)walls);
    return walls;
}

bool Held(Object* src, const Fld& key) {
    Object* s = Ref(src, key);
    return s && Bit(s, G.iasPressed);
}

bool IsFps() {
    if (!G.player || !G.playerType || G.fps < 0) return true;
    Object* t = Single(G.player);
    return !t || Val<int>(t, G.playerType, G.fps) == G.fps;
}

bool PadFrozen(Object* cc, Object* im) {
    if (!G.ccFreeze || Val<int>(cc, G.ccFreeze) <= 0) return false;
    if (!G.pad || !G.imDevice) return true;
    Object* dev = Ref(im, G.imDevice);
    return dev && mono::IsSubclass(mono::ClassOf(dev), G.pad);
}

Object* Ready(const Ctx& c) {
    if (c.dead || !c.activated || c.levelOver || c.paused || c.inIntro || c.menuFocused) return nullptr;
    Object* gsm = Static(R.gsmInstance);
    if (!Alive(gsm) || Bit(gsm, R.gsmCameraLocked) || Bit(gsm, R.gsmInputLocked)) return nullptr;
    Object* im = Single(G.im);
    Object* src = im ? Ref(im, G.imSource) : nullptr;
    if (!src || (!Held(src, G.piFire1) && !Held(src, G.piFire2))) return nullptr;
    Object* cc = Ref(c.player, G.nmCc);
    if (!Alive(cc)) cc = Single(G.cc);
    if (!Alive(cc) || !Bit(cc, G.ccActivated) || Bit(cc, G.ccPlatformer) || !Enabled(cc) || PadFrozen(cc, im)) return nullptr;
    if (G.imCombo && CallBool(G.imCombo, im, {}, false)) return nullptr;
    return IsFps() ? cc : nullptr;
}

void Refresh(const Ctx& c) {
    if (c.now < nextRefresh && refreshPlayer == c.playerId) return;
    nextRefresh = c.now + kRefreshEvery;
    refreshPlayer = c.playerId;
    Object* tracker = Single(G.tracker);
    Object* list = tracker ? Call(G.enemies, tracker) : nullptr;
    std::unordered_map<Object*, size_t> index;
    for (size_t i = 0; i < foes.size(); ++i)
        if (Object* e = foes[i].eid.get()) index[e] = i;
    std::unordered_set<Object*> seen;
    std::vector<Foe> keep;
    for (Object* e : ListItems(list)) {
        if (keep.size() >= kMaxFoes) break;
        if (!Alive(e) || !mono::IsSubclass(mono::ClassOf(e), G.eid) || Bit(e, G.eidDead) || !seen.insert(e).second) continue;
        auto it = index.find(e);
        if (it != index.end()) {
            keep.push_back(std::move(foes[it->second]));
            continue;
        }
        keep.emplace_back();
        keep.back().eid.set(e);
    }
    for (Foe& f : foes) Free(f);
    foes = std::move(keep);
}

Object* HeadTag() {
    if (!headTag.held()) headTag.set(mono::NewString("Head"));
    return headTag.get();
}

void Scan(Foe& f) {
    f.scanned = true;
    Object* eid = f.eid.get();
    if (!Alive(eid)) return;
    f.body.set(Comp(eid, G.collider));
    Object* tag = canHeads ? HeadTag() : nullptr;
    if (!tag) return;
    std::vector<Object*> parts = CompsInChildren(eid, G.part, true);
    size_t n = std::min(parts.size(), kMaxParts);
    for (size_t i = 0; i < n && f.heads.size() < kMaxHeads; ++i) {
        Object* owner = Ref(parts[i], G.partEid);
        if (owner && owner != eid) continue;
        if (!CallBool(G.compareTag, parts[i], {tag}, false)) continue;
        Object* col = Comp(parts[i], G.collider);
        if (!col) continue;
        f.heads.emplace_back();
        f.heads.back().set(col);
    }
}

void ScanSome() {
    int budget = kScanPerTick;
    for (Foe& f : foes) {
        if (budget <= 0) break;
        if (f.scanned) continue;
        Scan(f);
        --budget;
    }
}

bool Center(Object* col, Vec3& p) {
    if (!Alive(col)) return false;
    Box b = CallVal<Box>(G.bounds, col, {}, Box{});
    bool empty = b.extents.x <= 0.0f && b.extents.y <= 0.0f && b.extents.z <= 0.0f;
    if (empty || !Finite(b.center) || !Finite(b.extents)) return false;
    p = b.center;
    return true;
}

void Points(Foe& f, Object* eid) {
    Vec3 p;
    for (Hold& h : f.heads)
        if (Center(h.get(), p)) points.push_back(p);
    if (!points.empty()) return;
    Object* wp = Ref(eid, G.eidWeak);
    if (Alive(wp) && ActiveInHierarchy(wp)) {
        if (wp != f.weak.get()) {
            f.weak.set(wp);
            f.weakCol.set(Comp(wp, G.collider));
        }
        points.push_back(Center(f.weakCol.get(), p) ? p : Position(wp));
        return;
    }
    if (Center(f.body.get(), p)) {
        points.push_back(p);
        return;
    }
    if (!ActiveInHierarchy(eid)) return;
    Object* oc = Ref(eid, G.eidCenter);
    points.push_back(Position(Alive(oc) ? oc : eid));
}

bool Pick(Vec3 eye, Vec3 fwd, float cosArea, Vec3& best) {
    cands.clear();
    for (Foe& f : foes) {
        Object* eid = f.eid.get();
        if (!f.scanned || !Alive(eid) || Bit(eid, G.eidDead)) continue;
        points.clear();
        Points(f, eid);
        for (const Vec3& p : points) {
            Vec3 d = Sub(p, eye);
            float len = Len(d);
            if (!(len > 0.05f) || len > kRange || !Finite(p)) continue;
            float cv = Dot(fwd, d) / len;
            if (cv >= cosArea) cands.push_back({p, cv});
        }
    }
    if (cands.empty()) return false;
    std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.cosv > b.cosv; });
    int mask = Walls();
    int mode = G.ignore >= 0 ? G.ignore : 1;
    int rays = 0;
    for (const Cand& k : cands) {
        if (rays++ >= kMaxRays) break;
        Vec3 from = eye, to = k.p;
        if (CallBool(G.linecast, nullptr, {&from, &to, &mask, &mode}, true)) continue;
        best = k.p;
        return true;
    }
    return false;
}

Vec3 Lens(Object* cc) {
    Vec3 v{0.0f, 0.0f, 1.0f};
    Quat o = Val<Quat>(cc, G.ccOffset, Quat{});
    float n = std::sqrt(o.x * o.x + o.y * o.y + o.z * o.z + o.w * o.w);
    if (std::isfinite(n) && n > 1e-4f) v = Rotate({o.x / n, o.y / n, o.z / n, o.w / n}, v);
    float roll = (Val<float>(cc, G.ccTransZ) + Val<float>(cc, G.ccTiltZ)) / kDeg;
    if (std::isfinite(roll) && roll != 0.0f) {
        float s = std::sin(roll), k = std::cos(roll);
        v = {k * v.x - s * v.y, s * v.x + k * v.y, v.z};
    }
    return Norm(v);
}

Vec3 PitchBy(Vec3 g, float deg) {
    float a = deg / kDeg;
    float s = std::sin(a), k = std::cos(a);
    return {g.x, g.y * k + g.z * s, g.z * k - g.y * s};
}

bool SolvePitch(Vec3 g, float ty, float cur, float& out) {
    float r = std::sqrt(g.y * g.y + g.z * g.z);
    if (!(r > 1e-4f) || !std::isfinite(ty)) return false;
    float base = std::atan2(g.z, g.y) * kDeg;
    float half = std::acos(std::clamp(ty / r, -1.0f, 1.0f)) * kDeg;
    bool found = false;
    for (float t : {base + half, base - half}) {
        float a = Delta(0.0f, t);
        if (!(std::fabs(a) <= 90.001f)) continue;
        a = std::clamp(a, -90.0f, 90.0f);
        if (!found || std::fabs(Delta(cur, a)) < std::fabs(Delta(cur, out))) out = a;
        found = true;
    }
    return found;
}

float Pull(const Ctx& c) {
    float s = c.value(kStrength, 10.0f);
    s = std::isfinite(s) ? std::clamp(s, 1.0f, 10.0f) : 10.0f;
    if (s > 9.999f) return 1.0f;
    float rate = 1.5f * std::pow(1.5f, s - 1.0f);
    return 1.0f - std::exp(-rate * c.udt);
}

float Area(const Ctx& c) {
    float a = c.value(kArea, 30.0f);
    a = std::isfinite(a) ? std::clamp(a, 5.0f, 90.0f) : 30.0f;
    return std::cos(a / kDeg);
}

Vec3 Lead(const Ctx& c, Vec3 cam) {
    Object* rb = G.nmRb ? Ref(c.player, G.nmRb) : nullptr;
    if (!Alive(rb) || !R.rbGetVelocity) return cam;
    Vec3 v = Velocity(rb);
    if (!Finite(v) || Len(v) > 500.0f) return cam;
    return Add(cam, Mul(v, c.dt));
}

bool Turn(const Ctx& c, Object* cc, Object* ct, Vec3 eye, Vec3 target, float f) {
    Object* parent = Call(G.trParent, ct);
    if (!Alive(parent)) return false;
    Quat q = CallVal<Quat>(G.trRotation, parent, {}, Quat{});
    Vec3 t = Norm(Sub(target, eye));
    if (!Finite(t) || Len(t) < 0.5f) return false;
    Vec3 local{Dot(t, Rotate(q, {1.0f, 0.0f, 0.0f})), Dot(t, Rotate(q, {0.0f, 1.0f, 0.0f})), Dot(t, Rotate(q, {0.0f, 0.0f, 1.0f}))};
    Vec3 g = Lens(cc);
    float rx = Val<float>(cc, G.ccRotX), ry = Val<float>(cc, G.ccRotY);
    if (!std::isfinite(rx) || !std::isfinite(ry) || !Finite(local) || !Finite(g)) return false;
    float cur = Delta(0.0f, rx);
    float pitch = cur;
    if (!SolvePitch(g, local.y, cur, pitch)) return false;
    Vec3 p = PitchBy(g, pitch);
    float yaw = 0.0f;
    if (Flat(local) > 1e-4f && Flat(p) > 1e-4f) yaw = Delta(std::atan2(p.x, p.z) * kDeg, std::atan2(local.x, local.z) * kDeg);
    float dp = Delta(cur, pitch);
    if (std::fabs(dp) < 1e-4f && std::fabs(yaw) < 1e-4f) return true;
    SetVal<float>(cc, G.ccRotX, std::clamp(cur + dp * f, -90.0f, 90.0f));
    SetVal<float>(cc, G.ccRotY, ry + yaw * f);
    bool no = false, good = false;
    Call(G.ccApply, cc, {&no}, &good);
    if (good) return true;
    brokenPlayer = c.playerId;
    LOG("aim: the camera did not turn, Softaim rests until the next life");
    return false;
}

}

bool Resolve() {
    G = Game{};
    ok = canHeads = canAuto = false;
    G.nm = Cls("NewMovement");
    G.cc = Cls("CameraController");
    G.nmCc = Typed(G.nm, "cc", G.cc);
    G.nmRb = Typed(G.nm, "rb", R.rigidbody);
    G.ccActivated = F(G.cc, "activated");
    G.ccRotX = F(G.cc, "rotationX");
    G.ccRotY = F(G.cc, "rotationY");
    G.ccTransZ = F(G.cc, "transitionRotationZ");
    G.ccTiltZ = F(G.cc, "tiltRotationZ");
    G.ccOffset = F(G.cc, "rotationOffset");
    G.ccFreeze = F(G.cc, "gamepadFreezeCount");
    G.ccPlatformer = F(G.cc, "platformerCamera");
    G.ccApply = MT(G.cc, "ApplyRotations", {"System.Boolean"});
    G.trParent = M(R.transform, "get_parent", 0);
    G.trRotation = M(R.transform, "get_rotation", 0);

    G.im = Cls("InputManager");
    Class* input = Cls("PlayerInput");
    Class* state = Cls("InputActionState");
    G.imSource = Typed(G.im, "<InputSource>k__BackingField", input);
    G.imDevice = F(G.im, "<LastButtonDevice>k__BackingField");
    G.imCombo = M(G.im, "PerformingCheatMenuCombo", 0);
    G.piFire1 = Typed(input, "Fire1", state);
    G.piFire2 = Typed(input, "Fire2", state);
    G.iasPressed = F(state, "<IsPressed>k__BackingField");
    G.pad = Cls("UnityEngine.InputSystem.Gamepad");

    G.tracker = Cls("EnemyTracker");
    G.enemies = M(G.tracker, "GetCurrentEnemies", 0);
    G.eid = Cls("EnemyIdentifier");
    G.eidDead = F(G.eid, "dead");
    G.eidWeak = Typed(G.eid, "weakPoint", R.gameObject);
    G.eidCenter = Typed(G.eid, "overrideCenter", R.transform);
    G.part = Cls("EnemyIdentifierIdentifier");
    G.partEid = Typed(G.part, "eid", G.eid);
    G.collider = Cls("UnityEngine.Collider");
    G.bounds = M(G.collider, "get_bounds", 0);
    G.compareTag = MT(R.component, "CompareTag", {"System.String"});

    G.linecast = MT(Cls("UnityEngine.Physics"), "Linecast",
                    {"UnityEngine.Vector3", "UnityEngine.Vector3", "System.Int32", "UnityEngine.QueryTriggerInteraction"});
    G.ignore = Enum(Cls("UnityEngine.QueryTriggerInteraction"), "Ignore");
    G.maskGet = M(Cls("LayerMaskDefaults"), "Get", 1);
    G.envLmd = Enum(Cls("LMD"), "Environment");

    G.player = Cls("PlayerTracker");
    G.playerType = F(G.player, "playerType");
    G.fps = Enum(Cls("PlayerType"), "FPS");

    G.targeter = Cls("CameraFrustumTargeter");
    G.autoOn = F(G.targeter, "isEnabled");
    G.autoAimed = F(G.targeter, "<IsAutoAimed>k__BackingField");

    bool camOk = G.cc && G.ccActivated && G.ccRotX && G.ccRotY && G.ccApply && G.trParent && G.trRotation && R.trForward && R.trGetPosition;
    bool keysOk = G.im && G.imSource && G.piFire1 && G.piFire2 && G.iasPressed;
    bool foesOk = G.tracker && G.enemies && G.eid && G.eidDead && G.collider && G.bounds && R.compGetComponent && R.goGetComponent;
    bool stateOk = R.gsmInstance && R.gsmCameraLocked && R.gsmInputLocked;
    ok = camOk && keysOk && foesOk && stateOk && G.linecast;
    canHeads = ok && G.part && G.compareTag && R.compGetComponentsInChildren;
    canAuto = ok && G.targeter && G.autoOn && G.autoOn.isStatic && G.autoAimed;
    if (ok && !canHeads) LOG("aim: enemy heads cannot be found, Softaim aims at weak spots and bodies");
    if (ok && !canAuto) LOG("aim: the game's own auto-aim cannot be paused, if it is on in the game's settings it may still bend shots");
    if (ok && (!G.maskGet || G.envLmd < 0)) LOG("aim: the game's wall layers were not found, Softaim uses its own copy of them");
    LOG("aim: softaim %s", ok ? "ready" : "off, a part of the game it needs was not found");
    return ok;
}

bool Supported(const std::string& id) { return ok && (id == kSoft || id == kStrength || id == kArea); }

void Tick(const Ctx& c) {
    if (!ok || !c.on(kSoft)) {
        Unforce();
        if (!foes.empty()) DropFoes();
        return;
    }
    if (!c.inLevel || !Alive(c.player) || c.dead || c.paused) {
        Unforce();
        return;
    }
    Refresh(c);
    ScanSome();
    Object* cc = c.playerId != brokenPlayer ? Ready(c) : nullptr;
    Object* ct = cc ? TransformOf(cc) : nullptr;
    if (!ct) {
        Unforce();
        return;
    }
    Vec3 cam = CallVal<Vec3>(R.trGetPosition, ct);
    Vec3 fwd = Norm(CallVal<Vec3>(R.trForward, ct));
    Vec3 target;
    if (!Finite(cam) || !Finite(fwd) || Len(fwd) < 0.5f || !Pick(cam, fwd, Area(c), target) || !Turn(c, cc, ct, Lead(c, cam), target, Pull(c))) {
        Unforce();
        return;
    }
    Force();
}

void Release(const Ctx& c) {
    Unforce();
    DropFoes();
    brokenPlayer = 0;
    if (c.stopping) headTag.reset();
}

void Action(const std::string&, const Ctx&) {}

bool Busy() { return forced; }

}
