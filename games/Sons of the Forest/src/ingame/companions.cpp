#include "ingame/cheats.h"
#include "ingame/log.h"
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace sotf {

namespace {

namespace fs = std::filesystem;

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;
using un::Quat;
using un::Vec3;

const int kNoState = INT_MIN;
const float kPinSlack = 1.0f;
const float kFront = 2.0f;
const float kSpawnFront = 2.5f;
const float kBehind = 3.0f;
const float kSpacing = 1.5f;
const double kReviveEvery = 0.5;
const double kSpawnCooldown = 10.0;
const double kStateRetry = 1.0;

struct Refs {
    Class *actor = nullptr, *manager = nullptr, *sim = nullptr, *simActor = nullptr, *state = nullptr, *typeIds = nullptr,
          *vailStats = nullptr, *statsManager = nullptr, *stat = nullptr, *health = nullptr, *effects = nullptr,
          *effectTypes = nullptr, *robby = nullptr, *injured = nullptr, *localPlayer = nullptr, *carryBody = nullptr,
          *cutscenes = nullptr, *gameState = nullptr, *setup = nullptr, *saves = nullptr, *coop = nullptr;
    Fld acId, acState, acAnimator, acWorld, acDead, acDamage, acStats, acEffects;
    Fld mgInstance, mgActive, mgReserved;
    Fld wsInstance, wsActors, wsRobby, wsVirginia, wsLoading;
    Fld saType, saState, saRemoved, saStats, saPosition, saReal;
    Fld stId, stWorld, stDying, stInjured, stDead, stCart, stRaft, stCaptured;
    Fld vsStats, smStats, statCur, statMax;
    Fld robbyInjured, carryActor, lpCarry, csInstance, csActive;
    Method *acRevive = nullptr, *acSetDamage = nullptr, *acSnap = nullptr, *acRobby = nullptr, *acGetAnimator = nullptr;
    Method *mgRemove = nullptr, *mgFind = nullptr, *mgAnimSpeed = nullptr;
    Method *wsRemove = nullptr, *wsSpawn = nullptr;
    Method *saSetPosition = nullptr, *saSetDestination = nullptr, *saGetPosition = nullptr, *saIsLiving = nullptr;
    Method *smGetStat = nullptr, *statSet = nullptr, *fxRemove = nullptr;
    Method *animGetSpeed = nullptr, *animSetSpeed = nullptr;
    Method *lpTransform = nullptr, *lpCaves = nullptr, *carrying = nullptr;
    Method *robbyDead = nullptr, *virginiaDead = nullptr, *setRobbyDead = nullptr, *setVirginiaDead = nullptr;
    Method *saveType = nullptr, *gameId = nullptr, *slotForGuid = nullptr, *slotFolder = nullptr;
    int robbyType = -1, virginiaType = -1;
    int burning = -1, bleeding = -1, poisoned = -1, gettingUp = -1;
    Object* healthType = nullptr;
    Class *boltEntity = nullptr, *reviveEvent = nullptr;
    Method *beAttached = nullptr, *revCreate = nullptr, *revTarget = nullptr, *evSend = nullptr;
    int onlyServer = 6, reliable = 2;
} R;

struct Avail {
    bool find = false, health = false, heal = false, revive = false, bring = false, freeze = false, god = false;
    bool flagKelvin = false, flagVirginia = false, guestRevive = false;
} A;

struct StateIds {
    bool ok = false;
    bool logged = false;
    double tried = -1000;
    int world = kNoState, dying = kNoState, injured = kNoState, dead = kNoState, cart = kNoState, raft = kNoState,
        captured = kNoState;
} S;

struct Anchor {
    uint32_t h = 0;
    Vec3 at;
};

struct Kind {
    const char* name;
    const char* their;
    const char* heal;
    const char* revive;
    const char* bring;
    const char* freeze;
    const char* god;
    bool kelvin;
    float sentiment;
    int type = -1;
    std::vector<uint32_t> frozen, guarded;
    std::vector<Anchor> anchors;
    double lastRevive = -1000, spawnedAt = -1000;
    bool wasAlive = false;
};

Kind kinds[2] = {
    {"Kelvin", "his", "kelvin_heal", "kelvin_revive", "kelvin_bring", "kelvin_freeze", "kelvin_godmode", true, 0.0f},
    {"Virginia", "her", "virginia_heal", "virginia_revive", "virginia_bring", "virginia_freeze", "virginia_godmode", false, 60.0f}};

struct Mate {
    Object* w = nullptr;
    Object* real = nullptr;
    int state = kNoState;
    bool dead = false;
    bool dying = false;
    bool converting = false;
};

enum class Fill { Missing, Full, Done };

enum class Saved { Done, Nothing, Failed };

bool Inst(const Fld& f) { return f && !f.isStatic; }

bool Glob(const Fld& f) { return f && f.isStatic; }

Method* StaticM(Class* c, const char* name, int args) {
    if (!c) return nullptr;
    Method* m = il2::FindStaticMethod(c, name, args);
    if (!m) un::Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
    return m;
}

int StateOffset() { return R.stId.off >= 0x10 ? R.stId.off - 0x10 : 0; }

int StateOf(Object* o, const Fld& f) {
    if (!o || !Inst(f) || !Inst(R.stId)) return kNoState;
    return il2::Get<int>(o, f.off + StateOffset());
}

int ReadState(const Fld& f) {
    if (!Glob(f)) return kNoState;
    alignas(16) unsigned char buf[0x40] = {};
    if (!il2::StaticGet(f.f, buf, sizeof(buf))) return kNoState;
    int v = 0;
    std::memcpy(&v, buf + StateOffset(), sizeof(v));
    return v;
}

bool States(double now, bool force) {
    if (S.ok) return true;
    if (!force && now - S.tried < kStateRetry) return false;
    S.tried = now;
    S.world = ReadState(R.stWorld);
    S.dying = ReadState(R.stDying);
    S.injured = ReadState(R.stInjured);
    S.dead = ReadState(R.stDead);
    S.cart = ReadState(R.stCart);
    S.raft = ReadState(R.stRaft);
    S.captured = ReadState(R.stCaptured);
    S.ok = S.dying != kNoState && S.dead != kNoState && S.dying != S.dead && S.dying >= 0 && S.dead >= 0;
    if (S.ok) {
        LOG("Kelvin and Virginia: states world %d, dying %d, injured %d, dead %d, golf cart %d, raft %d, captured %d", S.world, S.dying,
            S.injured, S.dead, S.cart, S.raft, S.captured);
    } else if (!S.logged) {
        S.logged = true;
        LOG("Kelvin and Virginia: the companion states could not be read yet (dying %d, dead %d)", S.dying, S.dead);
    }
    return S.ok;
}

Object* World() {
    if (!Glob(R.wsInstance)) return nullptr;
    Object* ws = un::Static(R.wsInstance);
    return un::Alive(ws) ? ws : nullptr;
}

Object* Manager() {
    if (!Glob(R.mgInstance)) return nullptr;
    Object* mg = un::Static(R.mgInstance);
    return un::Alive(mg) ? mg : nullptr;
}

bool SimLoading() { return Glob(R.wsLoading) && un::StaticVal<bool>(R.wsLoading, false); }

bool Cutscene() {
    if (!Glob(R.csInstance) || !Inst(R.csActive)) return false;
    Object* cm = un::Static(R.csInstance);
    return un::Alive(cm) && un::Alive(un::Ref(cm, R.csActive));
}

bool InCaves() { return R.lpCaves && un::CallBool(R.lpCaves, nullptr); }

void AddWorld(std::vector<Object*>& out, Object* w, int type) {
    if (!w || un::Val<int>(w, R.saType, -1) != type) return;
    if (std::find(out.begin(), out.end(), w) == out.end()) out.push_back(w);
}

std::vector<Object*> WorldActors(Object* ws, const Kind& k, bool thorough) {
    std::vector<Object*> out;
    const Fld& own = k.kelvin ? R.wsRobby : R.wsVirginia;
    if (Inst(own))
        for (Object* w : il2::ListItems(un::Ref(ws, own))) AddWorld(out, w, k.type);
    if ((thorough || !Inst(own)) && Inst(R.wsActors))
        for (Object* w : il2::ListItems(un::Ref(ws, R.wsActors))) AddWorld(out, w, k.type);
    return out;
}

std::vector<Object*> RealActors(int type) {
    std::vector<Object*> out;
    Object* mg = Manager();
    if (!mg) return out;
    for (Object* a : il2::ListItems(un::Ref(mg, R.mgActive)))
        if (un::Alive(a) && un::Val<int>(a, R.acId, -1) == type) out.push_back(a);
    return out;
}

bool Find(const Kind& k, bool thorough, std::vector<Mate>& out) {
    out.clear();
    Object* ws = World();
    if (!ws || !S.ok) return false;
    std::vector<Object*> reals = RealActors(k.type);
    for (Object* w : WorldActors(ws, k, thorough)) {
        if (un::Val<uint8_t>(w, R.saRemoved) != 0) continue;
        Mate m;
        m.w = w;
        for (Object* a : reals)
            if (un::Ref(a, R.acWorld) == w) {
                m.real = a;
                break;
            }
        int simState = StateOf(w, R.saState);
        m.state = m.real ? StateOf(m.real, R.acState) : simState;
        m.dead = simState == S.dead || (m.real && (un::Val<uint8_t>(m.real, R.acDead) != 0 || m.state == S.dead));
        m.dying = !m.dead && m.state == S.dying;
        m.converting = !m.real && un::Val<uint8_t>(w, R.saReal) != 0;
        out.push_back(m);
    }
    return true;
}

bool AnyAlive(const std::vector<Mate>& mates) {
    for (const Mate& m : mates)
        if (!m.dead) return true;
    return false;
}

bool AnyLiving(const Kind& k) {
    Object* ws = World();
    if (!ws) return true;
    for (Object* w : WorldActors(ws, k, true)) {
        if (R.saIsLiving) {
            if (un::CallBool(R.saIsLiving, w, {}, true)) return true;
        } else if (un::Val<uint8_t>(w, R.saRemoved) == 0 && StateOf(w, R.saState) != S.dead) {
            return true;
        }
    }
    for (Object* a : RealActors(k.type)) {
        if (un::Val<uint8_t>(a, R.acDead, 1) != 0) continue;
        Object* w = un::Ref(a, R.acWorld);
        if (w && (!R.saIsLiving || un::CallBool(R.saIsLiving, w, {}, true))) return true;
    }
    return false;
}

Object* HealthType() {
    if (!R.healthType && R.health) R.healthType = il2::TypeObject(R.health);
    return R.healthType;
}

Object* StatsOf(const Mate& m) {
    Object* sm = un::Ref(m.w, R.saStats);
    if (!sm && m.real) {
        Object* vs = un::Ref(m.real, R.acStats);
        if (un::Alive(vs)) sm = un::Ref(vs, R.vsStats);
    }
    return sm;
}

Object* HealthOf(const Mate& m) {
    Object* sm = StatsOf(m);
    if (!sm || !R.health) return nullptr;
    if (Inst(R.smStats)) {
        std::vector<Object*> stats = il2::ListItems(un::Ref(sm, R.smStats));
        for (Object* s : stats)
            if (s && il2::ClassOf(s) == R.health) return s;
        for (Object* s : stats)
            if (s && il2::IsSubclass(il2::ClassOf(s), R.health)) return s;
    }
    if (R.smGetStat)
        if (Object* t = HealthType()) return un::Call(R.smGetStat, sm, {t});
    return nullptr;
}

Fill Top(Object* hp) {
    if (!hp || !R.statSet) return Fill::Missing;
    float mx = un::Val<float>(hp, R.statMax);
    if (!(mx > 0.0f) || !std::isfinite(mx)) return Fill::Missing;
    if (un::Val<float>(hp, R.statCur) >= mx - 0.01f) return Fill::Full;
    un::Call(R.statSet, hp, {&mx});
    return Fill::Done;
}

void Cleanse(Object* real) {
    Object* fx = un::Ref(real, R.acEffects);
    if (!fx || !R.fxRemove) return;
    for (int t : {R.bleeding, R.poisoned, R.burning}) {
        if (t < 0) continue;
        int v = t;
        un::Call(R.fxRemove, fx, {&v});
    }
}

void GetUp(const Mate& m) {
    Object* hp = HealthOf(m);
    Top(hp);
    if (m.real && R.acRevive) un::Call(R.acRevive, m.real);
    Top(hp);
}

Object* AnimatorOf(Object* a) {
    if (Inst(R.acAnimator)) return un::Ref(a, R.acAnimator);
    return R.acGetAnimator ? un::Call(R.acGetAnimator, a) : nullptr;
}

Vec3 SimPos(Object* w) {
    if (Inst(R.saPosition)) return un::Val<Vec3>(w, R.saPosition);
    return un::CallVec3(R.saGetPosition, w);
}

void Stay(Object* w, Vec3 at) {
    if (!R.saSetDestination) return;
    bool family = false;
    un::Call(R.saSetDestination, w, {&at, &family});
}

void Track(std::vector<uint32_t>& held, Object* o) {
    for (uint32_t h : held)
        if (il2::Pinned(h) == o) return;
    if (uint32_t h = il2::Pin(o)) held.push_back(h);
}

template <class F>
void Prune(std::vector<uint32_t>& held, const std::vector<Object*>& keep, F restore) {
    for (size_t i = 0; i < held.size();) {
        Object* o = il2::Pinned(held[i]);
        if (o && std::find(keep.begin(), keep.end(), o) != keep.end()) {
            ++i;
            continue;
        }
        if (o) restore(o);
        il2::Unpin(held[i]);
        held.erase(held.begin() + (std::ptrdiff_t)i);
    }
}

Anchor* AnchorOf(Kind& k, Object* w) {
    for (Anchor& a : k.anchors)
        if (il2::Pinned(a.h) == w) return &a;
    return nullptr;
}

void AddAnchor(Kind& k, Object* w, Vec3 at) {
    Anchor a;
    a.h = il2::Pin(w);
    a.at = at;
    if (a.h) k.anchors.push_back(a);
}

void DropAnchor(Kind& k, Object* w) {
    for (size_t i = 0; i < k.anchors.size(); ++i) {
        if (il2::Pinned(k.anchors[i].h) != w) continue;
        il2::Unpin(k.anchors[i].h);
        k.anchors.erase(k.anchors.begin() + (std::ptrdiff_t)i);
        return;
    }
}

void PruneAnchors(Kind& k, const std::vector<Mate>& mates) {
    for (size_t i = 0; i < k.anchors.size();) {
        Object* w = il2::Pinned(k.anchors[i].h);
        bool keep = false;
        for (const Mate& m : mates)
            if (m.w == w && !m.dead && !m.real) keep = true;
        if (w && keep) {
            ++i;
            continue;
        }
        il2::Unpin(k.anchors[i].h);
        k.anchors.erase(k.anchors.begin() + (std::ptrdiff_t)i);
    }
}

void Unfreeze(Object* a) {
    if (!un::Alive(a) || !R.animSetSpeed) return;
    Object* anim = AnimatorOf(a);
    if (!un::Alive(anim)) return;
    if (!R.animGetSpeed || un::CallFloat(R.animGetSpeed, anim, {}, -1.0f) != 0.0f) return;
    float speed = R.mgAnimSpeed ? un::CallFloat(R.mgAnimSpeed, nullptr, {}, 1.0f) : 1.0f;
    if (!(speed >= 0.0f) || !std::isfinite(speed)) speed = 1.0f;
    un::Call(R.animSetSpeed, anim, {&speed});
}

bool GettingUp(Object* a) {
    if (!R.robby || !Inst(R.robbyInjured) || R.gettingUp < 0) return false;
    Object* r = R.acRobby ? un::Call(R.acRobby, a) : nullptr;
    if (!r || !il2::IsSubclass(il2::ClassOf(r), R.robby)) r = un::Comp(a, R.robby);
    if (!un::Alive(r)) return false;
    return un::Val<int>(r, R.robbyInjured, -1) == R.gettingUp;
}

void Unguard(Object* a, const Kind& k) {
    if (!un::Alive(a) || !R.acSetDamage || un::Val<uint8_t>(a, R.acDamage, 1) != 0) return;
    if (k.kelvin && GettingUp(a)) return;
    bool on = true;
    un::Call(R.acSetDamage, a, {&on});
}

void ReleaseFreeze(Kind& k) {
    for (uint32_t h : k.frozen) {
        if (Object* o = il2::Pinned(h)) Unfreeze(o);
        il2::Unpin(h);
    }
    k.frozen.clear();
    for (const Anchor& a : k.anchors) il2::Unpin(a.h);
    k.anchors.clear();
}

void ReleaseGuard(Kind& k) {
    for (uint32_t h : k.guarded) {
        if (Object* o = il2::Pinned(h)) Unguard(o, k);
        il2::Unpin(h);
    }
    k.guarded.clear();
    k.wasAlive = false;
}

void Freeze(Kind& k, const std::vector<Mate>& mates) {
    std::vector<Object*> keep;
    for (const Mate& m : mates) {
        if (m.dead) continue;
        if (m.real) {
            DropAnchor(k, m.w);
            Object* anim = AnimatorOf(m.real);
            if (!un::Alive(anim)) continue;
            keep.push_back(m.real);
            Track(k.frozen, m.real);
            float zero = 0.0f;
            un::Call(R.animSetSpeed, anim, {&zero});
            continue;
        }
        if (m.converting) continue;
        Vec3 now = SimPos(m.w);
        Anchor* a = AnchorOf(k, m.w);
        if (!a) {
            AddAnchor(k, m.w, now);
            Stay(m.w, now);
            continue;
        }
        if (un::Dist(now, a->at) > kPinSlack) {
            Vec3 at = a->at;
            un::Call(R.saSetPosition, m.w, {&at});
            Stay(m.w, at);
        }
    }
    Prune(k.frozen, keep, [](Object* o) { Unfreeze(o); });
    PruneAnchors(k, mates);
}

void Guard(Kind& k, const std::vector<Mate>& mates, const Ctx& c) {
    std::vector<Object*> keep;
    int living = 0;
    bool died = false;
    for (const Mate& m : mates) {
        if (m.dead) {
            died = true;
            continue;
        }
        ++living;
        Object* hp = HealthOf(m);
        Top(hp);
        if (!m.real) continue;
        keep.push_back(m.real);
        if (un::Val<uint8_t>(m.real, R.acDamage) != 0) {
            Track(k.guarded, m.real);
            bool off = false;
            un::Call(R.acSetDamage, m.real, {&off});
        }
        if (m.dying && c.now - k.lastRevive >= kReviveEvery) {
            k.lastRevive = c.now;
            un::Call(R.acRevive, m.real);
            Top(hp);
        }
    }
    if (k.wasAlive && living == 0 && died) Note(std::string(k.name) + " died anyway, use Revive " + k.name);
    k.wasAlive = living > 0;
    Prune(k.guarded, keep, [&k](Object* o) { Unguard(o, k); });
}

bool Pose(const Ctx& c, Vec3& pos, Vec3& fwd) {
    Object* t = R.lpTransform ? un::Call(R.lpTransform, nullptr) : nullptr;
    if (!un::Alive(t)) t = un::TransformOf(c.player);
    if (!un::Alive(t)) return false;
    pos = un::Position(t);
    fwd = un::Norm(un::Flat(un::Forward(t)));
    if (un::Len(fwd) < 0.5f) fwd = Vec3{0.0f, 0.0f, 1.0f};
    return std::isfinite(pos.x) && std::isfinite(pos.y) && std::isfinite(pos.z) && un::Len(pos) > 0.01f;
}

Quat Facing(Vec3 from, Vec3 to) {
    Vec3 d = un::Flat(un::Sub(to, from));
    float yaw = std::atan2(d.x, d.z);
    return Quat{0.0f, std::sin(yaw * 0.5f), 0.0f, std::cos(yaw * 0.5f)};
}

bool DeadFlag(const Kind& k) {
    Method* m = k.kelvin ? R.robbyDead : R.virginiaDead;
    return m && un::CallBool(m, nullptr);
}

void SetDeadFlag(const Kind& k, bool dead) {
    Method* m = k.kelvin ? R.setRobbyDead : R.setVirginiaDead;
    if (m) un::Call(m, nullptr, {&dead});
}

bool BodyCarried(const Kind& k, const Ctx& c) {
    Object* cb = un::Ref(c.player, R.lpCarry);
    if (un::Alive(cb) && (!R.carrying || un::CallBool(R.carrying, cb))) {
        Object* body = un::Ref(cb, R.carryActor);
        if (un::Alive(body) && un::Val<int>(body, R.acId, -1) == k.type) return true;
    }
    Object* mg = Manager();
    if (!mg || !Inst(R.mgReserved)) return false;
    for (const auto& e : il2::DictEntries(un::Ref(mg, R.mgReserved)))
        if (un::Alive(e.first) && un::Val<int>(e.first, R.acId, -1) == k.type) return true;
    return false;
}

void RemoveBody(const Mate& m) {
    Object* body = m.real;
    if (!body && R.mgFind) {
        Object* a = un::Call(R.mgFind, nullptr, {m.w});
        if (un::Alive(a)) body = a;
    }
    if (body && R.mgRemove) un::Call(R.mgRemove, nullptr, {body});
    Object* ws = World();
    if (ws && R.wsRemove && un::Val<uint8_t>(m.w, R.saRemoved) == 0) un::Call(R.wsRemove, ws, {m.w});
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

bool SlotFolder(std::string& dir) {
    dir.clear();
    if (!R.saveType || !R.gameId || !R.slotForGuid || !R.slotFolder) return false;
    bool ok = false;
    Object* boxed = un::Call(R.saveType, nullptr, {}, &ok);
    if (!ok || !boxed) return false;
    int type = il2::Result<int>(boxed, -1);
    if (type < 0) return false;
    Object* gid = un::Call(R.gameId, nullptr, {}, &ok);
    if (!ok || il2::Str(gid).empty()) return false;
    Object* found = un::Call(R.slotForGuid, nullptr, {&type, gid}, &ok);
    if (!ok || !found) return false;
    uint32_t slot = il2::Result<uint32_t>(found, 0);
    if (slot == 0) return true;
    int id = (int)slot;
    Object* path = un::Call(R.slotFolder, nullptr, {&type, &id}, &ok);
    if (!ok) return false;
    dir = il2::Str(path);
    return !dir.empty();
}

fs::path GameSaves() {
    PWSTR low = nullptr;
    fs::path out;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppDataLow, 0, nullptr, &low)) && low)
        out = fs::path(low) / L"Endnight" / L"SonsOfTheForest" / L"Saves";
    if (low) CoTaskMemFree(low);
    return out;
}

bool CopyTree(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return false;
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    int files = 0;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        fs::path rel = fs::relative(it->path(), src, e2);
        if (e2) return false;
        uintmax_t a = it->file_size(e2);
        if (e2) return false;
        uintmax_t b = fs::file_size(dst / rel, e2);
        if (e2 || a != b) return false;
        ++files;
    }
    return !ec && files > 0;
}

Saved BackupSave(const Kind& k, std::wstring& where) {
    where.clear();
    std::string dir;
    bool known = SlotFolder(dir);
    try {
        const std::wstring& base = Folder();
        if (base.empty()) return Saved::Failed;
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf_s(stamp, L"_%04d-%02d-%02d_%02d-%02d-%02d-%03d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                   t.wMilliseconds);
        fs::path root = fs::path(base) / L"save-backups" / (L"Revive" + Wide(k.name) + stamp);
        std::error_code ec;
        fs::path src, dst;
        if (known) {
            if (dir.empty()) return Saved::Nothing;
            std::wstring w = Wide(dir);
            while (!w.empty() && (w.back() == L'/' || w.back() == L'\\')) w.pop_back();
            src = fs::path(w).make_preferred();
            if (src.empty() || !fs::is_directory(src, ec)) return Saved::Nothing;
            dst = root / src.parent_path().filename() / src.filename();
        } else {
            src = GameSaves();
            if (src.empty()) return Saved::Failed;
            if (!fs::is_directory(src, ec)) return ec ? Saved::Failed : Saved::Nothing;
            dst = root / L"Saves";
        }
        if (!CopyTree(src, dst)) return Saved::Failed;
        where = dst.wstring();
        return Saved::Done;
    } catch (...) {
        return Saved::Failed;
    }
}

bool Spawn(const Kind& k, Vec3 at, Quat q) {
    int type = k.type;
    float sentiment = k.sentiment;
    bool ok = false;
    un::Call(R.wsSpawn, nullptr, {&type, &at, &q, &sentiment}, &ok);
    return ok;
}

const char* Busy(int st) {
    if (st == kNoState) return nullptr;
    if (st == S.injured) return " is still hurt and cannot be moved yet";
    if (st == S.cart) return " is sitting in the golf cart";
    if (st == S.raft) return " is on the raft";
    if (st == S.captured) return " cannot be moved right now";
    return nullptr;
}

void NotHere(const Kind& k, const std::vector<Mate>& mates) {
    bool dead = false;
    for (const Mate& m : mates)
        if (m.dead) dead = true;
    if (!dead) dead = DeadFlag(k);
    std::string who = k.name;
    if (dead) Note(who + " is dead, use Revive " + who);
    else Note(who + " is not in this world yet");
}

bool Ready(const Ctx& c, const Kind& k, bool thorough, std::vector<Mate>& mates) {
    if (!c.inWorld || !c.player) {
        Note("Load a save first");
        return false;
    }
    if (SimLoading() || !World()) {
        Note("Wait until the world has finished loading");
        return false;
    }
    if (!States(c.now, true) || !Find(k, thorough, mates)) {
        Note("The game is not ready yet, try again in a moment");
        return false;
    }
    return true;
}

void HealAction(Kind& k, const Ctx& c) {
    std::vector<Mate> mates;
    if (!Ready(c, k, false, mates)) return;
    std::string who = k.name;
    int living = 0, missing = 0, done = 0, raised = 0, waiting = 0;
    for (const Mate& m : mates) {
        if (m.dead) continue;
        ++living;
        Fill f = Top(HealthOf(m));
        if (f == Fill::Missing) ++missing;
        else if (f == Fill::Done) ++done;
        if (!m.real) {
            if (m.dying) ++waiting;
            continue;
        }
        Cleanse(m.real);
        if (m.dying) {
            GetUp(m);
            ++raised;
        }
    }
    if (living == 0) {
        NotHere(k, mates);
        return;
    }
    if (missing == living) {
        Note("Could not reach " + who + "'s health");
        return;
    }
    if (raised > 0) Note(who + " is healed and back on " + k.their + " feet");
    else if (waiting > 0) Note(who + " is healed and gets up when you are close");
    else if (done > 0) Note(who + " is healed");
    else Note(who + " already has full health");
}

void ReviveAction(Kind& k, const Ctx& c) {
    std::vector<Mate> mates;
    if (!Ready(c, k, true, mates)) return;
    std::string who = k.name;
    int living = 0, raised = 0, waiting = 0;
    for (const Mate& m : mates) {
        if (m.dead) continue;
        ++living;
        if (!m.dying) continue;
        if (m.real) {
            Cleanse(m.real);
            GetUp(m);
            ++raised;
        } else {
            Top(HealthOf(m));
            ++waiting;
        }
    }
    if (living > 0) {
        if (raised > 0) Note(who + " is back on " + k.their + " feet");
        else if (waiting > 0) Note(who + " is healed and gets up when you are close");
        else Note(who + " is alive, there is nothing to revive");
        return;
    }
    if (AnyLiving(k)) {
        LOG("Kelvin and Virginia: %s looked dead but the game still lists %s as living, nothing was spawned", k.name, k.name);
        Note(who + " is alive, there is nothing to revive");
        return;
    }
    if (c.now - k.spawnedAt < kSpawnCooldown) {
        Note(who + " is already on the way");
        return;
    }
    bool flagged = DeadFlag(k);
    if (mates.empty() && !flagged) {
        Note(who + " is not in this world yet");
        return;
    }
    if (InCaves()) {
        Note("Leave the cave first, " + who + " cannot come in");
        return;
    }
    if (Cutscene()) {
        Note("Wait until the cutscene is over");
        return;
    }
    Vec3 p, f;
    if (!Pose(c, p, f)) {
        Note("Your position is not known yet, try again in a moment");
        return;
    }
    if (BodyCarried(k, c)) {
        Note(who + "'s body is being carried, put it down and try again in a few seconds");
        return;
    }
    std::wstring where;
    Saved saved = BackupSave(k, where);
    if (saved == Saved::Failed) {
        LOG("Kelvin and Virginia: the save could not be backed up, %s was not revived", k.name);
        Note("Could not back up your save, so " + who + " was not revived");
        return;
    }
    if (saved == Saved::Done) LOG("Kelvin and Virginia: save backed up to %s", Narrow(where).c_str());
    else LOG("Kelvin and Virginia: this game has no save yet, nothing to back up");
    WarnAchievementsOnce();
    for (const Mate& m : mates) RemoveBody(m);
    Vec3 at = un::Add(p, un::Mul(f, kSpawnFront));
    bool spawned = Spawn(k, at, Facing(at, p));
    if (!AnyLiving(k)) {
        LOG("Kelvin and Virginia: %s could not be spawned (%s)", k.name, spawned ? "no error" : il2::LastException().c_str());
        if (!flagged) SetDeadFlag(k, true);
        Note(who + " could not be brought back, see fathom.log");
        return;
    }
    if (!spawned) LOG("Kelvin and Virginia: the spawn reported an error but %s is back", k.name);
    k.spawnedAt = c.now;
    SetDeadFlag(k, false);
    LOG("Kelvin and Virginia: %s revived, %d old entries removed", k.name, (int)mates.size());
    if (saved == Saved::Done) Note(who + " is back next to you, your save was backed up first");
    else Note(who + " is back next to you");
}

void BringAction(Kind& k, const Ctx& c) {
    std::vector<Mate> mates;
    if (!Ready(c, k, false, mates)) return;
    std::string who = k.name;
    if (!AnyAlive(mates)) {
        NotHere(k, mates);
        return;
    }
    if (InCaves()) {
        Note(who + " cannot come into caves");
        return;
    }
    if (Cutscene()) {
        Note("Wait until the cutscene is over");
        return;
    }
    Vec3 p, f;
    if (!Pose(c, p, f)) {
        Note("Your position is not known yet, try again in a moment");
        return;
    }
    Vec3 side{f.z, 0.0f, -f.x};
    int nearby = 0, away = 0, slot = 0;
    std::string why;
    for (const Mate& m : mates) {
        if (m.dead) continue;
        if (m.dying) {
            why = who + " is down, use Revive " + who + " first";
            continue;
        }
        int st = m.real ? m.state : StateOf(m.w, R.saState);
        if (const char* b = Busy(st)) {
            why = who + b;
            continue;
        }
        Vec3 off = un::Mul(side, kSpacing * (float)slot);
        bool ok = false;
        if (m.real) {
            Vec3 at = un::Add(un::Add(p, un::Mul(f, kFront)), off);
            float spread = 0.0f;
            un::Call(R.acSnap, m.real, {&at, &spread}, &ok);
            if (ok) ++nearby;
        } else {
            Vec3 at = un::Add(un::Sub(p, un::Mul(f, kBehind)), off);
            un::Call(R.saSetPosition, m.w, {&at}, &ok);
            if (ok) {
                Stay(m.w, at);
                if (Anchor* a = AnchorOf(k, m.w)) a->at = at;
                ++away;
            }
        }
        ++slot;
    }
    if (nearby + away == 0) {
        Note(why.empty() ? "Could not move " + who + ", see fathom.log" : why);
        return;
    }
    if (away == 0) Note(who + " is here");
    else Note(who + " is on the way and shows up right behind you");
}

bool Guest(const Ctx& c) { return c.session == core::Session::Client; }

bool HostRevive(const Kind& k) { return A.revive && (k.kelvin ? A.flagKelvin : A.flagVirginia); }

bool SendRevive(Object* a) {
    Object* be = un::Comp(a, R.boltEntity);
    if (!un::Alive(be) || !un::CallBool(R.beAttached, be, {}, false)) return false;
    uint8_t targets = (uint8_t)R.onlyServer, mode = (uint8_t)R.reliable;
    bool ok = false;
    Object* ev = un::Call(R.revCreate, nullptr, {&targets, &mode}, &ok);
    if (!ok || !ev) return false;
    un::Call(R.revTarget, ev, {be});
    ok = false;
    un::Call(R.evSend, ev, {}, &ok);
    return ok;
}

void GuestRevive(Kind& k, const Ctx& c) {
    std::string who = k.name;
    if (!c.inWorld || !c.player) {
        Note("Load a save first");
        return;
    }
    if (!States(c.now, true)) {
        Note("The game is not ready yet, try again in a moment");
        return;
    }
    if (c.now - k.lastRevive < kReviveEvery) return;
    int alive = 0, sent = 0;
    for (Object* a : RealActors(k.type)) {
        if (un::Val<uint8_t>(a, R.acDead, 1) != 0) continue;
        ++alive;
        if (StateOf(a, R.acState) != S.dying) continue;
        if (SendRevive(a)) ++sent;
    }
    LOG("Kelvin and Virginia: guest revive of %s, %d nearby, %d requests sent", k.name, alive, sent);
    if (sent > 0) {
        k.lastRevive = c.now;
        WarnAchievementsOnce();
        Note("Asked the host to revive " + who);
    } else if (alive > 0) {
        Note(who + " is not down, there is nothing to revive");
    } else {
        Note(who + " is not near you. A guest can only revive " + who + " while down, not after death");
    }
}

bool Supported(const std::string& id) {
    for (const Kind& k : kinds) {
        if (id == k.heal) return A.heal;
        if (id == k.revive) return HostRevive(k) || A.guestRevive;
        if (id == k.bring) return A.bring;
        if (id == k.freeze) return A.freeze;
        if (id == k.god) return A.god;
    }
    return false;
}

bool Resolve(std::string& err) {
    const char* vail = "Sons.Ai.Vail";
    const char* stats = "Sons.StatSystem";
    R.actor = un::C(vail, vail, "VailActor");
    R.manager = un::C(vail, vail, "VailActorManager");
    R.sim = un::C(vail, vail, "VailWorldSimulation");
    R.simActor = un::C(vail, vail, "WorldSimActor");
    R.state = un::C(vail, vail, "State");
    R.typeIds = un::C(vail, vail, "VailActorTypeId");
    R.vailStats = un::C(vail, vail, "VailStatsManager");
    R.statsManager = un::C(stats, stats, "StatsManager");
    R.stat = un::C(stats, stats, "Stat");
    R.health = un::C(stats, stats, "HealthStat");
    R.effects = un::C("Sons.Gameplay.StatusEffects", "Sons.Gameplay", "StatusEffectManager");
    R.effectTypes = un::C("Sons.Gameplay.StatusEffects", "Sons.Gameplay", "StatusEffectType");
    R.robby = un::C("Sons", "Sons.Gameplay", "Robby");
    R.injured = R.robby ? il2::Nested(R.robby, "InjuredState") : nullptr;
    if (R.robby && !R.injured) un::Missing("Robby.InjuredState");
    R.localPlayer = un::C("Sons", "TheForest.Utils", "LocalPlayer");
    R.carryBody = un::C("Sons", "Sons.Gameplay.CarryBody", "CarryBody");
    R.cutscenes = un::C("Sons", "Sons.Cutscenes", "CutsceneManager");
    R.gameState = un::C("Sons.Gameplay.GameSetup", "Sons.Save", "GameState");
    R.setup = un::C("Sons.Gameplay.GameSetup", "Sons.Gameplay.GameSetup", "GameSetupManager");
    R.saves = un::C("Sons.Save", "Sons.Save", "SaveGameManager");
    R.coop = un::C("Sons.Multiplayer", "Sons.Multiplayer", "CoopSaveUtils");

    R.acId = un::F(R.actor, "_id");
    R.acState = un::F(R.actor, "_currentState");
    R.acAnimator = un::F(R.actor, "_animator");
    R.acWorld = un::F(R.actor, "_worldSimActor");
    R.acDead = un::F(R.actor, "_isDead");
    R.acDamage = un::F(R.actor, "_damageEnabled");
    R.acStats = un::F(R.actor, "_statsManager");
    R.acEffects = un::F(R.actor, "_statusEffectManager");
    R.mgInstance = un::F(R.manager, "_instance");
    R.mgActive = un::F(R.manager, "_activeActors");
    R.mgReserved = un::F(R.manager, "_reservedBodies");
    R.wsInstance = un::F(R.sim, "_instance");
    R.wsActors = un::F(R.sim, "_actors");
    R.wsRobby = un::F(R.sim, "_robbyActors");
    R.wsVirginia = un::F(R.sim, "_virginiaActors");
    R.wsLoading = un::F(R.sim, "<IsLoadingSave>k__BackingField");
    R.saType = un::F(R.simActor, "_typeId");
    R.saState = un::F(R.simActor, "_state");
    R.saRemoved = un::F(R.simActor, "_removed");
    R.saStats = un::F(R.simActor, "_statsManager");
    R.saPosition = un::F(R.simActor, "_position");
    R.saReal = un::F(R.simActor, "<IsRealActor>k__BackingField");
    R.stId = un::F(R.state, "_id");
    R.stWorld = un::F(R.state, "World");
    R.stDying = un::F(R.state, "Dying");
    R.stInjured = un::F(R.state, "Injured");
    R.stDead = un::F(R.state, "Dead");
    R.stCart = un::F(R.state, "InGolfCart");
    R.stRaft = un::F(R.state, "OnRaft");
    R.stCaptured = un::F(R.state, "Captured");
    R.vsStats = un::F(R.vailStats, "_statsManager");
    R.smStats = un::F(R.statsManager, "_stats");
    R.statCur = un::F(R.stat, "_currentValue");
    R.statMax = un::F(R.stat, "_max");
    R.robbyInjured = un::F(R.robby, "_injuredState");
    R.carryActor = un::F(R.carryBody, "_deadBodyActor");
    R.lpCarry = un::F(R.localPlayer, "_carryBody");
    R.csInstance = un::F(R.cutscenes, "_instance");
    R.csActive = un::F(R.cutscenes, "_activeCutscene");

    R.acRevive = un::M(R.actor, "Revive", 0);
    R.acSetDamage = un::M(R.actor, "SetDamageEnabled", std::vector<std::string>{"System.Boolean"});
    R.acSnap = un::M(R.actor, "SnapToNavMesh", std::vector<std::string>{"UnityEngine.Vector3", "System.Single"});
    R.acRobby = un::M(R.actor, "GetRobbyCallbacks", 0);
    R.acGetAnimator = un::M(R.actor, "GetAnimator", 0);
    R.mgRemove = un::M(R.manager, "RemoveActor", std::vector<std::string>{"Sons.Ai.Vail.VailActor"});
    R.mgFind = un::M(R.manager, "FindActiveActor", std::vector<std::string>{"Sons.Ai.Vail.WorldSimActor"});
    R.mgAnimSpeed = StaticM(R.manager, "GetAnimSpeed", 0);
    R.wsRemove = un::M(R.sim, "RemoveActor", std::vector<std::string>{"Sons.Ai.Vail.WorldSimActor"});
    R.wsSpawn = un::M(R.sim, "SpawnCompanion",
                      std::vector<std::string>{"Sons.Ai.Vail.VailActorTypeId", "UnityEngine.Vector3", "UnityEngine.Quaternion", "System.Single"});
    R.saSetPosition = un::M(R.simActor, "SetPosition", std::vector<std::string>{"UnityEngine.Vector3"});
    R.saSetDestination = un::M(R.simActor, "SetDestination", std::vector<std::string>{"UnityEngine.Vector3", "System.Boolean"});
    R.saGetPosition = un::M(R.simActor, "Position", 0);
    R.saIsLiving = un::M(R.simActor, "IsLiving", 0);
    R.smGetStat = un::M(R.statsManager, "GetStat", std::vector<std::string>{"System.Type"});
    R.statSet = un::M(R.stat, "SetCurrentValue", std::vector<std::string>{"System.Single"});
    R.fxRemove = un::M(R.effects, "Remove", std::vector<std::string>{"Sons.Gameplay.StatusEffectType"});
    R.animGetSpeed = un::M(un::E.animator, "get_speed", 0);
    R.animSetSpeed = un::M(un::E.animator, "set_speed", std::vector<std::string>{"System.Single"});
    R.lpTransform = StaticM(R.localPlayer, "get_Transform", 0);
    R.lpCaves = StaticM(R.localPlayer, "get_IsInCaves", 0);
    R.carrying = un::M(R.carryBody, "IsCarryingBody", 0);
    R.robbyDead = StaticM(R.gameState, "IsRobbyDead", 0);
    R.virginiaDead = StaticM(R.gameState, "IsVirginiaDead", 0);
    R.setRobbyDead = StaticM(R.gameState, "SetIsRobbyDead", 1);
    R.setVirginiaDead = StaticM(R.gameState, "SetIsVirginiaDead", 1);
    R.saveType = StaticM(R.setup, "GetSaveGameType", 0);
    R.gameId = StaticM(R.setup, "GetGameId", 0);
    R.slotForGuid = un::M(R.coop, "GetSaveIdForGuid", std::vector<std::string>{"Sons.Save.SaveGameType", "System.String"});
    R.slotFolder = un::M(R.saves, "GetSaveFolderForId", std::vector<std::string>{"Sons.Save.SaveGameType", "System.Int32"});
    R.boltEntity = un::C("bolt", "", "BoltEntity");
    R.beAttached = un::M(R.boltEntity, "get_isAttached", 0);
    R.reviveEvent = un::C("bolt.user", "", "VailReviveEvent");
    R.revCreate = un::M(R.reviveEvent, "Create", std::vector<std::string>{"Bolt.GlobalTargets", "Bolt.ReliabilityModes"});
    R.revTarget = un::M(R.reviveEvent, "set_Target", 1);
    R.evSend = un::M(R.reviveEvent, "Send", 0);
    Class* targets = un::C("bolt", "Bolt", "GlobalTargets");
    Class* modes = un::C("bolt", "Bolt", "ReliabilityModes");
    R.onlyServer = targets ? (int)il2::EnumValue(targets, "OnlyServer", 6) : 6;
    R.reliable = modes ? (int)il2::EnumValue(modes, "ReliableOrdered", 2) : 2;

    R.robbyType = R.typeIds ? (int)il2::EnumValue(R.typeIds, "Robby", -1) : -1;
    R.virginiaType = R.typeIds ? (int)il2::EnumValue(R.typeIds, "Virginia", -1) : -1;
    R.burning = R.effectTypes ? (int)il2::EnumValue(R.effectTypes, "Burning", -1) : -1;
    R.bleeding = R.effectTypes ? (int)il2::EnumValue(R.effectTypes, "Bleeding", -1) : -1;
    R.poisoned = R.effectTypes ? (int)il2::EnumValue(R.effectTypes, "Poisoned", -1) : -1;
    R.gettingUp = R.injured ? (int)il2::EnumValue(R.injured, "GettingUp", -1) : -1;
    kinds[0].type = R.robbyType;
    kinds[1].type = R.virginiaType;
    LOG("Kelvin and Virginia: type ids %d and %d", R.robbyType, R.virginiaType);

    bool lists = (Inst(R.wsRobby) && Inst(R.wsVirginia)) || Inst(R.wsActors);
    A.find = Glob(R.wsInstance) && lists && Inst(R.saType) && Inst(R.saState) && Inst(R.stId) && Glob(R.stDying) && Glob(R.stDead) &&
             Glob(R.mgInstance) && Inst(R.mgActive) && Inst(R.acId) && Inst(R.acWorld) && Inst(R.acState) && Inst(R.acDead) &&
             R.robbyType >= 0 && R.virginiaType >= 0 && R.robbyType != R.virginiaType;
    A.health = R.health && R.statSet && Inst(R.statCur) && Inst(R.statMax) && Inst(R.saStats) && (Inst(R.smStats) || R.smGetStat);
    A.heal = A.find && A.health;
    A.revive = A.heal && R.acRevive && R.wsSpawn && R.mgRemove && R.wsRemove;
    A.flagKelvin = R.robbyDead && R.setRobbyDead;
    A.flagVirginia = R.virginiaDead && R.setVirginiaDead;
    A.bring = A.find && R.acSnap && R.saSetPosition && R.saSetDestination;
    A.freeze = A.find && R.animSetSpeed && R.animGetSpeed && (Inst(R.acAnimator) || R.acGetAnimator) && R.saSetPosition &&
               R.saSetDestination && (Inst(R.saPosition) || R.saGetPosition);
    A.god = A.heal && R.acSetDamage && Inst(R.acDamage) && R.acRevive;
    A.guestRevive = Glob(R.mgInstance) && Inst(R.mgActive) && Inst(R.acId) && Inst(R.acState) && Inst(R.acDead) && Inst(R.stId) &&
                    Glob(R.stDying) && Glob(R.stDead) && R.robbyType >= 0 && R.virginiaType >= 0 && R.robbyType != R.virginiaType &&
                    R.boltEntity && R.beAttached && R.revCreate && R.revTarget && R.evSend;
    LOG("Kelvin and Virginia: guest revive %d", A.guestRevive ? 1 : 0);

    bool any = false;
    for (const Kind& k : kinds)
        for (const char* id : {k.heal, k.revive, k.bring, k.freeze, k.god}) {
            if (Supported(id)) any = true;
            else LOG("Kelvin and Virginia: %s is not available", id);
        }
    if (!any) err = "Kelvin and Virginia were not found in this version of the game";
    return any;
}

void Tick(const Ctx& c) {
    bool live = c.inWorld && c.player && !c.stopping && c.session != core::Session::Client;
    for (Kind& k : kinds) {
        bool god = live && A.god && c.s.On(k.god);
        bool freeze = live && A.freeze && c.s.On(k.freeze);
        if (!god) ReleaseGuard(k);
        if (!freeze) ReleaseFreeze(k);
        if (!god && !freeze) continue;
        if (SimLoading() || !States(c.now, false)) continue;
        std::vector<Mate> mates;
        if (!Find(k, false, mates)) continue;
        if (god) Guard(k, mates, c);
        if (freeze) {
            if (Cutscene()) ReleaseFreeze(k);
            else Freeze(k, mates);
        }
    }
}

bool Action(const std::string& id, const Ctx& c) {
    if (!Supported(id)) return false;
    bool guest = Guest(c);
    for (Kind& k : kinds) {
        if (id == k.revive) {
            if (guest && A.guestRevive) GuestRevive(k, c);
            else if (!guest && HostRevive(k)) ReviveAction(k, c);
            else Note(guest ? "This only works when you host or play alone" : "That is not available right now");
            return true;
        }
        if (guest && (id == k.heal || id == k.bring)) {
            Note(std::string("Only the host can do that, ") + k.name + " is run by the host's game");
            return true;
        }
        if (id == k.heal) {
            HealAction(k, c);
            return true;
        }
        if (id == k.bring) {
            BringAction(k, c);
            return true;
        }
    }
    return false;
}

void Release(const Ctx&) {
    for (Kind& k : kinds) {
        ReleaseGuard(k);
        ReleaseFreeze(k);
        k.lastRevive = -1000;
        k.spawnedAt = -1000;
    }
}

}

const Group& CompanionsGroup() {
    static Group g{"Kelvin and Virginia", &Resolve, &Supported, &Tick, &Action, &Release, nullptr, nullptr};
    return g;
}

}
