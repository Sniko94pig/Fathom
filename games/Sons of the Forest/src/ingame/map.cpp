#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include "ingame/unity.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <climits>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

namespace sotf {

namespace {

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;
using un::Vec2;
using un::Vec3;

const char* kSons = "Sons";
const char* kCoreAsm = "UnityEngine.CoreModule";
const char* kUiAsm = "UnityEngine.UI";
const float kWorld = 2048.0f;
const int kRelief = 256;

struct URect {
    float x = 0, y = 0, w = 0, h = 0;
};
static_assert(sizeof(URect) == 16, "Rect is 16 bytes");

struct Refs {
    Class* localPlayer = nullptr;
    Method *lpTransform = nullptr, *lpCamTr = nullptr, *lpStats = nullptr, *lpFpc = nullptr, *lpView = nullptr, *lpInCaves = nullptr,
           *lpEndgame = nullptr, *lpGolfCart = nullptr, *lpGliding = nullptr, *lpRaft = nullptr, *lpSpecial = nullptr,
           *lpCaveCheck = nullptr, *lpTeleport = nullptr;
    Class* stats = nullptr;
    Method *statsDead = nullptr, *statsDying = nullptr;
    Class* cutscenes = nullptr;
    Method* cutscenePlaying = nullptr;
    Class* knightV = nullptr;
    Method* knightActive = nullptr;
    Class* fpc = nullptr;
    Method *fpcClearFall = nullptr, *fpcSetGrounded = nullptr, *fpcGrounded = nullptr;
    Fld fpcPrevGrounded;
    Class* terrain = nullptr;
    Method *terrainHeight = nullptr, *terrainInside = nullptr;
    Class* golfCart = nullptr;
    Class *caveEntrance = nullptr, *areaMask = nullptr;
    Fld ceArea;
    Class *worldSim = nullptr, *zoneBase = nullptr, *village = nullptr;
    Fld wsInstance, wsZones, zbName;
    Method* villageCenter = nullptr;
    Class *gps = nullptr, *graphic = nullptr, *rawImage = nullptr, *image = nullptr, *sprite = nullptr, *rectTransform = nullptr;
    Fld gpsRoot, gpsSurface, gpsWorldSize;
    Method *gpsList = nullptr, *rtRect = nullptr, *rtCorners = nullptr, *trInverse = nullptr, *rawUv = nullptr, *imgSprite = nullptr,
           *spriteRect = nullptr;
    Class *renderTexture = nullptr, *graphics = nullptr, *texture2d = nullptr, *imageConversion = nullptr;
    Method *rtGet = nullptr, *rtRelease = nullptr, *rtActiveGet = nullptr, *rtActiveSet = nullptr, *blit = nullptr, *tex2dCtor = nullptr,
           *readPixels = nullptr, *encodePng = nullptr;
} R;

struct Values {
    int viewWorld = 2, viewInventory = 3;
} K;

std::vector<std::pair<int, std::string>> areaNames;
bool canTeleport = false, canGps = false, canRelief = false;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;

struct Request {
    bool undo = false;
    bool useZ = false;
    float x = 0, y = 0, z = 0;
};

std::mutex qLock;
std::vector<Request> requests;

struct Place {
    core::Location loc;
    bool ground = false;
};

std::vector<Place> places;
std::vector<uint32_t> carts;
std::vector<core::Marker> pendMarkers;
double nextPlaces = 0, nextCarts = 0, nextMarkers = 0;
bool pubPlaces = false, pubMarkers = false, pubUndo = false, undoState = false;

struct Spot {
    bool valid = false;
    Vec3 pos;
} undo;

struct Arrival {
    bool active = false;
    bool ground = false;
    int fixes = 0;
    double start = 0;
    double groundedSince = 0;
} arrival;

enum class Pic { Wait, Gps, Relief, Done };
Pic pic = Pic::Wait;
double picStart = 0, picNext = 0;
int picTries = 0;
std::vector<float> reliefH;
std::vector<uint8_t> reliefIn;
int reliefRow = 0;
bool pubCalib = false;
float calU[2] = {0, 0}, calV[2] = {0, 0}, calX[2] = {0, 0}, calY[2] = {0, 0};

int insideState = 0;
Object* lastPlayer = nullptr;
int playerId = 0;

Method* StaticM(Class* c, const char* name, int args) {
    if (!c) return nullptr;
    Method* m = il2::FindStaticMethod(c, name, args);
    if (!m) un::Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
    return m;
}

bool Flag(Method* m) { return m && un::CallBool(m, nullptr); }

Object* PlayerTr(const Ctx& c) {
    Object* t = R.lpTransform ? un::Call(R.lpTransform, nullptr) : nullptr;
    if (un::Alive(t)) return t;
    return un::TransformOf(c.player);
}

Object* Fpc() {
    Object* f = R.lpFpc ? un::Call(R.lpFpc, nullptr) : nullptr;
    return un::Alive(f) ? f : nullptr;
}

std::string Pretty(const std::string& raw) {
    std::string s;
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i] == '_' || raw[i] == '-' || raw[i] == '.' ? ' ' : raw[i];
        unsigned char prev = i > 0 ? (unsigned char)raw[i - 1] : 0;
        bool split = i > 0 && ((std::isupper((unsigned char)ch) && std::islower(prev)) ||
                               (std::isdigit((unsigned char)ch) && std::isalpha(prev)) ||
                               (std::isalpha((unsigned char)ch) && std::isdigit(prev)));
        if (split) s += ' ';
        s += ch;
    }
    std::string out;
    for (char ch : s) {
        if (ch == ' ' && (out.empty() || out.back() == ' ')) continue;
        out += ch;
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (!out.empty()) out[0] = (char)std::toupper((unsigned char)out[0]);
    return out;
}

bool Contains(const std::string& s, const char* part) {
    std::string a = s, b = part;
    for (char& ch : a) ch = (char)std::tolower((unsigned char)ch);
    for (char& ch : b) ch = (char)std::tolower((unsigned char)ch);
    return a.find(b) != std::string::npos;
}

bool WriteBytes(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != (DWORD)n || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

void SetUndo(bool v) {
    undoState = v;
    pubUndo = true;
}

void Publish() {
    if (!model || !modelLock) return;
    if (!pubPlaces && !pubMarkers && !pubUndo && !pubCalib) return;
    std::unique_lock<std::mutex> g(*modelLock, std::try_to_lock);
    if (!g.owns_lock()) return;
    if (pubUndo) model->canUndoTeleport = undoState;
    if (pubPlaces) {
        std::vector<core::Location> list;
        list.reserve(places.size());
        for (const Place& p : places) list.push_back(p.loc);
        model->places.swap(list);
    }
    if (pubMarkers) model->markers.swap(pendMarkers);
    if (pubCalib) {
        model->calibPoints = 2;
        for (int i = 0; i < 2; ++i) {
            model->calibU[i] = calU[i];
            model->calibV[i] = calV[i];
            model->calibX[i] = calX[i];
            model->calibY[i] = calY[i];
        }
    }
    pendMarkers.clear();
    pubUndo = pubPlaces = pubMarkers = pubCalib = false;
}

bool TerrainAt(float x, float z, float& h) {
    if (!R.terrainHeight) return false;
    Vec3 p{x, 0.0f, z};
    bool ok = false;
    Object* r = un::Call(R.terrainHeight, nullptr, {&p}, &ok);
    if (!ok || !r) return false;
    h = il2::Result<float>(r, 0.0f);
    return std::isfinite(h) && std::fabs(h) < 100000.0f;
}

bool InsideRaw(float x, float y, float z) {
    Vec3 p{x, y, z};
    return un::CallBool(R.terrainInside, nullptr, {&p}, true);
}

bool Inside(const Ctx& c, float x, float y, float z) {
    if (!R.terrainInside || insideState < 0) return true;
    if (insideState == 0) {
        Object* tr = PlayerTr(c);
        if (!tr) return true;
        Vec3 p = un::Position(tr);
        float h = 0;
        if (!TerrainAt(p.x, p.z, h)) return true;
        insideState = InsideRaw(p.x, h, p.z) ? 1 : -1;
        if (insideState < 0) {
            LOG("map: island bounds check ignored");
            return true;
        }
    }
    return InsideRaw(x, y, z);
}

float SurfaceAt(float x, float z, float h) {
    un::RayHit hit;
    Vec3 origin{x, h + 61.0f, z}, down{0.0f, -1.0f, 0.0f};
    if (un::Raycast(origin, down, 62.0f, hit, ~(1 << 2), 1) && hit.normal.y > 0.6f && hit.point.y > h + 0.3f && hit.point.y < h + 60.0f)
        return hit.point.y;
    return h;
}

bool KnightVRiding() {
    if (!R.knightV || !R.knightActive || !R.lpSpecial) return false;
    Object* go = un::Call(R.lpSpecial, nullptr);
    if (!un::Alive(go)) return false;
    Object* a = un::CompInChildren(go, R.knightV, true);
    return a && un::CallBool(R.knightActive, a);
}

const char* Blocked(const Ctx& c) {
    if (!c.inWorld || !c.player) return "Load a save first";
    if (c.session == core::Session::Client) return "Teleport is off when you join someone else's game";
    Object* st = R.lpStats ? un::Call(R.lpStats, nullptr) : nullptr;
    if (un::Alive(st)) {
        if (R.statsDead && un::CallBool(R.statsDead, st)) return "Not while you are dead";
        if (R.statsDying && un::CallBool(R.statsDying, st)) return "Not while you are knocked down";
    }
    if (Flag(R.cutscenePlaying)) return "Wait until the cutscene ends";
    if (Flag(R.lpEndgame)) return "Not during the ending";
    if (Flag(R.lpGolfCart)) return "Get out of the golf cart first";
    if (Flag(R.lpGliding)) return "Land the hang glider first";
    if (R.lpRaft && un::Alive(un::Call(R.lpRaft, nullptr))) return "Get off the raft first";
    if (KnightVRiding()) return "Get off the Knight V first";
    if (R.lpView) {
        int v = un::CallInt(R.lpView, nullptr, {}, K.viewWorld);
        if (v == K.viewInventory) return "Close the inventory first";
        if (v != K.viewWorld) return "Wait until you can move again";
    }
    return nullptr;
}

void Protect() {
    Object* f = Fpc();
    if (!f || !un::Enabled(f)) return;
    if (R.fpcClearFall) un::Call(R.fpcClearFall, f);
    if (R.fpcSetGrounded) {
        bool no = false;
        un::Call(R.fpcSetGrounded, f, {&no});
    }
    if (R.fpcPrevGrounded) un::SetVal<bool>(f, R.fpcPrevGrounded, false);
}

void MoveTo(Vec3 p, bool ground, double now) {
    if (R.lpCaveCheck) {
        bool canExit = true;
        un::Call(R.lpCaveCheck, nullptr, {&p, &canExit});
    }
    un::Call(R.lpTeleport, nullptr, {&p});
    Protect();
    arrival = Arrival{};
    arrival.active = true;
    arrival.ground = ground;
    arrival.start = now;
}

const Place* PlaceAt(float x, float y) {
    for (const Place& p : places)
        if (std::fabs(p.loc.x - x) < 0.5f && std::fabs(p.loc.y - y) < 0.5f) return &p;
    return nullptr;
}

bool CartNear(Vec3 t) {
    for (uint32_t h : carts) {
        Object* o = il2::Pinned(h);
        if (!un::Alive(o)) continue;
        Vec3 p = un::Position(o);
        if (std::hypot(p.x - t.x, p.z - t.z) < 2.0f && std::fabs(p.y - t.y) < 3.0f) return true;
    }
    return false;
}

void DoTeleport(const Ctx& c, const Request& r, double now) {
    if (const char* why = Blocked(c)) {
        Note(why);
        return;
    }
    Object* tr = PlayerTr(c);
    if (!tr) {
        Note("Wait until you can move again");
        return;
    }
    Vec3 from = un::Position(tr);
    Vec3 target{r.x, r.z, -r.y};
    bool ground = !r.useZ;
    if (r.useZ) {
        if (const Place* p = PlaceAt(r.x, r.y)) ground = p->ground;
        if (CartNear(target)) {
            target.x += 3.0f;
            ground = true;
        }
    }
    if (!std::isfinite(target.x) || !std::isfinite(target.y) || !std::isfinite(target.z)) return;
    if (ground) {
        float h = 0;
        if (!TerrainAt(target.x, target.z, h)) {
            Note("Could not find the ground there");
            return;
        }
        if (!Inside(c, target.x, h, target.z)) {
            Note("That spot is outside the island");
            return;
        }
        target.y = SurfaceAt(target.x, target.z, h) + 1.0f;
    }
    undo.valid = true;
    undo.pos = from;
    SetUndo(true);
    MoveTo(target, ground, now);
    LOG("map: teleported to %.0f %.0f %.0f", target.x, target.y, target.z);
}

void DoUndo(const Ctx& c, double now) {
    if (!undo.valid) {
        SetUndo(false);
        Note("Nothing to undo");
        return;
    }
    if (const char* why = Blocked(c)) {
        Note(why);
        return;
    }
    Vec3 back = undo.pos;
    undo = Spot{};
    SetUndo(false);
    MoveTo(back, false, now);
}

void StepArrival(const Ctx& c, double now) {
    if (!arrival.active) return;
    double t = now - arrival.start;
    Object* f = Fpc();
    bool live = f && un::Enabled(f);
    bool grounded = live && R.fpcGrounded && un::CallBool(R.fpcGrounded, f);
    if (live && !grounded && R.fpcClearFall) un::Call(R.fpcClearFall, f);
    if (arrival.ground && arrival.fixes < 4 && t > 0.4 && t < 6.0 && !Flag(R.lpInCaves)) {
        Object* tr = PlayerTr(c);
        float h = 0;
        if (tr) {
            Vec3 p = un::Position(tr);
            if (TerrainAt(p.x, p.z, h) && p.y < h - 2.0f) {
                Vec3 q{p.x, SurfaceAt(p.x, p.z, h) + 1.0f, p.z};
                un::Call(R.lpTeleport, nullptr, {&q});
                Protect();
                ++arrival.fixes;
                LOG("map: lifted the player back above the ground");
            }
        }
    }
    if (grounded && t > 0.3) {
        if (arrival.groundedSince <= 0) arrival.groundedSince = now;
    } else {
        arrival.groundedSince = 0;
    }
    bool settled = arrival.groundedSince > 0 && now - arrival.groundedSince > 0.5;
    if ((settled && (!arrival.ground || t > 6.0)) || t > 12.0) arrival = Arrival{};
}

void DropCarts() {
    for (uint32_t h : carts) il2::Unpin(h);
    carts.clear();
}

void RefreshCarts() {
    DropCarts();
    if (!R.golfCart) return;
    for (Object* o : un::FindAll(R.golfCart, false))
        if (uint32_t h = il2::Weak(o)) carts.push_back(h);
}

std::string AreaName(int value) {
    for (const auto& [v, n] : areaNames)
        if (v == value) return n;
    return {};
}

void BuildPlaces() {
    std::vector<Place> out;
    if (R.caveEntrance && R.ceArea) {
        struct Ent {
            std::string name;
            Vec3 pos;
        };
        std::vector<Ent> ents;
        for (Object* e : un::FindAll(R.caveEntrance, true)) {
            std::string name = AreaName(un::Val<int>(e, R.ceArea, 0));
            if (name.empty()) continue;
            Vec3 p = un::Position(e);
            if (!std::isfinite(p.x) || !std::isfinite(p.y) || !std::isfinite(p.z)) continue;
            if (std::fabs(p.x) < 0.01f && std::fabs(p.y) < 0.01f && std::fabs(p.z) < 0.01f) continue;
            bool dup = false;
            for (const Ent& o : ents)
                if (o.name == name && un::Dist(o.pos, p) < 12.0f) dup = true;
            if (!dup) ents.push_back({name, p});
        }
        std::sort(ents.begin(), ents.end(), [](const Ent& a, const Ent& b) {
            if (a.name != b.name) return a.name < b.name;
            if (a.pos.x != b.pos.x) return a.pos.x < b.pos.x;
            return a.pos.z < b.pos.z;
        });
        for (size_t i = 0; i < ents.size(); ++i) {
            size_t total = 0, index = 0;
            for (size_t j = 0; j < ents.size(); ++j)
                if (ents[j].name == ents[i].name) {
                    ++total;
                    if (j < i) ++index;
                }
            Place p;
            p.loc.name = total > 1 ? ents[i].name + " (entrance " + std::to_string(index + 1) + ")" : ents[i].name;
            p.loc.x = ents[i].pos.x;
            p.loc.y = -ents[i].pos.z;
            p.loc.z = ents[i].pos.y + 1.0f;
            out.push_back(p);
        }
    }
    if (R.village && R.wsInstance && R.wsZones) {
        Object* ws = un::Static(R.wsInstance);
        std::vector<Place> villages;
        if (un::Alive(ws))
            for (Object* zn : il2::ListItems(un::Ref(ws, R.wsZones))) {
                if (!un::Alive(zn) || !il2::IsSubclass(il2::ClassOf(zn), R.village)) continue;
                Vec3 p = un::Position(zn);
                if (R.villageCenter) {
                    bool ok = false;
                    Object* r = un::Call(R.villageCenter, zn, {}, &ok);
                    Vec3 cp = ok ? il2::Result<Vec3>(r, p) : p;
                    if (std::isfinite(cp.x) && std::isfinite(cp.z) && (std::fabs(cp.x) > 0.01f || std::fabs(cp.z) > 0.01f)) p = cp;
                }
                std::string raw = R.zbName ? il2::Str(un::Ref(zn, R.zbName)) : std::string();
                if (raw.empty()) {
                    raw = un::Name(zn);
                    size_t at = raw.find("Cannibal");
                    if (at != std::string::npos) raw.erase(at, 8);
                }
                std::string name = Pretty(raw);
                std::string label = name.empty() ? "Cannibal village" : Contains(name, "village") ? name : "Village: " + name;
                Place v;
                v.loc.name = label;
                v.loc.x = p.x;
                v.loc.y = -p.z;
                v.loc.z = p.y;
                v.ground = true;
                villages.push_back(v);
            }
        std::sort(villages.begin(), villages.end(), [](const Place& a, const Place& b) { return a.loc.name < b.loc.name; });
        for (size_t i = 0; i < villages.size(); ++i) {
            int same = 0;
            for (size_t j = 0; j < i; ++j)
                if (villages[j].loc.name == villages[i].loc.name) ++same;
            if (same > 0) villages[i].loc.name += " " + std::to_string(same + 1);
        }
        out.insert(out.end(), villages.begin(), villages.end());
    }
    std::stable_sort(out.begin(), out.end(), [](const Place& a, const Place& b) { return a.loc.name < b.loc.name; });
    bool same = out.size() == places.size();
    for (size_t i = 0; same && i < out.size(); ++i)
        same = out[i].loc.name == places[i].loc.name && out[i].loc.x == places[i].loc.x && out[i].loc.y == places[i].loc.y &&
               out[i].loc.z == places[i].loc.z;
    if (same) return;
    places = std::move(out);
    pubPlaces = true;
    LOG("map places: %d", (int)places.size());
}

void BuildMarkers(const Ctx& c) {
    std::vector<core::Marker> mk;
    if (Object* tr = PlayerTr(c)) {
        Vec3 p = un::Position(tr);
        Object* cam = R.lpCamTr ? un::Call(R.lpCamTr, nullptr) : nullptr;
        if (!un::Alive(cam)) cam = tr;
        Vec3 f = un::Forward(cam);
        float yaw = std::fabs(f.x) + std::fabs(f.z) > 1e-4f ? std::atan2(-f.z, f.x) : 0.0f;
        mk.push_back({core::MarkerType::Player, "You", p.x, -p.z, p.y, yaw});
    }
    for (uint32_t h : carts) {
        Object* o = il2::Pinned(h);
        if (!un::Alive(o)) continue;
        Vec3 p = un::Position(o);
        mk.push_back({core::MarkerType::Vehicle, "Golf cart", p.x, -p.z, p.y, 0});
    }
    for (const Place& pl : places) mk.push_back({core::MarkerType::Place, pl.loc.name, pl.loc.x, pl.loc.y, pl.loc.z, 0});
    pendMarkers = std::move(mk);
    pubMarkers = true;
}

bool SameNative(Object* a, Object* b) {
    if (!un::Alive(a) || !un::Alive(b)) return false;
    return il2::Get<void*>(a, un::E.cachedPtr.off) == il2::Get<void*>(b, un::E.cachedPtr.off);
}

int TexSize(Object* tex, const char* getter) {
    Method* m = il2::FindMethod(il2::ClassOf(tex), getter, 0);
    return m ? un::CallInt(m, tex, {}, 0) : 0;
}

std::vector<uint8_t> GrabPng(Object* tex, float u0, float v0, float uw, float vh, int pw, int ph) {
    std::vector<uint8_t> out;
    Object* rt = un::Call(R.rtGet, nullptr, {&pw, &ph});
    if (!un::Alive(rt)) return out;
    Object* prev = un::Call(R.rtActiveGet, nullptr);
    Vec2 scale{uw, vh}, offset{u0, v0};
    bool blitted = false;
    un::Call(R.blit, nullptr, {tex, rt, &scale, &offset}, &blitted);
    un::Call(R.rtActiveSet, nullptr, {rt});
    bool active = SameNative(un::Call(R.rtActiveGet, nullptr), rt);
    Object* t2 = nullptr;
    bool made = false;
    if (blitted && active) {
        t2 = il2::New(R.texture2d);
        if (t2) un::Call(R.tex2dCtor, t2, {&pw, &ph}, &made);
    }
    if (made) {
        float rect[4] = {0.0f, 0.0f, (float)pw, (float)ph};
        int zero = 0;
        bool read = false;
        un::Call(R.readPixels, t2, {rect, &zero, &zero}, &read);
        Object* png = read ? un::Call(R.encodePng, nullptr, {t2}) : nullptr;
        size_t n = il2::Length(png);
        if (n >= 1000) {
            const uint8_t* d = (const uint8_t*)il2::Data(png);
            out.assign(d, d + n);
        }
    }
    un::Call(R.rtActiveSet, nullptr, {un::Alive(prev) ? prev : nullptr});
    un::Call(R.rtRelease, nullptr, {rt});
    if (made) un::Destroy(t2);
    return out;
}

bool MapCorners(Object* root, Object* surf, Vec2 ws, float out[4][2]) {
    Object* arr = il2::NewArray(un::E.vector3, 4);
    if (!arr || il2::Length(arr) < 4) return false;
    bool ok = false;
    un::Call(R.rtCorners, surf, {arr}, &ok);
    if (!ok) return false;
    Vec3 wc[4];
    std::memcpy(wc, il2::Data(arr), sizeof(wc));
    bool rok = false;
    Object* boxed = un::Call(R.rtRect, root, {}, &rok);
    if (!rok) return false;
    URect rr = il2::Result<URect>(boxed, URect{});
    if (!(rr.w > 1.0f) || !(rr.h > 1.0f)) return false;
    float cx = rr.x + rr.w * 0.5f, cy = rr.y + rr.h * 0.5f;
    for (int k = 0; k < 4; ++k) {
        bool tok = false;
        Object* r = un::Call(R.trInverse, root, {&wc[k]}, &tok);
        if (!tok) return false;
        Vec3 l = il2::Result<Vec3>(r, Vec3{});
        float wx = (l.x - cx) / (rr.w * 0.5f) * ws.x;
        float wz = (l.y - cy) / (rr.h * 0.5f) * ws.y;
        if (!std::isfinite(wx) || !std::isfinite(wz)) return false;
        out[k][0] = wx;
        out[k][1] = -wz;
    }
    return true;
}

struct GpsPick {
    Object* root = nullptr;
    Object* surf = nullptr;
    Object* tex = nullptr;
    Vec2 ws;
    float u0 = 0, v0 = 0, uw = 1, vh = 1;
    int tw = 0, th = 0;
    double area = 0;
};

bool ReadGps(Object* t, GpsPick& g) {
    Object* root = un::Ref(t, R.gpsRoot);
    Object* surf = un::Ref(t, R.gpsSurface);
    if (!un::Alive(root) || !un::Alive(surf)) return false;
    Vec2 ws = un::Val<Vec2>(t, R.gpsWorldSize, Vec2{});
    if (!(ws.x > 100.0f) || !(ws.y > 100.0f) || ws.x > 20000.0f || ws.y > 20000.0f) return false;
    Object* gr = un::Comp(surf, R.graphic);
    if (!gr) return false;
    Class* gc = il2::ClassOf(gr);
    Method* mt = il2::FindMethod(gc, "get_mainTexture", 0);
    Object* tex = mt ? un::Call(mt, gr) : nullptr;
    if (!un::Alive(tex)) return false;
    int tw = TexSize(tex, "get_width"), th = TexSize(tex, "get_height");
    if (tw < 64 || th < 64) return false;
    float u0 = 0, v0 = 0, uw = 1, vh = 1;
    if (R.rawImage && R.rawUv && il2::IsSubclass(gc, R.rawImage)) {
        bool ok = false;
        Object* r = un::Call(R.rawUv, gr, {}, &ok);
        URect uv = ok ? il2::Result<URect>(r, URect{0, 0, 1, 1}) : URect{0, 0, 1, 1};
        u0 = uv.x;
        v0 = uv.y;
        uw = uv.w;
        vh = uv.h;
    } else if (R.image && R.imgSprite && R.spriteRect && il2::IsSubclass(gc, R.image)) {
        Object* sp = un::Call(R.imgSprite, gr);
        if (un::Alive(sp)) {
            bool ok = false;
            Object* r = un::Call(R.spriteRect, sp, {}, &ok);
            URect sr = ok ? il2::Result<URect>(r, URect{}) : URect{};
            if (sr.w > 16.0f && sr.h > 16.0f) {
                u0 = sr.x / tw;
                v0 = sr.y / th;
                uw = sr.w / tw;
                vh = sr.h / th;
            }
        }
    }
    if (!(uw > 0.01f) || !(vh > 0.01f) || uw > 4.0f || vh > 4.0f || !std::isfinite(u0) || !std::isfinite(v0)) return false;
    g.root = root;
    g.surf = surf;
    g.tex = tex;
    g.ws = ws;
    g.u0 = u0;
    g.v0 = v0;
    g.uw = uw;
    g.vh = vh;
    g.tw = tw;
    g.th = th;
    g.area = (double)tw * uw * (double)th * vh;
    return true;
}

void SetCalib(float u1, float v1, float x0, float y0, float x1, float y1) {
    calU[0] = 0;
    calV[0] = 0;
    calU[1] = u1;
    calV[1] = v1;
    calX[0] = x0;
    calY[0] = y0;
    calX[1] = x1;
    calY[1] = y1;
    pubCalib = true;
}

bool ExportGps() {
    std::vector<Object*> list;
    if (R.gpsList)
        for (Object* o : il2::ListItems(un::Call(R.gpsList, nullptr)))
            if (un::Alive(o)) list.push_back(o);
    for (Object* o : un::FindAll(R.gps, true)) list.push_back(o);
    GpsPick best;
    for (Object* t : list) {
        GpsPick g;
        if (ReadGps(t, g) && g.area > best.area) best = g;
    }
    if (!best.tex) return false;
    float m[4][2];
    if (!MapCorners(best.root, best.surf, best.ws, m)) return false;
    using cf = std::complex<float>;
    cf tl(m[1][0], m[1][1]), tr(m[2][0], m[2][1]), br(m[3][0], m[3][1]), bl(m[0][0], m[0][1]);
    float wl = std::abs(tr - tl), hl = std::abs(bl - tl);
    if (!(wl > 200.0f) || !(hl > 200.0f) || wl > 80000.0f || hl > 80000.0f) {
        LOG("map: GPS picture has an unexpected size");
        return false;
    }
    int pw = (int)std::lround(std::clamp(best.tw * best.uw, 512.0f, 2048.0f));
    int ph = (int)std::lround(pw * hl / wl);
    if (ph > 2048) {
        pw = (int)std::lround(pw * 2048.0f / ph);
        ph = 2048;
    }
    if (pw < 64 || ph < 64) return false;
    cf z1(0.0f, 0.0f), z3((float)pw, (float)ph);
    cf a = (br - tl) / (z3 - z1);
    cf b = tl - a * z1;
    float err = std::max(std::abs(a * cf((float)pw, 0.0f) + b - tr), std::abs(a * cf(0.0f, (float)ph) + b - bl));
    if (err > 0.03f * std::abs(br - tl)) {
        LOG("map: GPS picture does not line up, using the relief picture");
        picTries = 99;
        return false;
    }
    std::vector<uint8_t> png = GrabPng(best.tex, best.u0, best.v0, best.uw, best.vh, pw, ph);
    if (png.empty()) return false;
    if (!WriteBytes(Folder() + L"\\map.png", png.data(), png.size())) {
        LOG("map: could not save map.png");
        return false;
    }
    SetCalib((float)pw, (float)ph, tl.real(), tl.imag(), br.real(), br.imag());
    overlay::ReloadMap();
    LOG("map picture saved from the GPS (%dx%d, world %.0f x %.0f)", pw, ph, best.ws.x, best.ws.y);
    return true;
}

void PngSink(void* ctx, void* data, int size) {
    std::vector<uint8_t>* out = (std::vector<uint8_t>*)ctx;
    const uint8_t* d = (const uint8_t*)data;
    out->insert(out->end(), d, d + size);
}

void Ramp(float t, float& r, float& g, float& b) {
    static const float stops[][4] = {{0.00f, 92, 120, 78},  {0.20f, 104, 132, 82},  {0.45f, 128, 138, 92},
                                     {0.65f, 142, 126, 100}, {0.82f, 160, 156, 150}, {1.00f, 232, 232, 236}};
    t = std::clamp(t, 0.0f, 1.0f);
    for (int i = 1; i < 6; ++i) {
        if (t > stops[i][0] && i < 5) continue;
        float k = (t - stops[i - 1][0]) / std::max(1e-4f, stops[i][0] - stops[i - 1][0]);
        k = std::clamp(k, 0.0f, 1.0f);
        r = stops[i - 1][1] + (stops[i][1] - stops[i - 1][1]) * k;
        g = stops[i - 1][2] + (stops[i][2] - stops[i - 1][2]) * k;
        b = stops[i - 1][3] + (stops[i][3] - stops[i - 1][3]) * k;
        return;
    }
}

bool FinishRelief() {
    const int n = kRelief;
    float lo = 1e30f, hi = -1e30f;
    for (int k = 0; k < n * n; ++k)
        if (reliefIn[k] && std::isfinite(reliefH[k])) {
            lo = std::min(lo, reliefH[k]);
            hi = std::max(hi, reliefH[k]);
        }
    if (!(hi > lo + 1.0f)) return false;
    float cell = 2.0f * kWorld / n;
    float lx = -1.0f, ly = 1.6f, lz = 1.0f;
    float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= ll;
    ly /= ll;
    lz /= ll;
    auto at = [&](int i, int j) {
        i = std::clamp(i, 0, n - 1);
        j = std::clamp(j, 0, n - 1);
        float h = reliefH[j * n + i];
        return std::isfinite(h) ? h : lo;
    };
    std::vector<uint8_t> px((size_t)n * n * 4, 255);
    for (int j = 0; j < n; ++j)
        for (int i = 0; i < n; ++i) {
            size_t k = (size_t)j * n + i;
            uint8_t* p = &px[k * 4];
            if (!reliefIn[k] || !std::isfinite(reliefH[k])) {
                p[0] = 26;
                p[1] = 52;
                p[2] = 82;
                continue;
            }
            float t = (reliefH[k] - lo) / (hi - lo);
            float r = 0, g = 0, b = 0;
            if (t < 0.015f) {
                r = 46;
                g = 92;
                b = 128;
            } else {
                Ramp(t, r, g, b);
                float dx = (at(i + 1, j) - at(i - 1, j)) / (2.0f * cell);
                float dz = (at(i, j - 1) - at(i, j + 1)) / (2.0f * cell);
                float nx = -dx, ny = 1.0f, nz = -dz;
                float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
                float shade = (nx * lx + ny * ly + nz * lz) / nl / ly;
                shade = std::clamp(shade, 0.45f, 1.25f);
                r *= shade;
                g *= shade;
                b *= shade;
            }
            p[0] = (uint8_t)std::clamp(r, 0.0f, 255.0f);
            p[1] = (uint8_t)std::clamp(g, 0.0f, 255.0f);
            p[2] = (uint8_t)std::clamp(b, 0.0f, 255.0f);
        }
    std::vector<uint8_t> png;
    if (!stbi_write_png_to_func(&PngSink, &png, n, n, 4, px.data(), n * 4) || png.empty()) return false;
    if (!WriteBytes(Folder() + L"\\map.png", png.data(), png.size())) return false;
    SetCalib((float)n, (float)n, -kWorld, -kWorld, kWorld, kWorld);
    overlay::ReloadMap();
    LOG("map picture drawn from the terrain (%dx%d)", n, n);
    return true;
}

void ClearRelief() {
    reliefH.clear();
    reliefIn.clear();
    reliefRow = 0;
}

void StepRelief(const Ctx& c) {
    const int n = kRelief;
    if (reliefH.empty()) {
        reliefH.assign((size_t)n * n, NAN);
        reliefIn.assign((size_t)n * n, 0);
        reliefRow = 0;
    }
    auto t0 = std::chrono::steady_clock::now();
    float cell = 2.0f * kWorld / n;
    while (reliefRow < n) {
        float z = kWorld - (reliefRow + 0.5f) * cell;
        for (int i = 0; i < n; ++i) {
            float x = -kWorld + (i + 0.5f) * cell;
            size_t k = (size_t)reliefRow * n + i;
            float h = 0;
            if (TerrainAt(x, z, h)) {
                reliefH[k] = h;
                reliefIn[k] = Inside(c, x, h, z) ? 1 : 0;
            }
        }
        ++reliefRow;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(3)) break;
    }
    if (reliefRow < n) return;
    if (!FinishRelief()) LOG("map: no picture could be made, the map shows a plain grid");
    ClearRelief();
    pic = Pic::Done;
}

void StepPicture(const Ctx& c, double now) {
    if (pic == Pic::Done) return;
    if (Folder().empty() || (!canGps && !canRelief)) {
        pic = Pic::Done;
        return;
    }
    if (pic == Pic::Wait) {
        if (picStart <= 0) picStart = now;
        if (now - picStart < 5.0) return;
        pic = canGps ? Pic::Gps : Pic::Relief;
        picTries = 0;
        picNext = 0;
    }
    if (pic == Pic::Gps) {
        if (now < picNext) return;
        picNext = now + 2.0;
        if (ExportGps()) {
            pic = Pic::Done;
            return;
        }
        if (++picTries >= 6) {
            pic = canRelief ? Pic::Relief : Pic::Done;
            ClearRelief();
            if (pic == Pic::Done) LOG("map: the GPS picture could not be saved");
        }
        return;
    }
    if (pic == Pic::Relief) StepRelief(c);
}

void Look(Class*& c, const char* assembly, const char* ns, const char* name) { c = un::C(assembly, ns, name); }

void ResolvePlayer() {
    Look(R.localPlayer, kSons, "TheForest.Utils", "LocalPlayer");
    if (R.localPlayer) {
        R.lpTransform = StaticM(R.localPlayer, "get_Transform", 0);
        R.lpCamTr = StaticM(R.localPlayer, "get_MainCamTr", 0);
        R.lpStats = StaticM(R.localPlayer, "get_Stats", 0);
        R.lpFpc = StaticM(R.localPlayer, "get_FpCharacter", 0);
        R.lpView = StaticM(R.localPlayer, "get_CurrentView", 0);
        R.lpInCaves = StaticM(R.localPlayer, "get_IsInCaves", 0);
        R.lpEndgame = StaticM(R.localPlayer, "get_IsInEndgame", 0);
        R.lpGolfCart = StaticM(R.localPlayer, "get_IsInGolfCart", 0);
        R.lpGliding = StaticM(R.localPlayer, "get_IsGliding", 0);
        R.lpRaft = StaticM(R.localPlayer, "get_AttachedRaft", 0);
        R.lpSpecial = StaticM(R.localPlayer, "get_SpecialActions", 0);
        R.lpCaveCheck = un::M(R.localPlayer, "CheckCaveForcedEnter", std::vector<std::string>{"UnityEngine.Vector3", "System.Boolean"});
        R.lpTeleport = un::M(R.localPlayer, "TeleportTo", std::vector<std::string>{"UnityEngine.Vector3"});
        if (R.lpCaveCheck && il2::IsInstance(R.lpCaveCheck)) R.lpCaveCheck = nullptr;
        if (R.lpTeleport && il2::IsInstance(R.lpTeleport)) R.lpTeleport = nullptr;
    }
    if (Class* inv = un::C(kSons, "TheForest.Items.Inventory", "PlayerInventory")) {
        if (Class* views = il2::Nested(inv, "PlayerViews")) {
            K.viewWorld = (int)il2::EnumValue(views, "World", K.viewWorld);
            K.viewInventory = (int)il2::EnumValue(views, "Inventory", K.viewInventory);
        } else {
            un::Missing("PlayerInventory.PlayerViews");
        }
    }
    Look(R.stats, kSons, "", "PlayerStats");
    if (R.stats) {
        R.statsDead = un::M(R.stats, "IsDead", 0);
        R.statsDying = un::M(R.stats, "IsDying", 0);
    }
    Look(R.cutscenes, kSons, "Sons.Cutscenes", "CutsceneManager");
    R.cutscenePlaying = StaticM(R.cutscenes, "IsCutscenePlaying", 0);
    Look(R.knightV, kSons, "Sons.Gameplay", "PlayerKnightVAction");
    R.knightActive = un::M(R.knightV, "IsActive", 0);
    Look(R.fpc, kSons, "", "FirstPersonCharacter");
    if (R.fpc) {
        R.fpcClearFall = un::M(R.fpc, "ClearFallDamageFlag", 0);
        R.fpcSetGrounded = un::M(R.fpc, "set_Grounded", std::vector<std::string>{"System.Boolean"});
        R.fpcGrounded = un::M(R.fpc, "get_Grounded", 0);
        R.fpcPrevGrounded = un::F(R.fpc, "_prevGrounded");
    }
    Look(R.terrain, "Endnight", "Endnight.Environment", "TerrainUtilities");
    R.terrainHeight = un::M(R.terrain, "GetTerrainHeight", std::vector<std::string>{"UnityEngine.Vector3"});
    R.terrainInside = un::M(R.terrain, "IsInsideBounds", std::vector<std::string>{"UnityEngine.Vector3"});
}

void ResolvePlaces() {
    Look(R.golfCart, kSons, "Sons.Gameplay", "GolfCartController");
    Look(R.caveEntrance, "Sons.Areas", "Sons.Areas", "CaveEntrance");
    Look(R.areaMask, "Sons.Areas", "Sons.Areas", "AreaMask");
    if (R.caveEntrance) R.ceArea = un::F(R.caveEntrance, "_areaId");
    areaNames.clear();
    if (R.areaMask)
        for (const char* n : {"CaveA", "CaveB", "CaveC", "CaveD", "CaveF", "CaveG", "CaveH", "BunkerA", "BunkerB", "BunkerC", "BunkerD",
                              "BunkerE", "BunkerF", "BunkerG"}) {
            long long v = il2::EnumValue(R.areaMask, n, LLONG_MIN);
            if (v != LLONG_MIN && v != 0) areaNames.push_back({(int)v, Pretty(n)});
        }
    Look(R.worldSim, "Sons.Ai.Vail", "Sons.Ai.Vail", "VailWorldSimulation");
    if (R.worldSim) {
        R.wsInstance = un::F(R.worldSim, "_instance");
        R.wsZones = un::F(R.worldSim, "_vailZoneBases");
    }
    Look(R.zoneBase, "Sons.Ai.Vail", "Sons.Ai.Vail", "VailZoneBase");
    if (R.zoneBase) R.zbName = un::F(R.zoneBase, "_displayName");
    Look(R.village, kSons, "Sons.Characters", "CannibalVillage");
    R.villageCenter = un::M(R.village, "GetCenterPosition", 0);
}

void ResolvePicture() {
    Look(R.gps, kSons, "Sons.Gameplay.GPS", "GPSTrackerSystem");
    if (R.gps) {
        R.gpsRoot = un::F(R.gps, "_mapRoot");
        R.gpsSurface = un::F(R.gps, "_surfaceMap");
        R.gpsWorldSize = un::F(R.gps, "_worldSize");
        R.gpsList = StaticM(R.gps, "get_GpsTrackers", 0);
    }
    Look(R.graphic, kUiAsm, "UnityEngine.UI", "Graphic");
    Look(R.rawImage, kUiAsm, "UnityEngine.UI", "RawImage");
    Look(R.image, kUiAsm, "UnityEngine.UI", "Image");
    Look(R.sprite, kCoreAsm, "UnityEngine", "Sprite");
    Look(R.rectTransform, kCoreAsm, "UnityEngine", "RectTransform");
    R.rawUv = un::M(R.rawImage, "get_uvRect", 0);
    R.imgSprite = un::M(R.image, "get_overrideSprite", 0);
    R.spriteRect = un::M(R.sprite, "get_textureRect", 0);
    R.rtRect = un::M(R.rectTransform, "get_rect", 0);
    R.rtCorners = un::M(R.rectTransform, "GetWorldCorners", 1);
    R.trInverse = un::M(un::E.transform, "InverseTransformPoint", std::vector<std::string>{"UnityEngine.Vector3"});
    Look(R.renderTexture, kCoreAsm, "UnityEngine", "RenderTexture");
    Look(R.graphics, kCoreAsm, "UnityEngine", "Graphics");
    Look(R.texture2d, kCoreAsm, "UnityEngine", "Texture2D");
    Look(R.imageConversion, "UnityEngine.ImageConversionModule", "UnityEngine", "ImageConversion");
    R.rtGet = un::M(R.renderTexture, "GetTemporary", std::vector<std::string>{"System.Int32", "System.Int32"});
    R.rtRelease = un::M(R.renderTexture, "ReleaseTemporary", 1);
    R.rtActiveGet = StaticM(R.renderTexture, "get_active", 0);
    R.rtActiveSet = StaticM(R.renderTexture, "set_active", 1);
    R.blit = un::M(R.graphics, "Blit",
                   std::vector<std::string>{"UnityEngine.Texture", "UnityEngine.RenderTexture", "UnityEngine.Vector2", "UnityEngine.Vector2"});
    R.tex2dCtor = un::M(R.texture2d, ".ctor", std::vector<std::string>{"System.Int32", "System.Int32"});
    R.readPixels = un::M(R.texture2d, "ReadPixels", std::vector<std::string>{"UnityEngine.Rect", "System.Int32", "System.Int32"});
    R.encodePng = un::M(R.imageConversion, "EncodeToPNG", std::vector<std::string>{"UnityEngine.Texture2D"});
}

bool Resolve(std::string&) {
    ResolvePlayer();
    ResolvePlaces();
    ResolvePicture();
    canTeleport = R.localPlayer && R.lpTransform && R.lpTeleport && R.terrainHeight && un::E.cachedPtr;
    canGps = R.gps && R.gpsRoot && R.gpsSurface && R.gpsWorldSize && R.graphic && R.rtRect && R.rtCorners && R.trInverse && un::E.vector3 &&
             R.rtGet && R.rtRelease && R.rtActiveGet && R.rtActiveSet && R.blit && R.texture2d && R.tex2dCtor && R.readPixels &&
             R.encodePng && un::E.cachedPtr;
    canRelief = R.terrainHeight != nullptr;
    if (!canTeleport) LOG("map: teleport parts were not found");
    return true;
}

bool Supported(const std::string& id) { return id == "map" && canTeleport; }

void PlayerChanged(const Ctx& c) {
    if (c.player == lastPlayer) return;
    lastPlayer = c.player;
    int id = un::InstanceId(c.player);
    if (id == playerId) return;
    if (playerId != 0) {
        arrival = Arrival{};
        undo = Spot{};
        SetUndo(false);
    }
    playerId = id;
}

void Tick(const Ctx& c) {
    if (!model || !canTeleport) return;
    std::vector<Request> todo;
    {
        std::lock_guard<std::mutex> g(qLock);
        todo.swap(requests);
    }
    if (!c.inWorld || !c.player || c.session == core::Session::Client) {
        if (!todo.empty())
            Note(c.session == core::Session::Client ? "Teleport is off when you join someone else's game" : "Load a save first");
        Publish();
        return;
    }
    PlayerChanged(c);
    double now = c.now;
    for (const Request& r : todo) {
        if (r.undo) DoUndo(c, now);
        else DoTeleport(c, r, now);
    }
    StepArrival(c, now);
    bool visible = c.s.menuOpen && c.s.mapVisible;
    if (visible) {
        if (now >= nextPlaces) {
            nextPlaces = now + 30.0;
            BuildPlaces();
        }
        if (now >= nextCarts) {
            nextCarts = now + 3.0;
            RefreshCarts();
        }
        if (now >= nextMarkers && !pubMarkers) {
            nextMarkers = now + 0.25;
            BuildMarkers(c);
        }
    }
    StepPicture(c, now);
    Publish();
}

bool Action(const std::string&, const Ctx&) { return false; }

void Release(const Ctx&) {
    {
        std::lock_guard<std::mutex> g(qLock);
        requests.clear();
    }
    arrival = Arrival{};
    undo = Spot{};
    DropCarts();
    places.clear();
    nextPlaces = nextCarts = nextMarkers = 0;
    lastPlayer = nullptr;
    playerId = 0;
    insideState = 0;
    if (pic != Pic::Done) {
        pic = Pic::Wait;
        picStart = 0;
        picNext = 0;
        picTries = 0;
        ClearRelief();
    }
    undoState = false;
    pubUndo = true;
    pubPlaces = true;
    pendMarkers.clear();
    pubMarkers = true;
    Publish();
}

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    if (!canTeleport) return;
    m.teleport = [](float x, float y, float z, bool useZ) {
        Request r;
        r.x = x;
        r.y = y;
        r.z = z;
        r.useZ = useZ;
        std::lock_guard<std::mutex> g(qLock);
        if (requests.size() < 8) requests.push_back(r);
    };
    m.undoTeleport = [] {
        Request r;
        r.undo = true;
        std::lock_guard<std::mutex> g(qLock);
        if (requests.size() < 8) requests.push_back(r);
    };
}

}

const Group& MapGroup() {
    static Group g{"Map", &Resolve, &Supported, &Tick, &Action, &Release, &Wire, nullptr};
    return g;
}

}
