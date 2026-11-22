#include "ingame/audio.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include <atomic>

namespace audio {

namespace {

using UpdateFn = int(__fastcall*)(void*);
using MasterFn = int(__fastcall*)(void*, void**);
using FloatFn = int(__fastcall*)(void*, float);
using VoidFn = int(__fastcall*)(void*);
using CreateFn = int(__fastcall*)(void*, const char*, unsigned, void*, void**);
using PlayFn = int(__fastcall*)(void*, void*, void*, bool, void**);
using PausedFn = int(__fastcall*)(void*, bool);

UpdateFn oUpdate = nullptr;
MasterFn getMaster = nullptr;
FloatFn setPitch = nullptr;
FloatFn setVolume = nullptr;
VoidFn stop = nullptr;
VoidFn release = nullptr;
CreateFn createSound = nullptr;
PlayFn playSound = nullptr;
PausedFn setPaused = nullptr;
std::atomic<void*> sys{nullptr};
bool ready = false;

int __fastcall hkUpdate(void* s) {
    gd::Busy busy;
    sys = s;
    return oUpdate(s);
}

}

bool Install() {
    if (ready) return true;
    getMaster = (MasterFn)gd::FmodFn("?getMasterChannelGroup@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVChannelGroup@2@@Z");
    setPitch = (FloatFn)gd::FmodFn("?setPitch@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z");
    setVolume = (FloatFn)gd::FmodFn("?setVolume@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z");
    stop = (VoidFn)gd::FmodFn("?stop@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
    release = (VoidFn)gd::FmodFn("?release@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
    createSound = (CreateFn)gd::FmodFn("?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z");
    playSound = (PlayFn)gd::FmodFn("?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAVSound@2@PEAVChannelGroup@2@_NPEAPEAVChannel@2@@Z");
    setPaused = (PausedFn)gd::FmodFn("?setPaused@ChannelControl@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z");
    void* update = gd::FmodFn("?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
    if (!getMaster || !setPitch || !setVolume || !stop || !release || !createSound || !playSound || !setPaused || !update) {
        LOG("audio: FMOD functions were not found");
        return false;
    }
    if (!gd::Hook(update, (void*)&hkUpdate, (void**)&oUpdate, "audio update")) return false;
    ready = true;
    return true;
}

bool Ready() { return ready && sys.load(); }

bool SetMasterPitch(float pitch) {
    void* s = sys.load();
    if (!ready || !s) return false;
    void* master = nullptr;
    if (getMaster(s, &master) != 0 || !master) return false;
    return setPitch(master, pitch) == 0;
}

void* Load(const std::string& utf8Path) {
    void* s = sys.load();
    if (!ready || !s) return nullptr;
    void* sound = nullptr;
    if (createSound(s, utf8Path.c_str(), 0, nullptr, &sound) != 0) return nullptr;
    return sound;
}

void* Play(void* sound, float volume, float pitch) {
    void* s = sys.load();
    if (!ready || !s || !sound) return nullptr;
    void* channel = nullptr;
    if (playSound(s, sound, nullptr, true, &channel) != 0 || !channel) return nullptr;
    setVolume(channel, volume);
    setPitch(channel, pitch);
    setPaused(channel, false);
    return channel;
}

void Stop(void* channel) {
    if (ready && channel) stop(channel);
}

void Free(void* sound) {
    if (ready && sound) release(sound);
}

}
