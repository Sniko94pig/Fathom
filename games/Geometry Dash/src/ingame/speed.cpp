#include "ingame/audio.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include <algorithm>
#include <cmath>

namespace speed {

namespace {

bool ready = false;
float applied = 1.0f;
int refresh = 0;

float Wanted() {
    float s = gd::C().speed.load();
    if (!std::isfinite(s)) s = 1.0f;
    return std::clamp(s, 0.1f, 3.0f);
}

}

bool Install() {
    ready = audio::Install();
    return ready;
}

bool Ready() { return ready; }

void Tick() {
    if (!ready) return;
    float want = Wanted();
    bool changed = std::fabs(want - applied) > 1e-4f;
    if (!changed && std::fabs(want - 1.0f) > 1e-4f && ++refresh >= 120) {
        refresh = 0;
        changed = true;
    }
    if (changed && audio::SetMasterPitch(want)) applied = want;
}

float Scale(float dt) {
    if (!ready) return dt;
    return dt * Wanted();
}

void Off() {
    gd::C().speed = 1.0f;
    if (ready && std::fabs(applied - 1.0f) > 1e-4f && audio::SetMasterPitch(1.0f)) applied = 1.0f;
}

}
