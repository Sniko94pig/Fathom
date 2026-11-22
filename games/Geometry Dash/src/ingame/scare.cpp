#include "ingame/audio.h"
#include "ingame/gd.h"
#include "ingame/image.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include "ingame/overlay.h"
#include "ingame/scare_art.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <functional>
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

struct Worker {
    HANDLE handle = nullptr;
    std::function<void()> fn;
    static DWORD WINAPI Run(void* p) {
        Worker* w = (Worker*)p;
        try {
            w->fn();
        } catch (...) {
        }
        return 0;
    }
    void Start(std::function<void()> f) {
        Join();
        fn = std::move(f);
        handle = CreateThread(nullptr, 0, Run, this, 0, nullptr);
        if (!handle) Run(this);
    }
    void Join() {
        if (!handle) return;
        WaitForSingleObject(handle, INFINITE);
        CloseHandle(handle);
        handle = nullptr;
    }
};

constexpr int kFaceSize = 512;
constexpr double kShow = 1.0;
constexpr double kSound = 1.4;
constexpr double kFade = 0.15;

bool ready = false;
fs::path userDir, builtinDir;
std::mt19937 rng;

Worker facesWorker;
std::atomic<bool> facesDone{false};
std::vector<Image> faces;
uint64_t faceTex[scare_art::kFaces] = {};
void* screams[scare_art::kScreams] = {};
bool screamTried[scare_art::kScreams] = {};

bool wasOn = false;
float plannedEvery = 0.0f;
float seenEvery = 0.0f;
double everyChangedAt = 0.0;
double nextAt = 0.0;
std::atomic<bool> nowWanted{false};

bool showing = false;
double startAt = 0.0;
uint64_t tex = 0;
int texW = 0, texH = 0;
bool texOwned = false;
void* channel = nullptr;
void* userSound = nullptr;
double soundAt = 0.0;

Pick pending;
bool pendingValid = false;
Worker decodeWorker;
std::atomic<bool> decodeDone{false};
Image decoded;

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
    if (!image::Load(path.wstring(), img.rgba, img.w, img.h)) return {};
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

void JoinDecode() { decodeWorker.Join(); }

void Prepare() {
    JoinDecode();
    pending = Choose();
    pendingValid = true;
    decoded = Image{};
    decodeDone = false;
    if (pending.picture.empty()) {
        decodeDone = true;
        return;
    }
    fs::path path = pending.picture;
    decodeWorker.Start([path] {
        decoded = Decode(path);
        decodeDone = true;
    });
}

bool FacesReady() {
    if (!facesDone.load()) return false;
    facesWorker.Join();
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
    float s = gd::C().speed.load();
    if (!std::isfinite(s)) s = 1.0f;
    s = std::clamp(s, 0.1f, 3.0f);
    return 1.0f / s;
}

bool ShowFace(int face) {
    if (!FacesReady() || face < 0 || face >= scare_art::kFaces) return false;
    if (!faceTex[face]) {
        const Image& f = faces[(size_t)face];
        faceTex[face] = overlay::MakeTexture(f.rgba.data(), f.w, f.h);
    }
    tex = faceTex[face];
    texW = kFaceSize;
    texH = kFaceSize;
    return tex != 0;
}

bool ShowImage(const Image& img) {
    if (img.rgba.empty()) return false;
    tex = overlay::MakeTexture(img.rgba.data(), img.w, img.h);
    texOwned = tex != 0;
    texW = img.w;
    texH = img.h;
    return tex != 0;
}

void PlaySoundFor(const Pick& p) {
    void* snd = nullptr;
    if (!p.sound.empty()) {
        userSound = audio::Load(gd::Narrow(p.sound.wstring()), true);
        if (!userSound) LOG("jumpscare sound could not be played: %s", gd::Narrow(p.sound.filename().wstring()).c_str());
        snd = userSound;
    }
    if (!snd) {
        int s = p.scream >= 0 ? p.scream : Roll(scare_art::kScreams);
        if (!screamTried[s]) {
            screamTried[s] = true;
            fs::path wav = builtinDir / (L"scream" + std::to_wstring(s + 1) + L".wav");
            screams[s] = audio::Load(gd::Narrow(wav.wstring()), false);
        }
        snd = screams[s];
    }
    channel = snd ? audio::Play(snd, 1.0f, Pitch()) : nullptr;
}

void Trigger() {
    DropPicture();
    StopSound();
    Pick p = pendingValid ? pending : Choose();
    bool shown = false;
    bool used = false;
    if (p.face >= 0) {
        shown = ShowFace(p.face);
        used = true;
    } else if (decodeDone.load()) {
        JoinDecode();
        shown = ShowImage(decoded);
        if (!shown) LOG("jumpscare picture could not be read: %s", gd::Narrow(p.picture.filename().wstring()).c_str());
        used = true;
    }
    if (!shown) {
        Pick fallback = p;
        fallback.face = Roll(scare_art::kFaces);
        fallback.picture.clear();
        shown = ShowFace(fallback.face);
        if (!shown) return;
    }
    PlaySoundFor(p);
    startAt = Clock();
    soundAt = startAt;
    showing = true;
    if (used) {
        pendingValid = false;
        Prepare();
    }
}

void Plan(double now) {
    float every = std::clamp(gd::C().scareEvery.load(), 1.0f, 30.0f);
    double factor = std::uniform_real_distribution<double>(0.5, 1.5)(rng);
    nextAt = now + (double)every * 60.0 * factor;
    plannedEvery = every;
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
    rng.seed(std::random_device{}());
    userDir = fs::path(gd::Folder()) / L"jumpscares";
    builtinDir = fs::path(gd::Folder()) / L"builtin";
    std::error_code ec;
    fs::create_directories(userDir, ec);
    try {
        WriteScreams();
    } catch (...) {
        LOG("jumpscare sounds could not be written");
    }
    facesWorker.Start([] {
        std::vector<Image> out((size_t)scare_art::kFaces);
        for (int i = 0; i < scare_art::kFaces; ++i) {
            scare_art::Face(i, kFaceSize, out[(size_t)i].rgba);
            out[(size_t)i].w = kFaceSize;
            out[(size_t)i].h = kFaceSize;
        }
        faces.swap(out);
        facesDone = true;
    });
    if (!audio::Install()) LOG("jumpscares will have no sound");
    ready = true;
    return true;
}

bool Ready() { return ready; }

void Tick(float) {
    if (!ready) return;
    if (facesDone.load()) facesWorker.Join();
    if (decodeDone.load()) decodeWorker.Join();
    double now = Clock();
    bool on = gd::C().scares.load();
    float every = gd::C().scareEvery.load();
    if (std::fabs(every - seenEvery) > 0.01f) {
        seenEvery = every;
        everyChangedAt = now;
    }
    if (on && !wasOn) {
        Plan(now);
        if (!pendingValid) Prepare();
    } else if (on && std::fabs(plannedEvery - every) > 0.01f && now - everyChangedAt >= 1.0) {
        Plan(now);
    }
    wasOn = on;
    if (nowWanted.exchange(false)) {
        if (!pendingValid) Prepare();
        Trigger();
    } else if (on && !showing && now >= nextAt && !gd::C().menuOpen.load()) {
        if (!pendingValid) Prepare();
        Trigger();
        Plan(now);
    }
    if (showing && now - startAt > kShow) DropPicture();
    if (channel) {
        double t = now - soundAt;
        if (t > kSound) StopSound();
        else if (t > kSound - kFade) audio::SetVolume(channel, (float)std::max(0.0, (kSound - t) / kFade));
    } else if (userSound && now - soundAt > kSound) {
        StopSound();
    }
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
    JoinDecode();
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
    facesWorker.Join();
    JoinDecode();
}

void Forget() {
    tex = 0;
    texOwned = false;
    showing = false;
    for (uint64_t& t : faceTex) t = 0;
}

}
