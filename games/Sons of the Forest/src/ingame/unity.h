#pragma once
#include "ingame/il2cpp.h"
#include <cmath>
#include <initializer_list>
#include <string>
#include <vector>

namespace un {

using il2::Class;
using il2::Field;
using il2::Method;
using il2::Object;

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Quat {
    float x = 0, y = 0, z = 0, w = 1;
};

struct RayHit {
    Vec3 point, normal;
    uint32_t face = 0;
    float distance = 0;
    Vec2 uv;
    int collider = 0;
};

struct Fld {
    Field* f = nullptr;
    int off = -1;
    bool isStatic = false;
    explicit operator bool() const { return f && (isStatic || off >= 0); }
};

Fld F(Class* c, const char* name);
Fld F(Class* c, std::initializer_list<const char*> names);
Method* M(Class* c, const char* name, int args);
Method* M(Class* c, const char* name, const std::vector<std::string>& params);
Class* C(const char* assembly, const char* ns, const char* name);

struct Engine {
    Class *object = nullptr, *component = nullptr, *gameObject = nullptr, *transform = nullptr, *behaviour = nullptr,
          *monoBehaviour = nullptr, *time = nullptr, *camera = nullptr, *collider = nullptr, *rigidbody = nullptr,
          *physics = nullptr, *cursor = nullptr, *screen = nullptr, *characterController = nullptr, *animator = nullptr,
          *vector3 = nullptr, *quaternion = nullptr, *systemType = nullptr, *systemString = nullptr, *systemObject = nullptr,
          *systemInt32 = nullptr, *systemSingle = nullptr;
    Fld cachedPtr;
    Method *findObjectsOfType = nullptr, *findObjectsOfTypeInactive = nullptr, *findObjectOfType = nullptr, *destroy = nullptr,
           *instantiate = nullptr, *getName = nullptr, *getInstanceID = nullptr, *dontDestroy = nullptr;
    Method *compTransform = nullptr, *compGameObject = nullptr, *compGetComponent = nullptr, *compGetComponentInChildren = nullptr,
           *compGetComponentsInChildren = nullptr, *compGetComponentInParent = nullptr;
    Method *goGetComponent = nullptr, *goAddComponent = nullptr, *goTransform = nullptr, *goSetActive = nullptr,
           *goActiveSelf = nullptr, *goActiveInHierarchy = nullptr;
    Method *trGetPosition = nullptr, *trSetPosition = nullptr, *trGetRotation = nullptr, *trSetRotation = nullptr,
           *trForward = nullptr, *trRight = nullptr, *trUp = nullptr, *trGetEuler = nullptr, *trSetEuler = nullptr,
           *trParent = nullptr;
    Method *getEnabled = nullptr, *setEnabled = nullptr;
    Method *deltaTime = nullptr, *unscaledDeltaTime = nullptr, *getTimeScale = nullptr, *setTimeScale = nullptr,
           *frameCount = nullptr, *realtime = nullptr;
    Method *camMain = nullptr, *camGetFov = nullptr, *camSetFov = nullptr, *camWorldToScreen = nullptr;
    Method *colGetEnabled = nullptr, *colSetEnabled = nullptr;
    Method *rbGetVelocity = nullptr, *rbSetVelocity = nullptr, *rbGetKinematic = nullptr, *rbSetKinematic = nullptr,
           *rbGetDetect = nullptr, *rbSetDetect = nullptr, *rbGetGravity = nullptr, *rbSetGravity = nullptr,
           *rbMovePosition = nullptr;
    Method *raycast = nullptr;
    Method *cursorGetLock = nullptr, *cursorSetLock = nullptr, *cursorGetVisible = nullptr, *cursorSetVisible = nullptr;
};

extern Engine E;
extern std::vector<std::string> missing;

bool Resolve(std::string& err);
void Missing(const char* what);

bool Alive(Object* o);
Object* Static(const Fld& f);
Object* Ref(Object* o, const Fld& f);

template <class T>
T Val(Object* o, const Fld& f, T fallback = T{}) {
    return o && f && !f.isStatic ? il2::Get<T>(o, f.off) : fallback;
}

template <class T>
void SetVal(Object* o, const Fld& f, T v) {
    if (o && f && !f.isStatic) il2::Put<T>(o, f.off, v);
}

template <class T>
T StaticVal(const Fld& f, T fallback = T{}) {
    if (!f || !f.isStatic) return fallback;
    T v{};
    return il2::StaticGet(f.f, &v, sizeof(T)) ? v : fallback;
}

template <class T>
bool SetStaticVal(const Fld& f, T v) {
    if (!f || !f.isStatic) return false;
    return il2::StaticSet(f.f, &v);
}

void SetRefField(Object* o, const Fld& f, Object* value);

Object* Call(Method* m, void* self, std::initializer_list<void*> args = {}, bool* ok = nullptr);
bool CallBool(Method* m, void* self, std::initializer_list<void*> args = {}, bool fallback = false);
float CallFloat(Method* m, void* self, std::initializer_list<void*> args = {}, float fallback = 0);
int CallInt(Method* m, void* self, std::initializer_list<void*> args = {}, int fallback = 0);
Vec3 CallVec3(Method* m, void* self, std::initializer_list<void*> args = {}, Vec3 fallback = {});

Object* TransformOf(Object* compOrGo);
Object* GameObjectOf(Object* comp);
Vec3 Position(Object* compOrGo);
void SetPosition(Object* compOrGo, Vec3 p);
Quat Rotation(Object* compOrGo);
void SetRotation(Object* compOrGo, Quat q);
Vec3 Forward(Object* compOrGo);
Vec3 Right(Object* compOrGo);
Object* Comp(Object* compOrGo, Class* type);
Object* CompInChildren(Object* compOrGo, Class* type, bool inactive = false);
std::vector<Object*> CompsInChildren(Object* compOrGo, Class* type, bool inactive = false);
Object* CompInParent(Object* compOrGo, Class* type);
std::vector<Object*> FindAll(Class* type, bool inactive = false);
Object* FindOne(Class* type);
bool Enabled(Object* behaviour);
void SetEnabled(Object* behaviour, bool on);
bool Active(Object* go);
void SetActive(Object* go, bool on);
float DeltaTime();
int FrameCount();
int InstanceId(Object* o);
std::string Name(Object* o);
Object* MainCamera();
bool Raycast(Vec3 origin, Vec3 dir, float maxDist, RayHit& hit, int mask = ~0);
Object* Instantiate(Object* original, Vec3 pos, Quat rot);
void Destroy(Object* o);

inline Vec3 Add(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
inline Vec3 Sub(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline Vec3 Mul(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
inline float Dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline float Len(Vec3 a) { return std::sqrt(Dot(a, a)); }
inline float Dist(Vec3 a, Vec3 b) { return Len(Sub(a, b)); }
inline Vec3 Norm(Vec3 a) {
    float l = Len(a);
    return l > 1e-6f ? Mul(a, 1.0f / l) : Vec3{};
}
inline Vec3 Flat(Vec3 a) { return {a.x, 0, a.z}; }

}
