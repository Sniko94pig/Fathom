#include "ingame/modules.h"

namespace safe {

bool Install() { return false; }
bool Ready() { return false; }
void Tick() {}
void Frame() {}
void Off() {}
bool InLevel() { return false; }
bool Cheated() { return false; }
bool Holding() { return false; }
bool Locked() { return false; }
void MarkCheated(const char*) {}
const char* Status(bool& good) {
    good = false;
    return nullptr;
}
Protect::Protect() {}
Protect::~Protect() {}

}
