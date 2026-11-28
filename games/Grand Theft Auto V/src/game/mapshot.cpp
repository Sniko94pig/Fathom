#include "game/internal.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include "ui/atlas.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

namespace gtav {

namespace {

const float kMinX = -4000.0f;
const float kMinY = -8800.0f;
const int kCols = 11;
const int kRows = 16;
const float kTile = 800.0f;
const int kPx = 256;
const float kHeight = 2000.0f;

struct Saved {
    uint32_t weather1 = 0, weather2 = 0;
    float weatherMix = 0;
    int hour = 12, minute = 0, second = 0;
    bool invincible = false;
    int frozen = 0;
};

struct Scan {
    bool active = false;
    int cam = 0;
    std::vector<int> order;
    size_t next = 0;
    int phase = 0;
    int retries = 0;
    int misses = 0;
    int done = 0;
    double until = 0;
    Saved saved;
} scan;

struct Landing {
    bool active = false;
    int entity = 0;
    bool invincible = false;
    double until = 0;
} landing;

std::vector<uint8_t> image;
std::mutex imageLock;
std::atomic<int> ask{0};
std::atomic<int> workers{0};
std::atomic<bool> loaded{false};

std::wstring PngPath() { return Folder() + L"\\mapscan.png"; }

void Publish(std::vector<uint8_t> rgba, int w, int h) {
    overlay::SetMapImage(std::move(rgba), w, h);
    WithModel([](core::Model& m) {
        m.mapMinX = kMinX;
        m.mapMinY = kMinY;
        m.mapMaxX = kMinX + kCols * kTile;
        m.mapMaxY = kMinY + kRows * kTile;
        m.mapPhotos = kCols * kRows;
    });
}

void Progress(bool on, float progress, const std::string& text) {
    WithModel([&](core::Model& m) {
        m.mapScanning = on;
        m.mapScanProgress = progress;
        m.mapScanText = text;
    });
}

struct Job {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    std::wstring path;
    bool save = false;
    HMODULE pin = nullptr;
};

HMODULE Pin() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCWSTR)&Pin, &m);
    return m;
}

void SaveWork(Job& j) {
    std::vector<uint8_t> png;
    auto sink = [](void* ctx, void* data, int size) {
        auto* out = (std::vector<uint8_t>*)ctx;
        out->insert(out->end(), (uint8_t*)data, (uint8_t*)data + size);
    };
    if (!stbi_write_png_to_func(sink, &png, j.w, j.h, 4, j.rgba.data(), j.w * 4) || png.empty()) {
        LOG("map picture could not be encoded");
        return;
    }
    std::ofstream f(std::filesystem::path(j.path), std::ios::binary | std::ios::trunc);
    f.write((const char*)png.data(), (std::streamsize)png.size());
    LOG("map picture saved (%zu KB)", png.size() / 1024);
}

void LoadWork(Job& j) {
    std::vector<uint8_t> px;
    int w = 0, h = 0;
    if (!ui::LoadImageFile(j.path, px, w, h) || w != kCols * kPx || h != kRows * kPx) {
        LOG("saved map picture could not be used");
        return;
    }
    {
        std::lock_guard<std::mutex> g(imageLock);
        if (image.empty()) image = px;
    }
    Publish(std::move(px), w, h);
    loaded = true;
    LOG("saved map picture loaded");
}

DWORD WINAPI JobThread(void* p) {
    HMODULE pin = ((Job*)p)->pin;
    {
        std::unique_ptr<Job> job((Job*)p);
        if (job->save) SaveWork(*job);
        else LoadWork(*job);
    }
    --workers;
    if (pin) FreeLibraryAndExitThread(pin, 0);
    return 0;
}

void RunJob(Job* job) {
    job->pin = Pin();
    ++workers;
    HANDLE t = CreateThread(nullptr, 0, JobThread, job, 0, nullptr);
    if (t) {
        CloseHandle(t);
        return;
    }
    --workers;
    if (job->pin) FreeLibrary(job->pin);
    delete job;
}

void SaveAsync(std::vector<uint8_t> rgba, int w, int h) {
    auto* job = new Job;
    job->rgba = std::move(rgba);
    job->w = w;
    job->h = h;
    job->path = PngPath();
    job->save = true;
    RunJob(job);
}

void Stitch(const std::vector<uint8_t>& shot, int w, int h, int col, int row) {
    std::lock_guard<std::mutex> g(imageLock);
    int s = std::min(w, h);
    int x0 = (w - s) / 2, y0 = (h - s) / 2;
    int stride = kCols * kPx;
    for (int oy = 0; oy < kPx; ++oy) {
        int sy0 = y0 + oy * s / kPx, sy1 = std::max(sy0 + 1, y0 + (oy + 1) * s / kPx);
        for (int ox = 0; ox < kPx; ++ox) {
            int sx0 = x0 + ox * s / kPx, sx1 = std::max(sx0 + 1, x0 + (ox + 1) * s / kPx);
            unsigned r = 0, g = 0, b = 0, n = 0;
            for (int sy = sy0; sy < sy1; sy += 2)
                for (int sx = sx0; sx < sx1; sx += 2) {
                    const uint8_t* p = shot.data() + ((size_t)sy * w + sx) * 4;
                    r += p[0];
                    g += p[1];
                    b += p[2];
                    ++n;
                }
            if (!n) continue;
            uint8_t* o = image.data() + ((size_t)(row * kPx + oy) * stride + (size_t)col * kPx + ox) * 4;
            o[0] = (uint8_t)(r / n);
            o[1] = (uint8_t)(g / n);
            o[2] = (uint8_t)(b / n);
            o[3] = 255;
        }
    }
}

void LandNow() {
    if (!landing.active) return;
    if (landing.entity && n::DOES_ENTITY_EXIST(landing.entity) && !On("noclip")) n::FREEZE_ENTITY_POSITION(landing.entity, 0);
    if (st.player >= 0 && !On("god_mode")) n::SET_PLAYER_INVINCIBLE(st.player, landing.invincible ? 1 : 0);
    landing = Landing{};
}

void LandTick() {
    if (!landing.active) return;
    if (!landing.entity || !n::DOES_ENTITY_EXIST(landing.entity)) {
        LandNow();
        return;
    }
    V3 p = n::GET_ENTITY_COORDS(landing.entity, false);
    n::REQUEST_COLLISION_AT_COORD(p.x, p.y, p.z);
    if (n::HAS_COLLISION_LOADED_AROUND_ENTITY(landing.entity) || st.time > landing.until) LandNow();
}

void Finish(bool complete) {
    if (!scan.active) return;
    overlay::CancelShot();
    if (scan.cam) {
        n::RENDER_SCRIPT_CAMS(0, 0, 0, 1, 0, 0);
        if (n::DOES_CAM_EXIST(scan.cam)) n::DESTROY_CAM(scan.cam, 0);
    }
    n::CLEAR_FOCUS();
    n::CLEAR_WEATHER_TYPE_PERSIST();
    n::CLEAR_OVERRIDE_WEATHER();
    if (scan.saved.weather1) n::SET_CURR_WEATHER_STATE(scan.saved.weather1, scan.saved.weather2, scan.saved.weatherMix);
    n::SET_CLOCK_TIME(scan.saved.hour, scan.saved.minute, scan.saved.second);
    if (!On("stop_time")) n::PAUSE_CLOCK(0);
    landing.active = true;
    landing.entity = scan.saved.frozen;
    landing.invincible = scan.saved.invincible;
    landing.until = st.time + 6.0;
    int w = kCols * kPx, h = kRows * kPx;
    if (scan.done > 0) {
        std::vector<uint8_t> copy;
        {
            std::lock_guard<std::mutex> g(imageLock);
            copy = image;
        }
        SaveAsync(copy, w, h);
        Publish(std::move(copy), w, h);
    }
    LOG("map scan %s: %d of %d pictures", complete ? "finished" : "stopped", scan.done, kCols * kRows);
    Note(complete ? "Map scan finished" : "Map scan stopped, " + std::to_string(scan.done) + " pictures kept");
    Progress(false, 0, "");
    scan = Scan{};
}

void Begin() {
    if (scan.active) return;
    LandNow();
    if (!st.ped || !st.alive || st.cutscene || st.switching) {
        Note("The map scan needs your character to be in control");
        return;
    }
    CameraOff();
    Saved s;
    n::GET_CURR_WEATHER_STATE(&s.weather1, &s.weather2, &s.weatherMix);
    s.hour = n::GET_CLOCK_HOURS();
    s.minute = n::GET_CLOCK_MINUTES();
    s.second = n::GET_CLOCK_SECONDS();
    bool base = false;
    s.invincible = GodBaseInvincible(base) ? base : n::GET_PLAYER_INVINCIBLE(st.player) != 0;
    s.frozen = st.vehicle ? st.vehicle : st.ped;
    float fov = 2.0f * std::atan((kTile * 0.5f) / kHeight) * 180.0f / 3.14159265f;
    int cam = n::CREATE_CAM_WITH_PARAMS("DEFAULT_SCRIPTED_CAMERA", kMinX, -kMinY, kHeight, -89.9f, 0.0f, 0.0f, fov, 1, 2);
    if (!cam) {
        Note("The game did not allow the scan camera right now");
        return;
    }
    n::SET_CAM_FAR_CLIP(cam, 12000.0f);
    n::RENDER_SCRIPT_CAMS(1, 0, 0, 1, 0, 0);
    n::SET_WEATHER_TYPE_NOW_PERSIST("EXTRASUNNY");
    n::SET_CLOCK_TIME(12, 0, 0);
    n::PAUSE_CLOCK(1);
    n::FREEZE_ENTITY_POSITION(s.frozen, 1);
    n::SET_PLAYER_INVINCIBLE(st.player, 1);
    {
        std::lock_guard<std::mutex> g(imageLock);
        if (image.size() != (size_t)kCols * kPx * kRows * kPx * 4) image.assign((size_t)kCols * kPx * kRows * kPx * 4, 0);
    }
    scan = Scan{};
    scan.active = true;
    scan.cam = cam;
    scan.saved = s;
    for (int r = 0; r < kRows; ++r)
        for (int c = 0; c < kCols; ++c) scan.order.push_back(r * kCols + ((r % 2) ? kCols - 1 - c : c));
    LOG("map scan started, fov %.2f", fov);
    Progress(true, 0, "Getting ready...");
}

void Step() {
    if (!scan.active) return;
    if (!st.ped || st.cutscene || st.switching) {
        Finish(false);
        return;
    }
    n::HIDE_HUD_AND_RADAR_THIS_FRAME();
    n::THEFEED_HIDE_THIS_FRAME();
    n::DISABLE_ALL_CONTROL_ACTIONS(0);
    if (n::IS_PAUSE_MENU_ACTIVE() || !overlay::WindowActive()) {
        if (scan.phase == 2) {
            overlay::CancelShot();
            scan.phase = 1;
        }
        scan.until = st.time + 1.0;
        return;
    }
    if (scan.next >= scan.order.size()) {
        Finish(true);
        return;
    }
    int tile = scan.order[scan.next];
    int col = tile % kCols, row = tile / kCols;
    float cx = kMinX + (col + 0.5f) * kTile;
    float cyMap = kMinY + (row + 0.5f) * kTile;
    float cy = -cyMap;
    if (scan.phase == 0) {
        n::SET_CAM_COORD(scan.cam, cx, cy, kHeight);
        n::SET_CAM_ROT(scan.cam, -89.9f, 0.0f, 0.0f, 2);
        n::SET_FOCUS_POS_AND_VEL(cx, cy, 0.0f, 0.0f, 0.0f, 0.0f);
        scan.until = st.time + (scan.next == 0 ? 4.0 : 1.6);
        scan.phase = 1;
        return;
    }
    n::SET_FOCUS_POS_AND_VEL(cx, cy, 0.0f, 0.0f, 0.0f, 0.0f);
    if (scan.phase == 1) {
        if (st.time < scan.until) return;
        overlay::RequestShot();
        scan.until = st.time + 3.0;
        scan.phase = 2;
        return;
    }
    std::vector<uint8_t> shot;
    int w = 0, h = 0;
    if (overlay::TakeShot(shot, w, h) && w > 0 && h > 0 && shot.size() >= (size_t)w * h * 4) {
        Stitch(shot, w, h, col, row);
        ++scan.done;
        scan.misses = 0;
    } else if (st.time < scan.until) {
        return;
    } else if (scan.retries < 1) {
        ++scan.retries;
        overlay::RequestShot();
        scan.until = st.time + 3.0;
        return;
    } else {
        LOG("map scan: no picture for tile %d", tile);
        if (++scan.misses >= 3) {
            LOG("map scan: the game's pictures could not be read, giving up");
            Note("The map scan stopped: Fathom could not read the game's pictures");
            Finish(false);
            return;
        }
    }
    scan.retries = 0;
    scan.phase = 0;
    ++scan.next;
    char buf[96];
    std::snprintf(buf, sizeof(buf), "Taking pictures: %zu of %zu", scan.next, scan.order.size());
    Progress(true, (float)scan.next / (float)scan.order.size(), buf);
    if (scan.next % 25 == 0) {
        std::vector<uint8_t> copy;
        {
            std::lock_guard<std::mutex> g(imageLock);
            copy = image;
        }
        Publish(std::move(copy), kCols * kPx, kRows * kPx);
    }
}

void LoadSaved() {
    std::wstring path = PngPath();
    std::error_code ec;
    if (!std::filesystem::exists(std::filesystem::path(path), ec)) return;
    auto* job = new Job;
    job->path = path;
    RunJob(job);
}

}

void MapshotInit(core::Model&) {
    WithModel([](core::Model& m) { m.mapScan = [](bool start) { ask = start ? 1 : 2; }; });
    LoadSaved();
}

void MapshotTick() {
    int a = ask.exchange(0);
    if (a == 1) Begin();
    if (a == 2 && scan.active) Finish(false);
    Step();
    LandTick();
}

void MapshotOff() {
    Finish(false);
    LandNow();
}

bool MapshotBusy() { return scan.active || landing.active; }

bool MapshotWriting() { return workers.load() > 0; }

bool MapshotBaseInvincible(bool& value) {
    if (scan.active) {
        value = scan.saved.invincible;
        return true;
    }
    if (landing.active) {
        value = landing.invincible;
        return true;
    }
    return false;
}

}
