#pragma once
#include "core/model.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace game {

void Wire(core::Model& model, std::mutex& lock);
void SetFolder(const std::wstring& folder);
void SetBackup(std::function<std::string()> fn);
bool Attach(void* module);
bool Shutdown();
void Detach(void* module);
bool WantsUnload(std::string& why);
std::vector<std::string> TakeNotes();
void SetMenuState(bool open, bool focused, bool mapVisible);

}
