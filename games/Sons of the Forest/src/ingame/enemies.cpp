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
const float kNetDamage = 4096.0f;
const float kNetCut = 2048.0f;
const double kCutDelay = 0.5;

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

    Class *boltEntity = nullptr, *hitEvent = nullptr, *damageController = nullptr, *damageNode = nullptr;
    Fld acDamageCtrl, dmNodes;
    Method *beAttached = nullptr, *hitCreate = nullptr, *hitTarget = nullptr, *hitDamage = nullptr, *hitNode = nullptr,
           *hitImpact = nullptr, *hitDir = nullptr, *hitVelocity = nullptr, *evSend = nullptr, *nodeId = nullptr,
           *nodeArmour = nullptr, *sendCut = nullptr;
    int onlyServer = 6, reliable = 2, melee = 2;

    bool killOk = false, oneHitOk = false, dismemberOk = false, invisibleOk = false, ghostOk = false;
    bool guestKillOk = false, guestCutOk = false;
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

struct PendingCut {
    patch::Hold actor;
    double at = 0;
};
std::vector<PendingCut> cuts;

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

    R.boltEntity = un::C("bolt", "", "BoltEntity");
    if (R.boltEntity) R.beAttached = un::M(R.boltEntity, "get_isAttached", 0);
    R.hitEvent = un::C("bolt.user", "", "VailHitEvent");
    if (R.hitEvent) {
        R.hitCreate = un::M(R.hitEvent, "Create", std::vector<std::string>{"Bolt.GlobalTargets", "Bolt.ReliabilityModes"});
        R.hitTarget = un::M(R.hitEvent, "set_Target", 1);
        R.hitDamage = un::M(R.hitEvent, "set_Damage", std::vector<std::string>{"System.Single"});
        R.hitNode = un::M(R.hitEvent, "set_DamageNodeId", std::vector<std::string>{"System.Int32"});
        R.hitImpact = un::M(R.hitEvent, "set_ImpactType", std::vector<std::string>{"System.Int32"});
        R.hitDir = un::M(R.hitEvent, "set_SenderDir", std::vector<std::string>{"UnityEngine.Vector3"});
        R.hitVelocity = un::M(R.hitEvent, "set_Velocity", std::vector<std::string>{"UnityEngine.Vector3"});
        R.evSend = un::M(R.hitEvent, "Send", 0);
    }
    R.onlyServer = Enum(un::C("bolt", "Bolt", "GlobalTargets"), "OnlyServer", 6);
    R.reliable = Enum(un::C("bolt", "Bolt", "ReliabilityModes"), "ReliableOrdered", 2);
    R.melee = Enum(un::C("Sons.Item", "", "ImpactType"), "Melee", 2);
    R.damageController = un::C("Sons.Weapon", "Sons.Weapon", "DamageController");
    R.damageNode = un::C("Sons.Weapon", "Sons.Weapon", "DamageNode");
    if (R.actor) R.acDamageCtrl = un::F(R.actor, "_damageController");
    if (R.damageController) R.dmNodes = un::F(R.damageController, "_damageNodes");
    if (R.damageNode) {
        R.nodeId = un::M(R.damageNode, "GetId", 0);
        R.nodeArmour = un::M(R.damageNode, "IsArmour", 0);
    }
    if (R.dismember) {
        R.sendCut = un::M(R.dismember, "SendImpactEventToServer", 2);
        if (R.sendCut && il2::ParamTypeName(R.sendCut, 1) != "System.Single") {
            un::Missing("DismembermentController.SendImpactEventToServer(DismemberPartId, float)");
            R.sendCut = nullptr;
        }
    }

    bool actors = R.mgrInstance && R.mgrInstance.isStatic && R.mgrActors && !R.mgrActors.isStatic && R.acId && R.acDead &&
                  (R.actorClass || R.isEnemy);
    R.killOk = actors && (R.killActor || (R.forceDeath && R.ragdoll));
    R.oneHitOk = R.killOk && R.acDamageByPlayer;
    R.dismemberOk = actors && R.acDismember && R.dcDefinitions && R.dcLimit && R.dfPart && R.dfCut && R.cut;
    bool events = R.boltEntity && R.beAttached && R.hitCreate && R.hitTarget && R.hitDamage && R.hitNode && R.hitImpact && R.evSend &&
                  R.acDamageCtrl && !R.acDamageCtrl.isStatic && R.dmNodes && !R.dmNodes.isStatic && R.nodeId;
    R.guestKillOk = actors && events;
    R.guestCutOk = R.guestKillOk && R.acDismember && R.dcDefinitions && R.dfPart && R.dfCut && R.sendCut;
    bool player = R.getStimuli || R.lpStimuli;
    R.invisibleOk = player && R.visibility && R.pvInvisible;
    R.ghostOk = player && R.psFlags && R.invisibleBit > 0;
    LOG("enemies: one hit %d, kill %d, dismember %d, invisibility %d, ghost %d, guest kill %d, guest dismember %d", R.oneHitOk,
        R.killOk, R.dismemberOk, R.invisibleOk, R.ghostOk, R.guestKillOk, R.guestCutOk);
    if (!R.killOk && !R.dismemberOk && !R.invisibleOk && !R.ghostOk && !R.guestKillOk) {
        err = "the game's enemy functions were not found";
        return false;
    }
    return true;
}

bool Supported(const std::string& id) {
    if (id == "one_hit_kill") return R.oneHitOk;
    if (id == "enemy_range") return R.killOk || R.dismemberOk || R.guestKillOk;
    if (id == "kill_in_range") return R.killOk || R.guestKillOk;
    if (id == "dismember_in_range") return R.dismemberOk || R.guestCutOk;
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

bool Guest(const Ctx& c) { return c.session == core::Session::Client; }

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
        if (!Guest(c) && Health(a) <= 0.0f) continue;
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

bool NodeIdOf(Object* a, int& id) {
    Object* ctrl = un::Ref(a, R.acDamageCtrl);
    if (!un::Alive(ctrl)) return false;
    Object* pick = nullptr;
    for (Object* n : il2::ListItems(un::Ref(ctrl, R.dmNodes))) {
        if (!un::Alive(n)) continue;
        if (!pick) pick = n;
        if (!R.nodeArmour || !un::CallBool(R.nodeArmour, n, {}, false)) {
            pick = n;
            break;
        }
    }
    if (!pick) return false;
    bool ok = false;
    Object* r = un::Call(R.nodeId, pick, {}, &ok);
    if (!ok || !r) return false;
    id = il2::Result<int>(r);
    return true;
}

Object* EntityOf(Object* a) {
    Object* be = un::Comp(a, R.boltEntity);
    if (!un::Alive(be) || !un::CallBool(R.beAttached, be, {}, false)) return nullptr;
    return be;
}

bool SendHit(Object* a, un::Vec3 from) {
    Object* be = EntityOf(a);
    int node = 0;
    if (!be || !NodeIdOf(a, node)) return false;
    uint8_t targets = (uint8_t)R.onlyServer, mode = (uint8_t)R.reliable;
    bool ok = false;
    Object* ev = un::Call(R.hitCreate, nullptr, {&targets, &mode}, &ok);
    if (!ok || !ev) return false;
    float damage = kNetDamage;
    int impact = R.melee;
    un::Call(R.hitTarget, ev, {be});
    un::Call(R.hitDamage, ev, {&damage});
    un::Call(R.hitNode, ev, {&node});
    un::Call(R.hitImpact, ev, {&impact});
    un::Vec3 at;
    if (PositionOf(a, at)) {
        un::Vec3 dir = un::Norm(un::Flat(un::Sub(at, from)));
        if (R.hitDir) un::Call(R.hitDir, ev, {&dir});
        if (R.hitVelocity) un::Call(R.hitVelocity, ev, {&dir});
    }
    ok = false;
    un::Call(R.evSend, ev, {}, &ok);
    return ok;
}

bool HasLimbs(Object* a) {
    Object* dc = un::Ref(a, R.acDismember);
    if (!un::Alive(dc) || un::Val<bool>(dc, R.dcDisabled)) return false;
    for (Object* d : il2::ListItems(un::Ref(dc, R.dcDefinitions)))
        if (d && !un::Val<bool>(d, R.dfCut) && IsLimb(un::Val<int>(d, R.dfPart, -1))) return true;
    return false;
}

int SendCuts(Object* a) {
    Object* dc = un::Ref(a, R.acDismember);
    if (!un::Alive(dc)) return 0;
    std::vector<int> parts;
    for (Object* d : il2::ListItems(un::Ref(dc, R.dcDefinitions))) {
        if (!d || un::Val<bool>(d, R.dfCut)) continue;
        int part = un::Val<int>(d, R.dfPart, -1);
        if (IsLimb(part) && std::find(parts.begin(), parts.end(), part) == parts.end()) parts.push_back(part);
    }
    int sent = 0;
    for (int part : parts) {
        if (!un::Alive(dc)) break;
        int p = part;
        float damage = kNetCut;
        bool ok = false;
        un::Call(R.sendCut, dc, {&p, &damage}, &ok);
        if (ok) ++sent;
    }
    return sent;
}

void ClearCuts() {
    for (PendingCut& p : cuts) p.actor.reset();
    cuts.clear();
}

void RunCuts(const Ctx& c) {
    int sent = 0;
    for (size_t i = 0; i < cuts.size();) {
        if (c.now < cuts[i].at) {
            ++i;
            continue;
        }
        Object* a = cuts[i].actor.get();
        if (a && un::Alive(a)) sent += SendCuts(a);
        cuts[i].actor.reset();
        cuts.erase(cuts.begin() + (std::ptrdiff_t)i);
    }
    if (sent > 0) LOG("dismember in range (guest): %d limb requests sent", sent);
}

bool GuestReady(const Ctx& c, un::Vec3& from) {
    if (CutscenePlaying()) {
        Note("Not during a cutscene");
        return false;
    }
    if (!PlayerPosition(c, from)) {
        Note("Your position is not known yet, try again in a moment");
        return false;
    }
    return true;
}

void GuestKill(const Ctx& c) {
    un::Vec3 from;
    if (!GuestReady(c, from)) return;
    WarnAchievementsOnce();
    std::vector<Object*> list = Targets(c);
    if (list.empty()) {
        Note("No enemies in range");
        return;
    }
    int sent = 0;
    for (Object* a : list)
        if (un::Alive(a) && SendHit(a, from)) ++sent;
    LOG("kill in range (guest): hits sent to %d of %d", sent, (int)list.size());
    if (sent == 0) Note("Could not send the hits to the host");
    else Note("Sent a killing hit to " + Count(sent, "enemy", "enemies") + ", the host applies it");
}

void GuestDismember(const Ctx& c) {
    un::Vec3 from;
    if (!GuestReady(c, from)) return;
    WarnAchievementsOnce();
    std::vector<Object*> list = Targets(c);
    if (list.empty()) {
        Note("No enemies in range");
        return;
    }
    int queued = 0, limbless = 0;
    for (Object* a : list) {
        if (!un::Alive(a)) continue;
        if (!HasLimbs(a)) {
            ++limbless;
            continue;
        }
        bool known = false;
        for (const PendingCut& p : cuts)
            if (p.actor.get() == a) known = true;
        if (known || !SendHit(a, from)) continue;
        PendingCut p;
        p.actor.set(a);
        p.at = c.now + kCutDelay;
        cuts.push_back(p);
        ++queued;
    }
    LOG("dismember in range (guest): %d of %d enemies, %d without limbs", queued, (int)list.size(), limbless);
    if (queued > 0) Note("Cutting the limbs off " + Count(queued, "enemy", "enemies") + " through the host");
    else if (limbless > 0) Note("No enemies in range could lose limbs");
    else Note("Could not send the hits to the host");
}

void OneHit(const Ctx& c) {
    bool on = R.oneHitOk && c.inWorld && !Guest(c) && c.s.On("one_hit_kill");
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
    ClearCuts();
}

void Tick(const Ctx& c) {
    if (!relaxed.empty()) Unrelax();
    if (!c.inWorld) {
        if (oneHitLive || invisState.have || invisState.obj.held() || ghostHeld.held() || visStim.held() || !cuts.empty()) Release(c);
        return;
    }
    if (!cuts.empty()) {
        if (Guest(c) && c.cheats) RunCuts(c);
        else ClearCuts();
    }
    OneHit(c);
    Stealth(c);
}

bool Action(const std::string& id, const Ctx& c) {
    bool guest = Guest(c);
    if (id == "kill_in_range") {
        if (guest ? !R.guestKillOk : !R.killOk) {
            Note(guest ? "This only works when you host or play alone" : "That is not available right now");
            return true;
        }
        if (guest) GuestKill(c);
        else KillInRange(c);
        return true;
    }
    if (id == "dismember_in_range") {
        if (guest ? !R.guestCutOk : !R.dismemberOk) {
            Note(guest ? "This only works when you host or play alone" : "That is not available right now");
            return true;
        }
        if (guest) GuestDismember(c);
        else DismemberInRange(c);
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
