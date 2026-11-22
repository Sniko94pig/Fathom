#pragma once
#include "ingame/mono.h"
#include "ingame/unity.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace uk {

using mono::Object;
using un::Fld;

struct Snap;

enum class Scene { Other, Level, MainMenu };

struct Ctx {
    const Snap* snap = nullptr;
    Object* player = nullptr;
    int playerId = 0;
    Scene scene = Scene::Other;
    bool inLevel = false;
    bool dead = false;
    bool activated = false;
    bool levelOver = false;
    bool paused = false;
    bool inIntro = false;
    bool loading = false;
    bool mainMenu = false;
    bool menuOpen = false;
    bool menuFocused = false;
    bool stopping = false;
    float dt = 0;
    float udt = 0;
    int frame = -1;
    double now = 0;

    bool on(const std::string& id) const;
    float value(const std::string& id, float fallback = 1.0f) const;
    void Note(const std::string& text) const;
};

void Note(const std::string& text);
const std::wstring& Folder();
std::string Backup();
std::string LastBackup();

struct Marker {
    float x = 0, y = 0;
    std::string label;
    uint32_t color = 0xFFFFFFFFu;
    float dist = -1.0f;
};

constexpr uint32_t Rgb(int r, int g, int b, int a = 255) {
    return (uint32_t)(r & 255) | ((uint32_t)(g & 255) << 8) | ((uint32_t)(b & 255) << 16) | ((uint32_t)(a & 255) << 24);
}

void Mark(const std::string& owner, std::vector<Marker> markers);
std::vector<Marker> Marks();

struct Hold {
    uint32_t h = 0;
    Hold() = default;
    Hold(const Hold&) = delete;
    Hold& operator=(const Hold&) = delete;
    Hold(Hold&& o) noexcept : h(o.h) { o.h = 0; }
    Hold& operator=(Hold&& o) noexcept {
        if (this != &o) {
            reset();
            h = o.h;
            o.h = 0;
        }
        return *this;
    }
    Object* get() const { return h ? mono::Pinned(h) : nullptr; }
    void set(Object* o) {
        if (h) mono::Unpin(h);
        h = o ? mono::Pin(o) : 0;
    }
    void reset() { set(nullptr); }
    bool held() const { return h != 0; }
};

struct Scaled {
    Hold obj;
    float base = 0, written = 0;
};

inline void Unscale(Scaled& s, const Fld& f) {
    Object* o = s.obj.get();
    if (o && f && std::fabs(un::Val<float>(o, f) - s.written) < 1e-4f) un::SetVal<float>(o, f, s.base);
    s.obj.reset();
}

inline void Scale(Object* o, const Fld& f, float mult, Scaled& s) {
    if (!f) return;
    Object* held = s.obj.get();
    if (s.obj.h && held != o) Unscale(s, f);
    if (!o || std::fabs(mult - 1.0f) < 1e-4f) {
        if (s.obj.h) Unscale(s, f);
        return;
    }
    float cur = un::Val<float>(o, f);
    if (!s.obj.h) {
        s.obj.set(o);
        s.base = cur;
    } else if (std::fabs(cur - s.written) > 1e-4f) {
        s.base = cur;
    }
    float want = s.base * mult;
    if (cur != want) un::SetVal<float>(o, f, want);
    s.written = want;
}

struct Flag {
    Hold obj;
    bool was = false;
};

inline void ForceFlag(Object* o, const Fld& f, bool want, Flag& s) {
    if (!f) return;
    Object* held = s.obj.get();
    if (s.obj.h && (held != o || !want)) {
        if (held && !s.was) un::SetVal<uint8_t>(held, f, 0);
        s.obj.reset();
    }
    if (!want || !o) return;
    if (!s.obj.h) {
        s.was = un::Val<uint8_t>(o, f) != 0;
        s.obj.set(o);
    }
    if (!un::Val<uint8_t>(o, f)) un::SetVal<uint8_t>(o, f, 1);
}

struct Disabled {
    Hold obj;
};

inline void ForceOff(Object* beh, bool want, Disabled& s) {
    Object* held = s.obj.get();
    if (s.obj.h && (held != beh || !want)) {
        if (un::Alive(held)) un::SetEnabled(held, true);
        s.obj.reset();
    }
    if (!want || !un::Alive(beh)) return;
    if (!s.obj.h) {
        if (un::Enabled(beh)) {
            un::SetEnabled(beh, false);
            s.obj.set(beh);
        }
    } else if (un::Enabled(beh)) {
        un::SetEnabled(beh, false);
    }
}

}
