#include "ingame/game.h"
#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/unity.h"
#include "MinHook.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>

namespace sotf {

namespace {

using core::Feature;
using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;

struct Slot {
    const Group* g = nullptr;
    bool ok = false;
    bool dropped = false;
    int faults = 0;
};
std::vector<Slot> slots;
std::vector<size_t> droppedPending;

std::mutex qLock;
std::vector<std::string> queued;
std::vector<std::string> notes;

std::atomic<bool> menuOpen{false}, menuFocused{false};
std::atomic<int> stop{0};
std::atomic<int> ticking{0};
std::atomic<int> inHook{0};
std::atomic<ULONGLONG> lastTick{0};
std::atomic<void*> gameWnd{nullptr};
std::wstring folder;

std::unordered_map<std::string, float> sliderDefaults;
Snap lastSnap, prevSnap;
bool wasInWorld = false;
int faults = 0;
core::Session lastSession = core::Session::Solo;
bool sessionKnown = false;

struct WorldRefs {
    Class* localPlayer = nullptr;
    Fld lpInstance;
    Class* bolt = nullptr;
    Method *boltRunning = nullptr, *boltServer = nullptr, *boltClient = nullptr;
    Class* loadScene = nullptr;
    Fld lsLoading, lsMainLoaded;
    Method* saveLoading = nullptr;
    Class *inputSystem = nullptr, *inputState = nullptr, *softwareCursor = nullptr;
    Method *inSetState = nullptr, *inGetState = nullptr, *inCursor = nullptr, *curRefresh = nullptr;
    int lockedState = 0;
} W;

using TickFn = void (*)(const void*);
TickFn oTick = nullptr;
void* tickTarget = nullptr;
bool hooked = false;

Snap OffSnap(const Snap& from) {
    Snap s;
    for (const auto& [id, on] : from.on) s.on[id] = false;
    for (const auto& [id, v] : from.value) {
        auto it = sliderDefaults.find(id);
        s.value[id] = it != sliderDefaults.end() ? it->second : v;
    }
    s.menuOpen = from.menuOpen;
    s.menuFocused = from.menuFocused;
    s.mapVisible = from.mapVisible;
    return s;
}

void ApplyDropped() {
    for (size_t i : droppedPending) {
        if (i >= slots.size()) continue;
        const Group* g = slots[i].g;
        for (Feature& f : model->features)
            if (g->supported && g->supported(f.id)) {
                f.available = false;
                f.missing = "Switched off after an error in the game";
            }
    }
    droppedPending.clear();
}

Snap TakeSnap() {
    if (!modelLock || !model) return lastSnap;
    std::unique_lock<std::mutex> g(*modelLock, std::try_to_lock);
    if (!g.owns_lock()) return lastSnap;
    if (!droppedPending.empty()) ApplyDropped();
    if (sessionKnown && model->session != lastSession) model->session = lastSession;
    model->inWorld = wasInWorld;
    Snap s;
    bool allowed = model->cheatsAllowed();
    for (const Feature& f : model->features) {
        bool live = f.available && !f.building && allowed;
        if (f.kind == core::Kind::Toggle) s.on[f.id] = live && f.on;
        else if (f.kind == core::Kind::Slider) s.value[f.id] = live ? f.value : f.def;
    }
    s.menuOpen = menuOpen.load();
    s.menuFocused = menuFocused.load();
    s.mapVisible = model->mapVisible;
    lastSnap = s;
    return s;
}

core::Session DetectSession() {
    if (!W.boltRunning || !un::CallBool(W.boltRunning, nullptr)) return core::Session::Solo;
    if (W.boltServer && un::CallBool(W.boltServer, nullptr)) return core::Session::Host;
    if (W.boltClient && un::CallBool(W.boltClient, nullptr)) return core::Session::Client;
    return core::Session::Solo;
}

void PublishSession(core::Session s) {
    if (sessionKnown && s == lastSession) return;
    bool wasClient = sessionKnown && lastSession == core::Session::Client;
    lastSession = s;
    sessionKnown = true;
    if (s == core::Session::Client) Note("You joined someone else's game, cheats are off");
    else if (wasClient) Note("Cheats are available again");
    LOG("session: %s", s == core::Session::Client ? "client" : s == core::Session::Host ? "host" : "single player");
}

Object* LocalPlayer() {
    Object* p = un::Static(W.lpInstance);
    return un::Alive(p) ? p : nullptr;
}

bool Loading() {
    if (W.lsLoading && un::StaticVal<bool>(W.lsLoading, false)) return true;
    if (W.lsMainLoaded && !un::StaticVal<bool>(W.lsMainLoaded, true)) return true;
    if (W.saveLoading && un::CallBool(W.saveLoading, nullptr)) return true;
    return false;
}

bool cursorFreed = false;
bool ownLock = false;
int lockWas = 0;
bool visibleWas = false;

bool InputLocked() {
    if (!W.inGetState) return false;
    int st = W.lockedState;
    return un::CallBool(W.inGetState, nullptr, {&st});
}

void SetInputLock(bool on) {
    if (!W.inSetState) return;
    int st = W.lockedState;
    un::Call(W.inSetState, nullptr, {&st, &on});
}

void MenuInput(bool stopping) {
    bool focus = !stopping && menuOpen.load() && menuFocused.load();
    if (focus) {
        if (!cursorFreed) {
            lockWas = un::CallInt(un::E.cursorGetLock, nullptr);
            visibleWas = un::CallBool(un::E.cursorGetVisible, nullptr);
            cursorFreed = true;
        }
        if (W.inSetState && !InputLocked()) {
            SetInputLock(true);
            ownLock = true;
        }
        if (un::E.cursorSetLock && un::CallInt(un::E.cursorGetLock, nullptr) != 0) {
            int none = 0;
            un::Call(un::E.cursorSetLock, nullptr, {&none});
        }
        if (un::E.cursorSetVisible && !un::CallBool(un::E.cursorGetVisible, nullptr)) {
            bool yes = true;
            un::Call(un::E.cursorSetVisible, nullptr, {&yes});
        }
    } else if (cursorFreed) {
        cursorFreed = false;
        if (ownLock) {
            SetInputLock(false);
            ownLock = false;
        }
        Object* cursor = W.inCursor ? un::Call(W.inCursor, nullptr) : nullptr;
        if (cursor && W.curRefresh) {
            un::Call(W.curRefresh, cursor);
        } else {
            if (un::E.cursorSetLock) un::Call(un::E.cursorSetLock, nullptr, {&lockWas});
            if (un::E.cursorSetVisible) un::Call(un::E.cursorSetVisible, nullptr, {&visibleWas});
        }
    }
}

int Filter(unsigned int code) {
    if (faults++ < 20) LOG("crash avoided in the game update (code %08X)", code);
    return EXCEPTION_EXECUTE_HANDLER;
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
        return g->action(*id, *c);
    } __except (Filter(GetExceptionCode())) {
        return false;
    }
}

void ReleaseGroups(const Ctx& c) {
    for (Slot& sl : slots)
        if (sl.ok && sl.g->release && !GuardRelease(sl.g, &c)) LOG("%s could not switch everything off", sl.g->name);
}

void Drop(size_t i, const Ctx& c) {
    Slot& sl = slots[i];
    sl.ok = false;
    sl.dropped = true;
    LOG("%s stopped after repeated errors", sl.g->name);
    if (sl.g->release) GuardRelease(sl.g, &c);
    droppedPending.push_back(i);
    Note(std::string("Some ") + sl.g->name + " cheats were switched off after an error");
}

void RunActions(const Ctx& c) {
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> g(qLock);
        todo.swap(queued);
    }
    for (const std::string& id : todo) {
        if (!c.inWorld) {
            Note("Load a save first");
            continue;
        }
        if (c.session == core::Session::Client) {
            Note("Cheats are off when you join someone else's game");
            continue;
        }
        bool done = false;
        for (Slot& sl : slots) {
            if (!sl.ok || !sl.g->action || !sl.g->supported || !sl.g->supported(id)) continue;
            if (!GuardAction(sl.g, &id, &c)) LOG("action %s failed", id.c_str());
            done = true;
            break;
        }
        if (!done) Note("That is not available right now");
    }
}

void Tick() {
    lastTick = GetTickCount64();
    Snap s = TakeSnap();
    double now = GetTickCount64() / 1000.0;
    float dt = std::clamp(un::DeltaTime(), 0.0f, 0.1f);
    core::Session session = DetectSession();
    PublishSession(session);
    if (session == core::Session::Client) s = OffSnap(s);
    Object* player = LocalPlayer();
    bool inWorld = player != nullptr && !Loading();
    MenuInput(false);
    Ctx c{s, prevSnap, inWorld ? player : nullptr, inWorld, false, session, now, dt};
    if (!inWorld && wasInWorld) ReleaseGroups(c);
    for (size_t i = 0; i < slots.size(); ++i) {
        Slot& sl = slots[i];
        if (!sl.ok || !sl.g->tick) continue;
        if (!GuardTick(sl.g, &c) && ++sl.faults >= 25) Drop(i, c);
    }
    RunActions(c);
    wasInWorld = inWorld;
    prevSnap = s;
}

void StopFrame() {
    lastTick = GetTickCount64();
    Snap off = OffSnap(lastSnap);
    Object* player = LocalPlayer();
    bool inWorld = player != nullptr && !Loading();
    Ctx c{off, prevSnap, inWorld ? player : nullptr, inWorld, true, lastSession, GetTickCount64() / 1000.0, 0.0f};
    MenuInput(true);
    ReleaseGroups(c);
    prevSnap = off;
    wasInWorld = false;
}

void SafeTick() {
    __try {
        Tick();
    } __except (Filter(GetExceptionCode())) {
    }
}

void SafeStop() {
    __try {
        StopFrame();
    } __except (Filter(GetExceptionCode())) {
    }
}

void HookTick(const void* mi) {
    inHook.fetch_add(1);
    int st = stop.load();
    if (st < 2) {
        ticking.fetch_add(1);
        if (st == 1) {
            SafeStop();
            stop = 2;
            LOG("cheats switched off");
        } else {
            SafeTick();
        }
        ticking.fetch_sub(1);
    }
    oTick(mi);
    inHook.fetch_sub(1);
}

}

void Note(const std::string& text) {
    std::lock_guard<std::mutex> g(qLock);
    if (notes.size() < 32) notes.push_back(text);
}

void WarnAchievementsOnce() {
    static bool warned = false;
    if (warned) return;
    warned = true;
    Note("Heads up: this can unlock Steam achievements, and Steam keeps them");
}

bool GameFocused() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

bool KeyDown(int vk) { return GameFocused() && !menuFocused.load() && (GetAsyncKeyState(vk) & 0x8000) != 0; }

const std::wstring& Folder() { return folder; }

}

namespace sotfgame {

using namespace sotf;

bool Prepare(std::string& err) {
    if (!un::Resolve(err)) return false;
    W.localPlayer = il2::FindClass("Sons", "TheForest.Utils", "LocalPlayer");
    W.lpInstance = W.localPlayer ? un::F(W.localPlayer, "_instance") : Fld{};
    if (!W.localPlayer || !W.lpInstance) {
        err = "the game's player was not found";
        return false;
    }
    W.bolt = il2::FindClass("bolt", "", "BoltNetwork");
    if (W.bolt) {
        W.boltRunning = il2::Getter(W.bolt, "isRunning");
        W.boltServer = il2::Getter(W.bolt, "isServer");
        W.boltClient = il2::Getter(W.bolt, "isClient");
    } else {
        un::Missing("BoltNetwork");
    }
    W.loadScene = un::C("Sons.Loading", "Sons.Loading", "LoadScene");
    if (W.loadScene) {
        W.lsLoading = un::F(W.loadScene, "_isLoading");
        W.lsMainLoaded = un::F(W.loadScene, "_isMainSceneLoadComplete");
    }
    if (Class* sgm = un::C("Sons.Save", "Sons.Save", "SaveGameManager")) W.saveLoading = il2::FindStaticMethod(sgm, "LoadInProgress", 0);
    W.inputSystem = un::C("Sons.Input", "Sons.Input", "InputSystem");
    W.inputState = un::C("Sons.Input", "TheForest.Utils", "InputState");
    if (W.inputSystem && W.inputState) {
        W.inSetState = un::M(W.inputSystem, "SetState", std::vector<std::string>{"TheForest.Utils.InputState", "System.Boolean"});
        W.inGetState = un::M(W.inputSystem, "GetState", std::vector<std::string>{"TheForest.Utils.InputState"});
        W.inCursor = il2::FindStaticMethod(W.inputSystem, "get_Cursor", 0);
        W.softwareCursor = il2::Nested(W.inputSystem, "SoftwareCursor");
        W.curRefresh = W.softwareCursor ? un::M(W.softwareCursor, "RefreshCursorVisibility", 0) : nullptr;
        W.lockedState = (int)il2::EnumValue(W.inputState, "Locked", 0);
    }
    if (slots.empty()) {
        for (const Group* g : {&VitalsGroup(), &MovementGroup(), &WeaponsGroup(), &EnemiesGroup(), &BuildingGroup(), &WorldGroup(),
                               &CompanionsGroup(), &SpawnGroup(), &MapGroup()}) {
            Slot sl;
            sl.g = g;
            std::string gerr;
            sl.ok = !g->resolve || g->resolve(gerr);
            if (!sl.ok) LOG("%s not available: %s", g->name, gerr.c_str());
            slots.push_back(sl);
        }
    }
    return true;
}

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    sliderDefaults.clear();
    for (Feature& f : m.features)
        if (f.kind == core::Kind::Slider) sliderDefaults[f.id] = f.def;
    auto none = [](Feature&) {};
    for (Feature& f : m.features) {
        const Group* owner = nullptr;
        for (Slot& sl : slots)
            if (sl.ok && sl.g->supported && sl.g->supported(f.id)) {
                owner = sl.g;
                break;
            }
        if (!f.page.empty()) {
            f.available = owner != nullptr;
            if (!owner) f.missing = "Not found in this version of the game";
            continue;
        }
        if (!owner) {
            f.available = false;
            f.missing = "Not found in this version of the game";
            continue;
        }
        f.available = true;
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
    for (Slot& sl : slots)
        if (sl.ok && sl.g->wire) sl.g->wire(m, lock);
}

bool Start(std::string& err) {
    Class* canvas = il2::FindClass("UnityEngine.UIModule", "UnityEngine", "Canvas");
    Method* m = canvas ? il2::FindStaticMethod(canvas, "SendWillRenderCanvases", 0) : nullptr;
    void* target = il2::MethodPointer(m);
    if (!target) {
        err = "could not find the game's frame function";
        return false;
    }
    if (MH_CreateHook(target, (void*)&HookTick, (void**)&oTick) != MH_OK || MH_EnableHook(target) != MH_OK) {
        err = "could not attach to the game's frame";
        return false;
    }
    tickTarget = target;
    hooked = true;
    LOG("attached to the game's frame");
    for (Slot& sl : slots) {
        if (!sl.ok || !sl.g->start) continue;
        std::string gerr;
        if (!sl.g->start(gerr)) LOG("%s could not attach some parts: %s", sl.g->name, gerr.c_str());
    }
    return true;
}

void RequestStop() {
    int expected = 0;
    stop.compare_exchange_strong(expected, hooked ? 1 : 2);
}

bool Stopped() { return stop.load() >= 2 && ticking.load() == 0; }

bool SafeToUnload() { return stop.load() == 2; }

bool Ticking() { return GetTickCount64() - lastTick.load() < 2000; }

bool Idle() { return inHook.load() == 0; }

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

}
