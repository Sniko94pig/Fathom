#pragma once
#include "game/shv.h"
#include <cstdint>

namespace n {

struct V3 {
    float x = 0;
    float y = 0;
    float z = 0;
};

inline void ADD_AMMO_TO_PED(int ped, uint32_t weaponHash, int ammo) {
    shv::Invoke<void>(0x78F0424C34306220ull, ped, weaponHash, ammo);
}

inline void ADD_PED_AMMO_BY_TYPE(int ped, uint32_t ammoTypeHash, int ammo) {
    shv::Invoke<void>(0x2472622CE1F2D45Full, ped, ammoTypeHash, ammo);
}

inline int ADD_RELATIONSHIP_GROUP(const char* name, uint32_t* groupHash) {
    return shv::Invoke<int>(0xF372BC22FCB88606ull, name, groupHash);
}

inline void APPLY_FORCE_TO_ENTITY(int entity, int forceFlags, float x, float y, float z, float offX, float offY, float offZ, int boneIndex, int isDirectionRel, int ignoreUpVec, int isForceRel, int p12, int p13) {
    shv::Invoke<void>(0xC5F68BE9613E2D18ull, entity, forceFlags, x, y, z, offX, offY, offZ, boneIndex, isDirectionRel, ignoreUpVec, isForceRel, p12, p13);
}

inline int CAN_PED_RAGDOLL(int ped) {
    return shv::Invoke<int>(0x128F79EDCECE4FD5ull, ped);
}

inline void CLEAR_FOCUS() {
    shv::Invoke<void>(0x31B73D1EA9F01DA2ull);
}

inline void CLEAR_PED_BLOOD_DAMAGE(int ped) {
    shv::Invoke<void>(0x8FE22675A5A45817ull, ped);
}

inline void CLEAR_PED_TASKS(int ped) {
    shv::Invoke<void>(0xE1EF3C1216AFF2CDull, ped);
}

inline void CLEAR_PED_TASKS_IMMEDIATELY(int ped) {
    shv::Invoke<void>(0xAAA34F8A7CB32098ull, ped);
}

inline void CLEAR_PLAYER_WANTED_LEVEL(int player) {
    shv::Invoke<void>(0xB302540597885499ull, player);
}

inline void CLEAR_RELATIONSHIP_BETWEEN_GROUPS(int relationship, uint32_t group1, uint32_t group2) {
    shv::Invoke<void>(0x5E29243FB56FC6D4ull, relationship, group1, group2);
}

inline int CREATE_CAM_WITH_PARAMS(const char* camName, float posX, float posY, float posZ, float rotX, float rotY, float rotZ, float fov, int p8, int p9) {
    return shv::Invoke<int>(0xB51194800B257161ull, camName, posX, posY, posZ, rotX, rotY, rotZ, fov, p8, p9);
}

inline int CREATE_VEHICLE(uint32_t modelHash, float x, float y, float z, float heading, int isNetwork, int bScriptHostVeh, int p7) {
    return shv::Invoke<int>(0xAF35D0D2583051B0ull, modelHash, x, y, z, heading, isNetwork, bScriptHostVeh, p7);
}

inline void DESTROY_CAM(int cam, int bScriptHostCam) {
    shv::Invoke<void>(0x865908C81A2C22E9ull, cam, bScriptHostCam);
}

inline void DISABLE_ALL_CONTROL_ACTIONS(int control) {
    shv::Invoke<void>(0x5F4B6931816E599Bull, control);
}

inline void DISABLE_CONTROL_ACTION(int control, int action, int disableRelatedActions) {
    shv::Invoke<void>(0xFE99B66D079CF6BCull, control, action, disableRelatedActions);
}

inline int DOES_CAM_EXIST(int cam) {
    return shv::Invoke<int>(0xA7A932170592B50Eull, cam);
}

inline int DOES_ENTITY_EXIST(int entity) {
    return shv::Invoke<int>(0x7239B21A38F536BAull, entity);
}

inline int DOES_TEXT_LABEL_EXIST(const char* gxt) {
    return shv::Invoke<int>(0xAC09CA973C564252ull, gxt);
}

inline void FREEZE_ENTITY_POSITION(int entity, int toggle) {
    shv::Invoke<void>(0x428CA6DBD1094446ull, entity, toggle);
}

inline int GET_AMMO_IN_CLIP(int ped, uint32_t weaponHash, int* ammo) {
    return shv::Invoke<int>(0x2E1202248937775Cull, ped, weaponHash, ammo);
}

inline int GET_AMMO_IN_PED_WEAPON(int ped, uint32_t weaponhash) {
    return shv::Invoke<int>(0x015A522136D7F951ull, ped, weaponhash);
}

inline float GET_APPROX_HEIGHT_FOR_POINT(float x, float y) {
    return shv::Invoke<float>(0x29C24BFBED8AB8FBull, x, y);
}

inline V3 GET_BLIP_INFO_ID_COORD(int blip) {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0xFA7C7F0AADF25D09ull, blip);
    return {r.x, r.y, r.z};
}

inline float GET_DISABLED_CONTROL_NORMAL(int control, int action) {
    return shv::Invoke<float>(0x11E65974A982637Cull, control, action);
}

inline const char* GET_DISPLAY_NAME_FROM_VEHICLE_MODEL(uint32_t modelHash) {
    return shv::Invoke<const char*>(0xB215AAC32D25D019ull, modelHash);
}

inline uint32_t GET_DLC_VEHICLE_MODEL(int dlcVehicleIndex) {
    return shv::Invoke<uint32_t>(0xECC01B7C5763333Cull, dlcVehicleIndex);
}

inline int GET_DLC_WEAPON_DATA(int dlcWeaponIndex, void* outData) {
    return shv::Invoke<int>(0x79923CD21BECE14Eull, dlcWeaponIndex, outData);
}

inline int GET_ENTITY_CAN_BE_DAMAGED(int entity) {
    return shv::Invoke<int>(0xD95CC5D2AB15A09Full, entity);
}

inline int GET_ENTITY_COLLISION_DISABLED(int entity) {
    return shv::Invoke<int>(0xCCF1E97BEFDAE480ull, entity);
}

inline V3 GET_ENTITY_COORDS(int entity, int alive) {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0x3FEF770D40960D5Aull, entity, alive);
    return {r.x, r.y, r.z};
}

inline float GET_ENTITY_HEADING(int entity) {
    return shv::Invoke<float>(0xE83D4F9BA2A38914ull, entity);
}

inline int GET_ENTITY_HEALTH(int entity) {
    return shv::Invoke<int>(0xEEF059FAD016D209ull, entity);
}

inline int GET_ENTITY_MAX_HEALTH(int entity) {
    return shv::Invoke<int>(0x15D757606D170C3Cull, entity);
}

inline uint32_t GET_ENTITY_MODEL(int entity) {
    return shv::Invoke<uint32_t>(0x9F47B058362C84B5ull, entity);
}

inline int GET_ENTITY_PROOFS(int entity, int* bulletProof, int* fireProof, int* explosionProof, int* collisionProof, int* meleeProof, int* steamProof, int* p7, int* drownProof) {
    return shv::Invoke<int>(0xBE8CD9BE829BBEBFull, entity, bulletProof, fireProof, explosionProof, collisionProof, meleeProof, steamProof, p7, drownProof);
}

inline V3 GET_ENTITY_VELOCITY(int entity) {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0x4805D2B1D8CF94A9ull, entity);
    return {r.x, r.y, r.z};
}

inline const char* GET_FILENAME_FOR_AUDIO_CONVERSATION(const char* labelName) {
    return shv::Invoke<const char*>(0x7B5280EBA9840C72ull, labelName);
}

inline V3 GET_FINAL_RENDERED_CAM_COORD() {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0xA200EB1EE790F448ull);
    return {r.x, r.y, r.z};
}

inline int GET_FIRST_BLIP_INFO_ID(int blipSprite) {
    return shv::Invoke<int>(0x1BEDE233E6CD2A1Full, blipSprite);
}

inline int GET_FOLLOW_PED_CAM_VIEW_MODE() {
    return shv::Invoke<int>(0x8D4D46230B2C353Aull);
}

inline int GET_FOLLOW_VEHICLE_CAM_VIEW_MODE() {
    return shv::Invoke<int>(0xA4FF579AC0E3AAAEull);
}

inline float GET_FRAME_TIME() {
    return shv::Invoke<float>(0x15C40837039FFAF7ull);
}

inline V3 GET_GAMEPLAY_CAM_COORD() {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0x14D6F5678D8F1B37ull);
    return {r.x, r.y, r.z};
}

inline float GET_GAMEPLAY_CAM_FOV() {
    return shv::Invoke<float>(0x65019750A0324133ull);
}

inline V3 GET_GAMEPLAY_CAM_ROT(int rotationOrder) {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0x837765A25378F0BBull, rotationOrder);
    return {r.x, r.y, r.z};
}

inline int GET_GROUND_Z_FOR_3D_COORD(float x, float y, float z, float* groundZ, int ignoreWater, int p5) {
    return shv::Invoke<int>(0xC906A7DAB05C8D2Bull, x, y, z, groundZ, ignoreWater, p5);
}

inline int GET_MAX_AMMO(int ped, uint32_t weaponHash, int* ammo) {
    return shv::Invoke<int>(0xDC16122C7A20C933ull, ped, weaponHash, ammo);
}

inline int GET_MAX_AMMO_IN_CLIP(int ped, uint32_t weaponHash, int p2) {
    return shv::Invoke<int>(0xA38DCFFCEA8962FAull, ped, weaponHash, p2);
}

inline int GET_MAX_WANTED_LEVEL() {
    return shv::Invoke<int>(0x462E0DB9B137DC5Full);
}

inline int GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(uint32_t scriptHash) {
    return shv::Invoke<int>(0x2C83A9DA6BFFC4F9ull, scriptHash);
}

inline int GET_NUM_DLC_VEHICLES() {
    return shv::Invoke<int>(0xA7A866D21CD2329Bull);
}

inline int GET_NUM_DLC_WEAPONS() {
    return shv::Invoke<int>(0xEE47635F352DA367ull);
}

inline V3 GET_OFFSET_FROM_ENTITY_IN_WORLD_COORDS(int entity, float offsetX, float offsetY, float offsetZ) {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0x1899F328B0E12848ull, entity, offsetX, offsetY, offsetZ);
    return {r.x, r.y, r.z};
}

inline int GET_PED_ARMOUR(int ped) {
    return shv::Invoke<int>(0x9483AF821605B1D8ull, ped);
}

inline V3 GET_PED_BONE_COORDS(int ped, int boneId, float offsetX, float offsetY, float offsetZ) {
    shv::NVec3 r = shv::Invoke<shv::NVec3>(0x17C07FC640E86B4Eull, ped, boneId, offsetX, offsetY, offsetZ);
    return {r.x, r.y, r.z};
}

inline int GET_PED_IN_VEHICLE_SEAT(int vehicle, int seatIndex, int p2) {
    return shv::Invoke<int>(0xBB40DD2270B65366ull, vehicle, seatIndex, p2);
}

inline int GET_PED_PARACHUTE_STATE(int ped) {
    return shv::Invoke<int>(0x79CFD9827CC979B6ull, ped);
}

inline uint32_t GET_PED_RELATIONSHIP_GROUP_HASH(int ped) {
    return shv::Invoke<uint32_t>(0x7DBDD04862D95F04ull, ped);
}

inline int GET_PED_TYPE(int ped) {
    return shv::Invoke<int>(0xFF059E1E4C01E63Cull, ped);
}

inline int GET_PLAYER_INVINCIBLE(int player) {
    return shv::Invoke<int>(0xB721981B2B939E07ull, player);
}

inline int GET_PLAYER_MAX_ARMOUR(int player) {
    return shv::Invoke<int>(0x92659B4CE1863CB3ull, player);
}

inline int GET_RENDERING_CAM() {
    return shv::Invoke<int>(0x5234F9F10919EABAull);
}

inline int GET_SCREEN_COORD_FROM_WORLD_COORD(float worldX, float worldY, float worldZ, float* screenX, float* screenY) {
    return shv::Invoke<int>(0x34E82F05DF2974F5ull, worldX, worldY, worldZ, screenX, screenY);
}

inline uint32_t GET_SELECTED_PED_WEAPON(int ped) {
    return shv::Invoke<uint32_t>(0x0A6DB4965674D243ull, ped);
}

inline int GET_VEHICLE_CLASS_FROM_NAME(uint32_t modelHash) {
    return shv::Invoke<int>(0xDEDF1C8BD47C2200ull, modelHash);
}

inline int GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(int vehicle) {
    return shv::Invoke<int>(0xA7C4F2C6E744A550ull, vehicle);
}

inline int GET_VEHICLE_MODEL_NUMBER_OF_SEATS(uint32_t modelHash) {
    return shv::Invoke<int>(0x2AD93716F184EDA4ull, modelHash);
}

inline int GET_VEHICLE_PED_IS_IN(int ped, int includeEntering) {
    return shv::Invoke<int>(0x9A9112A0FE9A4713ull, ped, includeEntering);
}

inline int GET_VEHICLE_TYRES_CAN_BURST(int vehicle) {
    return shv::Invoke<int>(0x678B9BB8C3F58FEBull, vehicle);
}

inline int GET_WATER_HEIGHT(float x, float y, float z, float* height) {
    return shv::Invoke<int>(0xF6829842C06AE524ull, x, y, z, height);
}

inline uint32_t GET_WEAPONTYPE_GROUP(uint32_t weaponHash) {
    return shv::Invoke<uint32_t>(0xC3287EE3050FB74Cull, weaponHash);
}

inline int GET_WEAPON_CLIP_SIZE(uint32_t weaponHash) {
    return shv::Invoke<int>(0x583BE370B1EC6EB4ull, weaponHash);
}

inline float GET_WEAPON_DAMAGE(uint32_t weaponHash, uint32_t componentHash) {
    return shv::Invoke<float>(0x3133B907D8B32053ull, weaponHash, componentHash);
}

inline float GET_WEAPON_TIME_BETWEEN_SHOTS(uint32_t weaponHash) {
    return shv::Invoke<float>(0x065D2AACAD8CF7A4ull, weaponHash);
}

inline void GIVE_WEAPON_TO_PED(int ped, uint32_t weaponHash, int ammoCount, int isHidden, int bForceInHand) {
    shv::Invoke<void>(0xBF0FD6E56C964FCBull, ped, weaponHash, ammoCount, isHidden, bForceInHand);
}

inline int HAS_MODEL_LOADED(uint32_t model) {
    return shv::Invoke<int>(0x98A4EB5D89A0C952ull, model);
}

inline int HAS_PED_GOT_WEAPON(int ped, uint32_t weaponHash, int p2) {
    return shv::Invoke<int>(0x8DECB02F88F428BCull, ped, weaponHash, p2);
}

inline int IS_CUTSCENE_PLAYING() {
    return shv::Invoke<int>(0xD3C2E180A40F031Eull);
}

inline int IS_ENTITY_A_MISSION_ENTITY(int entity) {
    return shv::Invoke<int>(0x0A7B270912999B3Cull, entity);
}

inline int IS_ENTITY_DEAD(int entity, int p1) {
    return shv::Invoke<int>(0x5F9532F3B5CC2551ull, entity, p1);
}

inline int IS_ENTITY_IN_AIR(int entity) {
    return shv::Invoke<int>(0x886E37EC497200B6ull, entity);
}

inline int IS_ENTITY_TOUCHING_ENTITY(int entity, int targetEntity) {
    return shv::Invoke<int>(0x17FFC1B2BA35A494ull, entity, targetEntity);
}

inline int IS_ENTITY_VISIBLE(int entity) {
    return shv::Invoke<int>(0x47D6F43D77935C75ull, entity);
}

inline int IS_GAMEPLAY_CAM_SHAKING() {
    return shv::Invoke<int>(0x016C090630DF1F89ull);
}

inline int IS_MODEL_A_VEHICLE(uint32_t model) {
    return shv::Invoke<int>(0x19AAC8F07BFEC53Eull, model);
}

inline int IS_MODEL_IN_CDIMAGE(uint32_t model) {
    return shv::Invoke<int>(0x35B9E0803292B641ull, model);
}

inline int IS_PED_A_PLAYER(int ped) {
    return shv::Invoke<int>(0x12534C348C6CB68Bull, ped);
}

inline int IS_PED_DEAD_OR_DYING(int ped, int checkMeleeDeathFlags) {
    return shv::Invoke<int>(0x3317DEDB88C95038ull, ped, checkMeleeDeathFlags);
}

inline int IS_PED_HUMAN(int ped) {
    return shv::Invoke<int>(0xB980061DA992779Dull, ped);
}

inline int IS_PED_IN_ANY_VEHICLE(int ped, int atGetIn) {
    return shv::Invoke<int>(0x997ABD671D25CA0Bull, ped, atGetIn);
}

inline int IS_PED_IN_COMBAT(int ped, int target) {
    return shv::Invoke<int>(0x4859F1FC66A6278Eull, ped, target);
}

inline int IS_PED_IN_PARACHUTE_FREE_FALL(int ped) {
    return shv::Invoke<int>(0x7DCE8BDA0F1C1200ull, ped);
}

inline int IS_PED_RAGDOLL(int ped) {
    return shv::Invoke<int>(0x47E4E977581C5B55ull, ped);
}

inline int IS_PED_RELOADING(int ped) {
    return shv::Invoke<int>(0x24B100C68C645951ull, ped);
}

inline int IS_PED_SHOOTING(int ped) {
    return shv::Invoke<int>(0x34616828CD07F1A1ull, ped);
}

inline int IS_PLAYER_DEAD(int player) {
    return shv::Invoke<int>(0x424D4687FA1E5652ull, player);
}

inline int IS_PLAYER_FREE_AIMING(int player) {
    return shv::Invoke<int>(0x2E397FD2ECD37C87ull, player);
}

inline int IS_PLAYER_SWITCH_IN_PROGRESS() {
    return shv::Invoke<int>(0xD9D2CFFF49FAB35Full);
}

inline int IS_SCREEN_FADED_OUT() {
    return shv::Invoke<int>(0xB16FCE9DDC7BA182ull);
}

inline int IS_THIS_MODEL_A_HELI(uint32_t model) {
    return shv::Invoke<int>(0xDCE4334788AF94EAull, model);
}

inline int IS_THIS_MODEL_A_PLANE(uint32_t model) {
    return shv::Invoke<int>(0xA0948AB42D7BA0DEull, model);
}

inline int IS_THIS_MODEL_A_TRAIN(uint32_t model) {
    return shv::Invoke<int>(0xAB935175B22E822Bull, model);
}

inline int IS_VEHICLE_DRIVEABLE(int vehicle, int isOnFireCheck) {
    return shv::Invoke<int>(0x4C241E39B23DF959ull, vehicle, isOnFireCheck);
}

inline int IS_WAYPOINT_ACTIVE() {
    return shv::Invoke<int>(0x1DD1F58F493F1DA5ull);
}

inline int IS_WEAPON_VALID(uint32_t weaponHash) {
    return shv::Invoke<int>(0x937C71165CF334B3ull, weaponHash);
}

inline int NETWORK_IS_GAME_IN_PROGRESS() {
    return shv::Invoke<int>(0x10FAB35428CCC9D7ull);
}

inline int NETWORK_IS_IN_SESSION() {
    return shv::Invoke<int>(0xCA97246103B63917ull);
}

inline int NETWORK_IS_SESSION_STARTED() {
    return shv::Invoke<int>(0x9DE624D2FC4B603Full);
}

inline void PAUSE_CLOCK(int toggle) {
    shv::Invoke<void>(0x4055E40BD2DBEC1Dull, toggle);
}

inline int PLAYER_ID() {
    return shv::Invoke<int>(0x4F8644AF03D0E0D6ull);
}

inline int PLAYER_PED_ID() {
    return shv::Invoke<int>(0xD80958FC74E988A6ull);
}

inline void REMOVE_RELATIONSHIP_GROUP(uint32_t groupHash) {
    shv::Invoke<void>(0xB6BA2444AB393DA2ull, groupHash);
}

inline void REMOVE_WEAPON_FROM_PED(int ped, uint32_t weaponHash) {
    shv::Invoke<void>(0x4899CB088EDF59B8ull, ped, weaponHash);
}

inline void RENDER_SCRIPT_CAMS(int render, int ease, int easeTime, int p3, int p4, int p5) {
    shv::Invoke<void>(0x07E5B515DB0636FCull, render, ease, easeTime, p3, p4, p5);
}

inline void REQUEST_COLLISION_AT_COORD(float x, float y, float z) {
    shv::Invoke<void>(0x07503F7948F491A7ull, x, y, z);
}

inline void REQUEST_MODEL(uint32_t model) {
    shv::Invoke<void>(0x963D27A58DF860ACull, model);
}

inline void RESET_PED_VISIBLE_DAMAGE(int ped) {
    shv::Invoke<void>(0x3AC1F7B898F30C05ull, ped);
}

inline void RESTORE_PLAYER_STAMINA(int player, float p1) {
    shv::Invoke<void>(0xA352C1B864CAFD33ull, player, p1);
}

inline int SET_AMMO_IN_CLIP(int ped, uint32_t weaponHash, int ammo) {
    return shv::Invoke<int>(0xDCD2A934D65CB497ull, ped, weaponHash, ammo);
}

inline void SET_BLOCKING_OF_NON_TEMPORARY_EVENTS(int ped, int toggle) {
    shv::Invoke<void>(0x9F8AA94D6D97DBF4ull, ped, toggle);
}

inline void SET_CAM_COORD(int cam, float posX, float posY, float posZ) {
    shv::Invoke<void>(0x4D41783FB745E42Eull, cam, posX, posY, posZ);
}

inline void SET_CAM_FOV(int cam, float fieldOfView) {
    shv::Invoke<void>(0xB13C14F66A00D047ull, cam, fieldOfView);
}

inline void SET_CAM_ROT(int cam, float rotX, float rotY, float rotZ, int rotationOrder) {
    shv::Invoke<void>(0x85973643155D0B07ull, cam, rotX, rotY, rotZ, rotationOrder);
}

inline void SET_CURRENT_PED_WEAPON(int ped, uint32_t weaponHash, int bForceInHand) {
    shv::Invoke<void>(0xADF692B254977C0Cull, ped, weaponHash, bForceInHand);
}

inline void SET_DISABLE_VEHICLE_PETROL_TANK_DAMAGE(int vehicle, int toggle) {
    shv::Invoke<void>(0x37C8252A7C92D017ull, vehicle, toggle);
}

inline void SET_DISABLE_VEHICLE_PETROL_TANK_FIRES(int vehicle, int toggle) {
    shv::Invoke<void>(0x465BF26AB9684352ull, vehicle, toggle);
}

inline void SET_DISPATCH_COPS_FOR_PLAYER(int player, int toggle) {
    shv::Invoke<void>(0xDB172424876553F4ull, player, toggle);
}

inline void SET_ENTITY_AS_MISSION_ENTITY(int entity, int bScriptHostObject, int bGrabFromOtherScript) {
    shv::Invoke<void>(0xAD738C3085FE7E11ull, entity, bScriptHostObject, bGrabFromOtherScript);
}

inline void SET_ENTITY_CAN_BE_DAMAGED(int entity, int toggle) {
    shv::Invoke<void>(0x1760FFA8AB074D66ull, entity, toggle);
}

inline void SET_ENTITY_COLLISION(int entity, int toggle, int keepPhysics) {
    shv::Invoke<void>(0x1A9205C1B9EE827Full, entity, toggle, keepPhysics);
}

inline void SET_ENTITY_COORDS_NO_OFFSET(int entity, float xPos, float yPos, float zPos, int xAxis, int yAxis, int zAxis) {
    shv::Invoke<void>(0x239A3351AC1DA385ull, entity, xPos, yPos, zPos, xAxis, yAxis, zAxis);
}

inline void SET_ENTITY_HEADING(int entity, float heading) {
    shv::Invoke<void>(0x8E2530AA8ADA980Eull, entity, heading);
}

inline void SET_ENTITY_HEALTH(int entity, int health, int instigator, uint32_t weaponType) {
    shv::Invoke<void>(0x6B76DC1F3AE6E6A3ull, entity, health, instigator, weaponType);
}

inline void SET_ENTITY_INVINCIBLE(int entity, int toggle, int dontResetOnCleanup) {
    shv::Invoke<void>(0x3882114BDE571AD4ull, entity, toggle, dontResetOnCleanup);
}

inline void SET_ENTITY_PROOFS(int entity, int bulletProof, int fireProof, int explosionProof, int collisionProof, int meleeProof, int steamProof, int dontResetOnCleanup, int waterProof) {
    shv::Invoke<void>(0xFAEE099C6F890BB8ull, entity, bulletProof, fireProof, explosionProof, collisionProof, meleeProof, steamProof, dontResetOnCleanup, waterProof);
}

inline void SET_ENTITY_VELOCITY(int entity, float x, float y, float z) {
    shv::Invoke<void>(0x1C99BB7B6E96D16Full, entity, x, y, z);
}

inline void SET_ENTITY_VISIBLE(int entity, int toggle, int p2) {
    shv::Invoke<void>(0xEA1C610A04DB6BBBull, entity, toggle, p2);
}

inline void SET_FOCUS_POS_AND_VEL(float x, float y, float z, float offsetX, float offsetY, float offsetZ) {
    shv::Invoke<void>(0xBB7454BAFF08FE25ull, x, y, z, offsetX, offsetY, offsetZ);
}

inline void SET_GAMEPLAY_CAM_SHAKE_AMPLITUDE(float amplitude) {
    shv::Invoke<void>(0xA87E00932DB4D85Dull, amplitude);
}

inline void SET_HELI_BLADES_FULL_SPEED(int vehicle) {
    shv::Invoke<void>(0xA178472EBB8AE60Dull, vehicle);
}

inline void SET_MODEL_AS_NO_LONGER_NEEDED(uint32_t model) {
    shv::Invoke<void>(0xE532F5D78798DAABull, model);
}

inline void SET_PED_AMMO(int ped, uint32_t weaponHash, int ammo, int p3) {
    shv::Invoke<void>(0x14E56BC5B5DB6A19ull, ped, weaponHash, ammo, p3);
}

inline void SET_PED_ARMOUR(int ped, int amount) {
    shv::Invoke<void>(0xCEA04D83135264CCull, ped, amount);
}

inline void SET_PED_AS_NO_LONGER_NEEDED(int* ped) {
    shv::Invoke<void>(0x2595DD4236549CE3ull, ped);
}

inline void SET_PED_CAN_RAGDOLL(int ped, int toggle) {
    shv::Invoke<void>(0xB128377056A54E2Aull, ped, toggle);
}

inline void SET_PED_COMBAT_ATTRIBUTES(int ped, int attributeId, int enabled) {
    shv::Invoke<void>(0x9F7794730795E019ull, ped, attributeId, enabled);
}

inline void SET_PED_FLEE_ATTRIBUTES(int ped, int attributeFlags, int enable) {
    shv::Invoke<void>(0x70A2D1137C8ED7C9ull, ped, attributeFlags, enable);
}

inline void SET_PED_GRAVITY(int ped, int toggle) {
    shv::Invoke<void>(0x9FF447B6B6AD960Aull, ped, toggle);
}

inline void SET_PED_INFINITE_AMMO(int ped, int toggle, uint32_t weaponHash) {
    shv::Invoke<void>(0x3EDCB0505123623Bull, ped, toggle, weaponHash);
}

inline void SET_PED_INTO_VEHICLE(int ped, int vehicle, int seatIndex) {
    shv::Invoke<void>(0xF75B0D629E1C063Dull, ped, vehicle, seatIndex);
}

inline void SET_PED_KEEP_TASK(int ped, int toggle) {
    shv::Invoke<void>(0x971D38760FBC02EFull, ped, toggle);
}

inline void SET_PED_RELATIONSHIP_GROUP_HASH(int ped, uint32_t hash) {
    shv::Invoke<void>(0xC80A74AC829DDD92ull, ped, hash);
}

inline void SET_PED_SUFFERS_CRITICAL_HITS(int ped, int toggle) {
    shv::Invoke<void>(0xEBD76F2359F190ACull, ped, toggle);
}

inline int SET_PED_TO_RAGDOLL(int ped, int time1, int time2, int ragdollType, int p4, int p5, int p6) {
    return shv::Invoke<int>(0xAE99FB955581844Aull, ped, time1, time2, ragdollType, p4, p5, p6);
}

inline void SET_PLAYER_INVINCIBLE(int player, int toggle) {
    shv::Invoke<void>(0x239528EACDC3E7DEull, player, toggle);
}

inline void SET_PLAYER_WANTED_LEVEL(int player, int wantedLevel, int disableNoMission) {
    shv::Invoke<void>(0x39FF19C64EF7DA5Bull, player, wantedLevel, disableNoMission);
}

inline void SET_PLAYER_WANTED_LEVEL_NOW(int player, int p1) {
    shv::Invoke<void>(0xE0A7D1E497FFCD6Full, player, p1);
}

inline void SET_POLICE_IGNORE_PLAYER(int player, int toggle) {
    shv::Invoke<void>(0x32C62AA929C2DA6Aull, player, toggle);
}

inline void SET_RELATIONSHIP_BETWEEN_GROUPS(int relationship, uint32_t group1, uint32_t group2) {
    shv::Invoke<void>(0xBF25EB89375A37ADull, relationship, group1, group2);
}

inline void SET_TIME_SCALE(float timeScale) {
    shv::Invoke<void>(0x1D408577D440E81Eull, timeScale);
}

inline void SET_VEHICLE_AS_NO_LONGER_NEEDED(int* vehicle) {
    shv::Invoke<void>(0x629BFA74418D6239ull, vehicle);
}

inline void SET_VEHICLE_BODY_HEALTH(int vehicle, float value) {
    shv::Invoke<void>(0xB77D05AC8C78AADBull, vehicle, value);
}

inline void SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(int vehicle, int state) {
    shv::Invoke<void>(0x4C7028F78FFD3681ull, vehicle, state);
}

inline void SET_VEHICLE_CAN_BREAK(int vehicle, int toggle) {
    shv::Invoke<void>(0x59BF8C3D52C92F66ull, vehicle, toggle);
}

inline void SET_VEHICLE_DEFORMATION_FIXED(int vehicle) {
    shv::Invoke<void>(0x953DA1E1B12C0491ull, vehicle);
}

inline void SET_VEHICLE_DIRT_LEVEL(int vehicle, float dirtLevel) {
    shv::Invoke<void>(0x79D3B596FE44EE8Bull, vehicle, dirtLevel);
}

inline void SET_VEHICLE_DOORS_LOCKED(int vehicle, int doorLockStatus) {
    shv::Invoke<void>(0xB664292EAECF7FA6ull, vehicle, doorLockStatus);
}

inline void SET_VEHICLE_ENGINE_CAN_DEGRADE(int vehicle, int toggle) {
    shv::Invoke<void>(0x983765856F2564F9ull, vehicle, toggle);
}

inline void SET_VEHICLE_ENGINE_HEALTH(int vehicle, float health) {
    shv::Invoke<void>(0x45F6D8EEF34ABEF1ull, vehicle, health);
}

inline void SET_VEHICLE_ENGINE_ON(int vehicle, int value, int instantly, int disableAutoStart) {
    shv::Invoke<void>(0x2497C4717C8B881Eull, vehicle, value, instantly, disableAutoStart);
}

inline void SET_VEHICLE_EXPLODES_ON_HIGH_EXPLOSION_DAMAGE(int vehicle, int toggle) {
    shv::Invoke<void>(0x71B0892EC081D60Aull, vehicle, toggle);
}

inline void SET_VEHICLE_FIXED(int vehicle) {
    shv::Invoke<void>(0x115722B1B9C14C1Cull, vehicle);
}

inline void SET_VEHICLE_FORWARD_SPEED(int vehicle, float speed) {
    shv::Invoke<void>(0xAB54A438726D25D5ull, vehicle, speed);
}

inline void SET_VEHICLE_GRAVITY(int vehicle, int toggle) {
    shv::Invoke<void>(0x89F149B6131E57DAull, vehicle, toggle);
}

inline void SET_VEHICLE_HAS_BEEN_OWNED_BY_PLAYER(int vehicle, int owned) {
    shv::Invoke<void>(0x2B5F9D2AF1F1722Dull, vehicle, owned);
}

inline void SET_VEHICLE_NEEDS_TO_BE_HOTWIRED(int vehicle, int toggle) {
    shv::Invoke<void>(0xFBA550EA44404EE6ull, vehicle, toggle);
}

inline int SET_VEHICLE_ON_GROUND_PROPERLY(int vehicle, float p1) {
    return shv::Invoke<int>(0x49733E92263139D1ull, vehicle, p1);
}

inline void SET_VEHICLE_PETROL_TANK_HEALTH(int vehicle, float health) {
    shv::Invoke<void>(0x70DB57649FA8D0D8ull, vehicle, health);
}

inline void SET_VEHICLE_TYRES_CAN_BURST(int vehicle, int toggle) {
    shv::Invoke<void>(0xEB9DC3C7D8596C46ull, vehicle, toggle);
}

inline void SET_VEHICLE_TYRE_FIXED(int vehicle, int tyreIndex) {
    shv::Invoke<void>(0x6E13FC662B882D1Dull, vehicle, tyreIndex);
}

inline void SET_VEHICLE_UNDRIVEABLE(int vehicle, int toggle) {
    shv::Invoke<void>(0x8ABA6AF54B942B95ull, vehicle, toggle);
}

inline void SET_VEHICLE_WHEELS_CAN_BREAK(int vehicle, int enabled) {
    shv::Invoke<void>(0x29B18B4FD460CA8Full, vehicle, enabled);
}

inline void SHOOT_SINGLE_BULLET_BETWEEN_COORDS_IGNORE_ENTITY(float x1, float y1, float z1, float x2, float y2, float z2, int damage, int p7, uint32_t weaponHash, int ownerPed, int isAudible, int isInvisible, float speed, int entity, int p14) {
    shv::Invoke<void>(0xE3A7742E0B7A2F8Bull, x1, y1, z1, x2, y2, z2, damage, p7, weaponHash, ownerPed, isAudible, isInvisible, speed, entity, p14);
}

inline int STAT_GET_INT(uint32_t statHash, int* outValue, int p2) {
    return shv::Invoke<int>(0x767FBC2AC802EF3Dull, statHash, outValue, p2);
}

inline int STAT_SET_INT(uint32_t statName, int value, int save) {
    return shv::Invoke<int>(0xB3271D7AB655B441ull, statName, value, save);
}

inline void STOP_ENTITY_FIRE(int entity) {
    shv::Invoke<void>(0x7F0DD2EBBB651AFFull, entity);
}

inline void STOP_GAMEPLAY_CAM_SHAKING(int p0) {
    shv::Invoke<void>(0x0EF93E9F3D08C178ull, p0);
}

inline void TASK_COMBAT_HATED_TARGETS_AROUND_PED(int ped, float radius, int combatFlags) {
    shv::Invoke<void>(0x7BF835BB9E2698C8ull, ped, radius, combatFlags);
}

inline void TASK_SKY_DIVE(int ped, int instant) {
    shv::Invoke<void>(0x601736CFE536B0A0ull, ped, instant);
}

inline void TASK_STAND_STILL(int ped, int time) {
    shv::Invoke<void>(0x919BE13EED931959ull, ped, time);
}

inline void TASK_WANDER_STANDARD(int ped, float heading, int flags) {
    shv::Invoke<void>(0xBB9CE077274F6A1Bull, ped, heading, flags);
}

inline void WASH_DECALS_FROM_VEHICLE(int vehicle, float p1) {
    shv::Invoke<void>(0x5B712761429DBC14ull, vehicle, p1);
}

}
