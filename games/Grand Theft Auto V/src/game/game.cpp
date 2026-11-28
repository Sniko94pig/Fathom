#include "game/game.h"
#include "game/internal.h"
#include "game/shv.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstring>
#include <unordered_map>

namespace gtav {

State st;

namespace {

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
std::wstring folder;
std::function<std::string()> backupFn;
bool backedUp = false;

struct FeatureState {
    bool on = false;
    float value = 0.0f;
};

std::unordered_map<std::string, FeatureState> snap;
std::mutex reqLock;
std::vector<std::string> pressedQueue, pressedNow;
std::vector<std::function<void()>> postQueue, postNow;
std::mutex noteLock;
std::vector<std::string> notes;
std::unordered_map<std::string, bool> implemented;

std::atomic<bool> stopping{false}, stopped{false}, ticking{false}, initDone{false};
std::atomic<bool> menuOpen{false}, menuFocused{false}, mapVisible{false};
std::atomic<bool> leave{false};
std::mutex leaveLock;
std::string leaveWhy;
bool attached = false;
uint64_t lastTickAt = 0;

void RequestLeave(const std::string& why) {
    {
        std::lock_guard<std::mutex> g(leaveLock);
        if (leaveWhy.empty()) leaveWhy = why;
    }
    leave = true;
}

const char* OnlineReason() {
    if (n::NETWORK_IS_SESSION_STARTED()) return "a network session started";
    if (n::NETWORK_IS_IN_SESSION()) return "the game is in a network session";
    if (n::NETWORK_IS_GAME_IN_PROGRESS()) return "a network game is in progress";
    static const uint32_t freemode = Joaat("freemode");
    if (n::GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(freemode) > 0) return "GTA Online scripts are running";
    return nullptr;
}

void Refresh() {
    st.player = n::PLAYER_ID();
    st.ped = n::PLAYER_PED_ID();
    bool exists = st.ped && n::DOES_ENTITY_EXIST(st.ped);
    st.alive = exists && !n::IS_ENTITY_DEAD(st.ped, false) && !n::IS_PLAYER_DEAD(st.player);
    st.vehicle = exists && n::IS_PED_IN_ANY_VEHICLE(st.ped, false) ? n::GET_VEHICLE_PED_IS_IN(st.ped, false) : 0;
    st.driver = st.vehicle && n::GET_PED_IN_VEHICLE_SEAT(st.vehicle, -1, false) == st.ped;
    st.cutscene = n::IS_CUTSCENE_PLAYING() != 0;
    st.switching = n::IS_PLAYER_SWITCH_IN_PROGRESS() != 0;
    st.faded = n::IS_SCREEN_FADED_OUT() != 0;
    st.model = exists ? (uint32_t)n::GET_ENTITY_MODEL(st.ped) : 0;
    static const uint32_t michael = Joaat("player_zero"), franklin = Joaat("player_one"), trevor = Joaat("player_two");
    st.character = st.model == michael ? 0 : st.model == franklin ? 1 : st.model == trevor ? 2 : -1;
    if (exists) {
        st.pos = n::GET_ENTITY_COORDS(st.ped, false);
        st.heading = n::GET_ENTITY_HEADING(st.ped);
    }
}

void Snapshot() {
    {
        std::lock_guard<std::mutex> g(*modelLock);
        for (const core::Feature& f : model->features) snap[f.id] = {f.on, f.value};
    }
    st.menuOpen = menuOpen;
    st.menuFocused = menuFocused;
    st.mapVisible = mapVisible;
    std::lock_guard<std::mutex> g(reqLock);
    pressedNow.swap(pressedQueue);
    pressedQueue.clear();
    postNow.swap(postQueue);
    postQueue.clear();
}

void OffAll() {
    MapshotOff();
    CameraOff();
    MovementOff();
    AimOff();
    WeaponsOff();
    TrollOff();
    VehiclesOff();
    WorldOff();
    GiverOff();
    PlayerOff();
}

void InitModules() {
    PlayerInit();
    MovementInit();
    WeaponsInit();
    AimInit();
    GiverInit(*model);
    VehiclesInit(*model);
    WorldInit(*model);
    CameraInit();
    TrollInit();
    MapshotInit(*model);
    std::lock_guard<std::mutex> g(*modelLock);
    for (core::Feature& f : model->features) {
        if (!f.counts) {
            f.available = true;
            continue;
        }
        if (!implemented.count(f.id) && f.missing == "Checking the game...") f.missing = "Not found in this version of the game";
    }
}

void Tick() {
    double now = Now();
    st.dt = st.time > 0 ? (float)std::clamp(now - st.time, 0.0, 0.1) : 0.0f;
    st.time = now;
    ++st.tick;
    lastTickAt = GetTickCount64();
    if (const char* why = OnlineReason()) {
        OffAll();
        RequestLeave(std::string("Fathom switched everything off and unloads: ") + why);
        stopped = true;
        return;
    }
    if (!initDone) {
        InitModules();
        initDone = true;
        LOG("modules ready");
    }
    Refresh();
    Snapshot();
    if (stopping) {
        OffAll();
        stopped = true;
        return;
    }
    if (st.menuOpen && st.menuFocused) n::DISABLE_ALL_CONTROL_ACTIONS(0);
    for (auto& fn : postNow) {
        if (fn) fn();
    }
    postNow.clear();
    PlayerTick();
    MovementTick();
    WeaponsTick();
    AimTick();
    GiverTick();
    VehiclesTick();
    WorldTick();
    CameraTick();
    TrollTick();
    MapshotTick();
    pressedNow.clear();
}

void ScriptMain() {
    LOG("connected to the game's script thread");
    ticking = true;
    for (;;) {
        if (!stopped) {
            try {
                Tick();
            } catch (const std::exception& e) {
                static int reported = 0;
                if (reported++ < 5) LOG("error in game update: %s", e.what());
            } catch (...) {
                static int reported = 0;
                if (reported++ < 5) LOG("unknown error in game update");
            }
        }
        shv::Wait(0);
    }
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

void Ready(const char* id, bool ok, const char* missing) {
    std::lock_guard<std::mutex> g(*modelLock);
    implemented[id] = ok;
    core::Feature* f = model->find(id);
    if (!f) return;
    f->available = ok;
    if (!ok) f->missing = missing ? missing : "Not found in this version of the game";
    if (f->kind == core::Kind::Action && f->page.empty()) {
        f->apply = [](core::Feature& x) {
            std::lock_guard<std::mutex> q(reqLock);
            pressedQueue.push_back(x.id);
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

bool BackupOnce() {
    if (backedUp) return true;
    if (!backupFn) return false;
    std::string path = backupFn();
    if (path.empty()) return false;
    backedUp = true;
    Note("Your saves were backed up first");
    return true;
}

std::wstring Folder() { return folder; }

uint32_t Joaat(const char* s) {
    uint32_t h = 0;
    for (; s && *s; ++s) {
        h += (uint8_t)std::tolower((unsigned char)*s);
        h += h << 10;
        h ^= h >> 6;
    }
    h += h << 3;
    h ^= h >> 11;
    h += h << 15;
    return h;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

std::vector<int> AllPeds() {
    std::vector<int> out(1024);
    int n = shv::Peds(out.data(), (int)out.size());
    out.resize((size_t)std::clamp(n, 0, (int)out.size()));
    return out;
}

std::vector<int> AllVehicles() {
    std::vector<int> out(1024);
    int n = shv::Vehicles(out.data(), (int)out.size());
    out.resize((size_t)std::clamp(n, 0, (int)out.size()));
    return out;
}

void Post(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(reqLock);
    if (postQueue.size() < 256) postQueue.push_back(std::move(fn));
}

V3 RotToDir(const V3& rot) {
    const float d = 3.14159265f / 180.0f;
    float pitch = rot.x * d, yaw = rot.z * d;
    return {-std::sin(yaw) * std::cos(pitch), std::cos(yaw) * std::cos(pitch), std::sin(pitch)};
}

V3 CamForward() { return RotToDir(n::GET_GAMEPLAY_CAM_ROT(2)); }

}

namespace game {

using namespace gtav;

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    for (core::Feature& f : m.features) {
        f.available = false;
        f.missing = "Checking the game...";
    }
}

void SetFolder(const std::wstring& f) { folder = f; }

void SetBackup(std::function<std::string()> fn) { backupFn = std::move(fn); }

bool Attach(void* module) {
    if (!shv::Ready()) return false;
    attached = shv::Register(module, &ScriptMain);
    if (attached) LOG("registered with Script Hook V");
    return attached;
}

bool Shutdown() {
    stopping = true;
    if (!attached || !ticking) return true;
    ULONGLONG start = GetTickCount64();
    while (!stopped) {
        Sleep(20);
        if (GetTickCount64() - start > 60000) {
            LOG("the game did not run a script update in 60 seconds");
            return false;
        }
    }
    return true;
}

void Detach(void* module) {
    ULONGLONG start = GetTickCount64();
    while (MapshotWriting() && GetTickCount64() - start < 20000) Sleep(50);
    if (!attached) return;
    shv::Unregister(module);
    attached = false;
    Sleep(500);
}

bool Ticking() { return ticking.load(); }

bool WantsUnload(std::string& why) {
    if (!leave) return false;
    std::lock_guard<std::mutex> g(leaveLock);
    why = leaveWhy;
    return true;
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
