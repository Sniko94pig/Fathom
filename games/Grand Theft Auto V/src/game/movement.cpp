#include "game/internal.h"
#include "ingame/log.h"
#include <windows.h>
#include <algorithm>

namespace gtav {

namespace {

const int kOnFoot[] = {21, 22, 30, 31, 32, 33, 34, 35, 36, 44, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153, 154};
const int kInCar[] = {59, 60, 61, 62, 63, 64, 71, 72, 73, 76, 87, 88, 89, 90, 107, 108, 109, 110, 111, 112, 113};

struct Input {
    float fwd = 0, side = 0, up = 0;
    bool fast = false;
    bool any() const { return fwd != 0 || side != 0 || up != 0; }
};

struct Flight {
    bool active = false;
    int entity = 0;
    bool vehicle = false;
    bool flying = false;
    bool ragdoll = true;
} fly;

struct Clip {
    bool active = false;
    int entity = 0;
    bool vehicle = false;
    bool collisionWasOff = false;
} clip;

Input ReadInput() {
    Input in;
    if (On("freecam")) return in;
    in.fwd = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    in.side = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    in.up = (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - (KeyDown(VK_CONTROL) ? 1.0f : 0.0f);
    in.fast = KeyDown(VK_SHIFT);
    return in;
}

void BlockControls(bool vehicle) {
    for (int c : kOnFoot) n::DISABLE_CONTROL_ACTION(0, c, 1);
    if (vehicle)
        for (int c : kInCar) n::DISABLE_CONTROL_ACTION(0, c, 1);
}

V3 MoveDir(const Input& in, float& yawOut) {
    V3 rot = n::GET_GAMEPLAY_CAM_ROT(2);
    V3 fwd = RotToDir(rot);
    const float d = 3.14159265f / 180.0f;
    float yaw = rot.z * d;
    V3 right{std::cos(yaw), std::sin(yaw), 0.0f};
    yawOut = rot.z;
    V3 dir = Add(Add(Mul(fwd, in.fwd), Mul(right, in.side)), V3{0, 0, in.up});
    return Len(dir) > 1e-4f ? Norm(dir) : V3{0, 0, 0};
}

int Target(bool& vehicle) {
    vehicle = false;
    if (!st.ped || !st.alive) return 0;
    if (st.vehicle) {
        if (!st.driver) return 0;
        vehicle = true;
        return st.vehicle;
    }
    return st.ped;
}

void SettleOnGround(int e) {
    if (!e || !n::DOES_ENTITY_EXIST(e)) return;
    V3 p = n::GET_ENTITY_COORDS(e, false);
    float gz = 0;
    bool below = n::GET_GROUND_Z_FOR_3D_COORD(p.x, p.y, p.z + 1.0f, &gz, 0, 0) != 0;
    if (below && p.z - gz <= 3.0f) return;
    float top = 0;
    if (!below) {
        if (!n::GET_GROUND_Z_FOR_3D_COORD(p.x, p.y, 1000.0f, &top, 0, 0)) return;
        if (top <= p.z) return;
        gz = top;
    }
    n::SET_ENTITY_COORDS_NO_OFFSET(e, p.x, p.y, gz + 1.0f, 0, 0, 0);
    n::SET_ENTITY_VELOCITY(e, 0, 0, 0);
}

void FlyRelease(bool settle) {
    if (!fly.active) return;
    int e = fly.entity;
    if (e && n::DOES_ENTITY_EXIST(e)) {
        if (fly.vehicle) {
            n::SET_VEHICLE_GRAVITY(e, 1);
        } else {
            n::SET_PED_GRAVITY(e, 1);
            n::SET_PED_CAN_RAGDOLL(e, fly.ragdoll ? 1 : 0);
            if (fly.flying) n::CLEAR_PED_TASKS(e);
        }
        if (settle && fly.flying) SettleOnGround(e);
    }
    fly = Flight{};
}

void FlyTick() {
    bool vehicle = false;
    int e = On("fly") && !On("noclip") ? Target(vehicle) : 0;
    if (!e) {
        FlyRelease(true);
        return;
    }
    if (fly.active && fly.entity != e) FlyRelease(true);
    if (!fly.active) {
        fly.active = true;
        fly.entity = e;
        fly.vehicle = vehicle;
        fly.ragdoll = vehicle ? true : n::CAN_PED_RAGDOLL(e) != 0;
        LOG("fly on for %s %d", vehicle ? "vehicle" : "ped", e);
    }
    Input in = ReadInput();
    float yaw = 0;
    V3 dir = MoveDir(in, yaw);
    float speed = (vehicle ? 20.0f : 12.0f) * std::clamp(Value("fly_speed"), 1.0f, 10.0f) * (in.fast ? 2.5f : 1.0f);
    if (vehicle) {
        BlockControls(true);
        n::SET_VEHICLE_GRAVITY(e, 0);
        V3 v = Mul(dir, speed);
        n::SET_ENTITY_VELOCITY(e, v.x, v.y, v.z);
        if (in.any()) n::SET_ENTITY_HEADING(e, yaw);
        fly.flying = true;
        return;
    }
    bool airborne = n::IS_ENTITY_IN_AIR(e) != 0;
    if (!fly.flying) {
        if (!(in.up > 0 || (airborne && in.any()))) return;
        fly.flying = true;
        n::SET_PED_CAN_RAGDOLL(e, 0);
        if (!airborne) {
            V3 p = n::GET_ENTITY_COORDS(e, false);
            n::SET_ENTITY_COORDS_NO_OFFSET(e, p.x, p.y, p.z + 1.5f, 0, 0, 0);
        }
        n::TASK_SKY_DIVE(e, 1);
    }
    if (!airborne && in.up <= 0 && fly.flying && !in.any()) {
        fly.flying = false;
        n::SET_PED_GRAVITY(e, 1);
        n::SET_PED_CAN_RAGDOLL(e, fly.ragdoll ? 1 : 0);
        n::CLEAR_PED_TASKS(e);
        return;
    }
    BlockControls(false);
    n::SET_PED_CAN_RAGDOLL(e, 0);
    n::SET_PED_GRAVITY(e, 0);
    V3 v = Mul(dir, speed);
    n::SET_ENTITY_VELOCITY(e, v.x, v.y, v.z);
    n::SET_ENTITY_HEADING(e, yaw);
    if (!n::IS_PED_IN_PARACHUTE_FREE_FALL(e) && n::GET_PED_PARACHUTE_STATE(e) <= 0) n::TASK_SKY_DIVE(e, 1);
}

void ClipRelease() {
    if (!clip.active) return;
    int e = clip.entity;
    if (e && n::DOES_ENTITY_EXIST(e)) {
        n::FREEZE_ENTITY_POSITION(e, 0);
        if (!clip.collisionWasOff) n::SET_ENTITY_COLLISION(e, 1, 1);
        n::SET_ENTITY_VELOCITY(e, 0, 0, 0);
        SettleOnGround(e);
    }
    clip = Clip{};
}

void ClipTick() {
    bool vehicle = false;
    int e = On("noclip") ? Target(vehicle) : 0;
    if (!e) {
        ClipRelease();
        return;
    }
    if (clip.active && clip.entity != e) ClipRelease();
    if (!clip.active) {
        clip.active = true;
        clip.entity = e;
        clip.vehicle = vehicle;
        clip.collisionWasOff = n::GET_ENTITY_COLLISION_DISABLED(e) != 0;
        LOG("noclip on for %s %d", vehicle ? "vehicle" : "ped", e);
    }
    BlockControls(vehicle);
    n::SET_ENTITY_COLLISION(e, 0, 0);
    n::FREEZE_ENTITY_POSITION(e, 1);
    n::SET_ENTITY_VELOCITY(e, 0, 0, 0);
    Input in = ReadInput();
    float yaw = 0;
    V3 dir = MoveDir(in, yaw);
    float speed = 8.0f * std::clamp(Value("noclip_speed"), 1.0f, 10.0f) * (in.fast ? 3.0f : 1.0f);
    V3 p = n::GET_ENTITY_COORDS(e, false);
    if (in.any()) {
        V3 q = Add(p, Mul(dir, speed * std::min(st.dt, 0.05f)));
        n::SET_ENTITY_COORDS_NO_OFFSET(e, q.x, q.y, q.z, 0, 0, 0);
    }
    n::SET_ENTITY_HEADING(e, yaw);
}

}

void MovementInit() {
    Ready("fly", true);
    Ready("noclip", true);
}

void MovementTick() {
    ClipTick();
    FlyTick();
}

void MovementOff() {
    ClipRelease();
    FlyRelease(true);
}

bool MovementBusy() { return fly.active || clip.active; }

}
