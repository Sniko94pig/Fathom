#include "ingame/groups.h"
#include <string>

namespace ukaim {

bool Resolve() { return true; }

bool Supported(const std::string& id) { return id == "softaim" || id == "aim_strength" || id == "aim_area"; }

void Tick(const uk::Ctx&) {}

void Release(const uk::Ctx&) {}

void Action(const std::string&, const uk::Ctx&) {}

bool Busy() { return false; }

}
