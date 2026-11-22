#pragma once
#include "core/model.h"
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ukgame {

bool Prepare(std::string& err);
void Wire(core::Model& model, std::mutex& lock);
bool Start(std::string& err);
void RequestStop();
bool Stopped();
bool SafeToUnload();
bool HoldsHooks();
bool Ticking();
std::vector<std::string> TakeNotes();
void SetMenu(bool open, bool focused);
void SetGameWindow(void* hwnd);
void SetFolder(const std::wstring& dir);
void SetBackup(std::function<std::string()> backup);
void SetLastBackup(const std::string& path);
void Note(const std::string& text);

}
