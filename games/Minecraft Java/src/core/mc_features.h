#pragma once
#include "core/model.h"
#include <string>

namespace mc {

void BuildFeatures(core::Model& m);
bool IsHack(const core::Feature& f);
const char* BaritoneSetting(const core::Feature& f);
std::string BaritoneCommand(const core::Feature& f, std::string& error);

}
