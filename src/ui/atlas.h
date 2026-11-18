#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace ui {

struct AtlasRect {
    float u0, v0, u1, v1;
};

struct Atlas {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;
    std::unordered_map<std::string, AtlasRect> rects;
};

Atlas BuildAtlas(const std::wstring& folder, int cell);
bool LogoPixels(std::vector<uint8_t>& rgba, int& width, int& height);
bool LoadImageFile(const std::wstring& path, std::vector<uint8_t>& rgba, int& width, int& height);

}
