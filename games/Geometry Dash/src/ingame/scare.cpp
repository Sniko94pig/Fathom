#include "ingame/audio.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include "ingame/overlay.h"
#include "ingame/scare_art.h"
#include "ui/atlas.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <future>
#include <random>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace scare {

namespace {

struct Image {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
};

struct Pick {
    int face = -1;
    fs::path picture;
    int scream = -1;
    fs::path sound;
};

constexpr int kFaceSize = 512;
constexpr double kShow = 1.0;
constexpr double kSound = 1.4;

bool ready = false;
fs::path userDir, builtinDir;
std::mt19937 rng{std::random_device{}()};
std::future<std::vector<Image>> facesJob;
std::vector<Image> faces;
uint64_t faceTex[scare_art::kFaces] = {};
void* screams[scare_art::kScreams] = {};
bool screamTried[scare_art::kScreams] = {};

bool wasOn = false;
float plannedEvery = 0.0f;
double nextAt = 0.0;
std::atomic<bool> nowWanted{false};

bool showing = false;
double startAt = 0.0;
uint64_t tex = 0;
int texW = 0, texH = 0;
bool texOwned = false;
void* channel = nullptr;
void* userSound = nullptr;
double soundUntil = 0.0;

Pick pending;
bool pendingValid = false;
std::future<Image> pendingImage;

double Clock() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)freq.QuadPart;
}

std::wstring Lower(std::wstring s) {
    for (wchar_t& c : s) c = (wchar_t)std::towlower(c);
    return s;
}

bool IsPicture(const fs::path& p) {
    std::wstring e = Lower(p.extension().wstring());
    return e == L".png" || e == L".jpg" || e == L".jpeg" || e == L".bmp" || e == L".gif" || e == L".tga";
}

bool IsSound(const fs::path& p) {
    std::wstring e = Lower(p.extension().wstring());
    return e == L".mp3" || e == L".ogg" || e == L".wav" || e == L".flac";
}

void Scan(std::vector<fs::path>& pics, std::vector<fs::path>& sounds) {
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(userDir, ec)) {
        if (!e.is_regular_file(ec)) continue;
        if (IsPicture(e.path())) pics.push_back(e.path());
        else if (IsSound(e.path())) sounds.push_back(e.path());
    }
}

int Roll(int n) { return n <= 1 ? 0 : std::uniform_int_distribution<int>(0, n - 1)(rng); }

Pick Choose() {
    std::vector<fs::path> pics, sounds;
    Scan(pics, sounds);
    Pick p;
    int pi = Roll(scare_art::kFaces + (int)pics.size());
    if (pi < scare_art::kFaces) p.face = pi;
    else p.picture = pics[(size_t)(pi - scare_art::kFaces)];
    if (!p.picture.empty()) {
        std::wstring stem = Lower(p.picture.stem().wstring());
        for (const fs::path& s : sounds)
            if (Lower(s.stem().wstring()) == stem) {
                p.sound = s;
                return p;
            }
    }
    int si = Roll(scare_art::kScreams + (int)sounds.size());
    if (si < scare_art::kScreams) p.scream = si;
    else p.sound = sounds[(size_t)(si - scare_art::kScreams)];
    return p;
}

Image Decode(const fs::path& path) {
    Image img;
    if (!ui::LoadImageFile(path.wstring(), img.rgba, img.w, img.h)) return {};
    int big = std::max(img.w, img.h);
    if (big > 2048) {
        int f = (big + 2047) / 2048;
        int nw = std::max(1, img.w / f), nh = std::max(1, img.h / f);
        std::vector<uint8_t> out((size_t)nw * nh * 4);
        for (int y = 0; y < nh; ++y)
            for (int x = 0; x < nw; ++x)
                for (int c = 0; c < 4; ++c) {
                    int sum = 0, cnt = 0;
                    for (int dy = 0; dy < f; ++dy)
                        for (int dx = 0; dx < f; ++dx) {
                            int sx = x * f + dx, sy = y * f + dy;
                            if (sx >= img.w || sy >= img.h) continue;
                            sum += img.rgba[((size_t)sy * img.w + sx) * 4 + c];
                            ++cnt;
                        }
                    out[((size_t)y * nw + x) * 4 + c] = (uint8_t)(cnt ? sum / cnt : 0);
                }
        img.rgba.swap(out);
        img.w = nw;
        img.h = nh;
    }
    return img;
}

void Prepare() {
    pending = Choose();
    pendingValid = true;
    if (!pending.picture.empty()) {
        fs::path path = pending.picture;
        pendingImage = std::async(std::launch::async, [path] { return Decode(path); });
    }
}

void Plan(double now) {
    float every = std::clamp(gd::C().scareEvery.load(), 1.0f, 30.0f);
    double factor = std::uniform_real_distribution<double>(0.5, 1.5)(rng);
    nextAt = now + (double)every * 60.0 * factor;
    plannedEvery = every;
    Prepare();
}

bool FacesReady() {
    if (faces.empty() && facesJob.valid()) faces = facesJob.get();
    return faces.size() == (size_t)scare_art::kFaces;
}

void StopSound() {
    if (channel) audio::Stop(channel);
    channel = nullptr;
    if (userSound) audio::Free(userSound);
    userSound = nullptr;
}

void DropPicture() {
    if (texOwned && tex) overlay::FreeTexture(tex);
    tex = 0;
    texOwned = false;
    showing = false;
}

float Pitch() {
    float s = std::clamp(gd::C().speed.load(), 0.1f, 3.0f);
    return std::isfinite(s) && s > 0.0f ? 1.0f / s : 1.0f;
}

void Start(Pick p, Image* img) {
    DropPicture();
    StopSound();
    if (p.face >= 0) {
        if (!FacesReady()) return;
        if (!faceTex[p.face]) {
            const Image& f = faces[(size_t)p.face];
            faceTex[p.face] = overlay::MakeTexture(f.rgba.data(), f.w, f.h);
        }
        tex = faceTex[p.face];
        texW = kFaceSize;
        texH = kFaceSize;
    } else {
        Image local;
        if (!img || img->rgba.empty()) {
            local = Decode(p.picture);
            img = &local;
        }
        if (img->rgba.empty()) {
            LOG("jumpscare picture could not be read: %s", gd::Narrow(p.picture.filename().wstring()).c_str());
            p.face = Roll(scare_art::kFaces);
            Start(p, nullptr);
            return;
        }
        tex = overlay::MakeTexture(img->rgba.data(), img->w, img->h);
        texOwned = tex != 0;
        texW = img->w;
        texH = img->h;
    }
    if (!tex) return;
    void* snd = nullptr;
    if (p.scream >= 0) {
        if (!screamTried[p.scream]) {
            screamTried[p.scream] = true;
            fs::path wav = builtinDir / (L"scream" + std::to_wstring(p.scream + 1) + L".wav");
            screams[p.scream] = audio::Load(gd::Narrow(wav.wstring()));
        }
        snd = screams[p.scream];
    } else if (!p.sound.empty()) {
        userSound = audio::Load(gd::Narrow(p.sound.wstring()));
        if (!userSound) LOG("jumpscare sound could not be played: %s", gd::Narrow(p.sound.filename().wstring()).c_str());
        snd = userSound;
    }
    channel = snd ? audio::Play(snd, 1.0f, Pitch()) : nullptr;
    startAt = Clock();
    soundUntil = startAt + kSound;
    showing = true;
}

void Trigger(bool planned) {
    if (!planned) {
        Start(Choose(), nullptr);
        return;
    }
    Pick p;
    Image img;
    if (pendingValid) {
        p = pending;
        if (pendingImage.valid()) img = pendingImage.get();
    } else {
        p = Choose();
    }
    pendingValid = false;
    Start(p, img.rgba.empty() ? nullptr : &img);
}

void WriteScreams() {
    std::error_code ec;
    fs::create_directories(builtinDir, ec);
    for (int i = 0; i < scare_art::kScreams; ++i) {
        std::vector<uint8_t> wav;
        scare_art::Scream(i, wav);
        fs::path path = builtinDir / (L"scream" + std::to_wstring(i + 1) + L".wav");
        if (fs::exists(path, ec) && fs::file_size(path, ec) == wav.size()) continue;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write((const char*)wav.data(), (std::streamsize)wav.size());
    }
}

}

bool Install() {
    userDir = fs::path(gd::Folder()) / L"jumpscares";
    builtinDir = fs::path(gd::Folder()) / L"builtin";
    std::error_code ec;
    fs::create_directories(userDir, ec);
    try {
        WriteScreams();
    } catch (...) {
        LOG("jumpscare sounds could not be written");
    }
    facesJob = std::async(std::launch::async, [] {
        std::vector<Image> out((size_t)scare_art::kFaces);
        for (int i = 0; i < scare_art::kFaces; ++i) {
            scare_art::Face(i, kFaceSize, out[(size_t)i].rgba);
            out[(size_t)i].w = kFaceSize;
            out[(size_t)i].h = kFaceSize;
        }
        return out;
    });
    if (!audio::Install()) LOG("jumpscares will have no sound");
    ready = true;
    return true;
}

bool Ready() { return ready; }

void Tick(float) {
    if (!ready) return;
    double now = Clock();
    bool on = gd::C().scares.load();
    if (on && (!wasOn || std::fabs(plannedEvery - gd::C().scareEvery.load()) > 0.01f)) Plan(now);
    wasOn = on;
    if (nowWanted.exchange(false)) {
        Trigger(false);
    } else if (on && !showing && now >= nextAt && !gd::C().menuOpen.load()) {
        Trigger(true);
        Plan(now);
    }
    if (showing && now - startAt > kShow) DropPicture();
    if ((channel || userSound) && now > soundUntil) StopSound();
}

void Frame() {
    if (!showing || !tex || texW <= 0 || texH <= 0) return;
    double t = Clock() - startAt;
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ImVec2 ds = ImGui::GetIO().DisplaySize;
    float k = (float)std::min(1.0, t / kShow);
    float zoom = 1.0f + 0.18f * std::exp(-6.0f * (float)t);
    float scale = std::max(ds.x / (float)texW, ds.y / (float)texH) * zoom;
    float w = (float)texW * scale, h = (float)texH * scale;
    float shake = 16.0f * (1.0f - k);
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float ox = u(rng) * shake, oy = u(rng) * shake;
    float alpha = t < 0.85 ? 1.0f : std::max(0.0f, 1.0f - (float)(t - 0.85) / 0.15f);
    int a = (int)(255.0f * alpha);
    ImVec2 p0(ds.x * 0.5f - w * 0.5f + ox, ds.y * 0.5f - h * 0.5f + oy);
    dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(0, 0, 0, a));
    dl->AddImage((ImTextureID)tex, p0, ImVec2(p0.x + w, p0.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32(255, 255, 255, a));
    if (t < 0.12) dl->AddRectFilled(ImVec2(0, 0), ds, IM_COL32(255, 0, 0, (int)(110.0 * (1.0 - t / 0.12))));
}

void Now() {
    if (ready) nowWanted = true;
}

void Off() {
    gd::C().scares = false;
    wasOn = false;
    nowWanted = false;
    StopSound();
    DropPicture();
    if (pendingImage.valid()) pendingImage.wait();
    pendingImage = {};
    pendingValid = false;
    for (int i = 0; i < scare_art::kScreams; ++i) {
        if (screams[i]) audio::Free(screams[i]);
        screams[i] = nullptr;
        screamTried[i] = false;
    }
}

void Cleanup() {
    DropPicture();
    for (uint64_t& t : faceTex) {
        if (t) overlay::FreeTexture(t);
        t = 0;
    }
    if (facesJob.valid()) facesJob.wait();
}

}
