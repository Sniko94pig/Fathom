#include "ingame/modules.h"

namespace speed {

bool Install() { return false; }
bool Ready() { return false; }
void Tick() {}
float Scale(float dt) { return dt; }
void Off() {}

}
