#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace launcher::art {

enum Kind { Capsule = 0, Hero = 1 };

struct Image {
    int appId = 0;
    int kind = 0;
    int width = 0, height = 0;
    std::vector<uint8_t> rgba;
};

void Start(const std::vector<std::pair<int, int>>& wanted, const std::wstring& cacheDir, bool web = true);
bool Take(Image& out);
void Stop();

}
