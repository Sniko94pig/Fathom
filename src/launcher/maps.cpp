#include "launcher/maps.h"
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <climits>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fs = std::filesystem;

namespace launcher::maps {

namespace {

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

bool LoadPng(const fs::path& p, Image& img) {
    std::string bytes = ReadAll(p);
    if (bytes.size() < 64) return false;
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
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

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string Trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::vector<std::string> Lines(const std::string& text) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        out.push_back(Trim(text.substr(start, end - start)));
        start = end + 1;
    }
    return out;
}

std::string Pretty(const std::string& raw) {
    std::string out;
    for (size_t i = 0; i < raw.size(); ++i) {
        char c = raw[i];
        if (c == '_' || c == '-') {
            out += ' ';
            continue;
        }
        if (i > 0 && std::isupper((unsigned char)c) && std::islower((unsigned char)raw[i - 1])) out += ' ';
        out += c;
    }
    if (!out.empty()) out[0] = (char)std::toupper((unsigned char)out[0]);
    return out;
}

struct Biome {
    const char* key;
    const char* name;
    uint8_t r, g, b;
};

const Biome kBiomes[] = {
    {"safeshallows", "Safe Shallows", 120, 206, 194},  {"kelpforest", "Kelp Forest", 62, 142, 72},
    {"crashzone", "Crash Zone", 214, 172, 104},         {"grassyplateaus", "Grassy Plateaus", 178, 92, 78},
    {"mushroomforest", "Mushroom Forest", 140, 96, 182}, {"grandreef", "Grand Reef", 52, 98, 166},
    {"kooshzone", "Bulb Zone", 104, 126, 208},          {"dunes", "Dunes", 214, 190, 136},
    {"mountains", "Mountains", 140, 128, 112},          {"underwaterislands", "Underwater Islands", 74, 160, 164},
    {"bloodkelp", "Blood Kelp Zone", 120, 38, 56},      {"sparsereef", "Sparse Reef", 68, 130, 148},
    {"seatreaderpath", "Sea Treader's Path", 184, 156, 112}, {"bloodkelptwo", "Blood Kelp Trench", 100, 30, 48},
    {"cragfield", "Crag Field", 126, 94, 66},
};

const Biome* FindBiome(const std::string& name) {
    std::string k = Lower(name);
    for (const Biome& b : kBiomes)
        if (k == b.key) return &b;
    return nullptr;
}

struct Sn1 {
    fs::path data;
    std::vector<uint8_t> biome;
    std::vector<const Biome*> table;
};

bool LoadSn1(const fs::path& root, Sn1& s) {
    s.data = root / L"Subnautica_Data" / L"StreamingAssets" / L"SNUnmanagedData" / L"Build18";
    std::string map = ReadAll(s.data / L"biomeMap.bin");
    if (map.size() < 2 + 1024 * 1024) return false;
    s.biome.assign(map.begin() + 2, map.begin() + 2 + 1024 * 1024);
    std::vector<std::string> names = Lines(ReadAll(s.data / L"biomes.csv"));
    s.table.assign(256, nullptr);
    for (size_t i = 1; i < names.size() && i <= 256; ++i) s.table[i - 1] = FindBiome(names[i]);
    return true;
}

void Sn1Labels(const Sn1& s, std::vector<Label>& out) {
    const int N = 1024;
    const std::vector<uint8_t>& b = s.biome;
    std::vector<float> dist((size_t)N * N);
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            uint8_t id = b[(size_t)j * N + i];
            bool edge = i == 0 || j == 0 || i == N - 1 || j == N - 1 || b[(size_t)j * N + i - 1] != id || b[(size_t)j * N + i + 1] != id ||
                        b[(size_t)(j - 1) * N + i] != id || b[(size_t)(j + 1) * N + i] != id;
            dist[(size_t)j * N + i] = edge ? 0.0f : 1e9f;
        }
    const float diag = 1.41421356f;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            float& d = dist[(size_t)j * N + i];
            if (i > 0) d = std::min(d, dist[(size_t)j * N + i - 1] + 1.0f);
            if (j > 0) {
                d = std::min(d, dist[(size_t)(j - 1) * N + i] + 1.0f);
                if (i > 0) d = std::min(d, dist[(size_t)(j - 1) * N + i - 1] + diag);
                if (i < N - 1) d = std::min(d, dist[(size_t)(j - 1) * N + i + 1] + diag);
            }
        }
    for (int j = N - 1; j >= 0; --j)
        for (int i = N - 1; i >= 0; --i) {
            float& d = dist[(size_t)j * N + i];
            if (i < N - 1) d = std::min(d, dist[(size_t)j * N + i + 1] + 1.0f);
            if (j < N - 1) {
                d = std::min(d, dist[(size_t)(j + 1) * N + i] + 1.0f);
                if (i < N - 1) d = std::min(d, dist[(size_t)(j + 1) * N + i + 1] + diag);
                if (i > 0) d = std::min(d, dist[(size_t)(j + 1) * N + i - 1] + diag);
            }
        }
    struct Area {
        int round, rank;
        float best;
        Label label;
    };
    std::vector<Area> areas;
    for (int id = 0; id < 256; ++id) {
        const Biome* bm = s.table[id];
        if (!bm) continue;
        std::vector<std::pair<int, int>> picked;
        for (int round = 0; round < 2; ++round) {
            float best = 0;
            int bi = -1, bj = -1;
            for (int j = 0; j < N; ++j)
                for (int i = 0; i < N; ++i) {
                    if (b[(size_t)j * N + i] != id) continue;
                    float d = dist[(size_t)j * N + i];
                    if (d <= best) continue;
                    bool taken = false;
                    for (const auto& p : picked)
                        if ((p.first - i) * (p.first - i) + (p.second - j) * (p.second - j) < 140 * 140) taken = true;
                    if (taken) continue;
                    best = d;
                    bi = i;
                    bj = j;
                }
            if (bi < 0 || best < (round == 0 ? 8.0f : 28.0f)) break;
            picked.push_back({bi, bj});
            areas.push_back({round, (int)(bm - kBiomes), best, Label{bm->name, (bi + 0.5f) / N, 1.0f - (bj + 0.5f) / N, true, best / N}});
        }
    }
    std::stable_sort(areas.begin(), areas.end(), [](const Area& l, const Area& r) {
        if (l.round != r.round) return l.round < r.round;
        if (l.round == 0 && l.rank != r.rank) return l.rank < r.rank;
        return l.best > r.best;
    });
    for (const Area& a : areas) out.push_back(a.label);
    std::vector<std::string> rows = Lines(ReadAll(s.data / L"signals.csv"));
    for (size_t r = 1; r < rows.size(); ++r) {
        std::vector<std::string> f;
        size_t start = 0;
        while (start <= rows[r].size()) {
            size_t end = rows[r].find(',', start);
            if (end == std::string::npos) end = rows[r].size();
            f.push_back(Trim(rows[r].substr(start, end - start)));
            start = end + 1;
        }
        if (f.size() < 4 || f[3].empty()) continue;
        float x = 0, y = 0, z = 0;
        if (sscanf_s(f[2].c_str(), "%f %f %f", &x, &y, &z) != 3) continue;
        float u = (x + 2048.0f) / 4096.0f, v = (2048.0f - z) / 4096.0f;
        bool same = false;
        for (const Label& l : out)
            if (!l.area && std::fabs(l.u - u) < 0.004f && std::fabs(l.v - v) < 0.004f) same = true;
        if (!same) out.push_back({Pretty(f[3]), u, v, false});
    }
}

struct Node {
    int index, x, y, z, size;
};

bool Sn1Heights(const Sn1& s, std::vector<int16_t>& top) {
    const int N = 4096;
    const int16_t none = INT16_MIN;
    top.assign((size_t)N * N, none);
    fs::path dir = s.data / L"CompiledOctreesCache";
    std::vector<Node> stack;
    stack.reserve(64);
    int done = 0, files = 0;
    for (int bx = 0; bx < 26; ++bx)
        for (int bz = 0; bz < 26; ++bz) {
            if (stopping) return false;
            int x0 = bx * 160, z0 = bz * 160, x1 = std::min(N, x0 + 160), z1 = std::min(N, z0 + 160);
            for (int by = 19; by >= 0; --by) {
                wchar_t name[96];
                swprintf_s(name, L"compiled-batch-%d-%d-%d.optoctrees", bx, by, bz);
                std::string file = ReadAll(dir / name);
                if (file.size() < 4) continue;
                ++files;
                const uint8_t* b = (const uint8_t*)file.data();
                size_t n = file.size(), pos = 4;
                for (int t = 0; t < 125; ++t) {
                    if (pos + 2 > n) break;
                    int count = b[pos] | (b[pos + 1] << 8);
                    pos += 2;
                    if (count == 0 || pos + (size_t)count * 4 > n) break;
                    const uint8_t* nodes = b + pos;
                    pos += (size_t)count * 4;
                    int ox = t / 25, oy = (t / 5) % 5, oz = t % 5;
                    stack.clear();
                    stack.push_back({0, x0 + ox * 32, by * 160 + oy * 32 - 3040, z0 + oz * 32, 32});
                    while (!stack.empty()) {
                        Node it = stack.back();
                        stack.pop_back();
                        const uint8_t* nd = nodes + (size_t)it.index * 4;
                        int child = nd[2] | (nd[3] << 8);
                        if (child == 0 || it.size <= 1 || child + 7 >= count) {
                            if (nd[0] == 0) continue;
                            int16_t ty = (int16_t)(it.y + it.size);
                            for (int zz = it.z; zz < it.z + it.size && zz < N; ++zz) {
                                int16_t* row = top.data() + (size_t)zz * N;
                                for (int xx = it.x; xx < it.x + it.size && xx < N; ++xx)
                                    if (row[xx] < ty) row[xx] = ty;
                            }
                            continue;
                        }
                        int h = it.size / 2;
                        for (int c = 0; c < 8; ++c)
                            stack.push_back({child + c, it.x + ((c >> 2) & 1) * h, it.y + ((c >> 1) & 1) * h, it.z + (c & 1) * h, h});
                    }
                }
                bool full = true;
                for (int zz = z0; zz < z1 && full; ++zz)
                    for (int xx = x0; xx < x1; ++xx)
                        if (top[(size_t)zz * N + xx] == none) {
                            full = false;
                            break;
                        }
                if (full) break;
            }
            ++done;
            if (done % 13 == 0) SetStatus(Subnautica, "Drawing the map from your game files, " + std::to_string(done * 100 / 676) + "%");
        }
    return files > 0;
}

void Mix(float* c, float r, float g, float b, float t) {
    c[0] += (r - c[0]) * t;
    c[1] += (g - c[1]) * t;
    c[2] += (b - c[2]) * t;
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

}

bool DrawSubnautica(const std::wstring& root, const std::wstring& png, std::string& error) {
    Sn1 s;
    if (!LoadSn1(root, s)) {
        error = "Subnautica's world files were not found";
        return false;
    }
    std::vector<int16_t> top;
    if (!Sn1Heights(s, top)) {
        if (!stopping) error = "Subnautica's terrain files could not be read";
        return false;
    }
    const int N = 4096, M = 2048;
    const float none = -1e9f;
    std::vector<float> h((size_t)M * M, none);
    for (int py = 0; py < M; ++py)
        for (int px = 0; px < M; ++px) {
            float best = none;
            for (int dz = 0; dz < 2; ++dz)
                for (int dx = 0; dx < 2; ++dx) {
                    int zi = N - 1 - (py * 2 + dz), xi = px * 2 + dx;
                    int16_t v = top[(size_t)zi * N + xi];
                    if (v != INT16_MIN) best = std::max(best, (float)v);
                }
            h[(size_t)py * M + px] = best;
        }
    std::vector<uint8_t> rgba((size_t)M * M * 4);
    for (int py = 0; py < M; ++py)
        for (int px = 0; px < M; ++px) {
            float hv = h[(size_t)py * M + px];
            uint8_t* o = rgba.data() + ((size_t)py * M + px) * 4;
            o[3] = 255;
            if (hv == none) {
                o[0] = 7;
                o[1] = 16;
                o[2] = 31;
                continue;
            }
            int zi = N - 1 - py * 2, xi = px * 2;
            const Biome* bm = s.table[s.biome[(size_t)(zi / 4) * 1024 + xi / 4]];
            float c[3];
            if (hv > 0) {
                SeaColor(hv, c);
            } else {
                float d = -hv;
                if (bm) {
                    c[0] = bm->r;
                    c[1] = bm->g;
                    c[2] = bm->b;
                } else {
                    c[0] = 30;
                    c[1] = 46;
                    c[2] = 66;
                }
                Mix(c, 6, 16, 36, std::clamp((d - 20.0f) / 900.0f, 0.0f, 0.78f));
                if (d < 12) Mix(c, 170, 230, 225, (12 - d) / 12.0f * 0.35f);
            }
            auto at = [&](int x, int y) {
                if (x < 0 || y < 0 || x >= M || y >= M) return hv;
                float v = h[(size_t)y * M + x];
                return v == none ? hv : v;
            };
            float hl = at(px - 1, py), hr = at(px + 1, py), hu = at(px, py - 1), hd = at(px, py + 1);
            float shade = std::clamp(1.0f + ((hl - hr) + (hu - hd)) / 8.0f * 0.45f, 0.55f, 1.4f);
            if (hv <= 0 && (std::floor(hv / 50.0f) != std::floor(hr / 50.0f) || std::floor(hv / 50.0f) != std::floor(hd / 50.0f))) shade *= 0.86f;
            for (int k = 0; k < 3; ++k) o[k] = (uint8_t)std::clamp(c[k] * shade, 0.0f, 255.0f);
        }
    if (!SavePng(png, rgba, M, M)) {
        error = "The map picture could not be saved";
        return false;
    }
    return true;
}

namespace {

bool Uniform(const unsigned char* px, int w, int h) {
    int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
    for (size_t i = 0; i < (size_t)w * h; i += 7)
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], (int)px[i * 4 + c]);
            hi[c] = std::max(hi[c], (int)px[i * 4 + c]);
        }
    return hi[0] - lo[0] < 6 && hi[1] - lo[1] < 6 && hi[2] - lo[2] < 6;
}

int64_t Key(int x, int y) { return ((int64_t)x << 32) | (uint32_t)y; }

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
    std::wstring world = best.stem().wstring().substr(4);
    std::string data = ReadAll(best);
    size_t cells = data.size() / 10;
    if (!cells) return false;
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
    x0 -= 6;
    y0 -= 6;
    x1 += 6;
    y1 += 6;
    int cw = x1 - x0 + 1, ch = y1 - y0 + 1;
    if (cw > 4000 || ch > 4000) return false;
    int ppc = std::max(cw, ch) <= 1500 ? 2 : 1;
    int W = cw * ppc, H = ch * ppc;
    const int16_t none = INT16_MIN;
    std::vector<int16_t> g((size_t)cw * ch, none);
    for (size_t i = 0; i < cells; ++i) {
        int32_t x, y;
        int16_t hv;
        std::memcpy(&x, data.data() + i * 10, 4);
        std::memcpy(&y, data.data() + i * 10 + 4, 4);
        std::memcpy(&hv, data.data() + i * 10 + 8, 2);
        g[(size_t)(y - y0) * cw + (x - x0)] = hv;
    }
    auto at = [&](int cx, int cy, float fallback) {
        if (cx < 0 || cy < 0 || cx >= cw || cy >= ch) return fallback;
        int16_t v = g[(size_t)cy * cw + cx];
        return v == none ? fallback : v / 10.0f;
    };
    img.width = W;
    img.height = H;
    img.rgba.assign((size_t)W * H * 4, 0);
    for (int j = 0; j < H; ++j)
        for (int i = 0; i < W; ++i) {
            int cx = i / ppc, cy = j / ppc;
            int16_t raw = g[(size_t)cy * cw + cx];
            uint8_t* p = img.rgba.data() + ((size_t)j * W + i) * 4;
            if (raw == none) {
                p[0] = 7;
                p[1] = 16;
                p[2] = 31;
                p[3] = 255;
                continue;
            }
            float hv = raw / 10.0f, c[3];
            SeaColor(hv, c);
            float hl = at(cx - 1, cy, hv), hr = at(cx + 1, cy, hv), hu = at(cx, cy - 1, hv), hd = at(cx, cy + 1, hv);
            float shade = std::clamp(1.0f + ((hl - hr) + (hu - hd)) / 32.0f * 0.9f, 0.55f, 1.35f);
            if (std::floor(hv / 25.0f) != std::floor(hr / 25.0f) || std::floor(hv / 25.0f) != std::floor(hd / 25.0f)) shade *= 0.8f;
            for (int k = 0; k < 3; ++k) p[k] = (uint8_t)std::clamp(c[k] * shade, 0.0f, 255.0f);
            p[3] = 255;
        }
    double ox = (x0 - 0.5) * 8.0, oy = (y0 - 0.5) * 8.0, mpp = 8.0 / ppc;
    int tileOut = (int)std::lround(256.0 / mpp);
    std::unordered_map<int64_t, fs::path> tiles;
    std::unordered_map<int64_t, bool> good;
    for (auto it = fs::directory_iterator(folder / L"maptiles" / world, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::wstring n = it->path().stem().wstring();
        bool fallback = n.size() > 2 && n.compare(n.size() - 2, 2, L"_r") == 0;
        if (fallback) n.resize(n.size() - 2);
        int tx = 0, ty = 0;
        if (swscanf_s(n.c_str(), L"%d_%d", &tx, &ty) != 2) continue;
        int64_t k = Key(tx, ty);
        if (fallback && good.count(k) && good[k]) continue;
        tiles[k] = it->path();
        good[k] = !fallback;
    }
    for (const auto& [k, path] : tiles) {
        int tx = (int)(k >> 32), ty = (int)(int32_t)(uint32_t)k;
        std::string bytes = ReadAll(path);
        int w = 0, h = 0, n = 0;
        unsigned char* px = bytes.empty() ? nullptr : stbi_load_from_memory((const stbi_uc*)bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
        if (!px) continue;
        if (w <= 0 || h <= 0 || Uniform(px, w, h)) {
            stbi_image_free(px);
            continue;
        }
        int px0 = (int)std::lround((tx * 256.0 - ox) / mpp), py0 = (int)std::lround((ty * 256.0 - oy) / mpp);
        for (int j = 0; j < tileOut; ++j) {
            int y = py0 + j;
            if (y < 0 || y >= H) continue;
            for (int i = 0; i < tileOut; ++i) {
                int x = px0 + i;
                if (x < 0 || x >= W) continue;
                int sx0 = i * w / tileOut, sx1 = std::max(sx0 + 1, (i + 1) * w / tileOut);
                int sy0 = j * h / tileOut, sy1 = std::max(sy0 + 1, (j + 1) * h / tileOut);
                unsigned sum[3] = {0, 0, 0};
                int cnt = 0, lit = 0;
                for (int sy = sy0; sy < sy1 && sy < h; ++sy)
                    for (int sx = sx0; sx < sx1 && sx < w; ++sx) {
                        const unsigned char* s = px + ((size_t)sy * w + sx) * 4;
                        for (int c = 0; c < 3; ++c) sum[c] += s[c];
                        if (s[0] > 4 || s[1] > 4 || s[2] > 4) ++lit;
                        ++cnt;
                    }
                if (!cnt || lit * 2 < cnt) continue;
                uint8_t* d = img.rgba.data() + ((size_t)y * W + x) * 4;
                int16_t raw = g[(size_t)(y / ppc) * cw + (x / ppc)];
                float t = raw == none ? 0.15f : std::clamp(-raw / 10.0f / 500.0f, 0.0f, 0.55f);
                const float deep[3] = {18, 52, 96};
                for (int c = 0; c < 3; ++c) d[c] = (uint8_t)std::clamp((float)sum[c] / cnt * (1.0f - t) + deep[c] * t, 0.0f, 255.0f);
                d[3] = 255;
            }
        }
        stbi_image_free(px);
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
        if (!r.found[g]) {
            img.note = "This game is not installed on this PC.";
            Push(std::move(img));
            continue;
        }
        fs::path folder = r.folder[g];
        if (g == Subnautica) {
            fs::path png = folder / L"map.png";
            std::error_code ec;
            if (r.redraw || !fs::exists(png, ec)) {
                SetStatus(g, "Drawing the map from your game files");
                std::string err;
                if (!DrawSubnautica(r.root[g], png.wstring(), err)) {
                    SetStatus(g, "");
                    img.note = err.empty() ? "Drawing stopped." : err + ".";
                    Push(std::move(img));
                    continue;
                }
            }
            if (LoadPng(png, img)) {
                Sn1 s;
                if (LoadSn1(r.root[g], s)) Sn1Labels(s, img.labels);
                img.note = "Drawn from your Subnautica files. The map in the game uses this picture too.";
            } else {
                img.note = "The map picture could not be opened.";
            }
        } else if (g == Subnautica2) {
            SetStatus(g, "Putting the map together");
            if (ComposeSn2(folder, img))
                img.note = "Only the places you have been to are drawn. In the game, open Spawner & Map, then Map, and press Scan whole map to draw the rest.";
            else
                img.note = "Nothing is drawn yet. In the game, open Spawner & Map, then Map, and press Scan whole map.";
        } else {
            if (LoadPng(folder / L"map.png", img)) img.note = "Made from the game's own GPS map.";
            else img.note = "Open the map in the game once, then it shows up here.";
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
