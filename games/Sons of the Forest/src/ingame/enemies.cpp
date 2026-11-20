#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/patch.h"
#include "ingame/unity.h"
#include <algorithm>
#include <climits>
#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

namespace sotf {

namespace {

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;

const char* kVail = "Sons.Ai.Vail";
const int kTypeCount = 101;
const float kCutDamage = 100000.0f;
const float kNoLimit = 1e9f;
const float kMinRange = 5.0f;
const float kMaxRange = 200.0f;
const float kDefaultRange = 50.0f;

struct Refs {
    Class *manager = nullptr, *actor = nullptr, *types = nullptr, *typeIds = nullptr, *classIds = nullptr;
    Fld mgrInstance, mgrActors;
    Fld acId, acDead, acDamageEnabled, acDamageByPlayer, acDismember;
    Method *killActor = nullptr, *health = nullptr, *forceDeath = nullptr, *ragdoll = nullptr;
    Method *actorClass = nullptr, *isEnemy = nullptr, *isFriendly = nullptr;
    int cannibal = 1, creepy = 2, ragdollOn = 1;
    std::vector<int> never;

    Class *dismember = nullptr, *definition = nullptr, *partIds = nullptr;
    Fld dcDisabled, dcOnlyDead, dcLimit, dcDefinitions, dfPart, dfCut, dfController;
    Method* cut = nullptr;
    std::vector<int> limbs;

    Class *localPlayer = nullptr, *stimuli = nullptr, *flags = nullptr, *visibility = nullptr, *views = nullptr;
    Fld lpTransform, lpStimuli, psFlags, pvInvisible;
    Method *getStimuli = nullptr, *currentView = nullptr, *setInvisible = nullptr, *cutscene = nullptr;
    int invisibleBit = 256;
    std::vector<int> hiddenViews;

    bool killOk = false, oneHitOk = false, dismemberOk = false, invisibleOk = false, ghostOk = false;
} R;

bool enemyType[kTypeCount] = {};
bool tableReady = false;
bool tableLogged = false;
double tableRetry = 0;

struct Seen {
    float damage = 0;
    unsigned gen = 0;
};
std::unordered_map<Object*, Seen> seen;
unsigned generation = 0;
bool oneHitLive = false;

struct Relaxed {
    patch::Hold ctrl;
    float orig = 0;
};
std::vector<Relaxed> relaxed;

patch::Forced<bool> invisState;
patch::Hold visStim, visComp;
double visRetry = 0;
patch::Hold ghostHeld;

Method* StaticMethod(Class* c, const char* name, int args) {
    if (!c) return nullptr;
    Method* m = il2::FindStaticMethod(c, name, args);
    if (!m) un::Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
    return m;
}

int Enum(Class* c, const char* name, int expected) {
    if (!c) return expected;
    long long v = il2::EnumValue(c, name, LLONG_MIN);
    if (v == LLONG_MIN) {
        un::Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
        return expected;
    }
    if (v != expected) LOG("%s.%s is %lld (expected %d)", il2::ClassName(c), name, v, expected);
    return (int)v;
}

bool Resolve(std::string& err) {
    R.manager = un::C(kVail, "Sons.Ai.Vail", "VailActorManager");
    R.actor = un::C(kVail, "Sons.Ai.Vail", "VailActor");
    R.types = un::C(kVail, "Sons.Ai.Vail", "VailTypes");
    R.typeIds = un::C(kVail, "Sons.Ai.Vail", "VailActorTypeId");
    R.classIds = un::C(kVail, "Sons.Ai.Vail", "VailActorClassId");
    if (R.manager) {
        R.mgrInstance = un::F(R.manager, "_instance");
        R.mgrActors = un::F(R.manager, "_activeActors");
        R.killActor = un::M(R.manager, "KillActor", 1);
    }
    if (R.actor) {
        R.acId = un::F(R.actor, "_id");
        R.acDead = un::F(R.actor, "_isDead");
        R.acDamageEnabled = un::F(R.actor, "_damageEnabled");
        R.acDamageByPlayer = un::F(R.actor, "_damageByPlayer");
        R.acDismember = un::F(R.actor, "_dismembermentController");
        R.health = un::M(R.actor, "GetHealth", 0);
        R.forceDeath = un::M(R.actor, "ForceDeath", 0);
        R.ragdoll = un::M(R.actor, "SetRagdoll", 2);
        if (R.ragdoll && il2::ParamTypeName(R.ragdoll, 0) != "System.Boolean") {
            un::Missing("VailActor.SetRagdoll(bool, RagdollState)");
            R.ragdoll = nullptr;
        }
        if (R.ragdoll) {
            Class* state = il2::ParamClass(R.ragdoll, 1);
            if (!state || !il2::IsEnum(state)) state = il2::FindClass("Sons.Ragdoll", "Sons.Ragdoll", "RagdollManager/RagdollState");
            R.ragdollOn = Enum(state, "Enabled", 1);
        }
    }
    R.actorClass = StaticMethod(R.types, "GetActorClass", 1);
    R.isEnemy = StaticMethod(R.types, "IsEnemy", 1);
    R.isFriendly = StaticMethod(R.types, "IsPlayerFriendly", 1);
    R.cannibal = Enum(R.classIds, "Cannibal", 1);
    R.creepy = Enum(R.classIds, "Creepy", 2);
    R.never = {Enum(R.typeIds, "Timmy", 8), Enum(R.typeIds, "Robby", 9), Enum(R.typeIds, "Virginia", 10),
               Enum(R.typeIds, "PlayerRobby", 100)};

    R.dismember = un::C(kVail, "Sons.Ai.Vail", "DismembermentController");
    if (R.dismember) {
        R.definition = il2::Nested(R.dismember, "DismembermentDefinition");
        R.partIds = il2::Nested(R.dismember, "DismemberPartId");
        if (!R.definition) un::Missing("DismembermentController.DismembermentDefinition");
        if (!R.partIds) un::Missing("DismembermentController.DismemberPartId");
        R.dcDisabled = un::F(R.dismember, "_disableDismemberment");
        R.dcOnlyDead = un::F(R.dismember, "_dismemberOnlyWhenDead");
        R.dcLimit = un::F(R.dismember, "_dismemberHealthLimit");
        R.dcDefinitions = un::F(R.dismember, "_definitions");
    }
    if (R.definition) {
        R.dfPart = un::F(R.definition, "_partId");
        R.dfCut = un::F(R.definition, "_dismembered");
        R.dfController = un::F(R.definition, "_dismembermentController");
        R.cut = un::M(R.definition, "ApplyDismemberDamage", 3);
        if (R.cut && il2::ParamTypeName(R.cut, 0) != "System.Single") {
            un::Missing("DismembermentDefinition.ApplyDismemberDamage(float, IImpactData, PartSpawnOverride)");
            R.cut = nullptr;
        }
    }
    R.limbs = {Enum(R.partIds, "RightArm", 1), Enum(R.partIds, "LeftArm", 2), Enum(R.partIds, "RightLeg", 3),
               Enum(R.partIds, "LeftLeg", 4)};

    R.localPlayer = un::C("Sons", "TheForest.Utils", "LocalPlayer");
    if (R.localPlayer) {
        R.lpTransform = un::F(R.localPlayer, "_transform");
        R.lpStimuli = un::F(R.localPlayer, "_playerStimuli");
        R.getStimuli = StaticMethod(R.localPlayer, "get_PlayerStimuli", 0);
        R.currentView = StaticMethod(R.localPlayer, "get_CurrentView", 0);
    }
    R.views = R.currentView ? il2::ReturnClass(R.currentView) : nullptr;
    if (!R.views || !il2::IsEnum(R.views)) {
        Class* inventory = un::C("Sons", "TheForest.Items.Inventory", "PlayerInventory");
        R.views = inventory ? il2::Nested(inventory, "PlayerViews") : nullptr;
    }
    R.hiddenViews = {Enum(R.views, "PlaneCrash", -1), Enum(R.views, "Loading", 0), Enum(R.views, "WakingUp", 1),
                     Enum(R.views, "Hidden", 10)};
    R.stimuli = un::C(kVail, "Sons.Ai.Vail.StimuliTypes", "PlayerStimuli");
    if (R.stimuli) {
        R.psFlags = un::F(R.stimuli, "_stimuliFlags");
        R.setInvisible = un::M(R.stimuli, "SetInvisible", 1);
        R.flags = il2::Nested(R.stimuli, "Flags");
        if (!R.flags) un::Missing("PlayerStimuli.Flags");
        R.invisibleBit = Enum(R.flags, "Invisible", 256);
    }
    R.visibility = un::C("Sons", "Sons.Player", "PlayerVisibility");
    if (R.visibility) R.pvInvisible = un::F(R.visibility, "_invisible");
    R.cutscene = StaticMethod(un::C("Sons", "Sons.Cutscenes", "CutsceneManager"), "IsCutscenePlaying", 0);

    bool actors = R.mgrInstance && R.mgrInstance.isStatic && R.mgrActors && !R.mgrActors.isStatic && R.acId && R.acDead &&
                  (R.actorClass || R.isEnemy);
    R.killOk = actors && (R.killActor || (R.forceDeath && R.ragdoll));
    R.oneHitOk = R.killOk && R.acDamageByPlayer;
    R.dismemberOk = actors && R.acDismember && R.dcDefinitions && R.dcLimit && R.dfPart && R.dfCut && R.cut;
    bool player = R.getStimuli || R.lpStimuli;
    R.invisibleOk = player && R.visibility && R.pvInvisible;
    R.ghostOk = player && R.psFlags && R.invisibleBit > 0;
    LOG("enemies: one hit %d, kill %d, dismember %d, invisibility %d, ghost %d", R.oneHitOk, R.killOk, R.dismemberOk,
        R.invisibleOk, R.ghostOk);
    if (!R.killOk && !R.dismemberOk && !R.invisibleOk && !R.ghostOk) {
        err = "the game's enemy functions were not found";
        return false;
    }
    return true;
}

bool Supported(const std::string& id) {
    if (id == "one_hit_kill") return R.oneHitOk;
    if (id == "enemy_range") return R.killOk || R.dismemberOk;
    if (id == "kill_in_range") return R.killOk;
    if (id == "dismember_in_range") return R.dismemberOk;
    if (id == "invisibility") return R.invisibleOk;
    if (id == "ghost") return R.ghostOk;
    return false;
}

bool BuildTable(double now, bool force) {
    if (tableReady) return true;
    if (!force && now < tableRetry) return false;
    tableRetry = now + 5.0;
    int found = 0;
    for (int i = 0; i < kTypeCount; ++i) {
        int id = i;
        bool enemy = false, known = false;
        if (R.actorClass) {
            bool ok = false;
            Object* r = un::Call(R.actorClass, nullptr, {&id}, &ok);
            if (ok && r) {
                int cls = il2::Result<int>(r, 0);
                enemy = cls == R.cannibal || cls == R.creepy;
                known = true;
            }
        }
        if (!known && R.isEnemy) enemy = un::CallBool(R.isEnemy, nullptr, {&id}, false);
        if (enemy && R.isFriendly && un::CallBool(R.isFriendly, nullptr, {&id}, true)) enemy = false;
        if (enemy && std::find(R.never.begin(), R.never.end(), i) != R.never.end()) enemy = false;
        enemyType[i] = enemy;
        if (enemy) ++found;
    }
    tableReady = found > 0;
    if (tableReady) LOG("enemy types found: %d", found);
    else if (!tableLogged) LOG("enemy types could not be read yet");
    tableLogged = true;
    return tableReady;
}

bool IsEnemy(Object* a) {
    int id = un::Val<int>(a, R.acId, -1);
    return id >= 0 && id < kTypeCount && enemyType[id];
}

Object* Manager() {
    Object* m = un::Static(R.mgrInstance);
    return un::Alive(m) ? m : nullptr;
}

std::vector<Object*> Actors(Object* mgr) {
    if (!mgr) return {};
    return il2::ListItems(un::Ref(mgr, R.mgrActors));
}

float Health(Object* a) { return R.health ? un::CallFloat(R.health, a, {}, 1.0f) : 1.0f; }

bool Gone(Object* a) { return !un::Alive(a) || un::Val<bool>(a, R.acDead) || Health(a) <= 0.0f; }

bool KillOne(Object* mgr, Object* a) {
    if (R.killActor && un::Alive(mgr)) un::Call(R.killActor, mgr, {a});
    if (Gone(a)) return true;
    if (R.forceDeath) {
        un::Call(R.forceDeath, a);
        if (R.ragdoll && un::Alive(a)) {
            bool on = true;
            int state = R.ragdollOn;
            un::Call(R.ragdoll, a, {&on, &state});
        }
    }
    return Gone(a);
}

bool PositionOf(Object* o, un::Vec3& out) {
    Object* t = un::TransformOf(o);
    if (!t || !un::E.trGetPosition) return false;
    bool ok = false;
    Object* r = un::Call(un::E.trGetPosition, t, {}, &ok);
    if (!ok || !r) return false;
    out = il2::Result<un::Vec3>(r);
    return std::isfinite(out.x) && std::isfinite(out.y) && std::isfinite(out.z);
}

bool PlayerPosition(const Ctx& c, un::Vec3& out) {
    Object* t = un::Ref(c.player, R.lpTransform);
    if (un::Alive(t) && PositionOf(t, out)) return true;
    return PositionOf(c.player, out);
}

bool CutscenePlaying() { return R.cutscene && un::CallBool(R.cutscene, nullptr); }

bool Candidate(Object* a) {
    if (!un::Alive(a) || !IsEnemy(a)) return false;
    if (un::Val<bool>(a, R.acDead)) return false;
    return un::Val<bool>(a, R.acDamageEnabled, true);
}

std::vector<Object*> Targets(const Ctx& c) {
    std::vector<Object*> out;
    Object* mgr = Manager();
    un::Vec3 center;
    if (!mgr || !c.player || !BuildTable(c.now, true) || !PlayerPosition(c, center)) return out;
    float range = std::clamp(c.s.Value("enemy_range", kDefaultRange), kMinRange, kMaxRange);
    for (Object* a : Actors(mgr)) {
        if (!Candidate(a)) continue;
        un::Vec3 p;
        if (!PositionOf(a, p) || un::Dist(p, center) > range) continue;
        if (Health(a) <= 0.0f) continue;
        out.push_back(a);
    }
    return out;
}

std::string Count(int n, const char* one, const char* many) {
    return n == 1 ? std::string("1 ") + one : std::to_string(n) + " " + many;
}

void KillInRange(const Ctx& c) {
    if (CutscenePlaying()) {
        Note("Not during a cutscene");
        return;
    }
    WarnAchievementsOnce();
    std::vector<Object*> list = Targets(c);
    if (list.empty()) {
        Note("No enemies in range");
        return;
    }
    Object* mgr = Manager();
    int killed = 0;
    for (Object* a : list)
        if (un::Alive(a) && !Gone(a) && KillOne(mgr, a)) ++killed;
    LOG("kill in range: %d of %d", killed, (int)list.size());
    if (killed == 0) Note("Could not kill the enemies in range");
    else Note("Killed " + Count(killed, "enemy", "enemies"));
}

void Unrelax() {
    for (Relaxed& r : relaxed) {
        Object* ctrl = r.ctrl.get();
        if (ctrl && un::Alive(ctrl) && un::Val<float>(ctrl, R.dcLimit) == kNoLimit) un::SetVal<float>(ctrl, R.dcLimit, r.orig);
        r.ctrl.reset();
    }
    relaxed.clear();
}

void Relax(Object* ctrl) {
    if (!un::Alive(ctrl) || !R.dcLimit) return;
    for (Relaxed& r : relaxed)
        if (r.ctrl.get() == ctrl) return;
    Relaxed r;
    r.orig = un::Val<float>(ctrl, R.dcLimit);
    r.ctrl.set(ctrl);
    relaxed.push_back(r);
    un::SetVal<float>(ctrl, R.dcLimit, kNoLimit);
}

bool IsLimb(int part) { return std::find(R.limbs.begin(), R.limbs.end(), part) != R.limbs.end(); }

bool DismemberOne(Object* mgr, Object* a) {
    Object* dc = un::Ref(a, R.acDismember);
    if (!un::Alive(dc) || un::Val<bool>(dc, R.dcDisabled)) return false;
    std::vector<Object*> todo;
    for (Object* d : il2::ListItems(un::Ref(dc, R.dcDefinitions))) {
        if (!d || un::Val<bool>(d, R.dfCut)) continue;
        if (!IsLimb(un::Val<int>(d, R.dfPart, -1))) continue;
        todo.push_back(d);
    }
    if (todo.empty()) return false;
    if (un::Val<bool>(dc, R.dcOnlyDead) && !Gone(a) && !(R.killOk && KillOne(mgr, a))) return false;
    if (!un::Alive(a) || !un::Alive(dc)) return false;
    Relax(dc);
    for (Object* d : todo) Relax(un::Ref(d, R.dfController));
    int cut = 0;
    for (Object* d : todo) {
        if (un::Val<bool>(d, R.dfCut)) continue;
        float damage = kCutDamage;
        un::Call(R.cut, d, {&damage, nullptr, nullptr});
        if (un::Val<bool>(d, R.dfCut)) ++cut;
    }
    Unrelax();
    return cut > 0;
}

void DismemberInRange(const Ctx& c) {
    if (CutscenePlaying()) {
        Note("Not during a cutscene");
        return;
    }
    WarnAchievementsOnce();
    std::vector<Object*> list = Targets(c);
    if (list.empty()) {
        Note("No enemies in range");
        return;
    }
    Object* mgr = Manager();
    int done = 0;
    for (Object* a : list)
        if (un::Alive(a) && DismemberOne(mgr, a)) ++done;
    Unrelax();
    LOG("dismember in range: %d of %d", done, (int)list.size());
    if (done == 0) Note("No enemies in range could lose limbs");
    else Note("Cut the limbs off " + Count(done, "enemy", "enemies"));
}

void OneHit(const Ctx& c) {
    bool on = R.oneHitOk && c.inWorld && c.s.On("one_hit_kill");
    if (!on) {
        if (oneHitLive) {
            seen.clear();
            oneHitLive = false;
        }
        return;
    }
    if (!oneHitLive) {
        seen.clear();
        oneHitLive = true;
    }
    if (!BuildTable(c.now, false)) return;
    Object* mgr = Manager();
    if (!mgr) {
        seen.clear();
        return;
    }
    ++generation;
    for (Object* a : Actors(mgr)) {
        if (!un::Alive(a) || !IsEnemy(a) || un::Val<bool>(a, R.acDead)) continue;
        float d = un::Val<float>(a, R.acDamageByPlayer);
        auto [it, fresh] = seen.try_emplace(a, Seen{d, generation});
        float before = it->second.damage;
        it->second.damage = d;
        it->second.gen = generation;
        if (fresh || !(d > before + 0.01f)) continue;
        if (!un::Val<bool>(a, R.acDamageEnabled, true) || Health(a) <= 0.0f) continue;
        if (KillOne(mgr, a)) WarnAchievementsOnce();
    }
    for (auto it = seen.begin(); it != seen.end();) {
        if (it->second.gen != generation) it = seen.erase(it);
        else ++it;
    }
}

Object* Stimuli(const Ctx& c) {
    Object* s = R.getStimuli ? un::Call(R.getStimuli, nullptr) : nullptr;
    if (!un::Alive(s)) s = un::Ref(c.player, R.lpStimuli);
    return un::Alive(s) ? s : nullptr;
}

Object* Visibility(const Ctx& c, Object* stim) {
    Object* pv = visComp.get();
    bool same = visStim.get() == stim;
    if (same && pv && un::Alive(pv)) return pv;
    if (same && c.now < visRetry) return nullptr;
    visStim.set(stim);
    visRetry = c.now + 2.0;
    pv = un::Comp(stim, R.visibility);
    if (!pv && c.player) pv = un::CompInChildren(c.player, R.visibility, true);
    visComp.set(pv);
    return pv;
}

bool HiddenView() {
    if (!R.currentView) return false;
    int view = un::CallInt(R.currentView, nullptr, {}, INT_MIN);
    return std::find(R.hiddenViews.begin(), R.hiddenViews.end(), view) != R.hiddenViews.end();
}

bool HasGhostBit(Object* stim) { return (un::Val<int>(stim, R.psFlags) & R.invisibleBit) != 0; }

void SetGhostBit(Object* stim, bool on) {
    if (!un::Alive(stim) || HasGhostBit(stim) == on) return;
    if (R.setInvisible) {
        bool v = on;
        un::Call(R.setInvisible, stim, {&v});
        if (HasGhostBit(stim) == on) return;
    }
    int flags = un::Val<int>(stim, R.psFlags);
    un::SetVal<int>(stim, R.psFlags, on ? (flags | R.invisibleBit) : (flags & ~R.invisibleBit));
}

void ReleaseGhost() {
    Object* s = ghostHeld.get();
    if (s && un::Alive(s)) SetGhostBit(s, HiddenView());
    ghostHeld.reset();
}

void Stealth(const Ctx& c) {
    bool wantInvisible = R.invisibleOk && c.inWorld && c.s.On("invisibility");
    bool wantGhost = R.ghostOk && c.inWorld && c.s.On("ghost");
    Object* stim = wantInvisible || wantGhost ? Stimuli(c) : nullptr;
    Object* pv = wantInvisible && stim ? Visibility(c, stim) : nullptr;
    patch::Force<bool>(pv, R.pvInvisible, pv != nullptr, true, invisState);
    if (!wantInvisible && visStim.held()) {
        visStim.reset();
        visComp.reset();
    }
    if (wantGhost && stim) {
        if (ghostHeld.get() != stim) {
            ReleaseGhost();
            ghostHeld.set(stim);
        }
        SetGhostBit(stim, true);
    } else if (ghostHeld.held()) {
        ReleaseGhost();
    }
}

void Release(const Ctx&) {
    Unrelax();
    patch::Release(invisState, R.pvInvisible);
    ReleaseGhost();
    visStim.reset();
    visComp.reset();
    visRetry = 0;
    seen.clear();
    oneHitLive = false;
}

void Tick(const Ctx& c) {
    if (!relaxed.empty()) Unrelax();
    if (!c.inWorld) {
        if (oneHitLive || invisState.have || invisState.obj.held() || ghostHeld.held() || visStim.held()) Release(c);
        return;
    }
    OneHit(c);
    Stealth(c);
}

bool Action(const std::string& id, const Ctx& c) {
    if (id == "kill_in_range" && R.killOk) {
        KillInRange(c);
        return true;
    }
    if (id == "dismember_in_range" && R.dismemberOk) {
        DismemberInRange(c);
        return true;
    }
    return false;
}

}

const Group& EnemiesGroup() {
    static Group g{"Enemy", &Resolve, &Supported, &Tick, &Action, &Release, nullptr};
    return g;
}

}
