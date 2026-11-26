#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <mutex>
#include <set>

namespace bl3 {

namespace {

struct TravelSpot {
    ue::FWeak station;
    std::string name;
};

struct Request {
    float x = 0, y = 0, z = 0;
    bool useZ = false;
    bool undo = false;
};

std::vector<TravelSpot> travels;
std::set<std::string> dlcAll, dlcOwned;
bool dlcDone = false;
double dlcNext = 0;
std::mutex reqLock;
std::vector<Request> requests;
bool haveUndo = false;
ue::FVector undoPos;
double placesAt = -100, markersAt = 0;
void* lastWorld = nullptr;
std::vector<core::Marker> fixedMarkers;
const float kTravelBase = -9000000.0f;

const char* kPaidDlc[] = {"dandelion", "hibiscus", "geranium", "alisma", "ixora", "ixora2"};

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string DlcFolder(const std::string& path) {
    std::string l = Lower(path);
    size_t p = l.find("/patchdlc/");
    if (p == std::string::npos) return {};
    size_t s = p + 10, e = l.find('/', s);
    return l.substr(s, e == std::string::npos ? std::string::npos : e - s);
}

void ToMap(const ue::FVector& w, float& mx, float& my) {
    mx = w.y / 100.0f;
    my = -w.x / 100.0f;
}

ue::FVector FromMap(float mx, float my, float zMeters) { return {-my * 100.0f, mx * 100.0f, zMeters * 100.0f}; }

std::string TextAt(void* obj, const char* prop) {
    ue::Prop p = obj ? ue::FindProp(ue::ClassOf(obj), prop) : ue::Prop{};
    return p ? ue::TextToString((uint8_t*)obj + p.offset) : std::string();
}

std::string Clean(std::string s) {
    for (const char* pre : {"BP_", "IO_", "BPIO_", "Prop_"})
        if (s.rfind(pre, 0) == 0) s = s.substr(std::strlen(pre));
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '_') c = ' ';
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)s[i - 1])) out.push_back(' ');
        out.push_back(c);
    }
    while (!out.empty() && (out.back() == ' ' || std::isdigit((unsigned char)out.back()))) out.pop_back();
    return out.empty() ? s : out;
}

ue::TArray<void*> Arr(void* obj, const char* prop) {
    ue::TArray<void*> a;
    ue::Prop p = obj ? ue::FindProp(ue::ClassOf(obj), prop) : ue::Prop{};
    if (p) a = ue::Read<ue::TArray<void*>>(obj, p);
    if (a.num < 0 || a.num > 10000) a.num = 0;
    return a;
}

std::vector<void*> ZoneMaps(void* planet, bool licensed) {
    std::vector<void*> out;
    ue::Call c(Lib("PlanetData"), "GetPlanetZoneMaps");
    c.Set<void*>("PlayerController", st.pc).Set<void*>("Planet", planet).SetBool("FindOnlyDiscovered", false).SetBool("FindOnlyLicensed", licensed);
    if (!c.Run()) return out;
    auto a = c.Get<ue::TArray<void*>>("OutZoneMaps");
    for (int i = 0; a.data && i < a.num && i < 512; ++i) {
        void* z = ue::At<void*>(a.data + i, 0);
        if (ue::Alive(z)) out.push_back(z);
    }
    return out;
}

std::vector<void*> Planets() {
    std::vector<void*> out;
    void* gg = GameplayGlobals();
    void* galaxy = gg ? ue::Ptr(gg, "GalaxyMap") : nullptr;
    auto a = Arr(galaxy, "PlanetData");
    for (int i = 0; a.data && i < a.num && i < 64; ++i) {
        void* p = ue::At<void*>(a.data + i, 0);
        if (ue::Alive(p)) out.push_back(p);
    }
    return out;
}

void ComputeDlc() {
    if (dlcDone || st.time < dlcNext || !st.pc) return;
    dlcNext = st.time + 10.0;
    std::vector<void*> planets = Planets();
    if (planets.empty()) return;
    for (void* planet : planets) {
        for (void* z : ZoneMaps(planet, false)) {
            void* dlc = ue::Ptr(z, "AssociatedDLC");
            std::string f = dlc ? DlcFolder(ue::Path(dlc)) : DlcFolder(ue::Path(z));
            if (!f.empty()) dlcAll.insert(f);
        }
        for (void* z : ZoneMaps(planet, true)) {
            void* dlc = ue::Ptr(z, "AssociatedDLC");
            std::string f = dlc ? DlcFolder(ue::Path(dlc)) : DlcFolder(ue::Path(z));
            if (!f.empty()) dlcOwned.insert(f);
        }
    }
    dlcDone = true;
    std::string all, owned;
    for (auto& s : dlcAll) all += s + " ";
    for (auto& s : dlcOwned) owned += s + " ";
    LOG("DLC with maps: %s| owned: %s", all.c_str(), owned.c_str());
}

void BuildTravels() {
    travels.clear();
    for (void* planet : Planets()) {
        std::string planetName = TextAt(planet, "DisplayName");
        ue::Call c(Lib("PlanetData"), "GetPlanetFastTravelStations");
        c.Set<void*>("WorldContextObject", st.world).Set<void*>("Planet", planet).SetBool("FindOnlyDiscovered", true).Set<void*>("ViewingPlayer", st.pc);
        if (!c.Run()) continue;
        auto a = c.Get<ue::TArray<void*>>("OutFastTravelStations");
        for (int i = 0; a.data && i < a.num && i < 512; ++i) {
            void* s = ue::At<void*>(a.data + i, 0);
            if (!ue::Alive(s) || !DlcAllowed(ue::Path(s))) continue;
            std::string n = TextAt(s, "DisplayName");
            if (n.empty()) n = Clean(ue::Name(s));
            travels.push_back({ue::MakeWeak(s), (planetName.empty() ? std::string() : planetName + ": ") + n});
        }
    }
    std::sort(travels.begin(), travels.end(), [](const TravelSpot& a, const TravelSpot& b) { return a.name < b.name; });
}

void* CurrentZoneMap() {
    ue::Call lv(Lib("GameplayStatics"), "GetCurrentLevelName");
    lv.Set<void*>("WorldContextObject", st.world).SetBool("bRemovePrefixString", true);
    if (!lv.Run()) return nullptr;
    std::string level = ue::FStringToUtf8(lv.Get<ue::FString>("ReturnValue"));
    if (level.empty()) return nullptr;
    ue::Call z(Lib("OakBlueprintLibrary"), "GetZoneMapForLevel");
    z.Set<void*>("WorldContextObject", st.world).SetString("LevelName", ue::Wide(level));
    if (!z.Run()) return nullptr;
    void* zm = z.Get<void*>("ReturnValue");
    return ue::Alive(zm) ? zm : nullptr;
}

ue::FVector ComponentLocation(void* comp) {
    ue::Call c(comp, "K2_GetComponentLocation");
    return c.Run() ? c.Get<ue::FVector>("ReturnValue") : ue::FVector{};
}

void BuildPlaces() {
    std::vector<core::Location> places;
    fixedMarkers.clear();
    void* tracker = st.gameState ? ue::Ptr(st.gameState, "TravelStationTracker") : nullptr;
    auto stations = Arr(tracker, "FastTravelStations");
    for (int i = 0; stations.data && i < stations.num && i < 256; ++i) {
        void* comp = ue::At<void*>(stations.data + i, 0);
        if (!ue::Alive(comp)) continue;
        void* data = ue::Ptr(comp, "FastTravelData");
        std::string n = data ? TextAt(data, "DisplayName") : std::string();
        if (n.empty()) n = "Fast travel station";
        ue::FVector w = ComponentLocation(comp);
        float mx, my;
        ToMap(w, mx, my);
        places.push_back({"Station: " + n, mx, my, w.z / 100.0f + 1.5f});
        fixedMarkers.push_back({core::MarkerType::Base, n, mx, my, w.z / 100.0f, 0});
    }
    void* zone = CurrentZoneMap();
    void* poiStruct = Cls("ZoneMapPOIEntry");
    void* chalStruct = Cls("ZoneMapChallengeEntry");
    if (zone && poiStruct) {
        ue::Prop lp = ue::FindProp(ue::ClassOf(zone), "ZoneMapPOIList");
        auto list = ue::Read<ue::TArray<uint8_t>>(zone, lp);
        int size = ue::StructSize(poiStruct);
        ue::Prop typeP = ue::FindProp(poiStruct, "PointOfInterestType"), nameP = ue::FindProp(poiStruct, "PointOfInterestObjectName"),
                 xfP = ue::FindProp(poiStruct, "PointOfInterestTransform");
        for (int i = 0; lp && list.data && size > 0 && xfP && i < list.num && i < 512; ++i) {
            uint8_t* e = list.data + (size_t)i * size;
            void* type = ue::At<void*>(e, typeP.offset);
            std::string n = ue::Alive(type) ? TextAt(type, "Description") : std::string();
            if (n.empty() && nameP) n = Clean(ue::FStringToUtf8(ue::At<ue::FString>(e, nameP.offset)));
            if (n.empty()) continue;
            ue::FVector w = ue::At<ue::FVector>(e, xfP.offset + 0x10);
            float mx, my;
            ToMap(w, mx, my);
            places.push_back({n, mx, my, w.z / 100.0f + 1.5f});
            fixedMarkers.push_back({core::MarkerType::Wreck, n, mx, my, w.z / 100.0f, 0});
        }
    }
    if (zone && chalStruct) {
        ue::Prop lp = ue::FindProp(ue::ClassOf(zone), "ZoneMapChallengeList");
        auto list = ue::Read<ue::TArray<uint8_t>>(zone, lp);
        int size = ue::StructSize(chalStruct);
        ue::Prop nameP = ue::FindProp(chalStruct, "ChallengeActorName"), xfP = ue::FindProp(chalStruct, "ChallengeWaypointTransform");
        for (int i = 0; lp && list.data && size > 0 && xfP && i < list.num && i < 512; ++i) {
            uint8_t* e = list.data + (size_t)i * size;
            std::string n = nameP ? Clean(ue::FStringToUtf8(ue::At<ue::FString>(e, nameP.offset))) : std::string();
            if (n.empty()) n = "Crew challenge";
            ue::FVector w = ue::At<ue::FVector>(e, xfP.offset + 0x10);
            float mx, my;
            ToMap(w, mx, my);
            places.push_back({"Challenge: " + n, mx, my, w.z / 100.0f + 1.5f});
            fixedMarkers.push_back({core::MarkerType::DataBox, n, mx, my, w.z / 100.0f, 0});
        }
    }
    if (st.alone) {
        if (travels.empty()) BuildTravels();
        for (size_t i = 0; i < travels.size(); ++i) places.push_back({"Travel to " + travels[i].name, kTravelBase - (float)i * 10.0f, 0.0f, (float)i});
    }
    WithModel([&](core::Model& m) {
        m.places = std::move(places);
        m.mapDrawn = true;
    });
}

void PublishMarkers() {
    std::vector<core::Marker> markers = fixedMarkers;
    void* me = st.vehicle ? st.vehicle : st.pawn;
    ue::FVector w = Location(me);
    ue::FRotator r = st.pc ? ControlRotation() : Rotation(me);
    float mx, my;
    ToMap(w, mx, my);
    markers.push_back({core::MarkerType::Player, "You", mx, my, w.z / 100.0f, r.yaw + 90.0f});
    int n = 0;
    for (void* v : Objects(Scan::Vehicles)) {
        if (!ue::Alive(v) || v == st.vehicle || ++n > 24) continue;
        ue::FVector vw = Location(v);
        if (Dist(vw, w) > 100000.0f) continue;
        float vx, vy;
        ToMap(vw, vx, vy);
        markers.push_back({core::MarkerType::Vehicle, "Vehicle", vx, vy, vw.z / 100.0f, 0});
    }
    bool undo = haveUndo;
    WithModel([&](core::Model& m) {
        m.markers = std::move(markers);
        m.canUndoTeleport = undo;
    });
}

void DoTeleport(const Request& rq) {
    if (rq.undo) {
        if (!haveUndo) return;
        void* who = st.vehicle ? st.vehicle : st.pawn;
        if (st.vehicle) MoveVehicle(st.vehicle, undoPos, Rotation(st.vehicle));
        else Teleport(who, undoPos, Rotation(who));
        haveUndo = false;
        Note("Teleport undone");
        return;
    }
    if (rq.x <= kTravelBase + 1.0f) {
        size_t idx = (size_t)std::max(0l, std::lround(rq.z));
        if (!st.alone) {
            Note("Travel to another map only works when you play alone");
            return;
        }
        if (idx >= travels.size()) return;
        void* station = ue::WeakGet(travels[idx].station);
        if (!station) {
            Note("That station is not available right now");
            return;
        }
        ue::Call c(Lib("FastTravelStationComponent"), "FastTravelToStation");
        c.Set<void*>("WorldContextObject", st.world).Set<void*>("DestinationStationData", station).Set<void*>("ActivatingPawn", st.pawn);
        if (c.Run()) Note("Travelling to " + travels[idx].name);
        else Note("The game did not allow that trip right now");
        return;
    }
    ue::FVector target = FromMap(rq.x, rq.y, rq.z);
    if (!rq.useZ) {
        ue::FVector ground;
        void* me = st.vehicle ? st.vehicle : st.pawn;
        ue::FVector cur = Location(me);
        if (TraceDown({target.x, target.y, cur.z + 100000.0f}, 300000.0f, ground)) target.z = ground.z + 120.0f;
        else target.z = cur.z + 200.0f;
    }
    if (st.vehicle) {
        if (VehicleHasOtherPlayers(st.vehicle)) {
            Note("Someone else is in your car, teleport skipped");
            return;
        }
        undoPos = Location(st.vehicle);
        if (MoveVehicle(st.vehicle, {target.x, target.y, target.z + 150.0f}, Rotation(st.vehicle))) haveUndo = true;
        return;
    }
    undoPos = Location(st.pawn);
    if (Teleport(st.pawn, target, Rotation(st.pawn))) haveUndo = true;
    else Note("Could not teleport there");
}

}

bool DlcAllowed(const std::string& path) {
    std::string f = DlcFolder(path);
    if (f.empty()) return true;
    bool paid = dlcAll.count(f) > 0;
    for (const char* p : kPaidDlc)
        if (f == p) paid = true;
    if (!paid) return true;
    return dlcOwned.count(f) > 0;
}

bool DlcKnown() { return dlcDone; }

void WorldInit(core::Model& m) {
    bool tp = HasFunc("Actor", "K2_TeleportTo");
    Ready("map", tp);
    WithModel([](core::Model& mm) {
        mm.teleport = [](float x, float y, float z, bool useZ) {
            std::lock_guard<std::mutex> g(reqLock);
            requests.push_back({x, y, z, useZ, false});
        };
        mm.undoTeleport = [] {
            std::lock_guard<std::mutex> g(reqLock);
            requests.push_back({0, 0, 0, false, true});
        };
    });
    (void)m;
}

void WorldTick() {
    ComputeDlc();
    if (st.world != lastWorld) {
        lastWorld = st.world;
        placesAt = st.time - 100.0;
        travels.clear();
        haveUndo = false;
        fixedMarkers.clear();
    }
    bool show = st.mapVisible || st.menuOpen;
    if (show && st.time - placesAt > 5.0) {
        placesAt = st.time;
        BuildPlaces();
    }
    if (show && st.time - markersAt > 0.2) {
        markersAt = st.time;
        PublishMarkers();
    }
    std::vector<Request> todo;
    {
        std::lock_guard<std::mutex> g(reqLock);
        todo.swap(requests);
    }
    for (const Request& r : todo) DoTeleport(r);
}

void WorldOff() {
    std::lock_guard<std::mutex> g(reqLock);
    requests.clear();
}

}
