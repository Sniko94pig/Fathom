#include "ui/icons.h"
#include "ui/loader.h"
#include "ui/menu.h"
#include <algorithm>
#include <cmath>

namespace ui {

namespace {

struct Swatch {
    const char* name;
    ImVec4 col;
};

const Swatch kSwatches[] = {
    {"Teal", ImVec4(0.247f, 0.847f, 0.816f, 1.0f)},   {"Blue", ImVec4(0.376f, 0.647f, 0.980f, 1.0f)},
    {"Violet", ImVec4(0.655f, 0.545f, 0.980f, 1.0f)}, {"Pink", ImVec4(0.957f, 0.447f, 0.714f, 1.0f)},
    {"Orange", ImVec4(0.984f, 0.573f, 0.235f, 1.0f)}, {"Green", ImVec4(0.290f, 0.871f, 0.502f, 1.0f)},
};

}

int AccentCount() { return (int)(sizeof(kSwatches) / sizeof(kSwatches[0])); }
const char* AccentName(int i) { return kSwatches[std::clamp(i, 0, AccentCount() - 1)].name; }
ImVec4 AccentColor(int i) { return kSwatches[std::clamp(i, 0, AccentCount() - 1)].col; }

void Menu::LoaderPage() {
    Loader& l = *loader;
    LoaderSettings& s = l.s;
    Masonry ms = BeginMasonry();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ms.colW;
    float bh = S(36);

    {
        float h = S(38) + S(52) * 4 + S(8);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "General");
        ToggleRow("startup", ImVec2(p.x + S(8), y), w - S(16), "Start Fathom with Windows", "Opens in the taskbar when you log in",
                  &s.startWithWindows, true);
        y += S(52);
        ToggleRow("autoinject", ImVec2(p.x + S(8), y), w - S(16), "Inject automatically", "Loads the menu 30 seconds after the game starts",
                  &s.autoInject, true);
        y += S(52);
        ToggleRow("autoclose", ImVec2(p.x + S(8), y), w - S(16), "Close Fathom after injecting", "The menu keeps running in the game",
                  &s.closeAfterInject, true);
        y += S(52);
        ToggleRow("ontop", ImVec2(p.x + S(8), y), w - S(16), "Keep Fathom on top", "Stays above your other windows", &s.onTop, true);
    }
    {
        float h = S(38) + S(60) + S(56) + S(8);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Appearance");
        if (appScaleEdit < 0) appScaleEdit = s.scale;
        bool active = false;
        SliderRow("appscale", ImVec2(p.x + S(8), y), w - S(16), "App size", &appScaleEdit, 0.8f, 1.4f, 1.0f, 0.05f, "%.0f%%", 100.0f, true,
                  0.0f, &active);
        if (!active) s.scale = appScaleEdit;
        y += S(60);
        RowBase("accentrow", ImVec2(p.x + S(8), y), w - S(16), S(48), "Accent color", AccentName(s.accent), true, nullptr,
                S(32) * AccentCount());
        float r = S(10);
        for (int i = AccentCount() - 1, k = 0; i >= 0; --i, ++k) {
            ImVec2 c(p.x + w - S(16) - S(14) - k * S(32), y + S(24));
            ImGui::SetCursorScreenPos(ImVec2(c.x - S(14), c.y - S(14)));
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("sw", ImVec2(S(28), S(28)))) s.accent = i;
            bool hov = ImGui::IsItemHovered();
            ImGui::PopID();
            if (hov) Tip(AccentName(i));
            ImVec4 col = AccentColor(i);
            if (i == s.accent) dl->AddCircle(c, r + S(4), ImGui::GetColorU32(col), 32, S(2));
            else if (hov) dl->AddCircle(c, r + S(4), White(0.25f), 32, S(1.5f));
            dl->AddCircleFilled(c, r, ImGui::GetColorU32(col), 32);
        }
    }
    {
        float h = S(38) + S(52) * 3 + S(8) + bh + S(16);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Library");
        ToggleRow("soon", ImVec2(p.x + S(8), y), w - S(16), "Show coming soon games", "Games Fathom does not support yet", &s.showSoon, true);
        y += S(52);
        ToggleRow("instfirst", ImVec2(p.x + S(8), y), w - S(16), "Installed games first", "Games on this PC go to the top", &s.installedFirst,
                  true);
        y += S(52);
        ToggleRow("webart", ImVec2(p.x + S(8), y), w - S(16), "Download missing artwork", "Gets covers from Steam's website when needed",
                  &s.webArt, true);
        y += S(52) + S(8);
        if (Button("reloadart", "Reload artwork", ImVec2(p.x + S(16), y), ImVec2(w - S(32), bh), Btn::Ghost, ICON_REFRESH_CW,
                   (bool)l.refreshArt)) {
            l.refreshArt();
            Notify("Reloading the game artwork", ICON_REFRESH_CW);
        }
    }
    {
        float h = S(38) + S(48) + S(52) + S(8) + bh + S(16);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Updates");
        std::string ver = "Fathom " + l.version;
        Text(dl, T().medium, 13.5f, ImVec2(p.x + S(16), y + S(4)), White(0.95f), ver.c_str());
        const char* info = l.updateText.empty() ? "Updates come from GitHub" : l.updateText.c_str();
        TextFit(dl, T().regular, 12, ImVec2(p.x + S(16), y + S(24)), w - S(32), l.updateReady ? Accent() : White(0.48f), info);
        y += S(48);
        ToggleRow("autoupdate", ImVec2(p.x + S(8), y), w - S(16), "Check when Fathom starts", "Tells you when a new version is out",
                  &s.checkUpdates, true);
        y += S(52) + S(8);
        float bw = std::floor((w - S(40)) * 0.5f);
        if (Button("checknow", l.updateBusy ? "Checking\xe2\x80\xa6" : "Check now", ImVec2(p.x + S(16), y), ImVec2(bw, bh), Btn::Ghost,
                   ICON_REFRESH_CW, !l.updateBusy && (bool)l.checkUpdates))
            l.checkUpdates();
        if (Button("update", "Update now", ImVec2(p.x + S(24) + bw, y), ImVec2(bw, bh), Btn::Primary, ICON_DOWNLOAD,
                   l.updateReady && !l.updateBusy && (bool)l.installUpdate))
            Ask("Update Fathom?", "Fathom downloads the newest installer, closes, and opens the installer. Press Install there.", "Update",
                [this] {
                    loader->installUpdate();
                    Notify("Downloading the installer\xe2\x80\xa6", ICON_DOWNLOAD);
                });
    }
    {
        float h = S(38) + bh * 2 + S(8) + S(42);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Folders and backups");
        float bw = std::floor((w - S(40)) * 0.5f);
        if (Button("home", "Fathom folder", ImVec2(p.x + S(16), y), ImVec2(bw, bh), Btn::Ghost, ICON_FOLDER_OPEN, (bool)l.openHome))
            l.openHome();
        if (Button("backups", "Save backups", ImVec2(p.x + S(24) + bw, y), ImVec2(bw, bh), Btn::Ghost, ICON_ARCHIVE, (bool)l.openBackups))
            l.openBackups();
        y += bh + S(8);
        if (Button("backupnow", "Back up saves now", ImVec2(p.x + S(16), y), ImVec2(bw, bh), Btn::Tint, ICON_SAVE, (bool)l.backupSaves)) {
            std::string path = l.backupSaves();
            if (path.empty()) {
                Notify("No Subnautica 2 saves found", ICON_TRIANGLE_ALERT);
            } else {
                l.lastBackup = path;
                Notify("Saves backed up", ICON_ARCHIVE);
            }
        }
        if (Button("gamefolder", "Game mod folder", ImVec2(p.x + S(24) + bw, y), ImVec2(bw, bh), Btn::Ghost, ICON_FOLDER_OPEN,
                   l.gameFound && (bool)l.openGame))
            l.openGame();
        std::string last = l.lastBackup.empty() ? "Backups go to the save-backups folder" : "Last: " + l.lastBackup;
        TextFit(dl, T().regular, 11.5f, ImVec2(p.x + S(16), y + bh + S(12)), w - S(32), White(0.38f), last.c_str());
    }
    {
        float h = S(38) + S(40) + bh + S(16);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Reset");
        TextFit(dl, T().regular, 12, ImVec2(p.x + S(16), y + S(4)), w - S(32), White(0.55f), "Puts every setting on this page back to normal.");
        TextFit(dl, T().regular, 12, ImVec2(p.x + S(16), y + S(20)), w - S(32), White(0.38f), "Your games, saves and backups are not touched.");
        y += S(40);
        if (Button("resetall", "Reset Fathom settings", ImVec2(p.x + S(16), y), ImVec2(w - S(32), bh), Btn::Danger, ICON_ROTATE_CCW))
            Ask("Reset Fathom settings?", "Every setting on this page goes back to how it was when you installed Fathom.", "Reset", [this] {
                loader->s = LoaderSettings{};
                appScaleEdit = -1.0f;
                Notify("Settings reset", ICON_ROTATE_CCW);
            });
    }
    ms.End();
}

}
