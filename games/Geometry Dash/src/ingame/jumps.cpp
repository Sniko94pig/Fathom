#include "ingame/gd.h"
#include "ingame/gdx.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include <intrin.h>
#include <cstdint>
#include <cstring>

namespace jumps {

namespace {

using PushFn = bool(__fastcall*)(void*, int);

constexpr uint32_t kPushButton = 0x4F6200;
constexpr uint32_t kPushCallP1 = 0x33C6D7;
constexpr uint32_t kPushCallP2 = 0x33C713;
constexpr uint32_t kCallSize = 5;
constexpr int kJumpButton = 1;

constexpr uintptr_t kJumpHeld = 0x98D;
constexpr uintptr_t kJumpFresh = 0x98E;
constexpr uintptr_t kShip = 0x9C1;
constexpr uintptr_t kUfo = 0x9C2;
constexpr uintptr_t kWave = 0x9C4;
constexpr uintptr_t kDead = 0x9C8;
constexpr uintptr_t kOnGround = 0x9C9;
constexpr uintptr_t kSwing = 0x9CC;
constexpr uintptr_t kDashing = 0x9EC;
constexpr uintptr_t kRising = 0xA24;
constexpr uintptr_t kLocked = 0xA32;
constexpr uintptr_t kNoControls = 0xA33;
constexpr uintptr_t kRings = 0xA40;
constexpr uintptr_t kGameLayer = 0xC28;
constexpr uintptr_t kFallStart = 0xC44;

PushFn oPush = nullptr;
uintptr_t fromP1 = 0;
uintptr_t fromP2 = 0;
bool ready = false;

bool Flag(const void* p, uintptr_t off) { return gd::F<uint8_t>(p, off) != 0; }

bool CallsPush(uint32_t at) {
    const uint8_t* code = static_cast<const uint8_t*>(gd::Exe(at));
    if (!code || code[0] != 0xE8) return false;
    int32_t rel = 0;
    std::memcpy(&rel, code + 1, sizeof(rel));
    return static_cast<int64_t>(at) + kCallSize + rel == static_cast<int64_t>(kPushButton);
}

bool InGame(void* p) {
    void* layer = gd::F<void*>(p, kGameLayer);
    if (!layer) return false;
    if (gd::F<void*>(layer, gdx::off::GBGL_player1) != p && gd::F<void*>(layer, gdx::off::GBGL_player2) != p) return false;
    return gd::cc::IsA(layer, ".?AVPlayLayer@@") || gd::cc::IsA(layer, ".?AVLevelEditorLayer@@");
}

bool CanJump(void* p) {
    if (Flag(p, kOnGround) || Flag(p, kDead) || Flag(p, kLocked) || Flag(p, kNoControls) || Flag(p, kDashing)) return false;
    if (Flag(p, kShip) || Flag(p, kUfo) || Flag(p, kWave) || Flag(p, kSwing)) return false;
    return gd::cc::Count(gd::F<void*>(p, kRings)) == 0 && InGame(p);
}

bool __fastcall hkPush(void* p, int button) {
    gd::Busy busy;
    uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    if (!gd::C().jumps.load() || button != kJumpButton || !p || (from != fromP1 && from != fromP2) || !CanJump(p))
        return oPush(p, button);
    uint32_t fall = gd::F<uint32_t>(p, kFallStart);
    bool rising = Flag(p, kRising);
    gd::F<uint8_t>(p, kOnGround) = 1;
    bool result = oPush(p, button);
    if (Flag(p, kJumpHeld) && Flag(p, kJumpFresh)) {
        gd::F<uint8_t>(p, kOnGround) = 0;
        if (!rising) gd::F<uint32_t>(p, kFallStart) = fall;
    }
    return result;
}

}

bool Install() {
    if (!gd::Supported()) return false;
    if (!CallsPush(kPushCallP1) || !CallsPush(kPushCallP2)) {
        LOG("unlimited jumps: the jump button code is not where it was expected");
        return false;
    }
    fromP1 = reinterpret_cast<uintptr_t>(gd::Exe(kPushCallP1 + kCallSize));
    fromP2 = reinterpret_cast<uintptr_t>(gd::Exe(kPushCallP2 + kCallSize));
    ready = gd::HookExe(kPushButton, (void*)&hkPush, (void**)&oPush, "unlimited jumps");
    return ready;
}

bool Ready() { return ready; }

void Tick() {}

void Off() { gd::C().jumps = false; }

}
