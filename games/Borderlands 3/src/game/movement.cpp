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

void SetVelocityZ(void* mc, float z, bool zeroXY) {
    int o = Off(mc, "Velocity");
    if (o < 0) return;
    ue::FVector v = ue::At<ue::FVector>(mc, o);
    v.z = z;
    if (zeroXY) v.x = v.y = 0;
    ue::Put<ue::FVector>(mc, o, v);
}

struct FlyState {
    bool active = false;
    bool noclip = false;
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
        if (fly.collisionOff) SetCollision(pawn, true);
        if (mc && Mode(mc) == kFlying) SetMode(mc, kFalling);
    }
    if (!flyAttrs.empty()) RestoreAttrs(flyAttrs);
    fly = {};
}

void TickFly(bool wantFly, bool wantNoclip) {
    bool want = wantFly || wantNoclip;
    void* pawn = ue::WeakGet(fly.pawn);
    if (fly.active && (!want || pawn != st.pawn || fly.noclip != wantNoclip)) StopFly();
    if (!want || !st.pawn || st.vehicle) {
        if (fly.active) StopFly();
        return;
    }
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
    }
    if (Mode(mc) != kFlying) SetMode(mc, kFlying);
    float speed = std::clamp(Value("fly_speed"), 1.0f, 10.0f);
    float walk = 600.0f;
    ue::Prop ws = ue::FindProp(ue::ClassOf(mc), "MaxWalkSpeed");
    if (ws) {
        float w = ue::At<float>(mc, ws.offset + 4);
        if (w > 50.0f && w < 5000.0f) walk = w;
    }
    SetAttr(flyAttrs, mc, "MaxFlySpeed", (wantNoclip ? walk : 900.0f) * speed);
    if (wantNoclip) {
        ue::FVector at = Location(st.pawn);
        ue::FVector floor;
        float half = 90.0f;
        void* cap = ue::Ptr(st.pawn, "CapsuleComponent");
        if (cap) {
            float h = ue::GetFloat(cap, "CapsuleHalfHeight", 90.0f);
            if (h > 10.0f && h < 400.0f) half = h;
        }
        if (TraceDown({at.x, at.y, at.z - half + 60.0f}, 20000.0f, floor)) {
            float target = floor.z + half + 2.0f;
            SetVelocityZ(mc, std::clamp((target - at.z) * 12.0f, -3000.0f, 3000.0f), false);
        } else {
            SetVelocityZ(mc, 0.0f, false);
        }
    } else {
        float up = (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - ((KeyDown(VK_LCONTROL) || KeyDown(VK_CONTROL)) ? 1.0f : 0.0f);
        SetVelocityZ(mc, up * 700.0f * speed, false);
    }
}

struct FreeCam {
    bool active = false;
    ue::FWeak actor;
    ue::FWeak pc;
    float x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
    double lastCheck = 0;
} cam;

void StopFreeCam() {
    if (!cam.active) return;
    if (ue::WeakGet(cam.pc) == st.pc && st.pc) {
        void* back = st.vehicle ? st.vehicle : st.pawn;
        if (back) SetViewTarget(back);
        IgnoreInput(false, false);
    }
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
    cam.x = pos.x;
    cam.y = pos.y;
    cam.z = pos.z;
    cam.yaw = rot.yaw;
    cam.pitch = std::clamp(rot.pitch > 180 ? rot.pitch - 360 : rot.pitch, -89.0f, 89.0f);
    SetViewTarget(actor);
    IgnoreInput(true, true);
    float dx, dy;
    TakeMouse(dx, dy);
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
    }
    float mx = 0, my = 0;
    TakeMouse(mx, my);
    if (!(st.menuOpen)) {
        cam.yaw += mx * 0.07f;
        cam.pitch = std::clamp(cam.pitch - my * 0.07f, -89.0f, 89.0f);
    }
    float fwd = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    float side = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    float up = (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - ((KeyDown(VK_LCONTROL) || KeyDown(VK_CONTROL)) ? 1.0f : 0.0f);
    float speed = 800.0f * std::clamp(Value("fly_speed"), 1.0f, 10.0f) * (KeyDown(VK_LSHIFT) ? 3.0f : 1.0f);
    float P = cam.pitch * 3.14159265f / 180.0f, Y = cam.yaw * 3.14159265f / 180.0f;
    float fx = std::cos(P) * std::cos(Y), fy = std::cos(P) * std::sin(Y), fz = std::sin(P);
    float rx = -std::sin(Y), ry = std::cos(Y);
    cam.x += (fx * fwd + rx * side) * speed * st.dt;
    cam.y += (fy * fwd + ry * side) * speed * st.dt;
    cam.z += (fz * fwd + up) * speed * st.dt;
    ue::Call c(actor, "K2_SetActorLocationAndRotation");
    c.Set("NewLocation", ue::FVector{cam.x, cam.y, cam.z}).Set("NewRotation", ue::FRotator{cam.pitch, cam.yaw, 0.0f});
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
    if (fly.active) StopFly();
    StopFreeCam();
    if (timeChanged) GameSpeed(false);
}

}
