#pragma once
#include <string>

namespace audio {

bool Install();
bool Ready();
bool SetMasterPitch(float pitch);
void* Load(const std::string& utf8Path, bool stream);
void* Play(void* sound, float volume, float pitch);
void Stop(void* channel);
void SetVolume(void* channel, float volume);
void Free(void* sound);

}
