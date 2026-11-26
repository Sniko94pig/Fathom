#include "game/internal.h"
#include "ingame/log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace bl3 {

namespace {

const wchar_t* kReason = L"FathomMenu";

struct PoolRef {
    uint8_t raw[0x18] = {};
};

bool ReadPool(void* obj, const char* prop, PoolRef& out) {
    ue::Prop p = obj ? ue::FindProp(ue::ClassOf(obj), prop) : ue::Prop{};
    if (!p || p.size < (int)sizeof(out.raw)) return false;
    return ue::SafeCopy(out.raw, (uint8_t*)obj + p.offset, sizeof(out.raw));
}

void FillPool(const PoolRef& ref) {
    void* lib = Lib("GameResourcePoolFunctionLibrary");
    ue::Call b(lib, "BreakResourcePoolReference");
    b.SetRaw("InPool", ref.raw, sizeof(ref.raw));
    if (!b.Run() || !b.GetBool("bValid")) return;
    float cur = b.Get<float>("CurrentValue"), mx = b.Get<float>("MaxValue");
    if (mx <= 0 || cur >= mx) return;
    ue::Call s(lib, "SetResourcePoolValue");
    s.SetRaw("InPool", ref.raw, sizeof(ref.raw)).Set("InValue", mx);
    s.Run();
}

struct GodState {
    ue::FWeak vehicle;
    bool active = false;
    bool wasOn = false;
} god;

struct FlyState {
    ue::FWeak vehicle;
    bool active = false;
    bool noclip = false;
} fly;

ue::FWeak lastVehicle;

void* VehicleDamage(void* v) { return v ? ue::Ptr(v, "DamageComponent") : nullptr; }

void SetGod(void* v, bool on) {
    void* d = VehicleDamage(v);
    ue::Call c(d, "SetGodMode");
    c.SetBool("bInGodMode", on);
    c.Run();
}

bool IsGod(void* v) {
    void* d = VehicleDamage(v);
    ue::Call c(d, "IsInGodMode");
    return c.Run() && c.GetBool("ReturnValue");
}

void TickGod(bool want) {
    void* cur = ue::WeakGet(god.vehicle);
    if (god.active && (!want || cur != st.vehicle)) {
        if (cur && !god.wasOn) SetGod(cur, false);
        god = {};
    }
    if (want && !god.active && st.vehicle) {
        god.wasOn = IsGod(st.vehicle);
        SetGod(st.vehicle, true);
        god.active = true;
        god.vehicle = ue::MakeWeak(st.vehicle);
    }
    if (god.active && st.vehicle) {
        PoolRef hp;
        if ((st.tick % 15) == 0 && ReadPool(st.vehicle, "Health", hp)) FillPool(hp);
    }
}

void SetGravity(void* v, bool on) {
    void* mesh = ue::Ptr(v, "Mesh");
    ue::Call c(mesh, "SetEnableGravity");
    c.SetBool("bGravityEnabled", on);
    c.Run();
}

void SetVelocity(void* v, const ue::FVector& lin) {
    void* mesh = ue::Ptr(v, "Mesh");
    if (!mesh) return;
    ue::Call l(mesh, "SetAllPhysicsLinearVelocity");
    l.Set("NewVel", lin).SetBool("bAddToCurrent", false);
    l.Run();
    ue::Call a(mesh, "SetAllPhysicsAngularVelocity");
    a.Set("NewAngVel", ue::FVector{}).SetBool("bAddToCurrent", false);
    a.Run();
}

void Physics(void* v, bool enable) {
    ue::Call c(v, enable ? "EnableVehiclePhysics" : "DisableVehiclePhysics");
    c.Set("Reason", ue::MakeName(kReason));
    c.Run();
}

void Collision(void* v, bool on) {
    ue::Call c(v, "SetActorEnableCollision");
    c.SetBool("bNewActorEnableCollision", on);
    c.Run();
}

void StopFly() {
    void* v = ue::WeakGet(fly.vehicle);
    if (v) {
        if (fly.noclip) {
            Collision(v, true);
            Physics(v, true);
        }
        SetGravity(v, true);
        SetVelocity(v, {});
    }
    fly = {};
}

void Input(float& fwd, float& side, float& up) {
    fwd = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    side = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    up = (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - ((KeyDown(VK_LCONTROL) || KeyDown(VK_CONTROL)) ? 1.0f : 0.0f);
}

void TickFly(bool wantFly, bool wantNoclip) {
    bool want = (wantFly || wantNoclip) && st.vehicle && !VehicleHasOtherPlayers(st.vehicle);
    void* cur = ue::WeakGet(fly.vehicle);
    if (fly.active && (!want || cur != st.vehicle || fly.noclip != wantNoclip)) StopFly();
    if (!want) return;
    void* v = st.vehicle;
    if (!fly.active) {
        fly.active = true;
        fly.noclip = wantNoclip;
        fly.vehicle = ue::MakeWeak(v);
        SetGravity(v, false);
        if (wantNoclip) {
            Physics(v, false);
            Collision(v, false);
        }
    }
    ue::FVector camLoc;
    ue::FRotator camRot;
    float fov;
    if (!CameraView(camLoc, camRot, fov)) camRot = ControlRotation();
    float f, s, u;
    Input(f, s, u);
    float speed = 1200.0f * std::clamp(Value("fly_speed"), 1.0f, 10.0f) * (KeyDown(VK_LSHIFT) ? 2.0f : 1.0f);
    ue::FVector fw = Forward(camRot);
    float Y = camRot.yaw * 3.14159265f / 180.0f;
    ue::FVector right{-std::sin(Y), std::cos(Y), 0.0f};
    ue::FVector vel = Add(Add(Mul(fw, f * speed), Mul(right, s * speed)), {0, 0, u * speed});
    if (wantNoclip) {
        ue::FVector at = Location(v);
        ue::FVector next = Add(at, Mul(vel, st.dt));
        ue::Call c(v, "K2_SetActorLocationAndRotation");
        c.Set("NewLocation", next).Set("NewRotation", ue::FRotator{0.0f, camRot.yaw, 0.0f}).SetBool("bSweep", false).SetBool("bTeleport", true);
        c.Run();
    } else {
        SetVelocity(v, vel);
    }
}

void TickBoost(bool want) {
    if (!want || !st.vehicle) return;
    void* boost = ue::Ptr(st.vehicle, "BoostComponent");
    PoolRef ref;
    if (boost && ReadPool(boost, "BoostPool", ref)) FillPool(ref);
}

void BringVehicle() {
    if (st.vehicle) {
        Note("You are already in a car");
        return;
    }
    void* car = nullptr;
    if (st.pawn) {
        ue::Call c(st.pawn, "GetOwnedVehicle");
        if (c.Run()) car = c.Get<void*>("ReturnValue");
    }
    if (!ue::Alive(car)) car = ue::WeakGet(lastVehicle);
    if (!ue::Alive(car)) {
        Note("Drive a car first, or spawn one at a Catch-A-Ride");
        return;
    }
    if (VehicleHasOtherPlayers(car)) {
        Note("Someone else is in that car");
        return;
    }
    ue::FVector at = Location(st.pawn);
    ue::FRotator r = Rotation(st.pawn);
    ue::FVector fw = Forward({0.0f, r.yaw, 0.0f});
    ue::FVector spot = Add(at, Mul(fw, 700.0f));
    ue::FVector ground;
    if (TraceDown({spot.x, spot.y, spot.z + 500.0f}, 3000.0f, ground)) spot.z = ground.z + 200.0f;
    else spot.z += 200.0f;
    if (MoveVehicle(car, spot, {0.0f, r.yaw, 0.0f})) Note("Your car is here");
    else Note("Could not move your car");
}

}

bool VehicleHasOtherPlayers(void* vehicle) {
    if (!vehicle) return false;
    ue::Call c(vehicle, "GetAllOccupants");
    c.SetBool("bIncludeDetaching", true);
    if (!c.Run()) return false;
    auto a = c.Get<ue::TArray<void*>>("ReturnValue");
    for (int i = 0; a.data && i < a.num && i < 16; ++i) {
        void* p = ue::At<void*>(a.data + i, 0);
        if (ue::Alive(p) && p != st.pawn && IsPlayerPawn(p)) return true;
    }
    return false;
}

bool MoveVehicle(void* vehicle, const ue::FVector& loc, const ue::FRotator& rot) {
    if (!vehicle) return false;
    bool ok = Teleport(vehicle, loc, rot);
    SetVelocity(vehicle, {});
    return ok;
}

void VehicleInit() {
    Ready("vehicle_boost", Has("OakVehicle", "BoostComponent") && Has("VehicleBoostComponent", "BoostPool"));
    Ready("vehicle_god", Has("OakVehicle", "DamageComponent") && HasFunc("DamageComponent", "SetGodMode"));
    Ready("vehicle_fly", Has("Vehicle", "Mesh") && HasFunc("PrimitiveComponent", "SetEnableGravity") &&
                             HasFunc("PrimitiveComponent", "SetAllPhysicsLinearVelocity"));
    Ready("vehicle_noclip", HasFunc("OakVehicle", "DisableVehiclePhysics") && HasFunc("OakVehicle", "EnableVehiclePhysics"));
    Ready("bring_vehicle", HasFunc("OakCharacter", "GetOwnedVehicle") && HasFunc("Actor", "K2_TeleportTo"));
}

void VehicleTick() {
    if (st.vehicle) lastVehicle = ue::MakeWeak(st.vehicle);
    TickBoost(On("vehicle_boost"));
    TickGod(On("vehicle_god"));
    TickFly(On("vehicle_fly"), On("vehicle_noclip"));
    if (Pressed("bring_vehicle")) BringVehicle();
}

void VehicleOff() {
    TickGod(false);
    if (fly.active) StopFly();
}

}
