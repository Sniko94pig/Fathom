#pragma once
#include "core/model.h"
#include "ingame/mono.h"
#include <functional>
#include <mutex>
#include <string>

namespace sn1spawn {

void Wire(core::Model& model, std::mutex& lock, const std::wstring& dir, std::function<void(const std::wstring&)> loadIcons);
void Tick(mono::Object* player, bool inGame, double now, bool mapVisible);
void Release();

}
