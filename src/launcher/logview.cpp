#include "launcher/logview.h"
#include "ui/icons.h"
#include "ui/ui.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <filesystem>

namespace launcher {

using namespace ui;

namespace {

enum class Tone { Plain, Good, Warn, Bad };

ImU32 Rgb(float r, float g, float b, float a = 1.0f) { return ImGui::ColorConvertFloat4ToU32(ImVec4(r, g, b, a)); }

std::string Lower(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = (char)std::tolower((unsigned char)c);
    return out;
}

bool Has(const std::string& s, std::initializer_list<const char*> words) {
    for (const char* w : words)
        if (s.find(w) != std::string::npos) return true;
    return false;
}

Tone Classify(const std::string& text) {
    std::string s = Lower(text);
    if (Has(s, {"fail", "error", "could not", "couldn't", "cannot", "not found", "missing", "crash", "exception", "refused", "invalid"}))
        return Tone::Bad;
    if (Has(s, {"warn", "waiting", "retry", "skipp", "slow", "outdated", "stays loaded", "timed out", "timeout"})) return Tone::Warn;
    if (Has(s, {"ready", "injected", "loaded", "found", "started", "attached", "hooked", "unloaded", "done", "success", "ok "}))
        return Tone::Good;
    return Tone::Plain;
}

ImU32 ToneColor(Tone t) {
    switch (t) {
        case Tone::Bad: return Rgb(1.0f, 0.47f, 0.47f);
        case Tone::Warn: return Rgb(0.98f, 0.76f, 0.33f);
        case Tone::Good: return Rgb(0.45f, 0.88f, 0.62f);
        default: return White(0.80f);
    }
}

std::string Count(size_t n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s + (n == 1 ? " line" : " lines");
}

}

void LogView::Open(const std::wstring& p, const std::string& t) {
    path = p;
    title = t;
    file = std::filesystem::path(p).filename().string();
    open = true;
    follow = true;
    jump = true;
    lines.clear();
    stamp = size = 0;
    missing = true;
    Reload(true);
    lastCheck = GetTickCount64();
}

void LogView::Close() { open = false; }

void LogView::Reload(bool force) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) {
        if (!missing) changedAt = ImGui::GetTime();
        lines.clear();
        missing = true;
        return;
    }
    unsigned long long t = ((unsigned long long)info.ftLastWriteTime.dwHighDateTime << 32) | info.ftLastWriteTime.dwLowDateTime;
    unsigned long long s = ((unsigned long long)info.nFileSizeHigh << 32) | info.nFileSizeLow;
    if (!force && !missing && t == stamp && s == size) return;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const unsigned long long kMax = 1ull << 20;
    unsigned long long from = s > kMax ? s - kMax : 0;
    LARGE_INTEGER li;
    li.QuadPart = (LONGLONG)from;
    SetFilePointerEx(h, li, nullptr, FILE_BEGIN);
    std::string data((size_t)(s - from), '\0');
    DWORD got = 0;
    if (!data.empty()) ReadFile(h, data.data(), (DWORD)data.size(), &got, nullptr);
    CloseHandle(h);
    data.resize(got);
    bool grew = !missing && s > size;
    stamp = t;
    size = s;
    missing = false;
    lines.clear();
    size_t start = 0;
    if (from) {
        size_t nl = data.find('\n');
        start = nl == std::string::npos ? data.size() : nl + 1;
    }
    while (start < data.size()) {
        size_t nl = data.find('\n', start);
        size_t end = nl == std::string::npos ? data.size() : nl;
        std::string l = data.substr(start, end - start);
        if (!l.empty() && l.back() == '\r') l.pop_back();
        lines.push_back(std::move(l));
        start = end + 1;
    }
    if (grew || force) changedAt = ImGui::GetTime();
    if (follow) jump = true;
}

void LogView::Body(float x0, float y0, float x1, float y1) {
    ImFont* mono = T().mono ? T().mono : T().regular;
    float fs = 12.5f;
    float lh = S(21);
    float gutter = S(52);
    ImGui::SetCursorScreenPos(ImVec2(x0, y0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, S(8));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, ImVec4(1, 1, 1, 0.12f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, ImVec4(1, 1, 1, 0.22f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, ImVec4(1, 1, 1, 0.30f));
    ImGui::BeginChild("##logbody", ImVec2(x1 - x0, y1 - y0), 0, ImGuiWindowFlags_HorizontalScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.y += S(10);
    float scroll = ImGui::GetScrollY();
    float viewH = y1 - y0;
    int n = (int)lines.size();
    int first = std::max(0, (int)((scroll - S(10)) / lh) - 1);
    int last = std::min(n, first + (int)(viewH / lh) + 3);
    ImVec2 mouse = ImGui::GetMousePos();
    bool bodyHover = ImGui::IsWindowHovered();
    float widest = 0.0f;
    char num[16];
    for (int i = first; i < last; ++i) {
        const std::string& l = lines[(size_t)i];
        float y = origin.y + i * lh;
        bool rowHover = bodyHover && mouse.y >= y && mouse.y < y + lh;
        Tone tone = Classify(l);
        if (rowHover) dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + std::max(x1 - x0, widest), y + lh), White(0.035f));
        if (tone == Tone::Bad) {
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x1, y + lh), Rgb(1.0f, 0.3f, 0.3f, 0.06f));
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + S(2), y + lh), Rgb(1.0f, 0.45f, 0.45f, 0.9f));
        } else if (tone == Tone::Warn) {
            dl->AddRectFilled(ImVec2(x0, y), ImVec2(x0 + S(2), y + lh), Rgb(0.98f, 0.76f, 0.33f, 0.8f));
        }
        std::snprintf(num, sizeof(num), "%d", i + 1);
        float ty = y + std::floor((lh - Measure(mono, fs, "0").y) * 0.5f);
        TextRight(dl, mono, fs, ImVec2(origin.x + gutter - S(14), ty), White(rowHover ? 0.42f : 0.18f), num);
        float x = origin.x + gutter;
        std::string msg = l;
        if (l.size() > 2 && l[0] == '[') {
            size_t close = l.find(']');
            if (close != std::string::npos && close < 20) {
                std::string stampText = l.substr(0, close + 1);
                Text(dl, mono, fs, ImVec2(x, ty), White(0.30f), stampText.c_str());
                x += Measure(mono, fs, stampText.c_str()).x + S(10);
                msg = l.substr(close + 1);
                if (!msg.empty() && msg[0] == ' ') msg.erase(0, 1);
            }
        }
        Text(dl, mono, fs, ImVec2(x, ty), ToneColor(tone), msg.c_str());
        widest = std::max(widest, x + Measure(mono, fs, msg.c_str()).x - origin.x + S(24));
    }
    float promptY = origin.y + n * lh;
    if (!missing) {
        float ty = promptY + std::floor((lh - Measure(mono, fs, "0").y) * 0.5f);
        Text(dl, mono, fs, ImVec2(origin.x + gutter, ty), Accent(0.95f), ">");
        if (std::fmod(ImGui::GetTime(), 1.06) < 0.56) {
            float cx = origin.x + gutter + Measure(mono, fs, "> ").x;
            dl->AddRectFilled(ImVec2(cx, ty + S(1)), ImVec2(cx + S(7.5f), ty + Measure(mono, fs, "0").y - S(1)), Accent(0.85f));
        }
    }
    ImGui::SetCursorScreenPos(ImVec2(origin.x, origin.y));
    ImGui::Dummy(ImVec2(std::max(widest, 1.0f), (n + 1) * lh + S(14)));
    bool atBottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - lh * 0.5f;
    if (jump) {
        ImGui::SetScrollHereY(1.0f);
        jump = false;
        follow = true;
    } else if (ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.0f) {
        follow = atBottom && ImGui::GetIO().MouseWheel < 0.0f;
    } else if (ImGui::IsWindowFocused() && ImGui::IsMouseDragging(0)) {
        follow = atBottom;
    }
    if (atBottom && ImGui::GetIO().MouseWheel < 0.0f) follow = true;
    ImGui::EndChild();
    ImGui::PopStyleColor(4);
    ImGui::PopStyleVar(2);

    ImDrawList* fg = ImGui::GetWindowDrawList();
    if (missing || lines.empty()) {
        const char* head = missing ? "No log file yet" : "The log is empty";
        const char* sub = missing ? "It is made the first time you press Inject." : "Lines show up here while the menu runs in the game.";
        float cy = (y0 + y1) * 0.5f;
        ImVec2 hs = Measure(T().semibold, 15, head), ss = Measure(T().regular, 12.5f, sub);
        Text(fg, mono, 22, ImVec2((x0 + x1) * 0.5f - Measure(mono, 22, ">_").x * 0.5f, cy - S(62)), Accent(0.55f), ">_");
        Text(fg, T().semibold, 15, ImVec2((x0 + x1) * 0.5f - hs.x * 0.5f, cy - S(22)), White(0.85f), head);
        Text(fg, T().regular, 12.5f, ImVec2((x0 + x1) * 0.5f - ss.x * 0.5f, cy + S(4)), White(0.45f), sub);
    } else if (!follow) {
        ImVec2 bs(S(150), S(32));
        ImVec2 bp((x0 + x1) * 0.5f - bs.x * 0.5f, y1 - bs.y - S(16));
        Glow(fg, bp, ImVec2(bp.x + bs.x, bp.y + bs.y), S(16), ImVec4(0, 0, 0, 1), 0.5f, S(14));
        if (Button("latest", "Jump to latest", bp, bs, Btn::Primary, ICON_DOWNLOAD)) jump = true;
    }
}

void LogView::Draw() {
    float anim = Ease(ImHashStr("fathom_logview"), open ? 1.0f : 0.0f, 18.0f);
    if (anim < 0.01f) return;
    unsigned long long now = GetTickCount64();
    if (open && now - lastCheck >= 350) {
        lastCheck = now;
        Reload(false);
    }
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos);
    ImGui::SetNextWindowSize(vp->Size);
    if (open) ImGui::SetNextWindowFocus();
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, anim);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoSavedSettings |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollWithMouse;
    if (!open) flags |= ImGuiWindowFlags_NoInputs;
    ImGui::Begin("##fathom_log", nullptr, flags);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + vp->Size.y), Black(0.62f));

    float w = std::min(vp->Size.x - S(64), S(920));
    float h = vp->Size.y - S(72);
    ImVec2 a(vp->Pos.x + std::floor((vp->Size.x - w) * 0.5f), vp->Pos.y + std::floor((vp->Size.y - h) * 0.5f + (1.0f - anim) * S(14)));
    ImVec2 b(a.x + w, a.y + h);
    float r = S(14), bar = S(48), foot = S(36);
    Glow(dl, a, b, r, ImVec4(0, 0, 0, 1), 0.65f, S(30));
    dl->AddRectFilled(a, b, Rgb(0.024f, 0.035f, 0.050f, 0.995f), r);
    dl->AddRectFilled(a, ImVec2(b.x, a.y + bar), White(0.03f), r, ImDrawFlags_RoundCornersTop);
    dl->AddLine(ImVec2(a.x, a.y + bar), ImVec2(b.x, a.y + bar), White(0.07f));
    dl->AddRectFilled(ImVec2(a.x, b.y - foot), b, White(0.025f), r, ImDrawFlags_RoundCornersBottom);
    dl->AddLine(ImVec2(a.x, b.y - foot), ImVec2(b.x, b.y - foot), White(0.06f));
    dl->AddRect(a, b, White(0.10f), r);

    float cy = a.y + bar * 0.5f;
    const ImU32 dots[3] = {Rgb(1.0f, 0.37f, 0.34f), Rgb(1.0f, 0.74f, 0.18f), Rgb(0.16f, 0.79f, 0.25f)};
    ImVec2 mouse = ImGui::GetMousePos();
    bool dotsHover = open && mouse.x >= a.x + S(12) && mouse.x <= a.x + S(70) && mouse.y >= cy - S(10) && mouse.y <= cy + S(10);
    for (int i = 0; i < 3; ++i) {
        ImVec2 c(a.x + S(24) + i * S(19), cy);
        dl->AddCircleFilled(c, S(6), dots[i]);
        if (dotsHover && i == 0) {
            dl->AddLine(ImVec2(c.x - S(2.5f), c.y - S(2.5f)), ImVec2(c.x + S(2.5f), c.y + S(2.5f)), Black(0.6f), S(1.3f));
            dl->AddLine(ImVec2(c.x - S(2.5f), c.y + S(2.5f)), ImVec2(c.x + S(2.5f), c.y - S(2.5f)), Black(0.6f), S(1.3f));
        }
    }
    bool close = false;
    if (dotsHover && ImGui::IsMouseClicked(0)) close = true;

    ImFont* mono = T().mono ? T().mono : T().regular;
    float tx = a.x + S(92);
    Text(dl, mono, 13, ImVec2(tx, cy - Measure(mono, 13, ">_").y * 0.5f), Accent(), ">_");
    tx += Measure(mono, 13, ">_").x + S(10);
    Text(dl, T().semibold, 13.5f, ImVec2(tx, cy - Measure(T().semibold, 13.5f, file.c_str()).y * 0.5f), White(0.92f), file.c_str());
    tx += Measure(T().semibold, 13.5f, file.c_str()).x + S(8);
    std::string sub = "\xc2\xb7  " + title;
    Text(dl, T().regular, 13, ImVec2(tx, cy - Measure(T().regular, 13, sub.c_str()).y * 0.5f), White(0.42f), sub.c_str());
    tx += Measure(T().regular, 13, sub.c_str()).x + S(14);

    double t = ImGui::GetTime();
    bool fresh = !missing && t - changedAt < 2.5;
    const char* chip = missing ? "NO FILE" : "LIVE";
    ImVec2 cs = Measure(T().semibold, 10, chip);
    ImVec2 ca(tx, cy - S(10)), cb(tx + cs.x + S(26), cy + S(10));
    ImU32 chipCol = missing ? White(0.5f) : Rgb(0.45f, 0.88f, 0.62f);
    dl->AddRectFilled(ca, cb, missing ? White(0.06f) : Rgb(0.45f, 0.88f, 0.62f, 0.12f), S(10));
    float pulse = missing ? 0.0f : 0.5f + 0.5f * (float)std::sin(t * (fresh ? 9.0 : 3.0));
    if (!missing) dl->AddCircleFilled(ImVec2(ca.x + S(10), cy), S(3.5f) + S(2.5f) * pulse, Rgb(0.45f, 0.88f, 0.62f, 0.25f * pulse));
    dl->AddCircleFilled(ImVec2(ca.x + S(10), cy), S(3), chipCol);
    Text(dl, T().semibold, 10, ImVec2(ca.x + S(18), cy - cs.y * 0.5f), chipCol, chip);

    float ib = S(32);
    float bx = b.x - S(10) - ib;
    if (IconButton("close", ICON_X, ImVec2(bx, cy - ib * 0.5f), ib)) close = true;
    bx -= ib + S(6);
    if (IconButton("folder", ICON_FOLDER_OPEN, ImVec2(bx, cy - ib * 0.5f), ib)) {
        std::wstring dir = std::filesystem::path(path).parent_path().wstring();
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    if (ImGui::IsItemHovered()) Tip("Open the folder");
    bx -= ib + S(6);
    if (IconButton("copy", t - copiedAt < 1.6 ? ICON_CHECK : ICON_COPY, ImVec2(bx, cy - ib * 0.5f), ib, !lines.empty())) {
        std::string all;
        for (const std::string& l : lines) all += l + "\n";
        ImGui::SetClipboardText(all.c_str());
        copiedAt = t;
    }
    if (ImGui::IsItemHovered()) Tip("Copy the whole log");
    if (t - copiedAt < 1.6) TextRight(dl, T().medium, 12, ImVec2(bx - S(10), cy - S(8)), Accent(0.9f), "Copied");

    Body(a.x + S(1), a.y + bar + S(1), b.x - S(1), b.y - foot - S(1));

    float fy = b.y - foot * 0.5f;
    std::string left = missing ? "Waiting for the file" : Count(lines.size());
    Text(dl, mono, 11.5f, ImVec2(a.x + S(18), fy - Measure(mono, 11.5f, left.c_str()).y * 0.5f), White(0.42f), left.c_str());
    if (!missing) {
        float lx = a.x + S(18) + Measure(mono, 11.5f, left.c_str()).x + S(14);
        const char* state = follow ? "following new lines" : "paused, scroll to the end to follow";
        dl->AddCircleFilled(ImVec2(lx, fy), S(2.5f), follow ? Accent(0.9f) : White(0.35f));
        Text(dl, mono, 11.5f, ImVec2(lx + S(9), fy - Measure(mono, 11.5f, state).y * 0.5f), White(0.42f), state);
    }
    const char* hint = "Esc to close";
    TextRight(dl, mono, 11.5f, ImVec2(b.x - S(18), fy - Measure(mono, 11.5f, hint).y * 0.5f), White(0.30f), hint);

    bool inside = mouse.x >= a.x && mouse.x <= b.x && mouse.y >= a.y && mouse.y <= b.y;
    if (open && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && !inside && ImGui::IsMouseClicked(0)) close = true;
    if (open && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) close = true;
    if (close) open = false;
    ImGui::End();
    ImGui::PopStyleVar(2);
}

}
