#include "ui/icons.h"
#include "ui/loader.h"
#include "ui/menu.h"
#include <algorithm>
#include <cmath>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace ui {

namespace {

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

struct Swatch {
    const char* name;
    ImVec4 col;
};

const Swatch kSwatches[] = {
    {"Red", ImVec4(1.000f, 0.275f, 0.333f, 1.0f)},    {"Ember", ImVec4(1.000f, 0.478f, 0.271f, 1.0f)},
    {"Gold", ImVec4(1.000f, 0.769f, 0.000f, 1.0f)},   {"Green", ImVec4(0.239f, 0.863f, 0.518f, 1.0f)},
    {"Blue", ImVec4(0.184f, 0.722f, 1.000f, 1.0f)},   {"Violet", ImVec4(0.706f, 0.420f, 1.000f, 1.0f)},
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
        SliderRow("appscale", ImVec2(p.x + S(8), y), w - S(16), "App size", &appScaleEdit, 0.7f, 2.0f, 1.0f, 0.05f, "%.0f%%", 100.0f, true,
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
            ImVec2 a(c.x - r - S(1), c.y - r - S(1)), b(c.x + r + S(1), c.y + r + S(1));
            if (i == s.accent) ParaLine(dl, ImVec2(a.x - S(4), a.y - S(4)), ImVec2(b.x + S(4), b.y + S(4)), White(0.95f), S(2));
            else if (hov) ParaLine(dl, ImVec2(a.x - S(4), a.y - S(4)), ImVec2(b.x + S(4), b.y + S(4)), White(0.3f), S(1.5f));
            Para(dl, a, b, ImGui::GetColorU32(col));
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
        float h = S(38) + S(52) + S(8) + bh * 2 + S(8) + S(16);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Folders and backups");
        ToggleRow("backupinject", ImVec2(p.x + S(8), y), w - S(16), "Back up when you inject", "Saves a copy of the game's saves first",
                  &s.backupOnInject, true);
        y += S(52) + S(8);
        float bw = std::floor((w - S(40)) * 0.5f);
        if (Button("home", "Fathom folder", ImVec2(p.x + S(16), y), ImVec2(bw, bh), Btn::Ghost, ICON_FOLDER_OPEN, (bool)l.openHome))
            l.openHome();
        if (Button("backups", "Backups folder", ImVec2(p.x + S(24) + bw, y), ImVec2(bw, bh), Btn::Ghost, ICON_ARCHIVE, (bool)l.openBackups))
            l.openBackups();
        y += bh + S(8);
        if (Button("backupnow", "Back up saves now", ImVec2(p.x + S(16), y), ImVec2(bw, bh), Btn::Tint, ICON_SAVE, (bool)l.backupSaves)) {
            std::string path = l.backupSaves();
            if (path.empty()) {
                Notify("No game saves found", ICON_TRIANGLE_ALERT);
            } else {
                l.lastBackup = path;
                l.backupsTime = -100.0;
                Notify("Saves backed up", ICON_ARCHIVE);
            }
        }
        if (Button("gamefolder", "Game mod folder", ImVec2(p.x + S(24) + bw, y), ImVec2(bw, bh), Btn::Ghost, ICON_FOLDER_OPEN,
                   l.gameFound && (bool)l.openGame))
            l.openGame();
    }
    {
        double now = ImGui::GetTime();
        if (l.listBackups && now - l.backupsTime > 5.0) {
            std::vector<BackupEntry> fresh = l.listBackups();
            for (BackupEntry& e : fresh)
                for (const BackupEntry& o : l.backups)
                    if (o.path == e.path) e.picked = o.picked;
            l.backups = std::move(fresh);
            l.backupsTime = now;
        }
        std::vector<BackupEntry>& list = l.backups;
        int picked = 0, groups = 0;
        for (size_t i = 0; i < list.size(); ++i) {
            picked += list[i].picked;
            groups += i == 0 || list[i].game != list[i - 1].game;
        }
        float rowH = S(44), headH = S(30);
        float full = list.size() * rowH + groups * headH;
        float listH = list.empty() ? S(48) : std::min(full, S(330));
        float h = S(38) + listH + S(12) + bh + S(16);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Save backups");
        if (list.empty()) {
            TextFit(dl, T().regular, 12.5f, ImVec2(p.x + S(16), y + S(4)), w - S(32), White(0.55f), "No backups yet.");
            TextFit(dl, T().regular, 12, ImVec2(p.x + S(16), y + S(22)), w - S(32), White(0.38f),
                    "Fathom makes one when you inject, or press Back up saves now.");
        } else {
            ImGui::SetCursorScreenPos(ImVec2(p.x + S(8), y));
            ImGui::BeginChild("backuplist", ImVec2(w - S(16), listH), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            ImDrawList* cl = ImGui::GetWindowDrawList();
            ImVec2 o = ImGui::GetCursorScreenPos();
            float cw = ImGui::GetContentRegionAvail().x;
            float cy = o.y;
            for (size_t i = 0; i < list.size(); ++i) {
                BackupEntry& e = list[i];
                if (i == 0 || e.game != list[i - 1].game) {
                    int n = 0, sel = 0;
                    for (size_t k = i; k < list.size() && list[k].game == e.game; ++k) {
                        ++n;
                        sel += list[k].picked;
                    }
                    std::string head = e.game + "  \xc2\xb7  " + std::to_string(n) + (n == 1 ? " backup" : " backups");
                    Upper(cl, T().semibold, 10.5f, ImVec2(o.x + S(8), cy + S(10)), White(0.5f), head.c_str());
                    const char* all = sel == n ? "Unselect" : "Select all";
                    ImVec2 as = Measure(T().medium, 11.5f, all);
                    ImVec2 ap(o.x + cw - S(8) - as.x, cy + S(9));
                    ImGui::SetCursorScreenPos(ImVec2(ap.x - S(4), ap.y - S(3)));
                    ImGui::PushID((int)i);
                    if (ImGui::InvisibleButton("all", ImVec2(as.x + S(8), as.y + S(6))))
                        for (size_t k = i; k < list.size() && list[k].game == e.game; ++k) list[k].picked = sel != n;
                    bool ah = ImGui::IsItemHovered();
                    ImGui::PopID();
                    Text(cl, T().medium, 11.5f, ap, ah ? Accent() : White(0.45f), all);
                    cy += headH;
                }
                ImVec2 a(o.x, cy), b(o.x + cw, cy + rowH - S(4));
                ImGui::SetCursorScreenPos(a);
                std::string key = Narrow(e.path);
                ImGui::PushID(key.c_str());
                if (ImGui::InvisibleButton("row", ImVec2(cw, rowH - S(4)))) e.picked = !e.picked;
                bool hov = ImGui::IsItemHovered();
                ImGui::PopID();
                cl->AddRectFilled(a, b, e.picked ? Accent(0.10f) : White(hov ? 0.05f : 0.025f));
                ImVec2 ba(a.x + S(10), a.y + (rowH - S(4)) * 0.5f - S(8)), bb(ba.x + S(18), ba.y + S(16));
                Para(cl, ba, bb, e.picked ? Accent() : White(0.08f));
                if (!e.picked) ParaLine(cl, ba, bb, White(hov ? 0.35f : 0.18f));
                else Icon(cl, ImVec2((ba.x + bb.x) * 0.5f, (ba.y + bb.y) * 0.5f), S(11), White(0.95f), ICON_CHECK);
                ImVec2 ss = Measure(T().regular, 11.5f, e.size.c_str());
                Text(cl, T().regular, 11.5f, ImVec2(b.x - S(10) - ss.x, a.y + (rowH - S(4) - ss.y) * 0.5f), White(0.45f), e.size.c_str());
                float tx = bb.x + S(12), tw = b.x - S(20) - ss.x - tx;
                if (e.note.empty()) {
                    ImVec2 ws = Measure(T().medium, 13, e.when.c_str());
                    TextFit(cl, T().medium, 13, ImVec2(tx, a.y + (rowH - S(4) - ws.y) * 0.5f), tw, White(0.92f), e.when.c_str());
                } else {
                    TextFit(cl, T().medium, 13, ImVec2(tx, a.y + S(4)), tw, White(0.92f), e.when.c_str());
                    TextFit(cl, T().regular, 11, ImVec2(tx, a.y + S(21)), tw, White(0.42f), e.note.c_str());
                }
                if (hov) Tip(key.c_str());
                cy += rowH;
            }
            ImGui::SetCursorScreenPos(o);
            ImGui::Dummy(ImVec2(cw, cy - o.y));
            ImGui::EndChild();
        }
        y += listH + S(12);
        float bw = std::floor((w - S(40)) * 0.5f);
        bool allPicked = !list.empty() && picked == (int)list.size();
        if (Button("pickall", allPicked ? "Unselect all" : "Select all", ImVec2(p.x + S(16), y), ImVec2(bw, bh), Btn::Ghost, ICON_CHECK,
                   !list.empty()))
            for (BackupEntry& e : list) e.picked = !allPicked;
        std::string del = picked ? "Delete (" + std::to_string(picked) + ")" : "Delete";
        if (Button("delbackups", del.c_str(), ImVec2(p.x + S(24) + bw, y), ImVec2(bw, bh), Btn::Danger, ICON_TRASH_2,
                   picked > 0 && (bool)l.recycleBackups)) {
            std::string title = picked == 1 ? "Delete 1 backup?" : "Delete " + std::to_string(picked) + " backups?";
            Ask(title.c_str(),
                "The backups you picked go to the Recycle Bin. Once you empty the Recycle Bin they are gone for good, and those saves can't "
                "be brought back.",
                "Delete", [this] {
                    std::vector<std::wstring> paths;
                    for (const BackupEntry& e : loader->backups)
                        if (e.picked) paths.push_back(e.path);
                    int n = loader->recycleBackups(paths);
                    loader->backupsTime = -100.0;
                    if (n < 0) Notify("Some backups could not be deleted", ICON_TRIANGLE_ALERT);
                    else Notify(n == 1 ? "1 backup moved to the Recycle Bin" : std::to_string(n) + " backups moved to the Recycle Bin", ICON_TRASH_2);
                });
        }
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
