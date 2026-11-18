#include "launcher/home.h"
#include "ui/icons.h"
#include "ui/ui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
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

struct GameInfo {
    const char* name;
    const char* tag;
    bool ready;
};

const GameInfo kGames[] = {
    {"Subnautica 2", "SN2", true},          {"Borderlands", "BL", false},           {"Borderlands 2", "BL2", false},
    {"Borderlands 3", "BL3", false},        {"Borderlands 4", "BL4", false},        {"Cyberpunk 2077", "CP", false},
    {"Fallout 4", "FO4", false},            {"Fallout: New Vegas", "FNV", false},   {"Outlast", "OL", false},
    {"Satisfactory", "SAT", false},         {"Sons of the Forest", "SOF", false},   {"Subnautica", "SN", false},
    {"Subnautica: Below Zero", "BZ", false}, {"The Forest", "TF", false},           {"The Last of Us Part I", "TLOU", false},
    {"The Last of Us Part II", "TLOU2", false},
};

void Row(ImDrawList* dl, ImVec2 p, float w, const char* label, const std::string& value, bool good) {
    Text(dl, T().medium, 12.5f, p, White(0.62f), label);
    Icon(dl, ImVec2(p.x + w - S(8), p.y + S(8)), S(14), good ? Accent() : Danger(), good ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT);
    TextFit(dl, T().regular, 12, ImVec2(p.x + S(120), p.y + S(1)), w - S(150), White(0.8f), value.c_str());
}

}

void Home::Draw() {
    if (game == 0) DrawSubnautica();
    else DrawLibrary();
}

void Home::DrawLibrary() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    Text(dl, T().semibold, 18, o, White(0.97f), "Pick a game");
    Text(dl, T().regular, 12.5f, ImVec2(o.x, o.y + S(28)), White(0.5f), "Subnautica 2 is ready. The other games are coming soon.");
    const int cols = 3;
    const int count = (int)(sizeof(kGames) / sizeof(kGames[0]));
    float gap = S(10), top = o.y + S(58);
    float tw = std::floor((w - gap * (cols - 1)) / cols), th = S(64);
    for (int i = 0; i < count; ++i) {
        const GameInfo& g = kGames[i];
        ImVec2 a(o.x + (i % cols) * (tw + gap), top + (i / cols) * (th + gap)), b(a.x + tw, a.y + th);
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("game", ImVec2(tw, th));
        bool hov = ImGui::IsItemHovered();
        float hv = Ease(ImGui::GetID("hover"), hov && g.ready ? 1.0f : 0.0f, 18.0f);
        ImGui::PopID();
        dl->AddRectFilled(a, b, White(g.ready ? 0.045f + 0.03f * hv : 0.022f), S(12));
        dl->AddRect(a, b, g.ready ? Accent(0.30f + 0.35f * hv) : White(0.045f), S(12));
        if (g.ready && hv > 0.01f) Glow(dl, a, b, S(12), T().accent, 0.10f * hv, S(14));
        ImVec2 ba(a.x + S(12), a.y + (th - S(40)) * 0.5f), bb(ba.x + S(40), ba.y + S(40));
        dl->AddRectFilled(ba, bb, g.ready ? Accent(0.16f) : White(0.05f), S(10));
        float tagSize = std::strlen(g.tag) > 3 ? 10.0f : 12.5f;
        ImVec2 ts = Measure(T().semibold, tagSize, g.tag);
        Text(dl, T().semibold, tagSize, ImVec2((ba.x + bb.x - ts.x) * 0.5f, (ba.y + bb.y - ts.y) * 0.5f), g.ready ? Accent() : White(0.45f), g.tag);
        float nx = bb.x + S(12), nw = b.x - nx - S(10);
        TextFit(dl, T().medium, 13.5f, ImVec2(nx, a.y + S(14)), nw, White(g.ready ? 0.95f : 0.62f), g.name);
        if (g.ready) {
            dl->AddCircleFilled(ImVec2(nx + S(4), a.y + S(42)), S(3.5f), Accent(), 12);
            Text(dl, T().medium, 11.5f, ImVec2(nx + S(13), a.y + S(35)), Accent(0.9f), "Ready");
        } else {
            Text(dl, T().regular, 11.5f, ImVec2(nx, a.y + S(35)), White(0.38f), "Coming soon");
        }
        if (clicked && g.ready) game = i;
        if (hov && !g.ready) Tip("Fathom does not support this game yet");
    }
    int rows = (count + cols - 1) / cols;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, top + rows * (th + gap) - o.y));
}

void Home::DrawSubnautica() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 start = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    if (Button("back", "All games", start, ImVec2(S(128), S(34)), Btn::Ghost, ICON_UNDO_2, true)) game = -1;
    Text(dl, T().semibold, 15, ImVec2(start.x + S(144), start.y + S(8)), White(0.9f), "Subnautica 2");
    ImVec2 o(start.x, start.y + S(48));
    bool running = state != inject::GameState::NotRunning;
    bool injected = state == inject::GameState::Injected;

    float hh = S(150);
    ImVec2 a = o, b(o.x + w, o.y + hh);
    Card(dl, a, b);
    if (injected) Glow(dl, a, b, S(12), T().accent, 0.18f, S(18));
    ImVec2 ic(a.x + S(52), a.y + hh * 0.5f);
    dl->AddCircleFilled(ic, S(30), injected ? Accent(0.16f) : running ? Accent(0.08f) : White(0.05f), 40);
    Icon(dl, ic, S(26), injected ? Accent() : running ? Accent(0.8f) : White(0.5f),
         injected ? ICON_CIRCLE_CHECK : running ? ICON_ZAP : ICON_PLAY);
    const char* title = injected ? "Menu is running in the game" : running ? "Ready to inject" : "Game not running";
    const char* sub = injected ? "Press Insert in the game to open it. Press End in the game to unload it."
                    : running  ? "The game is open. Press Inject to load the menu into it."
                               : "Start Subnautica 2 first, then press Inject.";
    Text(dl, T().semibold, 20, ImVec2(a.x + S(100), a.y + S(44)), White(0.97f), title);
    Text(dl, T().regular, 12.5f, ImVec2(a.x + S(100), a.y + S(76)), White(0.55f), sub, w - S(330));

    ImVec2 bs(S(180), S(46));
    ImVec2 bp(b.x - S(24) - bs.x, a.y + (hh - bs.y) * 0.5f);
    if (busy) {
        Button("main", "Injecting\xe2\x80\xa6", bp, bs, Btn::Primary, ICON_REFRESH_CW, false);
    } else if (injected) {
        Button("main", "Injected", bp, bs, Btn::Tint, ICON_CHECK, false);
    } else if (running) {
        if (Button("main", "Inject", bp, bs, Btn::Primary, ICON_ZAP, paths.found)) injectNow = true;
    } else {
        if (Button("main", "Start game", bp, bs, Btn::Primary, ICON_PLAY, true)) startNow = true;
    }

    float y = b.y + S(16);
    float colW = std::floor((w - S(16)) * 0.5f);
    float ch = S(220);
    ImVec2 c1(o.x, y), c2(o.x + colW + S(16), y);
    Card(dl, c1, ImVec2(c1.x + colW, y + ch));
    Caption(dl, ImVec2(c1.x + S(16), y + S(15)), "HOW IT WORKS");
    Step(dl, ImVec2(c1.x + S(16), y + S(44)), colW - S(32), 1, "Check your settings", "Keys and menu size are in the Settings tab.", false);
    Step(dl, ImVec2(c1.x + S(16), y + S(98)), colW - S(32), 2, "Start the game", "Wait for the main menu or load your save.", running);
    Step(dl, ImVec2(c1.x + S(16), y + S(152)), colW - S(32), 3, "Press Inject", "Then press Insert in the game to open the menu.", injected);

    Card(dl, c2, ImVec2(c2.x + colW, y + ch));
    Caption(dl, ImVec2(c2.x + S(16), y + S(15)), "FILES");
    std::error_code ec;
    bool hasDll = paths.found && (std::filesystem::exists(paths.dll, ec) || std::filesystem::exists(paths.bundled, ec));
    Row(dl, ImVec2(c2.x + S(16), y + S(46)), colW - S(32), "Game folder", paths.found ? "Subnautica2 (Steam)" : "Not found", paths.found);
    if (paths.found && ImGui::IsMouseHoveringRect(ImVec2(c2.x, y + S(40)), ImVec2(c2.x + colW, y + S(66)))) Tip(Narrow(paths.win64).c_str());
    Row(dl, ImVec2(c2.x + S(16), y + S(74)), colW - S(32), "Menu file", hasDll ? "fathom.dll" : "Missing", hasDll);
    Row(dl, ImVec2(c2.x + S(16), y + S(102)), colW - S(32), "Settings", paths.found ? "settings.json" : "-", paths.found);
    float bw = std::floor((colW - S(40)) * 0.5f);
    if (Button("folder", "Open folder", ImVec2(c2.x + S(16), y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_FOLDER_OPEN,
               paths.found))
        ShellExecuteW(nullptr, L"open", paths.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    bool hasLog = paths.found && std::filesystem::exists(paths.log, ec);
    if (Button("log", "Open log", ImVec2(c2.x + S(24) + bw, y + ch - S(52)), ImVec2(bw, S(34)), Btn::Ghost, ICON_INFO, hasLog))
        ShellExecuteW(nullptr, L"open", paths.log.c_str(), nullptr, nullptr, SW_SHOWNORMAL);

    ImGui::SetCursorScreenPos(start);
    ImGui::Dummy(ImVec2(w, y + ch - start.y));
}

}
