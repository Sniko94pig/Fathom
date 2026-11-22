#include "ingame/groups.h"
#include <windows.h>
#include <cstdint>
#include <cstring>
#include <cwchar>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace ukorbs {

using namespace uk;
using namespace un;

namespace {

struct Game {
    Class *bonus = nullptr, *stats = nullptr, *checkpoint = nullptr, *nm = nullptr, *collider = nullptr;
    Class *tracker = nullptr, *timer = nullptr, *style = nullptr, *saver = nullptr;
    Fld ghost, soul, beenFound, used, number, effect;
    Fld secrets, prev, found, restarts, current, rooms;
    Fld playerCollider, playerType, slot;
    Method *trigger = nullptr, *parent = nullptr, *compareTag = nullptr, *savePath = nullptr;
    int fps = -1;
};

struct Orb {
    Hold go, tr;
    int index = -1;
    bool soul = false;
};

const double kScanEvery = 2.0;
const double kScanGap = 1.0;
const size_t kMaxOrbs = 256;
const int kMaxDepth = 64;
const uint32_t kSecretColor = Rgb(255, 255, 255);
const uint32_t kSoulColor = Rgb(255, 48, 48);

Game G;
bool canEsp = false, canSouls = false, canRooms = false, canCollect = false;

std::vector<Orb> orbs;
Hold stats, point;
int restarts = 0;
double nextScan = 0, lastScan = -1;
bool rescan = false, shown = false;
std::string usedBackup;

bool Bit(Object* o, const Fld& f) { return Val<uint8_t>(o, f) != 0; }

bool Is(Object* o, const char* name) { return o && std::strcmp(mono::ClassName(mono::ClassOf(o)), name) == 0; }

void* Native(Object* o) { return Alive(o) ? mono::Get<void*>(o, R.cachedPtr.off) : nullptr; }

std::string Count(int n, const char* what) { return std::to_string(n) + " " + what + (n == 1 ? "" : "s"); }

bool Ints(Object* list, std::vector<int>& out) {
    if (!list) return true;
    Class* k = mono::ClassOf(list);
    mono::Field* items = mono::FindField(k, "_items");
    mono::Field* size = mono::FindField(k, "_size");
    if (!items || !size) return false;
    Object* arr = mono::Get<Object*>(list, mono::Offset(items));
    int n = mono::Get<int>(list, mono::Offset(size));
    if (!arr || n <= 0) return true;
    if (!Is(arr, "Int32[]")) return false;
    size_t len = mono::Length(arr);
    const int* d = (const int*)mono::Data(arr, (int)sizeof(int));
    for (int i = 0; d && i < n && (size_t)i < len; ++i) out.push_back(d[i]);
    return true;
}

bool Found(Object* sm, std::unordered_set<int>& out) {
    std::vector<int> list;
    bool ok = Ints(Ref(sm, G.prev), list) && Ints(Ref(sm, G.found), list);
    out.insert(list.begin(), list.end());
    return ok;
}

Object* Secrets(Object* sm) {
    Object* arr = Ref(sm, G.secrets);
    return Is(arr, "GameObject[]") ? arr : nullptr;
}

Object* Entry(Object* arr, size_t i) { return i < mono::Length(arr) ? mono::At(arr, i) : nullptr; }

bool Open(Object* b, int index) {
    if (!Alive(b) || Bit(b, G.ghost) || Bit(b, G.beenFound) || Bit(b, G.used)) return false;
    return index < 0 || Val<int>(b, G.number, -1) == index;
}

void Forget() {
    for (Orb& o : orbs) {
        o.go.reset();
        o.tr.reset();
    }
    orbs.clear();
}

void Drop() {
    Forget();
    stats.reset();
    point.reset();
    restarts = 0;
    nextScan = 0;
    lastScan = -1;
    rescan = false;
    if (shown) Mark("orbs", {});
    shown = false;
}

void Keep(Object* go, int index, bool soul) {
    if (orbs.size() >= kMaxOrbs) return;
    Object* tr = TransformOf(go);
    if (!tr) return;
    Orb o;
    o.go.set(go);
    o.tr.set(tr);
    o.index = index;
    o.soul = soul;
    orbs.push_back(std::move(o));
}

void Rooms(std::unordered_set<void*>& out) {
    for (Object* cp : FindAll(G.checkpoint, true))
        for (Object* room : ListItems(Ref(cp, G.rooms)))
            if (void* p = Native(TransformOf(room))) out.insert(p);
}

bool Inside(Object* go, const std::unordered_set<void*>& rooms) {
    if (rooms.empty()) return false;
    Object* t = TransformOf(go);
    for (int d = 0; t && d < kMaxDepth; ++d) {
        if (rooms.count(Native(t))) return true;
        t = Call(G.parent, t);
    }
    return false;
}

void Scan(const Ctx& c) {
    Forget();
    lastScan = c.now;
    nextScan = c.now + kScanEvery;
    rescan = false;
    Object* sm = Single(G.stats);
    stats.set(sm);
    point.set(Ref(sm, G.current));
    restarts = Val<int>(sm, G.restarts, 0);
    std::unordered_set<void*> listed;
    std::unordered_set<int> found;
    Found(sm, found);
    Object* arr = Secrets(sm);
    size_t n = mono::Length(arr);
    for (size_t i = 0; i < n; ++i) {
        Object* go = mono::At(arr, i);
        if (!Alive(go)) continue;
        listed.insert(Native(go));
        if (found.count((int)i)) continue;
        Object* b = Comp(go, G.bonus);
        if (Open(b, (int)i)) Keep(go, (int)i, Bit(b, G.soul));
    }
    if (!canSouls) return;
    std::unordered_set<void*> rooms;
    bool roomsRead = false;
    for (Object* b : FindAll(G.bonus, true)) {
        if (!Bit(b, G.soul) || !Open(b, -1)) continue;
        Object* go = GameObjectOf(b);
        if (!go || listed.count(Native(go))) continue;
        if (!ActiveInHierarchy(go)) {
            if (!canRooms) continue;
            if (!roomsRead) {
                Rooms(rooms);
                roomsRead = true;
            }
            if (Inside(go, rooms)) continue;
        }
        Keep(go, -1, true);
    }
}

bool Stale(const Ctx& c) {
    if (c.now >= nextScan) return true;
    if (c.now < lastScan + kScanGap) return false;
    if (rescan) return true;
    if (!stats.held()) return false;
    Object* sm = stats.get();
    return !Alive(sm) || Val<int>(sm, G.restarts, 0) != restarts || Ref(sm, G.current) != point.get();
}

void Draw(const Ctx& c) {
    std::vector<Marker> list;
    Object* cam = orbs.empty() ? nullptr : MainCamera();
    if (cam) {
        Object* arr = Secrets(stats.get());
        Vec3 me = Position(c.player);
        for (const Orb& o : orbs) {
            Object* go = o.go.get();
            Object* tr = o.tr.get();
            if (!Alive(go) || !Alive(tr)) continue;
            if (o.index >= 0 && Entry(arr, (size_t)o.index) != go) {
                rescan = true;
                continue;
            }
            Vec3 p = Position(tr);
            Vec2 v;
            if (!Viewport(p, v, cam)) continue;
            list.push_back({v.x, v.y, o.soul ? "Soul orb" : "Secret orb", o.soul ? kSoulColor : kSecretColor, Dist(p, me)});
        }
    }
    if (list.empty() && !shown) return;
    shown = !list.empty();
    Mark("orbs", std::move(list));
}

bool Platformer() {
    if (!G.tracker || !G.playerType || G.fps < 0) return false;
    Object* t = Single(G.tracker);
    return t && Val<int>(t, G.playerType, G.fps) != G.fps;
}

Object* PlayerCollider(Object* nm) {
    Object* col = Ref(nm, G.playerCollider);
    if (!Alive(col)) col = Comp(nm, G.collider);
    if (!Alive(col) || !mono::IsSubclass(mono::ClassOf(col), G.collider)) return nullptr;
    return CallBool(G.compareTag, col, {mono::NewString("Player")}) ? col : nullptr;
}

bool Ready() { return Single(G.timer) && Single(G.style) && Single(G.tracker) && Single(R.cameraController); }

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

std::wstring Clean(std::wstring p) {
    for (wchar_t& ch : p)
        if (ch == L'/') ch = L'\\';
    while (!p.empty() && p.back() == L'\\') p.pop_back();
    return p;
}

bool SlotPathOk(const std::string& path, int slot) {
    std::wstring dir = Clean(uk::Folder());
    size_t cut = dir.find_last_of(L'\\');
    if (cut == std::wstring::npos) return false;
    std::wstring want = dir.substr(0, cut) + L"\\Saves\\Slot" + std::to_wstring(slot + 1);
    std::wstring got = Clean(Widen(path));
    return !got.empty() && _wcsicmp(want.c_str(), got.c_str()) == 0;
}

std::string Short(const std::string& backup) {
    std::wstring p = Clean(Widen(backup));
    size_t cut = p.find_last_of(L'\\');
    std::wstring leaf = cut == std::wstring::npos ? p : p.substr(cut + 1);
    return "fathom\\save_backup\\" + Narrow(leaf);
}

void Collect(const Ctx& c) {
    if (!c.inLevel || !Alive(c.player)) {
        c.Note(Platformer() ? "Not available in the platformer part" : "Start a level first");
        return;
    }
    if (c.dead) {
        c.Note("Respawn first");
        return;
    }
    if (c.levelOver) {
        c.Note("The level is already over");
        return;
    }
    if (c.paused) {
        c.Note("Unpause the game first");
        return;
    }
    if (Platformer()) {
        c.Note("Not available in the platformer part");
        return;
    }
    Object* sm = Single(G.stats);
    Object* arr = Secrets(sm);
    size_t n = mono::Length(arr);
    if (n == 0) {
        c.Note("This level has no secret orbs");
        return;
    }
    std::unordered_set<int> found;
    if (!Found(sm, found)) {
        LOG("orbs: collect refused, the found secrets of this level could not be read");
        c.Note("Could not read the secrets of this level, nothing was changed");
        return;
    }
    std::vector<int> todo;
    int souls = 0;
    for (size_t i = 0; i < n; ++i) {
        Object* go = mono::At(arr, i);
        if (!Alive(go) || found.count((int)i)) continue;
        Object* b = Comp(go, G.bonus);
        if (!Open(b, (int)i)) continue;
        if (Bit(b, G.soul)) ++souls;
        else todo.push_back((int)i);
    }
    std::string left = souls > 0 ? ", " + Count(souls, "red soul orb") + " left for you" : std::string();
    if (todo.empty()) {
        c.Note("No secret orbs left to collect" + left);
        return;
    }
    Object* col = PlayerCollider(c.player);
    if (!col || !Ready()) {
        LOG("orbs: collect refused, %s", col ? "a part of the game the pickup needs is missing" : "the player collider was not found");
        c.Note("Could not collect the orbs right now, nothing was changed");
        return;
    }
    bool ok = false;
    int slot = StaticVal<int>(G.slot, -1);
    std::string path = mono::Str(Call(G.savePath, nullptr, {}, &ok));
    if (!ok || slot < 0 || slot > 4 || !SlotPathOk(path, slot)) {
        LOG("orbs: collect refused, the save folder is \"%s\" (slot %d)", path.c_str(), slot + 1);
        c.Note("Your save folder is not where Fathom expects it, nothing was changed");
        return;
    }
    std::string backup = uk::LastBackup();
    if (backup.empty() || backup == usedBackup) backup = uk::Backup();
    if (backup.empty()) {
        c.Note("Save backup failed, nothing was changed");
        return;
    }
    usedBackup = backup;
    int done = 0;
    for (int i : todo) {
        Object* go = Entry(Secrets(sm), (size_t)i);
        Object* b = Alive(go) ? Comp(go, G.bonus) : nullptr;
        if (!Open(b, i) || Bit(b, G.soul)) continue;
        if (!Alive(Ref(b, G.effect))) {
            LOG("orbs: secret %d has no pickup effect, left alone", i);
            continue;
        }
        bool called = false;
        Call(G.trigger, b, {col}, &called);
        Object* now = Secrets(sm);
        if (now && !Entry(now, (size_t)i)) {
            ++done;
            continue;
        }
        LOG("orbs: secret %d could not be collected%s, stopped", i, called ? "" : " (the game threw an error)");
        break;
    }
    int missed = (int)todo.size() - done;
    LOG("orbs: collected %d of %d secret orbs in %s", done, (int)todo.size(), mono::Str(Static(R.shCurrent)).c_str());
    std::string msg = done > 0 ? "Collected " + Count(done, "secret orb") : std::string("No secret orb could be collected");
    if (done > 0 && missed > 0) msg += ", " + std::to_string(missed) + " could not be collected";
    if (done > 0) msg += left;
    c.Note(msg + ". Backup: " + Short(backup));
}

}

bool Resolve() {
    G = Game{};
    G.bonus = Cls("Bonus");
    G.ghost = F(G.bonus, "ghost");
    G.soul = F(G.bonus, "superCharge");
    G.beenFound = F(G.bonus, "beenFound");
    G.used = F(G.bonus, "activated");
    G.number = F(G.bonus, "secretNumber");
    G.effect = F(G.bonus, "breakEffect");
    G.trigger = MT(G.bonus, "OnTriggerEnter", {"UnityEngine.Collider"});

    G.stats = Cls("StatsManager");
    G.secrets = F(G.stats, "secretObjects");
    G.prev = F(G.stats, "prevSecrets");
    G.found = F(G.stats, "newSecrets");
    G.restarts = F(G.stats, "restarts");
    G.current = F(G.stats, "currentCheckPoint");

    G.checkpoint = Cls("CheckPoint");
    G.rooms = F(G.checkpoint, "defaultRooms");
    G.parent = M(Cls("UnityEngine.Transform"), "get_parent", 0);

    G.nm = Cls("NewMovement");
    G.playerCollider = F(G.nm, "playerCollider");
    G.collider = Cls("UnityEngine.Collider");
    G.compareTag = MT(Cls("UnityEngine.Component"), "CompareTag", {"System.String"});
    G.tracker = Cls("PlayerTracker");
    G.playerType = F(G.tracker, "playerType");
    G.fps = Enum(Cls("PlayerType"), "FPS");
    G.timer = Cls("TimeController");
    G.style = Cls("StyleHUD");

    G.saver = Cls("GameProgressSaver");
    G.savePath = M(G.saver, "get_SavePath", 0);
    G.slot = F(G.saver, "currentSlot");

    bool lists = G.prev && G.found;
    bool engine = R.cachedPtr && R.goGetComponent && R.goTransform;
    bool marks = engine && R.trGetPosition && R.camMain && R.camToViewport;
    canEsp = G.bonus && G.stats && G.secrets && G.ghost && G.soul && G.number && marks;
    canSouls = canEsp && R.findObjectsOfTypeInactive && R.compGameObject && R.goActiveInHierarchy;
    canRooms = G.checkpoint && G.rooms && G.parent;
    canCollect = engine && G.bonus && G.stats && G.secrets && lists && G.ghost && G.soul && G.number && G.effect && G.trigger && G.nm &&
                 G.collider && G.compareTag && G.timer && G.style && G.tracker && R.cameraController && G.savePath && G.slot;

    if (canEsp && !lists) LOG("orbs: orb ESP works, but orbs found before a checkpoint restart may still show");
    if (canSouls && !canRooms) LOG("orbs: orb ESP shows red soul orbs only in parts of the level that are active");
    LOG("orbs: orb ESP %s, red soul orbs %s, collect all orbs %s", canEsp ? "ready" : "off", canSouls ? "ready" : "off",
        canCollect ? "ready" : "off");
    return canEsp || canCollect;
}

bool Supported(const std::string& id) {
    if (id == "orb_esp") return canEsp;
    if (id == "collect_all_orbs") return canCollect;
    return false;
}

void Tick(const Ctx& c) {
    bool want = canEsp && c.on("orb_esp") && c.inLevel && Alive(c.player) && !c.dead && !c.paused && !c.levelOver;
    if (!want) {
        if (shown || stats.held() || point.held() || !orbs.empty()) Drop();
        return;
    }
    if (Stale(c)) Scan(c);
    Draw(c);
}

void Release(const Ctx&) {
    Drop();
    Mark("orbs", {});
}

void Action(const std::string& id, const Ctx& c) {
    if (id == "collect_all_orbs" && canCollect) Collect(c);
}

bool Busy() { return false; }

}
