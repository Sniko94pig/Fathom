#pragma once
#include "core/model.h"
#include "game/ue.h"
#include <cmath>
#include <functional>
#include <string>
#include <vector>

namespace bl3 {

struct State {
    void* engine = nullptr;
    void* world = nullptr;
    void* gameInstance = nullptr;
    void* gameState = nullptr;
    void* pc = nullptr;
    void* pawn = nullptr;
    void* camera = nullptr;
    void* vehicle = nullptr;
    bool seated = false;
    core::Session session = core::Session::Solo;
    int others = 0;
    bool inWorld = false;
    bool canCheat = false;
    bool alone = false;
    bool menuOpen = false;
    bool mapVisible = false;
    float dt = 0.0f;
    double time = 0.0;
    unsigned long long tick = 0;
};

extern State st;

enum class Scan { Characters, Vehicles, Pickups, Lootables, Projectiles, Abilities, InventoryData, ItemPools, PoolExpansions, Balances, Count };

bool On(const char* id);
float Value(const char* id);
bool Pressed(const char* id);
bool Submitted(const char* id, std::vector<std::string>& inputs);
void Ready(const char* id, bool ok, const char* missing = nullptr);
void Note(const std::string& text);
void TurnOff(const char* id, const std::string& why);
void WithModel(const std::function<void(core::Model&)>& fn);
bool KeyDown(int vk);
std::string BackupNow();
std::wstring Folder();
const std::vector<void*>& Objects(Scan kind);
bool ScanComplete(Scan kind);

void* Lib(const char* path);
void* Cls(const char* name);
bool Has(const char* cls, const char* member);
bool HasFunc(const char* cls, const char* func);

ue::FVector Location(void* actor);
ue::FRotator Rotation(void* actor);
bool Teleport(void* actor, const ue::FVector& loc, const ue::FRotator& rot);
bool CameraView(ue::FVector& loc, ue::FRotator& rot, float& fov);
ue::FRotator ControlRotation();
void SetControlRotation(const ue::FRotator& r);
void* DamageComp(void* actor);
float Health(void* actor);
float MaxHealth(void* actor);
bool Dead(void* actor);
bool IsHostile(void* actor);
bool IsEnemy(void* actor);
int Hostility(void* actor, bool& strict, bool& loose);
ue::FVector QuickLocation(void* actor);
bool CritMarked(void* region);
bool IsPlayerPawn(void* actor);
void* ComponentOf(void* actor, const char* cls);
void* HeldWeapon();
std::vector<void*> PlayerWeapons();
std::string DisplayName(void* obj);
void* Globals();
void* GameplayGlobals();
bool DlcAllowed(const std::string& path);
bool DlcOwnedPath(const std::string& path);
bool DlcKnown();
void RefillAllAmmoNow();
bool MoveVehicle(void* vehicle, const ue::FVector& loc, const ue::FRotator& rot);
bool VehicleHasOtherPlayers(void* vehicle);
bool DrivingAlone();
void* SpawnActor(const char* cls, const ue::FVector& at, float yaw);
void DestroyActor(void* actor);
void SetViewTarget(void* target);
void* ViewTarget();
void IgnoreInput(bool look, bool move);
struct TraceHitInfo {
    ue::FVector at;
    void* actor = nullptr;
    bool inside = false;
};

bool TraceDown(const ue::FVector& from, float depth, ue::FVector& hit);
bool TraceLine(const ue::FVector& from, const ue::FVector& to, ue::FVector& hit);
bool TraceHit(const ue::FVector& from, const ue::FVector& to, TraceHitInfo& info);
void TakeMouse(float& dx, float& dy);

struct AttrFloat {
    int32_t base;
    float value;
    float baseValue;
};

struct Tracked {
    ue::FWeak obj;
    int offset = -1;
    float natural = 0.0f;
    float written = 0.0f;
};

void ScaleAttr(std::vector<Tracked>& list, void* obj, const char* prop, float factor);
void SetAttr(std::vector<Tracked>& list, void* obj, const char* prop, float value);
void RestoreAttrs(std::vector<Tracked>& list);

inline ue::FVector Add(const ue::FVector& a, const ue::FVector& b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline ue::FVector Sub(const ue::FVector& a, const ue::FVector& b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline ue::FVector Mul(const ue::FVector& a, float s) { return {a.x * s, a.y * s, a.z * s}; }
inline float Len(const ue::FVector& a) { return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z); }
inline float Dist(const ue::FVector& a, const ue::FVector& b) { return Len(Sub(a, b)); }
ue::FVector Forward(const ue::FRotator& r);
ue::FRotator LookAt(const ue::FVector& from, const ue::FVector& to);
float AngleBetween(const ue::FRotator& a, const ue::FRotator& b);

void PlayerInit();
void PlayerTick();
void PlayerOff();
void WeaponsInit();
void WeaponsTick();
void WeaponsOff();
void SkillsInit();
void SkillsTick();
void SkillsOff();
void MovementInit();
void MovementTick();
void MovementOff();
void AimInit();
void AimTick();
void AimOff();
void LootInit();
void LootTick();
void LootOff();
void GiverInit(core::Model& m);
void GiverTick();
void GiverOff();
void WorldInit(core::Model& m);
void WorldTick();
void WorldOff();
void MapshotInit(core::Model& m);
void MapshotTick();
void MapshotOff();
void VehicleInit();
void VehicleTick();
void VehicleOff();

}
