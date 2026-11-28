#pragma once
#include <cstddef>

namespace gtav::lists {

struct Weapon {
    const char* model;
    const char* name;
    const char* category;
};

struct Ammo {
    const char* type;
    const char* name;
    int rounds;
};

struct Armor {
    const char* name;
    int amount;
};

struct Snack {
    const char* name;
    int percent;
};

extern const Weapon kWeapons[];
extern const size_t kWeaponCount;
extern const Ammo kAmmo[];
extern const size_t kAmmoCount;
extern const Armor kArmor[];
extern const size_t kArmorCount;
extern const Snack kSnacks[];
extern const size_t kSnackCount;
extern const char* const kVehicles[];
extern const size_t kVehicleCount;
extern const char* const kVehicleClasses[];
extern const size_t kVehicleClassCount;

}
