#include "game/game.h"
#include "core/bl3_features.h"
#include "game/internal.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace bl3 {

State st;

namespace {

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
DWORD gameThread = 0;
std::wstring folder;
std::function<std::string()> backupFn;

struct FeatureState {
    bool on = false;
    float value = 0.0f;
};

std::unordered_map<std::string, FeatureState> snap;
std::mutex reqLock;
std::vector<std::string> pressedQueue, pressedNow;
std::vector<std::pair<std::string, std::vector<std::string>>> submitQueue, submitNow;
std::mutex noteLock;
std::vector<std::string> notes;
std::unordered_map<std::string, bool> implemented;
std::unordered_map<std::string, std::string> missingWhy;

std::atomic<bool> stopping{false}, stopped{false};
std::atomic<int> inflight{0};
bool inTick = false;
std::atomic<bool> menuOpen{false}, menuFocused{false}, mapVisible{false};

using PEFn = void(__fastcall*)(void*, void*, void*);
PEFn oPE = nullptr;
void* peTarget = nullptr;

void* scanRoots[(int)Scan::Count] = {};
std::vector<void*> scanBuilding[(int)Scan::Count], scanReady[(int)Scan::Count];
bool scanDone[(int)Scan::Count] = {};
int32_t scanCursor = 0;
std::unordered_map<void*, uint32_t> classMask;
ue::FWeak lastWorld;

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

void InitScanRoots() {
    const char* names[(int)Scan::Count] = {"OakCharacter",  "OakVehicle",    "InventoryItemPickup", "LootableObject",        "LightProjectile",
                                           "OakActionAbility", "InventoryData", "ItemPoolData",        "ItemPoolExpansionData",
                                           "InventoryBalanceData"};
    for (int k = 0; k < (int)Scan::Count; ++k) scanRoots[k] = ue::FindClass(names[k]);
}

uint32_t MaskFor(void* cls) {
    if (!cls) return 0;
    auto it = classMask.find(cls);
    if (it != classMask.end()) return it->second;
    uint32_t mask = 0;
    int guard = 0;
    for (void* c = cls; c && guard < 64; c = ue::SuperOf(c), ++guard)
        for (int k = 0; k < (int)Scan::Count; ++k)
            if (scanRoots[k] && c == scanRoots[k]) mask |= 1u << k;
    classMask[cls] = mask;
    return mask;
}

void ScanStep() {
    ue::FWeak w = ue::MakeWeak(st.world);
    if (w.index != lastWorld.index || w.serial != lastWorld.serial) {
        lastWorld = w;
        classMask.clear();
        scanCursor = 0;
        for (int k = 0; k < (int)Scan::Count; ++k) {
            scanBuilding[k].clear();
            scanReady[k].clear();
            scanDone[k] = false;
        }
    }
    if (!st.inWorld) return;
    int32_t n = ue::ObjectCount();
    double until = Now() + 0.00025;
    int budget = 30000;
    for (; budget > 0 && scanCursor < n; --budget, ++scanCursor) {
        if ((budget & 255) == 0 && Now() > until) break;
        void* o = ue::ObjectAt(scanCursor);
        if (!o) continue;
        uint32_t mask = MaskFor(ue::ClassOf(o));
        if (!mask || ue::IsDefault(o)) continue;
        for (int k = 0; k < (int)Scan::Count; ++k)
            if (mask & (1u << k)) scanBuilding[k].push_back(o);
    }
    if (scanCursor >= n) {
        for (int k = 0; k < (int)Scan::Count; ++k) {
            scanReady[k].swap(scanBuilding[k]);
            scanBuilding[k].clear();
            scanDone[k] = true;
        }
        scanCursor = 0;
    }
}

ue::TArray<void*> ArrayOf(void* obj, const char* prop) {
    ue::TArray<void*> a;
    if (!obj) return a;
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    if (p) a = ue::Read<ue::TArray<void*>>(obj, p);
    if (a.num < 0 || a.num > 4096) a.num = 0;
    return a;
}

void* ArrayAt(const ue::TArray<void*>& a, int i) {
    void* v = nullptr;
    if (i >= 0 && i < a.num && a.data) ue::SafeRead(a.data + i, v);
    return v;
}

void Refresh() {
    static void* engineCls = nullptr;
    if (!ue::Alive(st.engine)) {
        if (!engineCls) engineCls = ue::FindObject("/Script/Engine.GameEngine");
        st.engine = ue::FindFirstOf(engineCls);
    }
    void* vp = ue::Ptr(st.engine, "GameViewport");
    st.world = vp ? ue::Ptr(vp, "World") : nullptr;
    st.gameInstance = st.world ? ue::Ptr(st.world, "OwningGameInstance") : nullptr;
    st.gameState = st.world ? ue::Ptr(st.world, "GameState") : nullptr;
    ue::TArray<void*> lps = ArrayOf(st.gameInstance, "LocalPlayers");
    void* lp = ArrayAt(lps, 0);
    st.pc = ue::Alive(lp) ? ue::Ptr(lp, "PlayerController") : nullptr;
    static void* playerCls = nullptr;
    static void* vehicleCls = nullptr;
    if (!playerCls) playerCls = ue::FindClass("OakCharacter_Player");
    if (!vehicleCls) vehicleCls = ue::FindClass("OakVehicle");
    void* pawn = st.pc ? ue::Ptr(st.pc, "Pawn") : nullptr;
    if (!ue::Alive(pawn)) pawn = nullptr;
    static void* seatCls = nullptr;
    if (!seatCls) seatCls = ue::FindClass("SeatPawn");
    void* character = nullptr;
    if (pawn && ue::IsA(pawn, playerCls)) {
        character = pawn;
    } else if (pawn && ((vehicleCls && ue::IsA(pawn, vehicleCls)) || (seatCls && ue::IsA(pawn, seatCls)))) {
        for (void* c : {ue::Ptr(st.pc, "OakCharacter"), ue::Ptr(st.pc, "CachedPrimaryCharacter")})
            if (c && ue::Alive(c) && ue::IsA(c, playerCls)) {
                character = c;
                break;
            }
    }
    st.pawn = character;
    st.seated = pawn && character && pawn != character;
    st.camera = st.pc ? ue::Ptr(st.pc, "PlayerCameraManager") : nullptr;
    st.vehicle = nullptr;
    if (pawn && pawn != character && vehicleCls && ue::IsA(pawn, vehicleCls)) st.vehicle = pawn;
    if (!st.vehicle && st.pawn) {
        ue::Call c(st.pawn, "GetVehicle");
        if (c.Run()) {
            void* v = c.Get<void*>("ReturnValue");
            if (ue::Alive(v)) st.vehicle = v;
        }
    }
    void* nd = st.world ? ue::Ptr(st.world, "NetDriver") : nullptr;
    st.session = !nd ? core::Session::Solo : (ue::Ptr(nd, "ServerConnection") ? core::Session::Client : core::Session::Host);
    int others = 0;
    if (st.gameState) {
        void* mine = st.pc ? ue::Ptr(st.pc, "PlayerState") : nullptr;
        ue::TArray<void*> ps = ArrayOf(st.gameState, "PlayerArray");
        for (int i = 0; i < ps.num; ++i) {
            void* p = ArrayAt(ps, i);
            if (!ue::Alive(p) || p == mine) continue;
            if (ue::GetBool(p, "bIsABot") || ue::GetBool(p, "bOnlySpectator") || ue::GetBool(p, "bIsInactive")) continue;
            ++others;
        }
        if (ue::GetBool(st.gameState, "bClientJoining")) others = std::max(others, 1);
    }
    if (nd) {
        ue::TArray<void*> conns = ArrayOf(nd, "ClientConnections");
        others = std::max(others, conns.num);
    }
    others += std::max(0, lps.num - 1);
    if (st.session == core::Session::Client) others = std::max(others, 1);
    st.others = others;
    st.inWorld = st.pawn != nullptr;
    st.canCheat = st.session != core::Session::Client;
    st.alone = st.canCheat && st.others == 0;
}

void Snapshot() {
    {
        std::lock_guard<std::mutex> g(*modelLock);
        for (const core::Feature& f : model->features) snap[f.id] = {f.on, f.value};
    }
    st.menuOpen = menuOpen;
    st.mapVisible = mapVisible;
    std::lock_guard<std::mutex> g(reqLock);
    pressedNow.swap(pressedQueue);
    pressedQueue.clear();
    submitNow.swap(submitQueue);
    submitQueue.clear();
}

void Gate() {
    bool client = st.session == core::Session::Client;
    std::vector<std::string> out;
    {
        std::lock_guard<std::mutex> g(*modelLock);
        model->session = st.session;
        model->inWorld = st.inWorld;
        if (client && model->activeCount() > 0) {
            core::DisableAll(*model);
            out.push_back("Cheats are off: you are not the host");
        }
        for (core::Feature& f : model->features) {
            auto it = implemented.find(f.id);
            if (it == implemented.end() || !it->second) continue;
            if (!AloneOnly(f.id)) continue;
            if (!st.alone) {
                if (f.kind == core::Kind::Toggle && f.on) {
                    f.on = false;
                    snap[f.id].on = false;
                    out.push_back(f.label + " is off: another player is in your game");
                }
                f.available = false;
                f.missing = "Only when you play alone";
            } else {
                f.available = true;
            }
        }
    }
    if (!out.empty()) {
        std::lock_guard<std::mutex> g(noteLock);
        for (auto& s : out) notes.push_back(s);
    }
}

void OffAll() {
    PlayerOff();
    WeaponsOff();
    SkillsOff();
    MovementOff();
    AimOff();
    LootOff();
    GiverOff();
    WorldOff();
    MapshotOff();
    VehicleOff();
}

void Tick() {
    static double last = 0;
    double now = Now();
    if (now - last < 1.0 / 120.0) return;
    st.dt = (float)std::clamp(now - last, 0.0, 0.1);
    last = now;
    st.time = now;
    ++st.tick;
    Refresh();
    Snapshot();
    ScanStep();
    if (stopping) {
        OffAll();
        stopped = true;
        return;
    }
    Gate();
    if (!st.inWorld || !st.canCheat) {
        OffAll();
        pressedNow.clear();
        submitNow.clear();
        return;
    }
    PlayerTick();
    WeaponsTick();
    SkillsTick();
    MovementTick();
    AimTick();
    LootTick();
    GiverTick();
    WorldTick();
    MapshotTick();
    VehicleTick();
    pressedNow.clear();
    submitNow.clear();
}

void __fastcall HookedPE(void* obj, void* fn, void* parms) {
    ++inflight;
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
    --inflight;
}

}

bool On(const char* id) {
    auto it = snap.find(id);
    return it != snap.end() && it->second.on;
}

float Value(const char* id) {
    auto it = snap.find(id);
    return it != snap.end() ? it->second.value : 0.0f;
}

bool Pressed(const char* id) {
    auto it = std::find(pressedNow.begin(), pressedNow.end(), id);
    if (it == pressedNow.end()) return false;
    pressedNow.erase(it);
    return true;
}

bool Submitted(const char* id, std::vector<std::string>& inputs) {
    for (auto it = submitNow.begin(); it != submitNow.end(); ++it) {
        if (it->first != id) continue;
        inputs = it->second;
        submitNow.erase(it);
        return true;
    }
    return false;
}

void Ready(const char* id, bool ok, const char* missing) {
    std::lock_guard<std::mutex> g(*modelLock);
    implemented[id] = ok;
    core::Feature* f = model->find(id);
    if (!f) return;
    f->available = ok;
    if (!ok) f->missing = missing ? missing : "Not found in this version of the game";
    if (f->kind == core::Kind::Action) {
        f->apply = [](core::Feature& x) {
            std::lock_guard<std::mutex> q(reqLock);
            pressedQueue.push_back(x.id);
        };
    } else if (f->kind == core::Kind::Input) {
        f->apply = [](core::Feature& x) {
            std::lock_guard<std::mutex> q(reqLock);
            submitQueue.push_back({x.id, x.inputs});
            x.result = "Working on it...";
        };
    }
    if (!ok) LOG("feature %s unavailable: %s", id, f->missing.c_str());
}

void Note(const std::string& text) {
    std::lock_guard<std::mutex> g(noteLock);
    notes.push_back(text);
}

void TurnOff(const char* id, const std::string& why) {
    {
        std::lock_guard<std::mutex> g(*modelLock);
        core::Feature* f = model->find(id);
        if (!f || !f->on) return;
        f->on = false;
    }
    snap[id].on = false;
    if (!why.empty()) Note(why);
}

void WithModel(const std::function<void(core::Model&)>& fn) {
    std::lock_guard<std::mutex> g(*modelLock);
    fn(*model);
}

bool KeyDown(int vk) {
    if (menuOpen && menuFocused) return false;
    if (!overlay::GameActive()) return false;
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

std::string BackupNow() { return backupFn ? backupFn() : std::string(); }

std::wstring Folder() { return folder; }

const std::vector<void*>& Objects(Scan kind) { return scanReady[(int)kind]; }

bool ScanComplete(Scan kind) { return scanDone[(int)kind]; }

void* Lib(const char* name) {
    if (std::strchr(name, '/')) return ue::FindObject(name);
    static std::unordered_map<std::string, ue::FWeak> cache;
    auto it = cache.find(name);
    if (it != cache.end()) {
        void* o = ue::WeakGet(it->second);
        if (o) return o;
    }
    void* cls = ue::FindClass(name);
    void* pkg = cls ? ue::OuterOf(cls) : nullptr;
    if (!pkg) return nullptr;
    void* o = ue::FindObject(ue::Name(pkg) + ".Default__" + name);
    if (o) cache[name] = ue::MakeWeak(o);
    return o;
}

void* Cls(const char* name) { return ue::FindClass(name); }

bool Has(const char* cls, const char* member) {
    void* c = ue::FindClass(cls);
    return c && (bool)ue::FindProp(c, member);
}

bool HasFunc(const char* cls, const char* func) {
    void* c = ue::FindClass(cls);
    return c && ue::FindFunction(c, func) != nullptr;
}

ue::FVector Location(void* actor) {
    ue::Call c(actor, "K2_GetActorLocation");
    if (!c.Run()) return {};
    return c.Get<ue::FVector>("ReturnValue");
}

ue::FRotator Rotation(void* actor) {
    ue::Call c(actor, "K2_GetActorRotation");
    if (!c.Run()) return {};
    return c.Get<ue::FRotator>("ReturnValue");
}

bool Teleport(void* actor, const ue::FVector& loc, const ue::FRotator& rot) {
    ue::Call c(actor, "K2_TeleportTo");
    c.Set("DestLocation", loc).Set("DestRotation", rot);
    return c.Run() && c.GetBool("ReturnValue");
}

bool CameraView(ue::FVector& loc, ue::FRotator& rot, float& fov) {
    if (!st.camera) return false;
    ue::Call l(st.camera, "GetCameraLocation"), r(st.camera, "GetCameraRotation"), f(st.camera, "GetFOVAngle");
    if (!l.Run() || !r.Run() || !f.Run()) return false;
    loc = l.Get<ue::FVector>("ReturnValue");
    rot = r.Get<ue::FRotator>("ReturnValue");
    fov = f.Get<float>("ReturnValue");
    return fov > 1.0f && fov < 179.0f;
}

ue::FRotator ControlRotation() {
    if (!st.pc) return {};
    return ue::Read<ue::FRotator>(st.pc, ue::FindProp(ue::ClassOf(st.pc), "ControlRotation"));
}

void SetControlRotation(const ue::FRotator& r) {
    ue::Call c(st.pc, "SetControlRotation");
    c.Set("NewRotation", r);
    c.Run();
}

void* DamageComp(void* actor) {
    if (!actor) return nullptr;
    void* d = ue::Ptr(actor, "OakDamageComponent");
    if (!d) d = ue::Ptr(actor, "DamageComponent");
    return d;
}

float Health(void* actor) {
    void* d = DamageComp(actor);
    ue::Call c(d, "GetCurrentHealth");
    return c.Run() ? c.Get<float>("ReturnValue") : 0.0f;
}

float MaxHealth(void* actor) {
    void* d = DamageComp(actor);
    ue::Call c(d, "GetMaxHealth");
    return c.Run() ? c.Get<float>("ReturnValue") : 0.0f;
}

bool Dead(void* actor) {
    void* d = DamageComp(actor);
    if (!d) return true;
    if (ue::GetBool(d, "bCurrentlyDead")) return true;
    return Health(actor) <= 0.0f;
}

bool IsPlayerPawn(void* actor) {
    if (!actor) return false;
    static void* playerCls = nullptr;
    if (!playerCls) playerCls = ue::FindClass("OakCharacter_Player");
    if (playerCls && ue::IsA(actor, playerCls)) return true;
    ue::Call c(actor, "IsPlayerControlled");
    return c.Run() && c.GetBool("ReturnValue");
}

namespace {

int Attitude(void* actor) {
    ue::Call c(Lib("OakBlueprintLibrary"), "GetAttitudeTowardPlayer");
    c.Set<void*>("SourceActor", actor);
    return c.Run() ? (int)c.Get<uint8_t>("ReturnValue") : -1;
}

bool TeamHostile(void* actor) {
    void* team = st.pawn ? ue::Ptr(actor, "TargetableComponent") : nullptr;
    if (!ue::Alive(team)) return false;
    ue::Call t(team, "GetTeamAttitudeTowardsActor");
    t.Set<void*>("Actor", st.pawn);
    return t.Run() && t.Get<uint8_t>("ReturnValue") == 2;
}

}

bool IsHostile(void* actor) {
    if (!actor || actor == st.pawn || IsPlayerPawn(actor)) return false;
    int a = Attitude(actor);
    return a >= 0 ? a == 2 : TeamHostile(actor);
}

bool IsEnemy(void* actor) {
    if (!actor || actor == st.pawn || IsPlayerPawn(actor)) return false;
    return Attitude(actor) == 2 || TeamHostile(actor);
}

int Hostility(void* actor, bool& strict, bool& loose) {
    strict = loose = false;
    if (!actor || actor == st.pawn || IsPlayerPawn(actor)) return 0;
    int a = Attitude(actor);
    if (a == 2) {
        strict = loose = true;
        return a;
    }
    loose = TeamHostile(actor);
    strict = a < 0 && loose;
    return a;
}

ue::FVector QuickLocation(void* actor) {
    static int ro = -2, lo = -1, ao = -1;
    if (ro == -2) {
        ue::Prop r = ue::FindProp(Cls("Actor"), "RootComponent");
        ue::Prop l = ue::FindProp(Cls("SceneComponent"), "RelativeLocation");
        ue::Prop a = ue::FindProp(Cls("SceneComponent"), "AttachParent");
        ro = r ? r.offset : -1;
        lo = l ? l.offset : -1;
        ao = a ? a.offset : -1;
    }
    void* root = (actor && ro >= 0) ? ue::At<void*>(actor, ro) : nullptr;
    if (root && lo >= 0 && ao >= 0 && !ue::At<void*>(root, ao)) {
        ue::FVector v = ue::At<ue::FVector>(root, lo);
        if (std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z)) return v;
    }
    return Location(actor);
}

void* ComponentOf(void* actor, const char* cls) {
    void* c = ue::FindClass(cls);
    if (!actor || !c) return nullptr;
    ue::Call call(actor, "GetComponentByClass");
    call.Set<void*>("ComponentClass", c);
    if (!call.Run()) return nullptr;
    void* r = call.Get<void*>("ReturnValue");
    return ue::Alive(r) ? r : nullptr;
}

void* HeldWeapon() {
    if (!st.pawn) return nullptr;
    ue::Call c(st.pawn, "GetActiveWeapon");
    c.Set<uint8_t>("Slot", 0);
    if (!c.Run()) return nullptr;
    void* w = c.Get<void*>("ReturnValue");
    return ue::Alive(w) ? w : nullptr;
}

std::vector<void*> PlayerWeapons() {
    std::vector<void*> out;
    void* held = HeldWeapon();
    if (held) out.push_back(held);
    if (st.vehicle && DrivingAlone()) {
        static void* vehWeaponCls = nullptr;
        if (!vehWeaponCls) vehWeaponCls = ue::FindClass("OakWeapon_Vehicle");
        void* awc = ComponentOf(st.vehicle, "ActiveWeaponsComponent");
        for (uint8_t s = 0; awc && s < 4; ++s) {
            ue::Call c(awc, "GetWeapon");
            c.Set<uint8_t>("Slot", s);
            if (!c.Run()) continue;
            void* w = c.Get<void*>("ReturnValue");
            if (ue::Alive(w) && std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
        }
    }
    return out;
}

std::string DisplayName(void* obj) {
    if (!obj) return {};
    std::string n = ue::Name(ue::ClassOf(obj));
    for (const char* pre : {"BPChar_", "BP_", "Char_"})
        if (n.rfind(pre, 0) == 0) n = n.substr(std::strlen(pre));
    if (n.size() > 2 && n.compare(n.size() - 2, 2, "_C") == 0) n.resize(n.size() - 2);
    for (char& c : n)
        if (c == '_') c = ' ';
    return n;
}

void* Globals() {
    static ue::FWeak w;
    static double retry = 0;
    void* g = ue::WeakGet(w);
    if (g || st.time < retry) return g;
    g = ue::FindFirstOf(ue::FindClass("OakGlobalsData"));
    if (g) w = ue::MakeWeak(g);
    else retry = st.time + 5.0;
    return g;
}

void* GameplayGlobals() {
    static ue::FWeak w;
    static double retry = 0;
    void* gg = ue::WeakGet(w);
    if (gg || st.time < retry) return gg;
    void* g = Globals();
    gg = g ? ue::Ptr(g, "GameplayGlobals") : nullptr;
    if (!gg) gg = ue::FindFirstOf(ue::FindClass("GameplayGlobals"));
    if (gg) w = ue::MakeWeak(gg);
    else retry = st.time + 5.0;
    return gg;
}

void* SpawnActor(const char* cls, const ue::FVector& at, float yaw) {
    void* c = std::strchr(cls, '/') ? ue::FindObject(cls) : ue::FindClass(cls);
    if (!c || !st.world) return nullptr;
    float y = yaw * 3.14159265f / 180.0f;
    alignas(16) float xf[12] = {0, 0, std::sin(y / 2), std::cos(y / 2), at.x, at.y, at.z, 0, 1, 1, 1, 0};
    void* gs = Lib("GameplayStatics");
    ue::Call sp(gs, "BeginDeferredActorSpawnFromClass");
    sp.Set<void*>("WorldContextObject", st.world).Set<void*>("ActorClass", c).SetRaw("SpawnTransform", xf, sizeof(xf));
    sp.Set<uint8_t>("CollisionHandlingOverride", 1).Set<void*>("Owner", nullptr);
    void* actor = sp.Run() ? sp.Get<void*>("ReturnValue") : nullptr;
    if (!ue::Alive(actor)) return nullptr;
    ue::Call fin(gs, "FinishSpawningActor");
    fin.Set<void*>("Actor", actor).SetRaw("SpawnTransform", xf, sizeof(xf));
    fin.Run();
    return ue::Alive(actor) ? actor : nullptr;
}

void DestroyActor(void* actor) {
    if (!ue::Alive(actor)) return;
    ue::Call c(actor, "GbxDestroyActor");
    c.SetBool("bDestroyImmediately", true);
    if (!c.Run()) ue::Call(actor, "K2_DestroyActor_DEPRECATED").Run();
}

void SetViewTarget(void* target) {
    if (!st.pc || !ue::Alive(target)) return;
    ue::Call c(st.pc, "SetViewTargetWithBlend");
    c.Set<void*>("NewViewTarget", target).Set<float>("BlendTime", 0.0f).Set<uint8_t>("BlendFunc", 0).Set<float>("BlendExp", 0.0f);
    c.SetBool("bLockOutgoing", false);
    c.Run();
}

void* ViewTarget() {
    ue::Call c(st.pc, "GetViewTarget");
    return c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
}

void IgnoreInput(bool look, bool move) {
    if (!st.pc) return;
    ue::Call l(st.pc, "SetIgnoreLookInput");
    l.SetBool("bNewLookInput", look);
    l.Run();
    ue::Call m(st.pc, "SetIgnoreMoveInput");
    m.SetBool("bNewMoveInput", move);
    m.Run();
}

bool TraceDown(const ue::FVector& from, float depth, ue::FVector& hit) {
    static void* hitStruct = nullptr;
    if (!hitStruct) hitStruct = ue::FindClass("HitResult");
    ue::Prop impact = hitStruct ? ue::FindProp(hitStruct, "ImpactPoint") : ue::Prop{};
    if (!impact) return false;
    void* ignore[1] = {st.pawn};
    ue::TArray<void*> ignoreList{ignore, st.pawn ? 1 : 0, 1};
    ue::Call c(Lib("KismetSystemLibrary"), "LineTraceSingle");
    ue::FVector to{from.x, from.y, from.z - depth};
    c.Set<void*>("WorldContextObject", st.world).Set("Start", from).Set("End", to).Set<uint8_t>("TraceChannel", 0).SetBool("bTraceComplex", false);
    c.Set("ActorsToIgnore", ignoreList).Set<uint8_t>("DrawDebugType", 0).SetBool("bIgnoreSelf", true);
    if (!c.Run() || !c.GetBool("ReturnValue")) return false;
    const uint8_t* out = c.Raw("OutHit");
    if (!out) return false;
    std::memcpy(&hit, out + impact.offset, sizeof(hit));
    return true;
}

void TakeMouse(float& dx, float& dy) { overlay::TakeLook(dx, dy); }

namespace {

Tracked* Track(std::vector<Tracked>& list, void* obj, int offset) {
    for (Tracked& t : list)
        if (t.offset == offset && ue::WeakGet(t.obj) == obj) return &t;
    Tracked t;
    t.obj = ue::MakeWeak(obj);
    t.offset = offset;
    t.natural = ue::At<float>(obj, offset);
    t.written = std::nanf("");
    list.push_back(t);
    return &list.back();
}

void Prune(std::vector<Tracked>& list) {
    list.erase(std::remove_if(list.begin(), list.end(), [](const Tracked& t) { return !ue::WeakGet(t.obj); }), list.end());
}

}

void ScaleAttr(std::vector<Tracked>& list, void* obj, const char* prop, float factor) {
    if (!obj) return;
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    if (!p) return;
    int off = p.offset + 4;
    if (list.size() > 64) Prune(list);
    Tracked* t = Track(list, obj, off);
    float cur = ue::At<float>(obj, off);
    if (!(cur == t->written)) t->natural = cur;
    float want = t->natural * factor;
    if (cur != want) ue::Put<float>(obj, off, want);
    t->written = want;
}

void SetAttr(std::vector<Tracked>& list, void* obj, const char* prop, float value) {
    if (!obj) return;
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), prop);
    if (!p) return;
    int off = p.offset + 4;
    if (list.size() > 64) Prune(list);
    Tracked* t = Track(list, obj, off);
    float cur = ue::At<float>(obj, off);
    if (!(cur == t->written)) t->natural = cur;
    if (cur != value) ue::Put<float>(obj, off, value);
    t->written = value;
}

void RestoreAttrs(std::vector<Tracked>& list) {
    for (Tracked& t : list) {
        void* o = ue::WeakGet(t.obj);
        if (!o) continue;
        if (ue::At<float>(o, t.offset) == t.written) ue::Put<float>(o, t.offset, t.natural);
    }
    list.clear();
}

ue::FVector Forward(const ue::FRotator& r) {
    float p = r.pitch * 3.14159265f / 180.0f, y = r.yaw * 3.14159265f / 180.0f;
    return {std::cos(p) * std::cos(y), std::cos(p) * std::sin(y), std::sin(p)};
}

ue::FRotator LookAt(const ue::FVector& from, const ue::FVector& to) {
    ue::FVector d = Sub(to, from);
    float h = std::sqrt(d.x * d.x + d.y * d.y);
    ue::FRotator r;
    r.yaw = std::atan2(d.y, d.x) * 180.0f / 3.14159265f;
    r.pitch = std::atan2(d.z, h) * 180.0f / 3.14159265f;
    r.roll = 0;
    return r;
}

float AngleBetween(const ue::FRotator& a, const ue::FRotator& b) {
    ue::FVector fa = Forward(a), fb = Forward(b);
    float d = std::clamp(fa.x * fb.x + fa.y * fb.y + fa.z * fb.z, -1.0f, 1.0f);
    return std::acos(d) * 180.0f / 3.14159265f;
}

}

namespace game {

using namespace bl3;

void Wire(core::Model& m, std::mutex& lock, unsigned long thread) {
    model = &m;
    modelLock = &lock;
    gameThread = thread;
    for (core::Feature& f : m.features) {
        f.available = false;
        f.missing = "Checking the game...";
    }
}

void SetFolder(const std::wstring& f) { folder = f; }

void SetBackup(std::function<std::string()> fn) { backupFn = std::move(fn); }

bool Hook() {
    InitScanRoots();
    PlayerInit();
    WeaponsInit();
    SkillsInit();
    MovementInit();
    AimInit();
    LootInit();
    GiverInit(*model);
    WorldInit(*model);
    MapshotInit(*model);
    VehicleInit();
    {
        std::lock_guard<std::mutex> g(*modelLock);
        for (core::Feature& f : model->features) {
            if (implemented.count(f.id) || !f.counts) continue;
            if (f.missing == "Checking the game...") f.missing = "Not found in this version of the game";
        }
        for (core::Feature& f : model->features) {
            if (f.counts) continue;
            f.available = true;
        }
    }
    peTarget = ue::ProcessEventAddress();
    if (!peTarget) return false;
    if (MH_CreateHook(peTarget, (void*)&HookedPE, (void**)&oPE) != MH_OK || MH_EnableHook(peTarget) != MH_OK) {
        LOG("could not hook ProcessEvent");
        return false;
    }
    LOG("game loop hooked");
    return true;
}

bool Shutdown() {
    stopping = true;
    if (!peTarget || !oPE) return true;
    ULONGLONG start = GetTickCount64();
    while (!stopped) {
        Sleep(20);
        if (GetTickCount64() - start > 60000) {
            LOG("the game did not run an update in 60 seconds");
            return false;
        }
    }
    return true;
}

void Unhook() {
    if (peTarget) MH_DisableHook(peTarget);
    ULONGLONG start = GetTickCount64();
    while (inflight > 0 && GetTickCount64() - start < 3000) Sleep(5);
    Sleep(50);
}

std::vector<std::string> TakeNotes() {
    std::lock_guard<std::mutex> g(noteLock);
    std::vector<std::string> out;
    out.swap(notes);
    return out;
}

void SetMenuState(bool open, bool focused, bool map) {
    menuOpen = open;
    menuFocused = focused;
    mapVisible = map;
}

}
