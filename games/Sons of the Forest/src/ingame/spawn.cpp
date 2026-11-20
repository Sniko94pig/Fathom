#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include "ingame/unity.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdint>
#include <deque>
#include <initializer_list>
#include <mutex>
#include <random>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace sotf {

namespace {

using core::Item;
using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;
using un::Quat;
using un::Vec3;

const char* kSons = "Sons";
const char* kItemAsm = "Sons.Item";
const char* kVailAsm = "Sons.Ai.Vail";
const char* kCoreAsm = "UnityEngine.CoreModule";
const char* kGolfCart = "golf_cart";

const std::vector<std::string> kItemCats = {"Weapons", "Ammo", "Weapon mods", "Clothing & armor", "Food & medicine", "Resources", "Tools",
                                            "Other"};
const std::vector<std::string> kWorldCats = {"Vehicles", "Weapons", "Ammo", "Weapon mods", "Clothing & armor", "Food & medicine",
                                             "Resources", "Tools", "Other"};
const std::vector<std::string> kCreatureCats = {"Cannibals", "Mutants", "Animals", "Water animals", "Other"};

struct NullableVec3 {
    uint8_t has = 0;
    uint8_t pad[3] = {0, 0, 0};
    float x = 0, y = 0, z = 0;
};
static_assert(sizeof(NullableVec3) == 16, "Nullable<Vector3> is 16 bytes");

struct Refs {
    Class* localPlayer = nullptr;
    Fld lpInventory, lpStats, lpCamTr;
    Method *lpGetInventory = nullptr, *lpInCaves = nullptr;
    Class* inventory = nullptr;
    Fld invView;
    Method *invAdd = nullptr, *invSpace = nullptr, *invSetNew = nullptr, *invRightHand = nullptr;
    Class* itemInstance = nullptr;
    Fld iiId, iiData;
    Class* stats = nullptr;
    Method *statsDead = nullptr, *statsDying = nullptr;
    Class* cutscenes = nullptr;
    Method* cutscenePlaying = nullptr;
    Class *dbManager = nullptr, *database = nullptr, *itemData = nullptr, *uiData = nullptr, *types = nullptr, *guiType = nullptr;
    Fld dbInstance, dbList;
    Fld idId, idName, idType, idGui, idMax, idCanSpawn, idExclude, idNoStore, idDropUnequip, idLegacy, idPrefab, idUi;
    Fld uiIcon, uiTitle;
    Method* uiGetTitle = nullptr;
    Class* itemUtils = nullptr;
    Method *spawnPickup = nullptr, *validForMode = nullptr;
    Class *renderTexture = nullptr, *graphics = nullptr, *texture2d = nullptr, *imageConversion = nullptr;
    Method *rtGet = nullptr, *rtRelease = nullptr, *rtActiveGet = nullptr, *rtActiveSet = nullptr, *blit = nullptr, *tex2dCtor = nullptr,
           *readPixels = nullptr, *encodePng = nullptr;
    Class* golfCart = nullptr;
    Fld cartRb, cartActors;
    Method *cartHasPlayer = nullptr, *cartUpdateActivator = nullptr;
    Method *rbSetPos = nullptr, *rbSetRot = nullptr, *rbSetAngular = nullptr;
    Class* terrain = nullptr;
    Method* terrainHeight = nullptr;
    Class *charManager = nullptr, *charDefinition = nullptr;
    Fld cmInstance, cmDefs, defId;
    Method *cmAdd = nullptr, *defType = nullptr;
    Class *vailTypes = nullptr, *actorTypeId = nullptr, *actorClassId = nullptr;
    Method* actorClassOf = nullptr;
    Class *worldSim = nullptr, *worldActor = nullptr, *actorManager = nullptr;
    Fld simInstance, simActors, waType, waUid, waDebug, waRemoved;
    Method *simRemove = nullptr, *vamFind = nullptr, *vamRemove = nullptr;
} R;

struct Values {
    int equippable = 1, clothing = 2, crafting = 4, edible = 8, ammo = 32, projectile = 64, ranged = 512, weapon = 4096, armor = 16384,
        axe = 32768;
    int guiWeapon = 1, guiArmor = 2, guiAmmo = 3, guiMod = 4, guiThrown = 5;
    int viewPlaneCrash = -1, viewLoading = 0, viewWakingUp = 1, viewWorld = 2, viewDeath = 7, viewEnd = 12;
    int classCannibal = 1, classCreepy = 2, classAnimal = 3;
    int artifact = 707, emergencyPack = 483, knightV = 630, glider = 626;
} K;

std::unordered_set<int> skipTypes, waterTypes;
std::vector<std::pair<std::string, int>> defaultLoadout;
bool canCatalog = false, canAdd = false, canAddAll = false, canWorld = false, canCart = false, canCreature = false, canRemove = false,
     canIcons = false;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;

enum class Act { Add, World, Creature, Duplicate, Loadout };

struct Request {
    Act type = Act::Add;
    std::string id;
    int amount = 1;
    std::vector<std::pair<std::string, int>> loadout;
};

std::mutex qLock;
std::vector<Request> requests;

struct Info {
    int id = 0;
    size_t index = 0;
    std::string name, category;
    int maxAmount = 0;
    bool noStore = false, prefab = false, story = false, canSpawn = true, dropUnequip = false, legacy = false, valid = true, icon = false;
};

std::unordered_map<int, Info> info;
std::vector<Info> building;
int catStage = 0;
size_t catNext = 0, catTotal = 0;
bool catReady = false;

std::vector<Item> pendItems, pendWorld, pendCreatures, lastItems, lastWorld;
bool pubItems = false, pubCreatures = false;

std::vector<int> iconQueue;
size_t iconNext = 0;
bool iconsQueued = false, iconsDone = false, iconDirMade = false;
int iconsTried = 0, iconsWritten = 0;

std::vector<Item> creatureBuild;
size_t creatureNext = 0;
bool creaturesBuilt = false;
std::unordered_map<std::string, std::string> creatureNames;
std::unordered_map<int, std::string> typeNames;

struct AddJob {
    bool all = false;
    std::vector<std::pair<int, int>> entries;
    size_t next = 0;
    int given = 0;
    int already = 0;
    int unknown = 0;
    std::vector<std::string> failed;
};
std::deque<AddJob> addJobs;

struct SpawnJob {
    int id = 0;
    int left = 0;
    int done = 0;
    bool vehicle = false;
    double start = 0;
    std::string name;
};
std::deque<SpawnJob> spawnJobs;

std::unordered_set<int> mine, known;
double claimUntil = 0, nextClaim = 0;

std::mt19937 rng;
bool seeded = false;

float Rand01() {
    if (!seeded) {
        rng.seed(static_cast<unsigned>(GetTickCount64()));
        seeded = true;
    }
    return std::uniform_real_distribution<float>(0.0f, 1.0f)(rng);
}

std::string Trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) ++a;
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}

std::string Lower(std::string s) {
    for (char& ch : s) ch = (char)std::tolower((unsigned char)ch);
    return s;
}

std::string Pretty(const std::string& raw) {
    std::string s;
    for (size_t i = 0; i < raw.size(); ++i) {
        unsigned char ch = (unsigned char)raw[i];
        if (ch == '_' || ch == '-') ch = ' ';
        if (ch == ' ') {
            if (!s.empty() && s.back() != ' ') s += ' ';
            continue;
        }
        if (i > 0 && !s.empty() && s.back() != ' ') {
            unsigned char p = (unsigned char)raw[i - 1];
            if ((std::isupper(ch) && std::islower(p)) || (std::isdigit(ch) && std::isalpha(p))) s += ' ';
        }
        s += (char)ch;
    }
    s = Trim(s);
    if (!s.empty()) s[0] = (char)std::toupper((unsigned char)s[0]);
    return s;
}

bool LooksLikeKey(const std::string& s) {
    if (s.empty()) return true;
    if (s.rfind("No translation", 0) == 0) return true;
    bool lower = false, underscore = false, space = false;
    for (char ch : s) {
        if (std::islower((unsigned char)ch)) lower = true;
        if (ch == '_') underscore = true;
        if (ch == ' ') space = true;
    }
    return underscore && !lower && !space;
}

int ParseId(const std::string& s) {
    int v = 0;
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc() && r.ptr == s.data() + s.size() ? v : 0;
}

bool Flag(Object* o, const Fld& f) { return un::Val<uint8_t>(o, f, 0) != 0; }

Quat Yaw(float angle) {
    Quat q;
    q.x = 0;
    q.y = std::sin(angle * 0.5f);
    q.z = 0;
    q.w = std::cos(angle * 0.5f);
    return q;
}

Quat Facing(Vec3 fwd) { return Yaw(std::atan2(fwd.x, fwd.z)); }

Vec3 FlatForward(Object* tr) {
    Vec3 f = un::Norm(un::Flat(un::Forward(tr)));
    if (un::Len(f) < 0.5f) f = Vec3{0, 0, 1};
    return f;
}

Item MakeItem(const std::string& id, const std::string& name, const std::string& category, bool hidden) {
    Item it;
    it.id = id;
    it.name = name;
    it.category = category;
    it.hidden = hidden;
    it.real = true;
    return it;
}

void SortItems(std::vector<Item>& v, const std::vector<std::string>& order) {
    auto rank = [&](const std::string& c) { return (size_t)(std::find(order.begin(), order.end(), c) - order.begin()); };
    std::stable_sort(v.begin(), v.end(), [&](const Item& a, const Item& b) {
        size_t ra = rank(a.category), rb = rank(b.category);
        if (ra != rb) return ra < rb;
        std::string la = Lower(a.name), lb = Lower(b.name);
        if (la != lb) return la < lb;
        return a.id < b.id;
    });
}

bool Same(const std::vector<Item>& a, const std::vector<Item>& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].id != b[i].id || a[i].name != b[i].name || a[i].category != b[i].category || a[i].hidden != b[i].hidden) return false;
    return true;
}

std::string NameOf(int id) {
    auto it = info.find(id);
    if (it != info.end() && !it->second.name.empty()) return it->second.name;
    return "item " + std::to_string(id);
}

std::string CreatureName(const std::string& id) {
    auto it = creatureNames.find(id);
    return it != creatureNames.end() ? it->second : Pretty(id);
}

void Push(Request r) {
    std::lock_guard<std::mutex> g(qLock);
    if (requests.size() < 64) requests.push_back(std::move(r));
}

Object* Inventory(const Ctx& c) {
    Object* inv = un::Ref(c.player, R.lpInventory);
    if (!un::Alive(inv) && R.lpGetInventory) inv = un::Call(R.lpGetInventory, nullptr);
    return un::Alive(inv) ? inv : nullptr;
}

Object* CameraTr(const Ctx& c) {
    Object* t = un::Ref(c.player, R.lpCamTr);
    if (un::Alive(t)) return t;
    if (Object* cam = un::MainCamera()) return un::TransformOf(cam);
    return un::TransformOf(c.player);
}

const char* Blocked(const Ctx& c) {
    if (!c.inWorld || !c.player) return "Load a save first";
    if (c.session == core::Session::Client && !c.cheats) return "The host has cheats switched off";
    Object* st = un::Ref(c.player, R.lpStats);
    if (un::Alive(st)) {
        if (R.statsDead && un::CallBool(R.statsDead, st)) return "Not while you are dead";
        if (R.statsDying && un::CallBool(R.statsDying, st)) return "Not while you are knocked down";
    }
    if (R.cutscenePlaying && un::CallBool(R.cutscenePlaying, nullptr)) return "Wait until the cutscene ends";
    Object* inv = Inventory(c);
    if (!inv) return "Wait until you can move again";
    if (R.invView) {
        int v = un::Val<int>(inv, R.invView, K.viewWorld);
        if (v == K.viewPlaneCrash || v == K.viewLoading || v == K.viewWakingUp || v == K.viewDeath || v == K.viewEnd)
            return "Wait until you can move again";
    }
    return nullptr;
}

std::vector<Object*> DatabaseItems() {
    Object* db = un::Static(R.dbInstance);
    if (!un::Alive(db)) return {};
    return il2::ListItems(un::Ref(db, R.dbList));
}

Object* ItemDataById(int id) {
    if (id <= 0) return nullptr;
    std::vector<Object*> all = DatabaseItems();
    auto it = info.find(id);
    if (it != info.end() && it->second.index < all.size()) {
        Object* d = all[it->second.index];
        if (un::Alive(d) && un::Val<int>(d, R.idId, -1) == id) return d;
    }
    for (Object* d : all)
        if (un::Alive(d) && un::Val<int>(d, R.idId, -1) == id) return d;
    return nullptr;
}

std::string CategoryOf(int type, int gui) {
    bool weapon = (type & (K.weapon | K.ranged | K.axe)) != 0 || gui == K.guiWeapon || gui == K.guiThrown;
    if (gui == K.guiAmmo || (type & K.ammo) != 0 || ((type & K.projectile) != 0 && !weapon)) return "Ammo";
    if (gui == K.guiMod) return "Weapon mods";
    if (weapon) return "Weapons";
    if (gui == K.guiArmor || (type & (K.armor | K.clothing)) != 0) return "Clothing & armor";
    if ((type & K.edible) != 0) return "Food & medicine";
    if ((type & K.crafting) != 0) return "Resources";
    if ((type & K.equippable) != 0) return "Tools";
    return "Other";
}

Info ReadItem(Object* d) {
    Info in;
    in.id = un::Val<int>(d, R.idId, 0);
    std::string dev = Trim(il2::Str(un::Ref(d, R.idName)));
    int type = un::Val<int>(d, R.idType, 0);
    int gui = un::Val<int>(d, R.idGui, 0);
    in.maxAmount = un::Val<int>(d, R.idMax, 0);
    in.canSpawn = !R.idCanSpawn || Flag(d, R.idCanSpawn);
    in.story = Flag(d, R.idExclude);
    in.noStore = Flag(d, R.idNoStore);
    in.dropUnequip = Flag(d, R.idDropUnequip);
    in.legacy = Flag(d, R.idLegacy);
    in.prefab = un::Alive(un::Ref(d, R.idPrefab));
    Object* ui = un::Ref(d, R.idUi);
    std::string title;
    if (ui && R.uiGetTitle) title = Trim(il2::Str(un::Call(R.uiGetTitle, ui)));
    if (ui && LooksLikeKey(title)) title = Trim(il2::Str(un::Ref(ui, R.uiTitle)));
    if (LooksLikeKey(title)) title.clear();
    in.name = title.empty() ? Pretty(dev) : title;
    in.icon = ui && un::Alive(un::Ref(ui, R.uiIcon));
    in.valid = !R.validForMode || un::CallBool(R.validForMode, nullptr, {d}, true);
    in.category = CategoryOf(type, gui);
    return in;
}

void FinishCatalog() {
    catStage = 2;
    std::unordered_map<int, Info> map;
    std::vector<Item> items, world;
    for (Info& in : building) {
        if (map.count(in.id)) continue;
        std::string sid = std::to_string(in.id);
        bool hidden = in.story || in.id == K.artifact || !in.valid || in.legacy || !in.canSpawn || in.name.empty();
        if (!in.noStore && in.maxAmount > 0) items.push_back(MakeItem(sid, in.name, in.category, hidden));
        if (in.prefab && canWorld) {
            bool vehicle = in.id == K.knightV || in.id == K.glider;
            world.push_back(MakeItem(sid, in.name, vehicle ? "Vehicles" : in.category, vehicle ? false : hidden));
        }
        map.emplace(in.id, in);
    }
    if (canCart) world.push_back(MakeItem(kGolfCart, "Golf cart (bring nearest)", "Vehicles", false));
    building.clear();
    SortItems(items, kItemCats);
    SortItems(world, kWorldCats);
    info = std::move(map);
    catReady = true;
    if (!iconsQueued) {
        iconsQueued = true;
        for (const auto& [id, in] : info)
            if (in.icon) iconQueue.push_back(id);
        std::sort(iconQueue.begin(), iconQueue.end());
    }
    LOG("item catalog: %d items, %d in your inventory list, %d in the world list", (int)info.size(), (int)items.size(), (int)world.size());
    if (Same(items, lastItems) && Same(world, lastWorld)) {
        pendItems.clear();
        pendWorld.clear();
        pubItems = false;
        return;
    }
    pendItems = std::move(items);
    pendWorld = std::move(world);
    pubItems = true;
}

void StepCatalog() {
    if (!canCatalog || catStage == 2) return;
    std::vector<Object*> all = DatabaseItems();
    if (all.empty()) return;
    if (catStage == 0 || catTotal != all.size()) {
        catStage = 1;
        catNext = 0;
        catTotal = all.size();
        building.clear();
    }
    auto t0 = std::chrono::steady_clock::now();
    size_t end = std::min(all.size(), catNext + 40);
    while (catNext < end) {
        Object* d = all[catNext];
        size_t index = catNext++;
        if (un::Alive(d)) {
            Info in = ReadItem(d);
            in.index = index;
            if (in.id > 0) building.push_back(std::move(in));
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(4)) break;
    }
    if (catNext >= all.size()) FinishCatalog();
}

std::string TypeName(int type) {
    auto it = typeNames.find(type);
    if (it != typeNames.end()) return it->second;
    std::string name = R.actorTypeId ? Pretty(il2::EnumName(R.actorTypeId, type)) : std::string();
    typeNames[type] = name;
    return name;
}

std::string CreatureCategory(int type, int cls) {
    if (waterTypes.count(type)) return "Water animals";
    if (cls == K.classCannibal) return "Cannibals";
    if (cls == K.classCreepy) return "Mutants";
    if (cls == K.classAnimal) return "Animals";
    return "Other";
}

void StepCreatureList() {
    if (!canCreature || creaturesBuilt) return;
    Object* cm = un::Static(R.cmInstance);
    if (!un::Alive(cm)) return;
    std::vector<Object*> defs = il2::ListItems(un::Ref(cm, R.cmDefs));
    if (defs.empty()) return;
    if (creatureNext > defs.size()) {
        creatureNext = 0;
        creatureBuild.clear();
    }
    size_t end = std::min(defs.size(), creatureNext + 16);
    for (; creatureNext < end; ++creatureNext) {
        Object* d = defs[creatureNext];
        if (!un::Alive(d)) continue;
        std::string id = Trim(il2::Str(un::Ref(d, R.defId)));
        if (id.empty() || id.find(' ') != std::string::npos) continue;
        int type = un::CallInt(R.defType, d, {}, 0);
        if (type <= 0 || skipTypes.count(type)) continue;
        int cls = R.actorClassOf ? un::CallInt(R.actorClassOf, nullptr, {&type}, 0) : 0;
        std::string name = TypeName(type);
        if (name.empty()) name = Pretty(id);
        creatureBuild.push_back(MakeItem(id, name, CreatureCategory(type, cls), false));
    }
    if (creatureNext < defs.size()) return;
    creaturesBuilt = true;
    std::unordered_map<std::string, int> uses;
    std::unordered_set<std::string> ids;
    std::vector<Item> list;
    for (const Item& it : creatureBuild)
        if (ids.insert(it.id).second) {
            list.push_back(it);
            ++uses[it.name];
        }
    creatureBuild.clear();
    for (Item& it : list) {
        if (uses[it.name] > 1) it.name += " (" + it.id + ")";
        creatureNames[it.id] = it.name;
    }
    SortItems(list, kCreatureCats);
    LOG("creature list: %d kinds", (int)list.size());
    pendCreatures = std::move(list);
    pubCreatures = true;
}

void Publish() {
    if ((!pubItems && !pubCreatures) || !model || !modelLock) return;
    std::vector<Item> items, world, creatures;
    bool doItems = pubItems, doCreatures = pubCreatures;
    if (doItems) {
        items = pendItems;
        world = pendWorld;
    }
    if (doCreatures) creatures = pendCreatures;
    {
        std::unique_lock<std::mutex> g(*modelLock, std::try_to_lock);
        if (!g.owns_lock()) return;
        if (doItems) {
            model->items.swap(items);
            model->worldItems.swap(world);
        }
        if (doCreatures) model->creatureItems.swap(creatures);
    }
    if (doItems) {
        lastItems = std::move(pendItems);
        lastWorld = std::move(pendWorld);
        pendItems.clear();
        pendWorld.clear();
        pubItems = false;
    }
    if (doCreatures) {
        pendCreatures.clear();
        pubCreatures = false;
    }
}

bool WriteBytes(const std::wstring& path, const void* data, size_t n) {
    std::wstring tmp = path + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(h, data, (DWORD)n, &written, nullptr);
    CloseHandle(h);
    if (!ok || written != (DWORD)n || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

bool SameNative(Object* a, Object* b) {
    if (!un::Alive(a) || !un::Alive(b)) return false;
    return il2::Get<void*>(a, un::E.cachedPtr.off) == il2::Get<void*>(b, un::E.cachedPtr.off);
}

bool ExportIcon(int id, const std::wstring& path) {
    Object* d = ItemDataById(id);
    Object* ui = d ? un::Ref(d, R.idUi) : nullptr;
    Object* tex = ui ? un::Ref(ui, R.uiIcon) : nullptr;
    if (!un::Alive(tex)) return false;
    int size = 64;
    Object* rt = un::Call(R.rtGet, nullptr, {&size, &size});
    if (!un::Alive(rt)) return false;
    Object* prev = un::Call(R.rtActiveGet, nullptr);
    bool blitted = false;
    un::Call(R.blit, nullptr, {tex, rt}, &blitted);
    un::Call(R.rtActiveSet, nullptr, {rt});
    bool active = SameNative(un::Call(R.rtActiveGet, nullptr), rt);
    Object* t2 = nullptr;
    bool made = false, wrote = false;
    if (blitted && active) {
        t2 = il2::New(R.texture2d);
        if (t2) un::Call(R.tex2dCtor, t2, {&size, &size}, &made);
    }
    if (made) {
        float rect[4] = {0.0f, 0.0f, (float)size, (float)size};
        int zero = 0;
        bool read = false;
        un::Call(R.readPixels, t2, {rect, &zero, &zero}, &read);
        Object* png = read ? un::Call(R.encodePng, nullptr, {t2}) : nullptr;
        size_t n = il2::Length(png);
        if (n >= 300) wrote = WriteBytes(path, il2::Data(png), n);
    }
    un::Call(R.rtActiveSet, nullptr, {un::Alive(prev) ? prev : nullptr});
    un::Call(R.rtRelease, nullptr, {rt});
    if (made) un::Destroy(t2);
    return wrote;
}

void StepIcons() {
    if (!iconsQueued || iconsDone) return;
    if (!canIcons || Folder().empty()) {
        iconsDone = true;
        return;
    }
    std::wstring dir = Folder() + L"\\icons";
    if (!iconDirMade) {
        CreateDirectoryW(dir.c_str(), nullptr);
        iconDirMade = true;
    }
    auto t0 = std::chrono::steady_clock::now();
    int work = 0, checks = 0;
    while (iconNext < iconQueue.size() && work < 4 && checks < 96) {
        int id = iconQueue[iconNext++];
        ++checks;
        std::wstring path = dir + L"\\" + std::to_wstring(id) + L".png";
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ++work;
            ++iconsTried;
            if (ExportIcon(id, path)) ++iconsWritten;
        }
        if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(5)) break;
    }
    bool gaveUp = iconsTried >= 24 && iconsWritten == 0;
    if (iconNext < iconQueue.size() && !gaveUp) return;
    iconsDone = true;
    if (gaveUp) LOG("item icons could not be saved, the menu shows category icons instead");
    else LOG("item icons ready (%d new)", iconsWritten);
    if (iconsWritten > 0) overlay::LoadAtlas(dir);
}

int Give(Object* inv, int id, int want) {
    if (!inv || !R.invAdd || !R.invSpace || id <= 0 || want <= 0) return -1;
    bool ok = false;
    Object* r = un::Call(R.invSpace, inv, {&id}, &ok);
    if (!ok) return -1;
    int space = il2::Result<int>(r, 0);
    if (space <= 0) return 0;
    int n = std::min(want, space);
    WarnAchievementsOnce();
    if (id == K.artifact) {
        static bool told = false;
        if (!told) {
            told = true;
            Note("The finished Artifact also unlocks it in Creative mode for all your saves");
        }
    }
    if (R.invSetNew) {
        bool isNew = false;
        un::Call(R.invSetNew, inv, {&id, &isNew});
    }
    bool prevent = true, crafted = false;
    bool added = un::CallBool(R.invAdd, inv, {&id, &n, &prevent, &crafted, nullptr}, false);
    return added ? n : -1;
}

void AddOne(const Ctx& c, const std::string& sid, int amount) {
    int id = ParseId(sid);
    Object* inv = Inventory(c);
    if (id <= 0 || !inv) {
        Note("That item can't be added");
        return;
    }
    int want = std::clamp(amount, 1, 999);
    int got = Give(inv, id, want);
    if (got < 0) Note("The game refused to add " + NameOf(id));
    else if (got == 0) Note("No room for more " + NameOf(id));
    else if (got < want) Note("Only room for " + std::to_string(got) + " " + NameOf(id));
}

void StartLoadout(const std::vector<std::pair<std::string, int>>& lo) {
    if (lo.empty()) {
        Note("Your loadout is empty");
        return;
    }
    if (addJobs.size() >= 4) {
        Note("Still giving items, wait a moment");
        return;
    }
    AddJob j;
    for (const auto& e : lo) {
        int id = ParseId(e.first);
        if (id > 0) j.entries.push_back({id, std::clamp(e.second, 1, 999)});
        else ++j.unknown;
    }
    addJobs.push_back(std::move(j));
}

void StartAddAll(const Ctx& c) {
    if (const char* why = Blocked(c)) {
        Note(why);
        return;
    }
    if (!catReady) {
        Note("The item list is still loading, try again in a moment");
        return;
    }
    for (const AddJob& j : addJobs)
        if (j.all) {
            Note("Already adding all items");
            return;
        }
    AddJob j;
    j.all = true;
    for (const auto& [id, in] : info) {
        if (in.maxAmount <= 0 || in.story || !in.canSpawn || in.dropUnequip || in.noStore || in.legacy || !in.valid) continue;
        if (id == K.emergencyPack || id == K.artifact) continue;
        j.entries.push_back({id, INT_MAX});
    }
    std::sort(j.entries.begin(), j.entries.end());
    if (j.entries.empty()) {
        Note("No items to add");
        return;
    }
    addJobs.push_back(std::move(j));
}

void FinishAddJob(const AddJob& j) {
    if (j.all) {
        Note(j.given > 0 ? "Added " + std::to_string(j.given) + " different items" : "Your inventory already has everything");
        return;
    }
    if (!j.failed.empty()) {
        std::string text = "Could not give " + j.failed[0];
        for (size_t i = 1; i < j.failed.size() && i < 3; ++i) text += ", " + j.failed[i];
        if (j.failed.size() > 3) text += " and " + std::to_string(j.failed.size() - 3) + " more";
        Note(text);
    } else if (j.given == 0 && j.already > 0) {
        Note("You already carry as much as fits of everything in your loadout");
    } else if (j.given == 0) {
        Note("Nothing from your loadout could be given");
    }
    if (j.unknown > 0) Note(std::to_string(j.unknown) + " loadout entries are not items in this game");
}

void StepAddJobs(const Ctx& c) {
    if (addJobs.empty()) return;
    if (const char* why = Blocked(c)) {
        Note(why);
        addJobs.clear();
        return;
    }
    Object* inv = Inventory(c);
    if (!inv) return;
    AddJob& j = addJobs.front();
    int budget = 20;
    while (budget-- > 0 && j.next < j.entries.size()) {
        auto [id, amount] = j.entries[j.next++];
        int got = Give(inv, id, amount);
        if (got > 0) ++j.given;
        else if (got == 0) ++j.already;
        else if (!j.all) j.failed.push_back(NameOf(id));
    }
    if (j.next < j.entries.size()) return;
    FinishAddJob(j);
    addJobs.pop_front();
}

bool RayGround(Vec3 p, float fromY, float& y) {
    un::RayHit hit;
    Vec3 origin{p.x, fromY, p.z};
    Vec3 down{0, -1, 0};
    if (!un::Raycast(origin, down, 60.0f, hit, ~0, 1)) return false;
    y = hit.point.y;
    return true;
}

float CartGround(Vec3 p, float eyeY) {
    float y = 0;
    if (RayGround(p, eyeY + 0.5f, y)) return y;
    if (R.terrainHeight) {
        bool ok = false;
        Object* r = un::Call(R.terrainHeight, nullptr, {&p}, &ok);
        if (ok) return il2::Result<float>(r, eyeY - 1.6f);
    }
    return eyeY - 1.6f;
}

bool ActorsAboard(Object* cart) {
    Object* arr = un::Ref(cart, R.cartActors);
    size_t n = il2::Length(arr);
    for (size_t i = 0; i < n; ++i)
        if (un::Alive(il2::At(arr, i))) return true;
    return false;
}

void BringCart(const Ctx& c) {
    if (c.session == core::Session::Client) {
        Note("Only the host can move golf carts");
        return;
    }
    if (!canCart) {
        Note("Golf carts are not available");
        return;
    }
    if (R.lpInCaves && un::CallBool(R.lpInCaves, nullptr)) {
        Note("Golf carts can't come into caves");
        return;
    }
    Object* cam = CameraTr(c);
    if (!cam) return;
    Vec3 eye = un::Position(cam);
    Vec3 fwd = FlatForward(cam);
    Object* best = nullptr;
    float bestD = 1e30f;
    for (Object* cart : un::FindAll(R.golfCart)) {
        if (R.cartHasPlayer && un::CallBool(R.cartHasPlayer, cart)) continue;
        if (ActorsAboard(cart)) continue;
        float d = un::Dist(un::Position(cart), eye);
        if (d < bestD) {
            bestD = d;
            best = cart;
        }
    }
    if (!best) {
        Note("No free golf cart was found. The game can't make new ones");
        return;
    }
    Vec3 target = un::Add(eye, un::Mul(fwd, 5.0f));
    target.y = CartGround(target, eye.y) + 1.0f;
    Quat rot = Facing(fwd);
    Object* rb = un::Ref(best, R.cartRb);
    if (!un::Alive(rb)) rb = un::Comp(best, un::E.rigidbody);
    Vec3 zero{};
    if (rb) {
        bool kinematic = un::E.rbGetKinematic && un::CallBool(un::E.rbGetKinematic, rb);
        if (!kinematic) {
            if (un::E.rbSetVelocity) un::Call(un::E.rbSetVelocity, rb, {&zero});
            if (R.rbSetAngular) un::Call(R.rbSetAngular, rb, {&zero});
        }
        if (R.rbSetPos) un::Call(R.rbSetPos, rb, {&target});
        if (R.rbSetRot) un::Call(R.rbSetRot, rb, {&rot});
    }
    Object* body = rb ? rb : best;
    un::SetPosition(body, target);
    un::SetRotation(body, rot);
    if (R.cartUpdateActivator) un::Call(R.cartUpdateActivator, best);
    LOG("golf cart moved %.0f m", bestD);
    Note("Brought a golf cart from " + std::to_string((int)bestD) + " m away");
}

bool SpawnUnit(const Ctx& c, Object* d, const SpawnJob& j) {
    Object* cam = CameraTr(c);
    if (!cam || !R.spawnPickup) return false;
    Vec3 eye = un::Position(cam);
    Vec3 fwd = FlatForward(cam);
    Vec3 pos = un::Add(eye, un::Mul(fwd, j.vehicle ? 3.5f : 2.0f));
    Quat rot;
    if (j.vehicle) {
        rot = Facing(fwd);
    } else {
        float a = Rand01() * 6.2831853f, r = std::sqrt(Rand01()) * 0.35f;
        pos.x += std::cos(a) * r;
        pos.z += std::sin(a) * r;
        rot = Yaw(Rand01() * 6.2831853f);
    }
    pos.y = eye.y - (j.vehicle ? 0.6f : 0.3f);
    float ground = 0;
    if (RayGround(pos, eye.y + 0.2f, ground)) pos.y = std::max(pos.y, ground + (j.vehicle ? 1.0f : 0.4f));
    if (!j.vehicle) pos.y += (float)(j.done % 4) * 0.15f;
    Vec3 zero{};
    bool no = false, yes = true;
    int one = 1;
    NullableVec3 noVelocity, noSpin;
    bool ok = false;
    Object* go = un::Call(R.spawnPickup, nullptr,
                          {d, &pos, &rot, &zero, &no, &yes, &yes, &no, nullptr, nullptr, &one, &noVelocity, &noSpin, &yes, &no}, &ok);
    return go != nullptr || (ok && c.session == core::Session::Client);
}

void QueueWorld(const Ctx& c, const std::string& sid, int amount) {
    if (sid == kGolfCart) {
        BringCart(c);
        return;
    }
    int id = ParseId(sid);
    if (id <= 0) return;
    if (!canWorld) {
        Note("Spawning items is not available");
        return;
    }
    if (spawnJobs.size() >= 16) {
        Note("Still spawning, wait a moment");
        return;
    }
    SpawnJob j;
    j.id = id;
    j.vehicle = id == K.knightV || id == K.glider;
    j.left = std::clamp(amount, 1, j.vehicle ? 5 : 50);
    j.start = c.now;
    j.name = NameOf(id);
    spawnJobs.push_back(std::move(j));
}

void StepSpawnJobs(const Ctx& c) {
    if (spawnJobs.empty()) return;
    if (const char* why = Blocked(c)) {
        Note(why);
        spawnJobs.clear();
        return;
    }
    int budget = 2;
    while (budget > 0 && !spawnJobs.empty()) {
        SpawnJob& j = spawnJobs.front();
        if (c.now - j.start > 20.0) {
            Note("Stopped spawning " + j.name);
            spawnJobs.pop_front();
            continue;
        }
        Object* d = ItemDataById(j.id);
        if (!d || !SpawnUnit(c, d, j)) {
            Note("Could not spawn " + j.name);
            spawnJobs.pop_front();
            continue;
        }
        WarnAchievementsOnce();
        ++j.done;
        --j.left;
        --budget;
        if (j.left <= 0) spawnJobs.pop_front();
    }
}

void Duplicate(const Ctx& c) {
    Object* inv = Inventory(c);
    Object* held = inv && R.invRightHand ? un::Call(R.invRightHand, inv) : nullptr;
    int id = held ? un::Val<int>(held, R.iiId, 0) : 0;
    if (held && id <= 0) {
        Object* d = un::Ref(held, R.iiData);
        if (un::Alive(d)) id = un::Val<int>(d, R.idId, 0);
    }
    if (id <= 0) {
        Note("Hold an item in your hand first");
        return;
    }
    auto it = info.find(id);
    bool storable = it == info.end() || (!it->second.noStore && it->second.maxAmount > 0);
    if (!storable) {
        if (canWorld && it->second.prefab) QueueWorld(c, std::to_string(id), 1);
        else Note(NameOf(id) + " can't be copied");
        return;
    }
    int got = Give(inv, id, 1);
    if (got == 0) Note("No room for another " + NameOf(id));
    else if (got < 0) Note("The game refused to copy " + NameOf(id));
}

template <class Fn>
void ForEachActor(Fn fn) {
    Object* sim = un::Static(R.simInstance);
    if (!un::Alive(sim)) return;
    for (Object* a : il2::ListItems(un::Ref(sim, R.simActors)))
        if (a) fn(a);
}

void BeginClaim(double now) {
    if (!canRemove) return;
    if (now > claimUntil) {
        known.clear();
        ForEachActor([](Object* a) {
            if (Flag(a, R.waDebug)) known.insert(un::Val<int>(a, R.waUid, -1));
        });
    }
    claimUntil = now + 3.0;
}

void Claim() {
    if (!canRemove || claimUntil <= 0) return;
    ForEachActor([](Object* a) {
        if (!Flag(a, R.waDebug) || Flag(a, R.waRemoved)) return;
        int uid = un::Val<int>(a, R.waUid, -1);
        if (uid < 0 || known.count(uid)) return;
        if (R.waType && skipTypes.count(un::Val<int>(a, R.waType, 0))) return;
        mine.insert(uid);
    });
}

void StepClaims(double now) {
    if (claimUntil <= 0) return;
    if (now > claimUntil) {
        Claim();
        claimUntil = 0;
        known.clear();
        return;
    }
    if (now < nextClaim) return;
    nextClaim = now + 0.25;
    Claim();
}

void SpawnCreature(const Ctx& c, const std::string& id, int amount) {
    if (c.session == core::Session::Client) {
        Note("Only the host can spawn creatures");
        return;
    }
    if (!canCreature) {
        Note("Spawning creatures is not available");
        return;
    }
    Object* cm = un::Static(R.cmInstance);
    if (!un::Alive(cm)) {
        Note("Creatures can't be spawned here");
        return;
    }
    if (id.empty() || id.find(' ') != std::string::npos) return;
    int n = std::clamp(amount, 1, 20);
    BeginClaim(c.now);
    std::string args = id + " " + std::to_string(n);
    bool instant = false;
    bool ok = un::CallBool(R.cmAdd, cm, {il2::NewString(args.c_str()), &instant}, false);
    Claim();
    if (!ok) {
        Note("Could not spawn " + CreatureName(id) + " here");
        return;
    }
    WarnAchievementsOnce();
}

int RemoveMine() {
    Object* sim = un::Static(R.simInstance);
    if (!un::Alive(sim)) return -1;
    std::vector<uint32_t> held;
    for (Object* a : il2::ListItems(un::Ref(sim, R.simActors)))
        if (a && !Flag(a, R.waRemoved) && mine.count(un::Val<int>(a, R.waUid, -1))) held.push_back(il2::Pin(a));
    int n = 0;
    for (uint32_t h : held) {
        Object* a = il2::Pinned(h);
        if (a && !Flag(a, R.waRemoved)) {
            Object* va = R.vamFind ? un::Call(R.vamFind, nullptr, {a}) : nullptr;
            if (un::Alive(va) && R.vamRemove) un::Call(R.vamRemove, nullptr, {va});
            else un::Call(R.simRemove, sim, {a});
            ++n;
        }
        il2::Unpin(h);
    }
    mine.clear();
    known.clear();
    claimUntil = 0;
    return n;
}

void RemoveCreatures(const Ctx& c) {
    if (!c.inWorld || c.session == core::Session::Client) return;
    Claim();
    if (mine.empty()) {
        Note("There are no creatures you spawned to remove");
        return;
    }
    int n = RemoveMine();
    if (n < 0) Note("Creatures can't be removed right now");
    else if (n == 0) Note("The creatures you spawned are already gone");
    else Note("Removed " + std::to_string(n) + (n == 1 ? " creature" : " creatures"));
    if (n > 0) LOG("removed %d spawned creatures", n);
}

void DropRequests(const char* why) {
    bool had = false;
    {
        std::lock_guard<std::mutex> g(qLock);
        had = !requests.empty();
        requests.clear();
    }
    if (had) Note(why);
}

void RunRequests(const Ctx& c) {
    std::vector<Request> todo;
    {
        std::lock_guard<std::mutex> g(qLock);
        todo.swap(requests);
    }
    if (todo.empty()) return;
    if (const char* why = Blocked(c)) {
        Note(why);
        return;
    }
    for (Request& r : todo) {
        switch (r.type) {
        case Act::Add: AddOne(c, r.id, r.amount); break;
        case Act::World: QueueWorld(c, r.id, r.amount); break;
        case Act::Creature: SpawnCreature(c, r.id, r.amount); break;
        case Act::Duplicate: Duplicate(c); break;
        case Act::Loadout: StartLoadout(r.loadout); break;
        }
    }
}

Method* StaticM(Class* c, const char* name, int args) {
    if (!c) return nullptr;
    Method* m = il2::FindStaticMethod(c, name, args);
    if (!m) un::Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
    return m;
}

int Lit(Class* c, const char* name, int fallback) {
    if (!c) return fallback;
    long long v = il2::EnumValue(c, name, LLONG_MIN);
    if (v == LLONG_MIN) {
        un::Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
        return fallback;
    }
    return (int)v;
}

void ResolvePlayer() {
    R.localPlayer = un::C(kSons, "TheForest.Utils", "LocalPlayer");
    if (R.localPlayer) {
        R.lpInventory = un::F(R.localPlayer, "_inventory");
        R.lpStats = un::F(R.localPlayer, "_stats");
        R.lpCamTr = un::F(R.localPlayer, "_mainCamTr");
        R.lpGetInventory = StaticM(R.localPlayer, "get_Inventory", 0);
        R.lpInCaves = StaticM(R.localPlayer, "get_IsInCaves", 0);
    }
    R.inventory = un::C(kSons, "TheForest.Items.Inventory", "PlayerInventory");
    if (R.inventory) {
        R.invView = un::F(R.inventory, "_currentView");
        R.invAdd = un::M(R.inventory, "AddItem", 5);
        R.invSpace = un::M(R.inventory, "GetAvailableSpaceForItem", 1);
        R.invSetNew = un::M(R.inventory, "SetIsNewItemInInventory", 2);
        R.invRightHand = un::M(R.inventory, "get_RightHandItem", 0);
        if (Class* views = il2::Nested(R.inventory, "PlayerViews")) {
            K.viewPlaneCrash = Lit(views, "PlaneCrash", K.viewPlaneCrash);
            K.viewLoading = Lit(views, "Loading", K.viewLoading);
            K.viewWakingUp = Lit(views, "WakingUp", K.viewWakingUp);
            K.viewWorld = Lit(views, "World", K.viewWorld);
            K.viewDeath = Lit(views, "Death", K.viewDeath);
            K.viewEnd = Lit(views, "EndDeactivate", K.viewEnd);
        } else {
            un::Missing("PlayerInventory.PlayerViews");
        }
    }
    R.itemInstance = un::C(kSons, "Sons.Inventory", "ItemInstance");
    if (R.itemInstance) {
        R.iiId = un::F(R.itemInstance, "_itemID");
        R.iiData = un::F(R.itemInstance, "_data");
    }
    R.stats = un::C(kSons, "", "PlayerStats");
    if (R.stats) {
        R.statsDead = un::M(R.stats, "IsDead", 0);
        R.statsDying = un::M(R.stats, "IsDying", 0);
    }
    R.cutscenes = un::C(kSons, "Sons.Cutscenes", "CutsceneManager");
    R.cutscenePlaying = StaticM(R.cutscenes, "IsCutscenePlaying", 0);
}

void ResolveItems() {
    R.dbManager = un::C(kItemAsm, "Sons.Items.Core", "ItemDatabaseManager");
    if (R.dbManager) R.dbInstance = un::F(R.dbManager, "_instance");
    R.database = un::C(kItemAsm, "Sons.Items.Core", "ItemDatabase");
    if (R.database) {
        R.dbList = un::F(R.database, "_itemDataList");
        K.artifact = Lit(R.database, "ArtifactId", K.artifact);
        K.emergencyPack = Lit(R.database, "EmergencyPackId", K.emergencyPack);
        K.knightV = Lit(R.database, "KnightVId", K.knightV);
        K.glider = Lit(R.database, "HangGliderId", K.glider);
    }
    defaultLoadout.clear();
    for (const auto& [name, id, amount] : std::initializer_list<std::tuple<const char*, int, int>>{
             {"PistolItemId", 355, 1},  {"PistolAmmoId", 362, 999},  {"ShotgunId", 358, 1},       {"BuckshotAmmoId", 364, 999},
             {"SlugAmmoId", 363, 999},  {"GpsTrackerId", 412, 1},    {"GpsLocatorId", 529, 1},    {"FlashlightId", 471, 1},
             {"BatteriesId", 527, 999}, {"RopegunId", 522, 1},       {"ZiplineRopeId", 523, 999}, {"RebreatherId", 444, 1},
             {"AirCanisterId", 469, 999}, {"HealthMixId", 455, 999}, {"EnergyMixId", 461, 999},   {"TacticalAxeId", 379, 1},
             {"CombatKnifeId", 380, 1}})
        defaultLoadout.push_back({std::to_string(Lit(R.database, name, id)), amount});
    R.itemData = un::C(kItemAsm, "Sons.Items.Core", "ItemData");
    if (R.itemData) {
        R.idId = un::F(R.itemData, "_id");
        R.idName = un::F(R.itemData, "_name");
        R.idType = un::F(R.itemData, "_type");
        R.idGui = un::F(R.itemData, "_guiType");
        R.idMax = un::F(R.itemData, "_maxAmount");
        R.idCanSpawn = un::F(R.itemData, "_canBeSpawned");
        R.idExclude = un::F(R.itemData, "_excludeFromSpawnAll");
        R.idNoStore = un::F(R.itemData, "_canNotBeStored");
        R.idDropUnequip = un::F(R.itemData, "_alwaysDropOnUnequip");
        R.idLegacy = un::F(R.itemData, "_isLegacyItem");
        R.idPrefab = un::F(R.itemData, "_pickupPrefab");
        R.idUi = un::F(R.itemData, "_uiData");
        R.guiType = il2::Nested(R.itemData, "GuiType");
        if (R.guiType) {
            K.guiWeapon = Lit(R.guiType, "Weapon", K.guiWeapon);
            K.guiArmor = Lit(R.guiType, "Armor", K.guiArmor);
            K.guiAmmo = Lit(R.guiType, "Ammo", K.guiAmmo);
            K.guiMod = Lit(R.guiType, "WeaponMod", K.guiMod);
            K.guiThrown = Lit(R.guiType, "ThrownWeapon", K.guiThrown);
        } else {
            un::Missing("ItemData.GuiType");
        }
    }
    R.types = un::C(kItemAsm, "Sons.Items.Core", "Types");
    if (R.types) {
        K.equippable = Lit(R.types, "Equippable", K.equippable);
        K.clothing = Lit(R.types, "Clothing", K.clothing);
        K.crafting = Lit(R.types, "CraftingMaterial", K.crafting);
        K.edible = Lit(R.types, "Edible", K.edible);
        K.ammo = Lit(R.types, "Ammo", K.ammo);
        K.projectile = Lit(R.types, "Projectile", K.projectile);
        K.ranged = Lit(R.types, "RangedWeapon", K.ranged);
        K.weapon = Lit(R.types, "Weapon", K.weapon);
        K.armor = Lit(R.types, "Armor", K.armor);
        K.axe = Lit(R.types, "Axe", K.axe);
    }
    R.uiData = un::C(kItemAsm, "", "ItemUiData");
    if (R.uiData) {
        R.uiIcon = un::F(R.uiData, "_icon");
        R.uiTitle = un::F(R.uiData, "_title");
        R.uiGetTitle = un::M(R.uiData, "GetTitleLocalized", 0);
    }
    R.itemUtils = un::C(kSons, "TheForest.Items.Utils", "ItemUtils");
    if (R.itemUtils) {
        R.spawnPickup = StaticM(R.itemUtils, "SpawnItemPickup", 15);
        R.validForMode = StaticM(R.itemUtils, "IsValidForCurrentGameMode", 1);
    }
}

void ResolveIcons() {
    R.renderTexture = un::C(kCoreAsm, "UnityEngine", "RenderTexture");
    R.graphics = un::C(kCoreAsm, "UnityEngine", "Graphics");
    R.texture2d = un::C(kCoreAsm, "UnityEngine", "Texture2D");
    R.imageConversion = un::C("UnityEngine.ImageConversionModule", "UnityEngine", "ImageConversion");
    R.rtGet = un::M(R.renderTexture, "GetTemporary", std::vector<std::string>{"System.Int32", "System.Int32"});
    R.rtRelease = un::M(R.renderTexture, "ReleaseTemporary", 1);
    R.rtActiveGet = StaticM(R.renderTexture, "get_active", 0);
    R.rtActiveSet = StaticM(R.renderTexture, "set_active", 1);
    R.blit = un::M(R.graphics, "Blit", std::vector<std::string>{"UnityEngine.Texture", "UnityEngine.RenderTexture"});
    R.tex2dCtor = un::M(R.texture2d, ".ctor", std::vector<std::string>{"System.Int32", "System.Int32"});
    R.readPixels = un::M(R.texture2d, "ReadPixels", std::vector<std::string>{"UnityEngine.Rect", "System.Int32", "System.Int32"});
    R.encodePng = un::M(R.imageConversion, "EncodeToPNG", std::vector<std::string>{"UnityEngine.Texture2D"});
}

void ResolveVehicles() {
    R.golfCart = un::C(kSons, "Sons.Gameplay", "GolfCartController");
    if (R.golfCart) {
        R.cartRb = un::F(R.golfCart, "_rigidbody");
        R.cartActors = un::F(R.golfCart, "_localConnectedActors");
        R.cartHasPlayer = un::M(R.golfCart, "HasAnyPlayerInCart", 0);
        R.cartUpdateActivator = un::M(R.golfCart, "UpdateDistanceActivatorPosition", 0);
    }
    if (un::E.rigidbody) {
        R.rbSetPos = un::M(un::E.rigidbody, "set_position", 1);
        R.rbSetRot = un::M(un::E.rigidbody, "set_rotation", 1);
        R.rbSetAngular = un::M(un::E.rigidbody, "set_angularVelocity", 1);
    }
    R.terrain = un::C("Endnight", "Endnight.Environment", "TerrainUtilities");
    R.terrainHeight = un::M(R.terrain, "GetTerrainHeight", std::vector<std::string>{"UnityEngine.Vector3"});
}

void ResolveCreatures() {
    R.charManager = un::C(kSons, "Sons.Characters", "CharacterManager");
    if (R.charManager) {
        R.cmInstance = un::F(R.charManager, "<Instance>k__BackingField");
        R.cmDefs = un::F(R.charManager, "_definitions");
        R.cmAdd = un::M(R.charManager, "DebugAddCharacter", 2);
    }
    R.charDefinition = un::C(kSons, "Sons.Characters", "CharacterDefinition");
    if (R.charDefinition) {
        R.defId = un::F(R.charDefinition, "_id");
        R.defType = un::M(R.charDefinition, "GetActorTypeId", 0);
    }
    R.vailTypes = un::C(kVailAsm, "Sons.Ai.Vail", "VailTypes");
    R.actorClassOf = StaticM(R.vailTypes, "GetActorClass", 1);
    R.actorTypeId = un::C(kVailAsm, "Sons.Ai.Vail", "VailActorTypeId");
    R.actorClassId = un::C(kVailAsm, "Sons.Ai.Vail", "VailActorClassId");
    K.classCannibal = Lit(R.actorClassId, "Cannibal", K.classCannibal);
    K.classCreepy = Lit(R.actorClassId, "Creepy", K.classCreepy);
    K.classAnimal = Lit(R.actorClassId, "Animal", K.classAnimal);
    skipTypes.clear();
    waterTypes.clear();
    for (const auto& [name, v] : std::initializer_list<std::pair<const char*, int>>{
             {"None", 0},         {"Timmy", 8},           {"Robby", 9},            {"Virginia", 10},       {"Slug_Deprecated", 22},
             {"SpongeTest", 31},  {"DemonBoss", 45},      {"PuffyBossMale", 46},   {"PuffyBossFemale", 47}, {"BossMutant", 48},
             {"CreepyVirginia", 49}, {"Helicopter", 90}, {"PlayerRobby", 100}})
        skipTypes.insert(Lit(R.actorTypeId, name, v));
    for (const auto& [name, v] :
         std::initializer_list<std::pair<const char*, int>>{{"Turtle", 3}, {"Shark", 4}, {"Salmon", 24}, {"KillerWhale", 36}})
        waterTypes.insert(Lit(R.actorTypeId, name, v));
    R.worldSim = un::C(kVailAsm, "Sons.Ai.Vail", "VailWorldSimulation");
    if (R.worldSim) {
        R.simInstance = un::F(R.worldSim, "_instance");
        R.simActors = un::F(R.worldSim, "_actors");
        R.simRemove = un::M(R.worldSim, "RemoveActor", 1);
    }
    R.worldActor = un::C(kVailAsm, "Sons.Ai.Vail", "WorldSimActor");
    if (R.worldActor) {
        R.waType = un::F(R.worldActor, "_typeId");
        R.waUid = un::F(R.worldActor, "_uniqueId");
        R.waDebug = un::F(R.worldActor, "<IsDebugSpawned>k__BackingField");
        R.waRemoved = un::F(R.worldActor, "_removed");
    }
    R.actorManager = un::C(kVailAsm, "Sons.Ai.Vail", "VailActorManager");
    if (R.actorManager) {
        R.vamFind = un::M(R.actorManager, "FindActiveActor", std::vector<std::string>{"Sons.Ai.Vail.WorldSimActor"});
        R.vamRemove = StaticM(R.actorManager, "RemoveActor", 1);
    }
}

bool Resolve(std::string& err) {
    ResolvePlayer();
    ResolveItems();
    ResolveIcons();
    ResolveVehicles();
    ResolveCreatures();
    bool player = R.localPlayer && (R.lpInventory || R.lpGetInventory);
    canCatalog = R.dbInstance && R.dbList && R.idId && R.idName && R.idMax && R.idNoStore;
    canAdd = canCatalog && player && R.invAdd && R.invSpace;
    canAddAll = canAdd && R.idCanSpawn && R.idExclude && R.idDropUnequip;
    canWorld = canCatalog && R.idPrefab && R.spawnPickup && (R.lpCamTr || un::E.camMain);
    canCart = R.golfCart && un::E.findObjectsOfType && (R.lpCamTr || un::E.camMain);
    canCreature = R.cmInstance && R.cmDefs && R.cmAdd && R.defId && R.defType;
    canRemove = canCreature && R.simInstance && R.simActors && R.waUid && R.waDebug && R.waRemoved && R.simRemove;
    canIcons = canCatalog && R.idUi && R.uiIcon && R.rtGet && R.rtRelease && R.rtActiveGet && R.rtActiveSet && R.blit && R.texture2d &&
               R.tex2dCtor && R.readPixels && R.encodePng && un::E.cachedPtr;
    LOG("spawner parts: add %d, add all %d, world %d, golf cart %d, creatures %d, remove %d, icons %d", canAdd, canAddAll, canWorld, canCart,
        canCreature, canRemove, canIcons);
    if (!canAdd && !canWorld && !canCart && !canCreature) {
        err = "the game's item and creature functions were not found";
        return false;
    }
    return true;
}

bool Supported(const std::string& id) {
    if (id == "add_items" || id == "instant_loadout") return canAdd;
    if (id == "add_all_items") return canAddAll;
    if (id == "spawner") return canWorld;
    if (id == "vehicle_spawner") return canWorld || canCart;
    if (id == "creature_spawner") return canCreature;
    if (id == "remove_spawned_creatures") return canRemove;
    return false;
}

void Tick(const Ctx& c) {
    Publish();
    if (!c.inWorld || !c.player) {
        DropRequests("Load a save first");
        return;
    }
    StepCatalog();
    StepCreatureList();
    StepIcons();
    RunRequests(c);
    StepAddJobs(c);
    StepSpawnJobs(c);
    StepClaims(c.now);
}

bool Action(const std::string& id, const Ctx& c) {
    if (id == "add_all_items") {
        if (canAddAll) StartAddAll(c);
        return true;
    }
    if (id == "remove_spawned_creatures") {
        if (canRemove) RemoveCreatures(c);
        return true;
    }
    return false;
}

void Release(const Ctx&) {
    {
        std::lock_guard<std::mutex> g(qLock);
        requests.clear();
    }
    addJobs.clear();
    spawnJobs.clear();
    mine.clear();
    known.clear();
    claimUntil = 0;
    nextClaim = 0;
    building.clear();
    catStage = 0;
    catNext = 0;
    catTotal = 0;
    if (!creaturesBuilt) {
        creatureNext = 0;
        creatureBuild.clear();
    }
}

void Wire(core::Model& m, std::mutex& lock) {
    model = &m;
    modelLock = &lock;
    m.categories = kItemCats;
    m.worldCategories = kWorldCats;
    m.creatureCategories = kCreatureCats;
    if (canAdd) {
        m.spawn = [](const Item& it, int n) {
            Request r;
            r.type = Act::Add;
            r.id = it.id;
            r.amount = n;
            Push(std::move(r));
        };
        m.giveLoadout = [] {
            Request r;
            r.type = Act::Loadout;
            if (model) r.loadout = model->loadout;
            Push(std::move(r));
        };
        m.duplicateHeld = [] {
            Request r;
            r.type = Act::Duplicate;
            Push(std::move(r));
        };
        if (m.loadout.empty()) m.loadout = defaultLoadout;
    }
    if (canWorld || canCart) {
        m.spawnWorld = [](const Item& it, int n) {
            Request r;
            r.type = Act::World;
            r.id = it.id;
            r.amount = n;
            Push(std::move(r));
        };
    }
    if (canCreature) {
        m.spawnCreature = [](const Item& it, int n) {
            Request r;
            r.type = Act::Creature;
            r.id = it.id;
            r.amount = n;
            Push(std::move(r));
        };
    }
}

}

const Group& SpawnGroup() {
    static Group g{"Spawner", &Resolve, &Supported, &Tick, &Action, &Release, &Wire, nullptr};
    return g;
}

}
