#include "ingame/gd.h"
#include "ingame/gdx.h"
#include "ingame/log.h"
#include "ingame/modules.h"
#include "ui/ui.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace macro {

namespace {

using QueueFn = void(__fastcall*)(void*, float, bool);
using ButtonFn = void(__fastcall*)(void*, bool, int, bool);
using PauseFn = void(__fastcall*)(void*, bool);
using ReleaseFn = void(__fastcall*)(void*);
using EndFn = void(__fastcall*)(void*, const void*);
using PlatformerEndFn = void(__fastcall*)(void*, const void*, bool);
using VariableFn = bool(__fastcall*)(void*, const char*);
using GrowFn = void*(__fastcall*)(void*, void*, const void*);

constexpr uint32_t kProcessQueued = 0x33A9A0;
constexpr uint32_t kHandleButton = 0x33C5B0;
constexpr uint32_t kPauseGame = 0x51A370;
constexpr uint32_t kReleaseAll = 0x4FD450;
constexpr uint32_t kPlayEnd = 0x50A2A0;
constexpr uint32_t kPlayPlatformerEnd = 0x50ACE0;
constexpr uint32_t kGameVariable = 0x27E5C0;
constexpr uint32_t kQueueGrow = 0x807C0;
constexpr uint32_t kPauseReleases[] = {0x51A51D, 0x51A52E, 0x51A573, 0x51A584, 0x51AC8A, 0x51AC9B};

constexpr uintptr_t kTimeWarp = 0x330;
constexpr uintptr_t kCommandIndex = 0x3E0;
constexpr uintptr_t kStepIndex = 0x3EC;
constexpr uintptr_t kAttempts = 0x3164;
constexpr uintptr_t kStartPos = 0x3270;
constexpr uintptr_t kUseReplay = 0x3280;
constexpr uintptr_t kPlayerDied = 0x334A;
constexpr uintptr_t kQueue = 0x3400;
constexpr uintptr_t kEndStarted = 0x3670;
constexpr uintptr_t kTimestamp = 0x38E8;
constexpr uintptr_t kBetweenSteps = 0x38F0;
constexpr uintptr_t kClickBetweenSteps = 0x38F1;
constexpr uintptr_t kLevelIdA = 0x148;
constexpr uintptr_t kLevelIdB = 0x14C;
constexpr uintptr_t kLevelName = 0x158;

constexpr int kSynthetic = 0x4D414352;
constexpr int kVersion = 2;
constexpr int kMarkerBase = 10;
constexpr uintptr_t kLevelCbsOverride = 0x634;
constexpr double kTick = 1.0 / 240.0;
constexpr size_t kMaxEvents = 2000000;
constexpr const char* kFormat = "fathom-gd-macro";

struct Cmd {
    int button;
    uint8_t push;
    uint8_t p2;
    uint8_t pad[2];
    int step;
    int target;
    double time;
};
static_assert(sizeof(Cmd) == 24, "button command layout");

struct Queue {
    Cmd* first;
    Cmd* last;
    Cmd* end;
};

struct Ev {
    int step = 0;
    int button = 0;
    bool down = false;
    bool p2 = false;
};

struct Level {
    int id = 0;
    std::string name;
    bool platformer = false;
};

struct Macro {
    Level level;
    std::vector<Ev> events;
    bool halfSteps = false;
};

enum class Why { None, Cheat, Practice, StartPos, Replay, Died, Complete, Full };

struct Attempt {
    void* layer = nullptr;
    int counter = 0;
    int last = 0;
    bool fresh = false;
    bool rec = false;
    bool bot = false;
    bool played = false;
    bool won = false;
    bool saved = false;
    int endStep = 0;
    Why why = Why::None;
    Level level;
    std::vector<Ev> events;
    std::shared_ptr<const Macro> macro;
    size_t cursor = 0;
    bool held[2][4] = {};
};

struct SaveResult {
    bool ok = false;
    std::string file;
    std::string error;
};

QueueFn oQueue = nullptr;
PauseFn oPause = nullptr;
ReleaseFn oRelease = nullptr;
EndFn oEnd = nullptr;
PlatformerEndFn oPlatformerEnd = nullptr;
ButtonFn handleButton = nullptr;
VariableFn gameVariable = nullptr;
GrowFn queueGrow = nullptr;

std::atomic<bool> hooked{false};
std::atomic<bool> recReady{false};
std::atomic<bool> playReady{false};
bool stopped = false;
uintptr_t exeLo = 0;
uintptr_t exeHi = 0;
fs::path folder;

Attempt att;
std::vector<Cmd> scratch;
std::shared_ptr<const Macro> loaded;
std::string loadedKey;
bool loadedBad = false;
void* notedLayer = nullptr;
bool playWas = false;
void* cbsLayer = nullptr;
uint8_t cbsOld = 0;
struct SaveJob {
    HANDLE handle = nullptr;
    fs::path dir;
    Level level;
    std::string text;
    SaveResult result;
};
SaveJob* saving = nullptr;
int saveState = 0;

Queue& Q(void* gl) { return gd::F<Queue>(gl, kQueue); }

size_t Size(void* gl) {
    const Queue& q = Q(gl);
    return q.first && q.last > q.first ? static_cast<size_t>(q.last - q.first) : 0;
}

template <class Drop>
void Prune(void* gl, Drop drop) {
    Queue& q = Q(gl);
    if (!q.first || q.last <= q.first) return;
    Cmd* out = q.first;
    for (Cmd* c = q.first; c < q.last; ++c) {
        if (drop(*c)) continue;
        if (out != c) *out = *c;
        ++out;
    }
    q.last = out;
}

void DropSynthetic(void* gl) {
    Prune(gl, [](const Cmd& c) { return c.step == kSynthetic; });
}

void DropAll(void* gl) {
    Queue& q = Q(gl);
    if (q.first && q.last > q.first) q.last = q.first;
}

void Push(void* gl, const Cmd& c) {
    Queue& q = Q(gl);
    if (q.last && q.last < q.end) {
        *q.last = c;
        ++q.last;
        return;
    }
    if (queueGrow) queueGrow(&q, q.last, &c);
}

void Snapshot(void* gl, size_t n) {
    const Queue& q = Q(gl);
    scratch.assign(q.first, q.first + n);
}

std::string ReadString(const void* obj, uintptr_t off) {
    size_t size = gd::F<size_t>(obj, off + 0x10);
    size_t cap = gd::F<size_t>(obj, off + 0x18);
    if (size > 512 || size > cap) return {};
    const char* text = cap > 15 ? gd::F<const char*>(obj, off) : reinterpret_cast<const char*>(reinterpret_cast<uintptr_t>(obj) + off);
    return text ? std::string(text, size) : std::string();
}

Level ReadLevel(void* pl) {
    Level l;
    l.platformer = gd::F<uint8_t>(pl, gdx::off::GBGL_isPlatformer) != 0;
    void* lvl = gdx::Level(pl);
    if (!lvl) return l;
    l.id = std::max(0, gdx::Seeded(lvl, kLevelIdA, kLevelIdB));
    l.name = ReadString(lvl, kLevelName);
    return l;
}

std::string Key(const Level& l) { return l.id > 0 ? "id " + std::to_string(l.id) : "local " + l.name; }

std::wstring Trim(const std::wstring& s) {
    size_t a = 0, b = s.size();
    while (a < b && s[a] == L' ') ++a;
    while (b > a && (s[b - 1] == L' ' || s[b - 1] == L'.')) --b;
    return s.substr(a, b - a);
}

std::wstring SafeName(const std::string& name) {
    std::wstring out;
    for (wchar_t ch : gd::Widen(name))
        if (ch >= 32 && !std::wcschr(L"<>:\"/\\|?*", ch)) out.push_back(ch);
    out = Trim(out);
    if (out.size() > 60) out = Trim(out.substr(0, 60));
    return out.empty() ? std::wstring(L"Level") : out;
}

std::wstring FileName(const Level& l) {
    std::wstring tag = l.id > 0 ? L" (" + std::to_wstring(l.id) + L")" : std::wstring(L" (local)");
    return SafeName(l.name) + tag + L".gdm";
}

std::vector<fs::path> Existing(const fs::path& dir, const Level& l) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::path exact = dir / FileName(l);
    if (fs::is_regular_file(exact, ec)) out.push_back(exact);
    if (l.id <= 0) return out;
    std::wstring tail = L" (" + std::to_wstring(l.id) + L").gdm";
    for (fs::directory_iterator it(dir, ec), done; !ec && it != done; it.increment(ec)) {
        std::error_code fe;
        if (!it->is_regular_file(fe)) continue;
        std::wstring n = it->path().filename().wstring();
        if (n.size() > tail.size() && n.compare(n.size() - tail.size(), tail.size(), tail) == 0 && it->path() != exact)
            out.push_back(it->path());
    }
    return out;
}

int Num(const nlohmann::json& v) {
    if (v.is_boolean()) return v.get<bool>() ? 1 : 0;
    if (v.is_number_integer()) return v.get<int>();
    return -1;
}

std::shared_ptr<const Macro> Parse(const std::string& text, const Level& want) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return nullptr;
    try {
        int version = j.value("version", 0);
        if (j.value("format", std::string()) != kFormat || version < 1 || version > kVersion) return nullptr;
        const nlohmann::json& lv = j.at("level");
        const nlohmann::json& list = j.at("events");
        if (!lv.is_object() || !list.is_array()) return nullptr;
        auto m = std::make_shared<Macro>();
        m->level.id = lv.value("id", 0);
        m->level.name = lv.value("name", std::string());
        m->level.platformer = lv.value("platformer", false);
        if (m->level.id != want.id || m->level.platformer != want.platformer) return nullptr;
        if (want.id <= 0 && m->level.name != want.name) return nullptr;
        if (list.size() > kMaxEvents) return nullptr;
        m->events.reserve(list.size());
        for (const nlohmann::json& e : list) {
            if (!e.is_array() || e.size() < 4) return nullptr;
            Ev ev;
            ev.step = Num(e[0]);
            ev.button = Num(e[1]);
            int down = Num(e[2]);
            int p2 = Num(e[3]);
            bool marker = version >= 2 && ev.button > kMarkerBase && ev.button <= kMarkerBase + 3;
            if (ev.step < 0 || ev.button < 0 || (ev.button > 3 && !marker) || down < 0 || p2 < 0) return nullptr;
            ev.down = down != 0;
            ev.p2 = p2 != 0;
            if (ev.step & 1) m->halfSteps = true;
            m->events.push_back(ev);
        }
        std::stable_sort(m->events.begin(), m->events.end(), [](const Ev& a, const Ev& b) { return a.step < b.step; });
        return m;
    } catch (...) {
        return nullptr;
    }
}

std::shared_ptr<const Macro> Load(const Level& l, bool& bad) {
    bad = false;
    std::vector<fs::path> files;
    try {
        files = Existing(folder, l);
    } catch (...) {
        return nullptr;
    }
    for (const fs::path& p : files) {
        std::string name = gd::Narrow(p.filename().wstring());
        std::shared_ptr<const Macro> m;
        try {
            std::error_code ec;
            auto size = fs::file_size(p, ec);
            if (!ec && size <= 64ull * 1024 * 1024) {
                std::ifstream in(p, std::ios::binary);
                if (in) {
                    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                    m = Parse(text, l);
                }
            }
        } catch (...) {
            m = nullptr;
        }
        if (m) {
            LOG("macro loaded: %s (%d inputs)", name.c_str(), static_cast<int>(m->events.size()));
            return m;
        }
        bad = true;
        LOG("macro file could not be used: %s", name.c_str());
    }
    return nullptr;
}

std::string Quote(const std::string& s) { return nlohmann::json(s).dump(-1, ' ', false, nlohmann::json::error_handler_t::replace); }

std::string Stamp() {
    SYSTEMTIME t;
    GetLocalTime(&t);
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return buf;
}

std::string Serialize(const Macro& m, int steps) {
    std::ostringstream o;
    o << "{\n";
    o << "  \"format\": \"" << kFormat << "\",\n";
    o << "  \"version\": " << kVersion << ",\n";
    o << "  \"game\": \"2.2091\",\n";
    o << "  \"recorded\": " << Quote(Stamp()) << ",\n";
    o << "  \"level\": {\"id\": " << m.level.id << ", \"name\": " << Quote(m.level.name)
      << ", \"platformer\": " << (m.level.platformer ? "true" : "false") << "},\n";
    o << "  \"steps\": " << steps << ",\n";
    o << "  \"stepsPerSecond\": 480,\n";
    o << "  \"buttons\": [\"release all\", \"jump\", \"left\", \"right\"],\n";
    o << "  \"splitMarkers\": \"buttons 11 to 13 only make the game split that step, like jump, left and right would\",\n";
    o << "  \"eventFields\": [\"step\", \"button\", \"down\", \"player2\"],\n";
    o << "  \"events\": [";
    for (size_t i = 0; i < m.events.size(); ++i) {
        const Ev& e = m.events[i];
        o << (i ? ",\n    [" : "\n    [") << e.step << ", " << e.button << ", " << (e.down ? 1 : 0) << ", " << (e.p2 ? 1 : 0) << "]";
    }
    o << (m.events.empty() ? "]\n" : "\n  ]\n") << "}\n";
    return o.str();
}

SaveResult Store(const fs::path& dir, const Level& level, const std::string& text) {
    SaveResult r;
    try {
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::wstring name = FileName(level);
        fs::path target = dir / name;
        r.file = gd::Narrow(name);
        std::vector<fs::path> old = Existing(dir, level);
        fs::path tmp = target;
        tmp += L".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) {
                r.error = "the file could not be created";
                return r;
            }
            out.write(text.data(), static_cast<std::streamsize>(text.size()));
            out.flush();
            if (!out.good()) {
                r.error = "the file could not be written";
                out.close();
                fs::remove(tmp, ec);
                return r;
            }
        }
        for (const fs::path& p : old) {
            fs::path bak = p;
            bak += L".bak";
            bool kept = p == target ? CopyFileW(p.c_str(), bak.c_str(), FALSE) != 0
                                    : MoveFileExW(p.c_str(), bak.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
            if (!kept) {
                LOG("macro: %s could not be kept as a backup", gd::Narrow(p.filename().wstring()).c_str());
                r.error = "the old macro could not be kept as a backup";
                fs::remove(tmp, ec);
                return r;
            }
        }
        if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
            r.error = "the file could not be replaced";
            fs::remove(tmp, ec);
            return r;
        }
        r.ok = true;
    } catch (...) {
        r.error = "unexpected error";
    }
    return r;
}

void Report(const SaveResult& r) {
    if (r.ok) {
        LOG("macro saved: %s", r.file.c_str());
        gd::Note("Macro saved: " + r.file);
    } else {
        LOG("macro could not be saved (%s): %s", r.file.c_str(), r.error.c_str());
        gd::Note("The macro could not be saved");
    }
}

DWORD WINAPI SaveThread(void* arg) {
    SaveJob* job = static_cast<SaveJob*>(arg);
    job->result = Store(job->dir, job->level, job->text);
    return 0;
}

void PollSave(bool wait) {
    if (!saving) return;
    if (saving->handle && WaitForSingleObject(saving->handle, wait ? INFINITE : 0) != WAIT_OBJECT_0) return;
    if (saving->handle) CloseHandle(saving->handle);
    SaveJob* job = saving;
    saving = nullptr;
    saveState = job->result.ok ? 2 : 3;
    Report(job->result);
    delete job;
}

bool Flip(void* gl) {
    if (!gameVariable || gd::F<uint8_t>(gl, gdx::off::GBGL_isPlatformer) || gd::F<uint8_t>(gl, kUseReplay)) return false;
    void* gm = gdx::GameManager();
    return gm && gameVariable(gm, "0010");
}

float TimeWarp(void* gl) {
    float tw = gd::F<float>(gl, kTimeWarp);
    return std::isfinite(tw) && tw > 0.0f ? tw : 1.0f;
}

double StepLength(float tw) { return static_cast<double>(std::min(1.0f, tw)) * kTick; }

double TickLength(float tw) { return StepLength(tw) / static_cast<double>(tw); }

Why Blocker(void* pl) {
    if (gd::F<uint8_t>(pl, gdx::off::GBGL_isPracticeMode)) return Why::Practice;
    if (gd::F<uint8_t>(pl, gdx::off::GBGL_isTestMode) || gd::F<void*>(pl, kStartPos)) return Why::StartPos;
    if (gd::F<uint8_t>(pl, kUseReplay)) return Why::Replay;
    return Why::None;
}

void Mark(Why w) {
    if (att.why == Why::None) att.why = w;
}

bool Recording() { return att.rec && !att.won && att.why == Why::None && gd::C().record.load(); }

uint8_t CbsSetting(void* pl) {
    void* lvl = gdx::Level(pl);
    int forced = lvl ? gd::F<int>(lvl, kLevelCbsOverride) : 0;
    if (forced == 1) return 1;
    if (forced == 2) return 0;
    void* gm = gdx::GameManager();
    return gm && gameVariable && gameVariable(gm, "0177") ? 1 : 0;
}

void RestoreCbs() {
    if (cbsLayer && cbsLayer == gdx::PlayLayer()) gd::F<uint8_t>(cbsLayer, kClickBetweenSteps) = gameVariable ? CbsSetting(cbsLayer) : cbsOld;
    cbsLayer = nullptr;
}

void ForceCbs(void* pl) {
    uint8_t& flag = gd::F<uint8_t>(pl, kClickBetweenSteps);
    if (cbsLayer != pl) {
        RestoreCbs();
        cbsLayer = pl;
        cbsOld = flag;
    } else if (flag != 1) {
        cbsOld = flag;
    }
    flag = 1;
}

void EndBot() {
    if (att.bot && att.layer && att.layer == gdx::PlayLayer()) DropSynthetic(att.layer);
    RestoreCbs();
    att.bot = false;
    att.macro.reset();
}

void ReleaseHeld(void* gl) {
    if (!handleButton) return;
    bool flip = Flip(gl);
    for (int who = 0; who < 2; ++who)
        for (int b = 1; b < 4; ++b)
            if (att.held[who][b]) {
                att.held[who][b] = false;
                handleButton(gl, false, b, (who == 0) != flip);
            }
}

void StopBot(void* gl) {
    if (!att.bot) return;
    ReleaseHeld(gl);
    EndBot();
    LOG("macro bot stopped, the player has control");
}

void Discard() {
    att.rec = false;
    att.events.clear();
}

void Save() {
    att.saved = true;
    if (!att.rec || att.why != Why::None || !gd::C().record.load()) return;
    auto m = std::make_shared<Macro>();
    m->level = att.level;
    m->events = att.events;
    for (const Ev& e : m->events)
        if (e.step & 1) m->halfSteps = true;
    std::string text = Serialize(*m, att.endStep);
    loaded = m;
    loadedKey = Key(m->level);
    loadedBad = false;
    PollSave(true);
    saveState = 1;
    SaveJob* job = new SaveJob();
    job->dir = folder;
    job->level = m->level;
    job->text = std::move(text);
    job->handle = CreateThread(nullptr, 0, SaveThread, job, 0, nullptr);
    if (!job->handle) job->result = Store(job->dir, job->level, job->text);
    saving = job;
    LOG("level beaten while recording, saving the macro (%d inputs)", static_cast<int>(m->events.size()));
}

void Begin(void* pl, int step) {
    if (att.rec && att.won && !att.saved) Save();
    EndBot();
    att.layer = pl;
    att.counter = gd::F<int>(pl, kAttempts);
    att.last = step;
    att.won = false;
    att.saved = false;
    att.endStep = 0;
    att.why = Why::None;
    att.events.clear();
    att.cursor = 0;
    std::memset(att.held, 0, sizeof(att.held));
    att.level = ReadLevel(pl);
    att.fresh = step == 0 && Blocker(pl) == Why::None;
    gd::Cfg& c = gd::C();
    bool play = c.play.load();
    att.rec = att.fresh && recReady.load() && c.record.load() && !play;
    att.bot = att.fresh && playReady.load() && play && loaded && loadedKey == Key(att.level);
    att.played = false;
    if (att.bot) att.macro = loaded;
}

void Sync(void* pl, int step) {
    if (pl != att.layer || gd::F<int>(pl, kAttempts) != att.counter || step < att.last) Begin(pl, step);
    else if (step > att.last) att.last = step;
}

void Check(void* pl) {
    if (!att.rec || att.won) return;
    gd::Cfg& c = gd::C();
    if (c.noclip.load() || c.jumps.load()) Mark(Why::Cheat);
    Why b = Blocker(pl);
    if (b != Why::None) Mark(b);
    if (gd::F<uint8_t>(pl, kPlayerDied)) Mark(Why::Died);
}

bool MakeEv(int step, const Cmd& c, bool flip, Ev& e) {
    if (c.button <= 0 || c.button > 255) return false;
    e.step = step;
    e.button = c.button;
    e.down = c.push != 0;
    e.p2 = (c.p2 != 0) != flip;
    return true;
}

bool Room() {
    if (att.events.size() < kMaxEvents) return true;
    Mark(Why::Full);
    return false;
}

bool queueCalled = false;
bool pauseCalled = false;

void CallQueue(void* gl, float dt, bool clear) {
    queueCalled = true;
    oQueue(gl, dt, clear);
}

void CallPause(void* pl, bool unfocused) {
    pauseCalled = true;
    oPause(pl, unfocused);
}

void RecordStep(void* gl, float dt, bool clear, int step) {
    if (dt <= 0.0f) return;
    size_t before = Size(gl);
    if (!before) {
        CallQueue(gl, dt, clear);
        return;
    }
    Snapshot(gl, before);
    CallQueue(gl, dt, clear);
    size_t after = Size(gl);
    size_t used = after <= before ? before - after : 0;
    if (!used) {
        if ((step & 1) == 0 || !Room()) return;
        for (const Cmd& c : scratch) {
            if (c.button != 1 || c.step == kSynthetic) continue;
            Ev e;
            if (!MakeEv(step, c, Flip(gl), e)) break;
            e.button += kMarkerBase;
            att.events.push_back(e);
            break;
        }
        return;
    }
    bool flip = Flip(gl);
    for (size_t i = 0; i < used && Room(); ++i) {
        Ev e;
        if (MakeEv(step, scratch[i], flip, e)) att.events.push_back(e);
    }
}

void Apply(void* gl, const Ev& e, bool flip) {
    if (e.button > kMarkerBase) return;
    int who = e.p2 ? 1 : 0;
    if (e.button <= 0) {
        void* p = gd::F<void*>(gl, who ? gdx::off::GBGL_player2 : gdx::off::GBGL_player1);
        if (p && oRelease) oRelease(p);
        for (bool& h : att.held[who]) h = false;
        return;
    }
    if (!handleButton) return;
    handleButton(gl, e.down, e.button, (who == 0) != flip);
    if (e.button < 4) att.held[who][e.button] = e.down;
}

void Schedule(void* gl, int next, double start) {
    if (!att.macro) return;
    const std::vector<Ev>& ev = att.macro->events;
    size_t j = att.cursor;
    while (j < ev.size() && ev[j].step <= next) ++j;
    if (j >= ev.size() || ev[j].step != next + 1) return;
    float tw = TimeWarp(gl);
    double span = TickLength(tw);
    double when = start + span * 0.5 + span * 0.25 * std::min(1.0, static_cast<double>(tw));
    bool flip = Flip(gl);
    for (; j < ev.size() && ev[j].step == next + 1; ++j) {
        const Ev& e = ev[j];
        if (e.button <= 0) continue;
        Cmd c{};
        c.button = e.button > kMarkerBase ? e.button - kMarkerBase : e.button;
        c.push = static_cast<uint8_t>(e.down ? 1 : 0);
        c.p2 = static_cast<uint8_t>((!e.p2) != flip ? 0 : 1);
        c.step = kSynthetic;
        c.target = next + 1;
        c.time = when;
        Push(gl, c);
    }
}

void BotStep(void* gl, float dt, bool clear, int step) {
    DropAll(gl);
    CallQueue(gl, dt, clear);
    if (dt <= 0.0f || !att.macro) return;
    if (!att.played) {
        att.played = true;
        safe::MarkCheated("Play macro");
        LOG("macro bot is playing this attempt (%d inputs)", static_cast<int>(att.macro->events.size()));
    }
    const std::vector<Ev>& ev = att.macro->events;
    bool between = gd::F<uint8_t>(gl, kBetweenSteps) != 0;
    bool firstHalf = between && (step & 1) == 0;
    int limit = between ? step : step + 1;
    if (att.cursor < ev.size() && ev[att.cursor].step <= limit) {
        bool flip = Flip(gl);
        while (att.cursor < ev.size() && ev[att.cursor].step <= limit) Apply(gl, ev[att.cursor++], flip);
    }
    if (firstHalf) return;
    double span = StepLength(TimeWarp(gl)) * (between ? 0.5 : 1.0);
    Schedule(gl, between ? step + 1 : step + 2, gd::F<double>(gl, kTimestamp) + span);
}

void QueueBody(void* gl, float dt, bool clear) {
    if (stopped || !gl || gl != gdx::PlayLayer()) {
        CallQueue(gl, dt, clear);
        return;
    }
    int step = gd::F<int>(gl, kStepIndex);
    Sync(gl, step);
    gd::Cfg& c = gd::C();
    if (att.bot && !c.play.load()) StopBot(gl);
    if (att.rec && !c.record.load()) Discard();
    if (att.bot) {
        BotStep(gl, dt, clear, step);
        return;
    }
    Check(gl);
    if (Recording()) {
        RecordStep(gl, dt, clear, step);
        return;
    }
    CallQueue(gl, dt, clear);
}

void PauseBody(void* pl, bool unfocused) {
    if (stopped || !pl || pl != gdx::PlayLayer()) {
        CallPause(pl, unfocused);
        return;
    }
    int step = gd::F<int>(pl, kCommandIndex);
    Sync(pl, step);
    DropSynthetic(pl);
    if (att.bot && gd::C().play.load()) {
        DropAll(pl);
        CallPause(pl, unfocused);
        return;
    }
    if (!Recording()) {
        CallPause(pl, unfocused);
        return;
    }
    size_t before = Size(pl);
    std::vector<Cmd> queued;
    if (before) queued.assign(Q(pl).first, Q(pl).first + before);
    size_t mark = att.events.size();
    CallPause(pl, unfocused);
    size_t after = Size(pl);
    size_t used = after <= before ? before - after : 0;
    if (!used || !att.rec || mark > att.events.size()) return;
    bool flip = Flip(pl);
    std::vector<Ev> flushed;
    for (size_t i = 0; i < used; ++i) {
        Ev e;
        if (MakeEv(step, queued[i], flip, e)) flushed.push_back(e);
    }
    if (att.events.size() + flushed.size() > kMaxEvents) {
        Mark(Why::Full);
        return;
    }
    att.events.insert(att.events.begin() + static_cast<std::ptrdiff_t>(mark), flushed.begin(), flushed.end());
}

void __fastcall hkQueue(void* gl, float dt, bool clear) {
    gd::Busy busy;
    queueCalled = false;
    try {
        QueueBody(gl, dt, clear);
    } catch (...) {
        LOG("macro: error during a game step");
        if (!queueCalled) oQueue(gl, dt, clear);
    }
}

void __fastcall hkPause(void* pl, bool unfocused) {
    gd::Busy busy;
    pauseCalled = false;
    try {
        PauseBody(pl, unfocused);
    } catch (...) {
        LOG("macro: error while pausing");
        if (!pauseCalled) oPause(pl, unfocused);
    }
}

bool PauseRelease(uintptr_t from) {
    for (uint32_t r : kPauseReleases)
        if (from == exeLo + r) return true;
    return false;
}

int Who(void* pl, void* player) {
    if (player == gd::F<void*>(pl, gdx::off::GBGL_player1)) return 0;
    if (player == gd::F<void*>(pl, gdx::off::GBGL_player2)) return 1;
    return -1;
}

void __fastcall hkRelease(void* player) {
    gd::Busy busy;
    uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    void* pl = nullptr;
    int who = -1;
    if (!stopped && player && (att.bot || att.rec)) {
        pl = gdx::PlayLayer();
        if (pl && pl == att.layer) who = Who(pl, player);
    }
    if (who >= 0 && PauseRelease(from)) {
        int step = gd::F<int>(pl, kCommandIndex);
        try {
            Sync(pl, step);
        } catch (...) {
            LOG("macro: error while tracking the attempt");
        }
        if (att.bot && gd::C().play.load()) return;
        if (Recording() && Room()) {
            Ev e;
            e.step = step;
            e.p2 = who == 1;
            try {
                att.events.push_back(e);
            } catch (...) {
                Mark(Why::Full);
            }
        }
    }
    oRelease(player);
    if (who >= 0 && att.bot)
        for (bool& h : att.held[who]) h = false;
}

void Ended(void* pl, uintptr_t from, bool starting) {
    if (stopped || !pl || pl != gdx::PlayLayer()) return;
    Sync(pl, gd::F<int>(pl, kCommandIndex));
    if (from < exeLo || from >= exeHi) {
        Mark(Why::Complete);
        return;
    }
    if (starting && !att.won) {
        att.won = true;
        att.endStep = gd::F<int>(pl, kCommandIndex);
    }
}

void __fastcall hkEnd(void* pl, const void* pos) {
    gd::Busy busy;
    uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    try {
        Ended(pl, from, pl && gd::F<uint8_t>(pl, kEndStarted) == 0);
    } catch (...) {
        LOG("macro: error at the level end");
    }
    oEnd(pl, pos);
}

void __fastcall hkPlatformerEnd(void* pl, const void* pos, bool instant) {
    gd::Busy busy;
    uintptr_t from = reinterpret_cast<uintptr_t>(_ReturnAddress());
    try {
        Ended(pl, from, true);
    } catch (...) {
        LOG("macro: error at the level end");
    }
    oPlatformerEnd(pl, pos, instant);
}

void Prepare(void* pl) {
    Level l = att.layer == pl ? att.level : ReadLevel(pl);
    std::string key = Key(l);
    if (key != loadedKey) {
        loadedKey = key;
        loaded = Load(l, loadedBad);
        notedLayer = nullptr;
    }
    if (!loaded && notedLayer != pl) {
        notedLayer = pl;
        gd::Note(loadedBad ? "The macro for this level could not be read" : "No macro for this level yet");
    }
}

void Leave() {
    if (!att.layer) return;
    if (att.rec && att.won && !att.saved) Save();
    cbsLayer = nullptr;
    att.layer = nullptr;
    att.counter = 0;
    att.last = 0;
    att.fresh = false;
    att.rec = false;
    att.bot = false;
    att.played = false;
    att.won = false;
    att.saved = false;
    att.why = Why::None;
    att.events.clear();
    att.macro.reset();
    att.cursor = 0;
    std::memset(att.held, 0, sizeof(att.held));
    notedLayer = nullptr;
    loadedKey.clear();
    loaded.reset();
}

bool CallsRelease(uint32_t ret) {
    const uint8_t* code = static_cast<const uint8_t*>(gd::Exe(ret - 5));
    if (!code || code[0] != 0xE8) return false;
    int32_t rel = 0;
    std::memcpy(&rel, code + 1, sizeof(rel));
    return static_cast<int64_t>(ret) + rel == static_cast<int64_t>(kReleaseAll);
}

template <class T>
T Fn(uint32_t rva) {
    return gd::IsFunctionStart(rva) ? reinterpret_cast<T>(gd::Exe(rva)) : nullptr;
}

const char* RecReason(Why w) {
    switch (w) {
    case Why::Cheat: return "REC: cheat used, this run won't save";
    case Why::Practice: return "REC: practice mode";
    case Why::StartPos: return "REC: start position";
    case Why::Replay: return "REC: replay";
    case Why::Complete: return "REC: Complete level used";
    case Why::Died: return "REC: died, this run won't save";
    case Why::Full: return "REC: run too long";
    default: return nullptr;
    }
}

const char* BotReason(Why w) {
    switch (w) {
    case Why::Practice: return "BOT: practice mode";
    case Why::StartPos: return "BOT: start position";
    case Why::Replay: return "BOT: replay";
    default: return "BOT: next attempt";
    }
}

void Label(const char* text, ImU32 dot, bool live, bool dim) {
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    ui::Theme& t = ui::T();
    const float size = 14.0f;
    const float r = 4.0f;
    ImVec2 ts = ui::Measure(t.semibold, size, text);
    ImVec2 a(14.0f, 14.0f);
    ImVec2 b(a.x + 10.0f + r * 2.0f + 7.0f + ts.x + 10.0f, a.y + ts.y + 10.0f);
    dl->AddRectFilled(a, b, IM_COL32(0, 0, 0, 150), 5.0f);
    ImVec4 col = ImGui::ColorConvertU32ToFloat4(dot);
    if (live) col.w *= 0.6f + 0.4f * std::sin(static_cast<float>(ImGui::GetTime()) * 4.0f);
    dl->AddCircleFilled(ImVec2(a.x + 10.0f + r, (a.y + b.y) * 0.5f), r, ImGui::ColorConvertFloat4ToU32(col));
    ImU32 ink = dim ? IM_COL32(225, 225, 225, 170) : IM_COL32(255, 255, 255, 240);
    ui::Text(dl, t.semibold, size, ImVec2(a.x + 10.0f + r * 2.0f + 7.0f, a.y + 5.0f), ink, text);
}

}

bool Install() {
    if (!gd::Supported()) return false;
    folder = fs::path(gd::Folder()) / L"macros";
    std::error_code ec;
    fs::create_directories(folder, ec);
    bool dirOk = fs::is_directory(folder, ec);
    exeLo = gd::ExeBase();
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(exeLo);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(exeLo + static_cast<uintptr_t>(dos->e_lfanew));
    exeHi = exeLo + nt->OptionalHeader.SizeOfImage;
    handleButton = Fn<ButtonFn>(kHandleButton);
    gameVariable = Fn<VariableFn>(kGameVariable);
    queueGrow = Fn<GrowFn>(kQueueGrow);
    bool calls = true;
    for (uint32_t r : kPauseReleases) calls = calls && CallsRelease(r);
    if (!calls) LOG("macros: the pause code is not where it was expected");
    if (!dirOk) LOG("macros: the macros folder could not be created");
    bool q = gd::HookExe(kProcessQueued, (void*)&hkQueue, (void**)&oQueue, "macro input steps");
    bool p = gd::HookExe(kPauseGame, (void*)&hkPause, (void**)&oPause, "macro pause");
    bool r = gd::HookExe(kReleaseAll, (void*)&hkRelease, (void**)&oRelease, "macro button release");
    bool e = gd::HookExe(kPlayEnd, (void*)&hkEnd, (void**)&oEnd, "macro level end");
    bool pe = gd::HookExe(kPlayPlatformerEnd, (void*)&hkPlatformerEnd, (void**)&oPlatformerEnd, "macro platformer end");
    bool base = q && p && r && calls && handleButton && gameVariable && queueGrow;
    hooked = q;
    playReady = base;
    recReady = base && e && pe && dirOk;
    LOG("macros: recording %s, playback %s", recReady.load() ? "ready" : "unavailable", playReady.load() ? "ready" : "unavailable");
    return recReady.load() || playReady.load();
}

bool RecordReady() { return recReady.load(); }

bool PlayReady() { return playReady.load(); }

void Tick() {
    if (!hooked.load() || stopped) return;
    PollSave(false);
    gd::Cfg& c = gd::C();
    bool play = c.play.load() && playReady.load();
    if (play && !playWas) {
        PollSave(true);
        loadedKey.clear();
        loaded.reset();
        notedLayer = nullptr;
    }
    playWas = play;
    void* pl = gdx::PlayLayer();
    if (!pl) {
        Leave();
        return;
    }
    if (play) Prepare(pl);
    Sync(pl, gd::F<int>(pl, kCommandIndex));
    DropSynthetic(pl);
    if (att.bot && !c.play.load()) StopBot(pl);
    if (att.rec && !c.record.load()) Discard();
    Check(pl);
    if (att.rec && att.won && !att.saved) Save();
    if (!att.bot) return;
    if (att.played) safe::MarkCheated("Play macro");
    DropAll(pl);
    if (att.macro && att.macro->halfSteps) ForceCbs(pl);
    int next = gd::F<int>(pl, kCommandIndex);
    if ((next & 1) == 0) Schedule(pl, next, gd::F<double>(pl, kTimestamp));
}

void Frame() {
    if (!hooked.load() || stopped) return;
    void* pl = gdx::PlayLayer();
    if (!pl || pl != att.layer) return;
    gd::Cfg& c = gd::C();
    const ImU32 red = IM_COL32(255, 72, 88, 255);
    const ImU32 green = IM_COL32(72, 222, 128, 255);
    const ImU32 gray = IM_COL32(160, 160, 160, 255);
    const ImU32 amber = IM_COL32(255, 196, 92, 255);
    if (c.play.load() && playReady.load()) {
        if (att.bot) {
            Label("BOT", green, false, false);
            return;
        }
        if (loaded && loadedKey == Key(att.level)) Label(BotReason(Blocker(pl)), gray, false, true);
        return;
    }
    if (!c.record.load() || !recReady.load()) return;
    if (att.rec) {
        if (att.saved && att.why == Why::None) {
            if (saveState == 3) Label("REC: could not save", amber, false, true);
            else if (saveState == 2) Label("REC saved", green, false, false);
            else Label("REC saving", gray, false, true);
            return;
        }
        if (const char* why = RecReason(att.why)) {
            Label(why, amber, false, true);
            return;
        }
        Label("REC", red, !att.won, false);
        return;
    }
    const char* why = RecReason(Blocker(pl));
    Label(why ? why : "REC: next attempt", gray, false, true);
}

void Off() {
    if (hooked.load() && !stopped) {
        try {
            if (att.rec && att.won && !att.saved) Save();
        } catch (...) {
            LOG("macro could not be saved while closing");
        }
        void* pl = gdx::PlayLayer();
        if (pl && pl == att.layer) {
            StopBot(pl);
            DropSynthetic(pl);
        }
        RestoreCbs();
    }
    stopped = true;
    gd::C().record = false;
    gd::C().play = false;
    PollSave(true);
    cbsLayer = nullptr;
    att.layer = nullptr;
    att.rec = false;
    att.bot = false;
    att.won = false;
    att.events.clear();
    att.events.shrink_to_fit();
    att.macro.reset();
    loaded.reset();
    loadedKey.clear();
    scratch.clear();
    scratch.shrink_to_fit();
}

}
