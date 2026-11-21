#include "launcher/home.h"
#include "launcher/art.h"
#include "launcher/games.h"
#include "ui/icons.h"
#include "ui/ui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>

namespace launcher {

using namespace ui;

namespace {

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void Card(ImDrawList* dl, ImVec2 a, ImVec2 b, bool hot = false) {
    dl->AddRectFilled(a, b, White(0.035f));
    dl->AddRectFilled(a, ImVec2(b.x, a.y + S(2)), hot ? Accent() : White(0.09f));
}

void Caption(ImDrawList* dl, ImVec2 p, const char* t) { Title(dl, p, t); }

void Step(ImDrawList* dl, ImVec2 p, float w, int n, const char* title, const char* body, bool done) {
    ImVec2 a(p.x, p.y + S(1)), b(p.x + S(28), p.y + S(27));
    Para(dl, a, b, done ? Accent(0.18f) : White(0.06f));
    ParaLine(dl, a, b, done ? Accent(0.5f) : White(0.10f));
    ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    char num[4];
    std::snprintf(num, sizeof(num), "%d", n);
    if (done) {
        Icon(dl, c, S(14), Accent(), ICON_CHECK);
    } else {
        ImVec2 ts = Measure(T().semibold, 12, num);
        Text(dl, T().semibold, 12, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), White(0.9f), num);
    }
    Text(dl, T().medium, 13.5f, ImVec2(p.x + S(40), p.y + S(1)), White(done ? 0.55f : 0.95f), title);
    TextFit(dl, T().regular, 12, ImVec2(p.x + S(40), p.y + S(19)), w - S(40), White(0.45f), body);
}

void Row(ImDrawList* dl, ImVec2 p, float w, const char* label, const std::string& value, bool good) {
    Text(dl, T().medium, 12.5f, p, White(0.55f), label);
    Icon(dl, ImVec2(p.x + w - S(8), p.y + S(8)), S(14), good ? Good() : Danger(), good ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT);
    TextFit(dl, T().medium, 12.5f, ImVec2(p.x + S(120), p.y), w - S(150), White(0.9f), value.c_str());
    dl->AddLine(ImVec2(p.x, p.y + S(22)), ImVec2(p.x + w, p.y + S(22)), White(0.06f));
}

ImVec2 Badge(ImDrawList* dl, ImVec2 p, const GameDef& g) {
    const char* text = g.ready ? "Ready" : g.building ? "Being built" : "Soon";
    ImVec2 ts = MeasureUpper(T().semibold, 10, text);
    float h = S(20);
    ImVec2 b(p.x + ts.x + S(18), p.y + h);
    ImU32 bg = g.ready ? Accent() : g.building ? ImGui::GetColorU32(ImVec4(0.98f, 0.74f, 0.30f, 1.0f)) : Black(0.72f);
    Para(dl, p, b, bg);
    Upper(dl, T().semibold, 10, ImVec2(p.x + S(9), p.y + (h - ts.y) * 0.5f), g.building ? Black(0.9f) : White(0.95f), text);
    return b;
}

void Status(ImDrawList* dl, ImVec2 p, bool inst) {
    if (inst) {
        Diamond(dl, ImVec2(p.x + S(4), p.y + S(7)), S(3.5f), Accent());
        Text(dl, T().regular, 11.5f, ImVec2(p.x + S(13), p.y), White(0.75f), "Installed");
    } else {
        Text(dl, T().regular, 11.5f, p, White(0.38f), "Not installed");
    }
}

void Cover(ImDrawList* dl, const Art& art, ImVec2 a, ImVec2 b, float rounding, ImU32 tint) {
    float ar = art.w / std::max(1.0f, art.h), br = (b.x - a.x) / std::max(1.0f, b.y - a.y);
    ImVec2 uv0(0, 0), uv1(1, 1);
    if (ar > br) {
        float f = br / ar;
        uv0.x = (1 - f) * 0.5f;
        uv1.x = 1 - uv0.x;
    } else {
        float f = ar / br;
        uv0.y = (1 - f) * 0.5f;
        uv1.y = 1 - uv0.y;
    }
    dl->AddImageRounded(art.tex, a, b, uv0, uv1, tint, rounding);
}

void Placeholder(ImDrawList* dl, ImVec2 a, ImVec2 b, const GameDef& g, float rounding) {
    dl->AddRectFilled(a, b, g.ready ? Accent(0.10f) : White(0.045f), rounding);
    if (rounding <= 0.0f) dl->AddRect(a, b, White(0.06f));
    float size = std::strlen(g.tag) > 3 ? 18.0f : 24.0f;
    ImVec2 ts = Measure(T().semibold, size, g.tag);
    Text(dl, T().semibold, size, ImVec2((a.x + b.x - ts.x) * 0.5f, (a.y + b.y - ts.y) * 0.5f), g.ready ? Accent(0.9f) : White(0.4f), g.tag);
}

}

long long ArtKey(int appId, int kind) { return (long long)appId * 4 + kind; }

const Art* Home::Find(int appId, int kind) const {
    auto it = art.find(ArtKey(appId, kind));
    return it != art.end() && it->second.tex ? &it->second : nullptr;
}

void Home::Draw() {
    if (game >= 0 && game < (int)Games().size()) DrawGame();
    else if (maps) DrawMaps();
    else DrawLibrary();
}

void Home::DrawMaps() {
    mapsWanted = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, avail = ImGui::GetContentRegionAvail().y;
    static const char* names[3] = {"Subnautica", "Subnautica 2", "Sons of the Forest"};
    float x = o.x;
    for (int g = 0; g < 3; ++g) {
        std::string id = "mapgame" + std::to_string(g);
        if (Chip(id.c_str(), names[g], ImVec2(x, o.y), mapGame == g)) mapGame = g;
        x += ChipWidth(names[g]) + S(8);
    }
    MapPic& pic = mapPics[mapGame];
    const std::string& status = mapStatus[mapGame];
    float bw = S(150);
    if (Button("mapredo", mapGame == 0 ? "Draw again" : "Refresh", ImVec2(o.x + w - bw, o.y - S(2)), ImVec2(bw, S(34)), Btn::Ghost, ICON_REFRESH_CW,
               !mapsBusy)) {
        mapsRedraw = mapGame == 0;
        mapsReload = true;
    }
    float top = o.y + S(48), h = std::max(S(280), avail - S(48) - S(40));
    ImVec2 a(o.x, top), b(o.x + w, top + h), c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.027f, 0.063f, 0.122f, 1.0f)));
    if (pic.tex && pic.w > 0 && pic.h > 0) {
        float fit = std::min(w / pic.w, h / pic.h);
        float& zoom = mapZoom[mapGame];
        float& cu = mapCx[mapGame];
        float& cv = mapCy[mapGame];
        if (zoom <= 0) zoom = fit;
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetCursorScreenPos(a);
        ImGui::InvisibleButton("mapview", ImVec2(w, h));
        bool hov = ImGui::IsItemHovered();
        if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0, 0.0f)) {
            cu -= io.MouseDelta.x / (zoom * pic.w);
            cv -= io.MouseDelta.y / (zoom * pic.h);
        }
        if (hov && io.MouseWheel != 0.0f) {
            float mu = cu + (io.MousePos.x - c.x) / (zoom * pic.w), mv = cv + (io.MousePos.y - c.y) / (zoom * pic.h);
            zoom = std::clamp(zoom * std::pow(1.2f, io.MouseWheel), fit * 0.8f, fit * 14.0f);
            cu = mu - (io.MousePos.x - c.x) / (zoom * pic.w);
            cv = mv - (io.MousePos.y - c.y) / (zoom * pic.h);
        }
        if (hov && ImGui::IsMouseDoubleClicked(0)) {
            zoom = fit;
            cu = cv = 0.5f;
        }
        cu = std::clamp(cu, 0.0f, 1.0f);
        cv = std::clamp(cv, 0.0f, 1.0f);
        auto toScreen = [&](float u, float v) { return ImVec2(c.x + (u - cu) * pic.w * zoom, c.y + (v - cv) * pic.h * zoom); };
        dl->PushClipRect(a, b, true);
        dl->AddImage(pic.tex, toScreen(0, 0), toScreen(1, 1));
        bool close = zoom >= fit * 2.2f;
        const MapLabel* hot = nullptr;
        bool hotNamed = false;
        std::vector<ImVec4> used;
        auto room = [&](ImVec2 p0, ImVec2 p1) {
            for (const ImVec4& r : used)
                if (p0.x < r.z && p1.x > r.x && p0.y < r.w && p1.y > r.y) return false;
            used.push_back(ImVec4(p0.x, p0.y, p1.x, p1.y));
            return true;
        };
        auto inView = [&](ImVec2 p) { return p.x > a.x - S(80) && p.x < b.x + S(80) && p.y > a.y - S(20) && p.y < b.y + S(20); };
        float pad = S(6);
        std::vector<std::pair<const std::string*, ImVec2>> named;
        for (const MapLabel& l : pic.labels) {
            ImVec2 p = toScreen(l.u, l.v);
            if (!l.area || !inView(p)) continue;
            bool twice = false;
            for (const auto& n : named)
                if (*n.first == l.name && std::hypot(n.second.x - p.x, n.second.y - p.y) < S(240)) twice = true;
            if (twice) continue;
            ImVec2 ts = MeasureUpper(T().semibold, 11, l.name.c_str());
            float reach = l.reach * pic.w * zoom;
            static const ImVec2 moves[] = {{0, 0}, {0, -1}, {0, 1}, {-0.5f, 0}, {0.5f, 0}, {-0.5f, -1}, {0.5f, -1}, {-0.5f, 1}, {0.5f, 1}};
            for (const ImVec2& m : moves) {
                ImVec2 shift(m.x * (ts.x * 0.5f + pad), m.y * (ts.y + pad));
                if (std::hypot(shift.x, shift.y) > reach && (m.x != 0 || m.y != 0)) continue;
                ImVec2 tp(std::floor(p.x + shift.x - ts.x * 0.5f), std::floor(p.y + shift.y - ts.y * 0.5f));
                if (!room(ImVec2(tp.x - pad, tp.y - pad * 0.5f), ImVec2(tp.x + ts.x + pad, tp.y + ts.y + pad * 0.5f))) continue;
                Upper(dl, T().semibold, 11, ImVec2(tp.x + 1, tp.y + 1), Black(0.85f), l.name.c_str());
                Upper(dl, T().semibold, 11, tp, White(0.95f), l.name.c_str());
                named.push_back({&l.name, p});
                break;
            }
        }
        for (const MapLabel& l : pic.labels) {
            ImVec2 p = toScreen(l.u, l.v);
            if (l.area || !inView(p)) continue;
            Diamond(dl, p, S(4.5f), Accent());
            bool shown = false;
            if (close) {
                ImVec2 ts = Measure(T().medium, 11.5f, l.name.c_str());
                ImVec2 tp(std::floor(p.x + S(8)), std::floor(p.y - S(8)));
                shown = room(tp, ImVec2(tp.x + ts.x + pad, tp.y + ts.y));
                if (shown) {
                    Text(dl, T().medium, 11.5f, ImVec2(tp.x + 1, tp.y + 1), Black(0.85f), l.name.c_str());
                    Text(dl, T().medium, 11.5f, tp, White(0.92f), l.name.c_str());
                }
            }
            if (hov && std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y) < S(10)) {
                hot = &l;
                hotNamed = shown;
            }
        }
        dl->PopClipRect();
        if (hot && !hotNamed) Tip(hot->name.c_str());
    } else {
        std::string msg = !status.empty() ? status + "\xe2\x80\xa6" : pic.loaded ? pic.note : "Loading the map\xe2\x80\xa6";
        float tw = std::min(w - S(64), S(520));
        ImGui::PushFont(T().regular, S(13));
        ImVec2 ts = ImGui::CalcTextSize(msg.c_str(), nullptr, false, tw);
        ImGui::PopFont();
        Text(dl, T().regular, 13, ImVec2(std::floor(c.x - ts.x * 0.5f), std::floor(c.y - ts.y * 0.5f)), White(0.6f), msg.c_str(), tw);
    }
    dl->AddRect(a, b, White(0.07f));
    std::string foot = !status.empty() ? status + "\xe2\x80\xa6" : pic.tex ? pic.note + "  Scroll to zoom, drag to move, double-click to see it all." : std::string();
    TextFit(dl, T().regular, 12, ImVec2(o.x, b.y + S(12)), w, White(0.5f), foot.c_str());
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, b.y + S(34) - o.y));
}

void Home::DrawLibrary() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    const auto& games = Games();
    int ready = 0, building = 0;
    for (const GameDef& g : games) {
        ready += g.ready ? 1 : 0;
        building += g.building ? 1 : 0;
    }
    int owned = 0;
    for (int i = 0; i < (int)games.size(); ++i) owned += games[i].ready && i < (int)installed.size() && installed[i] ? 1 : 0;
    Input(mine ? "minesearch" : "gamesearch", &search, mine ? "Search your games" : "Search games", o, S(280), ICON_SEARCH);
    char buf[96];
    if (mine)
        std::snprintf(buf, sizeof(buf), owned == 1 ? "%d game with mods on this PC" : "%d games with mods on this PC", owned);
    else if (building)
        std::snprintf(buf, sizeof(buf), "%d ready  \xc2\xb7  %d being built  \xc2\xb7  %d coming soon", ready, building,
                      (int)games.size() - ready - building);
    else
        std::snprintf(buf, sizeof(buf), "%d ready  \xc2\xb7  %d coming soon", ready, (int)games.size() - ready);
    TextRight(dl, T().regular, 12, ImVec2(o.x + w, o.y + S(11)), White(0.45f), buf);

    std::string q = Lower(search);
    std::vector<int> shown;
    for (int i = 0; i < (int)games.size(); ++i)
        if ((showSoon || games[i].ready || games[i].building) && (q.empty() || Lower(games[i].name).find(q) != std::string::npos)) shown.push_back(i);
    if (installedFirst)
        std::stable_partition(shown.begin(), shown.end(), [&](int i) { return i < (int)installed.size() && installed[i]; });

    std::vector<int> feat, rest;
    for (int i : shown) (games[i].ready ? feat : rest).push_back(i);
    float gap = S(16), top = o.y + S(54);
    if (mine) {
        std::vector<int> own;
        for (int i : feat)
            if (i < (int)installed.size() && installed[i]) own.push_back(i);
        feat.swap(own);
        rest.clear();
        if (feat.empty()) {
            float eh = S(110);
            Card(dl, ImVec2(o.x, top), ImVec2(o.x + w, top + eh));
            Caption(dl, ImVec2(o.x + S(16), top + S(15)), q.empty() ? "No games yet" : "Nothing found");
            Text(dl, T().regular, 13, ImVec2(o.x + S(16), top + S(40)), White(0.7f),
                 q.empty() ? "A game shows up here when Fathom has mods for it and it is installed with Steam on this PC."
                           : "None of your games match the search.",
                 w - S(32));
            ImGui::SetCursorScreenPos(o);
            ImGui::Dummy(ImVec2(w, top + eh - o.y));
            return;
        }
    }

    if (!feat.empty()) {
        int n = (int)feat.size();
        int fc = std::min(n, 2);
        bool carousel = !mine && n > fc;
        int rows = carousel ? 1 : (n + fc - 1) / fc;
        float arrow = S(44);
        float areaW = carousel ? w - arrow - gap : w;
        float fw = std::floor((areaW - gap * (fc - 1)) / fc), fh = S(150), step = fw + gap;
        float off = carousel ? Ease(ImGui::GetID("featoff"), (float)featStart, 10.0f, (float)featStart) : 0.0f;
        if (carousel) dl->PushClipRect(ImVec2(o.x, top - S(2)), ImVec2(o.x + areaW, top + fh + S(2)), true);
        for (int k = 0; k < n; ++k) {
            float p = (float)(k % fc), row = carousel ? 0.0f : (float)(k / fc);
            if (carousel) {
                p = std::fmod((float)k - off, (float)n);
                if (p < -1.0f) p += (float)n;
                if (p >= (float)(n - 1)) p -= (float)n;
                if (p <= -1.0f || p >= (float)fc) continue;
            }
            int idx = feat[k];
            const GameDef& g = games[idx];
            ImVec2 a(std::floor(o.x + p * step), top + row * (fh + gap)), b(a.x + fw, a.y + fh);
            bool whole = p > -0.02f && p < (float)fc - 0.98f;
            bool clicked = false, hovered = false;
            ImGui::PushID(idx);
            if (whole) {
                ImGui::SetCursorScreenPos(a);
                clicked = ImGui::InvisibleButton("feat", ImVec2(fw, fh));
                hovered = ImGui::IsItemHovered();
            }
            float hv = Ease(ImGui::GetID("hover"), hovered ? 1.0f : 0.0f, 18.0f);
            ImGui::PopID();
            float shade = 0.8f + 0.2f * hv;
            ImU32 tint = ImGui::GetColorU32(ImVec4(shade, shade, shade, 1));
            if (const Art* hero = Find(g.appId, art::Hero)) Cover(dl, *hero, a, b, 0.0f, tint);
            else if (const Art* cap = Find(g.appId, art::Capsule)) Cover(dl, *cap, a, b, 0.0f, tint);
            else Placeholder(dl, a, b, g, 0.0f);
            float mid = a.x + fw * 0.6f;
            dl->AddRectFilledMultiColor(a, ImVec2(mid, b.y), Bg(0.97f), Bg(0.55f), Bg(0.55f), Bg(0.97f));
            dl->AddRectFilledMultiColor(ImVec2(mid, a.y), b, Bg(0.55f), Bg(0.05f), Bg(0.05f), Bg(0.55f));
            dl->AddRect(a, b, hv > 0.01f ? Mix(ImVec4(1, 1, 1, 0.07f), T().accent, hv, 0.9f) : White(0.07f));
            dl->AddRectFilled(a, ImVec2(a.x + S(4), b.y), Accent());
            float x = a.x + S(22);
            float size = 24.0f;
            ImVec2 ts = MeasureUpper(T().semibold, size, g.name);
            while (size > 15.0f && ts.x > fw - S(44)) {
                size -= 1.0f;
                ts = MeasureUpper(T().semibold, size, g.name);
            }
            float ty = b.y - S(18) - S(16) - S(8) - ts.y;
            Badge(dl, ImVec2(x, ty - S(28)), g);
            Upper(dl, T().semibold, size, ImVec2(x, ty), White(0.98f), g.name);
            bool inst = idx < (int)installed.size() && installed[idx];
            Text(dl, T().regular, 12, ImVec2(x, b.y - S(18) - S(15)), White(0.6f), inst ? "Installed on this PC" : "Not installed");
            if (clicked) game = idx;
        }
        if (carousel) dl->PopClipRect();
        if (carousel && IconButton("featnext", ICON_CHEVRON_RIGHT, ImVec2(o.x + w - arrow, top + std::floor((fh - arrow) * 0.5f)), arrow))
            ++featStart;
        top += rows * (fh + gap) + S(6);
        if (!rest.empty()) {
            ImVec2 ls = MeasureUpper(T().semibold, 11.5f, "All games");
            Upper(dl, T().semibold, 11.5f, ImVec2(o.x, top), White(0.92f), "All games");
            dl->AddRectFilled(ImVec2(o.x, top + ls.y + S(5)), ImVec2(o.x + ls.x, top + ls.y + S(7)), Accent());
            top += ls.y + S(22);
        }
    }

    int cols = std::clamp((int)((w + gap) / (S(118) + gap)), 3, 14);
    float cw = std::floor((w - gap * (cols - 1)) / cols), ph = std::floor(cw * 1.5f), ch = ph + S(46);
    for (int k = 0; k < (int)rest.size(); ++k) {
        int idx = rest[k];
        const GameDef& g = games[idx];
        ImVec2 a(o.x + (k % cols) * (cw + gap), top + (k / cols) * (ch + gap));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(idx);
        bool clicked = ImGui::InvisibleButton("game", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered();
        float hv = Ease(ImGui::GetID("hover"), hov ? 1.0f : 0.0f, 18.0f);
        ImGui::PopID();
        float lift = std::floor(hv * S(3));
        ImVec2 pa(a.x, a.y - lift), pb(a.x + cw, a.y + ph - lift);
        float shade = g.ready || g.building ? 1.0f : 0.5f + 0.35f * hv;
        if (const Art* cap = Find(g.appId, art::Capsule)) Cover(dl, *cap, pa, pb, 0.0f, ImGui::GetColorU32(ImVec4(shade, shade, shade, 1)));
        else Placeholder(dl, pa, pb, g, 0.0f);
        dl->AddRect(pa, pb, hv > 0.01f ? Mix(ImVec4(1, 1, 1, 0.06f), T().accent, hv) : White(0.06f));
        if (g.ready || hv > 0.01f) dl->AddRectFilled(ImVec2(pa.x, pb.y - S(4)), pb, Accent(g.ready ? 1.0f : hv));
        Badge(dl, ImVec2(pa.x + S(8), pa.y + S(8)), g);
        TextFit(dl, T().semibold, 13, ImVec2(a.x, a.y + ph + S(9)), cw, White(g.ready || g.building ? 0.96f : 0.8f), g.name);
        bool inst = idx < (int)installed.size() && installed[idx];
        Status(dl, ImVec2(a.x, a.y + ph + S(28)), inst);
        if (clicked) game = idx;
    }
    if (shown.empty()) Text(dl, T().regular, 13, ImVec2(o.x, top + S(8)), White(0.45f), "No game matches your search");
    int rows = rest.empty() ? 0 : ((int)rest.size() + cols - 1) / cols;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, std::max(top + rows * (ch + gap), top + S(40)) - o.y));
}

void Home::DrawGame() {
    const GameDef& g = Games()[game];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float bh = std::min(std::floor(w * 0.28f), S(300));
    ImVec2 a = o, b(o.x + w, o.y + bh);
    if (const Art* hero = Find(g.appId, art::Hero)) Cover(dl, *hero, a, b, 0.0f, IM_COL32_WHITE);
    else if (const Art* cap = Find(g.appId, art::Capsule)) Cover(dl, *cap, a, b, 0.0f, ImGui::GetColorU32(ImVec4(0.55f, 0.55f, 0.55f, 1)));
    else Placeholder(dl, a, b, g, 0.0f);
    float mid = a.x + w * 0.55f;
    dl->AddRectFilledMultiColor(a, ImVec2(mid, b.y), Bg(0.96f), Bg(0.6f), Bg(0.6f), Bg(0.96f));
    dl->AddRectFilledMultiColor(ImVec2(mid, a.y), b, Bg(0.6f), Bg(0.1f), Bg(0.1f), Bg(0.6f));
    dl->AddRectFilledMultiColor(ImVec2(a.x, b.y - bh * 0.35f), b, Bg(0.0f), Bg(0.0f), Bg(0.7f), Bg(0.7f));
    dl->AddRect(a, b, White(0.07f));
    dl->AddRectFilled(a, ImVec2(a.x + S(4), b.y), Accent());

    bool sn2 = std::strcmp(g.id, "subnautica2") == 0;
    bool sn1 = std::strcmp(g.id, "subnautica") == 0;
    bool sotf = std::strcmp(g.id, "sonsoftheforest") == 0;
    bool mc = std::strcmp(g.id, "minecraft") == 0;
    const inject::Paths& gp = sn1 ? sn1Paths : sotf ? sotfPaths : paths;
    inject::GameState gs = sn1 ? sn1State : sotf ? sotfState : state;
    bool running = gs != inject::GameState::NotRunning;
    bool injected = gs == inject::GameState::Injected;
    std::string line;
    if (mc)
        line = mcBusy ? "Setting up the Fathom install in the Minecraft Launcher."
               : mcLinked && !mcWorld ? "Connected. Join a world, then press " + openKey + " in the game for the Fathom menu."
               : mcLinked && mcMenu ? "Connected. Press " + openKey + " in the game for the Fathom menu, or use the pages on the left."
               : mcLinked ? "Connected. If the menu does not open, set Graphics API to Prefer OpenGL in Video Settings."
               : mcOpen ? "Minecraft is open without Fathom. Close it, press Start game and play the Fathom install."
               : mcNeedFabric ? "Install Fabric first, then press Start game."
                              : "Press Start game. Fathom makes its own install in the Minecraft Launcher.";
    else if (g.building) line = "The menu for this game is being built. Look at the modules on the left.";
    else if (!g.ready) line = "Coming soon. Fathom does not support this game yet.";
    else if (injected) line = "The menu is running. Press " + openKey + " in the game to open it, " + unloadKey + " to unload it.";
    else if (running) line = "The game is open. Press Inject to load the menu into it.";
    else line = "Start the game first, then press Inject.";
    ImVec2 ns = MeasureUpper(T().semibold, 32, g.name);
    Upper(dl, T().semibold, 32, ImVec2(a.x + S(28), b.y - S(50) - ns.y), White(0.98f), g.name);
    Text(dl, T().regular, 13, ImVec2(a.x + S(28), b.y - S(40)), White(0.8f), line.c_str(), w - S(280));

    ImVec2 bs(S(196), S(50));
    ImVec2 bp(b.x - S(28) - bs.x, b.y - S(28) - bs.y);
    if (mc) {
        if (mcBusy) Button("main", mcStep.c_str(), bp, bs, Btn::Primary, ICON_REFRESH_CW, false);
        else if (mcLinked) Button("main", "Connected", bp, bs, Btn::Tint, ICON_CHECK, false);
        else if (mcOpen) Button("main", "Running", bp, bs, Btn::Tint, ICON_PLAY, false);
        else if (Button("main", "Start game", bp, bs, Btn::Primary, ICON_PLAY, true)) startNow = true;
    } else if (g.building) {
        if (Button("main", "Start game", bp, bs, Btn::Primary, ICON_PLAY, true)) StartGame(g);
    } else if (!g.ready) {
        Button("main", "Coming soon", bp, bs, Btn::Ghost, ICON_LOCK, false);
    } else if (busy) {
        Button("main", "Injecting\xe2\x80\xa6", bp, bs, Btn::Primary, ICON_REFRESH_CW, false);
    } else if (injected) {
        Button("main", "Injected", bp, bs, Btn::Tint, ICON_CHECK, false);
    } else if (running) {
        if (Button("main", "Inject", bp, bs, Btn::Primary, ICON_ZAP, gp.found)) {
            injectNow = true;
            injectGame = sn1 ? 1 : sotf ? 2 : 0;
        }
    } else {
        if (Button("main", "Start game", bp, bs, Btn::Primary, ICON_PLAY, true)) startNow = true;
    }

    float y = b.y + S(16);
    if (mc) {
        DrawMinecraftCards(y, w, o);
        return;
    }
    if ((sn2 || sn1 || sotf) && g.ready) {
        DrawGameCards(y, w, o, gp, gs, sn1 ? "Subnautica (Steam)" : sotf ? "Sons Of The Forest (Steam)" : "Subnautica2 (Steam)",
                      sn1 ? "fathom_sn1.dll" : sotf ? "fathom_sotf.dll" : "fathom.dll");
        return;
    }
    float ch = S(132);
    Card(dl, ImVec2(o.x, y), ImVec2(o.x + w, y + ch), g.building);
    Caption(dl, ImVec2(o.x + S(16), y + S(15)), g.building ? "Being built" : "Coming soon");
    Text(dl, T().regular, 13, ImVec2(o.x + S(16), y + S(40)), White(0.7f),
         g.building ? "Every page on the left shows the modules that are coming. They do not work in the game yet, so there is no Inject button."
                    : "Fathom will get a menu for this game in a later update. Updates come through the installer.",
         w - S(32));
    bool inst = game < (int)installed.size() && installed[game];
    Row(dl, ImVec2(o.x + S(16), y + S(86)), w - S(32) - S(140) - S(24), "On this PC",
        inst ? (game < (int)installedWith.size() && installedWith[game] ? installedWith[game] : "Installed on this PC") : "Not installed", inst);
    std::string store = g.appId > 0 ? "https://store.steampowered.com/app/" + std::to_string(g.appId) : g.site ? g.site : "";
    if (!store.empty() &&
        Button("store", g.appId > 0 ? "Steam page" : "Website", ImVec2(o.x + w - S(16) - S(140), y + ch - S(50)), ImVec2(S(140), S(34)), Btn::Ghost, ICON_GLOBE, true)) {
        std::wstring url(store.begin(), store.end());
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y + ch - o.y));
}

void Home::DrawMinecraftCards(float y, float w, ImVec2 o) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float colW = std::floor((w - S(16)) * 0.5f);
    float ch = S(220);
    ImVec2 c1(o.x, y), c2(o.x + colW + S(16), y);
    Card(dl, c1, ImVec2(c1.x + colW, y + ch), true);
    Caption(dl, ImVec2(c1.x + S(16), y + S(15)), "How it works");
    Step(dl, ImVec2(c1.x + S(16), y + S(44)), colW - S(32), 1, "Press Start game", "Fathom makes a Fathom install with Fabric API.", mcReady);
    Step(dl, ImVec2(c1.x + S(16), y + S(98)), colW - S(32), 2, "Play Fathom", "Pick Fathom in the Minecraft Launcher.", mcLinked);
    std::string last = "Press " + openKey + " in a world";
    Step(dl, ImVec2(c1.x + S(16), y + S(152)), colW - S(32), 3, last.c_str(), "The Fathom menu opens in the game.", mcLinked && mcWorld && mcMenu);

    Card(dl, c2, ImVec2(c2.x + colW, y + ch));
    Caption(dl, ImVec2(c2.x + S(16), y + S(15)), "Fathom install");
    std::string api = mcApi;
    if (api.rfind("fabric-api-", 0) == 0) api = api.substr(11);
    if (api.size() > 4 && api.compare(api.size() - 4, 4, ".jar") == 0) api.resize(api.size() - 4);
    Row(dl, ImVec2(c2.x + S(16), y + S(46)), colW - S(32), "Minecraft", mcGame.empty() ? (mcNeedFabric ? "Needs Fabric" : "-") : mcGame, !mcGame.empty());
    Row(dl, ImVec2(c2.x + S(16), y + S(74)), colW - S(32), "Fabric API", api.empty() ? "Added on Start game" : api, !api.empty());
    Row(dl, ImVec2(c2.x + S(16), y + S(102)), colW - S(32), "Fathom mod", mcReady ? "Ready" : "Added on Start game", mcReady);
    if (!mcMods.empty() && ImGui::IsMouseHoveringRect(ImVec2(c2.x, y + S(40)), ImVec2(c2.x + colW, y + S(124)))) Tip(Narrow(mcMods).c_str());
    float bw = std::floor((colW - S(40)) * 0.5f);
    std::error_code ec;
    if (mcNeedFabric) {
        if (Button("mcfabric", "Get Fabric", ImVec2(c2.x + S(16), y + ch - S(52)), ImVec2(bw, S(34)), Btn::Tint, ICON_DOWNLOAD, true))
            ShellExecuteW(nullptr, L"open", L"https://fabricmc.net/use/installer/", nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        bool hasMods = !mcMods.empty() && std::filesystem::is_directory(mcMods, ec);
        if (Button("mcmods", "Open mods folder", ImVec2(c2.x + S(16), y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_FOLDER_OPEN, hasMods))
            ShellExecuteW(nullptr, L"open", mcMods.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    bool hasLog = !mcLog.empty() && std::filesystem::exists(mcLog, ec);
    if (Button("mclog", "Open log", ImVec2(c2.x + S(24) + bw, y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_INFO, hasLog)) {
        logPath = mcLog;
        logTitle = "Minecraft";
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y + ch - o.y));
}

void Home::DrawGameCards(float y, float w, ImVec2 o, const inject::Paths& paths, inject::GameState state, const char* folderName, const char* dllName) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool running = state != inject::GameState::NotRunning;
    bool injected = state == inject::GameState::Injected;
    float colW = std::floor((w - S(16)) * 0.5f);
    float ch = S(220);
    ImVec2 c1(o.x, y), c2(o.x + colW + S(16), y);
    Card(dl, c1, ImVec2(c1.x + colW, y + ch), true);
    Caption(dl, ImVec2(c1.x + S(16), y + S(15)), "How it works");
    Step(dl, ImVec2(c1.x + S(16), y + S(44)), colW - S(32), 1, "Check the cheats", "Every page on the left is a part of the menu.", false);
    Step(dl, ImVec2(c1.x + S(16), y + S(98)), colW - S(32), 2, "Start the game", "Wait for the main menu or load your save.", running);
    std::string last = "Then press " + openKey + " in the game to open the menu.";
    Step(dl, ImVec2(c1.x + S(16), y + S(152)), colW - S(32), 3, "Press Inject", last.c_str(), injected);

    Card(dl, c2, ImVec2(c2.x + colW, y + ch));
    Caption(dl, ImVec2(c2.x + S(16), y + S(15)), "Files");
    std::error_code ec;
    bool hasDll = paths.found && (std::filesystem::exists(paths.dll, ec) || std::filesystem::exists(paths.bundled, ec));
    Row(dl, ImVec2(c2.x + S(16), y + S(46)), colW - S(32), "Game folder", paths.found ? folderName : "Not found", paths.found);
    if (paths.found && ImGui::IsMouseHoveringRect(ImVec2(c2.x, y + S(40)), ImVec2(c2.x + colW, y + S(66)))) Tip(Narrow(paths.win64).c_str());
    Row(dl, ImVec2(c2.x + S(16), y + S(74)), colW - S(32), "Menu file", hasDll ? dllName : "Missing", hasDll);
    Row(dl, ImVec2(c2.x + S(16), y + S(102)), colW - S(32), "Settings", paths.found ? "settings.json" : "-", paths.found);
    float bw = std::floor((colW - S(40)) * 0.5f);
    if (Button("folder", "Open folder", ImVec2(c2.x + S(16), y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_FOLDER_OPEN, paths.found))
        ShellExecuteW(nullptr, L"open", paths.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    bool hasLog = paths.found && std::filesystem::exists(paths.log, ec);
    if (Button("log", "Open log", ImVec2(c2.x + S(24) + bw, y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_INFO, hasLog))
    {
        logPath = paths.log;
        logTitle = Games()[game].name;
    }

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y + ch - o.y));
}

}
