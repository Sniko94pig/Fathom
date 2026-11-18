#include "ui/ui.h"
#include "imgui_stdlib.h"
#include <cmath>
#include <unordered_map>

extern const unsigned char font_inter_regular[];
extern const unsigned int font_inter_regular_size;
extern const unsigned char font_inter_medium[];
extern const unsigned int font_inter_medium_size;
extern const unsigned char font_inter_semibold[];
extern const unsigned int font_inter_semibold_size;
extern const unsigned char font_lucide[];
extern const unsigned int font_lucide_size;

namespace ui {

namespace {
Theme g_theme;
std::unordered_map<ImGuiID, float> g_anim;
std::string g_tip;
std::string g_tipShown;
float g_tipAnim = 0.0f;

ImU32 Mul(ImVec4 c, float a) {
    c.w *= a * ImGui::GetStyle().Alpha;
    if (c.w < 0) c.w = 0;
    if (c.w > 1) c.w = 1;
    return ImGui::ColorConvertFloat4ToU32(c);
}

ImVec2 Snap(ImVec2 p) {
    return ImVec2(std::floor(p.x + 0.5f), std::floor(p.y + 0.5f));
}
}

Theme& T() {
    return g_theme;
}

void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    ImFontConfig cfg;
    cfg.FontDataOwnedByAtlas = false;
    g_theme.regular = io.Fonts->AddFontFromMemoryTTF((void*)font_inter_regular, (int)font_inter_regular_size, 14.0f, &cfg);
    g_theme.medium = io.Fonts->AddFontFromMemoryTTF((void*)font_inter_medium, (int)font_inter_medium_size, 14.0f, &cfg);
    g_theme.semibold = io.Fonts->AddFontFromMemoryTTF((void*)font_inter_semibold, (int)font_inter_semibold_size, 14.0f, &cfg);
    g_theme.icons = io.Fonts->AddFontFromMemoryTTF((void*)font_lucide, (int)font_lucide_size, 16.0f, &cfg);
    io.FontDefault = g_theme.regular;
}

void ApplyStyle() {
    ImGuiStyle& st = ImGui::GetStyle();
    st.FontSizeBase = S(14);
    st.FontScaleMain = 1.0f;
    st.WindowRounding = S(16);
    st.ChildRounding = S(12);
    st.FrameRounding = S(8);
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 0;
    st.FrameBorderSize = 0;
    st.ItemSpacing = ImVec2(0, 0);
    st.ScrollbarSize = S(6);
    st.ScrollbarRounding = S(3);
    st.DisabledAlpha = 1.0f;
    st.AntiAliasedLines = true;
    st.AntiAliasedFill = true;
    st.CircleTessellationMaxError = 0.15f;
    st.CurveTessellationTol = 0.6f;
    st.Colors[ImGuiCol_Text] = ImVec4(1, 1, 1, 0.94f);
    st.Colors[ImGuiCol_TextDisabled] = ImVec4(1, 1, 1, 0.36f);
    st.Colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_Border] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_FrameBgActive] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(1, 1, 1, 0.12f);
    st.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(1, 1, 1, 0.20f);
    st.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(1, 1, 1, 0.28f);
    st.Colors[ImGuiCol_TextSelectedBg] = ImVec4(g_theme.accent.x, g_theme.accent.y, g_theme.accent.z, 0.35f);
    st.Colors[ImGuiCol_NavCursor] = ImVec4(0, 0, 0, 0);
    st.Colors[ImGuiCol_InputTextCursor] = g_theme.accent;
}

float S(float v) {
    return std::floor(v * g_theme.scale + 0.5f);
}

float Dt() {
    return ImGui::GetIO().DeltaTime;
}

ImU32 Accent(float a) {
    return Mul(g_theme.accent, a);
}

ImU32 White(float a) {
    return Mul(ImVec4(1, 1, 1, 1), a);
}

ImU32 Black(float a) {
    return Mul(ImVec4(0, 0, 0, 1), a);
}

ImU32 Bg(float a) {
    return Mul(g_theme.bg, a);
}

ImU32 Danger(float a) {
    return Mul(g_theme.danger, a);
}

ImU32 Mix(const ImVec4& a, const ImVec4& b, float t, float alpha) {
    ImVec4 c(a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t, a.w + (b.w - a.w) * t);
    return Mul(c, alpha);
}

float Ease(ImGuiID id, float target, float speed, float init) {
    auto it = g_anim.find(id);
    if (it == g_anim.end()) it = g_anim.emplace(id, init).first;
    float& v = it->second;
    v += (target - v) * (1.0f - std::exp(-speed * Dt()));
    if (std::fabs(target - v) < 0.002f) v = target;
    return v;
}

void SetAnim(ImGuiID id, float v) {
    g_anim[id] = v;
}

ImVec2 Measure(ImFont* font, float size, const char* text) {
    ImGui::PushFont(font, S(size));
    ImVec2 r = ImGui::CalcTextSize(text, nullptr, false, -1.0f);
    ImGui::PopFont();
    return r;
}

void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float wrap) {
    ImGui::PushFont(font, S(size));
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), Snap(pos), col, text, nullptr, wrap);
    ImGui::PopFont();
}

void TextRight(ImDrawList* dl, ImFont* font, float size, ImVec2 rightTop, ImU32 col, const char* text) {
    ImVec2 sz = Measure(font, size, text);
    Text(dl, font, size, ImVec2(rightTop.x - sz.x, rightTop.y), col, text);
}

void TextFit(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float maxW, ImU32 col, const char* text) {
    if (Measure(font, size, text).x <= maxW) {
        Text(dl, font, size, pos, col, text);
        return;
    }
    std::string s = text;
    while (!s.empty()) {
        s.pop_back();
        while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
        std::string t = s + "\xe2\x80\xa6";
        if (Measure(font, size, t.c_str()).x <= maxW) {
            Text(dl, font, size, pos, col, t.c_str());
            return;
        }
    }
}

void Icon(ImDrawList* dl, ImVec2 center, float size, ImU32 col, const char* icon) {
    ImGui::PushFont(g_theme.icons, size);
    ImVec2 sz = ImGui::CalcTextSize(icon);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), Snap(ImVec2(center.x - sz.x * 0.5f, center.y - sz.y * 0.5f)), col, icon);
    ImGui::PopFont();
}

void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, const ImVec4& col, float strength, float spread) {
    int n = (int)spread;
    if (n < 1 || strength <= 0.0f) return;
    for (int i = n; i >= 1; --i) {
        float t = (float)i / (float)n;
        float al = strength * (1.0f - t) * (1.0f - t) * 0.42f;
        if (al < 0.004f) continue;
        dl->AddRect(ImVec2(a.x - i, a.y - i), ImVec2(b.x + i, b.y + i), Mul(col, al), rounding + i, 1.6f);
    }
}

ImVec2 SwitchSize() {
    return ImVec2(S(36), S(20));
}

void Switch(ImDrawList* dl, ImVec2 pos, float t, float dim) {
    ImVec2 sz = SwitchSize();
    float r = sz.y * 0.5f;
    ImVec2 b(pos.x + sz.x, pos.y + sz.y);
    if (t > 0.01f) Glow(dl, pos, b, r, g_theme.accent, t * dim, S(9));
    dl->AddRectFilled(pos, b, Mix(ImVec4(1, 1, 1, 0.14f), g_theme.accent, t, dim), r);
    float kx = pos.x + r + (sz.x - sz.y) * t;
    dl->AddCircleFilled(ImVec2(kx, pos.y + r), r - S(3), Mix(ImVec4(1, 1, 1, 0.85f), ImVec4(1, 1, 1, 1), t, dim), 24);
}

bool Button(const char* id, const char* label, ImVec2 pos, ImVec2 size, Btn style, const char* icon, bool enabled) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("b", size) && enabled;
    bool hov = ImGui::IsItemHovered() && enabled;
    bool held = ImGui::IsItemActive() && enabled;
    float h = Ease(ImGui::GetID("h"), hov ? 1.0f : 0.0f, 18.0f);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float dim = enabled ? 1.0f : 0.38f;
    float press = held ? 0.85f : 1.0f;
    ImU32 bg, fg;
    switch (style) {
    case Btn::Primary:
        bg = Accent((0.86f + 0.14f * h) * dim * press);
        fg = Bg(dim);
        break;
    case Btn::Tint:
        bg = Accent((0.12f + 0.10f * h) * dim * press);
        fg = Accent(dim);
        break;
    case Btn::Danger:
        bg = Danger((0.14f + 0.12f * h) * dim * press);
        fg = Danger(dim);
        break;
    default:
        bg = White((0.06f + 0.05f * h) * dim * press);
        fg = White(0.9f * dim);
        break;
    }
    dl->AddRectFilled(pos, ImVec2(pos.x + size.x, pos.y + size.y), bg, S(8));
    ImVec2 ts = Measure(g_theme.medium, 13, label);
    float iw = icon ? S(22) : 0.0f;
    float x = pos.x + (size.x - ts.x - iw) * 0.5f;
    if (icon) Icon(dl, ImVec2(x + S(8), pos.y + size.y * 0.5f), S(15), fg, icon);
    Text(dl, g_theme.medium, 13, ImVec2(x + iw, pos.y + (size.y - ts.y) * 0.5f), fg, label);
    return pressed;
}

bool IconButton(const char* id, const char* icon, ImVec2 pos, float size, bool enabled, bool danger) {
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("b", ImVec2(size, size)) && enabled;
    bool hov = ImGui::IsItemHovered() && enabled;
    float h = Ease(ImGui::GetID("h"), hov ? 1.0f : 0.0f, 18.0f);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float dim = enabled ? 1.0f : 0.38f;
    dl->AddRectFilled(pos, ImVec2(pos.x + size, pos.y + size), danger ? Danger(0.16f * h) : White((0.05f + 0.06f * h) * dim), S(8));
    ImU32 fg = danger ? Mix(ImVec4(1, 1, 1, 0.6f), g_theme.danger, h, dim) : White((0.65f + 0.3f * h) * dim);
    Icon(dl, ImVec2(pos.x + size * 0.5f, pos.y + size * 0.5f), S(15), fg, icon);
    return pressed;
}

float ChipWidth(const char* label) {
    return Measure(g_theme.medium, 12, label).x + S(24);
}

bool Chip(const char* id, const char* label, ImVec2 pos, bool selected, float width) {
    float w = width > 0 ? width : ChipWidth(label);
    float hgt = S(28);
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("c", ImVec2(w, hgt));
    float h = Ease(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
    float s = Ease(ImGui::GetID("s"), selected ? 1.0f : 0.0f, 18.0f, selected ? 1.0f : 0.0f);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec4 off(1, 1, 1, 0.05f + 0.04f * h);
    ImVec4 on(g_theme.accent.x, g_theme.accent.y, g_theme.accent.z, 0.16f);
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + hgt), Mix(off, on, s), hgt * 0.5f);
    ImVec2 ts = Measure(g_theme.medium, 12, label);
    Text(dl, g_theme.medium, 12, ImVec2(pos.x + (w - ts.x) * 0.5f, pos.y + (hgt - ts.y) * 0.5f),
         Mix(ImVec4(1, 1, 1, 0.62f + 0.3f * h), g_theme.accent, s), label);
    return pressed;
}

bool Segmented(const char* id, const char* const* labels, int count, int* index, ImVec2 pos, float width) {
    float hgt = S(36);
    float pad = S(4);
    float cell = (width - pad * 2) / (float)count;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + hgt), White(0.05f), S(10));
    ImGui::PushID(id);
    float fi = Ease(ImGui::GetID("sel"), (float)*index, 18.0f, (float)*index);
    ImVec2 sp(pos.x + pad + cell * fi, pos.y + pad);
    dl->AddRectFilled(sp, ImVec2(sp.x + cell, sp.y + hgt - pad * 2), White(0.10f), S(7));
    bool changed = false;
    for (int i = 0; i < count; ++i) {
        ImVec2 cp(pos.x + pad + cell * i, pos.y + pad);
        ImGui::SetCursorScreenPos(cp);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("s", ImVec2(cell, hgt - pad * 2)) && *index != i) {
            *index = i;
            changed = true;
        }
        float h = Ease(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
        float a = Ease(ImGui::GetID("a"), *index == i ? 1.0f : 0.0f, 18.0f, *index == i ? 1.0f : 0.0f);
        ImGui::PopID();
        ImVec2 ts = Measure(g_theme.medium, 13, labels[i]);
        float al = 0.55f + 0.2f * h + 0.4f * a;
        Text(dl, g_theme.medium, 13, ImVec2(cp.x + (cell - ts.x) * 0.5f, cp.y + (hgt - pad * 2 - ts.y) * 0.5f),
             White(al > 0.96f ? 0.96f : al), labels[i]);
    }
    ImGui::PopID();
    return changed;
}

bool Input(const char* id, std::string* value, const char* hint, ImVec2 pos, float width, const char* icon) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float hgt = S(36);
    ImGui::PushID(id);
    ImGuiID fid = ImGui::GetID("f");
    float padL = icon ? S(36) : S(12);
    ImGui::PushFont(g_theme.regular, S(13));
    float fh = ImGui::GetFontSize();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, S(4)));
    ImGui::SetCursorScreenPos(ImVec2(pos.x + padL, pos.y + (hgt - fh) * 0.5f - S(4)));
    ImGui::SetNextItemWidth(width - padL - S(12));
    ImDrawListSplitter split;
    split.Split(dl, 2);
    split.SetCurrentChannel(dl, 1);
    bool changed = ImGui::InputTextWithHint("##in", hint, value);
    bool active = ImGui::IsItemActive();
    bool hov = ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + width, pos.y + hgt));
    ImGui::PopStyleVar();
    ImGui::PopFont();
    float f = Ease(fid, active ? 1.0f : 0.0f, 18.0f);
    split.SetCurrentChannel(dl, 0);
    dl->AddRectFilled(pos, ImVec2(pos.x + width, pos.y + hgt), White(0.05f + (hov ? 0.02f : 0.0f)), S(8));
    if (f > 0.01f) dl->AddRect(pos, ImVec2(pos.x + width, pos.y + hgt), Accent(0.55f * f), S(8), 1.0f);
    if (icon) Icon(dl, ImVec2(pos.x + S(18), pos.y + hgt * 0.5f), S(15), White(0.45f + 0.3f * f), icon);
    split.Merge(dl);
    ImGui::PopID();
    return changed;
}

void Tip(const char* text) {
    g_tip = text;
}

void DrawTip() {
    bool want = !g_tip.empty();
    if (want) g_tipShown = g_tip;
    g_tip.clear();
    float target = want ? 1.0f : 0.0f;
    g_tipAnim += (target - g_tipAnim) * (1.0f - std::exp(-18.0f * Dt()));
    if (g_tipAnim < 0.02f || g_tipShown.empty()) return;
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_tipAnim);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ts = Measure(g_theme.regular, 12, g_tipShown.c_str());
    ImVec2 m = ImGui::GetIO().MousePos;
    ImVec2 vp = ImGui::GetMainViewport()->Size;
    ImVec2 p(m.x + S(14), m.y + S(18));
    ImVec2 sz(ts.x + S(20), ts.y + S(14));
    if (p.x + sz.x > vp.x - S(8)) p.x = vp.x - S(8) - sz.x;
    if (p.y + sz.y > vp.y - S(8)) p.y = m.y - S(10) - sz.y;
    dl->AddRectFilled(p, ImVec2(p.x + sz.x, p.y + sz.y), Bg(0.96f), S(8));
    dl->AddRect(p, ImVec2(p.x + sz.x, p.y + sz.y), White(0.10f), S(8));
    Text(dl, g_theme.regular, 12, ImVec2(p.x + S(10), p.y + S(7)), White(0.9f), g_tipShown.c_str());
    ImGui::PopStyleVar();
}

}
