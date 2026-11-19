#include "ingame/unity.h"
#include "ingame/log.h"
#include <algorithm>
#include <unordered_map>

namespace un {

Engine E;
std::vector<std::string> missing;

namespace {

const char* kCore = "UnityEngine.CoreModule";
const char* kPhysics = "UnityEngine.PhysicsModule";
const char* kAnimation = "UnityEngine.AnimationModule";
const char* kCorlib = "mscorlib";

std::unordered_map<Class*, Object*> typeObjects;

Object* TypeOf(Class* c) {
    if (!c) return nullptr;
    auto it = typeObjects.find(c);
    if (it != typeObjects.end()) return it->second;
    Object* t = il2::TypeObject(c);
    if (t) typeObjects[c] = t;
    return t;
}

bool IsGameObject(Object* o) { return o && E.gameObject && il2::ClassOf(o) == E.gameObject; }

}

void Missing(const char* what) {
    if (std::find(missing.begin(), missing.end(), what) == missing.end()) {
        missing.push_back(what);
        LOG("not found: %s", what);
    }
}

Class* C(const char* assembly, const char* ns, const char* name) {
    Class* c = il2::FindClass(assembly, ns, name);
    if (!c) Missing((std::string(ns && *ns ? ns : "") + (ns && *ns ? "." : "") + name).c_str());
    return c;
}

Fld F(Class* c, const char* name) {
    Fld out;
    if (!c) return out;
    out.f = il2::FindField(c, name);
    if (!out.f) {
        Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
        return out;
    }
    out.isStatic = il2::IsStatic(out.f);
    out.off = out.isStatic ? -1 : il2::Offset(out.f);
    return out;
}

Fld F(Class* c, std::initializer_list<const char*> names) {
    Fld out;
    if (!c) return out;
    for (const char* n : names) {
        Field* f = il2::FindField(c, n);
        if (!f) continue;
        out.f = f;
        out.isStatic = il2::IsStatic(f);
        out.off = out.isStatic ? -1 : il2::Offset(f);
        return out;
    }
    Missing((std::string(il2::ClassName(c)) + "." + *names.begin()).c_str());
    return out;
}

Method* M(Class* c, const char* name, int args) {
    if (!c) return nullptr;
    Method* m = il2::FindMethod(c, name, args);
    if (!m) Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
    return m;
}

Method* M(Class* c, const char* name, const std::vector<std::string>& params) {
    if (!c) return nullptr;
    Method* m = il2::FindMethod(c, name, params);
    if (!m) Missing((std::string(il2::ClassName(c)) + "." + name).c_str());
    return m;
}

bool Resolve(std::string& err) {
    E.object = C(kCore, "UnityEngine", "Object");
    E.component = C(kCore, "UnityEngine", "Component");
    E.gameObject = C(kCore, "UnityEngine", "GameObject");
    E.transform = C(kCore, "UnityEngine", "Transform");
    E.behaviour = C(kCore, "UnityEngine", "Behaviour");
    E.monoBehaviour = C(kCore, "UnityEngine", "MonoBehaviour");
    E.time = C(kCore, "UnityEngine", "Time");
    E.camera = C(kCore, "UnityEngine", "Camera");
    E.cursor = C(kCore, "UnityEngine", "Cursor");
    E.screen = C(kCore, "UnityEngine", "Screen");
    E.vector3 = C(kCore, "UnityEngine", "Vector3");
    E.quaternion = C(kCore, "UnityEngine", "Quaternion");
    E.physics = C(kPhysics, "UnityEngine", "Physics");
    E.collider = C(kPhysics, "UnityEngine", "Collider");
    E.rigidbody = C(kPhysics, "UnityEngine", "Rigidbody");
    E.characterController = C(kPhysics, "UnityEngine", "CharacterController");
    E.animator = C(kAnimation, "UnityEngine", "Animator");
    E.systemType = C(kCorlib, "System", "Type");
    E.systemString = C(kCorlib, "System", "String");
    E.systemObject = C(kCorlib, "System", "Object");
    E.systemInt32 = C(kCorlib, "System", "Int32");
    E.systemSingle = C(kCorlib, "System", "Single");

    E.cachedPtr = F(E.object, "m_CachedPtr");
    E.findObjectsOfType = M(E.object, "FindObjectsOfType", std::vector<std::string>{"System.Type"});
    E.findObjectsOfTypeInactive = M(E.object, "FindObjectsOfType", std::vector<std::string>{"System.Type", "System.Boolean"});
    E.findObjectOfType = M(E.object, "FindObjectOfType", std::vector<std::string>{"System.Type"});
    E.destroy = M(E.object, "Destroy", std::vector<std::string>{"UnityEngine.Object"});
    E.instantiate = M(E.object, "Instantiate",
                      std::vector<std::string>{"UnityEngine.Object", "UnityEngine.Vector3", "UnityEngine.Quaternion"});
    E.getName = E.object ? il2::Getter(E.object, "name") : nullptr;
    E.getInstanceID = M(E.object, "GetInstanceID", 0);
    E.dontDestroy = M(E.object, "DontDestroyOnLoad", std::vector<std::string>{"UnityEngine.Object"});

    E.compTransform = M(E.component, "get_transform", 0);
    E.compGameObject = M(E.component, "get_gameObject", 0);
    E.compGetComponent = M(E.component, "GetComponent", std::vector<std::string>{"System.Type"});
    E.compGetComponentInChildren = M(E.component, "GetComponentInChildren", std::vector<std::string>{"System.Type", "System.Boolean"});
    E.compGetComponentsInChildren = M(E.component, "GetComponentsInChildren", std::vector<std::string>{"System.Type", "System.Boolean"});
    E.compGetComponentInParent = M(E.component, "GetComponentInParent", std::vector<std::string>{"System.Type"});

    E.goGetComponent = M(E.gameObject, "GetComponent", std::vector<std::string>{"System.Type"});
    E.goAddComponent = M(E.gameObject, "AddComponent", std::vector<std::string>{"System.Type"});
    E.goTransform = M(E.gameObject, "get_transform", 0);
    E.goSetActive = M(E.gameObject, "SetActive", std::vector<std::string>{"System.Boolean"});
    E.goActiveSelf = M(E.gameObject, "get_activeSelf", 0);
    E.goActiveInHierarchy = M(E.gameObject, "get_activeInHierarchy", 0);

    E.trGetPosition = M(E.transform, "get_position", 0);
    E.trSetPosition = M(E.transform, "set_position", 1);
    E.trGetRotation = M(E.transform, "get_rotation", 0);
    E.trSetRotation = M(E.transform, "set_rotation", 1);
    E.trForward = M(E.transform, "get_forward", 0);
    E.trRight = M(E.transform, "get_right", 0);
    E.trUp = M(E.transform, "get_up", 0);
    E.trGetEuler = M(E.transform, "get_eulerAngles", 0);
    E.trSetEuler = M(E.transform, "set_eulerAngles", 1);
    E.trParent = M(E.transform, "get_parent", 0);

    E.getEnabled = M(E.behaviour, "get_enabled", 0);
    E.setEnabled = M(E.behaviour, "set_enabled", 1);

    E.deltaTime = M(E.time, "get_deltaTime", 0);
    E.unscaledDeltaTime = M(E.time, "get_unscaledDeltaTime", 0);
    E.getTimeScale = M(E.time, "get_timeScale", 0);
    E.setTimeScale = M(E.time, "set_timeScale", 1);
    E.frameCount = M(E.time, "get_frameCount", 0);
    E.realtime = M(E.time, "get_realtimeSinceStartup", 0);

    E.camMain = M(E.camera, "get_main", 0);
    E.camGetFov = M(E.camera, "get_fieldOfView", 0);
    E.camSetFov = M(E.camera, "set_fieldOfView", 1);
    E.camWorldToScreen = M(E.camera, "WorldToScreenPoint", std::vector<std::string>{"UnityEngine.Vector3"});

    E.colGetEnabled = M(E.collider, "get_enabled", 0);
    E.colSetEnabled = M(E.collider, "set_enabled", 1);

    E.rbGetVelocity = M(E.rigidbody, "get_velocity", 0);
    E.rbSetVelocity = M(E.rigidbody, "set_velocity", 1);
    E.rbGetKinematic = M(E.rigidbody, "get_isKinematic", 0);
    E.rbSetKinematic = M(E.rigidbody, "set_isKinematic", 1);
    E.rbGetDetect = M(E.rigidbody, "get_detectCollisions", 0);
    E.rbSetDetect = M(E.rigidbody, "set_detectCollisions", 1);
    E.rbGetGravity = M(E.rigidbody, "get_useGravity", 0);
    E.rbSetGravity = M(E.rigidbody, "set_useGravity", 1);
    E.rbMovePosition = M(E.rigidbody, "MovePosition", std::vector<std::string>{"UnityEngine.Vector3"});

    E.raycast = M(E.physics, "Raycast",
                  std::vector<std::string>{"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.RaycastHit&", "System.Single",
                                           "System.Int32"});

    E.cursorGetLock = M(E.cursor, "get_lockState", 0);
    E.cursorSetLock = M(E.cursor, "set_lockState", 1);
    E.cursorGetVisible = M(E.cursor, "get_visible", 0);
    E.cursorSetVisible = M(E.cursor, "set_visible", 1);

    if (!E.object || !E.component || !E.gameObject || !E.transform || !E.behaviour || !E.cachedPtr || !E.findObjectsOfType ||
        !E.compTransform || !E.compGameObject || !E.trGetPosition || !E.trSetPosition || !E.getEnabled || !E.setEnabled ||
        !E.deltaTime || !E.systemType) {
        err = "the game's engine functions were not found";
        return false;
    }
    return true;
}

bool Alive(Object* o) { return o && E.cachedPtr && il2::Get<void*>(o, E.cachedPtr.off) != nullptr; }

Object* Static(const Fld& f) { return StaticVal<Object*>(f, nullptr); }

Object* Ref(Object* o, const Fld& f) { return Val<Object*>(o, f, nullptr); }

void SetRefField(Object* o, const Fld& f, Object* value) {
    if (o && f && !f.isStatic) il2::SetRef(o, f.f, value);
}

Object* Call(Method* m, void* self, std::initializer_list<void*> args, bool* ok) {
    if (ok) *ok = false;
    if (!m) return nullptr;
    if (il2::IsInstance(m) && !self) return nullptr;
    void* a[16] = {};
    size_t n = 0;
    for (void* p : args) {
        if (n >= 16) break;
        a[n++] = p;
    }
    return il2::Invoke(m, self, n ? a : nullptr, ok);
}

bool CallBool(Method* m, void* self, std::initializer_list<void*> args, bool fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? il2::Result<bool>(r, fallback) : fallback;
}

float CallFloat(Method* m, void* self, std::initializer_list<void*> args, float fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? il2::Result<float>(r, fallback) : fallback;
}

int CallInt(Method* m, void* self, std::initializer_list<void*> args, int fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? il2::Result<int>(r, fallback) : fallback;
}

Vec3 CallVec3(Method* m, void* self, std::initializer_list<void*> args, Vec3 fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? il2::Result<Vec3>(r, fallback) : fallback;
}

Object* TransformOf(Object* compOrGo) {
    if (!Alive(compOrGo)) return nullptr;
    if (E.transform && il2::ClassOf(compOrGo) == E.transform) return compOrGo;
    return Call(IsGameObject(compOrGo) ? E.goTransform : E.compTransform, compOrGo);
}

Object* GameObjectOf(Object* comp) {
    if (!Alive(comp)) return nullptr;
    if (IsGameObject(comp)) return comp;
    return Call(E.compGameObject, comp);
}

Vec3 Position(Object* compOrGo) { return CallVec3(E.trGetPosition, TransformOf(compOrGo)); }

void SetPosition(Object* compOrGo, Vec3 p) {
    Object* t = TransformOf(compOrGo);
    if (t) Call(E.trSetPosition, t, {&p});
}

Quat Rotation(Object* compOrGo) {
    Object* t = TransformOf(compOrGo);
    bool ok = false;
    Object* r = t ? Call(E.trGetRotation, t, {}, &ok) : nullptr;
    return ok ? il2::Result<Quat>(r, Quat{}) : Quat{};
}

void SetRotation(Object* compOrGo, Quat q) {
    Object* t = TransformOf(compOrGo);
    if (t) Call(E.trSetRotation, t, {&q});
}

Vec3 Forward(Object* compOrGo) { return CallVec3(E.trForward, TransformOf(compOrGo), {}, Vec3{0, 0, 1}); }

Vec3 Right(Object* compOrGo) { return CallVec3(E.trRight, TransformOf(compOrGo), {}, Vec3{1, 0, 0}); }

Object* Comp(Object* compOrGo, Class* type) {
    if (!Alive(compOrGo) || !type) return nullptr;
    Object* t = TypeOf(type);
    if (!t) return nullptr;
    Object* r = IsGameObject(compOrGo) ? Call(E.goGetComponent, compOrGo, {t}) : Call(E.compGetComponent, compOrGo, {t});
    return Alive(r) ? r : nullptr;
}

Object* CompInChildren(Object* compOrGo, Class* type, bool inactive) {
    if (!Alive(compOrGo) || !type) return nullptr;
    Object* holder = IsGameObject(compOrGo) ? TransformOf(compOrGo) : compOrGo;
    Object* t = TypeOf(type);
    if (!holder || !t) return nullptr;
    Object* r = Call(E.compGetComponentInChildren, holder, {t, &inactive});
    return Alive(r) ? r : nullptr;
}

std::vector<Object*> CompsInChildren(Object* compOrGo, Class* type, bool inactive) {
    std::vector<Object*> out;
    if (!Alive(compOrGo) || !type) return out;
    Object* holder = IsGameObject(compOrGo) ? TransformOf(compOrGo) : compOrGo;
    Object* t = TypeOf(type);
    if (!holder || !t) return out;
    Object* arr = Call(E.compGetComponentsInChildren, holder, {t, &inactive});
    size_t n = il2::Length(arr);
    for (size_t i = 0; i < n; ++i)
        if (Object* o = il2::At(arr, i); Alive(o)) out.push_back(o);
    return out;
}

Object* CompInParent(Object* compOrGo, Class* type) {
    if (!Alive(compOrGo) || !type) return nullptr;
    Object* holder = IsGameObject(compOrGo) ? TransformOf(compOrGo) : compOrGo;
    Object* t = TypeOf(type);
    if (!holder || !t) return nullptr;
    Object* r = Call(E.compGetComponentInParent, holder, {t});
    return Alive(r) ? r : nullptr;
}

std::vector<Object*> FindAll(Class* type, bool inactive) {
    std::vector<Object*> out;
    Object* t = TypeOf(type);
    if (!t) return out;
    Object* arr = inactive && E.findObjectsOfTypeInactive ? Call(E.findObjectsOfTypeInactive, nullptr, {t, &inactive})
                                                          : Call(E.findObjectsOfType, nullptr, {t});
    size_t n = il2::Length(arr);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (Object* o = il2::At(arr, i); Alive(o)) out.push_back(o);
    return out;
}

Object* FindOne(Class* type) {
    Object* t = TypeOf(type);
    if (!t) return nullptr;
    if (E.findObjectOfType) {
        Object* r = Call(E.findObjectOfType, nullptr, {t});
        return Alive(r) ? r : nullptr;
    }
    std::vector<Object*> all = FindAll(type);
    return all.empty() ? nullptr : all.front();
}

bool Enabled(Object* behaviour) { return Alive(behaviour) && CallBool(E.getEnabled, behaviour); }

void SetEnabled(Object* behaviour, bool on) {
    if (Alive(behaviour)) Call(E.setEnabled, behaviour, {&on});
}

bool Active(Object* go) {
    Object* g = GameObjectOf(go);
    return g && CallBool(E.goActiveSelf, g);
}

void SetActive(Object* go, bool on) {
    Object* g = GameObjectOf(go);
    if (g) Call(E.goSetActive, g, {&on});
}

float DeltaTime() { return CallFloat(E.deltaTime, nullptr, {}, 0.0f); }

int FrameCount() { return CallInt(E.frameCount, nullptr, {}, 0); }

int InstanceId(Object* o) { return Alive(o) ? CallInt(E.getInstanceID, o) : 0; }

std::string Name(Object* o) { return Alive(o) ? il2::Str(Call(E.getName, o)) : std::string(); }

Object* MainCamera() {
    Object* c = Call(E.camMain, nullptr);
    return Alive(c) ? c : nullptr;
}

bool Raycast(Vec3 origin, Vec3 dir, float maxDist, RayHit& hit, int mask) {
    if (!E.raycast) return false;
    hit = RayHit{};
    return CallBool(E.raycast, nullptr, {&origin, &dir, &hit, &maxDist, &mask});
}

Object* Instantiate(Object* original, Vec3 pos, Quat rot) {
    if (!original || !E.instantiate) return nullptr;
    Object* r = Call(E.instantiate, nullptr, {original, &pos, &rot});
    return Alive(r) ? r : nullptr;
}

void Destroy(Object* o) {
    if (Alive(o) && E.destroy) Call(E.destroy, nullptr, {o});
}

}
