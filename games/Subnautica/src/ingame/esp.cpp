#include "ingame/esp.h"
#include "imgui.h"
#include "ui/ui.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <mutex>

namespace esp {

namespace {

std::mutex lock;
Frame current;

struct Proj {
    const Frame& f;
    ImVec2 size;
    float M(const float* m, int r, int c) const { return m[c * 4 + r]; }
    bool operator()(float x, float y, float z, ImVec2& out) const {
        const float* v = f.view;
        const float* p = f.proj;
        float vx = M(v, 0, 0) * x + M(v, 0, 1) * y + M(v, 0, 2) * z + M(v, 0, 3);
        float vy = M(v, 1, 0) * x + M(v, 1, 1) * y + M(v, 1, 2) * z + M(v, 1, 3);
        float vz = M(v, 2, 0) * x + M(v, 2, 1) * y + M(v, 2, 2) * z + M(v, 2, 3);
        float vw = M(v, 3, 0) * x + M(v, 3, 1) * y + M(v, 3, 2) * z + M(v, 3, 3);
        float cx = M(p, 0, 0) * vx + M(p, 0, 1) * vy + M(p, 0, 2) * vz + M(p, 0, 3) * vw;
        float cy = M(p, 1, 0) * vx + M(p, 1, 1) * vy + M(p, 1, 2) * vz + M(p, 1, 3) * vw;
        float cw = M(p, 3, 0) * vx + M(p, 3, 1) * vy + M(p, 3, 2) * vz + M(p, 3, 3) * vw;
        if (cw < 0.05f) return false;
        out = ImVec2((cx / cw * 0.5f + 0.5f) * size.x, (0.5f - cy / cw * 0.5f) * size.y);
        return true;
    }
};

ImU32 CreatureColor(int kind) {
    if (kind == 2) return ui::Danger();
    if (kind == 1) return IM_COL32(255, 176, 96, 255);
    return ui::Accent();
}

ImU32 ItemColor(int kind) {
    if (kind == 1) return IM_COL32(200, 160, 255, 255);
    if (kind == 2) return IM_COL32(255, 214, 102, 255);
    if (kind == 3) return IM_COL32(255, 160, 90, 255);
    return IM_COL32(220, 240, 255, 255);
}

void DrawList(const Frame& f, const std::vector<Dot>& dots, bool items, float scale) {
    if (dots.empty()) return;
    Proj proj{f, ImGui::GetIO().DisplaySize};
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    bool showName = items ? f.iName : f.cName, showDist = items ? f.iDist : f.cDist, showBox = items ? f.iBox : f.cBox;
    bool showHealth = !items && f.cHealth;
    ImFont* font = ui::T().medium;
    float size = (items ? 12.0f : 13.0f) * scale;
    for (const Dot& d : dots) {
        ImU32 col = items ? ItemColor(d.kind) : CreatureColor(d.kind);
        float dx = d.x - f.px, dy = d.y - f.py, dz = d.z - f.pz;
        float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
        float alpha = std::clamp(1.2f - dist / (items ? 150.0f : 300.0f), 0.45f, 1.0f);
        ImVec2 p;
        if (!proj(d.x, d.y, d.z, p)) continue;
        ImVec2 label(p.x, p.y - 8 * scale);
        bool drewBox = false;
        if (showBox && d.box) {
            float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
            int n = 0;
            for (int k = 0; k < 8; ++k) {
                ImVec2 s;
                if (!proj(d.bx + ((k & 1) ? d.ex : -d.ex), d.by + ((k & 2) ? d.ey : -d.ey), d.bz + ((k & 4) ? d.ez : -d.ez), s)) continue;
                ++n;
                x0 = std::min(x0, s.x);
                y0 = std::min(y0, s.y);
                x1 = std::max(x1, s.x);
                y1 = std::max(y1, s.y);
            }
            if (n == 8 && x1 - x0 < 4000 && y1 - y0 < 4000) {
                dl->AddRect(ImVec2(x0 + 1, y0 + 1), ImVec2(x1 + 1, y1 + 1), IM_COL32(0, 0, 0, (int)(140 * alpha)), 3.0f, 1.5f);
                dl->AddRect(ImVec2(x0, y0), ImVec2(x1, y1), (col & 0x00FFFFFF) | ((ImU32)(230 * alpha) << 24), 3.0f, 1.5f);
                label = ImVec2((x0 + x1) * 0.5f, y0);
                drewBox = true;
            }
        }
        if (!drewBox) dl->AddCircleFilled(p, 3.0f * scale, col, 12);
        std::string line;
        if (showName) line = d.name;
        if (showDist) line += (line.empty() ? "" : "  ") + std::to_string((int)dist) + " m";
        if (!line.empty() && font) {
            ImVec2 ts = font->CalcTextSizeA(size, FLT_MAX, 0, line.c_str());
            ImVec2 tp(label.x - ts.x * 0.5f, label.y - ts.y - 4);
            dl->AddText(font, size, ImVec2(tp.x + 1, tp.y + 1), IM_COL32(0, 0, 0, (int)(200 * alpha)), line.c_str());
            dl->AddText(font, size, tp, items ? ((col & 0x00FFFFFF) | ((ImU32)(255 * alpha) << 24)) : ui::White(alpha), line.c_str());
        }
        if (showHealth && d.health >= 0) {
            float w = 40.0f * scale, h = 3.0f * scale;
            ImVec2 a(label.x - w * 0.5f, label.y - 2), b(label.x + w * 0.5f, label.y - 2 + h);
            if (!drewBox) {
                a = ImVec2(p.x - w * 0.5f, p.y + 6);
                b = ImVec2(p.x + w * 0.5f, p.y + 6 + h);
            }
            dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, (int)(160 * alpha)), h);
            dl->AddRectFilled(a, ImVec2(a.x + w * std::clamp(d.health, 0.0f, 1.0f), b.y), col, h);
        }
    }
}

}

void Publish(Frame&& f) {
    std::lock_guard<std::mutex> g(lock);
    current = std::move(f);
}

void Clear() {
    std::lock_guard<std::mutex> g(lock);
    current = Frame{};
}

void Draw(float scale) {
    std::lock_guard<std::mutex> g(lock);
    if (!current.valid) return;
    if (current.creatures) DrawList(current, current.creatureDots, false, scale);
    if (current.items) DrawList(current, current.itemDots, true, scale);
}

}
