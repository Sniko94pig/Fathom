#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/patch.h"
#include "ingame/unity.h"
#include <algorithm>
#include <cmath>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace sotf {

namespace {

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;

const char* kSons = "Sons";
const char* kItems = "Sons.Item";
const int kStackCap = 999;
const int kFinishPerFrame = 15;
const int kAddsPerFrame = 4;
const int kMaxAddsPerNode = 500;
const int kLogId = 78;
const int kStoneId = 640;

struct Refs {
    Class *localPlayer = nullptr, *inventory = nullptr, *placeAction = nullptr, *singleOp = nullptr, *treeOp = nullptr, *multiOp = nullptr;
    Class *scs = nullptr, *node = nullptr, *held = nullptr, *heldData = nullptr, *setup = nullptr;
    Class *crafting = nullptr, *recipeDb = nullptr, *recipe = nullptr, *recipeKind = nullptr, *ingredient = nullptr;
    Class *itemManager = nullptr, *itemDb = nullptr, *itemData = nullptr;
    Class *printer = nullptr, *blueprint = nullptr, *cheats = nullptr, *treeCut = nullptr, *loadScene = nullptr, *saves = nullptr;
    Fld lpInventory, lpInventoryStatic;
    Fld invPlaceAction, invHeld, invCraftDb;
    Fld psaPlacing, psaActiveOp, psaSingle, psaTree, psaMulti, psaScs;
    Fld singleValid, singleNode, treeValid, treeNode, treeTarget, multiNodes;
    Fld scsInstance, scsNodes, scsInstantBuild, scsInfinite, scsNoCuttings;
    Fld nodePending;
    Method *nodeComplete = nullptr, *nodeNext = nullptr, *nodeTryAdd = nullptr;
    Fld heldInfinite, heldItems, heldDataId, heldItemId;
    Method* creativeSetting = nullptr;
    Fld csInstance, csRecipeDb, dbRecipes, recipeType, recipeIngredients, ingReusable;
    int craftNewItem = -1;
    Fld imInstance, idbItems, itemId, itemMax, itemUnique, itemNoStore, itemName;
    Method* getItems = nullptr;
    Fld prCur, prMax, prPrinting, prPaused, prTime, prBlueprint, bpRequired, bpTotal;
    Method* prRefill = nullptr;
    Fld chOverrides, chOneHit, tcOneHitFell, lsLoading;
    Fld tcBroken, tcActiveHit, tcTriggered, tcLastForward;
    Method* tcFell = nullptr;
    Method* loadInProgress = nullptr;
} R;

struct Support {
    bool noReq = false, anywhere = false, finish = false, autoFill = false, noCraft = false, noLimit = false;
    bool resin = false, fastPrint = false, oneHit = false, instantFall = false, treeFell = false;
    bool guestAdd = false, logHack = false, stoneHack = false;
} S;

struct StaticFlag {
    bool have = false;
    bool orig = false;
};

struct Printer {
    uint32_t h = 0;
    bool paid = false;
};

struct Cap {
    uint32_t h = 0;
    int id = 0;
    int orig = 0;
};

struct Job {
    std::vector<uint32_t> nodes;
    size_t next = 0;
    int done = 0;
    int adds = 0;
};

struct GuestFill {
    std::unordered_set<Object*> seen;
    std::unordered_map<Object*, double> fresh;
    bool armed = false;
    double scan = 0, lastPlacing = -100;
};

patch::Forced<bool> scsInfinite, heldInfinite, instantBuild, oneHitTrees, noCuttings;
StaticFlag treeFall;
Job job, fill;
GuestFill guest;
std::vector<int> logIds, stoneIds;
double heldIdScan = 0;
std::vector<uint32_t> trees;
double treeScan = 0;
std::vector<uint32_t> reused;
patch::Hold craftDb;
double craftCheck = 0;
std::unordered_map<Object*, Cap> caps;
std::unordered_set<int> heldOnly;
double capScan = 0;
std::vector<Printer> printers;
double printerScan = 0, resinTick = 0;

Fld Find(Class* c, const char* name, bool isStatic, const char* type = nullptr) {
    Fld f = un::F(c, name);
    if (!f) return f;
    if (f.isStatic != isStatic || (type && il2::FieldTypeName(f.f) != type)) {
        LOG("building: %s.%s is not the expected field", il2::ClassName(c), name);
        return Fld{};
    }
    return f;
}

Class* Inner(Class* outer, const char* name) {
    if (!outer) return nullptr;
    Class* c = il2::Nested(outer, name);
    if (!c) un::Missing((std::string(il2::ClassName(outer)) + "." + name).c_str());
    return c;
}

bool IsA(Object* o, Class* k) {
    Class* c = o ? il2::ClassOf(o) : nullptr;
    return c && k && (c == k || il2::IsSubclass(c, k));
}

Object* Inventory(Object* player) {
    Object* inv = un::Ref(player, R.lpInventory);
    if (!un::Alive(inv)) inv = un::Static(R.lpInventoryStatic);
    return un::Alive(inv) ? inv : nullptr;
}

Object* PlaceAction(Object* player) {
    Object* psa = un::Ref(Inventory(player), R.invPlaceAction);
    return un::Alive(psa) ? psa : nullptr;
}

Object* HeldController(Object* player) {
    Object* ctrl = un::Ref(Inventory(player), R.invHeld);
    return un::Alive(ctrl) && IsA(ctrl, R.held) ? ctrl : nullptr;
}

Object* Scs(Object* player) {
    Object* s = un::Static(R.scsInstance);
    if (un::Alive(s) && IsA(s, R.scs)) return s;
    s = un::Ref(PlaceAction(player), R.psaScs);
    return un::Alive(s) && IsA(s, R.scs) ? s : nullptr;
}

bool CreativeMode() { return R.creativeSetting && un::CallBool(R.creativeSetting, nullptr, {}, false); }

void ForceBuildFlag(Object* o, const Fld& f, bool want, patch::Forced<bool>& s) {
    if (!f) return;
    bool fresh = want && un::Alive(o) && (!s.have || s.obj.get() != o);
    patch::Force<bool>(o, f, want, true, s);
    if (fresh && s.have && !s.orig && CreativeMode()) s.orig = true;
}

void DropFlag(patch::Forced<bool>& s, const Fld& f) {
    Object* o = s.obj.get();
    if (s.have && o && f && un::Val<bool>(o, f) == s.written) un::SetVal<bool>(o, f, s.orig);
    s.obj.reset();
    s.have = false;
}

void HoldFlag(Object* o, const Fld& f, bool want, patch::Forced<bool>& s) {
    if (!f) return;
    if (s.have && (s.obj.get() != o || !want)) DropFlag(s, f);
    if (!want || !o) return;
    bool cur = un::Val<bool>(o, f);
    if (!s.have) {
        s.obj.set(o);
        s.orig = cur;
        s.have = true;
    } else if (cur != s.written) {
        s.orig = cur;
    }
    if (!cur) un::SetVal<bool>(o, f, true);
    s.written = true;
}

bool ReadFlag(const Fld& f, bool& out) {
    out = false;
    return f && f.isStatic && il2::StaticGet(f.f, &out, sizeof(out));
}

void ForceStatic(const Fld& f, bool want, StaticFlag& s) {
    if (!want && !s.have) return;
    bool cur = false;
    if (!ReadFlag(f, cur)) return;
    if (!want) {
        if (cur) un::SetStaticVal<bool>(f, s.orig);
        s.have = false;
        return;
    }
    if (!s.have) {
        s.orig = cur;
        s.have = true;
    } else if (!cur) {
        s.orig = false;
    }
    if (!cur) un::SetStaticVal<bool>(f, true);
}

void BuildAnywhere(Object* player) {
    Object* psa = PlaceAction(player);
    if (!psa || !un::Val<bool>(psa, R.psaPlacing)) return;
    Object* op = un::Ref(psa, R.psaActiveOp);
    if (!op) return;
    Class* k = il2::ClassOf(op);
    if (k == R.singleOp) {
        if (!un::Val<bool>(op, R.singleValid) && un::Alive(un::Ref(op, R.singleNode))) un::SetVal<bool>(op, R.singleValid, true);
    } else if (R.treeOp && k == R.treeOp && R.treeValid && R.treeNode && R.treeTarget) {
        if (!un::Val<bool>(op, R.treeValid) && un::Alive(un::Ref(op, R.treeNode)) && un::Alive(un::Ref(op, R.treeTarget)))
            un::SetVal<bool>(op, R.treeValid, true);
    }
}

void DropJob(Job& j) {
    for (size_t i = j.next; i < j.nodes.size(); ++i) il2::Unpin(j.nodes[i]);
    j = Job{};
}

std::unordered_set<Object*> Previews(Object* psa) {
    std::unordered_set<Object*> previews;
    if (!psa) return previews;
    for (const Fld* opField : {&R.psaSingle, &R.psaTree}) {
        Object* op = un::Ref(psa, *opField);
        Class* k = op ? il2::ClassOf(op) : nullptr;
        if (!k) continue;
        Object* n = nullptr;
        if (k == R.singleOp) n = un::Ref(op, R.singleNode);
        else if (k == R.treeOp) n = un::Ref(op, R.treeNode);
        if (n) previews.insert(n);
    }
    Object* multi = un::Ref(psa, R.psaMulti);
    if (multi && il2::ClassOf(multi) == R.multiOp)
        for (Object* line : il2::ListItems(un::Ref(multi, R.multiNodes)))
            for (Object* n : il2::ListItems(line))
                if (n) previews.insert(n);
    return previews;
}

void StartFinish(const Ctx& c) {
    if (!job.nodes.empty()) {
        Note("Blueprints are already being finished");
        return;
    }
    Object* psa = PlaceAction(c.player);
    if (psa && un::Val<bool>(psa, R.psaPlacing)) {
        Note("Place or cancel the blueprint you are holding first");
        return;
    }
    Object* scs = Scs(c.player);
    std::unordered_set<Object*> previews = Previews(psa);
    for (Object* n : il2::ListItems(un::Ref(scs, R.scsNodes))) {
        if (!un::Alive(n) || !IsA(n, R.node) || previews.count(n) || un::Val<bool>(n, R.nodePending)) continue;
        job.nodes.push_back(il2::Pin(n));
    }
    if (job.nodes.empty()) Note("No unfinished blueprints found");
}

void HostStep(Job& j) {
    int budget = kFinishPerFrame;
    while (budget > 0 && j.next < j.nodes.size()) {
        uint32_t h = j.nodes[j.next++];
        Object* n = il2::Pinned(h);
        if (un::Alive(n) && !un::Val<bool>(n, R.nodePending)) {
            bool relocated = false, ok = false;
            un::Call(R.nodeComplete, n, {&relocated}, &ok);
            if (ok) ++j.done;
            --budget;
        }
        il2::Unpin(h);
    }
}

void GuestStep(Job& j, const Ctx& c) {
    std::unordered_set<Object*> previews = Previews(PlaceAction(c.player));
    int budget = kAddsPerFrame;
    while (budget > 0 && j.next < j.nodes.size()) {
        Object* n = il2::Pinned(j.nodes[j.next]);
        bool more = false;
        if (un::Alive(n) && IsA(n, R.node) && !un::Val<bool>(n, R.nodePending) && !previews.count(n) && j.adds < kMaxAddsPerNode) {
            int id = un::CallInt(R.nodeNext, n, {}, -1);
            if (id >= 0) {
                int one = 1;
                bool local = true, ok = false;
                un::Call(R.nodeTryAdd, n, {&id, &one, &local}, &ok);
                --budget;
                ++j.adds;
                more = ok;
            } else if (j.adds > 0) {
                ++j.done;
            }
        }
        if (more) continue;
        il2::Unpin(j.nodes[j.next++]);
        j.adds = 0;
    }
}

void RunJob(Job& j, const Ctx& c, bool report) {
    bool guestJob = c.session == core::Session::Client;
    if (!c.inWorld || !c.player || !c.cheats || (guestJob && !S.guestAdd)) {
        DropJob(j);
        return;
    }
    if (guestJob) GuestStep(j, c);
    else HostStep(j);
    if (j.next < j.nodes.size()) return;
    int done = j.done;
    DropJob(j);
    if (!report) return;
    if (done <= 0) Note("No blueprints could be finished");
    else if (done == 1) Note("1 blueprint finished");
    else Note(std::to_string(done) + " blueprints finished");
}

void DropGuestFill() {
    guest = GuestFill{};
    DropJob(fill);
}

void TrackGuestFill(const Ctx& c, bool want) {
    if (!want) {
        if (guest.armed || !fill.nodes.empty()) DropGuestFill();
        return;
    }
    Object* psa = PlaceAction(c.player);
    if (psa && un::Val<bool>(psa, R.psaPlacing)) guest.lastPlacing = c.now;
    if (c.now < guest.scan) return;
    guest.scan = c.now + 0.2;
    Object* scs = Scs(c.player);
    if (!scs) return;
    std::unordered_set<Object*> previews = Previews(psa);
    std::unordered_set<Object*> seen;
    std::unordered_map<Object*, double> fresh;
    for (Object* n : il2::ListItems(un::Ref(scs, R.scsNodes))) {
        if (!un::Alive(n) || !IsA(n, R.node)) continue;
        if (!guest.armed || guest.seen.count(n) || previews.count(n) || un::Val<bool>(n, R.nodePending)) {
            seen.insert(n);
            continue;
        }
        auto it = guest.fresh.find(n);
        if (it == guest.fresh.end()) {
            if (c.now - guest.lastPlacing > 3.0) seen.insert(n);
            else fresh[n] = c.now;
            continue;
        }
        if (c.now - it->second < 0.4) {
            fresh[n] = it->second;
            continue;
        }
        fill.nodes.push_back(il2::Pin(n));
        seen.insert(n);
    }
    guest.seen.swap(seen);
    guest.fresh.swap(fresh);
    guest.armed = true;
}

void DropTrees() {
    for (uint32_t h : trees) il2::Unpin(h);
    trees.clear();
    treeScan = 0;
}

void FellHitTrees(const Ctx& c) {
    if (c.now >= treeScan) {
        for (uint32_t h : trees) il2::Unpin(h);
        trees.clear();
        for (Object* o : un::FindAll(R.treeCut))
            if (uint32_t h = il2::Weak(o)) trees.push_back(h);
        treeScan = c.now + 0.5;
    }
    for (uint32_t h : trees) {
        Object* t = il2::Pinned(h);
        if (!un::Alive(t) || un::Val<bool>(t, R.tcBroken) || !un::Val<bool>(t, R.tcActiveHit)) continue;
        if (R.tcLastForward && c.player) {
            un::Vec3 dir = un::Norm(un::Flat(un::Forward(c.player)));
            if (un::Len(dir) > 0.5f) un::SetVal<un::Vec3>(t, R.tcLastForward, dir);
        }
        un::SetVal<bool>(t, R.tcTriggered, true);
        bool straightToStump = false;
        un::Call(R.tcFell, t, {&straightToStump});
    }
}

void RestoreReuse() {
    for (uint32_t h : reused) {
        Object* o = il2::Pinned(h);
        if (o && un::Val<bool>(o, R.ingReusable)) un::SetVal<bool>(o, R.ingReusable, false);
        il2::Unpin(h);
    }
    reused.clear();
    craftDb.reset();
    craftCheck = 0;
}

void ApplyReuse(Object* db) {
    for (Object* recipe : il2::ListItems(un::Ref(db, R.dbRecipes))) {
        if (!un::Alive(recipe) || un::Val<int>(recipe, R.recipeType) != R.craftNewItem) continue;
        for (Object* ing : il2::ListItems(un::Ref(recipe, R.recipeIngredients))) {
            if (!ing || un::Val<bool>(ing, R.ingReusable)) continue;
            reused.push_back(il2::Pin(ing));
            un::SetVal<bool>(ing, R.ingReusable, true);
        }
    }
}

void Crafting(const Ctx& c, bool want) {
    if (!want) {
        if (craftDb.held() || !reused.empty()) RestoreReuse();
        return;
    }
    WarnAchievementsOnce();
    Object* cs = un::Static(R.csInstance);
    Object* db = un::Alive(cs) ? un::Ref(cs, R.csRecipeDb) : nullptr;
    if (!un::Alive(db)) db = un::Ref(Inventory(c.player), R.invCraftDb);
    if (!un::Alive(db)) return;
    if (db != craftDb.get()) {
        RestoreReuse();
        ApplyReuse(db);
        craftDb.set(db);
        craftCheck = c.now + 2.0;
        return;
    }
    if (c.now < craftCheck) return;
    craftCheck = c.now + 2.0;
    for (uint32_t h : reused) {
        Object* o = il2::Pinned(h);
        if (o && !un::Val<bool>(o, R.ingReusable)) un::SetVal<bool>(o, R.ingReusable, true);
    }
}

bool LoadingSave() {
    bool loading = false;
    if (ReadFlag(R.lsLoading, loading) && loading) return true;
    return R.loadInProgress && un::CallBool(R.loadInProgress, nullptr, {}, false);
}

Object* ItemList() {
    if (R.imInstance && R.idbItems) {
        Object* db = un::Static(R.imInstance);
        return un::Alive(db) ? un::Ref(db, R.idbItems) : nullptr;
    }
    return R.getItems ? un::Call(R.getItems, nullptr) : nullptr;
}

void FindHeldIds(double now) {
    if (now < heldIdScan) return;
    heldIdScan = now + 5.0;
    if (logIds.empty()) logIds.push_back(kLogId);
    if (stoneIds.empty()) stoneIds.push_back(kStoneId);
    if (!R.itemName || !R.itemId) {
        heldIdScan = 1e300;
        return;
    }
    std::vector<int> logs, stones;
    bool any = false;
    for (Object* item : il2::ListItems(ItemList())) {
        if (!un::Alive(item)) continue;
        any = true;
        std::string name = il2::Str(un::Ref(item, R.itemName));
        int id = un::Val<int>(item, R.itemId);
        if (name == "Log" || name == "Log_legacy") logs.push_back(id);
        else if (name == "Stone") stones.push_back(id);
    }
    if (!any) return;
    if (!logs.empty()) logIds = logs;
    if (!stones.empty()) stoneIds = stones;
    heldIdScan = 1e300;
}

bool HoldingHacked(Object* ctrl, bool logs, bool stones, double now) {
    if (!ctrl || (!logs && !stones)) return false;
    FindHeldIds(now);
    int id = un::Val<int>(ctrl, R.heldItemId);
    if (logs && std::find(logIds.begin(), logIds.end(), id) != logIds.end()) return true;
    return stones && std::find(stoneIds.begin(), stoneIds.end(), id) != stoneIds.end();
}

void LearnHeldOnly(Object* player) {
    if (!R.heldItems || !R.heldDataId) return;
    Object* arr = un::Ref(HeldController(player), R.heldItems);
    size_t n = il2::Length(arr);
    for (size_t i = 0; i < n; ++i)
        if (Object* d = il2::At(arr, i)) heldOnly.insert(un::Val<int>(d, R.heldDataId));
}

void RestoreCaps() {
    for (auto& [item, cap] : caps) {
        Object* o = il2::Pinned(cap.h);
        if (un::Alive(o) && un::Val<int>(o, R.itemMax) == kStackCap) un::SetVal<int>(o, R.itemMax, cap.orig);
        il2::Unpin(cap.h);
    }
    caps.clear();
    capScan = 0;
}

void KeepCaps() {
    for (auto it = caps.begin(); it != caps.end();) {
        Object* o = il2::Pinned(it->second.h);
        bool alive = un::Alive(o);
        int cur = alive ? un::Val<int>(o, R.itemMax) : 0;
        if (!alive || cur > kStackCap || heldOnly.count(it->second.id)) {
            if (alive && cur == kStackCap) un::SetVal<int>(o, R.itemMax, it->second.orig);
            il2::Unpin(it->second.h);
            it = caps.erase(it);
            continue;
        }
        if (cur != kStackCap) {
            it->second.orig = cur;
            un::SetVal<int>(o, R.itemMax, kStackCap);
        }
        ++it;
    }
    for (Object* item : il2::ListItems(ItemList())) {
        if (!un::Alive(item) || caps.count(item)) continue;
        int mx = un::Val<int>(item, R.itemMax);
        if (mx < 2 || mx >= kStackCap || un::Val<bool>(item, R.itemUnique)) continue;
        if (R.itemNoStore && un::Val<bool>(item, R.itemNoStore)) continue;
        int id = un::Val<int>(item, R.itemId);
        if (heldOnly.count(id)) continue;
        Cap cap;
        cap.h = il2::Pin(item);
        cap.id = id;
        cap.orig = mx;
        caps[item] = cap;
        un::SetVal<int>(item, R.itemMax, kStackCap);
    }
}

void ItemCaps(const Ctx& c, bool on) {
    bool want = on && c.cheats && (c.inWorld || LoadingSave());
    if (!want) {
        if (!caps.empty() || capScan != 0) RestoreCaps();
        return;
    }
    if (c.now < capScan) return;
    capScan = c.now + 2.0;
    if (c.inWorld) LearnHeldOnly(c.player);
    KeepCaps();
}

void DropPrinters() {
    for (Printer& p : printers) il2::Unpin(p.h);
    printers.clear();
    printerScan = 0;
    resinTick = 0;
}

void ScanPrinters(double now) {
    if (now < printerScan) return;
    printerScan = now + 5.0;
    std::vector<Printer> next;
    for (Object* o : un::FindAll(R.printer)) {
        Printer p;
        for (Printer& old : printers)
            if (old.h && il2::Pinned(old.h) == o) {
                p = old;
                old.h = 0;
                break;
            }
        if (!p.h) p.h = il2::Weak(o);
        if (p.h) next.push_back(p);
    }
    for (Printer& old : printers) il2::Unpin(old.h);
    printers.swap(next);
}

void KeepResin(double now) {
    if (now < resinTick) return;
    resinTick = now + 1.0;
    for (Printer& p : printers) {
        Object* o = il2::Pinned(p.h);
        if (!un::Alive(o)) continue;
        float cur = un::Val<float>(o, R.prCur), mx = un::Val<float>(o, R.prMax);
        if (!(mx > 0.0f) || !(cur < mx - 1.0f)) continue;
        int add = (int)std::floor(mx - std::max(cur, 0.0f));
        bool network = true;
        if (add > 0) un::Call(R.prRefill, o, {&add, &network});
    }
}

void FastPrint(bool unlimited) {
    for (Printer& p : printers) {
        Object* o = il2::Pinned(p.h);
        if (!un::Alive(o)) continue;
        if (!un::Val<bool>(o, R.prPrinting)) {
            p.paid = false;
            continue;
        }
        if (un::Val<bool>(o, R.prPaused)) continue;
        Object* bp = un::Ref(o, R.prBlueprint);
        if (!un::Alive(bp)) continue;
        float total = un::Val<float>(bp, R.bpTotal), t = un::Val<float>(o, R.prTime);
        if (!(total > 0.0f) || !(t < total)) continue;
        if (!unlimited && !p.paid) {
            float required = (float)std::max(un::Val<int>(bp, R.bpRequired), 0);
            float need = (1.0f - std::clamp(t / total, 0.0f, 1.0f)) * required;
            float resin = un::Val<float>(o, R.prCur);
            if (resin < need) {
                if (resin > 0.5f && required > 0.0f) {
                    un::SetVal<float>(o, R.prTime, std::min(total, std::max(t, 0.0f) + resin / required * total));
                    un::SetVal<float>(o, R.prCur, 0.0f);
                }
                continue;
            }
            un::SetVal<float>(o, R.prCur, std::max(resin - need, 0.0f));
        }
        p.paid = true;
        un::SetVal<float>(o, R.prTime, total);
    }
}

void Printers(const Ctx& c, bool live) {
    bool resin = S.resin && live && c.s.On("unlimited_resin");
    bool fast = S.fastPrint && live && c.session != core::Session::Client && c.s.On("fast_printing");
    if (!resin && !fast) {
        if (!printers.empty() || printerScan != 0) DropPrinters();
        return;
    }
    WarnAchievementsOnce();
    ScanPrinters(c.now);
    if (resin) KeepResin(c.now);
    if (fast) FastPrint(resin);
}

bool Supported(const std::string& id) {
    if (id == "no_build_requirements") return S.noReq;
    if (id == "build_anywhere") return S.anywhere;
    if (id == "finish_blueprints") return S.finish;
    if (id == "always_fill_blueprints") return S.autoFill || S.guestAdd;
    if (id == "no_craft_requirements") return S.noCraft;
    if (id == "no_item_limit") return S.noLimit;
    if (id == "unlimited_resin") return S.resin;
    if (id == "fast_printing") return S.fastPrint;
    if (id == "one_hit_trees") return S.oneHit;
    if (id == "instant_tree_fall") return S.instantFall;
    if (id == "log_hack") return S.logHack;
    if (id == "stone_hack") return S.stoneHack;
    return false;
}

bool Resolve(std::string& err) {
    R.localPlayer = un::C(kSons, "TheForest.Utils", "LocalPlayer");
    R.lpInventory = Find(R.localPlayer, "_inventory", false);
    R.lpInventoryStatic = Find(R.localPlayer, "<Inventory>k__BackingField", true);
    R.inventory = un::C(kSons, "TheForest.Items.Inventory", "PlayerInventory");
    R.invPlaceAction = Find(R.inventory, "_playerStructurePlaceAction", false);
    R.invHeld = Find(R.inventory, "<HeldOnlyItemController>k__BackingField", false);
    R.invCraftDb = Find(R.inventory, "_craftingRecipeDatabase", false);

    R.placeAction = un::C(kSons, "TheForest.Player.Actions", "PlayerStructurePlaceAction");
    R.psaPlacing = Find(R.placeAction, "<InPlacementMode>k__BackingField", false, "System.Boolean");
    R.psaActiveOp = Find(R.placeAction, "_activePlaceOperation", false);
    R.psaSingle = Find(R.placeAction, "_defaultPlaceOperation", false);
    R.psaTree = Find(R.placeAction, "_treeStructurePlaceOperation", false);
    R.psaScs = Find(R.placeAction, "_structureCraftingSystem", false);
    R.singleOp = Inner(R.placeAction, "SinglePlaceOperation");
    R.singleValid = Find(R.singleOp, "<HasValidPlacementTarget>k__BackingField", false, "System.Boolean");
    R.singleNode = Find(R.singleOp, "<PreviewStructureNode>k__BackingField", false);
    R.treeOp = Inner(R.placeAction, "TreeStructurePlaceOperation");
    R.treeValid = Find(R.treeOp, "<HasValidPlacementTarget>k__BackingField", false, "System.Boolean");
    R.treeNode = Find(R.treeOp, "<PreviewStructureNode>k__BackingField", false);
    R.treeTarget = Find(R.treeOp, "<TargetTree>k__BackingField", false);
    R.psaMulti = Find(R.placeAction, "_multiplePlaceOperation", false);
    R.multiOp = Inner(R.placeAction, "MultiplePlaceOperation");
    R.multiNodes = Find(R.multiOp, "_nodesBetweenPoints", false);

    R.scs = un::C(kSons, "Sons.Crafting.Structures", "StructureCraftingSystem");
    Class* singleton = il2::Parent(R.scs);
    R.scsInstance = singleton ? Find(singleton, "_instance", true) : Fld{};
    R.scsNodes = Find(R.scs, "_activeStructureNodes", false);
    R.scsInstantBuild = Find(R.scs, "<InstantBuild>k__BackingField", false, "System.Boolean");
    R.scsInfinite = Find(R.scs, "<InfiniteItemHack>k__BackingField", false, "System.Boolean");
    R.scsNoCuttings = Find(R.scs, "<BlockCuttingsSpawn>k__BackingField", false, "System.Boolean");
    R.node = un::C(kSons, "Sons.Crafting.Structures", "StructureCraftingNode");
    R.nodePending = Find(R.node, "_pendingDestroy", false, "System.Boolean");
    R.nodeComplete = un::M(R.node, "StructureCompleted", std::vector<std::string>{"System.Boolean"});
    R.nodeNext = un::M(R.node, "GetNextRequiredIngredientId", 0);
    R.nodeTryAdd = un::M(R.node, "TryAddIngredient", std::vector<std::string>{"System.Int32", "System.Int32", "System.Boolean"});

    R.held = un::C(kSons, "TheForest.Items.Special", "HeldOnlyItemController");
    R.heldInfinite = Find(R.held, "<InfiniteHack>k__BackingField", false, "System.Boolean");
    R.heldItemId = Find(R.held, "_heldItemId", false, "System.Int32");
    R.heldItems = Find(R.held, "_heldOnlyItems", false);
    R.heldData = Inner(R.held, "ControllerItemData");
    R.heldDataId = Find(R.heldData, "_itemId", false, "System.Int32");
    R.setup = un::C("Sons.Gameplay.GameSetup", "Sons.Gameplay.GameSetup", "GameSetupManager");
    R.creativeSetting = R.setup ? il2::FindStaticMethod(R.setup, "GetCreativeModeSetting", 0) : nullptr;

    R.crafting = un::C(kSons, "Sons.Crafting", "CraftingSystem");
    R.csInstance = Find(R.crafting, "<Instance>k__BackingField", true);
    R.csRecipeDb = Find(R.crafting, "_recipeDatabase", false);
    R.recipeDb = un::C(kSons, "Sons.Crafting", "CraftingRecipeDatabase");
    R.dbRecipes = Find(R.recipeDb, "_recipes", false);
    R.recipe = un::C(kSons, "Sons.Crafting", "CraftingRecipe");
    R.recipeType = Find(R.recipe, "_recipeType", false);
    R.recipeIngredients = Find(R.recipe, "_ingredients", false);
    R.recipeKind = Inner(R.recipe, "Type");
    R.craftNewItem = R.recipeKind ? (int)il2::EnumValue(R.recipeKind, "CraftNewItem", -1) : -1;
    R.ingredient = un::C(kSons, "Sons.Crafting", "CraftingIngredient");
    R.ingReusable = Find(R.ingredient, "IsReusable", false, "System.Boolean");

    R.itemManager = un::C(kItems, "Sons.Items.Core", "ItemDatabaseManager");
    R.imInstance = Find(R.itemManager, "_instance", true);
    R.getItems = R.itemManager ? il2::FindStaticMethod(R.itemManager, "get_Items", 0) : nullptr;
    R.itemDb = un::C(kItems, "Sons.Items.Core", "ItemDatabase");
    R.idbItems = Find(R.itemDb, "_itemDataList", false);
    R.itemData = un::C(kItems, "Sons.Items.Core", "ItemData");
    R.itemId = Find(R.itemData, "_id", false, "System.Int32");
    R.itemMax = Find(R.itemData, "_maxAmount", false, "System.Int32");
    R.itemUnique = Find(R.itemData, "_eachInstanceIsUnique", false, "System.Boolean");
    R.itemNoStore = Find(R.itemData, "_canNotBeStored", false, "System.Boolean");
    R.itemName = Find(R.itemData, "_name", false, "System.String");

    R.printer = un::C(kSons, "", "Resin3dPrinter");
    R.prCur = Find(R.printer, "_currentResinVolume", false, "System.Single");
    R.prMax = Find(R.printer, "_maxResinVolume", false, "System.Single");
    R.prPrinting = Find(R.printer, "_isPrinting", false, "System.Boolean");
    R.prPaused = Find(R.printer, "_isPaused", false, "System.Boolean");
    R.prTime = Find(R.printer, "_timeInCurrentPrintJob", false, "System.Single");
    R.prBlueprint = Find(R.printer, "_printedItemBlueprint", false);
    R.prRefill = un::M(R.printer, "RefillResin", std::vector<std::string>{"System.Int32", "System.Boolean"});
    R.blueprint = un::C(kSons, "", "PrintedItemBlueprint");
    R.bpRequired = Find(R.blueprint, "_requiredResinVolume", false, "System.Int32");
    R.bpTotal = Find(R.blueprint, "_totalPrintTime", false, "System.Single");

    R.cheats = un::C("Sons.Settings", "Sons.Settings", "Cheats");
    R.chOverrides = Find(R.cheats, "Overrides", true);
    R.chOneHit = Find(R.cheats, "<OneHitTreeCutting>k__BackingField", false, "System.Boolean");
    R.treeCut = un::C(kSons, "Sons.Gameplay.TreeCutting", "TreeCutManager");
    R.tcOneHitFell = Find(R.treeCut, "UseOneHitFell", true, "System.Boolean");
    R.tcBroken = Find(R.treeCut, "_broken", false, "System.Boolean");
    R.tcActiveHit = Find(R.treeCut, "<IsActivelyBeingHit>k__BackingField", false, "System.Boolean");
    R.tcTriggered = Find(R.treeCut, "_thisManagerTriggeredTheFell", false, "System.Boolean");
    R.tcLastForward = Find(R.treeCut, "_lastHitSourceForward", false, "UnityEngine.Vector3");
    R.tcFell = R.treeCut ? un::M(R.treeCut, "FellTree", std::vector<std::string>{"System.Boolean"}) : nullptr;

    R.loadScene = un::C("Sons.Loading", "Sons.Loading", "LoadScene");
    R.lsLoading = Find(R.loadScene, "_isLoading", true, "System.Boolean");
    R.saves = un::C("Sons.Save", "Sons.Save", "SaveGameManager");
    R.loadInProgress = R.saves ? il2::FindStaticMethod(R.saves, "LoadInProgress", 0) : nullptr;

    bool inv = R.localPlayer && (R.lpInventory || R.lpInventoryStatic) && R.inventory;
    bool place = inv && R.placeAction && R.invPlaceAction && R.psaPlacing;
    bool scs = R.scs && (R.scsInstance || (place && R.psaScs));
    S.noReq = scs && R.scsInfinite;
    S.autoFill = scs && R.scsInstantBuild;
    S.finish = scs && place && R.scsNodes && R.node && R.nodePending && R.nodeComplete;
    S.anywhere = place && R.psaActiveOp && R.singleOp && R.singleValid && R.singleNode;
    S.noCraft = ((R.csInstance && R.csRecipeDb) || (inv && R.invCraftDb)) && R.dbRecipes && R.recipeType && R.recipeIngredients &&
                R.ingReusable && R.craftNewItem >= 0;
    S.noLimit = ((R.imInstance && R.idbItems) || R.getItems) && R.itemId && R.itemMax && R.itemUnique;
    S.resin = R.printer && R.prCur && R.prMax && R.prRefill;
    S.fastPrint = R.printer && R.prCur && R.prPrinting && R.prPaused && R.prTime && R.prBlueprint && R.bpRequired && R.bpTotal;
    S.oneHit = R.chOverrides && R.chOneHit;
    S.treeFell = R.treeCut && R.tcBroken && R.tcActiveHit && R.tcTriggered && R.tcFell;
    S.instantFall = R.tcOneHitFell || S.treeFell;
    S.guestAdd = S.finish && R.nodeNext && R.nodeTryAdd && R.scsInfinite;
    S.logHack = inv && R.invHeld && R.held && R.heldInfinite && R.heldItemId;
    S.stoneHack = S.logHack;

    std::string off;
    for (const char* id : {"no_build_requirements", "build_anywhere", "finish_blueprints", "always_fill_blueprints", "no_craft_requirements",
                           "no_item_limit", "unlimited_resin", "fast_printing", "one_hit_trees", "instant_tree_fall", "log_hack", "stone_hack"})
        if (!Supported(id)) off += std::string(off.empty() ? "" : ", ") + id;
    if (!off.empty()) LOG("building: not available: %s", off.c_str());
    if (!S.guestAdd) LOG("building: blueprints cannot be filled as a guest");
    if (!S.noReq && !S.anywhere && !S.finish && !S.autoFill && !S.noCraft && !S.noLimit && !S.resin && !S.fastPrint && !S.oneHit &&
        !S.instantFall && !S.logHack) {
        err = "none of the building parts were found";
        return false;
    }
    return true;
}

void Tick(const Ctx& c) {
    bool live = c.inWorld && c.player && c.cheats;
    bool host = c.session != core::Session::Client;
    Object* player = live ? c.player : nullptr;
    bool noReq = S.noReq && live && c.s.On("no_build_requirements");
    bool autoFill = S.autoFill && live && host && c.s.On("always_fill_blueprints");
    if (S.guestAdd) TrackGuestFill(c, live && !host && c.s.On("always_fill_blueprints"));
    bool guestAdding = S.guestAdd && live && !host && (!job.nodes.empty() || !fill.nodes.empty());
    bool infinite = noReq || guestAdding;
    Object* scs = infinite || autoFill ? Scs(player) : nullptr;
    if (noReq) WarnAchievementsOnce();
    if (R.scsInfinite) ForceBuildFlag(infinite ? scs : nullptr, R.scsInfinite, infinite, scsInfinite);
    if (R.scsNoCuttings) patch::Force<bool>(guestAdding ? scs : nullptr, R.scsNoCuttings, guestAdding, true, noCuttings);
    if (R.heldInfinite) {
        bool logs = S.logHack && live && c.s.On("log_hack");
        bool stones = S.stoneHack && live && c.s.On("stone_hack");
        Object* ctrl = noReq || logs || stones ? HeldController(player) : nullptr;
        bool held = ctrl && (noReq || HoldingHacked(ctrl, logs, stones, c.now));
        ForceBuildFlag(held ? ctrl : nullptr, R.heldInfinite, held, heldInfinite);
    }
    if (S.autoFill) patch::Force<bool>(scs, R.scsInstantBuild, autoFill, true, instantBuild);
    if (S.anywhere && live && c.s.On("build_anywhere")) BuildAnywhere(player);
    if (!job.nodes.empty()) RunJob(job, c, true);
    if (!fill.nodes.empty()) RunJob(fill, c, false);
    if (S.noCraft) Crafting(c, live && c.s.On("no_craft_requirements"));
    if (S.noLimit) ItemCaps(c, c.s.On("no_item_limit"));
    Printers(c, live);
    if (S.oneHit) {
        bool want = live && c.s.On("one_hit_trees");
        HoldFlag(want ? un::Static(R.chOverrides) : nullptr, R.chOneHit, want, oneHitTrees);
    }
    if (S.instantFall) {
        bool fall = live && c.s.On("instant_tree_fall");
        if (R.tcOneHitFell) ForceStatic(R.tcOneHitFell, fall, treeFall);
        if (S.treeFell) {
            if (fall) FellHitTrees(c);
            else if (!trees.empty() || treeScan != 0) DropTrees();
        }
    }
}

bool Action(const std::string& id, const Ctx& c) {
    if (id != "finish_blueprints" || !S.finish) return false;
    if (!c.inWorld || !c.player || !c.cheats) return true;
    if (c.session == core::Session::Client && !S.guestAdd) {
        Note("That only works when you host or play alone");
        return true;
    }
    StartFinish(c);
    return true;
}

void Release(const Ctx&) {
    patch::Release(scsInfinite, R.scsInfinite);
    patch::Release(noCuttings, R.scsNoCuttings);
    patch::Release(heldInfinite, R.heldInfinite);
    patch::Release(instantBuild, R.scsInstantBuild);
    DropFlag(oneHitTrees, R.chOneHit);
    ForceStatic(R.tcOneHitFell, false, treeFall);
    DropTrees();
    DropJob(job);
    DropGuestFill();
    RestoreReuse();
    RestoreCaps();
    DropPrinters();
}

}

const Group& BuildingGroup() {
    static Group g{"Building", &Resolve, &Supported, &Tick, &Action, &Release, nullptr};
    return g;
}

}
