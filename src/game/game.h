#pragma once
#include "core/model.h"
#include <mutex>
#include <string>
#include <vector>

namespace game {

void Wire(core::Model& model, std::mutex& lock, unsigned long gameThread);
void SetFolder(const std::wstring& folder);
bool Hook();
void Shutdown();
void Unhook();
std::vector<std::string> TakeNotes();
void SetMenuState(bool open, bool mapVisible, bool legitVisible);

}
