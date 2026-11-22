#include "ui/menu.h"
#include <chrono>
#include "ui/icons.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

using core::Feature;
using core::Kind;
using core::Tab;

namespace {

const float kW = 960, kH = 620, kSide = 208, kTop = 64;

struct TabDef {
    Tab tab;
    const char* name;
    const char* icon;
};

const TabDef kTabs[] = {
    {Tab::Survival, "Survival", ICON_HEART_PULSE}, {Tab::Biomods, "Biomods", ICON_DNA},
    {Tab::Crafting, "Crafting & Base", ICON_HAMMER}, {Tab::World, "World & Vehicles", ICON_GLOBE},
    {Tab::Spawner, "Spawner & Map", ICON_PACKAGE}, {Tab::BaseHub, "Base Hub", ICON_HOUSE},
    {Tab::Legit, "Legit Run", ICON_SHIELD_CHECK}, {Tab::Settings, "Settings", ICON_SETTINGS},
};

const char* TabName(const core::Model& m, Tab t) {
    for (const core::TabLabel& l : m.tabLabels)
        if (l.tab == t && !l.name.empty()) return l.name.c_str();
    return kTabs[(int)t].name;
}

const char* TabIcon(const core::Model& m, Tab t) {
    for (const core::TabLabel& l : m.tabLabels)
        if (l.tab == t && l.icon) return l.icon;
    return kTabs[(int)t].icon;
}

bool Plain(const core::Model& m, Tab t) { return std::find(m.plainTabs.begin(), m.plainTabs.end(), t) != m.plainTabs.end(); }

void Approach(float& v, float target, float speed) {
    v += (target - v) * (1.0f - std::exp(-speed * Dt()));
    if (std::fabs(target - v) < 0.002f) v = target;
}

float Pill(ImDrawList* dl, ImVec2 rightCenter, const char* icon, const char* text, bool accent) {
    ImVec2 ts = Measure(T().medium, 12, text);
    float w = ts.x + S(44), h = S(28);
    ImVec2 a(rightCenter.x - w, rightCenter.y - h * 0.5f);
    ImVec2 b(a.x + w, a.y + h);
    Para(dl, a, b, accent ? Accent(0.14f) : White(0.05f));
    ParaLine(dl, a, b, accent ? Accent(0.45f) : White(0.08f));
    Icon(dl, ImVec2(a.x + S(20), rightCenter.y), S(14), accent ? Accent() : White(0.55f), icon);
    Text(dl, T().medium, 12, ImVec2(a.x + S(31), rightCenter.y - ts.y * 0.5f), accent ? Accent() : White(0.8f), text);
    return w;
}

}

Menu::Menu(core::Model& model) : m(model) {
    scaleEdit = m.menuScale;
}

void Menu::SyncFromModel() {
    scaleEdit = m.menuScale;
}

void Menu::Notify(const std::string& text, const char* icon) {
    toasts.push_back({text, icon ? icon : ICON_INFO, 0.0f, 0.0f, false});
    if (toasts.size() > 5) toasts.erase(toasts.begin());
}

void Menu::Ask(const std::string& title, const std::string& body, const std::string& yes, std::function<void()> fn) {
    confirmTitle = title;
    confirmBody = body;
    confirmYes = yes;
    confirmFn = std::move(fn);
    confirmOpen = true;
}

void Menu::HandleKeys() {
    int frame = ImGui::GetFrameCount();
    if (capture) {
        if (frame == captureFrame) return;
        for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
            ImGuiKey key = (ImGuiKey)k;
            if (key >= ImGuiKey_GamepadStart && key <= ImGuiKey_MouseWheelY) continue;
            if (key >= ImGuiKey_ReservedForModCtrl) continue;
            if (!ImGui::IsKeyPressed(key, false)) continue;
            *capture = key == ImGuiKey_Escape ? 0 : k;
            capture = nullptr;
            skipKeysFrame = frame;
            return;
        }
        if (ImGui::IsMouseClicked(0)) capture = nullptr;
        return;
    }
    if (frame == skipKeysFrame || embedded) return;
    if (ImGui::GetIO().WantTextInput) return;
    if (m.keyOpen && ImGui::IsKeyPressed((ImGuiKey)m.keyOpen, false)) {
        if (!open) {
            open = true;
            focused = true;
        } else if (!focused) {
            focused = true;
        } else {
            open = false;
        }
    }
    if (open && focused && !confirmOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) Unfocus();
    if (m.keyUnload && ImGui::IsKeyPressed((ImGuiKey)m.keyUnload, false)) m.requestUnload = true;
    if (m.loadoutKey && m.giveLoadout && ImGui::IsKeyPressed((ImGuiKey)m.loadoutKey, false)) {
        if (m.loadout.empty()) Notify("Your loadout is empty", ICON_INFO);
        else if (!m.cheatsAllowed()) Notify("Cheats are off: you are not the host", ICON_USERS);
        else
            GuardedSpawn([this] {
                if (m.giveLoadout) m.giveLoadout();
                Notify("Loadout given", ICON_PACKAGE);
            });
    }
    for (Feature& f : m.features) {
        bool action = f.kind == Kind::Action && !f.confirm && f.page.empty();
        if (!f.key || (f.kind != Kind::Toggle && !action) || !ImGui::IsKeyPressed((ImGuiKey)f.key, false)) continue;
        if (f.building) {
            Notify(f.label + " is still being built", ICON_HAMMER);
        } else if (!f.available) {
            Notify(f.label + " is not available", ICON_LOCK);
        } else if (!m.cheatsAllowed()) {
            Notify("Cheats are off: you are not the host", ICON_USERS);
        } else if (action) {
            RunAction(f);
        } else {
            Toggle(f, !f.on);
        }
    }
}

void Menu::Unfocus() {
    focused = false;
    capture = nullptr;
    ImGui::SetWindowFocus(nullptr);
}

void Menu::Frame() {
    PumpBackup();
    float scale = uiScale > 0.0f ? uiScale : m.menuScale;
    if (uiScale <= 0.0f && !embedded) {
        ImVec2 vs = ImGui::GetMainViewport()->Size;
        if (vs.x > 0.0f && vs.y > 0.0f) {
            scale *= std::clamp(vs.y / 1080.0f, 1.0f, 3.0f);
            scale = std::min(scale, std::max(0.5f, std::min(vs.x / (kW + 48.0f), vs.y / (kH + 48.0f))));
        }
    }
    T().scale = scale;
    ApplyStyle();
    m.mapVisible = false;
    m.legitVisible = false;
    if (embedded) {
        open = true;
        focused = true;
    }
    if (open && focused && !embedded && !confirmOpen && ImGui::IsMouseClicked(0) &&
        !ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow))
        Unfocus();
    HandleKeys();
    Approach(openAnim, open ? 1.0f : 0.0f, 16.0f);

    if (openAnim > 0.01f) {
        ImGuiViewport* vp = ImGui::GetMainViewport();
        ImVec2 size = embedded ? vp->Size : ImVec2(S(kW), S(kH));
        ImGui::SetNextWindowSize(size);
        if (embedded) ImGui::SetNextWindowPos(vp->Pos, ImGuiCond_Always);
        else ImGui::SetNextWindowPos(vp->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        float dim = Ease(ImGui::GetID("focusdim"), focused ? 1.0f : 0.55f, 12.0f, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, openAnim * dim);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollWithMouse;
        if (!open || !focused) flags |= ImGuiWindowFlags_NoInputs;
        if (embedded) flags |= ImGuiWindowFlags_NoMove;
        ImGui::Begin("##fathom", nullptr, flags);
        ImVec2 p = ImGui::GetWindowPos();
        windowPos = p;
        windowSize = size;
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 q(p.x + size.x, p.y + size.y);
        if (!embedded) Glow(ImGui::GetBackgroundDrawList(), p, q, 0.0f, ImVec4(0, 0, 0, 1), 0.55f, S(28));
        dl->AddRectFilled(p, q, Bg(embedded ? 1.0f : m.menuOpacity));
        dl->AddRectFilled(p, ImVec2(p.x + S(kSide), q.y), White(0.03f));
        dl->AddLine(ImVec2(p.x + S(kSide), p.y), ImVec2(p.x + S(kSide), q.y), White(0.07f));
        if (!embedded) {
            dl->AddRect(p, q, White(0.08f));
            dl->AddRectFilled(p, ImVec2(q.x, p.y + S(3)), Accent());
        }
        Sidebar(p, size);
        TopBar(p, size);
        Content(p, size);
        ImGui::End();
        ImGui::PopStyleVar(2);
        Confirm();
    }
    Toasts();
    DrawTip();
}

void Menu::Sidebar(ImVec2 p, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float w = S(kSide);
    ImVec2 lp(p.x + S(20), p.y + S(18));
    if (m.logoTexture) {
        dl->AddImage((ImTextureID)m.logoTexture, ImVec2(lp.x - S(2), lp.y - S(2)), ImVec2(lp.x + S(34), lp.y + S(34)));
    } else {
        Para(dl, lp, ImVec2(lp.x + S(34), lp.y + S(32)), Accent());
        Icon(dl, ImVec2(lp.x + S(17), lp.y + S(16)), S(18), White(), ICON_WAVES);
    }
    Upper(dl, T().semibold, 14, ImVec2(lp.x + S(44), lp.y), White(0.97f), "Fathom");
    Text(dl, T().regular, 11, ImVec2(lp.x + S(44), lp.y + S(18)), White(0.45f), embedded ? "Mod loader" : m.gameName.c_str());

    float top = p.y + S(80), th = S(40), gap = S(4);
    struct Entry {
        int kind;
        const TabDef* td;
        const char* name;
        const char* icon;
        int sub = 0;
    };
    std::vector<Entry> list;
    if (backName && onBack) list.push_back({0, nullptr, backName, ICON_UNDO_2});
    if (homePage) {
        list.push_back({1, nullptr, homeName, homeIcon ? homeIcon : ICON_ZAP});
        for (int i = 0; i < (int)homeTabs.size(); ++i) list.push_back({1, nullptr, homeTabs[i].name, homeTabs[i].icon, i + 1});
    }
    int hi = ActiveHome();
    std::vector<Tab> order = m.tabs;
    if (order.empty())
        for (const TabDef& d : kTabs) order.push_back(d.tab);
    for (Tab t : order)
        if (showGameTabs || t == Tab::Settings) list.push_back({2, &kTabs[(int)t], TabName(m, t), TabIcon(m, t)});
    int count = (int)list.size();
    int idx = -1;
    for (int i = 0; i < count; ++i)
        if ((onHome && list[i].kind == 1 && list[i].sub == hi) || (!onHome && list[i].kind == 2 && list[i].td->tab == tab)) idx = i;
    if (idx < 0) {
        for (int i = 0; i < count && idx < 0; ++i)
            if (list[i].kind == 1) idx = i;
        if (idx >= 0) onHome = true;
        else idx = 0;
    }
    float fi = Ease(ImGui::GetID("tabsel"), (float)idx, 18.0f, (float)idx);
    float iy = top + fi * (th + gap);
    Para(dl, ImVec2(p.x + S(12), iy), ImVec2(p.x + w - S(12), iy + th), Accent());

    for (int i = 0; i < count; ++i) {
        const Entry& e = list[i];
        const char* name = e.name;
        const char* icon = e.icon;
        bool sel = i == idx;
        ImVec2 tp(p.x + S(12), top + i * (th + gap));
        ImGui::SetCursorScreenPos(tp);
        ImGui::PushID(i);
        ImGui::PushID(name);
        if (ImGui::InvisibleButton("t", ImVec2(w - S(24), th))) {
            if (e.kind == 0) {
                onBack();
            } else {
                onHome = e.kind == 1;
                if (onHome) homeIndex = e.sub;
                if (e.td) tab = e.td->tab;
            }
        }
        float h = Ease(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
        float a = Ease(ImGui::GetID("a"), sel ? 1.0f : 0.0f, 18.0f, sel ? 1.0f : 0.0f);
        ImGui::PopID();
        ImGui::PopID();
        if (h > 0.01f && a < 0.99f) Para(dl, tp, ImVec2(tp.x + w - S(24), tp.y + th), White(0.045f * h * (1 - a)));
        ImU32 col = Mix(ImVec4(1, 1, 1, 0.58f + 0.34f * h), ImVec4(1, 1, 1, 1), a);
        Icon(dl, ImVec2(tp.x + S(26), tp.y + th * 0.5f), S(17), col, icon);
        ImVec2 ts = Measure(sel ? T().semibold : T().medium, 13.5f, name);
        Text(dl, sel ? T().semibold : T().medium, 13.5f, ImVec2(tp.x + S(46), tp.y + (th - ts.y) * 0.5f), col, name);
    }

    float by = p.y + size.y - S(68);
    bool client = m.session == core::Session::Client;
    const char* st = !m.inWorld ? "Not in a save" : client ? "Co-op \xc2\xb7 not host" : m.session == core::Session::Host ? "Co-op \xc2\xb7 host" : "Single player";
    ImU32 dot = !m.inWorld ? White(0.35f) : client ? Danger() : Accent();
    if (!status.empty()) {
        st = status.c_str();
        dot = statusGood ? Accent() : White(0.35f);
    }
    dl->AddLine(ImVec2(p.x + S(12), by - S(14)), ImVec2(p.x + S(kSide) - S(12), by - S(14)), White(0.07f));
    Diamond(dl, ImVec2(p.x + S(26), by + S(8)), S(4), dot);
    Text(dl, T().medium, 12, ImVec2(p.x + S(38), by), White(0.72f), st);
    std::string hint = embedded  ? std::string("Changes are saved automatically")
                       : !focused ? std::string(core::KeyName(m.keyOpen)) + " to use the menu"
                                  : std::string(core::KeyName(m.keyOpen)) + " hide  \xc2\xb7  click outside to play";
    Text(dl, T().regular, 11, ImVec2(p.x + S(20), by + S(26)), White(0.36f), hint.c_str());
}

void Menu::TopBar(ImVec2 p, ImVec2 size) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int hi = ActiveHome();
    const char* title = homePage && onHome ? (hi ? homeTabs[hi - 1].name : homeName) : TabName(m, tab);
    ImVec2 ts = MeasureUpper(T().semibold, 20, title);
    float tx = p.x + S(kSide) + S(24), ty = p.y + std::floor((S(kTop) - ts.y) * 0.5f) - S(2);
    Upper(dl, T().semibold, 20, ImVec2(tx, ty), White(0.98f), title);
    dl->AddRectFilled(ImVec2(tx, ty + ts.y + S(6)), ImVec2(tx + S(36), ty + ts.y + S(9)), Accent());

    fpsTimer += Dt();
    if (fpsTimer > 0.25f) {
        fpsTimer = 0.0f;
        fpsShown = ImGui::GetIO().Framerate;
    }
    if (embedded) return;
    char buf[48];
    int n = m.activeCount();
    std::snprintf(buf, sizeof(buf), "%d active", n);
    float x = p.x + size.x - S(24);
    float cy = p.y + S(kTop) * 0.5f;
    x -= Pill(dl, ImVec2(x, cy), ICON_ZAP, buf, n > 0) + S(8);
    std::snprintf(buf, sizeof(buf), "%.0f FPS", fpsShown);
    Pill(dl, ImVec2(x, cy), ICON_ACTIVITY, buf, false);
}

void Menu::Banner() {
    bool client = m.session == core::Session::Client && !m.cheatsAllowed() && tab != Tab::Settings;
    bool legit = m.legit && (int)tab <= (int)Tab::World;
    if (!client && !legit) return;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float w = ImGui::GetContentRegionAvail().x, h = S(40);
    char buf[96];
    if (client) {
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Danger(0.12f));
        dl->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), Danger());
        Icon(dl, ImVec2(p.x + S(20), p.y + h * 0.5f), S(15), Danger(), ICON_USERS);
        Text(dl, T().medium, 12.5f, ImVec2(p.x + S(36), p.y + S(12)), Danger(), "You are not the host, so cheats are switched off in this session.");
    } else {
        std::snprintf(buf, sizeof(buf), "Legit Run is on. Sliders are capped to Tier %d limits.", m.legitTier + 1);
        dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), Accent(0.10f));
        dl->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), Accent());
        Icon(dl, ImVec2(p.x + S(20), p.y + h * 0.5f), S(15), Accent(), ICON_SHIELD_CHECK);
        Text(dl, T().medium, 12.5f, ImVec2(p.x + S(36), p.y + S(12)), Accent(), buf);
    }
    ImGui::Dummy(ImVec2(w, h + S(12)));
}

void Menu::Content(ImVec2 p, ImVec2 size) {
    bool home = homePage && onHome;
    int hi = ActiveHome();
    int key = home ? -1 - hi : (int)tab;
    if (key != lastTab) {
        lastTab = key;
        SetAnim(ImGui::GetID("tabfade"), 0.0f);
    }
    float fade = Ease(ImGui::GetID("tabfade"), 1.0f, 13.0f);
    ImGui::SetCursorScreenPos(ImVec2(p.x + S(kSide), p.y + S(kTop) + 1));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(S(24), S(20)));
    std::string homeId = "##home" + std::to_string(hi);
    ImGui::BeginChild(home ? homeId.c_str() : kTabs[(int)tab].name, ImVec2(size.x - S(kSide), size.y - S(kTop) - 1),
                      ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoBackground);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * fade);
    float slide = std::floor((1.0f - fade) * S(12));
    if (slide > 0) ImGui::Dummy(ImVec2(1, slide));
    if (home) {
        homePage();
    } else {
        Banner();
        switch (tab) {
        case Tab::Spawner:
            if (Plain(m, tab)) {
                if (page.empty()) FeaturePage(tab);
                else SubPage();
            } else {
                SpawnerPage();
            }
            break;
        case Tab::BaseHub:
            if (Plain(m, tab)) FeaturePage(tab);
            else HubPage();
            break;
        case Tab::Legit:
            if (Plain(m, tab)) FeaturePage(tab);
            else LegitPage();
            break;
        case Tab::Settings: loader ? LoaderPage() : SettingsPage(); break;
        default: FeaturePage(tab); break;
        }
    }
    ImGui::PopStyleVar();
    ImGui::EndChild();
    ImGui::PopStyleVar();
}

ImVec2 Menu::Masonry::Place(float h) {
    int c = y[0] <= y[1] ? 0 : 1;
    ImVec2 p(origin.x + c * (colW + gap), origin.y + y[c]);
    y[c] += h + gap;
    return p;
}

void Menu::Masonry::End() {
    ImGui::SetCursorScreenPos(origin);
    ImGui::Dummy(ImVec2(colW * 2 + gap, std::max(y[0], y[1])));
}

Menu::Masonry Menu::BeginMasonry() {
    Masonry ms;
    ms.origin = ImGui::GetCursorScreenPos();
    ms.gap = S(16);
    ms.colW = std::floor((ImGui::GetContentRegionAvail().x - ms.gap) * 0.5f);
    ms.y[0] = ms.y[1] = 0;
    return ms;
}

float Menu::CardBegin(ImVec2 pos, float w, float h, const char* title) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), White(0.035f));
    dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + S(2)), White(0.09f));
    if (!title) return pos.y + S(8);
    Title(dl, ImVec2(pos.x + S(16), pos.y + S(15)), title);
    return pos.y + S(38);
}

bool Menu::RowBase(const char* id, ImVec2 pos, float w, float h, const char* label, const char* hint, bool enabled, const char* tip,
                   float reserveRight) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::SetCursorScreenPos(pos);
    ImGui::PushID(id);
    ImGui::SetNextItemAllowOverlap();
    bool pressed = ImGui::InvisibleButton("r", ImVec2(w, h));
    bool hov = ImGui::IsItemHovered();
    float hv = Ease(ImGui::GetID("h"), hov && enabled ? 1.0f : 0.0f, 18.0f);
    ImGui::PopID();
    if (hv > 0.01f) {
        dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), White(0.035f * hv));
        dl->AddRectFilled(pos, ImVec2(pos.x + S(2), pos.y + h), Accent(hv));
    }
    float dim = enabled ? 1.0f : 0.38f;
    float tw = w - S(16) - reserveRight;
    if (hint && hint[0]) {
        TextFit(dl, T().medium, 13.5f, ImVec2(pos.x + S(8), pos.y + S(9)), tw, White(0.95f * dim), label);
        TextFit(dl, T().regular, 12, ImVec2(pos.x + S(8), pos.y + S(28)), tw, White(0.48f * dim), hint);
    } else {
        ImVec2 ts = Measure(T().medium, 13.5f, label);
        TextFit(dl, T().medium, 13.5f, ImVec2(pos.x + S(8), pos.y + (h - ts.y) * 0.5f), tw, White(0.95f * dim), label);
    }
    if (hov && tip) Tip(tip);
    return pressed && enabled;
}

bool Menu::ToggleRow(const char* id, ImVec2 pos, float w, const char* label, const char* hint, bool* v, bool enabled, const char* tip,
                     bool locked) {
    float h = hint && hint[0] ? S(52) : S(40);
    bool pressed = RowBase(id, pos, w, h, label, hint, enabled, tip, SwitchSize().x + S(12));
    ImGui::PushID(id);
    float t = Ease(ImGui::GetID("t"), *v ? 1.0f : 0.0f, 16.0f, *v ? 1.0f : 0.0f);
    ImGui::PopID();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 ss = SwitchSize();
    ImVec2 sp(pos.x + w - S(8) - ss.x, pos.y + std::floor((h - ss.y) * 0.5f));
    if (locked) Icon(dl, ImVec2(sp.x + ss.x - S(10), sp.y + ss.y * 0.5f), S(15), White(0.28f), ICON_LOCK);
    else Switch(dl, sp, t, enabled ? 1.0f : 0.4f);
    if (pressed) {
        *v = !*v;
        return true;
    }
    return false;
}

bool Menu::SliderRow(const char* id, ImVec2 pos, float w, const char* label, float* v, float mn, float mx, float def, float step,
                     const char* fmt, float fmtMul, bool enabled, float cap, bool* active) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImGui::PushID(id);
    float dim = enabled ? 1.0f : 0.38f;
    float top = cap > 0 ? std::min(mx, cap) : mx;
    Text(dl, T().medium, 13.5f, ImVec2(pos.x + S(8), pos.y + S(9)), White(0.95f * dim), label);
    char buf[32];
    std::snprintf(buf, sizeof(buf), fmt, *v * fmtMul);
    bool moved = std::fabs(*v - def) > 0.001f;
    TextRight(dl, T().medium, 13, ImVec2(pos.x + w - S(8), pos.y + S(10)), moved ? Accent(dim) : White(0.55f * dim), buf);

    float kr = S(7);
    float x0 = pos.x + S(8) + kr, x1 = pos.x + w - S(8) - kr, ty = pos.y + S(42);
    ImGui::SetCursorScreenPos(ImVec2(pos.x + S(8), ty - S(12)));
    ImGui::InvisibleButton("s", ImVec2(w - S(16), S(24)));
    bool act = ImGui::IsItemActive() && enabled;
    bool hov = ImGui::IsItemHovered() && enabled;
    bool changed = false;
    if (act) {
        float f = std::clamp((ImGui::GetIO().MousePos.x - x0) / (x1 - x0), 0.0f, 1.0f);
        float nv = mn + f * (mx - mn);
        nv = std::round(nv / step) * step;
        nv = std::clamp(nv, mn, top);
        if (std::fabs(nv - *v) > 0.0001f) {
            *v = nv;
            changed = true;
        }
    }
    if (hov && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        float nv = std::clamp(def, mn, top);
        if (nv != *v) {
            *v = nv;
            changed = true;
        }
    }
    float f = std::clamp((*v - mn) / (mx - mn), 0.0f, 1.0f);
    float k = Ease(ImGui::GetID("k"), f, 26.0f, f);
    float hv = Ease(ImGui::GetID("h"), hov || act ? 1.0f : 0.0f, 18.0f);
    float kx = x0 + (x1 - x0) * k;
    dl->AddRectFilled(ImVec2(x0 - kr, ty - S(3)), ImVec2(x1 + kr, ty + S(3)), White(0.09f * dim));
    dl->AddRectFilled(ImVec2(x0 - kr, ty - S(3)), ImVec2(kx, ty + S(3)), Accent(dim));
    if (cap > 0 && top < mx - 0.001f) {
        float cx = x0 + (x1 - x0) * (top - mn) / (mx - mn);
        dl->AddRectFilled(ImVec2(cx + kr + S(2), ty - S(5)), ImVec2(cx + kr + S(4), ty + S(5)), White(0.35f), S(1));
        if (hov) Tip("Capped by Legit Run");
    }
    float kw = S(4) + S(1) * hv, kh = S(10) + S(1.5f) * hv;
    Para(dl, ImVec2(kx - kw - S(2), ty - kh), ImVec2(kx + kw + S(2), ty + kh), White(dim));
    ImGui::PopID();
    if (active) *active = act;
    return changed;
}

bool Menu::KeyChip(const char* id, ImVec2 rightCenter, int* key) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    bool cap = capture == key;
    const char* label = cap ? "Press a key\xe2\x80\xa6" : *key ? core::KeyName(*key) : "None";
    ImVec2 ts = Measure(T().medium, 12, label);
    float w = std::max(ts.x + S(20), S(56)), h = S(26);
    ImVec2 a(rightCenter.x - w, rightCenter.y - h * 0.5f);
    ImGui::SetCursorScreenPos(a);
    ImGui::PushID(id);
    bool pressed = ImGui::InvisibleButton("k", ImVec2(w, h));
    float hv = Ease(ImGui::GetID("h"), ImGui::IsItemHovered() ? 1.0f : 0.0f, 18.0f);
    ImGui::PopID();
    dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), cap ? Accent(0.14f) : White(0.06f + 0.04f * hv));
    dl->AddRect(a, ImVec2(a.x + w, a.y + h), cap ? Accent(0.7f) : White(0.09f));
    if (!cap) dl->AddRectFilled(ImVec2(a.x, a.y + h - S(2)), ImVec2(a.x + w, a.y + h), White(0.09f));
    Text(dl, T().medium, 12, ImVec2(a.x + (w - ts.x) * 0.5f, a.y + (h - ts.y) * 0.5f),
         cap ? Accent() : White(*key ? 0.9f : 0.42f), label);
    if (pressed) {
        capture = key;
        captureFrame = ImGui::GetFrameCount();
    }
    return pressed;
}

float Menu::FeatureRowHeight(const Feature& f) const {
    if (f.kind == Kind::Slider && !f.building) return S(60);
    if (f.kind == Kind::Input && !f.building) return (f.hint.empty() ? S(40) : S(52)) + S(44);
    return f.hint.empty() ? S(40) : S(52);
}

void Menu::WithBackup(std::function<void()> then) {
    if (backupJob.valid()) {
        Notify("A save backup is still running", ICON_INFO);
        return;
    }
    if (!m.backupSaves || embedded) {
        if (then) then();
        return;
    }
    auto backup = m.backupSaves;
    backupJob = std::async(std::launch::async, [backup] { return backup(); });
    afterBackup = std::move(then);
    Notify("Backing up your saves first\xe2\x80\xa6", ICON_REFRESH_CW);
}

void Menu::PumpBackup() {
    if (!backupJob.valid() || backupJob.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
    std::string path = backupJob.get();
    std::function<void()> then = std::move(afterBackup);
    afterBackup = nullptr;
    if (path.empty()) {
        Notify("Backup failed, nothing was changed", ICON_TRIANGLE_ALERT);
        return;
    }
    m.lastBackup = path;
    if (then) then();
}

void Menu::Toggle(Feature& f, bool on) {
    if (on && f.id.rfind("unlock_", 0) == 0 && !embedded) {
        Feature* pf = &f;
        WithBackup([this, pf] {
            core::SetFeature(m, *pf, true);
            if (pf->on) Notify(pf->label + " on", ICON_CHECK);
        });
        return;
    }
    core::SetFeature(m, f, on);
    if (f.counts) Notify(f.label + (f.on ? " on" : " off"), f.on ? ICON_CHECK : ICON_X);
    if (f.on && f.id == "free_camera" && !embedded) Unfocus();
}

void Menu::RunPermanent(Feature& f) {
    Feature* pf = &f;
    Ask("Are you sure?", f.label + " cannot be undone. Your save files are backed up first.", "Yes, do it", [this, pf] {
        WithBackup([this, pf] {
            if (pf->apply) pf->apply(*pf);
            Notify(pf->label + " done", ICON_CHECK);
        });
    });
}

void Menu::RunAction(Feature& f) {
    if (f.confirm) {
        RunPermanent(f);
        return;
    }
    f.result.clear();
    if (f.apply) f.apply(f);
    Notify(f.result.empty() ? f.label + " done" : f.result, f.result.empty() ? ICON_CHECK : ICON_INFO);
}

void Menu::FeatureRow(Feature& f, ImVec2 pos, float w) {
    if (f.building) {
        float h = FeatureRowHeight(f);
        RowBase(f.id.c_str(), pos, w, h, f.label.c_str(), f.hint.c_str(), false, "This module is still being built", BuildingWidth() + S(8));
        Building(ImGui::GetWindowDrawList(), ImVec2(pos.x + w - S(8), pos.y + h * 0.5f));
        return;
    }
    bool allowed = m.cheatsAllowed();
    bool enabled = f.available && allowed;
    const char* tip = !f.available ? f.missing.c_str() : !allowed ? "Only the host can use cheats" : nullptr;
    if (f.kind == Kind::Toggle) {
        bool v = f.on;
        if (ToggleRow(f.id.c_str(), pos, w, f.label.c_str(), f.hint.c_str(), &v, enabled, tip, !f.available)) Toggle(f, v);
    } else if (f.kind == Kind::Slider) {
        float v = f.value;
        float cap = m.legit ? m.sliderMax(f) : 0.0f;
        if (SliderRow(f.id.c_str(), pos, w, f.label.c_str(), &v, f.min, f.max, f.def, f.step, f.fmt.c_str(), 1.0f, enabled, cap))
            core::SetValue(m, f, v);
        if (tip && ImGui::IsMouseHoveringRect(pos, ImVec2(pos.x + w, pos.y + S(60)))) Tip(tip);
    } else if (f.kind == Kind::Input) {
        float top = f.hint.empty() ? S(40) : S(52);
        RowBase(f.id.c_str(), pos, w, top, f.label.c_str(), f.hint.c_str(), enabled, tip, 0.0f);
        float bw = S(84), gap = S(8), y = pos.y + top;
        size_t n = f.inputs.size();
        auto weight = [&](size_t i) { return i < f.inputSizes.size() && f.inputSizes[i] > 0 ? f.inputSizes[i] : 1.0f; };
        float total = 0.0f;
        for (size_t i = 0; i < n; ++i) total += weight(i);
        float room = w - S(16) - bw - gap - gap * (n > 0 ? (float)(n - 1) : 0.0f);
        float x = pos.x + S(8);
        for (size_t i = 0; i < n; ++i) {
            float iw = std::floor(room * weight(i) / std::max(total, 1.0f));
            std::string iid = f.id + "_in" + std::to_string(i);
            Input(iid.c_str(), &f.inputs[i], i < f.inputHints.size() ? f.inputHints[i].c_str() : "", ImVec2(x, y), iw);
            x += iw + gap;
        }
        std::string bid = "act_" + f.id;
        if (Button(bid.c_str(), f.actionLabel.c_str(), ImVec2(pos.x + w - S(8) - bw, y), ImVec2(bw, S(36)), Btn::Tint, nullptr, enabled)) {
            f.result.clear();
            if (f.apply) f.apply(f);
            Notify(f.result.empty() ? f.label + " sent" : f.result, f.result.empty() ? ICON_CHECK : ICON_INFO);
        }
    } else {
        float h = FeatureRowHeight(f);
        float bw = S(84);
        RowBase(f.id.c_str(), pos, w, h, f.label.c_str(), f.hint.c_str(), enabled, tip, bw + S(8));
        std::string bid = "act_" + f.id;
        if (Button(bid.c_str(), f.actionLabel.c_str(), ImVec2(pos.x + w - S(8) - bw, pos.y + (h - S(30)) * 0.5f), ImVec2(bw, S(30)),
                   Btn::Tint, nullptr, enabled || !f.page.empty())) {
            if (!f.page.empty()) {
                page = f.page;
                SetAnim(ImGui::GetID("pagefade"), 0.0f);
            } else {
                RunAction(f);
            }
        }
    }
}

void Menu::FeaturePage(Tab t) {
    std::vector<std::pair<std::string, std::vector<Feature*>>> groups;
    for (Feature& f : m.features) {
        if (f.tab != t) continue;
        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == f.group; });
        if (it == groups.end()) {
            groups.push_back({f.group, {}});
            it = groups.end() - 1;
        }
        it->second.push_back(&f);
    }
    Masonry ms = BeginMasonry();
    for (auto& g : groups) {
        float h = S(38) + S(8);
        for (Feature* f : g.second) h += FeatureRowHeight(*f);
        ImVec2 pos = ms.Place(h);
        float y = CardBegin(pos, ms.colW, h, g.first.c_str());
        for (Feature* f : g.second) {
            FeatureRow(*f, ImVec2(pos.x + S(8), y), ms.colW - S(16));
            y += FeatureRowHeight(*f);
        }
    }
    if (t == Tab::Biomods && !Plain(m, t)) {
        bool allowed = m.cheatsAllowed() && (bool)m.setAdaptation;
        float rowH = S(52);
        float h = S(38) + S(8) + (m.adaptations.empty() ? S(56) : rowH * m.adaptations.size());
        ImVec2 pos = ms.Place(h);
        float y = CardBegin(pos, ms.colW, h, "Adaptations");
        if (m.adaptations.empty()) {
            Text(ImGui::GetWindowDrawList(), T().regular, 12, ImVec2(pos.x + S(16), y + S(8)), White(0.45f),
                 "The list appears once you are in a save", ms.colW - S(32));
        }
        for (size_t i = 0; i < m.adaptations.size(); ++i) {
            core::Adaptation& a = m.adaptations[i];
            bool v = a.on;
            std::string id = "adapt_" + a.id;
            if (ToggleRow(id.c_str(), ImVec2(pos.x + S(8), y), ms.colW - S(16), a.name.c_str(), a.hint.c_str(), &v, allowed,
                          allowed ? nullptr : "Only works in game, as the host")) {
                a.on = v;
                m.setAdaptation((int)i, v);
                Notify(a.name + (v ? " added" : " removed"), v ? ICON_CHECK : ICON_X);
            }
            y += rowH;
        }
    }
    ms.End();
}

void Menu::Confirm() {
    Approach(confirmAnim, confirmOpen ? 1.0f : 0.0f, 18.0f);
    if (confirmAnim < 0.01f) return;
    ImGui::SetNextWindowPos(windowPos);
    ImGui::SetNextWindowSize(windowSize);
    if (confirmOpen) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, openAnim * confirmAnim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoMove;
    if (!confirmOpen) flags |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##fathom_confirm", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(windowPos, ImVec2(windowPos.x + windowSize.x, windowPos.y + windowSize.y), Black(0.6f));

    float w = S(400), wrap = w - S(48);
    ImGui::PushFont(T().regular, S(13));
    ImVec2 bs = ImGui::CalcTextSize(confirmBody.c_str(), nullptr, false, wrap);
    ImGui::PopFont();
    float h = S(24) + S(22) + S(10) + bs.y + S(24) + S(36) + S(24);
    ImVec2 c(windowPos.x + std::floor((windowSize.x - w) * 0.5f),
             windowPos.y + std::floor((windowSize.y - h) * 0.5f) + std::floor((1.0f - confirmAnim) * S(10)));
    Glow(dl, c, ImVec2(c.x + w, c.y + h), 0.0f, ImVec4(0, 0, 0, 1), 0.5f, S(20));
    dl->AddRectFilled(c, ImVec2(c.x + w, c.y + h), Bg(0.985f));
    dl->AddRect(c, ImVec2(c.x + w, c.y + h), White(0.10f));
    dl->AddRectFilled(c, ImVec2(c.x + w, c.y + S(3)), Accent());
    Upper(dl, T().semibold, 16, ImVec2(c.x + S(24), c.y + S(24)), White(0.97f), confirmTitle.c_str());
    Text(dl, T().regular, 13, ImVec2(c.x + S(24), c.y + S(56)), White(0.68f), confirmBody.c_str(), wrap);
    float bw = S(112), by = c.y + h - S(24) - S(36);
    bool yes = Button("yes", confirmYes.c_str(), ImVec2(c.x + w - S(24) - bw, by), ImVec2(bw, S(36)), Btn::Primary);
    bool no = Button("no", "Cancel", ImVec2(c.x + w - S(24) - bw - S(8) - S(92), by), ImVec2(S(92), S(36)), Btn::Ghost);
    if (confirmOpen && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) no = true;
    if (confirmOpen && (yes || no)) {
        confirmOpen = false;
        if (yes && confirmFn) confirmFn();
        confirmFn = nullptr;
    }
    ImGui::End();
    ImGui::PopStyleVar(2);
}

void Menu::Toasts() {
    if (toasts.empty()) return;
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const float life = 2.8f;
    float y = vp->Pos.y + vp->Size.y - S(24);
    for (int i = (int)toasts.size() - 1; i >= 0; --i) {
        ToastMsg& t = toasts[i];
        t.age += Dt();
        float in = std::clamp(t.age / 0.18f, 0.0f, 1.0f);
        float out = std::clamp((life - t.age) / 0.25f, 0.0f, 1.0f);
        float a = std::min(in, out);
        a = a * a * (3.0f - 2.0f * a);
        ImVec2 ts = Measure(T().medium, 13, t.text.c_str());
        float h = S(40), w = ts.x + S(66);
        float target = y - h;
        if (!t.placed) {
            t.y = target;
            t.placed = true;
        }
        Approach(t.y, target, 18.0f);
        float x = vp->Pos.x + vp->Size.x - S(24) - w + (1.0f - a) * S(24);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, a);
        ImVec2 p(x, t.y), q(x + w, t.y + h);
        Glow(dl, p, q, 0.0f, ImVec4(0, 0, 0, 1), 0.35f, S(12));
        Para(dl, p, q, Bg(0.97f));
        ParaLine(dl, p, q, White(0.09f));
        Para(dl, p, ImVec2(p.x + Slant(h) + S(4), q.y), Accent());
        Icon(dl, ImVec2(x + S(28), t.y + h * 0.5f), S(15), Accent(), t.icon);
        Text(dl, T().medium, 13, ImVec2(x + S(44), t.y + (h - ts.y) * 0.5f), White(0.93f), t.text.c_str());
        ImGui::PopStyleVar();
        y -= (h + S(8)) * out;
    }
    toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const ToastMsg& t) { return t.age >= life; }), toasts.end());
}

}
