#include "ingame/gd.h"
#include "ingame/gdx.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace rgb {

namespace {

struct Color3 {
    uint8_t r, g, b;
};

static_assert(sizeof(Color3) == 3, "Color3 must match ccColor3B");

using SetColorFn = void(__fastcall*)(void*, const Color3*);
using GetColorFn = const Color3*(__fastcall*)(void*);
using ChildrenFn = void*(__fastcall*)(void*);
using UpdateFn = void(__fastcall*)(void*);

constexpr uint32_t kPlayerSetColor = 0x4FB630;
constexpr uint32_t kPlayerRgbaTable = 0x7D4E80;
constexpr uint32_t kSetSecondColor = 0x4FB6C0;
constexpr uint32_t kUpdateLevelColors = 0x312620;

constexpr uintptr_t kRgba = 0x140;
constexpr uintptr_t kChildrenSlot = 0x1E0;

constexpr uintptr_t kGmEditorLayer = 0x238;

constexpr uintptr_t kPlaybackMode = 0x89C;
constexpr uintptr_t kEffectManager = 0x968;
constexpr uintptr_t kBackground = 0x10E0;
constexpr uintptr_t kGroundLayer = 0x10F0;
constexpr uintptr_t kGroundLayer2 = 0x10F8;
constexpr uintptr_t kMiddleground = 0x1100;
constexpr uintptr_t kPreviewMode = 0x3A69;

constexpr uintptr_t kSprite1 = 0x198;
constexpr uintptr_t kSprite2 = 0x1A0;
constexpr uintptr_t kLineSprite = 0x1B0;

constexpr uintptr_t kEmActions = 0x2E0;
constexpr uintptr_t kEmSprites = 0x2F8;
constexpr uintptr_t kEmChannels = 0x44E;

constexpr uintptr_t kIconGlow = 0x758;
constexpr uintptr_t kVehicleGlow = 0x780;
constexpr uintptr_t kTrail = 0x7A8;
constexpr uintptr_t kWaveTrail = 0x7B8;
constexpr uintptr_t kPlayEffects = 0x7EC;
constexpr uintptr_t kSwitchWaveColor = 0x832;
constexpr uintptr_t kRobot = 0x8A8;
constexpr uintptr_t kSpider = 0x8B0;
constexpr uintptr_t kNoTrailTint = 0x930;
constexpr uintptr_t kOriginalMain = 0x982;
constexpr uintptr_t kOriginalSecond = 0x985;
constexpr uintptr_t kHasGlowColor = 0x988;
constexpr uintptr_t kGlowColor = 0x989;
constexpr uintptr_t kIsDart = 0x9C4;
constexpr uintptr_t kPlayerColor2 = 0xA95;
constexpr uintptr_t kPartGlow = 0x300;

constexpr uintptr_t kPlayerParts[] = {0x740, 0x748, 0x760, 0x768, kRobot, kSpider};
constexpr uintptr_t kPartParts[] = {0x2B0, 0x2F8, kPartGlow};
constexpr uintptr_t kParts[] = {kRobot, kSpider};
constexpr uintptr_t kPlayers[] = {gdx::off::GBGL_player1, gdx::off::GBGL_player2};
constexpr uintptr_t kGrounds[] = {kGroundLayer, kGroundLayer2};

constexpr float kCycleSeconds = 3.0f;

struct Saved {
    bool has = false;
    Color3 c{};
};

struct State {
    void* layer = nullptr;
    bool editor = false;
    Saved bg, ground1, ground2, line, middle1, middle2;
    Saved wave[2];
    bool seen[2] = {false, false};
};

SetColorFn setSecondColor = nullptr;
UpdateFn updateLevelColors = nullptr;
std::atomic<bool> ready{false};
State st;
float phase = 0.0f;

void* Ptr(const void* obj, uintptr_t off) { return obj ? gd::F<void*>(obj, off) : nullptr; }

bool Flag(const void* obj, uintptr_t off) { return gd::F<uint8_t>(obj, off) != 0; }

void** Table(void* obj) { return obj ? *reinterpret_cast<void***>(obj) : nullptr; }

void Tint(void* node, const Color3& c) {
    if (!node) return;
    void* rgba = static_cast<uint8_t*>(node) + kRgba;
    void** vt = Table(rgba);
    if (!vt || !vt[0]) return;
    reinterpret_cast<SetColorFn>(vt[0])(rgba, &c);
}

bool ColorOf(void* node, Color3& out) {
    if (!node) return false;
    void* rgba = static_cast<uint8_t*>(node) + kRgba;
    void** vt = Table(rgba);
    if (!vt || !vt[1]) return false;
    const Color3* c = reinterpret_cast<GetColorFn>(vt[1])(rgba);
    if (!c) return false;
    out = *c;
    return true;
}

void* Children(void* node) {
    void** vt = Table(node);
    if (!vt || !vt[kChildrenSlot / sizeof(void*)]) return nullptr;
    return reinterpret_cast<ChildrenFn>(vt[kChildrenSlot / sizeof(void*)])(node);
}

void TintChildren(void* node, const Color3& c) {
    void* arr = Children(node);
    unsigned n = gd::cc::Count(arr);
    for (unsigned i = 0; i < n; ++i) Tint(gd::cc::At(arr, i), c);
}

void Remember(Saved& s, void* node) { s.has = ColorOf(node, s.c); }

void Keep(Saved& s, void* node) {
    if (!s.has) Remember(s, node);
}

void KeepChild(Saved& s, void* node) {
    if (s.has) return;
    void* arr = Children(node);
    s.has = gd::cc::Count(arr) > 0 && ColorOf(gd::cc::At(arr, 0), s.c);
}

void KeepTint(Saved& s, void* node, const Color3& c) {
    Keep(s, node);
    Tint(node, c);
}

void KeepTintChildren(Saved& s, void* node, const Color3& c) {
    KeepChild(s, node);
    TintChildren(node, c);
}

uint8_t Channel(float v) { return static_cast<uint8_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); }

Color3 Hue(float h) {
    h -= std::floor(h);
    float x = h * 6.0f;
    int i = std::clamp(static_cast<int>(x), 0, 5);
    float f = std::clamp(x - static_cast<float>(i), 0.0f, 1.0f);
    float r = 0.0f, g = 0.0f, b = 0.0f;
    switch (i) {
    case 0:
        r = 1.0f;
        g = f;
        break;
    case 1:
        r = 1.0f - f;
        g = 1.0f;
        break;
    case 2:
        g = 1.0f;
        b = f;
        break;
    case 3:
        g = 1.0f - f;
        b = 1.0f;
        break;
    case 4:
        r = f;
        b = 1.0f;
        break;
    default:
        r = 1.0f;
        b = 1.0f - f;
        break;
    }
    return {Channel(r), Channel(g), Channel(b)};
}

float Advance(float t, float dt) {
    if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
    dt = std::min(dt, 1.0f);
    float s = gd::C().rgbSpeed.load();
    if (!std::isfinite(s)) s = 1.0f;
    s = std::clamp(s, 0.1f, 5.0f);
    t += dt * s / kCycleSeconds;
    t -= std::floor(t);
    return std::isfinite(t) ? t : 0.0f;
}

void* EditorLayer() {
    void* ed = Ptr(gdx::GameManager(), kGmEditorLayer);
    return ed && gd::cc::IsA(ed, ".?AVLevelEditorLayer@@") ? ed : nullptr;
}

void* Current(bool& editor) {
    editor = false;
    if (void* pl = gdx::PlayLayer()) return pl;
    void* ed = EditorLayer();
    if (!ed || gd::F<int>(ed, kPlaybackMode) == 0) return nullptr;
    editor = true;
    return ed;
}

bool Alive() {
    if (!st.layer) return false;
    return st.editor ? EditorLayer() == st.layer : gdx::PlayLayer() == st.layer;
}

bool PlayerOk(void* p) {
    if (!p || !gd::cc::IsA(p, ".?AVPlayerObject@@")) return false;
    for (uintptr_t off : kPlayerParts)
        if (!gd::F<void*>(p, off)) return false;
    for (uintptr_t part : kParts)
        for (uintptr_t off : kPartParts)
            if (!Ptr(Ptr(p, part), off)) return false;
    return true;
}

void TintGlow(void* p, const Color3& c) {
    Tint(Ptr(p, kIconGlow), c);
    Tint(Ptr(p, kVehicleGlow), c);
    for (uintptr_t part : kParts) TintChildren(Ptr(Ptr(p, part), kPartGlow), c);
}

void TintTrails(void* p, const Color3& main, const Color3& second) {
    bool dart = Flag(p, kIsDart);
    if (!Flag(p, kNoTrailTint) && !dart) Tint(Ptr(p, kTrail), second);
    if (Flag(p, kPlayEffects) || dart) Tint(Ptr(p, kWaveTrail), Flag(p, kSwitchWaveColor) ? second : main);
}

void PaintPlayer(void* p, const Color3& main, const Color3& second, const Color3& glow) {
    Tint(p, main);
    setSecondColor(p, &second);
    TintGlow(p, glow);
    TintTrails(p, main, second);
}

void RestorePlayer(void* p, const Saved& wave) {
    Color3 main = gd::F<Color3>(p, kOriginalMain);
    Color3 second = gd::F<Color3>(p, kOriginalSecond);
    Color3 bright = gd::F<Color3>(p, kPlayerColor2);
    Color3 glow = Flag(p, kHasGlowColor) ? gd::F<Color3>(p, kGlowColor) : bright;
    Tint(p, main);
    setSecondColor(p, &second);
    TintGlow(p, glow);
    if (!Flag(p, kNoTrailTint) && !Flag(p, kIsDart)) Tint(Ptr(p, kTrail), bright);
    if (Flag(p, kPlayEffects)) Tint(Ptr(p, kWaveTrail), Flag(p, kSwitchWaveColor) ? bright : main);
    else if (wave.has) Tint(Ptr(p, kWaveTrail), wave.c);
}

void PaintLevel(void* layer, const Color3& ground, const Color3& line) {
    KeepTint(st.bg, Ptr(layer, kBackground), ground);
    for (uintptr_t off : kGrounds) {
        void* g = Ptr(layer, off);
        if (!g) continue;
        KeepTintChildren(st.ground1, Ptr(g, kSprite1), ground);
        KeepTintChildren(st.ground2, Ptr(g, kSprite2), ground);
        KeepTint(st.line, Ptr(g, kLineSprite), line);
    }
    void* mg = Ptr(layer, kMiddleground);
    if (!mg) return;
    KeepTintChildren(st.middle1, Ptr(mg, kSprite1), ground);
    KeepTintChildren(st.middle2, Ptr(mg, kSprite2), ground);
}

void Begin(void* layer, bool editor) {
    st = State{};
    st.layer = layer;
    st.editor = editor;
}

void Apply(void* layer, float t) {
    Color3 first = Hue(t);
    Color3 second = Hue(t + 0.5f);
    Color3 ground = Hue(t + 0.25f);
    Color3 line = Hue(t + 0.75f);
    for (int i = 0; i < 2; ++i) {
        void* p = Ptr(layer, kPlayers[i]);
        if (!PlayerOk(p)) continue;
        if (!st.seen[i]) {
            Remember(st.wave[i], Ptr(p, kWaveTrail));
            st.seen[i] = true;
        }
        PaintPlayer(p, i == 0 ? first : second, i == 0 ? second : first, second);
    }
    PaintLevel(layer, ground, line);
}

bool VectorHolds(void* em, uintptr_t off) {
    uintptr_t first = gd::F<uintptr_t>(em, off);
    uintptr_t last = gd::F<uintptr_t>(em, off + sizeof(void*));
    return first && last >= first && (last - first) / sizeof(void*) >= kEmChannels;
}

bool LevelReady(void* layer) {
    void* em = Ptr(layer, kEffectManager);
    if (!em || !VectorHolds(em, kEmActions) || !VectorHolds(em, kEmSprites)) return false;
    if (!Ptr(layer, kBackground)) return false;
    for (uintptr_t off : kGrounds) {
        void* g = Ptr(layer, off);
        if (!g || !Ptr(g, kSprite1) || !Ptr(g, kLineSprite)) return false;
    }
    return true;
}

void RestoreLevel() {
    void* layer = st.layer;
    bool live = !st.editor || Flag(layer, kPreviewMode);
    if (live && LevelReady(layer)) {
        updateLevelColors(layer);
        return;
    }
    if (st.bg.has) Tint(Ptr(layer, kBackground), st.bg.c);
    for (uintptr_t off : kGrounds) {
        void* g = Ptr(layer, off);
        if (!g) continue;
        if (st.ground1.has) TintChildren(Ptr(g, kSprite1), st.ground1.c);
        if (st.ground2.has) TintChildren(Ptr(g, kSprite2), st.ground2.c);
        if (st.line.has) Tint(Ptr(g, kLineSprite), st.line.c);
    }
    void* mg = Ptr(layer, kMiddleground);
    if (!mg) return;
    if (st.middle1.has) TintChildren(Ptr(mg, kSprite1), st.middle1.c);
    if (st.middle2.has) TintChildren(Ptr(mg, kSprite2), st.middle2.c);
}

void Release() {
    if (st.layer && Alive()) {
        for (int i = 0; i < 2; ++i) {
            void* p = Ptr(st.layer, kPlayers[i]);
            if (PlayerOk(p)) RestorePlayer(p, st.wave[i]);
        }
        RestoreLevel();
    }
    st = State{};
}

template <class F>
F Fn(uint32_t rva) {
    if (!gd::IsFunctionStart(rva)) {
        LOG("rgb: %X is not a function start", rva);
        return nullptr;
    }
    return reinterpret_cast<F>(gd::Exe(rva));
}

bool TableOk() {
    if (!gd::IsFunctionStart(kPlayerSetColor)) return false;
    auto slot = reinterpret_cast<const uintptr_t*>(gd::Exe(kPlayerRgbaTable));
    return slot && *slot == reinterpret_cast<uintptr_t>(gd::Exe(kPlayerSetColor));
}

}

bool Install() {
    if (!gd::Supported()) return false;
    setSecondColor = Fn<SetColorFn>(kSetSecondColor);
    updateLevelColors = Fn<UpdateFn>(kUpdateLevelColors);
    bool tableOk = TableOk();
    if (!tableOk) LOG("rgb: the player color table does not match this game version");
    bool ok = setSecondColor && updateLevelColors && tableOk;
    ready = ok;
    return ok;
}

bool Ready() { return ready.load(); }

void Tick(float dt) {
    if (!ready.load()) return;
    bool editor = false;
    void* layer = Current(editor);
    bool want = layer && gd::C().rgb.load();
    if (st.layer && (!want || layer != st.layer)) Release();
    if (!want) return;
    if (!st.layer) Begin(layer, editor);
    phase = Advance(phase, dt);
    Apply(layer, phase);
}

void Off() {
    gd::C().rgb = false;
    if (ready.load()) Release();
}

}
