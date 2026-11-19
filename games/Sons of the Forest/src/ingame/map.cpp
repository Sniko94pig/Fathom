#include "ingame/cheats.h"

namespace sotf {

namespace {

bool Resolve(std::string&) { return true; }

bool Supported(const std::string&) { return false; }

void Tick(const Ctx&) {}

bool Action(const std::string&, const Ctx&) { return false; }

void Release(const Ctx&) {}

}

const Group& MapGroup() {
    static Group g{"Map", &Resolve, &Supported, &Tick, &Action, &Release, nullptr};
    return g;
}

}
