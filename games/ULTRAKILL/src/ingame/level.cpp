#include "ingame/groups.h"
#include <string>

namespace uklevel {

bool Resolve() { return true; }

bool Supported(const std::string& id) { return id == "kill_enemies_in_range" || id == "kill_range" || id == "challenge_shower"; }

void Tick(const uk::Ctx&) {}

void Release(const uk::Ctx&) {}

void Action(const std::string&, const uk::Ctx&) {}

bool Busy() { return false; }

bool Outside() { return true; }

}
