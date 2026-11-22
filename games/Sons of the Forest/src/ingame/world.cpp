#include "ingame/cheats.h"
#include "ingame/log.h"
#include "ingame/patch.h"
#include "ingame/unity.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace sotf {

namespace {

namespace fs = std::filesystem;

using il2::Class;
using il2::Method;
using il2::Object;
using un::Fld;

const float kFovDefault = 75.0f;
const float kInstant = 1000.0f;
const int kMaxSaves = 30;
const size_t kKeepBackups = 20;

struct Refs {
    Class *localPlayer = nullptr, *playerStats = nullptr, *cutscenes = nullptr;
    Fld lpAnimator;
    Method *lpGetAnimator = nullptr, *lpMidAction = nullptr, *lpBlockFull = nullptr, *lpGrounded = nullptr, *lpInWorld = nullptr,
           *lpStats = nullptr, *psDead = nullptr, *psDying = nullptr, *csPlaying = nullptr;

    Class* timeHolder = nullptr;
    Fld todInstance, todSpeed, todFinal;

    Class *postManager = nullptr, *volume = nullptr, *volumeProfile = nullptr, *floatParam = nullptr, *colorAdjust = nullptr;
    Fld ppInstance, ppVolume, vpComponents, fpValue, fpOverride, caExposure;
    Method* volProfile = nullptr;

    Class* fovManager = nullptr;
    Fld fmActive, fmTarget, fmDefault, fmGlobal;
    Method *fmRegister = nullptr, *fmUnregister = nullptr;

    Class* console = nullptr;
    Fld dcInstance, dcShowConsole, dcRoutine, dcInput, dcBlock;
    std::vector<Fld> dcPanels;
    Method *dcCheck = nullptr, *dcShow = nullptr, *stopAll = nullptr;

    Class *saveManager = nullptr, *saveType = nullptr, *setupManager = nullptr, *coopSave = nullptr, *gameState = nullptr;
    Fld smActiveType, gsInstance;
    Method *smSave = nullptr, *smSaveToSlot = nullptr, *smFolder = nullptr, *smIds = nullptr, *smLoading = nullptr, *gsmType = nullptr,
           *gsmGameId = nullptr, *csSlot = nullptr, *gsGetName = nullptr, *gsSetName = nullptr, *gsIsGenerated = nullptr,
           *gsUpdateName = nullptr, *gsNewName = nullptr, *csSavePlayer = nullptr;
    int clientType = 2;
} R;

Object* Live(Object* o) { return un::Alive(o) ? o : nullptr; }

bool TimeReady() { return R.todInstance && R.todInstance.isStatic && R.todSpeed && !R.todSpeed.isStatic; }

bool BrightReady() {
    return R.ppInstance && R.ppInstance.isStatic && R.ppVolume && R.volProfile && R.vpComponents && R.colorAdjust && R.caExposure &&
           R.fpValue && R.fpOverride && !R.fpValue.isStatic && !R.fpOverride.isStatic && R.fpValue.off > R.fpOverride.off;
}

bool FovReady() {
    return R.fovManager && R.fmActive && R.fmTarget && R.fmDefault && R.fmGlobal && R.fmGlobal.isStatic && R.fmRegister && R.fmUnregister &&
           ((R.lpAnimator && R.lpAnimator.isStatic) || R.lpGetAnimator) && un::E.compGetComponentInChildren;
}

bool ConsoleReady() {
    return R.dcInstance && R.dcInstance.isStatic && R.dcShowConsole && R.dcRoutine && R.dcInput && R.dcBlock && !R.dcBlock.isStatic &&
           R.dcCheck && R.dcShow && R.stopAll && un::E.getEnabled && un::E.setEnabled && un::E.compGameObject && un::E.goActiveInHierarchy;
}

bool SaveReady() {
    bool save = R.smSave || (R.smSaveToSlot && R.smActiveType && R.smActiveType.isStatic);
    return save && R.smFolder && R.smIds && R.gsmType && R.gsmGameId && R.csSlot;
}

bool GuestSaveReady() { return R.csSavePlayer && R.smFolder && R.smIds && R.gsmType && R.gsmGameId && R.csSlot; }

bool Guest(const Ctx& c) { return c.session == core::Session::Client; }

patch::Forced<float> frozen;
double frozenSince = 0;
bool frozenNoted = false;

void ReleaseTime() {
    patch::Release(frozen, R.todSpeed);
    frozenNoted = false;
}

void StopTime(const Ctx& c) {
    if (!TimeReady()) return;
    bool want = c.inWorld && !Guest(c) && c.s.On("stop_time");
    Object* tod = want ? Live(un::Static(R.todInstance)) : nullptr;
    bool had = frozen.have;
    Object* before = frozen.obj.get();
    patch::Force<float>(tod, R.todSpeed, tod != nullptr, 0.0f, frozen);
    if (!frozen.have) {
        frozenNoted = false;
        return;
    }
    if (!had || before != tod) frozenSince = c.now;
    if (frozenNoted || !R.todFinal || c.now - frozenSince < 5.0) return;
    float speed = un::Val<float>(tod, R.todFinal);
    if (std::fabs(speed) > 1e-6f) {
        frozenNoted = true;
        Note("Time could not be stopped in this world");
        LOG("stop time: the clock still runs (speed %.4f)", speed);
    }
}

struct Bright {
    patch::Hold mgr, param;
    float base = 0, written = 0;
    bool have = false, noted = false;
    int misses = 0;
    double nextCheck = 0;
} bright;

Object* FindExposure(Object* pm) {
    Object* vol = Live(un::Ref(pm, R.ppVolume));
    if (!vol) return nullptr;
    Object* prof = Live(un::Call(R.volProfile, vol));
    if (!prof) return nullptr;
    for (Object* comp : il2::ListItems(un::Ref(prof, R.vpComponents))) {
        if (!comp || il2::ClassOf(comp) != R.colorAdjust) continue;
        Object* p = un::Ref(comp, R.caExposure);
        return p && un::Val<bool>(p, R.fpOverride) ? p : nullptr;
    }
    return nullptr;
}

void ReleaseBright() {
    Object* p = bright.param.get();
    if (bright.have && p && std::fabs(un::Val<float>(p, R.fpValue) - bright.written) < 1e-4f) un::SetVal<float>(p, R.fpValue, bright.base);
    bright.param.reset();
    bright.mgr.reset();
    bright.have = false;
    bright.nextCheck = 0;
}

void Brightness(const Ctx& c) {
    if (!BrightReady()) return;
    float mult = std::clamp(c.s.Value("brightness", 1.0f), 0.5f, 3.0f);
    bool want = c.inWorld && std::fabs(mult - 1.0f) > 1e-3f;
    Object* pm = want ? Live(un::Static(R.ppInstance)) : nullptr;
    if (!pm) {
        if (bright.mgr.held() || bright.param.held()) ReleaseBright();
        bright.misses = 0;
        return;
    }
    if (bright.mgr.held() && bright.mgr.get() != pm) ReleaseBright();
    Object* p = bright.param.get();
    if (c.now >= bright.nextCheck) {
        bright.nextCheck = c.now + 2.0;
        Object* found = FindExposure(pm);
        if (found != p) {
            ReleaseBright();
            bright.nextCheck = c.now + 2.0;
            p = found;
            if (p) {
                bright.mgr.set(pm);
                bright.param.set(p);
            }
        }
        if (p) {
            bright.misses = 0;
        } else if (++bright.misses >= 3 && !bright.noted) {
            bright.noted = true;
            Note("Brightness can't be changed in this part of the game");
            LOG("brightness: the game's brightness setting was not found");
        }
    }
    if (!p || !un::Val<bool>(p, R.fpOverride)) return;
    float cur = un::Val<float>(p, R.fpValue);
    if (!bright.have) {
        bright.base = cur;
        bright.have = true;
    } else if (std::fabs(cur - bright.written) > 1e-4f) {
        bright.base = cur;
    }
    float target = bright.base + std::log2(mult);
    if (cur != target) un::SetVal<float>(p, R.fpValue, target);
    bright.written = target;
}

struct FovState {
    patch::Hold fm;
    double nextFind = 0;
} fovs;

Object* FindFovManager() {
    Object* anim = R.lpAnimator && R.lpAnimator.isStatic ? Live(un::Static(R.lpAnimator)) : nullptr;
    if (!anim && R.lpGetAnimator) anim = Live(un::Call(R.lpGetAnimator, nullptr));
    if (!anim) return nullptr;
    return un::CompInChildren(anim, R.fovManager, true);
}

void ReleaseFov() {
    Object* fm = fovs.fm.get();
    if (un::Alive(fm) && un::Ref(fm, R.fmActive) == fm) {
        float speed = kInstant;
        un::Call(R.fmUnregister, fm, {fm, &speed, nullptr});
    }
    fovs.fm.reset();
    fovs.nextFind = 0;
}

void Fov(const Ctx& c) {
    if (!FovReady()) return;
    float deg = std::round(std::clamp(c.s.Value("fov", kFovDefault), 60.0f, 120.0f));
    bool want = c.inWorld && std::fabs(deg - kFovDefault) > 0.5f;
    if (!want) {
        if (fovs.fm.held()) ReleaseFov();
        return;
    }
    Object* fm = fovs.fm.get();
    if (fm && !un::Alive(fm)) {
        fovs.fm.reset();
        fovs.nextFind = 0;
        fm = nullptr;
    }
    if (c.now >= fovs.nextFind) {
        fovs.nextFind = c.now + 2.0;
        Object* found = FindFovManager();
        if (found != fm) {
            ReleaseFov();
            fovs.nextFind = c.now + 2.0;
            fm = found;
            if (fm) fovs.fm.set(fm);
        }
    }
    if (!fm || un::Val<float>(fm, R.fmDefault) <= 0.0f) return;
    float target = deg - (float)un::StaticVal<int>(R.fmGlobal, 0);
    Object* act = un::Ref(fm, R.fmActive);
    if (act && act != fm && un::Alive(act)) return;
    if (act == fm && std::fabs(un::Val<float>(fm, R.fmTarget) - target) < 0.01f) return;
    float speed = kInstant;
    un::Call(R.fmRegister, fm, {fm, &target, &speed, nullptr});
}

struct ConsoleState {
    patch::Hold dc;
    patch::Forced<bool> unblock;
    bool settled = false, other = false, started = false, gaveUp = false;
    int forced = 0;
    double since = 0, nextCheck = 0;
} con;

bool otherNoted = false;

bool AnyShown(Object* dc) {
    if (un::Val<bool>(dc, R.dcShowConsole)) return true;
    for (const Fld& f : R.dcPanels)
        if (un::Val<bool>(dc, f)) return true;
    return false;
}

bool Listening(Object* dc) { return Live(un::Ref(dc, R.dcRoutine)) && un::Ref(dc, R.dcInput) != nullptr; }

void StopListening(Object* dc) {
    Object* mb = Live(un::Ref(dc, R.dcRoutine));
    if (mb) un::Call(R.stopAll, mb);
    if (un::Ref(dc, R.dcInput)) un::SetRefField(dc, R.dcInput, nullptr);
}

bool Unblocked() { return con.unblock.have && con.unblock.orig != con.unblock.written; }

void ReleaseConsole() {
    Object* dc = Live(con.dc.get());
    if (dc && (con.started || Unblocked())) {
        if (un::Val<bool>(dc, R.dcShowConsole)) {
            bool off = false;
            un::Call(R.dcShow, dc, {&off});
        }
        if (con.started) {
            for (const Fld& f : R.dcPanels)
                if (un::Val<bool>(dc, f)) un::SetVal<bool>(dc, f, false);
            if (un::Enabled(dc)) un::Call(R.dcCheck, dc);
            StopListening(dc);
            if (un::Enabled(dc)) un::SetEnabled(dc, false);
        }
        LOG("developer console switched back off");
    }
    patch::Release(con.unblock, R.dcBlock);
    con.dc.reset();
    con.settled = con.other = con.started = con.gaveUp = false;
    con.forced = 0;
    con.nextCheck = 0;
}

void Console(const Ctx& c) {
    if (!ConsoleReady()) return;
    bool want = c.inWorld && c.s.On("enable_console");
    Object* dc = want ? Live(un::Static(R.dcInstance)) : nullptr;
    if (con.dc.held() && (!dc || con.dc.get() != dc)) ReleaseConsole();
    if (!dc) return;
    if (!con.dc.held()) {
        con.dc.set(dc);
        con.since = c.now;
    }
    if (con.other) return;
    if (con.settled) patch::Force<bool>(dc, R.dcBlock, true, false, con.unblock);
    if (con.gaveUp || c.now < con.nextCheck) return;
    con.nextCheck = c.now + 0.25;
    bool enabled = un::Enabled(dc);
    if (!con.settled) {
        con.forced = enabled && !AnyShown(dc) ? con.forced + 1 : 0;
        if (c.now - con.since < 1.0) return;
        if (con.forced >= 3) {
            con.other = true;
            LOG("developer console: another mod keeps it switched on, leaving it alone");
            if (!otherNoted) {
                otherNoted = true;
                Note("Another mod already runs the developer console");
            }
            return;
        }
        con.settled = true;
        patch::Force<bool>(dc, R.dcBlock, true, false, con.unblock);
        if (enabled || Listening(dc)) {
            LOG("developer console unlocked");
            Note("Developer console is on, press F1 to open it");
            return;
        }
    }
    if (enabled || Listening(dc)) return;
    if (con.started) {
        con.gaveUp = true;
        Note("Something else switched the developer console off");
        LOG("developer console: switched off by something else");
        return;
    }
    Object* go = un::GameObjectOf(dc);
    if (!go || !un::CallBool(un::E.goActiveInHierarchy, go)) return;
    un::SetEnabled(dc, true);
    un::Call(R.dcCheck, dc);
    con.started = true;
    bool on = un::Enabled(dc), listening = Listening(dc);
    LOG("developer console started (enabled %d, listening %d)", on ? 1 : 0, listening ? 1 : 0);
    if (on || listening) {
        Note("Developer console is on, press F1 to open it");
    } else {
        con.gaveUp = true;
        Note("The developer console could not be started");
    }
}

double lastSave = -100;

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return {};
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

fs::file_time_type DataTime(const fs::path& dir) {
    fs::file_time_type best = fs::file_time_type::min();
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        std::wstring ext = it->path().extension().wstring();
        if (_wcsicmp(ext.c_str(), L".zip") != 0 && _wcsicmp(ext.c_str(), L".json") != 0) continue;
        fs::file_time_type t = it->last_write_time(e2);
        if (!e2 && t > best) best = t;
    }
    return best;
}

bool CopySlot(const fs::path& src, const fs::path& dst) {
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) return false;
    fs::copy(src, dst, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    if (ec) return false;
    int files = 0;
    for (fs::recursive_directory_iterator it(src, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || e2) continue;
        fs::path rel = fs::relative(it->path(), src, e2);
        if (e2) return false;
        uintmax_t a = it->file_size(e2);
        if (e2) return false;
        uintmax_t b = fs::file_size(dst / rel, e2);
        if (e2 || a != b) return false;
        ++files;
    }
    return !ec && files > 0;
}

void Prune(const fs::path& root, const std::wstring& prefix) {
    std::error_code ec;
    std::vector<fs::path> mine;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || e2) continue;
        if (it->path().filename().wstring().rfind(prefix, 0) == 0) mine.push_back(it->path());
    }
    if (ec || mine.size() <= kKeepBackups) return;
    std::sort(mine.begin(), mine.end());
    for (size_t i = 0; i + kKeepBackups < mine.size(); ++i) {
        std::error_code e2;
        fs::remove_all(mine[i], e2);
    }
}

bool BackupSlot(const fs::path& src, uint32_t slot, std::wstring& where) {
    where.clear();
    try {
        std::error_code ec;
        if (!fs::is_directory(src, ec)) return !ec;
        const std::wstring& base = Folder();
        if (base.empty()) return false;
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t prefix[40], name[96];
        swprintf_s(prefix, L"InstantSave_%010u_", (unsigned)slot);
        swprintf_s(name, L"%ls%04d-%02d-%02d_%02d-%02d-%02d-%03d", prefix, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
                   t.wMilliseconds);
        fs::path root = fs::path(base) / L"save-backups";
        fs::path dst = root / name;
        if (!CopySlot(src, dst)) return false;
        Prune(root, prefix);
        where = dst.wstring();
        return true;
    } catch (...) {
        return false;
    }
}

int SaveCount(int type) {
    Object* ids = nullptr;
    bool ok = false;
    un::Call(R.smIds, nullptr, {&type, &ids}, &ok);
    if (!ok) return -1;
    return ids ? il2::ListCount(ids) : 0;
}

bool Filled(Object* s) { return s && !il2::Str(s).empty(); }

Object* SaveName() {
    if (!R.gsGetName || !R.gsSetName || !R.gsInstance || !R.gsInstance.isStatic) return il2::NewString("");
    Object* gs = Live(un::Static(R.gsInstance));
    if (!gs) return il2::NewString("SaveGame");
    Object* name = un::Call(R.gsGetName, gs);
    Object* pick = Filled(name) ? name : nullptr;
    if (pick && R.gsIsGenerated && R.gsUpdateName && un::CallBool(R.gsIsGenerated, nullptr, {pick})) {
        Object* updated = un::Call(R.gsUpdateName, gs, {pick});
        if (Filled(updated)) pick = updated;
    }
    if (!pick && R.gsNewName) {
        Object* made = un::Call(R.gsNewName, gs);
        if (Filled(made)) pick = made;
    }
    if (!pick) pick = il2::NewString("SaveGame");
    un::Call(R.gsSetName, gs, {pick});
    return pick;
}

std::string SaveBlocked() {
    if (R.smLoading && un::CallBool(R.smLoading, nullptr)) return "Can't save while the game is loading";
    if (R.csPlaying && un::CallBool(R.csPlaying, nullptr)) return "Can't save during a cutscene";
    Object* stats = R.lpStats ? Live(un::Call(R.lpStats, nullptr)) : nullptr;
    if (stats && ((R.psDead && un::CallBool(R.psDead, stats)) || (R.psDying && un::CallBool(R.psDying, stats))))
        return "Can't save while you are dead or knocked out";
    if (R.lpInWorld && !un::CallBool(R.lpInWorld, nullptr, {}, true)) return "Close the inventory or wake up first, then save";
    if ((R.lpMidAction && un::CallBool(R.lpMidAction, nullptr)) || (R.lpBlockFull && un::CallBool(R.lpBlockFull, nullptr)))
        return "Finish what you are doing first, then save";
    if (R.lpGrounded && !un::CallBool(R.lpGrounded, nullptr)) return "Stand on solid ground first, then save";
    return {};
}

bool CallSave(int type, Object* name, int id) {
    bool ok = false;
    if (R.smSave) {
        un::Call(R.smSave, nullptr, {&type, name, &id}, &ok);
        return ok;
    }
    if (!R.smSaveToSlot || un::StaticVal<int>(R.smActiveType, -1) != type) return false;
    un::Call(R.smSaveToSlot, nullptr, {&id, name}, &ok);
    return ok;
}

void GuestSave(const Ctx& c, int type) {
    Object* gameId = un::Call(R.gsmGameId, nullptr);
    uint32_t slot = 0;
    if (Filled(gameId)) {
        bool found = false;
        Object* boxed = un::Call(R.csSlot, nullptr, {&type, gameId}, &found);
        if (!found) {
            Note("Saving failed, the list of saves could not be read");
            return;
        }
        slot = il2::Result<uint32_t>(boxed, 0);
    }
    int client = R.clientType;
    int id = (int)slot;
    fs::path dir;
    fs::file_time_type before = fs::file_time_type::min();
    std::wstring backup;
    int countBefore = 0;
    if (slot != 0) {
        std::wstring folder = Widen(il2::Str(un::Call(R.smFolder, nullptr, {&client, &id})));
        if (folder.empty()) {
            Note("Saving failed, your character's save folder was not found");
            return;
        }
        dir = fs::path(folder);
        if (!BackupSlot(dir, slot, backup)) {
            Note("Could not back up your old character save, nothing was saved");
            LOG("instant save (guest): backup of slot %010u failed", (unsigned)slot);
            return;
        }
        before = DataTime(dir);
    } else {
        countBefore = SaveCount(client);
        if (countBefore < 0) {
            Note("Saving failed, the list of saves could not be read");
            return;
        }
    }
    lastSave = c.now;
    bool ok = false;
    un::Call(R.csSavePlayer, nullptr, {}, &ok);
    if (!ok) {
        Note("Saving failed, see fathom.log");
        LOG("instant save (guest): the game's player save call failed: %s", il2::LastException().c_str());
        return;
    }
    if (slot != 0) {
        fs::file_time_type after = DataTime(dir);
        if (after == fs::file_time_type::min() || after == before) {
            Note("The game did not write your character save, see fathom.log");
            LOG("instant save (guest): slot %010u was not rewritten", (unsigned)slot);
            return;
        }
        Note(backup.empty() ? "Saved your character, the host saves the world"
                            : "Saved your character, the old one was copied to Fathom's save-backups folder");
        LOG("instant save (guest): wrote character slot %010u", (unsigned)slot);
    } else {
        if (SaveCount(client) <= countBefore) {
            Note("The game did not write your character save, see fathom.log");
            LOG("instant save (guest): no new character slot appeared");
            return;
        }
        Note("Saved your character, the host saves the world");
        LOG("instant save (guest): wrote a new character slot");
    }
}

void InstantSave(const Ctx& c) {
    if (c.now - lastSave < 3.0) {
        Note("Still saving, try again in a moment");
        return;
    }
    if (!c.inWorld || !c.player) {
        Note("Load a save first");
        return;
    }
    std::string why = SaveBlocked();
    if (!why.empty()) {
        Note(why);
        return;
    }
    int type = un::CallInt(R.gsmType, nullptr, {}, -1);
    if (type < 0) {
        Note("Saving failed, the game did not say what kind of game this is");
        return;
    }
    if (Guest(c) || type == R.clientType) {
        if (GuestSaveReady()) GuestSave(c, type);
        else Note("Only the host can save this game");
        return;
    }
    if (!SaveReady()) {
        Note("Saving is not available right now");
        return;
    }
    Object* gameId = un::Call(R.gsmGameId, nullptr);
    uint32_t slot = 0;
    if (Filled(gameId)) {
        bool found = false;
        Object* boxed = un::Call(R.csSlot, nullptr, {&type, gameId}, &found);
        if (!found) {
            Note("Saving failed, the list of saves could not be read");
            return;
        }
        slot = il2::Result<uint32_t>(boxed, 0);
    }
    int id = (int)slot;
    fs::path dir;
    fs::file_time_type before = fs::file_time_type::min();
    std::wstring backup;
    int countBefore = 0;
    if (slot != 0) {
        std::wstring folder = Widen(il2::Str(un::Call(R.smFolder, nullptr, {&type, &id})));
        if (folder.empty()) {
            Note("Saving failed, this game's save folder was not found");
            return;
        }
        dir = fs::path(folder);
        if (!BackupSlot(dir, slot, backup)) {
            Note("Could not back up the old save, nothing was saved");
            LOG("instant save: backup of slot %010u failed", (unsigned)slot);
            return;
        }
        before = DataTime(dir);
    } else {
        countBefore = SaveCount(type);
        if (countBefore < 0) {
            Note("Saving failed, the list of saves could not be read");
            return;
        }
        if (countBefore >= kMaxSaves) {
            Note("There are already 30 saves, delete one in the game first");
            return;
        }
    }
    Object* name = SaveName();
    lastSave = c.now;
    if (!CallSave(type, name, id)) {
        Note("Saving failed, see fathom.log");
        LOG("instant save: the game's save call failed: %s", il2::LastException().c_str());
        return;
    }
    if (slot != 0) {
        fs::file_time_type after = DataTime(dir);
        if (after == fs::file_time_type::min() || after == before) {
            Note("The game did not write the save, see fathom.log");
            LOG("instant save: slot %010u was not rewritten", (unsigned)slot);
            return;
        }
        Note(backup.empty() ? "Game saved" : "Game saved, the old save was copied to Fathom's save-backups folder");
        LOG("instant save: wrote slot %010u (%s)", (unsigned)slot, il2::Str(name).c_str());
    } else {
        if (SaveCount(type) <= countBefore) {
            Note("The game did not write the save, see fathom.log");
            LOG("instant save: no new slot appeared");
            return;
        }
        Note("Game saved in a new slot");
        LOG("instant save: wrote a new slot (%s)", il2::Str(name).c_str());
    }
}

bool Resolve(std::string& err) {
    R.localPlayer = un::C("Sons", "TheForest.Utils", "LocalPlayer");
    if (R.localPlayer) {
        R.lpAnimator = un::F(R.localPlayer, "<Animator>k__BackingField");
        R.lpGetAnimator = un::M(R.localPlayer, "get_Animator", 0);
        R.lpMidAction = un::M(R.localPlayer, "get_IsInMidAction", 0);
        R.lpBlockFull = un::M(R.localPlayer, "get_BlockFullBodyInteraction", 0);
        R.lpGrounded = un::M(R.localPlayer, "IsGroundedOrKnightVGrounded", 0);
        R.lpInWorld = un::M(R.localPlayer, "get_IsInWorld", 0);
        R.lpStats = un::M(R.localPlayer, "get_Stats", 0);
    }
    if ((R.playerStats = un::C("Sons", "", "PlayerStats"))) {
        R.psDead = un::M(R.playerStats, "IsDead", 0);
        R.psDying = un::M(R.playerStats, "IsDying", 0);
    }
    if ((R.cutscenes = un::C("Sons", "Sons.Cutscenes", "CutsceneManager"))) R.csPlaying = un::M(R.cutscenes, "IsCutscenePlaying", 0);

    if ((R.timeHolder = un::C("Sons.Atmosphere", "Sons.Environment", "TimeOfDayHolder"))) {
        R.todInstance = un::F(R.timeHolder, "_instance");
        R.todSpeed = un::F(R.timeHolder, "_baseSpeedMultiplier");
        R.todFinal = un::F(R.timeHolder, "_finalSpeed");
        if (R.todFinal.isStatic) R.todFinal = Fld{};
    }

    const char* core = "Unity.RenderPipelines.Core.Runtime";
    if ((R.postManager = un::C("Sons.PostProcessing", "Sons.PostProcessing", "PostProcessingManager"))) {
        R.ppInstance = un::F(R.postManager, "_instance");
        R.ppVolume = un::F(R.postManager, "_brightnessOverrideVolumeTarget");
    }
    if ((R.volume = un::C(core, "UnityEngine.Rendering", "Volume"))) R.volProfile = un::M(R.volume, "get_profile", 0);
    if ((R.volumeProfile = un::C(core, "UnityEngine.Rendering", "VolumeProfile"))) R.vpComponents = un::F(R.volumeProfile, "components");
    if ((R.floatParam = un::C(core, "UnityEngine.Rendering", "FloatParameter"))) {
        R.fpValue = un::F(R.floatParam, "m_Value");
        R.fpOverride = un::F(R.floatParam, "m_OverrideState");
    }
    if ((R.colorAdjust = un::C("Unity.RenderPipelines.HighDefinition.Runtime", "UnityEngine.Rendering.HighDefinition", "ColorAdjustments")))
        R.caExposure = un::F(R.colorAdjust, "postExposure");

    if ((R.fovManager = un::C("Sons.FieldOfView", "Sons.FieldOfView", "FovManager"))) {
        R.fmActive = un::F(R.fovManager, "_activeSource");
        R.fmTarget = un::F(R.fovManager, "_currentTargetFieldOfView");
        R.fmDefault = un::F(R.fovManager, "_defaultFov");
        R.fmGlobal = un::F(R.fovManager, "_globalOffset");
        R.fmRegister = un::M(R.fovManager, "RegisterAbsolute", 4);
        R.fmUnregister = un::M(R.fovManager, "Unregister", 3);
    }

    R.dcPanels.clear();
    if ((R.console = un::C("Sons", "TheForest", "DebugConsole"))) {
        R.dcInstance = un::F(R.console, "Instance");
        R.dcShowConsole = un::F(R.console, "_showConsole");
        R.dcRoutine = un::F(R.console, "_routineMB");
        R.dcInput = un::F(R.console, "_inputRoutine");
        R.dcBlock = un::F(R.console, "_blockConsole");
        for (const char* n : {"_showLog", "_showOverlay", "_showGamePadWheel", "_showPlayerStats", "_showFramerate", "_showPlayerVisibility"})
            if (Fld f = un::F(R.console, n); f && !f.isStatic) R.dcPanels.push_back(f);
        R.dcCheck = un::M(R.console, "CheckDisplayState", 0);
        R.dcShow = un::M(R.console, "ShowConsole", 1);
    }
    if (un::E.monoBehaviour) R.stopAll = un::M(un::E.monoBehaviour, "StopAllCoroutines", 0);

    if ((R.saveType = un::C("Sons.Save", "Sons.Save", "SaveGameType"))) R.clientType = (int)il2::EnumValue(R.saveType, "MultiplayerClient", 2);
    if ((R.saveManager = un::C("Sons.Save", "Sons.Save", "SaveGameManager"))) {
        R.smSave = il2::FindMethod(R.saveManager, "Save", std::vector<std::string>{"Sons.Save.SaveGameType", "System.String", "System.Int32"});
        if (R.smSave && R.saveType && il2::ParamClass(R.smSave, 0) != R.saveType) R.smSave = nullptr;
        R.smSaveToSlot = un::M(R.saveManager, "SaveToSlot", 2);
        R.smActiveType = un::F(R.saveManager, "_activeSaveGameType");
        R.smFolder = un::M(R.saveManager, "GetSaveFolderForId", 2);
        R.smIds = un::M(R.saveManager, "TryGetSaveGameIds", 2);
        R.smLoading = un::M(R.saveManager, "LoadInProgress", 0);
        if (!R.smSave) LOG("instant save: using the game's save-to-slot call");
    }
    if ((R.setupManager = un::C("Sons.Gameplay.GameSetup", "Sons.Gameplay.GameSetup", "GameSetupManager"))) {
        R.gsmType = un::M(R.setupManager, "GetSaveGameType", 0);
        R.gsmGameId = un::M(R.setupManager, "GetGameId", 0);
    }
    if ((R.coopSave = un::C("Sons.Multiplayer", "Sons.Multiplayer", "CoopSaveUtils"))) {
        R.csSlot = un::M(R.coopSave, "GetSaveIdForGuid", 2);
        R.csSavePlayer = un::M(R.coopSave, "SavePlayerData", 0);
    }
    if ((R.gameState = un::C("Sons.Gameplay.GameSetup", "Sons.Save", "GameState"))) {
        R.gsInstance = un::F(R.gameState, "_instance");
        R.gsGetName = un::M(R.gameState, "GetGameName", 0);
        R.gsSetName = un::M(R.gameState, "SetGameName", 1);
        R.gsIsGenerated = un::M(R.gameState, "ContainsGeneratedSaveGameName", 1);
        R.gsUpdateName = un::M(R.gameState, "UpdateGeneratedSaveGameName", 1);
        R.gsNewName = un::M(R.gameState, "GenerateSaveGameName", 0);
    }

    bool t = TimeReady(), b = BrightReady(), f = FovReady(), k = ConsoleReady(), s = SaveReady(), g = GuestSaveReady();
    LOG("world parts: time %d, brightness %d, field of view %d, console %d, save %d, guest save %d", t ? 1 : 0, b ? 1 : 0, f ? 1 : 0,
        k ? 1 : 0, s ? 1 : 0, g ? 1 : 0);
    if (!t && !b && !f && !k && !s && !g) {
        err = "none of the world parts were found";
        return false;
    }
    return true;
}

bool Supported(const std::string& id) {
    if (id == "stop_time") return TimeReady();
    if (id == "brightness") return BrightReady();
    if (id == "fov") return FovReady();
    if (id == "enable_console") return ConsoleReady();
    if (id == "instant_save") return SaveReady() || GuestSaveReady();
    return false;
}

void Tick(const Ctx& c) {
    StopTime(c);
    Brightness(c);
    Fov(c);
    Console(c);
}

bool Action(const std::string& id, const Ctx& c) {
    if (id != "instant_save" || !(SaveReady() || GuestSaveReady())) return false;
    InstantSave(c);
    return true;
}

void Release(const Ctx&) {
    ReleaseTime();
    ReleaseBright();
    ReleaseFov();
    ReleaseConsole();
}

}

const Group& WorldGroup() {
    static Group g{"World", &Resolve, &Supported, &Tick, &Action, &Release, nullptr};
    return g;
}

}
