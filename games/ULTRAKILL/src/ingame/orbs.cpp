#include "ingame/groups.h"
#include <string>

namespace ukorbs {

bool Resolve() { return true; }

bool Supported(const std::string& id) { return id == "orb_esp" || id == "collect_all_orbs"; }

void Tick(const uk::Ctx&) {}

void Release(const uk::Ctx&) {}

void Action(const std::string&, const uk::Ctx&) {}

bool Busy() { return false; }

}
