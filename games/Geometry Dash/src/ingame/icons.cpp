#include "ingame/modules.h"

namespace icons {

bool Install() { return false; }
bool Ready() { return false; }
void Set(bool) {}
void Tick() {}
void Off() {}
bool BlockUpload(const char*) { return false; }

}
