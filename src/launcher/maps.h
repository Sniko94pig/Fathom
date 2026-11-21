#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace launcher::maps {

enum Game { Subnautica = 0, Subnautica2 = 1, SonsOfTheForest = 2, Count = 3 };

struct Label {
    std::string name;
    float u = 0, v = 0;
    bool area = true;
    float reach = 0;
};

struct Image {
    int game = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
    std::vector<Label> labels;
    std::string note;
    bool partial = false;
};

struct Request {
    std::wstring root[Count];
    std::wstring folder[Count];
    bool found[Count] = {false, false, false};
    bool redraw = false;
};

void Start(const Request& r);
bool Take(Image& out);
std::string Status(int game);
bool Busy();
void Stop();
bool DrawSubnautica(const std::wstring& root, const std::wstring& png, std::string& error);

}
