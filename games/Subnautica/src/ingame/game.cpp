#include "ingame/game.h"
#include "ingame/esp.h"
#include "ingame/log.h"
#include "ingame/unity.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace sn1game {

namespace {

using core::Feature;
using mono::Object;
using namespace un;

enum T {
    T_GOD, T_HEALTH, T_RAD, T_FOOD, T_WATER, T_OXYGEN, T_GHOST, T_NOCLIP, T_FLY,
    T_CRAFT, T_BUILD, T_DECON, T_FREE, T_HULL, T_POWER, T_PRINTS, T_SCAN,
    T_FOG, T_VHULL, T_VENERGY, T_VINVIS, T_ESP, T_IESP,
    T_E_HOSTILE, T_E_PASSIVE, T_E_LEV, T_E_NAME, T_E_DIST, T_E_HEALTH, T_E_BOX,
    T_I_RES, T_I_FRAG, T_I_DATA, T_I_CRATE, T_I_NAME, T_I_DIST, T_I_BOX, T_COUNT
};
const char* kToggleIds[T_COUNT] = {
    "godmode", "unlimited_health", "radiation_immunity", "unlimited_food", "unlimited_water", "infinite_oxygen", "ghost", "noclip", "fly",
    "instant_craft", "instant_build", "instant_deconstruct", "free_craft", "base_integrity", "unlimited_power", "unlock_blueprints", "instant_scan",
    "remove_fog", "vehicle_hull", "vehicle_energy", "vehicle_invisible", "esp", "item_esp",
    "esp_hostile", "esp_passive", "esp_leviathan", "esp_name", "esp_distance", "esp_health", "esp_box",
    "itemesp_resources", "itemesp_fragments", "itemesp_data", "itemesp_crates", "itemesp_name", "itemesp_distance", "itemesp_box"};
const int kFirstSub = T_E_HOSTILE;

enum S { S_SPEED, S_JUMP, S_FLY, S_GAME, S_VSPEED, S_CLEAR, S_RANGE, S_COUNT };
const char* kSliderIds[S_COUNT] = {"player_speed", "jump_height", "fly_speed", "game_speed", "vehicle_speed", "clear_radius", "esp_range"};
const float kSliderDefaults[S_COUNT] = {1, 1, 1, 1, 1, 50, 150};

const char* kActions[] = {"refill_health", "refill_food", "refill_water", "refill_oxygen", "clear_hostiles"};

struct Snap {
    bool on[T_COUNT];
    float v[S_COUNT];
};

Snap Defaults() {
    Snap s{};
    for (int i = kFirstSub; i < T_COUNT; ++i) s.on[i] = true;
    for (int i = 0; i < S_COUNT; ++i) s.v[i] = kSliderDefaults[i];
    return s;
}

Snap Off() {
    Snap s = Defaults();
    for (int i = 0; i < kFirstSub; ++i) s.on[i] = false;
    return s;
}

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
Feature* tog[T_COUNT] = {};
Feature* sld[S_COUNT] = {};
Snap lastSnap = Defaults();
Snap prev = Off();

std::mutex qLock;
std::vector<std::string> queued;
std::vector<std::string> notes;

std::atomic<bool> menuOpen{false}, menuFocused{false};
std::atomic<int> stop{0};
std::atomic<int> ticking{0};
std::atomic<ULONGLONG> lastTick{0};
std::atomic<void*> gameWnd{nullptr};
uint32_t delegateHandle = 0;
int faults = 0;
bool wasInGame = false;

std::unordered_set<int> levSet, hostileSet;
std::unordered_map<int, int> kindCache;
int fragmentTech = -1;

void Note(const std::string& s) {
    std::lock_guard<std::mutex> g(qLock);
    if (notes.size() < 32) notes.push_back(s);
}

double Now() { return GetTickCount64() / 1000.0; }

bool Key(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

bool GameFocused() {
    HWND w = GetForegroundWindow();
    if (!w) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
Vec3 Mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
float Dist(Vec3 a, Vec3 b) { return Len(Sub(a, b)); }

struct Hold {
    uint32_t h = 0;
    Object* get() const { return h ? mono::Pinned(h) : nullptr; }
    void set(Object* o) {
        if (h) mono::Unpin(h);
        h = o ? mono::Pin(o) : 0;
    }
    void reset() { set(nullptr); }
};

struct Scaled {
    Hold obj;
    float base = 0, written = 0;
};

void Unscale(Scaled& s, const Fld& f) {
    Object* o = s.obj.get();
    if (o && f && std::fabs(Val<float>(o, f) - s.written) < 1e-4f) SetVal<float>(o, f, s.base);
    s.obj.reset();
}

void Scale(Object* o, const Fld& f, float mult, Scaled& s) {
    if (!f) return;
    Object* held = s.obj.get();
    if (s.obj.h && held != o) Unscale(s, f);
    if (!o || std::fabs(mult - 1.0f) < 1e-4f) {
        if (s.obj.h) Unscale(s, f);
        return;
    }
    float cur = Val<float>(o, f);
    if (!s.obj.h) {
        s.obj.set(o);
        s.base = cur;
    } else if (std::fabs(cur - s.written) > 1e-4f) {
        s.base = cur;
    }
    float want = s.base * mult;
    if (cur != want) SetVal<float>(o, f, want);
    s.written = want;
}

struct Flag {
    Hold obj;
};

void ForceFlag(Object* o, const Fld& f, bool want, Flag& s) {
    if (!f) return;
    Object* held = s.obj.get();
    if (s.obj.h && (held != o || !want)) {
        if (held) SetVal<uint8_t>(held, f, 0);
        s.obj.reset();
    }
    if (want && o) {
        if (!Val<uint8_t>(o, f)) SetVal<uint8_t>(o, f, 1);
        if (!s.obj.h) s.obj.set(o);
    }
}

struct Off_ {
    Hold obj;
};

void ForceOff(Object* beh, bool want, Off_& s) {
    Object* held = s.obj.get();
    if (s.obj.h && (held != beh || !want)) {
        if (Alive(held)) SetEnabled(held, true);
        s.obj.reset();
    }
    if (!want || !Alive(beh)) return;
    if (!s.obj.h) {
        if (Enabled(beh)) {
            SetEnabled(beh, false);
            s.obj.set(beh);
        }
    } else if (Enabled(beh)) {
        SetEnabled(beh, false);
    }
}

Snap TakeSnap() {
    if (!modelLock) return lastSnap;
    std::unique_lock<std::mutex> g(*modelLock, std::try_to_lock);
    if (!g.owns_lock()) return lastSnap;
    Snap s = Defaults();
    for (int i = 0; i < T_COUNT; ++i) {
        Feature* f = tog[i];
        if (i < kFirstSub) s.on[i] = f && f->available && !f->building && f->on;
        else s.on[i] = f ? f->on : true;
    }
    for (int i = 0; i < S_COUNT; ++i) {
        Feature* f = sld[i];
        if (!f) continue;
        s.v[i] = (f->available && !f->building) || i == S_RANGE ? f->value : f->def;
    }
    lastSnap = s;
    return s;
}

int Kind(int tt) {
    auto it = kindCache.find(tt);
    if (it != kindCache.end()) return it->second;
    int k = 0;
    if (levSet.count(tt)) k = 2;
    else if (hostileSet.count(tt)) k = 1;
    else if (R.cdBehaviour && tt > 0) {
        int b = CallInt(R.cdBehaviour, nullptr, {&tt}, 0);
        if (b == 6) k = 2;
        else if (b == 4) k = 1;
    }
    kindCache[tt] = k;
    return k;
}

int ownedCheats = 0;

void GameModes(const Snap& s, bool active) {
    if (!R.gmCheats || !R.gmActivate || !R.gmDeactivate) return;
    int want = 0;
    if (active) {
        if (s.on[T_RAD]) want |= 512;
        if (s.on[T_OXYGEN]) want |= 64;
        if (s.on[T_GHOST]) want |= 128;
        if (s.on[T_PRINTS]) want |= 8;
        if (s.on[T_FREE]) want |= 4;
    }
    int cur = 0;
    mono::StaticGet(R.gmCheats.cls, R.gmCheats.f, &cur);
    for (int bit : {512, 64, 128, 8, 4}) {
        if ((want & bit) && !(cur & bit)) {
            Call(R.gmActivate, nullptr, {&bit});
            ownedCheats |= bit;
        } else if (!(want & bit) && (ownedCheats & bit)) {
            if (cur & bit) Call(R.gmDeactivate, nullptr, {&bit});
            ownedCheats &= ~bit;
        }
    }
}

Flag godFlag;
bool suffSaved = false;
float suffOrig = 0;

void RefillOxygen(Object* player) {
    Object* ox = Ref(player, R.plOxygen);
    if (!Alive(ox)) return;
    float cap = CallFloat(R.oxCapacity, ox, {}, 0);
    if (cap > 0) Call(R.oxAdd, ox, {&cap});
}

void CyclopsGuard(Object* player) {
    if (!R.cdInteriorFx || !R.cyclopsDestruction) return;
    Object* sub = Call(R.plCurrentSub, player);
    if (!Alive(sub) || !Val<uint8_t>(sub, R.srCyclops) || !Val<uint8_t>(sub, R.srDestroyed)) return;
    Object* ev = CompInChildren(sub, R.cyclopsDestruction);
    if (ev && Ref(ev, R.cdInteriorFx)) mono::SetRef(ev, R.cdInteriorFx.f, nullptr);
}

void Vitals(Object* player, const Snap& s) {
    Object* live = Ref(player, R.plLive);
    ForceFlag(Alive(live) ? live : nullptr, R.lmInvincible, s.on[T_GOD], godFlag);
    if (R.plSuffTime) {
        if (s.on[T_GOD]) {
            if (!suffSaved) {
                suffOrig = Val<float>(player, R.plSuffTime);
                suffSaved = true;
            }
            if (Val<float>(player, R.plSuffTime) < 1e6f) SetVal<float>(player, R.plSuffTime, 1e7f);
            if (!prev.on[T_GOD] && CallBool(R.plIsSuffocating, player)) Call(R.plSuffReset, player);
        } else if (suffSaved) {
            if (player) SetVal<float>(player, R.plSuffTime, suffOrig);
            suffSaved = false;
        }
    }
    if (!player) return;
    if (s.on[T_GOD]) CyclopsGuard(player);
    if (Alive(live) && s.on[T_HEALTH]) {
        float mx = CallFloat(R.lmMaxHealth, live, {}, 0);
        float h = Val<float>(live, R.lmHealth);
        if (mx > 0 && h > 0 && h < mx) SetVal<float>(live, R.lmHealth, mx);
    }
    if (s.on[T_FOOD] || s.on[T_WATER]) {
        Object* sv = Comp(player, R.survival);
        if (sv) {
            if (s.on[T_FOOD] && Val<float>(sv, R.svFood) < 100.0f) SetVal<float>(sv, R.svFood, 100.0f);
            if (s.on[T_WATER] && Val<float>(sv, R.svWater) < 100.0f) SetVal<float>(sv, R.svWater, 100.0f);
        }
    }
    if (s.on[T_OXYGEN] && !prev.on[T_OXYGEN]) RefillOxygen(player);
}

Off_ playerEco, vehicleEco, cyclopsEco;
Hold lastVehicle;
double lastCalm = 0;

void DropTargets(std::initializer_list<Object*> targets) {
    if (!R.lastTarget || !R.ltGetTarget || !R.ltSetTarget) return;
    for (Object* lt : FindAll(R.lastTarget)) {
        Object* t = Call(R.ltGetTarget, lt);
        if (!t) continue;
        for (Object* x : targets)
            if (x && t == x) {
                Call(R.ltSetTarget, lt, {nullptr});
                break;
            }
    }
}

void CalmCyclopsHunters(Object* cyclops) {
    Object* go = GameObjectOf(cyclops);
    for (Object* ac : FindAll(R.attackCyclops)) {
        Object* trait = Ref(ac, R.acAggress);
        if (trait && R.ctValue) SetVal<float>(trait, R.ctValue, 0.0f);
        if (go && Ref(ac, R.acTarget) == go) Call(R.acStop, ac);
    }
}

void Stealth(Object* player, Object* veh, Object* cyc, const Snap& s, double now) {
    bool inVeh = veh || cyc;
    bool hide = player && (s.on[T_GHOST] || (s.on[T_VINVIS] && inVeh));
    ForceOff(player ? Comp(player, R.ecoTarget) : nullptr, hide, playerEco);
    ForceOff(veh ? Comp(veh, R.ecoTarget) : nullptr, s.on[T_VINVIS] && veh, vehicleEco);
    ForceOff(cyc ? Comp(cyc, R.ecoTarget) : nullptr, s.on[T_VINVIS] && cyc, cyclopsEco);
    if (!player) {
        lastVehicle.reset();
        return;
    }
    Object* cur = veh ? veh : cyc;
    bool changed = cur != lastVehicle.get();
    if (changed) lastVehicle.set(cur);
    bool ghostStarted = s.on[T_GHOST] && !prev.on[T_GHOST];
    bool invisStarted = s.on[T_VINVIS] && inVeh && (!prev.on[T_VINVIS] || changed);
    if (ghostStarted || invisStarted)
        DropTargets({GameObjectOf(player), veh ? GameObjectOf(veh) : nullptr, cyc ? GameObjectOf(cyc) : nullptr});
    if (s.on[T_VINVIS] && cyc && now - lastCalm > 0.25) {
        lastCalm = now;
        CalmCyclopsHunters(cyc);
    }
}

Scaled swimMult, swimAccel, walkFwd, walkBack, walkSide, walkAccel, jumpHeight;
bool noclipActive = false;
bool flyActive = false;
int castMask = 0;

int LayerMask() {
    if (!castMask && R.pmLayerMask) {
        int m = 0;
        mono::StaticGet(R.pmLayerMask.cls, R.pmLayerMask.f, &m);
        castMask = m;
    }
    return castMask ? castMask : ~(1 << 2);
}

bool Cast(Vec3 c, Vec3 dir, float dist, float& allowed, Vec3& normal) {
    float radius = 0.3f, half = 0.5f, skin = 0.05f;
    float maxd = dist + skin;
    int mask = LayerMask(), qti = 1;
    RayHit hit{};
    bool any;
    if (R.capsuleCast) {
        Vec3 p1 = {c.x, c.y + half, c.z}, p2 = {c.x, c.y - half, c.z};
        any = CallBool(R.capsuleCast, nullptr, {&p1, &p2, &radius, &dir, &hit, &maxd, &mask, &qti});
    } else if (R.sphereCast) {
        any = CallBool(R.sphereCast, nullptr, {&c, &radius, &dir, &hit, &maxd, &mask, &qti});
    } else {
        return false;
    }
    if (!any) return false;
    allowed = std::clamp(hit.distance - skin, 0.0f, dist);
    normal = hit.normal;
    return true;
}

Vec3 Sweep(Vec3 c, Vec3 move) {
    float dist = Len(move);
    if (dist < 1e-5f) return c;
    Vec3 n = Mul(move, 1.0f / dist);
    float allowed = 0;
    Vec3 normal;
    if (!Cast(c, n, dist, allowed, normal)) return Add(c, move);
    Vec3 p = Add(c, Mul(n, allowed));
    Vec3 rem = Sub(move, Mul(n, allowed));
    Vec3 slide = Sub(rem, Mul(normal, Dot(rem, normal)));
    float sl = Len(slide);
    if (sl < 1e-4f) return p;
    Vec3 sn = Mul(slide, 1.0f / sl);
    float a2 = 0;
    Vec3 n2;
    if (!Cast(p, sn, sl, a2, n2)) return Add(p, slide);
    return Add(p, Mul(sn, a2));
}

void Fly(Object* player, Object* pc, const Snap& s, bool inVeh) {
    int mode = player ? Val<int>(player, R.plMode) : 0;
    bool cine = player && CallBool(R.plCinematic, player);
    bool can = player && Alive(pc) && s.on[T_FLY] && !inVeh && mode == 0 && !cine;
    if (!can) {
        if (flyActive) {
            if (player && Alive(pc) && !inVeh && mode == 0) {
                int t = 1;
                Call(R.pcSetEnabled, pc, {&t});
            }
            flyActive = false;
        }
        return;
    }
    if (!flyActive) {
        int f = 0;
        Call(R.pcSetEnabled, pc, {&f});
        flyActive = true;
    }
    if (!GameFocused() || menuFocused.load()) return;
    float fw = (Key('W') ? 1.0f : 0.0f) - (Key('S') ? 1.0f : 0.0f);
    float rt = (Key('D') ? 1.0f : 0.0f) - (Key('A') ? 1.0f : 0.0f);
    float up = (Key(VK_SPACE) ? 1.0f : 0.0f) - (Key(VK_LCONTROL) || Key(VK_RCONTROL) ? 1.0f : 0.0f);
    if (fw == 0 && rt == 0 && up == 0) return;
    Object* cam = Call(R.camMain, nullptr);
    Object* ct = Alive(cam) ? TransformOf(cam) : nullptr;
    if (!ct) return;
    Vec3 f = mono::Result<Vec3>(Call(R.trForward, ct));
    Vec3 r = mono::Result<Vec3>(Call(R.trRight, ct));
    Vec3 dir = Add(Add(Mul(f, fw), Mul(r, rt)), Vec3{0, up, 0});
    float l = Len(dir);
    if (l < 1e-4f) return;
    dir = Mul(dir, 1.0f / l);
    float speed = 8.0f * std::clamp(s.v[S_FLY], 0.05f, 20.0f);
    float dt = std::min(DeltaTime(), 0.1f);
    if (dt <= 0) return;
    Vec3 move = Mul(dir, speed * dt);
    Vec3 pos = Position(player);
    Vec3 center = R.pcCenter ? mono::Result<Vec3>(Call(R.pcCenter, pc), pos) : pos;
    Vec3 off = Sub(center, pos);
    bool through = s.on[T_NOCLIP] && R.plUnderwater && CallBool(R.plUnderwater, player);
    Vec3 next = through ? Add(center, move) : Sweep(center, move);
    SetPosition(player, Sub(next, off));
}

void Noclip(Object* pc, const Snap& s, bool inVeh) {
    Object* uw = Ref(pc, R.pcUnderwater);
    Object* active = Ref(pc, R.pcActive);
    bool swimming = Alive(uw) && active == uw && !inVeh && !flyActive;
    Object* rb = Ref(uw, R.pmRb);
    if (s.on[T_NOCLIP] && swimming && Alive(rb)) {
        if (CallBool(R.rbGetDetect, rb)) {
            int f = 0;
            Call(R.rbSetDetect, rb, {&f});
        }
        noclipActive = true;
    } else if (noclipActive) {
        if (swimming && Alive(rb)) {
            int t = 1;
            Call(R.rbSetDetect, rb, {&t});
        }
        noclipActive = false;
    }
}

void Movement(Object* player, const Snap& s, bool inVeh) {
    Object* pc = player ? Ref(player, R.plController) : nullptr;
    if (!Alive(pc)) pc = nullptr;
    Object* uw = Ref(pc, R.pcUnderwater);
    Object* gr = Ref(pc, R.pcGround);
    float k = std::clamp(s.v[S_SPEED], 0.1f, 20.0f);
    Scale(uw, R.pmDebugSpeed, k, swimMult);
    Scale(uw, R.pmWaterAccel, k, swimAccel);
    Scale(gr, R.pmForward, k, walkFwd);
    Scale(gr, R.pmBackward, k, walkBack);
    Scale(gr, R.pmStrafe, k, walkSide);
    Scale(gr, R.pmGroundAccel, k, walkAccel);
    Object* jumping = Ref(gr, R.gmJumping);
    Scale(jumping, R.jmpBaseHeight, std::clamp(s.v[S_JUMP], 0.1f, 20.0f), jumpHeight);
    Fly(player, pc, s, inVeh);
    if (pc) Noclip(pc, s, inVeh);
    else noclipActive = false;
}

bool fastBuildOwned = false, fastScanOwned = false;
double lastCraftScan = 0;

void Crafting(const Snap& s, double now, bool inGame) {
    Object* nc = Static(R.ncMain);
    if (Alive(nc)) {
        if (inGame && (s.on[T_BUILD] || s.on[T_DECON])) {
            int btn = 2;
            bool decon = R.giButtonHeld && CallBool(R.giButtonHeld, nullptr, {&btn});
            uint8_t want = (decon ? s.on[T_DECON] : s.on[T_BUILD]) ? 1 : 0;
            if (Val<uint8_t>(nc, R.ncFastBuild) != want) SetVal<uint8_t>(nc, R.ncFastBuild, want);
            fastBuildOwned = true;
        } else if (fastBuildOwned) {
            SetVal<uint8_t>(nc, R.ncFastBuild, 0);
            fastBuildOwned = false;
        }
        if (inGame && s.on[T_SCAN]) {
            if (!Val<uint8_t>(nc, R.ncFastScan)) SetVal<uint8_t>(nc, R.ncFastScan, 1);
            fastScanOwned = true;
        } else if (fastScanOwned) {
            SetVal<uint8_t>(nc, R.ncFastScan, 0);
            fastScanOwned = false;
        }
    }
    if (!inGame || !(s.on[T_CRAFT] || s.on[T_BUILD]) || now - lastCraftScan < 0.25) return;
    lastCraftScan = now;
    for (Object* cl : FindAll(R.crafterLogic)) {
        if (!Val<int>(cl, R.clTech)) continue;
        float b = Val<float>(cl, R.clBegin), e = Val<float>(cl, R.clEnd);
        if (e <= b + 0.02f) continue;
        bool bay = R.constructorInput && Comp(cl, R.constructorInput) != nullptr;
        if (bay ? s.on[T_BUILD] : s.on[T_CRAFT]) SetVal<float>(cl, R.clEnd, b + 0.01f);
    }
    if (!s.on[T_BUILD]) return;
    for (Object* v : FindAll(R.vfxConstructing)) {
        if (Val<uint8_t>(v, R.vcDone)) continue;
        float c = Val<float>(v, R.vcConstructed);
        if (c < 0 || c >= 1) continue;
        SetVal<float>(v, R.vcDelay, -1.0f);
        SetVal<float>(v, R.vcConstructed, 1.0f);
    }
}

std::unordered_map<int, float> hullOrig;
double lastBaseScan = 0, lastPowerScan = 0;
Hold powerSrc, powerGo;
std::vector<uint32_t> powered;

Object* MakePowerSource() {
    if (!R.goCtor || !R.goAddComponent || !R.powerSource) return nullptr;
    Object* go = mono::New(R.gameObject);
    if (!go) return nullptr;
    Object* name = mono::NewString("FathomPower");
    bool ok = false;
    Call(R.goCtor, go, {name}, &ok);
    if (!ok || !Alive(go)) return nullptr;
    if (R.dontDestroy) Call(R.dontDestroy, nullptr, {go});
    Object* t = mono::TypeObject(R.powerSource);
    Object* src = t ? Call(R.goAddComponent, go, {t}) : nullptr;
    if (!Alive(src)) {
        Call(R.destroy, nullptr, {go});
        return nullptr;
    }
    SetEnabled(src, false);
    powerGo.set(go);
    powerSrc.set(src);
    return src;
}

void ReleasePower() {
    Object* src = powerSrc.get();
    for (uint32_t h : powered) {
        Object* r = mono::Pinned(h);
        if (r && src && Ref(r, R.prInternal) == src) mono::SetRef(r, R.prInternal.f, nullptr);
        mono::Unpin(h);
    }
    powered.clear();
    Object* go = powerGo.get();
    if (Alive(go)) Call(R.destroy, nullptr, {go});
    powerSrc.reset();
    powerGo.reset();
}

void Power(const Snap& s, double now, bool inGame) {
    if (!inGame || !s.on[T_POWER]) {
        if (!powered.empty() || powerSrc.h) ReleasePower();
        return;
    }
    if (!R.prList || !R.prInternal || !R.psPower || !R.psMax) return;
    Object* src = powerSrc.get();
    if (!Alive(src)) {
        if (powerSrc.h || !powered.empty()) ReleasePower();
        src = MakePowerSource();
        lastPowerScan = 0;
    }
    if (!src) return;
    SetVal<float>(src, R.psMax, 10000.0f);
    SetVal<float>(src, R.psPower, 10000.0f);
    if (now - lastPowerScan < 0.5) return;
    lastPowerScan = now;
    for (Object* r : ListItems(Static(R.prList))) {
        if (!Alive(r) || !mono::IsSubclass(mono::ClassOf(r), R.basePowerRelay)) continue;
        Object* in = Ref(r, R.prInternal);
        if (in == src || (in && Alive(in))) continue;
        mono::SetRef(r, R.prInternal.f, src);
        powered.push_back(mono::Pin(r));
    }
}

void Bases(const Snap& s, double now, bool inGame) {
    if (inGame && s.on[T_HULL]) {
        if (now - lastBaseScan >= 0.5) {
            lastBaseScan = now;
            for (Object* bh : FindAll(R.baseHull)) {
                float v = Val<float>(bh, R.bhStrength);
                if (v >= 1.0f) continue;
                int id = InstanceId(bh);
                if (!hullOrig.count(id)) hullOrig[id] = v;
                SetVal<float>(bh, R.bhStrength, 1.0f);
            }
            for (Object* lk : FindAll(R.leakable)) {
                Object* lm = Ref(lk, R.lkLive);
                if (!Alive(lm)) continue;
                float mx = CallFloat(R.lmMaxHealth, lm, {}, 0);
                float h = Val<float>(lm, R.lmHealth);
                if (mx > 0 && h < mx) {
                    float add = mx - h;
                    Call(R.lmAddHealth, lm, {&add});
                }
            }
            for (Object* fs : FindAll(R.floodSim)) {
                Object* arr = Ref(fs, R.fsCells);
                size_t n = mono::Length(arr);
                float* d = n ? (float*)mono::Data(arr, 4) : nullptr;
                for (size_t i = 0; d && i < n; ++i)
                    if (d[i] > 0) d[i] = 0;
            }
        }
    } else if (!hullOrig.empty()) {
        if (inGame)
            for (Object* bh : FindAll(R.baseHull)) {
                auto it = hullOrig.find(InstanceId(bh));
                if (it != hullOrig.end() && Val<float>(bh, R.bhStrength) == 1.0f) SetVal<float>(bh, R.bhStrength, it->second);
            }
        hullOrig.clear();
    }
    Power(s, now, inGame);
}

bool speedOwned = false;

void GameSpeed(const Snap& s, bool inGame) {
    float k = inGame ? std::clamp(s.v[S_GAME], 0.05f, 10.0f) : 1.0f;
    bool want = std::fabs(k - 1.0f) > 1e-3f;
    if (!want && !speedOwned) return;
    float target = want ? k : 1.0f;
    if (R.ftCached) mono::StaticSet(R.ftCached.cls, R.ftCached.f, &target);
    bool frozen = R.ftHasFreezers && CallBool(R.ftHasFreezers, nullptr);
    if (!frozen) {
        float ts = CallFloat(R.getTimeScale, nullptr, {}, 1.0f);
        if (std::fabs(ts - target) > 1e-3f) Call(R.setTimeScale, nullptr, {&target});
    }
    speedOwned = want;
}

std::vector<uint32_t> fogOff;
double lastFogScan = 0;

void Fog(const Snap& s, double now, bool inGame) {
    if (inGame && s.on[T_FOG]) {
        if (now - lastFogScan < 1.0) return;
        lastFogScan = now;
        for (Object* w : FindAll(R.waterscape))
            if (Enabled(w)) {
                SetEnabled(w, false);
                fogOff.push_back(mono::Pin(w));
            }
    } else if (!fogOff.empty()) {
        for (uint32_t h : fogOff) {
            Object* w = mono::Pinned(h);
            if (Alive(w)) SetEnabled(w, true);
            mono::Unpin(h);
        }
        fogOff.clear();
    }
}

Flag hullFlag;
Scaled vF, vB, vS, vV, vT, cF, cE, cV, cB;

void Vehicles(Object* veh, Object* cyc, const Snap& s) {
    Object* live = veh ? Ref(veh, R.vhLive) : cyc ? Ref(cyc, R.srLive) : nullptr;
    ForceFlag(Alive(live) ? live : nullptr, R.lmInvincible, s.on[T_VHULL], hullFlag);
    if (s.on[T_VENERGY]) {
        if (veh && R.vhEnergy && R.vhAddEnergy) {
            float c = 0, cap = 0;
            Call(R.vhEnergy, veh, {&c, &cap});
            if (cap > 0 && c < cap) {
                float add = cap - c;
                Call(R.vhAddEnergy, veh, {&add});
            }
        }
        if (cyc) {
            Object* relay = Ref(cyc, R.srRelay);
            if (Alive(relay)) {
                float p = CallFloat(R.prGetPower, relay, {}, 0), m = CallFloat(R.prGetMax, relay, {}, 0);
                if (m > 0 && p < m) {
                    float add = m - p, out = 0;
                    Call(R.prModify, relay, {&add, &out});
                }
            }
        }
    }
    float k = std::clamp(s.v[S_VSPEED], 0.1f, 20.0f);
    Scale(veh, R.vhForward, k, vF);
    Scale(veh, R.vhBackward, k, vB);
    Scale(veh, R.vhSide, k, vS);
    Scale(veh, R.vhVertical, k, vV);
    Object* exo = veh && R.exosuit && mono::IsSubclass(mono::ClassOf(veh), R.exosuit) ? veh : nullptr;
    Scale(exo, R.exThrust, k, vT);
    Object* sc = cyc ? CompInChildren(cyc, R.subControl) : nullptr;
    Scale(sc, R.scForward, k, cF);
    Scale(sc, R.scPerEngine, k, cE);
    Scale(sc, R.scVertical, k, cV);
    Scale(sc, R.scPerBallast, k, cB);
}

struct Tracked {
    uint32_t tr = 0, live = 0;
    float maxHealth = 0;
    float ox = 0, oy = 0, oz = 0, ex = 0, ey = 0, ez = 0;
    bool box = false;
    int kind = 0;
    std::string name;
};
std::vector<Tracked> tracked;
std::vector<esp::Dot> itemCache;
double lastCreatureScan = 0, lastItemScan = 0;
bool espLive = false;

void ReleaseTracked() {
    for (Tracked& t : tracked) {
        mono::Unpin(t.tr);
        mono::Unpin(t.live);
    }
    tracked.clear();
}

void RescanCreatures(Vec3 pp, const Snap& s) {
    ReleaseTracked();
    float range = s.v[S_RANGE];
    for (Object* c : FindAll(R.creature)) {
        Object* tr = TransformOf(c);
        if (!tr) continue;
        Vec3 p = Position(tr);
        if (Dist(p, pp) > range) continue;
        int tt = TechOf(GameObjectOf(c));
        int kind = Kind(tt);
        if ((kind == 0 && !s.on[T_E_PASSIVE]) || (kind == 1 && !s.on[T_E_HOSTILE]) || (kind == 2 && !s.on[T_E_LEV])) continue;
        Object* lm = Ref(c, R.crLive);
        if (Alive(lm) && Val<float>(lm, R.lmHealth) <= 0) continue;
        Tracked t;
        t.tr = mono::Pin(tr);
        t.live = Alive(lm) ? mono::Pin(lm) : 0;
        t.maxHealth = Alive(lm) ? CallFloat(R.lmMaxHealth, lm, {}, 0) : 0;
        t.kind = kind;
        t.name = TechName(tt);
        if (t.name.empty()) t.name = "Creature";
        Object* col = CompInChildren(c, R.collider);
        if (col && R.colliderBounds) {
            Bounds b = mono::Result<Bounds>(Call(R.colliderBounds, col));
            if (b.extents.x > 0 || b.extents.y > 0 || b.extents.z > 0) {
                t.box = true;
                t.ox = b.center.x - p.x;
                t.oy = b.center.y - p.y;
                t.oz = b.center.z - p.z;
                t.ex = b.extents.x;
                t.ey = b.extents.y;
                t.ez = b.extents.z;
            }
        }
        tracked.push_back(std::move(t));
        if (tracked.size() >= 300) break;
    }
}

void RescanItems(Vec3 pp, const Snap& s) {
    itemCache.clear();
    float range = s.v[S_RANGE];
    auto add = [&](Object* comp, int kind, const std::string& name) {
        if (itemCache.size() >= 400) return;
        Vec3 p = Position(comp);
        if (Dist(p, pp) > range) return;
        esp::Dot d;
        d.x = d.bx = p.x;
        d.y = d.by = p.y;
        d.z = d.bz = p.z;
        d.ex = d.ey = d.ez = 0.4f;
        d.box = true;
        d.kind = kind;
        d.name = name;
        itemCache.push_back(std::move(d));
    };
    if (s.on[T_I_RES] || s.on[T_I_FRAG])
        for (Object* rt : FindAll(R.resourceTracker)) {
            Vec3 p = Position(rt);
            if (Dist(p, pp) > range) continue;
            if (R.blueprintTarget && Comp(rt, R.blueprintTarget)) continue;
            int tt = Val<int>(rt, R.rtTech), ov = Val<int>(rt, R.rtOverride);
            bool frag = (fragmentTech > 0 && tt == fragmentTech) || (R.techFragment && Comp(rt, R.techFragment));
            if (frag ? !s.on[T_I_FRAG] : !s.on[T_I_RES]) continue;
            std::string name = TechName(ov > 0 ? ov : tt);
            if (name.empty()) name = frag ? "Fragment" : "Resource";
            add(rt, frag ? 1 : 0, name);
        }
    if (s.on[T_I_DATA]) {
        for (Object* bp : FindAll(R.blueprintTarget))
            if (!Val<uint8_t>(bp, R.bpUsed)) add(bp, 2, "Data box");
        for (Object* st : FindAll(R.storyTarget)) add(st, 2, "PDA");
    }
    if (s.on[T_I_CRATE])
        for (Object* sc : FindAll(R.supplyCrate))
            if (!Val<uint8_t>(sc, R.scOpen)) add(sc, 3, "Supply crate");
}

void Esp(Object* player, const Snap& s, double now) {
    bool cre = player && s.on[T_ESP], itm = player && s.on[T_IESP];
    if (!cre && !itm) {
        if (espLive) {
            ReleaseTracked();
            itemCache.clear();
            esp::Clear();
            espLive = false;
        }
        return;
    }
    espLive = true;
    Object* cam = Call(R.camMain, nullptr);
    if (!Alive(cam) || !R.camView || !R.camProj) {
        esp::Clear();
        return;
    }
    esp::Frame f;
    Object* v = Call(R.camView, cam);
    Object* p = Call(R.camProj, cam);
    if (!v || !p) return;
    std::memcpy(f.view, mono::Unbox(v), sizeof(f.view));
    std::memcpy(f.proj, mono::Unbox(p), sizeof(f.proj));
    Vec3 pp = Position(player);
    f.px = pp.x;
    f.py = pp.y;
    f.pz = pp.z;
    f.valid = true;
    f.creatures = cre;
    f.items = itm;
    f.cName = s.on[T_E_NAME];
    f.cDist = s.on[T_E_DIST];
    f.cHealth = s.on[T_E_HEALTH];
    f.cBox = s.on[T_E_BOX];
    f.iName = s.on[T_I_NAME];
    f.iDist = s.on[T_I_DIST];
    f.iBox = s.on[T_I_BOX];
    if (cre) {
        if (now - lastCreatureScan >= 0.5) {
            lastCreatureScan = now;
            RescanCreatures(pp, s);
        }
        f.creatureDots.reserve(tracked.size());
        for (const Tracked& t : tracked) {
            Object* tr = mono::Pinned(t.tr);
            if (!Alive(tr)) continue;
            Vec3 c = Position(tr);
            esp::Dot d;
            d.x = c.x;
            d.y = c.y;
            d.z = c.z;
            d.box = t.box;
            d.bx = c.x + t.ox;
            d.by = c.y + t.oy;
            d.bz = c.z + t.oz;
            d.ex = t.ex;
            d.ey = t.ey;
            d.ez = t.ez;
            d.kind = t.kind;
            d.name = t.name;
            Object* lm = mono::Pinned(t.live);
            if (lm && t.maxHealth > 0) {
                float h = Val<float>(lm, R.lmHealth);
                if (h <= 0) continue;
                d.health = h / t.maxHealth;
            }
            f.creatureDots.push_back(std::move(d));
        }
    } else if (!tracked.empty()) {
        ReleaseTracked();
    }
    if (itm) {
        if (now - lastItemScan >= 1.0) {
            lastItemScan = now;
            RescanItems(pp, s);
        }
        f.itemDots = itemCache;
    } else {
        itemCache.clear();
    }
    esp::Publish(std::move(f));
}

void ClearHostiles(Object* player, float radius) {
    Vec3 pp = Position(player);
    int n = 0, dmg = 0;
    for (Object* c : FindAll(R.creature)) {
        if (Dist(Position(c), pp) > radius) continue;
        if (Kind(TechOf(GameObjectOf(c))) != 1) continue;
        Object* lm = Ref(c, R.crLive);
        if (!Alive(lm) || Val<float>(lm, R.lmHealth) <= 0) continue;
        Call(R.lmKill, lm, {&dmg});
        ++n;
    }
    if (n) Note("Cleared " + std::to_string(n) + (n == 1 ? " hostile creature" : " hostile creatures"));
    else Note("No hostile creatures in range");
}

void Actions(Object* player, bool inGame, const Snap& s) {
    std::vector<std::string> todo;
    {
        std::lock_guard<std::mutex> g(qLock);
        todo.swap(queued);
    }
    for (const std::string& a : todo) {
        if (!inGame) {
            Note("Load a save first");
            continue;
        }
        if (a == "refill_health") {
            Object* live = Ref(player, R.plLive);
            if (!Alive(live)) continue;
            float mx = CallFloat(R.lmMaxHealth, live, {}, 0), h = Val<float>(live, R.lmHealth);
            if (mx > 0 && h > 0 && h < mx) {
                float add = mx - h;
                Call(R.lmAddHealth, live, {&add});
            }
        } else if (a == "refill_food" || a == "refill_water") {
            Object* sv = Comp(player, R.survival);
            const Fld& f = a == "refill_food" ? R.svFood : R.svWater;
            if (sv && Val<float>(sv, f) < 100.0f) SetVal<float>(sv, f, 100.0f);
        } else if (a == "refill_oxygen") {
            RefillOxygen(player);
        } else if (a == "clear_hostiles") {
            ClearHostiles(player, s.v[S_CLEAR]);
        }
    }
}

bool cursorFreed = false, lockWas = false;

void MenuInput(bool stopping) {
    bool focus = !stopping && menuOpen.load() && menuFocused.load();
    if (focus) {
        if (R.giClearFrame && R.frameCount) {
            int until = CallInt(R.frameCount, nullptr, {}, 0) + 2;
            mono::StaticSet(R.giClearFrame.cls, R.giClearFrame.f, &until);
        } else if (R.giClearInput) {
            int n = 2;
            Call(R.giClearInput, nullptr, {&n});
        }
        if (!cursorFreed) {
            lockWas = R.uwGetLock && CallBool(R.uwGetLock, nullptr);
            cursorFreed = true;
        }
        if (R.uwGetLock && R.uwSetLock && CallBool(R.uwGetLock, nullptr)) {
            int f = 0;
            Call(R.uwSetLock, nullptr, {&f});
        }
    } else if (cursorFreed) {
        cursorFreed = false;
        if (lockWas && R.uwSetLock) {
            int t = 1;
            Call(R.uwSetLock, nullptr, {&t});
        }
    }
}

void ReleaseWorld(const Snap& off) {
    Vitals(nullptr, off);
    Stealth(nullptr, nullptr, nullptr, off, 0);
    Movement(nullptr, off, false);
    Vehicles(nullptr, nullptr, off);
    flyActive = false;
    noclipActive = false;
    ReleaseTracked();
    itemCache.clear();
    if (espLive) {
        esp::Clear();
        espLive = false;
    }
}

void Unsubscribe() {
    Object* del = mono::Pinned(delegateHandle);
    if (del) Call(R.removeWillRender, nullptr, {del});
    mono::Unpin(delegateHandle);
    delegateHandle = 0;
}

void Tick() {
    try {
        lastTick = GetTickCount64();
        bool stopping = stop.load() == 1;
        Snap s = stopping ? Off() : TakeSnap();
        double now = Now();
        MenuInput(stopping);
        Object* player = Static(R.playerMain);
        bool inGame = Alive(player) && !Loading();
        GameModes(s, inGame);
        GameSpeed(s, inGame);
        Crafting(s, now, inGame);
        Bases(s, now, inGame);
        Fog(s, now, inGame);
        if (inGame) {
            Object* veh = Call(R.plGetVehicle, player);
            if (!Alive(veh)) veh = nullptr;
            Object* sub = Call(R.plCurrentSub, player);
            Object* cyc = Alive(sub) && Val<uint8_t>(sub, R.srCyclops) ? sub : nullptr;
            Vitals(player, s);
            Stealth(player, veh, cyc, s, now);
            Movement(player, s, veh || cyc);
            Vehicles(veh, cyc, s);
            Esp(player, s, now);
        } else if (wasInGame || stopping) {
            ReleaseWorld(Off());
        }
        wasInGame = inGame;
        Actions(player, inGame, s);
        prev = s;
        if (stopping) {
            ReleaseWorld(Off());
            ReleasePower();
            Unsubscribe();
            stop = 2;
        }
    } catch (...) {
        if (faults++ < 5) LOG("error in the game update");
    }
}

int Filter(unsigned int code) {
    if (faults++ < 5) LOG("crash avoided in the game update (code %08X)", code);
    return EXCEPTION_EXECUTE_HANDLER;
}

void SafeTick() {
    __try {
        Tick();
    } __except (Filter(GetExceptionCode())) {
    }
}

void __stdcall OnRender() {
    if (stop.load() == 2) return;
    ticking.fetch_add(1);
    SafeTick();
    ticking.fetch_sub(1);
}

bool Supported(const std::string& id) {
    if (id == "godmode") return (bool)R.lmInvincible;
    if (id == "unlimited_health" || id == "refill_health") return R.lmHealth && R.lmMaxHealth;
    if (id == "radiation_immunity" || id == "infinite_oxygen" || id == "ghost" || id == "unlock_blueprints" || id == "free_craft")
        return R.gmCheats && R.gmActivate;
    if (id == "unlimited_food" || id == "unlimited_water" || id == "refill_food" || id == "refill_water") return R.svFood && R.svWater;
    if (id == "refill_oxygen") return R.oxAdd && R.oxCapacity;
    if (id == "player_speed") return R.pmDebugSpeed && R.pmForward;
    if (id == "jump_height") return R.gmJumping && R.jmpBaseHeight;
    if (id == "noclip") return R.rbSetDetect && R.pmRb;
    if (id == "fly" || id == "fly_speed") return R.pcSetEnabled && R.trForward && R.camMain;
    if (id == "instant_craft") return R.clEnd && R.clBegin;
    if (id == "instant_build" || id == "instant_deconstruct") return R.ncMain && R.ncFastBuild;
    if (id == "instant_scan") return R.ncMain && R.ncFastScan;
    if (id == "base_integrity") return R.bhStrength && R.lkLive;
    if (id == "unlimited_power") return R.prList && R.prInternal && R.goAddComponent;
    if (id == "game_speed") return R.setTimeScale && R.ftCached;
    if (id == "remove_fog") return R.waterscape != nullptr;
    if (id == "vehicle_hull") return R.vhLive && R.srLive;
    if (id == "vehicle_energy") return R.vhEnergy && R.prModify;
    if (id == "vehicle_invisible") return R.ecoTarget != nullptr;
    if (id == "vehicle_speed") return R.vhForward && R.scForward;
    if (id == "clear_radius" || id == "clear_hostiles") return R.lmKill && R.creature;
    if (id == "esp" || id == "item_esp") return R.camView && R.camProj;
    return true;
}

void ResolveKinds() {
    levSet.clear();
    hostileSet.clear();
    kindCache.clear();
    for (const char* n : {"ReaperLeviathan", "GhostLeviathan", "GhostLeviathanJuvenile", "SeaDragon", "SeaEmperor", "SeaEmperorLeviathan",
                          "SeaEmperorJuvenile", "Reefback", "SeaTreader"}) {
        int v = mono::EnumValue(R.techType, n);
        if (v > 0) levSet.insert(v);
    }
    for (const char* n : {"Stalker", "Sandshark", "BoneShark", "Biter", "Bleeder", "Blighter", "Shocker", "Warper", "CrabSquid",
                          "Crabsnake", "LavaLizard", "Mesmer", "SpineEel", "CaveCrawler", "LavaLarva", "Crash", "PrecursorDroid"}) {
        int v = mono::EnumValue(R.techType, n);
        if (v > 0) hostileSet.insert(v);
    }
    fragmentTech = mono::EnumValue(R.techType, "Fragment");
}

}

bool Prepare(std::string& err) {
    if (!un::Resolve(err)) return false;
    ResolveKinds();
    return true;
}

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    for (int i = 0; i < T_COUNT; ++i) tog[i] = m.find(kToggleIds[i]);
    for (int i = 0; i < S_COUNT; ++i) sld[i] = m.find(kSliderIds[i]);
    auto none = [](Feature&) {};
    for (Feature& f : m.features) {
        bool action = false, known = false;
        for (const char* a : kActions)
            if (f.id == a) action = known = true;
        for (const char* t : kToggleIds)
            if (f.id == t) known = true;
        for (const char* t : kSliderIds)
            if (f.id == t) known = true;
        if (!known) continue;
        f.available = Supported(f.id);
        if (!f.available) {
            f.missing = "Not found in this version of the game";
            continue;
        }
        if (action) {
            std::string id = f.id;
            f.apply = [id](Feature&) {
                std::lock_guard<std::mutex> g(qLock);
                if (queued.size() < 16) queued.push_back(id);
            };
        } else {
            f.apply = none;
        }
    }
}

bool Start(std::string& err) {
    Object* type = mono::TypeObject(R.willRenderType);
    void* fn = (void*)&OnRender;
    bool ok = false;
    Object* del = type ? Call(R.delegateForPointer, nullptr, {&fn, type}, &ok) : nullptr;
    if (!ok || !del) {
        err = "could not create the game callback";
        return false;
    }
    delegateHandle = mono::Pin(del);
    Call(R.addWillRender, nullptr, {del}, &ok);
    if (!ok) {
        mono::Unpin(delegateHandle);
        delegateHandle = 0;
        err = "could not attach to the game's frame";
        return false;
    }
    LOG("attached to the game's frame");
    return true;
}

void RequestStop() {
    int expected = 0;
    stop.compare_exchange_strong(expected, 1);
}

bool Stopped() { return stop.load() == 2 && ticking.load() == 0; }

void ForceStop() {
    if (stop.load() == 2) return;
    stop = 2;
    for (int i = 0; i < 200 && ticking.load() > 0; ++i) Sleep(10);
    Unsubscribe();
    ReleaseTracked();
    for (uint32_t h : fogOff) mono::Unpin(h);
    fogOff.clear();
    for (uint32_t h : powered) mono::Unpin(h);
    powered.clear();
    esp::Clear();
}

bool Ticking() { return GetTickCount64() - lastTick.load() < 2000; }

std::vector<std::string> TakeNotes() {
    std::lock_guard<std::mutex> g(qLock);
    std::vector<std::string> out;
    out.swap(notes);
    return out;
}

void SetMenu(bool open, bool focused) {
    menuOpen = open;
    menuFocused = focused;
}

void SetGameWindow(void* hwnd) { gameWnd = hwnd; }

}
