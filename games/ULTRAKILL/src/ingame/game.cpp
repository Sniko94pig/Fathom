#include "ingame/game.h"
#include "ingame/cheats.h"
#include "ingame/groups.h"
#include "ingame/log.h"
#include "ingame/unity.h"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <iterator>
#include <unordered_map>

namespace uk {

struct Snap {
    std::unordered_map<std::string, bool> on;
    std::unordered_map<std::string, float> value;
    bool inUse = false;
};

namespace {

using core::Feature;
using un::R;

struct Group {
    const char* name;
    const char* label;
    bool (*resolve)();
    bool (*supported)(const std::string&);
    void (*tick)(const Ctx&);
    void (*release)(const Ctx&);
    void (*action)(const std::string&, const Ctx&);
    bool (*busy)();
    bool (*outside)();
};

const Group kGroups[] = {
    {"player", "player", ukplayer::Resolve, ukplayer::Supported, ukplayer::Tick, ukplayer::Release, ukplayer::Action, ukplayer::Busy, ukplayer::Outside},
    {"movement", "movement", ukmove::Resolve, ukmove::Supported, ukmove::Tick, ukmove::Release, ukmove::Action, ukmove::Busy, nullptr},
    {"weapons_a", "weapon", ukweapa::Resolve, ukweapa::Supported, ukweapa::Tick, ukweapa::Release, ukweapa::Action, ukweapa::Busy, nullptr},
    {"weapons_b", "weapon", ukweapb::Resolve, ukweapb::Supported, ukweapb::Tick, ukweapb::Release, ukweapb::Action, ukweapb::Busy, nullptr},
    {"shop", "shop", ukshop::Resolve, ukshop::Supported, ukshop::Tick, ukshop::Release, ukshop::Action, ukshop::Busy, nullptr},
    {"aim", "aiming", ukaim::Resolve, ukaim::Supported, ukaim::Tick, ukaim::Release, ukaim::Action, ukaim::Busy, nullptr},
    {"orbs", "orb", ukorbs::Resolve, ukorbs::Supported, ukorbs::Tick, ukorbs::Release, ukorbs::Action, ukorbs::Busy, nullptr},
    {"level", "level", uklevel::Resolve, uklevel::Supported, uklevel::Tick, uklevel::Release, uklevel::Action, uklevel::Busy, uklevel::Outside},
};

const int kMaxFaults = 25;
const char* const kQuiet[] = {"orb_esp", "challenge_shower"};

struct Slot {
    const Group* g = nullptr;
    bool ok = false;
    bool dropped = false;
    bool outside = false;
    int faults = 0;
};

struct World {
    int frame = -1;
    Scene scene = Scene::Other;
    bool loading = false, mainMenu = true, paused = false, inIntro = false;
    bool inLevel = false, dead = false, activated = false, levelOver = false;
    Object* nm = nullptr;
    Object* om = nullptr;
    Object* gsm = nullptr;
    int nmId = 0;
};

struct MarkSet {
    std::string owner;
    int tick = 0;
    ULONGLONG at = 0;
    std::vector<Marker> list;
};

enum class Way { None, Hooks, Flag, Lost };

struct Upload {
    const char* name;
    void* code;
    bool on;
    bool stuck;
};

const int kMarkFrames = 3;
const ULONGLONG kMarkMs = 1000;
const size_t kMaxMarks = 256;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
std::vector<Slot> slots;
std::vector<std::string> featureIds;
std::unordered_map<std::string, float> sliderDefaults;
std::vector<std::string> pendingOff;
Snap snap;
bool guardOk = false;

std::mutex qLock;
std::vector<std::string> queued;
std::vector<std::string> notes;

std::mutex backupLock;
std::function<std::string()> backupFn;
std::string lastBackup;

std::atomic<bool> menuOpen{false}, menuFocused{false};
std::atomic<int> stop{0};
std::atomic<int> ticking{0};
std::atomic<ULONGLONG> lastTick{0};
std::atomic<void*> gameWnd{nullptr};
std::atomic<bool> subscribed{false};
std::wstring folder;
uint32_t delegateHandle = 0;
bool unsubscribed = false;
bool inside = false;
int stopAttempts = 0;
int lastFrame = -1;
int faults = 0;
const char* faultWhere = "the game update";

bool wasInLevel = false, wasDead = false;
int lastNmId = 0;
Scene lastScene = Scene::Other;

bool tainted = false;
int runId = 0;
bool warned = false;
bool flagReady = false, hooksReady = false;
std::atomic<bool> keepHooks{false};
Way way = Way::None;
Upload uploads[] = {{"SubmitLevelScore", nullptr, false, false},
                    {"SubmitCyberGrindScore", nullptr, false, false},
                    {"SubmitFishSize", nullptr, false, false}};

std::mutex markLock;
std::vector<MarkSet> markSets;
std::atomic<int> tickNo{0};

struct Locks {
    bool held = false;
    uint8_t cursor = 0, input = 0, camera = 0;
    int lockState = 0;
    bool visible = true;
};

uint32_t keyHandle = 0;
bool lockOn = false;
Locks locks;
bool cursorSaved = false;
int lockWas = 0;
bool visibleWas = true;
Hold frozenOm, camWatch;
bool camWasEnabled = false, camWoke = false;

int Filter(unsigned int code) {
    if (faults++ < 20) LOG("crash avoided in %s (code %08X)", faultWhere, code);
    return EXCEPTION_EXECUTE_HANDLER;
}

bool GuardResolve(const Group* g, bool* ok) {
    __try {
        *ok = g->resolve();
        return true;
    } __except (Filter(GetExceptionCode())) {
        *ok = false;
        return false;
    }
}

bool GuardSupported(const Group* g, const std::string* id) {
    __try {
        return g->supported(*id);
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool GuardTick(const Group* g, const Ctx* c) {
    __try {
        g->tick(*c);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool GuardRelease(const Group* g, const Ctx* c) {
    __try {
        g->release(*c);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool GuardAction(const Group* g, const std::string* id, const Ctx* c) {
    __try {
        g->action(*id, *c);
        return true;
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool GuardBusy(const Group* g, bool* busy) {
    __try {
        *busy = g->busy();
        return true;
    } __except (Filter(GetExceptionCode())) {
        *busy = true;
        return false;
    }
}

bool GuardOutside(const Group* g) {
    __try {
        return g->outside && g->outside();
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

bool GuardCompile(mono::Method* m, void** code) {
    __try {
        *code = mono::Compile(m);
        return *code != nullptr;
    } __except (Filter(GetExceptionCode())) {
        *code = nullptr;
        return false;
    }
}

bool Owns(const Slot& sl, const std::string& id) {
    if (!sl.ok) return false;
    faultWhere = sl.g->name;
    bool yes = GuardSupported(sl.g, &id);
    faultWhere = "the game update";
    return yes;
}

bool Owned(const std::string& id) {
    for (const Slot& sl : slots)
        if (Owns(sl, id)) return true;
    return false;
}

bool Bit(Object* o, const Fld& f) { return o && f && un::Val<uint8_t>(o, f) != 0; }

Object* LiveGsm() {
    Object* gsm = un::Static(R.gsmInstance);
    return un::Alive(gsm) ? gsm : nullptr;
}

void ApplyOff() {
    for (const std::string& id : pendingOff) {
        Feature* f = model->find(id);
        if (!f) continue;
        f->available = false;
        f->missing = "Switched off after an error in the game";
    }
    pendingOff.clear();
}

bool Quiet(const std::string& id) {
    for (const char* q : kQuiet)
        if (id == q) return true;
    return false;
}

void TakeSnap() {
    if (!modelLock || !model) return;
    std::unique_lock<std::mutex> g(*modelLock, std::try_to_lock);
    if (!g.owns_lock()) return;
    if (!pendingOff.empty()) ApplyOff();
    bool inUse = false;
    for (const Feature& f : model->features) {
        bool live = f.available && !f.building;
        if (f.kind == core::Kind::Toggle) {
            bool on = live && f.on;
            snap.on[f.id] = on;
            inUse = inUse || (on && !Quiet(f.id));
        } else if (f.kind == core::Kind::Slider) {
            float v = live ? f.value : f.def;
            snap.value[f.id] = v;
            inUse = inUse || (f.counts && std::fabs(v - f.def) > 0.001f);
        }
    }
    snap.inUse = inUse;
}

Snap OffSnap() {
    Snap s;
    for (const auto& [id, on] : snap.on) s.on[id] = false;
    for (const auto& [id, v] : snap.value) {
        auto it = sliderDefaults.find(id);
        s.value[id] = it != sliderDefaults.end() ? it->second : v;
    }
    return s;
}

World ReadWorld(int frame) {
    World w;
    w.frame = frame;
    w.loading = un::Static(R.shPending) != nullptr;
    w.gsm = LiveGsm();
    w.om = un::Single(R.optionsManager);
    w.mainMenu = !w.om || Bit(w.om, R.omMainMenu);
    w.paused = Bit(w.om, R.omPaused);
    w.inIntro = Bit(w.om, R.omInIntro);
    if (!w.loading && w.om && w.mainMenu) w.scene = Scene::MainMenu;
    if (w.loading || w.mainMenu) return w;
    Object* nm = un::Single(R.newMovement);
    if (!nm || !un::ActiveInHierarchy(nm)) return w;
    w.inLevel = true;
    w.scene = Scene::Level;
    w.nm = nm;
    w.nmId = un::InstanceId(nm);
    w.dead = Bit(nm, R.nmDead);
    w.activated = Bit(nm, R.nmActivated);
    w.levelOver = Bit(nm, R.nmLevelOver);
    return w;
}

Ctx MakeCtx(const Snap& s, const World& w, bool stopping) {
    Ctx c;
    c.snap = &s;
    c.player = w.inLevel ? w.nm : nullptr;
    c.playerId = w.inLevel ? w.nmId : 0;
    c.scene = w.scene;
    c.inLevel = w.inLevel;
    c.dead = w.dead;
    c.activated = w.activated;
    c.levelOver = w.levelOver;
    c.paused = w.paused;
    c.inIntro = w.inIntro;
    c.loading = w.loading;
    c.mainMenu = w.mainMenu;
    c.menuOpen = menuOpen.load();
    c.menuFocused = c.menuOpen && menuFocused.load();
    c.stopping = stopping;
    c.dt = stopping ? 0.0f : std::clamp(un::DeltaTime(), 0.0f, 0.1f);
    c.udt = stopping ? 0.0f : std::clamp(un::UnscaledDeltaTime(), 0.0f, 0.1f);
    c.frame = w.frame;
    c.now = GetTickCount64() / 1000.0;
    return c;
}

Object* MenuKey() {
    if (!keyHandle) keyHandle = mono::Pin(mono::NewString("fathom-menu"));
    return mono::Pinned(keyHandle);
}

void FreeMenuKey() {
    mono::Unpin(keyHandle);
    keyHandle = 0;
}

bool StateActive(Object* gsm) {
    Object* key = MenuKey();
    return gsm && key && un::CallBool(R.gsmIsActive, gsm, {key});
}

void SaveLocks(Object* gsm) {
    locks.held = true;
    locks.cursor = un::Val<uint8_t>(gsm, R.gsmCursorLocked);
    locks.input = un::Val<uint8_t>(gsm, R.gsmInputLocked);
    locks.camera = un::Val<uint8_t>(gsm, R.gsmCameraLocked);
    locks.lockState = un::CallInt(R.cursorGetLock, nullptr);
    locks.visible = un::CallBool(R.cursorGetVisible, nullptr, {}, true);
}

bool NoGameStates(Object* gsm) {
    Object* order = un::Ref(gsm, R.gsmOrder);
    mono::Field* size = order ? mono::FindField(mono::ClassOf(order), "_size") : nullptr;
    return size && mono::Get<int>(order, mono::Offset(size)) == 0;
}

void RestoreLocks(Object* gsm) {
    if (!locks.held) return;
    locks.held = false;
    if (!gsm || !NoGameStates(gsm)) return;
    un::SetVal<uint8_t>(gsm, R.gsmCursorLocked, locks.cursor);
    un::SetVal<uint8_t>(gsm, R.gsmInputLocked, locks.input);
    un::SetVal<uint8_t>(gsm, R.gsmCameraLocked, locks.camera);
    if (R.cursorSetLock) un::Call(R.cursorSetLock, nullptr, {&locks.lockState});
    if (R.cursorSetVisible) un::Call(R.cursorSetVisible, nullptr, {&locks.visible});
}

bool PushState(Object* gsm) {
    if (!R.gameState || !R.gsCtor || !R.gsmRegister || !R.gsPlayerLock || !R.gsCameraLock || !R.gsCursorLock || !R.gsPriority) return false;
    Object* key = MenuKey();
    Object* st = key ? mono::New(R.gameState) : nullptr;
    if (!st) return false;
    bool ok = false;
    un::Call(R.gsCtor, st, {key}, &ok);
    if (!ok) return false;
    un::SetVal<int>(st, R.gsPlayerLock, R.lockLock);
    un::SetVal<int>(st, R.gsCameraLock, R.lockLock);
    un::SetVal<int>(st, R.gsCursorLock, R.lockUnlock);
    un::SetVal<int>(st, R.gsPriority, 1000);
    SaveLocks(gsm);
    un::Call(R.gsmRegister, gsm, {st}, &ok);
    return ok;
}

void PopState(Object* gsm) {
    if (R.gsmPop && StateActive(gsm)) un::Call(R.gsmPop, gsm, {MenuKey()});
    RestoreLocks(gsm);
}

void MenuLock(Object* gsm, bool focus) {
    if (focus && gsm && R.gsmIsActive) {
        if (StateActive(gsm)) lockOn = true;
        else if (PushState(gsm)) lockOn = true;
    }
    if (!focus && lockOn) {
        PopState(gsm ? gsm : LiveGsm());
        lockOn = false;
    }
}

void FreeCursor() {
    if (R.cursorSetLock && un::CallInt(R.cursorGetLock, nullptr) != 0) {
        int none = 0;
        un::Call(R.cursorSetLock, nullptr, {&none});
    }
    if (R.cursorSetVisible && !un::CallBool(R.cursorGetVisible, nullptr, {}, true)) {
        bool yes = true;
        un::Call(R.cursorSetVisible, nullptr, {&yes});
    }
}

void MenuCursor(bool focus) {
    if (focus) {
        if (lockOn) {
            cursorSaved = false;
        } else if (!cursorSaved) {
            lockWas = un::CallInt(R.cursorGetLock, nullptr);
            visibleWas = un::CallBool(R.cursorGetVisible, nullptr, {}, true);
            cursorSaved = true;
        }
        FreeCursor();
        return;
    }
    if (!cursorSaved) return;
    cursorSaved = false;
    if (R.cursorSetLock) un::Call(R.cursorSetLock, nullptr, {&lockWas});
    if (R.cursorSetVisible) un::Call(R.cursorSetVisible, nullptr, {&visibleWas});
}

void WatchCamera() {
    Object* cam = un::Single(R.cameraController);
    if (!cam) return;
    bool en = un::Enabled(cam);
    if (cam != camWatch.get()) {
        camWatch.set(cam);
        camWasEnabled = en;
        camWoke = false;
        return;
    }
    if (en && !camWasEnabled) camWoke = true;
    camWasEnabled = en;
}

void ReleaseFrozen(const World& w) {
    Object* om = frozenOm.get();
    if (un::Alive(om) && Bit(om, R.omFrozen)) un::SetVal<uint8_t>(om, R.omFrozen, 0);
    Object* cam = camWatch.get();
    bool wake = camWoke && un::Alive(cam) && un::Enabled(cam) && !Bit(cam, R.ccActivated) && !Bit(om, R.omPaused) && w.inLevel &&
                w.activated && !w.dead;
    if (wake) un::SetVal<uint8_t>(cam, R.ccActivated, 1);
    frozenOm.reset();
    camWatch.reset();
    camWoke = false;
}

void MenuFreeze(const World& w, bool focus) {
    if (!R.omFrozen) return;
    if (frozenOm.held() && (!focus || frozenOm.get() != w.om)) ReleaseFrozen(w);
    if (!focus || !w.om) return;
    if (!frozenOm.held()) {
        if (Bit(w.om, R.omFrozen)) return;
        frozenOm.set(w.om);
    }
    if (!Bit(w.om, R.omFrozen)) un::SetVal<uint8_t>(w.om, R.omFrozen, 1);
    WatchCamera();
}

void MenuInput(const World& w, bool stopping) {
    bool focus = !stopping && menuOpen.load() && menuFocused.load();
    MenuLock(w.gsm, focus);
    MenuFreeze(w, focus);
    MenuCursor(focus);
}

void Skip() {}

void FreeUploads() {
    for (Upload& u : uploads) {
        MH_STATUS st = u.code ? MH_RemoveHook(u.code) : MH_OK;
        if (st != MH_OK && st != MH_ERROR_NOT_CREATED) {
            LOG("leaderboard guard: LeaderboardController.%s could not be unhooked, the menu file stays loaded", u.name);
            keepHooks = true;
            continue;
        }
        u.code = nullptr;
        u.on = false;
        u.stuck = false;
    }
    hooksReady = false;
}

void MakeUploads() {
    mono::Method* methods[] = {R.lbLevel, R.lbCyber, R.lbFish};
    bool all = R.statsManager && R.getInstanceID;
    faultWhere = "the leaderboard guard";
    for (size_t i = 0; i < std::size(uploads); ++i) {
        void* code = nullptr;
        bool ok = methods[i] && GuardCompile(methods[i], &code) && MH_CreateHook(code, (void*)&Skip, nullptr) == MH_OK;
        if (ok) uploads[i].code = code;
        else LOG("leaderboard guard: LeaderboardController.%s cannot be blocked", uploads[i].name);
        all = all && ok;
    }
    faultWhere = "the game update";
    hooksReady = all;
    if (!all) FreeUploads();
}

bool Block() {
    bool all = hooksReady;
    for (Upload& u : uploads) {
        if (!u.code || u.on) continue;
        MH_STATUS st = MH_EnableHook(u.code);
        u.on = st == MH_OK || st == MH_ERROR_ENABLED;
        all = all && u.on;
    }
    return all;
}

void Unblock() {
    for (Upload& u : uploads) {
        if (!u.on || u.stuck) continue;
        MH_STATUS st = MH_DisableHook(u.code);
        if (st == MH_OK || st == MH_ERROR_DISABLED) {
            u.on = false;
        } else {
            u.stuck = true;
            LOG("leaderboard guard: LeaderboardController.%s stays blocked", u.name);
        }
    }
}

bool FlagRun(Object* sm) {
    if (!flagReady) return false;
    Object* ac = un::Single(R.assistController);
    if (ac) un::SetVal<uint8_t>(ac, R.acCheats, 1);
    Object* cached = un::Ref(sm, R.smAsscon);
    bool other = cached != ac && un::Alive(cached);
    if (other) un::SetVal<uint8_t>(cached, R.acCheats, 1);
    return ac || other;
}

Way Protect() {
    if (hooksReady && Block()) {
        LOG("cheats in use, this run will not go to the leaderboards (Fathom blocks its uploads)");
        return Way::Hooks;
    }
    if (hooksReady) LOG("leaderboard guard: the upload block could not be switched on");
    if (flagReady) {
        LOG("cheats in use, this run will not go to the leaderboards (the game marks it as cheated)");
        return Way::Flag;
    }
    LOG("leaderboard guard: this run cannot be kept off the leaderboards");
    return Way::Lost;
}

bool SafetyPreTick(bool inUse, bool oneShot, bool inLevel) {
    Object* sm = un::Single(R.statsManager);
    int smId = sm ? un::InstanceId(sm) : 0;
    if (inUse || oneShot) {
        tainted = true;
        runId = smId;
    } else if (tainted && (smId ? smId != runId : !inLevel)) {
        tainted = false;
        runId = 0;
        LOG("every cheat is off, the next run can go to the leaderboards");
    }
    if (!tainted) {
        if (way != Way::None) Unblock();
        way = Way::None;
        return true;
    }
    if (!smId && !inLevel) return true;
    if (way == Way::None) {
        way = Protect();
        if (way == Way::Lost) return false;
    }
    if (way == Way::Flag) FlagRun(sm);
    if (way != Way::Lost && smId && !warned) {
        warned = true;
        Note("Scores from this run will not go to the leaderboards");
    }
    return true;
}

void SafetyRelease(const World& w) {
    bool hold = false;
    if (tainted && (way == Way::Hooks || way == Way::Flag)) {
        bool run = true, marked = false;
        try {
            Object* sm = un::Single(R.statsManager);
            run = sm || w.inLevel;
            marked = run && FlagRun(sm);
        } catch (...) {
        }
        if (marked) LOG("the run is marked as cheated, so it stays off the leaderboards after unloading");
        hold = run && !marked && way == Way::Hooks;
    }
    if (hold && !keepHooks) LOG("the upload block stays on until the game closes, this run could not be marked as cheated");
    if (hold) keepHooks = true;
    if (!keepHooks) FreeUploads();
    tainted = false;
    runId = 0;
    way = Way::None;
}

void AgeMarks() {
    int now = tickNo.load();
    std::lock_guard<std::mutex> g(markLock);
    markSets.erase(std::remove_if(markSets.begin(), markSets.end(), [now](const MarkSet& s) { return now - s.tick >= kMarkFrames; }),
                   markSets.end());
}

void ClearMarks() {
    std::lock_guard<std::mutex> g(markLock);
    markSets.clear();
}

bool AnyBusy() {
    bool any = false;
    for (Slot& sl : slots) {
        if (!sl.ok) continue;
        bool busy = true;
        faultWhere = sl.g->name;
        if (!GuardBusy(sl.g, &busy)) ++sl.faults;
        any = any || busy;
    }
    faultWhere = "the game update";
    return any;
}

void ReleaseGroups(const Ctx& c, bool onlyOutside = false) {
    for (Slot& sl : slots) {
        if (!sl.ok || (onlyOutside && !sl.outside)) continue;
        faultWhere = sl.g->name;
        if (GuardRelease(sl.g, &c)) continue;
        ++sl.faults;
        LOG("%s could not switch everything off", sl.g->name);
    }
    faultWhere = "the game update";
}

void Drop(Slot& sl, const Ctx& c) {
    sl.ok = false;
    sl.dropped = true;
    LOG("%s cheats stopped after %d errors", sl.g->name, sl.faults);
    faultWhere = sl.g->name;
    if (!GuardRelease(sl.g, &c)) LOG("%s could not switch everything off", sl.g->name);
    for (const std::string& id : featureIds) {
        faultWhere = sl.g->name;
        bool mine = GuardSupported(sl.g, &id);
        if (mine && !Owned(id)) pendingOff.push_back(id);
    }
    faultWhere = "the game update";
    Note(std::string("Some ") + sl.g->label + " cheats were switched off after an error");
}

void Unguarded(const Ctx& c) {
    LOG("the leaderboard guard stopped working, every cheat is switched off");
    for (const std::string& id : featureIds)
        if (Owned(id)) pendingOff.push_back(id);
    ReleaseGroups(c);
    for (Slot& sl : slots) sl.ok = false;
    Note("Every cheat was switched off because the leaderboard guard stopped working");
}

void TickGroups(const Ctx& c) {
    for (Slot& sl : slots) {
        if (!sl.ok || (!c.inLevel && !sl.outside)) continue;
        faultWhere = sl.g->name;
        if (!GuardTick(sl.g, &c)) ++sl.faults;
    }
    faultWhere = "the game update";
}

std::vector<std::string> TakeQueued() {
    std::lock_guard<std::mutex> g(qLock);
    std::vector<std::string> out;
    out.swap(queued);
    return out;
}

bool AnyOneShot(const std::vector<std::string>& todo) {
    for (const std::string& id : todo)
        if (Owned(id)) return true;
    return false;
}

void RunActions(const std::vector<std::string>& todo, const Ctx& c) {
    for (const std::string& id : todo) {
        bool any = false;
        for (Slot& sl : slots) {
            if (!Owns(sl, id)) continue;
            any = true;
            faultWhere = sl.g->name;
            if (!GuardAction(sl.g, &id, &c)) {
                ++sl.faults;
                LOG("%s: %s failed", sl.g->name, id.c_str());
            }
            faultWhere = "the game update";
        }
        if (!any) Note("That is not available right now");
    }
}

void DropFaulty(const Ctx& c) {
    for (Slot& sl : slots)
        if (sl.ok && sl.faults >= kMaxFaults) Drop(sl, c);
}

std::string SceneName() { return mono::Str(un::Static(R.shCurrent)); }

void Transitions(const World& w, const Ctx& c) {
    if (wasInLevel && !w.inLevel) {
        LOG("no live player any more (level left, loading or player hidden)");
        ReleaseGroups(c);
        ClearMarks();
    } else if (wasInLevel && w.nmId != lastNmId) {
        LOG("the player object changed");
        ReleaseGroups(c);
    } else if (wasInLevel && w.dead && !wasDead) {
        LOG("the player died");
        ReleaseGroups(c);
    } else if (w.scene != lastScene) {
        ReleaseGroups(c, true);
    }
    if (!wasInLevel && w.inLevel) LOG("in a level: %s", SceneName().c_str());
}

void Remember(const World& w) {
    wasInLevel = w.inLevel;
    wasDead = w.dead;
    lastNmId = w.nmId;
    lastScene = w.scene;
}

void Tick(int frame) {
    lastTick = GetTickCount64();
    ++tickNo;
    TakeSnap();
    World w = ReadWorld(frame);
    MenuInput(w, false);
    std::vector<std::string> todo = TakeQueued();
    Ctx c = MakeCtx(snap, w, false);
    if (!SafetyPreTick(snap.inUse || AnyBusy(), AnyOneShot(todo), w.inLevel)) {
        Unguarded(c);
        todo.clear();
    }
    Transitions(w, c);
    TickGroups(c);
    RunActions(todo, c);
    DropFaulty(c);
    AgeMarks();
    Remember(w);
}

template <class F>
void Step(F&& fn) {
    try {
        fn();
    } catch (...) {
        if (faults++ < 20) LOG("error while switching the cheats off");
    }
}

bool Unsubscribe() {
    if (!delegateHandle) return true;
    Object* del = mono::Pinned(delegateHandle);
    bool ok = !del;
    if (del) un::Call(R.removeWillRender, nullptr, {del}, &ok);
    if (ok) {
        mono::Unpin(delegateHandle);
        delegateHandle = 0;
        subscribed = false;
    }
    return ok;
}

void StopFrame() {
    lastTick = GetTickCount64();
    Snap off = OffSnap();
    World w;
    Step([&] { w = ReadWorld(un::FrameCount()); });
    Ctx c;
    Step([&] { c = MakeCtx(off, w, true); });
    Step([&] { MenuInput(w, true); });
    Step([&] { ReleaseGroups(c); });
    Step([&] { SafetyRelease(w); });
    Step([&] { ClearMarks(); });
    Step([&] { FreeMenuKey(); });
    Step([&] { unsubscribed = Unsubscribe(); });
    wasInLevel = false;
    wasDead = false;
    lastNmId = 0;
    lastScene = Scene::Other;
}

void FrameTick() {
    int fc = un::FrameCount();
    if (fc >= 0 && fc == lastFrame) return;
    lastFrame = fc;
    Tick(fc);
}

void SafeTick() {
    __try {
        FrameTick();
    } __except (Filter(GetExceptionCode())) {
    }
}

void SafeStop() {
    __try {
        StopFrame();
    } __except (Filter(GetExceptionCode())) {
    }
}

void __stdcall OnRender() {
    int st = stop.load();
    if (st >= 2 || inside) return;
    inside = true;
    ticking.fetch_add(1);
    if (st == 1) {
        SafeStop();
        if (unsubscribed) {
            stop = 2;
            LOG("cheats switched off and detached from the game");
        } else if (++stopAttempts >= 5) {
            stop = 3;
            LOG("could not detach from the game");
        }
    } else {
        SafeTick();
    }
    inside = false;
    ticking.fetch_sub(1);
}

void ResolveGroups() {
    for (const Group& g : kGroups) {
        Slot sl;
        sl.g = &g;
        un::missing.clear();
        bool ok = false;
        faultWhere = g.name;
        GuardResolve(&g, &ok);
        faultWhere = "the game update";
        for (const std::string& m : un::missing) LOG("%s: not found in the game: %s", g.name, m.c_str());
        if (!ok) LOG("%s cheats are not available", g.name);
        sl.ok = ok && guardOk;
        faultWhere = g.name;
        sl.outside = sl.ok && GuardOutside(&g);
        faultWhere = "the game update";
        slots.push_back(sl);
    }
    un::missing.clear();
}

}

bool Ctx::on(const std::string& id) const {
    if (!snap) return false;
    auto it = snap->on.find(id);
    return it != snap->on.end() && it->second;
}

float Ctx::value(const std::string& id, float fallback) const {
    if (!snap) return fallback;
    auto it = snap->value.find(id);
    return it != snap->value.end() ? it->second : fallback;
}

void Ctx::Note(const std::string& text) const { uk::Note(text); }

void Note(const std::string& text) {
    std::lock_guard<std::mutex> g(qLock);
    if (notes.size() < 32) notes.push_back(text);
}

const std::wstring& Folder() { return folder; }

std::string Backup() {
    std::function<std::string()> fn;
    {
        std::lock_guard<std::mutex> g(backupLock);
        fn = backupFn;
    }
    return fn ? fn() : std::string();
}

std::string LastBackup() {
    std::lock_guard<std::mutex> g(backupLock);
    return lastBackup;
}

void Mark(const std::string& owner, std::vector<Marker> markers) {
    std::lock_guard<std::mutex> g(markLock);
    auto it = std::find_if(markSets.begin(), markSets.end(), [&](const MarkSet& s) { return s.owner == owner; });
    if (markers.empty()) {
        if (it != markSets.end()) markSets.erase(it);
        return;
    }
    if (markers.size() > kMaxMarks) markers.resize(kMaxMarks);
    if (it == markSets.end()) {
        markSets.emplace_back();
        it = markSets.end() - 1;
        it->owner = owner;
    }
    it->tick = tickNo.load();
    it->at = GetTickCount64();
    it->list = std::move(markers);
}

std::vector<Marker> Marks() {
    std::vector<Marker> out;
    std::lock_guard<std::mutex> g(markLock);
    ULONGLONG now = GetTickCount64();
    for (const MarkSet& s : markSets)
        if (now - s.at < kMarkMs) out.insert(out.end(), s.list.begin(), s.list.end());
    return out;
}

}

namespace ukgame {

using namespace uk;

bool Prepare(std::string& err) {
    if (!un::Resolve(err)) return false;
    for (const std::string& m : un::missing) LOG("not found in the game: %s", m.c_str());
    if (!slots.empty()) return true;
    flagReady = R.assistController && R.acCheats && R.msGetInstance;
    MakeUploads();
    guardOk = hooksReady || flagReady;
    if (hooksReady) LOG("leaderboard guard: Fathom blocks the score uploads of cheated runs");
    else if (flagReady) LOG("leaderboard guard: the upload block is not available, cheated runs are marked as cheated in the game");
    else LOG("the leaderboard guard was not found, every cheat stays off");
    ResolveGroups();
    return true;
}

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    featureIds.clear();
    sliderDefaults.clear();
    snap = Snap{};
    auto none = [](Feature&) {};
    for (Feature& f : m.features) {
        featureIds.push_back(f.id);
        if (f.kind == core::Kind::Toggle) snap.on[f.id] = false;
        if (f.kind == core::Kind::Slider) {
            sliderDefaults[f.id] = f.def;
            snap.value[f.id] = f.def;
        }
        f.available = Owned(f.id);
        if (!f.available) {
            f.missing = "Not found in this version of the game";
            continue;
        }
        if (f.kind == core::Kind::Action) {
            std::string id = f.id;
            f.apply = [id](Feature&) {
                std::lock_guard<std::mutex> g(qLock);
                if (queued.size() < 16) queued.push_back(id);
            };
        } else {
            f.apply = none;
        }
    }
}

bool Start(std::string& err) {
    Object* type = mono::TypeObject(R.willRenderType);
    void* fn = (void*)&OnRender;
    bool ok = false;
    Object* del = type ? un::Call(R.delegateForPointer, nullptr, {&fn, type}, &ok) : nullptr;
    if (!ok || !del) {
        err = "could not create the game callback";
        return false;
    }
    delegateHandle = mono::Pin(del);
    un::Call(R.addWillRender, nullptr, {del}, &ok);
    if (!ok) {
        mono::Unpin(delegateHandle);
        delegateHandle = 0;
        err = "could not attach to the game's frame";
        return false;
    }
    subscribed = true;
    LOG("attached to the game's frame");
    return true;
}

void RequestStop() {
    int expected = 0;
    stop.compare_exchange_strong(expected, subscribed ? 1 : 2);
}

bool Stopped() { return stop.load() >= 2 && ticking.load() == 0; }

bool SafeToUnload() { return stop.load() == 2 && !keepHooks.load(); }

bool HoldsHooks() { return keepHooks.load(); }

bool Ticking() { return GetTickCount64() - lastTick.load() < 2000; }

std::vector<std::string> TakeNotes() {
    std::lock_guard<std::mutex> g(qLock);
    std::vector<std::string> out;
    out.swap(notes);
    return out;
}

void SetMenu(bool open, bool focused) {
    menuOpen = open;
    menuFocused = focused;
}

void SetGameWindow(void* hwnd) { gameWnd = hwnd; }

void SetFolder(const std::wstring& dir) { folder = dir; }

void SetBackup(std::function<std::string()> backup) {
    std::lock_guard<std::mutex> g(backupLock);
    backupFn = std::move(backup);
}

void SetLastBackup(const std::string& path) {
    std::lock_guard<std::mutex> g(backupLock);
    lastBackup = path;
}

void Note(const std::string& text) { uk::Note(text); }

}
