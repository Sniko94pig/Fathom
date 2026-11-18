#ifdef FATHOM_LAUNCHER_SHOTS
#include "launcher/art.h"
#include "launcher/games.h"
#include "launcher/home.h"
#include <chrono>
#include <thread>
#endif
#include "preview/sample.h"
#include "ui/icons.h"
#include "ui/menu.h"
#include "core/sn1_features.h"
#include "ui/ui.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace {

struct Canvas {
    int w = 0, h = 0;
    std::vector<float> px;
};

inline float Edge(const ImVec2& a, const ImVec2& b, float x, float y) {
    return (b.x - a.x) * (y - a.y) - (b.y - a.y) * (x - a.x);
}

inline bool Owns(const ImVec2& a, const ImVec2& b) {
    float dx = b.x - a.x, dy = b.y - a.y;
    return dy > 0 || (dy == 0 && dx < 0);
}

void Sample(const ImTextureData* tex, float u, float v, float out[4]) {
    float fx = u * tex->Width - 0.5f, fy = v * tex->Height - 0.5f;
    int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
    float tx = fx - x0, ty = fy - y0;
    const unsigned char* pix = (const unsigned char*)tex->Pixels;
    int bpp = tex->BytesPerPixel;
    float acc[4] = {0, 0, 0, 0};
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < 2; ++i) {
            int x = std::clamp(x0 + i, 0, tex->Width - 1), y = std::clamp(y0 + j, 0, tex->Height - 1);
            float wgt = (i ? tx : 1 - tx) * (j ? ty : 1 - ty);
            const unsigned char* p = pix + (size_t)(y * tex->Width + x) * bpp;
            if (bpp == 4) {
                for (int c = 0; c < 4; ++c) acc[c] += wgt * p[c];
            } else {
                acc[0] += wgt * 255;
                acc[1] += wgt * 255;
                acc[2] += wgt * 255;
                acc[3] += wgt * p[0];
            }
        }
    for (int c = 0; c < 4; ++c) out[c] = acc[c] / 255.0f;
}

void Triangle(Canvas& cv, const ImTextureData* tex, ImDrawVert v0, ImDrawVert v1, ImDrawVert v2, const ImVec4& clip) {
    float area = Edge(v0.pos, v1.pos, v2.pos.x, v2.pos.y);
    if (area == 0) return;
    if (area < 0) {
        std::swap(v1, v2);
        area = -area;
    }
    int x0 = std::max((int)std::floor(std::min({v0.pos.x, v1.pos.x, v2.pos.x})), (int)clip.x);
    int y0 = std::max((int)std::floor(std::min({v0.pos.y, v1.pos.y, v2.pos.y})), (int)clip.y);
    int x1 = std::min((int)std::ceil(std::max({v0.pos.x, v1.pos.x, v2.pos.x})), (int)clip.z);
    int y1 = std::min((int)std::ceil(std::max({v0.pos.y, v1.pos.y, v2.pos.y})), (int)clip.w);
    bool o0 = Owns(v1.pos, v2.pos), o1 = Owns(v2.pos, v0.pos), o2 = Owns(v0.pos, v1.pos);
    float c0[4], c1[4], c2[4];
    for (int c = 0; c < 4; ++c) {
        c0[c] = ((v0.col >> (8 * c)) & 0xFF) / 255.0f;
        c1[c] = ((v1.col >> (8 * c)) & 0xFF) / 255.0f;
        c2[c] = ((v2.col >> (8 * c)) & 0xFF) / 255.0f;
    }
    bool flatUv = v0.uv.x == v1.uv.x && v0.uv.x == v2.uv.x && v0.uv.y == v1.uv.y && v0.uv.y == v2.uv.y;
    float flat[4];
    if (flatUv) Sample(tex, v0.uv.x, v0.uv.y, flat);
    float inv = 1.0f / area;
    for (int y = y0; y < y1; ++y) {
        float py = y + 0.5f;
        for (int x = x0; x < x1; ++x) {
            float pxl = x + 0.5f;
            float w0 = Edge(v1.pos, v2.pos, pxl, py), w1 = Edge(v2.pos, v0.pos, pxl, py), w2 = Edge(v0.pos, v1.pos, pxl, py);
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            if ((w0 == 0 && !o0) || (w1 == 0 && !o1) || (w2 == 0 && !o2)) continue;
            w0 *= inv;
            w1 *= inv;
            w2 *= inv;
            float t[4];
            if (flatUv) {
                t[0] = flat[0];
                t[1] = flat[1];
                t[2] = flat[2];
                t[3] = flat[3];
            } else {
                Sample(tex, w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x, w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y, t);
            }
            float a = (w0 * c0[3] + w1 * c1[3] + w2 * c2[3]) * t[3];
            if (a <= 0) continue;
            float* d = &cv.px[(size_t)(y * cv.w + x) * 3];
            for (int c = 0; c < 3; ++c) {
                float s = (w0 * c0[c] + w1 * c1[c] + w2 * c2[c]) * t[c];
                d[c] = s * a + d[c] * (1 - a);
            }
        }
    }
}

void UpdateTextures(ImDrawData* dd) {
    if (!dd->Textures) return;
    for (ImTextureData* t : *dd->Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            t->SetTexID((ImTextureID)(intptr_t)t);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy && t->UnusedFrames > 0) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

void Raster(ImDrawData* dd, Canvas& cv) {
    float fs = dd->FramebufferScale.x;
    for (ImDrawList* list : dd->CmdLists) {
        for (const ImDrawCmd& cmd : list->CmdBuffer) {
            if (cmd.UserCallback) continue;
            const ImTextureData* tex = (const ImTextureData*)(intptr_t)cmd.GetTexID();
            if (!tex) continue;
            ImVec4 clip((cmd.ClipRect.x - dd->DisplayPos.x) * fs, (cmd.ClipRect.y - dd->DisplayPos.y) * fs,
                        (cmd.ClipRect.z - dd->DisplayPos.x) * fs, (cmd.ClipRect.w - dd->DisplayPos.y) * fs);
            clip.x = std::max(clip.x, 0.0f);
            clip.y = std::max(clip.y, 0.0f);
            clip.z = std::min(clip.z, (float)cv.w);
            clip.w = std::min(clip.w, (float)cv.h);
            if (clip.z <= clip.x || clip.w <= clip.y) continue;
            const ImDrawIdx* idx = list->IdxBuffer.Data + cmd.IdxOffset;
            const ImDrawVert* vtx = list->VtxBuffer.Data + cmd.VtxOffset;
            for (unsigned i = 0; i + 2 < cmd.ElemCount; i += 3) {
                ImDrawVert a = vtx[idx[i]], b = vtx[idx[i + 1]], c = vtx[idx[i + 2]];
                for (ImDrawVert* v : {&a, &b, &c}) {
                    v->pos.x = (v->pos.x - dd->DisplayPos.x) * fs;
                    v->pos.y = (v->pos.y - dd->DisplayPos.y) * fs;
                }
                Triangle(cv, tex, a, b, c, clip);
            }
        }
    }
}

bool SavePng(const Canvas& cv, const std::string& path) {
    std::vector<unsigned char> out((size_t)cv.w * cv.h * 3);
    for (size_t i = 0; i < out.size(); ++i) out[i] = (unsigned char)std::clamp(cv.px[i] * 255.0f + 0.5f, 0.0f, 255.0f);
    return stbi_write_png(path.c_str(), cv.w, cv.h, 3, out.data(), cv.w * 3) != 0;
}

struct Shot {
    const char* name;
    std::function<void(core::Model&, ui::Menu&)> setup;
    ImVec2 mouse;
    std::function<void(core::Model&, ui::Menu&, int)> during;
};

}

#ifdef FATHOM_LAUNCHER_SHOTS
void LoadShotArt(launcher::Home& home, const std::string& outDir) {
    static std::vector<std::pair<long long, launcher::Art>> loaded;
    static bool done = false;
    if (!done) {
        done = true;
        std::vector<std::pair<int, int>> wanted;
        for (const auto& g : launcher::Games()) wanted.push_back({g.appId, launcher::art::Capsule});
        for (const auto& g : launcher::Games()) wanted.push_back({g.appId, launcher::art::Hero});
        std::wstring dir(outDir.begin(), outDir.end());
        launcher::art::Start(wanted, dir + L"/art_cache");
        size_t got = 0;
        for (int wait = 0; wait < 600 && got < wanted.size(); ++wait) {
            launcher::art::Image img;
            bool any = false;
            while (launcher::art::Take(img)) {
                any = true;
                ++got;
                ImTextureData* t = IM_NEW(ImTextureData)();
                t->Format = ImTextureFormat_RGBA32;
                t->Width = img.width;
                t->Height = img.height;
                t->BytesPerPixel = 4;
                t->Pixels = (unsigned char*)IM_ALLOC(img.rgba.size());
                std::memcpy(t->Pixels, img.rgba.data(), img.rgba.size());
                loaded.push_back({launcher::ArtKey(img.appId, img.kind), {(ImTextureID)(intptr_t)t, (float)img.width, (float)img.height}});
            }
            if (!any) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        launcher::art::Stop();
    }
    for (const auto& [k, a] : loaded) home.art[k] = a;
}
#endif

int main(int argc, char** argv) {
    std::string outDir = argc > 1 ? argv[1] : ".";
    float density = argc > 2 ? (float)std::atof(argv[2]) : 2.0f;
    float menuScale = argc > 3 ? (float)std::atof(argv[3]) : 1.0f;
    const ImVec2 display(1120, 720);

    auto on = [](core::Model& m, const char* id) { core::SetFeature(m, *m.find(id), true); };

    std::vector<Shot> shots = {
        {"1-survival",
         [&](core::Model& m, ui::Menu& u) {
             on(m, "godmode");
             on(m, "infinite_oxygen");
             on(m, "unlimited_battery");
             core::SetValue(m, *m.find("player_speed"), 2.0f);
             m.find("no_radiation")->available = false;
         },
         ImVec2(520, 220),
         [](core::Model&, ui::Menu& u, int f) {
             if (f == 60) u.Notify("Godmode on", ICON_CHECK);
             if (f == 75) u.Notify("Infinite oxygen on", ICON_CHECK);
         }},
        {"2-biomods", [&](core::Model& m, ui::Menu& u) {
             u.tab = core::Tab::Biomods;
             on(m, "zero_cooldowns");
         }, ImVec2(900, 260), nullptr},
        {"3-crafting-confirm",
         [&](core::Model& m, ui::Menu& u) {
             u.tab = core::Tab::Crafting;
             on(m, "instant_craft");
             u.Ask("Are you sure?", "Unlock all blueprints cannot be undone. Your save files are backed up first.", "Yes, do it", [] {});
         },
         ImVec2(700, 420), nullptr},
        {"4-world", [&](core::Model& m, ui::Menu& u) {
             u.tab = core::Tab::World;
             on(m, "esp");
             on(m, "vehicle_hull");
             core::SetValue(m, *m.find("game_speed"), 1.5f);
         }, ImVec2(0, 0), nullptr},
        {"5-spawner", [](core::Model&, ui::Menu& u) { u.tab = core::Tab::Spawner; }, ImVec2(470, 318),
         [](core::Model&, ui::Menu&, int f) {
             ImGuiIO& io = ImGui::GetIO();
             if (f == 40) io.AddMouseButtonEvent(0, true);
             if (f == 42) io.AddMouseButtonEvent(0, false);
             if (f == 50) io.AddMousePosEvent(480, 392);
         }},
        {"6-map", [](core::Model&, ui::Menu& u) {
             u.tab = core::Tab::Spawner;
             u.spawnSub = 1;
         }, ImVec2(528, 421), nullptr},
        {"6b-map-scan", [](core::Model& m, ui::Menu& u) {
             u.tab = core::Tab::Spawner;
             u.spawnSub = 1;
             if (m.mapScan) m.mapScan(true);
         }, ImVec2(0, 0), nullptr},
        {"7-basehub", [](core::Model& m, ui::Menu& u) {
             u.tab = core::Tab::BaseHub;
             m.ghostActive = true;
             m.ghostName = "Deep lab";
         }, ImVec2(0, 0), nullptr},
        {"8-basehub-shared", [](core::Model&, ui::Menu& u) {
             u.tab = core::Tab::BaseHub;
             u.hubSub = 1;
         }, ImVec2(0, 0), nullptr},
        {"9-legit", [](core::Model& m, ui::Menu& u) {
             u.tab = core::Tab::Legit;
             m.legit = true;
         }, ImVec2(0, 0), nullptr},
#ifdef FATHOM_LAUNCHER_SHOTS
        {"12-launcher-games", [&](core::Model&, ui::Menu& u) {
             static launcher::Home home;
             LoadShotArt(home, outDir);
             home.game = -1;
             u.showGameTabs = false;
             u.homePage = [] { home.Draw(); };
             u.homeName = "Games";
             u.homeIcon = ICON_LAYERS;
             u.onHome = true;
         }, ImVec2(470, 230), nullptr},
        {"13-launcher-subnautica", [&](core::Model&, ui::Menu& u) {
             static launcher::Home home;
             LoadShotArt(home, outDir);
             home.game = 0;
             u.backName = "All games";
             u.onBack = [] {};
             u.homeName = "Subnautica 2";
             u.homePage = [] { home.Draw(); };
             u.homeIcon = ICON_PLAY;
             u.onHome = true;
         }, ImVec2(0, 0), nullptr},
        {"14-launcher-wide", [&](core::Model&, ui::Menu& u) {
             static launcher::Home home;
             LoadShotArt(home, outDir);
             home.game = -1;
             u.embedded = true;
             u.showGameTabs = false;
             u.homePage = [] { home.Draw(); };
             u.homeName = "Games";
             u.homeIcon = ICON_LAYERS;
             u.onHome = true;
         }, ImVec2(0, 0), nullptr},
        {"15-launcher-settings", [&](core::Model&, ui::Menu& u) {
             static launcher::Home home;
             static ui::Loader loader;
             loader.version = FATHOM_VERSION;
             loader.updateText = "You have the newest version";
             loader.gameFound = true;
             loader.s.accent = 1;
             loader.checkUpdates = [] {};
             loader.openHome = [] {};
             loader.openBackups = [] {};
             loader.openGame = [] {};
             loader.refreshArt = [] {};
             loader.backupSaves = [] { return std::string(); };
             ui::T().accent = ui::AccentColor(loader.s.accent);
             u.embedded = true;
             u.showGameTabs = false;
             u.loader = &loader;
             u.homePage = [] { home.Draw(); };
             u.homeName = "Games";
             u.homeIcon = ICON_LAYERS;
             u.onHome = false;
             u.tab = core::Tab::Settings;
         }, ImVec2(0, 0), nullptr},
        {"16-launcher-sn1", [&](core::Model& m, ui::Menu& u) {
             static launcher::Home home;
             LoadShotArt(home, outDir);
             m = core::Model();
             sn1::BuildFeatures(m);
             home.game = launcher::FindGame("subnautica");
             u.embedded = true;
             u.backName = "All games";
             u.onBack = [] {};
             u.homeName = "Subnautica";
             u.homePage = [] { home.Draw(); };
             u.homeIcon = ICON_PLAY;
             u.onHome = true;
         }, ImVec2(0, 0), nullptr},
#endif
        {"17-sn1-survival", [](core::Model& m, ui::Menu&) {
             m = core::Model();
             sn1::BuildFeatures(m);
         }, ImVec2(0, 0), nullptr},
        {"18-sn1-crafting", [](core::Model& m, ui::Menu& u) {
             m = core::Model();
             sn1::BuildFeatures(m);
             u.tab = core::Tab::Crafting;
         }, ImVec2(0, 0), nullptr},
        {"19-sn1-world", [](core::Model& m, ui::Menu& u) {
             m = core::Model();
             sn1::BuildFeatures(m);
             u.tab = core::Tab::World;
         }, ImVec2(0, 0), nullptr},
        {"20-sn1-spawner", [](core::Model& m, ui::Menu& u) {
             m = core::Model();
             sn1::BuildFeatures(m);
             u.tab = core::Tab::Spawner;
         }, ImVec2(0, 0), nullptr},
        {"10-settings", [](core::Model&, ui::Menu& u) { u.tab = core::Tab::Settings; }, ImVec2(0, 0), nullptr},
        {"11-not-host", [&](core::Model& m, ui::Menu& u) {
             m.session = core::Session::Client;
         }, ImVec2(0, 0), nullptr},
    };

    for (const Shot& shot : shots) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        io.DisplaySize = display;
        io.DisplayFramebufferScale = ImVec2(density, density);
        io.DeltaTime = 1.0f / 60.0f;
        ui::T() = ui::Theme();
        ui::LoadFonts();

        core::Model model;
        preview::Fill(model);
        model.menuScale = menuScale;
        ui::Menu menu(model);
        if (shot.setup) shot.setup(model, menu);
        io.AddMousePosEvent(shot.mouse.x, shot.mouse.y);

        Canvas cv;
        cv.w = (int)(display.x * density);
        cv.h = (int)(display.y * density);
        cv.px.assign((size_t)cv.w * cv.h * 3, 0.0f);
        const int frames = 120;
        for (int f = 0; f < frames; ++f) {
            if (shot.during) shot.during(model, menu, f);
            ImGui::NewFrame();
            preview::Backdrop();
            menu.Frame();
            ImGui::Render();
            ImDrawData* dd = ImGui::GetDrawData();
            UpdateTextures(dd);
            if (f == frames - 1) Raster(dd, cv);
        }
        std::string path = outDir + "/" + shot.name + ".png";
        std::printf("%s %s\n", SavePng(cv, path) ? "wrote" : "FAILED", path.c_str());
        ImGui::DestroyContext();
    }
    return 0;
}
