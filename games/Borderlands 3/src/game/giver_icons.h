#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace bl3::giver_icons {

enum Type { Pistol, Smg, Rifle, Shotgun, Sniper, Launcher, Shield, Grenade, ClassMod, Artifact, Ammo, Other, TypeCount };

struct Spec {
    std::string id;
    int type = Other;
    int tier = 0;
    std::string name;
};

struct Known {
    int tier = 0;
    std::string name;
};

std::wstring Dir();
std::unordered_map<std::string, Known> Remembered(const std::wstring& dir);
std::vector<uint8_t> Render(int type, int tier, int& size);
void Request(std::vector<Spec> specs, const std::wstring& dir);
void Pump();
void Stop();

}
