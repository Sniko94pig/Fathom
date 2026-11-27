#include "game/internal.h"
#include "ingame/log.h"
#include <chrono>
#include <future>
#include <string>
#include <vector>

namespace bl3 {

namespace {

const float kReach = 2000000.0f;
const double kHold = 30.0;
const double kRefreshGap = 2.0;

const char* kGlobalFields[] = {"PlayerUnfogRadius", "VehicleUnfogRadius", "DefaultUnfogHeight", "DefaultVehicleUnfogHeight"};
const char* kZoneFields[] = {"CustomUnfogHeight", "CustomVehicleUnfogHeight"};

struct Raised {
    ue::FWeak obj;
    int offset = -1;
    float original = 0.0f;
};

enum class Step { Idle, Backup, Ready };

Step step = Step::Idle;
std::future<std::string> backup;
std::vector<Raised> raised;
ue::FWeak zone;
ue::FWeak boosted;
bool zoneSeen = false;
bool told = false;
double holdUntil = 0, refreshAt = 0;

bool Same(const ue::FWeak& a, const ue::FWeak& b) { return a.index == b.index && a.serial == b.serial; }

void* FogComponent() {
    void* c = st.pc ? ue::Ptr(st.pc, "FoDComponent") : nullptr;
    return ue::Alive(c) ? c : nullptr;
}

void* MapGlobals() {
    void* gg = GameplayGlobals();
    void* g = gg ? ue::Ptr(gg, "ZoneMapGlobals") : nullptr;
    return ue::Alive(g) ? g : nullptr;
}

void* FogZone(void* comp) {
    ue::Prop p = ue::FindProp(ue::ClassOf(comp), "CurrentLevelZoneMap");
    if (!p) return nullptr;
    ue::FWeak w{ue::At<int32_t>(comp, p.offset), ue::At<int32_t>(comp, p.offset + 4), nullptr};
    void* z = w.serial ? ue::WeakGet(w) : nullptr;
    return ue::Alive(z) ? z : nullptr;
}

void Raise(void* obj, const char* field) {
    ue::Prop p = ue::FindProp(ue::ClassOf(obj), field);
    if (!p || ue::PropType(p) != "FloatProperty") return;
    ue::FWeak w = ue::MakeWeak(obj);
    bool known = false;
    for (const Raised& r : raised)
        if (Same(r.obj, w) && r.offset == p.offset) known = true;
    if (!known) raised.push_back({w, p.offset, ue::Read<float>(obj, p)});
    ue::Write<float>(obj, p, kReach);
}

void Refresh(void* comp) {
    bool riding = st.vehicle != nullptr || st.seated;
    ue::Call c(comp, riding ? "OnPlayerEnteringVehicle" : "OnPlayerExitedVehicle");
    c.Set<void*>("PawnAttachSlotComponent", nullptr);
    c.Run();
}

void Release() {
    if (raised.empty()) return;
    for (const Raised& r : raised) {
        void* o = ue::WeakGet(r.obj);
        if (o) ue::Put<float>(o, r.offset, r.original);
    }
    raised.clear();
    void* comp = ue::WeakGet(boosted);
    if (comp && ue::Alive(comp)) Refresh(comp);
    boosted = {};
    LOG("explore map: reveal range back to normal");
}

void Reset() {
    Release();
    zone = {};
    zoneSeen = false;
    told = false;
    step = Step::Idle;
}

void Reveal(void* comp, void* globals, void* z) {
    for (const char* f : kGlobalFields) Raise(globals, f);
    for (const char* f : kZoneFields) Raise(z, f);
    Refresh(comp);
    boosted = ue::MakeWeak(comp);
    holdUntil = st.time + kHold;
    refreshAt = st.time + kRefreshGap;
    LOG("explore map: revealing %s", ue::Name(z).c_str());
    if (!told) {
        told = true;
        Note("Exploring the whole map, it turns blue in a few seconds");
    }
}

}

void ExploreInit() {
    bool ok = Has("OakPlayerController", "FoDComponent") && Has("FogOfDiscoveryComponent", "CurrentLevelZoneMap") &&
              HasFunc("FogOfDiscoveryComponent", "OnPlayerEnteringVehicle") && HasFunc("FogOfDiscoveryComponent", "OnPlayerExitedVehicle") &&
              Has("GameplayGlobals", "ZoneMapGlobals");
    for (const char* f : kGlobalFields) ok = ok && Has("ZoneMapGlobalsData", f);
    for (const char* f : kZoneFields) ok = ok && Has("ZoneMapData", f);
    Ready("explore_map", ok);
}

bool ExploreBusy() {
    return step == Step::Backup && backup.valid() && backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
}

void ExploreTick() {
    if (!On("explore_map")) {
        if (step == Step::Backup) {
            if (backup.valid() && backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
            if (backup.valid()) backup.get();
        }
        if (step != Step::Idle || !raised.empty()) Reset();
        return;
    }
    if (step == Step::Idle) {
        backup = std::async(std::launch::async, [] { return BackupNow(); });
        step = Step::Backup;
        Note("Backing up your saves first...");
        return;
    }
    if (step == Step::Backup) {
        if (backup.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        std::string path = backup.get();
        if (path.empty()) {
            step = Step::Idle;
            TurnOff("explore_map", "Save backup failed, the map was not changed");
            return;
        }
        LOG("explore map: saves backed up to %s", path.c_str());
        step = Step::Ready;
        zoneSeen = false;
    }
    if (!raised.empty() && st.time >= holdUntil) {
        Release();
        return;
    }
    void* comp = FogComponent();
    void* globals = MapGlobals();
    if (!comp || !globals) return;
    void* z = FogZone(comp);
    ue::FWeak zw = ue::MakeWeak(z);
    if (!zoneSeen || !Same(zw, zone)) {
        Release();
        zone = zw;
        zoneSeen = true;
        if (z) Reveal(comp, globals, z);
        return;
    }
    if (!raised.empty() && st.time >= refreshAt) {
        refreshAt = st.time + kRefreshGap;
        Refresh(comp);
    }
}

void ExploreOff() {
    if (!raised.empty() || zoneSeen) {
        Release();
        zoneSeen = false;
    }
    if (step == Step::Ready && !On("explore_map")) {
        step = Step::Idle;
        told = false;
    }
}

}
