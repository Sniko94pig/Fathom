#include "launcher/home.h"
#include "launcher/art.h"
#include "launcher/games.h"
#include "ui/icons.h"
#include "ui/ui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
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

void Card(ImDrawList* dl, ImVec2 a, ImVec2 b) {
    dl->AddRectFilled(a, b, White(0.035f), S(12));
    dl->AddRect(a, b, White(0.05f), S(12));
}

void Caption(ImDrawList* dl, ImVec2 p, const char* t) { Text(dl, T().semibold, 10.5f, p, White(0.42f), t); }

void Step(ImDrawList* dl, ImVec2 p, float w, int n, const char* title, const char* body, bool done) {
    ImVec2 c(p.x + S(14), p.y + S(14));
    dl->AddCircleFilled(c, S(13), done ? Accent(0.18f) : White(0.06f), 24);
    char num[4];
    std::snprintf(num, sizeof(num), "%d", n);
    if (done) {
        Icon(dl, c, S(14), Accent(), ICON_CHECK);
    } else {
        ImVec2 ts = Measure(T().semibold, 12, num);
        Text(dl, T().semibold, 12, ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f), White(0.8f), num);
    }
    Text(dl, T().medium, 13.5f, ImVec2(p.x + S(38), p.y + S(2)), White(done ? 0.55f : 0.95f), title);
    TextFit(dl, T().regular, 12, ImVec2(p.x + S(38), p.y + S(20)), w - S(38), White(0.45f), body);
}

void Row(ImDrawList* dl, ImVec2 p, float w, const char* label, const std::string& value, bool good) {
    Text(dl, T().medium, 12.5f, p, White(0.62f), label);
    Icon(dl, ImVec2(p.x + w - S(8), p.y + S(8)), S(14), good ? Accent() : Danger(), good ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT);
    TextFit(dl, T().regular, 12, ImVec2(p.x + S(120), p.y + S(1)), w - S(150), White(0.8f), value.c_str());
}

void Badge(ImDrawList* dl, ImVec2 p, const char* text, bool accent) {
    ImVec2 ts = Measure(T().semibold, 10.5f, text);
    ImVec2 b(p.x + ts.x + S(16), p.y + S(20));
    dl->AddRectFilled(p, b, accent ? Accent(0.92f) : Black(0.62f), S(10));
    Text(dl, T().semibold, 10.5f, ImVec2(p.x + S(8), p.y + (S(20) - ts.y) * 0.5f), accent ? Black(0.9f) : White(0.88f), text);
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
    else DrawLibrary();
}

void Home::DrawLibrary() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    const auto& games = Games();
    int ready = 0;
    for (const GameDef& g : games) ready += g.ready ? 1 : 0;
    Input("gamesearch", &search, "Search games", o, S(280), ICON_SEARCH);
    char buf[96];
    std::snprintf(buf, sizeof(buf), "%d ready  \xc2\xb7  %d coming soon", ready, (int)games.size() - ready);
    TextRight(dl, T().regular, 12, ImVec2(o.x + w, o.y + S(11)), White(0.45f), buf);

    std::string q = Lower(search);
    std::vector<int> shown;
    for (int i = 0; i < (int)games.size(); ++i)
        if (q.empty() || Lower(games[i].name).find(q) != std::string::npos) shown.push_back(i);

    const int cols = 6;
    float gap = S(14), top = o.y + S(54);
    float cw = std::floor((w - gap * (cols - 1)) / cols), ph = std::floor(cw * 1.5f), ch = ph + S(62);
    for (int k = 0; k < (int)shown.size(); ++k) {
        int idx = shown[k];
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
        if (g.ready && hv > 0.01f) Glow(dl, pa, pb, S(10), T().accent, 0.16f * hv, S(16));
        float shade = g.ready ? 1.0f : 0.5f + 0.3f * hv;
        if (const Art* cap = Find(g.appId, art::Capsule)) Cover(dl, *cap, pa, pb, S(10), ImGui::GetColorU32(ImVec4(shade, shade, shade, 1)));
        else Placeholder(dl, pa, pb, g, S(10));
        dl->AddRect(pa, pb, g.ready ? Accent(0.35f + 0.5f * hv) : White(0.06f + 0.12f * hv), S(10), S(1.5f));
        Badge(dl, ImVec2(pa.x + S(8), pa.y + S(8)), g.ready ? "Ready" : "Soon", g.ready);
        Text(dl, T().medium, 12.5f, ImVec2(a.x + S(2), a.y + ph + S(8)), White(g.ready ? 0.95f : 0.7f), g.name, cw - S(4));
        bool inst = idx < (int)installed.size() && installed[idx];
        Text(dl, T().regular, 11, ImVec2(a.x + S(2), a.y + ph + S(44)), inst ? Accent(0.75f) : White(0.35f), inst ? "Installed" : "Not installed");
        if (clicked) game = idx;
    }
    if (shown.empty()) Text(dl, T().regular, 13, ImVec2(o.x, top + S(8)), White(0.45f), "No game matches your search");
    int rows = std::max(1, ((int)shown.size() + cols - 1) / cols);
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, top + rows * (ch + gap) - o.y));
}

void Home::DrawGame() {
    const GameDef& g = Games()[game];
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    float bh = std::min(std::floor(w * 0.32f), S(230));
    ImVec2 a = o, b(o.x + w, o.y + bh);
    if (const Art* hero = Find(g.appId, art::Hero)) Cover(dl, *hero, a, b, S(14), IM_COL32_WHITE);
    else if (const Art* cap = Find(g.appId, art::Capsule)) Cover(dl, *cap, a, b, S(14), ImGui::GetColorU32(ImVec4(0.55f, 0.55f, 0.55f, 1)));
    else Placeholder(dl, a, b, g, S(14));
    float fadeTop = a.y + bh * 0.25f, solid = b.y - S(28);
    dl->AddRectFilledMultiColor(ImVec2(a.x, fadeTop), ImVec2(b.x, solid), Black(0.0f), Black(0.0f), Black(0.86f), Black(0.86f));
    dl->AddRectFilled(ImVec2(a.x, solid), b, Black(0.86f), S(14), ImDrawFlags_RoundCornersBottom);
    dl->AddRect(a, b, White(0.07f), S(14));

    bool sn2 = std::strcmp(g.id, "subnautica2") == 0;
    bool running = state != inject::GameState::NotRunning;
    bool injected = state == inject::GameState::Injected;
    std::string line;
    if (!g.ready) line = "Coming soon. Fathom does not support this game yet.";
    else if (injected) line = "The menu is running. Press " + openKey + " in the game to open it, " + unloadKey + " to unload it.";
    else if (running) line = "The game is open. Press Inject to load the menu into it.";
    else line = "Start the game first, then press Inject.";
    Text(dl, T().semibold, 26, ImVec2(a.x + S(24), b.y - S(78)), White(0.98f), g.name);
    Text(dl, T().regular, 13, ImVec2(a.x + S(24), b.y - S(40)), White(0.78f), line.c_str(), w - S(260));

    ImVec2 bs(S(170), S(44));
    ImVec2 bp(b.x - S(24) - bs.x, b.y - S(24) - bs.y);
    if (!g.ready) {
        Button("main", "Coming soon", bp, bs, Btn::Ghost, ICON_LOCK, false);
    } else if (busy) {
        Button("main", "Injecting\xe2\x80\xa6", bp, bs, Btn::Primary, ICON_REFRESH_CW, false);
    } else if (injected) {
        Button("main", "Injected", bp, bs, Btn::Tint, ICON_CHECK, false);
    } else if (running) {
        if (Button("main", "Inject", bp, bs, Btn::Primary, ICON_ZAP, paths.found)) injectNow = true;
    } else {
        if (Button("main", "Start game", bp, bs, Btn::Primary, ICON_PLAY, true)) startNow = true;
    }

    float y = b.y + S(16);
    if (sn2 && g.ready) {
        DrawSubnauticaCards(y, w, o);
        return;
    }
    float ch = S(132);
    Card(dl, ImVec2(o.x, y), ImVec2(o.x + w, y + ch));
    Caption(dl, ImVec2(o.x + S(16), y + S(15)), "COMING SOON");
    Text(dl, T().regular, 13, ImVec2(o.x + S(16), y + S(40)), White(0.7f),
         "Fathom will get a menu for this game in a later update. Updates come through the installer.", w - S(32));
    bool inst = game < (int)installed.size() && installed[game];
    Row(dl, ImVec2(o.x + S(16), y + S(86)), w * 0.5f, "On this PC", inst ? "Installed with Steam" : "Not installed", inst);
    std::string store = "https://store.steampowered.com/app/" + std::to_string(g.appId);
    if (Button("store", "Steam page", ImVec2(o.x + w - S(16) - S(140), y + ch - S(50)), ImVec2(S(140), S(34)), Btn::Ghost, ICON_GLOBE, true)) {
        std::wstring url(store.begin(), store.end());
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y + ch - o.y));
}

void Home::DrawSubnauticaCards(float y, float w, ImVec2 o) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool running = state != inject::GameState::NotRunning;
    bool injected = state == inject::GameState::Injected;
    float colW = std::floor((w - S(16)) * 0.5f);
    float ch = S(220);
    ImVec2 c1(o.x, y), c2(o.x + colW + S(16), y);
    Card(dl, c1, ImVec2(c1.x + colW, y + ch));
    Caption(dl, ImVec2(c1.x + S(16), y + S(15)), "HOW IT WORKS");
    Step(dl, ImVec2(c1.x + S(16), y + S(44)), colW - S(32), 1, "Check the cheats", "Every page on the left is a part of the menu.", false);
    Step(dl, ImVec2(c1.x + S(16), y + S(98)), colW - S(32), 2, "Start the game", "Wait for the main menu or load your save.", running);
    std::string last = "Then press " + openKey + " in the game to open the menu.";
    Step(dl, ImVec2(c1.x + S(16), y + S(152)), colW - S(32), 3, "Press Inject", last.c_str(), injected);

    Card(dl, c2, ImVec2(c2.x + colW, y + ch));
    Caption(dl, ImVec2(c2.x + S(16), y + S(15)), "FILES");
    std::error_code ec;
    bool hasDll = paths.found && (std::filesystem::exists(paths.dll, ec) || std::filesystem::exists(paths.bundled, ec));
    Row(dl, ImVec2(c2.x + S(16), y + S(46)), colW - S(32), "Game folder", paths.found ? "Subnautica2 (Steam)" : "Not found", paths.found);
    if (paths.found && ImGui::IsMouseHoveringRect(ImVec2(c2.x, y + S(40)), ImVec2(c2.x + colW, y + S(66)))) Tip(Narrow(paths.win64).c_str());
    Row(dl, ImVec2(c2.x + S(16), y + S(74)), colW - S(32), "Menu file", hasDll ? "fathom.dll" : "Missing", hasDll);
    Row(dl, ImVec2(c2.x + S(16), y + S(102)), colW - S(32), "Settings", paths.found ? "settings.json" : "-", paths.found);
    float bw = std::floor((colW - S(40)) * 0.5f);
    if (Button("folder", "Open folder", ImVec2(c2.x + S(16), y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_FOLDER_OPEN, paths.found))
        ShellExecuteW(nullptr, L"open", paths.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    bool hasLog = paths.found && std::filesystem::exists(paths.log, ec);
    if (Button("log", "Open log", ImVec2(c2.x + S(24) + bw, y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_INFO, hasLog))
        ShellExecuteW(nullptr, L"open", paths.log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y + ch - o.y));
}

}
