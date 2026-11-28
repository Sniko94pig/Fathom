#pragma once
#include "core/model.h"
#include "game/natives.h"
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gtav {

using n::V3;

struct State {
    int player = -1;
    int ped = 0;
    int vehicle = 0;
    bool driver = false;
    bool alive = false;
    bool cutscene = false;
    bool switching = false;
    bool faded = false;
    bool loading = false;
    int character = -1;
    uint32_t model = 0;
    V3 pos;
    float heading = 0;
    bool menuOpen = false;
    bool menuFocused = false;
    bool mapVisible = false;
    float dt = 0;
    double time = 0;
    uint64_t tick = 0;
};

extern State st;

bool On(const char* id);
float Value(const char* id);
bool Pressed(const char* id);
void Ready(const char* id, bool ok, const char* missing = nullptr);
void Note(const std::string& text);
void TurnOff(const char* id, const std::string& why);
void WithModel(const std::function<void(core::Model&)>& fn);
bool KeyDown(int vk);
bool BackupOnce();
std::wstring Folder();
uint32_t Joaat(const char* s);
double Now();
std::vector<int> AllPeds();
std::vector<int> AllVehicles();
void Post(std::function<void()> fn);

inline V3 Add(const V3& a, const V3& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline V3 Sub(const V3& a, const V3& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline V3 Mul(const V3& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Len(const V3& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline float Dist(const V3& a, const V3& b) { return Len(Sub(a, b)); }
inline V3 Norm(const V3& a) {
    float l = Len(a);
    return l > 1e-5f ? Mul(a, 1.0f / l) : V3{0, 0, 0};
}
V3 CamForward();
V3 RotToDir(const V3& rot);

void PlayerInit();
void PlayerTick();
void PlayerOff();
void MovementInit();
void MovementTick();
void MovementOff();
bool MovementBusy();
void WeaponsInit();
void WeaponsTick();
void WeaponsOff();
void AimInit();
void AimTick();
void AimOff();
void SilentShots(uint32_t weapon, int shots);
const std::vector<uint32_t>& KnownWeapons();
bool IsGun(uint32_t weapon);
void GiverInit(core::Model& m);
void GiverTick();
void GiverOff();
void VehiclesInit(core::Model& m);
void VehiclesTick();
void VehiclesOff();
void WorldInit(core::Model& m);
void WorldTick();
void WorldOff();
void CameraInit();
void CameraTick();
void CameraOff();
void TrollInit();
void TrollTick();
void TrollOff();
void MapshotInit(core::Model& m);
void MapshotTick();
void MapshotOff();
bool MapshotBusy();
bool MapshotWriting();

}
