#include "game/internal.h"
#include "game/mem.h"
#include "game/shv.h"
#include "ingame/log.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace gtav {

namespace {

struct Layout {
    bool tried = false;
    bool found = false;
    int pedOff = -1;
    int infoOff = -1;
    int nameOff = -1;
    int clipOff = -1;
    int tbsOff = -1;
    int dmgOff = -1;
    bool shaped = false;
    double nextTry = 0;
    int attempts = 0;
} lay;

struct Original {
    uint32_t weapon = 0;
    float tbs = 0;
    float spread = 0;
    float batchSpread = 0;
    bool tbsSet = false;
    bool spreadSet = false;
    bool batchSet = false;
};

std::unordered_map<uintptr_t, Original> touched;
uintptr_t activeInfo = 0;

struct Seen {
    uint32_t weapon = 0;
    int clip = -1;
    int total = -1;
} seen;

bool noRecoilShake = false;

uint32_t Selected() { return st.ped ? (uint32_t)n::GET_SELECTED_PED_WEAPON(st.ped) : 0; }

uintptr_t Deref(uintptr_t p) {
    uintptr_t v = 0;
    return mem::Read(p, v) && mem::HeapLike(v) ? v : 0;
}

bool Search(uintptr_t ped, uint32_t w, int clip, float tbs, float dmg) {
    static const int kNameOffs[] = {0x10, 0x8, 0xC, 0x14, 0x18};
    std::vector<uint8_t> block(0x480);
    for (int po = 0x10; po < 0x2400; po += 8) {
        uintptr_t mgr = Deref(ped + po);
        if (!mgr) continue;
        for (int io = 0x8; io < 0x80; io += 8) {
            uintptr_t info = Deref(mgr + io);
            if (!info) continue;
            for (int no : kNameOffs) {
                uint32_t h = 0;
                if (!mem::Read(info + no, h) || h != w) continue;
                if (!mem::Copy(block.data(), (const void*)info, block.size())) continue;
                int c = -1, t = -1, d = -1;
                std::vector<int> cs, ts, ds;
                for (int k = 0x20; k + 4 <= 0x300; k += 4) {
                    int32_t iv;
                    std::memcpy(&iv, block.data() + k, 4);
                    if (iv == clip) cs.push_back(k);
                }
                for (int k = 0x40; k + 4 <= (int)block.size(); k += 4) {
                    float fv;
                    std::memcpy(&fv, block.data() + k, 4);
                    if (fv == tbs) ts.push_back(k);
                    if (k < 0x300 && dmg > 0 && fv == dmg) ds.push_back(k);
                }
                bool shaped = false;
                for (int ck : cs)
                    for (int tk : ts)
                        if (tk - ck == 0xCC) {
                            c = ck;
                            t = tk;
                            for (int dk : ds)
                                if (dk - ck == 0x40) {
                                    d = dk;
                                    shaped = true;
                                }
                        }
                if (c < 0 && cs.size() == 1) c = cs[0];
                if (t < 0 && ts.size() == 1) t = ts[0];
                if (t < 0) continue;
                lay.pedOff = po;
                lay.infoOff = io;
                lay.nameOff = no;
                lay.clipOff = c;
                lay.tbsOff = t;
                lay.dmgOff = d;
                lay.shaped = shaped;
                LOG("weapon data found: ped+0x%X -> +0x%X, name +0x%X, clip +0x%X, time between shots +0x%X, damage +0x%X, known shape %d", po,
                    io, no, c, t, d, shaped ? 1 : 0);
                return true;
            }
        }
    }
    return false;
}

uintptr_t InfoFor(uint32_t w) {
    if (!st.ped || !w || !IsGun(w)) return 0;
    uintptr_t ped = (uintptr_t)shv::Address(st.ped);
    if (!ped) return 0;
    int clip = n::GET_WEAPON_CLIP_SIZE(w);
    float tbs = n::GET_WEAPON_TIME_BETWEEN_SHOTS(w);
    float dmg = n::GET_WEAPON_DAMAGE(w, 0);
    if (clip <= 0 || !(tbs > 0.0f)) return 0;
    if (!lay.found) {
        if (st.time < lay.nextTry) return 0;
        lay.tried = true;
        ++lay.attempts;
        lay.nextTry = st.time + 5.0;
        lay.found = Search(ped, w, clip, tbs, dmg);
        if (!lay.found) {
            if (lay.attempts <= 3) LOG("weapon data not found yet (attempt %d)", lay.attempts);
            return 0;
        }
    }
    uintptr_t info = Deref(Deref(ped + lay.pedOff) + lay.infoOff);
    if (!info) return 0;
    uint32_t h = 0;
    if (!mem::Read(info + lay.nameOff, h) || h != w) return 0;
    float t = 0;
    if (!mem::Read(info + lay.tbsOff, t) || std::fabs(t - tbs) > 1e-6f) return 0;
    return info;
}

void RestoreInfo(uintptr_t info) {
    auto it = touched.find(info);
    if (it == touched.end()) return;
    uint32_t h = 0;
    if (mem::Read(info + lay.nameOff, h) && h == it->second.weapon) {
        if (it->second.tbsSet) mem::Write(info + lay.tbsOff, it->second.tbs);
        if (it->second.spreadSet) mem::Write(info + lay.clipOff + 4, it->second.spread);
        if (it->second.batchSet) mem::Write(info + lay.tbsOff - 0x18, it->second.batchSpread);
    }
    touched.erase(it);
}

void RestoreAll() {
    std::vector<uintptr_t> keys;
    for (auto& [k, v] : touched) keys.push_back(k);
    for (uintptr_t k : keys) RestoreInfo(k);
    activeInfo = 0;
}

void TuneTick(uint32_t w) {
    float rate = std::clamp(Value("fire_rate"), 1.0f, 10.0f);
    bool fast = rate > 1.001f;
    bool tight = On("no_spread");
    if (!fast && !tight) {
        if (!touched.empty()) RestoreAll();
        return;
    }
    uintptr_t info = st.alive ? InfoFor(w) : 0;
    if (activeInfo && activeInfo != info) RestoreInfo(activeInfo);
    activeInfo = info;
    if (!info) return;
    Original& o = touched[info];
    o.weapon = w;
    if (fast) {
        if (!o.tbsSet) {
            mem::Read(info + lay.tbsOff, o.tbs);
            o.tbsSet = true;
        }
        mem::Write(info + lay.tbsOff, o.tbs / rate);
    } else if (o.tbsSet) {
        mem::Write(info + lay.tbsOff, o.tbs);
        o.tbsSet = false;
    }
    bool canSpread = false;
    if (lay.shaped && lay.clipOff >= 0) {
        int32_t c = 0;
        canSpread = mem::Read(info + lay.clipOff, c) && c == n::GET_WEAPON_CLIP_SIZE(w);
    }
    if (tight && canSpread) {
        if (!o.spreadSet) {
            float s = 0;
            if (mem::Read(info + lay.clipOff + 4, s) && s >= 0.0f && s <= 30.0f) {
                o.spread = s;
                o.spreadSet = true;
            }
        }
        if (o.spreadSet) mem::Write(info + lay.clipOff + 4, 0.0f);
        if (!o.batchSet) {
            int count = 0;
            float bs = 0;
            if (mem::Read(info + lay.tbsOff - 0x1C, count) && count >= 1 && count <= 32 && mem::Read(info + lay.tbsOff - 0x18, bs) &&
                bs >= 0.0f && bs <= 2.0f) {
                o.batchSpread = bs;
                o.batchSet = true;
            }
        }
        if (o.batchSet) mem::Write(info + lay.tbsOff - 0x18, 0.0f);
    } else {
        if (o.spreadSet) {
            mem::Write(info + lay.clipOff + 4, o.spread);
            o.spreadSet = false;
        }
        if (o.batchSet) {
            mem::Write(info + lay.tbsOff - 0x18, o.batchSpread);
            o.batchSet = false;
        }
    }
}

void RecoilTick(uint32_t w) {
    if (!On("no_recoil") || !st.alive || !IsGun(w)) {
        noRecoilShake = false;
        return;
    }
    bool busy = n::IS_PED_SHOOTING(st.ped) || n::IS_PLAYER_FREE_AIMING(st.player) || (seen.weapon == w && noRecoilShake);
    if (!busy) return;
    noRecoilShake = n::IS_PED_SHOOTING(st.ped) != 0;
    if (n::IS_GAMEPLAY_CAM_SHAKING()) {
        n::SET_GAMEPLAY_CAM_SHAKE_AMPLITUDE(0.0f);
        n::STOP_GAMEPLAY_CAM_SHAKING(1);
    }
}

int CountShots(uint32_t w, int clip, int total) {
    int shots = 0;
    if (seen.weapon == w) {
        if (seen.total >= 0 && total >= 0 && total < seen.total) shots = seen.total - total;
        else if (seen.clip >= 0 && clip >= 0 && clip < seen.clip) shots = seen.clip - clip;
    }
    if (shots == 0 && n::IS_PED_SHOOTING(st.ped)) shots = 1;
    return std::min(shots, 6);
}

void AmmoTick(uint32_t w, int& clip, int& total) {
    bool unlimited = On("unlimited_ammo") && st.alive;
    if (!st.alive || !IsGun(w)) return;
    if (unlimited) {
        int max = 0;
        if (n::GET_MAX_AMMO(st.ped, w, &max) && max > 0 && total < max) {
            n::SET_PED_AMMO(st.ped, w, max, 0);
            total = n::GET_AMMO_IN_PED_WEAPON(st.ped, w);
        }
    }
    if (On("no_reload") && !n::IS_PED_RELOADING(st.ped)) {
        int maxClip = n::GET_MAX_AMMO_IN_CLIP(st.ped, w, 1);
        if (maxClip > 0 && clip >= 0 && clip < maxClip && total > clip) {
            int want = std::min(maxClip, total);
            if (n::SET_AMMO_IN_CLIP(st.ped, w, want)) clip = want;
        }
    }
}

void RestoreAmmo() {
    if (!st.ped || !st.alive) {
        Note("Restore ammo works when your character is alive");
        return;
    }
    int count = 0;
    for (uint32_t w : KnownWeapons()) {
        if (!n::HAS_PED_GOT_WEAPON(st.ped, w, 0)) continue;
        int max = 0;
        if (n::GET_MAX_AMMO(st.ped, w, &max) && max > 0) n::SET_PED_AMMO(st.ped, w, max, 0);
        int maxClip = n::GET_MAX_AMMO_IN_CLIP(st.ped, w, 1);
        if (maxClip > 0) n::SET_AMMO_IN_CLIP(st.ped, w, maxClip);
        ++count;
    }
    Note(count ? "Ammo refilled for " + std::to_string(count) + " weapons" : "You have no weapons to refill");
}

}

void WeaponsInit() {
    Ready("restore_ammo", true);
    Ready("unlimited_ammo", true);
    Ready("no_reload", true);
    Ready("fire_rate", true);
    Ready("no_recoil", true);
    Ready("no_spread", true);
}

void WeaponsTick() {
    uint32_t w = st.alive ? Selected() : 0;
    int clip = -1, total = -1;
    if (w && IsGun(w)) {
        if (!n::GET_AMMO_IN_CLIP(st.ped, w, &clip)) clip = -1;
        total = n::GET_AMMO_IN_PED_WEAPON(st.ped, w);
        int shots = CountShots(w, clip, total);
        if (shots > 0) SilentShots(w, shots);
    }
    AmmoTick(w, clip, total);
    TuneTick(w);
    RecoilTick(w);
    seen.weapon = w;
    seen.clip = clip;
    seen.total = total;
    if (Pressed("restore_ammo")) RestoreAmmo();
    static bool told = false;
    if (!told && lay.attempts >= 3 && !lay.found && (On("no_spread") || Value("fire_rate") > 1.001f)) {
        told = true;
        Note("Fire rate and No spread could not find the weapon data in this game version");
    }
    static bool toldSpread = false;
    if (!toldSpread && lay.found && !lay.shaped && On("no_spread")) {
        toldSpread = true;
        Note("No spread is not available in this game version");
        LOG("no spread: weapon data layout not confirmed");
    }
}

void WeaponsOff() {
    RestoreAll();
    seen = Seen{};
}

}
