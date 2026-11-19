#pragma once
#include <string>
#include <vector>

namespace esp {

struct Dot {
    float x = 0, y = 0, z = 0;
    float bx = 0, by = 0, bz = 0;
    float ex = 0, ey = 0, ez = 0;
    bool box = false;
    float health = -1;
    int kind = 0;
    std::string name;
};

struct Frame {
    float view[16] = {};
    float proj[16] = {};
    float px = 0, py = 0, pz = 0;
    bool valid = false;
    bool creatures = false, items = false;
    bool cName = true, cDist = true, cHealth = true, cBox = true;
    bool iName = true, iDist = true, iBox = true;
    std::vector<Dot> creatureDots, itemDots;
};

void Publish(Frame&& f);
void Clear();
void Draw(float scale);

}
