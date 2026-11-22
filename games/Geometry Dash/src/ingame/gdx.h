#pragma once
#include "ingame/gd.h"
#include <cstdint>

namespace gdx {

namespace rva {

constexpr uint32_t GameManagerInstance = 0x90BFD0;
constexpr uint32_t GameStatsManagerInstance = 0x90BFE0;

constexpr uint32_t PlayLayer_init = 0x5039E0;
constexpr uint32_t PlayLayer_dtor = 0x503640;
constexpr uint32_t PlayLayer_resetLevel = 0x518C00;
constexpr uint32_t PlayLayer_levelComplete = 0x5061B0;
constexpr uint32_t PlayLayer_destroyPlayer = 0x512390;
constexpr uint32_t PlayLayer_onQuit = 0x51B0B0;
constexpr uint32_t PlayLayer_writePersistentSave = 0x517F10;
constexpr uint32_t PlayLayer_processCheckpoints = 0x513890;
constexpr uint32_t PlayLayer_shouldDrawDebugHitboxes = 0x505BC0;
constexpr uint32_t PlayLayer_shouldDrawHitboxTrail = 0x505BF0;
constexpr uint32_t PlayLayer_shouldRecordHitboxTrail = 0x505C20;
constexpr uint32_t EndLevelLayer_customSetup = 0x203D90;

constexpr uint32_t GJGameLevel_savePercentage = 0x2641B0;

constexpr uint32_t GSM_getStat = 0x2DB160;
constexpr uint32_t GSM_incrementStat = 0x2DA460;
constexpr uint32_t GSM_awardCurrencyForLevel = 0x2E4380;
constexpr uint32_t GSM_awardDiamondsForLevel = 0x2E4B40;
constexpr uint32_t GSM_awardSecretKey = 0x2F65E0;
constexpr uint32_t GSM_completedLevel = 0x2E1C00;
constexpr uint32_t GSM_completedDemonLevel = 0x2E1D50;
constexpr uint32_t GSM_completedStarLevel = 0x2E1FE0;
constexpr uint32_t GSM_completedMapPack = 0x2E26F0;
constexpr uint32_t GSM_incrementChallenge = 0x2E5AE0;
constexpr uint32_t GSM_storeSecretCoin = 0x2E3980;
constexpr uint32_t GSM_storeUserCoin = 0x2E3700;

constexpr uint32_t GM_isIconUnlocked = 0x2773D0;
constexpr uint32_t GM_isColorUnlocked = 0x2777A0;
constexpr uint32_t GM_encodeDataTo = 0x285900;
constexpr uint32_t GLM_updateUserScore = 0x236270;

}

namespace off {

constexpr uintptr_t GM_playLayer = 0x230;

constexpr uintptr_t GBGL_level = 0x878;
constexpr uintptr_t GBGL_player1 = 0xDB8;
constexpr uintptr_t GBGL_player2 = 0xDC0;
constexpr uintptr_t GBGL_isPlatformer = 0x317E;
constexpr uintptr_t GBGL_isPracticeMode = 0x32E0;
constexpr uintptr_t GBGL_isTestMode = 0x3328;
constexpr uintptr_t GBGL_anticheatSpike = 0x38E0;

constexpr uintptr_t PL_checkpointArray = 0x3A38;
constexpr uintptr_t PL_uncommittedJumps = 0x3BD0;

constexpr uintptr_t GO_uniqueID = 0x39C;

constexpr uintptr_t GJL_normalPercentSeed = 0x3A4;
constexpr uintptr_t GJL_normalPercentRand = 0x3A0;
constexpr uintptr_t GJL_bestPercentSeed = 0x3B4;
constexpr uintptr_t GJL_bestPercentRand = 0x3B8;
constexpr uintptr_t GJL_practicePercent = 0x3C0;
constexpr uintptr_t GJL_levelType = 0x4C4;

}

inline void* GameManager() {
    if (!gd::Supported()) return nullptr;
    return *reinterpret_cast<void**>(gd::ExeBase() + rva::GameManagerInstance);
}

inline void* GameStatsManager() {
    if (!gd::Supported()) return nullptr;
    return *reinterpret_cast<void**>(gd::ExeBase() + rva::GameStatsManagerInstance);
}

inline void* PlayLayer() {
    void* gm = GameManager();
    if (!gm) return nullptr;
    void* pl = gd::F<void*>(gm, off::GM_playLayer);
    return pl && gd::cc::IsA(pl, ".?AVPlayLayer@@") ? pl : nullptr;
}

inline void* Level(void* layer) { return layer ? gd::F<void*>(layer, off::GBGL_level) : nullptr; }

inline int Seeded(const void* obj, uintptr_t seed, uintptr_t rand) { return gd::F<int>(obj, seed) - gd::F<int>(obj, rand); }

}
