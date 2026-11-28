#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace launcher::maps {

enum Game { Subnautica = 0, Subnautica2 = 1, SonsOfTheForest = 2, Borderlands3 = 3, Count = 4 };

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

struct Sheet {
    const char* file;
    const char* name;
    const char* note;
    bool dlc;
};

struct Image {
    int game = 0;
    int sheet = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::string note;
    bool partial = false;
};

struct Request {
    std::wstring folder[Count];
    bool found[Count] = {};
};

const char* Title(int game);
const Kind* Kinds(int game, int& count);
const Sheet* Sheets(int game, int& count);
const Place* Places(int game, int& count);
const Place* SheetPlaces(int game, int sheet, int& count);
const Area* Areas(int game, int& count);
bool World(int game, float& x0, float& z0, float& x1, float& z1);
const char* Axis(int game);

void Start(const Request& r);
bool Take(Image& out);
std::string Status(int game);
bool Busy();
void Stop();

}
