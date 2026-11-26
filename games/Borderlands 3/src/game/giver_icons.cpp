#include "game/giver_icons.h"
#include "game/internal.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <thread>
#include <utility>

#pragma warning(push, 0)
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#pragma warning(pop)

namespace fs = std::filesystem;

namespace bl3::giver_icons {

namespace {

constexpr int kSize = 64;
constexpr int kArt = 1;
constexpr int kSamples = 4;
const char* kManifestHead = "fathom-giver-pictures";

enum Kind { kBox, kEllipse, kRing, kSeg, kPoly };

struct Op {
    int kind = kBox;
    bool cut = false;
    float v[6] = {};
    std::vector<float> pts;
};

using Ops = std::vector<Op>;
using Mask = std::vector<float>;

Op Make(int kind, bool cut, std::initializer_list<float> v) {
    Op o;
    o.kind = kind;
    o.cut = cut;
    int i = 0;
    for (float f : v)
        if (i < 6) o.v[i++] = f;
    return o;
}

Op Box(float x0, float y0, float x1, float y1, float r = 0, bool cut = false) { return Make(kBox, cut, {x0, y0, x1, y1, r}); }
Op Ellipse(float cx, float cy, float rx, float ry, bool cut = false) { return Make(kEllipse, cut, {cx, cy, rx, ry}); }
Op Ring(float cx, float cy, float ro, float ri, bool cut = false) { return Make(kRing, cut, {cx, cy, ro, ri}); }
Op Seg(float x0, float y0, float x1, float y1, float r, bool cut = false) { return Make(kSeg, cut, {x0, y0, x1, y1, r}); }

Op Poly(std::vector<float> pts, bool cut = false) {
    Op o;
    o.kind = kPoly;
    o.cut = cut;
    o.pts = std::move(pts);
    return o;
}

std::vector<float> Star(float cx, float cy, float ro, float ri, int points) {
    std::vector<float> p;
    for (int i = 0; i < points * 2; ++i) {
        float a = -1.5707963f + (float)i * 3.14159265f / (float)points;
        float r = (i % 2 == 0) ? ro : ri;
        p.push_back(cx + std::cos(a) * r);
        p.push_back(cy + std::sin(a) * r);
    }
    return p;
}

bool Inside(const Op& o, float x, float y) {
    switch (o.kind) {
    case kBox: {
        if (x < o.v[0] || x > o.v[2] || y < o.v[1] || y > o.v[3]) return false;
        float r = o.v[4];
        if (r <= 0) return true;
        float cx = std::clamp(x, o.v[0] + r, o.v[2] - r), cy = std::clamp(y, o.v[1] + r, o.v[3] - r);
        float dx = x - cx, dy = y - cy;
        return dx * dx + dy * dy <= r * r;
    }
    case kEllipse: {
        float dx = (x - o.v[0]) / o.v[2], dy = (y - o.v[1]) / o.v[3];
        return dx * dx + dy * dy <= 1.0f;
    }
    case kRing: {
        float dx = x - o.v[0], dy = y - o.v[1], d = dx * dx + dy * dy;
        return d <= o.v[2] * o.v[2] && d >= o.v[3] * o.v[3];
    }
    case kSeg: {
        float vx = o.v[2] - o.v[0], vy = o.v[3] - o.v[1], wx = x - o.v[0], wy = y - o.v[1];
        float len = vx * vx + vy * vy;
        float t = len > 0 ? std::clamp((wx * vx + wy * vy) / len, 0.0f, 1.0f) : 0.0f;
        float dx = wx - vx * t, dy = wy - vy * t;
        return dx * dx + dy * dy <= o.v[4] * o.v[4];
    }
    case kPoly: {
        bool in = false;
        size_t n = o.pts.size() / 2;
        for (size_t i = 0, j = n - 1; i < n; j = i++) {
            float xi = o.pts[i * 2], yi = o.pts[i * 2 + 1], xj = o.pts[j * 2], yj = o.pts[j * 2 + 1];
            if ((yi > y) != (yj > y) && x < (xj - xi) * (y - yi) / (yj - yi) + xi) in = !in;
        }
        return in;
    }
    }
    return false;
}

Mask Rasterize(const Ops& ops, float ox = 0, float oy = 0, float scale = 1.0f) {
    Mask m((size_t)kSize * kSize, 0.0f);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            int hits = 0;
            for (int j = 0; j < kSamples; ++j)
                for (int i = 0; i < kSamples; ++i) {
                    float px = 32.0f + ((float)x + ((float)i + 0.5f) / kSamples - ox - 32.0f) / scale;
                    float py = 32.0f + ((float)y + ((float)j + 0.5f) / kSamples - oy - 32.0f) / scale;
                    bool in = false;
                    for (const Op& o : ops)
                        if (o.cut == in && Inside(o, px, py)) in = !o.cut;
                    if (in) ++hits;
                }
            m[(size_t)y * kSize + x] = (float)hits / (kSamples * kSamples);
        }
    return m;
}

Mask Blur(const Mask& m) {
    Mask out(m.size(), 0.0f);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            float s = 0;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int xx = x + dx, yy = y + dy;
                    if (xx >= 0 && yy >= 0 && xx < kSize && yy < kSize) s += m[(size_t)yy * kSize + xx];
                }
            out[(size_t)y * kSize + x] = s / 9.0f;
        }
    return out;
}

Ops ShieldOps(float s, bool cut) {
    auto X = [&](float x) { return 32.0f + (x - 32.0f) * s; };
    auto Y = [&](float y) { return 32.0f + (y - 32.0f) * s; };
    std::vector<float> pts = {15, 28, 49, 28, 48.6f, 34, 46.5f, 40.5f, 42.5f, 46, 37.5f, 50.2f,
                              32, 52.8f, 26.5f, 50.2f, 21.5f, 46, 17.5f, 40.5f, 15.4f, 34};
    for (size_t i = 0; i + 1 < pts.size(); i += 2) {
        pts[i] = X(pts[i]);
        pts[i + 1] = Y(pts[i + 1]);
    }
    return {Box(X(15), Y(13), X(49), Y(30), 5 * s, cut), Poly(pts, cut)};
}

Ops Shape(int type) {
    Ops o;
    auto add = [&](const Ops& more) { o.insert(o.end(), more.begin(), more.end()); };
    switch (type) {
    case Pistol:
        o = {Box(14, 19, 50, 27.5f, 2),
             Box(12.5f, 19.5f, 15.5f, 23.5f, 1),
             Box(46, 17, 48.5f, 19.5f, 0.5f),
             Box(16, 17, 19, 19.5f, 0.5f),
             Box(25, 26.5f, 44, 31, 1),
             Poly({15.5f, 26, 27, 26, 24.5f, 46, 13, 46}),
             Box(25.5f, 29, 37.5f, 38.5f, 3.5f),
             Box(28, 29, 35, 35.5f, 2, true),
             Box(30.2f, 29.5f, 32.2f, 34, 0.8f)};
        break;
    case Smg:
        o = {Box(15, 21, 45, 29.5f, 2),
             Box(44, 23, 55.5f, 27, 1),
             Box(51.5f, 19.5f, 53.5f, 23, 0.5f),
             Box(22, 17.5f, 34, 21.5f, 1),
             Poly({22, 29, 29.5f, 29, 27, 42, 19.5f, 42}),
             Poly({33, 29, 38.5f, 29, 40.5f, 47, 35, 47}),
             Box(5.5f, 22, 16, 25.5f, 1),
             Box(5, 21, 9, 33.5f, 1.5f)};
        break;
    case Rifle:
        o = {Box(15, 23, 40, 30.5f, 1.5f),
             Box(39, 23.5f, 50.5f, 29.5f, 1.5f),
             Box(49.5f, 25, 59.5f, 27.6f, 0.8f),
             Box(55, 21, 57, 25, 0.4f),
             Box(20.5f, 19, 30.5f, 23.5f, 1),
             Poly({4, 24.5f, 15.5f, 23.5f, 15.5f, 30.5f, 9, 35, 4, 35}),
             Poly({19, 30, 25, 30, 23, 40.5f, 17, 40.5f}),
             Poly({28, 30, 34.5f, 30, 37.5f, 42, 35.5f, 46, 30.5f, 44.5f})};
        break;
    case Shotgun:
        o = {Box(19, 22.5f, 59, 26.5f, 1),
             Box(36, 26, 52, 31.5f, 2),
             Box(16.5f, 21.5f, 31, 30, 1.5f),
             Poly({3, 25, 17.5f, 23, 17.5f, 30, 8, 36, 3, 36}),
             Box(20, 28.5f, 28.5f, 35.5f, 3),
             Box(22.2f, 28.5f, 26.3f, 33, 1.5f, true),
             Box(55.5f, 20.5f, 57.5f, 22.8f, 0.5f)};
        break;
    case Sniper:
        o = {Box(33, 26.5f, 59, 28.7f, 0.8f),
             Box(56.5f, 25.3f, 61, 29.9f, 0.8f),
             Box(14, 24.5f, 35, 31, 1.5f),
             Seg(18, 19.5f, 36, 19.5f, 3.3f),
             Box(34.5f, 15.6f, 39, 23.4f, 1.2f),
             Box(14.5f, 16.4f, 18.5f, 22.6f, 1),
             Box(21, 21, 23.5f, 25, 0.4f),
             Box(29.5f, 21, 32, 25, 0.4f),
             Poly({3, 25.5f, 14.5f, 24.5f, 14.5f, 31, 7, 36, 3, 36}),
             Poly({17.5f, 30.5f, 23, 30.5f, 21, 39.5f, 15.5f, 39.5f}),
             Box(25.5f, 30.5f, 30.5f, 36, 1),
             Seg(47, 28.5f, 51.5f, 39, 1.1f),
             Seg(47, 28.5f, 42.5f, 39, 1.1f)};
        break;
    case Launcher:
        o = {Box(9, 21.5f, 52, 31.5f, 4.5f),
             Poly({50, 20.5f, 58.5f, 17.5f, 58.5f, 35.5f, 50, 32.5f}),
             Box(5, 22.5f, 10.5f, 30.5f, 1.5f),
             Box(15.5f, 21, 18, 32, 0, true),
             Box(43.5f, 21, 46, 32, 0, true),
             Poly({21, 31, 26.5f, 31, 24.5f, 41, 19, 41}),
             Box(35, 31, 39.5f, 39, 1.5f),
             Box(27, 17.5f, 33, 21.8f, 1),
             Box(29, 15, 31, 18, 0.5f)};
        break;
    case Shield:
        add(ShieldOps(1.0f, false));
        add(ShieldOps(0.74f, true));
        add(ShieldOps(0.56f, false));
        break;
    case Grenade:
        o = {Ellipse(31, 37.5f, 12.5f, 13.5f),
             Box(25.5f, 19.5f, 36.5f, 26.5f, 2),
             Poly({36, 20, 41.5f, 21.5f, 44.5f, 33, 42, 35, 39.5f, 25, 36, 24.5f}),
             Ring(21.5f, 19, 5.2f, 3.0f),
             Seg(24.8f, 21.2f, 27, 22.5f, 1.0f),
             Box(17, 36.3f, 45, 38.8f, 0, true),
             Box(29.8f, 27, 32.3f, 52, 0, true)};
        break;
    case ClassMod:
        o.push_back(Box(19, 19, 45, 45, 3.5f));
        for (int k = 0; k < 3; ++k) {
            float c = 25.0f + 7.0f * (float)k;
            o.push_back(Box(c - 1.6f, 13.5f, c + 1.6f, 19.5f, 0.6f));
            o.push_back(Box(c - 1.6f, 44.5f, c + 1.6f, 50.5f, 0.6f));
            o.push_back(Box(13.5f, c - 1.6f, 19.5f, c + 1.6f, 0.6f));
            o.push_back(Box(44.5f, c - 1.6f, 50.5f, c + 1.6f, 0.6f));
        }
        o.push_back(Poly(Star(32, 32.5f, 9.5f, 4.0f, 5), true));
        break;
    case Artifact:
        o = {Poly({18, 25, 25, 16, 39, 16, 46, 25, 32, 51}),
             Seg(18.5f, 25, 45.5f, 25, 1.1f, true),
             Seg(28.5f, 16.5f, 24.5f, 24.5f, 1.0f, true),
             Seg(35.5f, 16.5f, 39.5f, 24.5f, 1.0f, true),
             Seg(26.5f, 26.5f, 31, 43, 1.0f, true),
             Seg(37.5f, 26.5f, 33, 43, 1.0f, true),
             Poly(Star(49, 14.5f, 5.0f, 1.4f, 4))};
        break;
    case Ammo:
        for (int i = 0; i < 3; ++i) {
            float x0 = 15.5f + 11.5f * (float)i, top = i == 1 ? 26.0f : 29.0f;
            o.push_back(Box(x0, top, x0 + 8.5f, 50, 1));
            o.push_back(Ellipse(x0 + 4.25f, top, 4.25f, 10.5f));
        }
        o.push_back(Box(13, 44.5f, 50, 46.6f, 0, true));
        break;
    default:
        o = {Box(17, 21, 47, 47, 2.5f), Box(17, 27, 47, 29.5f, 0, true), Box(30.75f, 29.5f, 33.25f, 47, 0, true)};
        break;
    }
    return o;
}

float ShapeScale(int type) {
    switch (type) {
    case Pistol: return 1.14f;
    case Smg: return 1.04f;
    case Shield:
    case ClassMod: return 1.08f;
    case Grenade:
    case Artifact:
    case Ammo:
    case Other: return 1.1f;
    default: return 1.0f;
    }
}

struct Art {
    Mask tile, inner;
    std::array<Mask, TypeCount> shape, shadow;
};

const Art& Masks() {
    static const Art art = [] {
        Art a;
        a.tile = Rasterize({Box(1, 1, 63, 63, 12)});
        a.inner = Rasterize({Box(2.6f, 2.6f, 61.4f, 61.4f, 10.4f)});
        for (int t = 0; t < TypeCount; ++t) {
            Ops ops = Shape(t);
            float s = ShapeScale(t);
            a.shape[(size_t)t] = Rasterize(ops, 0, 0, s);
            a.shadow[(size_t)t] = Blur(Rasterize(ops, 0.8f, 1.6f, s));
        }
        return a;
    }();
    return art;
}

uint8_t Byte(float v) { return (uint8_t)std::clamp((int)std::lround(v), 0, 255); }

void Sink(void* ctx, void* data, int size) { static_cast<std::string*>(ctx)->append(static_cast<const char*>(data), (size_t)size); }

std::string Encode(int type, int tier) {
    int size = 0;
    std::vector<uint8_t> px = Render(type, tier, size);
    std::string out;
    if (px.empty() || !stbi_write_png_to_func(&Sink, &out, size, size, 4, px.data(), size * 4)) out.clear();
    return out;
}

bool Save(const fs::path& p, const std::string& bytes) {
    fs::path tmp = p;
    tmp += L".tmp";
    std::error_code ec;
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), (std::streamsize)bytes.size());
        out.close();
        if (!out) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    for (int attempt = 0; attempt < 4; ++attempt) {
        ec.clear();
        fs::rename(tmp, p, ec);
        if (!ec) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::error_code ignore;
    fs::remove(tmp, ignore);
    return false;
}

struct Entry {
    int type = -1, tier = -1, art = -1;
    std::string name;
};

std::string OneLine(std::string s) {
    for (char& c : s)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

std::map<std::string, Entry> ReadManifest(const std::wstring& dir) {
    std::map<std::string, Entry> m;
    std::ifstream in(fs::path(dir) / L"pictures.txt");
    std::string line;
    if (!in || !std::getline(in, line) || line.rfind(kManifestHead, 0) != 0) return m;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string id;
        Entry e;
        if (!(s >> id >> e.type >> e.tier >> e.art)) continue;
        std::getline(s >> std::ws, e.name);
        e.name = OneLine(e.name);
        m[id] = e;
    }
    return m;
}

bool WriteManifest(const std::wstring& dir, const std::map<std::string, Entry>& m) {
    std::string text = std::string(kManifestHead) + "\n";
    for (const auto& [id, e] : m) {
        text += id + " " + std::to_string(e.type) + " " + std::to_string(e.tier) + " " + std::to_string(e.art);
        std::string name = OneLine(e.name);
        if (!name.empty()) text += " " + name;
        text += "\n";
    }
    return Save(fs::path(dir) / L"pictures.txt", text);
}

int Tidy(const std::wstring& dir, const std::map<std::string, Entry>& manifest) {
    std::vector<fs::path> stale;
    std::error_code ec;
    for (const auto& f : fs::directory_iterator(dir, ec)) {
        const fs::path& fp = f.path();
        std::wstring ext = fp.extension().wstring();
        if (ext == L".tmp") stale.push_back(fp);
        else if (ext == L".png" && !manifest.count(ue::Utf8(fp.stem().wstring()))) stale.push_back(fp);
    }
    int removed = 0;
    for (const fs::path& fp : stale) {
        std::error_code rm;
        if (fs::remove(fp, rm) && fp.extension() == L".png") ++removed;
    }
    return removed;
}

std::thread* worker = nullptr;
std::atomic<bool> cancel{false}, finished{false}, completed{false}, atlasLoaded{false};
std::vector<Spec> current, pending;
std::wstring currentDir, pendingDir;
bool havePending = false;

void Run(const std::vector<Spec>& specs, const std::wstring& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    std::map<std::string, Entry> manifest = ReadManifest(dir);
    std::map<std::pair<int, int>, std::string> made;
    int written = 0, kept = 0, bad = 0;
    bool changed = false;
    for (const Spec& s : specs) {
        if (cancel) break;
        Entry& e = manifest[s.id];
        std::string name = OneLine(s.name);
        if (!name.empty() && name != e.name) {
            e.name = name;
            changed = true;
        }
        fs::path p = fs::path(dir) / (ue::Wide(s.id) + L".png");
        std::error_code exist;
        if (e.type == s.type && e.tier == s.tier && e.art == kArt && fs::exists(p, exist)) {
            ++kept;
            continue;
        }
        std::string& png = made[{s.type, s.tier}];
        if (png.empty()) png = Encode(s.type, s.tier);
        if (png.empty() || !Save(p, png)) {
            ++bad;
            continue;
        }
        e.type = s.type;
        e.tier = s.tier;
        e.art = kArt;
        changed = true;
        ++written;
    }
    int removed = cancel ? 0 : Tidy(dir, manifest);
    if (changed && !WriteManifest(dir, manifest)) LOG("giver: the item picture list could not be saved");
    if (cancel) return;
    completed = true;
    LOG("giver: item pictures ready, %d new, %d already there, %d old ones removed%s", written, kept, removed,
        bad ? ", some could not be saved" : "");
    if (written > 0 || removed > 0 || !atlasLoaded) {
        overlay::LoadAtlas(dir);
        atlasLoaded = true;
    }
}

}

std::wstring Dir() {
    std::wstring f = Folder();
    return f.empty() ? std::wstring() : f + L"\\giver_icons";
}

std::unordered_map<std::string, Known> Remembered(const std::wstring& dir) {
    std::unordered_map<std::string, Known> out;
    if (dir.empty()) return out;
    for (const auto& [id, e] : ReadManifest(dir)) {
        Known k;
        if (e.tier >= 1 && e.tier <= 5) k.tier = e.tier;
        k.name = e.name;
        if (k.tier > 0 || !k.name.empty()) out[id] = k;
    }
    return out;
}

std::vector<uint8_t> Render(int type, int tier, int& size) {
    size = kSize;
    const Art& a = Masks();
    size_t ti = (size_t)std::clamp(type, 0, TypeCount - 1);
    static const float kTint[6][3] = {{88, 96, 110}, {150, 156, 164}, {62, 170, 70}, {44, 122, 232}, {156, 72, 222}, {236, 128, 28}};
    const float* c = kTint[std::clamp(tier, 0, 5)];
    std::vector<uint8_t> px((size_t)kSize * kSize * 4, 0);
    for (int y = 0; y < kSize; ++y)
        for (int x = 0; x < kSize; ++x) {
            size_t i = (size_t)y * kSize + x;
            float alpha = a.tile[i];
            if (alpha <= 0.0f) continue;
            float t = ((float)y + 0.5f) / kSize;
            float gx = ((float)x + 0.5f - 32.0f) / 34.0f, gy = ((float)y + 0.5f - 26.0f) / 34.0f;
            float glow = std::max(0.0f, 1.0f - std::sqrt(gx * gx + gy * gy));
            glow = glow * glow * 0.14f;
            float border = std::max(0.0f, a.tile[i] - a.inner[i]);
            float sil = a.shape[ti][i], shadow = a.shadow[ti][i];
            uint8_t* d = &px[i * 4];
            for (int k = 0; k < 3; ++k) {
                float top = c[k] + (255.0f - c[k]) * 0.20f, bottom = c[k] * 0.70f;
                float v = top + (bottom - top) * t;
                v += (255.0f - v) * glow;
                float edge = c[k] + (255.0f - c[k]) * 0.55f;
                v += (edge - v) * border * 0.6f;
                v *= 1.0f - shadow * 0.45f;
                v += (252.0f - v) * sil;
                d[k] = Byte(v);
            }
            d[3] = Byte(alpha * 255.0f);
        }
    return px;
}

void Request(std::vector<Spec> specs, const std::wstring& dir) {
    if (dir.empty() || specs.empty()) return;
    pending = std::move(specs);
    pendingDir = dir;
    havePending = true;
    Pump();
}

void Pump() {
    if (worker && finished) {
        worker->join();
        delete worker;
        worker = nullptr;
    }
    if (worker || !havePending) return;
    havePending = false;
    current = std::move(pending);
    pending.clear();
    currentDir = pendingDir;
    cancel = false;
    finished = false;
    completed = false;
    try {
        worker = new std::thread([] {
            try {
                Run(current, currentDir);
            } catch (...) {
                LOG("giver: making item pictures failed");
            }
            finished = true;
        });
    } catch (...) {
        worker = nullptr;
        LOG("giver: could not start making item pictures");
    }
}

void Stop() {
    if (!worker) return;
    cancel = true;
    worker->join();
    delete worker;
    worker = nullptr;
    if (!completed && !havePending) {
        pending = current;
        pendingDir = currentDir;
        havePending = true;
    }
}

}
