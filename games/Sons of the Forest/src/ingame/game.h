#pragma once
#include "core/model.h"
#include <mutex>
#include <string>
#include <vector>

namespace sotfgame {

bool Prepare(std::string& err);
void Wire(core::Model& model, std::mutex& lock);
bool Start(std::string& err);
void RequestStop();
bool Stopped();
bool SafeToUnload();
bool Ticking();
bool Idle();
std::vector<std::string> TakeNotes();
void SetMenu(bool open, bool focused);
void SetGameWindow(void* hwnd);
void SetFolder(const std::wstring& dir);

}
