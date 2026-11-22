#include "ingame/gd.h"
#include "ingame/gdx.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <random>
#include <string>
#include <vector>

namespace icons {

namespace {

using UnlockFn = bool(__fastcall*)(void*, int, int);
using DictFn = void(__fastcall*)(void*, void*);
using SelfFn = void(__fastcall*)(void*);
using CountFn = int(__fastcall*)(void*, int);
using IntFn = void(__fastcall*)(void*, int);
using FrameFn = void(__fastcall*)(void*, int, int);
using ScaleFn = void(__fastcall*)(void*, float);
using ChildrenFn = void*(__fastcall*)(void*);

constexpr uint32_t kCheckUsedIcons = 0x27F960;
constexpr uint32_t kDataLoaded = 0x2826B0;
constexpr uint32_t kCountForType = 0x27D010;
constexpr uint32_t kGarageSelectTab = 0x37E8B0;
constexpr uint32_t kGaragePlayerColors = 0x380480;
constexpr uint32_t kSimplePlayerFrame = 0x382A80;
constexpr uint32_t kColorPageMode = 0xA20E0;
constexpr uint32_t kColorPageIcons = 0xA2E80;

constexpr uint32_t kIconCallers[] = {0x37F295, 0x37FBD9, 0xA1EBC};
constexpr uint32_t kColorCallers[] = {0xA22D3, 0xA26F8};

constexpr uintptr_t kGlow = 0x3B0;
constexpr uintptr_t kIconType = 0x3B4;
constexpr uintptr_t kGaragePlayer = 0x1D8;
constexpr uintptr_t kGarageClosing = 0x200;
constexpr uintptr_t kGarageTab = 0x204;
constexpr uintptr_t kColorPageModeField = 0x298;
constexpr uintptr_t kSetScaleSlot = 0xA0;

constexpr const char* kGarageClass = ".?AVGJGarageLayer@@";
constexpr const char* kColorPageClass = ".?AVCharacterColorPage@@";

constexpr int kDeathEffect = 98;
constexpr int kSpecial = 99;
constexpr int kShipFire = 101;
constexpr int kJetpack = 8;
constexpr int kColor1 = 2;
constexpr int kColor2 = 3;
constexpr int kGlowUnlockId = 2;
constexpr int kColorLast = 106;
constexpr int kRandMax = 1000;
constexpr int64_t kPickMax = 100000;
constexpr float kJetpackScale = 1.5f;
constexpr float kPlayerScale = 1.6f;
constexpr ULONGLONG kSaveDelay = 1000;
constexpr int kLogLimit = 10;

struct Slot {
    uintptr_t off;
    bool color;
    int type;
    const char* key;
};

constexpr Slot kSlots[] = {
    {0x2EC, false, 0, "cube"},
    {0x2F8, false, 1, "ship"},
    {0x304, false, 2, "ball"},
    {0x310, false, 3, "ufo"},
    {0x31C, false, 4, "wave"},
    {0x328, false, 5, "robot"},
    {0x334, false, 6, "spider"},
    {0x340, false, 7, "swing"},
    {0x34C, true, kColor1, "color1"},
    {0x358, true, kColor2, "color2"},
    {0x364, true, kColor2, "glow_color"},
    {0x370, false, kSpecial, "trail"},
    {0x37C, false, kShipFire, "ship_fire"},
    {0x388, false, kDeathEffect, "death_effect"},
    {0x394, false, kJetpack, "jetpack"},
};

constexpr int kSlotCount = (int)(sizeof(kSlots) / sizeof(kSlots[0]));
constexpr int kFormSlot[] = {0, 1, 2, 3, 4, 5, 6, 7, 14};

struct Picks {
    int value[kSlotCount] = {};
    bool has[kSlotCount] = {};
    bool glow = false;
    bool hasGlow = false;
    int type = 0;
    bool hasType = false;
};

UnlockFn oIconUnlocked = nullptr;
UnlockFn oColorUnlocked = nullptr;
DictFn oEncode = nullptr;
DictFn oDataLoaded = nullptr;
SelfFn oCheckUsed = nullptr;
SelfFn oUpdateScore = nullptr;

CountFn countForType = nullptr;
IntFn garageSelectTab = nullptr;
SelfFn garagePlayerColors = nullptr;
FrameFn playerFrame = nullptr;
IntFn colorPageMode = nullptr;
SelfFn colorPageIcons = nullptr;
ChildrenFn nodeChildren = nullptr;

std::atomic<bool> ready{false};
std::atomic<bool> active{false};
std::atomic<bool> faking{false};
std::atomic<bool> uploadSwap{false};
std::atomic<DWORD> mainThread{0};
std::atomic<void*> tracked{nullptr};
thread_local int depth = 0;

gd::Seed realSeed[kSlotCount] = {};
int realVal[kSlotCount] = {};
int shownVal[kSlotCount] = {};
uint8_t realGlow = 0;
uint8_t shownGlow = 0;
int realType = 0;
int shownType = 0;

Picks picks;
bool picksLoaded = false;
bool dirty = false;
ULONGLONG dirtyAt = 0;
bool refresh = false;
int saveLogs = 0;
int uploadLogs = 0;
std::atomic<uint32_t> rollState{0x9E3779B9u};

int Roll() {
    uint32_t x = rollState.fetch_add(0x9E3779B9u);
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return static_cast<int>(x % static_cast<uint32_t>(kRandMax + 1));
}

std::wstring PicksPath() { return gd::Folder() + L"\\icons.json"; }

int FormOf(int type) { return type >= 0 && type <= kJetpack ? kFormSlot[type] : -1; }

bool KitIcon(int type) { return (type >= 0 && type <= kJetpack) || type == kDeathEffect || type == kSpecial || type == kShipFire; }

bool KitColor(int type) { return type == kColor1 || type == kColor2; }

template <size_t N>
bool CalledFrom(const void* ret, const uint32_t (&sites)[N]) {
    uintptr_t at = reinterpret_cast<uintptr_t>(ret);
    uintptr_t base = gd::ExeBase();
    if (at < base) return false;
    for (uint32_t s : sites)
        if (at - base == s) return true;
    return false;
}

gd::Seed ReadSeed(void* gm, int i) {
    uintptr_t o = kSlots[i].off;
    return {gd::F<int>(gm, o), gd::F<int>(gm, o + 4), gd::F<int>(gm, o + 8)};
}

int ReadValue(void* gm, int i) { return gd::F<int>(gm, kSlots[i].off) - gd::F<int>(gm, kSlots[i].off + 4); }

void WriteSeed(void* gm, int i, const gd::Seed& s) {
    uintptr_t o = kSlots[i].off;
    gd::F<int>(gm, o + 8) = s.c;
    gd::F<int>(gm, o + 4) = s.b;
    gd::F<int>(gm, o) = s.a;
}

void WriteValue(void* gm, int i, int v) {
    int r = Roll();
    WriteSeed(gm, i, gd::Seed{r + v, r, v});
}

bool Owned(void* gm, int i, int v) {
    const Slot& s = kSlots[i];
    return s.color ? oColorUnlocked(gm, v, s.type) : oIconUnlocked(gm, v, s.type);
}

bool GlowOwned(void* gm) { return oIconUnlocked(gm, kGlowUnlockId, kSpecial); }

bool Valid(void* gm, int i, int v) {
    const Slot& s = kSlots[i];
    if (s.color) return v >= 0 && v <= kColorLast;
    int n = countForType ? countForType(gm, s.type) : 0;
    return v >= 1 && v <= n;
}

bool Overridden() {
    for (int i = 0; i < kSlotCount; ++i)
        if (shownVal[i] != realVal[i]) return true;
    return shownGlow != realGlow || shownType != realType;
}

bool Same(const Picks& a, const Picks& b) {
    for (int i = 0; i < kSlotCount; ++i)
        if (a.has[i] != b.has[i] || (a.has[i] && a.value[i] != b.value[i])) return false;
    if (a.hasGlow != b.hasGlow || (a.hasGlow && a.glow != b.glow)) return false;
    return a.hasType == b.hasType && (!a.hasType || a.type == b.type);
}

void LoadPicks() {
    if (picksLoaded) return;
    picksLoaded = true;
    if (gd::Folder().empty()) return;
    try {
        std::ifstream in(PicksPath(), std::ios::binary);
        if (!in) return;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) return;
        for (int i = 0; i < kSlotCount; ++i) {
            auto it = j.find(kSlots[i].key);
            if (it == j.end() || !it->is_number_integer()) continue;
            int64_t v = it->get<int64_t>();
            if (v < 0 || v > kPickMax) continue;
            picks.value[i] = (int)v;
            picks.has[i] = true;
        }
        auto glow = j.find("glow");
        if (glow != j.end() && glow->is_boolean()) {
            picks.glow = glow->get<bool>();
            picks.hasGlow = true;
        }
        auto type = j.find("icon_type");
        if (type != j.end() && type->is_number_integer()) {
            int64_t v = type->get<int64_t>();
            if (v >= 0 && v <= kJetpack) {
                picks.type = (int)v;
                picks.hasType = true;
            }
        }
    } catch (...) {
        picks = Picks{};
        LOG("unlock all icons: icons.json could not be read, starting with no picks");
    }
}

void SavePicks() {
    dirty = false;
    if (gd::Folder().empty()) return;
    try {
        nlohmann::json j = nlohmann::json::object();
        for (int i = 0; i < kSlotCount; ++i)
            if (picks.has[i]) j[kSlots[i].key] = picks.value[i];
        if (picks.hasGlow) j["glow"] = picks.glow;
        if (picks.hasType) j["icon_type"] = picks.type;
        std::wstring path = PicksPath();
        std::wstring tmp = path + L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return;
            out << j.dump(2);
            if (!out.good()) return;
        }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) LOG("unlock all icons: icons.json could not be saved");
    } catch (...) {
        LOG("unlock all icons: icons.json could not be saved");
    }
}

void Remember() {
    Picks now;
    for (int i = 0; i < kSlotCount; ++i)
        if (shownVal[i] != realVal[i]) {
            now.has[i] = true;
            now.value[i] = shownVal[i];
        }
    if (shownGlow != realGlow) {
        now.hasGlow = true;
        now.glow = shownGlow != 0;
    }
    if (shownType != realType) {
        now.hasType = true;
        now.type = shownType;
    }
    if (Same(now, picks)) return;
    picks = now;
    dirty = true;
    dirtyAt = GetTickCount64();
}

void Sync(void* gm) {
    bool changed[kSlotCount] = {};
    bool any = false;
    for (int i = 0; i < kSlotCount; ++i) {
        int v = ReadValue(gm, i);
        if (v == shownVal[i]) continue;
        if (Owned(gm, i, v)) {
            realSeed[i] = ReadSeed(gm, i);
            realVal[i] = v;
        }
        shownVal[i] = v;
        changed[i] = true;
        any = true;
    }
    uint8_t glow = gd::F<uint8_t>(gm, kGlow);
    if (glow != shownGlow) {
        if (!glow || GlowOwned(gm)) realGlow = glow;
        shownGlow = glow;
        any = true;
    }
    int type = gd::F<int>(gm, kIconType);
    int form = FormOf(type);
    if (type != shownType || (form >= 0 && changed[form])) {
        if (form < 0 || shownVal[form] == realVal[form]) realType = type;
        shownType = type;
        any = true;
    }
    if (any) Remember();
    faking = Overridden();
}

void PutReal(void* gm) {
    for (int i = 0; i < kSlotCount; ++i)
        if (shownVal[i] != realVal[i]) WriteSeed(gm, i, realSeed[i]);
    if (shownGlow != realGlow) gd::F<uint8_t>(gm, kGlow) = realGlow;
    if (shownType != realType) gd::F<int>(gm, kIconType) = realType;
}

void PutShown(void* gm) {
    for (int i = 0; i < kSlotCount; ++i)
        if (shownVal[i] != realVal[i]) WriteValue(gm, i, shownVal[i]);
    if (shownGlow != realGlow) gd::F<uint8_t>(gm, kGlow) = shownGlow;
    if (shownType != realType) gd::F<int>(gm, kIconType) = shownType;
    faking = Overridden();
}

void Capture(void* gm) {
    bool changed = false;
    for (int i = 0; i < kSlotCount; ++i) {
        bool had = shownVal[i] != realVal[i];
        int v = ReadValue(gm, i);
        if (v != realVal[i]) changed = true;
        realSeed[i] = ReadSeed(gm, i);
        realVal[i] = v;
        if (!had) shownVal[i] = v;
    }
    bool hadGlow = shownGlow != realGlow;
    uint8_t glow = gd::F<uint8_t>(gm, kGlow);
    if (glow != realGlow) changed = true;
    realGlow = glow;
    if (!hadGlow) shownGlow = glow;
    bool hadType = shownType != realType;
    int type = gd::F<int>(gm, kIconType);
    if (type != realType) changed = true;
    realType = type;
    if (!hadType) shownType = type;
    if (changed) Remember();
}

struct Scope {
    void* gm = nullptr;
    bool full = false;

    explicit Scope(void* self) {
        if (depth++ > 0 || !active.load() || !self || self != tracked.load()) return;
        gm = self;
        full = GetCurrentThreadId() == mainThread.load();
        if (full) Sync(gm);
        PutReal(gm);
    }

    ~Scope() {
        --depth;
        if (!gm) return;
        if (full) Capture(gm);
        PutShown(gm);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

bool Activate() {
    void* gm = gdx::GameManager();
    if (!gm) return false;
    LoadPicks();
    tracked = gm;
    for (int i = 0; i < kSlotCount; ++i) {
        realSeed[i] = ReadSeed(gm, i);
        realVal[i] = ReadValue(gm, i);
        shownVal[i] = realVal[i];
    }
    realGlow = shownGlow = gd::F<uint8_t>(gm, kGlow);
    realType = shownType = gd::F<int>(gm, kIconType);
    int applied = 0;
    for (int i = 0; i < kSlotCount; ++i) {
        int v = picks.value[i];
        if (!picks.has[i] || v == realVal[i] || !Valid(gm, i, v)) continue;
        WriteValue(gm, i, v);
        shownVal[i] = v;
        ++applied;
    }
    uint8_t glow = static_cast<uint8_t>(picks.glow ? 1 : 0);
    if (picks.hasGlow && glow != realGlow) {
        gd::F<uint8_t>(gm, kGlow) = glow;
        shownGlow = glow;
        ++applied;
    }
    int form = FormOf(picks.type);
    if (picks.hasType && picks.type != realType && form >= 0 && shownVal[form] != realVal[form]) {
        gd::F<int>(gm, kIconType) = picks.type;
        shownType = picks.type;
    }
    active = true;
    faking = Overridden();
    refresh = true;
    LOG("unlock all icons on, %d saved picks shown", applied);
    return true;
}

void Deactivate() {
    if (!active.load()) return;
    void* gm = tracked.load();
    if (gm && gm == gdx::GameManager()) {
        Sync(gm);
        for (int i = 0; i < kSlotCount; ++i)
            if (shownVal[i] != realVal[i]) {
                WriteSeed(gm, i, realSeed[i]);
                shownVal[i] = realVal[i];
            }
        if (shownGlow != realGlow) {
            gd::F<uint8_t>(gm, kGlow) = realGlow;
            shownGlow = realGlow;
        }
        if (shownType != realType) {
            gd::F<int>(gm, kIconType) = realType;
            shownType = realType;
        }
    }
    active = false;
    faking = false;
    refresh = true;
    if (dirty) SavePicks();
    LOG("unlock all icons off, your own icons are back");
}

void RefreshGarage(void* layer) {
    if (gd::F<uint8_t>(layer, kGarageClosing)) return;
    void* gm = gdx::GameManager();
    void* player = gd::F<void*>(layer, kGaragePlayer);
    if (gm && player) {
        int type = gd::F<int>(gm, kIconType);
        int form = FormOf(type);
        if (form >= 0 && playerFrame) {
            playerFrame(player, ReadValue(gm, form), type);
            void** vt = *reinterpret_cast<void***>(player);
            reinterpret_cast<ScaleFn>(vt[kSetScaleSlot / sizeof(void*)])(player, type == kJetpack ? kJetpackScale : kPlayerScale);
        }
        if (garagePlayerColors) garagePlayerColors(layer);
    }
    if (garageSelectTab) garageSelectTab(layer, gd::F<int>(layer, kGarageTab));
}

void RefreshColorPage(void* page) {
    if (colorPageMode) colorPageMode(page, gd::F<int>(page, kColorPageModeField));
    if (colorPageIcons) colorPageIcons(page);
}

void Collect(void* parent, const char* type, std::vector<void*>& out) {
    void* kids = parent ? nodeChildren(parent) : nullptr;
    unsigned n = gd::cc::Count(kids);
    for (unsigned i = 0; i < n; ++i) {
        void* node = gd::cc::At(kids, i);
        if (gd::cc::IsA(node, type)) out.push_back(node);
    }
}

void RefreshUi() {
    if (!nodeChildren) return;
    void* scene = gd::cc::RunningScene();
    if (!scene) return;
    std::vector<void*> garages, pages;
    Collect(scene, kGarageClass, garages);
    Collect(scene, kColorPageClass, pages);
    for (void* g : garages)
        if (!gd::F<uint8_t>(g, kGarageClosing)) Collect(g, kColorPageClass, pages);
    for (void* g : garages) RefreshGarage(g);
    for (void* p : pages) RefreshColorPage(p);
}

bool __fastcall hkIconUnlocked(void* gm, int id, int type) {
    gd::Busy busy;
    if (active.load() && depth == 0 && KitIcon(type) && CalledFrom(_ReturnAddress(), kIconCallers)) return true;
    return oIconUnlocked(gm, id, type);
}

bool __fastcall hkColorUnlocked(void* gm, int id, int type) {
    gd::Busy busy;
    if (active.load() && depth == 0 && KitColor(type) && CalledFrom(_ReturnAddress(), kColorCallers)) return true;
    return oColorUnlocked(gm, id, type);
}

void __fastcall hkEncode(void* gm, void* dict) {
    gd::Busy busy;
    {
        Scope real(gm);
        if (real.gm && faking.load() && saveLogs < kLogLimit) {
            ++saveLogs;
            LOG("unlock all icons: the save gets your own icons");
        }
        oEncode(gm, dict);
    }
    if (dirty && GetCurrentThreadId() == mainThread.load()) SavePicks();
}

void __fastcall hkUpdateScore(void* glm) {
    gd::Busy busy;
    Scope real(gdx::GameManager());
    if (real.gm && faking.load() && uploadLogs < kLogLimit) {
        ++uploadLogs;
        LOG("unlock all icons: the profile update gets your own icons");
    }
    oUpdateScore(glm);
}

void __fastcall hkCheckUsed(void* gm) {
    gd::Busy busy;
    Scope real(gm);
    oCheckUsed(gm);
}

void __fastcall hkDataLoaded(void* gm, void* dict) {
    gd::Busy busy;
    Scope real(gm);
    oDataLoaded(gm, dict);
}

template <class F>
bool HookOne(uint32_t rva, F detour, F* orig, const char* name) {
    return gd::HookExe(rva, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(orig), name);
}

template <class F>
F Fn(uint32_t rva) {
    return gd::IsFunctionStart(rva) ? reinterpret_cast<F>(gd::Exe(rva)) : nullptr;
}

}

bool Install() {
    if (!gd::Supported()) return false;
    namespace r = gdx::rva;
    countForType = Fn<CountFn>(kCountForType);
    garageSelectTab = Fn<IntFn>(kGarageSelectTab);
    garagePlayerColors = Fn<SelfFn>(kGaragePlayerColors);
    playerFrame = Fn<FrameFn>(kSimplePlayerFrame);
    colorPageMode = Fn<IntFn>(kColorPageMode);
    colorPageIcons = Fn<SelfFn>(kColorPageIcons);
    nodeChildren = reinterpret_cast<ChildrenFn>(gd::CocosFn("?getChildren@CCNode@cocos2d@@UEAAPEAVCCArray@2@XZ"));
    rollState = std::random_device{}();
    bool ok = HookOne(r::GM_isIconUnlocked, &hkIconUnlocked, &oIconUnlocked, "icon unlock check");
    ok &= HookOne(r::GM_isColorUnlocked, &hkColorUnlocked, &oColorUnlocked, "color unlock check");
    ok &= HookOne(r::GM_encodeDataTo, &hkEncode, &oEncode, "icons in the save");
    ok &= HookOne(kCheckUsedIcons, &hkCheckUsed, &oCheckUsed, "used icon check");
    ok &= HookOne(kDataLoaded, &hkDataLoaded, &oDataLoaded, "save load");
    uploadSwap = HookOne(r::GLM_updateUserScore, &hkUpdateScore, &oUpdateScore, "profile update");
    if (!ok) {
        LOG("unlock all icons: some game functions could not be hooked, it stays off");
        return false;
    }
    if (!uploadSwap.load()) {
        if (!safe::Ready()) {
            LOG("unlock all icons: the profile update can't be covered, it stays off");
            return false;
        }
        LOG("unlock all icons: profile updates are held while picked icons are shown");
    }
    if (!garageSelectTab || !garagePlayerColors || !playerFrame || !nodeChildren)
        LOG("unlock all icons: the icon kit won't redraw by itself when the switch flips");
    if (!countForType) LOG("unlock all icons: saved icon picks can't be checked, only saved colors come back");
    ready = true;
    LOG("unlock all icons ready");
    return true;
}

bool Ready() { return ready.load(); }

void Set(bool on) {
    if (!ready.load()) return;
    mainThread = GetCurrentThreadId();
    if (on) {
        if (!active.load()) Activate();
    } else {
        Deactivate();
    }
}

void Tick() {
    if (!ready.load()) return;
    mainThread = GetCurrentThreadId();
    bool want = gd::C().icons.load();
    if (active.load()) {
        void* gm = gdx::GameManager();
        if (!want) {
            Deactivate();
        } else if (!gm) {
            active = false;
            faking = false;
        } else if (gm != tracked.load()) {
            Activate();
        } else {
            Sync(gm);
        }
    } else if (want) {
        Activate();
    }
    if (dirty && GetTickCount64() - dirtyAt >= kSaveDelay) SavePicks();
    if (refresh) {
        refresh = false;
        RefreshUi();
    }
}

void Off() {
    gd::C().icons = false;
    if (!ready.load()) return;
    Deactivate();
    if (dirty) SavePicks();
    if (refresh) {
        refresh = false;
        RefreshUi();
    }
}

bool BlockUpload(const char* url) {
    if (uploadSwap.load() || !faking.load() || !url) return false;
    return std::strstr(url, "updateGJUserScore") != nullptr;
}

}
