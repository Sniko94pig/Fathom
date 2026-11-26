#include "launcher/home.h"
#include "launcher/art.h"
#include "launcher/games.h"
#include "launcher/maps.h"
#include "ui/icons.h"
#include "ui/ui.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <vector>

namespace launcher {

using namespace ui;
namespace mapdata = launcher::maps;

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
    else if (help) DrawHelp();
    else DrawLibrary();
}

namespace {

const char* const kHelpEmail = "ion_fiber94@tutamail.com";
const char* const kDiscordUrl = "";
const char* const kWebsiteUrl = "";

struct Faq {
    const char* q;
    const char* a;
};

const Faq kFaq[] = {
    {"Is Fathom free?", "Yes. For now Fathom is also open source, so you can read all of its code on GitHub. Updates are free too."},
    {"How do I open the menu in a game?",
     "Open the game's page in Fathom and press Start game. When the game has loaded, press Inject. Then press the open key in the game. The game's page shows which key that is, and you can change it in the menu's Settings."},
    {"The menu doesn't show up. What can I do?",
     "Wait until the game's main menu or your save has fully loaded, then press Inject again. If it still doesn't show, press Open log on the game's page and send the log to us by email, or join our Discord and ask there."},
    {"Can I get banned?",
     "Fathom is made for single-player and co-op games you play with friends. Never use cheats in online matches, ranked modes or on public servers. Where a game has leaderboards, Fathom tries to keep cheated runs off them, but using cheats online is always at your own risk."},
    {"Will cheats break my save?",
     "Every cheat switches off cleanly when you turn it off or close the menu. Fathom also backs up a game's saves every time you inject, and you can find every backup with its date and time in Settings."},
    {"My antivirus warns about Fathom.",
     "Mod menus load a menu file into the game, and some antivirus apps flag anything that does that. Fathom is open source, so anyone can check what it does. Only download Fathom from its GitHub releases page."},
    {"How do I update Fathom?", "Fathom checks for updates when it starts. You can also look for one yourself under Settings, Updates."},
    {"My game says Coming soon.",
     "More games are added all the time. Games marked Being built already show the modules that are coming. Want a game added? Send us an email or ask on our Discord."},
    {"How do I uninstall Fathom?",
     "Use Uninstall Fathom in the Fathom folder, or find Fathom in Windows Settings, Apps. Your game saves are not touched."},
};

}

void Home::DrawHelp() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    Text(dl, T().regular, 13, o, White(0.55f), "Stuck or found a bug? Ask on Discord, send us an email, or look at the common questions below.", w);
    float gap = S(16), top = o.y + S(36);
    int cols = w >= S(640) ? 3 : 1;
    float cw = std::floor((w - gap * (cols - 1)) / cols), ch = S(186);
    std::string mail = std::string("mailto:") + kHelpEmail;
    struct Contact {
        const char* id;
        const char* icon;
        const char* title;
        const char* body;
        const char* url;
        const char* button;
    };
    const Contact cs[3] = {
        {"discord", ICON_MESSAGE_CIRCLE, "Discord", "Chat with other players, share tips and get help fast.", kDiscordUrl, "Join Discord"},
        {"email", ICON_MAIL, "Email", "Questions, bugs or a game you want added.", mail.c_str(), "Send email"},
        {"website", ICON_GLOBE, "Website", "Guides, news and every Fathom download in one place.", kWebsiteUrl, "Open website"},
    };
    for (int i = 0; i < 3; ++i) {
        const Contact& c = cs[i];
        bool ready = c.url && *c.url;
        ImVec2 a(o.x + (i % cols) * (cw + gap), top + (i / cols) * (ch + gap)), b(a.x + cw, a.y + ch);
        Card(dl, a, b, ready);
        ImVec2 ta(a.x + S(16), a.y + S(18)), tb(ta.x + S(42), ta.y + S(38));
        Para(dl, ta, tb, ready ? Accent(0.18f) : White(0.06f));
        Icon(dl, ImVec2((ta.x + tb.x) * 0.5f, (ta.y + tb.y) * 0.5f), S(18), ready ? Accent() : White(0.55f), c.icon);
        Text(dl, T().semibold, 15, ImVec2(tb.x + S(14), ta.y + S(1)), White(0.96f), c.title);
        {
            const char* tag = ready ? "Open" : "Coming soon";
            ImVec2 bs = MeasureUpper(T().semibold, 9.5f, tag);
            ImVec2 ba(tb.x + S(14), ta.y + S(22)), bb(ba.x + bs.x + S(16), ba.y + S(16));
            Para(dl, ba, bb, ready ? Accent() : ImGui::GetColorU32(ImVec4(0.98f, 0.74f, 0.30f, 1.0f)));
            Upper(dl, T().semibold, 9.5f, ImVec2(ba.x + S(8), ba.y + (S(16) - bs.y) * 0.5f), ready ? White(0.95f) : Black(0.9f), tag);
        }
        float by = a.y + S(70);
        if (i == 1) {
            TextFit(dl, T().medium, 13, ImVec2(a.x + S(16), by), cw - S(32), Accent(), kHelpEmail);
            by += S(22);
        }
        Text(dl, T().regular, 12.5f, ImVec2(a.x + S(16), by), White(0.6f), c.body, cw - S(32));
        ImVec2 bp(a.x + S(16), b.y - S(50));
        float bw = cw - S(32) - (i == 1 ? S(42) : 0.0f);
        if (ready) {
            if (Button(c.id, c.button, bp, ImVec2(bw, S(34)), i == 1 ? Btn::Primary : Btn::Tint, c.icon, true)) {
                std::wstring url(c.url, c.url + std::strlen(c.url));
                ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
        } else {
            Button(c.id, "Coming soon", bp, ImVec2(bw, S(34)), Btn::Ghost, ICON_LOCK, false);
        }
        if (i == 1) {
            if (IconButton("copymail", ICON_COPY, ImVec2(b.x - S(16) - S(34), bp.y), S(34))) {
                ImGui::SetClipboardText(kHelpEmail);
                helpCopied = ImGui::GetTime();
            }
            if (ImGui::IsItemHovered()) Tip(ImGui::GetTime() - helpCopied < 2.0 ? "Copied" : "Copy the email address");
        }
    }
    int rows = (3 + cols - 1) / cols;
    float y = top + rows * (ch + gap) + S(10);
    Caption(dl, ImVec2(o.x, y), "Common questions");
    y += S(34);
    float tw = w - S(42) - S(48);
    for (int i = 0; i < (int)(sizeof(kFaq) / sizeof(kFaq[0])); ++i) {
        const Faq& f = kFaq[i];
        float hh = S(46);
        ImGui::SetCursorScreenPos(ImVec2(o.x, y));
        ImGui::PushID(i);
        bool clicked = ImGui::InvisibleButton("faq", ImVec2(w, hh));
        bool hov = ImGui::IsItemHovered();
        float t = Ease(ImGui::GetID("open"), helpOpen == i ? 1.0f : 0.0f, 14.0f);
        float hv = Ease(ImGui::GetID("hover"), hov ? 1.0f : 0.0f, 18.0f);
        ImGui::PopID();
        if (clicked) helpOpen = helpOpen == i ? -1 : i;
        if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImGui::PushFont(T().regular, S(13));
        ImVec2 as = ImGui::CalcTextSize(f.a, nullptr, false, tw);
        ImGui::PopFont();
        float ah = std::floor((as.y + S(16)) * t);
        ImVec2 a(o.x, y), b(o.x + w, y + hh + ah);
        dl->AddRectFilled(a, b, White(0.035f + 0.025f * std::max(hv, t)));
        dl->AddRectFilled(a, ImVec2(a.x + S(2), b.y), Accent(0.25f + 0.75f * t));
        Icon(dl, ImVec2(o.x + S(22), y + hh * 0.5f), S(15), t > 0.5f ? Accent() : White(0.45f), ICON_CIRCLE_HELP);
        ImVec2 qs = Measure(T().medium, 13.5f, f.q);
        TextFit(dl, T().medium, 13.5f, ImVec2(o.x + S(42), y + std::floor((hh - qs.y) * 0.5f)), tw, White(0.94f), f.q);
        Icon(dl, ImVec2(o.x + w - S(24), y + hh * 0.5f), S(15), White(0.45f + 0.4f * hv), helpOpen == i ? ICON_CHEVRON_UP : ICON_CHEVRON_DOWN);
        if (ah > 0.5f) {
            dl->PushClipRect(ImVec2(o.x, y + hh - S(8)), b, true);
            Text(dl, T().regular, 13, ImVec2(o.x + S(42), y + hh - S(4)), White(0.62f * t), f.a, tw);
            dl->PopClipRect();
        }
        y = b.y + S(6);
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, y - o.y + S(10)));
}

void Home::DrawMaps() {
    mapsWanted = true;
    if (mapGame < 0 || mapGame >= mapdata::Count) DrawMapList();
    else DrawMapView();
}

void Home::DrawMapList() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x;
    Text(dl, T().regular, 13, o, White(0.55f), "Pick a game to open its map. Every place is marked, and you can filter and search them.");
    float gap = S(16), top = o.y + S(36);
    int cols = w >= S(600) ? 3 : 2;
    float cw = std::floor((w - gap * (cols - 1)) / cols), ph = std::floor(cw * 0.66f), ch = ph + S(64);
    for (int g = 0; g < mapdata::Count; ++g) {
        const MapPic& pic = mapPics[g];
        ImVec2 a(o.x + (g % cols) * (cw + gap), top + (g / cols) * (ch + gap));
        ImGui::SetCursorScreenPos(a);
        ImGui::PushID(g);
        bool clicked = ImGui::InvisibleButton("mapcard", ImVec2(cw, ch));
        bool hov = ImGui::IsItemHovered();
        float hv = Ease(ImGui::GetID("hover"), hov ? 1.0f : 0.0f, 18.0f);
        ImGui::PopID();
        float lift = std::floor(hv * S(3));
        ImVec2 pa(a.x, a.y - lift), pb(a.x + cw, a.y + ph - lift);
        dl->AddRectFilled(pa, pb, ImGui::GetColorU32(ImVec4(0.027f, 0.063f, 0.122f, 1.0f)));
        if (pic.tex && pic.w > 0 && pic.h > 0) {
            float zoom = 1.0f + 0.05f * hv;
            float ar = (pb.x - pa.x) / (pb.y - pa.y), pr = pic.w / pic.h;
            float uw = std::min(1.0f, ar / pr) / zoom, vh = std::min(1.0f, pr / ar) / zoom;
            dl->AddImage(pic.tex, pa, pb, ImVec2(0.5f - uw * 0.5f, 0.5f - vh * 0.5f), ImVec2(0.5f + uw * 0.5f, 0.5f + vh * 0.5f));
        } else {
            Icon(dl, ImVec2((pa.x + pb.x) * 0.5f, (pa.y + pb.y) * 0.5f - S(10)), S(34), White(0.18f), ICON_MAP);
            const char* msg = !mapStatus[g].empty() ? mapStatus[g].c_str() : pic.loaded ? "Not drawn yet" : "Loading\xe2\x80\xa6";
            ImVec2 ms = Measure(T().regular, 12, msg);
            Text(dl, T().regular, 12, ImVec2(std::floor((pa.x + pb.x - ms.x) * 0.5f), (pa.y + pb.y) * 0.5f + S(18)), White(0.45f), msg);
        }
        dl->AddRectFilledMultiColor(ImVec2(pa.x, pb.y - ph * 0.35f), pb, Bg(0.0f), Bg(0.0f), Bg(0.85f), Bg(0.85f));
        dl->AddRect(pa, pb, hv > 0.01f ? Mix(ImVec4(1, 1, 1, 0.07f), T().accent, hv, 0.9f) : White(0.07f));
        dl->AddRectFilled(ImVec2(pa.x, pb.y - S(4)), pb, Accent(0.35f + 0.65f * hv));
        const char* badge = !pic.tex ? (pic.loaded ? "Not drawn yet" : "Loading") : pic.partial ? "Partly drawn" : "Full map";
        ImVec2 bs = MeasureUpper(T().semibold, 10, badge);
        ImVec2 ba(pa.x + S(10), pa.y + S(10)), bb(ba.x + bs.x + S(18), ba.y + S(20));
        ImU32 bcol = !pic.tex ? Black(0.72f) : pic.partial ? ImGui::GetColorU32(ImVec4(0.98f, 0.74f, 0.30f, 1.0f)) : Accent();
        Para(dl, ba, bb, bcol);
        Upper(dl, T().semibold, 10, ImVec2(ba.x + S(9), ba.y + (S(20) - bs.y) * 0.5f), pic.partial && pic.tex ? Black(0.9f) : White(0.95f), badge);
        Upper(dl, T().semibold, 18, ImVec2(pa.x + S(14), pb.y - S(36)), White(0.98f), mapdata::Title(g));
        int kc = 0, pc = 0;
        const mapdata::Kind* kinds = mapdata::Kinds(g, kc);
        const mapdata::Place* places = mapdata::Places(g, pc);
        std::string line;
        for (int k = 0; k < kc; ++k) {
            int n = 0;
            for (int i = 0; i < pc; ++i) n += places[i].kind == k;
            if (!line.empty()) line += "  \xc2\xb7  ";
            line += std::to_string(n) + " " + Lower(kinds[k].name);
        }
        if (line.empty()) line = pic.partial ? "Shows the places you have been to" : "No places marked for this game yet";
        float ly = a.y + ph + S(12);
        float x = a.x;
        for (int k = 0; k < kc && k < 6; ++k) {
            Diamond(dl, ImVec2(x + S(4), ly + S(8)), S(4), IM_COL32(kinds[k].r, kinds[k].g, kinds[k].b, 255));
            x += S(11);
        }
        TextFit(dl, T().regular, 12, ImVec2(x + (kc ? S(4) : 0), ly), a.x + cw - x - S(4), White(0.6f), line.c_str());
        Text(dl, T().regular, 11.5f, ImVec2(a.x, ly + S(22)), White(0.38f), pc ? (std::to_string(pc) + " places, click to open").c_str() : "Click to open");
        if (clicked) {
            mapGame = g;
            mapSearch.clear();
        }
    }
    int rows = (mapdata::Count + cols - 1) / cols;
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, top + rows * (ch + gap) - o.y));
}

void Home::DrawMapView() {
    const int gi = mapGame;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, avail = ImGui::GetContentRegionAvail().y;
    MapPic& pic = mapPics[gi];
    const std::string& status = mapStatus[gi];
    int kc = 0, pc = 0, ac = 0;
    const mapdata::Kind* kinds = mapdata::Kinds(gi, kc);
    const mapdata::Place* places = mapdata::Places(gi, pc);
    const mapdata::Area* areas = mapdata::Areas(gi, ac);
    float wx0 = 0, wz0 = 0, wx1 = 1, wz1 = 1;
    bool world = mapdata::World(gi, wx0, wz0, wx1, wz1);
    auto toUv = [&](float x, float z) { return ImVec2((x - wx0) / (wx1 - wx0), (wz1 - z) / (wz1 - wz0)); };
    auto kindCol = [&](int k, float alpha) { return IM_COL32(kinds[k].r, kinds[k].g, kinds[k].b, (int)(alpha * 255)); };

    if (Button("mapback", "All maps", o, ImVec2(S(124), S(34)), Btn::Ghost, ICON_ARROW_LEFT)) {
        mapGame = -1;
        ImGui::SetCursorScreenPos(o);
        ImGui::Dummy(ImVec2(w, S(40)));
        return;
    }
    Upper(dl, T().semibold, 18, ImVec2(o.x + S(142), o.y + S(7)), White(0.98f), mapdata::Title(gi));
    if (gi == mapdata::Subnautica2 && pic.partial &&
        Button("mapredo", "Refresh", ImVec2(o.x + w - S(130), o.y), ImVec2(S(130), S(34)), Btn::Ghost, ICON_REFRESH_CW, !mapsBusy))
        mapsReload = true;

    float top = o.y + S(50), h = std::max(S(320), avail - S(50) - S(34));
    float side = kc > 0 ? S(256) : 0.0f, sgap = kc > 0 ? S(16) : 0.0f;
    std::string q = Lower(mapSearch);
    auto visible = [&](int i) {
        const mapdata::Place& p = places[i];
        if (p.kind < 0 || p.kind >= 8 || mapHidden[gi][p.kind]) return false;
        if (q.empty()) return true;
        return Lower(p.name).find(q) != std::string::npos || Lower(p.note).find(q) != std::string::npos ||
               Lower(kinds[p.kind].name).find(q) != std::string::npos;
    };
    bool fly = false;
    if (kc > 0) {
        ImVec2 sa(o.x, top), sb(o.x + side, top + h);
        dl->AddRectFilled(sa, sb, White(0.03f));
        dl->AddRect(sa, sb, White(0.06f));
        float y = top + S(12);
        Input("mapsearch", &mapSearch, "Search places", ImVec2(sa.x + S(12), y), side - S(24), ICON_SEARCH);
        y += S(50);
        Upper(dl, T().semibold, 10.5f, ImVec2(sa.x + S(14), y), White(0.5f), "Show on the map");
        y += S(20);
        int rowsTotal = kc + (ac > 0 ? 1 : 0);
        for (int r = 0; r < rowsTotal; ++r) {
            bool isArea = r == kc;
            bool on = isArea ? mapAreaNames[gi] : !mapHidden[gi][r];
            ImVec2 ra(sa.x + S(6), y), rb(sb.x - S(6), y + S(30));
            ImGui::SetCursorScreenPos(ra);
            ImGui::PushID(1000 + r);
            if (ImGui::InvisibleButton("kind", ImVec2(rb.x - ra.x, rb.y - ra.y))) {
                if (isArea) mapAreaNames[gi] = !mapAreaNames[gi];
                else mapHidden[gi][r] = !mapHidden[gi][r];
            }
            bool rh = ImGui::IsItemHovered();
            ImGui::PopID();
            if (rh) dl->AddRectFilled(ra, rb, White(0.04f));
            ImVec2 c(ra.x + S(18), (ra.y + rb.y) * 0.5f);
            ImU32 col = isArea ? White(on ? 0.85f : 0.3f) : kindCol(r, on ? 1.0f : 0.3f);
            dl->AddCircleFilled(c, S(10), col, 20);
            Icon(dl, c, S(11.5f), Black(0.8f), isArea ? ICON_LANDMARK : kinds[r].icon);
            Text(dl, T().medium, 12.5f, ImVec2(ra.x + S(36), ra.y + S(7)), White(on ? 0.92f : 0.4f), isArea ? "Area names" : kinds[r].name);
            ImVec2 bx(rb.x - S(24), c.y - S(8)), by(rb.x - S(8), c.y + S(8));
            dl->AddRect(bx, by, White(on ? 0.0f : 0.25f), 0.0f, S(1.5f));
            if (on) {
                dl->AddRectFilled(bx, by, Accent());
                Icon(dl, ImVec2((bx.x + by.x) * 0.5f, (bx.y + by.y) * 0.5f), S(11), White(), ICON_CHECK);
            }
            if (!isArea) {
                int n = 0;
                for (int i = 0; i < pc; ++i) n += places[i].kind == r;
                std::string cnt = std::to_string(n);
                TextRight(dl, T().regular, 11.5f, ImVec2(bx.x - S(8), ra.y + S(8)), White(0.4f), cnt.c_str());
            }
            y += S(30);
        }
        y += S(10);
        dl->AddLine(ImVec2(sa.x + S(12), y), ImVec2(sb.x - S(12), y), White(0.06f));
        y += S(12);
        std::vector<int> list;
        for (int i = 0; i < pc; ++i)
            if (visible(i)) list.push_back(i);
        std::string head = "Places (" + std::to_string(list.size()) + ")";
        Upper(dl, T().semibold, 10.5f, ImVec2(sa.x + S(14), y), White(0.5f), head.c_str());
        y += S(22);
        float lh = sb.y - S(8) - y;
        if (lh > S(40)) {
            ImGui::SetCursorScreenPos(ImVec2(sa.x + S(4), y));
            ImGui::BeginChild("mapplaces", ImVec2(side - S(8), lh), ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
            ImDrawList* cl = ImGui::GetWindowDrawList();
            ImVec2 co = ImGui::GetCursorScreenPos();
            float rw = ImGui::GetContentRegionAvail().x, rh = S(44);
            for (size_t n = 0; n < list.size(); ++n) {
                int i = list[n];
                const mapdata::Place& p = places[i];
                ImVec2 ra(co.x, co.y + n * rh), rb(co.x + rw, co.y + (n + 1) * rh);
                ImGui::SetCursorScreenPos(ra);
                ImGui::PushID(i);
                bool hit = ImGui::InvisibleButton("place", ImVec2(rw, rh));
                bool hovered = ImGui::IsItemHovered();
                ImGui::PopID();
                bool sel = mapPick[gi] == i;
                if (sel) cl->AddRectFilled(ra, rb, Accent(0.16f));
                else if (hovered) cl->AddRectFilled(ra, rb, White(0.04f));
                if (sel) cl->AddRectFilled(ra, ImVec2(ra.x + S(3), rb.y), Accent());
                ImVec2 c(ra.x + S(20), ra.y + rh * 0.5f);
                cl->AddCircleFilled(c, S(9), kindCol(p.kind, 1.0f), 18);
                Icon(cl, c, S(10.5f), Black(0.8f), kinds[p.kind].icon);
                TextFit(cl, T().medium, 12.5f, ImVec2(ra.x + S(38), ra.y + S(6)), rw - S(44), White(0.92f), p.name);
                const char* sub = p.note && *p.note ? p.note : kinds[p.kind].name;
                TextFit(cl, T().regular, 11, ImVec2(ra.x + S(38), ra.y + S(24)), rw - S(44), White(0.45f), sub);
                if (hit) {
                    mapPick[gi] = i;
                    fly = true;
                }
            }
            if (list.empty()) Text(cl, T().regular, 12, ImVec2(co.x + S(10), co.y + S(6)), White(0.45f), "Nothing found");
            ImGui::SetCursorScreenPos(co);
            ImGui::Dummy(ImVec2(rw, std::max(1.0f, list.size() * rh)));
            ImGui::EndChild();
            dl = ImGui::GetWindowDrawList();
        }
    }

    ImVec2 a(o.x + side + sgap, top), b(o.x + w, top + h), c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
    float cw = b.x - a.x;
    dl->AddRectFilled(a, b, ImGui::GetColorU32(ImVec4(0.027f, 0.063f, 0.122f, 1.0f)));
    if (pic.tex && pic.w > 0 && pic.h > 0) {
        float fit = std::min(cw / pic.w, h / pic.h);
        float& zoom = mapZoom[gi];
        float& cu = mapCx[gi];
        float& cv = mapCy[gi];
        if (zoom <= 0) zoom = fit;
        float zmin = fit * 0.8f, zmax = fit * 16.0f;
        if (fly && mapPick[gi] >= 0 && world) {
            ImVec2 uv = toUv(places[mapPick[gi]].x, places[mapPick[gi]].z);
            mapFly[gi] = 1.0f;
            mapFlyU[gi] = uv.x;
            mapFlyV[gi] = uv.y;
            mapFlyZoom[gi] = std::clamp(std::max(zoom, fit * 4.0f), zmin, zmax);
        }
        ImGui::SetCursorScreenPos(a);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("mapview", ImVec2(cw, h));
        bool hov = ImGui::IsItemHovered();
        bool dragging = ImGui::IsItemActive() && ImGui::IsMouseDragging(0, S(3));
        if (dragging) {
            cu -= io.MouseDelta.x / (zoom * pic.w);
            cv -= io.MouseDelta.y / (zoom * pic.h);
            mapFly[gi] = 0;
        }
        auto zoomAt = [&](ImVec2 at, float nz) {
            float mu = cu + (at.x - c.x) / (zoom * pic.w), mv = cv + (at.y - c.y) / (zoom * pic.h);
            zoom = std::clamp(nz, zmin, zmax);
            cu = mu - (at.x - c.x) / (zoom * pic.w);
            cv = mv - (at.y - c.y) / (zoom * pic.h);
            mapFly[gi] = 0;
        };
        if (hov && io.MouseWheel != 0.0f) zoomAt(io.MousePos, zoom * std::pow(1.2f, io.MouseWheel));
        if (hov && ImGui::IsMouseDoubleClicked(0)) {
            mapFly[gi] = 1.0f;
            mapFlyU[gi] = mapFlyV[gi] = 0.5f;
            mapFlyZoom[gi] = fit;
        }
        if (mapFly[gi] > 0) {
            float t = 1.0f - std::exp(-Dt() * 9.0f);
            cu += (mapFlyU[gi] - cu) * t;
            cv += (mapFlyV[gi] - cv) * t;
            zoom += (mapFlyZoom[gi] - zoom) * t;
            if (std::fabs(mapFlyU[gi] - cu) < 1e-4f && std::fabs(mapFlyV[gi] - cv) < 1e-4f && std::fabs(mapFlyZoom[gi] - zoom) < fit * 0.002f) mapFly[gi] = 0;
        }
        cu = std::clamp(cu, 0.0f, 1.0f);
        cv = std::clamp(cv, 0.0f, 1.0f);
        auto toScreen = [&](float u, float v) { return ImVec2(c.x + (u - cu) * pic.w * zoom, c.y + (v - cv) * pic.h * zoom); };
        dl->PushClipRect(a, b, true);
        dl->AddImage(pic.tex, toScreen(0, 0), toScreen(1, 1));
        std::vector<ImVec4> used;
        auto room = [&](ImVec2 p0, ImVec2 p1) {
            for (const ImVec4& r : used)
                if (p0.x < r.z && p1.x > r.x && p0.y < r.w && p1.y > r.y) return false;
            used.push_back(ImVec4(p0.x, p0.y, p1.x, p1.y));
            return true;
        };
        auto inView = [&](ImVec2 p) { return p.x > a.x - S(80) && p.x < b.x + S(80) && p.y > a.y - S(20) && p.y < b.y + S(20); };
        float pad = S(6);
        if (world && mapAreaNames[gi]) {
            std::vector<std::pair<const char*, ImVec2>> named;
            for (int i = 0; i < ac; ++i) {
                const mapdata::Area& ar = areas[i];
                ImVec2 uv = toUv(ar.x, ar.z);
                ImVec2 p = toScreen(uv.x, uv.y);
                if (!inView(p)) continue;
                bool twice = false;
                for (const auto& n : named)
                    if (std::strcmp(n.first, ar.name) == 0 && std::hypot(n.second.x - p.x, n.second.y - p.y) < S(240)) twice = true;
                if (twice) continue;
                ImVec2 ts = MeasureUpper(T().semibold, 11, ar.name);
                float reach = ar.reach / (wx1 - wx0) * pic.w * zoom;
                static const ImVec2 moves[] = {{0, 0}, {0, -1}, {0, 1}, {-0.5f, 0}, {0.5f, 0}, {-0.5f, -1}, {0.5f, -1}, {-0.5f, 1}, {0.5f, 1}};
                for (const ImVec2& m : moves) {
                    ImVec2 shift(m.x * (ts.x * 0.5f + pad), m.y * (ts.y + pad));
                    if (std::hypot(shift.x, shift.y) > reach && (m.x != 0 || m.y != 0)) continue;
                    ImVec2 tp(std::floor(p.x + shift.x - ts.x * 0.5f), std::floor(p.y + shift.y - ts.y * 0.5f));
                    if (!room(ImVec2(tp.x - pad, tp.y - pad * 0.5f), ImVec2(tp.x + ts.x + pad, tp.y + ts.y + pad * 0.5f))) continue;
                    Upper(dl, T().semibold, 11, ImVec2(tp.x + 1, tp.y + 1), Black(0.85f), ar.name);
                    Upper(dl, T().semibold, 11, tp, White(0.9f), ar.name);
                    named.push_back({ar.name, p});
                    break;
                }
            }
        }
        int hot = -1;
        float hotDist = 1e9f;
        bool big = zoom >= fit * 1.8f, names = zoom >= fit * 3.2f;
        float rad = big ? S(10) : S(5.5f);
        if (world) {
            for (int i = 0; i < pc; ++i) {
                if (!visible(i)) continue;
                ImVec2 uv = toUv(places[i].x, places[i].z);
                ImVec2 p = toScreen(uv.x, uv.y);
                if (!inView(p)) continue;
                float d = std::hypot(io.MousePos.x - p.x, io.MousePos.y - p.y);
                if (hov && d < rad + S(4) && d < hotDist) {
                    hot = i;
                    hotDist = d;
                }
            }
            int pick = mapPick[gi];
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < pc; ++i) {
                    bool front = i == pick || i == hot;
                    if (front != (pass == 1) || !visible(i)) continue;
                    const mapdata::Place& pl = places[i];
                    ImVec2 uv = toUv(pl.x, pl.z);
                    ImVec2 p = toScreen(uv.x, uv.y);
                    if (!inView(p)) continue;
                    float r = front ? std::max(rad, S(10)) * (i == hot ? 1.12f : 1.0f) : rad;
                    if (i == pick) {
                        float t = (float)std::fmod(ImGui::GetTime(), 1.4) / 1.4f;
                        dl->AddCircle(p, r + S(4) + t * S(14), Accent(0.8f * (1.0f - t)), 32, S(2));
                        dl->AddCircle(p, r + S(3), White(0.95f), 32, S(2));
                    }
                    dl->AddCircleFilled(ImVec2(p.x, p.y + S(1.5f)), r + S(1.5f), Black(0.55f), 24);
                    dl->AddCircleFilled(p, r, kindCol(pl.kind, 1.0f), 24);
                    dl->AddCircle(p, r, Black(0.45f), 24, S(1.2f));
                    if (r >= S(9)) Icon(dl, p, r * 1.1f, Black(0.82f), kinds[pl.kind].icon);
                    if (names || front) {
                        ImVec2 ts = Measure(T().medium, 12, pl.name);
                        ImVec2 tp(std::floor(p.x + r + S(6)), std::floor(p.y - ts.y * 0.5f));
                        if (front || room(ImVec2(tp.x - S(2), tp.y), ImVec2(tp.x + ts.x + pad, tp.y + ts.y))) {
                            if (front) dl->AddRectFilled(ImVec2(tp.x - S(5), tp.y - S(3)), ImVec2(tp.x + ts.x + S(5), tp.y + ts.y + S(3)), Black(0.72f));
                            Text(dl, T().medium, 12, ImVec2(tp.x + 1, tp.y + 1), Black(0.85f), pl.name);
                            Text(dl, T().medium, 12, tp, White(0.96f), pl.name);
                        }
                    }
                }
        }
        if (hov && world) {
            float mu = cu + (io.MousePos.x - c.x) / (zoom * pic.w), mv = cv + (io.MousePos.y - c.y) / (zoom * pic.h);
            if (mu >= 0 && mu <= 1 && mv >= 0 && mv <= 1) {
                char buf[64];
                std::snprintf(buf, sizeof(buf), "X %d   %s %d", (int)std::lround(wx0 + mu * (wx1 - wx0)), mapdata::Axis(gi), (int)std::lround(wz1 - mv * (wz1 - wz0)));
                ImVec2 ts = Measure(T().medium, 11.5f, buf);
                ImVec2 pa(a.x + S(10), b.y - S(10) - ts.y - S(10)), pb(pa.x + ts.x + S(16), b.y - S(10));
                dl->AddRectFilled(pa, pb, Black(0.65f));
                Text(dl, T().medium, 11.5f, ImVec2(pa.x + S(8), pa.y + S(5)), White(0.85f), buf);
            }
        }
        dl->PopClipRect();
        if (hov && !dragging && ImGui::IsMouseReleased(0) && io.MouseDragMaxDistanceSqr[0] < S(4) * S(4)) mapPick[gi] = hot;
        if (hot >= 0 && !dragging) {
            const mapdata::Place& pl = places[hot];
            char tip[256];
            std::snprintf(tip, sizeof(tip), "%s\n%s\nX %d   %s %d", pl.name, pl.note && *pl.note ? pl.note : kinds[pl.kind].name, (int)std::lround(pl.x),
                          mapdata::Axis(gi), (int)std::lround(pl.z));
            Tip(tip);
        }
        float bsz = S(34);
        ImVec2 bp(b.x - bsz - S(10), a.y + S(10));
        if (IconButton("mapzin", ICON_ZOOM_IN, bp, bsz)) {
            mapFly[gi] = 1.0f;
            mapFlyU[gi] = cu;
            mapFlyV[gi] = cv;
            mapFlyZoom[gi] = std::clamp(zoom * 1.6f, zmin, zmax);
        }
        if (IconButton("mapzout", ICON_ZOOM_OUT, ImVec2(bp.x, bp.y + bsz + S(6)), bsz)) {
            mapFly[gi] = 1.0f;
            mapFlyU[gi] = cu;
            mapFlyV[gi] = cv;
            mapFlyZoom[gi] = std::clamp(zoom / 1.6f, zmin, zmax);
        }
        if (IconButton("mapfit", ICON_MAXIMIZE, ImVec2(bp.x, bp.y + (bsz + S(6)) * 2), bsz)) {
            mapFly[gi] = 1.0f;
            mapFlyU[gi] = mapFlyV[gi] = 0.5f;
            mapFlyZoom[gi] = fit;
        }
    } else {
        std::string msg = !status.empty() ? status + "\xe2\x80\xa6" : pic.loaded ? pic.note : "Loading the map\xe2\x80\xa6";
        float tw = std::min(cw - S(64), S(520));
        ImGui::PushFont(T().regular, S(13));
        ImVec2 ts = ImGui::CalcTextSize(msg.c_str(), nullptr, false, tw);
        ImGui::PopFont();
        Icon(dl, ImVec2(c.x, c.y - ts.y * 0.5f - S(28)), S(34), White(0.18f), ICON_MAP);
        Text(dl, T().regular, 13, ImVec2(std::floor(c.x - ts.x * 0.5f), std::floor(c.y - ts.y * 0.5f)), White(0.6f), msg.c_str(), tw);
    }
    dl->AddRect(a, b, White(0.07f));
    std::string foot = !status.empty() ? status + "\xe2\x80\xa6"
                       : pic.tex       ? pic.note + "  Scroll to zoom, drag to move, click a marker or a place in the list."
                                       : std::string();
    TextFit(dl, T().regular, 12, ImVec2(a.x, b.y + S(12)), cw, White(0.5f), foot.c_str());
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(w, b.y + S(32) - o.y));
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
        rest.swap(own);
        feat.clear();
        if (rest.empty()) {
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
    bool ukGame = std::strcmp(g.id, "ultrakill") == 0;
    bool gdGame = std::strcmp(g.id, "geometrydash") == 0;
    bool bl3Game = std::strcmp(g.id, "borderlands3") == 0;
    bool mc = std::strcmp(g.id, "minecraft") == 0;
    const inject::Paths& gp = sn1 ? sn1Paths : sotf ? sotfPaths : ukGame ? ukPaths : gdGame ? gdPaths : bl3Game ? bl3Paths : paths;
    inject::GameState gs = sn1 ? sn1State : sotf ? sotfState : ukGame ? ukState : gdGame ? gdState : bl3Game ? bl3State : state;
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
            injectGame = sn1 ? 1 : sotf ? 2 : ukGame ? 3 : gdGame ? 4 : bl3Game ? 5 : 0;
        }
    } else {
        if (Button("main", "Start game", bp, bs, Btn::Primary, ICON_PLAY, true)) startNow = true;
    }

    float y = b.y + S(16);
    if (mc) {
        DrawMinecraftCards(y, w, o);
        return;
    }
    if ((sn2 || sn1 || sotf || ukGame || gdGame || bl3Game) && g.ready) {
        DrawGameCards(y, w, o, gp, gs,
                      sn1 ? "Subnautica (Steam)" : sotf ? "Sons Of The Forest (Steam)" : ukGame ? "ULTRAKILL (Steam)" : gdGame ? "Geometry Dash (Steam)" : bl3Game ? "Borderlands 3 (Steam)" : "Subnautica2 (Steam)",
                      sn1 ? "fathom_sn1.dll" : sotf ? "fathom_sotf.dll" : ukGame ? "fathom_ultrakill.dll" : gdGame ? "fathom_gd.dll" : bl3Game ? "fathom_bl3.dll" : "fathom.dll");
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
