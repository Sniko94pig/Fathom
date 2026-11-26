#include "game/internal.h"
#include "ingame/log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace bl3 {

namespace {

const uint8_t kWalking = 1, kFalling = 3, kFlying = 5;

std::vector<Tracked> speedAttrs, flyAttrs;

struct FloatSave {
    ue::FWeak obj;
    int offset = -1;
    float natural = 0;
    float written = 0;
};

std::vector<FloatSave> floatSaves;

void ScaleFloat(void* obj, int offset, float factor) {
    if (!obj || offset < 0) return;
    FloatSave* s = nullptr;
    for (FloatSave& f : floatSaves)
        if (f.offset == offset && ue::WeakGet(f.obj) == obj) s = &f;
    if (!s) {
        floatSaves.push_back({ue::MakeWeak(obj), offset, ue::At<float>(obj, offset), std::nanf("")});
        s = &floatSaves.back();
    }
    float cur = ue::At<float>(obj, offset);
    if (!(cur == s->written)) s->natural = cur;
    float want = s->natural * factor;
    if (cur != want) ue::Put<float>(obj, offset, want);
    s->written = want;
}

void RestoreFloats(int lo, int hi) {
    for (auto it = floatSaves.begin(); it != floatSaves.end();) {
        if (it->offset < lo || it->offset > hi) {
            ++it;
            continue;
        }
        void* o = ue::WeakGet(it->obj);
        if (o && ue::At<float>(o, it->offset) == it->written) ue::Put<float>(o, it->offset, it->natural);
        it = floatSaves.erase(it);
    }
}

void* Movement() { return st.pawn ? ue::Ptr(st.pawn, "CharacterMovement") : nullptr; }

int Off(void* obj, const char* prop) {
    ue::Prop p = obj ? ue::FindProp(ue::ClassOf(obj), prop) : ue::Prop{};
    return p ? p.offset : -1;
}

uint8_t Mode(void* mc) {
    int o = Off(mc, "MovementMode");
    return o >= 0 ? ue::At<uint8_t>(mc, o) : 0;
}

void SetMode(void* mc, uint8_t mode) {
    ue::Call c(mc, "SetMovementMode");
    c.Set<uint8_t>("NewMovementMode", mode).Set<uint8_t>("NewCustomMode", 0);
    c.Run();
}

int MoveIgnored(void* pc) {
    ue::Call q(pc, "IsMoveInputIgnored");
    if (!q.Run()) return -1;
    return q.GetBool("ReturnValue") ? 1 : 0;
}

void SetMoveIgnore(void* pc, bool on) {
    ue::Call c(pc, "SetIgnoreMoveInput");
    c.SetBool("bNewMoveInput", on);
    c.Run();
}

const unsigned kFlyUser = 1, kCamUser = 2;

struct InputLock {
    ue::FWeak pc;
    unsigned users = 0;
    bool added = false;
    int total = 0;
    double checkAt = -100;
} lock;

void LockMove(unsigned user) {
    if (!st.pc) return;
    if (lock.users && ue::WeakGet(lock.pc) != st.pc) lock = {};
    lock.users |= user;
    lock.pc = ue::MakeWeak(st.pc);
    if (st.time - lock.checkAt < 0.5) return;
    lock.checkAt = st.time;
    if (lock.total >= 20 || MoveIgnored(st.pc) != 0) return;
    SetMoveIgnore(st.pc, true);
    lock.added = true;
    ++lock.total;
}

void UnlockMove(unsigned user) {
    if (!(lock.users & user)) return;
    lock.users &= ~user;
    if (lock.users) return;
    void* pc = ue::WeakGet(lock.pc);
    if (pc && lock.added && MoveIgnored(pc) != 0) SetMoveIgnore(pc, false);
    lock = {};
}

float Axis(int plus, int minus) { return (KeyDown(plus) ? 1.0f : 0.0f) - (KeyDown(minus) ? 1.0f : 0.0f); }

float UpAxis() { return (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - ((KeyDown(VK_LCONTROL)) ? 1.0f : 0.0f); }

ue::FVector Wish(const ue::FRotator& view, bool withPitch, float up) {
    float fwd = Axis('W', 'S'), side = Axis('D', 'A');
    float P = withPitch ? view.pitch * 3.14159265f / 180.0f : 0.0f, Y = view.yaw * 3.14159265f / 180.0f;
    ue::FVector f{std::cos(P) * std::cos(Y), std::cos(P) * std::sin(Y), std::sin(P)};
    ue::FVector r{-std::sin(Y), std::cos(Y), 0.0f};
    ue::FVector d = Add(Add(Mul(f, fwd), Mul(r, side)), {0.0f, 0.0f, up});
    float len = Len(d);
    return len > 1.0f ? Mul(d, 1.0f / len) : d;
}

float PitchOf(const ue::FRotator& r) {
    float p = std::remainder(r.pitch, 360.0f);
    return std::clamp(p, -89.0f, 89.0f);
}

void SetVelocity(void* mc, const ue::FVector& v) {
    int o = Off(mc, "Velocity");
    if (o >= 0) ue::Put<ue::FVector>(mc, o, v);
}

struct FlyState {
    bool active = false;
    bool noclip = false;
    bool paused = false;
    ue::FWeak pawn;
    bool collisionOff = false;
} fly;

void SetCollision(void* actor, bool on) {
    ue::Call c(actor, "SetActorEnableCollision");
    c.SetBool("bNewActorEnableCollision", on);
    c.Run();
}

void StopFly() {
    void* pawn = ue::WeakGet(fly.pawn);
    if (pawn) {
        void* mc = ue::Ptr(pawn, "CharacterMovement");
        if (fly.collisionOff) {
            SetCollision(pawn, true);
            if (!st.seated && !st.vehicle) {
                ue::FVector at = Location(pawn);
                Teleport(pawn, {at.x, at.y, at.z + 20.0f}, Rotation(pawn));
            }
        }
        if (mc && !st.seated && !st.vehicle) {
            SetVelocity(mc, {});
            if (Mode(mc) == kFlying) SetMode(mc, kFalling);
        }
    }
    UnlockMove(kFlyUser);
    if (!flyAttrs.empty()) RestoreAttrs(flyAttrs);
    fly = {};
}

void PauseFly() {
    UnlockMove(kFlyUser);
    if (!flyAttrs.empty()) RestoreAttrs(flyAttrs);
    fly.active = false;
    fly.paused = true;
    LOG("fly paused while you are in a car or a turret");
}

void Settle(bool want, bool wantNoclip) {
    void* pawn = ue::WeakGet(fly.pawn);
    if (fly.paused) {
        fly.paused = false;
        void* mc = pawn ? ue::Ptr(pawn, "CharacterMovement") : nullptr;
        if (!want && mc && Mode(mc) == kFlying) {
            SetVelocity(mc, {});
            SetMode(mc, kFalling);
        }
    }
    if (fly.collisionOff && !(want && wantNoclip && pawn && pawn == st.pawn)) {
        if (pawn) SetCollision(pawn, true);
        fly.collisionOff = false;
    }
    if (!fly.collisionOff) fly.pawn = {};
}

float FloorFollow(void* pawn) {
    ue::FVector at = Location(pawn);
    float half = 90.0f;
    void* cap = ue::Ptr(pawn, "CapsuleComponent");
    if (cap) {
        float h = ue::GetFloat(cap, "CapsuleHalfHeight", 90.0f);
        if (h > 10.0f && h < 400.0f) half = h;
    }
    ue::FVector floor;
    if (!TraceDown({at.x, at.y, at.z - half + 60.0f}, 20000.0f, floor)) return 0.0f;
    float target = floor.z + half + 2.0f;
    return std::clamp((target - at.z) * 12.0f, -3000.0f, 3000.0f);
}

void TickFly(bool wantFly, bool wantNoclip) {
    bool want = wantFly || wantNoclip;
    bool seated = st.vehicle || st.seated;
    void* pawn = ue::WeakGet(fly.pawn);
    if (fly.active && seated) PauseFly();
    if (fly.active && (!want || pawn != st.pawn || fly.noclip != wantNoclip)) StopFly();
    if (!fly.active && !seated && (fly.paused || fly.collisionOff)) Settle(want, wantNoclip);
    if (!want || !st.pawn || seated) return;
    void* mc = Movement();
    if (!mc) return;
    if (!fly.active) {
        fly.active = true;
        fly.noclip = wantNoclip;
        fly.pawn = ue::MakeWeak(st.pawn);
        if (wantNoclip) {
            SetCollision(st.pawn, false);
            fly.collisionOff = true;
        }
        LOG(wantNoclip ? "noclip on" : "fly on");
    }
    LockMove(kFlyUser);
    if (Mode(mc) != kFlying) SetMode(mc, kFlying);
    float slider = std::clamp(Value("fly_speed"), 1.0f, 10.0f);
    bool fast = KeyDown(VK_LSHIFT);
    ue::FRotator view = ControlRotation();
    ue::FVector vel;
    float speed;
    if (wantNoclip) {
        speed = 450.0f * slider * (fast ? 2.5f : 1.0f);
        vel = Mul(Wish(view, false, 0.0f), speed);
        vel.z = FloorFollow(st.pawn);
    } else {
        speed = 800.0f * slider * (fast ? 3.0f : 1.0f);
        vel = Mul(Wish(view, true, UpAxis()), speed);
    }
    SetAttr(flyAttrs, mc, "MaxFlySpeed", std::max(speed, 3000.0f) * 1.5f);
    SetVelocity(mc, vel);
}

struct FreeCam {
    bool active = false;
    ue::FWeak actor;
    ue::FWeak pc;
    ue::FRotator startRot;
    ue::FVector pos;
    double lastCheck = 0;
} cam;

void StopFreeCam() {
    if (!cam.active) return;
    void* pc = ue::WeakGet(cam.pc);
    if (pc && pc == st.pc) {
        void* back = ue::Ptr(st.pc, "Pawn");
        if (!ue::Alive(back)) back = st.vehicle ? st.vehicle : st.pawn;
        if (back) SetViewTarget(back);
        SetControlRotation(cam.startRot);
    }
    UnlockMove(kCamUser);
    DestroyActor(ue::WeakGet(cam.actor));
    cam = {};
    LOG("freecam off");
}

bool StartFreeCam() {
    ue::FVector pos;
    ue::FRotator rot;
    float fov = 90.0f;
    if (!st.pc || !CameraView(pos, rot, fov)) return false;
    void* actor = SpawnActor("/Script/Engine.CameraActor", pos, rot.yaw);
    if (!actor) {
        LOG("freecam: could not create a camera");
        return false;
    }
    void* comp = ue::Ptr(actor, "CameraComponent");
    if (comp) {
        ue::SetBool(comp, "bConstrainAspectRatio", false);
        ue::Call f(comp, "SetFieldOfView");
        f.Set<float>("InFieldOfView", fov);
        f.Run();
    }
    cam.active = true;
    cam.actor = ue::MakeWeak(actor);
    cam.pc = ue::MakeWeak(st.pc);
    cam.startRot = ControlRotation();
    cam.pos = pos;
    SetViewTarget(actor);
    LockMove(kCamUser);
    LOG("freecam on");
    return true;
}

void TickFreeCam(bool want) {
    if (cam.active && (!want || ue::WeakGet(cam.pc) != st.pc || !ue::WeakGet(cam.actor))) StopFreeCam();
    if (!want) return;
    if (!cam.active) {
        static double lastTry = 0;
        if (st.time - lastTry < 1.0) return;
        lastTry = st.time;
        if (!StartFreeCam()) return;
    }
    void* actor = ue::WeakGet(cam.actor);
    if (st.time - cam.lastCheck > 0.5) {
        cam.lastCheck = st.time;
        if (ViewTarget() != actor) SetViewTarget(actor);
        LockMove(kCamUser);
    }
    ue::FRotator view = ControlRotation();
    view.pitch = PitchOf(view);
    view.roll = 0.0f;
    float speed = 800.0f * std::clamp(Value("fly_speed"), 1.0f, 10.0f) * (KeyDown(VK_LSHIFT) ? 3.0f : 1.0f);
    float dt = std::min(st.dt, 0.1f);
    cam.pos = Add(cam.pos, Mul(Wish(view, true, UpAxis()), speed * dt));
    ue::Call c(actor, "K2_SetActorLocationAndRotation");
    c.Set("NewLocation", cam.pos).Set("NewRotation", view);
    c.SetBool("bSweep", false).SetBool("bTeleport", true);
    c.Run();
}

bool timeChanged = false;

void GameSpeed(bool allowed) {
    float v = Value("game_speed");
    bool want = allowed && std::fabs(v - 1.0f) > 0.01f;
    static float applied = 1.0f;
    if (want && (!timeChanged || std::fabs(applied - v) > 0.001f)) {
        ue::Call c(Lib("GameplayStatics"), "SetGlobalTimeDilation");
        c.Set<void*>("WorldContextObject", st.world).Set<float>("TimeDilation", std::clamp(v, 0.1f, 5.0f));
        if (c.Run()) {
            timeChanged = true;
            applied = v;
        }
    } else if (!want && timeChanged) {
        ue::Call c(Lib("GameplayStatics"), "SetGlobalTimeDilation");
        c.Set<void*>("WorldContextObject", st.world).Set<float>("TimeDilation", 1.0f);
        c.Run();
        timeChanged = false;
        applied = 1.0f;
    }
}

}

void MovementInit() {
    Ready("player_speed", Has("CharacterMovementComponent", "MaxWalkSpeed"));
    Ready("jump_height", Has("CharacterMovementComponent", "JumpZVelocity"));
    bool mode = HasFunc("CharacterMovementComponent", "SetMovementMode") && Has("MovementComponent", "Velocity");
    Ready("fly", mode);
    Ready("noclip", mode && HasFunc("Actor", "SetActorEnableCollision") && HasFunc("KismetSystemLibrary", "LineTraceSingle"));
    Ready("freecam", HasFunc("PlayerController", "SetViewTargetWithBlend") && HasFunc("GameplayStatics", "BeginDeferredActorSpawnFromClass"));
    Ready("game_speed", HasFunc("GameplayStatics", "SetGlobalTimeDilation"));
}

void MovementTick() {
    void* mc = Movement();
    float speed = Value("player_speed");
    if (speed > 1.001f && mc) {
        ScaleAttr(speedAttrs, mc, "MaxWalkSpeed", speed);
        ScaleAttr(speedAttrs, mc, "MaxWalkSpeedCrouched", speed);
        if (ue::FindProp(ue::ClassOf(mc), "MaxSprintSpeed")) ScaleAttr(speedAttrs, mc, "MaxSprintSpeed", speed);
        ScaleFloat(mc, Off(mc, "MaxAcceleration"), std::min(speed, 3.0f));
    } else if (!speedAttrs.empty()) {
        RestoreAttrs(speedAttrs);
        int o = Off(mc, "MaxAcceleration");
        if (o >= 0) RestoreFloats(o, o);
    }
    float jump = Value("jump_height");
    if (jump > 1.001f && mc) {
        float v = std::sqrt(jump);
        ScaleFloat(mc, Off(mc, "JumpZVelocity"), v);
        for (const char* g : {"JumpGoal_Default", "JumpGoal_Sprinting"}) {
            int o = Off(mc, g);
            if (o < 0) continue;
            ScaleFloat(mc, o, v);
            ScaleFloat(mc, o + 4, jump);
        }
    } else if (mc) {
        int o = Off(mc, "JumpZVelocity");
        if (o >= 0) RestoreFloats(o, o);
        for (const char* g : {"JumpGoal_Default", "JumpGoal_Sprinting"}) {
            int go = Off(mc, g);
            if (go >= 0) RestoreFloats(go, go + 4);
        }
    }
    TickFly(On("fly"), On("noclip"));
    TickFreeCam(On("freecam"));
    GameSpeed(st.alone);
}

void MovementOff() {
    if (!speedAttrs.empty()) RestoreAttrs(speedAttrs);
    RestoreFloats(0, 0x7FFFFFFF);
    if (fly.active || fly.collisionOff || fly.paused) StopFly();
    StopFreeCam();
    if (timeChanged) GameSpeed(false);
}

}
