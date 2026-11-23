#include "ingame/audio.h"
#include "ingame/gd.h"
#include "ingame/log.h"
#include <atomic>

namespace audio {

namespace {

using UpdateFn = int(__fastcall*)(void*);
using MasterFn = int(__fastcall*)(void*, void**);
using FloatFn = int(__fastcall*)(void*, float);

UpdateFn oUpdate = nullptr;
MasterFn getMaster = nullptr;
FloatFn setPitch = nullptr;
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
    void* update = gd::FmodFn("?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ");
    if (!getMaster || !setPitch || !update) {
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

}
