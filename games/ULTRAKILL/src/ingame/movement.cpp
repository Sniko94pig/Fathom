#include "ingame/groups.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>

namespace ukmove {

namespace {

using namespace uk;
using namespace un;

const std::string kDash = "unlimited_dash";
const std::string kJumps = "unlimited_jumps";
const std::string kWalls = "unlimited_walljumps";
const std::string kSpeed = "player_speed";
const std::string kFly = "fly";
const std::string kFlySpeed = "fly_speed";
const std::string kNoclip = "noclip";

enum Mode { M_NONE, M_FLY, M_CLIP };

struct Names {
    Class *nm, *im, *kib, *vcb, *vec2, *pad;
    Fld walk, stamina, wallJumps, cooldown, fakeFall, padFreeze, hp, hurtInv, levelOver, exploded, rb, gc, cc;
    Fld cam, camOn, camHor, camVer, gsmInstance, gsmLocked, source, device;
    Fld move, jumpKey, slideKey, dashKey, action, pressed, performed;
    Method *jump, *stop, *onGround, *heavyFall, *approve, *gravity, *readValue, *wasPerformed;
    Method *getKin, *setKin, *getPos, *setPos, *setVel, *getLayer, *setLayer, *sync;
    bool dash, jumps, walls, speed, fly, clip;
};

Names K{};

struct State {
    Mode mode = M_NONE;
    bool kin = false;
    Hold nm, rb, cc;
    Disabled kib, vcb;
    Scaled walk;
};

State S;

struct Keys {
    float x = 0, y = 0;
    bool up = false, down = false, fast = false;
};

bool Bit(Object* o, const Fld& f) { return Val<uint8_t>(o, f) != 0; }

bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float Slider(const Ctx& c, const std::string& id, float lo, float hi) {
    float v = c.value(id, 1.0f);
    return std::isfinite(v) ? std::clamp(v, lo, hi) : 1.0f;
}

bool Kinematic(Object* rb) { return CallBool(K.getKin, rb); }

void SetKinematic(Object* rb, bool on) { Call(K.setKin, rb, {&on}); }

void Gravity(Object* rb, bool on) { Call(K.gravity, nullptr, {rb, &on}); }

bool BodyPos(Object* rb, Vec3& out) {
    bool ok = false;
    Object* r = Call(K.getPos, rb, {}, &ok);
    if (!ok || !r) return false;
    out = mono::Result<Vec3>(r);
    return Finite(out);
}

bool Locked() {
    if (!K.gsmInstance || !K.gsmLocked) return false;
    Object* g = Static(K.gsmInstance);
    return !Alive(g) || Bit(g, K.gsmLocked);
}

Object* Source(Object** manager = nullptr) {
    Object* im = Single(K.im);
    if (manager) *manager = im;
    return im ? Ref(im, K.source) : nullptr;
}

bool Held(Object* src, const Fld& key) {
    Object* s = Ref(src, key);
    return s && Bit(s, K.pressed);
}

Keys ReadKeys() {
    Keys k;
    Object* src = Source();
    if (!src) return k;
    Object* mv = Ref(src, K.move);
    Object* act = mv ? Ref(mv, K.action) : nullptr;
    Object* val = act ? Call(K.readValue, act) : nullptr;
    if (val && mono::ClassOf(val) == K.vec2) {
        Vec2 v = mono::Result<Vec2>(val);
        float l = std::sqrt(v.x * v.x + v.y * v.y);
        if (std::isfinite(l)) {
            if (l > 1.0f) {
                v.x /= l;
                v.y /= l;
            }
            k.x = v.x;
            k.y = v.y;
        }
    }
    k.up = Held(src, K.jumpKey);
    k.down = Held(src, K.slideKey);
    k.fast = Held(src, K.dashKey);
    return k;
}

bool PadFrozen(Object* nm, Object* im) {
    if (Val<int>(nm, K.padFreeze) <= 0) return false;
    if (!K.pad || !K.device) return true;
    Object* dev = Ref(im, K.device);
    return dev && mono::IsSubclass(mono::ClassOf(dev), K.pad);
}

bool JumpedNow(Object* src, int frame) {
    Object* js = Ref(src, K.jumpKey);
    if (!js) return false;
    if (K.performed) return Val<int>(js, K.performed, -1) == frame;
    return CallBool(K.wasPerformed, js);
}

void AirJump(Object* nm, const Ctx& c) {
    Object* im = nullptr;
    Object* src = Source(&im);
    if (!src || !JumpedNow(src, c.frame)) return;
    if (Bit(nm, K.cooldown) || Val<int>(nm, K.fakeFall) > 0 || PadFrozen(nm, im) || Locked() || !Enabled(nm)) return;
    Object* gc = Ref(nm, K.gc);
    if (!Alive(gc) || CallBool(K.onGround, gc, {}, true)) return;
    Call(K.jump, nm);
}

void Unwalk() {
    Object* o = S.walk.obj.get();
    if (o && Alive(o)) Unscale(S.walk, K.walk);
    else S.walk.obj.reset();
}

void Speed(Object* nm, bool live, const Ctx& c) {
    float k = K.speed ? Slider(c, kSpeed, 1.0f, 5.0f) : 1.0f;
    if (live && std::fabs(k - 1.0f) > 0.001f) Scale(nm, K.walk, k, S.walk);
    else if (S.walk.obj.held()) Unwalk();
}

void Hurt(Object* nm, float dt) {
    if (!K.hurtInv) return;
    float inv = Val<float>(nm, K.hurtInv);
    if (inv > 0.0f && Val<int>(nm, K.hp, 1) > 0) {
        inv = std::max(0.0f, inv - dt);
        SetVal<float>(nm, K.hurtInv, inv);
    }
    if (inv > 0.0f || Bit(nm, K.levelOver)) return;
    Object* go = K.getLayer && K.setLayer ? GameObjectOf(nm) : nullptr;
    if (go && CallInt(K.getLayer, go, {}, -1) == 15) {
        int layer = 2;
        Call(K.setLayer, go, {&layer});
    }
    if (Bit(nm, K.exploded)) SetVal<uint8_t>(nm, K.exploded, 0);
}

void Unclip(Object* rb) {
    Object* kib = S.kib.obj.get();
    if (kib && Alive(kib)) Call(K.approve, kib);
    ForceOff(nullptr, false, S.kib);
    ForceOff(nullptr, false, S.vcb);
    if (S.kin && rb) SetKinematic(rb, false);
    S.kin = false;
}

void Roll(Object* cc, float x, float y) {
    if (!cc) return;
    SetVal<float>(cc, K.camHor, x);
    SetVal<float>(cc, K.camVer, y);
}

void Land(bool paused) {
    Object* nm = S.nm.get();
    Object* rb = S.rb.get();
    Object* cc = S.cc.get();
    if (!Alive(nm)) nm = nullptr;
    if (!Alive(rb)) rb = nullptr;
    if (!Alive(cc)) cc = nullptr;
    Unclip(rb);
    if (rb) Gravity(rb, true);
    if (nm) {
        bool frozen = rb && Kinematic(rb);
        if (!paused && !frozen && !Enabled(nm)) SetEnabled(nm, true);
        if (rb && !frozen && ActiveInHierarchy(nm)) Call(K.stop, nm);
    }
    Roll(cc, 0.0f, 0.0f);
    S.nm.reset();
    S.rb.reset();
    S.cc.reset();
    S.mode = M_NONE;
}

bool Applied() {
    return S.mode != M_NONE || S.kin || S.nm.held() || S.rb.held() || S.cc.held() || S.kib.obj.held() || S.vcb.obj.held();
}

Object* Part(Disabled& d, Object* nm, Class* cls) {
    Object* o = d.obj.get();
    return o && Alive(o) ? o : Comp(nm, cls);
}

void Fly(Object* nm, Mode want, const Ctx& c) {
    Object* rb = want != M_NONE ? Ref(nm, K.rb) : nullptr;
    if (!Alive(rb)) want = M_NONE;
    if (want == M_NONE) {
        if (Applied()) Land(c.paused);
        return;
    }
    Mode was = S.mode;
    if (was == M_NONE) {
        if (!Kinematic(rb)) Call(K.stop, nm);
        Object* gc = Ref(nm, K.gc);
        bool off = false;
        if (Alive(gc)) Call(K.heavyFall, gc, {&off});
        S.nm.set(nm);
    }
    if (S.rb.get() != rb) S.rb.set(rb);
    Object* cc = Ref(nm, K.cc);
    if (!Alive(cc)) cc = nullptr;
    if (S.cc.get() != cc) S.cc.set(cc);
    S.mode = want;
    if (want == M_CLIP) {
        ForceOff(Part(S.kib, nm, K.kib), true, S.kib);
        ForceOff(Part(S.vcb, nm, K.vcb), true, S.vcb);
        if (!Kinematic(rb)) {
            SetKinematic(rb, true);
            S.kin = true;
        }
    } else if (was == M_CLIP) {
        Unclip(rb);
        Vec3 still{};
        if (!Kinematic(rb)) Call(K.setVel, rb, {&still});
    }
    if (Enabled(nm)) SetEnabled(nm, false);
    Hurt(nm, c.dt);
    Keys k;
    if (cc && Bit(cc, K.camOn) && !Locked()) k = ReadKeys();
    float n = Slider(c, kFlySpeed, 1.0f, 10.0f) * (k.fast ? 2.5f : 1.0f);
    if (want == M_FLY) {
        Gravity(rb, false);
        if (!Kinematic(rb)) {
            Vec3 v{};
            if (k.x != 0.0f || k.y != 0.0f || k.up != k.down) {
                Object* ct = TransformOf(cc);
                Vec3 up = Up(rb);
                Vec3 d = Add(Mul(Right(ct), k.x), Mul(Forward(ct), k.y));
                if (k.up) d = Add(d, up);
                if (k.down) d = Sub(d, up);
                v = Mul(d, 30.0f * n);
            }
            if (Finite(v)) Call(K.setVel, rb, {&v});
        }
    } else if (was == M_CLIP && c.dt > 0.0f && (k.x != 0.0f || k.y != 0.0f || k.up != k.down)) {
        Object* cam = Ref(cc, K.cam);
        Object* ct = Alive(cam) ? TransformOf(cam) : TransformOf(cc);
        Vec3 up = Up(rb);
        float s = 40.0f * n;
        Vec3 v = Add(Mul(Forward(ct), k.y * s), Mul(Right(ct), k.x * s));
        if (k.up) v = Add(v, Mul(up, s));
        if (k.down) v = Sub(v, Mul(up, s));
        Vec3 p;
        if (Len(v) > 0.0f && Finite(v)) {
            Call(K.sync, nullptr);
            if (BodyPos(rb, p)) {
                p = Add(p, Mul(v, c.dt));
                Call(K.setPos, rb, {&p});
            }
        }
    }
    Roll(cc, k.x, k.y);
}

void Report(bool ok, const char* id) {
    if (!ok) LOG("movement: %s is not available, a part of the game it needs was not found", id);
}

}

bool Resolve() {
    Class* nm = K.nm = Cls("NewMovement");
    if (!nm) return false;
    K.walk = F(nm, "walkSpeed");
    K.stamina = F(nm, "boostCharge");
    K.wallJumps = F(nm, "currentWallJumps");
    K.cooldown = F(nm, "jumpCooldown");
    K.fakeFall = F(nm, "fakeFallRequests");
    K.padFreeze = F(nm, "gamepadFreezeCount");
    K.hp = F(nm, "hp");
    K.hurtInv = F(nm, "hurtInvincibility");
    K.levelOver = F(nm, "levelOver");
    K.exploded = F(nm, "exploded");
    K.rb = F(nm, "rb");
    K.gc = F(nm, "gc");
    K.cc = F(nm, "cc");
    K.jump = M(nm, "Jump", 0);
    K.stop = M(nm, "StopMovement", 0);
    Class* ground = Cls("GroundCheckGroup");
    K.onGround = M(ground, "get_onGround", 0);
    K.heavyFall = M(ground, "set_heavyFall", 1);
    Class* camera = Cls("CameraController");
    K.cam = F(camera, "cam");
    K.camOn = F(camera, "activated");
    K.camHor = F(camera, "movementHor");
    K.camVer = F(camera, "movementVer");
    Class* gsm = Cls("GameStateManager");
    K.gsmInstance = F(gsm, "<Instance>k__BackingField");
    K.gsmLocked = F(gsm, "<PlayerInputLocked>k__BackingField");
    K.im = Cls("InputManager");
    K.source = F(K.im, "<InputSource>k__BackingField");
    K.device = F(K.im, "<LastButtonDevice>k__BackingField");
    Class* input = Cls("PlayerInput");
    K.move = F(input, "Move");
    K.jumpKey = F(input, "Jump");
    K.slideKey = F(input, "Slide");
    K.dashKey = F(input, "Dodge");
    Class* state = Cls("InputActionState");
    K.action = F(state, "<Action>k__BackingField");
    K.pressed = F(state, "<IsPressed>k__BackingField");
    K.performed = F(state, "<PerformedFrame>k__BackingField");
    K.wasPerformed = M(state, "get_WasPerformedThisFrame", 0);
    K.readValue = M(Cls("UnityEngine.InputSystem.InputAction"), "ReadValueAsObject", 0);
    K.vec2 = Cls("UnityEngine.Vector2");
    K.pad = Cls("UnityEngine.InputSystem.Gamepad");
    K.kib = Cls("KeepInBounds");
    K.approve = M(K.kib, "ForceApproveNewPosition", 0);
    K.vcb = Cls("VerticalClippingBlocker");
    K.gravity = M(Cls("PhysicsExtensions"), "SetGravityMode", 2);
    Class* body = Cls("UnityEngine.Rigidbody");
    K.getKin = M(body, "get_isKinematic", 0);
    K.setKin = M(body, "set_isKinematic", 1);
    K.getPos = M(body, "get_position", 0);
    K.setPos = M(body, "set_position", 1);
    K.setVel = M(body, "set_velocity", 1);
    Class* go = Cls("UnityEngine.GameObject");
    K.getLayer = M(go, "get_layer", 0);
    K.setLayer = M(go, "set_layer", 1);
    K.sync = M(Cls("UnityEngine.Physics"), "SyncTransforms", 0);
    bool keys = K.im && K.source && K.move && K.jumpKey && K.slideKey && K.dashKey && K.action && K.pressed && K.readValue && K.vec2;
    bool player = K.rb && K.cc && K.camOn && K.stop && K.getKin && K.gravity;
    K.dash = static_cast<bool>(K.stamina);
    K.walls = static_cast<bool>(K.wallJumps);
    K.speed = static_cast<bool>(K.walk);
    K.jumps = K.jump && K.cooldown && K.gc && K.onGround && K.im && K.source && K.jumpKey && (K.performed || K.wasPerformed) &&
              K.gsmInstance && K.gsmLocked;
    K.fly = keys && player && K.setVel;
    K.clip = keys && player && K.setKin && K.getPos && K.setPos && K.kib && K.vcb && K.approve;
    Report(K.dash, "unlimited dash");
    Report(K.jumps, "unlimited jumps");
    Report(K.walls, "unlimited wall jumps");
    Report(K.speed, "player speed");
    Report(K.fly, "fly");
    Report(K.clip, "noclip");
    return K.dash || K.jumps || K.walls || K.speed || K.fly || K.clip;
}

bool Supported(const std::string& id) {
    if (id == kDash) return K.dash;
    if (id == kJumps) return K.jumps;
    if (id == kWalls) return K.walls;
    if (id == kSpeed) return K.speed;
    if (id == kFly) return K.fly;
    if (id == kNoclip) return K.clip;
    if (id == kFlySpeed) return K.fly || K.clip;
    return false;
}

void Tick(const Ctx& c) {
    Object* nm = c.inLevel && Alive(c.player) ? c.player : nullptr;
    if (S.nm.held() && S.nm.get() != nm) Land(c.paused);
    if (S.walk.obj.held() && S.walk.obj.get() != nm) Unwalk();
    bool live = nm && !c.dead && c.activated;
    if (live && K.dash && c.on(kDash) && Val<float>(nm, K.stamina, 300.0f) < 300.0f) SetVal<float>(nm, K.stamina, 300.0f);
    if (live && K.walls && c.on(kWalls) && Val<int>(nm, K.wallJumps) != 0) SetVal<int>(nm, K.wallJumps, 0);
    Speed(nm, live, c);
    Mode want = M_NONE;
    if (live && K.clip && c.on(kNoclip)) want = M_CLIP;
    else if (live && K.fly && c.on(kFly)) want = M_FLY;
    if (live && want == M_NONE && S.mode == M_NONE && K.jumps && c.on(kJumps)) AirJump(nm, c);
    Fly(nm, want, c);
}

void Release(const Ctx& c) {
    Land(c.paused);
    Unwalk();
}

void Action(const std::string&, const Ctx&) {}

bool Busy() {
    return Applied() || S.walk.obj.held();
}

}
