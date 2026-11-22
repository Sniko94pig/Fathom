#include "ingame/game.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include "ui/menu.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <atomic>
#include <fstream>

namespace game {

namespace {

using core::Feature;
using SchedFn = void(__fastcall*)(void*, float);

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
SchedFn oSched = nullptr;
std::atomic<int> stop{0};
std::atomic<ULONGLONG> lastTick{0};
std::atomic<bool> hooked{false};
std::atomic<bool> modulesReady{false};
std::atomic<bool> prefsDirty{false};

std::wstring PrefsPath() { return gd::Folder() + L"\\gd.json"; }

void AllOff() {
    try {
        scare::Off();
        rgb::Off();
        macro::Off();
        icons::Off();
        player::Off();
        speed::Off();
        safe::Off();
    } catch (...) {
        LOG("error while switching the cheats off");
    }
}

void __fastcall hkSched(void* self, float dt) {
    gd::Busy busy;
    lastTick = GetTickCount64();
    try {
        gd::RunQueue();
    } catch (...) {
    }
    int s = stop.load();
    if (s == 1) {
        AllOff();
        stop = 2;
    } else if (s == 0 && modulesReady.load()) {
        try {
            safe::Tick();
            speed::Tick();
            player::Tick();
            icons::Tick();
            macro::Tick();
            scare::Tick(dt);
        } catch (...) {
            static int reported = 0;
            if (reported++ < 5) LOG("error in the frame update");
        }
        float real = dt;
        oSched(self, speed::Scale(dt));
        try {
            rgb::Tick(real);
        } catch (...) {
            static int reported = 0;
            if (reported++ < 5) LOG("error in the color update");
        }
        return;
    }
    oSched(self, dt);
}

Feature* Find(const char* id) { return model ? model->find(id) : nullptr; }

void SwitchOff(const char* id) {
    Feature* f = Find(id);
    if (!f || !f->on) return;
    f->on = false;
    if (f->apply) f->apply(*f);
}

void Apply(Feature& f) {
    gd::Cfg& c = gd::C();
    const std::string& id = f.id;
    if (id == "game_speed") c.speed = std::clamp(f.value, 0.1f, 3.0f);
    else if (id == "noclip") c.noclip = f.on;
    else if (id == "unlimited_jumps") c.jumps = f.on;
    else if (id == "hitboxes") c.hitboxes = f.on;
    else if (id == "unlock_icons") {
        c.icons = f.on;
        icons::Set(f.on);
    } else if (id == "complete_level") {
        f.result = player::Complete();
    } else if (id == "macro_record") {
        c.record = f.on;
        if (f.on) SwitchOff("macro_play");
    } else if (id == "macro_play") {
        c.play = f.on;
        if (f.on) SwitchOff("macro_record");
    } else if (id == "rgb") {
        c.rgb = f.on;
    } else if (id == "rgb_speed") {
        c.rgbSpeed = std::clamp(f.value, 0.1f, 5.0f);
        prefsDirty = true;
    } else if (id == "jumpscares") {
        c.scares = f.on;
    } else if (id == "jumpscare_every") {
        c.scareEvery = std::clamp(f.value, 1.0f, 30.0f);
        prefsDirty = true;
    } else if (id == "jumpscare_now") {
        scare::Now();
    }
}

float PrefNumber(const nlohmann::json& j, const char* key, float fallback) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_number()) return fallback;
    float v = it->get<float>();
    return std::isfinite(v) ? v : fallback;
}

void LoadPrefs() {
    try {
        std::ifstream in(PrefsPath(), std::ios::binary);
        if (!in) return;
        nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
        if (!j.is_object()) return;
        if (Feature* f = Find("rgb_speed")) {
            f->value = std::clamp(PrefNumber(j, "rgb_speed", f->def), f->min, f->max);
            gd::C().rgbSpeed = f->value;
        }
        if (Feature* f = Find("jumpscare_every")) {
            f->value = std::clamp(PrefNumber(j, "jumpscare_every", f->def), f->min, f->max);
            gd::C().scareEvery = f->value;
        }
    } catch (...) {
        LOG("gd.json could not be read, using the defaults");
    }
}

void SetAvail(const char* id, bool ok, const char* why) {
    Feature* f = Find(id);
    if (!f) return;
    f->available = ok;
    f->missing = ok ? std::string() : std::string(why);
}

void UpdateAvailability() {
    std::lock_guard<std::mutex> g(*modelLock);
    bool sm = safe::Ready();
    const char* noGame = gd::Supported() ? "Not found in this version of Geometry Dash" : "Needs a Fathom update for this Geometry Dash version";
    const char* noSafe = gd::Supported() ? "Safe mode could not start, so this stays off" : noGame;
    SetAvail("game_speed", sm && speed::Ready(), sm ? noGame : noSafe);
    SetAvail("noclip", sm && player::NoclipReady(), sm ? noGame : noSafe);
    SetAvail("unlimited_jumps", sm && player::JumpsReady(), sm ? noGame : noSafe);
    SetAvail("hitboxes", player::HitboxReady(), noGame);
    SetAvail("unlock_icons", icons::Ready(), noGame);
    SetAvail("complete_level", sm && player::CompleteReady(), sm ? noGame : noSafe);
    SetAvail("macro_record", macro::RecordReady(), noGame);
    SetAvail("macro_play", sm && macro::PlayReady(), sm ? noGame : noSafe);
    SetAvail("rgb", rgb::Ready(), noGame);
    SetAvail("rgb_speed", rgb::Ready(), noGame);
    SetAvail("jumpscares", scare::Ready(), noGame);
    SetAvail("jumpscare_every", scare::Ready(), noGame);
    SetAvail("jumpscare_now", scare::Ready(), noGame);
    int on = 0;
    for (const Feature& f : model->features)
        if (f.available) ++on;
    LOG("%d of %d modules are ready", on, (int)model->features.size());
}

}

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    for (Feature& f : m.features) {
        f.available = false;
        f.missing = "Starting up";
        f.apply = [](Feature& ff) { Apply(ff); };
    }
    LoadPrefs();
}

bool Start(std::string& err) {
    if (!safe::Install()) LOG("safe mode is not available, the cheats that need it stay off");
    if (!speed::Install()) LOG("game speed is not available");
    if (!player::Install()) LOG("some player cheats are not available");
    if (!icons::Install()) LOG("unlock all icons is not available");
    if (!macro::Install()) LOG("macros are not available");
    if (!rgb::Install()) LOG("rgb is not available");
    if (!scare::Install()) LOG("jumpscares are not available");
    if (!gd::Hook(gd::cc::SchedulerUpdateTarget(), (void*)&hkSched, (void**)&oSched, "frame update")) {
        err = "could not attach to the game's frame update";
        return false;
    }
    hooked = true;
    modulesReady = true;
    UpdateAvailability();
    return true;
}

void BeforeMenu(ui::Menu& menu) {
    bool good = false;
    const char* st = safe::Status(good);
    menu.status = st ? st : "";
    menu.statusGood = good;
}

void AfterMenu() {
    try {
        safe::Frame();
        macro::Frame();
        scare::Frame();
    } catch (...) {
        static int reported = 0;
        if (reported++ < 5) LOG("error while drawing the labels");
    }
}

void RequestStop() {
    int expected = 0;
    stop.compare_exchange_strong(expected, hooked.load() ? 1 : 2);
}

bool Stopped() { return stop.load() >= 2; }

bool Ticking() { return GetTickCount64() - lastTick.load() < 2000; }

bool Holding() { return safe::Holding(); }

void SavePrefs(bool force) {
    if (!force && !prefsDirty.exchange(false)) return;
    nlohmann::json j;
    j["rgb_speed"] = gd::C().rgbSpeed.load();
    j["jumpscare_every"] = gd::C().scareEvery.load();
    std::wstring path = PrefsPath();
    std::wstring tmp = path + L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out << j.dump(2);
        if (!out.good()) return;
    }
    MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

}
