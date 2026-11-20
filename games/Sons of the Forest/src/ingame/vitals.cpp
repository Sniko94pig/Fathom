#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/patch.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace sotf {

namespace {

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;

const float kNoFallVelocity = 100000.0f;
const float kColdMargin = 2.0f;
const float kWarmMargin = 16.0f;
const double kLungRecheck = 0.5;

const char* kIds[] = {"godmode", "unlimited_health", "unlimited_stamina", "unlimited_hydration", "unlimited_fullness",
                      "unlimited_temperature", "unlimited_lung", "fully_rested", "restore_health", "restore_hunger",
                      "restore_hydration", "restore_stamina", "restore_temperature", "restore_lung", "restore_rest",
                      "no_sickness", "no_fall_damage", "strength_levels", "add_strength"};

struct VitalRefs {
    Class *localPlayer = nullptr, *vitals = nullptr, *stat = nullptr, *playerStats = nullptr, *dyingState = nullptr, *fpc = nullptr,
          *breathing = nullptr, *cheats = nullptr;
    Fld lpVitals, lpStats, lpFpc;
    Fld target, health, sickness, stamina, hydration, fullness, rested, lung, hasDied;
    Fld level, topLevel, basePerLevel, perLevel, progress, boost;
    Fld statCur, statTop;
    Fld downed, dying, body, coldAt, coolRate;
    Fld lungAir, lungCapacity;
    Fld fallVelocity;
    Fld overrides, godMode, noSurvival;
    Method *getHealth = nullptr, *getSickness = nullptr, *getStamina = nullptr, *getHydration = nullptr, *getFullness = nullptr,
           *getRested = nullptr;
    Method *setFullHealth = nullptr, *setSickness = nullptr, *applyHealing = nullptr, *setFullStamina = nullptr, *setHydration = nullptr,
           *setFullness = nullptr, *setFullRest = nullptr, *gainStrength = nullptr, *lungFinal = nullptr;
    int aliveState = 0;
} R;

struct Avail {
    bool player = false, god = false, health = false, stamina = false, hydration = false, fullness = false, temperature = false,
         lung = false, rest = false, sickness = false, fall = false, strength = false;
} A;

struct Flag {
    patch::Hold obj;
    uint8_t orig = 0;
    bool have = false;
};

struct Body {
    Object* vit = nullptr;
    Object* stats = nullptr;
};

enum class Fill { Missing, Full, Done };

Flag godFlag, survivalFlag;
patch::Forced<float> fallState;
patch::Hold sickVit;
float lastTarget = 0;
bool haveTarget = false;
float lungTop = 0;
double lungAt = -1;

bool Inst(const Fld& f) { return f && !f.isStatic; }

void ReleaseFlag(Flag& s, const Fld& f) {
    Object* o = s.obj.get();
    if (s.have && o && Inst(f) && un::Val<uint8_t>(o, f) == 1) un::SetVal<uint8_t>(o, f, s.orig);
    s.obj.reset();
    s.have = false;
}

void ForceFlag(Object* o, const Fld& f, bool want, Flag& s) {
    if (!Inst(f)) return;
    if (s.have && (s.obj.get() != o || !want)) ReleaseFlag(s, f);
    if (!want || !o) return;
    uint8_t cur = un::Val<uint8_t>(o, f);
    if (!s.have) {
        s.obj.set(o);
        s.orig = cur;
        s.have = true;
    } else if (cur != 1) {
        s.orig = cur;
    }
    if (cur != 1) un::SetVal<uint8_t>(o, f, 1);
}

bool Live(const Ctx& c) { return c.inWorld && c.player && c.session != core::Session::Client; }

bool Find(const Ctx& c, Body& b) {
    b = Body{};
    if (!A.player || !Live(c)) return false;
    Object* v = un::Ref(c.player, R.lpVitals);
    Object* s = un::Ref(c.player, R.lpStats);
    if (!un::Alive(v) || !un::Alive(s)) return false;
    b.vit = v;
    b.stats = s;
    return true;
}

bool Down(const Body& b) {
    if (un::Val<uint8_t>(b.vit, R.hasDied) != 0) return true;
    if (un::Val<int>(b.stats, R.dying, R.aliveState) != R.aliveState) return true;
    return un::Val<uint8_t>(b.stats, R.downed) != 0;
}

Object* StatOf(Object* vit, const Fld& f, Method* getter) {
    Object* o = un::Ref(vit, f);
    if (!o && getter) o = un::Call(getter, vit);
    return o;
}

float Cur(Object* st) { return un::Val<float>(st, R.statCur); }

float Top(Object* st) { return un::Val<float>(st, R.statTop); }

bool Low(Object* st) { return st && Cur(st) < Top(st) - 0.01f; }

Fill FillHealth(Object* vit) {
    Object* h = StatOf(vit, R.health, R.getHealth);
    if (!h) return Fill::Missing;
    Object* s = StatOf(vit, R.sickness, R.getSickness);
    float top = Top(h);
    bool low = Low(h) || un::Val<float>(vit, R.target, top) < top - 0.01f || (s && Cur(s) > 0.001f);
    if (!low) return Fill::Full;
    un::Call(R.setFullHealth, vit);
    return Fill::Done;
}

Fill FillStamina(Object* vit) {
    Object* st = StatOf(vit, R.stamina, R.getStamina);
    if (!st) return Fill::Missing;
    if (!Low(st)) return Fill::Full;
    un::Call(R.setFullStamina, vit);
    return Fill::Done;
}

Fill FillTo(Object* vit, const Fld& f, Method* getter, Method* setter) {
    Object* st = StatOf(vit, f, getter);
    if (!st) return Fill::Missing;
    if (!Low(st)) return Fill::Full;
    float top = Top(st);
    un::Call(setter, vit, {&top});
    return Fill::Done;
}

Fill FillRest(Object* vit) {
    Object* st = StatOf(vit, R.rested, R.getRested);
    if (!st) return Fill::Missing;
    if (!Low(st)) return Fill::Full;
    un::Call(R.setFullRest, vit);
    return Fill::Done;
}

float ColdMargin(Object* stats) {
    float rate = un::Val<float>(stats, R.coolRate, 0.0f);
    if (!std::isfinite(rate) || rate <= 0.0f) return kColdMargin;
    return std::clamp(rate * 4.0f, kColdMargin, kWarmMargin);
}

Fill Warm(Object* stats, float margin) {
    float coldAt = un::Val<float>(stats, R.coldAt);
    if (!std::isfinite(coldAt)) return Fill::Missing;
    float least = coldAt + margin;
    if (un::Val<float>(stats, R.body) >= least) return Fill::Full;
    un::SetVal<float>(stats, R.body, least);
    return Fill::Done;
}

float LungTop(Object* br, double now, bool fresh) {
    if (fresh || lungAt < 0 || now - lungAt >= kLungRecheck || !(lungTop > 0.0f)) {
        float top = R.lungFinal ? un::CallFloat(R.lungFinal, br, {}, -1.0f) : -1.0f;
        if (!(top > 0.0f) || !std::isfinite(top)) top = un::Val<float>(br, R.lungCapacity);
        lungTop = top;
        lungAt = now;
    }
    return lungTop;
}

Fill FillLung(Object* vit, double now, bool fresh) {
    Object* br = un::Ref(vit, R.lung);
    if (!br) return Fill::Missing;
    float top = LungTop(br, now, fresh);
    if (!(top > 0.0f) || !std::isfinite(top)) return Fill::Missing;
    if (un::Val<float>(br, R.lungAir) >= top - 0.001f) return Fill::Full;
    un::SetVal<float>(br, R.lungAir, top);
    return Fill::Done;
}

void ForgetSickness() {
    sickVit.reset();
    haveTarget = false;
}

void KeepHealthy(Object* vit) {
    if (sickVit.get() != vit) {
        sickVit.set(vit);
        haveTarget = false;
    }
    Object* s = StatOf(vit, R.sickness, R.getSickness);
    float sick = s ? Cur(s) : 0.0f;
    if (sick > 0.001f) {
        float target = un::Val<float>(vit, R.target);
        float drop = haveTarget ? std::max(0.0f, lastTarget - target) : 0.0f;
        float zero = 0.0f;
        un::Call(R.setSickness, vit, {&zero});
        float heal = std::min(sick, drop);
        if (heal > 0.01f) un::Call(R.applyHealing, vit, {&heal});
    }
    lastTarget = un::Val<float>(vit, R.target);
    haveTarget = true;
}

void Breathe(Object* vit, double now) {
    if (A.lung && vit) FillLung(vit, now, true);
}

void Godmode(const Ctx& c, const Body& b, bool up) {
    if (!A.god) return;
    bool want = up && c.s.On("godmode");
    if (!want && !godFlag.have && !survivalFlag.have) return;
    Object* ov = want ? un::Static(R.overrides) : nullptr;
    want = want && ov != nullptr;
    bool held = survivalFlag.have;
    ForceFlag(ov, R.godMode, want, godFlag);
    ForceFlag(ov, R.noSurvival, want, survivalFlag);
    if (held && !survivalFlag.have && up) Breathe(b.vit, c.now);
}

void FallDamage(Object* player, bool want) {
    if (!A.fall) return;
    if (!want && !fallState.have) return;
    Object* fpc = want ? un::Ref(player, R.lpFpc) : nullptr;
    want = want && un::Alive(fpc);
    patch::Force<float>(want ? fpc : nullptr, R.fallVelocity, want, kNoFallVelocity, fallState);
}

void Report(Fill r, const char* done, const char* full) {
    if (r == Fill::Done) Note(done);
    else if (r == Fill::Full) Note(full);
    else Note("That is not available right now");
}

void AddStrength(const Ctx& c, Object* vit) {
    float want = c.s.Value("strength_levels", 1.0f);
    if (!std::isfinite(want)) want = 1.0f;
    int n = std::clamp((int)std::lround(want), 1, 10);
    int start = un::Val<int>(vit, R.level);
    int top = un::Val<int>(vit, R.topLevel);
    if (start >= top) {
        Note("Strength is already at the highest level");
        return;
    }
    if (!(un::Val<float>(vit, R.boost, 1.0f) > 0.0f)) {
        Note("A food effect is blocking strength gains right now");
        return;
    }
    WarnAchievementsOnce();
    int goal = std::min(start + n, top);
    for (int guard = 0; guard < 4 * n + 8; ++guard) {
        int level = un::Val<int>(vit, R.level);
        if (level >= goal) break;
        float req = un::Val<float>(vit, R.perLevel);
        if (!(req >= 1e-30f)) req = un::Val<float>(vit, R.basePerLevel);
        float cur = un::Val<float>(vit, R.progress);
        if (!std::isfinite(req) || !(req > 0.0f) || !std::isfinite(cur)) break;
        float need = req - cur + std::max(0.01f, req * 1e-4f);
        if (need > req * 0.999f) need = req * 0.999f;
        if (!(need > 0.0f)) need = req * 0.001f;
        float k = un::Val<float>(vit, R.boost, 1.0f);
        if (!(k > 0.0f)) break;
        if (std::fabs(k - 1.0f) > 1e-6f) need /= k;
        if (!std::isfinite(need) || !(need > 0.0f)) break;
        bool ok = false;
        un::Call(R.gainStrength, vit, {&need}, &ok);
        if (!ok) break;
        if (un::Val<int>(vit, R.level) == level && un::Val<float>(vit, R.progress) == cur) break;
    }
    int end = un::Val<int>(vit, R.level);
    LOG("strength level %d -> %d", start, end);
    if (end <= start) Note("Strength could not be raised right now");
    else if (end >= top) Note("Strength is now level " + std::to_string(end) + ", the highest level");
    else Note("Strength is now level " + std::to_string(end));
}

bool Supported(const std::string& id) {
    if (id == "godmode") return A.god;
    if (id == "unlimited_health" || id == "restore_health") return A.health;
    if (id == "unlimited_stamina" || id == "restore_stamina") return A.stamina;
    if (id == "unlimited_hydration" || id == "restore_hydration") return A.hydration;
    if (id == "unlimited_fullness" || id == "restore_hunger") return A.fullness;
    if (id == "unlimited_temperature" || id == "restore_temperature") return A.temperature;
    if (id == "unlimited_lung" || id == "restore_lung") return A.lung;
    if (id == "fully_rested" || id == "restore_rest") return A.rest;
    if (id == "no_sickness") return A.sickness;
    if (id == "no_fall_damage") return A.fall;
    if (id == "strength_levels" || id == "add_strength") return A.strength;
    return false;
}

bool Resolve(std::string& err) {
    const std::vector<std::string> single{"System.Single"};
    R.localPlayer = un::C("Sons", "TheForest.Utils", "LocalPlayer");
    R.vitals = un::C("Sons", "", "Vitals");
    R.stat = un::C("Sons.StatSystem", "Sons.StatSystem", "Stat");
    R.playerStats = un::C("Sons", "", "PlayerStats");
    R.dyingState = il2::Nested(R.playerStats, "DyingState");
    if (R.playerStats && !R.dyingState) un::Missing("PlayerStats.DyingState");
    R.fpc = un::C("Sons", "", "FirstPersonCharacter");
    R.breathing = un::C("Sons", "", "BreathingData");
    R.cheats = un::C("Sons.Settings", "Sons.Settings", "Cheats");

    R.lpVitals = un::F(R.localPlayer, "_vitals");
    R.lpStats = un::F(R.localPlayer, "_stats");
    R.lpFpc = un::F(R.localPlayer, "_fpc");

    R.target = un::F(R.vitals, "_targetHealth");
    R.health = un::F(R.vitals, "_health");
    R.sickness = un::F(R.vitals, "_sickness");
    R.stamina = un::F(R.vitals, "_stamina");
    R.hydration = un::F(R.vitals, "_hydration");
    R.fullness = un::F(R.vitals, "_fullness");
    R.rested = un::F(R.vitals, "_rested");
    R.lung = un::F(R.vitals, "LungBreathing");
    R.hasDied = un::F(R.vitals, "_hasDied");
    R.level = un::F(R.vitals, "_currentStrengthLevel");
    R.topLevel = un::F(R.vitals, "_maxStrengthLevel");
    R.basePerLevel = un::F(R.vitals, "_requiredBaseStrengthPerLevel");
    R.perLevel = un::F(R.vitals, "_requiredCurrentStrengthPerLevel");
    R.progress = un::F(R.vitals, "_currentStrength");
    R.boost = un::F(R.vitals, "_strengthBoostModifier");

    R.getHealth = un::M(R.vitals, "get_Health", 0);
    R.getSickness = un::M(R.vitals, "get_Sickness", 0);
    R.getStamina = un::M(R.vitals, "get_Stamina", 0);
    R.getHydration = un::M(R.vitals, "get_Hydration", 0);
    R.getFullness = un::M(R.vitals, "get_Fullness", 0);
    R.getRested = un::M(R.vitals, "get_Rested", 0);
    R.setFullHealth = un::M(R.vitals, "SetFullHealth", 0);
    R.setSickness = un::M(R.vitals, "SetSickness", single);
    R.applyHealing = un::M(R.vitals, "ApplyHealing", single);
    R.setFullStamina = un::M(R.vitals, "SetFullStamina", 0);
    R.setHydration = un::M(R.vitals, "SetHydration", single);
    R.setFullness = un::M(R.vitals, "SetFullness", single);
    R.setFullRest = un::M(R.vitals, "SetFullRest", 0);
    R.gainStrength = un::M(R.vitals, "GainStrength", single);

    R.statCur = un::F(R.stat, "_currentValue");
    R.statTop = un::F(R.stat, "_max");

    R.downed = un::F(R.playerStats, "_playerIsDowned");
    R.dying = un::F(R.playerStats, "_dyingState");
    R.body = un::F(R.playerStats, "_bodyTemperature");
    R.coldAt = un::F(R.playerStats, "_coldAtTemperature");
    R.coolRate = un::F(R.playerStats, "_bodyTemperatureChangeRate");

    R.lungAir = un::F(R.breathing, "CurrentLungAir");
    R.lungCapacity = un::F(R.breathing, "MaxLungAirCapacity");
    R.lungFinal = un::M(R.breathing, "get_MaxLungAirCapacityFinal", 0);

    R.fallVelocity = un::F(R.fpc, "_fallDamageVelocity");

    R.overrides = un::F(R.cheats, "Overrides");
    R.godMode = un::F(R.cheats, "<GodMode>k__BackingField");
    R.noSurvival = un::F(R.cheats, "<NoSurvival>k__BackingField");

    R.aliveState = R.dyingState ? (int)il2::EnumValue(R.dyingState, "Alive", 0) : 0;

    bool stats = Inst(R.statCur) && Inst(R.statTop);
    A.player = Inst(R.lpVitals) && Inst(R.lpStats) && Inst(R.hasDied) && Inst(R.dying) && Inst(R.downed);
    A.god = A.player && R.overrides && R.overrides.isStatic && Inst(R.godMode) && Inst(R.noSurvival);
    A.health = A.player && stats && R.setFullHealth != nullptr && (Inst(R.health) || R.getHealth != nullptr);
    A.stamina = A.player && stats && R.setFullStamina != nullptr && (Inst(R.stamina) || R.getStamina != nullptr);
    A.hydration = A.player && stats && R.setHydration != nullptr && (Inst(R.hydration) || R.getHydration != nullptr);
    A.fullness = A.player && stats && R.setFullness != nullptr && (Inst(R.fullness) || R.getFullness != nullptr);
    A.rest = A.player && stats && R.setFullRest != nullptr && (Inst(R.rested) || R.getRested != nullptr);
    A.temperature = A.player && Inst(R.body) && Inst(R.coldAt);
    A.lung = A.player && Inst(R.lung) && Inst(R.lungAir) && (R.lungFinal != nullptr || Inst(R.lungCapacity));
    A.sickness = A.player && stats && R.setSickness != nullptr && R.applyHealing != nullptr && Inst(R.target) &&
                 (Inst(R.sickness) || R.getSickness != nullptr);
    A.fall = Inst(R.lpFpc) && Inst(R.fallVelocity);
    A.strength = A.player && R.gainStrength != nullptr && Inst(R.level) && Inst(R.topLevel) && Inst(R.basePerLevel) &&
                 Inst(R.perLevel) && Inst(R.progress) && Inst(R.boost);

    bool any = false;
    for (const char* id : kIds) {
        if (Supported(id)) any = true;
        else LOG("Player: %s is not available", id);
    }
    if (!any) err = "the player's vitals were not found";
    return any;
}

void Tick(const Ctx& c) {
    Body b;
    bool up = !c.stopping && Find(c, b) && !Down(b);
    Godmode(c, b, up);
    FallDamage(c.player, !c.stopping && Live(c) && c.s.On("no_fall_damage"));
    if (!up) {
        ForgetSickness();
        lungAt = -1;
        return;
    }
    if (A.health && c.s.On("unlimited_health")) FillHealth(b.vit);
    if (A.stamina && c.s.On("unlimited_stamina")) FillStamina(b.vit);
    if (A.hydration && c.s.On("unlimited_hydration")) FillTo(b.vit, R.hydration, R.getHydration, R.setHydration);
    if (A.fullness && c.s.On("unlimited_fullness")) FillTo(b.vit, R.fullness, R.getFullness, R.setFullness);
    if (A.rest && c.s.On("fully_rested")) FillRest(b.vit);
    if (A.temperature && c.s.On("unlimited_temperature")) Warm(b.stats, ColdMargin(b.stats));
    if (A.lung && c.s.On("unlimited_lung")) FillLung(b.vit, c.now, !c.prev.On("unlimited_lung"));
    if (A.sickness && c.s.On("no_sickness")) KeepHealthy(b.vit);
    else ForgetSickness();
}

bool Action(const std::string& id, const Ctx& c) {
    if (id == "strength_levels" || !Supported(id)) return false;
    Body b;
    if (!Find(c, b)) {
        Note("Your character is not ready yet");
        return true;
    }
    if (Down(b)) {
        Note("That does not work while you are down or dead");
        return true;
    }
    if (id == "restore_health") {
        Report(FillHealth(b.vit), "Health is full again", "Health is already full");
    } else if (id == "restore_hunger") {
        Report(FillTo(b.vit, R.fullness, R.getFullness, R.setFullness), "You are no longer hungry", "You are not hungry");
    } else if (id == "restore_hydration") {
        Report(FillTo(b.vit, R.hydration, R.getHydration, R.setHydration), "You are no longer thirsty", "You are not thirsty");
    } else if (id == "restore_stamina") {
        Report(FillStamina(b.vit), "Stamina is full again", "Stamina is already full");
    } else if (id == "restore_temperature") {
        Report(Warm(b.stats, kWarmMargin), "You are warm again", "You are already warm");
    } else if (id == "restore_lung") {
        Report(FillLung(b.vit, c.now, true), "Your breath is full again", "Your breath is already full");
    } else if (id == "restore_rest") {
        Report(FillRest(b.vit), "You are fully rested again", "You are already rested");
    } else if (id == "add_strength") {
        AddStrength(c, b.vit);
    } else {
        return false;
    }
    return true;
}

void Release(const Ctx& c) {
    bool held = survivalFlag.have;
    ReleaseFlag(godFlag, R.godMode);
    ReleaseFlag(survivalFlag, R.noSurvival);
    patch::Release(fallState, R.fallVelocity);
    Body b;
    if (held && Find(c, b) && !Down(b)) Breathe(b.vit, c.now);
    ForgetSickness();
    lungTop = 0;
    lungAt = -1;
}

}

const Group& VitalsGroup() {
    static Group g{"Player", &Resolve, &Supported, &Tick, &Action, &Release, nullptr, nullptr};
    return g;
}

}
