#include "ingame/groups.h"

namespace ukmove {

bool Resolve() { return true; }

bool Supported(const std::string&) { return true; }

void Tick(const uk::Ctx&) {}

void Release(const uk::Ctx&) {}

void Action(const std::string&, const uk::Ctx&) {}

bool Busy() { return false; }

}
