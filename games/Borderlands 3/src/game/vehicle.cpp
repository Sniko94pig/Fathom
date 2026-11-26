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
    float yaw = 0;
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
        LOG("invincible car on");
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
            ue::FVector at = Location(v);
            ue::FRotator level{0.0f, Rotation(v).yaw, 0.0f};
            std::vector<ue::FVector> spots;
            for (float up : {100.0f, 2000.0f}) {
                ue::FVector ground;
                if (TraceDown({at.x, at.y, at.z + up}, 5000.0f + up, ground)) spots.push_back({at.x, at.y, ground.z + 160.0f});
            }
            spots.push_back({at.x, at.y, at.z + 60.0f});
            Collision(v, true);
            bool placed = false;
            for (const ue::FVector& s : spots)
                if (Teleport(v, s, level)) {
                    placed = true;
                    break;
                }
            if (!placed) {
                LOG("car noclip off: no free spot found near %.0f %.0f %.0f", at.x, at.y, at.z);
                Note("Could not find a free spot for your car, it may jump a little");
            }
            Physics(v, true);
        }
        SetGravity(v, true);
        SetVelocity(v, {});
    }
    if (fly.active) LOG(fly.noclip ? "car noclip off" : "car fly off");
    fly = {};
}

float Approach(float from, float to, float maxStep) {
    float d = std::remainder(to - from, 360.0f);
    return from + std::clamp(d, -maxStep, maxStep);
}

void TickFly(bool wantFly, bool wantNoclip) {
    bool want = (wantFly || wantNoclip) && st.vehicle && DrivingAlone();
    void* cur = ue::WeakGet(fly.vehicle);
    if (fly.active && (!want || cur != st.vehicle || fly.noclip != wantNoclip)) StopFly();
    if (!want) return;
    void* v = st.vehicle;
    if (!fly.active) {
        fly.active = true;
        fly.noclip = wantNoclip;
        fly.vehicle = ue::MakeWeak(v);
        fly.yaw = Rotation(v).yaw;
        SetGravity(v, false);
        if (wantNoclip) {
            Physics(v, false);
            Collision(v, false);
        }
        LOG(wantNoclip ? "car noclip on" : "car fly on");
    }
    ue::FVector camLoc;
    ue::FRotator camRot;
    float fov;
    if (!CameraView(camLoc, camRot, fov)) camRot = ControlRotation();
    float f = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    float s = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    float u = (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - ((KeyDown(VK_LCONTROL)) ? 1.0f : 0.0f);
    float speed = 1200.0f * std::clamp(Value("fly_speed"), 1.0f, 10.0f) * (KeyDown(VK_LSHIFT) ? 2.5f : 1.0f);
    ue::FVector fw = Forward(camRot);
    float Y = camRot.yaw * 3.14159265f / 180.0f;
    ue::FVector right{-std::sin(Y), std::cos(Y), 0.0f};
    ue::FVector dir = Add(Add(Mul(fw, f), Mul(right, s)), {0, 0, u});
    float len = Len(dir);
    if (len > 1.0f) dir = Mul(dir, 1.0f / len);
    ue::FVector vel = Mul(dir, speed);
    float dt = std::min(st.dt, 0.1f);
    fly.yaw = Approach(fly.yaw, camRot.yaw, 240.0f * dt);
    ue::FRotator level{0.0f, fly.yaw, 0.0f};
    if (wantNoclip) {
        ue::FVector next = Add(Location(v), Mul(vel, dt));
        ue::Call c(v, "K2_SetActorLocationAndRotation");
        c.Set("NewLocation", next).Set("NewRotation", level).SetBool("bSweep", false).SetBool("bTeleport", true);
        c.Run();
        SetVelocity(v, {});
    } else {
        ue::Call r(v, "K2_SetActorRotation");
        r.Set("NewRotation", level).SetBool("bTeleportPhysics", true);
        r.Run();
        SetVelocity(v, vel);
    }
}

void TickBoost(bool want) {
    static bool logged = false;
    if (!want || !st.vehicle) {
        logged = false;
        return;
    }
    void* boost = ue::Ptr(st.vehicle, "BoostComponent");
    PoolRef ref;
    bool ok = boost && ReadPool(boost, "BoostPool", ref);
    if (ok) FillPool(ref);
    if (!logged) {
        logged = true;
        LOG(ok ? "car boost refill on" : "this car has no boost");
    }
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
    struct Cached {
        ue::FWeak v;
        bool others = false;
        double at = -100;
    };
    static Cached cache[4];
    for (Cached& c : cache)
        if (ue::WeakGet(c.v) == vehicle && st.time - c.at < 1.0) return c.others;
    bool others = false;
    ue::Call n(vehicle, "GetSeatCount");
    int seats = n.Run() ? std::clamp(n.Get<int32_t>("ReturnValue"), 0, 16) : 0;
    for (int i = 0; i < seats; ++i) {
        ue::Call c(vehicle, "GetSeatOccupant");
        c.Set<int32_t>("SeatSlot", i).SetBool("bIncludeDetaching", true);
        if (!c.Run()) continue;
        void* p = c.Get<void*>("ReturnValue");
        if (ue::Alive(p) && p != st.pawn && IsPlayerPawn(p)) others = true;
    }
    if (seats == 0) {
        ue::Call c(vehicle, "GetAllOccupants");
        c.SetBool("bIncludeDetaching", true);
        if (c.Run()) {
            auto a = c.Get<ue::TArray<void*>>("ReturnValue");
            for (int i = 0; a.data && i < a.num && i < 16; ++i) {
                void* p = ue::At<void*>(a.data + i, 0);
                if (ue::Alive(p) && p != st.pawn && IsPlayerPawn(p)) others = true;
            }
        }
    }
    Cached* slot = &cache[0];
    for (Cached& x : cache)
        if (x.at < slot->at) slot = &x;
    *slot = {ue::MakeWeak(vehicle), others, st.time};
    return others;
}

bool DrivingAlone() {
    if (!st.vehicle) return false;
    static ue::FWeak lastV;
    static bool lastResult = false;
    static double lastAt = -100;
    if (ue::WeakGet(lastV) == st.vehicle && st.time - lastAt < 0.5) return lastResult;
    bool driver = false;
    ue::Call c(st.vehicle, "GetPrimaryOccupant");
    c.SetBool("bIncludeDetaching", false);
    if (c.Run()) driver = c.Get<void*>("ReturnValue") == st.pawn;
    lastResult = driver && !VehicleHasOtherPlayers(st.vehicle);
    lastV = ue::MakeWeak(st.vehicle);
    lastAt = st.time;
    return lastResult;
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
    Ready("vehicle_noclip", HasFunc("OakVehicle", "DisableVehiclePhysics") && HasFunc("OakVehicle", "EnableVehiclePhysics") &&
                                HasFunc("Actor", "K2_SetActorLocationAndRotation"));
    Ready("bring_vehicle", HasFunc("OakCharacter", "GetOwnedVehicle") && HasFunc("Actor", "K2_TeleportTo"));
}

void VehicleTick() {
    if (st.vehicle) lastVehicle = ue::MakeWeak(st.vehicle);
    bool mine = st.vehicle && DrivingAlone();
    TickBoost(On("vehicle_boost") && mine);
    TickGod(On("vehicle_god") && mine);
    TickFly(On("vehicle_fly"), On("vehicle_noclip"));
    if (Pressed("bring_vehicle")) BringVehicle();
}

void VehicleOff() {
    TickGod(false);
    if (fly.active) StopFly();
}

}
