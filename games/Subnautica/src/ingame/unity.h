#pragma once
#include "ingame/mono.h"
#include <initializer_list>
#include <string>
#include <vector>

namespace un {

using mono::Class;
using mono::Field;
using mono::Method;
using mono::Object;

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

struct Bounds {
    Vec3 center, extents;
};

struct RayHit {
    Vec3 point, normal;
    uint32_t face;
    float distance;
    float uv[2];
    int collider;
    int pad[4];
};

struct Fld {
    Class* cls = nullptr;
    Field* f = nullptr;
    int off = -1;
    explicit operator bool() const { return f && off >= 0; }
};

struct Enums {
    int noRadiation = -1, noOxygen = -1, noAggression = -1, noBlueprints = -1, noCost = -1;
    int deconstruct = -1, damageNormal = -1, modeNormal = -1, leviathan = -1, shark = -1, qtiIgnore = -1;
};

struct Refs {
    Class *object, *component, *gameObject, *transform, *behaviour, *time, *camera, *collider, *rigidbody, *physics,
        *canvas, *marshal;
    Fld cachedPtr;
    Method *findObjectsOfType, *getInstanceID, *destroy, *dontDestroy;
    Method *compTransform, *compGameObject, *compGetComponent, *compGetComponentInChildren;
    Method *goGetComponent, *goAddComponent, *goCtor, *goTransform;
    Method *trGetPosition, *trSetPosition, *trForward, *trRight;
    Method *getEnabled, *setEnabled;
    Method *deltaTime, *getTimeScale, *setTimeScale, *frameCount;
    Method *camMain, *camView, *camProj;
    Method *colliderBounds, *colliderEnabledSet;
    Method *rbSetDetect, *rbGetDetect, *rbSetVelocity;
    Method *sphereCast, *capsuleCast;
    Method *addWillRender, *removeWillRender, *delegateForPointer;
    Class* willRenderType;

    Class *player, *liveMixin, *survival, *oxygenMgr, *gameModeUtils, *ecoTarget, *lastTarget, *playerController, *playerMotor,
        *groundMotor, *noCost, *gameInput, *uweUtils, *freezeTime, *crafterLogic, *constructorInput, *vfxConstructing,
        *dayNight, *baseHull, *leakable, *floodSim, *powerRelay, *basePowerRelay, *powerSource, *waterscape, *vehicle,
        *exosuit, *subRoot, *subControl, *attackCyclops, *cyclopsDestruction, *creature, *craftData, *creatureData,
        *resourceTracker, *techFragment, *blueprintTarget, *storyTarget, *supplyCrate, *language, *techTypeExt, *techType,
        *waitScreen, *creatureTrait;

    Fld playerMain, plLive, plOxygen, plRigidbody, plGround, plController, plSuffTime, plMode, plMotorMode;
    Method *plGetVehicle, *plCurrentSub, *plIsSuffocating, *plSuffReset, *plCinematic, *plUnderwater, *pcCenter;
    Fld lmHealth, lmInvincible;
    Method *lmMaxHealth, *lmAddHealth, *lmKill;
    Fld svFood, svWater;
    Method *oxAdd, *oxCapacity;
    Fld gmCheats;
    Method *gmActivate, *gmDeactivate;
    Method *ltGetTarget, *ltSetTarget;
    Fld pcUnderwater, pcGround, pcActive, pcInVehicle;
    Method *pcSetEnabled, *pcSetMotorMode;
    Fld pmForward, pmBackward, pmStrafe, pmVertical, pmWaterAccel, pmGroundAccel, pmAirAccel, pmDebugSpeed, pmRb, pmLayerMask;
    Fld gmJumping, jmpBaseHeight;
    Fld ncMain, ncFastBuild, ncFastScan;
    Method *giButtonHeld, *giClearInput;
    Fld giClearFrame;
    Method *uwGetLock, *uwSetLock;
    Fld ftCached;
    Method *ftHasFreezers, *ftUpdate;
    Fld clTech, clBegin, clEnd;
    Fld vcConstructed, vcTime, vcDelay, vcDone;
    Fld dnMain, dnMinDeep, dnMinSurface;
    Method *dnTime;
    Fld bhStrength, lkLive, fsCells;
    Fld prList, prInternal;
    Fld psPower, psMax;
    Method *prGetPower, *prGetMax, *prModify;
    Fld vhLive, vhForward, vhBackward, vhSide, vhVertical, exThrust;
    Method *vhEnergy, *vhAddEnergy;
    Fld srCyclops, srLive, srRelay, srDestroyed;
    Fld scForward, scPerEngine, scVertical, scPerBallast;
    Fld acTarget, acAggress;
    Method *acStop;
    Fld ctValue;
    Fld cdInteriorFx;
    Fld crLive;
    Method *cdGetTechType, *cdBehaviour;
    Fld rtTech, rtOverride;
    Fld bpUsed, scOpen;
    Method *lgMain, *lgGet, *ttAsString;
    Method *wsWaiting;
    Class *gameModeOption, *damageType, *behaviourType;
    Enums E;
};

extern Refs R;
extern std::vector<std::string> missing;

bool Resolve(std::string& err);

bool Alive(Object* o);
Object* Static(const Fld& f);
Object* Ref(Object* o, const Fld& f);
template <class T>
T Val(Object* o, const Fld& f, T fallback = T{}) {
    return o && f ? mono::Get<T>(o, f.off) : fallback;
}
template <class T>
void SetVal(Object* o, const Fld& f, T v) {
    if (o && f) mono::Put<T>(o, f.off, v);
}

Object* Call(Method* m, void* self, std::initializer_list<void*> args = {}, bool* ok = nullptr);
bool CallBool(Method* m, void* self, std::initializer_list<void*> args = {});
float CallFloat(Method* m, void* self, std::initializer_list<void*> args = {}, float fallback = 0);
int CallInt(Method* m, void* self, std::initializer_list<void*> args = {}, int fallback = 0);

Object* TransformOf(Object* comp);
Object* GameObjectOf(Object* comp);
Vec3 Position(Object* comp);
void SetPosition(Object* comp, Vec3 p);
Object* Comp(Object* compOrGo, Class* type);
Object* CompInChildren(Object* comp, Class* type);
std::vector<Object*> FindAll(Class* type);
bool Enabled(Object* behaviour);
void SetEnabled(Object* behaviour, bool on);
float DeltaTime();
int InstanceId(Object* o);
std::string TechName(int techType);
int TechOf(Object* go);
std::vector<Object*> ListItems(Object* list);
bool Loading();

}
