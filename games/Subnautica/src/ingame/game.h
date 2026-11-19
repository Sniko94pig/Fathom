#pragma once
#include "core/model.h"
#include <mutex>
#include <string>
#include <vector>

namespace sn1game {

bool Prepare(std::string& err);
void Wire(core::Model& model, std::mutex& lock);
bool Start(std::string& err);
void RequestStop();
bool Stopped();
void ForceStop();
bool Ticking();
std::vector<std::string> TakeNotes();
void SetMenu(bool open, bool focused);
void SetGameWindow(void* hwnd);

}
