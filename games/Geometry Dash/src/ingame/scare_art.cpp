#include "ingame/scare_art.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace scare_art {

namespace {

constexpr float kPi = 3.14159265358979f;

uint32_t Hash(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

float Hash01(int x, int y, uint32_t seed) {
    uint32_t h = Hash((uint32_t)x * 73856093u ^ (uint32_t)y * 19349663u ^ seed * 83492791u);
    return (float)(h & 0xFFFFFF) / 16777215.0f;
}

float Smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float Noise(float x, float y, uint32_t seed) {
    int xi = (int)std::floor(x), yi = (int)std::floor(y);
    float fx = Smooth(x - (float)xi), fy = Smooth(y - (float)yi);
    float a = Hash01(xi, yi, seed), b = Hash01(xi + 1, yi, seed);
    float c = Hash01(xi, yi + 1, seed), d = Hash01(xi + 1, yi + 1, seed);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

float Fbm(float x, float y, uint32_t seed) {
    float s = 0.0f, amp = 0.5f, f = 1.0f;
    for (int i = 0; i < 5; ++i) {
        s += amp * Noise(x * f, y * f, seed + (uint32_t)i * 101u);
        f *= 2.03f;
        amp *= 0.5f;
    }
    return s;
}

float Clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

float Step(float e0, float e1, float x) {
    float t = Clamp01((x - e0) / (e1 - e0));
    return t * t * (3.0f - 2.0f * t);
}

float Ell(float x, float y, float cx, float cy, float rx, float ry) {
    float dx = (x - cx) / rx, dy = (y - cy) / ry;
    return std::sqrt(dx * dx + dy * dy);
}

struct Col {
    float r, g, b;
};

Col Mix(Col a, Col b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t}; }

Col Mul(Col a, float k) { return {a.r * k, a.g * k, a.b * k}; }

Col Add(Col a, Col b, float k) { return {a.r + b.r * k, a.g + b.g * k, a.b + b.b * k}; }

struct Eye {
    float x, y, rx, ry, px, py, pr;
};

struct Style {
    Col skin, glow, toothCol;
    float headRx, headRy, warp;
    Eye eyes[2];
    float mouthX, mouthY, mouthRx, mouthRy, curve;
    int toothCount;
    float toothLen;
    float glowFall = 0.08f;
    float glowAmt = 0.9f;
    bool tears, horns, slit;
    uint32_t seed;
};

Style StyleFor(int v) {
    Style s{};
    if (v == 1) {
        s.skin = {0.52f, 0.62f, 0.44f};
        s.glow = {0.95f, 0.95f, 0.85f};
        s.toothCol = {0.74f, 0.69f, 0.45f};
        s.headRx = 0.62f;
        s.headRy = 0.88f;
        s.warp = 0.08f;
        s.eyes[0] = {-0.26f, -0.16f, 0.21f, 0.25f, -0.23f, -0.12f, 0.035f};
        s.eyes[1] = {0.28f, -0.10f, 0.13f, 0.15f, 0.30f, -0.08f, 0.03f};
        s.mouthX = 0.02f;
        s.mouthY = 0.47f;
        s.mouthRx = 0.24f;
        s.mouthRy = 0.30f;
        s.curve = 0.0f;
        s.toothCount = 10;
        s.toothLen = 0.07f;
        s.tears = true;
        s.seed = 23;
    } else if (v == 2) {
        s.skin = {0.62f, 0.14f, 0.10f};
        s.glow = {1.0f, 0.86f, 0.25f};
        s.toothCol = {0.95f, 0.92f, 0.80f};
        s.headRx = 0.64f;
        s.headRy = 0.80f;
        s.warp = 0.05f;
        s.eyes[0] = {-0.27f, -0.12f, 0.19f, 0.09f, -0.25f, -0.12f, 0.05f};
        s.eyes[1] = {0.27f, -0.12f, 0.19f, 0.09f, 0.27f, -0.12f, 0.05f};
        s.mouthX = 0.0f;
        s.mouthY = 0.44f;
        s.mouthRx = 0.52f;
        s.mouthRy = 0.20f;
        s.curve = -0.04f;
        s.toothCount = 12;
        s.toothLen = 0.17f;
        s.horns = true;
        s.slit = true;
        s.seed = 37;
    } else {
        s.skin = {0.82f, 0.80f, 0.74f};
        s.glow = {1.0f, 0.12f, 0.08f};
        s.toothCol = {0.93f, 0.89f, 0.72f};
        s.headRx = 0.66f;
        s.headRy = 0.84f;
        s.warp = 0.04f;
        s.eyes[0] = {-0.27f, -0.12f, 0.17f, 0.20f, -0.25f, -0.10f, 0.045f};
        s.eyes[1] = {0.27f, -0.12f, 0.17f, 0.20f, 0.29f, -0.10f, 0.045f};
        s.mouthX = 0.0f;
        s.mouthY = 0.45f;
        s.mouthRx = 0.46f;
        s.mouthRy = 0.16f;
        s.curve = 0.05f;
        s.toothCount = 16;
        s.toothLen = 0.11f;
        s.glowFall = 0.35f;
        s.glowAmt = 1.0f;
        s.seed = 11;
    }
    return s;
}

Col Pixel(const Style& st, float x, float y, int px, int py) {
    float n = Fbm(x * 3.0f + 7.0f, y * 3.0f + 3.0f, st.seed);
    Col c{0.05f + 0.22f * n * n, 0.01f + 0.03f * n, 0.015f + 0.03f * n};
    c = Mul(c, Clamp01(1.1f - 0.75f * (x * x + y * y)));

    if (st.horns) {
        for (int side = -1; side <= 1; side += 2) {
            float t = (-0.45f - y) / 0.55f;
            if (t < 0.0f || t > 1.0f) continue;
            float cx = (float)side * (0.34f + 0.30f * t + 0.12f * t * t);
            float hw = 0.13f * (1.0f - t) + 0.004f;
            float dx = std::fabs(x - cx);
            if (dx < hw) {
                float shade = 0.55f + 0.45f * Fbm(x * 9.0f, y * 9.0f, st.seed + 5) - 0.35f * (dx / hw);
                c = Mix(c, Mul(Col{0.16f, 0.10f, 0.07f}, shade), Step(hw, hw * 0.8f, dx));
            }
        }
    }

    float hy = y - 0.08f;
    float wx = x + st.warp * std::sin(y * 6.0f + (float)st.seed);
    float rx = st.headRx * (1.0f - 0.18f * std::max(0.0f, hy));
    float d = Ell(wx, hy, 0.0f, 0.0f, rx, st.headRy);
    if (d < 1.03f) {
        float a = Step(1.03f, 0.97f, d);
        float tex = Fbm(x * 6.0f + 1.0f, y * 6.0f + 5.0f, st.seed + 3);
        float shade = Clamp01(1.05f - 0.65f * std::pow(d, 2.2f)) * (0.72f + 0.38f * tex);
        shade *= 0.9f + 0.2f * Clamp01(-hy);
        Col skin = Mul(st.skin, shade);
        float vein = std::fabs(Fbm(x * 4.5f + 9.0f, y * 4.5f + 2.0f, st.seed + 7) - 0.5f);
        if (vein < 0.018f) skin = Mix(skin, Col{0.25f, 0.02f, 0.02f}, (0.018f - vein) / 0.018f * 0.8f);
        for (const Eye& e : st.eyes) {
            float de = Ell(x, y, e.x, e.y, e.rx, e.ry);
            if (de < 1.7f) skin = Mul(skin, 0.35f + 0.65f * Step(1.0f, 1.7f, de));
        }
        for (int side = -1; side <= 1; side += 2)
            if (Ell(x, y, (float)side * 0.05f, 0.18f, 0.025f, 0.045f) < 1.0f) skin = Mul(skin, 0.15f);
        c = Mix(c, skin, a);

        for (const Eye& e : st.eyes) {
            float de = Ell(x, y, e.x, e.y, e.rx, e.ry);
            float dp = st.slit ? Ell(x, y, e.px, e.py, e.pr * 0.35f, e.pr * 1.6f) : Ell(x, y, e.px, e.py, e.pr, e.pr);
            float glow = std::exp(-dp * dp * st.glowFall);
            if (de < 1.0f) {
                Col eye{0.02f, 0.0f, 0.0f};
                eye = Add(eye, st.glow, glow * st.glowAmt);
                if (dp < 1.0f) eye = Mix(eye, Col{1.0f, 1.0f, 0.95f}, Step(1.0f, 0.3f, dp));
                c = Mix(c, eye, Step(1.0f, 0.94f, de));
            } else if (de < 1.4f) {
                c = Add(c, st.glow, glow * 0.25f * (1.4f - de) / 0.4f);
            }
            if (st.tears) {
                float yb = e.y + e.ry * 0.9f;
                if (y > yb && y < yb + 0.55f) {
                    float k = (y - yb) / 0.55f;
                    float w = 0.025f * (1.0f - k) + 0.006f;
                    float cx = e.x + 0.02f * std::sin(y * 20.0f);
                    if (std::fabs(x - cx) < w) c = Mul(Col{0.40f, 0.0f, 0.0f}, 0.7f + 0.3f * tex);
                }
            }
        }

        float mx = (x - st.mouthX) / st.mouthRx;
        float curve = -st.curve * mx * mx;
        float my = (y - st.mouthY - curve) / st.mouthRy;
        float dm = std::sqrt(mx * mx + my * my);
        if (dm < 1.0f) {
            Col inside = Mul(Col{0.06f, 0.0f, 0.0f}, 0.5f + 0.5f * (1.0f - dm));
            inside = Mix(inside, Col{0.28f, 0.0f, 0.0f}, Step(0.6f, 0.0f, dm) * 0.6f);
            float half = std::sqrt(std::max(0.0f, 1.0f - mx * mx)) * st.mouthRy;
            float top = st.mouthY + curve - half;
            float bottom = st.mouthY + curve + half;
            float pos = (mx * 0.5f + 0.5f) * (float)st.toothCount;
            int k = (int)std::floor(pos);
            float t = pos - (float)k;
            float len = st.toothLen * (0.6f + 0.4f * Hash01(k, 1, st.seed));
            if (st.horns) len *= (k % 3 == 0) ? 1.6f : 0.8f;
            float shape = 1.0f - std::fabs(2.0f * t - 1.0f);
            if (y - top < len * shape) {
                float depth = Clamp01((y - top) / std::max(len, 0.001f));
                inside = Mul(st.toothCol, 0.75f + 0.25f * (1.0f - depth));
            }
            float pos2 = pos + 0.5f;
            int k2 = (int)std::floor(pos2);
            float t2 = pos2 - (float)k2;
            float len2 = st.toothLen * 0.8f * (0.6f + 0.4f * Hash01(k2, 2, st.seed));
            float shape2 = 1.0f - std::fabs(2.0f * t2 - 1.0f);
            if (bottom - y < len2 * shape2) {
                float depth = Clamp01((bottom - y) / std::max(len2, 0.001f));
                inside = Mul(st.toothCol, 0.7f + 0.25f * (1.0f - depth));
            }
            c = inside;
        } else if (dm < 1.08f) {
            c = Mul(c, 0.4f);
        }
    }

    float g = (Hash01(px, py, st.seed + 99) - 0.5f) * 0.06f;
    c = {c.r + g, c.g + g, c.b + g};
    c = {std::pow(Clamp01(c.r), 0.9f), std::pow(Clamp01(c.g), 0.95f), std::pow(Clamp01(c.b), 0.95f)};
    return c;
}

float Saw(double p) { return (float)(2.0 * (p - std::floor(p)) - 1.0); }

float Frac(double p) { return (float)(p - std::floor(p)); }

void Put16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back((uint8_t)(v & 0xFF));
    out.push_back((uint8_t)(v >> 8));
}

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)((v >> (8 * i)) & 0xFF));
}

void PutTag(std::vector<uint8_t>& out, const char* tag) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)tag[i]);
}

}

void Face(int variant, int size, std::vector<uint8_t>& rgba) {
    Style st = StyleFor(variant);
    rgba.assign((size_t)size * size * 4, 255);
    for (int py = 0; py < size; ++py) {
        for (int px = 0; px < size; ++px) {
            float x = ((float)px + 0.5f) / (float)size * 2.0f - 1.0f;
            float y = ((float)py + 0.5f) / (float)size * 2.0f - 1.0f;
            Col c = Pixel(st, x, y, px, py);
            size_t i = ((size_t)py * size + px) * 4;
            rgba[i + 0] = (uint8_t)std::lround(Clamp01(c.r) * 255.0f);
            rgba[i + 1] = (uint8_t)std::lround(Clamp01(c.g) * 255.0f);
            rgba[i + 2] = (uint8_t)std::lround(Clamp01(c.b) * 255.0f);
            rgba[i + 3] = 255;
        }
    }
}

void Scream(int variant, std::vector<uint8_t>& wav) {
    const int rate = 44100;
    const float dur = 1.3f;
    const int n = (int)(rate * dur);
    std::vector<float> s((size_t)n);
    uint32_t rnd = 0x1234567u + (uint32_t)variant * 977u;
    auto noise = [&rnd] {
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        return (float)(rnd & 0xFFFF) / 32767.5f - 1.0f;
    };
    double ph[6] = {};
    float lp = 0.0f;
    float peak = 0.0f;
    for (int i = 0; i < n; ++i) {
        float t = (float)i / (float)rate;
        float env = Clamp01(t / 0.012f) * Clamp01((dur - t) / 0.3f);
        float x = 0.0f;
        if (variant == 1) {
            ph[0] += (120.0 - 45.0 * t) / rate;
            float sq = Frac(ph[0]) < 0.5f ? 1.0f : -1.0f;
            ph[1] += (1700.0 + 300.0 * std::sin(2.0 * kPi * 5.0 * t)) / rate;
            float shriek = (float)std::sin(2.0 * kPi * ph[1]) * (0.5f + 0.5f * (float)std::sin(2.0 * kPi * 23.0 * t));
            float nz = noise();
            lp += (nz - lp) * 0.08f;
            x = std::tanh(2.8f * (0.55f * sq + 0.5f * shriek + 0.9f * lp + 0.2f * nz));
        } else if (variant == 2) {
            ph[0] += (700.0 + 400.0 * std::sin(2.0 * kPi * 3.0 * t)) / rate;
            ph[1] += 1130.0 / rate;
            float ring = Saw(ph[0]) * (float)std::sin(2.0 * kPi * ph[1]);
            float gate = ((int)(t * 25.0f) % 3 == 2) ? 0.25f : 1.0f;
            x = (ring * 0.9f + 0.35f * noise()) * gate;
            x = std::round(x * 16.0f) / 16.0f;
            x = std::tanh(2.5f * x);
        } else {
            double f = 900.0 + 650.0 * t + 70.0 * std::sin(2.0 * kPi * 7.5 * t) + 40.0 * std::sin(2.0 * kPi * 13.0 * t);
            for (int k = 0; k < 5; ++k) {
                ph[k] += f * (1.0 + (k - 2) * 0.012) / rate;
                x += Saw(ph[k]);
            }
            x = std::tanh(3.2f * (x * 0.3f + 0.45f * noise()));
        }
        s[(size_t)i] = x * env;
        peak = std::max(peak, std::fabs(s[(size_t)i]));
    }
    float gain = peak > 0.0f ? 0.92f / peak : 1.0f;
    wav.clear();
    wav.reserve((size_t)n * 2 + 44);
    PutTag(wav, "RIFF");
    Put32(wav, (uint32_t)(36 + n * 2));
    PutTag(wav, "WAVE");
    PutTag(wav, "fmt ");
    Put32(wav, 16);
    Put16(wav, 1);
    Put16(wav, 1);
    Put32(wav, (uint32_t)rate);
    Put32(wav, (uint32_t)rate * 2);
    Put16(wav, 2);
    Put16(wav, 16);
    PutTag(wav, "data");
    Put32(wav, (uint32_t)(n * 2));
    for (float v : s) {
        int q = (int)std::lround(std::clamp(v * gain, -1.0f, 1.0f) * 32767.0f);
        Put16(wav, (uint16_t)(int16_t)q);
    }
}

}
