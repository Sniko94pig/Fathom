#include "game/internal.h"
#include "ingame/log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace gtav {

namespace {

const float kNormalFov = 50.0f;

struct Free {
    bool active = false;
    int cam = 0;
    V3 pos;
    V3 rot;
} freecam;

struct Wide {
    bool active = false;
    int cam = 0;
} wide;

bool OtherCamera(int ours) {
    int r = n::GET_RENDERING_CAM();
    return r != -1 && r != 0 && r != ours;
}

bool Blocked() { return !st.ped || st.cutscene || st.switching || st.faded; }

void StopRender(int& cam) {
    if (!cam) return;
    n::RENDER_SCRIPT_CAMS(0, 0, 0, 1, 0, 0);
    if (n::DOES_CAM_EXIST(cam)) n::DESTROY_CAM(cam, 0);
    cam = 0;
}

float FovSetting() { return std::clamp(Value("fov"), 30.0f, 120.0f); }

bool FovChanged() { return std::fabs(FovSetting() - kNormalFov) > 0.5f; }

void FreeRelease() {
    if (!freecam.active) return;
    StopRender(freecam.cam);
    n::CLEAR_FOCUS();
    freecam = Free{};
}

void FreeTick() {
    if (!On("freecam")) {
        FreeRelease();
        return;
    }
    if (Blocked()) {
        if (freecam.active) {
            FreeRelease();
            TurnOff("freecam", "Freecam turned off for a cutscene or loading screen");
        }
        return;
    }
    if (!freecam.active) {
        if (OtherCamera(wide.cam)) return;
        if (wide.active) {
            StopRender(wide.cam);
            wide = Wide{};
        }
        freecam.pos = n::GET_GAMEPLAY_CAM_COORD();
        freecam.rot = n::GET_GAMEPLAY_CAM_ROT(2);
        float fov = FovChanged() ? FovSetting() : n::GET_GAMEPLAY_CAM_FOV();
        freecam.cam = n::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", freecam.pos.x, freecam.pos.y, freecam.pos.z, freecam.rot.x, 0.0f,
                                                freecam.rot.z, fov, 1, 2);
        if (!freecam.cam) {
            TurnOff("freecam", "The game did not allow a free camera right now");
            return;
        }
        n::RENDER_SCRIPT_CAMS(1, 0, 0, 1, 0, 0);
        freecam.active = true;
        LOG("freecam on");
    }
    if (OtherCamera(freecam.cam)) {
        FreeRelease();
        TurnOff("freecam", "Freecam turned off: the game switched cameras");
        return;
    }
    n::DISABLE_ALL_CONTROL_ACTIONS(0);
    if (!(st.menuOpen && st.menuFocused)) {
        float lx = n::GET_DISABLED_CONTROL_NORMAL(0, 1), ly = n::GET_DISABLED_CONTROL_NORMAL(0, 2);
        freecam.rot.z -= lx * 8.0f;
        freecam.rot.x = std::clamp(freecam.rot.x - ly * 8.0f, -89.0f, 89.0f);
    }
    float fwd = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f);
    float side = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    float up = (KeyDown(VK_SPACE) ? 1.0f : 0.0f) - (KeyDown(VK_CONTROL) ? 1.0f : 0.0f);
    V3 f = RotToDir(freecam.rot);
    const float d = 3.14159265f / 180.0f;
    V3 r{std::cos(freecam.rot.z * d), std::sin(freecam.rot.z * d), 0.0f};
    V3 dir = Add(Add(Mul(f, fwd), Mul(r, side)), V3{0, 0, up});
    if (Len(dir) > 1e-4f) {
        float speed = 6.0f * std::clamp(Value("freecam_speed"), 1.0f, 10.0f) * (KeyDown(VK_SHIFT) ? 3.0f : 1.0f);
        freecam.pos = Add(freecam.pos, Mul(Norm(dir), speed * std::min(st.dt, 0.05f)));
    }
    n::SET_CAM_COORD(freecam.cam, freecam.pos.x, freecam.pos.y, freecam.pos.z);
    n::SET_CAM_ROT(freecam.cam, freecam.rot.x, 0.0f, freecam.rot.z, 2);
    n::SET_CAM_FOV(freecam.cam, FovChanged() ? FovSetting() : n::GET_GAMEPLAY_CAM_FOV());
    n::SET_FOCUS_POS_AND_VEL(freecam.pos.x, freecam.pos.y, freecam.pos.z, 0, 0, 0);
}

void WideRelease() {
    if (!wide.active) return;
    StopRender(wide.cam);
    wide = Wide{};
}

bool FirstPerson() {
    if (st.vehicle) return n::GET_FOLLOW_VEHICLE_CAM_VIEW_MODE() == 4;
    return n::GET_FOLLOW_PED_CAM_VIEW_MODE() == 4;
}

void WideTick() {
    bool want = FovChanged() && !freecam.active && !On("freecam") && !Blocked() && !FirstPerson();
    if (want && !wide.active && OtherCamera(0)) want = false;
    if (want && wide.active && OtherCamera(wide.cam)) want = false;
    if (!want) {
        WideRelease();
        return;
    }
    V3 pos = n::GET_GAMEPLAY_CAM_COORD();
    V3 rot = n::GET_GAMEPLAY_CAM_ROT(2);
    int mover = st.vehicle ? st.vehicle : st.ped;
    V3 vel = n::GET_ENTITY_VELOCITY(mover);
    pos = Add(pos, Mul(vel, n::GET_FRAME_TIME()));
    float fov = std::clamp(n::GET_GAMEPLAY_CAM_FOV() * FovSetting() / kNormalFov, 5.0f, 130.0f);
    if (!wide.active) {
        wide.cam = n::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", pos.x, pos.y, pos.z, rot.x, rot.y, rot.z, fov, 1, 2);
        if (!wide.cam) return;
        n::RENDER_SCRIPT_CAMS(1, 0, 0, 1, 0, 0);
        wide.active = true;
        LOG("wide view camera on");
    }
    n::SET_CAM_COORD(wide.cam, pos.x, pos.y, pos.z);
    n::SET_CAM_ROT(wide.cam, rot.x, rot.y, rot.z, 2);
    n::SET_CAM_FOV(wide.cam, fov);
}

}

void CameraInit() {
    Ready("freecam", true);
    Ready("fov", true);
}

void CameraTick() {
    if (MapshotBusy()) {
        FreeRelease();
        WideRelease();
        return;
    }
    FreeTick();
    WideTick();
}

void CameraOff() {
    FreeRelease();
    WideRelease();
}

}
