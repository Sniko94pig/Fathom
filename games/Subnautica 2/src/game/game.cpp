#include "game/game.h"
#include "game/log.h"
#include "game/overlay.h"
#include "game/ue.h"
#include "game/progression_data.h"
#include "stb_image.h"
#include "minhook/include/MinHook.h"
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <climits>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <functional>
#include <map>
#include <set>
#include <unordered_map>
#include <cwctype>
#include <chrono>
#include <future>

namespace game {

namespace {

using core::Feature;
using core::Kind;
using core::Session;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
DWORD gameThread = 0;

using PEFn = void(__fastcall*)(void*, void*, void*);
PEFn oPE = nullptr;
void* peTarget = nullptr;
std::atomic<int> peInflight{0};
std::atomic<bool> shuttingDown{false};
std::atomic<bool> shutdownDone{false};
thread_local bool inTick = false;

std::mutex taskLock;
std::vector<std::function<void()>> tasks;
std::mutex noteLock;
std::vector<std::string> notes;

ULONGLONG lastRefresh = 0, lastEnforce = 0, lastSpeed = 0;
void* loggedPawn = nullptr;

struct State {
    void* engine = nullptr;
    void* world = nullptr;
    void* pc = nullptr;
    void* pawn = nullptr;
    void* cm = nullptr;
    void* health = nullptr;
    void* movement = nullptr;
    void* charMove = nullptr;
    void* healthSet = nullptr;
    void* survivalSet = nullptr;
    void* vehicle = nullptr;
    void* sub = nullptr;
    void* lastPlayer = nullptr;
    void* triedCheatsFor = nullptr;
    Session session = Session::Solo;
    bool inWorld = false;
} st;

struct Desired {
    bool on = false;
    float value = 1.0f;
};
std::map<std::string, Desired> want;
std::unordered_map<std::string, void*> itemTypes;
std::unordered_map<void*, std::string> typeNames;
std::wstring iconDir;

struct TagCheat {
    const char* id;
    const char* func;
    const wchar_t* tag;
    ue::FName name{};
    bool resolved = false;
    bool ours = false;
    bool warned = false;
};

std::vector<TagCheat> tagCheats;

struct CVar {
    const wchar_t* name;
    float value;
    bool isFloat;
    float saved = 0;
    bool have = false;
    int retries = 0;
};

CVar perfVars[] = {
    {L"r.ShadowQuality", 0, false},          {L"r.ContactShadows", 0, false},         {L"r.CapsuleShadows", 0, false},
    {L"r.DistanceFieldShadowing", 0, false}, {L"r.LightFunctionQuality", 0, false},   {L"r.Lumen.DiffuseIndirect.Allow", 0, false},
    {L"r.Lumen.Reflections.Allow", 0, false}, {L"r.SSR.Quality", 0, false},           {L"r.AmbientOcclusionLevels", 0, false},
    {L"r.DistanceFieldAO", 0, false},        {L"r.VolumetricFog", 0, false},          {L"r.VolumetricCloud", 0, false},
    {L"r.DepthOfFieldQuality", 0, false},    {L"r.MotionBlurQuality", 0, false},      {L"r.BloomQuality", 0, false},
    {L"r.LensFlareQuality", 0, false},       {L"r.SceneColorFringeQuality", 0, false}, {L"r.LightShaftQuality", 0, false},
    {L"r.Tonemapper.Quality", 0, false},     {L"r.SSS.Quality", 0, false},            {L"r.RefractionQuality", 0, false},
    {L"r.ParticleLightQuality", 0, false},   {L"fx.Niagara.QualityLevel", 0, false},  {L"r.DetailMode", 0, false},
    {L"r.MaxAnisotropy", 0, false},          {L"r.SkeletalMeshLODBias", 2, false},    {L"r.Water.SingleLayer.Reflection", 0, false},
    {L"r.Streaming.MipBias", 2, true},       {L"r.ViewDistanceScale", 0.4f, true},    {L"r.StaticMeshLODDistanceScale", 3, true},
    {L"foliage.DensityScale", 0, true},      {L"grass.DensityScale", 0, true},        {L"r.Nanite.MaxPixelsPerEdge", 4, true},
    {L"r.EmitterSpawnRateScale", 0.5f, true}, {L"r.ScreenPercentage", 50, true},
};
bool perfActive = false;
ULONGLONG lastPerfCheck = 0;
bool timeActive = false;
float timeSaved = 1.0f;

struct Teleported {
    bool valid = false;
    ue::FVector loc;
} undoSpot;

struct Saved {
    void* owner = nullptr;
    bool active = false;
    float a = 0, b = 0, c = 0, d = 0;
};
Saved speedSave, jumpSave, tempSave;

void Note(const std::string& s) {
    std::lock_guard<std::mutex> g(noteLock);
    notes.push_back(s);
}

void Enqueue(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(taskLock);
    tasks.push_back(std::move(fn));
}

void* Cdo(const char* path) { return ue::Statics(path); }

void* Gameplay() { return Cdo("/Script/Engine.Default__GameplayStatics"); }
void* SystemLib() { return Cdo("/Script/Engine.Default__KismetSystemLibrary"); }

void* PlayerThing(const char* fn) {
    if (!st.world) return nullptr;
    ue::Call c(Gameplay(), fn);
    c.Set<void*>("WorldContextObject", st.world).Set<int32_t>("PlayerIndex", 0);
    if (!c.Run()) return nullptr;
    void* r = c.Get<void*>("ReturnValue");
    return ue::Alive(r) ? r : nullptr;
}

bool WorldFlag(const char* fn) {
    ue::Call c(SystemLib(), fn);
    c.Set<void*>("WorldContextObject", st.world);
    return c.Run() && c.GetBool("ReturnValue");
}

float CallFloat(void* obj, const char* fn);
void CollectMarkers(std::vector<core::Marker>& out);
void EnforceConfig(bool allowed);
void EnforceBiomods(bool allowed);
void EnforceVehicles(bool allowed);
void UpdateEsp();
void PumpItems();
void ClearCooldowns();
void UpdateItemEsp();
void EnforceTags(bool allowed);
void EnforceAbilityCooldowns(bool on);
double Dist(const ue::FVector& a, const ue::FVector& b);
void RefreshProgress();
void SaveSurvey();
void OnWorldChanged();
void EnforceExtras(bool allowed);
bool MapScanActive();
std::string LowerAscii(std::string s);
void* lastWorld = nullptr;

void* AttributeSet(void* healthComp, const char* cls) {
    void* asc = ue::Ptr(healthComp, "AbilitySystemComponent");
    if (!asc) return nullptr;
    void* setCls = ue::FindObject(std::string("/Script/UWEAbilitySystem.") + cls);
    auto arr = ue::Read<ue::TArray<void*>>(asc, ue::FindProp(ue::ClassOf(asc), "SpawnedAttributes"));
    for (int i = 0; i < arr.num && i < 64; ++i)
        if (ue::Alive(arr.data[i]) && ue::IsA(arr.data[i], setCls)) return arr.data[i];
    return nullptr;
}

void Refresh() {
    if (!ue::Valid(st.engine)) {
        void* cls = ue::FindObject("/Script/Engine.GameEngine");
        st.engine = cls ? ue::FindFirstOf(cls) : nullptr;
    }
    void* viewport = ue::Ptr(st.engine, "GameViewport");
    st.world = ue::Ptr(viewport, "World");
    if (st.world && !ue::Alive(st.world)) st.world = nullptr;
    if (st.world != lastWorld) {
        LOG("world changed: %s", st.world ? ue::Name(st.world).c_str() : "none");
        lastWorld = st.world;
        OnWorldChanged();
    }
    st.pc = PlayerThing("GetPlayerController");
    void* controlled = PlayerThing("GetPlayerCharacter");
    void* playerCls = ue::FindObject("/Script/Subnautica2.SN2PlayerCharacter");
    void* vehicleCls = ue::FindObject("/Script/Subnautica2.SN2PossessableVehicle");
    if (controlled && playerCls && ue::IsA(controlled, playerCls)) {
        st.pawn = controlled;
        st.lastPlayer = controlled;
    } else {
        st.pawn = ue::Alive(st.lastPlayer) && st.world ? st.lastPlayer : nullptr;
    }
    st.vehicle = controlled && vehicleCls && ue::IsA(controlled, vehicleCls) ? controlled : nullptr;
    st.inWorld = st.pawn != nullptr;
    st.sub = nullptr;
    if (st.pawn) {
        ue::Call c(st.pawn, "GetSubmarinePlayerIsIn");
        if (c.Run()) {
            void* s = c.Get<void*>("ReturnValue");
            st.sub = ue::Alive(s) ? s : nullptr;
        }
    }
    st.cm = st.pc ? ue::Ptr(st.pc, "CheatManager") : nullptr;
    if (st.world) {
        bool standalone = WorldFlag("IsStandalone");
        bool server = WorldFlag("IsServer");
        st.session = standalone ? Session::Solo : server ? Session::Host : Session::Client;
    }
    st.health = st.inWorld ? ue::Ptr(st.pawn, "HealthSetComponent") : nullptr;
    st.movement = st.inWorld ? ue::Ptr(st.pawn, "MovementSetComponent") : nullptr;
    st.charMove = st.inWorld ? ue::Ptr(st.pawn, "CharacterMovement") : nullptr;
    st.healthSet = st.health ? AttributeSet(st.health, "UWEHealthAttributeSet") : nullptr;
    st.survivalSet = st.health ? AttributeSet(st.health, "UWESurvivalAttributeSet") : nullptr;
    if (st.inWorld && st.pawn != loggedPawn) {
        loggedPawn = st.pawn;
        LOG("player found: %s, cheats %s, base swim %.1f walk %.1f, health set %s", ue::Name(st.pawn).c_str(),
            st.cm ? ue::Name(ue::ClassOf(st.cm)).c_str() : "none", st.movement ? CallFloat(st.movement, "GetBaseSwimSpeed") : -1.0f,
            st.movement ? CallFloat(st.movement, "GetBaseWalkSpeed") : -1.0f, st.healthSet ? "yes" : "no");
    }

    std::vector<core::Marker> marks;
    if (st.inWorld) {
        ue::Call loc(st.pawn, "K2_GetActorLocation");
        ue::Call rot(st.pawn, "K2_GetActorRotation");
        if (loc.Run() && rot.Run()) {
            ue::FVector p = loc.Get<ue::FVector>("ReturnValue");
            ue::FRotator r = rot.Get<ue::FRotator>("ReturnValue");
            marks.push_back({core::MarkerType::Player, "You", (float)(p.x / 100), (float)(p.y / 100), (float)(p.z / 100),
                             (float)(r.yaw * 3.14159265 / 180.0)});
        }
        CollectMarkers(marks);
    }

    std::lock_guard<std::mutex> g(*modelLock);
    model->markers = std::move(marks);
    model->inWorld = st.inWorld;
    model->session = st.session;
    for (Feature& f : model->features) want[f.id] = {f.on, f.value};
}

bool HasTag(TagCheat& t) {
    if (!t.resolved) {
        t.name = ue::MakeName(t.tag);
        t.resolved = t.name.index != 0;
    }
    ue::Call c(st.cm, "HasTag");
    c.Set<ue::FName>("Tag", t.name);
    return c.Run() && c.GetBool("ReturnValue");
}

void SetTag(TagCheat& t, bool on) {
    if (HasTag(t) == on) return;
    if (t.func[0]) ue::Call(st.cm, t.func).Run();
    if (HasTag(t) == on) return;
    ue::Call c(st.cm, "ToggleTag");
    c.SetString("TagName", t.tag);
    c.Run();
    if (HasTag(t) != on && !t.warned) {
        t.warned = true;
        LOG("tag %ls did not change", t.tag);
    }
}

bool scanForce = false;

float Float(const char* id) {
    auto it = want.find(id);
    return it != want.end() ? it->second.value : 1.0f;
}

bool On(const char* id) {
    auto it = want.find(id);
    return it != want.end() && it->second.on;
}

bool ScanProtects(const char* id) {
    if (!scanForce) return false;
    for (const char* f : {"godmode", "infinite_oxygen", "unlimited_food", "unlimited_water"})
        if (std::strcmp(id, f) == 0) return true;
    return false;
}

struct Attr {
    float base, cur;
};

Attr ReadAttr(void* set, const char* name) {
    ue::Prop p = ue::FindProp(ue::ClassOf(set), name);
    if (!p) return {0, 0};
    return {ue::Read<float>((uint8_t*)set + 8, p), ue::Read<float>((uint8_t*)set + 12, p)};
}

void WriteAttr(void* set, const char* name, float v) {
    ue::Prop p = ue::FindProp(ue::ClassOf(set), name);
    if (!p) return;
    ue::Write<float>((uint8_t*)set + 8, p, v);
    ue::Write<float>((uint8_t*)set + 12, p, v);
}

void WriteAttr(void* set, const char* name, Attr a) {
    ue::Prop p = ue::FindProp(ue::ClassOf(set), name);
    if (!p) return;
    ue::Write<float>((uint8_t*)set + 8, p, a.base);
    ue::Write<float>((uint8_t*)set + 12, p, a.cur);
}

float CallFloat(void* obj, const char* fn) {
    ue::Call c(obj, fn);
    return c.Run() ? c.Get<float>("ReturnValue") : 0.0f;
}

void CallSet(void* obj, const char* fn, const char* param, float v) {
    ue::Call c(obj, fn);
    c.Set<float>(param, v);
    c.Run();
}

void EnforceSpeed(bool allowed) {
    float v = Float("player_speed");
    bool active = allowed && st.charMove && std::fabs(v - 1.0f) > 0.001f;
    if (speedSave.active && (speedSave.owner != st.charMove || !active)) {
        if (ue::Valid(st.pawn) && speedSave.owner == st.charMove) {
            ue::Call(st.pawn, "UpdateMaxSpeed").Run();
        } else if (ue::Valid(speedSave.owner)) {
            void* cls = ue::ClassOf(speedSave.owner);
            ue::Write<float>(speedSave.owner, ue::FindProp(cls, "MaxSwimSpeed"), speedSave.a);
            ue::Write<float>(speedSave.owner, ue::FindProp(cls, "MaxWalkSpeed"), speedSave.b);
        }
        speedSave.active = false;
    }
    if (!active) return;
    void* cls = ue::ClassOf(st.charMove);
    ue::Prop swimP = ue::FindProp(cls, "MaxSwimSpeed"), walkP = ue::FindProp(cls, "MaxWalkSpeed");
    if (!swimP || !walkP) return;
    if (!speedSave.active) {
        speedSave = {st.charMove, true, ue::Read<float>(st.charMove, swimP), ue::Read<float>(st.charMove, walkP)};
        LOG("speed on: swim %.1f walk %.1f", speedSave.a, speedSave.b);
    }
    float swim = st.movement ? CallFloat(st.movement, "GetMaxSwimSpeed") : speedSave.a;
    float walk = st.movement ? CallFloat(st.movement, "GetMaxWalkSpeed") : speedSave.b;
    if (swim <= 0) swim = speedSave.a;
    if (walk <= 0) walk = speedSave.b;
    ue::Write<float>(st.charMove, swimP, swim * v);
    ue::Write<float>(st.charMove, walkP, walk * v);
}

void EnforceJump(bool allowed) {
    float v = Float("jump_height");
    bool active = allowed && st.charMove && std::fabs(v - 1.0f) > 0.001f;
    ue::Prop p = st.charMove ? ue::FindProp(ue::ClassOf(st.charMove), "JumpZVelocity") : ue::Prop{};
    if (jumpSave.active && (jumpSave.owner != st.charMove || !active)) {
        if (ue::Valid(jumpSave.owner)) ue::Write<float>(jumpSave.owner, ue::FindProp(ue::ClassOf(jumpSave.owner), "JumpZVelocity"), jumpSave.a);
        jumpSave.active = false;
    }
    if (!active || !p) return;
    if (!jumpSave.active) jumpSave = {st.charMove, true, ue::Read<float>(st.charMove, p)};
    ue::Write<float>(st.charMove, p, jumpSave.a * std::sqrt(v));
}

void EnforceAttrs(bool allowed) {
    void* set = st.healthSet;
    bool temp = allowed && set && On("stable_temperature");
    if (tempSave.active && (tempSave.owner != set || !temp)) {
        if (ue::Valid(tempSave.owner)) {
            WriteAttr(tempSave.owner, "DamageMultiplierCold", Attr{tempSave.a, tempSave.b});
            WriteAttr(tempSave.owner, "DamageMultiplierHeat", Attr{tempSave.c, tempSave.d});
        }
        tempSave.active = false;
    }
    if (temp) {
        if (!tempSave.active) {
            Attr c = ReadAttr(set, "DamageMultiplierCold"), h = ReadAttr(set, "DamageMultiplierHeat");
            tempSave = {set, true, c.base, c.cur, h.base, h.cur};
        }
        WriteAttr(set, "DamageMultiplierCold", 0.0f);
        WriteAttr(set, "DamageMultiplierHeat", 0.0f);
    }
}

int CVarInt(const wchar_t* name) {
    ue::Call c(SystemLib(), "GetConsoleVariableIntValue");
    c.SetString("VariableName", name);
    return c.Run() ? c.Get<int32_t>("ReturnValue") : 0;
}

void Console(const std::wstring& cmd) {
    if (!st.world) return;
    ue::Call c(SystemLib(), "ExecuteConsoleCommand");
    c.Set<void*>("WorldContextObject", st.world).SetString("Command", cmd).Set<void*>("SpecificPlayer", st.pc);
    c.Run();
}

float CVarValue(const CVar& v) {
    if (!v.isFloat) return (float)CVarInt(v.name);
    ue::Call c(SystemLib(), "GetConsoleVariableFloatValue");
    c.SetString("VariableName", v.name);
    return c.Run() ? c.Get<float>("ReturnValue") : 0.0f;
}

std::wstring CVarText(const CVar& v, float value) {
    if (!v.isFloat) return std::wstring(v.name) + L" " + std::to_wstring((int)std::lround(value));
    wchar_t buf[64];
    swprintf_s(buf, L"%.3f", value);
    return std::wstring(v.name) + L" " + buf;
}

void EnforcePerf(bool allowed) {
    if (!st.world) return;
    bool on = allowed && On("performance_mode");
    ULONGLONG now = GetTickCount64();
    if (on && perfActive) {
        if (now - lastPerfCheck < 3000) return;
        lastPerfCheck = now;
        for (CVar& v : perfVars) {
            if (!v.have || v.retries >= 3) continue;
            if (std::fabs(CVarValue(v) - v.value) < 0.01f) continue;
            ++v.retries;
            Console(CVarText(v, v.value));
        }
        return;
    }
    if (on == perfActive) return;
    if (on) {
        for (CVar& v : perfVars) {
            v.saved = CVarValue(v);
            v.have = true;
            v.retries = 0;
        }
        for (CVar& v : perfVars) Console(CVarText(v, v.value));
        lastPerfCheck = now;
        LOG("performance mode on");
    } else {
        for (int i = (int)(sizeof(perfVars) / sizeof(perfVars[0])) - 1; i >= 0; --i) {
            CVar& v = perfVars[i];
            if (v.have) Console(CVarText(v, v.saved));
            v.have = false;
        }
        LOG("performance mode off, settings restored");
        Note("Graphics are back to your settings. Restart the game before you change graphics options");
    }
    perfActive = on;
}

void EnforceTime(bool allowed) {
    if (!st.world) return;
    float v = Float("game_speed");
    bool on = allowed && std::fabs(v - 1.0f) > 0.001f;
    if (!on && !timeActive) return;
    ue::Call get(Gameplay(), "GetGlobalTimeDilation");
    get.Set<void*>("WorldContextObject", st.world);
    float cur = get.Run() ? get.Get<float>("ReturnValue") : 1.0f;
    if (on && !timeActive) {
        timeSaved = cur > 0 ? cur : 1.0f;
        timeActive = true;
    }
    float target = on ? v : timeSaved;
    if (std::fabs(cur - target) > 0.001f) {
        ue::Call set(Gameplay(), "SetGlobalTimeDilation");
        set.Set<void*>("WorldContextObject", st.world).Set<float>("TimeDilation", target);
        set.Run();
    }
    if (!on) timeActive = false;
}

void Enforce(bool allOff) {
    bool allowed = !allOff && st.session != Session::Client;
    if (st.inWorld && st.cm) {
        for (TagCheat& t : tagCheats) {
            bool on = allowed && On(t.id);
            if (on) {
                SetTag(t, true);
                t.ours = true;
            } else if (t.ours) {
                SetTag(t, false);
                t.ours = false;
            }
        }
    }
    EnforceSpeed(allowed && st.inWorld);
    EnforceJump(allowed && st.inWorld);
    EnforceAttrs(allowed && st.inWorld);
    EnforcePerf(allowed);
    EnforceTime(allowed && st.inWorld);
    EnforceConfig(allowed);
    EnforceTags(allowed && st.inWorld);
    EnforceAbilityCooldowns(allowed && st.inWorld && On("zero_cooldowns"));
    EnforceBiomods(allowed && st.inWorld);
    EnforceVehicles(allowed && st.inWorld);
    EnforceExtras(allowed);
}

struct CfgField {
    const char* feature;
    const char* group;
    const char* groupStruct;
    const char* field;
    bool isFloat;
    float onValue;
    const wchar_t* tag;
    bool active = false;
    float orig = 0;
    bool byScan = false;
};

CfgField cfgFields[] = {
    {"instant_craft", "CraftingSettings", "UWECraftingSettings", "QuickProccessing", false, 1.0f, nullptr},
    {"free_craft", "CraftingSettings", "UWECraftingSettings", "bEnableNoCost", false, 1.0f, L"Cheat.NoCost"},
    {"unlimited_biomod_energy", "BiomodSettings", "UWEBiomodSettings", "bEnableNoCost", false, 1.0f, nullptr},
    {"godmode", "SurvivalSettings", "UWESurvivalSettings", "bInvulnerable", false, 1.0f, L"Cheat.GodMode"},
    {"infinite_oxygen", "SurvivalSettings", "UWESurvivalSettings", "bBlockOxygenConsumption", false, 1.0f, L"Cheat.NoSuffocate"},
    {"unlimited_food", "SurvivalSettings", "UWESurvivalSettings", "bBlockFoodConsumption", false, 1.0f, L"Cheat.NoStarve"},
    {"unlimited_water", "SurvivalSettings", "UWESurvivalSettings", "bBlockWaterConsumption", false, 1.0f, L"Cheat.NoDehydrate"},
    {"unlimited_battery", "ToolSettings", "UWEToolSettings", "bEnableUnlimitedBattery", false, 1.0f, L"Cheat.UnlimitedBattery"},
    {"unlimited_power", "PowerConfigSettings", "UWEPowerConfigSettings", "bEnableUnlimitedPower", false, 1.0f, L"Cheat.UnlimitedPower"},
    {"unlock_blueprints", "CraftingSettings", "UWECraftingSettings", "bUnlockAllRecipes", false, 1.0f, nullptr},
    {"unlock_databank", "StorySettings", "UWEStorySettings", "bUnlockAllDatabankEntries", false, 1.0f, nullptr},
    {"unlock_dna", "BiomodSettings", "UWEBiomodSettings", "bUnlockAllBioAbilities", false, 1.0f, nullptr},
    {"unlock_dna", "BiomodSettings", "UWEBiomodSettings", "bUnlockAllBiomodSlots", false, 1.0f, nullptr},
};

void RemoveCheatTag(const wchar_t* tag) {
    if (!st.pawn || !tag) return;
    ue::FName n = ue::MakeName(tag);
    if (!n.index) return;
    static ue::FName one[1];
    one[0] = n;
    uint8_t container[0x40] = {};
    *(ue::TArray<ue::FName>*)container = {one, 1, 1};
    void* lib = Cdo("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary");
    for (int i = 0; i < 4; ++i) {
        ue::Call c(lib, "RemoveLooseGameplayTags");
        int size = std::min(c.ParamSize("GameplayTags"), (int)sizeof(container));
        c.Set<void*>("Actor", st.pawn).SetRaw("GameplayTags", container, size).SetBool("bShouldReplicate", true);
        c.Run();
    }
}
void NotifySettingsListeners(void* comp, const uint8_t* settings, int size) {
    ue::Prop d = ue::FindProp(ue::ClassOf(comp), "OnSettingsChanged");
    if (!d) return;
    auto list = ue::Read<ue::TArray<uint8_t>>(comp, d);
    static bool logged = false;
    int called = 0;
    for (int i = 0; i < list.num && i < 256 && list.data; ++i) {
        const uint8_t* e = list.data + (size_t)i * 16;
        int32_t index = *(const int32_t*)e, serial = *(const int32_t*)(e + 4);
        ue::FName fname = *(const ue::FName*)(e + 8);
        void* obj = ue::WeakGet(index, serial);
        if (!obj) continue;
        std::string cls = ue::Name(ue::ClassOf(obj));
        std::string fn = ue::NameToString(fname);
        if (!logged) LOG("settings listener: %s.%s", cls.c_str(), fn.c_str());
        if (cls.find("BatterySlot") != std::string::npos || cls.find("Dialogue") != std::string::npos) continue;
        void* func = ue::FindFunction(ue::ClassOf(obj), fn.c_str());
        if (!func) continue;
        ue::Call c(obj, func, 1);
        c.SetFirstRaw(settings, size);
        if (c.Run()) ++called;
    }
    logged = true;
    if (called == 0) {
        void* applier = st.pawn ? ue::Ptr(st.pawn, "GameConfigSettingsApplier") : nullptr;
        if (applier) {
            ue::Call a(applier, "OnSettingsChanged");
            a.SetRaw("ChangedSettings", settings, size);
            a.Run();
        }
    }
}

struct LooseTag {
    const char* feature;
    const wchar_t* tag;
    bool alsoState = false;
    void* appliedTo = nullptr;
    void* appliedState = nullptr;
};

LooseTag looseTags[] = {
    {"godmode", L"Cheat.GodMode"},           {"infinite_oxygen", L"Cheat.NoSuffocate"}, {"unlimited_food", L"Cheat.NoStarve"},
    {"unlimited_water", L"Cheat.NoDehydrate"}, {"unlimited_battery", L"Cheat.UnlimitedBattery"},
    {"unlimited_power", L"Cheat.UnlimitedPower"}, {"free_craft", L"Cheat.NoCost", true},
};

void ChangeTag(void* actor, const wchar_t* tag, bool add, int times) {
    if (!ue::Valid(actor) || !tag) return;
    ue::FName n = ue::MakeName(tag);
    if (!n.index) return;
    static ue::FName one[1];
    one[0] = n;
    uint8_t container[0x40] = {};
    *(ue::TArray<ue::FName>*)container = {one, 1, 1};
    void* lib = Cdo("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary");
    for (int i = 0; i < times; ++i) {
        ue::Call c(lib, add ? "AddLooseGameplayTags" : "RemoveLooseGameplayTags");
        int sz = std::min(c.ParamSize("GameplayTags"), (int)sizeof(container));
        c.Set<void*>("Actor", actor).SetRaw("GameplayTags", container, sz).SetBool("bShouldReplicate", true);
        c.Run();
    }
}

void EnforceTags(bool allowed) {
    void* ps = st.pc ? ue::Ptr(st.pc, "PlayerState") : nullptr;
    for (LooseTag& t : looseTags) {
        bool on = allowed && st.pawn && On(t.feature);
        if (on && t.appliedTo != st.pawn) {
            if (ue::Alive(t.appliedTo)) ChangeTag(t.appliedTo, t.tag, false, 2);
            ChangeTag(st.pawn, t.tag, true, 1);
            t.appliedTo = st.pawn;
        } else if (!on && t.appliedTo) {
            ChangeTag(t.appliedTo, t.tag, false, 3);
            t.appliedTo = nullptr;
        }
        bool onState = on && t.alsoState && ps;
        if (onState && t.appliedState != ps) {
            if (ue::Alive(t.appliedState)) ChangeTag(t.appliedState, t.tag, false, 2);
            ChangeTag(ps, t.tag, true, 1);
            t.appliedState = ps;
        } else if (!onState && t.appliedState) {
            ChangeTag(t.appliedState, t.tag, false, 3);
            t.appliedState = nullptr;
        }
    }
}

struct AbilitySave {
    float cooldown;
    void* effect;
};
std::unordered_map<void*, AbilitySave> abilitySaves;
ULONGLONG lastAbilityPass = 0;

void EnforceAbilityCooldowns(bool on) {
    if (!on) {
        for (auto& [obj, s] : abilitySaves) {
            if (!ue::Alive(obj)) continue;
            void* cls = ue::ClassOf(obj);
            ue::Prop gc = ue::FindProp(cls, "GenericCooldown"), ge = ue::FindProp(cls, "CooldownGameplayEffectClass");
            if (gc) ue::Write<float>(obj, gc, s.cooldown);
            if (ge) ue::Write<void*>(obj, ge, s.effect);
        }
        abilitySaves.clear();
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (now - lastAbilityPass < 500 || !st.pawn) return;
    lastAbilityPass = now;
    ue::Call ga(st.pawn, "GetASC");
    void* asc = ga.Run() ? ga.Get<void*>("ReturnValue") : nullptr;
    if (!ue::Valid(asc)) return;
    void* contStruct = ue::FindClassByName("GameplayAbilitySpecContainer");
    void* specStruct = ue::FindClassByName("GameplayAbilitySpec");
    ue::Prop cont = ue::FindProp(ue::ClassOf(asc), "ActivatableAbilities");
    if (!contStruct || !specStruct || !cont) return;
    int itemsOff = ue::FindProp(contStruct, "Items").offset;
    int specSize = ue::StructSize(specStruct);
    int abilityOff = ue::FindProp(specStruct, "Ability").offset;
    int repOff = ue::FindProp(specStruct, "ReplicatedInstances").offset;
    int nonRepOff = ue::FindProp(specStruct, "NonReplicatedInstances").offset;
    if (itemsOff < 0 || specSize <= 0 || abilityOff < 0) return;
    auto items = *(ue::TArray<uint8_t>*)((uint8_t*)asc + cont.offset + itemsOff);
    std::vector<void*> abilities;
    for (int i = 0; i < items.num && i < 512 && items.data; ++i) {
        uint8_t* spec = items.data + (size_t)i * specSize;
        abilities.push_back(*(void**)(spec + abilityOff));
        for (int off : {repOff, nonRepOff}) {
            if (off < 0) continue;
            auto inst = *(ue::TArray<void*>*)(spec + off);
            for (int k = 0; k < inst.num && k < 16 && inst.data; ++k) abilities.push_back(inst.data[k]);
        }
    }
    int changed = 0;
    for (void* a : abilities) {
        if (!ue::Valid(a)) continue;
        void* cls = ue::ClassOf(a);
        ue::Prop gc = ue::FindProp(cls, "GenericCooldown"), ge = ue::FindProp(cls, "CooldownGameplayEffectClass");
        if (!gc && !ge) continue;
        if (!abilitySaves.count(a)) {
            abilitySaves[a] = {gc ? ue::Read<float>(a, gc) : 0.0f, ge ? ue::Read<void*>(a, ge) : nullptr};
            ++changed;
        }
        if (gc) ue::Write<float>(a, gc, 0.0f);
        if (ge) ue::Write<void*>(a, ge, nullptr);
    }
    static bool logged = false;
    if (!logged && changed) {
        logged = true;
        LOG("cooldowns cleared on %d abilities", changed);
    }
}
void* ConfigComponent() {
    if (!st.world) return nullptr;
    ue::Call c(Cdo("/Script/UWEGameConfiguration.Default__UWEGameConfigurationStatics"), "GetConfigComponent");
    c.Set<void*>("WorldContextObject", st.world);
    if (!c.Run()) return nullptr;
    void* r = c.Get<void*>("ReturnValue");
    return ue::Valid(r) ? r : nullptr;
}

bool IsUnlockFeature(const char* id) { return std::strncmp(id, "unlock_", 7) == 0; }
void SnapshotUnlock(const std::string& id);
void RevertUnlock(const std::string& id);

void EnforceConfig(bool allowed) {
    if (!st.inWorld && !shuttingDown) return;
    void* comp = ConfigComponent();
    if (!comp) return;
    ue::Prop gs = ue::FindProp(ue::ClassOf(comp), "GameSettings");
    void* settingsStruct = ue::FindObject("/Script/UWEGameConfiguration.UWEGameConfigSettings");
    if (!gs || gs.size <= 0 || gs.size > 256 || !settingsStruct) return;
    uint8_t buf[256];
    std::memcpy(buf, (uint8_t*)comp + gs.offset, (size_t)gs.size);
    bool changed = false;
    std::set<std::string> turnedOn, turnedOff;
    for (CfgField& f : cfgFields) {
        void* groupStruct = ue::FindObject(std::string("/Script/UWEGameConfiguration.") + f.groupStruct);
        ue::Prop g = ue::FindProp(settingsStruct, f.group);
        ue::Prop p = groupStruct ? ue::FindProp(groupStruct, f.field) : ue::Prop{};
        if (!g || !p) continue;
        uint8_t* at = buf + g.offset + p.offset;
        float cur = f.isFloat ? *(float*)at : (*at ? 1.0f : 0.0f);
        bool userOn = allowed && On(f.feature);
        bool on = userOn || (allowed && ScanProtects(f.feature));
        float target;
        if (on) {
            if (!f.active) {
                f.orig = cur;
                f.active = true;
                f.byScan = !userOn;
                if (IsUnlockFeature(f.feature)) turnedOn.insert(f.feature);
            } else if (userOn) {
                f.byScan = false;
            }
            target = f.onValue;
        } else if (f.active) {
            target = f.isFloat || f.byScan ? f.orig : 0.0f;
            f.active = false;
            if (!f.byScan || f.orig < 0.5f) RemoveCheatTag(f.tag);
            f.byScan = false;
            if (IsUnlockFeature(f.feature)) turnedOff.insert(f.feature);
        } else {
            continue;
        }
        if (std::fabs(cur - target) < 0.0001f) continue;
        if (f.isFloat) *(float*)at = target;
        else *at = target > 0.5f ? 1 : 0;
        changed = true;
    }
    for (const std::string& id : turnedOn) SnapshotUnlock(id);
    if (changed) {
        std::memcpy((uint8_t*)comp + gs.offset, buf, (size_t)gs.size);
        NotifySettingsListeners(comp, buf, gs.size);
    }
    for (const std::string& id : turnedOff) RevertUnlock(id);
}

Saved scanSave, sampleSave, vSpeedSave, subSpeedSave;

void* BiomodSettingsObject() {
    void* cls = ue::FindObject("/Script/UWEBiomods.UWEBiomodsModuleSettings");
    if (!cls) return nullptr;
    void* inst = ue::FindFirstOf(cls);
    return inst ? inst : ue::FindObject("/Script/UWEBiomods.Default__UWEBiomodsModuleSettings");
}

void* sampleSettings = nullptr;

void EnforceBiomods(bool allowed) {
    if (allowed && On("zero_cooldowns") && st.pawn) {
        ue::Call asc(st.pawn, "GetASC");
        void* ge = ue::FindClassByName("GE_BiomodCooldown_C");
        if (asc.Run() && ge) {
            void* a = asc.Get<void*>("ReturnValue");
            ue::Call rm(a, "RemoveActiveGameplayEffectBySourceEffect");
            rm.Set<void*>("GameplayEffect", ge).Set<void*>("InstigatorAbilitySystemComponent", nullptr).Set<int32_t>("StacksToRemove", -1);
            rm.Run();
        }
    }
    if (allowed && On("unlimited_biomod_energy") && st.healthSet) {
        Attr mx = ReadAttr(st.healthSet, "MaxBiomodStability");
        if (mx.cur > 0) WriteAttr(st.healthSet, "BiomodStability", mx.cur);
    }

    void* mech = st.pawn ? ue::Ptr(st.pawn, "MechanicalSetComponent") : nullptr;
    bool scan = allowed && mech && (On("instant_scan") || On("instant_biosampler"));
    if (scanSave.active && (scanSave.owner != mech || !scan)) {
        if (ue::Valid(scanSave.owner)) CallSet(scanSave.owner, "SetScanSpeedScalar", "Val", scanSave.a);
        scanSave.active = false;
    }
    if (scan) {
        if (!scanSave.active) scanSave = {mech, true, CallFloat(mech, "GetScanSpeedScalar")};
        if (scanSave.a <= 0) scanSave.a = 1.0f;
        if (CallFloat(mech, "GetScanSpeedScalar") < scanSave.a * 50.0f - 0.01f) CallSet(mech, "SetScanSpeedScalar", "Val", scanSave.a * 50.0f);
    }

    bool sample = allowed && On("instant_biosampler");
    if (!ue::Valid(sampleSettings)) sampleSettings = BiomodSettingsObject();
    ue::Prop len = sampleSettings ? ue::FindProp(ue::ClassOf(sampleSettings), "PerActorSampleCooldownLength") : ue::Prop{};
    if (len) {
        if (sample && !sampleSave.active) sampleSave = {sampleSettings, true, (float)ue::Read<double>(sampleSettings, len)};
        if (sample) ue::Write<double>(sampleSettings, len, 0.0);
        if (!sample && sampleSave.active) {
            ue::Write<double>(sampleSettings, len, (double)sampleSave.a);
            sampleSave.active = false;
        }
    }
}

ULONGLONG lastRepair = 0;

void EnforceVehicles(bool allowed) {
    ULONGLONG now = GetTickCount64();
    if (allowed && On("vehicle_hull") && now - lastRepair > 1000) {
        lastRepair = now;
        if (st.vehicle) {
            void* h = ue::Ptr(st.vehicle, "HealthSetComponent");
            if (h && CallFloat(h, "GetHealth") < CallFloat(h, "GetMaxHealth")) ue::Call(h, "RestoreHealth").Run();
        }
        if (st.sub) {
            void* h = ue::Ptr(st.sub, "HealthSetComponent");
            if (!h || CallFloat(h, "GetHealth") < CallFloat(h, "GetMaxHealth")) {
                ue::Call(st.sub, "RepairAll").Run();
                ue::Call(st.sub, "DefloodAll").Run();
            }
        }
    }

    float v = Float("vehicle_speed");
    bool fast = allowed && std::fabs(v - 1.0f) > 0.001f;
    void* vm = st.vehicle ? ue::Ptr(st.vehicle, "CharacterMovement") : nullptr;
    if (vSpeedSave.active && (vSpeedSave.owner != vm || !fast)) {
        if (ue::Valid(vSpeedSave.owner)) {
            void* cls = ue::ClassOf(vSpeedSave.owner);
            ue::Write<float>(vSpeedSave.owner, ue::FindProp(cls, "MaxSwimSpeed"), vSpeedSave.a);
            ue::Write<float>(vSpeedSave.owner, ue::FindProp(cls, "MaxWalkSpeed"), vSpeedSave.b);
        }
        vSpeedSave.active = false;
    }
    if (fast && vm) {
        void* cls = ue::ClassOf(vm);
        ue::Prop sw = ue::FindProp(cls, "MaxSwimSpeed"), wk = ue::FindProp(cls, "MaxWalkSpeed");
        if (!vSpeedSave.active) vSpeedSave = {vm, true, ue::Read<float>(vm, sw), ue::Read<float>(vm, wk)};
        ue::Write<float>(vm, sw, vSpeedSave.a * v);
        ue::Write<float>(vm, wk, vSpeedSave.b * v);
    }
    void* sm = st.sub ? ue::Ptr(st.sub, "SubmarineMovement") : nullptr;
    if (subSpeedSave.active && (subSpeedSave.owner != sm || !fast)) {
        if (ue::Valid(subSpeedSave.owner)) {
            void* cls = ue::ClassOf(subSpeedSave.owner);
            ue::Write<float>(subSpeedSave.owner, ue::FindProp(cls, "MaxSpeed"), subSpeedSave.a);
            ue::Write<float>(subSpeedSave.owner, ue::FindProp(cls, "ForwardAcceleration"), subSpeedSave.b);
        }
        subSpeedSave.active = false;
    }
    if (fast && sm) {
        void* cls = ue::ClassOf(sm);
        ue::Prop ms = ue::FindProp(cls, "MaxSpeed"), fa = ue::FindProp(cls, "ForwardAcceleration");
        if (!subSpeedSave.active) subSpeedSave = {sm, true, ue::Read<float>(sm, ms), ue::Read<float>(sm, fa)};
        ue::Write<float>(sm, ms, subSpeedSave.a * v);
        ue::Write<float>(sm, fa, subSpeedSave.b * v);
    }
}

enum TrackKind { KVehicle, KBase, KSignal, KData, KCreature, KItem, KLoc, KNone };

struct Tracked {
    void* actor;
    int kind;
    int sub;
    std::string name;
};

std::vector<Tracked> tracked;
std::unordered_map<void*, std::pair<int, int>> classMemo;
std::unordered_map<void*, std::string> nameMemo;
ULONGLONG lastScan = 0;

std::string Pretty(std::string n) {
    if (n.rfind("BP_", 0) == 0) n = n.substr(3);
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_C") == 0) n.resize(n.size() - 2);
    while (!n.empty() && (std::isdigit((unsigned char)n.back()) || n.back() == '_')) n.pop_back();
    for (const char* p : {"SN2", "UWE"})
        if (n.rfind(p, 0) == 0) n = n.substr(std::strlen(p));
    std::string out;
    for (size_t i = 0; i < n.size(); ++i) {
        char c = n[i];
        if (c == '_') c = ' ';
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)n[i - 1])) out.push_back(' ');
        out.push_back(c);
    }
    return out;
}

std::unordered_map<void*, std::pair<float, ULONGLONG>> healthMemo;
struct Bounds {
    ue::FVector offset, extent;
    ULONGLONG at = 0;
};
std::unordered_map<void*, Bounds> boundsMemo;

std::pair<int, int> Classify(void* cls) {
    auto it = classMemo.find(cls);
    if (it != classMemo.end()) return it->second;
    static const std::pair<const char*, int> targets[] = {
        {"SN2PossessableVehicle", KVehicle}, {"UWESubmarine", KVehicle},      {"UWESculpturalBaseActor", KBase},
        {"BP_NarrativeSignal_C", KSignal},   {"BP_Beacon_C", KSignal},         {"BP_SupplyCrate_C", KData},
        {"BP_ComputerTextInterface_Terminal_C", KData}, {"UWEAIPawn", KCreature}, {"UWEAICharacter", KCreature},
        {"BP_BOID_FishSchool_C", KCreature}, {"SN2PickupItem", KItem}, {"BP_Blackbox_Clickable_01a_C", KData},
        {"BP_PlayerDied_Blackbox_Proto_C", KData}, {"BP_Nrtv_QuartzChip_C", KData}, {"BP_WorldSupplyLocker_C", KData},
        {"BP_Lifepod_C", KSignal}, {"BP_StaticLifepod_C", KSignal}, {"SN2PlayerStart", KLoc},
    };
    int kind = KNone, sub = 0;
    bool leviathan = false, large = false, school = false, submarine = false;
    for (void* c = cls; c; c = ue::SuperOf(c)) {
        std::string n = ue::Name(c);
        if (n.find("Leviathan") != std::string::npos) leviathan = true;
        if (n == "UWEAILargeFish") large = true;
        if (n == "BP_BOID_FishSchool_C") school = true;
        if (n == "UWESubmarine") submarine = true;
        for (const auto& t : targets)
            if (kind == KNone && n == t.first) kind = t.second;
    }
    if (kind == KCreature) sub = leviathan ? 2 : (large ? 1 : 0);
    if (kind == KCreature && school) sub = 3;
    if (kind == KVehicle && submarine) sub = 1;
    if (kind == KData) {
        std::string n = ue::Name(cls);
        if (n.find("SupplyCrate") != std::string::npos || n.find("SupplyLocker") != std::string::npos) sub = 1;
    }
    classMemo[cls] = {kind, sub};
    return {kind, sub};
}

bool MakeTracked(void* obj, Tracked& out) {
    if (!obj || ue::IsDefault(obj)) return false;
    void* cls = ue::ClassOf(obj);
    if (!cls) return false;
    auto [kind, sub] = Classify(cls);
    if (kind == KNone || obj == st.pawn || !ue::Alive(obj)) return false;
    auto nm = nameMemo.find(cls);
    if (nm == nameMemo.end()) {
        std::string n = kind == KBase ? std::string("Base") : sub == 3 && kind == KCreature ? std::string("Fish school") : Pretty(ue::Name(cls));
        static const std::pair<const char*, const char*> renames[] = {
            {"Blackbox", "Black box"},     {"QuartzChip", "Quartz chip"},   {"ComputerTextInterface", "Terminal"},
            {"SupplyCrate", "Supply crate"}, {"SupplyLocker", "Supply locker"}, {"Lifepod", "Lifepod"},
            {"NarrativeSignal", "Signal"}, {"Beacon", "Beacon"}};
        std::string cn = ue::Name(cls);
        for (const auto& r : renames)
            if (cn.find(r.first) != std::string::npos) {
                n = r.second;
                break;
            }
        nm = nameMemo.emplace(cls, n).first;
    }
    out = {obj, kind, sub, nm->second};
    return true;
}

void CleanMemos() {
    if (healthMemo.size() > 4000) healthMemo.clear();
    if (boundsMemo.size() > 4000) boundsMemo.clear();
}

void ScanActors() {
    std::vector<Tracked> found;
    int32_t count = ue::ObjectCount();
    Tracked t;
    for (int32_t i = 0; i < count; ++i)
        if (MakeTracked(ue::ObjectAt(i), t)) found.push_back(t);
    tracked.swap(found);
    CleanMemos();
}

std::vector<Tracked> scanBuild;
int32_t scanCursor = -1;

void ScanChunk(int budget) {
    if (scanCursor < 0) {
        scanBuild.clear();
        scanCursor = 0;
    }
    int32_t count = ue::ObjectCount();
    int32_t end = std::min(count, scanCursor + budget);
    Tracked t;
    for (int32_t i = scanCursor; i < end; ++i)
        if (MakeTracked(ue::ObjectAt(i), t)) scanBuild.push_back(t);
    scanCursor = end;
    if (scanCursor >= count) {
        tracked.swap(scanBuild);
        scanCursor = -1;
        CleanMemos();
    }
}

bool ActorPos(void* a, ue::FVector& out) {
    if (!ue::Valid(a)) return false;
    ue::Call c(a, "K2_GetActorLocation");
    if (!c.Run()) return false;
    out = c.Get<ue::FVector>("ReturnValue");
    return true;
}

int rootOff = -2, relOff = -1, parentOff = -1;

bool ActorPosFast(void* a, ue::FVector& out) {
    if (!ue::Valid(a)) return false;
    if (rootOff == -2) {
        void* actorCls = ue::FindObject("/Script/Engine.Actor");
        void* sceneCls = ue::FindObject("/Script/Engine.SceneComponent");
        rootOff = actorCls ? ue::FindProp(actorCls, "RootComponent").offset : -1;
        relOff = sceneCls ? ue::FindProp(sceneCls, "RelativeLocation").offset : -1;
        parentOff = sceneCls ? ue::FindProp(sceneCls, "AttachParent").offset : -1;
    }
    if (rootOff < 0 || relOff < 0 || parentOff < 0) return ActorPos(a, out);
    void* root = *(void**)((uint8_t*)a + rootOff);
    if (!ue::Valid(root)) return false;
    if (*(void**)((uint8_t*)root + parentOff)) return ActorPos(a, out);
    out = *(ue::FVector*)((uint8_t*)root + relOff);
    return true;
}

bool MoveActor(void* a, const ue::FVector& to) {
    if (!ue::Valid(a)) return false;
    ue::Call c(a, "K2_SetActorLocation");
    c.Set<ue::FVector>("NewLocation", to).SetBool("bSweep", false).SetBool("bTeleport", true);
    return c.Run();
}

std::atomic<bool> menuOpenFlag{false}, mapVisibleFlag{false}, legitVisibleFlag{false};

bool MenuOpen() { return menuOpenFlag.load(); }
bool MapVisible() { return mapVisibleFlag.load(); }
bool LegitVisible() { return legitVisibleFlag.load(); }

uint64_t lastScanFrame = 0;

void TrackActors() {
    bool need = MenuOpen() || On("esp") || On("item_esp") || scanForce;
    if (!need || !st.inWorld) return;
    uint64_t frame = overlay::FrameCount();
    if (frame == lastScanFrame) return;
    lastScanFrame = frame;
    if (scanCursor >= 0) {
        ScanChunk(12000);
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (now - lastScan < 2000 && !tracked.empty()) return;
    lastScan = now;
    ScanChunk(12000);
}
std::string TextOf(void* obj, const char* prop);
std::unordered_map<void*, std::string> signalNames;

std::string SignalName(void* actor) {
    auto it = signalNames.find(actor);
    if (it != signalNames.end()) return it->second;
    std::string n = TextOf(actor, "SignalText");
    if (n.empty()) n = "Signal";
    if (signalNames.size() > 2000) signalNames.clear();
    signalNames[actor] = n;
    return n;
}

void CollectMarkers(std::vector<core::Marker>& out) {
    if (!MenuOpen()) return;
    static const core::MarkerType types[] = {core::MarkerType::Vehicle, core::MarkerType::Base, core::MarkerType::Wreck,
                                             core::MarkerType::DataBox};
    std::vector<ue::FVector> pods;
    for (const Tracked& t : tracked) {
        if (t.kind != KSignal || t.name != "Lifepod") continue;
        ue::FVector p;
        if (ActorPosFast(t.actor, p)) pods.push_back(p);
    }
    for (const Tracked& t : tracked) {
        if (t.kind > KData) continue;
        ue::FVector p;
        if (!ActorPosFast(t.actor, p)) continue;
        if (std::fabs(p.x) < 1 && std::fabs(p.y) < 1 && std::fabs(p.z) < 1) continue;
        std::string name = t.name;
        if (t.kind == KSignal && name == "Signal") {
            name = SignalName(t.actor);
            for (const ue::FVector& pod : pods)
                if (Dist(p, pod) < 60.0) name = "Lifepod";
        }
        out.push_back({types[t.kind], name, (float)(p.x / 100), (float)(p.y / 100), (float)(p.z / 100), 0});
    }
}

struct Creature {
    Tracked t;
    ue::FVector p;
    double d;
};

std::vector<Creature> nearby;
ULONGLONG lastNearby = 0, lastEsp = 0, lastCam = 0;
void* camManager = nullptr;
ULONGLONG camManagerAt = 0;
ue::FVector eye;

ue::FVector Eye() { return eye; }

double Dist(const ue::FVector& a, const ue::FVector& b) {
    return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)) / 100.0;
}

void UpdateCamera() {
    ULONGLONG now = GetTickCount64();
    if (now - lastCam < ((On("esp") || On("item_esp")) ? 5u : 100u)) return;
    lastCam = now;
    if (!st.world || !st.inWorld) return;
    if (!ue::Valid(camManager) || now - camManagerAt > 1000) {
        camManagerAt = now;
        ue::Call cam(Gameplay(), "GetPlayerCameraManager");
        cam.Set<void*>("WorldContextObject", st.world).Set<int32_t>("PlayerIndex", 0);
        camManager = cam.Run() ? cam.Get<void*>("ReturnValue") : nullptr;
    }
    core::CameraView v;
    if (ue::Valid(camManager)) {
        ue::Call cl(camManager, "GetCameraLocation");
        ue::Call cr(camManager, "GetCameraRotation");
        ue::Call cf(camManager, "GetFOVAngle");
        if (cl.Run() && cr.Run() && cf.Run()) {
            ue::FVector p = cl.Get<ue::FVector>("ReturnValue");
            ue::FRotator r = cr.Get<ue::FRotator>("ReturnValue");
            v.x = p.x;
            v.y = p.y;
            v.z = p.z;
            v.pitch = r.pitch;
            v.yaw = r.yaw;
            v.roll = r.roll;
            v.fov = cf.Get<float>("ReturnValue");
            v.valid = true;
            eye = p;
            static double focal = 0;
            static int vw = 0, vh = 0;
            static ULONGLONG lastCal = 0;
            if (now - lastCal > 500 && st.pc) {
                lastCal = now;
                ue::Call vs(st.pc, "GetViewportSize");
                if (vs.Run()) {
                    vw = vs.Get<int32_t>("SizeX");
                    vh = vs.Get<int32_t>("SizeY");
                }
                double P = r.pitch * 3.14159265358979 / 180, Y = r.yaw * 3.14159265358979 / 180, R = r.roll * 3.14159265358979 / 180;
                double SP = std::sin(P), CP = std::cos(P), SY = std::sin(Y), CY = std::cos(Y), SR = std::sin(R), CR = std::cos(R);
                ue::FVector fwd{CP * CY, CP * SY, SP}, right{SR * SP * CY - CR * SY, SR * SP * SY + CR * CY, -SR * CP};
                ue::FVector t{p.x + fwd.x * 1000 + right.x * 300, p.y + fwd.y * 1000 + right.y * 300, p.z + fwd.z * 1000 + right.z * 300};
                ue::Call pr(st.pc, "ProjectWorldLocationToScreen");
                pr.Set<ue::FVector>("WorldLocation", t).SetBool("bPlayerViewportRelative", false);
                if (vw > 0 && pr.Run() && pr.GetBool("ReturnValue")) {
                    double s[2] = {0, 0};
                    pr.GetRaw("ScreenLocation", s, sizeof(s));
                    double fx = (s[0] - vw * 0.5) * 1000.0 / 300.0;
                    if (fx > 10) focal = fx;
                }
                if (focal <= 0 && vw > 0) focal = (vw * 0.5) / std::tan(v.fov * 3.14159265358979 / 360.0);
            }
            v.focal = focal;
            v.width = vw;
            v.height = vh;
        }
    }
    if (!v.valid && ActorPosFast(st.pawn, eye)) v.valid = false;
    std::lock_guard<std::mutex> g(*modelLock);
    model->camera = v;
}

struct FreeCamState {
    bool active = false;
    void* actor = nullptr;
    void* pc = nullptr;
    double x = 0, y = 0, z = 0, yaw = 0, pitch = 0;
    bool ignoredLook = false, ignoredMove = false;
} freeCam;

float seaLevel = 0;
bool seaKnown = false;

struct NoClipState {
    bool active = false;
    void* pawn = nullptr;
    void* move = nullptr;
    void* pc = nullptr;
    uint8_t savedMode = 0;
    uint8_t savedCustom = 0;
    bool ignoredMove = false;
    bool collisionOff = false;
    bool flying = false;
    bool underwater = false;
    bool swimBroken = false;
    int swimFails = 0;
    ULONGLONG waterAt = 0;
} noclip;

bool SpotBlocked(const ue::FVector& p, double radius, double half, void* who);
ue::FVector SafeSpot(const ue::FVector& p, void* who);

uint64_t lastFrameSeen = 0;
LARGE_INTEGER lastFrameTime{}, qpcFreq{};

bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

void SetIgnore(void* pc, bool look, bool value) {
    if (!ue::Alive(pc)) return;
    ue::Call c(pc, look ? "SetIgnoreLookInput" : "SetIgnoreMoveInput");
    c.SetBool(look ? "bNewLookInput" : "bNewMoveInput", value);
    c.Run();
}

bool CameraNow(ue::FVector& pos, ue::FRotator& rot, float& fov) {
    if (!ue::Alive(camManager)) {
        if (!st.world) return false;
        ue::Call cam(Gameplay(), "GetPlayerCameraManager");
        cam.Set<void*>("WorldContextObject", st.world).Set<int32_t>("PlayerIndex", 0);
        camManager = cam.Run() ? cam.Get<void*>("ReturnValue") : nullptr;
        camManagerAt = GetTickCount64();
        if (!ue::Alive(camManager)) return false;
    }
    ue::Call cl(camManager, "GetCameraLocation");
    ue::Call cr(camManager, "GetCameraRotation");
    ue::Call cf(camManager, "GetFOVAngle");
    if (!cl.Run() || !cr.Run()) return false;
    pos = cl.Get<ue::FVector>("ReturnValue");
    rot = cr.Get<ue::FRotator>("ReturnValue");
    fov = cf.Run() ? cf.Get<float>("ReturnValue") : 90.0f;
    if (fov < 20.0f || fov > 170.0f) fov = 90.0f;
    return true;
}

void* SpawnSimpleActor(const char* classPath, const ue::FVector& at, double yawDeg) {
    void* cls = ue::FindObject(classPath);
    if (!cls || !st.world) return nullptr;
    double yaw = yawDeg * 3.14159265358979 / 180.0;
    alignas(16) double xf[12] = {0, 0, std::sin(yaw / 2), std::cos(yaw / 2), at.x, at.y, at.z, 0, 1, 1, 1, 0};
    ue::Call sp(Gameplay(), "BeginDeferredActorSpawnFromClass");
    sp.Set<void*>("WorldContextObject", st.world).Set<void*>("ActorClass", cls).SetRaw("SpawnTransform", xf, sizeof(xf));
    sp.Set<uint8_t>("CollisionHandlingOverride", 1).Set<void*>("Owner", nullptr).Set<uint8_t>("TransformScaleMethod", 1);
    void* actor = sp.Run() ? sp.Get<void*>("ReturnValue") : nullptr;
    if (!ue::Alive(actor)) return nullptr;
    ue::Call fin(Gameplay(), "FinishSpawningActor");
    fin.Set<void*>("Actor", actor).SetRaw("SpawnTransform", xf, sizeof(xf)).Set<uint8_t>("TransformScaleMethod", 1);
    fin.Run();
    return ue::Alive(actor) ? actor : nullptr;
}

void SetViewTarget(void* pc, void* target) {
    if (!ue::Alive(pc) || !ue::Alive(target)) return;
    ue::Call c(pc, "SetViewTargetWithBlend");
    c.Set<void*>("NewViewTarget", target).Set<float>("BlendTime", 0.0f).Set<uint8_t>("BlendFunc", 0).Set<float>("BlendExp", 0.0f);
    c.SetBool("bLockOutgoing", false);
    c.Run();
}

void* ControlledPawn(void* pc) {
    ue::Call c(pc, "K2_GetPawn");
    void* p = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    if (ue::Alive(p)) return p;
    return st.vehicle ? st.vehicle : st.pawn;
}

void StopFreeCam() {
    if (!freeCam.active) return;
    if (ue::Alive(freeCam.pc)) {
        void* back = ControlledPawn(freeCam.pc);
        if (back) SetViewTarget(freeCam.pc, back);
        if (freeCam.ignoredLook) SetIgnore(freeCam.pc, true, false);
        if (freeCam.ignoredMove) SetIgnore(freeCam.pc, false, false);
    }
    if (ue::Alive(freeCam.actor)) ue::Call(freeCam.actor, "K2_DestroyActor").Run();
    freeCam = FreeCamState{};
    LOG("free camera off");
}

bool StartFreeCam() {
    ue::FVector pos;
    ue::FRotator rot;
    float fov;
    if (!st.pc || !CameraNow(pos, rot, fov)) return false;
    void* actor = SpawnSimpleActor("/Script/Engine.CameraActor", pos, rot.yaw);
    if (!actor) {
        LOG("free camera: could not create a camera");
        return false;
    }
    void* comp = ue::Ptr(actor, "CameraComponent");
    if (comp) {
        ue::WriteBool(comp, ue::FindProp(ue::ClassOf(comp), "bConstrainAspectRatio"), false);
        ue::Call f(comp, "SetFieldOfView");
        f.Set<float>("InFieldOfView", fov);
        f.Run();
    }
    freeCam.active = true;
    freeCam.actor = actor;
    freeCam.pc = st.pc;
    freeCam.x = pos.x;
    freeCam.y = pos.y;
    freeCam.z = pos.z;
    freeCam.yaw = rot.yaw;
    freeCam.pitch = std::clamp(rot.pitch > 180 ? rot.pitch - 360 : rot.pitch, -89.0, 89.0);
    SetViewTarget(st.pc, actor);
    SetIgnore(st.pc, true, true);
    SetIgnore(st.pc, false, true);
    freeCam.ignoredLook = freeCam.ignoredMove = true;
    float dx, dy;
    overlay::TakeLook(dx, dy);
    LOG("free camera on");
    return true;
}

void MoveInput(double& fwd, double& side, double& up, bool& fast) {
    fwd = side = up = 0;
    fast = false;
    if (!overlay::GameActive()) return;
    fwd = (KeyDown('W') ? 1 : 0) - (KeyDown('S') ? 1 : 0);
    side = (KeyDown('D') ? 1 : 0) - (KeyDown('A') ? 1 : 0);
    up = (KeyDown(VK_SPACE) ? 1 : 0) - ((KeyDown(VK_LCONTROL) || KeyDown('C')) ? 1 : 0);
    fast = KeyDown(VK_LSHIFT);
}

void TickFreeCam(bool allowed, double dt) {
    bool want = allowed && st.inWorld && st.pc && On("free_camera");
    if (freeCam.active && (!want || freeCam.pc != st.pc || !ue::Alive(freeCam.actor))) StopFreeCam();
    if (!want) return;
    if (!freeCam.active) {
        static ULONGLONG lastTry = 0;
        ULONGLONG now = GetTickCount64();
        if (now - lastTry < 1000) return;
        lastTry = now;
        if (!StartFreeCam()) return;
    }
    static ULONGLONG lastViewCheck = 0;
    ULONGLONG tick = GetTickCount64();
    if (tick - lastViewCheck > 500) {
        lastViewCheck = tick;
        ue::Call vt(freeCam.pc, "GetViewTarget");
        if (vt.Run() && vt.Get<void*>("ReturnValue") != freeCam.actor) SetViewTarget(freeCam.pc, freeCam.actor);
    }
    float mx, my;
    overlay::TakeLook(mx, my);
    if (overlay::GameActive()) {
        freeCam.yaw += mx * 0.07;
        freeCam.pitch = std::clamp(freeCam.pitch - my * 0.07, -89.0, 89.0);
    }
    double fwd, side, up;
    bool fast;
    MoveInput(fwd, side, up, fast);
    double speed = 1000.0 * std::clamp((double)Float("free_camera_speed"), 0.1, 20.0) * (fast ? 4.0 : 1.0);
    double P = freeCam.pitch * 3.14159265358979 / 180, Y = freeCam.yaw * 3.14159265358979 / 180;
    double fx = std::cos(P) * std::cos(Y), fy = std::cos(P) * std::sin(Y), fz = std::sin(P);
    double rx = -std::sin(Y), ry = std::cos(Y);
    freeCam.x += (fx * fwd + rx * side) * speed * dt;
    freeCam.y += (fy * fwd + ry * side) * speed * dt;
    freeCam.z += (fz * fwd + up) * speed * dt;
    ue::Call c(freeCam.actor, "K2_SetActorLocationAndRotation");
    c.Set<ue::FVector>("NewLocation", ue::FVector{freeCam.x, freeCam.y, freeCam.z});
    c.Set<ue::FRotator>("NewRotation", ue::FRotator{freeCam.pitch, freeCam.yaw, 0});
    c.SetBool("bSweep", false).SetBool("bTeleport", true);
    c.Run();
}

void SetMoveMode(void* move, uint8_t mode, uint8_t custom = 0) {
    ue::Call m(move, "SetMovementMode");
    m.Set<uint8_t>("NewMovementMode", mode).Set<uint8_t>("NewCustomMode", mode == 6 ? custom : (uint8_t)0);
    m.Run();
}

void SetCollision(void* actor, bool on) {
    ue::Call col(actor, "SetActorEnableCollision");
    col.SetBool("bNewActorEnableCollision", on);
    col.Run();
}

bool Underwater(void* actor) {
    ue::FVector at;
    if (!ActorPos(actor, at)) return false;
    ue::Call c(Cdo("/Script/UWEVolumeTracker.Default__UWEVolumeTrackerStatics"), "IsLocationUnderwater");
    c.Set<void*>("WorldContextObject", st.world).Set<ue::FVector>("Location", at).Set<float>("Radius", 30.0f);
    if (c.Run()) return c.GetBool("ReturnValue");
    return at.z < seaLevel * 100.0 - 50.0;
}

bool Attached(void* pawn) { return ue::Ptr(pawn, "AttachedToOwner") != nullptr; }

void Unstick(void* pawn) {
    if (Attached(pawn) || st.vehicle) return;
    ue::FVector at;
    double radius = 45.0, half = 95.0;
    if (void* cap = ue::Ptr(pawn, "CapsuleComponent")) {
        float r = CallFloat(cap, "GetScaledCapsuleRadius"), h = CallFloat(cap, "GetScaledCapsuleHalfHeight");
        if (r > 5.0f && r < 300.0f) radius = r;
        if (h > 5.0f && h < 400.0f) half = h;
    }
    if (!ActorPos(pawn, at) || !SpotBlocked(at, radius, half, pawn)) return;
    ue::FVector to = SafeSpot(at, pawn);
    if (std::fabs(to.x - at.x) + std::fabs(to.y - at.y) + std::fabs(to.z - at.z) > 1.0) {
        MoveActor(pawn, to);
        LOG("no-clip off inside something, moved to a free spot");
    }
}

void StopNoClip() {
    if (!noclip.active) return;
    bool pawnOk = ue::Alive(noclip.pawn);
    if (pawnOk && noclip.collisionOff) {
        SetCollision(noclip.pawn, true);
        Unstick(noclip.pawn);
    }
    bool seated = pawnOk && (Attached(noclip.pawn) || st.vehicle);
    if (ue::Alive(noclip.move) && noclip.flying && !seated) {
        ue::Prop vel = ue::FindProp(ue::ClassOf(noclip.move), "Velocity");
        if (vel) ue::Write<ue::FVector>(noclip.move, vel, ue::FVector{});
        SetMoveMode(noclip.move, pawnOk && Underwater(noclip.pawn) ? 4 : 3);
    }
    if (noclip.ignoredMove) SetIgnore(noclip.pc, false, false);
    noclip = NoClipState{};
    LOG("no-clip and fly off");
}

void FlyStep(void* pawn, ue::FVector delta, bool sweep) {
    if (!sweep) {
        ue::FVector here;
        if (ActorPos(pawn, here)) MoveActor(pawn, ue::FVector{here.x + delta.x, here.y + delta.y, here.z + delta.z});
        return;
    }
    static void* hitStruct = nullptr;
    if (!ue::Valid(hitStruct)) hitStruct = ue::FindObject("/Script/Engine.HitResult");
    ue::Prop bp = hitStruct ? ue::FindProp(hitStruct, "bBlockingHit") : ue::Prop{};
    ue::Prop sp = hitStruct ? ue::FindProp(hitStruct, "bStartPenetrating") : ue::Prop{};
    ue::Prop tp = hitStruct ? ue::FindProp(hitStruct, "Time") : ue::Prop{};
    ue::Prop np = hitStruct ? ue::FindProp(hitStruct, "Normal") : ue::Prop{};
    for (int pass = 0; pass < 3; ++pass) {
        ue::FVector here;
        if (!ActorPos(pawn, here)) return;
        ue::Call c(pawn, "K2_SetActorLocation");
        c.Set<ue::FVector>("NewLocation", ue::FVector{here.x + delta.x, here.y + delta.y, here.z + delta.z});
        c.SetBool("bSweep", true).SetBool("bTeleport", false);
        if (!c.Run()) return;
        int size = c.ParamSize("SweepHitResult");
        if (!bp || !np || size <= 0 || size > 1024) return;
        std::vector<uint8_t> hit((size_t)size);
        if (!c.GetRaw("SweepHitResult", hit.data(), size)) return;
        if ((hit[bp.offset] & bp.mask) == 0) return;
        ue::FVector n = *(ue::FVector*)(hit.data() + np.offset);
        if (sp && (hit[sp.offset] & sp.mask) != 0) {
            static void* hs = nullptr;
            if (!ue::Valid(hs)) hs = ue::FindObject("/Script/Engine.HitResult");
            ue::Prop dp = hs ? ue::FindProp(hs, "PenetrationDepth") : ue::Prop{};
            double depth = dp ? std::clamp((double)*(float*)(hit.data() + dp.offset), 0.0, 200.0) : 0.0;
            MoveActor(pawn, ue::FVector{here.x + n.x * (depth + 2.0), here.y + n.y * (depth + 2.0), here.z + n.z * (depth + 2.0)});
            continue;
        }
        double t = tp ? std::clamp((double)*(float*)(hit.data() + tp.offset), 0.0, 1.0) : 0.0;
        double into = delta.x * n.x + delta.y * n.y + delta.z * n.z;
        double left = 1.0 - t;
        delta = ue::FVector{(delta.x - n.x * into) * left, (delta.y - n.y * into) * left, (delta.z - n.z * into) * left};
        if (std::fabs(delta.x) + std::fabs(delta.y) + std::fabs(delta.z) < 1.0) return;
    }
}

void TickNoClip(bool allowed, double dt) {
    bool clip = On("noclip"), fly = On("fly");
    bool want = allowed && (clip || fly) && st.inWorld && st.pawn && st.charMove && !st.vehicle && st.pc && !Attached(st.pawn);
    if (noclip.active && (!want || noclip.pawn != st.pawn || noclip.pc != st.pc)) StopNoClip();
    if (!want) return;
    void* move = st.charMove;
    void* mcls = ue::ClassOf(move);
    ue::Prop modeP = ue::FindProp(mcls, "MovementMode");
    if (!noclip.active) {
        noclip.active = true;
        noclip.pawn = st.pawn;
        noclip.move = move;
        noclip.pc = st.pc;
        noclip.savedMode = modeP ? ue::Read<uint8_t>(move, modeP) : 3;
        ue::Prop customP = ue::FindProp(mcls, "CustomMovementMode");
        noclip.savedCustom = customP ? ue::Read<uint8_t>(move, customP) : 0;
        LOG("no-clip/fly on (movement mode was %d)", noclip.savedMode);
    }
    if (clip != noclip.collisionOff) {
        SetCollision(st.pawn, !clip);
        if (!clip) Unstick(st.pawn);
        noclip.collisionOff = clip;
    }
    ULONGLONG now = GetTickCount64();
    if (now - noclip.waterAt > 150) {
        noclip.waterAt = now;
        noclip.underwater = Underwater(st.pawn);
    }
    uint8_t mode = modeP ? ue::Read<uint8_t>(move, modeP) : 0;
    ue::Prop vel = ue::FindProp(mcls, "Velocity");
    bool flyNow = fly || !noclip.underwater || noclip.swimBroken;
    if (!flyNow) {
        if (noclip.ignoredMove) {
            SetIgnore(st.pc, false, false);
            noclip.ignoredMove = false;
        }
        if (noclip.flying) {
            if (vel) ue::Write<ue::FVector>(move, vel, ue::FVector{});
            SetMoveMode(move, 4);
            noclip.flying = false;
            noclip.swimFails = 0;
            return;
        }
        if (mode == 1 || mode == 3 || mode == 5) {
            SetMoveMode(move, 4);
            if (++noclip.swimFails > 30) {
                noclip.swimBroken = true;
                LOG("no-clip: the game will not swim without collision here, flying instead");
            }
        } else {
            noclip.swimFails = 0;
        }
        return;
    }
    if (!noclip.ignoredMove) {
        SetIgnore(st.pc, false, true);
        noclip.ignoredMove = true;
    }
    if (mode != 5) SetMoveMode(move, 5);
    noclip.flying = true;
    if (vel) ue::Write<ue::FVector>(move, vel, ue::FVector{});
    if (freeCam.active) return;
    double fwd, side, up;
    bool fast;
    MoveInput(fwd, side, up, fast);
    if (fwd == 0 && side == 0 && up == 0) return;
    ue::FVector cpos;
    ue::FRotator rot;
    float fov;
    if (!CameraNow(cpos, rot, fov)) return;
    double speed = 1000.0 * std::clamp((double)Float("fly_speed"), 0.1, 20.0) * (fast ? 4.0 : 1.0);
    double pitch = rot.pitch > 180 ? rot.pitch - 360 : rot.pitch;
    double P = pitch * 3.14159265358979 / 180, Y = rot.yaw * 3.14159265358979 / 180;
    double fx = std::cos(P) * std::cos(Y), fy = std::cos(P) * std::sin(Y), fz = std::sin(P);
    double rx = -std::sin(Y), ry = std::cos(Y);
    ue::FVector delta{(fx * fwd + rx * side) * speed * dt, (fy * fwd + ry * side) * speed * dt, (fz * fwd + up) * speed * dt};
    FlyStep(st.pawn, delta, !clip);
}

void FrameTick() {
    uint64_t frame = overlay::FrameCount();
    if (frame == lastFrameSeen) return;
    lastFrameSeen = frame;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (!qpcFreq.QuadPart) QueryPerformanceFrequency(&qpcFreq);
    double dt = lastFrameTime.QuadPart ? (double)(now.QuadPart - lastFrameTime.QuadPart) / (double)qpcFreq.QuadPart : 0.0;
    lastFrameTime = now;
    dt = std::clamp(dt, 0.0, 0.1);
    bool allowed = st.session != Session::Client && !MapScanActive();
    TickFreeCam(allowed, dt);
    TickNoClip(allowed, dt);
}

void UpdateNearby() {
    ULONGLONG now = GetTickCount64();
    if (now - lastNearby < 250) return;
    lastNearby = now;
    ue::FVector me = Eye();
    nearby.clear();
    for (const Tracked& t : tracked) {
        if (t.kind != KCreature) continue;
        ue::FVector p;
        if (!ActorPosFast(t.actor, p)) continue;
        double d = Dist(p, me);
        double range = t.sub == 2 ? 1500.0 : t.sub == 3 ? 300.0 : 400.0;
        if (d <= range) nearby.push_back({t, p, d});
    }
    std::sort(nearby.begin(), nearby.end(), [](const Creature& a, const Creature& b) { return a.d < b.d; });
    if (nearby.size() > 80) nearby.resize(80);
}

float HealthOf(void* actor, float* maxOut = nullptr) {
    void* hc = ue::Ptr(actor, "HealthComponent");
    if (!hc) hc = ue::Ptr(actor, "HealthSetComponent");
    if (!hc) return -1;
    float mx = CallFloat(hc, "GetMaxHealth");
    if (maxOut) *maxOut = mx;
    return CallFloat(hc, "GetHealth");
}


void FillBox(core::EspDot& dot, void* actor, const ue::FVector& p, bool want, double limit, double fallback, ULONGLONG now, ULONGLONG refresh) {
    Bounds& b = boundsMemo[actor];
    if (want && now - b.at > refresh) {
        ue::Call gb(actor, "GetActorBounds");
        gb.SetBool("bOnlyCollidingComponents", true).SetBool("bIncludeFromChildActors", false);
        b.offset = {0, 0, 0};
        b.extent = {fallback, fallback, fallback};
        if (gb.Run()) {
            ue::FVector o = gb.Get<ue::FVector>("Origin"), e = gb.Get<ue::FVector>("BoxExtent");
            if (e.x > 2 && e.y > 2 && e.z > 2 && e.x < limit && e.y < limit && e.z < limit && std::fabs(o.x - p.x) < limit &&
                std::fabs(o.y - p.y) < limit && std::fabs(o.z - p.z) < limit) {
                b.offset = {o.x - p.x, o.y - p.y, o.z - p.z};
                b.extent = e;
            }
        }
        b.at = now;
    }
    dot.x = p.x;
    dot.y = p.y;
    dot.z = p.z;
    if (want && b.at) {
        dot.box = true;
        dot.cx = p.x + b.offset.x;
        dot.cy = p.y + b.offset.y;
        dot.cz = p.z + b.offset.z;
        dot.ex = b.extent.x;
        dot.ey = b.extent.y;
        dot.ez = b.extent.z;
        dot.top = dot.cz + dot.ez;
    } else {
        dot.top = p.z + 60;
    }
}

void UpdateEsp() {
    ULONGLONG now = GetTickCount64();
    bool allowed = st.inWorld && st.pc && st.session != Session::Client;
    if (allowed) UpdateCamera();
    if (allowed) {
        UpdateNearby();
    }
    if (now - lastEsp < 33) return;
    lastEsp = now;
    std::vector<core::EspDot> dots;
    if (On("esp") && allowed) {
        bool box = On("esp_box");
        int shown = 0;
        for (Creature& n : nearby) {
            int sub = n.t.sub;
            bool filtered = (sub == 0 || sub == 3) ? !On("esp_passive") : sub == 1 ? !On("esp_hostile") : !On("esp_leviathan");
            if (filtered) continue;
            if (++shown > 60) break;
            ue::FVector p;
            if (ActorPosFast(n.t.actor, p)) n.p = p;
            core::EspDot dot;
            FillBox(dot, n.t.actor, n.p, box && sub != 3, sub == 2 ? 10000.0 : 1500.0, 60.0, now, 1000);
            float hp = -1;
            auto hm = healthMemo.find(n.t.actor);
            if (hm != healthMemo.end() && now - hm->second.second < 500) {
                hp = hm->second.first;
            } else if (sub != 3) {
                float mx = 0;
                float cur = HealthOf(n.t.actor, &mx);
                hp = mx > 0 && cur >= 0 ? cur / mx : -1;
                healthMemo[n.t.actor] = {hp, now};
            }
            dot.name = n.t.name;
            dot.dist = (float)Dist(n.p, Eye());
            dot.health = hp;
            dot.kind = sub == 3 ? 0 : sub;
            dots.push_back(dot);
        }
    }
    std::lock_guard<std::mutex> g(*modelLock);
    model->esp = std::move(dots);
}
ULONGLONG lastUse = 0;
std::unordered_map<std::string, int> useCounts;
void* useInventory = nullptr;
int GiveItem(void* type, int amount);
int CountItem(void* inv, void* type) {
    ue::Call c(inv, "GetItemCountByType");
    c.Set<void*>("ItemType", type);
    return c.Run() ? c.Get<int32_t>("ReturnValue") : -1;
}
void* Inventory();

void UnlimitedUse(bool allowed) {
    ULONGLONG now = GetTickCount64();
    if (now - lastUse < 300) return;
    lastUse = now;
    void* inv = allowed && On("unlimited_use") ? Inventory() : nullptr;
    if (!inv || inv != useInventory) {
        useCounts.clear();
        useInventory = inv;
        if (!inv) return;
    }
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> g(*modelLock);
        for (const core::Item& it : model->items)
            if (it.category == "Food" || it.category == "Healing" || it.category == "Tools" || it.category == "Equipment") ids.push_back(it.id);
    }
    for (const std::string& id : ids) {
        auto t = itemTypes.find(id);
        if (t == itemTypes.end() || !ue::Valid(t->second)) continue;
        int n = CountItem(inv, t->second);
        if (n < 0) continue;
        auto prev = useCounts.find(id);
        if (prev != useCounts.end() && n == prev->second - 1) {
            GiveItem(t->second, 1);
            n = prev->second;
        }
        useCounts[id] = n;
    }
}

void* adaptStruct = nullptr;

void* AdaptationsComponent() {
    void* ps = st.pc ? ue::Ptr(st.pc, "PlayerState") : nullptr;
    return ps ? ue::Ptr(ps, "AdaptationsComponent") : nullptr;
}

bool AdaptationAt(void* comp, int index, uint8_t*& elem, int& size) {
    if (!ue::Valid(adaptStruct)) adaptStruct = ue::FindClassByName("UWEPlayerAdaptation");
    size = adaptStruct ? ue::StructSize(adaptStruct) : 0;
    if (size <= 0 || size > 1024) return false;
    auto arr = ue::Read<ue::TArray<uint8_t>>(comp, ue::FindProp(ue::ClassOf(comp), "Adaptations"));
    if (!arr.data || index < 0 || index >= arr.num) return false;
    elem = arr.data + (size_t)index * size;
    return true;
}

std::string OneLine(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue;
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

std::string TextAt(const uint8_t* text, int size, bool cached = true) {
    if (size <= 0 || size > 64) return {};
    static std::unordered_map<uintptr_t, std::string> cache;
    uintptr_t key = cached ? *(const uintptr_t*)text : 0;
    if (key) {
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    ue::Call c(Cdo("/Script/Engine.Default__KismetTextLibrary"), "Conv_TextToString");
    c.SetRaw("InText", text, size);
    if (!c.Run()) return {};
    std::string out = OneLine(ue::FStringToUtf8(c.Get<ue::FString>("ReturnValue")));
    if (key) {
        if (cache.size() > 5000) cache.clear();
        cache[key] = out;
    }
    return out;
}

ULONGLONG lastAdapt = 0;

void RefreshAdaptations() {
    ULONGLONG now = GetTickCount64();
    if (now - lastAdapt < 1500 || !MenuOpen()) return;
    lastAdapt = now;
    void* comp = AdaptationsComponent();
    std::vector<core::Adaptation> list;
    if (comp) {
        auto arr = ue::Read<ue::TArray<uint8_t>>(comp, ue::FindProp(ue::ClassOf(comp), "Adaptations"));
        for (int i = 0; i < arr.num && i < 64; ++i) {
            uint8_t* e;
            int size;
            if (!AdaptationAt(comp, i, e, size)) break;
            ue::Prop nameP = ue::FindProp(adaptStruct, "Name"), descP = ue::FindProp(adaptStruct, "Description");
            ue::Prop tagP = ue::FindProp(adaptStruct, "AdaptationTag");
            core::Adaptation a;
            a.id = tagP ? ue::NameToString(*(ue::FName*)(e + tagP.offset)) : std::to_string(i);
            a.name = nameP ? TextAt(e + nameP.offset, nameP.size) : a.id;
            a.hint = descP ? TextAt(e + descP.offset, descP.size) : std::string();
            if (a.hint.size() > 80) a.hint = a.hint.substr(0, 77) + "...";
            if (a.name.empty()) a.name = a.id;
            ue::Call has(comp, "HasAdaptation");
            has.SetRaw("Adaptation", e, size);
            a.on = has.Run() && has.GetBool("ReturnValue");
            list.push_back(a);
        }
    }
    static size_t logged = 0;
    if (list.size() != logged) {
        logged = list.size();
        for (const core::Adaptation& a : list) LOG("adaptation: %s (%s)", a.name.c_str(), a.id.c_str());
    }
    if (list.size() > 2) list.resize(list.size() - 2);
    std::lock_guard<std::mutex> g(*modelLock);
    model->adaptations = std::move(list);
}

void SetAdaptation(int index, bool on) {
    void* comp = AdaptationsComponent();
    uint8_t* e;
    int size;
    if (!comp || !AdaptationAt(comp, index, e, size)) {
        Note("Load a save first");
        return;
    }
    std::vector<uint8_t> copy(e, e + size);
    ue::Call c(comp, "SetAdaptationEnabled");
    c.SetRaw("Adaptation", copy.data(), size).SetBool("bEnabled", on);
    if (!c.Run()) Note("The game refused that change");
    lastAdapt = 0;
}

const double kCell = 8.0;
std::unordered_map<int64_t, int16_t> survey;
std::unordered_map<int64_t, uint8_t> surveyTries;
std::vector<std::pair<int, int>> surveyPlan;
int planX = INT32_MIN, planY = INT32_MIN;
std::string surveyWorld;
bool surveyDirty = false, imageDirty = false;
ULONGLONG lastSurveySave = 0, lastImage = 0, lastSurvey = 0;
int mapCx = INT32_MIN, mapCy = INT32_MIN;
const int kMapSize = 1024;

int64_t Key(int x, int y) { return ((int64_t)x << 32) | (uint32_t)y; }

std::wstring SurveyFile() {
    std::wstring w(surveyWorld.begin(), surveyWorld.end());
    return std::filesystem::path(iconDir).parent_path().wstring() + L"\\map_" + w + L".bin";
}

void SaveSurvey() {
    if (surveyWorld.empty() || survey.empty()) return;
    std::ofstream out(SurveyFile(), std::ios::binary | std::ios::trunc);
    for (const auto& [k, h] : survey) {
        int32_t x = (int32_t)(k >> 32), y = (int32_t)(uint32_t)k;
        out.write((const char*)&x, 4);
        out.write((const char*)&y, 4);
        out.write((const char*)&h, 2);
    }
    surveyDirty = false;
}

void LoadSurvey() {
    survey.clear();
    surveyTries.clear();
    std::ifstream in(SurveyFile(), std::ios::binary);
    int32_t x, y;
    int16_t h;
    while (in.read((char*)&x, 4) && in.read((char*)&y, 4) && in.read((char*)&h, 2)) survey[Key(x, y)] = h;
    LOG("map: %zu surveyed spots loaded for %s", survey.size(), surveyWorld.c_str());
    imageDirty = true;
}

struct SpotRec {
    std::string kind, name;
    float x = 0, y = 0, z = 0;
};

std::map<std::string, SpotRec> spots;
std::string spotsWorld;
bool spotsDirty = false;
ULONGLONG lastSpotSave = 0, lastSpotPass = 0;

std::wstring SpotsFile() {
    std::wstring w(spotsWorld.begin(), spotsWorld.end());
    return std::filesystem::path(iconDir).parent_path().wstring() + L"\\places_" + w + L".json";
}

std::string SpotKey(const SpotRec& r) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%s:%d:%d:%d", r.kind.c_str(), (int)std::lround(r.x / 3.0f), (int)std::lround(r.y / 3.0f),
                  (int)std::lround(r.z / 3.0f));
    return buf;
}

void LoadSpots() {
    spots.clear();
    spotsDirty = false;
    std::ifstream in(SpotsFile());
    if (!in) return;
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_array()) return;
    for (const auto& e : j) {
        if (!e.is_object()) continue;
        SpotRec r;
        r.kind = e.value("kind", std::string());
        r.name = e.value("name", std::string());
        r.x = e.value("x", 0.0f);
        r.y = e.value("y", 0.0f);
        r.z = e.value("z", 0.0f);
        if (!r.kind.empty()) spots[SpotKey(r)] = r;
    }
    LOG("map: %zu places loaded for %s", spots.size(), spotsWorld.c_str());
}

void SaveSpots() {
    if (spotsWorld.empty()) return;
    nlohmann::json j = nlohmann::json::array();
    for (const auto& [k, r] : spots)
        j.push_back({{"kind", r.kind}, {"name", r.name}, {"x", std::round(r.x * 10.0f) / 10.0f}, {"y", std::round(r.y * 10.0f) / 10.0f},
                     {"z", std::round(r.z * 10.0f) / 10.0f}});
    std::wstring file = SpotsFile(), tmp = file + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(1);
        if (!out.good()) return;
    }
    if (MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) spotsDirty = false;
}

const char* SpotKind(const Tracked& t) {
    std::string cls = ue::Name(ue::ClassOf(t.actor));
    if (t.kind == KSignal) {
        if (cls.find("Beacon") != std::string::npos || cls == "BP_Lifepod_C") return nullptr;
        return cls.find("Lifepod") != std::string::npos ? "lifepod" : "signal";
    }
    if (t.kind != KData || cls.find("PlayerDied") != std::string::npos) return nullptr;
    if (cls.find("SupplyCrate") != std::string::npos) return "crate";
    if (cls.find("SupplyLocker") != std::string::npos) return "locker";
    if (cls.find("Terminal") != std::string::npos) return "terminal";
    if (cls.find("Blackbox") != std::string::npos) return "blackbox";
    if (cls.find("QuartzChip") != std::string::npos) return "chip";
    return nullptr;
}

void RecordSpots() {
    if (!st.inWorld || surveyWorld.empty() || iconDir.empty()) return;
    ULONGLONG now = GetTickCount64();
    if (now - lastSpotPass < 1000) return;
    lastSpotPass = now;
    if (spotsWorld != surveyWorld) {
        if (spotsDirty) SaveSpots();
        spotsWorld = surveyWorld;
        LoadSpots();
    }
    for (const Tracked& t : tracked) {
        if (t.kind != KSignal && t.kind != KData) continue;
        if (!ue::Alive(t.actor)) continue;
        const char* kind = SpotKind(t);
        if (!kind) continue;
        ue::FVector p;
        if (!ActorPosFast(t.actor, p)) continue;
        if (std::fabs(p.x) < 1 && std::fabs(p.y) < 1 && std::fabs(p.z) < 1) continue;
        SpotRec r;
        r.kind = kind;
        r.name = std::strcmp(kind, "signal") == 0 ? SignalName(t.actor) : t.name;
        r.x = (float)(p.x / 100);
        r.y = (float)(p.y / 100);
        r.z = (float)(p.z / 100);
        std::string k = SpotKey(r);
        if (spots.find(k) == spots.end()) {
            spots[k] = r;
            spotsDirty = true;
        }
    }
    if (spotsDirty && now - lastSpotSave >= 5000) {
        lastSpotSave = now;
        SaveSpots();
    }
}


bool TraceDown(double xm, double ym, double fromZ, float& hitZ, bool* water = nullptr) {
    static std::vector<void*> ignore(1);
    ignore[0] = st.pawn;
    ue::TArray<void*> ig{ignore.data(), 1, 1};
    ue::Call c(SystemLib(), "LineTraceSingle");
    c.Set<void*>("WorldContextObject", st.world)
        .Set<ue::FVector>("Start", ue::FVector{xm * 100, ym * 100, fromZ * 100})
        .Set<ue::FVector>("End", ue::FVector{xm * 100, ym * 100, -500000.0})
        .Set<uint8_t>("TraceChannel", 0)
        .SetBool("bTraceComplex", false)
        .Set<ue::TArray<void*>>("ActorsToIgnore", ig)
        .Set<uint8_t>("DrawDebugType", 0)
        .SetBool("bIgnoreSelf", true);
    if (!c.Run() || !c.GetBool("ReturnValue")) return false;
    static void* hitStruct = nullptr;
    if (!ue::Valid(hitStruct)) hitStruct = ue::FindObject("/Script/Engine.HitResult");
    ue::Prop ip = hitStruct ? ue::FindProp(hitStruct, "ImpactPoint") : ue::Prop{};
    int size = c.ParamSize("OutHit");
    if (!ip || size <= 0 || size > 1024) return false;
    std::vector<uint8_t> hit((size_t)size);
    if (!c.GetRaw("OutHit", hit.data(), size)) return false;
    hitZ = (float)(((ue::FVector*)(hit.data() + ip.offset))->z / 100.0);
    if (water) {
        *water = false;
        ue::Prop cp = ue::FindProp(hitStruct, "Component");
        if (cp && cp.offset + 8 <= size) {
            void* comp = ue::WeakGet(*(int32_t*)(hit.data() + cp.offset), *(int32_t*)(hit.data() + cp.offset + 4));
            if (comp) {
                std::string n = ue::Name(ue::ClassOf(comp));
                *water = n.find("Water") != std::string::npos || n.find("Ocean") != std::string::npos;
            }
        }
    }
    return true;
}

bool SpotBlocked(const ue::FVector& p, double radius, double half, void* who) {
    static std::vector<void*> ignore;
    ignore.clear();
    if (st.pawn) ignore.push_back(st.pawn);
    if (who && who != st.pawn) ignore.push_back(who);
    ue::TArray<void*> ig{ignore.data(), (int32_t)ignore.size(), (int32_t)ignore.size()};
    ue::Call c(SystemLib(), "CapsuleTraceSingle");
    c.Set<void*>("WorldContextObject", st.world)
        .Set<ue::FVector>("Start", ue::FVector{p.x, p.y, p.z + 2.0})
        .Set<ue::FVector>("End", ue::FVector{p.x, p.y, p.z - 2.0})
        .Set<float>("Radius", (float)radius)
        .Set<float>("HalfHeight", (float)half)
        .Set<uint8_t>("TraceChannel", 0)
        .SetBool("bTraceComplex", false)
        .Set<ue::TArray<void*>>("ActorsToIgnore", ig)
        .Set<uint8_t>("DrawDebugType", 0)
        .SetBool("bIgnoreSelf", true);
    if (!c.Run() || !c.GetBool("ReturnValue")) return false;
    static void* hitStruct = nullptr;
    if (!ue::Valid(hitStruct)) hitStruct = ue::FindObject("/Script/Engine.HitResult");
    ue::Prop sp = hitStruct ? ue::FindProp(hitStruct, "bStartPenetrating") : ue::Prop{};
    int size = c.ParamSize("OutHit");
    if (!sp || size <= 0 || size > 1024) return true;
    std::vector<uint8_t> hit((size_t)size);
    if (!c.GetRaw("OutHit", hit.data(), size)) return true;
    return (hit[sp.offset] & sp.mask) != 0;
}

ue::FVector SafeSpot(const ue::FVector& p, void* who) {
    if (!st.world) return p;
    bool vehicle = who && who != st.pawn;
    double radius = vehicle ? 260.0 : 45.0, half = vehicle ? 220.0 : 95.0;
    if (!SpotBlocked(p, radius, half, who)) return p;
    static const double rings[] = {150, 300, 450, 600, 800, 1000, 1300, 1600, 2000, 2500};
    static const double heights[] = {0, 150, 300, -150, 500};
    for (double r : rings)
        for (double dz : heights)
            for (int k = 0; k < 12; ++k) {
                double a = k * 3.14159265358979 / 6.0;
                ue::FVector q{p.x + std::cos(a) * r, p.y + std::sin(a) * r, p.z + dz};
                if (SpotBlocked(q, radius, half, who)) continue;
                LOG("teleport target was blocked, moved %.1f m", std::sqrt(r * r + dz * dz) / 100.0);
                return q;
            }
    for (double dz = 300; dz <= 4000; dz += 300) {
        ue::FVector q{p.x, p.y, p.z + dz};
        if (!SpotBlocked(q, radius, half, who)) {
            LOG("teleport target was blocked, moved %.1f m up", dz / 100.0);
            return q;
        }
    }
    LOG("teleport target blocked and no free spot found nearby");
    return p;
}

bool LineHit(const ue::FVector& a, const ue::FVector& b, void* who, ue::FVector* where) {
    static std::vector<void*> ignore;
    ignore.clear();
    if (st.pawn) ignore.push_back(st.pawn);
    if (who && who != st.pawn) ignore.push_back(who);
    ue::TArray<void*> ig{ignore.data(), (int32_t)ignore.size(), (int32_t)ignore.size()};
    ue::Call c(SystemLib(), "LineTraceSingle");
    c.Set<void*>("WorldContextObject", st.world)
        .Set<ue::FVector>("Start", a)
        .Set<ue::FVector>("End", b)
        .Set<uint8_t>("TraceChannel", 0)
        .SetBool("bTraceComplex", true)
        .Set<ue::TArray<void*>>("ActorsToIgnore", ig)
        .Set<uint8_t>("DrawDebugType", 0)
        .SetBool("bIgnoreSelf", true);
    if (!c.Run() || !c.GetBool("ReturnValue")) return false;
    if (!where) return true;
    static void* hitStruct = nullptr;
    if (!ue::Valid(hitStruct)) hitStruct = ue::FindObject("/Script/Engine.HitResult");
    ue::Prop ip = hitStruct ? ue::FindProp(hitStruct, "ImpactPoint") : ue::Prop{};
    int size = c.ParamSize("OutHit");
    if (!ip || size <= 0 || size > 1024) return false;
    std::vector<uint8_t> hit((size_t)size);
    if (!c.GetRaw("OutHit", hit.data(), size)) return false;
    *where = *(ue::FVector*)(hit.data() + ip.offset);
    return true;
}

ue::FVector OutOfSolid(const ue::FVector& p, void* who) {
    if (!st.world) return p;
    double sea = seaLevel * 100.0, startZ = p.z + 8000.0;
    if (p.z < sea - 100.0) startZ = std::min(startZ, sea - 100.0);
    if (startZ <= p.z + 20.0) return p;
    ue::FVector surface;
    if (!LineHit(ue::FVector{p.x, p.y, startZ}, p, who, &surface) || surface.z <= p.z + 20.0) return p;
    if (std::fabs(surface.z - sea) < 150.0) return p;
    if (LineHit(p, ue::FVector{p.x, p.y, surface.z + 50.0}, who, nullptr)) return p;
    bool vehicle = who && who != st.pawn;
    LOG("teleport target was inside something, moved up %.1f m", (surface.z - p.z) / 100.0);
    return ue::FVector{p.x, p.y, surface.z + (vehicle ? 400.0 : 150.0)};
}

bool TeleportTo(void* who, const ue::FVector& target) {
    ue::FVector cur;
    if (!who || !ActorPos(who, cur)) return false;
    ue::FVector to = SafeSpot(OutOfSolid(target, who), who);
    if (!MoveActor(who, to)) return false;
    undoSpot = {true, cur};
    std::lock_guard<std::mutex> g(*modelLock);
    model->canUndoTeleport = true;
    return true;
}

bool SurveyPoint(int cx, int cy, float& h) {
    double xm = cx * kCell, ym = cy * kCell;
    float z;
    bool water = false;
    if (!TraceDown(xm, ym, seaLevel + 300.0, z, &water)) return false;
    if (std::fabs(z - seaLevel) < 0.6f) {
        float z2;
        if (TraceDown(xm, ym, seaLevel - 1.0, z2)) z = z2;
        else if (water) return false;
    }
    h = z;
    return true;
}

bool GroundAt(float x, float y, float& h) {
    int cx = (int)std::floor(x / kCell + 0.5), cy = (int)std::floor(y / kCell + 0.5);
    auto it = survey.find(Key(cx, cy));
    if (it != survey.end()) {
        h = it->second / 10.0f;
        return true;
    }
    if (!st.world) return false;
    return SurveyPoint(cx, cy, h);
}

void PlanSurvey(int px, int py) {
    surveyPlan.clear();
    const int r = 90;
    for (int dy = -r; dy <= r; ++dy)
        for (int dx = -r; dx <= r; ++dx) {
            if (dx * dx + dy * dy > r * r) continue;
            int64_t k = Key(px + dx, py + dy);
            if (survey.count(k)) continue;
            auto t = surveyTries.find(k);
            if (t != surveyTries.end() && t->second >= 3) continue;
            surveyPlan.push_back({px + dx, py + dy});
        }
    std::sort(surveyPlan.begin(), surveyPlan.end(), [px, py](const auto& a, const auto& b) {
        int da = (a.first - px) * (a.first - px) + (a.second - py) * (a.second - py);
        int db = (b.first - px) * (b.first - px) + (b.second - py) * (b.second - py);
        return da > db;
    });
    planX = px;
    planY = py;
}

void Color(float h, uint8_t* px, float sea) {
    float d = sea - h;
    float r, g, b;
    if (d <= 0) {
        float t = std::clamp(-d / 30.0f, 0.0f, 1.0f);
        r = 214 + (96 - 214) * t;
        g = 196 + (134 - 196) * t;
        b = 140 + (84 - 140) * t;
    } else {
        static const float stops[][4] = {{0, 120, 214, 214}, {15, 63, 170, 200}, {60, 36, 112, 170}, {200, 22, 66, 128},
                                         {600, 12, 36, 82},  {1500, 6, 18, 46}};
        int i = 0;
        while (i < 4 && d > stops[i + 1][0]) ++i;
        float t = std::clamp((d - stops[i][0]) / (stops[i + 1][0] - stops[i][0]), 0.0f, 1.0f);
        r = stops[i][1] + (stops[i + 1][1] - stops[i][1]) * t;
        g = stops[i][2] + (stops[i + 1][2] - stops[i][2]) * t;
        b = stops[i][3] + (stops[i + 1][3] - stops[i][3]) * t;
    }
    px[0] = (uint8_t)std::clamp(r, 0.0f, 255.0f);
    px[1] = (uint8_t)std::clamp(g, 0.0f, 255.0f);
    px[2] = (uint8_t)std::clamp(b, 0.0f, 255.0f);
    px[3] = 255;
}

const int kMapPx = kMapSize * 2;
const double kTileM = 256.0;
const int kTilePx = 256;
const int kTileOut = 64;

std::mutex tileLock;
std::unordered_map<int64_t, std::vector<uint8_t>> tileCache;
std::unordered_map<int64_t, bool> tileGood;
std::unordered_map<int64_t, ULONGLONG> tileBlankAt;
std::vector<std::pair<int64_t, bool>> badTiles;
std::set<int64_t> flatTiles;
std::future<void> mapJob;
std::wstring tileDirFor;

struct CaptureConfig {
    int mode;
    double height;
};
const CaptureConfig kConfigs[] = {{0, 40}, {0, 150}, {0, 600}, {2, 40}, {2, 150}, {2, 600}, {1, 40}, {1, 150}, {1, 600}};
const int kConfigCount = (int)(sizeof(kConfigs) / sizeof(kConfigs[0]));

struct CaptureRig {
    void* actor = nullptr;
    void* comp = nullptr;
    void* rt = nullptr;
    bool failed = false;
    ULONGLONG last = 0, lastHide = 0;
    int captured = 0, blanks = 0, blankRun = 0;
    int config = 0;
    bool locked = false;
    std::set<int> debugKept;
} rig;

void ApplyCaptureMode() {
    if (!ue::Alive(rig.comp)) return;
    void* cls = ue::ClassOf(rig.comp);
    static const uint8_t sources[] = {7, 2, 2};
    static const uint8_t unlitModes[] = {1, 1, 0};
    int m = std::clamp(kConfigs[std::clamp(rig.config, 0, kConfigCount - 1)].mode, 0, 2);
    ue::Write<uint8_t>(rig.comp, ue::FindProp(cls, "CaptureSource"), sources[m]);
    ue::Prop unlit = ue::FindProp(cls, "UnlitViewmode");
    if (unlit) ue::Write<uint8_t>(rig.comp, unlit, unlitModes[m]);
}

void ResetRig() {
    int config = rig.failed ? 0 : rig.config;
    bool locked = rig.failed ? false : rig.locked;
    rig = CaptureRig{};
    rig.config = config;
    rig.locked = locked;
}

std::wstring TileDir() {
    std::wstring w(surveyWorld.begin(), surveyWorld.end());
    return std::filesystem::path(iconDir).parent_path().wstring() + L"\\maptiles\\" + w;
}

std::wstring TileFile(int tx, int ty, bool good) {
    return std::to_wstring(tx) + L"_" + std::to_wstring(ty) + (good ? L".png" : L"_r.png");
}

void LoadTileIndex() {
    std::lock_guard<std::mutex> g(tileLock);
    tileCache.clear();
    tileGood.clear();
    tileBlankAt.clear();
    badTiles.clear();
    flatTiles.clear();
    tileDirFor = TileDir();
    std::error_code ec;
    std::filesystem::create_directories(tileDirFor, ec);
    for (const auto& e : std::filesystem::directory_iterator(tileDirFor, ec)) {
        if (e.path().extension() != L".png") continue;
        std::string stem = e.path().stem().string();
        bool good = stem.size() < 2 || stem.compare(stem.size() - 2, 2, "_r") != 0;
        int tx = 0, ty = 0;
        if (sscanf_s(stem.c_str(), "%d_%d", &tx, &ty) != 2) continue;
        int64_t k = Key(tx, ty);
        auto it = tileGood.find(k);
        tileGood[k] = good || (it != tileGood.end() && it->second);
    }
    LOG("map photos: %zu pieces saved for %s", tileGood.size(), surveyWorld.c_str());
}

bool Uniform(const unsigned char* px, int w, int h);

bool DecodeTile(const std::wstring& dir, int tx, int ty, bool good, std::vector<uint8_t>& out) {
    std::ifstream in(std::filesystem::path(dir) / TileFile(tx, ty, good), std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    if (Uniform(px, w, h)) {
        stbi_image_free(px);
        std::lock_guard<std::mutex> g(tileLock);
        if (tileDirFor == dir) badTiles.push_back({Key(tx, ty), good});
        return false;
    }
    out.assign((size_t)kTileOut * kTileOut * 4, 0);
    int fx = std::max(1, w / kTileOut), fy = std::max(1, h / kTileOut);
    for (int j = 0; j < kTileOut; ++j)
        for (int i = 0; i < kTileOut; ++i) {
            unsigned sum[4] = {0, 0, 0, 0};
            int cnt = 0, lit = 0;
            for (int y = j * fy; y < (j + 1) * fy && y < h; ++y)
                for (int x = i * fx; x < (i + 1) * fx && x < w; ++x) {
                    const unsigned char* p = px + ((size_t)y * w + x) * 4;
                    for (int c = 0; c < 3; ++c) sum[c] += p[c];
                    if (p[0] > 4 || p[1] > 4 || p[2] > 4) ++lit;
                    ++cnt;
                }
            uint8_t* o = out.data() + ((size_t)j * kTileOut + i) * 4;
            if (!cnt || lit * 2 < cnt) continue;
            for (int c = 0; c < 3; ++c) o[c] = (uint8_t)(sum[c] / cnt);
            o[3] = 255;
        }
    stbi_image_free(px);
    return true;
}

struct MapJobData {
    int x0 = 0, y0 = 0;
    float sea = 0;
    std::vector<int16_t> h;
};

void ComposeMap(MapJobData job) {
    const int16_t none = INT16_MIN;
    auto H = [&](int cx, int cy) -> int16_t {
        if (cx < 0 || cy < 0 || cx >= kMapSize || cy >= kMapSize) return none;
        return job.h[(size_t)cy * kMapSize + cx];
    };
    std::vector<uint8_t> img((size_t)kMapPx * kMapPx * 4, 0);
    for (int j = 0; j < kMapPx; ++j)
        for (int i = 0; i < kMapPx; ++i) {
            int cx = i / 2, cy = j / 2;
            int16_t hv = H(cx, cy);
            if (hv == none) continue;
            float hgt = hv / 10.0f;
            uint8_t* p = img.data() + ((size_t)j * kMapPx + i) * 4;
            Color(hgt, p, job.sea);
            auto at = [&](int x, int y) {
                int16_t v = H(x, y);
                return v == none ? hgt : v / 10.0f;
            };
            float hl = at(cx - 1, cy), hr = at(cx + 1, cy), hu = at(cx, cy - 1), hd = at(cx, cy + 1);
            float shade = std::clamp(1.0f + ((hl - hr) + (hu - hd)) / (float)(4 * kCell) * 0.9f, 0.55f, 1.35f);
            bool contour = std::floor(hgt / 25.0f) != std::floor(hr / 25.0f) || std::floor(hgt / 25.0f) != std::floor(hd / 25.0f);
            if (contour) shade *= 0.8f;
            for (int c = 0; c < 3; ++c) p[c] = (uint8_t)std::clamp(p[c] * shade, 0.0f, 255.0f);
        }
    double ox = (job.x0 - 0.5) * kCell, oy = (job.y0 - 0.5) * kCell;
    int tx0 = (int)std::floor(ox / kTileM), ty0 = (int)std::floor(oy / kTileM);
    int tx1 = (int)std::floor((ox + kMapPx * 4.0) / kTileM), ty1 = (int)std::floor((oy + kMapPx * 4.0) / kTileM);
    std::wstring dir;
    std::vector<std::pair<int64_t, bool>> wanted;
    {
        std::lock_guard<std::mutex> g(tileLock);
        dir = tileDirFor;
        for (int ty = ty0; ty <= ty1; ++ty)
            for (int tx = tx0; tx <= tx1; ++tx) {
                auto it = tileGood.find(Key(tx, ty));
                if (it != tileGood.end()) wanted.push_back({Key(tx, ty), it->second});
            }
    }
    int drawn = 0;
    for (const auto& [k, good] : wanted) {
        int tx = (int)(k >> 32), ty = (int)(int32_t)(uint32_t)k;
        std::vector<uint8_t> tile;
        {
            std::lock_guard<std::mutex> g(tileLock);
            auto c = tileCache.find(k);
            if (c != tileCache.end()) tile = c->second;
        }
        if (tile.empty()) {
            if (!DecodeTile(dir, tx, ty, good, tile)) continue;
            std::lock_guard<std::mutex> g(tileLock);
            auto now = tileGood.find(k);
            if (tileDirFor == dir && now != tileGood.end() && now->second == good) tileCache[k] = tile;
        }
        int px0 = (int)std::lround((tx * kTileM - ox) / 4.0), py0 = (int)std::lround((ty * kTileM - oy) / 4.0);
        for (int j = 0; j < kTileOut; ++j) {
            int y = py0 + j;
            if (y < 0 || y >= kMapPx) continue;
            for (int i = 0; i < kTileOut; ++i) {
                int x = px0 + i;
                if (x < 0 || x >= kMapPx) continue;
                const uint8_t* s = tile.data() + ((size_t)j * kTileOut + i) * 4;
                if (!s[3]) continue;
                uint8_t* d = img.data() + ((size_t)y * kMapPx + x) * 4;
                int16_t hv = H(x / 2, y / 2);
                float t = 0.15f;
                if (hv != none) t = std::clamp((job.sea - hv / 10.0f) / 500.0f, 0.0f, 0.55f);
                const float deep[3] = {18, 52, 96};
                for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::clamp(s[c] * (1.0f - t) + deep[c] * t, 0.0f, 255.0f);
                d[3] = 255;
            }
        }
        ++drawn;
    }
    {
        std::lock_guard<std::mutex> g(*modelLock);
        model->mapMinX = (float)ox;
        model->mapMinY = (float)oy;
        model->mapMaxX = (float)(ox + kMapPx * 4.0);
        model->mapMaxY = (float)(oy + kMapPx * 4.0);
        model->mapPhotos = drawn;
    }
    overlay::SetMapImage(std::move(img), kMapPx, kMapPx);
}

void BuildMapImage() {
    if (mapCx == INT32_MIN || shuttingDown) return;
    if (mapJob.valid() && mapJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    MapJobData job;
    job.x0 = mapCx - kMapSize / 2;
    job.y0 = mapCy - kMapSize / 2;
    job.sea = seaLevel;
    job.h.assign((size_t)kMapSize * kMapSize, INT16_MIN);
    for (const auto& [k, hv] : survey) {
        int i = (int)(k >> 32) - job.x0, j = (int)(int32_t)(uint32_t)k - job.y0;
        if (i < 0 || j < 0 || i >= kMapSize || j >= kMapSize) continue;
        job.h[(size_t)j * kMapSize + i] = hv;
    }
    mapJob = std::async(std::launch::async, [job = std::move(job)]() mutable {
        try {
            ComposeMap(std::move(job));
        } catch (...) {
            LOG("map image failed");
        }
    });
    imageDirty = false;
}

bool SetupRig() {
    if (rig.failed) return false;
    if (ue::Alive(rig.actor) && ue::Alive(rig.comp) && ue::Alive(rig.rt)) return true;
    ResetRig();
    void* actor = SpawnSimpleActor("/Script/Engine.SceneCapture2D", ue::FVector{0, 0, (seaLevel - 50.0) * 100.0}, 0);
    void* comp = actor ? ue::Ptr(actor, "CaptureComponent2D") : nullptr;
    void* rl = Cdo("/Script/Engine.Default__KismetRenderingLibrary");
    void* rt = nullptr;
    if (comp) {
        ue::Call mk(rl, "CreateRenderTarget2D");
        float clear[4] = {0, 0, 0, 0};
        mk.Set<void*>("WorldContextObject", st.world).Set<int32_t>("Width", kTilePx).Set<int32_t>("Height", kTilePx).Set<uint8_t>("Format", 3);
        mk.SetRaw("ClearColor", clear, sizeof(clear)).SetBool("bAutoGenerateMipMaps", false).SetBool("bSupportUAVs", false);
        rt = mk.Run() ? mk.Get<void*>("ReturnValue") : nullptr;
    }
    if (!ue::Alive(actor) || !ue::Alive(comp) || !ue::Alive(rt)) {
        LOG("map photos: could not set up the camera (actor %p, capture %p, target %p)", actor, comp, rt);
        if (ue::Alive(actor)) ue::Call(actor, "K2_DestroyActor").Run();
        rig.failed = true;
        return false;
    }
    void* cls = ue::ClassOf(comp);
    ue::Write<void*>(comp, ue::FindProp(cls, "TextureTarget"), rt);
    ue::Write<uint8_t>(comp, ue::FindProp(cls, "ProjectionType"), 1);
    ue::Write<float>(comp, ue::FindProp(cls, "OrthoWidth"), (float)(kTileM * 100.0));

    ue::WriteBool(comp, ue::FindProp(cls, "bCaptureEveryFrame"), false);
    ue::WriteBool(comp, ue::FindProp(cls, "bCaptureOnMovement"), false);
    ue::WriteBool(comp, ue::FindProp(cls, "bAutoCalculateOrthoPlanes"), false);
    ue::WriteBool(comp, ue::FindProp(cls, "bAlwaysPersistRenderingState"), false);
    struct FlagSetting {
        ue::FString name;
        bool enabled;
        uint8_t pad[7];
    };
    static const wchar_t* off[] = {L"Fog", L"VolumetricFog", L"Atmosphere", L"Cloud", L"Particles", L"SkeletalMeshes", L"Translucency",
                                   L"LightShafts", L"PostProcessing", L"WaterMesh", L"Bloom", L"EyeAdaptation", L"MotionBlur"};
    std::vector<FlagSetting> flags;
    for (const wchar_t* n : off) flags.push_back({ue::FString{n, (int32_t)std::wcslen(n) + 1, (int32_t)std::wcslen(n) + 1}, false, {}});
    ue::TArray<FlagSetting> arr{flags.data(), (int32_t)flags.size(), (int32_t)flags.size()};
    ue::Call sf(comp, "SetShowFlagSettings");
    sf.SetRaw("InShowFlagSettings", &arr, sizeof(arr));
    sf.Run();
    rig.actor = actor;
    rig.comp = comp;
    rig.rt = rt;
    ApplyCaptureMode();
    LOG("map photos: camera ready (setting %d of %d%s)", rig.config + 1, kConfigCount, rig.locked ? ", already tested" : "");
    return true;
}

std::unordered_map<void*, bool> hideMemo;

bool HideClass(void* cls) {
    auto it = hideMemo.find(cls);
    if (it != hideMemo.end()) return it->second;
    bool hide = false;
    for (void* c = cls; c && !hide; c = ue::SuperOf(c)) {
        std::string n = ue::Name(c);
        if (n == "WaterBody" || n == "WaterZone" || n == "UWESkyActor" || n == "VolumetricCloud" || n == "ExponentialHeightFog" ||
            n == "LocalFogVolume" || n == "SkyAtmosphere")
            hide = true;
        if (n.find("Sky") != std::string::npos && n.rfind("BP_", 0) == 0) hide = true;
    }
    hideMemo[cls] = hide;
    return hide;
}

void HideForPhotos() {
    void* actorCls = ue::FindObject("/Script/Engine.Actor");
    int32_t count = ue::ObjectCount();
    int hidden = 0;
    for (int32_t i = 0; i < count; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o || ue::IsDefault(o)) continue;
        void* cls = ue::ClassOf(o);
        if (!cls || !HideClass(cls) || !ue::IsA(o, actorCls) || !ue::Alive(o)) continue;
        ue::Call h(rig.comp, "HideActorComponents");
        h.Set<void*>("InActor", o).SetBool("bIncludeFromChildActors", true);
        if (h.Run()) ++hidden;
    }
    LOG("map photos: hid %d water and sky actors", hidden);
}

bool Uniform(const unsigned char* px, int w, int h) {
    size_t total = (size_t)w * h;
    if (!total) return true;
    double sum[3] = {0, 0, 0};
    for (size_t i = 0; i < total; ++i)
        for (int c = 0; c < 3; ++c) sum[c] += px[i * 4 + c];
    size_t close = 0;
    for (size_t i = 0; i < total; ++i) {
        bool same = true;
        for (int c = 0; c < 3 && same; ++c) same = std::fabs(px[i * 4 + c] - sum[c] / total) <= 4.0;
        if (same) ++close;
    }
    return close * 100 >= total * 97;
}

int TileBlank(const std::wstring& file) {
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.empty()) return 1;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!px) return 1;
    size_t lit = 0, total = (size_t)w * h;
    for (size_t i = 0; i < total; ++i)
        if (px[i * 4] > 4 || px[i * 4 + 1] > 4 || px[i * 4 + 2] > 4) ++lit;
    bool flat = Uniform(px, w, h);
    stbi_image_free(px);
    if (lit * 50 < total) return 1;
    return flat ? 2 : 0;
}

const int kCellsPerTile = (int)(kTileM / kCell);

void StoreSurvey(int cx, int cy, float h) {
    survey[Key(cx, cy)] = (int16_t)std::clamp(h * 10.0f, -32000.0f, 32000.0f);
    surveyDirty = imageDirty = true;
}

bool TileTop(int tx, int ty, double& top, bool probe) {
    int cx0 = tx * kCellsPerTile, cy0 = ty * kCellsPerTile;
    int found = 0;
    float best = -1e9f;
    for (int j = 0; j < kCellsPerTile; ++j)
        for (int i = 0; i < kCellsPerTile; ++i) {
            auto it = survey.find(Key(cx0 + i, cy0 + j));
            if (it == survey.end()) continue;
            best = std::max(best, it->second / 10.0f);
            ++found;
        }
    if (found < 64 && probe && st.world) {
        static const int offs[] = {2, 9, 16, 23, 30};
        for (int oy : offs)
            for (int ox : offs) {
                int cx = cx0 + ox, cy = cy0 + oy;
                if (survey.count(Key(cx, cy))) continue;
                float h;
                if (!SurveyPoint(cx, cy, h)) continue;
                StoreSurvey(cx, cy, h);
                best = std::max(best, h);
                ++found;
            }
    }
    if (!found) return false;
    top = best;
    return true;
}

bool LiveTop(int tx, int ty, double& top) {
    int cx0 = tx * kCellsPerTile, cy0 = ty * kCellsPerTile, hits = 0;
    float best = -1e9f;
    static const int offs[3] = {6, 16, 26};
    for (int oy : offs)
        for (int ox : offs) {
            float h;
            if (!SurveyPoint(cx0 + ox, cy0 + oy, h)) continue;
            StoreSurvey(cx0 + ox, cy0 + oy, h);
            best = std::max(best, h);
            ++hits;
        }
    if (!hits) return false;
    double known;
    top = TileTop(tx, ty, known, false) ? std::max((double)best, known) : best;
    return true;
}

int CaptureTile(int tx, int ty, bool good, double top, bool trusted) {
    if (!SetupRig()) return 0;
    ULONGLONG now = GetTickCount64();
    if (!rig.lastHide || now - rig.lastHide > (MapScanActive() ? 60000u : 600000u)) {
        rig.lastHide = now;
        HideForPhotos();
    }
    int config = std::clamp(rig.config, 0, kConfigCount - 1);
    ue::FVector at{(tx + 0.5) * kTileM * 100.0, (ty + 0.5) * kTileM * 100.0, (top + kConfigs[config].height) * 100.0};
    ue::Call mv(rig.actor, "K2_SetActorLocationAndRotation");
    mv.Set<ue::FVector>("NewLocation", at).Set<ue::FRotator>("NewRotation", ue::FRotator{-90.0, -90.0, 0.0});
    mv.SetBool("bSweep", false).SetBool("bTeleport", true);
    if (!mv.Run()) return 0;
    if (!ue::Call(rig.comp, "CaptureScene").Run()) return 0;
    std::wstring dir = TileDir(), name = TileFile(tx, ty, good);
    ue::Call exp(Cdo("/Script/Engine.Default__KismetRenderingLibrary"), "ExportRenderTarget");
    exp.Set<void*>("WorldContextObject", st.world).Set<void*>("TextureRenderTarget", rig.rt);
    exp.SetString("FilePath", dir).SetString("FileName", name);
    if (!exp.Run()) return 0;
    std::wstring file = dir + L"\\" + name;
    int64_t k = Key(tx, ty);
    int blank = TileBlank(file);
    if (blank == 2) flatTiles.insert(k);
    if (blank) {
        std::error_code ec;
        bool kept = false;
        if (blank == 1 && !rig.locked && rig.debugKept.insert(config).second) {
            std::filesystem::path dbg = std::filesystem::path(dir) / L"debug";
            std::filesystem::create_directories(dbg, ec);
            wchar_t nm[96];
            swprintf_s(nm, L"setting%d_mode%d_%dm_%d_%d.png", config + 1, kConfigs[config].mode, (int)kConfigs[config].height, tx, ty);
            ec.clear();
            std::filesystem::rename(std::filesystem::path(file), dbg / nm, ec);
            kept = !ec;
        }
        if (!kept) std::filesystem::remove(std::filesystem::path(file), ec);
        ++rig.blanks;
        tileBlankAt[k] = now;
        if (!rig.locked && trusted && blank == 1 && ++rig.blankRun >= 3) {
            rig.blankRun = 0;
            if (rig.config + 1 >= kConfigCount) {
                LOG("map photos: every camera setting came out empty, keeping the depth map");
                rig.failed = true;
            } else {
                ++rig.config;
                tileBlankAt.clear();
                ApplyCaptureMode();
                LOG("map photos: pictures came out empty, trying setting %d (mode %d, %.0f m above the ground)", rig.config + 1,
                    kConfigs[rig.config].mode, kConfigs[rig.config].height);
            }
        }
        return blank == 2 ? 3 : 2;
    }
    if (!rig.locked) {
        rig.locked = true;
        LOG("map photos: setting %d works (mode %d, %.0f m above the ground)", config + 1, kConfigs[config].mode, kConfigs[config].height);
    }
    rig.blankRun = 0;
    ++rig.captured;
    if (good) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(dir) / TileFile(tx, ty, false), ec);
    }
    {
        std::lock_guard<std::mutex> g(tileLock);
        tileGood[k] = good;
        tileCache.erase(k);
    }
    if (rig.captured == 1 || rig.captured % 25 == 0) LOG("map photos: %d taken, %d empty", rig.captured, rig.blanks);
    imageDirty = true;
    return 1;
}

void DropBadTiles() {
    std::vector<std::pair<int64_t, bool>> list;
    std::wstring dir;
    {
        std::lock_guard<std::mutex> g(tileLock);
        list.swap(badTiles);
        dir = tileDirFor;
    }
    for (const auto& [k, good] : list) {
        int tx = (int)(k >> 32), ty = (int)(int32_t)(uint32_t)k;
        std::wstring file = (std::filesystem::path(dir) / TileFile(tx, ty, good)).wstring();
        if (TileBlank(file) != 2) continue;
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(file), ec);
        flatTiles.insert(k);
        std::lock_guard<std::mutex> g(tileLock);
        auto it = tileGood.find(k);
        if (it != tileGood.end() && it->second == good) {
            tileGood.erase(it);
            tileCache.erase(k);
        }
    }
}

void PumpPhotos(const ue::FVector& me) {
    DropBadTiles();
    if (rig.failed || !MapVisible()) return;
    ULONGLONG now = GetTickCount64();
    if (now - rig.last < 400u) return;
    rig.last = now;
    int ptx = (int)std::floor(me.x / 100.0 / kTileM), pty = (int)std::floor(me.y / 100.0 / kTileM);
    const int radius = 5;
    int bestX = 0, bestY = 0, bestD = INT_MAX;
    bool bestGood = false;
    {
        std::lock_guard<std::mutex> g(tileLock);
        for (int dy = -radius; dy <= radius; ++dy)
            for (int dx = -radius; dx <= radius; ++dx) {
                int d = dx * dx + dy * dy;
                if (d > radius * radius || d >= bestD) continue;
                bool nearPlayer = d <= 4;
                int64_t k = Key(ptx + dx, pty + dy);
                auto it = tileGood.find(k);
                if (it != tileGood.end() && (it->second || !nearPlayer)) continue;
                auto blank = tileBlankAt.find(k);
                if (blank != tileBlankAt.end() && now - blank->second < 300000) continue;
                if (flatTiles.count(k)) continue;
                bestD = d;
                bestX = ptx + dx;
                bestY = pty + dy;
                bestGood = nearPlayer;
            }
    }
    if (bestD == INT_MAX) return;
    double top;
    if (!LiveTop(bestX, bestY, top)) {
        tileBlankAt[Key(bestX, bestY)] = now;
        return;
    }
    if (CaptureTile(bestX, bestY, bestGood, top, bestD <= 2) == 0) {
        LOG("map photos: taking a picture failed, stopping");
        rig.failed = true;
    }
}

struct MapRun {
    bool active = false, returning = false;
    void* cam = nullptr;
    void* pc = nullptr;
    void* pawn = nullptr;
    void* move = nullptr;
    uint8_t savedMode = 0, savedCustom = 0;
    std::vector<std::pair<int, int>> tiles;
    size_t next = 0;
    int phase = 0, cell = 0;
    ULONGLONG phaseAt = 0, startedAt = 0, lastShown = 0, lastView = 0, lastCare = 0;
    int done = 0, empty = 0, work = 0, photos = 0;
    double camZ = 0;
    Attr vitals[3] = {};
    bool haveVitals = false;
    float health = -1;
} mapRun;

const char* kVitals[3] = {"Oxygen", "Food", "Water"};

bool MapScanActive() { return mapRun.active; }

void ShowScan(bool on, float progress, const std::string& text) {
    std::lock_guard<std::mutex> g(*modelLock);
    model->mapScanning = on;
    model->mapScanProgress = progress;
    model->mapScanText = text;
}

void* PartitionSubsystem() {
    static void* cls = nullptr;
    if (!ue::Valid(cls)) cls = ue::FindObject("/Script/Engine.WorldPartitionSubsystem");
    if (!cls || !st.world) return nullptr;
    ue::Call c(Cdo("/Script/Engine.Default__SubsystemBlueprintLibrary"), "GetWorldSubsystem");
    c.Set<void*>("ContextObject", st.world).Set<void*>("Class", cls);
    void* s = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    return ue::Alive(s) ? s : nullptr;
}

bool StreamingDone() {
    void* s = PartitionSubsystem();
    if (!s) return true;
    ue::Call c(s, "IsAllStreamingCompleted");
    return c.Run() && c.GetBool("ReturnValue");
}

void ScanArea(const ue::FVector& me, double& x0, double& y0, double& x1, double& y1) {
    auto fits = [](double a0, double b0, double a1, double b1) {
        return a1 - a0 > 50000.0 && b1 - b0 > 50000.0 && a1 - a0 < 3000000.0 && b1 - b0 < 3000000.0;
    };
    void* edgeCls = ue::FindObject("/Script/UWEEdgeOfWorld.UWEEdgeOfWorldSpline");
    void* edge = edgeCls ? ue::FindFirstOf(edgeCls) : nullptr;
    if (ue::Alive(edge)) {
        ue::Call b(edge, "GetActorBounds");
        b.SetBool("bOnlyCollidingComponents", false).SetBool("bIncludeFromChildActors", true);
        if (b.Run()) {
            ue::FVector o = b.Get<ue::FVector>("Origin"), e = b.Get<ue::FVector>("BoxExtent");
            if (fits(o.x - e.x, o.y - e.y, o.x + e.x, o.y + e.y)) {
                x0 = o.x - e.x;
                y0 = o.y - e.y;
                x1 = o.x + e.x;
                y1 = o.y + e.y;
                LOG("map scan: world edge spans %.0f x %.0f m", (x1 - x0) / 100.0, (y1 - y0) / 100.0);
                return;
            }
        }
    }
    ue::Call w(Cdo("/Script/Engine.Default__WorldPartitionBlueprintLibrary"), "GetRuntimeWorldBounds");
    uint8_t box[0x40] = {};
    int size = w.ParamSize("ReturnValue");
    if (size >= 49 && size <= (int)sizeof(box) && w.Run() && w.GetRaw("ReturnValue", box, size) && box[48]) {
        const double* d = (const double*)box;
        if (fits(d[0], d[1], d[3], d[4])) {
            x0 = d[0];
            y0 = d[1];
            x1 = d[3];
            y1 = d[4];
            LOG("map scan: world spans %.0f x %.0f m", (x1 - x0) / 100.0, (y1 - y0) / 100.0);
            return;
        }
    }
    x0 = me.x - 300000.0;
    y0 = me.y - 300000.0;
    x1 = me.x + 300000.0;
    y1 = me.y + 300000.0;
    LOG("map scan: world size unknown, using 3 km around the player");
}

void MoveScanCam(int tx, int ty) {
    ue::Call c(mapRun.cam, "K2_SetActorLocationAndRotation");
    c.Set<ue::FVector>("NewLocation", ue::FVector{(tx + 0.5) * kTileM * 100.0, (ty + 0.5) * kTileM * 100.0, mapRun.camZ * 100.0});
    c.Set<ue::FRotator>("NewRotation", ue::FRotator{-90.0, -90.0, 0.0});
    c.SetBool("bSweep", false).SetBool("bTeleport", true);
    c.Run();
}

void EndMapScan() {
    if (!mapRun.active) return;
    if (spotsDirty) SaveSpots();
    if (ue::Alive(mapRun.pc)) {
        void* back = ControlledPawn(mapRun.pc);
        if (back) SetViewTarget(mapRun.pc, back);
        SetIgnore(mapRun.pc, false, false);
        SetIgnore(mapRun.pc, true, false);
    }
    if (ue::Alive(mapRun.move) && mapRun.savedMode != 0) {
        uint8_t mode = mapRun.savedMode, custom = mapRun.savedCustom;
        if (mode == 5) {
            mode = 3;
            custom = 0;
        }
        ue::Call m(mapRun.move, "SetMovementMode");
        m.Set<uint8_t>("NewMovementMode", mode).Set<uint8_t>("NewCustomMode", mode == 6 ? custom : (uint8_t)0);
        m.Run();
    }
    if (ue::Alive(mapRun.cam)) ue::Call(mapRun.cam, "K2_DestroyActor").Run();
    LOG("map scan ended: %d of %zu pieces done, %d new pictures, %d empty", mapRun.done, mapRun.tiles.size(), mapRun.photos, mapRun.empty);
    mapRun = MapRun{};
    scanForce = false;
    ShowScan(false, 0, "");
}

void StopMapScan() {
    if (!mapRun.active || mapRun.returning) return;
    mapRun.returning = true;
    mapRun.phaseAt = GetTickCount64();
    if (ue::Alive(mapRun.pc) && ue::Alive(mapRun.pawn)) SetViewTarget(mapRun.pc, mapRun.pawn);
    ShowScan(true, 1.0f, "Coming back to your character...");
}

void StartMapScan() {
    if (mapRun.active) return;
    if (!st.inWorld || !st.pawn || !st.pc || !st.world || st.session == Session::Client) {
        Note("Load your save first");
        return;
    }
    if (st.vehicle) {
        Note("Get out of the vehicle first");
        return;
    }
    ue::FVector me;
    if (!st.charMove || !ActorPos(st.pawn, me) || surveyWorld.empty()) {
        Note("The map scan can't start right now");
        return;
    }
    if (rig.failed) {
        rig.failed = false;
        rig.locked = false;
        rig.config = 0;
        rig.blankRun = 0;
        rig.debugKept.clear();
        tileBlankAt.clear();
        ApplyCaptureMode();
    }
    if (!SetupRig()) {
        Note("The map camera could not be set up");
        return;
    }
    double x0, y0, x1, y1;
    ScanArea(me, x0, y0, x1, y1);
    int tx0 = (int)std::floor(x0 / 100.0 / kTileM), tx1 = (int)std::floor(x1 / 100.0 / kTileM);
    int ty0 = (int)std::floor(y0 / 100.0 / kTileM), ty1 = (int)std::floor(y1 / 100.0 / kTileM);
    if ((int64_t)(tx1 - tx0 + 1) * (ty1 - ty0 + 1) > 4000) {
        int px = (int)std::floor(me.x / 100.0 / kTileM), py = (int)std::floor(me.y / 100.0 / kTileM);
        tx0 = std::max(tx0, px - 16);
        tx1 = std::min(tx1, px + 16);
        ty0 = std::max(ty0, py - 16);
        ty1 = std::min(ty1, py + 16);
    }
    void* eow = Cdo("/Script/UWEEdgeOfWorld.Default__UWEEdgeOfWorldStatics");
    bool useEdge = false;
    if (eow) {
        ue::Call en(eow, "IsEdgeOfWorldEnabled");
        en.Set<void*>("WorldContextObject", st.world);
        useEdge = en.Run() && en.GetBool("ReturnValue");
    }
    auto inside = [&](double x, double y) {
        ue::Call c(eow, "IsPointWithinEdgeOfWorld");
        c.Set<void*>("WorldContextObject", st.world).Set<ue::FVector>("Point", ue::FVector{x, y, seaLevel * 100.0});
        return c.Run() && c.GetBool("ReturnValue");
    };
    std::vector<std::pair<int, int>> tiles;
    for (int pass = 0; pass < 2 && tiles.empty(); ++pass) {
        bool filter = useEdge && pass == 0;
        for (int ty = ty0; ty <= ty1; ++ty) {
            std::vector<std::pair<int, int>> row;
            for (int tx = tx0; tx <= tx1; ++tx) {
                if (filter) {
                    double cx = (tx + 0.5) * kTileM * 100.0, cy = (ty + 0.5) * kTileM * 100.0, h = kTileM * 50.0;
                    if (!inside(cx, cy) && !inside(cx - h, cy - h) && !inside(cx + h, cy - h) && !inside(cx - h, cy + h) && !inside(cx + h, cy + h))
                        continue;
                }
                row.push_back({tx, ty});
            }
            if ((ty - ty0) % 2) std::reverse(row.begin(), row.end());
            tiles.insert(tiles.end(), row.begin(), row.end());
        }
    }
    if (tiles.empty()) {
        Note("Could not work out the size of the map");
        return;
    }
    StopFreeCam();
    StopNoClip();
    void* cam = SpawnSimpleActor("/Script/Engine.CameraActor", ue::FVector{me.x, me.y, me.z + 5000.0}, 0);
    if (!cam) {
        Note("Could not create the map camera");
        return;
    }
    mapRun = MapRun{};
    mapRun.active = true;
    mapRun.cam = cam;
    mapRun.pc = st.pc;
    mapRun.pawn = st.pawn;
    mapRun.move = st.charMove;
    mapRun.tiles = std::move(tiles);
    mapRun.startedAt = mapRun.phaseAt = GetTickCount64();
    mapRun.camZ = seaLevel;
    void* mcls = ue::ClassOf(st.charMove);
    ue::Prop modeP = ue::FindProp(mcls, "MovementMode"), customP = ue::FindProp(mcls, "CustomMovementMode");
    mapRun.savedMode = modeP ? ue::Read<uint8_t>(st.charMove, modeP) : 3;
    mapRun.savedCustom = customP ? ue::Read<uint8_t>(st.charMove, customP) : 0;
    ue::Call m(st.charMove, "SetMovementMode");
    m.Set<uint8_t>("NewMovementMode", 0).Set<uint8_t>("NewCustomMode", 0);
    m.Run();
    ue::Prop vel = ue::FindProp(mcls, "Velocity");
    if (vel) ue::Write<ue::FVector>(st.charMove, vel, ue::FVector{});
    SetIgnore(st.pc, false, true);
    SetIgnore(st.pc, true, true);
    if (st.survivalSet) {
        for (int i = 0; i < 3; ++i) mapRun.vitals[i] = ReadAttr(st.survivalSet, kVitals[i]);
        mapRun.haveVitals = true;
    }
    mapRun.health = st.health ? CallFloat(st.health, "GetHealth") : -1.0f;
    scanForce = true;
    SetViewTarget(st.pc, cam);
    LOG("map scan started: %zu pieces of %.0f m, edge check %s", mapRun.tiles.size(), kTileM, useEdge ? "on" : "off");
    ShowScan(true, 0, "Starting the map scan...");
}

void KeepFrozen(ULONGLONG now) {
    if (ue::Alive(mapRun.move)) {
        ue::Prop modeP = ue::FindProp(ue::ClassOf(mapRun.move), "MovementMode");
        if (modeP && ue::Read<uint8_t>(mapRun.move, modeP) != 0) {
            ue::Call m(mapRun.move, "SetMovementMode");
            m.Set<uint8_t>("NewMovementMode", 0).Set<uint8_t>("NewCustomMode", 0);
            m.Run();
        }
    }
    if (!mapRun.returning && now - mapRun.lastView > 500) {
        mapRun.lastView = now;
        ue::Call vt(mapRun.pc, "GetViewTarget");
        if (vt.Run() && vt.Get<void*>("ReturnValue") != mapRun.cam) SetViewTarget(mapRun.pc, mapRun.cam);
    }
}

void AdvanceScan() {
    ++mapRun.next;
    ++mapRun.done;
    mapRun.phase = 0;
}

void TickMapScan() {
    if (!mapRun.active) return;
    ULONGLONG now = GetTickCount64();
    if (!ue::Alive(mapRun.pawn) || !ue::Alive(mapRun.pc) || !ue::Alive(mapRun.cam) || st.pawn != mapRun.pawn || !st.world) {
        LOG("map scan stopped: the player or the camera went away");
        EndMapScan();
        return;
    }
    KeepFrozen(now);
    if (now - mapRun.lastCare > 1000) {
        mapRun.lastCare = now;
        if (mapRun.haveVitals && st.survivalSet)
            for (int i = 0; i < 3; ++i)
                if (ReadAttr(st.survivalSet, kVitals[i]).cur < mapRun.vitals[i].cur - 0.01f) WriteAttr(st.survivalSet, kVitals[i], mapRun.vitals[i]);
        if (st.health && mapRun.health > 0 && CallFloat(st.health, "GetHealth") < mapRun.health - 0.01f) ue::Call(st.health, "RestoreHealth").Run();
    }
    if (mapRun.returning) {
        if (now - mapRun.phaseAt < 1000) return;
        bool walking = mapRun.savedMode == 1 || mapRun.savedMode == 2;
        ue::FVector at;
        bool ground = !walking || (ActorPos(mapRun.pawn, at) && LineHit(at, ue::FVector{at.x, at.y, at.z - 600.0}, mapRun.pawn, nullptr));
        if ((!StreamingDone() || !ground) && now - mapRun.phaseAt < 30000) return;
        Note(mapRun.next >= mapRun.tiles.size() ? "Map scan finished" : "Map scan stopped");
        EndMapScan();
        return;
    }
    size_t total = mapRun.tiles.size();
    if (now - mapRun.lastShown > 250) {
        mapRun.lastShown = now;
        double elapsed = (now - mapRun.startedAt) / 1000.0;
        double per = mapRun.work >= 3 ? elapsed / mapRun.work : 1.5;
        int minutes = (int)std::ceil((double)(total - std::min(mapRun.next, total)) * per / 60.0);
        std::string left = minutes <= 1 ? "less than a minute left" : "about " + std::to_string(minutes) + " min left";
        ShowScan(true, total ? (float)mapRun.next / (float)total : 1.0f,
                 "Piece " + std::to_string(std::min(mapRun.next + 1, total)) + " of " + std::to_string(total) + "  \xc2\xb7  " + left);
    }
    if (mapRun.phase == 0) {
        int skipped = 0;
        while (mapRun.next < total && skipped < 400) {
            auto [tx, ty] = mapRun.tiles[mapRun.next];
            bool good;
            {
                std::lock_guard<std::mutex> g(tileLock);
                auto it = tileGood.find(Key(tx, ty));
                good = it != tileGood.end() && it->second;
            }
            if (!good) break;
            ++mapRun.next;
            ++mapRun.done;
            ++skipped;
        }
        if (mapRun.next >= total) {
            LOG("map scan: all pieces done");
            StopMapScan();
            return;
        }
        if (skipped >= 400) return;
        auto [tx, ty] = mapRun.tiles[mapRun.next];
        MoveScanCam(tx, ty);
        mapRun.phase = 1;
        mapRun.phaseAt = now;
        mapRun.cell = 0;
        return;
    }
    auto [tx, ty] = mapRun.tiles[mapRun.next];
    if (mapRun.phase == 1) {
        if (now - mapRun.phaseAt < 900) return;
        if (!StreamingDone() && now - mapRun.phaseAt < 8000) return;
        mapRun.phase = 2;
        return;
    }
    if (mapRun.phase == 2) {
        int cx0 = tx * kCellsPerTile, cy0 = ty * kCellsPerTile, cells = kCellsPerTile * kCellsPerTile, budget = 192;
        while (mapRun.cell < cells && budget > 0) {
            int cx = cx0 + mapRun.cell % kCellsPerTile, cy = cy0 + mapRun.cell / kCellsPerTile;
            ++mapRun.cell;
            if (survey.count(Key(cx, cy))) continue;
            --budget;
            float h;
            if (SurveyPoint(cx, cy, h)) StoreSurvey(cx, cy, h);
        }
        if (mapRun.cell < cells) return;
        mapRun.phase = 3;
        mapRun.phaseAt = now;
        return;
    }
    if (now - mapRun.phaseAt < 150) return;
    mapRun.phaseAt = now;
    double top;
    if (!TileTop(tx, ty, top, false)) {
        ++mapRun.empty;
        ++mapRun.work;
        AdvanceScan();
        return;
    }
    mapRun.camZ = top + 120.0;
    int r = CaptureTile(tx, ty, true, top, true);
    if (r == 0 || rig.failed) {
        Note("Map pictures don't work here, the scan stopped");
        LOG("map scan: pictures failed, stopping");
        StopMapScan();
        return;
    }
    if (r == 2 && !rig.locked) return;
    if (r == 1) ++mapRun.photos;
    else ++mapRun.empty;
    ++mapRun.work;
    AdvanceScan();
}

void Survey() {
    if (!st.inWorld || !st.world || st.session == Session::Client) return;
    ULONGLONG now = GetTickCount64();
    if (now - lastSurvey < 30) return;
    std::string world = ue::Name(st.world);
    if (world != surveyWorld) {
        SaveSurvey();
        surveyWorld = world;
        LoadSurvey();
        LoadTileIndex();
        mapCx = INT32_MIN;
        planX = INT32_MIN;
    }
    if (!seaKnown) {
        ue::Call c(Cdo("/Script/UWEGameConfiguration.Default__UWEGameConfigurationStatics"), "GetGlobalOceanSeaLevel");
        if (c.Run()) seaLevel = c.Get<float>("ReturnValue") / 100.0f;
        seaKnown = true;
        LOG("sea level %.1f m", seaLevel);
    }
    static std::string cleanedFor;
    if (cleanedFor != surveyWorld) {
        cleanedFor = surveyWorld;
        std::error_code cleanEc;
        if (!std::filesystem::exists(SurveyFile() + L".clean", cleanEc)) {
            std::ofstream(SurveyFile() + L".clean") << "1";
            int16_t flat = (int16_t)std::lround(seaLevel * 10.0f);
            size_t removed = 0;
            for (auto it = survey.begin(); it != survey.end();) {
                if (it->second == flat) {
                    it = survey.erase(it);
                    ++removed;
                } else {
                    ++it;
                }
            }
            if (removed) {
                surveyDirty = imageDirty = true;
                LOG("map: removed %zu flat spots that were never really measured", removed);
            }
        }
    }
    if (now - lastSurvey < 30) return;
    lastSurvey = now;
    ue::FVector me;
    if (!ActorPos(st.vehicle ? st.vehicle : st.pawn, me)) return;
    int px = (int)std::floor(me.x / 100.0 / kCell + 0.5), py = (int)std::floor(me.y / 100.0 / kCell + 0.5);
    if (mapCx == INT32_MIN || std::abs(px - mapCx) > kMapSize / 2 - 80 || std::abs(py - mapCy) > kMapSize / 2 - 80) {
        mapCx = (int)std::floor(px / 64.0 + 0.5) * 64;
        mapCy = (int)std::floor(py / 64.0 + 0.5) * 64;
        imageDirty = true;
    }
    static ULONGLONG lastPlan = 0;
    if (!mapRun.active) {
        if (planX == INT32_MIN || std::abs(px - planX) > 8 || std::abs(py - planY) > 8 || (surveyPlan.empty() && now - lastPlan > 5000)) {
            lastPlan = now;
            PlanSurvey(px, py);
        }
        int budget = 32;
        while (budget-- > 0 && !surveyPlan.empty()) {
            auto [cx, cy] = surveyPlan.back();
            surveyPlan.pop_back();
            int64_t k = Key(cx, cy);
            if (survey.count(k)) continue;
            float h;
            if (SurveyPoint(cx, cy, h)) StoreSurvey(cx, cy, h);
            else ++surveyTries[k];
        }
        PumpPhotos(me);
    }
    if (imageDirty && now - lastImage > 4000 && MapVisible()) {
        lastImage = now;
        BuildMapImage();
    }
    if (surveyDirty && now - lastSurveySave > 60000) {
        lastSurveySave = now;
        SaveSurvey();
    }
}

std::vector<std::string> iconQueue;
bool itemsLoaded = false;
void* iconRt = nullptr;
ULONGLONG lastIcon = 0;
int iconsMade = 0;

bool Hidden(const std::string& id) {
    std::string t = id;
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    for (const char* k : {"test", "greeble", "noequipslot", "stackable", "weldtarget", "scrapdeconstruct", "_deconstructable",
                          "inventorytest", "buildingproject", "display_timeofday"})
        if (t.find(k) != std::string::npos) return true;
    return false;
}

std::string CategoryFor(const std::string&, const std::string& id) {
    std::string t = id;
    for (char& c : t) c = (char)std::tolower((unsigned char)c);
    auto any = [&](std::initializer_list<const char*> keys) {
        for (const char* k : keys)
            if (t.find(k) != std::string::npos) return true;
        return false;
    };
    if (any({"firstaid", "medigel"})) return "Healing";
    if (any({"cooked", "jerky", "souvlaki", "temaki", "chutney", "cookie", "muffin", "soup", "souffle", "pavlova", "pudding", "salad",
             "macaron", "nutrientblock", "da_water_", "waterfiltergland", "isotonic", "airag", "alcohol", "sugaranalog",
             "mineralizedwater", "nutritiousmussels", "_fruit", "airfruit", "funkyfruit", "seaolive"}))
        return "Food";
    if (any({"farmable_", "anemone", "plantmatter", "rotsac", "airbladder", "feelertree", "coralpad", "shootroot"})) return "Plants";
    if (any({"halfmoon", "geordie", "spineytail", "pneumo", "quadrate", "houndgar", "marrowbreach", "nibblershark", "seafrog",
             "flashslug", "waterslug", "scoutray", "da_mare", "leviathan", "egg", "charismaticslime", "sandspear", "harvestmoon",
             "bluemoon"}))
        return "Creatures";
    if (any({"fins", "oxygentank", "rebreather", "scubamask", "heatresist", "tadpole_", "battery", "powercell", "currentring",
             "jellyring", "oxygenbottle"}) &&
        !any({"terminal", "bank_", "powercellbank"}))
        return "Equipment";
    if (any({"scanner", "biosampler", "builder", "repairtool", "multitool", "flashlight", "flares", "glowstick", "lightstick",
             "sonicresonator", "beacon", "inoculator", "trackingtag", "dronebuddy", "da_emt", "disperser"}) &&
        !any({"station", "salvage"}))
        return "Tools";
    if (any({"copper", "titanium", "gold", "silver", "lead", "lithium", "nickel", "diamond", "quartz", "salt", "sulfur", "strontium",
             "tungsten", "troilite", "atacamite", "celestine", "fordite", "glass", "enamel", "rubber", "grease", "fiber", "silica",
             "rawore", "metalsalvage", "shell", "platecoral", "crystal", "clathrate", "acid", "ingot", "graphene", "wire",
             "wiringkit", "computerchip", "computercore", "biofuel", "biomass", "pigment", "hydraulicbulb", "axumtech", "slugooze",
             "crabfeces", "ooze", "unobtanium"}))
        return "Resources";
    return "Other";
}

std::string TextOf(void* obj, const char* prop) {
    static std::unordered_map<std::string, std::string> cache;
    char key[160];
    std::snprintf(key, sizeof(key), "%p.%s", obj, prop);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    if (!p || p.size <= 0 || p.size > 64) return {};
    ue::Call c(Cdo("/Script/Engine.Default__KismetTextLibrary"), "Conv_TextToString");
    c.SetRaw("InText", (uint8_t*)obj + p.offset, p.size);
    if (!c.Run()) return {};
    std::string s = OneLine(ue::FStringToUtf8(c.Get<ue::FString>("ReturnValue")));
    if (cache.size() > 20000) cache.clear();
    cache[key] = s;
    return s;
}

bool Placeholder(std::string s) {
    for (char& ch : s) ch = (char)std::toupper((unsigned char)ch);
    for (const char* k : {"PLACEHOLDER", "(PH)", " PH)", "[PH]", "DNL", "TBD", "TODO", "DO NOT", "TEST", "TESST", "UNUSED", "DEPRECATED",
                          "EXAMPLE", "SNAPSHOT", "DEBUG", "PROTOTYPE", "[WIP]", "(WIP)"})
        if (s.find(k) != std::string::npos) return true;
    return false;
}

bool LooksReal(void* type) {
    void* cls = ue::ClassOf(type);
    if (Placeholder(TextOf(type, "Name")) || Placeholder(TextOf(type, "ItemDescription"))) return false;
    ue::Prop thumb = ue::FindProp(cls, "Thumbnail");
    if (thumb && thumb.size >= 0x18) {
        ue::FName package = *(ue::FName*)((uint8_t*)type + thumb.offset + 8);
        if (package.index == 0) return false;
    }
    return true;
}
void LoadTables();
int ValidOf(const std::string& id);

int32_t ThumbPackage(void* type) {
    ue::Prop thumb = ue::FindProp(ue::ClassOf(type), "Thumbnail");
    if (!thumb || thumb.size < 0x18) return 0;
    return ((ue::FName*)((uint8_t*)type + thumb.offset + 8))->index;
}

void LoadItems() {
    ue::Call c(Cdo("/Script/Subnautica2.Default__SN2AssetRegistry"), "GetAllItemTypes");
    if (!c.Run()) return;
    auto arr = c.Get<ue::TArray<void*>>("ReturnValue");
    if (arr.num <= 0 || !arr.data) return;
    LoadTables();
    std::unordered_map<int32_t, std::set<std::string>> thumbUse;
    for (int i = 0; i < arr.num && i < 5000; ++i) {
        void* t = arr.data[i];
        if (!ue::Alive(t)) continue;
        int32_t pkg = ThumbPackage(t);
        if (pkg) thumbUse[pkg].insert(TextOf(t, "Name"));
    }
    std::vector<core::Item> items;
    std::map<std::string, int> cats;
    std::unordered_map<std::string, size_t> byName;
    int generic = 0, byWiki = 0, noPickup = 0, oddPickup = 0;
    for (int i = 0; i < arr.num && i < 5000; ++i) {
        void* t = arr.data[i];
        if (!ue::Alive(t)) continue;
        std::string id = ue::Name(t);
        if (id.empty()) continue;
        std::string name = TextOf(t, "Name");
        if (name.empty()) continue;
        ue::Prop pub = ue::FindProp(ue::ClassOf(t), "PublishedStatus");
        int32_t pkg = ThumbPackage(t);
        bool genericIcon = pkg && thumbUse[pkg].size() >= 6;
        if (genericIcon) ++generic;
        int valid = ValidOf(id);
        if (valid >= 0) ++byWiki;
        bool guess = Hidden(id) || (pub && ue::Read<uint8_t>(t, pub) != 0) || !LooksReal(t) || genericIcon;
        bool hide = valid == 1 ? Hidden(id) : valid == 0 ? true : guess;
        ue::Prop dest = ue::FindProp(ue::ClassOf(t), "PickupDestination");
        int destination = dest ? ue::Read<uint8_t>(t, dest) : 0;
        bool farmable = LowerAscii(id).find("farmable_") != std::string::npos;
        bool real = !hide;
        if (destination == 3 || farmable) {
            if (!hide) ++noPickup;
            hide = true;
        }
        if (destination == 1 && !hide && oddPickup++ < 40) LOG("item %s (%s) uses pickup destination 1", name.c_str(), id.c_str());

        void* cls = ue::ClassOf(t);
        std::string tagText = ue::NameToString(ue::Read<ue::FName>(t, ue::FindProp(cls, "Category")));
        tagText += " " + ue::NameToString(ue::Read<ue::FName>(t, ue::FindProp(cls, "TypeTag")));
        auto tags = ue::Read<ue::TArray<ue::FName>>(t, ue::FindProp(cls, "GameplayTags"));
        for (int k = 0; k < tags.num && k < 16 && tags.data; ++k) tagText += " " + ue::NameToString(tags.data[k]);
        core::Item item{id, name, CategoryFor(tagText, id), hide, real};
        auto seen = byName.find(name);
        if (seen != byName.end()) {
            const core::Item& old = items[seen->second];
            if ((old.hidden && !hide) || (old.hidden == hide && real && !old.real)) {
                itemTypes[id] = t;
                typeNames[t] = name;
                items[seen->second] = item;
            }
            continue;
        }
        byName[name] = items.size();
        items.push_back(item);
        itemTypes[id] = t;
        typeNames[t] = name;
    }
    for (const core::Item& it : items) ++cats[it.category + (it.hidden ? " (hidden)" : "")];
    LOG("items: %d use a shared placeholder icon, %d checked against the wiki list, %d more hidden because they can't be picked up", generic,
        byWiki, noPickup);
    std::sort(items.begin(), items.end(), [](const core::Item& a, const core::Item& b) { return a.name < b.name; });
    for (auto& [k, n] : cats) LOG("item category %s: %d", k.c_str(), n);
    LOG("items loaded: %zu", items.size());
    std::error_code ec;
    std::filesystem::create_directories(iconDir, ec);
    for (const core::Item& it : items)
        if (!std::filesystem::exists(iconDir + L"\\" + std::wstring(it.id.begin(), it.id.end()) + L".png", ec)) iconQueue.push_back(it.id);
    LOG("icons to make: %zu", iconQueue.size());
    core::SaveItems(items, std::filesystem::path(iconDir).parent_path().string() + "\\items.json");
    itemsLoaded = true;
    std::lock_guard<std::mutex> g(*modelLock);
    model->items = std::move(items);
}

struct IconJob {
    void* obj;
    std::string file;
};
std::vector<IconJob> extraIcons;
std::set<std::string> extraSeen;

void QueueIcon(void* obj, const std::string& file) {
    if (!obj || file.empty() || !extraSeen.insert(file).second) return;
    std::error_code ec;
    if (std::filesystem::exists(iconDir + L"\\" + std::wstring(file.begin(), file.end()) + L".png", ec)) return;
    extraIcons.push_back({obj, file});
}

bool MakeIconFor(void* type, const std::string& id);

bool MakeIcon(const std::string& id) { return MakeIconFor(itemTypes[id], id); }

bool MakeIconFor(void* type, const std::string& id) {
    if (!ue::Alive(type) || !st.world) return false;
    ue::Prop thumb = ue::FindProp(ue::ClassOf(type), "Thumbnail");
    if (!thumb || thumb.size > 64) return false;
    ue::Call load(SystemLib(), "LoadAsset_Blocking");
    load.SetRaw("Asset", (uint8_t*)type + thumb.offset, thumb.size);
    if (!load.Run()) return false;
    void* tex = load.Get<void*>("ReturnValue");
    if (!ue::Valid(tex)) return false;
    void* rl = Cdo("/Script/Engine.Default__KismetRenderingLibrary");
    if (!ue::Alive(iconRt)) {
        ue::Call mk(rl, "CreateRenderTarget2D");
        float clear[4] = {0, 0, 0, 0};
        mk.Set<void*>("WorldContextObject", st.world).Set<int32_t>("Width", 64).Set<int32_t>("Height", 64).Set<uint8_t>("Format", 3);
        mk.SetRaw("ClearColor", clear, sizeof(clear)).SetBool("bAutoGenerateMipMaps", false).SetBool("bSupportUAVs", false);
        if (!mk.Run()) return false;
        iconRt = mk.Get<void*>("ReturnValue");
        if (!ue::Valid(iconRt)) return false;
    }
    float clear[4] = {0, 0, 0, 0};
    ue::Call cl(rl, "ClearRenderTarget2D");
    cl.Set<void*>("WorldContextObject", st.world).Set<void*>("TextureRenderTarget", iconRt).SetRaw("ClearColor", clear, sizeof(clear));
    cl.Run();
    ue::Call begin(rl, "BeginDrawCanvasToRenderTarget");
    begin.Set<void*>("WorldContextObject", st.world).Set<void*>("TextureRenderTarget", iconRt);
    if (!begin.Run()) return false;
    void* canvas = begin.Get<void*>("Canvas");
    uint8_t ctx[128] = {};
    int ctxSize = begin.ParamSize("Context");
    if (ctxSize <= 0 || ctxSize > (int)sizeof(ctx) || !begin.GetRaw("Context", ctx, ctxSize)) return false;
    if (ue::Valid(canvas)) {
        double pos[2] = {0, 0}, size[2] = {64, 64}, cpos[2] = {0, 0}, csize[2] = {1, 1}, pivot[2] = {0.5, 0.5};
        float white[4] = {1, 1, 1, 1};
        ue::Call draw(canvas, "K2_DrawTexture");
        draw.Set<void*>("RenderTexture", tex).SetRaw("ScreenPosition", pos, sizeof(pos)).SetRaw("ScreenSize", size, sizeof(size));
        draw.SetRaw("CoordinatePosition", cpos, sizeof(cpos)).SetRaw("CoordinateSize", csize, sizeof(csize));
        draw.SetRaw("RenderColor", white, sizeof(white)).Set<uint8_t>("BlendMode", 2).Set<float>("Rotation", 0.0f);
        draw.SetRaw("PivotPoint", pivot, sizeof(pivot));
        draw.Run();
    }
    ue::Call end(rl, "EndDrawCanvasToRenderTarget");
    end.Set<void*>("WorldContextObject", st.world).SetRaw("Context", ctx, ctxSize);
    end.Run();
    ue::Call exp(rl, "ExportRenderTarget");
    exp.Set<void*>("WorldContextObject", st.world).Set<void*>("TextureRenderTarget", iconRt);
    exp.SetString("FilePath", iconDir).SetString("FileName", std::wstring(id.begin(), id.end()) + L".png");
    return exp.Run();
}

void PumpItems() {
    if (!st.world) return;
    if (!itemsLoaded) {
        static ULONGLONG lastLoadTry = 0;
        ULONGLONG t = GetTickCount64();
        if (t - lastLoadTry < 2000) return;
        lastLoadTry = t;
        LoadItems();
        if (!itemsLoaded) return;
    }
    ULONGLONG now = GetTickCount64();
    if ((iconQueue.empty() && extraIcons.empty()) || now - lastIcon < 120) return;
    lastIcon = now;
    bool made = false;
    if (!iconQueue.empty()) {
        std::string id = iconQueue.back();
        iconQueue.pop_back();
        made = MakeIcon(id);
    } else {
        IconJob job = extraIcons.back();
        extraIcons.pop_back();
        made = MakeIconFor(job.obj, job.file);
    }
    if (made) ++iconsMade;
    bool done = iconQueue.empty() && extraIcons.empty();
    static int atlasAt = 0;
    if (iconsMade > atlasAt && ((made && iconsMade % 40 == 0) || done)) {
        atlasAt = iconsMade;
        LOG("icons made: %d, left: %zu", iconsMade, iconQueue.size() + extraIcons.size());
        if (done) Note("Icons are ready");
        overlay::LoadAtlas(iconDir);
    }
}


std::wstring BasesDir() { return std::filesystem::path(iconDir).parent_path().wstring() + L"\\bases"; }

struct MapView {
    uint8_t* data = nullptr;
    int num = 0;
    const uint32_t* bits = nullptr;
};

bool ReadMapView(void* obj, const ue::Prop& p, MapView& v) {
    if (!p) return false;
    uint8_t* m = (uint8_t*)obj + p.offset;
    v.data = *(uint8_t**)m;
    v.num = *(int32_t*)(m + 8);
    uint32_t* sec = *(uint32_t**)(m + 0x20);
    v.bits = sec ? sec : (const uint32_t*)(m + 0x10);
    return v.data && v.num > 0 && v.num < 200000;
}

bool Alive(const MapView& v, int i) { return ((v.bits[i >> 5] >> (i & 31)) & 1) != 0; }

struct TrackerSnap {
    void* tracker = nullptr;
    int mapStride = 0;
    bool mapEmpty = false;
    bool entriesOk = false, repOk = false, mirrorsOk = false;
    std::map<std::string, int> map, entries, rep;
    std::map<std::wstring, int> mirrors;
};

struct UnlockSnap {
    TrackerSnap player, world;
    int recipes = -1, databank = -1;
    ULONGLONG secondPass = 0;
};
std::map<std::string, UnlockSnap> unlockSnaps;

int EventStride(void* tracker, MapView& v) {
    if (!ReadMapView(tracker, ue::FindProp(ue::ClassOf(tracker), "EventValues"), v)) return 0;
    for (int s : {0x2C, 0x30}) {
        int checked = 0, good = 0;
        for (int i = 0; i < v.num && checked < 12; ++i) {
            if (!Alive(v, i)) continue;
            ++checked;
            uint8_t* e = v.data + (size_t)i * s;
            ue::FName verb = *(ue::FName*)e;
            int32_t value = *(int32_t*)(e + 0x20);
            if (verb.index > 0 && !ue::NameToString(verb).empty() && value >= 0 && value < 100000000) ++good;
        }
        if (checked > 0 && good == checked) return s;
    }
    return 0;
}

struct EventArrayLayout {
    int arrayOff = -1, stride = 0, keyOff = -1, valueOff = -1;
};

EventArrayLayout EventArray() {
    EventArrayLayout L;
    void* arr = ue::FindClassByName("UWEEventArray");
    void* entry = ue::FindClassByName("UWEEventEntry");
    if (!arr || !entry) return L;
    L.arrayOff = ue::FindProp(arr, "Entries").offset;
    L.stride = ue::StructSize(entry);
    L.keyOff = ue::FindProp(entry, "Key").offset;
    L.valueOff = ue::FindProp(entry, "Value").offset;
    if (L.stride <= 0 || L.stride > 0x100 || L.keyOff < 0 || L.valueOff < 0 || L.keyOff + 0x20 > L.stride) L.arrayOff = -1;
    return L;
}

bool EventArrayAt(void* tracker, const char* name, const EventArrayLayout& L, ue::TArray<uint8_t>& out) {
    ue::Prop p = ue::FindProp(ue::ClassOf(tracker), name);
    if (!p || L.arrayOff < 0) return false;
    out = *(ue::TArray<uint8_t>*)((uint8_t*)tracker + p.offset + L.arrayOff);
    return out.num >= 0 && out.num < 200000 && (out.num == 0 || out.data);
}

bool UnlockVerb(const std::string& key) {
    std::string v = ue::NameToString(*(const ue::FName*)key.data());
    for (char& c : v) c = (char)std::tolower((unsigned char)c);
    return v.find("unlockableunlocked") != std::string::npos;
}

bool MirrorArrays(void* tracker, ue::TArray<ue::FString>& keys, ue::TArray<int32_t>& vals) {
    void* cls = ue::ClassOf(tracker);
    ue::Prop kp = ue::FindProp(cls, "KeysMirroredForSave"), vp = ue::FindProp(cls, "ValuesMirroredForSave");
    if (!kp || !vp) return false;
    keys = ue::Read<ue::TArray<ue::FString>>(tracker, kp);
    vals = ue::Read<ue::TArray<int32_t>>(tracker, vp);
    if (keys.num < 0 || vals.num < 0 || keys.num > 200000 || vals.num > 200000) return false;
    return (keys.num == 0 || keys.data) && (vals.num == 0 || vals.data);
}

void TakeTrackerSnap(void* tracker, TrackerSnap& snap) {
    snap = TrackerSnap{};
    if (!ue::Alive(tracker)) return;
    snap.tracker = tracker;
    MapView v;
    ue::Prop mp = ue::FindProp(ue::ClassOf(tracker), "EventValues");
    if (mp && *(int32_t*)((uint8_t*)tracker + mp.offset + 8) == 0) snap.mapEmpty = true;
    if (int stride = EventStride(tracker, v)) {
        snap.mapStride = stride;
        for (int i = 0; i < v.num; ++i) {
            if (!Alive(v, i)) continue;
            uint8_t* e = v.data + (size_t)i * stride;
            snap.map[std::string((const char*)e, 0x20)] = *(int32_t*)(e + 0x20);
        }
    }
    EventArrayLayout L = EventArray();
    for (int which = 0; which < 2; ++which) {
        ue::TArray<uint8_t> arr;
        if (!EventArrayAt(tracker, which ? "ReplicatedEntries" : "Entries", L, arr)) continue;
        auto& dst = which ? snap.rep : snap.entries;
        for (int i = 0; i < arr.num; ++i) {
            uint8_t* e = arr.data + (size_t)i * L.stride;
            dst[std::string((const char*)(e + L.keyOff), 0x20)] = *(int32_t*)(e + L.valueOff);
        }
        (which ? snap.repOk : snap.entriesOk) = true;
    }
    ue::TArray<ue::FString> keys;
    ue::TArray<int32_t> vals;
    if (MirrorArrays(tracker, keys, vals)) {
        for (int i = 0; i < keys.num && i < vals.num; ++i) {
            const ue::FString& k = keys.data[i];
            if (!k.data || k.num <= 1 || k.num > 4096) continue;
            snap.mirrors[std::wstring(k.data, (size_t)k.num - 1)] = vals.data[i];
        }
        snap.mirrorsOk = true;
    }
}

int RevertTracker(TrackerSnap& snap) {
    if (!snap.tracker || !ue::Alive(snap.tracker)) return 0;
    int changed = 0, kept = 0;
    auto target = [&kept](const std::map<std::string, int>& old, const std::string& key, int cur) {
        auto it = old.find(key);
        int before = it == old.end() ? 0 : it->second;
        if (cur <= before) return cur;
        if (!UnlockVerb(key)) {
            if (kept++ < 25)
                LOG("unlock undo: kept %s %s = %d (was %d)", ue::NameToString(*(const ue::FName*)key.data()).c_str(),
                    ue::NameToString(*(const ue::FName*)(key.data() + 0x18)).c_str(), cur, before);
            return cur;
        }
        return before;
    };
    MapView v;
    int nowStride = (snap.mapStride || snap.mapEmpty) ? EventStride(snap.tracker, v) : 0;
    if (nowStride && (nowStride == snap.mapStride || (snap.mapEmpty && !snap.mapStride))) {
        for (int i = 0; i < v.num; ++i) {
            if (!Alive(v, i)) continue;
            uint8_t* e = v.data + (size_t)i * nowStride;
            std::string key((const char*)e, 0x20);
            int32_t& val = *(int32_t*)(e + 0x20);
            int t = target(snap.map, key, val);
            if (t == val) continue;
            if (changed < 40)
                LOG("unlock undo: %s %s = %d -> %d", ue::NameToString(*(ue::FName*)e).c_str(), ue::NameToString(*(ue::FName*)(e + 0x18)).c_str(), val, t);
            val = t;
            ++changed;
        }
    }
    EventArrayLayout L = EventArray();
    for (int which = 0; which < 2; ++which) {
        if (!(which ? snap.repOk : snap.entriesOk)) continue;
        ue::TArray<uint8_t> arr;
        if (!EventArrayAt(snap.tracker, which ? "ReplicatedEntries" : "Entries", L, arr)) continue;
        const auto& old = which ? snap.rep : snap.entries;
        for (int i = 0; i < arr.num; ++i) {
            uint8_t* e = arr.data + (size_t)i * L.stride;
            int32_t& val = *(int32_t*)(e + L.valueOff);
            int t = target(old, std::string((const char*)(e + L.keyOff), 0x20), val);
            if (t != val) {
                val = t;
                ++changed;
            }
        }
    }
    ue::TArray<ue::FString> keys;
    ue::TArray<int32_t> vals;
    if (snap.mirrorsOk && MirrorArrays(snap.tracker, keys, vals)) {
        for (int i = 0; i < keys.num && i < vals.num; ++i) {
            const ue::FString& k = keys.data[i];
            if (!k.data || k.num <= 1 || k.num > 4096) continue;
            std::wstring ks(k.data, (size_t)k.num - 1);
            std::wstring low = ks;
            for (wchar_t& c : low) c = (wchar_t)std::towlower(c);
            if (low.find(L"unlockableunlocked") == std::wstring::npos) continue;
            auto it = snap.mirrors.find(ks);
            int before = it == snap.mirrors.end() ? 0 : it->second;
            if (vals.data[i] > before) {
                vals.data[i] = before;
                ++changed;
            }
        }
    }
    if (changed) ue::Call(snap.tracker, "StoreToSaveHandle").Run();
    return changed;
}

void* EventTracker(bool world) {
    if (!st.world) return nullptr;
    ue::Call c(Cdo("/Script/UWEEventTracker.Default__UWEEventTrackerStatics"), world ? "GetWorldEventTracker" : "GetLocalPlayerEventTracker");
    c.Set<void*>("WorldContextObject", st.world);
    void* t = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    return ue::Alive(t) ? t : nullptr;
}

int CountUnlockedRecipes();
int CountUnlockedDatabank();

void SnapshotUnlock(const std::string& id) {
    UnlockSnap& s = unlockSnaps[id];
    s = UnlockSnap{};
    TakeTrackerSnap(EventTracker(false), s.player);
    TakeTrackerSnap(EventTracker(true), s.world);
    s.recipes = CountUnlockedRecipes();
    s.databank = CountUnlockedDatabank();
    LOG("unlock snapshot for %s: player events %zu/%zu/%zu (map %d), world events %zu, %d blueprints, %d databank", id.c_str(),
        s.player.map.size(), s.player.entries.size(), s.player.mirrors.size(), s.player.mapStride, s.world.map.size(), s.recipes,
        s.databank);
    static bool shownKeys = false;
    if (!shownKeys && !s.player.mirrors.empty()) {
        shownKeys = true;
        int n = 0;
        for (const auto& [k, v] : s.player.mirrors) {
            if (n++ >= 3) break;
            std::string a;
            for (wchar_t c : k) a.push_back(c < 128 ? (char)c : '?');
            LOG("saved event key example: %s = %d", a.c_str(), v);
        }
    }
}

void RevertUnlock(const std::string& id) {
    auto it = unlockSnaps.find(id);
    if (it == unlockSnaps.end()) return;
    int n = RevertTracker(it->second.player) + RevertTracker(it->second.world);
    it->second.secondPass = GetTickCount64() + 2000;
    LOG("unlock undo for %s: %d values put back", id.c_str(), n);
}

int SetCount(const void* obj, const char* prop) {
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    if (!p) return -1;
    const uint8_t* s = (const uint8_t*)obj + p.offset;
    return *(const int32_t*)(s + 8) - *(const int32_t*)(s + 0x34);
}

int ArrayCount(const void* obj, const char* prop) {
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    return p ? *(const int32_t*)((const uint8_t*)obj + p.offset + 8) : -1;
}

void RefreshPdaLists() {
    void* recipeCls = ue::FindObject("/Script/Subnautica2.SN2RecipesListViewModel");
    void* dataCls = ue::FindObject("/Script/Subnautica2.SN2DatabankViewModel");
    if (!recipeCls && !dataCls) return;
    int32_t count = ue::ObjectCount();
    int lists = 0;
    for (int32_t i = 0; i < count; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o || ue::IsTemplate(o)) continue;
        bool recipes = recipeCls && ue::IsA(o, recipeCls);
        bool data = !recipes && dataCls && ue::IsA(o, dataCls);
        if ((!recipes && !data) || !ue::Alive(o)) continue;
        if (recipes && !ue::Ptr(o, "WorldContextObject")) continue;
        if (data && !ue::Ptr(o, "StoryGoalContainer")) continue;
        int before = recipes ? SetCount(o, "UnlockedRecipes") : ArrayCount(o, "Entries");
        ue::Call(o, "Refresh").Run();
        if (data) ue::Call(o, "RefreshNumUnread").Run();
        int after = recipes ? SetCount(o, "UnlockedRecipes") : ArrayCount(o, "Entries");
        if (lists++ < 8) LOG("PDA list %s refreshed: %d -> %d", recipes ? "blueprints" : "databank", before, after);
    }
    LOG("PDA lists refreshed: %d", lists);
}

void PumpUnlockUndo() {
    ULONGLONG now = GetTickCount64();
    bool refresh = false;
    for (auto it = unlockSnaps.begin(); it != unlockSnaps.end();) {
        UnlockSnap& s = it->second;
        if (!s.secondPass || now < s.secondPass) {
            ++it;
            continue;
        }
        int n = RevertTracker(s.player) + RevertTracker(s.world);
        int recipes = CountUnlockedRecipes(), databank = CountUnlockedDatabank();
        LOG("unlock undo check for %s: %d more values put back, blueprints %d (before %d), databank %d (before %d)", it->first.c_str(), n,
            recipes, s.recipes, databank, s.databank);
        bool left = (s.recipes >= 0 && recipes > s.recipes) || (s.databank >= 0 && databank > s.databank);
        if (left) Note("Some unlocks stayed. Your save from before is in the save-backups folder");
        it = unlockSnaps.erase(it);
        refresh = true;
    }
    if (refresh) RefreshPdaLists();
}

void* UEAlloc(size_t bytes, std::vector<std::wstring>& keep) {
    int chars = (int)((bytes + 1) / 2) + 4;
    ue::Call c(Cdo("/Script/Engine.Default__KismetStringLibrary"), "LeftPad");
    c.SetString("SourceString", L"").Set<int32_t>("ChCount", chars);
    if (!c.Run()) return nullptr;
    ue::FString s = c.Get<ue::FString>("ReturnValue");
    if (!s.data || s.num < chars) return nullptr;
    std::memset((void*)s.data, 0, bytes);
    return (void*)s.data;
}

struct CellLayout {
    int size = 0, coords = -1, brush = -1, baseBrush = -1, flags = -1, structure = -1, variant = -1, decon = -1, occupancy = -1;
    int module = -1, moduleCoords = -1, pieceHash = -1, rotation = -1, matchHash = -1, facing = -1, support = -1;
    bool ok = false;
};

CellLayout Layout() {
    CellLayout L;
    void* cell = ue::FindClassByName("UWESculpturalBaseCell");
    void* match = ue::FindClassByName("UWERuntimeModuleMatch");
    void* piece = ue::FindClassByName("UWERuntimeBaseModulePieceID");
    if (!cell || !match || !piece) return L;
    auto o = [](void* s, const char* n) { return ue::FindProp(s, n).offset; };
    L.size = ue::StructSize(cell);
    L.coords = o(cell, "Coords");
    L.brush = o(cell, "BrushType");
    L.baseBrush = o(cell, "BaseBrushType");
    L.flags = o(cell, "Flags");
    L.structure = o(cell, "StructureId");
    L.variant = o(cell, "VariantIndex");
    L.decon = o(cell, "DeconstructIDOverride");
    L.occupancy = o(cell, "OccupancyOverride");
    L.facing = o(cell, "FacingDirection");
    L.support = o(cell, "SupportAmount");
    int mm = o(cell, "ModuleMatch");
    int pid = o(match, "PieceId");
    L.module = mm + pid + o(piece, "Module");
    L.moduleCoords = mm + pid + o(piece, "Coords");
    L.pieceHash = mm + pid + o(piece, "ComputedHash");
    L.rotation = mm + o(match, "Rotation");
    L.matchHash = mm + o(match, "ComputedHash");
    L.ok = L.size > 0 && L.coords >= 0 && L.brush >= 0 && mm >= 0 && pid >= 0;
    return L;
}

void* NearestBase(double maxMeters) {
    ue::FVector me;
    if (!ActorPos(st.pawn, me)) return nullptr;
    void* best = nullptr;
    double bestD = maxMeters;
    for (const Tracked& t : tracked) {
        if (t.kind != KBase) continue;
        ue::FVector p;
        if (!ActorPos(t.actor, p)) continue;
        double d = Dist(p, me);
        if (d < bestD) {
            bestD = d;
            best = t.actor;
        }
    }
    return best;
}

void ListBases() {
    std::vector<core::BaseInfo> list;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(BasesDir(), ec)) {
        if (e.path().extension() != L".json") continue;
        std::ifstream in(e.path(), std::ios::binary);
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) continue;
        core::BaseInfo b;
        b.name = j.value("name", e.path().stem().string());
        b.date = j.value("date", std::string());
        b.pieces = j.contains("cells") && j["cells"].is_array() ? (int)j["cells"].size() : 0;
        b.file = e.path().string();
        list.push_back(b);
    }
    std::lock_guard<std::mutex> g(*modelLock);
    model->localBases = std::move(list);
}

void SaveBase(const std::string& name) {
    if (tracked.empty()) ScanActors();
    void* base = NearestBase(150.0);
    if (!base) {
        Note("No base within 150 m");
        return;
    }
    CellLayout L = Layout();
    MapView v;
    ue::Prop cm = ue::FindProp(ue::ClassOf(base), "CellMap");
    if (!L.ok || !ReadMapView(base, cm, v)) {
        Note("Could not read this base");
        LOG("base save failed: layout %d", (int)L.ok);
        return;
    }
    int stride = 0;
    for (int s : {0x78, 0x80, 0x70, 0x88}) {
        int checked = 0, good = 0;
        for (int i = 0; i < v.num && checked < 8; ++i) {
            if (!Alive(v, i)) continue;
            ++checked;
            uint8_t* e = v.data + (size_t)i * s;
            if (std::memcmp(e, e + 0x10 + L.coords, 12) == 0) ++good;
        }
        if (checked > 0 && good == checked) {
            stride = s;
            break;
        }
    }
    if (!stride) {
        Note("Could not read this base");
        LOG("base save failed: cell map layout not recognised");
        return;
    }
    ue::FVector origin;
    ActorPos(base, origin);
    ue::Call rot(base, "K2_GetActorRotation");
    rot.Run();
    double yaw = rot.Get<ue::FRotator>("ReturnValue").yaw;
    nlohmann::json cells = nlohmann::json::array();
    for (int i = 0; i < v.num; ++i) {
        if (!Alive(v, i)) continue;
        uint8_t* c = v.data + (size_t)i * stride + 0x10;
        auto iv = [&](int off) {
            int32_t* p = (int32_t*)(c + off);
            return nlohmann::json::array({p[0], p[1], p[2]});
        };
        nlohmann::json bb = nlohmann::json::array();
        auto arr = *(ue::TArray<ue::FName>*)(c + L.baseBrush);
        for (int k = 0; k < arr.num && k < 16 && arr.data; ++k) bb.push_back(ue::NameToString(arr.data[k]));
        void* module = *(void**)(c + L.module);
        cells.push_back({{"c", iv(L.coords)},
                         {"b", ue::NameToString(*(ue::FName*)(c + L.brush))},
                         {"bb", bb},
                         {"f", c[L.flags]},
                         {"s", c[L.structure]},
                         {"v", c[L.variant]},
                         {"d", c[L.decon]},
                         {"o", c[L.occupancy]},
                         {"m", ue::Valid(module) ? ue::Path(module) : std::string()},
                         {"mc", iv(L.moduleCoords)},
                         {"mh", *(uint32_t*)(c + L.pieceHash)},
                         {"r", *(int32_t*)(c + L.rotation)},
                         {"h", *(uint32_t*)(c + L.matchHash)},
                         {"fd", iv(L.facing)},
                         {"sa", *(float*)(c + L.support)}});
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    char date[32];
    std::snprintf(date, sizeof(date), "%04d-%02d-%02d %02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
    nlohmann::json j = {{"name", name},       {"date", date},
                        {"class", ue::Path(ue::ClassOf(base))},
                        {"location", {origin.x, origin.y, origin.z}},
                        {"yaw", yaw},       {"cells", cells}};
    std::error_code ec;
    std::filesystem::create_directories(BasesDir(), ec);
    std::string safe;
    for (char ch : name) safe.push_back(std::isalnum((unsigned char)ch) || ch == '-' || ch == '_' ? ch : '_');
    if (safe.empty()) safe = "base";
    std::ofstream out(std::filesystem::path(BasesDir()) / (safe + ".json"), std::ios::binary | std::ios::trunc);
    out << j.dump(1);
    LOG("base saved: %s, %zu cells, stride 0x%X, class %s", name.c_str(), cells.size(), stride, ue::Path(ue::ClassOf(base)).c_str());
    Note("Saved " + name + " (" + std::to_string(cells.size()) + " pieces)");
    ListBases();
}

void PlaceBase(const std::string& file) {
    std::ifstream in(std::filesystem::path(file), std::ios::binary);
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("cells") || !st.pawn || !st.world) {
        Note("Could not read that base file");
        return;
    }
    CellLayout L = Layout();
    void* dataCls = ue::FindClassByName("UWESavedBaseData");
    void* baseCls = ue::FindObject(j.value("class", std::string()));
    if (!baseCls) {
        if (tracked.empty()) ScanActors();
        for (const Tracked& t : tracked)
            if (t.kind == KBase) {
                baseCls = ue::ClassOf(t.actor);
                break;
            }
    }
    if (!L.ok || !dataCls || !baseCls) {
        Note("This game version can't place bases this way");
        LOG("base place failed: layout %d data %p class %p", (int)L.ok, dataCls, baseCls);
        return;
    }
    const auto& cells = j["cells"];
    size_t n = cells.size();
    std::vector<std::wstring> keep;
    uint8_t* buf = (uint8_t*)UEAlloc(n * L.size, keep);
    if (!buf) {
        Note("Could not prepare the base");
        return;
    }
    int skippedModules = 0;
    for (size_t i = 0; i < n; ++i) {
        const auto& e = cells[i];
        uint8_t* c = buf + i * L.size;
        auto setIv = [&](int off, const nlohmann::json& a) {
            if (!a.is_array() || a.size() != 3) return;
            int32_t* p = (int32_t*)(c + off);
            for (int k = 0; k < 3; ++k) p[k] = a[k].get<int32_t>();
        };
        setIv(L.coords, e["c"]);
        std::string brush = e.value("b", std::string());
        *(ue::FName*)(c + L.brush) = ue::MakeName(std::wstring(brush.begin(), brush.end()));
        const auto& bb = e["bb"];
        if (bb.is_array() && !bb.empty()) {
            ue::FName* tags = (ue::FName*)UEAlloc(bb.size() * sizeof(ue::FName), keep);
            if (tags) {
                for (size_t k = 0; k < bb.size(); ++k) {
                    std::string s = bb[k].get<std::string>();
                    tags[k] = ue::MakeName(std::wstring(s.begin(), s.end()));
                }
                *(ue::TArray<ue::FName>*)(c + L.baseBrush) = {tags, (int32_t)bb.size(), (int32_t)bb.size()};
            }
        }
        c[L.flags] = (uint8_t)e.value("f", 0);
        c[L.structure] = (uint8_t)e.value("s", 0);
        c[L.variant] = (uint8_t)e.value("v", 0);
        c[L.decon] = (uint8_t)e.value("d", 0);
        c[L.occupancy] = (uint8_t)e.value("o", 0);
        std::string mp = e.value("m", std::string());
        void* module = mp.empty() ? nullptr : ue::FindObject(mp);
        if (!mp.empty() && !module) ++skippedModules;
        *(void**)(c + L.module) = module;
        setIv(L.moduleCoords, e["mc"]);
        *(uint32_t*)(c + L.pieceHash) = e.value("mh", 0u);
        *(int32_t*)(c + L.rotation) = e.value("r", 0);
        *(uint32_t*)(c + L.matchHash) = e.value("h", 0u);
        setIv(L.facing, e["fd"]);
        *(float*)(c + L.support) = e.value("sa", 0.0f);
    }
    ue::Call so(Gameplay(), "SpawnObject");
    so.Set<void*>("ObjectClass", dataCls).Set<void*>("Outer", st.world);
    void* data = so.Run() ? so.Get<void*>("ReturnValue") : nullptr;
    if (!ue::Valid(data)) {
        Note("Could not prepare the base");
        return;
    }
    ue::Write<ue::TArray<uint8_t>>(data, ue::FindProp(ue::ClassOf(data), "Cells"), ue::TArray<uint8_t>{buf, (int32_t)n, (int32_t)n});

    ue::FVector me;
    ActorPos(st.pawn, me);
    ue::Call pr(st.pawn, "K2_GetActorRotation");
    pr.Run();
    double py = pr.Get<ue::FRotator>("ReturnValue").yaw * 3.14159265358979 / 180.0;
    double yaw = j.value("yaw", 0.0) * 3.14159265358979 / 180.0;
    ue::FVector at{me.x + std::cos(py) * 3000.0, me.y + std::sin(py) * 3000.0, me.z};
    alignas(16) double xf[12] = {0, 0, std::sin(yaw / 2), std::cos(yaw / 2), at.x, at.y, at.z, 0, 1, 1, 1, 0};

    ue::Call sp(Gameplay(), "BeginDeferredActorSpawnFromClass");
    sp.Set<void*>("WorldContextObject", st.world).Set<void*>("ActorClass", baseCls).SetRaw("SpawnTransform", xf, sizeof(xf));
    sp.Set<uint8_t>("CollisionHandlingOverride", 1).Set<void*>("Owner", nullptr).Set<uint8_t>("TransformScaleMethod", 1);
    void* actor = sp.Run() ? sp.Get<void*>("ReturnValue") : nullptr;
    if (!ue::Valid(actor)) {
        Note("The game refused to create the base");
        return;
    }
    void* acls = ue::ClassOf(actor);
    ue::Write<void*>(actor, ue::FindProp(acls, "InitialBaseData"), data);
    GUID guid;
    CoCreateGuid(&guid);
    ue::Write<GUID>(actor, ue::FindProp(acls, "BaseNetworkGUID"), guid);
    ue::Call fin(Gameplay(), "FinishSpawningActor");
    fin.Set<void*>("Actor", actor).SetRaw("SpawnTransform", xf, sizeof(xf)).Set<uint8_t>("TransformScaleMethod", 1);
    fin.Run();
    ue::Call up(actor, "ForceFullBaseUpdate");
    up.SetBool("ForInitialSetup", true).SetBool("UpdateCells", true).SetBool("UpdateProxies", true);
    up.Run();
    MapView after;
    int cellsNow = ReadMapView(actor, ue::FindProp(acls, "CellMap"), after) ? after.num : 0;
    LOG("base placed: %zu cells requested, cell map now %d, total %d, missing modules %d", n, cellsNow,
        ue::Read<int32_t>(actor, ue::FindProp(acls, "TotalCells")), skippedModules);
    Note(cellsNow > 0 ? "Base placed 30 m in front of you" : "The base was created but stayed empty (see log)");
}

const char* kCooldownEffects[] = {"GE_BiomodCooldown_C", "GE_GenericCooldown_C", "GE_ReducedDashCooldown_C",
                                  "GE_EmitSporesActiveCooldown_C", "GE_ScannerHintCooldown_C"};
ULONGLONG lastCooldown = 0;

void ClearCooldowns() {
    ULONGLONG now = GetTickCount64();
    if (now - lastCooldown < 50) return;
    lastCooldown = now;
    if (!On("zero_cooldowns") || !st.pawn || st.session == Session::Client) return;
    ue::Call asc(st.pawn, "GetASC");
    if (!asc.Run()) return;
    void* a = asc.Get<void*>("ReturnValue");
    if (!ue::Valid(a)) return;
    for (const char* name : kCooldownEffects) {
        void* ge = ue::FindClassByName(name);
        if (!ge) continue;
        ue::Call rm(a, "RemoveActiveGameplayEffectBySourceEffect");
        rm.Set<void*>("GameplayEffect", ge).Set<void*>("InstigatorAbilitySystemComponent", nullptr).Set<int32_t>("StacksToRemove", -1);
        rm.Run();
    }
}

ULONGLONG lastItemEsp = 0;
std::unordered_map<void*, std::pair<bool, ULONGLONG>> ownedMemo;

bool InWorldItem(void* actor, ULONGLONG now) {
    auto it = ownedMemo.find(actor);
    if (it != ownedMemo.end() && now - it->second.second < 3000) return it->second.first;
    bool free = ue::Alive(actor);
    void* cls = ue::ClassOf(actor);
    ue::Prop picked = ue::FindProp(cls, "bHasBeenPickedUp");
    if (free && picked && ue::ReadBool(actor, picked)) free = false;
    ue::Prop hidden = ue::FindProp(cls, "bHidden");
    if (free && hidden && ue::ReadBool(actor, hidden)) free = false;
    if (free && rootOff >= 0 && parentOff >= 0) {
        void* root = *(void**)((uint8_t*)actor + rootOff);
        if (!ue::Valid(root) || *(void**)((uint8_t*)root + parentOff)) free = false;
    }
    if (ownedMemo.size() > 4000) ownedMemo.clear();
    ownedMemo[actor] = {free, now};
    return free;
}

void UpdateItemEsp() {
    ULONGLONG now = GetTickCount64();
    if (now - lastItemEsp < 500) return;
    lastItemEsp = now;
    std::vector<core::EspDot> dots;
    if (On("item_esp") && st.inWorld && st.pc && st.session != Session::Client) {
        ue::FVector me = Eye();
        bool box = On("itemesp_box");
        struct Near {
            Tracked t;
            ue::FVector p;
            double d;
            int kind;
        };
        std::vector<Near> list;
        for (const Tracked& t : tracked) {
            int kind;
            if (t.kind == KItem) kind = 0;
            else if (t.kind == KData) kind = t.sub == 1 ? 2 : 1;
            else continue;
            if ((kind == 0 && !On("itemesp_items")) || (kind == 1 && !On("itemesp_data")) || (kind == 2 && !On("itemesp_crates"))) continue;
            ue::FVector p;
            if (!ActorPosFast(t.actor, p)) continue;
            double d = Dist(p, me);
            if (d > (kind == 0 ? 200.0 : 300.0)) continue;
            list.push_back({t, p, d, kind});
        }
        std::sort(list.begin(), list.end(), [](const Near& a, const Near& b) { return a.d < b.d; });
        for (const Near& n : list) {
            if (dots.size() >= 80) break;
            if (n.kind == 0 && !InWorldItem(n.t.actor, now)) continue;
            if (n.kind == 2) {
                ue::Prop opened = ue::FindProp(ue::ClassOf(n.t.actor), "CrateHasBeenOpened");
                if (opened && ue::ReadBool(n.t.actor, opened)) continue;
            }
            core::EspDot dot;
            dot.kind = n.kind;
            dot.dist = (float)n.d;
            dot.name = n.t.name;
            if (n.kind == 0) {
                void* type = ue::Ptr(n.t.actor, "ItemType");
                auto tn = type ? typeNames.find(type) : typeNames.end();
                if (tn != typeNames.end()) dot.name = tn->second;
            }
            FillBox(dot, n.t.actor, n.p, box, n.kind == 0 ? 400.0 : 1200.0, 25.0, now, 5000);
            if (!dot.box) dot.top = n.p.z + 30;
            dots.push_back(dot);
        }
    }
    std::lock_guard<std::mutex> g(*modelLock);
    model->itemEsp = std::move(dots);
}
std::map<int, ue::FName> verbs;
bool verbsLoaded = false;

void* PlayerState() { return st.pc ? ue::Ptr(st.pc, "PlayerState") : nullptr; }

ue::TArray<void*> AllOf(const char* fn) {
    ue::Call c(Cdo("/Script/Subnautica2.Default__SN2AssetRegistry"), fn);
    if (!c.Run()) return {};
    return c.Get<ue::TArray<void*>>("ReturnValue");
}

struct RuleEntry {
    uint8_t type = 0;
    ue::FName verb;
    uint8_t scope = 0;
    int32_t count = 0;
    void* asset = nullptr;
    ue::FName tag;
    std::vector<uint8_t> raw;
};

struct EntryLayout {
    int size = 0, type = -1, verb = -1, scope = -1, count = -1, asset = -1, tag = -1;
};

EntryLayout EntryOffsets() {
    EntryLayout L;
    void* s = ue::FindClassByName("UWERecipeUnlockRuleEntry");
    if (!s) return L;
    L.size = ue::StructSize(s);
    L.type = ue::FindProp(s, "EventType").offset;
    L.verb = ue::FindProp(s, "EventTrackerVerbTag").offset;
    L.scope = ue::FindProp(s, "RequirementScope").offset;
    L.count = ue::FindProp(s, "RequiredCount").offset;
    L.asset = ue::FindProp(s, "EventAsset").offset;
    L.tag = ue::FindProp(s, "EventTag").offset;
    if (L.size <= 0 || L.size > 0x100 || L.type < 0 || L.verb < 0 || L.count < 0 || L.asset < 0) L.size = 0;
    return L;
}

RuleEntry ParseEntry(const uint8_t* e, const EntryLayout& L) {
    RuleEntry r;
    r.type = e[L.type];
    r.verb = *(const ue::FName*)(e + L.verb);
    r.scope = L.scope >= 0 ? e[L.scope] : (uint8_t)0;
    r.count = *(const int32_t*)(e + L.count);
    r.asset = *(void* const*)(e + L.asset);
    r.tag = L.tag >= 0 ? *(const ue::FName*)(e + L.tag) : ue::FName{};
    r.raw.assign(e, e + L.size);
    return r;
}

std::vector<std::vector<RuleEntry>> RulesOf(void* recipe) {
    std::vector<std::vector<RuleEntry>> out;
    ue::Prop p = ue::FindProp(ue::ClassOf(recipe), "UpdatedUnlockingRequirements");
    void* ruleStruct = ue::FindClassByName("UWERecipeUnlockRules");
    EntryLayout L = EntryOffsets();
    if (!p || !ruleStruct || !L.size) return out;
    int rs = ue::StructSize(ruleStruct);
    int entriesOff = ue::FindProp(ruleStruct, "Entries").offset;
    if (rs <= 0 || entriesOff < 0) return out;
    auto rules = ue::Read<ue::TArray<uint8_t>>(recipe, p);
    for (int i = 0; i < rules.num && i < 16 && rules.data; ++i) {
        uint8_t* r = rules.data + (size_t)i * rs;
        auto entries = *(ue::TArray<uint8_t>*)(r + entriesOff);
        std::vector<RuleEntry> list;
        for (int k = 0; k < entries.num && k < 32 && entries.data; ++k) list.push_back(ParseEntry(entries.data + (size_t)k * L.size, L));
        out.push_back(list);
    }
    return out;
}

ue::FName DefaultVerb(int type) {
    static std::map<int, ue::FName> cache;
    auto it = cache.find(type);
    if (it != cache.end()) return it->second;
    const wchar_t* n = type == 0    ? L"EventTracker.Pickup"
                       : type == 5  ? L"EventTracker.Scanned"
                       : type == 10 ? L"EventTracker.CompletedScan"
                       : type == 15 ? L"EventTracker.UnlockableUnlocked"
                       : type == 20 ? L"EventTracker.DatacardCollected"
                                    : nullptr;
    ue::FName f = n ? ue::MakeName(n) : ue::FName{};
    if (f.index || !n) cache[type] = f;
    return f;
}

bool ExtractEntry(const RuleEntry& in, void* parent, RuleEntry& out) {
    EntryLayout L = EntryOffsets();
    if (!L.size || (int)in.raw.size() != L.size) return false;
    ue::Call c(Cdo("/Script/UWECrafting.Default__UWECraftingStatics"), "ExtractRule");
    if (!c.Ok() || c.ParamSize("InEntry") != L.size || c.ParamSize("ReturnValue") != L.size) return false;
    c.SetRaw("InEntry", in.raw.data(), L.size).Set<void*>("ParentAsset", parent);
    if (!c.Run()) return false;
    std::vector<uint8_t> r((size_t)L.size);
    if (!c.GetRaw("ReturnValue", r.data(), L.size)) return false;
    out = ParseEntry(r.data(), L);
    return true;
}

std::unordered_map<std::string, ue::TArray<void*>> allOfCache;

ue::TArray<void*> AllCached(const char* fn) {
    auto it = allOfCache.find(fn);
    if (it != allOfCache.end() && it->second.num > 0) return it->second;
    ue::TArray<void*> a = AllOf(fn);
    if (a.num > 0 && a.data) allOfCache[fn] = a;
    return a;
}

std::vector<void*> fogComps, crafterComps, fogBuild, crafterBuild;
std::unordered_map<void*, int> compClassMemo;
int32_t compCursor = -1;
bool compScanDone = false;
ULONGLONG lastCompScan = 0;

int CompKind(void* cls) {
    auto it = compClassMemo.find(cls);
    if (it != compClassMemo.end()) return it->second;
    static void* fogCls = nullptr;
    static void* crafterCls = nullptr;
    if (!ue::Valid(fogCls)) fogCls = ue::FindObject("/Script/Engine.ExponentialHeightFogComponent");
    if (!ue::Valid(crafterCls)) crafterCls = ue::FindObject("/Script/UWECrafting.UWECrafterComponent");
    int kind = 0;
    for (void* c = cls; c && !kind; c = ue::SuperOf(c)) {
        if (fogCls && c == fogCls) kind = 1;
        else if (crafterCls && c == crafterCls) kind = 2;
    }
    if (compClassMemo.size() > 20000) compClassMemo.clear();
    compClassMemo[cls] = kind;
    return kind;
}

void PumpComponentScan(bool wanted) {
    if (!wanted || !st.world) return;
    ULONGLONG now = GetTickCount64();
    if (compCursor < 0) {
        if (compScanDone && now - lastCompScan < 10000) return;
        lastCompScan = now;
        compCursor = 0;
        fogBuild.clear();
        crafterBuild.clear();
    }
    int32_t count = ue::ObjectCount();
    int32_t end = std::min(count, compCursor + 25000);
    for (int32_t i = compCursor; i < end; ++i) {
        void* o = ue::ObjectAt(i);
        if (!o || ue::IsTemplate(o)) continue;
        void* cls = ue::ClassOf(o);
        if (!cls) continue;
        int k = CompKind(cls);
        if (!k || !ue::Alive(o)) continue;
        (k == 1 ? fogBuild : crafterBuild).push_back(o);
    }
    compCursor = end;
    if (compCursor < count) return;
    compCursor = -1;
    static size_t loggedFog = SIZE_MAX, loggedCrafters = SIZE_MAX;
    if (fogBuild.size() != loggedFog || crafterBuild.size() != loggedCrafters) {
        loggedFog = fogBuild.size();
        loggedCrafters = crafterBuild.size();
        LOG("found %zu fog parts and %zu crafting stations", loggedFog, loggedCrafters);
    }
    fogComps.swap(fogBuild);
    crafterComps.swap(crafterBuild);
    compScanDone = true;
}

struct FogSave {
    float start = 0, volStart = 0;
    bool setStart = false, setVol = false;
};
std::unordered_map<void*, FogSave> fogSaves;
bool fogConsole = false;
int fogConsoleSaved = 1;
ULONGLONG lastFog = 0;
const float kFogStart = 25000.0f;

void EnforceFog(bool on) {
    if (!on) {
        int restored = 0;
        for (auto& [c, s] : fogSaves) {
            if (!ue::Alive(c)) continue;
            void* cls = ue::ClassOf(c);
            ue::Prop sp = ue::FindProp(cls, "StartDistance"), vp = ue::FindProp(cls, "VolumetricFogStartDistance");
            if (s.setStart && sp && std::fabs(ue::Read<float>(c, sp) - kFogStart) < 1.0f) CallSet(c, "SetStartDistance", "Value", s.start);
            if (s.setVol && vp && std::fabs(ue::Read<float>(c, vp) - kFogStart) < 1.0f)
                CallSet(c, "SetVolumetricFogStartDistance", "NewValue", s.volStart);
            ++restored;
        }
        if (!fogSaves.empty()) LOG("fog back to normal on %d parts", restored);
        fogSaves.clear();
        if (fogConsole && st.world) {
            Console(L"r.Fog " + std::to_wstring(fogConsoleSaved));
            fogConsole = false;
        }
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (now - lastFog < 1000) return;
    lastFog = now;
    int applied = 0;
    for (void* c : fogComps) {
        if (!ue::Alive(c)) continue;
        void* cls = ue::ClassOf(c);
        ue::Prop sp = ue::FindProp(cls, "StartDistance"), vp = ue::FindProp(cls, "VolumetricFogStartDistance");
        if (!sp) continue;
        float s = ue::Read<float>(c, sp), v = vp ? ue::Read<float>(c, vp) : 0.0f;
        bool fresh = !fogSaves.count(c);
        FogSave& fs = fogSaves[c];
        if (fresh) LOG("fog part %s: start %.0f, volumetric start %.0f", ue::Name(c).c_str(), s, v);
        if (s < kFogStart - 1.0f) {
            fs.start = s;
            fs.setStart = true;
            CallSet(c, "SetStartDistance", "Value", kFogStart);
        }
        if (vp && v < kFogStart - 1.0f) {
            fs.volStart = v;
            fs.setVol = true;
            CallSet(c, "SetVolumetricFogStartDistance", "NewValue", kFogStart);
        }
        ++applied;
    }
    if (applied && fogConsole) {
        Console(L"r.Fog " + std::to_wstring(fogConsoleSaved));
        fogConsole = false;
    }
    if (!applied && compScanDone && !fogConsole && st.inWorld) {
        fogConsoleSaved = CVarInt(L"r.Fog");
        if (fogConsoleSaved <= 0) fogConsoleSaved = 1;
        Console(L"r.Fog 0");
        fogConsole = true;
        LOG("no fog parts found, fog turned off with r.Fog");
    }
}

std::unordered_map<void*, float> craftSaves, crafterSaves;
bool craftActive = false;
ULONGLONG lastCraft = 0;
const float kCraftTime = 0.1f;

void EnforceCraftTimes(bool on) {
    if (!on) {
        if (!craftActive) return;
        int restored = 0;
        for (auto& [r, t] : craftSaves) {
            if (!ue::Alive(r)) continue;
            ue::Write<float>(r, ue::FindProp(ue::ClassOf(r), "CraftingTime"), t);
            ++restored;
        }
        for (auto& [c, t] : crafterSaves)
            if (ue::Alive(c)) ue::Write<float>(c, ue::FindProp(ue::ClassOf(c), "DefaultCraftingTime"), t);
        craftSaves.clear();
        crafterSaves.clear();
        craftActive = false;
        LOG("instant crafting off, %d crafting times put back", restored);
        return;
    }
    ULONGLONG now = GetTickCount64();
    if (craftActive && now - lastCraft < 3000) return;
    lastCraft = now;
    int changed = 0;
    auto recipes = AllCached("GetAllCraftingRecipes");
    for (int i = 0; i < recipes.num && recipes.data; ++i) {
        void* r = recipes.data[i];
        if (!ue::Alive(r)) continue;
        ue::Prop p = ue::FindProp(ue::ClassOf(r), "CraftingTime");
        if (!p) continue;
        float cur = ue::Read<float>(r, p);
        if (!craftSaves.count(r)) craftSaves[r] = cur;
        if (cur > kCraftTime) {
            ue::Write<float>(r, p, kCraftTime);
            ++changed;
        }
    }
    for (void* c : crafterComps) {
        if (!ue::Alive(c)) continue;
        ue::Prop p = ue::FindProp(ue::ClassOf(c), "DefaultCraftingTime");
        if (!p) continue;
        float cur = ue::Read<float>(c, p);
        if (!crafterSaves.count(c)) crafterSaves[c] = cur;
        if (cur > kCraftTime) {
            ue::Write<float>(c, p, kCraftTime);
            ++changed;
        }
    }
    if (!craftActive || changed) LOG("instant crafting: %d crafting times shortened (%zu recipes, %zu stations)", changed, craftSaves.size(), crafterSaves.size());
    craftActive = true;
}

void* lastBuildComp = nullptr;
float lastBuildProgress = 0;
ULONGLONG lastBuild = 0;
int buildLogs = 0;

void* WeakProp(void* obj, const char* prop) {
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    if (!p) return nullptr;
    const uint8_t* w = (const uint8_t*)obj + p.offset;
    void* o = ue::WeakGet(*(const int32_t*)w, *(const int32_t*)(w + 4));
    return ue::Alive(o) ? o : nullptr;
}

void* ConstructableOf(void* actor) {
    void* c = ue::Ptr(actor, "ConstructableComponent");
    if (c) return c;
    static void* cls = nullptr;
    if (!ue::Valid(cls)) cls = ue::FindObject("/Script/Subnautica2.SN2ConstructableComponent");
    if (!cls) return nullptr;
    ue::Call g(actor, "GetComponentByClass");
    g.Set<void*>("ComponentClass", cls);
    c = g.Run() ? g.Get<void*>("ReturnValue") : nullptr;
    return ue::Alive(c) ? c : nullptr;
}

void InstantBuild(bool allowed) {
    ULONGLONG now = GetTickCount64();
    if (now - lastBuild < 100) return;
    lastBuild = now;
    bool build = On("instant_build"), take = On("instant_deconstruct");
    if (!allowed || !st.world || (!build && !take)) {
        lastBuildComp = nullptr;
        return;
    }
    ue::Call t(Cdo("/Script/Subnautica2.Default__SN2Statics"), "GetLocalEquippedTool");
    t.Set<void*>("WorldContextObject", st.world);
    void* tool = t.Run() ? t.Get<void*>("ReturnValue") : nullptr;
    static void* builderCls = nullptr;
    if (!ue::Valid(builderCls)) builderCls = ue::FindObject("/Script/Subnautica2.SN2BuilderTool");
    if (!ue::Alive(tool) || !builderCls || !ue::IsA(tool, builderCls)) {
        lastBuildComp = nullptr;
        return;
    }
    void* comp = nullptr;
    for (const char* prop : {"CurrentTarget", "Ghost"}) {
        void* a = WeakProp(tool, prop);
        if (a && (comp = ConstructableOf(a))) break;
    }
    if (!comp) {
        lastBuildComp = nullptr;
        return;
    }
    float p = CallFloat(comp, "GetConstructionProgress");
    if (comp != lastBuildComp) {
        lastBuildComp = comp;
        lastBuildProgress = p;
        return;
    }
    float before = lastBuildProgress;
    lastBuildProgress = p;
    if (take && p < before - 0.0001f && p > 0.02f) {
        ue::Call(comp, "SetConstructionProgressToEmpty").Run();
        float after = CallFloat(comp, "GetConstructionProgress");
        bool ok = after < p - 0.0001f;
        if (!ok) {
            ue::Call s(comp, "SetConstructionProgress");
            s.Set<float>("Progress", 0.01f);
            ok = s.Run() && s.GetBool("ReturnValue");
            after = CallFloat(comp, "GetConstructionProgress");
        }
        if (buildLogs < 12) {
            ++buildLogs;
            LOG("instant deconstruct: %s progress %.3f -> %.3f (%s)", ue::Name(comp).c_str(), p, after, ok ? "done" : "game refused");
        }
        lastBuildProgress = after;
        return;
    }
    if (!build || p <= before + 0.0001f || p >= 0.98f) return;
    ue::Call(comp, "SetConstructionProgressToFull").Run();
    float after = CallFloat(comp, "GetConstructionProgress");
    bool ok = after > p + 0.0001f;
    if (!ok && p < 1.0f) {
        ue::Call s(comp, "SetConstructionProgress");
        s.Set<float>("Progress", 0.999f);
        ok = s.Run() && s.GetBool("ReturnValue");
        after = CallFloat(comp, "GetConstructionProgress");
    }
    if (buildLogs < 6) {
        ++buildLogs;
        LOG("instant build: %s progress %.3f -> %.3f (%s)", ue::Name(comp).c_str(), p, after, ok ? "finished" : "game refused");
    }
    lastBuildProgress = after;
}

void EnforceExtras(bool allowed) {
    EnforceFog(allowed && st.world && (On("hide_water_fog") || On("performance_mode")));
    EnforceCraftTimes(allowed && st.inWorld && On("instant_craft"));
}

void LoadVerbs() {
    if (verbsLoaded) return;
    auto recipes = AllCached("GetAllCraftingRecipes");
    for (int i = 0; i < recipes.num && recipes.data; ++i) {
        if (!ue::Alive(recipes.data[i])) continue;
        for (const auto& rule : RulesOf(recipes.data[i]))
            for (const RuleEntry& e : rule)
                if (e.verb.index && !verbs.count(e.type)) verbs[e.type] = e.verb;
    }
    verbsLoaded = !verbs.empty();
    for (const auto& [t, v] : verbs) LOG("event verb %d = %s", t, ue::NameToString(v).c_str());
}

bool FireEvent(const ue::FName& verb, void* asset, int count, bool world) {
    if (!verb.index || !ue::Alive(asset) || !st.world || (!world && !st.pawn)) return false;
    void* lib = Cdo("/Script/UWEEventTracker.Default__UWEEventTrackerStatics");
    if (world) {
        ue::Call c(lib, "NotifyWorldEventAsset");
        c.Set<void*>("WorldContextObject", st.world).Set<ue::FName>("Verb", verb).Set<void*>("Asset", asset).Set<double>("Value", (double)count);
        return c.Run();
    }
    ue::Call c(lib, "NotifyPlayerEventAsset");
    c.Set<void*>("Actor", st.pawn).Set<ue::FName>("Verb", verb).Set<void*>("Asset", asset).Set<int32_t>("Value", count);
    return c.Run();
}

bool FireTagEvent(const ue::FName& verb, const ue::FName& tag, int count, bool world) {
    if (!verb.index || !tag.index || !st.world || (!world && !st.pawn)) return false;
    void* lib = Cdo("/Script/UWEEventTracker.Default__UWEEventTrackerStatics");
    if (world) {
        ue::Call c(lib, "NotifyWorldEvent");
        c.Set<void*>("WorldContextObject", st.world).Set<ue::FName>("Verb", verb).Set<ue::FName>("Tag", tag).Set<double>("Value", (double)count);
        return c.Run();
    }
    ue::Call c(lib, "NotifyPlayerEvent");
    c.Set<void*>("Actor", st.pawn).Set<ue::FName>("Verb", verb).Set<ue::FName>("Tag", tag).Set<int32_t>("Value", count);
    return c.Run();
}

bool ScanDone(void* scan, void* ps) {
    ue::Call c(Cdo("/Script/UWEScanner.Default__UWEScannerStatics"), "IsScanDataFullyScannedForPlayer");
    c.Set<void*>("ScanData", scan).Set<void*>("PlayerState", ps);
    return c.Run() && c.GetBool("ReturnValue");
}

int CompleteScan(void* scan) {
    int needed = ue::Read<int32_t>(scan, ue::FindProp(ue::ClassOf(scan), "NumRequired"));
    if (needed <= 0) needed = 1;
    ue::FName first = DefaultVerb(5), full = DefaultVerb(10);
    int fired = 0;
    if (FireEvent(first, scan, needed, false)) ++fired;
    if (FireEvent(full, scan, 1, false)) ++fired;
    FireEvent(first, scan, needed, true);
    FireEvent(full, scan, 1, true);
    return fired;
}

void UnlockAllScans() {
    if (!DefaultVerb(5).index && !DefaultVerb(10).index) {
        Note("Could not find how the game records scans");
        return;
    }
    auto scans = AllCached("GetAllScanDatas");
    int n = 0;
    for (int i = 0; i < scans.num && scans.data; ++i)
        if (ue::Alive(scans.data[i]) && CompleteScan(scans.data[i])) ++n;
    LOG("unlock all scans: fired events for %d scans", n);
    Note(std::to_string(n) + " scans unlocked");
}

bool RecipeUnlocked(void* recipe, void* ps) {
    ue::Call c(Cdo("/Script/UWECrafting.Default__UWECraftingStatics"), "IsRecipeUnlockedForPlayer");
    c.Set<void*>("Recipe", recipe).Set<void*>("PlayerState", ps);
    return c.Run() && c.GetBool("ReturnValue");
}

int CountUnlockedRecipes() {
    void* ps = PlayerState();
    if (!ps) return -1;
    auto recipes = AllCached("GetAllCraftingRecipes");
    int n = 0;
    for (int i = 0; i < recipes.num && recipes.data; ++i)
        if (ue::Alive(recipes.data[i]) && RecipeUnlocked(recipes.data[i], ps)) ++n;
    return n;
}

int CountUnlockedDatabank() {
    auto entries = AllCached("GetAllDatabankEntries");
    int n = 0;
    for (int i = 0; i < entries.num && entries.data; ++i) {
        if (!ue::Alive(entries.data[i])) continue;
        ue::Call c(entries.data[i], "IsUnlocked");
        if (c.Run() && c.GetBool("ReturnValue")) ++n;
    }
    return n;
}

bool EntitlementRule(const std::vector<RuleEntry>& rule) {
    for (const RuleEntry& e : rule)
        if (e.type == 25) return true;
    return false;
}

int FireRule(const std::vector<RuleEntry>& rule, void* parent) {
    int fired = 0;
    static int logged = 0;
    for (const RuleEntry& src : rule) {
        RuleEntry e = src;
        RuleEntry x;
        bool extracted = ExtractEntry(src, parent, x) && (x.verb.index || x.asset || x.tag.index);
        if (extracted) {
            if (x.verb.index) e.verb = x.verb;
            if (x.asset) e.asset = x.asset;
            if (x.tag.index) e.tag = x.tag;
            e.scope = x.scope;
            if (x.count > 0) e.count = x.count;
        }
        if (!e.verb.index) e.verb = DefaultVerb(e.type);
        if (!e.verb.index) {
            auto v = verbs.find(e.type);
            if (v != verbs.end()) e.verb = v->second;
        }
        int n = std::max(1, e.count);
        bool world = e.scope == 0;
        bool ok = false;
        if (e.asset && ue::Alive(e.asset)) ok = FireEvent(e.verb, e.asset, n, world);
        else if (e.tag.index) ok = FireTagEvent(e.verb, e.tag, n, world);
        if (ok) ++fired;
        if (logged < 40) {
            ++logged;
            LOG("unlock event: type %d verb %s asset %s tag %s x%d %s%s: %s", e.type, ue::NameToString(e.verb).c_str(),
                e.asset && ue::Alive(e.asset) ? ue::Name(e.asset).c_str() : "-", ue::NameToString(e.tag).c_str(), n, world ? "world" : "player",
                extracted ? "" : " (own rule reading)", ok ? "sent" : "not sent");
        }
    }
    return fired;
}

bool UnlockRecipe(void* recipe, bool everyRule) {
    LoadVerbs();
    int fired = 0;
    for (const auto& rule : RulesOf(recipe)) {
        if (EntitlementRule(rule)) continue;
        fired += FireRule(rule, recipe);
        if (fired && !everyRule) break;
    }
    return fired > 0;
}

void LogRules(void* recipe, const std::string& name) {
    int i = 0;
    for (const auto& rule : RulesOf(recipe)) {
        for (const RuleEntry& e : rule)
            LOG("unlock rule %d for %s: type %d verb %s asset %s tag %s count %d scope %d", i, name.c_str(), e.type,
                ue::NameToString(e.verb).c_str(), e.asset ? ue::Name(e.asset).c_str() : "-", ue::NameToString(e.tag).c_str(), e.count, e.scope);
        ++i;
    }
    if (i == 0) LOG("unlock rule for %s: none", name.c_str());
}

void* SoftLoaded(const uint8_t* soft) { return ue::WeakGet(*(const int32_t*)soft, *(const int32_t*)(soft + 4)); }

std::string SoftAssetName(const uint8_t* soft) { return ue::NameToString(*(const ue::FName*)(soft + 0x10)); }

bool Published(void* asset) {
    ue::Prop p = ue::FindProp(ue::ClassOf(asset), "PublishedStatus");
    return !p || ue::Read<uint8_t>(asset, p) == 0;
}

std::unordered_map<std::string, void*> progressAssets;
ULONGLONG lastProgress = 0;

struct PendingUnlock {
    void* asset;
    std::string name;
    ULONGLONG at;
    int stage;
    bool scan;
};
std::vector<PendingUnlock> pendingUnlocks;

void CheckUnlocks() {
    if (pendingUnlocks.empty()) return;
    ULONGLONG now = GetTickCount64();
    void* ps = PlayerState();
    for (auto it = pendingUnlocks.begin(); it != pendingUnlocks.end();) {
        if (now - it->at < 1500) {
            ++it;
            continue;
        }
        bool ok = !ue::Alive(it->asset) || !ps || (it->scan ? ScanDone(it->asset, ps) : RecipeUnlocked(it->asset, ps));
        if (ok) {
            if (ps && ue::Alive(it->asset)) Note(it->name + (it->scan ? " scanned" : " unlocked"));
            lastProgress = 0;
            it = pendingUnlocks.erase(it);
            continue;
        }
        if (it->stage == 0 && !it->scan && UnlockRecipe(it->asset, true)) {
            it->stage = 1;
            it->at = now;
            ++it;
            continue;
        }
        if (!it->scan) LogRules(it->asset, it->name);
        Note("The game didn't allow unlocking " + it->name);
        it = pendingUnlocks.erase(it);
    }
}

std::unordered_map<std::string, int> tierById, validById, tierByName;
bool tablesReady = false;

std::string LowerAscii(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

int ValidOf(const std::string& id) {
    auto it = validById.find(id);
    return it == validById.end() ? -1 : it->second;
}

void LoadTables() {
    if (tablesReady) return;
    tablesReady = true;
    auto clampTier = [](int t) { return t >= 0 && t <= 4 ? t : -1; };
    for (const data::ItemRule& r : data::kItems) {
        if (!r.id || !r.id[0]) continue;
        tierById[r.id] = clampTier(r.tier);
        validById[r.id] = r.valid;
    }
    for (const data::NameTier& n : data::kNames) {
        if (!n.name || !n.name[0]) continue;
        std::string key = LowerAscii(n.name);
        tierByName[key] = clampTier(n.tier);
        size_t paren = key.find(" (");
        if (paren != std::string::npos && paren > 0) tierByName.emplace(key.substr(0, paren), clampTier(n.tier));
    }
    LOG("progression table: %zu items, %zu names", tierById.size(), tierByName.size());
}

struct RecipeInfo {
    std::string out;
    std::vector<std::string> reqs;
    bool defaultUnlocked = false;
    bool entitlementOnly = false;
    bool noRules = false;
};
std::unordered_map<void*, RecipeInfo> recipeInfo;
std::unordered_map<std::string, void*> recipeForItem;
std::unordered_map<void*, int> recipeTierMemo;

const RecipeInfo& InfoOf(void* r) {
    auto it = recipeInfo.find(r);
    if (it != recipeInfo.end()) return it->second;
    RecipeInfo info;
    void* cls = ue::ClassOf(r);
    void* outStruct = ue::FindClassByName("CraftingRecipeOutput");
    void* reqStruct = ue::FindClassByName("CraftingRecipeRequirement");
    int outSize = outStruct ? ue::StructSize(outStruct) : 0x58, reqSize = reqStruct ? ue::StructSize(reqStruct) : 0x30;
    auto outs = ue::Read<ue::TArray<uint8_t>>(r, ue::FindProp(cls, "Output"));
    if (outs.num > 0 && outs.data && outSize > 0) info.out = SoftAssetName(outs.data);
    auto reqs = ue::Read<ue::TArray<uint8_t>>(r, ue::FindProp(cls, "Requirements"));
    for (int k = 0; k < reqs.num && k < 16 && reqs.data && reqSize > 0; ++k) info.reqs.push_back(SoftAssetName(reqs.data + (size_t)k * reqSize));
    ue::Prop drs = ue::FindProp(cls, "DefaultRecipeState");
    info.defaultUnlocked = drs && ue::Read<uint8_t>(r, drs) == 1;
    auto rules = RulesOf(r);
    info.entitlementOnly = !rules.empty();
    size_t entries = 0;
    for (const auto& rule : rules) {
        entries += rule.size();
        if (!EntitlementRule(rule)) info.entitlementOnly = false;
    }
    info.noRules = entries == 0;
    return recipeInfo.emplace(r, std::move(info)).first->second;
}

int RecipeTier(void* r, int depth);

int ItemTier(const std::string& id, int depth = 0) {
    auto t = tierById.find(id);
    if (t != tierById.end() && t->second >= 0) return t->second;
    if (depth > 6) return -1;
    auto r = recipeForItem.find(id);
    return r != recipeForItem.end() ? RecipeTier(r->second, depth + 1) : -1;
}

int RecipeTier(void* r, int depth) {
    auto m = recipeTierMemo.find(r);
    if (m != recipeTierMemo.end()) return m->second;
    recipeTierMemo[r] = -1;
    const RecipeInfo& info = InfoOf(r);
    int tier = -1;
    auto t = tierById.find(info.out);
    if (t != tierById.end() && t->second >= 0) {
        tier = t->second;
    } else {
        for (const std::string& req : info.reqs) tier = std::max(tier, ItemTier(req, depth + 1));
    }
    recipeTierMemo[r] = tier;
    return tier;
}

const char* kStage[] = {"the start area", "early game", "mid game", "late game", "endgame"};

std::unordered_map<std::string, std::string> pingNames;

std::vector<core::ProgressEntry> PingLocations(const ue::FVector& me) {
    std::vector<core::ProgressEntry> out;
    void* gs = st.world ? ue::Ptr(st.world, "GameState") : nullptr;
    void* pings = gs ? ue::Ptr(gs, "PingSystemComponent") : nullptr;
    void* pingStruct = ue::FindClassByName("UWEPing");
    if (!pings || !pingStruct) return out;
    int size = ue::StructSize(pingStruct);
    int locOff = ue::FindProp(pingStruct, "Location").offset;
    ue::Prop nameP = ue::FindProp(pingStruct, "DisplayName");
    auto arr = ue::Read<ue::TArray<uint8_t>>(pings, ue::FindProp(ue::ClassOf(pings), "ActivePings"));
    if (size <= 0 || locOff < 0 || !nameP) return out;
    for (int i = 0; i < arr.num && i < 512 && arr.data; ++i) {
        uint8_t* p = arr.data + (size_t)i * size;
        ue::FVector loc = *(ue::FVector*)(p + locOff);
        core::ProgressEntry e;
        std::string key((const char*)p, 16);
        key.append((const char*)(p + nameP.offset), 8);
        auto known = pingNames.find(key);
        if (known != pingNames.end()) {
            e.name = known->second;
        } else {
            e.name = TextAt(p + nameP.offset, nameP.size, false);
            if (pingNames.size() > 2000) pingNames.clear();
            pingNames[key] = e.name;
        }
        if (e.name.empty() || Placeholder(e.name)) continue;
        e.detail = "Signal";
        e.hasPos = true;
        e.x = (float)(loc.x / 100);
        e.y = (float)(loc.y / 100);
        e.z = (float)(loc.z / 100);
        e.order = (int)Dist(loc, me);
        out.push_back(e);
    }
    return out;
}

int DepthTier(double depth) {
    if (depth < 60) return 0;
    if (depth < 150) return 1;
    if (depth < 400) return 2;
    if (depth < 900) return 3;
    return 4;
}

int cachedDataDone = 0, cachedDataTotal = 0, cachedGoalDone = 0, cachedGoalTotal = 0;
ULONGLONG lastStats = 0;

void RefreshProgress() {
    ULONGLONG now = GetTickCount64();
    CheckUnlocks();
    if (now - lastProgress < 3000 || !LegitVisible() || !st.inWorld) return;
    lastProgress = now;
    void* ps = PlayerState();
    if (!ps) return;
    LoadTables();
    std::vector<core::ProgressStat> stats;
    std::vector<core::ProgressEntry> lists[4];
    std::unordered_map<std::string, std::string> idByName;
    std::set<std::string> realItems;
    struct ItemRef {
        std::string id, name, category;
    };
    std::vector<ItemRef> visibleItems;
    {
        std::lock_guard<std::mutex> g(*modelLock);
        for (const core::Item& it : model->items) {
            if (it.real) {
                realItems.insert(it.id);
                idByName[LowerAscii(it.name)] = it.id;
            }
            if (!it.hidden) visibleItems.push_back({it.id, it.name, it.category});
        }
    }

    auto recipes = AllCached("GetAllCraftingRecipes");
    for (int i = 0; i < recipes.num && recipes.data; ++i) {
        void* r = recipes.data[i];
        if (!ue::Alive(r) || !Published(r)) continue;
        const RecipeInfo& info = InfoOf(r);
        if (!info.out.empty() && !recipeForItem.count(info.out)) recipeForItem[info.out] = r;
    }

    std::vector<int> evidence;
    struct Candidate {
        core::ProgressEntry e;
        int tier;
    };
    std::vector<Candidate> recipeRows, scanRows;
    std::unordered_map<std::string, int> recipeTierByName;
    int rDone = 0, rTotal = 0;
    std::set<std::string> giveIds;
    for (int i = 0; i < recipes.num && recipes.data; ++i) {
        void* r = recipes.data[i];
        if (!ue::Alive(r) || !Published(r)) continue;
        std::string name = TextOf(r, "Name");
        if (name.empty() || Placeholder(name)) continue;
        const RecipeInfo& info = InfoOf(r);
        if (info.entitlementOnly) continue;
        if (!info.out.empty() && !realItems.count(info.out)) continue;
        int tier = RecipeTier(r, 0);
        recipeTierByName[LowerAscii(name)] = tier;
        bool done = RecipeUnlocked(r, ps);
        if (info.noRules && !done) continue;
        ++rTotal;
        if (done) {
            ++rDone;
            if (!info.out.empty()) giveIds.insert(info.out);
            if (!info.defaultUnlocked && tier >= 0) evidence.push_back(tier);
            continue;
        }
        core::ProgressEntry e;
        e.id = ue::Name(r);
        e.name = name;
        e.icon = info.out;
        if (e.icon.empty()) {
            e.icon = "recipe_" + e.id;
            QueueIcon(r, e.icon);
        }
        int steps = 0;
        for (const auto& rule : RulesOf(r)) steps += (int)rule.size();
        int shown = tier < 0 ? 2 : tier;
        e.order = shown * 1000 + steps;
        e.detail = (steps ? std::to_string(steps) + (steps == 1 ? " step to unlock" : " steps to unlock") : std::string("Locked")) +
                   "  \xc2\xb7  " + kStage[shown];
        progressAssets["0:" + e.id] = r;
        recipeRows.push_back({e, shown});
    }
    stats.push_back({"Blueprints", rDone, rTotal});

    auto nameTier = [&](const std::string& name) {
        std::string low = LowerAscii(name);
        auto n = tierByName.find(low);
        if (n != tierByName.end() && n->second >= 0) return n->second;
        auto it = idByName.find(low);
        if (it != idByName.end()) {
            int t = ItemTier(it->second);
            if (t >= 0) return t;
        }
        auto rt = recipeTierByName.find(low);
        if (rt != recipeTierByName.end() && rt->second >= 0) return rt->second;
        return -1;
    };

    auto scans = AllCached("GetAllScanDatas");
    int sDone = 0, sTotal = 0;
    for (int i = 0; i < scans.num && scans.data; ++i) {
        void* s = scans.data[i];
        if (!ue::Alive(s) || !Published(s)) continue;
        std::string name = TextOf(s, "Name");
        if (name.empty() || Placeholder(name)) continue;
        ++sTotal;
        int tier = nameTier(name);
        if (ScanDone(s, ps)) {
            ++sDone;
            if (tier >= 0) evidence.push_back(tier);
            continue;
        }
        core::ProgressEntry e;
        e.id = ue::Name(s);
        e.name = name;
        auto icon = idByName.find(LowerAscii(name));
        if (icon != idByName.end()) {
            e.icon = icon->second;
        } else {
            e.icon = "scan_" + e.id;
            QueueIcon(s, e.icon);
        }
        int needed = ue::Read<int32_t>(s, ue::FindProp(ue::ClassOf(s), "NumRequired"));
        int shown = tier < 0 ? 2 : tier;
        e.detail = std::string(needed > 1 ? "Scan " + std::to_string(needed) + " times" : "Not scanned yet") + "  \xc2\xb7  " + kStage[shown];
        e.order = shown * 1000 + needed;
        progressAssets["2:" + e.id] = s;
        scanRows.push_back({e, shown});
    }
    stats.push_back({"Scans", sDone, sTotal});

    if (!lastStats || now - lastStats > 15000) {
        lastStats = now;
        auto entries = AllCached("GetAllDatabankEntries");
        cachedDataDone = cachedDataTotal = 0;
        for (int i = 0; i < entries.num && entries.data; ++i) {
            void* d = entries.data[i];
            if (!ue::Alive(d) || !Published(d)) continue;
            ++cachedDataTotal;
            ue::Call c(d, "IsUnlocked");
            if (c.Run() && c.GetBool("ReturnValue")) ++cachedDataDone;
        }
        auto goals = AllCached("GetAllStoryGoals");
        cachedGoalDone = cachedGoalTotal = 0;
        void* sg = Cdo("/Script/UWEStoryGoals.Default__UWEStoryGoalsStatics");
        for (int i = 0; i < goals.num && goals.data; ++i) {
            void* g = goals.data[i];
            if (!ue::Alive(g) || !Published(g)) continue;
            ++cachedGoalTotal;
            ue::Call c(sg, "IsStoryGoalUnlocked");
            c.Set<void*>("WorldContext", st.world).Set<void*>("StoryGoal", g).Set<void*>("ForPlayerState", ps);
            if (c.Run() && c.GetBool("ReturnValue")) ++cachedGoalDone;
        }
    }
    stats.push_back({"Databank", cachedDataDone, cachedDataTotal});
    stats.push_back({"Story", cachedGoalDone, cachedGoalTotal});

    void* inv = Inventory();
    if (inv) {
        for (const ItemRef& it : visibleItems) {
            int t = ItemTier(it.id);
            if (t <= 0) continue;
            auto type = itemTypes.find(it.id);
            if (type == itemTypes.end() || !ue::Alive(type->second)) continue;
            if (CountItem(inv, type->second) > 0) evidence.push_back(t);
        }
    }
    ue::FVector me;
    ActorPosFast(st.pawn, me);
    double depth = seaLevel - me.z / 100.0;
    evidence.push_back(DepthTier(depth));
    std::sort(evidence.begin(), evidence.end(), std::greater<int>());
    int stage = evidence.size() >= 2 ? evidence[1] : evidence.empty() ? 0 : std::max(0, evidence[0] - 1);
    stage = std::clamp(stage, 0, 4);
    int limit = stage + 1;
    static int loggedStage = -1;
    if (stage != loggedStage) {
        loggedStage = stage;
        LOG("legit run stage: %s (%zu clues, depth %.0f m)", kStage[stage], evidence.size(), depth);
    }

    for (Candidate& c : recipeRows)
        if (c.tier <= limit) lists[0].push_back(std::move(c.e));
    for (Candidate& c : scanRows)
        if (c.tier <= limit) lists[2].push_back(std::move(c.e));

    lists[1] = PingLocations(me);
    void* pod = nullptr;
    ue::FVector podPos{};
    double podBest = 1e18;
    for (const Tracked& t : tracked) {
        if (t.kind != KSignal || t.name != "Lifepod") continue;
        ue::FVector p;
        if (!ActorPosFast(t.actor, p) || (std::fabs(p.x) < 1 && std::fabs(p.y) < 1)) continue;
        double d = Dist(p, me);
        for (const core::ProgressEntry& e : lists[1])
            if (LowerAscii(e.name).find("lifepod") != std::string::npos &&
                Dist(p, ue::FVector{e.x * 100.0, e.y * 100.0, e.z * 100.0}) < 60.0)
                d -= 100000.0;
        if (d < podBest) {
            podBest = d;
            pod = t.actor;
            podPos = p;
        }
    }
    if (pod) {
        lists[1].erase(std::remove_if(lists[1].begin(), lists[1].end(),
                                      [&](const core::ProgressEntry& e) {
                                          return LowerAscii(e.name).find("lifepod") != std::string::npos &&
                                                 Dist(podPos, ue::FVector{e.x * 100.0, e.y * 100.0, e.z * 100.0}) < 60.0;
                                      }),
                       lists[1].end());
        core::ProgressEntry e;
        e.name = "Your lifepod";
        e.detail = "Lifepod";
        e.hasPos = true;
        e.x = (float)(podPos.x / 100);
        e.y = (float)(podPos.y / 100);
        e.z = (float)(podPos.z / 100);
        e.order = -1;
        lists[1].push_back(e);
    }
    for (const Tracked& t : tracked) {
        if (t.kind != KBase && t.kind != KVehicle) continue;
        ue::FVector p;
        if (!ActorPosFast(t.actor, p) || (std::fabs(p.x) < 1 && std::fabs(p.y) < 1)) continue;
        core::ProgressEntry e;
        e.name = t.name;
        e.detail = t.kind == KBase ? "Your base" : "Vehicle";
        e.hasPos = true;
        e.x = (float)(p.x / 100);
        e.y = (float)(p.y / 100);
        e.z = (float)(p.z / 100);
        e.order = (int)Dist(p, me);
        lists[1].push_back(e);
    }
    void* signalCls = ue::FindClassByName("BP_NarrativeSignal_C");
    void* goalLib = Cdo("/Script/UWEStoryGoals.Default__UWEStoryGoalsStatics");
    int signalsShown = 0, signalsSeen = 0;
    for (const Tracked& t : tracked) {
        if (t.kind != KSignal || !signalCls || !ue::IsA(t.actor, signalCls)) continue;
        ue::FVector p;
        if (!ActorPosFast(t.actor, p) || (std::fabs(p.x) < 1 && std::fabs(p.y) < 1)) continue;
        std::string name = SignalName(t.actor);
        if (name.empty() || name == "Signal" || Placeholder(name)) continue;
        ++signalsSeen;
        std::string low = LowerAscii(name);
        bool dup = false;
        for (const core::ProgressEntry& e : lists[1])
            if (LowerAscii(e.name) == low || Dist(p, ue::FVector{e.x * 100.0, e.y * 100.0, e.z * 100.0}) < 60.0) dup = true;
        if (dup) continue;
        int tier = DepthTier(seaLevel - p.z / 100.0);
        if (tier > limit) continue;
        auto goalDone = [&](const char* prop) {
            void* goal = ue::Ptr(t.actor, prop);
            if (!goal || !goalLib) return false;
            ue::Call c(goalLib, "IsStoryGoalUnlocked");
            c.Set<void*>("WorldContext", st.world).Set<void*>("StoryGoal", goal).Set<void*>("ForPlayerState", ps);
            return c.Run() && c.GetBool("ReturnValue");
        };
        if (goalDone("RemoveOnStoryGoal") || goalDone("HideSignalOnStoryGoal")) continue;
        bool known = !ue::Ptr(t.actor, "StoryGoal") || goalDone("StoryGoal");
        core::ProgressEntry e;
        e.name = name;
        e.detail = std::string(known ? "Signal" : "Not found yet") + "  \xc2\xb7  " + kStage[tier];
        e.hasPos = true;
        e.x = (float)(p.x / 100);
        e.y = (float)(p.y / 100);
        e.z = (float)(p.z / 100);
        e.order = 100000 + (int)Dist(p, me);
        lists[1].push_back(e);
        ++signalsShown;
    }
    static int loggedSignals = -1;
    if (signalsSeen != loggedSignals) {
        loggedSignals = signalsSeen;
        LOG("legit run: %d game signals found, %d fit your stage", signalsSeen, signalsShown);
    }

    for (const ItemRef& it : visibleItems) {
        bool resource = it.category == "Resources";
        if (!resource && !giveIds.count(it.id)) continue;
        int t = ItemTier(it.id);
        int shown = t < 0 ? 2 : t;
        if (shown > (resource ? stage : limit)) continue;
        core::ProgressEntry e;
        e.id = it.id;
        e.name = it.name;
        e.icon = it.id;
        e.detail = std::string(resource ? "Resource" : "You have the blueprint") + "  \xc2\xb7  " + kStage[shown];
        e.order = shown * 10 + (resource ? 0 : 1);
        lists[3].push_back(e);
    }

    std::lock_guard<std::mutex> g(*modelLock);
    model->progressStats = std::move(stats);
    for (int k = 0; k < 4; ++k) model->progress[k] = std::move(lists[k]);
    model->progressStage = kStage[stage];
    model->progressLoaded = true;
}

void ProgressAction(int kind, const core::ProgressEntry& e) {
    if (kind == 1) {
        if (MapScanActive()) {
            Note("Stop the map scan first");
            return;
        }
        void* who = st.vehicle ? st.vehicle : st.pawn;
        if (!TeleportTo(who, ue::FVector{e.x * 100.0, e.y * 100.0, (e.z + 2.0) * 100.0})) Note("Teleport failed");
        return;
    }
    if (kind == 3) {
        auto t = itemTypes.find(e.id);
        if (t != itemTypes.end() && GiveItem(t->second, 1) <= 0) Note("Your inventory is full");
        return;
    }
    auto a = progressAssets.find(std::to_string(kind) + ":" + e.id);
    if (a == progressAssets.end() || !ue::Alive(a->second)) {
        Note("Not available right now");
        return;
    }
    LoadVerbs();
    if (kind == 0) {
        if (!UnlockRecipe(a->second, false)) {
            LogRules(a->second, e.name);
            Note("The game didn't allow unlocking " + e.name);
            return;
        }
        pendingUnlocks.push_back({a->second, e.name, GetTickCount64(), 0, false});
    }
    if (kind == 2) {
        if (!CompleteScan(a->second)) {
            Note("The game didn't accept the scan for " + e.name);
            return;
        }
        pendingUnlocks.push_back({a->second, e.name, GetTickCount64(), 1, true});
    }
    lastProgress = GetTickCount64() - 2000;
}
void* Inventory() {
    if (!st.pawn) return nullptr;
    ue::Call c(st.pawn, "GetInventory");
    if (!c.Run()) return nullptr;
    void* inv = c.Get<void*>("ReturnValue");
    return ue::Valid(inv) ? inv : nullptr;
}

int GiveItem(void* type, int amount) {
    void* inv = Inventory();
    if (!inv || !ue::Valid(type)) return -1;
    ue::Call c(inv, "AddAsMuchAsPossibleToInventory");
    c.Set<void*>("ItemType", type).Set<int32_t>("Quantity", amount);
    if (!c.Run()) return -1;
    return c.Get<int32_t>("ReturnValue");
}

void OnWorldChanged() {
    tracked.clear();
    scanBuild.clear();
    scanCursor = -1;
    lastScan = 0;
    nearby.clear();
    healthMemo.clear();
    boundsMemo.clear();
    ownedMemo.clear();
    signalNames.clear();
    camManager = nullptr;
    progressAssets.clear();
    pendingUnlocks.clear();
    allOfCache.clear();
    recipeInfo.clear();
    recipeForItem.clear();
    recipeTierMemo.clear();
    unlockSnaps.clear();
    useCounts.clear();
    useInventory = nullptr;
    iconRt = nullptr;
    extraIcons.clear();
    extraSeen.clear();
    freeCam = FreeCamState{};
    noclip = NoClipState{};
    undoSpot = Teleported{};
    lastStats = 0;
    cachedDataDone = cachedDataTotal = cachedGoalDone = cachedGoalTotal = 0;
    ResetRig();
    mapRun = MapRun{};
    scanForce = false;
    fogComps.clear();
    fogBuild.clear();
    crafterComps.clear();
    crafterBuild.clear();
    compClassMemo.clear();
    compCursor = -1;
    compScanDone = false;
    fogSaves.clear();
    crafterSaves.clear();
    lastBuildComp = nullptr;
    EnforceAbilityCooldowns(false);
    for (Saved* s : {&speedSave, &jumpSave, &tempSave, &scanSave, &vSpeedSave, &subSpeedSave}) s->active = false;
    for (LooseTag& t : looseTags) t.appliedTo = t.appliedState = nullptr;
    for (CfgField& f : cfgFields) f.active = f.byScan = false;
    timeActive = false;
    st.lastPlayer = nullptr;
    st.pawn = nullptr;
    st.cm = nullptr;
    st.triedCheatsFor = nullptr;
    lastProgress = 0;
    lastAdapt = 0;
    std::lock_guard<std::mutex> g(*modelLock);
    model->esp.clear();
    model->itemEsp.clear();
    model->progressLoaded = false;
    model->canUndoTeleport = false;
    model->mapScanning = false;
    model->mapScanProgress = 0;
    model->mapScanText.clear();
    bool switched = false;
    for (Feature& f : model->features)
        if (f.on && f.id.rfind("unlock_", 0) == 0) {
            core::SetFeature(*model, f, false);
            want[f.id] = {false, f.value};
            switched = true;
        }
    if (switched) Note("Unlock all switches were turned off because you left the save");
}

void RunTasks() {
    std::vector<std::function<void()>> run;
    {
        std::lock_guard<std::mutex> g(taskLock);
        run.swap(tasks);
    }
    for (auto& fn : run) fn();
}

uint64_t lastTickFrame = 0;

bool Stale(void* p) { return p && !ue::Alive(p); }

void Tick() {
    if (shuttingDown) {
        if (!shutdownDone) {
            Refresh();
            EndMapScan();
            StopFreeCam();
            StopNoClip();
            Enforce(true);
            SaveSurvey();
            shutdownDone = true;
        }
        return;
    }
    uint64_t frame = overlay::FrameCount();
    if (frame == lastTickFrame) return;
    lastTickFrame = frame;
    ULONGLONG now = GetTickCount64();
    bool stale = Stale(st.world) || Stale(st.pc) || Stale(st.pawn) || Stale(st.charMove) || Stale(st.vehicle) || Stale(st.sub) ||
                 Stale(st.healthSet) || Stale(st.survivalSet) || Stale(st.movement) || Stale(st.health) || Stale(st.cm);
    if (stale || now - lastRefresh >= 250) {
        lastRefresh = now;
        Refresh();
        if (st.session == Session::Client) {
            std::lock_guard<std::mutex> g(*modelLock);
            if (model->activeCount() > 0) core::DisableAll(*model);
        }
    }
    RunTasks();
    if (now - lastSpeed >= 100) {
        lastSpeed = now;
        EnforceSpeed(st.inWorld && st.session != Session::Client);
    }
    if (now - lastEnforce >= 400) {
        lastEnforce = now;
        Enforce(false);
        PumpUnlockUndo();
    }
    FrameTick();
    TickMapScan();
    TrackActors();
    RecordSpots();
    UpdateEsp();
    ClearCooldowns();
    UpdateItemEsp();
    RefreshProgress();
    bool hostOk = st.inWorld && st.session != Session::Client;
    PumpComponentScan(hostOk && (On("hide_water_fog") || On("performance_mode") || On("instant_craft")));
    InstantBuild(hostOk);
    UnlimitedUse(hostOk);
    RefreshAdaptations();
    Survey();
    PumpItems();
}

void __fastcall HookedPE(void* obj, void* fn, void* parms) {
    ++peInflight;
    if (!inTick && GetCurrentThreadId() == gameThread) {
        inTick = true;
        try {
            Tick();
        } catch (const std::exception& e) {
            static int reported = 0;
            if (reported++ < 5) LOG("error in game update: %s", e.what());
        } catch (...) {
            static int reported = 0;
            if (reported++ < 5) LOG("unknown error in game update");
        }
        inTick = false;
    }
    oPE(obj, fn, parms);
    --peInflight;
}

void Implement(const char* id, std::function<void(Feature&)> apply) {
    Feature* f = model->find(id);
    if (!f) return;
    f->available = true;
    f->apply = std::move(apply);
}

}

void Wire(core::Model& m, std::mutex& lock, unsigned long thread) {
    model = &m;
    modelLock = &lock;
    gameThread = thread;
    auto none = [](Feature&) {};
    for (const TagCheat& t : tagCheats) Implement(t.id, none);
    Implement("stable_temperature", none);
    Implement("player_speed", none);
    Implement("jump_height", none);
    Implement("refill_health", [](Feature&) {
        Enqueue([] {
            if (st.health && ue::Call(st.health, "RestoreHealth").Run()) return;
            Note("Could not refill health");
        });
    });
    auto refill = [](const char* attr, const char* maxAttr, const char* what) {
        std::string a = attr, mx = maxAttr, w = what;
        return [a, mx, w](Feature&) {
            Enqueue([a, mx, w] {
                if (!st.survivalSet) {
                    Note("Could not refill " + w);
                    return;
                }
                Attr top = ReadAttr(st.survivalSet, mx.c_str());
                if (top.cur > 0) WriteAttr(st.survivalSet, a.c_str(), top.cur);
            });
        };
    };
    Implement("refill_food", refill("Food", "MaxFood", "hunger"));
    Implement("refill_water", refill("Water", "MaxWater", "thirst"));
    Implement("refill_oxygen", refill("Oxygen", "MaxOxygen", "oxygen"));
    for (const char* id : {"unlock_blueprints", "unlock_databank", "unlock_dna", "free_camera_speed", "fly_speed", "godmode", "infinite_oxygen", "unlimited_food", "unlimited_water", "unlimited_battery", "noclip", "fly",
                           "instant_build", "instant_deconstruct", "free_craft", "unlimited_power", "unlimited_use",
                           "esp_box", "hide_water_fog", "free_camera", "performance_mode", "game_speed", "instant_craft", "unlimited_biomod_energy",
                           "zero_cooldowns", "instant_scan", "instant_biosampler", "vehicle_hull", "vehicle_speed", "esp", "esp_hostile",
                           "esp_passive", "esp_leviathan", "esp_name", "esp_distance", "esp_health", "item_esp", "itemesp_items", "itemesp_data", "itemesp_crates", "itemesp_name", "itemesp_distance", "itemesp_box"})
        Implement(id, none);
    m.spawn = [](const core::Item& it, int amount) {
        std::string id = it.id, name = it.name;
        Enqueue([id, name, amount] {
            int added = GiveItem(itemTypes[id], amount);
            if (added < 0) Note("Could not add " + name);
            else if (added == 0) Note("Your inventory is full");
            else if (added < amount) Note("Only " + std::to_string(added) + " fit in your inventory");
        });
    };
    m.addAllResources = [] {
        Enqueue([] {
            std::vector<std::string> ids;
            {
                std::lock_guard<std::mutex> g(*modelLock);
                for (const core::Item& it : model->items)
                    if (it.category == "Resources") ids.push_back(it.id);
            }
            int total = 0;
            for (const std::string& id : ids) {
                int a = GiveItem(itemTypes[id], 5);
                if (a > 0) total += a;
            }
            if (total == 0) Note("Your inventory is full");
        });
    };
    m.duplicateHeld = [] {
        Enqueue([] {
            ue::Call c(Cdo("/Script/Subnautica2.Default__SN2Statics"), "GetLocalEquippedTool");
            c.Set<void*>("WorldContextObject", st.world);
            void* tool = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
            void* type = ue::Valid(tool) ? ue::Ptr(tool, "ItemType") : nullptr;
            if (!type) {
                Note("Hold an item in your hand first");
                return;
            }
            if (GiveItem(type, 1) <= 0) Note("Your inventory is full");
        });
    };
    Implement("unlock_scans", [](Feature&) { Enqueue([] { UnlockAllScans(); lastProgress = 0; }); });
    m.progressAction = [](int kind, const core::ProgressEntry& e) {
        core::ProgressEntry copy = e;
        Enqueue([kind, copy] { ProgressAction(kind, copy); });
    };
    m.teleport = [](float x, float y, float z, bool useZ) {
        Enqueue([x, y, z, useZ] {
            if (MapScanActive()) {
                Note("Stop the map scan first");
                return;
            }
            void* who = st.vehicle ? st.vehicle : st.pawn;
            ue::FVector cur;
            if (!who || !ActorPos(who, cur)) return;
            double zm = cur.z / 100.0;
            float ground = 0;
            if (useZ) zm = z + 1.5;
            else if (GroundAt(x, y, ground)) zm = ground + (ground > seaLevel ? 2.0 : 4.0);
            if (!TeleportTo(who, ue::FVector{x * 100.0, y * 100.0, zm * 100.0})) Note("Teleport failed");
        });
    };
    m.undoTeleport = [] {
        Enqueue([] {
            void* who = st.vehicle ? st.vehicle : st.pawn;
            if (!who || !undoSpot.valid || MapScanActive()) return;
            MoveActor(who, undoSpot.loc);
            undoSpot.valid = false;
            std::lock_guard<std::mutex> g(*modelLock);
            model->canUndoTeleport = false;
        });
    };
    m.mapScan = [](bool start) {
        Enqueue([start] {
            if (start) StartMapScan();
            else StopMapScan();
        });
    };
    m.saveBase = [](const std::string& name) { Enqueue([name] { SaveBase(name); }); };
    m.placeBase = [](const core::BaseInfo& b) {
        std::string file = b.file;
        Enqueue([file] { PlaceBase(file); });
    };
    m.deleteBase = [](const core::BaseInfo& b) {
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(b.file), ec);
        Enqueue([] { ListBases(); });
    };
    ListBases();
    m.setAdaptation = [](int index, bool on) {
        Enqueue([index, on] { SetAdaptation(index, on); });
    };
    if (Feature* f = m.find("stable_temperature")) f->hint = "No heat or cold damage";
    for (Feature& f : m.features)
        if (!f.available) f.missing = core::NotYetReason(f.id);
}

void SetFolder(const std::wstring& folder) { iconDir = folder + L"\\icons"; }

bool Hook() {
    peTarget = ue::ProcessEventAddress();
    if (!peTarget) return false;
    if (MH_CreateHook(peTarget, (void*)&HookedPE, (void**)&oPE) != MH_OK || MH_EnableHook(peTarget) != MH_OK) {
        LOG("ProcessEvent hook failed");
        return false;
    }
    LOG("game thread hook installed");
    return true;
}

void Shutdown() {
    shuttingDown = true;
    for (int i = 0; i < 300 && !shutdownDone; ++i) Sleep(10);
    if (!shutdownDone) LOG("game thread did not respond before unload");
    if (mapJob.valid()) mapJob.wait();
}

void Unhook() {
    if (peTarget) MH_DisableHook(peTarget);
    for (int i = 0; i < 200 && peInflight.load() > 0; ++i) Sleep(10);
}

void SetMenuState(bool open, bool mapVisible, bool legitVisible) {
    menuOpenFlag = open;
    mapVisibleFlag = mapVisible;
    legitVisibleFlag = legitVisible;
}

std::vector<std::string> TakeNotes() {
    std::lock_guard<std::mutex> g(noteLock);
    std::vector<std::string> out;
    out.swap(notes);
    return out;
}

}
