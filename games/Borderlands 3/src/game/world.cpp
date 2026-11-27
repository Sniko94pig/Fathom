#include "game/internal.h"
#include "ingame/log.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
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
ue::FWeak lastWorld;
std::vector<core::Marker> fixedMarkers;
const float kTravelBase = -9000000.0f;
const float kMissionZ = -1000000.0f;

struct MissionSpot {
    ue::FVector at;
    std::string label;
    void* owner = nullptr;
};

std::vector<MissionSpot> missionSpots;
std::string missionWhy;
double missionAt = -100;
std::map<std::pair<void*, void*>, std::string> missionLabels;

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

bool Usable(const ue::FVector& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) && std::fabs(v.x) + std::fabs(v.y) + std::fabs(v.z) > 1.0f &&
           std::fabs(v.x) < 2.0e7f && std::fabs(v.y) < 2.0e7f && std::fabs(v.z) < 2.0e7f;
}

float BelowCeiling(const ue::FVector& p, float up) {
    TraceHitInfo h;
    if (!TraceHit({p.x, p.y, p.z + 10.0f}, {p.x, p.y, p.z + up}, h)) return p.z + up;
    if (h.inside) return p.z;
    return std::max(p.z + 10.0f, h.at.z - 20.0f);
}

bool FloorNear(float x, float y, float refZ, ue::FVector& ground) {
    float tops[2] = {BelowCeiling({x, y, refZ + 40.0f}, 260.0f), refZ + 10.0f};
    for (float top : tops) {
        TraceHitInfo h;
        if (!TraceHit({x, y, top}, {x, y, top - 6000.0f}, h) || h.inside || h.at.z > refZ + 100.0f) continue;
        ground = h.at;
        return true;
    }
    return false;
}

std::string PlainText(const std::string& s) {
    std::string out;
    int depth = 0;
    for (char c : s) {
        if (c == '[' || c == '<') {
            ++depth;
            continue;
        }
        if ((c == ']' || c == '>') && depth > 0) {
            --depth;
            continue;
        }
        if (depth == 0 && c != '\r' && c != '\n') out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    size_t start = out.find_first_not_of(' ');
    if (start == std::string::npos || out.find('{') != std::string::npos) return {};
    return out.substr(start);
}

std::string FormattedText(void* obj, const char* prop) {
    void* fmt = Cls("GbxUIFormattedText");
    ue::Prop p = obj ? ue::FindProp(ue::ClassOf(obj), prop) : ue::Prop{};
    ue::Prop t = fmt ? ue::FindProp(fmt, "FormatText") : ue::Prop{};
    if (!p || !t) return {};
    return PlainText(ue::TextToString((uint8_t*)obj + p.offset + t.offset));
}

void* MissionTracker() {
    void* comp = st.pc ? ue::Ptr(st.pc, "PlayerMissionComponent") : nullptr;
    void* t = ue::Alive(comp) ? ue::Ptr(comp, "CachedMissionTracker") : nullptr;
    if (!ue::Alive(t) && st.gameState) t = ue::Ptr(st.gameState, "MissionTracker");
    return ue::Alive(t) ? t : nullptr;
}

void* TrackedMission(void* tracker) {
    void* cls = ue::Ptr(tracker, "TrackedMission");
    if (ue::Alive(cls)) return cls;
    ue::Call c(Lib("MissionFunctionLibrary"), "GetCurrentlyTrackedMission");
    c.Set<void*>("WorldContextObject", st.pc);
    cls = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
    return ue::Alive(cls) ? cls : nullptr;
}

std::string MissionLabel(void* mission, void* objective) {
    auto key = std::make_pair(mission, objective);
    auto it = missionLabels.find(key);
    if (it != missionLabels.end()) return it->second;
    std::string s = objective ? FormattedText(objective, "FormattedProgressMessage") : std::string();
    bool settled = true;
    if (s.empty()) {
        ue::Call c(Lib("MissionFunctionLibrary"), "GetMission");
        c.Set<void*>("MissionClass", mission).Set<void*>("WorldContextObject", st.pc);
        void* m = c.Run() ? c.Get<void*>("ReturnValue") : nullptr;
        if (ue::Alive(m)) s = FormattedText(m, "FormattedMissionName");
        else settled = false;
    }
    if (s.empty()) s = "mission waypoint";
    if (!settled) return s;
    if (missionLabels.size() > 256) missionLabels.clear();
    missionLabels[key] = s;
    return s;
}

std::vector<MissionSpot> FindMissionSpots(std::string& why) {
    std::vector<MissionSpot> out;
    void* tracker = MissionTracker();
    if (!tracker) {
        why = "Your missions are not loaded yet";
        return out;
    }
    void* tracked = TrackedMission(tracker);
    if (!tracked) {
        why = "You are not tracking a mission, pick one in the mission log first";
        return out;
    }
    void* iconStruct = Cls("ActiveMissionIconData");
    ue::Prop ip = ue::FindProp(ue::ClassOf(tracker), "ActiveIcons");
    int size = iconStruct ? ue::StructSize(iconStruct) : 0;
    ue::Prop compP = iconStruct ? ue::FindProp(iconStruct, "IconLocationComponent") : ue::Prop{};
    ue::Prop locP = iconStruct ? ue::FindProp(iconStruct, "IconLocation") : ue::Prop{};
    ue::Prop misP = iconStruct ? ue::FindProp(iconStruct, "MissionClass") : ue::Prop{};
    ue::Prop objP = iconStruct ? ue::FindProp(iconStruct, "MissionObjectives") : ue::Prop{};
    if (ip && locP && misP && size >= 0x30 && size <= 0x200) {
        auto list = ue::Read<ue::TArray<uint8_t>>(tracker, ip);
        for (int i = 0; list.data && i < list.num && i < 128; ++i) {
            const uint8_t* e = list.data + (size_t)i * size;
            if (ue::At<void*>(e, misP.offset) != tracked) continue;
            ue::FVector at = ue::At<ue::FVector>(e, locP.offset);
            void* owner = nullptr;
            if (compP) {
                ue::FWeak w{ue::At<int32_t>(e, compP.offset), ue::At<int32_t>(e, compP.offset + 4), nullptr};
                void* comp = w.serial ? ue::WeakGet(w) : nullptr;
                if (comp) {
                    ue::FVector c = ComponentLocation(comp);
                    if (Usable(c)) at = c;
                    ue::Call o(comp, "GetOwner");
                    if (o.Run()) owner = o.Get<void*>("ReturnValue");
                }
            }
            if (!Usable(at)) continue;
            void* objective = nullptr;
            if (objP) {
                auto objs = ue::At<ue::TArray<void*>>(e, objP.offset);
                if (objs.data && objs.num > 0 && objs.num < 64) objective = ue::At<void*>(objs.data, 0);
            }
            out.push_back({at, MissionLabel(tracked, ue::Alive(objective) ? objective : nullptr), owner});
        }
    }
    if (out.empty()) {
        auto ways = Arr(tracker, "CurrentOutOfMapWaypoints");
        for (int i = 0; ways.data && i < ways.num && i < 32; ++i) {
            void* comp = ue::At<void*>(ways.data + i, 0);
            if (!ue::Alive(comp)) continue;
            void* mc = ue::Ptr(comp, "MissionClass");
            if (mc && mc != tracked) continue;
            ue::FVector at = ComponentLocation(comp);
            if (!Usable(at)) continue;
            ue::Call o(comp, "GetOwner");
            void* owner = o.Run() ? o.Get<void*>("ReturnValue") : nullptr;
            out.push_back({at, "way to " + MissionLabel(tracked, nullptr), owner});
        }
    }
    if (out.empty()) why = "Your mission has no waypoint here right now";
    return out;
}

const std::vector<MissionSpot>& MissionSpots() {
    if (st.time - missionAt > 0.5 || st.time < missionAt) {
        missionAt = st.time;
        missionSpots = FindMissionSpots(missionWhy);
    }
    return missionSpots;
}

std::vector<core::Location> MissionRows() {
    const std::vector<MissionSpot>& spots = MissionSpots();
    if (spots.empty()) return {};
    void* me = st.vehicle ? st.vehicle : st.pawn;
    ue::FVector w = me ? Location(me) : ue::FVector{};
    const MissionSpot* best = &spots[0];
    for (const MissionSpot& s : spots)
        if (std::hypot(s.at.x - w.x, s.at.y - w.y) < std::hypot(best->at.x - w.x, best->at.y - w.y)) best = &s;
    float mx, my;
    ToMap(best->at, mx, my);
    return {{"Mission: " + best->label, mx, my, kMissionZ}};
}

bool IsMissionRow(const core::Location& l) { return l.z < kMissionZ * 0.5f; }

void BuildPlaces() {
    std::vector<core::Location> places = MissionRows();
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
            fixedMarkers.push_back({core::MarkerType::Place, n, mx, my, w.z / 100.0f, 0});
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
    for (const MissionSpot& s : MissionSpots()) {
        float sx, sy;
        ToMap(s.at, sx, sy);
        markers.push_back({core::MarkerType::Beacon, "Mission: " + s.label, sx, sy, kMissionZ, 0});
    }
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
    std::vector<core::Location> rows = MissionRows();
    WithModel([&](core::Model& m) {
        m.markers = std::move(markers);
        m.canUndoTeleport = undo;
        if (!m.mapDrawn) return;
        m.places.erase(std::remove_if(m.places.begin(), m.places.end(), IsMissionRow), m.places.end());
        m.places.insert(m.places.begin(), rows.begin(), rows.end());
    });
}

struct Landing {
    bool active = false;
    ue::FWeak pawn;
    double until = 0;
    float x = 0, y = 0;
    bool fromTop = false;
    float top = 0;
} landing;

void SetMode(uint8_t mode) {
    void* mc = st.pawn ? ue::Ptr(st.pawn, "CharacterMovement") : nullptr;
    ue::Call c(mc, "SetMovementMode");
    c.Set<uint8_t>("NewMovementMode", mode).Set<uint8_t>("NewCustomMode", 0);
    c.Run();
    ue::Prop vp = mc ? ue::FindProp(ue::ClassOf(mc), "Velocity") : ue::Prop{};
    if (vp) ue::Write<ue::FVector>(mc, vp, ue::FVector{});
}

void StepLanding() {
    if (!landing.active) return;
    if (ue::WeakGet(landing.pawn) != st.pawn || !st.pawn || st.vehicle) {
        landing.active = false;
        return;
    }
    ue::FVector cur = Location(st.pawn), ground;
    bool found = landing.fromTop ? FloorNear(landing.x, landing.y, landing.top, ground)
                                 : TraceDown({landing.x, landing.y, cur.z + 50000.0f}, 200000.0f, ground);
    if (found) {
        Teleport(st.pawn, {landing.x, landing.y, ground.z + 120.0f}, Rotation(st.pawn));
        SetMode(3);
        landing.active = false;
        return;
    }
    if (st.time > landing.until) {
        SetMode(3);
        landing.active = false;
        Note("The ground there did not load, you may fall");
        return;
    }
    SetMode(5);
}

bool FaceFor(const ue::FVector& from, const ue::FVector* lookAt, ue::FRotator& out) {
    if (!lookAt || std::hypot(from.x - lookAt->x, from.y - lookAt->y) < 10.0f) return false;
    out = LookAt(from, *lookAt);
    out.pitch = 0.0f;
    out.roll = 0.0f;
    return true;
}

bool MoveMe(const std::vector<ue::FVector>& targets, const ue::FVector* lookAt, bool grounded, bool fromTop, float top) {
    if (targets.empty()) return false;
    if (st.vehicle) {
        if (VehicleHasOtherPlayers(st.vehicle)) {
            Note("Someone else is in your car, teleport skipped");
            return false;
        }
        if (!grounded) {
            Note("That spot is not loaded yet, get closer or get out of the car first");
            return false;
        }
        ue::FVector before = Location(st.vehicle);
        for (const ue::FVector& t : targets) {
            ue::FRotator rot;
            if (!FaceFor(t, lookAt, rot)) rot = Rotation(st.vehicle);
            if (!MoveVehicle(st.vehicle, {t.x, t.y, t.z + 150.0f}, rot)) continue;
            undoPos = before;
            haveUndo = true;
            landing.active = false;
            return true;
        }
        Note("Could not move your car there");
        return false;
    }
    if (!st.pawn) return false;
    ue::FVector before = Location(st.pawn);
    for (const ue::FVector& t : targets) {
        ue::FRotator rot;
        bool turn = FaceFor(t, lookAt, rot);
        if (!turn) rot = Rotation(st.pawn);
        if (!Teleport(st.pawn, t, rot)) continue;
        undoPos = before;
        haveUndo = true;
        if (turn) SetControlRotation(rot);
        bool wasLanding = landing.active;
        landing.active = false;
        if (!grounded) {
            landing = {true, ue::MakeWeak(st.pawn), st.time + 8.0, t.x, t.y, fromTop, top};
            SetMode(5);
        } else if (wasLanding) {
            SetMode(3);
        }
        return true;
    }
    Note("Could not teleport there");
    return false;
}

void MissionTeleport(const ue::FVector& near, bool announce) {
    missionAt = -100;
    const std::vector<MissionSpot>& spots = MissionSpots();
    if (spots.empty()) {
        Note(missionWhy);
        return;
    }
    const MissionSpot* best = &spots[0];
    float bestD = 1e30f;
    for (const MissionSpot& s : spots) {
        float d = std::hypot(s.at.x - near.x, s.at.y - near.y);
        if (d < bestD) {
            bestD = d;
            best = &s;
        }
    }
    MissionSpot spot = *best;
    void* me = st.vehicle ? st.vehicle : st.pawn;
    if (!me) return;
    ue::FVector cur = Location(me), icon = spot.at, floor;
    std::vector<ue::FVector> targets;
    void* pawnCls = Cls("Pawn");
    if (FloorNear(icon.x, icon.y, icon.z, floor)) {
        float step = st.vehicle ? 600.0f : 250.0f;
        float base = std::hypot(cur.x - icon.x, cur.y - icon.y) > 1.0f ? std::atan2(cur.y - icon.y, cur.x - icon.x) : 0.0f;
        ue::FVector eye{icon.x, icon.y, floor.z + 100.0f};
        for (int k = 0; k < 8 && targets.size() < 2; ++k) {
            float a = base + (k % 2 ? 1.0f : -1.0f) * (float)((k + 1) / 2) * 0.7853982f;
            ue::FVector side{icon.x + std::cos(a) * step, icon.y + std::sin(a) * step, floor.z + 100.0f}, g;
            TraceHitInfo h;
            if (TraceHit(side, eye, h)) {
                bool target = !h.inside && h.actor && (h.actor == spot.owner || (pawnCls && ue::IsA(h.actor, pawnCls))) &&
                              std::hypot(h.at.x - icon.x, h.at.y - icon.y) < 200.0f;
                if (!target) continue;
            }
            if (!FloorNear(side.x, side.y, floor.z + 60.0f, g) || std::fabs(g.z - floor.z) > 150.0f) continue;
            targets.push_back({side.x, side.y, g.z + 120.0f});
        }
        targets.push_back({icon.x, icon.y, floor.z + 120.0f});
    }
    bool grounded = !targets.empty();
    if (!grounded) targets.push_back({icon.x, icon.y, icon.z + 200.0f});
    LOG("mission teleport: %s at %.0f %.0f %.0f, %s, %zu spots to try, %zu waypoints", spot.label.c_str(), icon.x, icon.y, icon.z,
        grounded ? "on the ground" : "ground not loaded", targets.size(), spots.size());
    if (MoveMe(targets, &icon, grounded, true, icon.z) && announce) Note("Teleported to " + spot.label);
}

void DoTeleport(const Request& rq) {
    if (rq.undo) {
        if (!haveUndo) return;
        if (st.vehicle) {
            if (VehicleHasOtherPlayers(st.vehicle)) {
                Note("Someone else is in your car, undo skipped");
                return;
            }
            MoveVehicle(st.vehicle, undoPos, Rotation(st.vehicle));
        } else {
            Teleport(st.pawn, undoPos, Rotation(st.pawn));
        }
        if (landing.active) {
            landing.active = false;
            SetMode(3);
        }
        haveUndo = false;
        Note("Teleport undone");
        return;
    }
    if (rq.useZ && rq.z < kMissionZ * 0.5f) {
        MissionTeleport(FromMap(rq.x, rq.y, 0.0f), false);
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
    bool grounded = true;
    if (!rq.useZ) {
        ue::FVector ground;
        void* me = st.vehicle ? st.vehicle : st.pawn;
        ue::FVector cur = Location(me);
        if (TraceDown({target.x, target.y, cur.z + 100000.0f}, 300000.0f, ground)) {
            target.z = ground.z + 120.0f;
        } else {
            target.z = cur.z + 300.0f;
            grounded = false;
        }
    }
    MoveMe({target}, nullptr, grounded, false, 0.0f);
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

bool DlcOwnedPath(const std::string& path) {
    std::string f = DlcFolder(path);
    return !f.empty() && dlcOwned.count(f) > 0;
}

void WorldInit(core::Model& m) {
    bool tp = HasFunc("Actor", "K2_TeleportTo");
    Ready("map", tp);
    Ready("teleport_mission", tp && Has("OakPlayerController", "PlayerMissionComponent") && Has("PlayerMissionComponent", "CachedMissionTracker") &&
                                  Has("MissionTrackerBase", "TrackedMission") && Has("MissionTracker", "ActiveIcons") &&
                                  Has("ActiveMissionIconData", "IconLocation") && Has("ActiveMissionIconData", "MissionClass"));
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

void ResetWorldState() {
    lastWorld = {};
    placesAt = -100;
    travels.clear();
    haveUndo = false;
    fixedMarkers.clear();
    landing.active = false;
    missionSpots.clear();
    missionLabels.clear();
    missionAt = -100;
    WithModel([](core::Model& m) {
        m.places.clear();
        m.markers.clear();
        m.canUndoTeleport = false;
    });
}

void WorldTick() {
    ComputeDlc();
    ue::FWeak w = ue::MakeWeak(st.world);
    if (w.index != lastWorld.index || w.serial != lastWorld.serial) {
        ResetWorldState();
        lastWorld = w;
    }
    StepLanding();
    bool show = st.mapVisible || st.menuOpen;
    if (show && (placesAt < 0 || st.time - placesAt > 60.0)) {
        placesAt = st.time;
        BuildPlaces();
    }
    if (show && st.time - markersAt > 0.2) {
        markersAt = st.time;
        PublishMarkers();
    }
    if (Pressed("teleport_mission")) {
        void* me = st.vehicle ? st.vehicle : st.pawn;
        if (me) MissionTeleport(Location(me), true);
    }
    std::vector<Request> todo;
    {
        std::lock_guard<std::mutex> g(reqLock);
        todo.swap(requests);
    }
    for (const Request& r : todo) DoTeleport(r);
}

void WorldOff() {
    if (lastWorld.index != 0 || haveUndo || landing.active) ResetWorldState();
    std::lock_guard<std::mutex> g(reqLock);
    requests.clear();
}

}
