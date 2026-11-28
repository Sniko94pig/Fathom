#include "game/internal.h"
#include "game/mem.h"
#include "ingame/log.h"
#include <algorithm>
#include <cmath>

namespace gtav {

namespace {

struct Place {
    const char* name;
    float x, y, z;
};

const Place kPlaces[] = {
    {"Michael's house", -852.4f, 160.0f, 65.6f},
    {"Franklin's house", 7.9f, 548.1f, 175.5f},
    {"Trevor's trailer", 1985.7f, 3812.2f, 32.2f},
    {"Los Santos airport", -1336.0f, -3044.0f, 13.9f},
    {"Airport entrance", -1034.6f, -2733.6f, 13.8f},
    {"Maze Bank Tower roof", -75.0f, -818.0f, 0.0f},
    {"Vinewood sign", 711.4f, 1198.1f, 0.0f},
    {"Mount Chiliad", 425.4f, 5614.3f, 766.5f},
    {"Fort Zancudo", -2047.4f, 3132.1f, 32.8f},
    {"Sandy Shores airfield", 1747.0f, 3273.7f, 41.1f},
    {"McKenzie airfield", 2121.7f, 4796.3f, 41.1f},
    {"Del Perro Pier", -1850.1f, -1231.7f, 13.0f},
    {"Ferris wheel", -1670.7f, -1125.0f, 13.0f},
    {"Vespucci Beach", -1385.0f, -1530.0f, 0.0f},
    {"Strip club", 127.4f, -1307.7f, 29.2f},
    {"Elysian Island", 338.2f, -2715.9f, 38.5f},
    {"Jetsam terminal", 760.4f, -2943.2f, 5.8f},
    {"El Burro Heights", 1384.0f, -2057.1f, 52.0f},
    {"Chumash", -3192.6f, 1100.0f, 20.2f},
    {"Wind farm", 2354.0f, 1830.3f, 101.1f},
    {"Paleto Bay", -276.0f, 6635.0f, 0.0f},
    {"Grapeseed", 1680.0f, 4800.0f, 0.0f},
    {"Mount Gordo", 2877.0f, 5911.0f, 0.0f},
    {"Humane Labs", 3617.0f, 3739.0f, 0.0f},
};

struct Teleport {
    bool active = false;
    int entity = 0;
    bool vehicle = false;
    float x = 0, y = 0, hover = 0;
    double started = 0;
} tp;

V3 undoPos;
bool canUndo = false;
double nextMarkers = 0;

struct Speed {
    bool scaled = false;
    std::vector<uintptr_t> addrs;
    bool probed = false;
    bool fast = false;
    bool broken = false;
    bool verified = false;
    float written = 1.0f;
    double checkStart = 0;
    double gameTime = 0;
} speed;

bool clockPaused = false;

int TpEntity(bool& vehicle) {
    vehicle = st.vehicle != 0;
    return st.vehicle ? st.vehicle : st.ped;
}

void PutAt(int e, bool vehicle, float x, float y, float z) {
    n::SET_ENTITY_COORDS_NO_OFFSET(e, x, y, z, 0, 0, 0);
    n::SET_ENTITY_VELOCITY(e, 0, 0, 0);
    if (vehicle) n::SET_VEHICLE_ON_GROUND_PROPERLY(e, 5.0f);
}

void Finish(bool ok) {
    if (tp.entity && n::DOES_ENTITY_EXIST(tp.entity)) n::FREEZE_ENTITY_POSITION(tp.entity, 0);
    n::CLEAR_FOCUS();
    if (!ok) Note("Could not find the ground there");
    tp = Teleport{};
}

void StartTeleport(float x, float y, float z, bool useZ) {
    if (!st.ped || !st.alive) {
        Note("Teleport works when your character is alive");
        return;
    }
    if (MapshotBusy()) {
        Note("Wait until the map scan is done");
        return;
    }
    bool vehicle = false;
    int e = TpEntity(vehicle);
    undoPos = n::GET_ENTITY_COORDS(e, false);
    canUndo = true;
    if (tp.active) Finish(true);
    if (useZ && z != 0.0f) {
        n::REQUEST_COLLISION_AT_COORD(x, y, z);
        PutAt(e, vehicle, x, y, z);
        return;
    }
    float approx = n::GET_APPROX_HEIGHT_FOR_POINT(x, y);
    tp.active = true;
    tp.entity = e;
    tp.vehicle = vehicle;
    tp.x = x;
    tp.y = y;
    tp.hover = (approx > -100.0f && approx < 1500.0f ? approx : 150.0f) + 5.0f;
    tp.started = st.time;
    n::FREEZE_ENTITY_POSITION(e, 1);
    n::SET_ENTITY_COORDS_NO_OFFSET(e, x, y, tp.hover, 0, 0, 0);
    n::SET_FOCUS_POS_AND_VEL(x, y, tp.hover, 0, 0, 0);
}

void TeleportTick() {
    if (!tp.active) return;
    if (!n::DOES_ENTITY_EXIST(tp.entity)) {
        Finish(true);
        return;
    }
    n::FREEZE_ENTITY_POSITION(tp.entity, 1);
    n::SET_ENTITY_COORDS_NO_OFFSET(tp.entity, tp.x, tp.y, tp.hover, 0, 0, 0);
    n::REQUEST_COLLISION_AT_COORD(tp.x, tp.y, tp.hover);
    n::SET_FOCUS_POS_AND_VEL(tp.x, tp.y, tp.hover, 0, 0, 0);
    double age = st.time - tp.started;
    bool loaded = n::HAS_COLLISION_LOADED_AROUND_ENTITY(tp.entity) != 0;
    float gz = 0;
    if ((loaded || age > 2.0) && n::GET_GROUND_Z_FOR_3D_COORD(tp.x, tp.y, 1000.0f, &gz, 0, 0) && gz != 0.0f) {
        float wh = 0;
        if (n::GET_WATER_HEIGHT(tp.x, tp.y, 1000.0f, &wh) && wh > gz) gz = wh;
        n::FREEZE_ENTITY_POSITION(tp.entity, 0);
        PutAt(tp.entity, tp.vehicle, tp.x, tp.y, gz + (tp.vehicle ? 1.5f : 1.0f));
        Finish(true);
        return;
    }
    if (age < 8.0) return;
    float wh = 0;
    float z = n::GET_APPROX_HEIGHT_FOR_POINT(tp.x, tp.y);
    if (n::GET_WATER_HEIGHT(tp.x, tp.y, 100.0f, &wh) && wh > z) z = wh;
    n::FREEZE_ENTITY_POSITION(tp.entity, 0);
    PutAt(tp.entity, tp.vehicle, tp.x, tp.y, z + 2.0f);
    LOG("teleport used the rough height %.1f at %.0f, %.0f", z, tp.x, tp.y);
    Finish(true);
}

float MapYaw(float heading) {
    const float d = 3.14159265f / 180.0f;
    float h = heading * d;
    return std::atan2(-std::cos(h), -std::sin(h));
}

void MarkersTick() {
    if (st.time < nextMarkers) return;
    nextMarkers = st.time + (st.mapVisible ? 0.1 : 0.5);
    std::vector<core::Marker> marks;
    if (st.ped) {
        core::Marker me;
        me.type = core::MarkerType::Player;
        me.name = "You";
        me.x = st.pos.x;
        me.y = -st.pos.y;
        me.z = st.pos.z;
        float heading = st.vehicle ? n::GET_ENTITY_HEADING(st.vehicle) : st.heading;
        me.yaw = MapYaw(heading);
        marks.push_back(me);
    }
    if (n::IS_WAYPOINT_ACTIVE()) {
        int blip = n::GET_FIRST_BLIP_INFO_ID(8);
        if (blip) {
            V3 w = n::GET_BLIP_INFO_ID_COORD(blip);
            core::Marker wp;
            wp.type = core::MarkerType::Beacon;
            wp.name = "Waypoint";
            wp.x = w.x;
            wp.y = -w.y;
            wp.z = 0;
            marks.push_back(wp);
        }
    }
    bool undo = canUndo;
    WithModel([&](core::Model& m) {
        m.markers = std::move(marks);
        m.canUndoTeleport = undo;
        m.inWorld = st.ped != 0;
    });
}

void ProbeSpeed() {
    speed.probed = true;
    n::SET_TIME_SCALE(0.6125f);
    std::vector<uintptr_t> a = mem::FindFloat(0.6125f, true);
    n::SET_TIME_SCALE(0.4375f);
    std::vector<uintptr_t> keep;
    for (uintptr_t p : a) {
        float v = 0;
        if (mem::Read(p, v) && v == 0.4375f) keep.push_back(p);
    }
    n::SET_TIME_SCALE(1.0f);
    std::vector<uintptr_t> ones;
    for (uintptr_t p : keep) {
        float v = 0;
        if (mem::Read(p, v) && v == 1.0f) ones.push_back(p);
    }
    if (!ones.empty() && ones.size() <= 3) {
        speed.addrs = ones;
        for (uintptr_t p : ones) LOG("game speed value candidate at game+0x%llX", (unsigned long long)(p - mem::GameBase()));
    } else {
        LOG("game speed value not found for speeds above 1x (%zu matches)", ones.size());
    }
}

void FastOff() {
    if (speed.fast)
        for (uintptr_t p : speed.addrs) mem::Write(p, 1.0f);
    speed.fast = false;
    speed.written = 1.0f;
    speed.checkStart = 0;
}

void CapSpeed() {
    FastOff();
    n::SET_TIME_SCALE(1.0f);
    speed.scaled = true;
    speed.broken = true;
    static bool told = false;
    if (told) return;
    told = true;
    WithModel([](core::Model& m) {
        if (core::Feature* f = m.find("game_speed")) {
            f->max = 1.0f;
            if (f->value > 1.0f) f->value = 1.0f;
        }
    });
    Note("This game can't go faster than 1x, so Game speed now stops at 1x");
    LOG("game speed above 1x is not possible, the slider now stops at 1x");
}

void SpeedTick() {
    float v = std::clamp(Value("game_speed"), 0.1f, 2.0f);
    bool off = std::fabs(v - 1.0f) < 0.001f;
    if (off) {
        FastOff();
        if (speed.scaled) n::SET_TIME_SCALE(1.0f);
        speed.scaled = false;
        return;
    }
    if (v < 1.0f) {
        FastOff();
        n::SET_TIME_SCALE(v);
        speed.scaled = true;
        return;
    }
    if (speed.broken) {
        CapSpeed();
        return;
    }
    if (!speed.probed) ProbeSpeed();
    if (speed.addrs.empty()) {
        CapSpeed();
        return;
    }
    for (uintptr_t p : speed.addrs) {
        float cur = 0;
        if (!mem::Read(p, cur) || (cur != 1.0f && cur != speed.written)) {
            LOG("game speed value changed by the game (%.3f), stopping", cur);
            CapSpeed();
            return;
        }
    }
    n::SET_TIME_SCALE(1.0f);
    speed.scaled = true;
    for (uintptr_t p : speed.addrs) mem::Write(p, v);
    speed.written = v;
    speed.fast = true;
    if (speed.verified) return;
    if (speed.checkStart == 0) {
        speed.checkStart = st.time;
        speed.gameTime = 0;
        return;
    }
    speed.gameTime += n::GET_FRAME_TIME();
    double real = st.time - speed.checkStart;
    if (real < 1.5) return;
    double ratio = speed.gameTime / real;
    if (ratio >= 1.0 + 0.5 * (v - 1.0)) {
        speed.verified = true;
        LOG("game speed above 1x works (measured %.2fx)", ratio);
    } else {
        LOG("game speed above 1x had no effect (measured %.2fx)", ratio);
        CapSpeed();
    }
}

void ClockTick() {
    if (On("stop_time")) {
        n::PAUSE_CLOCK(1);
        clockPaused = true;
    } else if (clockPaused) {
        n::PAUSE_CLOCK(0);
        clockPaused = false;
    }
}

}

void WorldInit(core::Model& m) {
    (void)m;
    std::vector<core::Location> places;
    for (const Place& p : kPlaces) places.push_back({p.name, p.x, -p.y, p.z});
    WithModel([&](core::Model& mm) {
        mm.places = std::move(places);
        mm.mapDrawn = true;
        mm.mapWords.chips = {"Vehicles", "Houses", "Waypoint", "Places"};
        mm.mapWords.empty = "Click anywhere to teleport. Scan whole map takes a picture of the map";
        mm.mapWords.photos = "Map pictures are taken with Scan whole map";
        mm.mapWords.photosMore = "";
        mm.mapWords.scanButton = "Scan whole map";
        mm.mapWords.scanTitle = "Scan the whole map?";
        mm.mapWords.scanBody = "A camera flies high over Los Santos and Blaine County and takes a picture of every part. The weather and time are "
                               "set to a clear noon while it runs and put back afterwards, and your character waits where it is. It takes about "
                               "5 to 10 minutes, and you can stop it at any time.";
        mm.teleport = [](float x, float y, float z, bool useZ) { Post([x, y, z, useZ] { StartTeleport(x, -y, z, useZ); }); };
        mm.undoTeleport = [] {
            Post([] {
                if (canUndo) StartTeleport(undoPos.x, undoPos.y, undoPos.z, true);
            });
        };
    });
    Ready("map", true);
    Ready("game_speed", true);
    Ready("stop_time", true);
}

void WorldTick() {
    TeleportTick();
    MarkersTick();
    SpeedTick();
    ClockTick();
}

void WorldOff() {
    if (tp.active) Finish(true);
    FastOff();
    if (speed.scaled) n::SET_TIME_SCALE(1.0f);
    speed.scaled = false;
    if (clockPaused) n::PAUSE_CLOCK(0);
    clockPaused = false;
}

}
