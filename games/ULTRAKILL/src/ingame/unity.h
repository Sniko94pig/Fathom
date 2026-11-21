#pragma once
#include "ingame/mono.h"
#include <cmath>
#include <initializer_list>
#include <string>
#include <type_traits>
#include <vector>

namespace un {

using mono::Class;
using mono::Field;
using mono::Method;
using mono::Object;

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Fld {
    Class* cls = nullptr;
    Field* f = nullptr;
    int off = -1;
    int size = 0;
    bool isStatic = false;
    explicit operator bool() const { return f && off >= 0; }
};

struct Refs {
    Class *object, *component, *gameObject, *transform, *behaviour, *time, *cursor, *rigidbody, *canvas, *marshal;
    Fld cachedPtr;
    Method *findObjectsOfType, *findObjectsOfTypeInactive, *getInstanceID, *getName;
    Method *compTransform, *compGameObject, *compGetComponent, *compGetComponentInChildren, *compGetComponentsInChildren;
    Method *goGetComponent, *goTransform, *goActiveInHierarchy, *goSetActive;
    Method *trGetPosition, *trSetPosition, *trForward, *trRight, *trUp;
    Method *getEnabled, *setEnabled, *activeAndEnabled;
    Method *deltaTime, *unscaledDeltaTime, *getTimeScale, *setTimeScale, *frameCount;
    Method *cursorGetLock, *cursorSetLock, *cursorGetVisible, *cursorSetVisible;
    Method *rbGetVelocity, *rbSetVelocity;
    Method *addWillRender, *removeWillRender, *delegateForPointer;
    Class* willRenderType;

    Class *monoSingleton, *newMovement, *optionsManager, *cameraController, *gameStateManager, *gameState, *lockMode, *sceneHelper,
        *assistController, *statsManager;
    Method* msGetInstance;
    Fld nmDead, nmActivated, nmLevelOver;
    Fld omMainMenu, omPaused, omInIntro, omFrozen;
    Fld ccActivated;
    Fld gsmInstance, gsmOrder, gsmCursorLocked, gsmInputLocked, gsmCameraLocked;
    Method *gsmIsActive, *gsmRegister, *gsmPop, *gsCtor;
    Fld gsPlayerLock, gsCameraLock, gsCursorLock, gsPriority;
    int lockLock, lockUnlock;
    Fld shPending, shCurrent;
    Fld acCheats, smAsscon;
};

extern Refs R;
extern std::vector<std::string> missing;

bool Resolve(std::string& err);

Class* Cls(const char* fullName);
Class* Cls(const char* assembly, const char* fullName);
Fld F(Class* c, const char* name);
Fld Inner(const Fld& structField, const char* name);
Method* M(Class* c, const char* name, int args);
Method* MT(Class* c, const char* name, const std::vector<std::string>& params);
int Enum(Class* enumClass, const char* name);
void Missing(const std::string& what);

bool Alive(Object* o);
Object* Single(Class* type);
bool Fits(const Fld& f, size_t bytes);
Object* Static(const Fld& f);
void SetStatic(const Fld& f, Object* value);
Object* Ref(Object* o, const Fld& f);
void SetRef(Object* o, const Fld& f, Object* value);

template <class T>
T Val(Object* o, const Fld& f, T fallback = T{}) {
    return o && f && !f.isStatic && Fits(f, sizeof(T)) ? mono::Get<T>(o, f.off) : fallback;
}

template <class T>
void SetVal(Object* o, const Fld& f, T v) {
    if (o && f && !f.isStatic && Fits(f, sizeof(T))) mono::Put<T>(o, f.off, v);
}

template <class T>
T StaticVal(const Fld& f, T fallback = T{}) {
    T v = fallback;
    if (f && Fits(f, sizeof(T))) mono::StaticGet(f.cls, f.f, &v);
    return v;
}

template <class T>
void SetStaticVal(const Fld& f, T v) {
    static_assert(!std::is_pointer_v<T>);
    if (f && Fits(f, sizeof(T))) mono::StaticSet(f.cls, f.f, &v);
}

Object* Call(Method* m, void* self, std::initializer_list<void*> args = {}, bool* ok = nullptr);

template <class T>
T CallVal(Method* m, void* self, std::initializer_list<void*> args = {}, T fallback = T{}) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? mono::Result<T>(r, fallback) : fallback;
}

bool CallBool(Method* m, void* self, std::initializer_list<void*> args = {}, bool fallback = false);
float CallFloat(Method* m, void* self, std::initializer_list<void*> args = {}, float fallback = 0);
int CallInt(Method* m, void* self, std::initializer_list<void*> args = {}, int fallback = 0);

bool IsGameObject(Object* o);
Object* TransformOf(Object* compOrGo);
Object* GameObjectOf(Object* compOrGo);
Vec3 Position(Object* compOrGo);
void SetPosition(Object* compOrGo, Vec3 p);
Vec3 Forward(Object* compOrGo);
Vec3 Right(Object* compOrGo);
Vec3 Up(Object* compOrGo);
Object* Comp(Object* compOrGo, Class* type);
Object* CompInChildren(Object* compOrGo, Class* type);
std::vector<Object*> CompsInChildren(Object* compOrGo, Class* type, bool inactive = false);
std::vector<Object*> FindAll(Class* type, bool inactive = false);
bool Enabled(Object* behaviour);
void SetEnabled(Object* behaviour, bool on);
bool ActiveAndEnabled(Object* behaviour);
bool ActiveInHierarchy(Object* compOrGo);
void SetActive(Object* compOrGo, bool on);
Vec3 Velocity(Object* rigidbody);
void SetVelocity(Object* rigidbody, Vec3 v);
float DeltaTime();
float UnscaledDeltaTime();
float TimeScale();
void SetTimeScale(float scale);
int FrameCount();
int InstanceId(Object* o);
std::string Name(Object* o);
std::vector<Object*> ListItems(Object* list);

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

}
