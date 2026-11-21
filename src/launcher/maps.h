#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace launcher::maps {

enum Game { Subnautica = 0, Subnautica2 = 1, SonsOfTheForest = 2, Count = 3 };

struct Kind {
    const char* name;
    const char* icon;
    uint8_t r, g, b;
};

struct Place {
    int kind;
    const char* name;
    const char* note;
    float x, y, z;
};

struct Area {
    const char* name;
    float x, z, reach;
};

struct Image {
    int game = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::string note;
    bool partial = false;
};

struct Request {
    std::wstring folder[Count];
    bool found[Count] = {false, false, false};
};

const char* Title(int game);
const Kind* Kinds(int game, int& count);
const Place* Places(int game, int& count);
const Area* Areas(int game, int& count);
bool World(int game, float& x0, float& z0, float& x1, float& z1);

void Start(const Request& r);
bool Take(Image& out);
std::string Status(int game);
bool Busy();
void Stop();

}
