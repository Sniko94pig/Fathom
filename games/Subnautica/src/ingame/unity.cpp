#include "ingame/unity.h"
#include "ingame/log.h"
#include <unordered_map>

namespace un {

Refs R{};
std::vector<std::string> missing;

namespace {

mono::Image *core, *phys, *uim, *game, *first, *corlib;
std::unordered_map<int, std::string> names;

Class* C(mono::Image* img, const char* ns, const char* name) {
    Class* c = mono::FindClass(img, ns, name);
    if (!c) missing.push_back(std::string("class ") + (ns && *ns ? std::string(ns) + "." : "") + name);
    return c;
}

Fld F(Class* c, const char* name) {
    Fld f;
    f.cls = c;
    f.f = c ? mono::FindField(c, name) : nullptr;
    f.off = f.f ? mono::Offset(f.f) : -1;
    if (c && !f.f) missing.push_back(std::string(mono::ClassName(c)) + "." + name);
    return f;
}

Method* M(Class* c, const char* name, int args) {
    Method* m = c ? mono::FindMethod(c, name, args) : nullptr;
    if (c && !m) missing.push_back(std::string(mono::ClassName(c)) + "." + name + "()");
    return m;
}

Method* MT(Class* c, const char* name, const std::vector<std::string>& params) {
    Method* m = c ? mono::FindMethod(c, name, params) : nullptr;
    if (c && !m) missing.push_back(std::string(mono::ClassName(c)) + "." + name + "(typed)");
    return m;
}

}

bool Resolve(std::string& err) {
    missing.clear();
    core = mono::FindImage("UnityEngine.CoreModule");
    phys = mono::FindImage("UnityEngine.PhysicsModule");
    uim = mono::FindImage("UnityEngine.UIModule");
    game = mono::FindImage("Assembly-CSharp");
    first = mono::FindImage("Assembly-CSharp-firstpass");
    corlib = mono::Corlib();
    if (!core || !game || !first || !corlib) {
        err = "the game's scripts are not loaded yet";
        return false;
    }

    R.object = C(core, "UnityEngine", "Object");
    R.component = C(core, "UnityEngine", "Component");
    R.gameObject = C(core, "UnityEngine", "GameObject");
    R.transform = C(core, "UnityEngine", "Transform");
    R.behaviour = C(core, "UnityEngine", "Behaviour");
    R.time = C(core, "UnityEngine", "Time");
    R.camera = C(core, "UnityEngine", "Camera");
    R.collider = C(phys, "UnityEngine", "Collider");
    R.rigidbody = C(phys, "UnityEngine", "Rigidbody");
    R.physics = C(phys, "UnityEngine", "Physics");
    R.canvas = C(uim, "UnityEngine", "Canvas");
    R.marshal = C(corlib, "System.Runtime.InteropServices", "Marshal");

    R.cachedPtr = F(R.object, "m_CachedPtr");
    R.findObjectsOfType = MT(R.object, "FindObjectsOfType", {"System.Type"});
    R.getInstanceID = M(R.object, "GetInstanceID", 0);
    R.destroy = MT(R.object, "Destroy", {"UnityEngine.Object"});
    R.dontDestroy = MT(R.object, "DontDestroyOnLoad", {"UnityEngine.Object"});
    R.compTransform = M(R.component, "get_transform", 0);
    R.compGameObject = M(R.component, "get_gameObject", 0);
    R.compGetComponent = MT(R.component, "GetComponent", {"System.Type"});
    R.compGetComponentInChildren = MT(R.component, "GetComponentInChildren", {"System.Type"});
    R.goGetComponent = MT(R.gameObject, "GetComponent", {"System.Type"});
    R.goAddComponent = MT(R.gameObject, "AddComponent", {"System.Type"});
    R.goCtor = MT(R.gameObject, ".ctor", {"System.String"});
    R.goTransform = M(R.gameObject, "get_transform", 0);
    R.trGetPosition = M(R.transform, "get_position", 0);
    R.trSetPosition = M(R.transform, "set_position", 1);
    R.trForward = M(R.transform, "get_forward", 0);
    R.trRight = M(R.transform, "get_right", 0);
    R.getEnabled = M(R.behaviour, "get_enabled", 0);
    R.setEnabled = M(R.behaviour, "set_enabled", 1);
    R.deltaTime = M(R.time, "get_deltaTime", 0);
    R.getTimeScale = M(R.time, "get_timeScale", 0);
    R.setTimeScale = M(R.time, "set_timeScale", 1);
    R.frameCount = M(R.time, "get_frameCount", 0);
    R.camMain = M(R.camera, "get_main", 0);
    R.camView = M(R.camera, "get_worldToCameraMatrix", 0);
    R.camProj = M(R.camera, "get_nonJitteredProjectionMatrix", 0);
    R.colliderBounds = M(R.collider, "get_bounds", 0);
    R.colliderEnabledSet = M(R.collider, "set_enabled", 1);
    R.rbSetDetect = M(R.rigidbody, "set_detectCollisions", 1);
    R.rbGetDetect = M(R.rigidbody, "get_detectCollisions", 0);
    R.rbSetVelocity = M(R.rigidbody, "set_velocity", 1);
    R.sphereCast = MT(R.physics, "SphereCast",
                      {"UnityEngine.Vector3", "System.Single", "UnityEngine.Vector3", "UnityEngine.RaycastHit&", "System.Single",
                       "System.Int32", "UnityEngine.QueryTriggerInteraction"});
    R.capsuleCast = MT(R.physics, "CapsuleCast",
                       {"UnityEngine.Vector3", "UnityEngine.Vector3", "System.Single", "UnityEngine.Vector3", "UnityEngine.RaycastHit&",
                        "System.Single", "System.Int32", "UnityEngine.QueryTriggerInteraction"});
    R.addWillRender = M(R.canvas, "add_willRenderCanvases", 1);
    R.removeWillRender = M(R.canvas, "remove_willRenderCanvases", 1);
    R.willRenderType = mono::ParamClass(R.addWillRender, 0);
    R.delegateForPointer = MT(R.marshal, "GetDelegateForFunctionPointer", {"System.IntPtr", "System.Type"});

    R.player = C(game, "", "Player");
    R.liveMixin = C(game, "", "LiveMixin");
    R.survival = C(game, "", "Survival");
    R.oxygenMgr = C(game, "", "OxygenManager");
    R.gameModeUtils = C(game, "", "GameModeUtils");
    R.ecoTarget = C(game, "", "EcoTarget");
    R.lastTarget = C(game, "", "LastTarget");
    R.playerController = C(game, "", "PlayerController");
    R.playerMotor = C(game, "", "PlayerMotor");
    R.groundMotor = C(game, "", "GroundMotor");
    R.noCost = C(game, "", "NoCostConsoleCommand");
    R.gameInput = C(game, "", "GameInput");
    R.uweUtils = C(first, "UWE", "Utils");
    R.freezeTime = C(first, "UWE", "FreezeTime");
    R.crafterLogic = C(game, "", "CrafterLogic");
    R.constructorInput = C(game, "", "ConstructorInput");
    R.vfxConstructing = C(game, "", "VFXConstructing");
    R.dayNight = C(game, "", "DayNightCycle");
    R.baseHull = C(game, "", "BaseHullStrength");
    R.leakable = C(game, "", "Leakable");
    R.floodSim = C(game, "", "BaseFloodSim");
    R.powerRelay = C(game, "", "PowerRelay");
    R.basePowerRelay = C(game, "", "BasePowerRelay");
    R.powerSource = C(game, "", "PowerSource");
    R.waterscape = C(game, "", "WaterscapeVolume");
    R.vehicle = C(game, "", "Vehicle");
    R.exosuit = C(game, "", "Exosuit");
    R.subRoot = C(game, "", "SubRoot");
    R.subControl = C(game, "", "SubControl");
    R.attackCyclops = C(game, "", "AttackCyclops");
    R.cyclopsDestruction = C(game, "", "CyclopsDestructionEvent");
    R.creature = C(game, "", "Creature");
    R.craftData = C(game, "", "CraftData");
    R.creatureData = C(game, "", "CreatureData");
    R.resourceTracker = C(game, "", "ResourceTracker");
    R.techFragment = C(game, "", "TechFragment");
    R.blueprintTarget = C(game, "", "BlueprintHandTarget");
    R.storyTarget = C(game, "", "StoryHandTarget");
    R.supplyCrate = C(game, "", "SupplyCrate");
    R.language = C(game, "", "Language");
    R.techTypeExt = C(game, "", "TechTypeExtensions");
    R.techType = C(game, "", "TechType");
    R.waitScreen = C(game, "", "WaitScreen");
    R.creatureTrait = C(game, "", "CreatureTrait");

    R.playerMain = F(R.player, "main");
    R.plLive = F(R.player, "liveMixin");
    R.plOxygen = F(R.player, "oxygenMgr");
    R.plRigidbody = F(R.player, "rigidBody");
    R.plGround = F(R.player, "groundMotor");
    R.plController = F(R.player, "<playerController>k__BackingField");
    R.plSuffTime = F(R.player, "suffocationTime");
    R.plMode = F(R.player, "mode");
    R.plMotorMode = F(R.player, "motorMode");
    R.plGetVehicle = M(R.player, "GetVehicle", 0);
    R.plCurrentSub = M(R.player, "get_currentSub", 0);
    R.plIsSuffocating = M(R.player, "IsSuffocating", 0);
    R.plSuffReset = M(R.player, "SuffocationReset", 0);
    R.plCinematic = M(R.player, "get_cinematicModeActive", 0);
    R.plUnderwater = M(R.player, "IsUnderwater", 0);
    R.pcCenter = M(R.playerController, "GetPlayerCenterPosition", 0);
    R.lmHealth = F(R.liveMixin, "health");
    R.lmInvincible = F(R.liveMixin, "invincible");
    R.lmMaxHealth = M(R.liveMixin, "get_maxHealth", 0);
    R.lmAddHealth = M(R.liveMixin, "AddHealth", 1);
    R.lmKill = M(R.liveMixin, "Kill", 1);
    R.svFood = F(R.survival, "food");
    R.svWater = F(R.survival, "water");
    R.oxAdd = M(R.oxygenMgr, "AddOxygen", 1);
    R.oxCapacity = M(R.oxygenMgr, "GetOxygenCapacity", 0);
    R.gmCheats = F(R.gameModeUtils, "currentCheats");
    R.gmActivate = M(R.gameModeUtils, "ActivateCheat", 1);
    R.gmDeactivate = M(R.gameModeUtils, "DeactivateCheat", 1);
    R.ltGetTarget = M(R.lastTarget, "get_target", 0);
    R.ltSetTarget = M(R.lastTarget, "set_target", 1);
    R.pcUnderwater = F(R.playerController, "underWaterController");
    R.pcGround = F(R.playerController, "groundController");
    R.pcActive = F(R.playerController, "activeController");
    R.pcInVehicle = F(R.playerController, "inVehicle");
    R.pcSetEnabled = M(R.playerController, "SetEnabled", 1);
    R.pcSetMotorMode = M(R.playerController, "SetMotorMode", 1);
    R.pmForward = F(R.playerMotor, "forwardMaxSpeed");
    R.pmBackward = F(R.playerMotor, "backwardMaxSpeed");
    R.pmStrafe = F(R.playerMotor, "strafeMaxSpeed");
    R.pmVertical = F(R.playerMotor, "verticalMaxSpeed");
    R.pmWaterAccel = F(R.playerMotor, "waterAcceleration");
    R.pmGroundAccel = F(R.playerMotor, "groundAcceleration");
    R.pmAirAccel = F(R.playerMotor, "airAcceleration");
    R.pmDebugSpeed = F(R.playerMotor, "debugSpeedMult");
    R.pmRb = F(R.playerMotor, "rb");
    R.pmLayerMask = F(R.playerMotor, "layerMask");
    R.gmJumping = F(R.groundMotor, "jumping");
    R.jmpBaseHeight = F(mono::FieldClass(R.gmJumping.f), "baseHeight");
    R.ncMain = F(R.noCost, "main");
    R.ncFastBuild = F(R.noCost, "<fastBuildCheat>k__BackingField");
    R.ncFastScan = F(R.noCost, "<fastScanCheat>k__BackingField");
    R.giButtonHeld = M(R.gameInput, "GetButtonHeld", 1);
    R.giClearInput = M(R.gameInput, "ClearInput", 1);
    R.giClearFrame = F(R.gameInput, "clearInputFrame");
    R.uwGetLock = M(R.uweUtils, "get_lockCursor", 0);
    R.uwSetLock = M(R.uweUtils, "set_lockCursor", 1);
    R.ftCached = F(R.freezeTime, "cachedTimeScale");
    R.ftHasFreezers = M(R.freezeTime, "HasFreezers", 0);
    R.ftUpdate = M(R.freezeTime, "UpdateTimeScale", 0);
    R.clTech = F(R.crafterLogic, "craftingTechType");
    R.clBegin = F(R.crafterLogic, "timeCraftingBegin");
    R.clEnd = F(R.crafterLogic, "timeCraftingEnd");
    R.vcConstructed = F(R.vfxConstructing, "constructed");
    R.vcTime = F(R.vfxConstructing, "timeToConstruct");
    R.vcDelay = F(R.vfxConstructing, "delay");
    R.vcDone = F(R.vfxConstructing, "isDone");
    R.dnMain = F(R.dayNight, "main");
    R.dnMinDeep = F(R.dayNight, "kMinDeepAmbientScalar");
    R.dnMinSurface = F(R.dayNight, "kMinSurfaceAmbientScalar");
    R.dnTime = M(R.dayNight, "get_timePassedAsFloat", 0);
    R.bhStrength = F(R.baseHull, "totalStrength");
    R.lkLive = F(R.leakable, "liveMixin");
    R.fsCells = F(R.floodSim, "cellWaterLevel");
    R.prList = F(R.powerRelay, "relayList");
    R.prInternal = F(R.powerRelay, "internalPowerSource");
    R.psPower = F(R.powerSource, "power");
    R.psMax = F(R.powerSource, "maxPower");
    R.prGetPower = M(R.powerRelay, "GetPower", 0);
    R.prGetMax = M(R.powerRelay, "GetMaxPower", 0);
    R.prModify = M(R.powerRelay, "ModifyPower", 2);
    R.vhLive = F(R.vehicle, "liveMixin");
    R.vhForward = F(R.vehicle, "forwardForce");
    R.vhBackward = F(R.vehicle, "backwardForce");
    R.vhSide = F(R.vehicle, "sidewardForce");
    R.vhVertical = F(R.vehicle, "verticalForce");
    R.exThrust = F(R.exosuit, "thrustPower");
    R.vhEnergy = M(R.vehicle, "GetEnergyValues", 2);
    R.vhAddEnergy = M(R.vehicle, "AddEnergy", 1);
    R.srCyclops = F(R.subRoot, "isCyclops");
    R.srLive = F(R.subRoot, "live");
    R.srRelay = F(R.subRoot, "powerRelay");
    R.srDestroyed = F(R.subRoot, "subDestroyed");
    R.scForward = F(R.subControl, "BaseForwardAccel");
    R.scPerEngine = F(R.subControl, "AccelPerEngine");
    R.scVertical = F(R.subControl, "BaseVerticalAccel");
    R.scPerBallast = F(R.subControl, "AccelPerBallast");
    R.acTarget = F(R.attackCyclops, "currentTarget");
    R.acAggress = F(R.attackCyclops, "aggressiveToNoise");
    R.acStop = M(R.attackCyclops, "StopAttack", 0);
    R.ctValue = F(R.creatureTrait, "Value");
    R.cdInteriorFx = F(R.cyclopsDestruction, "interiorFxGo");
    R.crLive = F(R.creature, "liveMixin");
    R.cdGetTechType = MT(R.craftData, "GetTechType", {"UnityEngine.GameObject"});
    R.cdBehaviour = MT(R.creatureData, "GetBehaviourType", {"TechType"});
    R.rtTech = F(R.resourceTracker, "techType");
    R.rtOverride = F(R.resourceTracker, "overrideTechType");
    R.bpUsed = F(R.blueprintTarget, "used");
    R.scOpen = F(R.supplyCrate, "open");
    R.lgMain = M(R.language, "get_main", 0);
    R.lgGet = MT(R.language, "Get", {"System.String"});
    R.ttAsString = M(R.techTypeExt, "AsString", 2);
    R.wsWaiting = M(R.waitScreen, "get_IsWaiting", 0);

    R.knownTech = C(game, "", "KnownTech");
    R.saveLoad = C(game, "", "SaveLoadManager");
    R.ktSet = F(R.knownTech, "knownTech");
    R.ktReady = F(R.knownTech, "initialized");
    R.ktAll = M(R.knownTech, "GetAllUnlockables", 1);
    R.ktNotifyAdd = M(R.knownTech, "NotifyAdd", 2);
    R.ktNotifyRemove = M(R.knownTech, "NotifyRemove", 1);
    R.ktNotifyChanged = M(R.knownTech, "NotifyChanged", 0);
    R.slMain = M(R.saveLoad, "get_main", 0);
    R.slSlot = M(R.saveLoad, "GetCurrentSlot", 0);

    R.gameModeOption = C(game, "", "GameModeOption");
    R.damageType = C(game, "", "DamageType");
    R.behaviourType = C(game, "", "BehaviourType");
    auto E = [](Class* c, const char* name) {
        int v = c ? mono::EnumValue(c, name) : -1;
        if (c && v < 0) missing.push_back(std::string(mono::ClassName(c)) + "." + name);
        return v;
    };
    R.E.noRadiation = E(R.gameModeOption, "NoRadiation");
    R.E.noOxygen = E(R.gameModeOption, "NoOxygen");
    R.E.noAggression = E(R.gameModeOption, "NoAggression");
    R.E.noBlueprints = E(R.gameModeOption, "NoBlueprints");
    R.E.noCost = E(R.gameModeOption, "NoCost");
    R.E.deconstruct = E(mono::ParamClass(R.giButtonHeld, 0), "Deconstruct");
    R.E.damageNormal = E(R.damageType, "Normal");
    R.E.modeNormal = E(mono::FieldClass(R.plMode.f), "Normal");
    R.E.leviathan = E(R.behaviourType, "Leviathan");
    R.E.shark = E(R.behaviourType, "Shark");
    R.E.qtiIgnore = E(mono::ParamClass(R.capsuleCast ? R.capsuleCast : R.sphereCast, R.capsuleCast ? 7 : 6), "Ignore");

    mono::Image* imgconv = mono::FindImage("UnityEngine.ImageConversionModule");
    R.monoBehaviour = C(core, "UnityEngine", "MonoBehaviour");
    R.invCommands = C(game, "", "InventoryConsoleCommands");
    R.utilsCls = C(game, "", "Utils");
    R.lwe = C(game, "", "LargeWorldEntity");
    R.inventory = C(game, "", "Inventory");
    R.pickupable = C(game, "", "Pickupable");
    R.itemsContainer = C(game, "", "ItemsContainer");
    R.inventoryItem = C(game, "", "InventoryItem");
    R.equipment = C(game, "", "Equipment");
    R.techData = C(game, "", "TechData");
    R.ingredient = C(game, "", "Ingredient");
    R.spriteManager = C(game, "", "SpriteManager");
    R.sprite = C(core, "UnityEngine", "Sprite");
    R.texture = C(core, "UnityEngine", "Texture");
    R.texture2d = C(core, "UnityEngine", "Texture2D");
    R.renderTexture = C(core, "UnityEngine", "RenderTexture");
    R.graphics = C(core, "UnityEngine", "Graphics");
    R.imageConversion = imgconv ? C(imgconv, "UnityEngine", "ImageConversion") : nullptr;
    R.fileCls = C(corlib, "System.IO", "File");
    R.enumCls = C(corlib, "System", "Enum");
    R.gotoCmd = C(game, "", "GotoConsoleCommand");
    R.teleportData = C(game, "", "TeleportCommandData");
    R.teleportPos = C(game, "", "TeleportPosition");
    R.beacon = C(game, "", "Beacon");
    R.equipmentType = C(game, "", "EquipmentType");
    R.itemAction = C(game, "", "ItemAction");
    R.quickSlotType = C(game, "", "QuickSlotType");
    R.harvestType = C(game, "", "HarvestType");

    R.startCoroutine = MT(R.monoBehaviour, "StartCoroutine", {"System.Collections.IEnumerator"});
    R.itemSpawn = M(R.invCommands, "ItemCmdSpawnAsync", 2);
    R.prefabAsync = MT(R.craftData, "GetPrefabForTechTypeAsync", {"TechType", "System.Boolean"});
    R.isAllowed = M(R.craftData, "IsAllowed", 1);
    R.createPrefab = M(R.utilsCls, "CreatePrefab", 3);
    R.lweRegister = M(R.lwe, "Register", 1);
    R.craftEnd = M(R.crafterLogic, "NotifyCraftEnd", 2);
    R.sendMessage = MT(R.gameObject, "SendMessage", {"System.String", "UnityEngine.SendMessageOptions"});
    R.invMain = F(R.inventory, "main");
    R.iiItem = F(R.inventoryItem, "<item>k__BackingField");
    R.ingTech = F(R.ingredient, "_techType");
    R.spReady = F(R.spriteManager, "hasInitialized");
    R.gotoMain = F(R.gotoCmd, "main");
    R.gotoData = F(R.gotoCmd, "data");
    R.gotoMoving = F(R.gotoCmd, "movingPlayer");
    R.tdLocations = F(R.teleportData, "locations");
    R.tpName = F(R.teleportPos, "name");
    R.tpPosition = F(R.teleportPos, "position");
    R.bcLabel = F(R.beacon, "label");
    R.srIsBase = F(R.subRoot, "isBase");
    R.invHeld = M(R.inventory, "GetHeld", 0);
    R.invContainer = M(R.inventory, "get_container", 0);
    R.invEquipment = M(R.inventory, "get_equipment", 0);
    R.invAction = MT(R.inventory, "ExecuteItemAction", {"ItemAction", "InventoryItem"});
    R.pkTech = M(R.pickupable, "GetTechType", 0);
    R.icGetItems = MT(R.itemsContainer, "GetItems", {"TechType"});
    R.eqFreeSlot = M(R.equipment, "GetFreeSlot", 2);
    R.tdEquip = M(R.techData, "GetEquipmentType", 1);
    R.tdSlot = M(R.techData, "GetSlotType", 1);
    R.tdBuildable = M(R.techData, "GetBuildable", 1);
    R.tdBackground = M(R.techData, "GetBackgroundType", 1);
    R.tdProcessed = M(R.techData, "GetProcessed", 1);
    R.tdIngredients = M(R.techData, "GetIngredients", 1);
    R.tdHarvest = M(R.techData, "GetHarvestType", 1);
    R.spGet = MT(R.spriteManager, "Get", {"TechType"});
    R.spDefault = M(R.spriteManager, "get_defaultSprite", 0);
    R.sprTexture = M(R.sprite, "get_texture", 0);
    R.sprUv = M(R.sprite, "get_uv", 0);
    R.texW = M(R.texture, "get_width", 0);
    R.texH = M(R.texture, "get_height", 0);
    R.rtGet = MT(R.renderTexture, "GetTemporary", {"System.Int32", "System.Int32"});
    R.rtRelease = M(R.renderTexture, "ReleaseTemporary", 1);
    R.rtActiveGet = M(R.renderTexture, "get_active", 0);
    R.rtActiveSet = M(R.renderTexture, "set_active", 1);
    R.blit = MT(R.graphics, "Blit", {"UnityEngine.Texture", "UnityEngine.RenderTexture", "UnityEngine.Vector2", "UnityEngine.Vector2"});
    R.tex2dCtor = MT(R.texture2d, ".ctor", {"System.Int32", "System.Int32"});
    R.readPixels = MT(R.texture2d, "ReadPixels", {"UnityEngine.Rect", "System.Int32", "System.Int32"});
    R.encodePng = MT(R.imageConversion, "EncodeToPNG", {"UnityEngine.Texture2D"});
    R.writeBytes = MT(R.fileCls, "WriteAllBytes", {"System.String", "System.Byte[]"});
    R.enumValues = MT(R.enumCls, "GetValues", {"System.Type"});
    R.gotoPos = MT(R.gotoCmd, "GotoPosition", {"UnityEngine.Vector3", "System.Boolean"});
    R.vhTeleport = M(R.vehicle, "TeleportVehicle", 3);
    R.trRotation = M(R.transform, "get_rotation", 0);
    R.raycast = MT(R.physics, "Raycast",
                   {"UnityEngine.Vector3", "UnityEngine.Vector3", "UnityEngine.RaycastHit&", "System.Single", "System.Int32",
                    "UnityEngine.QueryTriggerInteraction"});
    R.equipAction = E(R.itemAction, "Equip");
    R.sendDontRequire = E(mono::ParamClass(R.sendMessage, 1), "DontRequireReceiver");
    Fld bgField = F(R.techData, "defaultBackgroundType");
    R.backgroundType = bgField ? mono::FieldClass(bgField.f) : nullptr;
    R.respawnPoint = C(game, "", "RespawnPoint");
    R.rpSpawnPos = M(R.respawnPoint, "GetSpawnPosition", 0);
    R.vhDocked = M(R.vehicle, "get_docked", 0);
    R.plSetSub = MT(R.player, "SetCurrentSub", {"SubRoot", "System.Boolean"});
    R.plGetPod = M(R.player, "get_currentEscapePod", 0);
    R.plSetPod = M(R.player, "set_currentEscapePod", 1);
    R.plPodFlag = F(R.player, "escapePod");

    for (const std::string& s : missing) LOG("not found in the game: %s", s.c_str());
    if (!R.cachedPtr || !R.findObjectsOfType || !R.compTransform || !R.compGetComponent || !R.trGetPosition || !R.addWillRender ||
        !R.removeWillRender || !R.willRenderType || !R.delegateForPointer || !R.playerMain) {
        err = "the game's code is different from what Fathom expects";
        return false;
    }
    return true;
}

bool Alive(Object* o) { return o && R.cachedPtr && mono::Get<void*>(o, R.cachedPtr.off) != nullptr; }

Object* Static(const Fld& f) {
    Object* o = nullptr;
    if (f) mono::StaticGet(f.cls, f.f, &o);
    return o;
}

Object* Ref(Object* o, const Fld& f) {
    if (!o || !f) return nullptr;
    return mono::Get<Object*>(o, f.off);
}

Object* Call(Method* m, void* self, std::initializer_list<void*> args, bool* ok) {
    void* buf[12];
    size_t n = 0;
    for (void* a : args)
        if (n < 12) buf[n++] = a;
    return mono::Invoke(m, self, n ? buf : nullptr, ok);
}

bool CallBool(Method* m, void* self, std::initializer_list<void*> args) {
    Object* r = Call(m, self, args);
    return mono::Result<uint8_t>(r, 0) != 0;
}

float CallFloat(Method* m, void* self, std::initializer_list<void*> args, float fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? mono::Result<float>(r, fallback) : fallback;
}

int CallInt(Method* m, void* self, std::initializer_list<void*> args, int fallback) {
    bool ok = false;
    Object* r = Call(m, self, args, &ok);
    return ok ? mono::Result<int>(r, fallback) : fallback;
}

Object* TransformOf(Object* comp) {
    if (!Alive(comp)) return nullptr;
    Class* c = mono::ClassOf(comp);
    if (mono::IsSubclass(c, R.gameObject)) return Call(R.goTransform, comp);
    return Call(R.compTransform, comp);
}

Object* GameObjectOf(Object* comp) { return Alive(comp) ? Call(R.compGameObject, comp) : nullptr; }

Vec3 Position(Object* comp) {
    Object* t = mono::IsSubclass(mono::ClassOf(comp), R.transform) ? comp : TransformOf(comp);
    if (!Alive(t)) return {};
    return mono::Result<Vec3>(Call(R.trGetPosition, t));
}

void SetPosition(Object* comp, Vec3 p) {
    Object* t = mono::IsSubclass(mono::ClassOf(comp), R.transform) ? comp : TransformOf(comp);
    if (Alive(t)) Call(R.trSetPosition, t, {&p});
}

Object* Comp(Object* compOrGo, Class* type) {
    if (!Alive(compOrGo) || !type) return nullptr;
    Object* t = mono::TypeObject(type);
    if (!t) return nullptr;
    Object* r;
    if (mono::IsSubclass(mono::ClassOf(compOrGo), R.gameObject)) r = Call(R.goGetComponent, compOrGo, {t});
    else r = Call(R.compGetComponent, compOrGo, {t});
    return Alive(r) ? r : nullptr;
}

Object* CompInChildren(Object* comp, Class* type) {
    if (!Alive(comp) || !type) return nullptr;
    Object* t = mono::TypeObject(type);
    Object* r = t ? Call(R.compGetComponentInChildren, comp, {t}) : nullptr;
    return Alive(r) ? r : nullptr;
}

std::vector<Object*> FindAll(Class* type) {
    std::vector<Object*> out;
    if (!type) return out;
    Object* t = mono::TypeObject(type);
    Object* arr = t ? Call(R.findObjectsOfType, nullptr, {t}) : nullptr;
    size_t n = mono::Length(arr);
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        Object* o = mono::At(arr, i);
        if (Alive(o)) out.push_back(o);
    }
    return out;
}

bool Enabled(Object* behaviour) { return Alive(behaviour) && CallBool(R.getEnabled, behaviour); }

void SetEnabled(Object* behaviour, bool on) {
    if (!Alive(behaviour)) return;
    int v = on ? 1 : 0;
    Call(R.setEnabled, behaviour, {&v});
}

float DeltaTime() { return CallFloat(R.deltaTime, nullptr); }

int InstanceId(Object* o) { return Alive(o) ? CallInt(R.getInstanceID, o) : 0; }

std::string TechName(int techType) {
    auto it = names.find(techType);
    if (it != names.end()) return it->second;
    std::string out;
    int lower = 0;
    Object* key = Call(R.ttAsString, nullptr, {&techType, &lower});
    std::string k = mono::Str(key);
    Object* lang = Call(R.lgMain, nullptr);
    if (lang && key) out = mono::Str(Call(R.lgGet, lang, {key}));
    if (out.empty()) out = k;
    if (!k.empty()) names[techType] = out;
    return out;
}

int TechOf(Object* go) {
    if (!Alive(go)) return 0;
    return CallInt(R.cdGetTechType, nullptr, {go});
}

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

bool Loading() { return R.wsWaiting && CallBool(R.wsWaiting, nullptr); }

}
