#include "ingame/modules.h"

namespace player {

bool Install() { return false; }
bool NoclipReady() { return false; }
bool JumpsReady() { return false; }
bool HitboxReady() { return false; }
bool CompleteReady() { return false; }
std::string Complete() { return "Not available"; }
void Tick() {}
void Off() {}

}
