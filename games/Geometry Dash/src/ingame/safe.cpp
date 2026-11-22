#include "ingame/gd.h"
#include "ingame/gdx.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include "ui/ui.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <string>

namespace safe {

namespace {

using VoidFn = void(__fastcall*)(void*);
using InitFn = bool(__fastcall*)(void*, void*, bool);
using SavePctFn = void(__fastcall*)(void*, int, bool, int, int, bool);
using GetStatFn = int(__fastcall*)(void*, const char*);
using IncStatFn = void(__fastcall*)(void*, const char*, int);
using SetStatFn = void(__fastcall*)(void*, const char*, int);
using LevelIntFn = int(__fastcall*)(void*, void*);
using LevelVoidFn = void(__fastcall*)(void*, void*);
using BoolFn = bool(__fastcall*)(void*);
using ChallengeFn = void(__fastcall*)(void*, int, int);
using KeyFn = void(__fastcall*)(void*, const char*);
using SendFn = void(__fastcall*)(void*, void*);
using UrlFn = const char*(__fastcall*)(void*);
using RespCtorFn = void*(__fastcall*)(void*, void*);
using RespBoolFn = void(__fastcall*)(void*, bool);
using RespCodeFn = void(__fastcall*)(void*, long);
using SaveDictFn = bool(__fastcall*)(void*, const char*, bool);
using ManagerSaveFn = bool(__fastcall*)(void*, bool);
using WriteAtomicFn = bool(__fastcall*)(const void*, const void*);
using MallocFn = void* (*)(size_t);
using SelectorFn = void(__fastcall*)(void*, void*, void*);

constexpr uint32_t kCommitPlaytime = 0x51E8E0;
constexpr uint32_t kManagerSave = 0x77530;
constexpr uint32_t kWriteAtomic = 0x73820;
constexpr uint32_t kSetStat = 0x2DB290;

constexpr uintptr_t kTrackOff = 0x39FA;
constexpr uintptr_t kPlaytimeNow = 0x3C18;
constexpr uintptr_t kPlaytimeCommitted = 0x3B18;
constexpr uintptr_t kPlayerDead = 0x9C8;
constexpr uintptr_t kPlayerDied = 0x334A;
constexpr uintptr_t kEndAnimation = 0x3670;
constexpr uintptr_t kLevelDone = 0x3BD5;

constexpr uintptr_t kLevelAttempts = 0x364;
constexpr uintptr_t kLevelJumps = 0x370;
constexpr uintptr_t kPracticeAttempts = 0x6AC;
constexpr uintptr_t kOrbSeed = 0x3A8;
constexpr uintptr_t kOrbRand = 0x3AC;
constexpr uintptr_t kCompletions = 0x748;
constexpr uintptr_t kBestTime = 0x588;
constexpr uintptr_t kBestPoints = 0x5B8;

InitFn oInit = nullptr;
VoidFn oDtor = nullptr;
VoidFn oReset = nullptr;
VoidFn oComplete = nullptr;
VoidFn oQuit = nullptr;
VoidFn oPersist = nullptr;
VoidFn oProcessCheckpoints = nullptr;
VoidFn oEndSetup = nullptr;
VoidFn oCommitPlaytime = nullptr;
SavePctFn oSavePct = nullptr;
IncStatFn oIncStat = nullptr;
LevelIntFn oAwardCurrency = nullptr;
LevelIntFn oAwardDiamonds = nullptr;
BoolFn oAwardKey = nullptr;
LevelVoidFn oCompleted = nullptr;
LevelVoidFn oCompletedDemon = nullptr;
LevelVoidFn oCompletedStar = nullptr;
LevelVoidFn oCompletedPack = nullptr;
ChallengeFn oChallenge = nullptr;
KeyFn oSecretCoin = nullptr;
KeyFn oUserCoin = nullptr;
SendFn oSend = nullptr;
SaveDictFn oSaveDict = nullptr;
ManagerSaveFn oManagerSave = nullptr;
WriteAtomicFn oWriteAtomic = nullptr;

GetStatFn getStat = nullptr;
SetStatFn setStat = nullptr;
UrlFn getUrl = nullptr;
RespCtorFn respCtor = nullptr;
RespBoolFn respSucceed = nullptr;
RespCodeFn respCode = nullptr;
MallocFn crtMalloc = nullptr;

bool ready = false;
bool levelReady = false;
std::atomic<void*> layer{nullptr};
std::atomic<bool> cheated{false};
std::atomic<bool> tainted{false};
std::atomic<bool> quitting{false};
std::atomic<bool> locked{false};
std::atomic<bool> completeArmed{false};
std::string why;
int blockedCount = 0;

const char* const kStatKeys[] = {"3", "4", "5", "6", "7", "8", "12", "13", "14", "15", "21", "22", "28", "40", "41", "42"};

struct Counters {
    bool valid = false;
    void* level = nullptr;
    uint8_t attempts[12] = {};
    uint8_t jumps[12] = {};
    int practice = 0;
    bool statValid = false;
    int statAttempts = 0;
} counters;

constexpr int kProgressCount = 7;

struct Progress {
    bool valid = false;
    void* level = nullptr;
    std::map<std::string, int> stats;
    int values[kProgressCount] = {};
} progress;

bool ActiveNow() {
    gd::Cfg& c = gd::C();
    float s = c.speed.load();
    return std::fabs(s - 1.0f) > 0.001f || c.noclip.load() || c.jumps.load() || c.play.load();
}

int CheckpointCount(void* pl) {
    void* arr = pl ? gd::F<void*>(pl, gdx::off::PL_checkpointArray) : nullptr;
    return arr ? (int)gd::cc::Count(arr) : 0;
}

bool AttemptOver(void* pl) {
    void* p1 = gd::F<void*>(pl, gdx::off::GBGL_player1);
    if (p1 && gd::F<uint8_t>(p1, kPlayerDead)) return true;
    return gd::F<uint8_t>(pl, kPlayerDied) || gd::F<uint8_t>(pl, kEndAnimation) || gd::F<uint8_t>(pl, kLevelDone);
}

void LevelValues(void* lvl, int out[kProgressCount]) {
    namespace o = gdx::off;
    out[0] = gdx::Seeded(lvl, o::GJL_normalPercentSeed, o::GJL_normalPercentRand);
    out[1] = gd::F<int>(lvl, o::GJL_practicePercent);
    out[2] = gdx::Seeded(lvl, o::GJL_bestPercentSeed, o::GJL_bestPercentRand);
    out[3] = gdx::Seeded(lvl, kOrbSeed, kOrbRand);
    out[4] = gd::F<int>(lvl, kCompletions);
    out[5] = gd::F<int>(lvl, kBestTime);
    out[6] = gd::F<int>(lvl, kBestPoints);
}

void SnapCounters(void* pl) {
    counters = Counters{};
    void* lvl = gdx::Level(pl);
    if (!lvl) return;
    counters.level = lvl;
    std::memcpy(counters.attempts, (const uint8_t*)lvl + kLevelAttempts, 12);
    std::memcpy(counters.jumps, (const uint8_t*)lvl + kLevelJumps, 12);
    counters.practice = gd::F<int>(lvl, kPracticeAttempts);
    void* gsm = gdx::GameStatsManager();
    if (gsm && getStat) {
        counters.statAttempts = getStat(gsm, "2");
        counters.statValid = true;
    }
    counters.valid = true;
}

void RestoreCounters(void* pl) {
    if (!counters.valid) return;
    void* lvl = gdx::Level(pl);
    if (!lvl || lvl != counters.level) return;
    std::memcpy((uint8_t*)lvl + kLevelAttempts, counters.attempts, 12);
    std::memcpy((uint8_t*)lvl + kLevelJumps, counters.jumps, 12);
    gd::F<int>(lvl, kPracticeAttempts) = counters.practice;
    void* gsm = gdx::GameStatsManager();
    if (counters.statValid && gsm && getStat && setStat && getStat(gsm, "2") > counters.statAttempts) setStat(gsm, "2", counters.statAttempts);
}

void SnapProgress(void* pl) {
    progress = Progress{};
    void* gsm = gdx::GameStatsManager();
    if (gsm && getStat)
        for (const char* k : kStatKeys) progress.stats[k] = getStat(gsm, k);
    void* lvl = gdx::Level(pl);
    if (lvl) {
        progress.level = lvl;
        LevelValues(lvl, progress.values);
    }
    progress.valid = true;
}

void Lockdown(const std::string& reason) {
    if (locked.exchange(true)) return;
    LOG("safe mode lock: %s. Saving and server requests are stopped until the game restarts", reason.c_str());
    gd::Note("Safe mode stopped all saving. Close Geometry Dash to keep your old save");
}

void CheckLeak(void* pl) {
    if (!progress.valid) return;
    void* gsm = gdx::GameStatsManager();
    if (gsm && getStat)
        for (const auto& kv : progress.stats) {
            int now = getStat(gsm, kv.first.c_str());
            if (now > kv.second) {
                Lockdown("stat " + kv.first + " went from " + std::to_string(kv.second) + " to " + std::to_string(now) +
                         " during a cheated attempt");
                return;
            }
        }
    void* lvl = pl ? gdx::Level(pl) : nullptr;
    if (!lvl || lvl != progress.level) return;
    int now[kProgressCount];
    LevelValues(lvl, now);
    for (int i = 0; i < kProgressCount; ++i) {
        bool moved = i >= 5 ? now[i] != progress.values[i] : now[i] > progress.values[i];
        if (moved) {
            Lockdown("level progress " + std::to_string(i) + " went from " + std::to_string(progress.values[i]) + " to " +
                     std::to_string(now[i]) + " during a cheated attempt");
            return;
        }
    }
}

void Settle(void* pl) {
    if (!cheated.load() || !pl) return;
    RestoreCounters(pl);
    CheckLeak(pl);
}

void EndAttempt(void* pl) {
    Settle(pl);
    counters.valid = false;
    progress.valid = false;
}

void BeginSession(void* pl) {
    layer = pl;
    bool now = ActiveNow();
    cheated = now;
    tainted = now;
    quitting = false;
    completeArmed = false;
    SnapCounters(pl);
    SnapProgress(pl);
    if (now) LOG("level opened with cheats on, this attempt won't count");
}

void EndSession() {
    void* pl = layer.load();
    if (!pl) return;
    EndAttempt(pl);
    layer = nullptr;
    cheated = false;
    quitting = false;
    completeArmed = false;
    if (tainted.exchange(false)) LOG("left the level, server requests are allowed again");
}

bool Guarding() {
    void* pl = layer.load();
    return pl && cheated.load();
}

void Track() {
    void* pl = gdx::PlayLayer();
    void* cur = layer.load();
    if (pl != cur) {
        if (cur) EndSession();
        if (pl) BeginSession(pl);
    }
}

bool __fastcall hkInit(void* pl, void* level, bool dontCreate) {
    gd::Busy busy;
    bool ok = oInit(pl, level, dontCreate);
    if (ok) {
        if (layer.load() && layer.load() != pl) EndSession();
        BeginSession(pl);
    }
    return ok;
}

void __fastcall hkDtor(void* pl) {
    gd::Busy busy;
    if (layer.load() == pl) EndSession();
    oDtor(pl);
}

void __fastcall hkReset(void* pl) {
    gd::Busy busy;
    if (layer.load() != pl) {
        if (layer.load()) EndSession();
        BeginSession(pl);
    }
    bool resume = CheckpointCount(pl) > 0 || gd::F<uint8_t>(pl, gdx::off::GBGL_isPracticeMode) != 0;
    EndAttempt(pl);
    bool now = ActiveNow();
    bool was = cheated.load();
    bool next = resume ? (was || now) : now;
    cheated = next;
    if (next) {
        tainted = true;
        if (!was) LOG("new attempt with cheats on, it won't count");
    } else if (was) {
        LOG("clean attempt, progress counts again");
    }
    SnapCounters(pl);
    SnapProgress(pl);
    oReset(pl);
}

void __fastcall hkComplete(void* pl) {
    gd::Busy busy;
    if (layer.load() == pl) {
        if (completeArmed.exchange(false)) MarkCheated("Complete level");
        else if (ActiveNow()) MarkCheated("level completed with cheats on");
    }
    Protect p;
    if (p.active) LOG("level completed during a cheated attempt, nothing is saved");
    oComplete(pl);
}

void __fastcall hkQuit(void* pl) {
    gd::Busy busy;
    {
        Protect p;
        oQuit(pl);
    }
    if (layer.load() == pl) {
        EndAttempt(pl);
        quitting = true;
        tainted = false;
    }
}

void __fastcall hkPersist(void* pl) {
    gd::Busy busy;
    Protect p;
    oPersist(pl);
}

void __fastcall hkProcessCheckpoints(void* pl) {
    gd::Busy busy;
    Protect p;
    oProcessCheckpoints(pl);
}

void __fastcall hkEndSetup(void* self) {
    gd::Busy busy;
    Protect p;
    oEndSetup(self);
}

void __fastcall hkCommitPlaytime(void* pl) {
    gd::Busy busy;
    if (Guarding() && layer.load() == pl) {
        gd::F<double>(pl, kPlaytimeCommitted) = gd::F<double>(pl, kPlaytimeNow);
        return;
    }
    oCommitPlaytime(pl);
}

void __fastcall hkSavePct(void* level, int percent, bool practice, int clicks, int attempts, bool chk) {
    gd::Busy busy;
    if (Guarding()) {
        LOG("blocked saving %d%% on a cheated attempt", percent);
        return;
    }
    oSavePct(level, percent, practice, clicks, attempts, chk);
}

void __fastcall hkIncStat(void* gsm, const char* key, int amount) {
    gd::Busy busy;
    if (Guarding()) return;
    oIncStat(gsm, key, amount);
}

int __fastcall hkAwardCurrency(void* gsm, void* level) {
    gd::Busy busy;
    if (Guarding()) {
        LOG("blocked orbs on a cheated attempt");
        return 0;
    }
    return oAwardCurrency(gsm, level);
}

int __fastcall hkAwardDiamonds(void* gsm, void* level) {
    gd::Busy busy;
    if (Guarding()) {
        LOG("blocked diamonds on a cheated attempt");
        return 0;
    }
    return oAwardDiamonds(gsm, level);
}

bool __fastcall hkAwardKey(void* gsm) {
    gd::Busy busy;
    if (Guarding()) return false;
    return oAwardKey(gsm);
}

void __fastcall hkCompleted(void* gsm, void* level) {
    gd::Busy busy;
    if (Guarding()) {
        LOG("blocked a level completion on a cheated attempt");
        return;
    }
    oCompleted(gsm, level);
}

void __fastcall hkCompletedDemon(void* gsm, void* level) {
    gd::Busy busy;
    if (Guarding()) return;
    oCompletedDemon(gsm, level);
}

void __fastcall hkCompletedStar(void* gsm, void* level) {
    gd::Busy busy;
    if (Guarding()) return;
    oCompletedStar(gsm, level);
}

void __fastcall hkCompletedPack(void* gsm, void* pack) {
    gd::Busy busy;
    if (Guarding()) return;
    oCompletedPack(gsm, pack);
}

void __fastcall hkChallenge(void* gsm, int type, int amount) {
    gd::Busy busy;
    if (Guarding()) return;
    oChallenge(gsm, type, amount);
}

void __fastcall hkSecretCoin(void* gsm, const char* key) {
    gd::Busy busy;
    if (Guarding()) return;
    oSecretCoin(gsm, key);
}

void __fastcall hkUserCoin(void* gsm, const char* key) {
    gd::Busy busy;
    if (Guarding()) return;
    oUserCoin(gsm, key);
}

bool BlockRequest(const char* url) {
    if (locked.load()) return true;
    if (layer.load() && !quitting.load() && (tainted.load() || (levelReady && ActiveNow()))) return true;
    return icons::BlockUpload(url);
}

void Fail(void* client, void* request) {
    if (!respCtor || !respSucceed || !respCode || !crtMalloc) return;
    void* mem = crtMalloc(0xA0);
    if (!mem) return;
    std::memset(mem, 0, 0xA0);
    void* response = respCtor(mem, request);
    respSucceed(response, false);
    respCode(response, 0);
    void* target = gd::F<void*>(request, 0x98);
    void* selector = gd::F<void*>(request, 0xA0);
    if (target && selector) ((SelectorFn)selector)(target, client, response);
    gd::cc::Release(response);
}

void __fastcall hkSend(void* client, void* request) {
    gd::Busy busy;
    const char* url = request && getUrl ? getUrl(request) : "";
    if (request && BlockRequest(url ? url : "")) {
        if (blockedCount++ < 50) LOG("safe mode blocked a server request: %s", url ? url : "");
        gd::cc::Retain(request);
        bool queued = gd::Queue([client, request] {
            try {
                Fail(client, request);
            } catch (...) {
                LOG("error while cancelling a server request");
            }
            gd::cc::Release(request);
        });
        if (!queued) gd::cc::Release(request);
        return;
    }
    oSend(client, request);
}

bool __fastcall hkSaveDict(void* dict, const char* path, bool flag) {
    gd::Busy busy;
    if (locked.load()) {
        LOG("safe mode kept the game from saving %s", path ? path : "");
        return false;
    }
    return oSaveDict(dict, path, flag);
}

bool __fastcall hkManagerSave(void* manager, bool async) {
    gd::Busy busy;
    if (!locked.load() && levelReady) {
        void* pl = layer.load();
        if (pl && cheated.load()) Settle(pl);
    }
    if (locked.load()) {
        LOG("safe mode kept the game from saving");
        return false;
    }
    return oManagerSave(manager, async);
}

bool __fastcall hkWriteAtomic(const void* path, const void* data) {
    gd::Busy busy;
    if (locked.load()) {
        LOG("safe mode kept the game from writing a checkpoint file");
        return false;
    }
    return oWriteAtomic(path, data);
}

template <class F>
bool HookOne(uint32_t rva, F detour, F* orig, const char* name) {
    return gd::HookExe(rva, (void*)detour, (void**)orig, name);
}

}

bool Install() {
    HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
    crtMalloc = ucrt ? (MallocFn)GetProcAddress(ucrt, "malloc") : nullptr;
    getUrl = (UrlFn)gd::ExtFn("?getUrl@CCHttpRequest@extension@cocos2d@@QEAAPEBDXZ");
    respCtor = (RespCtorFn)gd::ExtFn("??0CCHttpResponse@extension@cocos2d@@QEAA@PEAVCCHttpRequest@12@@Z");
    respSucceed = (RespBoolFn)gd::ExtFn("?setSucceed@CCHttpResponse@extension@cocos2d@@QEAAX_N@Z");
    respCode = (RespCodeFn)gd::ExtFn("?setResponseCode@CCHttpResponse@extension@cocos2d@@QEAAXJ@Z");
    void* send = gd::ExtFn("?send@CCHttpClient@extension@cocos2d@@QEAAXPEAVCCHttpRequest@23@@Z");
    void* save = gd::CocosFn("?saveRootSubDictToCompressedFile@DS_Dictionary@@QEAA_NPEBD_N@Z");
    if (!crtMalloc || !getUrl || !respCtor || !respSucceed || !respCode) {
        LOG("safe mode: the request functions were not found");
        return false;
    }
    if (!gd::Hook(send, (void*)&hkSend, (void**)&oSend, "server requests")) return false;
    if (!gd::Hook(save, (void*)&hkSaveDict, (void**)&oSaveDict, "save file")) return false;
    if (!gd::Supported()) {
        LOG("safe mode: this game version is unknown, the level cheats stay off");
        return false;
    }
    getStat = (GetStatFn)gd::Exe(gdx::rva::GSM_getStat);
    setStat = gd::IsFunctionStart(kSetStat) ? (SetStatFn)gd::Exe(kSetStat) : nullptr;
    namespace r = gdx::rva;
    bool ok = setStat != nullptr;
    ok &= HookOne(kManagerSave, &hkManagerSave, &oManagerSave, "game save");
    ok &= HookOne(kWriteAtomic, &hkWriteAtomic, &oWriteAtomic, "checkpoint file");
    ok &= HookOne(r::PlayLayer_init, &hkInit, &oInit, "level start");
    ok &= HookOne(r::PlayLayer_dtor, &hkDtor, &oDtor, "level close");
    ok &= HookOne(r::PlayLayer_resetLevel, &hkReset, &oReset, "attempt start");
    ok &= HookOne(r::PlayLayer_levelComplete, &hkComplete, &oComplete, "level complete");
    ok &= HookOne(r::PlayLayer_onQuit, &hkQuit, &oQuit, "level quit");
    ok &= HookOne(r::PlayLayer_writePersistentSave, &hkPersist, &oPersist, "checkpoint save");
    ok &= HookOne(r::PlayLayer_processCheckpoints, &hkProcessCheckpoints, &oProcessCheckpoints, "checkpoints");
    ok &= HookOne(r::EndLevelLayer_customSetup, &hkEndSetup, &oEndSetup, "end screen");
    ok &= HookOne(kCommitPlaytime, &hkCommitPlaytime, &oCommitPlaytime, "play time");
    ok &= HookOne(r::GJGameLevel_savePercentage, &hkSavePct, &oSavePct, "save percent");
    ok &= HookOne(r::GSM_incrementStat, &hkIncStat, &oIncStat, "stats");
    ok &= HookOne(r::GSM_awardCurrencyForLevel, &hkAwardCurrency, &oAwardCurrency, "orbs");
    ok &= HookOne(r::GSM_awardDiamondsForLevel, &hkAwardDiamonds, &oAwardDiamonds, "diamonds");
    ok &= HookOne(r::GSM_awardSecretKey, &hkAwardKey, &oAwardKey, "keys");
    ok &= HookOne(r::GSM_completedLevel, &hkCompleted, &oCompleted, "completed level");
    ok &= HookOne(r::GSM_completedDemonLevel, &hkCompletedDemon, &oCompletedDemon, "completed demon");
    ok &= HookOne(r::GSM_completedStarLevel, &hkCompletedStar, &oCompletedStar, "completed star level");
    ok &= HookOne(r::GSM_completedMapPack, &hkCompletedPack, &oCompletedPack, "completed map pack");
    ok &= HookOne(r::GSM_incrementChallenge, &hkChallenge, &oChallenge, "quests");
    ok &= HookOne(r::GSM_storeSecretCoin, &hkSecretCoin, &oSecretCoin, "secret coins");
    ok &= HookOne(r::GSM_storeUserCoin, &hkUserCoin, &oUserCoin, "user coins");
    if (!ok) {
        LOG("safe mode: some level functions could not be hooked, the level cheats stay off");
        return false;
    }
    levelReady = true;
    ready = true;
    LOG("safe mode ready");
    return true;
}

bool Ready() { return ready && levelReady; }

void Tick() {
    if (!levelReady) return;
    Track();
    void* pl = layer.load();
    if (pl && !cheated.load() && ActiveNow() && !AttemptOver(pl)) MarkCheated("a cheat is on");
    if (pl && cheated.load() && !tainted.load() && !quitting.load()) tainted = true;
}

void Frame() {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    ui::Theme& t = ui::T();
    if (locked.load()) {
        const char* text = "Safe mode stopped all saving. Close Geometry Dash to keep your old save.";
        ImVec2 sz = ui::Measure(t.semibold, 18, text);
        ImVec2 p((ds.x - sz.x) * 0.5f, 24);
        dl->AddRectFilled(ImVec2(p.x - 14, p.y - 10), ImVec2(p.x + sz.x + 14, p.y + sz.y + 10), IM_COL32(150, 20, 30, 235));
        ui::Text(dl, t.semibold, 18, p, IM_COL32(255, 255, 255, 255), text);
        return;
    }
    if (!layer.load() || !cheated.load()) return;
    const char* text = "Safe mode: this attempt won't count";
    ImVec2 sz = ui::Measure(t.medium, 14, text);
    ImVec2 p(14, ds.y - sz.y - 14);
    dl->AddRectFilled(ImVec2(p.x - 8, p.y - 5), ImVec2(p.x + sz.x + 8, p.y + sz.y + 5), IM_COL32(0, 0, 0, 150));
    ui::Text(dl, t.medium, 14, p, IM_COL32(255, 210, 120, 240), text);
}

void Off() {}

bool InLevel() { return layer.load() != nullptr; }

bool Cheated() { return layer.load() && cheated.load(); }

bool Holding() { return locked.load() || (levelReady && layer.load() && tainted.load()); }

bool Locked() { return locked.load(); }

void MarkCheated(const char* reason) {
    void* pl = layer.load();
    if (!pl) return;
    if (!cheated.exchange(true)) {
        SnapProgress(pl);
        why = reason ? reason : "";
        LOG("this attempt won't count: %s", why.c_str());
    }
    if (!quitting.load()) tainted = true;
}

void ArmComplete() {
    if (layer.load()) completeArmed = true;
}

const char* Status(bool& good) {
    if (!gd::Supported()) {
        good = false;
        return "Unsupported game version";
    }
    if (locked.load()) {
        good = false;
        return "Saving stopped, close the game";
    }
    if (!layer.load()) {
        good = true;
        return "In the menus";
    }
    if (cheated.load()) {
        good = false;
        return "Safe mode: attempt won't count";
    }
    good = true;
    return "Playing, attempt counts";
}

Protect::Protect() {
    void* pl = layer.load();
    if (!levelReady || !pl || !cheated.load()) return;
    obj = pl;
    was = gd::F<uint8_t>(pl, gdx::off::GBGL_isTestMode) != 0;
    wasTrack = gd::F<uint8_t>(pl, kTrackOff) != 0;
    gd::F<uint8_t>(pl, gdx::off::GBGL_isTestMode) = 1;
    gd::F<uint8_t>(pl, kTrackOff) = 1;
    active = true;
}

Protect::~Protect() {
    if (!active) return;
    if (obj && layer.load() == obj) {
        gd::F<uint8_t>(obj, gdx::off::GBGL_isTestMode) = was ? 1 : 0;
        gd::F<uint8_t>(obj, kTrackOff) = wasTrack ? 1 : 0;
    }
}

}
