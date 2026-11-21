#include "ingame/groups.h"
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace ukweapa {

using namespace uk;
using namespace un;

namespace {

struct Tracked {
    Hold anim;
    float written = 1.0f;
    bool leaving = false;
};

struct Frame {
    const Ctx* c = nullptr;
    bool shots = false;
    bool charge = false;
    bool reload = false;
    float m = 1.0f;
    float k = 0.0f;
    float dt = 0.0f;
    float now = -1.0f;
    Object* wc = nullptr;
    int fire = -1;
    bool swung = false;
};

Class *gcCls, *wcCls, *dwCls, *widCls, *revCls, *shoCls, *hamCls, *hzCls, *imCls, *animCls, *mbCls;
Fld gcActivated, gcCurrent, gcDual, dwCurrent, widDelay;
Fld rVar, rGunReady, rShootReady, rPierceReady, rShootCharge, rPierceCharge, rCharging, rPierceShot, rAnim;
Fld sVar, sAnim, sCharging, sForce, sPrimary, sSaw, sSince;
Fld hVar, hGunReady, hAnim, hPrimary, hSwinging, hSwing, hPulled, hCooldown, hCharging, hForce, hSaw, hWid;
Fld wcRev0, wcRev1, wcRev2, wcRevAlt, wcNade, wcSaw, wcSawReset, wcCooldowns, wcYellows, wcYellowsTimer;
Fld hzHits, imSource, piFire1, iasPressed;
Method *hImpact, *imCombo, *getSpeed, *setSpeed, *mbInvoke, *mbIsInvoking, *mbPending, *timeNow;
bool shotsOk, chargeOk, reloadOk, swingOk, copySwingOk;
bool heldSwing = false;
std::vector<Tracked> tracked;

bool Of(Object* o, Class* k) { return o && k && mono::IsSubclass(mono::ClassOf(o), k); }

Fld Typed(Class* owner, const char* name, Class* type) {
    Fld f = F(owner, name);
    if (!f || !type) return {};
    if (f.isStatic || !mono::IsSubclass(mono::FieldClass(f.f), type)) {
        Missing(std::string(mono::ClassName(owner)) + "." + name + " has an unexpected type");
        return {};
    }
    return f;
}

Fld Floats(Class* owner, const char* name) {
    Fld f = F(owner, name);
    if (!f) return {};
    if (f.isStatic || std::string(mono::ClassName(mono::FieldClass(f.f))) != "Single[]") {
        Missing(std::string(mono::ClassName(owner)) + "." + name + " is not a float array");
        return {};
    }
    return f;
}

void Forget(size_t i) {
    tracked[i].anim.reset();
    tracked.erase(tracked.begin() + (std::ptrdiff_t)i);
}

void Normal(const Tracked& t, Object* a) {
    float cur = CallFloat(getSpeed, a, {}, t.written);
    if (std::fabs(cur - t.written) > 1e-3f || std::fabs(cur - 1.0f) < 1e-6f) return;
    float one = 1.0f;
    Call(setSpeed, a, {&one});
}

void Restore(bool all) {
    for (size_t i = 0; i < tracked.size();) {
        Tracked& t = tracked[i];
        Object* a = t.anim.get();
        if (!Alive(a)) {
            Forget(i);
            continue;
        }
        bool done = all || ActiveInHierarchy(a);
        if (done || !t.leaving) Normal(t, a);
        if (done) {
            Forget(i);
            continue;
        }
        t.leaving = true;
        ++i;
    }
}

void Prune() {
    for (size_t i = 0; i < tracked.size();) {
        if (!Alive(tracked[i].anim.get())) {
            Forget(i);
            continue;
        }
        ++i;
    }
}

void Speed(Object* anim, float m) {
    if (!getSpeed || !setSpeed || !Of(anim, animCls) || !Alive(anim)) return;
    Tracked* t = nullptr;
    for (Tracked& e : tracked)
        if (e.anim.get() == anim) {
            t = &e;
            break;
        }
    float cur = CallFloat(getSpeed, anim, {}, m);
    if (!t) {
        if ((std::fabs(cur - 1.0f) < 1e-4f && std::fabs(m - 1.0f) < 1e-4f) || tracked.size() >= 64) return;
        tracked.emplace_back();
        t = &tracked.back();
        t->anim.set(anim);
    }
    t->leaving = false;
    if (std::fabs(cur - m) > 1e-4f) Call(setSpeed, anim, {&m});
    t->written = m;
}

void Raise(Object* o, const Fld& f, float add, float cap) {
    if (!f) return;
    float v = Val<float>(o, f, cap);
    if (v < cap) SetVal<float>(o, f, std::min(cap, v + add));
}

void Rewind(Object* o, const Fld& f, const Frame& fr) {
    if (!f || fr.now < 0.0f) return;
    float t = Val<float>(o, f, fr.now);
    if (fr.now - t < 1.0f) SetVal<float>(o, f, t - fr.k * fr.dt);
}

void Fill(Object* o, const Fld& f, float v) {
    if (f && Val<float>(o, f, v) != v) SetVal<float>(o, f, v);
}

void Zero(Object* wc, const Fld& f) {
    Object* arr = Ref(wc, f);
    size_t n = mono::Length(arr);
    float* d = n ? (float*)mono::Data(arr, (int)sizeof(float)) : nullptr;
    if (!d) return;
    for (size_t i = 0; i < n; ++i)
        if (d[i] != 0.0f) d[i] = 0.0f;
}

void Refill(Object* wc) {
    Fill(wc, wcRev0, 100.0f);
    Fill(wc, wcRev1, 400.0f);
    Fill(wc, wcRev2, 300.0f);
    Fill(wc, wcNade, 1.0f);
    Fill(wc, wcSaw, 1.0f);
    Fill(wc, wcSawReset, 0.0f);
    Fill(wc, wcYellowsTimer, 0.0f);
    if (wcYellows && Val<int>(wc, wcYellows, 0) != 0) SetVal<int>(wc, wcYellows, 0);
    Zero(wc, wcRevAlt);
    Zero(wc, wcCooldowns);
}

void SawFix(Object* zone) {
    if (Of(zone, hzCls) && Alive(zone) && Val<int>(zone, hzHits, 0) != 0) SetVal<int>(zone, hzHits, 0);
}

Object* Fire1(Object* im) { return Ref(Ref(im, imSource), piFire1); }

bool Pressed() {
    Object* f1 = Fire1(Single(imCls));
    return f1 && Val<uint8_t>(f1, iasPressed) != 0;
}

bool Held(const Ctx& c) {
    if (c.dt <= 0.0f || c.paused || c.menuFocused || !c.activated) return false;
    Object* g = Static(R.gsmInstance);
    if (!Alive(g) || Val<uint8_t>(g, R.gsmInputLocked, 1) != 0) return false;
    Object* im = Single(imCls);
    Object* f1 = Fire1(im);
    if (!f1 || !Val<uint8_t>(f1, iasPressed)) return false;
    return !(imCombo && CallBool(imCombo, im, {}, true));
}

bool FireHeld(Frame& f) {
    if (f.fire < 0) f.fire = Held(*f.c) ? 1 : 0;
    return f.fire == 1;
}

void AutoSwing(Object* h, Frame& f, bool copy) {
    if (!swingOk || !Val<uint8_t>(h, hSwinging) || Val<float>(h, hSwing, 0.0f) != 1.0f || !Val<uint8_t>(h, hGunReady)) return;
    if (!FireHeld(f) || !ActiveAndEnabled(h)) return;
    if (!copy) {
        SetVal<uint8_t>(h, hSwinging, 0);
        SetVal<float>(h, hSwing, 0.0f);
        Call(hImpact, h);
        if (Val<float>(h, hCooldown, 0.5f) < 0.5f) SetVal<float>(h, hCooldown, 0.5f);
        f.swung = true;
        return;
    }
    if (!copySwingOk) return;
    Object* wid = Ref(h, hWid);
    if (!Alive(wid)) return;
    float delay = Val<float>(wid, widDelay, 0.0f);
    if (!std::isfinite(delay) || delay < 0.0f) return;
    Object* name = mono::NewString("Impact");
    if (!name || CallBool(mbIsInvoking, h, {name}, true)) return;
    SetVal<uint8_t>(h, hSwinging, 0);
    SetVal<float>(h, hSwing, 0.0f);
    SetVal<uint8_t>(h, hGunReady, 0);
    if (Val<float>(h, hCooldown, 0.5f) < 0.5f) SetVal<float>(h, hCooldown, 0.5f);
    Call(mbInvoke, h, {name, &delay});
    f.swung = true;
}

bool Pending(Object* b) { return !mbPending || CallBool(mbPending, b, {}, true); }

void Revolver(Object* r, Frame& f, bool copy) {
    int var = Val<int>(r, rVar, -1);
    if (f.shots) {
        Speed(Ref(r, rAnim), f.m);
        if (f.k > 0.0f && f.dt > 0.0f) {
            if (!Val<uint8_t>(r, rShootReady, 1) && !(copy && Pending(r))) Raise(r, rShootCharge, f.k * 200.0f * f.dt, 100.0f);
            if (var == 1 && !Val<uint8_t>(r, rPierceReady, 1)) Raise(r, rPierceCharge, f.k * 480.0f * f.dt, 100.0f);
        }
    }
    if (f.charge && (var == 0 || var == 2) && Val<uint8_t>(r, rCharging) && Val<uint8_t>(r, rGunReady)) {
        float v = Val<float>(r, rPierceShot, 0.0f);
        if (v > 0.0f && v < 100.0f) SetVal<float>(r, rPierceShot, 100.0f);
    }
    if (f.reload && var == 0) Raise(r, rPierceCharge, 100.0f, 100.0f);
}

void Shotgun(Object* s, Frame& f) {
    int var = Val<int>(s, sVar, -1);
    if (f.shots) {
        Speed(Ref(s, sAnim), f.m);
        if (f.k > 0.0f && f.dt > 0.0f && (var == 0 || var == 2)) Rewind(s, sSince, f);
    }
    if (f.charge) {
        if ((var == 0 || var == 2) && Val<uint8_t>(s, sCharging)) Raise(s, sForce, 60.0f, 60.0f);
        if (var == 1 && Val<int>(s, sPrimary, 0) == 1) SetVal<int>(s, sPrimary, 2);
    }
    if (f.reload && var == 2) SawFix(Ref(s, sSaw));
}

void Hammer(Object* h, Frame& f, bool copy) {
    int var = Val<int>(h, hVar, -1);
    if (f.shots) {
        Speed(Ref(h, hAnim), f.m);
        if (f.k > 0.0f && f.dt > 0.0f) {
            float cd = Val<float>(h, hCooldown, 0.0f);
            if (cd > 0.0f) SetVal<float>(h, hCooldown, std::max(0.0f, cd - f.k * f.dt));
            if (Val<uint8_t>(h, hSwinging)) Raise(h, hSwing, f.k * 2.0f * f.dt, 1.0f);
            Rewind(h, hPulled, f);
        }
    }
    if (f.charge) {
        if (!heldSwing && Val<uint8_t>(h, hSwinging)) Raise(h, hSwing, 1.0f, 1.0f);
        if (var == 2 && Val<uint8_t>(h, hCharging)) Raise(h, hForce, 60.0f, 60.0f);
        if (var == 1 && Val<int>(h, hPrimary, 0) == 1) SetVal<int>(h, hPrimary, 2);
    }
    if (f.reload && var == 2) SawFix(Ref(h, hSaw));
    if (f.shots) AutoSwing(h, f, copy);
}

Object* Weapons(Object* gc, std::vector<Object*>& out) {
    Object* cur = Ref(gc, gcCurrent);
    if (ActiveInHierarchy(cur)) out.push_back(cur);
    if (!dwCls || !dwCurrent || (gcDual && Val<int>(gc, gcDual, 0) <= 0)) return cur;
    for (Object* dw : CompsInChildren(gc, dwCls, false)) {
        Object* w = Ref(dw, dwCurrent);
        if (w && std::find(out.begin(), out.end(), w) == out.end() && ActiveInHierarchy(w)) out.push_back(w);
    }
    return cur;
}

void Apply(Frame& f) {
    const Ctx& c = *f.c;
    float v = c.value("fire_rate", 5.0f);
    f.m = std::isfinite(v) ? std::clamp(v, 1.0f, 20.0f) : 5.0f;
    f.k = f.m - 1.0f;
    f.dt = std::isfinite(c.dt) ? std::clamp(c.dt, 0.0f, 0.1f) : 0.0f;
    if (f.shots && f.k > 0.0f && f.dt > 0.0f && timeNow) f.now = CallFloat(timeNow, nullptr, {}, -1.0f);
    if (f.shots || f.reload) f.wc = Single(wcCls);
    if (f.reload && f.wc) Refill(f.wc);
    Object* gc = Single(gcCls);
    if (gc && Val<uint8_t>(gc, gcActivated)) {
        std::vector<Object*> list;
        Object* cur = Weapons(gc, list);
        for (Object* go : list) {
            bool copy = go != cur;
            if (Object* r = Comp(go, revCls)) {
                Revolver(r, f, copy);
            } else if (Object* s = Comp(go, shoCls)) {
                Shotgun(s, f);
            } else if (Object* h = Comp(go, hamCls)) {
                Hammer(h, f, copy);
            }
        }
    }
    if (f.shots && f.wc) Zero(f.wc, wcCooldowns);
}

}

bool Resolve() {
    const char* a = "Assembly-CSharp";
    gcCls = Cls(a, "GunControl");
    wcCls = Cls(a, "WeaponCharges");
    dwCls = Cls(a, "DualWield");
    widCls = Cls(a, "WeaponIdentifier");
    revCls = Cls(a, "Revolver");
    shoCls = Cls(a, "Shotgun");
    hamCls = Cls(a, "ShotgunHammer");
    hzCls = Cls(a, "HurtZone");
    imCls = Cls(a, "InputManager");
    Class* piCls = Cls(a, "PlayerInput");
    Class* iasCls = Cls(a, "InputActionState");
    animCls = Cls("UnityEngine.Animator");
    mbCls = Cls("UnityEngine.MonoBehaviour");

    gcActivated = F(gcCls, "activated");
    gcCurrent = Typed(gcCls, "currentWeapon", R.gameObject);
    gcDual = F(gcCls, "dualWieldCount");
    dwCurrent = Typed(dwCls, "currentWeapon", R.gameObject);
    widDelay = F(widCls, "delay");

    rVar = F(revCls, "gunVariation");
    rGunReady = F(revCls, "gunReady");
    rShootReady = F(revCls, "shootReady");
    rPierceReady = F(revCls, "pierceReady");
    rShootCharge = F(revCls, "shootCharge");
    rPierceCharge = F(revCls, "pierceCharge");
    rCharging = F(revCls, "chargingPierce");
    rPierceShot = F(revCls, "pierceShotCharge");
    rAnim = Typed(revCls, "anim", animCls);

    sVar = F(shoCls, "variation");
    sAnim = Typed(shoCls, "anim", animCls);
    sCharging = F(shoCls, "charging");
    sForce = F(shoCls, "grenadeForce");
    sPrimary = F(shoCls, "primaryCharge");
    sSaw = Typed(shoCls, "sawZone", hzCls);
    sSince = Inner(F(shoCls, "sinceLastCore"), "time");

    hVar = F(hamCls, "variation");
    hGunReady = F(hamCls, "gunReady");
    hAnim = Typed(hamCls, "anim", animCls);
    hPrimary = F(hamCls, "primaryCharge");
    hSwinging = F(hamCls, "chargingSwing");
    hSwing = F(hamCls, "swingCharge");
    hPulled = Inner(F(hamCls, "pulledOut"), "time");
    hCooldown = F(hamCls, "hammerCooldown");
    hCharging = F(hamCls, "charging");
    hForce = F(hamCls, "chargeForce");
    hSaw = Typed(hamCls, "sawZone", hzCls);
    hWid = Typed(hamCls, "wid", widCls);
    hImpact = M(hamCls, "Impact", 0);

    wcRev0 = F(wcCls, "rev0charge");
    wcRev1 = F(wcCls, "rev1charge");
    wcRev2 = F(wcCls, "rev2charge");
    wcRevAlt = Floats(wcCls, "revaltpickupcharges");
    wcNade = F(wcCls, "shoAltNadeCharge");
    wcSaw = F(wcCls, "shoSawCharge");
    wcSawReset = F(wcCls, "shoSawResetTimer");
    wcCooldowns = Floats(wcCls, "shoaltcooldowns");
    wcYellows = F(wcCls, "shoAltYellows");
    wcYellowsTimer = F(wcCls, "shoAltYellowsTimer");
    hzHits = F(hzCls, "chainsawNonlethalHits");

    imSource = Typed(imCls, "<InputSource>k__BackingField", piCls);
    piFire1 = Typed(piCls, "Fire1", iasCls);
    iasPressed = F(iasCls, "<IsPressed>k__BackingField");
    imCombo = M(imCls, "PerformingCheatMenuCombo", 0);
    getSpeed = M(animCls, "get_speed", 0);
    setSpeed = M(animCls, "set_speed", 1);
    mbInvoke = MT(mbCls, "Invoke", {"System.String", "System.Single"});
    mbIsInvoking = MT(mbCls, "IsInvoking", {"System.String"});
    mbPending = MT(mbCls, "IsInvoking", {});
    timeNow = M(R.time, "get_time", 0);

    bool gcOk = gcActivated && gcCurrent;
    bool speedOk = getSpeed && setSpeed;
    bool revShots = (speedOk && rAnim) || (rShootReady && rShootCharge);
    bool shoShots = (speedOk && sAnim) || (sVar && sSince);
    bool hamShots = hCooldown || (hSwinging && hSwing);
    shotsOk = gcOk && (revShots || shoShots || hamShots);
    bool revCharge = rVar && rCharging && rGunReady && rPierceShot;
    bool shoCharge = sVar && ((sCharging && sForce) || sPrimary);
    bool hamCharge = (hSwinging && hSwing) || (hVar && ((hCharging && hForce) || hPrimary));
    chargeOk = gcOk && (revCharge || shoCharge || hamCharge);
    reloadOk = wcRev0 || wcRev1 || wcRev2 || wcRevAlt || wcNade || wcSaw || wcCooldowns;
    swingOk = shotsOk && hImpact && hSwinging && hSwing && hGunReady && hCooldown && imSource && piFire1 && iasPressed &&
              R.gsmInstance && R.gsmInputLocked;
    copySwingOk = swingOk && hWid && widDelay && mbInvoke && mbIsInvoking;
    if (!speedOk) Missing("Animator speed, so the Revolver and Shotgun keep their own fire speed");
    if (shotsOk && !swingOk) Missing("a part of the held Jackhammer swing, so it swings on release only");
    LOG("weapons_a: unlimited shots %s, instant charge %s, no reload %s, held jackhammer swing %s", shotsOk ? "ready" : "off",
        chargeOk ? "ready" : "off", reloadOk ? "ready" : "off", swingOk ? "ready" : "off");
    return shotsOk || chargeOk || reloadOk;
}

bool Supported(const std::string& id) {
    if (id == "unlimited_shots" || id == "fire_rate") return shotsOk;
    if (id == "instant_charge") return chargeOk;
    if (id == "no_reload") return reloadOk;
    return false;
}

void Tick(const Ctx& c) {
    Frame f;
    f.c = &c;
    f.shots = shotsOk && c.on("unlimited_shots");
    f.charge = chargeOk && c.on("instant_charge");
    f.reload = reloadOk && c.on("no_reload");
    if (heldSwing && (!f.shots || !c.inLevel || c.dead || !Pressed())) heldSwing = false;
    if (!f.shots && !tracked.empty()) Restore(false);
    if (c.inLevel && !c.dead && (f.shots || f.charge || f.reload)) Apply(f);
    if (f.swung) heldSwing = true;
    if (!tracked.empty()) Prune();
}

void Release(const Ctx&) {
    heldSwing = false;
    Restore(true);
}

void Action(const std::string&, const Ctx&) {}

bool Busy() {
    for (const Tracked& t : tracked)
        if (Alive(t.anim.get())) return true;
    return false;
}

}
