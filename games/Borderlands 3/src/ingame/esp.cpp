#include "ingame/esp.h"
#include "ui/ui.h"
#include "imgui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <string>

namespace esp {

namespace {

struct Projector {
    double cx, cy, cz, fx, fy, fz, rx, ry, rz, ux, uy, uz, focal, halfW, halfH;
    bool ok = false;

    Projector(const core::CameraView& c, ImVec2 display) {
        if (!c.valid || c.fov <= 1.0f || display.x <= 0 || display.y <= 0) return;
        double P = c.pitch * 3.14159265358979 / 180, Y = c.yaw * 3.14159265358979 / 180, R = c.roll * 3.14159265358979 / 180;
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
        focal = halfW / std::tan(c.fov * 0.5 * 3.14159265358979 / 180.0);
        ok = true;
    }

    bool operator()(double x, double y, double z, ImVec2& out) const {
        double dx = x - cx, dy = y - cy, dz = z - cz;
        double vz = dx * fx + dy * fy + dz * fz;
        if (vz < 10) return false;
        double vx = dx * rx + dy * ry + dz * rz, vy = dx * ux + dy * uy + dz * uz;
        out = ImVec2((float)(halfW + vx * focal / vz), (float)(halfH - vy * focal / vz));
        return true;
    }
};

ImU32 LootColor(int kind) {
    switch (kind) {
    case 1: return IM_COL32(225, 225, 225, 255);
    case 2: return IM_COL32(96, 224, 96, 255);
    case 3: return IM_COL32(86, 156, 255, 255);
    case 4: return IM_COL32(196, 108, 255, 255);
    case 5: return IM_COL32(255, 154, 44, 255);
    case 6: return IM_COL32(255, 214, 102, 255);
    default: return IM_COL32(200, 220, 255, 255);
    }
}

void DrawDots(core::Model& m, const std::vector<core::EspDot>& dots, bool items) {
    if (dots.empty()) return;
    Projector proj(m.camera, ImGui::GetIO().DisplaySize);
    if (!proj.ok) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    auto on = [&](const char* id) {
        core::Feature* f = m.find(id);
        return !f || f->on;
    };
    bool showName = on(items ? "lootesp_name" : "esp_name"), showDist = on(items ? "lootesp_distance" : "esp_distance");
    bool showHealth = !items && on("esp_health"), showBox = !items && on("esp_box");
    ImFont* font = ui::T().medium;
    float size = (items ? 12.0f : 13.0f) * m.menuScale;
    for (const core::EspDot& d : dots) {
        ImU32 col = items ? LootColor(d.kind) : (d.kind == 1 ? IM_COL32(255, 176, 64, 255) : ui::Danger());
        float alpha = std::clamp(1.2f - d.dist / (items ? 120.0f : 250.0f), 0.45f, 1.0f);
        ImVec2 p;
        if (!proj(d.x, d.y, d.z, p)) continue;
        ImVec2 label;
        if (!proj(d.x, d.y, d.top, label)) label = ImVec2(p.x, p.y - 20);
        bool drewBox = false;
        if (showBox && d.box) {
            ImVec2 a, b;
            if (proj(d.x, d.y, d.top, a) && proj(d.x, d.y, d.z - (d.top - d.z) * 0.15, b)) {
                float h = std::fabs(b.y - a.y);
                float w = h * 0.45f;
                float x0 = a.x - w * 0.5f, x1 = a.x + w * 0.5f, y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
                dl->AddRect(ImVec2(x0 + 1, y0 + 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(0, 0, 0, (int)(140 * alpha)), 3.0f, 1.5f);
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), (col & 0x00FFFFFF) | ((ImU32)(230 * alpha) << 24), 3.0f, 1.5f);
                label = ImVec2((x0 + x1) * 0.5f, y0);
                drewBox = true;
            }
        }
        if (!drewBox) dl->AddCircleFilled(p, 3.0f * m.menuScale, col, 12);
        std::string line;
        if (showName) line = d.name;
        if (showDist) line += (line.empty() ? "" : "  ") + std::to_string((int)d.dist) + " m";
        if (!line.empty()) {
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, line.c_str());
            ImVec2 tp(label.x - ts.x * 0.5f, label.y - ts.y - 4);
            dl->AddText(font, size, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, (int)(200 * alpha)), line.c_str());
            dl->AddText(font, size, tp, items ? ((col & 0x00FFFFFF) | ((ImU32)(255 * alpha) << 24)) : ui::White(alpha), line.c_str());
        }
        if (showHealth && d.health >= 0) {
            float w = 40.0f * m.menuScale, h = 3.0f * m.menuScale;
            ImVec2 a(label.x - w * 0.5f, label.y - 2), b(label.x + w * 0.5f, label.y - 2 + h);
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, (int)(160 * alpha)), h);
            dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(d.health, 0.0f, 1.0f), b.y), col, h);
        }
    }
}

}

void Draw(core::Model& m) {
    DrawDots(m, m.esp, false);
    DrawDots(m, m.itemEsp, true);
}

}
