#include "ui/atlas.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

namespace fs = std::filesystem;

extern const unsigned char logo_png[];
extern const unsigned int logo_png_size;

namespace ui {

Atlas BuildAtlas(const std::wstring& folder, int cell) {
    Atlas a;
    std::vector<fs::path> files;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(folder, ec))
        if (e.path().extension() == L".png") files.push_back(e.path());
    if (files.empty()) return a;
    int perRow = (int)std::ceil(std::sqrt((double)files.size()));
    a.width = perRow * cell;
    a.height = ((int)files.size() + perRow - 1) / perRow * cell;
    a.rgba.assign((size_t)a.width * a.height * 4, 0);
    int slot = 0;
    for (const fs::path& f : files) {
        std::ifstream in(f, std::ios::binary);
        std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
        int w = 0, h = 0, n = 0;
        unsigned char* px = data.empty() ? nullptr : stbi_load_from_memory(data.data(), (int)data.size(), &w, &h, &n, 4);
        if (!px) continue;
        int cx = (slot % perRow) * cell, cy = (slot / perRow) * cell;
        for (int y = 0; y < cell; ++y)
            for (int x = 0; x < cell; ++x) {
                int sx = x * w / cell, sy = y * h / cell;
                const unsigned char* s = px + ((size_t)sy * w + sx) * 4;
                unsigned char* d = a.rgba.data() + ((size_t)(cy + y) * a.width + cx + x) * 4;
                d[0] = s[0];
                d[1] = s[1];
                d[2] = s[2];
                d[3] = s[3];
            }
        stbi_image_free(px);
        a.rects[f.stem().string()] = {(float)cx / a.width, (float)cy / a.height, (float)(cx + cell) / a.width,
                                      (float)(cy + cell) / a.height};
        ++slot;
    }
    return a;
}

bool LogoPixels(std::vector<uint8_t>& rgba, int& width, int& height) {
    int n = 0;
    unsigned char* px = stbi_load_from_memory(logo_png, (int)logo_png_size, &width, &height, &n, 4);
    if (!px) return false;
    rgba.assign(px, px + (size_t)width * height * 4);
    stbi_image_free(px);
    return true;
}

bool LoadImageFile(const std::wstring& path, std::vector<uint8_t>& rgba, int& width, int& height) {
    std::ifstream in(fs::path(path), std::ios::binary);
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    if (data.empty()) return false;
    int n = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &width, &height, &n, 4);
    if (!px) return false;
    if (width > 8192 || height > 8192) {
        stbi_image_free(px);
        return false;
    }
    rgba.assign(px, px + (size_t)width * height * 4);
    stbi_image_free(px);
    return true;
}

}