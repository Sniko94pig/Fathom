#include "game/internal.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iterator>
#include <memory>

namespace bl3 {

namespace {

const int kGrid = 6;
const int kPieces = kGrid * kGrid;
const int kShotPx = 512;
const int kPiecePx = 256;
const float kPieceCm = 20000.0f;
const float kAbove = 60000.0f;
const double kGap = 0.12;
const uint8_t kHalfFloat = 5, kOrtho = 1;

struct Setting {
    uint8_t source;
    bool flip;
};

const Setting kSettings[] = {{7, false}, {2, false}, {7, true}, {2, true}};
const int kSettingCount = (int)(sizeof(kSettings) / sizeof(kSettings[0]));

struct Piece {
    std::vector<uint8_t> rgba;
    bool read = false;
    bool blank = false;
};

struct Picture {
    std::vector<uint8_t> rgba;
    int drawn = 0;
};

struct Batch {
    std::vector<Piece> done;
    std::vector<std::future<Piece>> pending;
};

struct Run {
    bool active = false;
    bool stopAsked = false;
    bool composing = false;
    bool locked = false;
    bool hasFlag = false;
    bool flagDefault = false;
    void* world = nullptr;
    ue::FWeak actor, comp, rt;
    float topX = 0, leftY = 0, camZ = 0;
    std::vector<int> order;
    size_t next = 0;
    int setting = 0, tried = 0, blanks = 0;
    double nextAt = 0;
    std::vector<Piece> done;
    std::vector<std::future<Piece>> pending;
    std::future<Picture> picture;
} run;

struct Photo {
    bool shown = false;
    void* world = nullptr;
    std::string map;
} photo;

std::atomic<int> ask{0};
std::atomic<bool> cancel{false};
int goodSetting = 0;
uint8_t srgb[256][256] = {};
bool srgbReady = false;

std::wstring ShotDir() { return Folder() + L"\\mapshot"; }

std::wstring ShotName(int t) { return L"tile_" + std::to_wstring(t) + L".hdr"; }

std::wstring ShotFile(int t) { return ShotDir() + L"\\" + ShotName(t); }

void Sweep() {
    std::error_code ec;
    for (int t = 0; t < kPieces; ++t) std::filesystem::remove(std::filesystem::path(ShotFile(t)), ec);
    std::filesystem::remove(std::filesystem::path(ShotDir()), ec);
}

void BuildSrgb() {
    if (srgbReady) return;
    double edge[255];
    for (int i = 0; i < 255; ++i) {
        double c = (i + 0.5) / 255.0;
        edge[i] = c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
    }
    for (int e = 0; e < 256; ++e)
        for (int m = 0; m < 256; ++m) {
            double v = e ? m / 255.0 * std::ldexp(1.0, e - 128) : 0.0;
            srgb[e][m] = (uint8_t)(std::upper_bound(edge, edge + 255, v) - edge);
        }
    srgbReady = true;
}

bool NextLine(const std::vector<uint8_t>& f, size_t& pos, std::string& out) {
    out.clear();
    while (pos < f.size() && f[pos] != '\n') {
        if (out.size() >= 128) return false;
        out.push_back((char)f[pos++]);
    }
    if (pos >= f.size()) return false;
    ++pos;
    if (!out.empty() && out.back() == '\r') out.pop_back();
    return true;
}

bool SizeLine(const std::string& s, int& w, int& h, bool& flipY, bool& flipX) {
    const char* p = s.c_str();
    if ((p[0] != '-' && p[0] != '+') || p[1] != 'Y') return false;
    flipY = p[0] == '+';
    char* end = nullptr;
    long rows = std::strtol(p + 2, &end, 10);
    while (*end == ' ') ++end;
    if ((end[0] != '-' && end[0] != '+') || end[1] != 'X') return false;
    flipX = end[0] == '-';
    long cols = std::strtol(end + 2, &end, 10);
    if (rows < 1 || cols < 1 || rows > 8192 || cols > 8192) return false;
    w = (int)cols;
    h = (int)rows;
    return true;
}

bool Scanline(const std::vector<uint8_t>& f, size_t& pos, int w, uint8_t* row) {
    size_t n = f.size();
    if (w >= 8 && w < 32768 && pos + 4 <= n && f[pos] == 2 && f[pos + 1] == 2 && !(f[pos + 2] & 0x80)) {
        if (((f[pos + 2] << 8) | f[pos + 3]) != w) return false;
        pos += 4;
        for (int c = 0; c < 4; ++c)
            for (int x = 0; x < w;) {
                if (pos >= n) return false;
                int count = f[pos++];
                bool same = count > 128;
                if (same) count -= 128;
                size_t need = same ? 1 : (size_t)count;
                if (count == 0 || count > w - x || pos + need > n) return false;
                for (int k = 0; k < count; ++k, ++x) row[x * 4 + c] = f[same ? pos : pos + k];
                pos += need;
            }
        return true;
    }
    int shift = 0;
    for (int x = 0; x < w;) {
        if (pos + 4 > n) return false;
        const uint8_t* p = f.data() + pos;
        pos += 4;
        if (p[0] == 1 && p[1] == 1 && p[2] == 1) {
            size_t repeat = (size_t)p[3] << shift;
            if (x == 0 || shift > 16 || repeat > (size_t)(w - x)) return false;
            for (size_t k = 0; k < repeat; ++k, ++x) std::memcpy(row + x * 4, row + (x - 1) * 4, 4);
            shift += 8;
            continue;
        }
        std::memcpy(row + x * 4, p, 4);
        ++x;
        shift = 0;
    }
    return true;
}

bool DecodeHdr(const std::vector<uint8_t>& f, std::vector<uint8_t>& px, int& w, int& h) {
    size_t pos = 0;
    std::string line;
    if (!NextLine(f, pos, line) || line.rfind("#?", 0) != 0) return false;
    do {
        if (!NextLine(f, pos, line)) return false;
        if (line.rfind("FORMAT=", 0) == 0 && line != "FORMAT=32-bit_rle_rgbe") return false;
    } while (!line.empty());
    bool flipY = false, flipX = false;
    if (!NextLine(f, pos, line) || !SizeLine(line, w, h, flipY, flipX)) return false;
    std::vector<uint8_t> row((size_t)w * 4);
    px.assign((size_t)w * h * 4, 255);
    for (int y = 0; y < h; ++y) {
        if (!Scanline(f, pos, w, row.data())) return false;
        uint8_t* out = px.data() + (size_t)(flipY ? h - 1 - y : y) * w * 4;
        for (int x = 0; x < w; ++x) {
            const uint8_t* s = row.data() + (size_t)x * 4;
            uint8_t* d = out + (size_t)(flipX ? w - 1 - x : x) * 4;
            d[0] = srgb[s[3]][s[0]];
            d[1] = srgb[s[3]][s[1]];
            d[2] = srgb[s[3]][s[2]];
        }
    }
    return true;
}

bool Uniform(const std::vector<uint8_t>& px, int w, int h) {
    size_t total = (size_t)w * h;
    double sum[3] = {0, 0, 0};
    for (size_t i = 0; i < total; ++i)
        for (int c = 0; c < 3; ++c) sum[c] += px[i * 4 + c];
    size_t close = 0;
    for (size_t i = 0; i < total; ++i) {
        bool same = true;
        for (int c = 0; c < 3 && same; ++c) same = std::fabs(px[i * 4 + c] - sum[c] / (double)total) <= 4.0;
        if (same) ++close;
    }
    return close * 100 >= total * 97;
}

Piece Develop(const std::wstring& file) {
    Piece p;
    try {
        std::vector<uint8_t> bytes;
        {
            std::ifstream in(std::filesystem::path(file), std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        static const uint8_t png[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
        std::vector<uint8_t> px;
        int w = 0, h = 0;
        bool ok = false;
        if (!cancel && bytes.size() > sizeof(png))
            ok = std::memcmp(bytes.data(), png, sizeof(png)) == 0 ? ui::LoadImageFile(file, px, w, h) : DecodeHdr(bytes, px, w, h);
        std::error_code ec;
        std::filesystem::remove(std::filesystem::path(file), ec);
        if (!ok || w < 1 || h < 1 || px.size() < (size_t)w * h * 4) return p;
        p.read = true;
        p.blank = Uniform(px, w, h);
        p.rgba.assign((size_t)kPiecePx * kPiecePx * 4, 0);
        for (int oy = 0; oy < kPiecePx; ++oy) {
            int y0 = oy * h / kPiecePx, y1 = std::max(y0 + 1, (oy + 1) * h / kPiecePx);
            for (int ox = 0; ox < kPiecePx; ++ox) {
                int x0 = ox * w / kPiecePx, x1 = std::max(x0 + 1, (ox + 1) * w / kPiecePx);
                int sum[3] = {0, 0, 0}, solid = 0, count = 0;
                for (int y = y0; y < y1 && y < h; ++y)
                    for (int x = x0; x < x1 && x < w; ++x) {
                        const uint8_t* s = px.data() + ((size_t)y * w + x) * 4;
                        ++count;
                        if (s[0] <= 2 && s[1] <= 2 && s[2] <= 2) continue;
                        for (int c = 0; c < 3; ++c) sum[c] += s[c];
                        ++solid;
                    }
                if (!solid) continue;
                uint8_t* d = p.rgba.data() + ((size_t)oy * kPiecePx + ox) * 4;
                for (int c = 0; c < 3; ++c) d[c] = (uint8_t)(sum[c] / solid);
                d[3] = (uint8_t)(255 * solid / count);
            }
        }
    } catch (...) {
        p = Piece{};
    }
    return p;
}

Picture Compose(Batch& b) {
    Picture pic;
    try {
        int size = kGrid * kPiecePx;
        pic.rgba.assign((size_t)size * size * 4, 0);
        for (int t = 0; t < kPieces && t < (int)b.done.size() && t < (int)b.pending.size(); ++t) {
            if (b.pending[t].valid()) b.done[t] = b.pending[t].get();
            const Piece& p = b.done[t];
            if (cancel || !p.read || p.rgba.size() < (size_t)kPiecePx * kPiecePx * 4) continue;
            int ox = (t % kGrid) * kPiecePx, oy = (t / kGrid) * kPiecePx;
            for (int y = 0; y < kPiecePx; ++y)
                std::memcpy(pic.rgba.data() + ((size_t)(oy + y) * size + ox) * 4, p.rgba.data() + (size_t)y * kPiecePx * 4,
                            (size_t)kPiecePx * 4);
            if (!p.blank) ++pic.drawn;
        }
    } catch (...) {
        pic = Picture{};
    }
    return pic;
}

void Show(bool on, float progress, const std::string& text) {
    WithModel([&](core::Model& m) {
        m.mapScanning = on;
        m.mapScanProgress = progress;
        m.mapScanText = text;
    });
}

void Forget() {
    photo = Photo{};
    WithModel([](core::Model& m) {
        m.mapMinX = m.mapMinY = m.mapMaxX = m.mapMaxY = 0;
        m.mapPhotos = 0;
        m.mapTexture = 0;
    });
}

void CheckPhoto() {
    if (!photo.shown || !st.world || st.world == photo.world) return;
    if (ue::Name(st.world) == photo.map) {
        photo.world = st.world;
        return;
    }
    Forget();
}

void Remove(void* actor) {
    if (!ue::Alive(actor)) return;
    ue::Call gbx(actor, "GbxDestroyActor");
    gbx.SetBool("bDestroyImmediately", true);
    if (gbx.Ok() && gbx.Run()) return;
    ue::Call(actor, "K2_DestroyActor_DEPRECATED").Run();
}

void DropRig() {
    Remove(ue::WeakGet(run.actor));
    run.actor = run.comp = run.rt = ue::FWeak{};
}

ue::FVector Spot(int t) {
    return {run.topX - ((float)(t / kGrid) + 0.5f) * kPieceCm, run.leftY + ((float)(t % kGrid) + 0.5f) * kPieceCm, run.camZ};
}

bool SetupRig(const ue::FVector& at) {
    void* actor = SpawnActor("/Script/Engine.SceneCapture2D", at, 0.0f);
    void* comp = actor ? ue::Ptr(actor, "CaptureComponent2D") : nullptr;
    void* rt = nullptr;
    if (comp) {
        ue::Call mk(Lib("KismetRenderingLibrary"), "CreateRenderTarget2D");
        mk.Set<void*>("WorldContextObject", st.world).Set<int32_t>("Width", kShotPx).Set<int32_t>("Height", kShotPx);
        mk.Set<uint8_t>("Format", kHalfFloat);
        void* made = mk.Run() ? mk.Get<void*>("ReturnValue") : nullptr;
        if (ue::Alive(made)) rt = made;
    }
    if (!comp || !rt) {
        LOG("map photos: could not set up the camera (actor %p, capture %p, target %p)", actor, comp, rt);
        Remove(actor);
        return false;
    }
    void* cls = ue::ClassOf(comp);
    ue::Write<void*>(comp, ue::FindProp(cls, "TextureTarget"), rt);
    ue::Write<uint8_t>(comp, ue::FindProp(cls, "ProjectionType"), kOrtho);
    ue::SetFloat(comp, "OrthoWidth", kPieceCm);
    ue::SetBool(comp, "bCaptureEveryFrame", false);
    ue::SetBool(comp, "bCaptureOnMovement", false);
    ue::Prop flag = ue::FindProp(cls, "bGbxRuntimeCapture");
    run.hasFlag = (bool)flag;
    run.flagDefault = flag && ue::ReadBool(comp, flag);
    int hidden = 0;
    ue::ForEachOf(Cls("GbxSkyActor"), [&](void* sky) {
        ue::Call h(comp, "HideActorComponents");
        h.Set<void*>("InActor", sky);
        if (h.Run()) ++hidden;
        return true;
    });
    run.actor = ue::MakeWeak(actor);
    run.comp = ue::MakeWeak(comp);
    run.rt = ue::MakeWeak(rt);
    LOG("map photos: camera ready, %d sky actors hidden", hidden);
    return true;
}

void ApplySetting() {
    void* comp = ue::WeakGet(run.comp);
    if (!comp) return;
    void* cls = ue::ClassOf(comp);
    const Setting& s = kSettings[run.setting];
    ue::Write<uint8_t>(comp, ue::FindProp(cls, "CaptureSource"), s.source);
    ue::Prop flag = ue::FindProp(cls, "bGbxRuntimeCapture");
    if (flag) ue::WriteBool(comp, flag, s.flip != run.flagDefault);
}

bool NextSetting() {
    while (run.tried < kSettingCount) {
        run.setting = (run.setting + 1) % kSettingCount;
        ++run.tried;
        if (kSettings[run.setting].flip && !run.hasFlag) continue;
        ApplySetting();
        return true;
    }
    return false;
}

enum class Shot { Ok, Lost, Unsaved };

Shot Take(int t) {
    void* actor = ue::WeakGet(run.actor);
    void* comp = ue::WeakGet(run.comp);
    void* rt = ue::WeakGet(run.rt);
    if (!actor || !comp || !rt) return Shot::Lost;
    ue::Call mv(actor, "K2_SetActorLocationAndRotation");
    mv.Set("NewLocation", Spot(t)).Set("NewRotation", ue::FRotator{-90.0f, 0.0f, 0.0f}).SetBool("bSweep", false).SetBool("bTeleport", true);
    if (!mv.Run()) return Shot::Lost;
    void* lib = Lib("KismetRenderingLibrary");
    ue::Call clear(lib, "ClearRenderTarget2D");
    clear.Set<void*>("WorldContextObject", st.world).Set<void*>("TextureRenderTarget", rt).Set("ClearColor", ue::FLinearColor{0, 0, 0, 0});
    clear.Run();
    if (!ue::Call(comp, "CaptureScene").Run()) return Shot::Lost;
    std::filesystem::path file(ShotFile(t));
    std::error_code ec;
    std::filesystem::remove(file, ec);
    ue::Call exp(lib, "ExportRenderTarget");
    exp.Set<void*>("WorldContextObject", st.world).Set<void*>("TextureRenderTarget", rt);
    exp.SetString("FilePath", ShotDir()).SetString("FileName", ShotName(t));
    if (!exp.Run()) return Shot::Lost;
    ec.clear();
    uintmax_t size = std::filesystem::file_size(file, ec);
    if (!ec && size > 0) return Shot::Ok;
    LOG("map photos: the game wrote no picture to %s (%s), the folder name must not contain & %% # @ ^", ue::Utf8(file.wstring()).c_str(),
        ec ? "missing" : "empty");
    return Shot::Unsaved;
}

void End(const char* note) {
    cancel = true;
    DropRig();
    for (std::future<Piece>& f : run.pending)
        if (f.valid()) f.wait();
    if (run.picture.valid()) run.picture.wait();
    run = Run{};
    cancel = false;
    Sweep();
    Show(false, 0.0f, "");
    if (note) Note(note);
}

void Publish(Picture& pic) {
    int size = kGrid * kPiecePx;
    float span = kGrid * kPieceCm;
    float minX = run.leftY / 100.0f, maxX = (run.leftY + span) / 100.0f;
    float minY = -run.topX / 100.0f, maxY = (span - run.topX) / 100.0f;
    int drawn = pic.drawn;
    WithModel([&](core::Model& m) {
        m.mapMinX = minX;
        m.mapMinY = minY;
        m.mapMaxX = maxX;
        m.mapMaxY = maxY;
        m.mapPhotos = drawn;
        m.mapTexture = 0;
    });
    overlay::SetMapImage(std::move(pic.rgba), size, size);
    photo.shown = true;
    photo.world = st.world;
    photo.map = ue::Name(st.world);
}

void BeginCompose() {
    DropRig();
    if (run.next == 0) {
        End("Map scan stopped");
        return;
    }
    auto batch = std::make_shared<Batch>();
    batch->done = std::move(run.done);
    batch->pending = std::move(run.pending);
    run.done.clear();
    run.pending.clear();
    run.picture = std::async(std::launch::async, [batch] { return Compose(*batch); });
    run.composing = true;
    Show(true, 1.0f, "Putting the map together...");
}

void Start() {
    if (run.active) return;
    if (!st.world || !st.pawn) {
        Note("Load your save first");
        return;
    }
    Sweep();
    std::error_code ec;
    std::filesystem::path dir(ShotDir());
    std::filesystem::create_directories(dir, ec);
    if (!std::filesystem::is_directory(dir, ec)) {
        LOG("map photos: could not create %s", ue::Utf8(dir.wstring()).c_str());
        Note("Map photos could not be saved");
        return;
    }
    BuildSrgb();
    void* me = st.vehicle ? st.vehicle : st.pawn;
    ue::FVector at = Location(me), ground;
    float base = TraceDown(at, 500000.0f, ground) ? ground.z : at.z;
    float half = kGrid * kPieceCm * 0.5f;
    run = Run{};
    run.world = st.world;
    run.topX = at.x + half;
    run.leftY = at.y - half;
    run.camZ = base + kAbove;
    for (int t = 0; t < kPieces; ++t) run.order.push_back(t);
    auto rank = [](int t) {
        float dx = (float)(t % kGrid) - (kGrid - 1) * 0.5f, dy = (float)(t / kGrid) - (kGrid - 1) * 0.5f;
        return dx * dx + dy * dy;
    };
    std::stable_sort(run.order.begin(), run.order.end(), [&](int a, int b) { return rank(a) < rank(b); });
    if (!SetupRig(Spot(run.order[0]))) {
        run = Run{};
        Sweep();
        Note("The map camera could not be set up");
        return;
    }
    run.done.resize(kPieces);
    run.pending.resize(kPieces);
    run.setting = std::clamp(goodSetting, 0, kSettingCount - 1);
    run.tried = 1;
    run.active = true;
    ApplySetting();
    Show(true, 0.0f, "Starting the map photos...");
    LOG("map photos: %d pieces of %.0f m around %.0f, %.0f, camera at %.0f m, setting %d", kPieces, kPieceCm / 100.0f, at.y / 100.0f,
        -at.x / 100.0f, run.camZ / 100.0f, run.setting + 1);
}

void Stop() {
    if (!run.active || run.composing || run.stopAsked) return;
    run.stopAsked = true;
    Show(true, (float)run.next / (float)kPieces, "Stopping...");
}

void TickRun() {
    if (run.composing) {
        if (run.picture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return;
        Picture pic = run.picture.get();
        int drawn = pic.drawn;
        bool here = st.world == run.world;
        const char* note = "Map photos ready";
        if (run.stopAsked || !here) note = "Map scan stopped";
        else if (drawn <= 0) note = "The map photos came out empty";
        if (drawn > 0 && here) Publish(pic);
        LOG("map photos: done, %d of %d pieces have a picture", drawn, kPieces);
        End(note);
        return;
    }
    if (st.world != run.world) {
        End("Map scan stopped");
        return;
    }
    if (run.stopAsked || run.next >= run.order.size()) {
        BeginCompose();
        return;
    }
    if (st.time < run.nextAt) return;
    run.nextAt = st.time + kGap;
    int t = run.order[run.next];
    Shot shot = Take(t);
    if (shot != Shot::Ok) {
        if (shot == Shot::Lost) LOG("map photos: the camera stopped working");
        End(shot == Shot::Lost ? "The map camera stopped working" : "Map photos could not be saved");
        return;
    }
    std::wstring file = ShotFile(t);
    if (run.locked) {
        run.pending[t] = std::async(std::launch::async, [file] { return Develop(file); });
    } else {
        Piece p = Develop(file);
        if (!p.read) {
            LOG("map photos: the saved picture could not be read");
            End("Map photos could not be read");
            return;
        }
        if (!p.blank) {
            run.locked = true;
            goodSetting = run.setting;
            LOG("map photos: camera setting %d works", run.setting + 1);
        } else if (++run.blanks >= 2) {
            if (!NextSetting()) {
                LOG("map photos: every camera setting came out empty");
                End("Map photos don't work here");
                return;
            }
            LOG("map photos: pictures came out empty, trying camera setting %d", run.setting + 1);
            run.next = 0;
            run.blanks = 0;
            run.done.assign(kPieces, Piece{});
            Show(true, 0.0f, "Trying another camera setting...");
            return;
        }
        run.done[t] = std::move(p);
    }
    ++run.next;
    Show(true, (float)run.next / (float)kPieces, "Photo " + std::to_string(run.next) + " of " + std::to_string(kPieces));
}

}

void MapshotInit(core::Model& m) {
    (void)m;
    bool ok = Has("SceneCapture2D", "CaptureComponent2D") && Has("SceneCaptureComponent2D", "TextureTarget") &&
              Has("SceneCaptureComponent2D", "OrthoWidth") && HasFunc("SceneCaptureComponent2D", "CaptureScene") &&
              HasFunc("KismetRenderingLibrary", "CreateRenderTarget2D") && HasFunc("KismetRenderingLibrary", "ExportRenderTarget") &&
              HasFunc("GameplayStatics", "BeginDeferredActorSpawnFromClass") && !Folder().empty();
    if (!ok) {
        LOG("map photos are not available in this version of the game");
        return;
    }
    WithModel([](core::Model& mm) { mm.mapScan = [](bool start) { ask = start ? 1 : 2; }; });
}

void MapshotTick() {
    CheckPhoto();
    int a = ask.exchange(0);
    if (a == 1) Start();
    else if (a == 2) Stop();
    if (run.active) TickRun();
}

void MapshotOff() {
    ask = 0;
    CheckPhoto();
    if (!run.active) return;
    LOG("map photos: scan cancelled");
    End("Map scan stopped");
}

}
