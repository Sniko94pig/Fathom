#include "ingame/spawn.h"
#include "ingame/game.h"
#include "ingame/log.h"
#include "ingame/unity.h"
#include <windows.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

namespace sn1spawn {

namespace {

using core::Item;
using mono::Object;
using namespace un;

core::Model* model = nullptr;
std::mutex* modelLock = nullptr;
std::wstring folder;
std::function<void(const std::wstring&)> loadIcons;

enum class Act { Add, World, Creature, Duplicate, Loadout, Teleport, Undo };

struct Action {
    Act type = Act::Add;
    std::string id;
    int amount = 1;
    float x = 0, y = 0, z = 0;
    bool useZ = false;
    std::vector<std::pair<std::string, int>> loadout;
};

std::mutex qLock;
std::vector<Action> queue;

void Push(Action a) {
    std::lock_guard<std::mutex> g(qLock);
    if (queue.size() < 64) queue.push_back(std::move(a));
}

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct Tech {
    int tt = 0;
    std::string id, name;
    int equip = 0, slot = 0, background = 0, processed = 0, harvest = 0, behaviour = 0;
    bool buildable = false, hasIngredients = false, iconDefault = true, creature = false;
};

int eqNone = 0, eqHand = -1, slotNone = 0, harvestNone = 0;
std::unordered_set<int> wearTypes, equipTypes, plantBackgrounds, plantHarvests, creatureBehaviours;

std::vector<int> techQueue;
size_t techIdx = 0;
std::vector<Tech> techs;
std::unordered_map<int, size_t> techIndex;
std::unordered_map<std::string, int> idToTech;
std::unordered_set<int> ingredientSet;
bool catalogStarted = false, catalogDone = false;
mono::Class* ingClass = nullptr;
mono::Method *ingCount = nullptr, *ingItem = nullptr;

std::vector<std::pair<int, std::string>> iconQueue;
size_t iconIdx = 0;
bool iconsDone = false, iconsWritten = false;

struct Pending {
    uint32_t task = 0;
    int tt = 0;
    int count = 1;
    float dist = 2;
    bool spread = false;
    double start = 0;
    std::string name;
};
std::vector<Pending> pending;
std::unordered_map<mono::Class*, mono::Method*> resultMethods;

struct EquipJob {
    std::vector<int> techs;
    double until = 0, next = 0;
} equip;

struct Spot {
    bool valid = false;
    Vec3 pos;
    uint32_t sub = 0, pod = 0;
} undo;

struct Arrival {
    bool active = false;
    bool surface = false;
    double start = 0;
    float x = 0, z = 0;
    uint32_t sub = 0, pod = 0;
} arrival;

bool placesLoaded = false;
std::vector<core::Location> placesCopy;
double lastMarkers = 0;
bool wasIn = false;

const char* kGear[] = {"Beacon", "Constructor", "SmallStorage", "Gravsphere", "CyclopsDecoy", "Seaglide", "PipeSurfaceFloater", "Pipe",
                       "MapRoomCamera", "LEDLight"};
const char* kFood[] = {"FilteredWater", "DisinfectedWater", "BigFilteredWater", "StillsuitWater", "Coffee", "NutrientBlock", "Snack1",
                       "Snack2", "Snack3", "BulboTreePiece", "PurpleVegetable", "HangingFruit", "Melon", "SmallMelon"};

bool StartsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

bool EndsWith(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

bool InList(const std::string& id, const char* const* list, size_t n) {
    for (size_t i = 0; i < n; ++i)
        if (id == list[i]) return true;
    return false;
}

bool IsGear(const std::string& id) { return InList(id, kGear, sizeof(kGear) / sizeof(kGear[0])); }

bool IsFood(const std::string& id) {
    return StartsWith(id, "Cooked") || StartsWith(id, "Cured") || InList(id, kFood, sizeof(kFood) / sizeof(kFood[0]));
}

bool IsVehicle(const std::string& id) { return id == "Seamoth" || id == "Exosuit" || id == "Cyclops"; }

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::vector<Object*> AnyList(Object* list) {
    std::vector<Object*> out;
    if (!list) return out;
    mono::Class* c = mono::ClassOf(list);
    mono::Method* count = mono::FindMethod(c, "get_Count", 0);
    mono::Method* item = mono::FindMethod(c, "get_Item", 1);
    if (!count || !item) return out;
    int n = CallInt(count, list, {}, 0);
    for (int i = 0; i < n && i < 512; ++i) {
        int k = i;
        Object* o = Call(item, list, {&k});
        if (o) out.push_back(o);
    }
    return out;
}

int RayMask() {
    int m = 0;
    if (R.pmLayerMask) mono::StaticGet(R.pmLayerMask.cls, R.pmLayerMask.f, &m);
    if (!m) m = ~0;
    return m & ~(1 << 2);
}

void SetUndo(bool v) {
    std::lock_guard<std::mutex> g(*modelLock);
    model->canUndoTeleport = v;
}

float Dist2(Vec3 a, Vec3 b) {
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

void CollectIngredients(Object* coll, Tech& t) {
    if (!coll) return;
    mono::Class* c = mono::ClassOf(coll);
    if (c != ingClass) {
        ingClass = c;
        ingCount = mono::FindMethod(c, "get_Count", 0);
        ingItem = mono::FindMethod(c, "get_Item", 1);
    }
    if (!ingCount || !ingItem) return;
    int n = CallInt(ingCount, coll, {}, 0);
    t.hasIngredients = n > 0;
    for (int i = 0; i < n && i < 32; ++i) {
        int k = i;
        Object* ing = Call(ingItem, coll, {&k});
        int it = Val<int>(ing, R.ingTech, 0);
        if (it > 0) ingredientSet.insert(it);
    }
}

void StartCatalog() {
    catalogStarted = true;
    Object* type = R.techType ? mono::TypeObject(R.techType) : nullptr;
    Object* arr = type && R.enumValues ? Call(R.enumValues, nullptr, {type}) : nullptr;
    size_t n = mono::Length(arr);
    const int* d = n ? (const int*)mono::Data(arr, 4) : nullptr;
    std::unordered_set<int> seen;
    for (size_t i = 0; d && i < n; ++i)
        if (d[i] > 0 && seen.insert(d[i]).second) techQueue.push_back(d[i]);
    LOG("item catalog: %d tech types", (int)techQueue.size());
}

void Publish(std::vector<Item>& items, std::vector<Item>& world, std::vector<Item>& creatures) {
    std::vector<std::string> itemCats = {"Resources", "Food", "Healing", "Tools", "Equipment", "Creatures", "Plants", "Other"};
    std::vector<std::string> worldCats = {"Items", "Vehicles", "Placeable gear", "Creatures", "Plants"};
    std::vector<std::string> creatureCats = {"Passive", "Hostile", "Leviathans", "Eggs"};
    auto sorter = [](const std::vector<std::string>& cats) {
        return [cats](const Item& a, const Item& b) {
            auto ia = std::find(cats.begin(), cats.end(), a.category) - cats.begin();
            auto ib = std::find(cats.begin(), cats.end(), b.category) - cats.begin();
            if (ia != ib) return ia < ib;
            return a.name < b.name;
        };
    };
    std::sort(items.begin(), items.end(), sorter(itemCats));
    std::sort(world.begin(), world.end(), sorter(worldCats));
    std::sort(creatures.begin(), creatures.end(), sorter(creatureCats));
    std::lock_guard<std::mutex> g(*modelLock);
    model->items = items;
    model->categories = itemCats;
    model->worldItems = world;
    model->worldCategories = worldCats;
    model->creatureItems = creatures;
    model->creatureCategories = creatureCats;
}

void FinishCatalog() {
    catalogDone = true;
    std::unordered_set<std::string> seedPlants;
    for (const Tech& t : techs)
        if (plantBackgrounds.count(t.background) && EndsWith(t.id, "Seed")) seedPlants.insert(t.id.substr(0, t.id.size() - 4));
    std::vector<Item> items, world, creatures;
    for (const Tech& t : techs) {
        bool vehicle = IsVehicle(t.id), gear = IsGear(t.id);
        bool fragment = t.id.find("Fragment") != std::string::npos;
        bool egg = EndsWith(t.id, "Egg") && t.id.find("Undiscovered") == std::string::npos;
        bool plantItem = plantBackgrounds.count(t.background) > 0;
        bool food = IsFood(t.id);
        bool healing = t.id == "FirstAidKit";
        bool fish = t.creature && t.processed > 0;
        bool equipment = equipTypes.count(t.equip) > 0;
        bool tool = (eqHand > 0 && t.equip == eqHand) || t.slot != slotNone;
        bool resource = ingredientSet.count(t.tt) > 0;
        std::string lower = t.id;
        for (char& ch : lower) ch = (char)std::tolower((unsigned char)ch);
        bool odd = lower.find("test") != std::string::npos || lower.find("unused") != std::string::npos ||
                   lower.find("deprecated") != std::string::npos || lower.find("debug") != std::string::npos ||
                   lower.find("placeholder") != std::string::npos;
        bool unnamed = t.name.empty() || t.name == t.id;
        bool blocked = t.buildable || vehicle || fragment;
        bool known = equipment || tool || resource || t.hasIngredients || plantItem || food || healing || fish || egg || gear;
        bool itemLike = !blocked && (!t.creature || fish) && known;
        bool spare = !blocked && !t.creature && !known && !t.iconDefault && t.harvest == harvestNone;
        if (itemLike || spare) {
            std::string cat = !itemLike       ? "Other"
                              : healing       ? "Healing"
                              : food          ? "Food"
                              : (fish || egg) ? "Creatures"
                              : plantItem     ? "Plants"
                              : equipment     ? "Equipment"
                              : tool          ? "Tools"
                              : resource      ? "Resources"
                                              : "Other";
            items.push_back({t.id, t.name, cat, spare || unnamed || odd || t.iconDefault, true});
        }
        bool plant = !blocked && !t.creature && (plantItem || plantHarvests.count(t.harvest) > 0 || seedPlants.count(t.id) > 0);
        std::string wcat;
        if (vehicle) wcat = "Vehicles";
        else if (gear) wcat = "Placeable gear";
        else if (t.creature && !egg) wcat = "Creatures";
        else if (plant) wcat = "Plants";
        else if (itemLike || spare) wcat = "Items";
        if (!wcat.empty()) world.push_back({t.id, t.name, wcat, !vehicle && (spare || unnamed || odd), true});
        if (t.creature && !egg) {
            int k = sn1game::CreatureKind(t.tt);
            creatures.push_back({t.id, t.name, k == 2 ? "Leviathans" : k == 1 ? "Hostile" : "Passive", unnamed || odd, true});
        } else if (egg) {
            creatures.push_back({t.id, t.name, "Eggs", unnamed || odd, true});
        }
    }
    LOG("item catalog: %d items, %d world, %d creatures", (int)items.size(), (int)world.size(), (int)creatures.size());
    std::unordered_set<std::string> queued;
    for (const auto* list : {&items, &world, &creatures})
        for (const Item& it : *list)
            if (queued.insert(it.id).second) iconQueue.push_back({idToTech[it.id], it.id});
    Publish(items, world, creatures);
    CreateDirectoryW((folder + L"\\icons").c_str(), nullptr);
}

void StepCatalog() {
    Object* def = R.spDefault ? Call(R.spDefault, nullptr) : nullptr;
    size_t end = std::min(techQueue.size(), techIdx + 60);
    for (; techIdx < end; ++techIdx) {
        int tt = techQueue[techIdx];
        Tech t;
        t.tt = tt;
        int lower = 0;
        t.id = mono::Str(Call(R.ttAsString, nullptr, {&tt, &lower}));
        if (t.id.empty() || t.id == "None") continue;
        if (R.isAllowed && !CallBool(R.isAllowed, nullptr, {&tt})) continue;
        t.name = TechName(tt);
        t.equip = R.tdEquip ? CallInt(R.tdEquip, nullptr, {&tt}, eqNone) : eqNone;
        t.slot = R.tdSlot ? CallInt(R.tdSlot, nullptr, {&tt}, slotNone) : slotNone;
        t.buildable = R.tdBuildable && CallBool(R.tdBuildable, nullptr, {&tt});
        t.background = R.tdBackground ? CallInt(R.tdBackground, nullptr, {&tt}, 0) : 0;
        t.processed = R.tdProcessed ? CallInt(R.tdProcessed, nullptr, {&tt}, 0) : 0;
        t.harvest = R.tdHarvest ? CallInt(R.tdHarvest, nullptr, {&tt}, harvestNone) : harvestNone;
        t.behaviour = R.cdBehaviour ? CallInt(R.cdBehaviour, nullptr, {&tt}, 0) : 0;
        t.creature = creatureBehaviours.count(t.behaviour) > 0;
        if (R.tdIngredients) CollectIngredients(Call(R.tdIngredients, nullptr, {&tt}), t);
        Object* sp = R.spGet ? Call(R.spGet, nullptr, {&tt}) : nullptr;
        t.iconDefault = !Alive(sp) || sp == def;
        techIndex[tt] = techs.size();
        idToTech[t.id] = tt;
        techs.push_back(std::move(t));
    }
    if (techIdx >= techQueue.size()) FinishCatalog();
}

bool ExportIcon(int tt, const std::wstring& path, Object* def) {
    if (!R.spGet || !R.sprite || !R.sprTexture || !R.sprUv || !R.rtGet || !R.rtRelease || !R.rtActiveGet || !R.rtActiveSet ||
        !R.blit || !R.texture2d || !R.tex2dCtor || !R.readPixels || !R.encodePng || !R.writeBytes || !R.destroy)
        return false;
    Object* sp = Call(R.spGet, nullptr, {&tt});
    if (!Alive(sp) || sp == def || mono::ClassOf(sp) != R.sprite) return false;
    Object* tex = Call(R.sprTexture, sp);
    if (!Alive(tex)) return false;
    Object* uvs = Call(R.sprUv, sp);
    size_t n = mono::Length(uvs);
    const float* uv = n ? (const float*)mono::Data(uvs, 8) : nullptr;
    if (!uv) return false;
    float u0 = 1, v0 = 1, u1 = 0, v1 = 0;
    for (size_t i = 0; i < n; ++i) {
        u0 = std::min(u0, uv[i * 2]);
        u1 = std::max(u1, uv[i * 2]);
        v0 = std::min(v0, uv[i * 2 + 1]);
        v1 = std::max(v1, uv[i * 2 + 1]);
    }
    if (u1 <= u0 || v1 <= v0) return false;
    int size = 64;
    Object* rt = Call(R.rtGet, nullptr, {&size, &size});
    if (!Alive(rt)) return false;
    float scale[2] = {u1 - u0, v1 - v0}, offset[2] = {u0, v0};
    Object* prev = Call(R.rtActiveGet, nullptr);
    Call(R.blit, nullptr, {tex, rt, scale, offset});
    Call(R.rtActiveSet, nullptr, {rt});
    Object* t2 = mono::New(R.texture2d);
    bool ok = false, wrote = false;
    if (t2) Call(R.tex2dCtor, t2, {&size, &size}, &ok);
    if (ok) {
        float rect[4] = {0, 0, (float)size, (float)size};
        int zero = 0;
        Call(R.readPixels, t2, {rect, &zero, &zero});
        Object* bytes = Call(R.encodePng, nullptr, {t2});
        if (bytes && mono::Length(bytes) > 0) {
            Object* ps = mono::NewString(Narrow(path).c_str());
            Call(R.writeBytes, nullptr, {ps, bytes}, &wrote);
        }
    }
    Call(R.rtActiveSet, nullptr, {prev});
    Call(R.rtRelease, nullptr, {rt});
    if (ok && Alive(t2)) Call(R.destroy, nullptr, {t2});
    return wrote;
}

void StepIcons() {
    if (iconsDone) return;
    Object* def = R.spDefault ? Call(R.spDefault, nullptr) : nullptr;
    std::wstring dir = folder + L"\\icons";
    int work = 0;
    while (iconIdx < iconQueue.size() && work < 4) {
        const auto& e = iconQueue[iconIdx++];
        std::wstring path = dir + L"\\" + Widen(e.second) + L".png";
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        if (ExportIcon(e.first, path, def)) iconsWritten = true;
        ++work;
    }
    if (iconIdx < iconQueue.size()) return;
    iconsDone = true;
    LOG("item icons ready%s", iconsWritten ? " (new ones saved)" : "");
    if (iconsWritten && loadIcons) loadIcons(dir);
}

void AddItems(Object* player, int tt, int n) {
    if (!R.itemSpawn || !R.startCoroutine) {
        sn1game::Note("Adding items is not available");
        return;
    }
    n = std::clamp(n, 1, 999);
    Object* e = Call(R.itemSpawn, nullptr, {&n, &tt});
    if (e) Call(R.startCoroutine, player, {e});
}

const Tech* Find(int tt) {
    auto it = techIndex.find(tt);
    return it == techIndex.end() ? nullptr : &techs[it->second];
}

float WorldDistance(int tt) {
    const Tech* t = Find(tt);
    if (!t) return 2.5f;
    if (t->id == "Cyclops") return 35.0f;
    if (t->id == "Seamoth" || t->id == "Exosuit") return 6.0f;
    if (IsGear(t->id)) return 3.0f;
    if (t->creature) return sn1game::CreatureKind(tt) == 2 ? 30.0f : 5.0f;
    return 2.5f;
}

void QueueSpawn(Object* player, int tt, int count, double now) {
    const Tech* t = Find(tt);
    std::string name = t ? t->name : "item";
    if (!R.prefabAsync || !R.startCoroutine || !R.createPrefab) {
        sn1game::Note("Spawning is not available");
        return;
    }
    int no = 0;
    Object* task = Call(R.prefabAsync, nullptr, {&tt, &no});
    if (!task) {
        sn1game::Note("Could not load " + name);
        return;
    }
    Call(R.startCoroutine, player, {task});
    Pending p;
    p.task = mono::Pin(task);
    p.tt = tt;
    p.count = std::clamp(count, 1, 50);
    p.dist = WorldDistance(tt);
    p.spread = p.count > 1;
    p.start = now;
    p.name = name;
    pending.push_back(std::move(p));
}

void StepPending(double now) {
    for (auto it = pending.begin(); it != pending.end();) {
        Object* task = mono::Pinned(it->task);
        Object* prefab = nullptr;
        if (task) {
            mono::Class* c = mono::ClassOf(task);
            auto f = resultMethods.find(c);
            mono::Method* get = f != resultMethods.end() ? f->second : (resultMethods[c] = mono::FindMethod(c, "GetResult", 0));
            prefab = get ? Call(get, task) : nullptr;
        }
        if (!Alive(prefab)) {
            if (!task || now - it->start > 20) {
                sn1game::Note("Could not spawn " + it->name);
                mono::Unpin(it->task);
                it = pending.erase(it);
            } else {
                ++it;
            }
            continue;
        }
        int spread = it->spread ? 1 : 0;
        int tt = it->tt;
        for (int i = 0; i < it->count; ++i) {
            float dist = it->dist;
            Object* go = Call(R.createPrefab, nullptr, {prefab, &dist, &spread});
            if (!Alive(go)) continue;
            if (R.lweRegister) Call(R.lweRegister, nullptr, {go});
            if (R.craftEnd) Call(R.craftEnd, nullptr, {go, &tt});
            if (R.sendMessage && R.sendDontRequire >= 0) {
                Object* msg = mono::NewString("StartConstruction");
                int opt = R.sendDontRequire;
                Call(R.sendMessage, go, {msg, &opt});
            }
        }
        mono::Unpin(it->task);
        it = pending.erase(it);
    }
}

void StepEquip(double now) {
    if (equip.techs.empty() || now < equip.next) return;
    equip.next = now + 0.25;
    if (now > equip.until) {
        equip.techs.clear();
        return;
    }
    Object* inv = Static(R.invMain);
    if (!Alive(inv) || !R.invContainer || !R.invEquipment || !R.icGetItems || !R.eqFreeSlot || !R.invAction || R.equipAction < 0) {
        equip.techs.clear();
        return;
    }
    Object* cont = Call(R.invContainer, inv);
    Object* eq = Call(R.invEquipment, inv);
    if (!cont || !eq) return;
    for (auto it = equip.techs.begin(); it != equip.techs.end();) {
        int tt = *it;
        std::vector<Object*> items = AnyList(Call(R.icGetItems, cont, {&tt}));
        if (items.empty()) {
            ++it;
            continue;
        }
        int et = R.tdEquip ? CallInt(R.tdEquip, nullptr, {&tt}, eqNone) : eqNone;
        Object* slot = nullptr;
        if (CallBool(R.eqFreeSlot, eq, {&et, &slot})) {
            int act = R.equipAction;
            Call(R.invAction, inv, {&act, items[0]});
        }
        it = equip.techs.erase(it);
    }
}

void ClearArrival() {
    if (arrival.sub) mono::Unpin(arrival.sub);
    if (arrival.pod) mono::Unpin(arrival.pod);
    arrival = Arrival{};
}

void ClearUndo() {
    if (undo.sub) mono::Unpin(undo.sub);
    if (undo.pod) mono::Unpin(undo.pod);
    undo = Spot{};
}

Object* Nearest(Class* type, Vec3 p, float maxDist, bool basesOnly) {
    Object* best = nullptr;
    float bestD = maxDist * maxDist;
    if (!type) return nullptr;
    for (Object* o : FindAll(type)) {
        if (basesOnly && !Val<uint8_t>(o, R.srIsBase)) continue;
        float d = Dist2(Position(o), p);
        if (d < bestD) {
            bestD = d;
            best = o;
        }
    }
    return best;
}

void Travel(Object* player, Object* veh, Object* cyc, Vec3 target, bool surface, uint32_t subAfter, uint32_t podAfter, double now) {
    ClearArrival();
    if (veh || cyc) {
        if (subAfter) mono::Unpin(subAfter);
        if (podAfter) mono::Unpin(podAfter);
    }
    if (veh && R.vhTeleport) {
        Object* tr = TransformOf(veh);
        Quat rot = tr && R.trRotation ? mono::Result<Quat>(Call(R.trRotation, tr)) : Quat{};
        int keep = 0;
        Call(R.vhTeleport, veh, {&target, &rot, &keep});
        return;
    }
    if (veh) {
        SetPosition(veh, target);
        return;
    }
    if (cyc) {
        Vec3 sp = Position(cyc), pp = Position(player);
        SetPosition(cyc, target);
        SetPosition(player, {pp.x + target.x - sp.x, pp.y + target.y - sp.y, pp.z + target.z - sp.z});
        return;
    }
    Object* gc = Static(R.gotoMain);
    if (Alive(gc) && R.gotoPos) {
        int imm = 0;
        Call(R.gotoPos, gc, {&target, &imm});
    } else {
        SetPosition(player, target);
    }
    arrival.active = true;
    arrival.surface = surface;
    arrival.start = now;
    arrival.x = target.x;
    arrival.z = target.z;
    arrival.sub = subAfter;
    arrival.pod = podAfter;
}

void Teleport(Object* player, const Action& a, double now) {
    Object* veh = Call(R.plGetVehicle, player);
    if (!Alive(veh)) veh = nullptr;
    Object* sub = Call(R.plCurrentSub, player);
    Object* cyc = Alive(sub) && Val<uint8_t>(sub, R.srCyclops) ? sub : nullptr;
    bool foot = !veh && !cyc;
    ClearUndo();
    undo.valid = true;
    undo.pos = veh ? Position(veh) : cyc ? Position(cyc) : Position(player);
    if (foot) {
        if (Alive(sub) && Val<uint8_t>(sub, R.srIsBase)) undo.sub = mono::Pin(sub);
        Object* pod = R.plGetPod ? Call(R.plGetPod, player) : nullptr;
        if (Alive(pod)) undo.pod = mono::Pin(pod);
    }
    Vec3 target{a.x, a.useZ ? a.z : -1.0f, -a.y};
    uint32_t subAfter = 0;
    if (!a.useZ) {
        if (cyc) target.y = -12.0f;
    } else {
        Object* hitVeh = Nearest(R.vehicle, target, 4.0f, false);
        if (hitVeh == veh) hitVeh = nullptr;
        Object* hitSub = hitVeh ? nullptr : Nearest(R.subRoot, target, 4.0f, false);
        if (hitSub == cyc) hitSub = nullptr;
        if (hitVeh) {
            target.y += cyc ? 30.0f : veh ? 6.0f : 3.0f;
            if (foot && R.vhDocked && CallBool(R.vhDocked, hitVeh)) {
                Object* base = Nearest(R.subRoot, target, 40.0f, true);
                if (base) subAfter = mono::Pin(base);
            }
        } else if (hitSub) {
            if (foot) {
                Object* rp = R.respawnPoint && R.rpSpawnPos ? CompInChildren(hitSub, R.respawnPoint) : nullptr;
                if (Alive(rp)) target = mono::Result<Vec3>(Call(R.rpSpawnPos, rp), target);
                subAfter = mono::Pin(hitSub);
            } else {
                target.y += 30.0f;
            }
        }
    }
    Travel(player, veh, cyc, target, foot && !a.useZ, subAfter, 0, now);
    SetUndo(true);
}

void Undo(Object* player, double now) {
    if (!undo.valid) return;
    Spot back = undo;
    undo = Spot{};
    Object* veh = Call(R.plGetVehicle, player);
    if (!Alive(veh)) veh = nullptr;
    Object* sub = Call(R.plCurrentSub, player);
    Object* cyc = Alive(sub) && Val<uint8_t>(sub, R.srCyclops) ? sub : nullptr;
    Vec3 target = back.pos;
    if ((veh || cyc) && (back.sub || back.pod)) target.y += 30.0f;
    Travel(player, veh, cyc, target, false, back.sub, back.pod, now);
    SetUndo(false);
}

void StepArrival(Object* player, double now) {
    if (!arrival.active) return;
    if (now - arrival.start > 20) {
        ClearArrival();
        return;
    }
    if (now - arrival.start < 0.1) return;
    Object* gc = Static(R.gotoMain);
    if (Alive(gc) && R.gotoMoving && Val<uint8_t>(gc, R.gotoMoving)) return;
    if (arrival.sub || arrival.pod) {
        Object* sub = arrival.sub ? mono::Pinned(arrival.sub) : nullptr;
        if (Alive(sub) && R.plSetSub) {
            int forced = 1;
            Call(R.plSetSub, player, {sub, &forced});
        }
        Object* pod = arrival.pod ? mono::Pinned(arrival.pod) : nullptr;
        if (Alive(pod) && R.plSetPod) {
            Call(R.plSetPod, player, {pod});
            Object* flag = Ref(player, R.plPodFlag);
            mono::Method* up = flag ? mono::FindMethod(mono::ClassOf(flag), "Update", 1) : nullptr;
            int on = 1;
            if (up) Call(up, flag, {&on});
        }
        if (arrival.sub) mono::Unpin(arrival.sub);
        if (arrival.pod) mono::Unpin(arrival.pod);
        arrival.sub = arrival.pod = 0;
    }
    int qti = R.E.qtiIgnore;
    if (!arrival.surface || !R.raycast || qti < 0) {
        arrival.active = false;
        return;
    }
    Vec3 origin{arrival.x, 600.0f, arrival.z}, down{0, -1, 0};
    RayHit hit{};
    float maxd = 602.0f;
    int mask = RayMask();
    if (!CallBool(R.raycast, nullptr, {&origin, &down, &hit, &maxd, &mask, &qti})) {
        if (now - arrival.start > 10) arrival.active = false;
        return;
    }
    if (hit.point.y > -0.5f) SetPosition(player, {arrival.x, hit.point.y + 1.6f, arrival.z});
    arrival.active = false;
}

std::string Pretty(const std::string& raw) {
    std::string s;
    for (size_t i = 0; i < raw.size(); ++i) {
        char ch = raw[i] == '_' || raw[i] == '-' ? ' ' : raw[i];
        if (i > 0 && std::isdigit((unsigned char)ch) && std::isalpha((unsigned char)raw[i - 1])) s += ' ';
        s += ch;
    }
    bool up = true;
    for (char& ch : s) {
        if (up && std::isalpha((unsigned char)ch)) ch = (char)std::toupper((unsigned char)ch);
        up = ch == ' ';
    }
    return s;
}

void LoadPlaces() {
    Object* gc = Static(R.gotoMain);
    if (!Alive(gc)) return;
    placesLoaded = true;
    Object* data = Ref(gc, R.gotoData);
    Object* arr = data ? Ref(data, R.tdLocations) : nullptr;
    size_t n = mono::Length(arr);
    std::vector<core::Location> out;
    for (size_t i = 0; i < n; ++i) {
        Object* tp = mono::At(arr, i);
        if (!tp) continue;
        std::string name = Pretty(mono::Str(Ref(tp, R.tpName)));
        if (name.empty()) continue;
        Vec3 p = Val<Vec3>(tp, R.tpPosition);
        out.push_back({name, p.x, -p.z, p.y});
    }
    std::sort(out.begin(), out.end(), [](const core::Location& a, const core::Location& b) { return a.name < b.name; });
    placesCopy = out;
    LOG("map places: %d", (int)out.size());
    std::lock_guard<std::mutex> g(*modelLock);
    model->places = out;
}

void UpdateMarkers(Object* player) {
    std::vector<core::Marker> mk;
    Vec3 pp = Position(player);
    Object* cam = Call(R.camMain, nullptr);
    Object* ct = Alive(cam) ? TransformOf(cam) : nullptr;
    Vec3 f = ct ? mono::Result<Vec3>(Call(R.trForward, ct)) : Vec3{0, 0, 1};
    mk.push_back({core::MarkerType::Player, "You", pp.x, -pp.z, pp.y, std::atan2(-f.z, f.x)});
    for (Object* v : FindAll(R.vehicle)) {
        Vec3 p = Position(v);
        std::string name = TechName(TechOf(GameObjectOf(v)));
        mk.push_back({core::MarkerType::Vehicle, name.empty() ? "Vehicle" : name, p.x, -p.z, p.y, 0});
    }
    for (Object* s : FindAll(R.subRoot)) {
        Vec3 p = Position(s);
        if (Val<uint8_t>(s, R.srCyclops)) mk.push_back({core::MarkerType::Vehicle, "Cyclops", p.x, -p.z, p.y, 0});
        else if (Val<uint8_t>(s, R.srIsBase)) mk.push_back({core::MarkerType::Base, "Base", p.x, -p.z, p.y, 0});
    }
    if (R.beacon)
        for (Object* b : FindAll(R.beacon)) {
            Vec3 p = Position(b);
            std::string label = R.bcLabel ? mono::Str(Ref(b, R.bcLabel)) : std::string();
            mk.push_back({core::MarkerType::Beacon, label.empty() ? "Beacon" : label, p.x, -p.z, p.y, 0});
        }
    for (const core::Location& l : placesCopy) mk.push_back({core::MarkerType::Place, l.name, l.x, l.y, l.z, 0});
    std::lock_guard<std::mutex> g(*modelLock);
    model->markers = std::move(mk);
}

void RunActions(Object* player, double now) {
    std::vector<Action> todo;
    {
        std::lock_guard<std::mutex> g(qLock);
        todo.swap(queue);
    }
    auto tech = [](const std::string& id) {
        auto it = idToTech.find(id);
        return it == idToTech.end() ? 0 : it->second;
    };
    for (Action& a : todo) {
        switch (a.type) {
        case Act::Add:
        case Act::World:
        case Act::Creature: {
            int tt = tech(a.id);
            if (!tt) {
                sn1game::Note("The item list is still loading");
                break;
            }
            if (a.type == Act::Add) AddItems(player, tt, a.amount);
            else QueueSpawn(player, tt, a.amount, now);
            break;
        }
        case Act::Duplicate: {
            Object* inv = Static(R.invMain);
            Object* held = Alive(inv) && R.invHeld ? Call(R.invHeld, inv) : nullptr;
            int tt = Alive(held) && R.pkTech ? CallInt(R.pkTech, held, {}, 0) : 0;
            if (tt <= 0) sn1game::Note("Hold an item in your hand first");
            else AddItems(player, tt, 1);
            break;
        }
        case Act::Loadout: {
            int given = 0;
            for (const auto& e : a.loadout) {
                int tt = tech(e.first);
                if (!tt) continue;
                AddItems(player, tt, e.second);
                ++given;
                const Tech* t = Find(tt);
                if (t && wearTypes.count(t->equip)) equip.techs.push_back(tt);
            }
            if (!given) sn1game::Note(a.loadout.empty() ? "Your loadout is empty" : "The item list is still loading");
            equip.until = now + 8.0;
            equip.next = now + 0.5;
            break;
        }
        case Act::Teleport:
            Teleport(player, a, now);
            break;
        case Act::Undo:
            Undo(player, now);
            break;
        }
    }
}

}

void Wire(core::Model& m, std::mutex& lock, const std::wstring& dir, std::function<void(const std::wstring&)> load) {
    model = &m;
    modelLock = &lock;
    folder = dir;
    loadIcons = std::move(load);
    eqNone = std::max(0, mono::EnumValue(R.equipmentType, "None"));
    eqHand = mono::EnumValue(R.equipmentType, "Hand");
    slotNone = std::max(0, mono::EnumValue(R.quickSlotType, "None"));
    harvestNone = std::max(0, mono::EnumValue(R.harvestType, "None"));
    for (const char* n : {"Head", "Body", "Gloves", "Foots", "Tank", "Chip"}) {
        int v = mono::EnumValue(R.equipmentType, n);
        if (v > 0) {
            wearTypes.insert(v);
            equipTypes.insert(v);
        }
    }
    for (const char* n : {"CyclopsModule", "VehicleModule", "SeamothModule", "ExosuitModule", "ExosuitArm"}) {
        int v = mono::EnumValue(R.equipmentType, n);
        if (v > 0) equipTypes.insert(v);
    }
    for (const char* n : {"PlantWater", "PlantWaterSeed", "PlantAir", "PlantAirSeed"}) {
        int v = mono::EnumValue(R.backgroundType, n);
        if (v > 0) plantBackgrounds.insert(v);
    }
    for (const char* n : {"Pick", "DamageAlive"}) {
        int v = mono::EnumValue(R.harvestType, n);
        if (v > 0) plantHarvests.insert(v);
    }
    for (const char* n : {"SmallFish", "MediumFish", "Shark", "Whale", "Leviathan", "Trap", "Crab", "Poison"}) {
        int v = mono::EnumValue(R.behaviourType, n);
        if (v > 0) creatureBehaviours.insert(v);
    }
    bool canAdd = R.itemSpawn && R.startCoroutine && R.techType && R.enumValues;
    bool canSpawn = R.prefabAsync && R.startCoroutine && R.createPrefab && R.techType && R.enumValues;
    bool canMove = R.gotoMain && R.gotoPos;
    for (auto [id, ok] : {std::pair<const char*, bool>{"add_items", canAdd}, {"instant_loadout", canAdd}, {"spawner", canSpawn},
                          {"creature_spawner", canSpawn}, {"map", canMove}}) {
        core::Feature* f = m.find(id);
        if (!f) continue;
        f->available = ok;
        if (!ok) f->missing = "Not found in this version of the game";
    }
    m.spawn =[](const Item& it, int n) {
        Action a;
        a.type = Act::Add;
        a.id = it.id;
        a.amount = n;
        Push(std::move(a));
    };
    m.spawnWorld = [](const Item& it, int n) {
        Action a;
        a.type = Act::World;
        a.id = it.id;
        a.amount = n;
        Push(std::move(a));
    };
    m.spawnCreature = [](const Item& it, int n) {
        Action a;
        a.type = Act::Creature;
        a.id = it.id;
        a.amount = n;
        Push(std::move(a));
    };
    m.duplicateHeld = [] {
        Action a;
        a.type = Act::Duplicate;
        Push(std::move(a));
    };
    m.giveLoadout = [] {
        Action a;
        a.type = Act::Loadout;
        a.loadout = model->loadout;
        Push(std::move(a));
    };
    m.teleport = [](float x, float y, float z, bool useZ) {
        Action a;
        a.type = Act::Teleport;
        a.x = x;
        a.y = y;
        a.z = z;
        a.useZ = useZ;
        Push(std::move(a));
    };
    m.undoTeleport = [] {
        Action a;
        a.type = Act::Undo;
        Push(std::move(a));
    };
}

void Tick(Object* player, bool inGame, double now, bool mapVisible) {
    if (!model) return;
    if (!inGame) {
        if (wasIn) {
            wasIn = false;
            Release();
            std::lock_guard<std::mutex> g(*modelLock);
            model->inWorld = false;
        }
        std::vector<Action> todo;
        {
            std::lock_guard<std::mutex> g(qLock);
            todo.swap(queue);
        }
        if (!todo.empty()) sn1game::Note("Load a save first");
        return;
    }
    if (!wasIn) {
        wasIn = true;
        std::lock_guard<std::mutex> g(*modelLock);
        model->inWorld = true;
    }
    if (!catalogDone) {
        if (!catalogStarted) {
            uint8_t ready = 0;
            if (R.spReady) mono::StaticGet(R.spReady.cls, R.spReady.f, &ready);
            Object* lang = R.lgMain ? Call(R.lgMain, nullptr) : nullptr;
            if ((ready || !R.spReady) && lang) StartCatalog();
        } else {
            StepCatalog();
        }
    } else {
        StepIcons();
    }
    if (!placesLoaded) LoadPlaces();
    RunActions(player, now);
    StepPending(now);
    StepEquip(now);
    StepArrival(player, now);
    if (mapVisible && now - lastMarkers > 0.3) {
        lastMarkers = now;
        UpdateMarkers(player);
    }
}

void Release() {
    for (Pending& p : pending) mono::Unpin(p.task);
    pending.clear();
    equip.techs.clear();
    ClearArrival();
    ClearUndo();
    if (model && modelLock) SetUndo(false);
}

}
