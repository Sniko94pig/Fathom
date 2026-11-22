#include "ingame/gd.h"
#include "ingame/gdx.h"
#include "ingame/log.h"
#include "ingame/modules.h"

namespace player {

namespace {

using DestroyFn = void(__fastcall*)(void*, void*, void*);
using BoolFn = bool(__fastcall*)(void*);
using EndFn = void(__fastcall*)(void*, const void*);
using PlatformerEndFn = void(__fastcall*)(void*, const void*, bool);
using PosFn = const void*(__fastcall*)(void*);
using VirtPosFn = const void*(__fastcall*)(void*, void*);
using EnableFn = void(__fastcall*)(void*, bool);
using UpdateEndFn = void(__fastcall*)(void*, bool);
using SectionFn = void(__fastcall*)(void*, void*);
using LayerFn = void(__fastcall*)(void*);

constexpr uint32_t kPlayEndAnimation = 0x50A2A0;
constexpr uint32_t kPlayPlatformerEnd = 0x50ACE0;
constexpr uint32_t kEditorShouldDrawHitboxes = 0x406B80;
constexpr uint32_t kToggleDebugDraw = 0x505E40;
constexpr uint32_t kUpdateEndPos = 0x2095A0;
constexpr uint32_t kRemoveFromSection = 0x32F9F0;
constexpr uintptr_t kAddToSectionSlot = 0x4A8;
constexpr uintptr_t kSectionYFactor = 0x37A4;
constexpr uintptr_t kStarted = 0x3358;
constexpr uintptr_t kPaused = 0x3C47;
constexpr uintptr_t kEndPortal = 0x3320;
constexpr uintptr_t kEndAnimationStarted = 0x3670;
constexpr uintptr_t kEndReached = 0x3A28;
constexpr uintptr_t kUiLayer = 0x3640;
constexpr uintptr_t kUiPauseButton = 0x238;
constexpr uintptr_t kPlayerDead = 0x9C8;
constexpr uintptr_t kPortalPosSlot = 0x680;
constexpr uintptr_t kEnableSlot = 0x3A8;

DestroyFn oDestroy = nullptr;
BoolFn oShouldDraw = nullptr;
BoolFn oShouldTrail = nullptr;
BoolFn oShouldRecordTrail = nullptr;
BoolFn oEditorShouldDraw = nullptr;
EndFn playEnd = nullptr;
PlatformerEndFn playPlatformerEnd = nullptr;
PosFn nodePosition = nullptr;
LayerFn toggleDebugDraw = nullptr;
UpdateEndFn updateEndPos = nullptr;
SectionFn removeFromSection = nullptr;
bool shownHitboxes = false;

bool noclipOk = false;
bool hitboxOk = false;
bool completeOk = false;

bool IsAnticheat(void* pl, void* object) {
    if (!object) return false;
    void* spike = gd::F<void*>(pl, gdx::off::GBGL_anticheatSpike);
    if (!spike) return false;
    return object == spike || gd::F<int>(object, gdx::off::GO_uniqueID) == gd::F<int>(spike, gdx::off::GO_uniqueID);
}

void __fastcall hkDestroy(void* pl, void* who, void* object) {
    gd::Busy busy;
    if (gd::C().noclip.load() && !IsAnticheat(pl, object)) {
        safe::MarkCheated("noclip");
        return;
    }
    safe::Protect p;
    oDestroy(pl, who, object);
}

bool __fastcall hkShouldDraw(void* pl) {
    gd::Busy busy;
    if (gd::C().hitboxes.load()) return true;
    return oShouldDraw(pl);
}

bool __fastcall hkShouldTrail(void* pl) {
    gd::Busy busy;
    if (gd::C().hitboxes.load()) return false;
    return oShouldTrail(pl);
}

bool __fastcall hkShouldRecordTrail(void* pl) {
    gd::Busy busy;
    if (gd::C().hitboxes.load()) return false;
    return oShouldRecordTrail(pl);
}

bool __fastcall hkEditorShouldDraw(void* layer) {
    gd::Busy busy;
    if (gd::C().hitboxes.load()) return true;
    return oEditorShouldDraw(layer);
}

void DisablePause(void* pl) {
    void* ui = gd::F<void*>(pl, kUiLayer);
    void* button = ui ? gd::F<void*>(ui, kUiPauseButton) : nullptr;
    if (!button) return;
    void** vt = *(void***)button;
    ((EnableFn)vt[kEnableSlot / 8])(button, false);
}

void Finish() {
    void* pl = gdx::PlayLayer();
    if (!pl) return;
    void* p1 = gd::F<void*>(pl, gdx::off::GBGL_player1);
    if (!p1 || gd::F<uint8_t>(p1, kPlayerDead) || gd::F<uint8_t>(pl, kEndAnimationStarted) || gd::F<uint8_t>(pl, kEndReached)) return;
    safe::MarkCheated("Complete level");
    safe::ArmComplete();
    if (gd::F<uint8_t>(pl, gdx::off::GBGL_isPlatformer)) {
        float pos[2] = {0, 0};
        const float* cur = nodePosition ? (const float*)nodePosition(p1) : nullptr;
        if (cur) {
            pos[0] = cur[0];
            pos[1] = cur[1];
        }
        gd::F<uint8_t>(pl, kEndAnimationStarted) = 1;
        DisablePause(pl);
        LOG("completing the platformer level");
        playPlatformerEnd(pl, pos, true);
        return;
    }
    void* portal = gd::F<void*>(pl, kEndPortal);
    if (!portal) return;
    gd::F<uint8_t>(pl, kEndReached) = 1;
    if (gd::F<float>(pl, kSectionYFactor) != 0.0f && updateEndPos && removeFromSection) {
        updateEndPos(portal, false);
        removeFromSection(pl, portal);
        void** lvt = *(void***)pl;
        ((SectionFn)lvt[kAddToSectionSlot / 8])(pl, portal);
    }
    float tmp[2] = {0, 0};
    void** vt = *(void***)portal;
    const void* pos = ((VirtPosFn)vt[kPortalPosSlot / 8])(portal, tmp);
    DisablePause(pl);
    LOG("completing the level");
    playEnd(pl, pos ? pos : tmp);
}

}

bool Install() {
    if (!gd::Supported()) return false;
    namespace r = gdx::rva;
    noclipOk = gd::HookExe(r::PlayLayer_destroyPlayer, (void*)&hkDestroy, (void**)&oDestroy, "noclip");
    hitboxOk = gd::HookExe(r::PlayLayer_shouldDrawDebugHitboxes, (void*)&hkShouldDraw, (void**)&oShouldDraw, "hitboxes");
    if (hitboxOk) {
        gd::HookExe(r::PlayLayer_shouldDrawHitboxTrail, (void*)&hkShouldTrail, (void**)&oShouldTrail, "hitbox trail");
        gd::HookExe(r::PlayLayer_shouldRecordHitboxTrail, (void*)&hkShouldRecordTrail, (void**)&oShouldRecordTrail, "hitbox trail record");
        gd::HookExe(kEditorShouldDrawHitboxes, (void*)&hkEditorShouldDraw, (void**)&oEditorShouldDraw, "editor hitboxes");
    }
    playEnd = gd::IsFunctionStart(kPlayEndAnimation) ? (EndFn)gd::Exe(kPlayEndAnimation) : nullptr;
    playPlatformerEnd = gd::IsFunctionStart(kPlayPlatformerEnd) ? (PlatformerEndFn)gd::Exe(kPlayPlatformerEnd) : nullptr;
    nodePosition = (PosFn)gd::CocosFn("?getPosition@CCNode@cocos2d@@UEAAAEBVCCPoint@2@XZ");
    toggleDebugDraw = gd::IsFunctionStart(kToggleDebugDraw) ? (LayerFn)gd::Exe(kToggleDebugDraw) : nullptr;
    updateEndPos = gd::IsFunctionStart(kUpdateEndPos) ? (UpdateEndFn)gd::Exe(kUpdateEndPos) : nullptr;
    removeFromSection = gd::IsFunctionStart(kRemoveFromSection) ? (SectionFn)gd::Exe(kRemoveFromSection) : nullptr;
    completeOk = playEnd && playPlatformerEnd && nodePosition && updateEndPos && removeFromSection;
    if (!jumps::Install()) LOG("unlimited jumps is not available");
    return noclipOk && hitboxOk && completeOk;
}

bool NoclipReady() { return noclipOk; }

bool JumpsReady() { return jumps::Ready(); }

bool HitboxReady() { return hitboxOk; }

bool CompleteReady() { return completeOk; }

std::string Complete() {
    if (!completeOk) return "Complete level is not available";
    void* pl = gdx::PlayLayer();
    if (!pl) return "Open a level first";
    void* p1 = gd::F<void*>(pl, gdx::off::GBGL_player1);
    if (gd::F<uint8_t>(pl, kEndAnimationStarted) || gd::F<uint8_t>(pl, kEndReached)) return "The level is already finishing";
    if (!gd::F<uint8_t>(pl, kStarted)) return "Wait until the level has started";
    if (gd::F<uint8_t>(pl, kPaused)) return "Resume the level first, then try again";
    if (!p1 || gd::F<uint8_t>(p1, kPlayerDead)) return "Wait until you respawn, then try again";
    gd::Queue([] { Finish(); });
    return "Level completed. It won't count";
}

void RefreshHitboxes() {
    void* pl = gdx::PlayLayer();
    if (pl && toggleDebugDraw) toggleDebugDraw(pl);
}

void Tick() {
    jumps::Tick();
    bool want = gd::C().hitboxes.load();
    if (hitboxOk && want != shownHitboxes) {
        shownHitboxes = want;
        RefreshHitboxes();
    }
}

void Off() {
    gd::C().noclip = false;
    gd::C().hitboxes = false;
    if (hitboxOk && shownHitboxes) {
        shownHitboxes = false;
        RefreshHitboxes();
    }
    jumps::Off();
}

}
