#include "ingame/esp.h"
#include "game/ue.h"
#include "ui/ui.h"
#include "imgui.h"
#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <string>

namespace esp {

namespace {

constexpr double kPi = 3.14159265358979;

std::atomic<void*> liveCam{nullptr};
std::atomic<int> livePov{-1};
std::atomic<float> liveK{1.0f};

struct Projector {
    double cx = 0, cy = 0, cz = 0, fx = 0, fy = 0, fz = 0, rx = 0, ry = 0, rz = 0, ux = 0, uy = 0, uz = 0, focal = 0, halfW = 0, halfH = 0;
    bool ok = false;

    void Init(const core::CameraView& c, double f, ImVec2 display) {
        if (!(f > 1.0) || display.x <= 0 || display.y <= 0) return;
        double P = c.pitch * kPi / 180, Y = c.yaw * kPi / 180, R = c.roll * kPi / 180;
        double SP = std::sin(P), CP = std::cos(P), SY = std::sin(Y), CY = std::cos(Y), SR = std::sin(R), CR = std::cos(R);
        fx = CP * CY;
        fy = CP * SY;
        fz = SP;
        rx = SR * SP * CY - CR * SY;
        ry = SR * SP * SY + CR * CY;
        rz = -SR * CP;
        ux = -(CR * SP * CY + SR * SY);
        uy = CY * SR - CR * SP * SY;
        uz = CR * CP;
        cx = c.x;
        cy = c.y;
        cz = c.z;
        halfW = display.x * 0.5;
        halfH = display.y * 0.5;
        focal = f;
        ok = true;
    }

    bool operator()(double x, double y, double z, ImVec2& out) const {
        if (!ok) return false;
        double dx = x - cx, dy = y - cy, dz = z - cz;
        double vz = dx * fx + dy * fy + dz * fz;
        if (vz < 10) return false;
        double vx = dx * rx + dy * ry + dz * rz, vy = dx * ux + dy * uy + dz * uz;
        out = ImVec2((float)(halfW + vx * focal / vz), (float)(halfH - vy * focal / vz));
        return std::isfinite(out.x) && std::isfinite(out.y);
    }
};

bool LiveView(core::CameraView& v) {
    void* cam = liveCam.load();
    int off = livePov.load();
    if (!cam || off < 0) return false;
    float raw[7];
    if (!ue::SafeCopy(raw, (const uint8_t*)cam + off, sizeof(raw))) return false;
    for (float f : raw)
        if (!std::isfinite(f)) return false;
    if (raw[6] <= 5.0f || raw[6] >= 170.0f) return false;
    v.x = raw[0];
    v.y = raw[1];
    v.z = raw[2];
    v.pitch = raw[3];
    v.yaw = raw[4];
    v.roll = raw[5];
    v.fov = raw[6];
    v.valid = true;
    return true;
}

Projector MakeProjector(const core::Model& m, ImVec2 display) {
    Projector p;
    core::CameraView v;
    double k = liveK.load();
    if (!LiveView(v)) {
        v = m.camera;
        k = 1.0;
        if (v.valid && v.focal > 0 && v.width > 0 && v.fov > 1.0f) k = v.focal / ((v.width * 0.5) / std::tan(v.fov * 0.5 * kPi / 180.0));
    }
    if (!v.valid || v.fov <= 1.0f) return p;
    if (!(k > 0.25 && k < 4.0)) k = 1.0;
    p.Init(v, k * (display.x * 0.5) / std::tan(v.fov * 0.5 * kPi / 180.0), display);
    return p;
}

ImU32 Alpha(ImU32 c, float a) { return (c & 0x00FFFFFF) | ((ImU32)std::clamp(a * (float)((c >> 24) & 0xFF), 0.0f, 255.0f) << 24); }

ImU32 LootColor(int kind) {
    switch (kind) {
    case 1: return IM_COL32(232, 236, 240, 255);
    case 2: return IM_COL32(92, 226, 104, 255);
    case 3: return IM_COL32(72, 160, 255, 255);
    case 4: return IM_COL32(198, 112, 255, 255);
    case 5: return IM_COL32(255, 156, 40, 255);
    case 6: return IM_COL32(255, 214, 92, 255);
    default: return IM_COL32(210, 224, 255, 255);
    }
}

ImU32 HealthColor(float h) {
    h = std::clamp(h, 0.0f, 1.0f);
    float r = h < 0.5f ? 1.0f : 1.0f - (h - 0.5f) * 2.0f;
    float g = h < 0.5f ? h * 2.0f : 1.0f;
    return IM_COL32((int)(40 + 215 * r), (int)(40 + 190 * g), 60, 255);
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float alpha) {
    ImU32 sh = IM_COL32(0, 0, 0, (int)(220 * alpha));
    float o = std::max(1.0f, size / 14.0f);
    dl->AddText(font, size, ImVec2(pos.x - o, pos.y), sh, text);
    dl->AddText(font, size, ImVec2(pos.x + o, pos.y), sh, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y - o), sh, text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y + o), sh, text);
    dl->AddText(font, size, pos, Alpha(col, alpha), text);
}

ImVec2 Pill(ImDrawList* dl, ImFont* font, float size, ImVec2 anchor, bool above, ImU32 col, const std::string& text, float alpha, ImU32 accent) {
    ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, text.c_str());
    float padX = size * 0.45f, padY = size * 0.18f, bar = accent ? size * 0.22f : 0.0f;
    float w = ts.x + padX * 2 + bar, h = ts.y + padY * 2;
    ImVec2 a(anchor.x - w * 0.5f, above ? anchor.y - h : anchor.y);
    ImVec2 b(a.x + w, a.y + h);
    dl->AddRectFilled(a, b, IM_COL32(12, 14, 20, (int)(165 * alpha)), h * 0.35f);
    if (accent) dl->AddRectFilled(a, ImVec2(a.x + bar, b.y), Alpha(accent, alpha), h * 0.35f, ImDrawFlags_RoundCornersLeft);
    Text(dl, font, size, ImVec2(a.x + bar + padX, a.y + padY), col, text.c_str(), alpha);
    return ImVec2(w, h);
}

bool On(core::Model& m, const char* id) {
    core::Feature* f = m.find(id);
    return !f || f->on;
}

void DrawEnemies(core::Model& m, const Projector& proj, ImDrawList* dl, float base) {
    if (m.esp.empty()) return;
    bool showName = On(m, "esp_name"), showDist = On(m, "esp_distance"), showHealth = On(m, "esp_health"), showBox = On(m, "esp_box");
    ImFont* font = ui::T().semibold ? ui::T().semibold : ui::T().medium;
    for (const core::EspDot& d : m.esp) {
        bool badass = (d.kind & 1) != 0, hidden = (d.kind & 2) != 0;
        ImU32 col = badass ? IM_COL32(255, 150, 40, 255) : IM_COL32(255, 64, 72, 255);
        float alpha = std::clamp(1.25f - d.dist / 220.0f, 0.55f, 1.0f) * (hidden ? 0.7f : 1.0f);
        float scale = std::clamp(1.15f - d.dist / 300.0f, 0.75f, 1.0f);
        float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
        int seen = 0;
        bool haveBounds = d.ex > 0 && d.ey > 0 && d.ez > 0;
        if (haveBounds) {
            for (int i = 0; i < 8; ++i) {
                ImVec2 s;
                if (!proj(d.cx + ((i & 1) ? d.ex : -d.ex), d.cy + ((i & 2) ? d.ey : -d.ey), d.cz + ((i & 4) ? d.ez : -d.ez), s)) continue;
                x0 = std::min(x0, s.x);
                y0 = std::min(y0, s.y);
                x1 = std::max(x1, s.x);
                y1 = std::max(y1, s.y);
                ++seen;
            }
        }
        if (seen < 8) {
            ImVec2 top, bottom;
            if (!proj(d.x, d.y, d.top, top) || !proj(d.x, d.y, d.z, bottom)) continue;
            float h = std::fabs(bottom.y - top.y);
            x0 = top.x - h * 0.25f;
            x1 = top.x + h * 0.25f;
            y0 = std::min(top.y, bottom.y);
            y1 = std::max(top.y, bottom.y);
        }
        if (x1 - x0 < 4.0f) {
            float c = (x0 + x1) * 0.5f;
            x0 = c - 2.0f;
            x1 = c + 2.0f;
        }
        if (y1 - y0 < 6.0f) y0 = y1 - 6.0f;
        ImVec2 display = ImGui::GetIO().DisplaySize;
        if (x1 < -200 || x0 > display.x + 200 || y1 < -200 || y0 > display.y + 200) continue;
        float th = std::max(1.5f, 1.6f * base);
        if (showBox) {
            dl->AddRectFilled(ImVec2(x0, y0), ImVec2(x1, y1), IM_COL32(0, 0, 0, (int)(18 * alpha)), 2.0f);
            dl->AddRect(ImVec2(x0 - 1, y0 - 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(0, 0, 0, (int)(190 * alpha)), 2.0f, th + 1.5f);
            dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), Alpha(col, alpha), 2.0f, th);
        } else {
            ImVec2 c((x0 + x1) * 0.5f, y0);
            dl->AddCircleFilled(c, 4.0f * base, IM_COL32(0, 0, 0, (int)(200 * alpha)), 16);
            dl->AddCircleFilled(c, 3.0f * base, Alpha(col, alpha), 16);
        }
        if (showHealth && d.health >= 0) {
            float w = std::max(3.0f, 3.5f * base), gap = 3.0f * base;
            ImVec2 a(x0 - gap - w, y0), b(x0 - gap, y1);
            float fill = std::clamp(d.health, 0.0f, 1.0f);
            dl->AddRectFilled(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1), IM_COL32(0, 0, 0, (int)(200 * alpha)), 1.5f);
            dl->AddRectFilled(ImVec2(a.x, b.y - (b.y - a.y) * fill), b, Alpha(HealthColor(fill), alpha), 1.5f);
        }
        float nameSize = 18.5f * base * scale, distSize = 15.5f * base * scale;
        if (showName && !d.name.empty()) {
            ImVec2 ts = font->CalcTextSizeA(nameSize, FLT_MAX, 0, d.name.c_str());
            Text(dl, font, nameSize, ImVec2((x0 + x1) * 0.5f - ts.x * 0.5f, y0 - ts.y - 3.0f * base), badass ? col : IM_COL32(255, 255, 255, 255),
                 d.name.c_str(), alpha);
        }
        if (showDist) {
            std::string t = std::to_string((int)std::lround(d.dist)) + " m";
            ImVec2 ts = font->CalcTextSizeA(distSize, FLT_MAX, 0, t.c_str());
            Text(dl, font, distSize, ImVec2((x0 + x1) * 0.5f - ts.x * 0.5f, y1 + 2.0f * base), IM_COL32(225, 230, 238, 255), t.c_str(), alpha);
        }
    }
}

void DrawLoot(core::Model& m, const Projector& proj, ImDrawList* dl, float base) {
    if (m.itemEsp.empty()) return;
    bool showName = On(m, "lootesp_name"), showDist = On(m, "lootesp_distance");
    ImFont* font = ui::T().semibold ? ui::T().semibold : ui::T().medium;
    for (const core::EspDot& d : m.itemEsp) {
        ImVec2 p;
        if (!proj(d.x, d.y, d.z, p)) continue;
        ImU32 col = LootColor(d.kind);
        bool chest = d.kind == 6;
        float alpha = std::clamp(1.25f - d.dist / (chest ? 160.0f : 140.0f), 0.5f, 1.0f);
        float scale = std::clamp(1.1f - d.dist / 250.0f, 0.72f, 1.0f) * (d.kind == 5 ? 1.12f : 1.0f);
        float r = 4.2f * base * scale;
        if (chest) {
            dl->AddRectFilled(ImVec2(p.x - r - 1, p.y - r - 1), ImVec2(p.x + r + 1, p.y + r + 1), IM_COL32(0, 0, 0, (int)(210 * alpha)), 2.0f);
            dl->AddRectFilled(ImVec2(p.x - r, p.y - r), ImVec2(p.x + r, p.y + r), Alpha(col, alpha), 2.0f);
        } else {
            dl->AddCircleFilled(p, r + 1.2f, IM_COL32(0, 0, 0, (int)(210 * alpha)), 16);
            dl->AddCircleFilled(p, r, Alpha(col, alpha), 16);
        }
        bool label = chest ? d.dist < 50.0f : (d.dist < 75.0f || d.kind >= 4);
        if (!label) continue;
        std::string line;
        if (showName) line = d.name;
        if (showDist) line += (line.empty() ? "" : "  ") + std::to_string((int)std::lround(d.dist)) + " m";
        if (line.empty()) continue;
        float size = 16.5f * base * scale;
        Pill(dl, font, size, ImVec2(p.x, p.y - r - 3.0f * base), true, d.kind == 1 || chest ? IM_COL32(240, 242, 246, 255) : col, line, alpha, col);
    }
}

void DrawCircle(core::Model& m, const Projector& proj, ImDrawList* dl, float base) {
    if (!proj.ok) return;
    ImVec2 c((float)proj.halfW, (float)proj.halfH);
    auto ring = [&](const char* on, const char* area, ImU32 col) {
        core::Feature* f = m.find(on);
        core::Feature* a = m.find(area);
        if (!f || !f->on || !f->available || !a) return;
        float deg = std::clamp(a->value, 1.0f, 179.0f);
        if (deg >= 80.0f) return;
        float r = (float)(proj.focal * std::tan(deg * kPi / 180.0));
        if (!(r > 4.0f) || r > proj.halfW * 2.5f) return;
        dl->AddCircle(c, r, IM_COL32(0, 0, 0, 110), 96, 2.6f * base);
        dl->AddCircle(c, r, col, 96, 1.3f * base);
    };
    ring("silent_aim", "silent_area", IM_COL32(255, 255, 255, 120));
    ring("aim_assist", "aim_area", IM_COL32(120, 200, 255, 110));
}

}

void SetLiveCamera(void* manager, int povOffset, float focalScale) {
    liveK.store(focalScale);
    livePov.store(povOffset);
    liveCam.store(manager);
}

void ClearLiveCamera() {
    liveCam.store(nullptr);
    livePov.store(-1);
}

void Draw(core::Model& m) {
    bool any = !m.esp.empty() || !m.itemEsp.empty();
    core::Feature* sa = m.find("silent_aim");
    core::Feature* aa = m.find("aim_assist");
    bool rings = (sa && sa->on && sa->available) || (aa && aa->on && aa->available);
    if (!any && !rings) return;
    ImVec2 display = ImGui::GetIO().DisplaySize;
    Projector proj = MakeProjector(m, display);
    if (!proj.ok) return;
    float base = std::clamp(display.y / 1080.0f, 0.85f, 2.4f);
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    if (rings && m.inWorld) DrawCircle(m, proj, dl, base);
    DrawLoot(m, proj, dl, base);
    DrawEnemies(m, proj, dl, base);
}

}
