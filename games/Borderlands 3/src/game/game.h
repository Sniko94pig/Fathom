#pragma once
#include "core/model.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace game {

void Wire(core::Model& model, std::mutex& lock, unsigned long gameThread);
void SetFolder(const std::wstring& folder);
void SetBackup(std::function<std::string()> fn);
bool Hook();
bool Shutdown();
void Unhook();
std::vector<std::string> TakeNotes();
void SetMenuState(bool open, bool focused, bool mapVisible);

}
