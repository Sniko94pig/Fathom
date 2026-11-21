#include "launcher/maps.h"
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>

namespace fs = std::filesystem;

namespace launcher::maps {

namespace {

const int kSubnauticaPicture = 201;
const int kSonsPicture = 202;
const int kSubnautica2Picture = 203;

std::thread worker;
std::atomic<bool> stopping{false};
std::atomic<bool> busy{false};
std::mutex lock;
std::deque<Image> ready;
std::string status[Count];

void SetStatus(int g, const std::string& s) {
    std::lock_guard<std::mutex> l(lock);
    status[g] = s;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

bool Decode(const void* data, size_t size, Image& img) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)data, (int)size, &w, &h, &n, 4);
    if (!px || w <= 0 || h <= 0) {
        if (px) stbi_image_free(px);
        return false;
    }
    img.width = w;
    img.height = h;
    img.rgba.assign(px, px + (size_t)w * h * 4);
    stbi_image_free(px);
    return true;
}

bool LoadBundled(int id, Image& img) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(id), MAKEINTRESOURCEW(10));
    if (!res) return false;
    HGLOBAL mem = LoadResource(self, res);
    DWORD size = SizeofResource(self, res);
    const void* data = mem ? LockResource(mem) : nullptr;
    return data && size > 64 && Decode(data, size, img);
}

void Sink(void* ctx, void* data, int size) { ((std::string*)ctx)->append((const char*)data, (size_t)size); }

bool SavePng(const fs::path& p, const std::vector<uint8_t>& rgba, int w, int h) {
    std::string png;
    if (!stbi_write_png_to_func(&Sink, &png, w, h, 4, rgba.data(), w * 4) || png.empty()) return false;
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    fs::path tmp = p;
    tmp += L".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(png.data(), (std::streamsize)png.size());
        if (!out.good()) return false;
    }
    if (!MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

void SeaColor(float h, float* c) {
    float d = -h;
    if (d <= 0) {
        float t = std::clamp(-d / 40.0f, 0.0f, 1.0f);
        c[0] = 214 + (96 - 214) * t;
        c[1] = 196 + (134 - 196) * t;
        c[2] = 140 + (84 - 140) * t;
        return;
    }
    static const float stops[][4] = {{0, 120, 214, 214}, {15, 63, 170, 200}, {60, 36, 112, 170}, {200, 22, 66, 128}, {600, 12, 36, 82}, {1500, 6, 18, 46}};
    int i = 0;
    while (i < 4 && d > stops[i + 1][0]) ++i;
    float t = std::clamp((d - stops[i][0]) / (stops[i + 1][0] - stops[i][0]), 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k) c[k] = stops[i][k + 1] + (stops[i + 1][k + 1] - stops[i][k + 1]) * t;
}

bool ComposeSn2(const fs::path& folder, Image& img) {
    std::error_code ec;
    fs::path best;
    uintmax_t bestSize = 0;
    for (auto it = fs::directory_iterator(folder, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::wstring n = it->path().filename().wstring();
        if (n.rfind(L"map_", 0) != 0 || it->path().extension() != L".bin") continue;
        uintmax_t size = it->file_size(ec);
        if (size > bestSize) {
            bestSize = size;
            best = it->path();
        }
    }
    if (best.empty()) return false;
    std::string data = ReadAll(best);
    size_t cells = data.size() / 10;
    if (cells < 16) return false;
    int x0 = INT_MAX, y0 = INT_MAX, x1 = INT_MIN, y1 = INT_MIN;
    for (size_t i = 0; i < cells; ++i) {
        int32_t x, y;
        std::memcpy(&x, data.data() + i * 10, 4);
        std::memcpy(&y, data.data() + i * 10 + 4, 4);
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    x0 -= 8;
    y0 -= 8;
    x1 += 8;
    y1 += 8;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    if (cw > 4000 || ch > 4000) return false;
    const float none = -1e9f;
    std::vector<float> g((size_t)cw * ch, none);
    for (size_t i = 0; i < cells; ++i) {
        int32_t x, y;
        int16_t hv;
        std::memcpy(&x, data.data() + i * 10, 4);
        std::memcpy(&y, data.data() + i * 10 + 4, 4);
        std::memcpy(&hv, data.data() + i * 10 + 8, 2);
        g[(size_t)(y - y0) * cw + (x - x0)] = hv / 10.0f;
    }
    for (int pass = 0; pass < 4; ++pass) {
        std::vector<float> next = g;
        for (int y = 1; y < ch - 1; ++y)
            for (int x = 1; x < cw - 1; ++x) {
                if (g[(size_t)y * cw + x] != none) continue;
                float sum = 0;
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        float v = g[(size_t)(y + dy) * cw + x + dx];
                        if (v != none) {
                            sum += v;
                            ++n;
                        }
                    }
                if (n >= 5) next[(size_t)y * cw + x] = sum / n;
            }
        g.swap(next);
    }
    auto at = [&](int cx, int cy, float fallback) {
        if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) return fallback;
        float v = g[(size_t)cy * cw + cx];
        return v == none ? fallback : v;
    };
    int ppc = std::max(cw, ch) <= 1000 ? 3 : std::max(cw, ch) <= 1500 ? 2 : 1;
    int W = cw * ppc, H = ch * ppc;
    const float bg[3] = {7, 16, 31};
    img.width = W;
    img.height = H;
    img.rgba.assign((size_t)W * H * 4, 255);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            float fx = (i + 0.5f) / ppc - 0.5f, fy = (j + 0.5f) / ppc - 0.5f;
            int cx = (int)std::floor(fx), cy = (int)std::floor(fy);
            float tx = fx - cx, ty = fy - cy;
            float v00 = at(cx, cy, none), v10 = at(cx + 1, cy, none), v01 = at(cx, cy + 1, none), v11 = at(cx + 1, cy + 1, none);
            uint8_t* p = img.rgba.data() + ((size_t)j * W + i) * 4;
            int known = (v00 != none) + (v10 != none) + (v01 != none) + (v11 != none);
            if (!known) {
                for (int k = 0; k < 3; ++k) p[k] = (uint8_t)bg[k];
                continue;
            }
            float anyv = v00 != none ? v00 : v10 != none ? v10 : v01 != none ? v01 : v11;
            if (v00 == none) v00 = anyv;
            if (v10 == none) v10 = anyv;
            if (v01 == none) v01 = anyv;
            if (v11 == none) v11 = anyv;
            float hv = (v00 * (1 - tx) + v10 * tx) * (1 - ty) + (v01 * (1 - tx) + v11 * tx) * ty;
            float c[3];
            SeaColor(hv, c);
            float hl = at(cx - 1, cy, hv), hr = at(cx + 1, cy, hv), hu = at(cx, cy - 1, hv), hd = at(cx, cy + 1, hv);
            float shade = std::clamp(1.0f + ((hl - hr) + (hu - hd)) / 32.0f * 0.7f, 0.6f, 1.3f);
            if (std::floor(hv / 50.0f) != std::floor(hr / 50.0f) || std::floor(hv / 50.0f) != std::floor(hd / 50.0f)) shade *= 0.9f;
            float edge = known / 4.0f;
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::clamp(bg[k] + (c[k] * shade - bg[k]) * edge, 0.0f, 255.0f);
        }
    img.partial = true;
    return true;
}

void Push(Image&& img) {
    std::lock_guard<std::mutex> l(lock);
    ready.push_back(std::move(img));
}

void Run(Request r) {
    for (int g = 0; g < Count && !stopping; ++g) {
        Image img;
        img.game = g;
        if (g == Subnautica) {
            if (LoadBundled(kSubnauticaPicture, img)) {
                img.note = "The whole map, drawn from Subnautica's world files.";
                std::error_code ec;
                fs::path png = fs::path(r.folder[g]) / L"map.png";
                if (r.found[g] && !fs::exists(png, ec)) SavePng(png, img.rgba, img.width, img.height);
            } else {
                img.note = "The map could not be opened.";
            }
        } else if (g == SonsOfTheForest) {
            if (LoadBundled(kSonsPicture, img)) img.note = "The whole island, drawn from the game's terrain.";
            else img.note = "The map could not be opened.";
        } else if (LoadBundled(kSubnautica2Picture, img)) {
            img.note = "The whole map of Subnautica 2.";
        } else if (!r.found[g]) {
            img.note = "Subnautica 2 is not installed on this PC.";
        } else {
            SetStatus(g, "Putting the map together");
            if (ComposeSn2(r.folder[g], img))
                img.note = "Only the places you have been to. To draw the rest, open Spawner & Map in the game menu, then Map, and press Scan whole map.";
            else
                img.note = "Nothing is drawn yet. Open Spawner & Map in the game menu, then Map, and press Scan whole map.";
        }
        SetStatus(g, "");
        Push(std::move(img));
    }
    busy = false;
}

}

void Start(const Request& r) {
    Stop();
    {
        std::lock_guard<std::mutex> l(lock);
        ready.clear();
        for (std::string& s : status) s.clear();
    }
    stopping = false;
    busy = true;
    worker = std::thread(Run, r);
}

bool Take(Image& out) {
    std::lock_guard<std::mutex> l(lock);
    if (ready.empty()) return false;
    out = std::move(ready.front());
    ready.pop_front();
    return true;
}

std::string Status(int game) {
    std::lock_guard<std::mutex> l(lock);
    return game >= 0 && game < Count ? status[game] : std::string();
}

bool Busy() { return busy.load(); }

void Stop() {
    stopping = true;
    if (worker.joinable()) worker.join();
    busy = false;
}

}
