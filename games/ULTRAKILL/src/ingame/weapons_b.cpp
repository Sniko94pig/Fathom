#include "ingame/groups.h"
#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace ukweapb {

namespace {

using namespace un;
using uk::Ctx;

const int kRocketCap = 100;
const size_t kTrackCap = 64;

Class *gcCls, *wcCls, *dwCls, *nailCls, *zapCls, *railCls, *rocketCls, *imCls;
Fld gcActivated, gcCurrent, gcDuals, dwCurrent, widDelay;
Fld nVar, nAlt, nRate, nCurRate, nCooldown, nHeat, nBurn, nSinks, nZapper, zCharge, zAttached;
Fld rWid, rReady, lVar, lCooldown, lCharge, lRate;
Fld wSinks, wSawSinks, wAmmo, wSaws, wMagnets, wZapper, wRail, wRailPlayed, wStored, wRockets, wFreeze, wCannon, wFuel;
Fld imSource, piFire1, isPressed, isFrame;
Method *rShoot, *mbInvoke, *imCombo, *fixedStep;
bool okFind, okShots, okCharge, okReload, okAuto;

struct Guns {
    std::vector<Object*> nails, rails, rockets;
    Object* mainRail = nullptr;
};

float railAcc = 0;
int railId = 0, railBroken = 0;
std::unordered_map<int, bool> burst;
std::unordered_set<int> told;
bool stepTold = false;

bool Is(Object* o, const Fld& f) { return Val<uint8_t>(o, f) != 0; }

void Fill(Object* o, const Fld& f, float v) {
    if (o && f && Val<float>(o, f, v) != v) SetVal<float>(o, f, v);
}

void StopRail() {
    railAcc = 0;
    railId = 0;
}

void Forget() {
    StopRail();
    railBroken = 0;
    burst.clear();
}

float Rate(const Ctx& c) {
    float m = c.value("fire_rate", 5.0f);
    if (!(m >= 1.0f)) return 1.0f;
    return m > 20.0f ? 20.0f : m;
}

void Tell(int kind, int var, bool alt, float rate) {
    int key = kind * 64 + (var & 15) * 2 + (alt ? 1 : 0);
    if (!told.insert(key).second) return;
    if (kind == 0)
        LOG("weapons_b: nailgun variation %d%s waits %.3f hundredths of a second between shots", var, alt ? " (sawblade launcher)" : "",
            rate);
    else
        LOG("weapons_b: rocket launcher variation %d waits %.3f s between rockets", var, rate);
}

void TellStep() {
    if (stepTold || !fixedStep) return;
    stepTold = true;
    float s = CallFloat(fixedStep, nullptr);
    if (s > 0) LOG("weapons_b: physics step %.4f s, a nailgun fires at most %.0f times per second", s, 1.0f / s);
}

void KeepRail(Object* wc) {
    if (!wRail || Val<float>(wc, wRail, 5.0f) == 5.0f) return;
    SetVal<float>(wc, wRail, 5.0f);
    if (wRailPlayed) SetVal<uint8_t>(wc, wRailPlayed, 1);
}

void Meters(Object* wc, bool shots, bool reload) {
    if (!wc) return;
    if (shots || reload) {
        Fill(wc, wAmmo, 100.0f);
        Fill(wc, wSaws, 10.0f);
        KeepRail(wc);
    }
    if (shots) Fill(wc, wStored, 0.0f);
    if (!reload) return;
    Fill(wc, wSinks, 2.0f);
    Fill(wc, wSawSinks, 1.0f);
    Fill(wc, wMagnets, 3.0f);
    Fill(wc, wZapper, 5.0f);
    Fill(wc, wFreeze, 5.0f);
    Fill(wc, wCannon, 1.0f);
    Fill(wc, wFuel, 1.0f);
}

void Add(Object* go, bool main, Guns& g) {
    if (!Alive(go)) return;
    if (Object* n = Comp(go, nailCls)) {
        if (ActiveAndEnabled(n)) g.nails.push_back(n);
    } else if (Object* r = Comp(go, railCls)) {
        if (!ActiveAndEnabled(r)) return;
        g.rails.push_back(r);
        if (main) g.mainRail = r;
    } else if (Object* l = Comp(go, rocketCls)) {
        if (ActiveAndEnabled(l)) g.rockets.push_back(l);
    }
}

Guns Collect(Object* gc) {
    Guns g;
    Add(Ref(gc, gcCurrent), true, g);
    if (dwCls && dwCurrent && Val<int>(gc, gcDuals) > 0)
        for (Object* d : CompsInChildren(gc, dwCls)) Add(Ref(d, dwCurrent), false, g);
    return g;
}

void Overheat(Object* ng, bool burn) {
    int id = InstanceId(ng);
    if (!id) return;
    auto it = burst.find(id);
    bool was = it != burst.end() && it->second;
    if (burn && !was) {
        SetVal<float>(ng, nHeat, 1.0f);
    } else if (!burn && Val<float>(ng, nSinks) >= 1.0f && Val<float>(ng, nHeat, 1.0f) < 0.25f) {
        SetVal<float>(ng, nHeat, 0.25f);
    }
    if (it == burst.end() && burst.size() >= kTrackCap) burst.clear();
    burst[id] = burn;
}

void Nail(Object* ng, bool shots, bool charge, bool reload, float m) {
    int var = Val<int>(ng, nVar, -1);
    bool alt = Is(ng, nAlt);
    bool burn = Is(ng, nBurn);
    if (reload && var == 0 && nAlt) Fill(ng, nSinks, alt ? 1.0f : 2.0f);
    if (shots && nRate && nCurRate && nCooldown) {
        float rate = Val<float>(ng, nRate);
        Tell(0, var, alt, rate);
        if (var == 0 && burn) rate = alt ? 3.0f : rate - 2.5f;
        float want = std::max(0.0f, rate / m);
        SetVal<float>(ng, nCurRate, want);
        if (Val<float>(ng, nCooldown) > want) SetVal<float>(ng, nCooldown, want);
    }
    if (!charge) return;
    if (var == 2 && nZapper) {
        Object* z = Ref(ng, nZapper);
        if (Alive(z) && Is(z, zAttached) && Val<float>(z, zCharge, 5.0f) < 5.0f) SetVal<float>(z, zCharge, 5.0f);
    } else if (var == 0 && !alt && nAlt && nHeat && nSinks && nBurn) {
        Overheat(ng, burn);
    }
}

void Rocket(Object* rl, Object* wc, bool shots, bool charge, float m, float dt) {
    int var = Val<int>(rl, lVar, -1);
    if (shots) {
        if (lRate) Tell(1, var, false, Val<float>(rl, lRate));
        float cd = Val<float>(rl, lCooldown);
        bool crowded = wc && Val<int>(wc, wRockets) >= kRocketCap;
        if (cd > 0 && dt > 0 && m > 1.0f && !crowded) SetVal<float>(rl, lCooldown, std::max(0.0f, cd - (m - 1.0f) * dt));
    }
    if (charge && var == 1) {
        float cb = Val<float>(rl, lCharge);
        if (cb > 0 && cb < 1.0f) SetVal<float>(rl, lCharge, 1.0f);
    }
}

bool Fire(Object* rail) {
    Object* wid = Ref(rail, rWid);
    float delay = Alive(wid) ? Val<float>(wid, widDelay) : 0.0f;
    bool ok = false;
    if (delay != 0.0f && mbInvoke) {
        Object* name = mono::NewString("Shoot");
        if (name) Call(mbInvoke, rail, {name, &delay}, &ok);
    } else {
        Call(rShoot, rail, {}, &ok);
    }
    return ok;
}

void AutoRail(const Ctx& c, Object* gc, const Guns& g, float m) {
    Object* main = g.mainRail;
    if (!okAuto || !main) {
        StopRail();
        return;
    }
    int id = InstanceId(main);
    if (id != railId) {
        railId = id;
        railAcc = 0;
    }
    Object* im = Single(imCls);
    Object* src = Ref(im, imSource);
    Object* fire = src ? Ref(src, piFire1) : nullptr;
    if (!fire || !Is(fire, isPressed) || Val<int>(fire, isFrame, -1) == c.frame) {
        railAcc = 0;
        return;
    }
    if (id == railBroken || c.dt <= 0 || c.menuFocused || !Is(gc, gcActivated) || !Is(main, rReady)) return;
    Object* gsm = Static(R.gsmInstance);
    if (Alive(gsm) && Is(gsm, R.gsmInputLocked)) return;
    float every = 1.0f / m;
    railAcc += c.dt;
    if (railAcc < every) return;
    railAcc -= every;
    if (railAcc >= every) railAcc = 0;
    if (imCombo && CallBool(imCombo, im)) return;
    if (!Fire(main)) {
        railBroken = id;
        LOG("weapons_b: the railcannon did not fire, holding fire stays off for this railcannon");
        return;
    }
    for (Object* r : g.rails)
        if (r != main && Is(r, rReady)) Fire(r);
}

}

bool Resolve() {
    gcCls = Cls("GunControl");
    gcActivated = F(gcCls, "activated");
    gcCurrent = F(gcCls, "currentWeapon");
    gcDuals = F(gcCls, "dualWieldCount");
    dwCls = Cls("DualWield");
    dwCurrent = F(dwCls, "currentWeapon");
    widDelay = F(Cls("WeaponIdentifier"), "delay");

    nailCls = Cls("Nailgun");
    nVar = F(nailCls, "variation");
    nAlt = F(nailCls, "altVersion");
    nRate = F(nailCls, "fireRate");
    nCurRate = F(nailCls, "currentFireRate");
    nCooldown = F(nailCls, "fireCooldown");
    nHeat = F(nailCls, "heatUp");
    nBurn = F(nailCls, "burnOut");
    nSinks = F(nailCls, "heatSinks");
    nZapper = F(nailCls, "currentZapper");
    zapCls = Cls("Zapper");
    zCharge = F(zapCls, "charge");
    zAttached = F(zapCls, "attached");

    railCls = Cls("Railcannon");
    rWid = F(railCls, "wid");
    rReady = F(railCls, "gotStuff");
    rShoot = M(railCls, "Shoot", 0);

    rocketCls = Cls("RocketLauncher");
    lVar = F(rocketCls, "variation");
    lCooldown = F(rocketCls, "cooldown");
    lCharge = F(rocketCls, "cbCharge");
    lRate = F(rocketCls, "rateOfFire");

    wcCls = Cls("WeaponCharges");
    wSinks = F(wcCls, "naiHeatsinks");
    wSawSinks = F(wcCls, "naiSawHeatsinks");
    wAmmo = F(wcCls, "naiAmmo");
    wSaws = F(wcCls, "naiSaws");
    wMagnets = F(wcCls, "naiMagnetCharge");
    wZapper = F(wcCls, "naiZapperRecharge");
    wRail = F(wcCls, "raicharge");
    wRailPlayed = F(wcCls, "railChargePlayed");
    wStored = F(wcCls, "rocketcharge");
    wRockets = F(wcCls, "rocketCount");
    wFreeze = F(wcCls, "rocketFreezeTime");
    wCannon = F(wcCls, "rocketCannonballCharge");
    wFuel = F(wcCls, "rocketNapalmFuel");

    imCls = Cls("InputManager");
    imSource = F(imCls, "<InputSource>k__BackingField");
    piFire1 = F(Cls("PlayerInput"), "Fire1");
    Class* state = Cls("InputActionState");
    isPressed = F(state, "<IsPressed>k__BackingField");
    isFrame = F(state, "<PerformedFrame>k__BackingField");
    imCombo = M(imCls, "PerformingCheatMenuCombo", 0);
    mbInvoke = MT(Cls("UnityEngine.MonoBehaviour"), "Invoke", {"System.String", "System.Single"});
    fixedStep = M(R.time, "get_fixedDeltaTime", 0);

    okFind = gcCls && gcCurrent;
    bool nailRate = nailCls && nVar && nRate && nCurRate && nCooldown;
    bool rocketRate = rocketCls && lCooldown;
    bool railKeep = wcCls && wRail;
    bool ammo = wcCls && (wAmmo || wSaws);
    okShots = (okFind && (nailRate || rocketRate)) || railKeep || ammo;
    okAuto = okFind && railKeep && railCls && rShoot && rReady && imCls && imSource && piFire1 && isPressed && isFrame && gcActivated;
    bool cannon = rocketCls && lVar && lCharge;
    bool zap = nailCls && nVar && nZapper && zCharge && zAttached;
    bool heat = nailCls && nVar && nAlt && nBurn && nHeat && nSinks;
    okCharge = okFind && (cannon || zap || heat);
    bool meters = wcCls && (wSinks || wSawSinks || wAmmo || wSaws || wMagnets || wZapper || wRail || wFreeze || wCannon || wFuel);
    okReload = meters || (okFind && nailCls && nVar && nAlt && nSinks);
    if (okShots && !okAuto) LOG("weapons_b: the railcannon will not fire by itself while the button is held");
    return okShots || okCharge || okReload;
}

bool Supported(const std::string& id) {
    if (id == "unlimited_shots" || id == "fire_rate") return okShots;
    if (id == "instant_charge") return okCharge;
    if (id == "no_reload") return okReload;
    return false;
}

void Tick(const uk::Ctx& c) {
    bool shots = okShots && c.on("unlimited_shots");
    bool charge = okCharge && c.on("instant_charge");
    bool reload = okReload && c.on("no_reload");
    if (!shots) StopRail();
    if (!charge && !burst.empty()) burst.clear();
    if ((!shots && !charge && !reload) || !c.inLevel || c.dead) return;
    TellStep();
    float m = Rate(c);
    Object* wc = wcCls ? Single(wcCls) : nullptr;
    Meters(wc, shots, reload);
    Object* gc = okFind ? Single(gcCls) : nullptr;
    if (!gc) return;
    Guns g = Collect(gc);
    for (Object* n : g.nails) Nail(n, shots, charge, reload, m);
    for (Object* l : g.rockets) Rocket(l, wc, shots, charge, m, c.dt);
    if (shots) AutoRail(c, gc, g, m);
}

void Release(const uk::Ctx&) { Forget(); }

void Action(const std::string&, const uk::Ctx&) {}

bool Busy() { return false; }

}
