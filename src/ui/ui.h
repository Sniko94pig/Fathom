#pragma once
#include "imgui.h"
#include <string>

namespace ui {

struct Theme {
    ImVec4 bg{0.047f, 0.071f, 0.094f, 1.0f};
    ImVec4 accent{0.247f, 0.847f, 0.816f, 1.0f};
    ImVec4 danger{0.965f, 0.447f, 0.478f, 1.0f};
    float scale = 1.0f;
    ImFont* regular = nullptr;
    ImFont* medium = nullptr;
    ImFont* semibold = nullptr;
    ImFont* icons = nullptr;
    ImFont* mono = nullptr;
};

enum class Btn { Ghost, Primary, Tint, Danger };

Theme& T();
void LoadFonts();
void LoadMonoFont();
void ApplyStyle();

float S(float v);
float Dt();
ImU32 Accent(float a = 1.0f);
ImU32 White(float a = 1.0f);
ImU32 Black(float a = 1.0f);
ImU32 Bg(float a = 1.0f);
ImU32 Danger(float a = 1.0f);
ImU32 Mix(const ImVec4& a, const ImVec4& b, float t, float alpha = 1.0f);

float Ease(ImGuiID id, float target, float speed = 16.0f, float init = 0.0f);
void SetAnim(ImGuiID id, float v);

ImVec2 Measure(ImFont* font, float size, const char* text);
void Text(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, ImU32 col, const char* text, float wrap = 0.0f);
void TextRight(ImDrawList* dl, ImFont* font, float size, ImVec2 rightTop, ImU32 col, const char* text);
void TextFit(ImDrawList* dl, ImFont* font, float size, ImVec2 pos, float maxW, ImU32 col, const char* text);
void Icon(ImDrawList* dl, ImVec2 center, float size, ImU32 col, const char* icon);
void Glow(ImDrawList* dl, ImVec2 a, ImVec2 b, float rounding, const ImVec4& col, float strength, float spread);
void Switch(ImDrawList* dl, ImVec2 pos, float t, float dim);
ImVec2 SwitchSize();

bool Button(const char* id, const char* label, ImVec2 pos, ImVec2 size, Btn style = Btn::Ghost, const char* icon = nullptr,
            bool enabled = true);
bool IconButton(const char* id, const char* icon, ImVec2 pos, float size, bool enabled = true, bool danger = false);
float ChipWidth(const char* label);
bool Chip(const char* id, const char* label, ImVec2 pos, bool selected, float width = 0.0f);
bool Segmented(const char* id, const char* const* labels, int count, int* index, ImVec2 pos, float width);
bool Input(const char* id, std::string* value, const char* hint, ImVec2 pos, float width, const char* icon = nullptr);
float BuildingWidth();
void Building(ImDrawList* dl, ImVec2 rightCenter);
void Tip(const char* text);
void DrawTip();

}
