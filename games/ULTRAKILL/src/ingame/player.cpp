#include "ingame/groups.h"
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace ukplayer {

using namespace uk;
using namespace un;

namespace {

struct Game {
    Class *nm = nullptr, *inv = nullptr, *zone = nullptr, *stats = nullptr, *checkpoint = nullptr, *door = nullptr;
    Class *grenade = nullptr, *portal = nullptr, *hud = nullptr, *tracker = nullptr, *tc = nullptr, *plat = nullptr;
    Fld hp, antiHp, antiCooldown, difficulty, restarts, rb, rocket;
    Method *resetHard = nullptr, *fullHeal = nullptr;
    Fld invLast, invActive;
    Method *invCtor = nullptr, *invEnable = nullptr, *invDisable = nullptr;
    Fld zoneSafe, zoneAffected, zoneTarget, zoneStarted;
    int enemiesOnly = -1;
    Fld statsCheckpoint, statsSpawn, cpActivate, cpDoors;
    Method *doorUnlock = nullptr, *rideEnd = nullptr, *traveller = nullptr, *hudMessage = nullptr;
    Fld playerType, pmov;
    int fps = -1, platformer = -1;
    Fld platDead;
    Method *platRespawn = nullptr, *platSnap = nullptr, *activeSelf = nullptr;
    Fld timeScale, timeModifier, slowDown;
};

struct Zone {
    Hold h;
    int id = 0;
    bool was = true;
    bool woke = false;
};

const double kScanEvery = 0.25;
const size_t kMaxFlips = 3;
const float kLift = 1.25f;
const double kCatchGap = 0.5;

Game G;
bool canHealth = false, canGod = false, canRescue = false, canRefill = false, canSpeed = false, canPlat = false, canCatch = false;

Hold mine, prev;
bool flagOn = false;

std::vector<Zone> zones;
double nextScan = 0;
int scanRestarts = -1;
int scanPlayer = 0;

int platId = 0;
bool platDead = false;
double lastCatch = -1e9;

int tcId = 0;
bool speedOn = false;
bool haveMod = false;
float lastMod = 1.0f, gameMod = 1.0f, lastT = 1.0f;

bool IsFps() {
    if (!G.tracker || !G.playerType || G.fps < 0) return true;
    Object* t = Single(G.tracker);
    return !t || Val<int>(t, G.playerType, G.fps) == G.fps;
}

int Cap(Object* nm) {
    int d = Val<int>(nm, G.difficulty, 2);
    int r = Val<int>(nm, G.restarts, 0);
    return d == 0 || (d == 1 && r > 2) ? 200 : 100;
}

void KeepFull(Object* nm) {
    if (Val<float>(nm, G.antiHp) > 0.0f) SetVal<float>(nm, G.antiHp, 0.0f);
    if (Val<float>(nm, G.antiCooldown) > 0.0f) SetVal<float>(nm, G.antiCooldown, 0.0f);
    int cap = Cap(nm);
    if (Val<int>(nm, G.hp, cap) < cap) SetVal<int>(nm, G.hp, cap);
}

void FlagOn() {
    Object* me = mine.get();
    if (!me) {
        me = mono::New(G.inv);
        if (!me) return;
        if (G.invCtor) Call(G.invCtor, me);
        mine.set(me);
    }
    Object* cur = Static(G.invLast);
    bool active = !G.invActive || Val<uint8_t>(me, G.invActive) != 0;
    if (flagOn && cur == me && active) return;
    if (cur != me) prev.set(cur);
    bool ok = false;
    Call(G.invEnable, me, {nullptr}, &ok);
    if (ok) flagOn = true;
}

void FlagOff() {
    if (flagOn) {
        Object* me = mine.get();
        if (me) {
            if (Static(G.invLast) == me) SetStatic(G.invLast, prev.get());
            Call(G.invDisable, me);
        }
        flagOn = false;
    }
    prev.reset();
}

void Restore(Zone& z) {
    Object* o = z.h.get();
    if (z.woke && Alive(o) && Enabled(o)) SetEnabled(o, false);
    z.h.reset();
}

void DropZones() {
    for (Zone& z : zones) Restore(z);
    zones.clear();
    nextScan = 0;
    scanRestarts = -1;
    scanPlayer = 0;
}

bool Lethal(Object* o) {
    if (Val<uint8_t>(o, G.zoneSafe) != 0) return false;
    return G.enemiesOnly < 0 || Val<int>(o, G.zoneAffected, 0) != G.enemiesOnly;
}

bool Started(Object* o) { return G.zoneStarted && Val<uint8_t>(o, G.zoneStarted) != 0; }

void Scan() {
    std::unordered_map<int, size_t> index;
    for (size_t i = 0; i < zones.size(); ++i) index[zones[i].id] = i;
    std::vector<Zone> keep;
    for (Object* o : FindAll(G.zone)) {
        if (!Lethal(o)) continue;
        int id = InstanceId(o);
        if (!id) continue;
        auto it = index.find(id);
        if (it != index.end()) {
            if (zones[it->second].h.held()) keep.push_back(std::move(zones[it->second]));
            continue;
        }
        Zone z;
        z.id = id;
        z.h.set(o);
        bool en = Enabled(o);
        if (!en && Started(o)) {
            SetEnabled(o, true);
            en = Enabled(o);
            z.woke = en;
        }
        z.was = en;
        keep.push_back(std::move(z));
    }
    for (Zone& z : zones)
        if (z.h.held()) Restore(z);
    zones = std::move(keep);
}

void Home(Object* body, Object* stats) {
    Object* cp = stats ? Ref(stats, G.statsCheckpoint) : nullptr;
    if (Alive(cp)) {
        SetPosition(body, Add(Position(cp), Vec3{0.0f, kLift, 0.0f}));
        Object* act = Ref(cp, G.cpActivate);
        if (Alive(act)) SetActive(act, true);
        Object* doors = Ref(cp, G.cpDoors);
        size_t n = mono::Length(doors);
        for (size_t i = 0; i < n && G.doorUnlock; ++i) {
            Object* d = mono::At(doors, i);
            if (Alive(d)) Call(G.doorUnlock, d);
        }
    } else if (stats) {
        SetPosition(body, Val<Vec3>(stats, G.statsSpawn));
    }
}

void Whoops() {
    Object* hud = G.hudMessage ? Single(G.hud) : nullptr;
    if (!hud) return;
    int delay = 0;
    bool no = false, yes = true;
    Call(G.hudMessage, hud, {mono::NewString("Whoops, sorry about that."), mono::NewString(""), mono::NewString(""), &delay, &no, &no, &yes});
}

void Rescue(Object* nm, Object* zone) {
    Object* rb = Ref(nm, G.rb);
    if (Alive(rb)) SetVelocity(rb, Vec3{});
    Object* rocket = Ref(nm, G.rocket);
    if (Alive(rocket) && G.rideEnd) Call(G.rideEnd, rocket);
    Vec3 target = Val<Vec3>(zone, G.zoneTarget);
    Object* stats = Single(G.stats);
    if (target.x != 0.0f || target.y != 0.0f || target.z != 0.0f) SetPosition(nm, Add(target, Vec3{0.0f, kLift, 0.0f}));
    else Home(nm, stats);
    Object* portal = G.traveller ? Single(G.portal) : nullptr;
    if (portal) Call(G.traveller, portal, {nm});
    Whoops();
    LOG("player: god mode caught an instant-kill trap and put you back");
}

void Watch(const Ctx& c) {
    std::vector<Zone> keep;
    keep.reserve(zones.size());
    std::vector<size_t> hit;
    for (Zone& z : zones) {
        Object* o = z.h.get();
        if (!Alive(o)) {
            z.h.reset();
            continue;
        }
        bool en = Enabled(o);
        if (z.was && !en && (!G.zoneStarted || Started(o))) hit.push_back(keep.size());
        z.was = en;
        keep.push_back(std::move(z));
    }
    zones = std::move(keep);
    if (hit.empty()) return;
    bool many = hit.size() > kMaxFlips;
    if (!many && !c.levelOver && !c.paused && !c.on("fly") && !c.on("noclip")) Rescue(c.player, zones[hit.front()].h.get());
    for (size_t i : hit) {
        Object* o = zones[i].h.get();
        SetEnabled(o, true);
        bool en = Enabled(o);
        if (many && en) zones[i].woke = true;
        zones[i].was = en;
    }
}

void Zones(const Ctx& c) {
    int restarts = Val<int>(c.player, G.restarts, 0);
    if (c.now >= nextScan || restarts != scanRestarts || c.playerId != scanPlayer) {
        Scan();
        nextScan = c.now + kScanEvery;
        scanRestarts = restarts;
        scanPlayer = c.playerId;
    }
    Watch(c);
}

Object* Platformer(const Ctx& c) {
    if (c.inLevel || c.loading || c.scene != Scene::Other) return nullptr;
    Object* t = Single(G.tracker);
    if (!t || Val<int>(t, G.playerType, G.fps) != G.platformer) return nullptr;
    Object* p = Ref(t, G.pmov);
    return Alive(p) ? p : nullptr;
}

void Unwatch() {
    platId = 0;
    platDead = false;
}

bool Revive(Object* pm, const Ctx& c) {
    Object* stats = Single(G.stats);
    Object* go = GameObjectOf(pm);
    if (!stats || !go || c.now - lastCatch < kCatchGap) return false;
    bool self = CallBool(G.activeSelf, go, {}, true);
    if (!self) SetActive(go, true);
    if (!ActiveInHierarchy(go)) {
        if (!self) SetActive(go, false);
        return false;
    }
    bool ok = false;
    Call(G.platRespawn, pm, {}, &ok);
    if (!ok || Val<uint8_t>(pm, G.platDead) != 0) return false;
    lastCatch = c.now;
    Home(pm, stats);
    if (G.platSnap) Call(G.platSnap, pm);
    Whoops();
    LOG("player: god mode caught a deadly fall in a platformer part and put you back");
    return true;
}

bool Catch(Object* pm, const Ctx& c) {
    int id = InstanceId(pm);
    bool dead = Val<uint8_t>(pm, G.platDead) != 0;
    if (id != platId) {
        platId = id;
        platDead = dead;
        return !dead;
    }
    if (dead && !platDead && canCatch && Revive(pm, c)) dead = false;
    platDead = dead;
    return !dead;
}

float Wanted(const Ctx& c) {
    if (!c.inLevel) return 1.0f;
    float m = c.value("game_speed", 1.0f);
    if (!(m >= 0.05f && m <= 10.0f)) return 1.0f;
    return std::fabs(m - 1.0f) <= 0.01f ? 1.0f : m;
}

void SpeedApply(Object* tc, float mult, bool paused) {
    float base = Val<float>(tc, G.timeScale, 1.0f);
    float cur = Val<float>(tc, G.timeModifier, 1.0f);
    if (!haveMod || cur != lastMod) gameMod = cur;
    float target = gameMod * mult;
    float ts = TimeScale();
    if (cur != target) {
        SetVal<float>(tc, G.timeModifier, target);
        if (!paused && std::fabs(ts - base * cur) < 1e-6f) {
            ts = base * target;
            SetTimeScale(ts);
        }
    }
    lastMod = target;
    haveMod = true;
    float want = base * target;
    if (G.slowDown) {
        float sd = Val<float>(tc, G.slowDown, want);
        if (sd < lastT - 1e-4f) {
            bool pastNormal = want > base + 1e-4f && sd >= base - 1e-4f;
            if (sd > want || pastNormal) {
                SetVal<float>(tc, G.slowDown, want);
                if (!paused && ts == sd) {
                    ts = want;
                    SetTimeScale(ts);
                }
            }
        } else {
            float least = mult == 1.0f && want < base ? base : want;
            if (sd < least) SetVal<float>(tc, G.slowDown, least);
        }
    }
    lastT = want;
    if (mult != 1.0f && !paused && ts == base * gameMod && ts != want) SetTimeScale(want);
}

void SpeedTick(const Ctx& c) {
    if (!canSpeed) return;
    float m = Wanted(c);
    if (m == 1.0f && !speedOn) return;
    Object* tc = Single(G.tc);
    if (!tc) {
        speedOn = false;
        tcId = 0;
        haveMod = false;
        return;
    }
    int id = InstanceId(tc);
    if (id != tcId) {
        tcId = id;
        haveMod = false;
        lastT = Val<float>(tc, G.timeScale, 1.0f) * Val<float>(tc, G.timeModifier, 1.0f);
        if (speedOn && m == 1.0f) {
            speedOn = false;
            return;
        }
    }
    SpeedApply(tc, m, c.paused);
    speedOn = m != 1.0f;
}

void SpeedRelease(const Ctx& c) {
    if (speedOn && canSpeed) {
        Object* tc = Single(G.tc);
        if (tc && InstanceId(tc) == tcId) SpeedApply(tc, 1.0f, c.paused);
    }
    speedOn = false;
    tcId = 0;
    haveMod = false;
    gameMod = 1.0f;
    lastT = 1.0f;
}

}

bool Resolve() {
    G = Game{};
    G.nm = Cls("NewMovement");
    G.hp = F(G.nm, "hp");
    G.antiHp = F(G.nm, "antiHp");
    G.antiCooldown = F(G.nm, "antiHpCooldown");
    G.difficulty = F(G.nm, "difficulty");
    G.restarts = F(G.nm, "sameCheckpointRestarts");
    G.rb = F(G.nm, "rb");
    G.rocket = F(G.nm, "ridingRocket");
    G.resetHard = M(G.nm, "ResetHardDamage", 0);
    G.fullHeal = MT(G.nm, "FullHeal", {"System.Boolean"});

    G.inv = Cls("ULTRAKILL.Cheats.Invincibility");
    G.invLast = F(G.inv, "_lastInstance");
    G.invActive = F(G.inv, "<IsActive>k__BackingField");
    G.invCtor = M(G.inv, ".ctor", 0);
    G.invEnable = M(G.inv, "Enable", 1);
    G.invDisable = M(G.inv, "Disable", 0);

    G.tracker = Cls("PlayerTracker");
    G.playerType = F(G.tracker, "playerType");
    G.pmov = F(G.tracker, "pmov");
    Class* type = Cls("PlayerType");
    G.fps = Enum(type, "FPS");
    G.platformer = Enum(type, "Platformer");
    G.plat = Cls("PlatformerMovement");
    G.platDead = F(G.plat, "dead");
    G.platRespawn = M(G.plat, "Respawn", 0);
    G.platSnap = M(G.plat, "SnapCamera", 0);
    G.activeSelf = M(R.gameObject, "get_activeSelf", 0);

    G.zone = Cls("DeathZone");
    G.zoneSafe = F(G.zone, "notInstakill");
    G.zoneAffected = F(G.zone, "affected");
    G.zoneTarget = F(G.zone, "respawnTarget");
    G.zoneStarted = F(G.zone, "playerAffected");
    G.enemiesOnly = Enum(Cls("AffectedSubjects"), "EnemiesOnly");
    G.stats = Cls("StatsManager");
    G.statsCheckpoint = F(G.stats, "currentCheckPoint");
    G.statsSpawn = F(G.stats, "spawnPos");
    G.checkpoint = Cls("CheckPoint");
    G.cpActivate = F(G.checkpoint, "toActivate");
    G.cpDoors = F(G.checkpoint, "doorsToUnlock");
    G.door = Cls("Door");
    G.doorUnlock = M(G.door, "Unlock", 0);
    G.grenade = Cls("Grenade");
    G.rideEnd = M(G.grenade, "PlayerRideEnd", 0);
    G.portal = Cls("Assembly-CSharp", "ULTRAKILL.Portal.PortalManagerV2");
    G.traveller = M(G.portal, "UpdateTraveller", 1);
    G.hud = Cls("HudMessageReceiver");
    G.hudMessage = M(G.hud, "SendHudMessage", 7);

    G.tc = Cls("TimeController");
    G.timeScale = F(G.tc, "timeScale");
    G.timeModifier = F(G.tc, "timeScaleModifier");
    G.slowDown = F(G.tc, "slowDown");

    canHealth = G.nm && G.hp && G.antiHp;
    canGod = canHealth && G.inv && G.invLast && G.invEnable && G.invDisable;
    canRescue = canGod && G.zone && G.zoneSafe && G.stats && G.statsCheckpoint && G.statsSpawn && G.rb && R.findObjectsOfType &&
                R.getEnabled && R.setEnabled && R.trSetPosition;
    canPlat = canGod && G.tracker && G.playerType && G.pmov && G.fps >= 0 && G.platformer >= 0;
    canCatch = canPlat && G.plat && G.platDead && G.platRespawn && G.activeSelf && G.stats && G.statsCheckpoint && G.statsSpawn &&
               R.goSetActive && R.goActiveInHierarchy && R.compGameObject && R.trSetPosition;
    canRefill = G.nm && G.resetHard && G.fullHeal;
    canSpeed = G.tc && G.timeScale && G.timeModifier && R.getTimeScale && R.setTimeScale;

    if (!canHealth) LOG("player: unlimited health is off (health fields not found)");
    if (!canGod) LOG("player: god mode is off (the game's invincibility flag was not found)");
    else if (!canRescue) LOG("player: god mode works, but instant-kill traps will not put you back at the checkpoint");
    if (canGod && !canPlat) LOG("player: god mode does not cover the platformer parts");
    else if (canPlat && !canCatch) LOG("player: god mode covers the platformer parts, but falls and traps there still kill");
    if (!canRefill) LOG("player: refill health is off (heal methods not found)");
    if (!canSpeed) LOG("player: game speed is off (TimeController fields not found)");
    else if (!G.slowDown) LOG("player: game speed works, but slow-motion recoveries may change the sound pitch");
    return canHealth || canGod || canRefill || canSpeed;
}

bool Supported(const std::string& id) {
    if (id == "godmode") return canGod;
    if (id == "unlimited_health") return canHealth;
    if (id == "refill_health") return canRefill;
    if (id == "game_speed") return canSpeed;
    return false;
}

void Tick(const Ctx& c) {
    bool live = c.inLevel && Alive(c.player) && !c.dead;
    bool god = canGod && live && c.on("godmode");
    bool health = canHealth && live && c.on("unlimited_health");
    bool fps = (god || health) && IsFps();
    Object* plat = canPlat && c.on("godmode") ? Platformer(c) : nullptr;
    bool platLive = plat && Catch(plat, c);
    if (!plat) Unwatch();
    if ((god && fps) || platLive) FlagOn();
    else FlagOff();
    if ((god || health) && fps) KeepFull(c.player);
    if (god && fps && canRescue) Zones(c);
    else if (!zones.empty()) DropZones();
    SpeedTick(c);
}

void Release(const Ctx& c) {
    FlagOff();
    DropZones();
    Unwatch();
    SpeedRelease(c);
    if (c.stopping) mine.reset();
}

void Action(const std::string& id, const Ctx& c) {
    if (id != "refill_health" || !canRefill) return;
    if (!c.inLevel || !Alive(c.player)) {
        c.Note("Start a level first");
        return;
    }
    if (c.dead) {
        c.Note("Respawn first");
        return;
    }
    Call(G.resetHard, c.player);
    bool silent = true;
    Call(G.fullHeal, c.player, {&silent});
}

bool Busy() { return flagOn || speedOn || !zones.empty(); }

bool Outside() { return true; }

}
