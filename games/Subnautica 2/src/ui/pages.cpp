#include "ui/menu.h"
#include <complex>
#include "ui/icons.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace ui {

using core::Feature;
using core::Kind;
using core::MarkerType;

namespace {

std::string Lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

void Empty(ImDrawList* dl, ImVec2 a, ImVec2 size, const char* icon, const char* text) {
    ImVec2 ts = Measure(T().regular, 12.5f, text);
    Icon(dl, ImVec2(a.x + size.x * 0.5f, a.y + size.y * 0.5f - S(16)), S(22), White(0.22f), icon);
    Text(dl, T().regular, 12.5f, ImVec2(a.x + (size.x - ts.x) * 0.5f, a.y + size.y * 0.5f + S(4)), White(0.42f), text);
}

void Panel(ImDrawList* dl, ImVec2 a, ImVec2 size) {
    dl->AddRectFilled(a, ImVec2(a.x + size.x, a.y + size.y), White(0.035f), S(12));
    dl->AddRect(a, ImVec2(a.x + size.x, a.y + size.y), White(0.05f), S(12));
}

void Label(ImDrawList* dl, ImVec2 pos, const char* text) {
    Text(dl, T().semibold, 10.5f, pos, White(0.42f), text);
}

const char* CategoryIcon(const std::string& c) {
    if (c == "Resources") return ICON_LAYERS;
    if (c == "Food") return ICON_BOX;
    if (c == "Healing") return ICON_HEART_PULSE;
    if (c == "Tools") return ICON_HAMMER;
    if (c == "Equipment") return ICON_SHIELD_CHECK;
    if (c == "Creatures") return ICON_WAVES;
    if (c == "Plants") return ICON_SPARKLES;
    return ICON_PACKAGE;
}

const char* MarkerIcon(MarkerType t) {
    switch (t) {
    case MarkerType::Vehicle: return ICON_SHIP;
    case MarkerType::Base: return ICON_HOUSE;
    case MarkerType::Wreck: return ICON_ANCHOR;
    case MarkerType::DataBox: return ICON_BOX;
    default: return ICON_NAVIGATION;
    }
}

}

void Menu::Teleport(float x, float y, float z, bool useZ, const std::string& name) {
    if (!m.cheatsAllowed()) {
        Notify("Cheats are off: you are not the host", ICON_USERS);
        return;
    }
    if (!m.teleport) {
        Notify("Teleport is not available", ICON_LOCK);
        return;
    }
    m.teleport(x, y, z, useZ);
    Notify("Teleported to " + name, ICON_MAP_PIN);
}

void Menu::SpawnerPage() {
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec2 av = ImGui::GetContentRegionAvail();
    static const char* seg[] = {"Spawner", "Map"};
    if (Segmented("seg", seg, 2, &spawnSub, o, S(200))) SetAnim(ImGui::GetID("subfade"), 0.0f);
    float fade = Ease(ImGui::GetID("subfade"), 1.0f, 14.0f, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
    ImVec2 b(o.x, o.y + S(52));
    if (spawnSub == 0) SpawnerView(b, av.x, av.y - S(52));
    else MapView(b, av.x, av.y - S(52));
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(av);
}

void Menu::SpawnerView(ImVec2 b, float W, float H) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool allowed = m.cheatsAllowed();
    float rw = S(248), gap = S(16), lw = W - rw - gap;

    if (Input("search", &search, "Search items", b, lw, ICON_SEARCH)) selected = -1;

    float cx = b.x, cy = b.y + S(48);
    auto chip = [&](const std::string& name) {
        float cw = ChipWidth(name.c_str());
        if (cx + cw > b.x + lw) {
            cx = b.x;
            cy += S(36);
        }
        if (Chip(name.c_str(), name.c_str(), ImVec2(cx, cy), category == name)) {
            category = name;
            selected = -1;
        }
        cx += cw + S(8);
    };
    chip("All");
    for (const std::string& c : m.categories) chip(c);
    {
        const char* lbl = "Show unreleased";
        float cw = ChipWidth(lbl);
        if (cx + cw > b.x + lw) {
            cx = b.x;
            cy += S(36);
        }
        if (Chip("unrel", lbl, ImVec2(cx, cy), showHidden)) {
            showHidden = !showHidden;
            selected = -1;
        }
    }

    std::string q = Lower(search);
    std::vector<int> rows;
    for (int i = 0; i < (int)m.items.size(); ++i) {
        const core::Item& it = m.items[i];
        if (it.hidden && !showHidden) continue;
        if (category != "All" && it.category != category) continue;
        if (!q.empty() && Lower(it.name).find(q) == std::string::npos && Lower(it.id).find(q) == std::string::npos) continue;
        rows.push_back(i);
    }

    ImVec2 lp(b.x, cy + S(40));
    ImVec2 ls(lw, b.y + H - lp.y);
    Panel(dl, lp, ls);
    if (rows.empty()) {
        Empty(dl, lp, ls, ICON_PACKAGE, m.items.empty() ? "The item list loads once you are in a save" : "No items match");
    } else {
        ImGui::SetCursorScreenPos(ImVec2(lp.x + S(6), lp.y + S(6)));
        ImGui::BeginChild("items", ImVec2(ls.x - S(12), ls.y - S(12)), 0, ImGuiWindowFlags_NoBackground);
        ImDrawList* cl = ImGui::GetWindowDrawList();
        float rh = S(36), rwid = ImGui::GetContentRegionAvail().x;
        ImGuiListClipper clip;
        clip.Begin((int)rows.size(), rh);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                int idx = rows[r];
                const core::Item& it = m.items[idx];
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImGui::PushID(idx);
                if (ImGui::InvisibleButton("i", ImVec2(rwid, rh))) selected = idx;
                bool hov = ImGui::IsItemHovered();
                if (hov && ImGui::IsMouseDoubleClicked(0) && m.spawn && allowed) {
                    m.spawn(it, amount);
                    Notify("Spawned " + std::to_string(amount) + " x " + it.name, ICON_PACKAGE);
                }
                float hv = Ease(ImGui::GetID("h"), hov ? 1.0f : 0.0f, 18.0f);
                float sv = Ease(ImGui::GetID("s"), selected == idx ? 1.0f : 0.0f, 18.0f);
                ImGui::PopID();
                ImVec2 rq(rp.x + rwid - S(6), rp.y + rh - S(2));
                if (sv > 0.01f) cl->AddRectFilled(rp, rq, Accent(0.12f * sv), S(8));
                else if (hv > 0.01f) cl->AddRectFilled(rp, rq, White(0.04f * hv), S(8));
                ImVec2 cs = Measure(T().regular, 11.5f, it.category.c_str());
                float tx = rp.x + S(44);
                auto ic = m.icons.find(it.id);
                if (m.iconTexture && ic != m.icons.end()) {
                    const auto& u = ic->second;
                    cl->AddImage((ImTextureID)m.iconTexture, ImVec2(rp.x + S(8), rp.y + S(3)), ImVec2(rp.x + S(36), rp.y + S(31)),
                                 ImVec2(u[0], u[1]), ImVec2(u[2], u[3]));
                } else {
                    cl->AddRectFilled(ImVec2(rp.x + S(9), rp.y + S(4)), ImVec2(rp.x + S(35), rp.y + S(30)), Accent(0.10f), S(7));
                    Icon(cl, ImVec2(rp.x + S(22), rp.y + S(17)), S(14), Accent(0.85f), CategoryIcon(it.category));
                }
                TextFit(cl, T().medium, 13, ImVec2(tx, rp.y + S(9)), rwid - cs.x - S(48) - (tx - rp.x - S(12)),
                        Mix(ImVec4(1, 1, 1, 0.9f), T().accent, sv), it.name.c_str());
                Text(cl, T().regular, 11.5f, ImVec2(rq.x - S(12) - cs.x, rp.y + S(10)), White(0.38f), it.category.c_str());
            }
        }
        ImGui::EndChild();
    }

    ImVec2 rp(b.x + lw + gap, b.y);
    Panel(dl, rp, ImVec2(rw, H));
    float x = rp.x + S(16), y = rp.y + S(16), iw = rw - S(32);
    Label(dl, ImVec2(x, y), "SELECTED");
    y += S(22);
    const core::Item* sel = selected >= 0 && selected < (int)m.items.size() ? &m.items[selected] : nullptr;
    if (sel && m.iconTexture) {
        auto ic = m.icons.find(sel->id);
        if (ic != m.icons.end()) {
            const auto& u = ic->second;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x + S(64), y + S(64)), White(0.04f), S(10));
            dl->AddImage((ImTextureID)m.iconTexture, ImVec2(x + S(4), y + S(4)), ImVec2(x + S(60), y + S(60)), ImVec2(u[0], u[1]),
                         ImVec2(u[2], u[3]));
            y += S(74);
        }
    }
    TextFit(dl, T().semibold, 15, ImVec2(x, y), iw, White(sel ? 0.96f : 0.4f), sel ? sel->name.c_str() : "Nothing selected");
    y += S(22);
    TextFit(dl, T().regular, 11.5f, ImVec2(x, y), iw, White(0.4f), sel ? sel->id.c_str() : "Pick an item from the list");
    y += S(32);

    Label(dl, ImVec2(x, y), "AMOUNT");
    y += S(22);
    int stepAmt = ImGui::GetIO().KeyShift ? 10 : 1;
    if (IconButton("minus", ICON_MINUS, ImVec2(x, y), S(36))) amount -= stepAmt;
    if (IconButton("plus", ICON_PLUS, ImVec2(x + iw - S(36), y), S(36))) amount += stepAmt;
    amount = std::clamp(amount, 1, 999);
    dl->AddRectFilled(ImVec2(x + S(44), y), ImVec2(x + iw - S(44), y + S(36)), White(0.05f), S(8));
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", amount);
    ImVec2 ts = Measure(T().semibold, 14, buf);
    Text(dl, T().semibold, 14, ImVec2(x + (iw - ts.x) * 0.5f, y + (S(36) - ts.y) * 0.5f), White(0.95f), buf);
    y += S(44);
    static const int quick[] = {1, 5, 10, 50};
    float qw = (iw - S(8) * 3) / 4.0f;
    for (int i = 0; i < 4; ++i) {
        std::snprintf(buf, sizeof(buf), "%d", quick[i]);
        if (Chip(buf, buf, ImVec2(x + i * (qw + S(8)), y), amount == quick[i], qw)) amount = quick[i];
    }
    y += S(44);
    if (Button("spawn", "Spawn", ImVec2(x, y), ImVec2(iw, S(40)), Btn::Primary, ICON_PACKAGE, sel && m.spawn && allowed)) {
        m.spawn(*sel, amount);
        Notify("Spawned " + std::to_string(amount) + " x " + sel->name, ICON_PACKAGE);
    }

    float by = rp.y + H - S(16) - S(36) * 2 - S(8);
    dl->AddLine(ImVec2(x, by - S(16)), ImVec2(x + iw, by - S(16)), White(0.06f));
    if (Button("allres", "Add all resources", ImVec2(x, by), ImVec2(iw, S(36)), Btn::Ghost, ICON_LAYERS, m.addAllResources && allowed)) {
        m.addAllResources();
        Notify("Added all resources", ICON_LAYERS);
    }
    if (Button("dupe", "Duplicate held item", ImVec2(x, by + S(44)), ImVec2(iw, S(36)), Btn::Ghost, ICON_COPY,
               m.duplicateHeld && allowed)) {
        m.duplicateHeld();
        Notify("Duplicated held item", ICON_COPY);
    }
}

void Menu::MapView(ImVec2 b, float W, float H) {
    m.mapVisible = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGuiIO& io = ImGui::GetIO();
    float rw = S(248), gap = S(16), lw = W - rw - gap;
    const core::Marker* me = m.player();
    if (!mapInit) {
        mapInit = true;
        mapCx = me ? me->x : 0;
        mapCy = me ? me->y : 0;
        mapZoom = 0.22f;
    }
    float zoom = mapZoom * T().scale;
    ImVec2 a = b, q(b.x + lw, b.y + H);
    ImVec2 c((a.x + q.x) * 0.5f, (a.y + q.y) * 0.5f);
    dl->AddRectFilled(a, q, Black(0.30f), S(12));

    ImGui::SetCursorScreenPos(a);
    ImGui::InvisibleButton("map", ImVec2(lw, H));
    bool hov = ImGui::IsItemHovered();
    bool act = ImGui::IsItemActive();
    if (ImGui::IsItemActivated()) mapDragged = false;
    if (act && ImGui::IsMouseDragging(0, S(4))) {
        mapDragged = true;
        mapCx -= io.MouseDelta.x / zoom;
        mapCy -= io.MouseDelta.y / zoom;
    }
    if (hov && io.MouseWheel != 0.0f) {
        float wx = mapCx + (io.MousePos.x - c.x) / zoom, wy = mapCy + (io.MousePos.y - c.y) / zoom;
        mapZoom = std::clamp(mapZoom * std::pow(1.18f, io.MouseWheel), 0.02f, 4.0f);
        zoom = mapZoom * T().scale;
        mapCx = wx - (io.MousePos.x - c.x) / zoom;
        mapCy = wy - (io.MousePos.y - c.y) / zoom;
    }
    auto toScreen = [&](float wx, float wy) { return ImVec2(c.x + (wx - mapCx) * zoom, c.y + (wy - mapCy) * zoom); };

    using cf = std::complex<float>;
    cf ca(2.0f, 0.0f), cb(0.0f, 0.0f);
    bool hasImage = m.mapImageTexture && m.mapImageW > 0 && m.mapImageH > 0;
    if (hasImage) {
        cf z0(m.calibU[0], m.calibV[0]), z1(m.calibU[1], m.calibV[1]);
        cf w0(m.calibX[0], m.calibY[0]), w1(m.calibX[1], m.calibY[1]);
        if (m.calibPoints >= 2 && std::abs(z1 - z0) > 1.0f) {
            ca = (w1 - w0) / (z1 - z0);
            cb = w0 - ca * z0;
        } else if (m.calibPoints == 1) {
            cb = w0 - ca * z0;
        } else {
            cf center = me ? cf(me->x, me->y) : cf(0, 0);
            cb = center - ca * cf(m.mapImageW * 0.5f, m.mapImageH * 0.5f);
        }
    }
    auto imgToScreen = [&](float u, float v) {
        cf w = ca * cf(u, v) + cb;
        return toScreen(w.real(), w.imag());
    };

    dl->PushClipRect(a, q, true);
    if (m.mapTexture && m.mapMaxX > m.mapMinX) {
        dl->AddImage((ImTextureID)m.mapTexture, toScreen(m.mapMinX, m.mapMinY), toScreen(m.mapMaxX, m.mapMaxY));
    } else {
        ImVec2 ts = Measure(T().regular, 12, "The map fills in as you explore");
        Text(dl, T().regular, 12, ImVec2(c.x - ts.x * 0.5f, a.y + S(18)), White(0.4f), "The map fills in as you explore");
    }
    static const float steps[] = {10, 25, 50, 100, 250, 500, 1000, 2500, 5000};
    float step = steps[8];
    for (float s : steps)
        if (s * zoom >= S(56)) {
            step = s;
            break;
        }
    float wx0 = mapCx - (c.x - a.x) / zoom, wx1 = mapCx + (q.x - c.x) / zoom;
    float wy0 = mapCy - (c.y - a.y) / zoom, wy1 = mapCy + (q.y - c.y) / zoom;
    for (float gx = std::floor(wx0 / step) * step; gx <= wx1; gx += step) {
        float sx = std::floor(toScreen(gx, 0).x) + 0.5f;
        dl->AddLine(ImVec2(sx, a.y), ImVec2(sx, q.y), White(gx == 0 ? 0.09f : 0.035f));
    }
    for (float gy = std::floor(wy0 / step) * step; gy <= wy1; gy += step) {
        float sy = std::floor(toScreen(0, gy).y) + 0.5f;
        dl->AddLine(ImVec2(a.x, sy), ImVec2(q.x, sy), White(gy == 0 ? 0.09f : 0.035f));
    }

    const core::Marker* hot = nullptr;
    float best = S(14);
    for (const core::Marker& k : m.markers) {
        if (k.type == MarkerType::Player || !showType[(int)k.type]) continue;
        ImVec2 sp = toScreen(k.x, k.y);
        float d = std::hypot(sp.x - io.MousePos.x, sp.y - io.MousePos.y);
        if (hov && d < best) {
            best = d;
            hot = &k;
        }
    }
    for (const core::Marker& k : m.markers) {
        if (k.type == MarkerType::Player || !showType[(int)k.type]) continue;
        ImVec2 sp = toScreen(k.x, k.y);
        bool h = hot == &k;
        dl->AddCircleFilled(sp, S(12), h ? Accent(0.20f) : Bg(0.85f), 24);
        dl->AddCircle(sp, S(12), h ? Accent(0.8f) : White(0.14f), 24);
        Icon(dl, sp, S(13), h ? Accent() : White(0.82f), MarkerIcon(k.type));
    }
    if (me) {
        ImVec2 sp = toScreen(me->x, me->y);
        float cs = std::cos(me->yaw), sn = std::sin(me->yaw);
        auto rot = [&](float x, float y) { return ImVec2(sp.x + (x * cs - y * sn), sp.y + (x * sn + y * cs)); };
        dl->AddCircleFilled(sp, S(16), Accent(0.14f), 32);
        ImVec2 pts[4] = {rot(S(9), 0), rot(-S(6), S(6)), rot(-S(2.5f), 0), rot(-S(6), -S(6))};
        dl->AddConcavePolyFilled(pts, 4, Accent());
    }
    float bar = step * zoom;
    ImVec2 sb(a.x + S(16), q.y - S(18));
    dl->AddLine(sb, ImVec2(sb.x + bar, sb.y), White(0.5f), 1.5f);
    dl->AddLine(ImVec2(sb.x, sb.y - S(4)), ImVec2(sb.x, sb.y + S(1)), White(0.5f), 1.5f);
    dl->AddLine(ImVec2(sb.x + bar, sb.y - S(4)), ImVec2(sb.x + bar, sb.y + S(1)), White(0.5f), 1.5f);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.0f m", step);
    Text(dl, T().medium, 11, ImVec2(sb.x + bar + S(8), sb.y - S(8)), White(0.55f), buf);
    TextRight(dl, T().regular, 11, ImVec2(q.x - S(14), q.y - S(24)), White(0.36f), "Click to teleport  \xc2\xb7  drag to move  \xc2\xb7  scroll to zoom");
    if (m.mapTexture && m.mapMaxX > m.mapMinX) {
        std::string photo = m.mapPhotos > 0 ? "Photo map  \xc2\xb7  " + std::to_string(m.mapPhotos) + " pieces here, more are added while this menu is open"
                                             : "Pictures of the seabed are taken while this menu is open";
        Text(dl, T().regular, 11, ImVec2(a.x + S(16), a.y + S(14)), White(0.42f), photo.c_str());
    }
    dl->PopClipRect();
    dl->AddRect(a, q, White(0.06f), S(12));

    if (hot) {
        float d = me ? std::hypot(hot->x - me->x, hot->y - me->y) : 0;
        std::snprintf(buf, sizeof(buf), "%s  \xc2\xb7  %.0f m", hot->name.c_str(), d);
        Tip(buf);
    }
    if (ImGui::IsItemDeactivated() && !mapDragged && hov) {
        float wx = mapCx + (io.MousePos.x - c.x) / zoom, wy = mapCy + (io.MousePos.y - c.y) / zoom;
        if (hot) {
            Teleport(hot->x, hot->y, hot->z, true, hot->name);
        } else {
            Teleport(wx, wy, 0, false, "map point");
        }
    }

    ImVec2 rp(b.x + lw + gap, b.y);
    Panel(dl, rp, ImVec2(rw, H));
    float x = rp.x + S(16), y = rp.y + S(16), iw = rw - S(32);
    Label(dl, ImVec2(x, y), "SHOW");
    y += S(22);
    static const char* names[] = {"", "Vehicles", "Bases", "Signals", "Data boxes"};
    float cw = (iw - S(8)) * 0.5f;
    for (int i = 1; i <= 4; ++i) {
        ImVec2 cp(x + ((i - 1) % 2) * (cw + S(8)), y + ((i - 1) / 2) * S(36));
        if (Chip(names[i], names[i], cp, showType[i], cw)) showType[i] = !showType[i];
    }
    y += S(84);
    Label(dl, ImVec2(x, y), "SAVED LOCATIONS");
    y += S(22);

    float bottom = rp.y + H - S(16);
    bool scanning = m.mapScanning;
    float scanH = scanning ? S(36) + S(34) : S(36);
    float undoY = bottom - scanH - S(8) - S(36);
    float ny = undoY - S(8) - S(36);
    float listH = ny - S(12) - y;
    if (m.locations.empty()) {
        Empty(dl, ImVec2(x, y), ImVec2(iw, listH), ICON_MAP_PIN, "No saved locations yet");
    } else {
        ImGui::SetCursorScreenPos(ImVec2(x - S(8), y));
        ImGui::BeginChild("locs", ImVec2(iw + S(16), listH), 0, ImGuiWindowFlags_NoBackground);
        ImDrawList* cl = ImGui::GetWindowDrawList();
        float rwid = ImGui::GetContentRegionAvail().x;
        int remove = -1;
        for (int i = 0; i < (int)m.locations.size(); ++i) {
            const core::Location& l = m.locations[i];
            ImVec2 p = ImGui::GetCursorScreenPos();
            ImGui::PushID(i);
            ImGui::SetNextItemAllowOverlap();
            bool go = ImGui::InvisibleButton("l", ImVec2(rwid, S(36)));
            bool rh = ImGui::IsItemHovered() || ImGui::IsMouseHoveringRect(p, ImVec2(p.x + rwid, p.y + S(36)));
            float hv = Ease(ImGui::GetID("h"), rh ? 1.0f : 0.0f, 18.0f);
            if (hv > 0.01f) cl->AddRectFilled(p, ImVec2(p.x + rwid, p.y + S(34)), White(0.04f * hv), S(8));
            Icon(cl, ImVec2(p.x + S(16), p.y + S(17)), S(14), Accent(0.9f), ICON_MAP_PIN);
            TextFit(cl, T().medium, 12.5f, ImVec2(p.x + S(30), p.y + S(9)), rwid - S(70), White(0.9f), l.name.c_str());
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * hv);
            bool del = hv > 0.05f && IconButton("d", ICON_TRASH_2, ImVec2(p.x + rwid - S(32), p.y + S(3)), S(28), true, true);
            ImGui::PopStyleVar();
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + S(36)));
            if (del) remove = i;
            else if (go) Teleport(l.x, l.y, l.z, true, l.name);
        }
        ImGui::Dummy(ImVec2(1, 1));
        ImGui::EndChild();
        if (remove >= 0) {
            Notify("Removed " + m.locations[remove].name, ICON_TRASH_2);
            m.locations.erase(m.locations.begin() + remove);
        }
    }

    Input("locname", &locName, "Name this spot", ImVec2(x, ny), iw - S(44));
    if (IconButton("addloc", ICON_PLUS, ImVec2(x + iw - S(36), ny), S(36), me != nullptr)) {
        std::string name = locName.empty() ? "Location " + std::to_string(m.locations.size() + 1) : locName;
        m.locations.push_back({name, me->x, me->y, me->z});
        locName.clear();
        Notify("Saved " + name, ICON_MAP_PIN);
    }
    if (Button("undo", "Undo teleport", ImVec2(x, undoY), ImVec2(iw, S(36)), Btn::Ghost, ICON_UNDO_2,
               m.undoTeleport && m.canUndoTeleport && !scanning)) {
        m.undoTeleport();
        Notify("Back where you were", ICON_UNDO_2);
    }
    if (scanning) {
        float sy = bottom - scanH;
        TextFit(dl, T().regular, 11.5f, ImVec2(x, sy), iw, White(0.62f), m.mapScanText.c_str());
        float py = sy + S(20);
        float fill = iw * std::clamp(m.mapScanProgress, 0.0f, 1.0f);
        dl->AddRectFilled(ImVec2(x, py), ImVec2(x + iw, py + S(4)), White(0.08f), S(2));
        if (fill > 0.5f) dl->AddRectFilled(ImVec2(x, py), ImVec2(x + fill, py + S(4)), Accent(), S(2));
        if (Button("scanstop", "Stop scan", ImVec2(x, bottom - S(36)), ImVec2(iw, S(36)), Btn::Danger, ICON_X, (bool)m.mapScan)) {
            m.mapScan(false);
            Notify("Stopping the map scan", ICON_X);
        }
    } else if (Button("scan", "Scan whole map", ImVec2(x, bottom - S(36)), ImVec2(iw, S(36)), Btn::Tint, ICON_MAP,
                      m.mapScan && m.cheatsAllowed() && m.inWorld)) {
        Ask("Scan the whole map?",
            "A camera flies over the whole world and takes a picture of every part. Your character is frozen and can't be hurt while it "
            "runs. It takes about 10 to 30 minutes, and you can stop it at any time.",
            "Start scan", [this] {
                if (m.mapScan) m.mapScan(true);
                Notify("Map scan started", ICON_MAP);
            });
    }
}

void Menu::HubPage() {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    float W = ImGui::GetContentRegionAvail().x;
    bool allowed = m.cheatsAllowed();
    static const char* seg[] = {"My bases", "Shared"};
    if (Segmented("seg", seg, 2, &hubSub, o, S(200))) SetAnim(ImGui::GetID("subfade"), 0.0f);
    float fade = Ease(ImGui::GetID("subfade"), 1.0f, 14.0f, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
    float y = o.y + S(52);
    char buf[160];

    auto baseRow = [&](const core::BaseInfo& bi, int i, bool shared) {
        ImVec2 p(o.x, y);
        float h = S(64);
        Panel(dl, p, ImVec2(W, h));
        dl->AddRectFilled(ImVec2(p.x + S(14), p.y + S(14)), ImVec2(p.x + S(50), p.y + S(50)), Accent(0.10f), S(10));
        Icon(dl, ImVec2(p.x + S(32), p.y + S(32)), S(17), Accent(), shared ? ICON_CLOUD_DOWNLOAD : ICON_HOUSE);
        if (shared) std::snprintf(buf, sizeof(buf), "by %s  \xc2\xb7  %d pieces", bi.author.c_str(), bi.pieces);
        else std::snprintf(buf, sizeof(buf), "%d pieces  \xc2\xb7  %s", bi.pieces, bi.date.c_str());
        float tw = W - S(64) - S(260);
        TextFit(dl, T().medium, 14, ImVec2(p.x + S(64), p.y + S(14)), tw, White(0.95f), bi.name.c_str());
        TextFit(dl, T().regular, 12, ImVec2(p.x + S(64), p.y + S(35)), tw, White(0.46f), buf);
        ImGui::PushID(shared ? 1000 + i : i);
        float bx = p.x + W - S(14), byy = p.y + S(16);
        if (shared) {
            if (bi.downloaded) {
                ImVec2 ts = Measure(T().medium, 12.5f, "Saved");
                Icon(dl, ImVec2(bx - ts.x - S(14), p.y + h * 0.5f), S(14), Accent(), ICON_CHECK);
                Text(dl, T().medium, 12.5f, ImVec2(bx - ts.x, p.y + (h - ts.y) * 0.5f), Accent(), "Saved");
            } else if (Button("dl", "Download", ImVec2(bx - S(116), byy), ImVec2(S(116), S(32)), Btn::Tint, ICON_DOWNLOAD,
                              (bool)m.downloadBase)) {
                m.downloadBase(bi);
                Notify("Downloaded " + bi.name, ICON_DOWNLOAD);
            }
        } else {
            if (IconButton("del", ICON_TRASH_2, ImVec2(bx - S(32), byy), S(32), (bool)m.deleteBase, true)) {
                core::BaseInfo copy = bi;
                Ask("Delete this base file?", "\"" + bi.name + "\" is removed from your saved bases. Bases already placed in the game stay.",
                    "Delete", [this, copy] {
                        if (m.deleteBase) m.deleteBase(copy);
                        Notify("Deleted " + copy.name, ICON_TRASH_2);
                    });
            }
            if (Button("place", "Place copy", ImVec2(bx - S(40) - S(96), byy), ImVec2(S(96), S(32)), Btn::Tint, nullptr,
                       m.placeBase && allowed && m.inWorld)) {
                core::BaseInfo copy = bi;
                Ask("Place a copy of \"" + bi.name + "\"?",
                    "Experimental: the copy appears 30 m in front of you. Your save files are backed up first. Try it in a test save.",
                    "Place", [this, copy] {
                        if (m.backupSaves) {
                            std::string path = m.backupSaves();
                            if (path.empty()) {
                                Notify("Backup failed, nothing was placed", ICON_TRIANGLE_ALERT);
                                return;
                            }
                            m.lastBackup = path;
                        }
                        m.placeBase(copy);
                        Notify("Placing " + copy.name + "\xe2\x80\xa6", ICON_HOUSE);
                    });
            }
            if (Button("prev", "Preview", ImVec2(bx - S(40) - S(96) - S(8) - S(88), byy), ImVec2(S(88), S(32)), Btn::Ghost, nullptr,
                       m.previewBase && allowed)) {
                m.previewBase(bi);
                Notify("Ghost preview of " + bi.name, ICON_EYE);
            }
        }
        ImGui::PopID();
        y += h + S(8);
    };

    if (hubSub == 0) {
        float bw = S(176);
        Input("basename", &baseName, "Name for the base near you", ImVec2(o.x, y), W - bw - S(8), ICON_HOUSE);
        if (Button("savebase", "Save nearby base", ImVec2(o.x + W - bw, y), ImVec2(bw, S(36)), Btn::Primary, ICON_SAVE,
                   m.saveBase && m.inWorld)) {
            std::string name = baseName.empty() ? "Base " + std::to_string(m.localBases.size() + 1) : baseName;
            m.saveBase(name);
            baseName.clear();
            Notify("Saved " + name, ICON_SAVE);
        }
        y += S(44);
        Text(dl, T().regular, 12, ImVec2(o.x + S(2), y), White(0.42f), "Saves the base you are in, or the closest one within 150 m (rooms and corridors).");
        y += S(28);

        Panel(dl, ImVec2(o.x, y), ImVec2(W, S(52) + S(8)));
        if (ToggleRow("legs", ImVec2(o.x + S(8), y + S(4)), W - S(16), "Foundation legs reach the seabed",
                      "Legs stretch down to the ground when a base is placed", &m.legsToSeabed, true))
            Notify(m.legsToSeabed ? "Legs reach the seabed" : "Legs keep their saved length", ICON_CHECK);
        y += S(60) + S(12);

        if (m.ghostActive) {
            ImVec2 p(o.x, y);
            dl->AddRectFilled(p, ImVec2(p.x + W, p.y + S(52)), Accent(0.10f), S(12));
            dl->AddRect(p, ImVec2(p.x + W, p.y + S(52)), Accent(0.35f), S(12));
            Icon(dl, ImVec2(p.x + S(24), p.y + S(26)), S(16), Accent(), ICON_EYE);
            std::string t = "Previewing \"" + m.ghostName + "\". Move to where you want it.";
            TextFit(dl, T().medium, 13, ImVec2(p.x + S(44), p.y + S(17)), W - S(260), Accent(), t.c_str());
            if (Button("gplace", "Place here", ImVec2(p.x + W - S(10) - S(100), p.y + S(10)), ImVec2(S(100), S(32)), Btn::Primary, nullptr,
                       (bool)m.confirmGhost)) {
                m.confirmGhost();
                Notify("Placed " + m.ghostName, ICON_HOUSE);
            }
            if (Button("gcancel", "Cancel", ImVec2(p.x + W - S(10) - S(100) - S(8) - S(76), p.y + S(10)), ImVec2(S(76), S(32)), Btn::Ghost,
                       nullptr, (bool)m.cancelGhost))
                m.cancelGhost();
            y += S(64);
        }
        if (m.localBases.empty()) {
            Empty(dl, ImVec2(o.x, y), ImVec2(W, S(140)), ICON_HOUSE, "No saved bases yet");
            y += S(140);
        }
        for (int i = 0; i < (int)m.localBases.size(); ++i) baseRow(m.localBases[i], i, false);
    } else {
        float bw = S(108);
        dl->AddRectFilled(ImVec2(o.x, y), ImVec2(o.x + W - bw - S(8), y + S(36)), White(0.05f), S(8));
        Icon(dl, ImVec2(o.x + S(18), y + S(18)), S(15), White(0.45f), ICON_GITHUB);
        TextFit(dl, T().regular, 12.5f, ImVec2(o.x + S(36), y + S(10)), W - bw - S(60), White(0.6f), m.hubRepo.c_str());
        if (Button("refresh", "Refresh", ImVec2(o.x + W - bw, y), ImVec2(bw, S(36)), Btn::Ghost, ICON_REFRESH_CW, (bool)m.refreshShared)) {
            m.refreshShared();
            Notify("Shared list refreshed", ICON_REFRESH_CW);
        }
        y += S(52);
        if (m.sharedBases.empty()) {
            Empty(dl, ImVec2(o.x, y), ImVec2(W, S(180)), ICON_CLOUD_DOWNLOAD, "No shared bases loaded. Press Refresh.");
            y += S(180);
        }
        for (int i = 0; i < (int)m.sharedBases.size(); ++i) baseRow(m.sharedBases[i], i, true);
    }
    ImGui::PopStyleVar();
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(ImVec2(W, y - o.y));
}

void Menu::LegitPage() {
    m.legitVisible = true;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImVec2 av = ImGui::GetContentRegionAvail();
    float W = av.x;
    float y = o.y;
    bool allowed = m.cheatsAllowed() && (bool)m.progressAction && m.inWorld;
    char buf[160];

    float oh = S(104);
    Panel(dl, ImVec2(o.x, y), ImVec2(W, oh));
    int done = 0, total = 0;
    for (const core::ProgressStat& s : m.progressStats) {
        done += s.done;
        total += s.total;
    }
    float overall = total ? (float)done / (float)total : 0.0f;
    std::snprintf(buf, sizeof(buf), "%d%%", (int)std::round(overall * 100));
    Text(dl, T().semibold, 10.5f, ImVec2(o.x + S(16), y + S(16)), White(0.42f), "YOUR PROGRESS");
    Text(dl, T().semibold, 26, ImVec2(o.x + S(16), y + S(36)), m.progressStats.empty() ? White(0.3f) : Accent(), m.progressStats.empty() ? "--" : buf);
    Text(dl, T().regular, 11.5f, ImVec2(o.x + S(16), y + S(74)), White(0.42f), m.inWorld ? "of everything found so far" : "Load a save to see it");
    if (m.inWorld && !m.progressStage.empty()) {
        std::string stage = "Showing what fits " + m.progressStage + " and the next step";
        TextRight(dl, T().regular, 11.5f, ImVec2(o.x + W - S(16), y + S(74)), Accent(0.75f), stage.c_str());
    }
    float sx = o.x + S(120), sw = W - S(120) - S(16);
    int ns = (int)m.progressStats.size();
    if (ns > 0) {
        float cw = (sw - S(12) * (ns - 1)) / ns;
        for (int i = 0; i < ns; ++i) {
            const core::ProgressStat& s = m.progressStats[i];
            float px = sx + i * (cw + S(12));
            Text(dl, T().medium, 12.5f, ImVec2(px, y + S(30)), White(0.85f), s.name.c_str());
            std::snprintf(buf, sizeof(buf), "%d / %d", s.done, s.total);
            TextRight(dl, T().regular, 11.5f, ImVec2(px + cw, y + S(31)), White(0.45f), buf);
            float f = s.total ? (float)s.done / (float)s.total : 0.0f;
            ImGui::PushID(i);
            float fa = Ease(ImGui::GetID("pf"), f, 10.0f, f);
            ImGui::PopID();
            dl->AddRectFilled(ImVec2(px, y + S(56)), ImVec2(px + cw, y + S(62)), White(0.08f), S(3));
            if (fa > 0.003f) dl->AddRectFilled(ImVec2(px, y + S(56)), ImVec2(px + cw * fa, y + S(62)), Accent(), S(3));
        }
    }
    y += oh + S(16);

    static const char* seg[] = {"Blueprints", "Locations", "Scans", "Items"};
    if (Segmented("progseg", seg, 4, &progSub, ImVec2(o.x, y), S(380))) {
        SetAnim(ImGui::GetID("progfade"), 0.0f);
        progSearch.clear();
    }
    float iw = W - S(380) - S(12);
    Input("progsearch", &progSearch, "Search", ImVec2(o.x + S(392), y), iw, ICON_SEARCH);
    y += S(48);

    std::vector<core::ProgressEntry>& list = m.progress[progSub];
    std::string q = Lower(progSearch);
    std::vector<int> rows;
    for (int i = 0; i < (int)list.size(); ++i) {
        const core::ProgressEntry& e = list[i];
        if (!q.empty() && Lower(e.name).find(q) == std::string::npos) continue;
        rows.push_back(i);
    }
    std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) {
        if (list[a].done != list[b].done) return !list[a].done;
        return list[a].order < list[b].order;
    });

    ImVec2 lp(o.x, y), ls(W, o.y + av.y - y);
    Panel(dl, lp, ls);
    static const char* emptyText[] = {"Blueprints show up once you are in a save", "Locations show up once you are in a save",
                                      "Scans show up once you are in a save", "Items show up once you are in a save"};
    static const char* kindIcon[] = {ICON_HAMMER, ICON_MAP_PIN, ICON_SEARCH, ICON_PACKAGE};
    static const char* action[] = {"Unlock", "Teleport", "Unlock", "Get"};
    if (rows.empty()) {
        Empty(dl, lp, ls, kindIcon[progSub], list.empty() ? emptyText[progSub] : "Nothing matches");
    } else {
        float fade = Ease(ImGui::GetID("progfade"), 1.0f, 14.0f, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
        ImGui::SetCursorScreenPos(ImVec2(lp.x + S(6), lp.y + S(6)));
        ImGui::BeginChild("proglist", ImVec2(ls.x - S(12), ls.y - S(12)), 0, ImGuiWindowFlags_NoBackground);
        ImDrawList* cl = ImGui::GetWindowDrawList();
        float rh = S(48), rw = ImGui::GetContentRegionAvail().x;
        const core::Marker* me = m.player();
        ImGuiListClipper clip;
        clip.Begin((int)rows.size(), rh);
        while (clip.Step()) {
            for (int r = clip.DisplayStart; r < clip.DisplayEnd; ++r) {
                const core::ProgressEntry& e = list[rows[r]];
                ImVec2 rp = ImGui::GetCursorScreenPos();
                ImGui::PushID(rows[r]);
                ImGui::SetNextItemAllowOverlap();
                ImGui::InvisibleButton("row", ImVec2(rw, rh - S(4)));
                float hv = Ease(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
                if (hv > 0.01f) cl->AddRectFilled(rp, ImVec2(rp.x + rw, rp.y + rh - S(4)), White(0.04f * hv), S(8));
                float dim = e.done ? 0.5f : 1.0f;
                auto ic = m.icons.find(e.icon);
                if (m.iconTexture && !e.icon.empty() && ic != m.icons.end()) {
                    const auto& u = ic->second;
                    cl->AddImage((ImTextureID)m.iconTexture, ImVec2(rp.x + S(8), rp.y + S(6)), ImVec2(rp.x + S(40), rp.y + S(38)),
                                 ImVec2(u[0], u[1]), ImVec2(u[2], u[3]), ImGui::GetColorU32(ImVec4(1, 1, 1, dim)));
                } else {
                    cl->AddRectFilled(ImVec2(rp.x + S(8), rp.y + S(6)), ImVec2(rp.x + S(40), rp.y + S(38)), Accent(0.10f * dim), S(8));
                    Icon(cl, ImVec2(rp.x + S(24), rp.y + S(22)), S(15), Accent(dim), kindIcon[progSub]);
                }
                float tw = rw - S(52) - S(210);
                TextFit(cl, T().medium, 13.5f, ImVec2(rp.x + S(52), rp.y + S(7)), tw, White(0.95f * dim), e.name.c_str());
                std::string detail = e.detail;
                if (progSub == 1 && e.hasPos && me) {
                    std::snprintf(buf, sizeof(buf), "%.0f m away", std::hypot(e.x - me->x, e.y - me->y));
                    detail = detail.empty() ? std::string(buf) : detail + "  \xc2\xb7  " + buf;
                }
                TextFit(cl, T().regular, 11.5f, ImVec2(rp.x + S(52), rp.y + S(26)), tw, White(0.45f * dim), detail.c_str());
                float bx = rp.x + rw - S(8);
                if (e.done && progSub != 1 && progSub != 3) {
                    ImVec2 ts = Measure(T().medium, 12, "Done");
                    Icon(cl, ImVec2(bx - ts.x - S(14), rp.y + S(22)), S(13), Accent(0.8f), ICON_CHECK);
                    Text(cl, T().medium, 12, ImVec2(bx - ts.x, rp.y + S(22) - ts.y * 0.5f), Accent(0.8f), "Done");
                } else if (Button("act", action[progSub], ImVec2(bx - S(100), rp.y + S(6)), ImVec2(S(100), S(32)), Btn::Tint, nullptr,
                                  allowed)) {
                    core::ProgressEntry copy = e;
                    m.progressAction(progSub, copy);
                    Notify(std::string(progSub == 1 ? "Teleporting to " : progSub == 3 ? "Added " : "Unlocking ") + e.name,
                           progSub == 1 ? ICON_MAP_PIN : progSub == 3 ? ICON_CHECK : ICON_REFRESH_CW);
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
    }
    ImGui::SetCursorScreenPos(o);
    ImGui::Dummy(av);
}
void Menu::SettingsPage() {
    Masonry ms = BeginMasonry();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = ms.colW;

    {
        float h = S(38) + S(44) * 2 + S(8);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Menu keys");
        RowBase("kopen", ImVec2(p.x + S(8), y), w - S(16), S(44), "Open / close menu", "", true, nullptr, S(110));
        KeyChip("kopen", ImVec2(p.x + w - S(16), y + S(22)), &m.keyOpen);
        y += S(44);
        RowBase("kunload", ImVec2(p.x + S(8), y), w - S(16), S(44), "Unload menu from the game", "", true, nullptr, S(110));
        KeyChip("kunload", ImVec2(p.x + w - S(16), y + S(22)), &m.keyUnload);
        if (!m.keyOpen) m.keyOpen = ImGuiKey_Insert;
        if (!m.keyUnload) m.keyUnload = ImGuiKey_End;
    }
    {
        float h = S(38) + S(60) * 2 + S(52) + S(8);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Appearance");
        bool active = false;
        SliderRow("scale", ImVec2(p.x + S(8), y), w - S(16), "Menu size", &scaleEdit, 0.8f, 1.4f, 1.0f, 0.05f, "%.0f%%", 100.0f, true, 0.0f,
                  &active);
        if (!active) m.menuScale = scaleEdit;
        y += S(60);
        SliderRow("opacity", ImVec2(p.x + S(8), y), w - S(16), "Background opacity", &m.menuOpacity, 0.5f, 1.0f, 0.78f, 0.01f, "%.0f%%",
                  100.0f, true);
        y += S(60);
        ToggleRow("blur", ImVec2(p.x + S(8), y), w - S(16), "Blur behind the menu", "Real frosted glass, costs a little FPS", &m.blur,
                  m.blurAvailable, m.blurAvailable ? nullptr : "Not built yet (last phase)", !m.blurAvailable);
    }
    {
        float h = S(38) + S(36) + S(42);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Settings file");
        float bw = (w - S(32) - S(16)) / 3.0f;
        if (Button("save", "Save", ImVec2(p.x + S(16), y), ImVec2(bw, S(36)), Btn::Tint, ICON_SAVE))
            Notify(core::SaveSettings(m) ? "Settings saved" : "Could not write the settings file",
                   ICON_SAVE);
        if (Button("load", "Load", ImVec2(p.x + S(16) + bw + S(8), y), ImVec2(bw, S(36)), Btn::Ghost, ICON_FOLDER_OPEN)) {
            bool ok = core::LoadSettings(m);
            SyncFromModel();
            Notify(ok ? "Settings loaded" : "No settings file found", ICON_FOLDER_OPEN);
        }
        if (Button("reset", "Reset", ImVec2(p.x + S(16) + (bw + S(8)) * 2, y), ImVec2(bw, S(36)), Btn::Ghost, ICON_ROTATE_CCW)) {
            Ask("Reset all settings?", "Every cheat is switched off and all keys, sizes and sliders go back to their defaults.", "Reset",
                [this] {
                    core::ResetSettings(m);
                    SyncFromModel();
                    Notify("Settings reset", ICON_ROTATE_CCW);
                });
        }
        TextFit(dl, T().regular, 11.5f, ImVec2(p.x + S(16), y + S(46)), w - S(32), White(0.38f), m.settingsPath.c_str());
    }
    {
        float h = S(38) + S(36) + S(42);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Save files");
        if (Button("backup", "Back up saves now", ImVec2(p.x + S(16), y), ImVec2(w - S(32), S(36)), Btn::Tint, ICON_ARCHIVE,
                   (bool)m.backupSaves)) {
            std::string path = m.backupSaves();
            if (path.empty()) {
                Notify("Backup failed", ICON_TRIANGLE_ALERT);
            } else {
                m.lastBackup = path;
                Notify("Saves backed up", ICON_ARCHIVE);
            }
        }
        std::string last = m.lastBackup.empty() ? "No backup made in this session" : "Last: " + m.lastBackup;
        TextFit(dl, T().regular, 11.5f, ImVec2(p.x + S(16), y + S(46)), w - S(32), White(0.38f), last.c_str());
    }
    {
        int n = 0;
        for (const Feature& f : m.features) n += f.kind == Kind::Toggle && f.counts ? 1 : 0;
        float h = S(38) + n * S(38) + S(8);
        ImVec2 p = ms.Place(h);
        float y = CardBegin(p, w, h, "Feature hotkeys");
        for (Feature& f : m.features) {
            if (f.kind != Kind::Toggle || !f.counts) continue;
            std::string id = "hk_" + f.id;
            RowBase(id.c_str(), ImVec2(p.x + S(8), y), w - S(16), S(38), f.label.c_str(), "", true, nullptr, S(110));
            KeyChip(id.c_str(), ImVec2(p.x + w - S(16), y + S(19)), &f.key);
            y += S(38);
        }
    }
    ms.End();
}

}
