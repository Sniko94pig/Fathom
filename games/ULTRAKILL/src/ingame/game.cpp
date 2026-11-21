#include "ingame/game.h"
#include "ingame/cheats.h"
#include "ingame/groups.h"
#include "ingame/log.h"
#include "ingame/unity.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
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
};

const Group kGroups[] = {
    {"player", "player", ukplayer::Resolve, ukplayer::Supported, ukplayer::Tick, ukplayer::Release, ukplayer::Action, ukplayer::Busy},
    {"movement", "movement", ukmove::Resolve, ukmove::Supported, ukmove::Tick, ukmove::Release, ukmove::Action, ukmove::Busy},
    {"weapons_a", "weapon", ukweapa::Resolve, ukweapa::Supported, ukweapa::Tick, ukweapa::Release, ukweapa::Action, ukweapa::Busy},
    {"weapons_b", "weapon", ukweapb::Resolve, ukweapb::Supported, ukweapb::Tick, ukweapb::Release, ukweapb::Action, ukweapb::Busy},
    {"shop", "shop", ukshop::Resolve, ukshop::Supported, ukshop::Tick, ukshop::Release, ukshop::Action, ukshop::Busy},
};

const int kMaxFaults = 25;

struct Slot {
    const Group* g = nullptr;
    bool ok = false;
    bool dropped = false;
    int faults = 0;
};

struct World {
    int frame = -1;
    bool loading = false, mainMenu = true, paused = false, inIntro = false;
    bool inLevel = false, dead = false, activated = false, levelOver = false;
    Object* nm = nullptr;
    Object* om = nullptr;
    Object* gsm = nullptr;
    int nmId = 0;
};

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

bool tainted = false;
int runId = 0;
bool warned = false;

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
            inUse = inUse || on;
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
    if (w.loading || w.mainMenu) return w;
    Object* nm = un::Single(R.newMovement);
    if (!nm || !un::ActiveInHierarchy(nm)) return w;
    w.inLevel = true;
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

void SafetyPreTick(bool inUse, bool oneShot, bool inLevel) {
    Object* sm = un::Single(R.statsManager);
    int smId = sm ? un::InstanceId(sm) : 0;
    if (inUse || oneShot) {
        if (!tainted) LOG("cheats in use, this run will not go to the leaderboards");
        tainted = true;
        runId = smId;
    } else if (tainted && (smId == 0 || smId != runId)) {
        tainted = false;
        runId = 0;
        LOG("every cheat is off, the next run can go to the leaderboards");
    }
    if (!tainted || (!smId && !inLevel)) return;
    Object* ac = un::Single(R.assistController);
    if (ac) un::SetVal<uint8_t>(ac, R.acCheats, 1);
    Object* cached = un::Ref(sm, R.smAsscon);
    if (cached != ac && un::Alive(cached)) un::SetVal<uint8_t>(cached, R.acCheats, 1);
    if (smId && !warned) {
        warned = true;
        Note("Scores from this run will not go to the leaderboards");
    }
}

void SafetyRelease() {
    tainted = false;
    runId = 0;
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

void ReleaseGroups(const Ctx& c) {
    for (Slot& sl : slots) {
        if (!sl.ok) continue;
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

void TickGroups(const Ctx& c) {
    for (Slot& sl : slots) {
        if (!sl.ok) continue;
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
    } else if (wasInLevel && w.nmId != lastNmId) {
        LOG("the player object changed");
        ReleaseGroups(c);
    } else if (wasInLevel && w.dead && !wasDead) {
        LOG("the player died");
        ReleaseGroups(c);
    }
    if (!wasInLevel && w.inLevel) LOG("in a level: %s", SceneName().c_str());
}

void Remember(const World& w) {
    wasInLevel = w.inLevel;
    wasDead = w.dead;
    lastNmId = w.nmId;
}

void Tick(int frame) {
    lastTick = GetTickCount64();
    TakeSnap();
    World w = ReadWorld(frame);
    MenuInput(w, false);
    std::vector<std::string> todo = TakeQueued();
    Ctx c = MakeCtx(snap, w, false);
    SafetyPreTick(snap.inUse || AnyBusy(), AnyOneShot(todo), w.inLevel);
    Transitions(w, c);
    TickGroups(c);
    RunActions(todo, c);
    DropFaulty(c);
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
    Step([&] { SafetyRelease(); });
    Step([&] { FreeMenuKey(); });
    Step([&] { unsubscribed = Unsubscribe(); });
    wasInLevel = false;
    wasDead = false;
    lastNmId = 0;
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

}

namespace ukgame {

using namespace uk;

bool Prepare(std::string& err) {
    if (!un::Resolve(err)) return false;
    for (const std::string& m : un::missing) LOG("not found in the game: %s", m.c_str());
    guardOk = R.assistController && R.acCheats && R.msGetInstance;
    if (!guardOk) LOG("the leaderboard guard was not found, every cheat stays off");
    if (slots.empty()) ResolveGroups();
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

bool SafeToUnload() { return stop.load() == 2; }

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
