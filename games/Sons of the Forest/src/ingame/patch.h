#pragma once
#include "ingame/unity.h"
#include <cmath>
#include <unordered_map>

namespace patch {

using il2::Object;
using un::Fld;

struct Hold {
    uint32_t h = 0;
    Object* get() const { return h ? il2::Pinned(h) : nullptr; }
    void set(Object* o) {
        if (h) il2::Unpin(h);
        h = o ? il2::Pin(o) : 0;
    }
    void reset() { set(nullptr); }
    bool held() const { return h != 0; }
};

template <class T>
struct Forced {
    Hold obj;
    T orig{};
    T written{};
    bool have = false;
};

template <class T>
void Release(Forced<T>& s, const Fld& f) {
    Object* o = s.obj.get();
    if (s.have && o && f && un::Alive(o) && un::Val<T>(o, f) == s.written) un::SetVal<T>(o, f, s.orig);
    s.obj.reset();
    s.have = false;
}

template <class T>
void Force(Object* o, const Fld& f, bool want, T value, Forced<T>& s) {
    if (!f) return;
    Object* held = s.obj.get();
    if (s.have && (held != o || !want)) Release(s, f);
    if (!want || !o || !un::Alive(o)) return;
    T cur = un::Val<T>(o, f);
    if (!s.have) {
        s.obj.set(o);
        s.orig = cur;
        s.have = true;
    } else if (cur != s.written) {
        s.orig = cur;
    }
    if (cur != value) un::SetVal<T>(o, f, value);
    s.written = value;
}

struct Scaled {
    Hold obj;
    float base = 0, written = 0;
    bool have = false;
};

inline void Unscale(Scaled& s, const Fld& f) {
    Object* o = s.obj.get();
    if (s.have && o && f && un::Alive(o) && std::fabs(un::Val<float>(o, f) - s.written) < 1e-4f) un::SetVal<float>(o, f, s.base);
    s.obj.reset();
    s.have = false;
}

inline void Scale(Object* o, const Fld& f, float mult, Scaled& s) {
    if (!f) return;
    Object* held = s.obj.get();
    if (s.have && held != o) Unscale(s, f);
    if (!o || !un::Alive(o) || std::fabs(mult - 1.0f) < 1e-4f) {
        if (s.have) Unscale(s, f);
        return;
    }
    float cur = un::Val<float>(o, f);
    if (!s.have) {
        s.obj.set(o);
        s.base = cur;
        s.have = true;
    } else if (std::fabs(cur - s.written) > 1e-4f) {
        s.base = cur;
    }
    float want = s.base * mult;
    if (cur != want) un::SetVal<float>(o, f, want);
    s.written = want;
}

struct Disabled {
    Hold obj;
};

inline void ForceDisabled(Object* behaviour, bool want, Disabled& s) {
    Object* held = s.obj.get();
    if (s.obj.held() && (held != behaviour || !want)) {
        if (un::Alive(held)) un::SetEnabled(held, true);
        s.obj.reset();
    }
    if (!want || !un::Alive(behaviour)) return;
    if (!s.obj.held()) {
        if (un::Enabled(behaviour)) {
            un::SetEnabled(behaviour, false);
            s.obj.set(behaviour);
        }
    } else if (un::Enabled(behaviour)) {
        un::SetEnabled(behaviour, false);
    }
}

template <class T>
struct ForcedMany {
    struct Entry {
        uint32_t h = 0;
        T orig{};
        T written{};
    };
    std::unordered_map<int, Entry> items;
};

template <class T>
void ForceOne(Object* o, const Fld& f, T value, ForcedMany<T>& s) {
    if (!o || !f || !un::Alive(o)) return;
    int id = un::InstanceId(o);
    auto it = s.items.find(id);
    T cur = un::Val<T>(o, f);
    if (it == s.items.end()) {
        typename ForcedMany<T>::Entry e;
        e.h = il2::Pin(o);
        e.orig = cur;
        e.written = value;
        s.items[id] = e;
    } else {
        if (cur != it->second.written) it->second.orig = cur;
        it->second.written = value;
    }
    if (cur != value) un::SetVal<T>(o, f, value);
}

template <class T>
void ReleaseAll(ForcedMany<T>& s, const Fld& f) {
    for (auto& [id, e] : s.items) {
        Object* o = il2::Pinned(e.h);
        if (o && f && un::Alive(o) && un::Val<T>(o, f) == e.written) un::SetVal<T>(o, f, e.orig);
        il2::Unpin(e.h);
    }
    s.items.clear();
}

}
