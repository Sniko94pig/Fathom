#include "ingame/unity.h"
#include "ingame/log.h"
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace un {

Refs R{};
std::vector<std::string> missing;

namespace {

mono::Image *core, *phys, *uim, *game, *corlib;
std::unordered_map<std::string, mono::Image*> images;

const char* kSearch[] = {"Assembly-CSharp", "UnityEngine.CoreModule", "UnityEngine.PhysicsModule", "UnityEngine.AnimationModule",
                         "UnityEngine.UIModule", "UnityEngine.UI", "Unity.InputSystem", "UnityEngine.AudioModule", "mscorlib"};

mono::Image* Img(const char* assembly) {
    if (!assembly || !*assembly) return nullptr;
    if (_stricmp(assembly, "mscorlib") == 0) return mono::Corlib();
    auto it = images.find(assembly);
    if (it != images.end()) return it->second;
    mono::Image* img = mono::FindImage(assembly);
    if (img) images[assembly] = img;
    return img;
}

Class* Find(mono::Image* img, const char* fullName) {
    std::string s(fullName);
    size_t slash = s.find('/');
    size_t dot = s.rfind('.', slash);
    std::string ns = dot == std::string::npos ? std::string() : s.substr(0, dot);
    std::string name = dot == std::string::npos ? s : s.substr(dot + 1);
    return mono::FindClass(img, ns.c_str(), name.c_str());
}

Class* C(mono::Image* img, const char* ns, const char* name) {
    Class* c = mono::FindClass(img, ns, name);
    if (!c) missing.push_back(std::string("class ") + (ns && *ns ? std::string(ns) + "." : "") + name);
    return c;
}

bool IsA(Object* o, Class* type) { return o && type && mono::IsSubclass(mono::ClassOf(o), type); }

void Need(std::string& list, bool ok, const char* what) {
    if (ok) return;
    if (!list.empty()) list += ", ";
    list += what;
}

void ResolveEngine() {
    R.object = C(core, "UnityEngine", "Object");
    R.component = C(core, "UnityEngine", "Component");
    R.gameObject = C(core, "UnityEngine", "GameObject");
    R.transform = C(core, "UnityEngine", "Transform");
    R.behaviour = C(core, "UnityEngine", "Behaviour");
    R.time = C(core, "UnityEngine", "Time");
    R.cursor = C(core, "UnityEngine", "Cursor");
    R.rigidbody = phys ? C(phys, "UnityEngine", "Rigidbody") : nullptr;
    R.canvas = C(uim, "UnityEngine", "Canvas");
    R.marshal = C(corlib, "System.Runtime.InteropServices", "Marshal");

    R.cachedPtr = F(R.object, "m_CachedPtr");
    R.findObjectsOfType = MT(R.object, "FindObjectsOfType", {"System.Type"});
    R.findObjectsOfTypeInactive = MT(R.object, "FindObjectsOfType", {"System.Type", "System.Boolean"});
    R.getInstanceID = M(R.object, "GetInstanceID", 0);
    R.getName = M(R.object, "get_name", 0);
    R.compTransform = M(R.component, "get_transform", 0);
    R.compGameObject = M(R.component, "get_gameObject", 0);
    R.compGetComponent = MT(R.component, "GetComponent", {"System.Type"});
    R.compGetComponentInChildren = MT(R.component, "GetComponentInChildren", {"System.Type"});
    R.compGetComponentsInChildren = MT(R.component, "GetComponentsInChildren", {"System.Type", "System.Boolean"});
    R.goGetComponent = MT(R.gameObject, "GetComponent", {"System.Type"});
    R.goTransform = M(R.gameObject, "get_transform", 0);
    R.goActiveInHierarchy = M(R.gameObject, "get_activeInHierarchy", 0);
    R.goSetActive = M(R.gameObject, "SetActive", 1);
    R.trGetPosition = M(R.transform, "get_position", 0);
    R.trSetPosition = M(R.transform, "set_position", 1);
    R.trForward = M(R.transform, "get_forward", 0);
    R.trRight = M(R.transform, "get_right", 0);
    R.trUp = M(R.transform, "get_up", 0);
    R.getEnabled = M(R.behaviour, "get_enabled", 0);
    R.setEnabled = M(R.behaviour, "set_enabled", 1);
    R.activeAndEnabled = M(R.behaviour, "get_isActiveAndEnabled", 0);
    R.deltaTime = M(R.time, "get_deltaTime", 0);
    R.unscaledDeltaTime = M(R.time, "get_unscaledDeltaTime", 0);
    R.getTimeScale = M(R.time, "get_timeScale", 0);
    R.setTimeScale = M(R.time, "set_timeScale", 1);
    R.frameCount = M(R.time, "get_frameCount", 0);
    R.cursorGetLock = M(R.cursor, "get_lockState", 0);
    R.cursorSetLock = M(R.cursor, "set_lockState", 1);
    R.cursorGetVisible = M(R.cursor, "get_visible", 0);
    R.cursorSetVisible = M(R.cursor, "set_visible", 1);
    R.rbGetVelocity = M(R.rigidbody, "get_velocity", 0);
    R.rbSetVelocity = M(R.rigidbody, "set_velocity", 1);
    R.addWillRender = M(R.canvas, "add_willRenderCanvases", 1);
    R.removeWillRender = M(R.canvas, "remove_willRenderCanvases", 1);
    R.willRenderType = mono::ParamClass(R.addWillRender, 0);
    R.delegateForPointer = MT(R.marshal, "GetDelegateForFunctionPointer", {"System.IntPtr", "System.Type"});
    R.camera = C(core, "UnityEngine", "Camera");
    R.camMain = M(R.camera, "get_main", 0);
    R.camToViewport = MT(R.camera, "WorldToViewportPoint", {"UnityEngine.Vector3"});
}

void ResolveGame() {
    R.monoSingleton = C(game, "", "MonoSingleton");
    R.newMovement = C(game, "", "NewMovement");
    R.optionsManager = C(game, "", "OptionsManager");
    R.cameraController = C(game, "", "CameraController");
    R.gameStateManager = C(game, "", "GameStateManager");
    R.gameState = C(game, "", "GameState");
    R.lockMode = C(game, "", "LockMode");
    R.sceneHelper = C(game, "", "SceneHelper");
    R.assistController = C(game, "", "AssistController");
    R.statsManager = C(game, "", "StatsManager");

    R.msGetInstance = MT(R.monoSingleton, "GetInstance", {"System.Type"});
    R.nmDead = F(R.newMovement, "dead");
    R.nmActivated = F(R.newMovement, "activated");
    R.nmLevelOver = F(R.newMovement, "levelOver");
    R.omMainMenu = F(R.optionsManager, "mainMenu");
    R.omPaused = F(R.optionsManager, "paused");
    R.omInIntro = F(R.optionsManager, "inIntro");
    R.omFrozen = F(R.optionsManager, "frozen");
    R.ccActivated = F(R.cameraController, "activated");
    R.gsmInstance = F(R.gameStateManager, "<Instance>k__BackingField");
    R.gsmOrder = F(R.gameStateManager, "stateOrder");
    R.gsmCursorLocked = F(R.gameStateManager, "<CursorLocked>k__BackingField");
    R.gsmInputLocked = F(R.gameStateManager, "<PlayerInputLocked>k__BackingField");
    R.gsmCameraLocked = F(R.gameStateManager, "<CameraLocked>k__BackingField");
    R.gsmIsActive = MT(R.gameStateManager, "IsStateActive", {"System.String"});
    R.gsmRegister = MT(R.gameStateManager, "RegisterState", {"GameState"});
    R.gsmPop = MT(R.gameStateManager, "PopState", {"System.String"});
    R.gsCtor = MT(R.gameState, ".ctor", {"System.String"});
    R.gsPlayerLock = F(R.gameState, "playerInputLock");
    R.gsCameraLock = F(R.gameState, "cameraInputLock");
    R.gsCursorLock = F(R.gameState, "cursorLock");
    R.gsPriority = F(R.gameState, "priority");
    R.lockLock = R.lockMode ? mono::EnumValue(R.lockMode, "Lock") : -1;
    R.lockUnlock = R.lockMode ? mono::EnumValue(R.lockMode, "Unlock") : -1;
    if (R.lockLock < 0) R.lockLock = 1;
    if (R.lockUnlock < 0) R.lockUnlock = 2;
    R.shPending = F(R.sceneHelper, "<PendingScene>k__BackingField");
    R.shCurrent = F(R.sceneHelper, "<CurrentScene>k__BackingField");
    R.acCheats = F(R.assistController, "cheatsEnabled");
    R.smAsscon = F(R.statsManager, "asscon");
    R.leaderboard = C(game, "", "LeaderboardController");
    R.lbLevel = MT(R.leaderboard, "SubmitLevelScore",
                   {"System.String", "System.Int32", "System.Single", "System.Int32", "System.Int32", "System.Int32", "System.Boolean"});
    R.lbCyber = MT(R.leaderboard, "SubmitCyberGrindScore", {"System.Int32", "System.Single", "System.Int32", "System.Int32", "System.Single"});
    R.lbFish = MT(R.leaderboard, "SubmitFishSize", {"System.Int32"});
}

}

bool Resolve(std::string& err) {
    missing.clear();
    images.clear();
    core = Img("UnityEngine.CoreModule");
    phys = Img("UnityEngine.PhysicsModule");
    uim = Img("UnityEngine.UIModule");
    game = Img("Assembly-CSharp");
    corlib = mono::Corlib();
    if (!core || !uim || !game || !corlib) {
        err = "the game's scripts are not loaded yet";
        return false;
    }
    R = Refs{};
    ResolveEngine();
    ResolveGame();
    std::string hard;
    Need(hard, (bool)R.cachedPtr, "Object.m_CachedPtr");
    Need(hard, R.addWillRender && R.removeWillRender && R.willRenderType, "Canvas.willRenderCanvases");
    Need(hard, R.delegateForPointer != nullptr, "Marshal.GetDelegateForFunctionPointer");
    Need(hard, R.frameCount != nullptr, "Time.frameCount");
    Need(hard, R.msGetInstance != nullptr, "MonoSingleton.GetInstance");
    if (!hard.empty()) {
        err = "the game's code is different from what Fathom expects (" + hard + ")";
        return false;
    }
    return true;
}

Class* Cls(const char* fullName) {
    if (!fullName || !*fullName) return nullptr;
    for (const char* a : kSearch) {
        mono::Image* img = Img(a);
        Class* c = img ? Find(img, fullName) : nullptr;
        if (c) return c;
    }
    missing.push_back(std::string("class ") + fullName);
    return nullptr;
}

Class* Cls(const char* assembly, const char* fullName) {
    if (!fullName || !*fullName) return nullptr;
    mono::Image* img = Img(assembly);
    Class* c = img ? Find(img, fullName) : nullptr;
    if (!c) missing.push_back(std::string("class ") + fullName + " in " + (assembly ? assembly : "?"));
    return c;
}

Fld F(Class* c, const char* name) {
    Fld f;
    f.cls = c;
    f.f = c ? mono::FindField(c, name) : nullptr;
    if (f.f) {
        f.off = mono::Offset(f.f);
        f.size = mono::ValueSize(f.f);
        f.isStatic = mono::IsStatic(f.f);
        if (Class* owner = mono::FieldOwner(f.f)) f.cls = owner;
    }
    if (c && !f.f) missing.push_back(std::string(mono::ClassName(c)) + "." + name);
    return f;
}

Fld Inner(const Fld& structField, const char* name) {
    if (!structField || structField.isStatic) return {};
    Class* sc = mono::FieldClass(structField.f);
    if (!mono::IsValueType(sc)) {
        missing.push_back(std::string(mono::ClassName(sc)) + " is not a struct (" + name + ")");
        return {};
    }
    Fld in = F(sc, name);
    if (!in || in.isStatic) return {};
    in.off = structField.off + in.off - (int)(2 * sizeof(void*));
    in.cls = structField.cls;
    return in;
}

Method* M(Class* c, const char* name, int args) {
    Method* m = c ? mono::FindMethod(c, name, args) : nullptr;
    if (c && !m) missing.push_back(std::string(mono::ClassName(c)) + "." + name + "(" + std::to_string(args) + ")");
    return m;
}

Method* MT(Class* c, const char* name, const std::vector<std::string>& params) {
    Method* m = c ? mono::FindMethod(c, name, params) : nullptr;
    if (c && !m) {
        std::string sig;
        for (const std::string& p : params) sig += (sig.empty() ? "" : ", ") + p;
        missing.push_back(std::string(mono::ClassName(c)) + "." + name + "(" + sig + ")");
    }
    return m;
}

int Enum(Class* enumClass, const char* name) {
    int v = enumClass ? mono::EnumValue(enumClass, name) : -1;
    if (enumClass && v < 0) missing.push_back(std::string(mono::ClassName(enumClass)) + "." + name);
    return v;
}

void Missing(const std::string& what) { missing.push_back(what); }

bool Alive(Object* o) {
    if (!o || !R.cachedPtr) return false;
    return !IsA(o, R.object) || mono::Get<void*>(o, R.cachedPtr.off) != nullptr;
}

Object* Single(Class* type) {
    if (!type || !R.msGetInstance) return nullptr;
    Object* t = mono::TypeObject(type);
    Object* o = t ? Call(R.msGetInstance, nullptr, {t}) : nullptr;
    return Alive(o) ? o : nullptr;
}

bool Fits(const Fld& f, size_t bytes) {
    if (f.size <= 0 || (size_t)f.size == bytes) return true;
    static std::mutex lock;
    static std::unordered_set<Field*> told;
    std::lock_guard<std::mutex> g(lock);
    if (told.insert(f.f).second)
        LOG("%s.%s has %d bytes but was used as %d bytes, access refused", mono::ClassName(f.cls), mono::FieldName(f.f), f.size,
            (int)bytes);
    return false;
}

Object* Static(const Fld& f) {
    Object* o = nullptr;
    if (f && Fits(f, sizeof(o))) mono::StaticGet(f.cls, f.f, &o);
    return o;
}

void SetStatic(const Fld& f, Object* value) {
    if (f && Fits(f, sizeof(value))) mono::StaticSet(f.cls, f.f, value);
}

Object* Ref(Object* o, const Fld& f) {
    if (!o || !f || f.isStatic || !Fits(f, sizeof(Object*))) return nullptr;
    return mono::Get<Object*>(o, f.off);
}

void SetRef(Object* o, const Fld& f, Object* value) {
    if (o && f && !f.isStatic && Fits(f, sizeof(value))) mono::SetRef(o, f.f, value);
}

Object* Call(Method* m, void* self, std::initializer_list<void*> args, bool* ok) {
    void* buf[16];
    size_t n = 0;
    for (void* a : args)
        if (n < 16) buf[n++] = a;
    return mono::Invoke(m, self, n ? buf : nullptr, ok);
}

bool CallBool(Method* m, void* self, std::initializer_list<void*> args, bool fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? mono::Result<uint8_t>(r, (uint8_t)(fallback ? 1 : 0)) != 0 : fallback;
}

float CallFloat(Method* m, void* self, std::initializer_list<void*> args, float fallback) { return CallVal<float>(m, self, args, fallback); }

int CallInt(Method* m, void* self, std::initializer_list<void*> args, int fallback) { return CallVal<int>(m, self, args, fallback); }

bool IsGameObject(Object* o) { return IsA(o, R.gameObject); }

Object* TransformOf(Object* compOrGo) {
    if (!Alive(compOrGo)) return nullptr;
    if (IsA(compOrGo, R.transform)) return compOrGo;
    Object* t = IsGameObject(compOrGo) ? Call(R.goTransform, compOrGo) : Call(R.compTransform, compOrGo);
    return Alive(t) ? t : nullptr;
}

Object* GameObjectOf(Object* compOrGo) {
    if (!Alive(compOrGo)) return nullptr;
    if (IsGameObject(compOrGo)) return compOrGo;
    Object* go = Call(R.compGameObject, compOrGo);
    return Alive(go) ? go : nullptr;
}

Vec3 Position(Object* compOrGo) {
    Object* t = TransformOf(compOrGo);
    return t ? CallVal<Vec3>(R.trGetPosition, t) : Vec3{};
}

void SetPosition(Object* compOrGo, Vec3 p) {
    Object* t = TransformOf(compOrGo);
    if (t) Call(R.trSetPosition, t, {&p});
}

Vec3 Forward(Object* compOrGo) {
    Object* t = TransformOf(compOrGo);
    return t ? CallVal<Vec3>(R.trForward, t) : Vec3{};
}

Vec3 Right(Object* compOrGo) {
    Object* t = TransformOf(compOrGo);
    return t ? CallVal<Vec3>(R.trRight, t) : Vec3{};
}

Vec3 Up(Object* compOrGo) {
    Object* t = TransformOf(compOrGo);
    return t ? CallVal<Vec3>(R.trUp, t) : Vec3{};
}

Object* Comp(Object* compOrGo, Class* type) {
    if (!Alive(compOrGo) || !type) return nullptr;
    Object* t = mono::TypeObject(type);
    if (!t) return nullptr;
    Object* r = IsGameObject(compOrGo) ? Call(R.goGetComponent, compOrGo, {t}) : Call(R.compGetComponent, compOrGo, {t});
    return Alive(r) ? r : nullptr;
}

Object* CompInChildren(Object* compOrGo, Class* type) {
    Object* base = IsGameObject(compOrGo) ? TransformOf(compOrGo) : compOrGo;
    if (!Alive(base) || !type) return nullptr;
    Object* t = mono::TypeObject(type);
    Object* r = t ? Call(R.compGetComponentInChildren, base, {t}) : nullptr;
    return Alive(r) ? r : nullptr;
}

std::vector<Object*> CompsInChildren(Object* compOrGo, Class* type, bool inactive) {
    std::vector<Object*> out;
    Object* base = IsGameObject(compOrGo) ? TransformOf(compOrGo) : compOrGo;
    if (!Alive(base) || !type) return out;
    Object* t = mono::TypeObject(type);
    Object* arr = t ? Call(R.compGetComponentsInChildren, base, {t, &inactive}) : nullptr;
    size_t n = mono::Length(arr);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Object* o = mono::At(arr, i);
        if (Alive(o)) out.push_back(o);
    }
    return out;
}

std::vector<Object*> FindAll(Class* type, bool inactive) {
    std::vector<Object*> out;
    if (!type) return out;
    Object* t = mono::TypeObject(type);
    Object* arr = nullptr;
    if (t && inactive) arr = Call(R.findObjectsOfTypeInactive, nullptr, {t, &inactive});
    else if (t) arr = Call(R.findObjectsOfType, nullptr, {t});
    size_t n = mono::Length(arr);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Object* o = mono::At(arr, i);
        if (Alive(o)) out.push_back(o);
    }
    return out;
}

bool Enabled(Object* behaviour) { return Alive(behaviour) && IsA(behaviour, R.behaviour) && CallBool(R.getEnabled, behaviour); }

void SetEnabled(Object* behaviour, bool on) {
    if (!Alive(behaviour) || !IsA(behaviour, R.behaviour)) return;
    Call(R.setEnabled, behaviour, {&on});
}

bool ActiveAndEnabled(Object* behaviour) {
    return Alive(behaviour) && IsA(behaviour, R.behaviour) && CallBool(R.activeAndEnabled, behaviour);
}

bool ActiveInHierarchy(Object* compOrGo) {
    Object* go = GameObjectOf(compOrGo);
    return go && CallBool(R.goActiveInHierarchy, go);
}

void SetActive(Object* compOrGo, bool on) {
    Object* go = GameObjectOf(compOrGo);
    if (go) Call(R.goSetActive, go, {&on});
}

Vec3 Velocity(Object* rigidbody) {
    if (!Alive(rigidbody) || !IsA(rigidbody, R.rigidbody)) return {};
    return CallVal<Vec3>(R.rbGetVelocity, rigidbody);
}

void SetVelocity(Object* rigidbody, Vec3 v) {
    if (Alive(rigidbody) && IsA(rigidbody, R.rigidbody)) Call(R.rbSetVelocity, rigidbody, {&v});
}

float DeltaTime() { return CallFloat(R.deltaTime, nullptr); }

float UnscaledDeltaTime() { return CallFloat(R.unscaledDeltaTime, nullptr); }

float TimeScale() { return CallFloat(R.getTimeScale, nullptr, {}, 1.0f); }

void SetTimeScale(float scale) { Call(R.setTimeScale, nullptr, {&scale}); }

int FrameCount() { return CallInt(R.frameCount, nullptr, {}, -1); }

int InstanceId(Object* o) { return Alive(o) ? CallInt(R.getInstanceID, o) : 0; }

std::string Name(Object* o) { return Alive(o) ? mono::Str(Call(R.getName, o)) : std::string(); }

std::vector<Object*> ListItems(Object* list) {
    std::vector<Object*> out;
    if (!list) return out;
    Class* c = mono::ClassOf(list);
    Field* items = mono::FindField(c, "_items");
    Field* size = mono::FindField(c, "_size");
    if (!items || !size) return out;
    Object* arr = mono::Get<Object*>(list, mono::Offset(items));
    int n = mono::Get<int>(list, mono::Offset(size));
    size_t len = mono::Length(arr);
    for (int i = 0; i < n && (size_t)i < len; ++i) out.push_back(mono::At(arr, (size_t)i));
    return out;
}

Object* MainCamera() {
    Object* cam = R.camMain ? Call(R.camMain, nullptr) : nullptr;
    return Alive(cam) && IsA(cam, R.camera) ? cam : nullptr;
}

bool Viewport(Vec3 world, Vec2& out, Object* camera) {
    Object* cam = camera ? camera : MainCamera();
    if (!R.camToViewport || !Alive(cam) || !IsA(cam, R.camera)) return false;
    bool ok = false;
    Object* r = Call(R.camToViewport, cam, {&world}, &ok);
    if (!ok || !r) return false;
    Vec3 v = mono::Result<Vec3>(r);
    if (!(v.z > 0.0f) || !std::isfinite(v.x) || !std::isfinite(v.y)) return false;
    out = {v.x, 1.0f - v.y};
    return true;
}

}
