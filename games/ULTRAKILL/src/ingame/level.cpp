#include "ingame/groups.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uklevel {

using namespace uk;
using namespace un;

namespace {

const std::string kKill = "kill_enemies_in_range";
const std::string kRange = "kill_range";
const std::string kShower = "challenge_shower";

const double kApplyEvery = 0.1;
const double kScanEvery = 2.0;
const double kAnnounceEvery = 0.5;
const size_t kMaxText = 200;

struct Game {
    Class *tracker = nullptr, *eid = nullptr, *player = nullptr;
    Method *current = nullptr, *instaKill = nullptr;
    Fld dead, center, playerType, target;
    int platformer = -1;
    Class *panel = nullptr, *hover = nullptr, *tmp = nullptr, *uiText = nullptr;
    Fld level, progress, forceOff, icon, hoverPanel, smLevel;
    Method *find = nullptr, *activeSelf = nullptr, *tmpText = nullptr, *uiGetText = nullptr, *isCustom = nullptr, *sceneName = nullptr;
};

struct Panel {
    Hold panel, trigger, desc;
    int level = 0;
    bool mine = false;
    bool descWas = false;
    bool told = false;
};

enum State { S_LOCKED, S_OPEN, S_BEATEN };

Game G;
bool canKill = false, canPlatformer = false, canShower = false, canHover = false, canRead = false, canText = false;

std::vector<Panel> panels;
std::unordered_map<int, std::string> texts;
size_t toldTexts = 0;
bool scanned = false;
double nextScan = 0, nextApply = 0, nextAnnounce = 0;
bool announce = true;
int announcedRun = 0;

bool Finite(Vec3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

float Range(const Ctx& c) {
    float r = c.value(kRange, 50.0f);
    return std::isfinite(r) ? std::clamp(r, 5.0f, 1000.0f) : 50.0f;
}

Object* Platformer() {
    if (!canPlatformer) return nullptr;
    Object* pt = Single(G.player);
    if (!pt || Val<int>(pt, G.playerType, -1) != G.platformer) return nullptr;
    Object* t = Ref(pt, G.target);
    return Alive(t) ? t : nullptr;
}

bool From(const Ctx& c, Vec3& out) {
    Object* who = nullptr;
    if (c.inLevel && Alive(c.player)) who = c.player;
    else if (c.scene == Scene::Other && !c.loading && !c.mainMenu) who = Platformer();
    if (!who) return false;
    out = Position(who);
    return Finite(out);
}

bool Dead(Object* e) { return Val<uint8_t>(e, G.dead) != 0; }

bool Within(Object* o, Vec3 from, float range) {
    Vec3 p = Position(o);
    if (!Finite(p)) return false;
    Vec3 d = Sub(p, from);
    return Dot(d, d) <= range * range;
}

bool Near(Object* e, Vec3 from, float range) {
    if (Within(e, from, range)) return true;
    Object* center = G.center ? Ref(e, G.center) : nullptr;
    return Alive(center) && Within(center, from, range);
}

void Kill(const Ctx& c) {
    Vec3 from;
    if (!From(c, from)) {
        c.Note("Start a level first");
        return;
    }
    float range = Range(c);
    Object* tracker = Single(G.tracker);
    if (!tracker) {
        c.Note("No enemies in range");
        return;
    }
    bool ok = false;
    Object* list = Call(G.current, tracker, {}, &ok);
    if (!ok || !list) {
        c.Note("Could not read the enemy list, nothing was killed");
        return;
    }
    Hold keep;
    keep.set(list);
    std::vector<Object*> hit;
    for (Object* e : ListItems(list))
        if (Alive(e) && !Dead(e) && Near(e, from, range)) hit.push_back(e);
    for (Object* e : hit)
        if (!Dead(e) && Alive(e)) Call(G.instaKill, e);
    size_t killed = 0;
    for (Object* e : hit)
        if (Dead(e)) ++killed;
    keep.reset();
    LOG("level: killed %d of %d enemies within %.0f m", (int)killed, (int)hit.size(), range);
    if (hit.empty()) c.Note("No enemies in range");
    else if (killed == hit.size()) c.Note("Killed " + std::to_string(killed) + (killed == 1 ? " enemy" : " enemies"));
    else c.Note("Killed " + std::to_string(killed) + " of " + std::to_string(hit.size()) + " enemies in range");
}

std::string Clean(const std::string& raw) {
    std::string out;
    out.reserve(raw.size());
    bool space = false;
    auto put = [&](char ch) {
        if (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t') {
            space = !out.empty();
            return;
        }
        if (space) out += ' ';
        space = false;
        out += ch;
    };
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i];
        if (ch == '<') {
            size_t end = raw.find('>', i + 1);
            size_t at = i + 1 < raw.size() && raw[i + 1] == '/' ? i + 2 : i + 1;
            bool tag = end != std::string::npos && end - i <= 64 && at < end && raw.find('<', i + 1) > end &&
                       (std::isalpha((unsigned char)raw[at]) || raw[at] == '#');
            if (tag) {
                bool br = end - at >= 2 && std::tolower((unsigned char)raw[at]) == 'b' && std::tolower((unsigned char)raw[at + 1]) == 'r' &&
                          (end == at + 2 || raw[at + 2] == ' ' || raw[at + 2] == '/');
                if (br) put(' ');
                i = end;
                continue;
            }
        }
        if (ch == '\\' && i + 1 < raw.size() && (raw[i + 1] == 'n' || raw[i + 1] == 'r' || raw[i + 1] == 't')) {
            put(' ');
            ++i;
            continue;
        }
        put(ch);
    }
    return out;
}

size_t Title(const std::string& s) {
    const char* word = "CHALLENGE";
    size_t i = 0, k = 0;
    bool spaced = false, caps = true;
    while (word[k]) {
        if (i >= s.size()) return 0;
        char ch = s[i++];
        if (ch == ' ' && k > 0) {
            spaced = true;
            continue;
        }
        if (std::toupper((unsigned char)ch) != word[k]) return 0;
        caps = caps && ch == word[k];
        ++k;
    }
    size_t j = i;
    while (j < s.size() && s[j] == ' ') ++j;
    bool sep = j < s.size() && (s[j] == ':' || s[j] == '-');
    if (j < s.size() && !sep && !spaced && !caps) return 0;
    if (j == i && j < s.size() && !sep) return 0;
    if (sep) ++j;
    while (j < s.size() && s[j] == ' ') ++j;
    return j;
}

void Collect(Object* root, Class* cls, Method* get, std::string& out) {
    if (!cls || !get) return;
    for (Object* t : CompsInChildren(root, cls, true)) {
        std::string s = Clean(mono::Str(Call(mono::Virtual(t, get), t)));
        s.erase(0, Title(s));
        if (s.empty()) continue;
        if (!out.empty()) out += ' ';
        out += s;
    }
}

std::string Read(Object* desc) {
    std::string out;
    Collect(desc, G.tmp, G.tmpText, out);
    if (out.empty()) Collect(desc, G.uiText, G.uiGetText, out);
    if (out.size() > kMaxText) {
        size_t cut = kMaxText;
        while (cut > 0 && ((unsigned char)out[cut] & 0xC0) == 0x80) --cut;
        out.resize(cut);
        out += "...";
    }
    return out;
}

Object* Trigger(Object* icon) {
    Object* tr = Alive(icon) ? TransformOf(icon) : nullptr;
    if (!tr) return nullptr;
    Object* t = Call(G.find, tr, {mono::NewString("EventTrigger")});
    return Alive(t) ? GameObjectOf(t) : nullptr;
}

void Learn(Panel& p, Object* panel, Object* icon, Object* trigger) {
    Object* roots[] = {trigger, icon, panel};
    Object* hover = nullptr;
    for (Object* root : roots) {
        if (hover || !Alive(root)) continue;
        std::vector<Object*> found = CompsInChildren(root, G.hover, true);
        if (!found.empty()) hover = found.front();
    }
    Object* desc = hover ? Ref(hover, G.hoverPanel) : nullptr;
    if (!Alive(desc)) return;
    p.desc.set(desc);
    if (!canRead) return;
    std::string text = Read(desc);
    if (!text.empty()) texts[p.level] = text;
}

State StateOf(Object* panel, int level) {
    if (!Alive(panel) || Val<uint8_t>(panel, G.forceOff, 1) != 0) return S_LOCKED;
    int progress = Val<int>(panel, G.progress, 0);
    if (progress < level) return S_LOCKED;
    return progress > level ? S_BEATEN : S_OPEN;
}

void Hide(Panel& p) {
    if (!p.mine) return;
    p.mine = false;
    if (StateOf(p.panel.get(), p.level) == S_BEATEN) return;
    Object* go = p.trigger.get();
    if (Alive(go)) SetActive(go, false);
    Object* desc = p.desc.get();
    if (!p.descWas && Alive(desc)) SetActive(desc, false);
}

void Show(Panel& p) {
    Object* go = p.trigger.get();
    if (!Alive(go)) {
        p.mine = false;
        return;
    }
    State s = StateOf(p.panel.get(), p.level);
    if (s == S_BEATEN) {
        p.mine = false;
        return;
    }
    if (s == S_LOCKED) {
        Hide(p);
        return;
    }
    if (CallBool(G.activeSelf, go, {}, true)) return;
    if (!p.mine) {
        Object* desc = p.desc.get();
        p.descWas = Alive(desc) && CallBool(G.activeSelf, desc, {}, true);
    }
    if (!p.told) {
        p.told = true;
        LOG("level: the challenge of level %d can be hovered in the level select", p.level);
    }
    SetActive(go, true);
    p.mine = true;
}

void Forget(Panel& p) {
    Hide(p);
    p.panel.reset();
    p.trigger.reset();
    p.desc.reset();
}

void DropAll() {
    for (Panel& p : panels) Forget(p);
    panels.clear();
}

void Scan() {
    std::vector<Panel> keep;
    std::unordered_set<Object*> known;
    for (Panel& p : panels) {
        Object* o = p.panel.get();
        if (!Alive(o)) {
            Forget(p);
            continue;
        }
        known.insert(o);
        keep.push_back(std::move(p));
    }
    panels = std::move(keep);
    for (Object* o : FindAll(G.panel, true)) {
        int level = Val<int>(o, G.level, 0);
        if (level <= 0 || level >= 100 || !known.insert(o).second) continue;
        Panel p;
        p.level = level;
        p.panel.set(o);
        Object* icon = Ref(o, G.icon);
        if (!Alive(icon)) icon = nullptr;
        Object* go = Trigger(icon);
        if (go) p.trigger.set(go);
        if (canHover) Learn(p, o, icon, go);
        panels.push_back(std::move(p));
    }
    scanned = !panels.empty();
    if (texts.size() != toldTexts) {
        toldTexts = texts.size();
        LOG("level: %d challenge texts read from the level select", (int)toldTexts);
    }
}

void Menu(const Ctx& c, bool on) {
    if ((on || !scanned) && c.now >= nextScan) {
        nextScan = c.now + kScanEvery;
        Scan();
    }
    if (!on) {
        for (Panel& p : panels) Hide(p);
        return;
    }
    if (c.now < nextApply) return;
    nextApply = c.now + kApplyEvery;
    for (Panel& p : panels) Show(p);
}

bool Stock(int level) {
    if (level <= 0 || level >= 100) return false;
    if (G.isCustom && CallBool(G.isCustom, nullptr, {}, true)) return false;
    std::string scene = mono::Str(Static(R.shCurrent));
    if (scene.empty()) return false;
    if (!G.sceneName) return scene.rfind("Level ", 0) == 0;
    int n = level;
    bool ok = false;
    std::string want = mono::Str(Call(G.sceneName, nullptr, {&n}, &ok));
    return ok && scene == want;
}

void Announce(const Ctx& c) {
    if (!announce || c.now < nextAnnounce) return;
    nextAnnounce = c.now + kAnnounceEvery;
    Object* sm = Single(R.statsManager);
    int run = sm ? InstanceId(sm) : 0;
    if (!run) return;
    announce = false;
    if (run == announcedRun) return;
    announcedRun = run;
    int level = Val<int>(sm, G.smLevel, 0);
    if (!Stock(level)) return;
    auto it = texts.find(level);
    if (it != texts.end()) {
        LOG("level: challenge note for level %d", level);
        c.Note("Challenge: " + it->second);
    } else if (texts.empty()) {
        c.Note("Challenge shower: open the main menu once so the challenges can be read");
    }
}

}

bool Resolve() {
    G = Game{};
    G.tracker = Cls("EnemyTracker");
    G.current = M(G.tracker, "GetCurrentEnemies", 0);
    G.eid = Cls("EnemyIdentifier");
    G.instaKill = M(G.eid, "InstaKill", 0);
    G.dead = F(G.eid, "dead");
    G.center = F(G.eid, "overrideCenter");
    G.player = Cls("PlayerTracker");
    G.playerType = F(G.player, "playerType");
    G.target = F(G.player, "target");
    G.platformer = Enum(Cls("PlayerType"), "Platformer");

    G.panel = Cls("LevelSelectPanel");
    G.level = F(G.panel, "levelNumber");
    G.progress = F(G.panel, "tempInt");
    G.forceOff = F(G.panel, "forceOff");
    G.icon = F(G.panel, "challengeIcon");
    G.find = MT(R.transform, "Find", {"System.String"});
    G.activeSelf = M(R.gameObject, "get_activeSelf", 0);
    G.hover = Cls("MenuChallengeHover");
    G.hoverPanel = F(G.hover, "panelToActivate");
    G.tmp = Cls("Unity.TextMeshPro", "TMPro.TMP_Text");
    G.tmpText = M(G.tmp, "get_text", 0);
    G.uiText = Cls("UnityEngine.UI.Text");
    G.uiGetText = M(G.uiText, "get_text", 0);
    G.smLevel = F(R.statsManager, "levelNumber");
    G.isCustom = M(R.sceneHelper, "get_IsPlayingCustom", 0);
    G.sceneName = MT(Cls("GetMissionName"), "GetSceneName", {"System.Int32"});

    canKill = G.tracker && G.current && G.eid && G.instaKill && G.dead && R.compTransform && R.trGetPosition;
    canPlatformer = G.player && G.playerType && G.target && G.platformer >= 0;
    canShower = G.panel && G.level && G.progress && G.forceOff && G.icon && G.find && G.activeSelf && R.goSetActive &&
                R.findObjectsOfTypeInactive && R.compTransform && R.compGameObject;
    canHover = canShower && G.hover && G.hoverPanel && R.compGetComponentsInChildren;
    canRead = canHover && ((G.tmp && G.tmpText) || (G.uiText && G.uiGetText));
    canText = canRead && R.statsManager && G.smLevel && R.shCurrent;

    if (!canKill) LOG("level: kill all enemies in range is off (the game's enemy list was not found)");
    else if (!canPlatformer) LOG("level: kill all enemies in range works, but not in the 3rd-person platformer parts");
    if (!canShower) LOG("level: challenge shower is off (the level select fields were not found)");
    else if (!canText) LOG("level: challenge shower works in the level select, but the challenge cannot be shown when a level starts");
    return canKill || canShower;
}

bool Supported(const std::string& id) {
    if (id == kKill || id == kRange) return canKill;
    if (id == kShower) return canShower;
    return false;
}

void Tick(const Ctx& c) {
    bool on = canShower && c.on(kShower);
    if (canShower && c.scene == Scene::MainMenu) Menu(c, on);
    else if (!panels.empty()) DropAll();
    if (on && canText && c.scene == Scene::Level) Announce(c);
}

void Release(const Ctx& c) {
    DropAll();
    scanned = false;
    nextScan = 0;
    nextApply = 0;
    nextAnnounce = 0;
    announce = true;
    if (c.stopping) {
        texts.clear();
        toldTexts = 0;
        announcedRun = 0;
    }
}

void Action(const std::string& id, const Ctx& c) {
    if (id == kKill && canKill) Kill(c);
}

bool Busy() { return false; }

bool Outside() { return true; }

}
