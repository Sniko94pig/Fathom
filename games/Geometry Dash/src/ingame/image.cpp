#include "ingame/image.h"
#include <fstream>
#include <iterator>

#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_ONLY_TGA
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include "stb_image.h"

namespace image {

bool Load(const std::wstring& path, std::vector<uint8_t>& rgba, int& width, int& height) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(in)), {});
    if (data.empty() || data.size() > 256u * 1024u * 1024u) return false;
    int n = 0;
    unsigned char* px = stbi_load_from_memory(data.data(), (int)data.size(), &width, &height, &n, 4);
    if (!px) return false;
    rgba.assign(px, px + (size_t)width * height * 4);
    stbi_image_free(px);
    return width > 0 && height > 0;
}

}
