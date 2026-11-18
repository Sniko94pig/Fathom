#include "launcher/home.h"
#include "ui/icons.h"
#include "ui/ui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
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

void Row(ImDrawList* dl, ImVec2 p, float w, const char* label, const std::string& value, bool good) {
    Text(dl, T().medium, 12.5f, p, White(0.62f), label);
    Icon(dl, ImVec2(p.x + w - S(8), p.y + S(8)), S(14), good ? Accent() : Danger(), good ? ICON_CIRCLE_CHECK : ICON_TRIANGLE_ALERT);
    TextFit(dl, T().regular, 12, ImVec2(p.x + S(120), p.y + S(1)), w - S(150), White(0.8f), value.c_str());
}

}

void Home::Draw() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
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

    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y + ch - o.y));
}

}
