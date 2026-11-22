#pragma once
#include "core/model.h"
#include <mutex>
#include <string>

namespace ui {
class Menu;
}

namespace game {

void Wire(core::Model& model, std::mutex& lock);
bool Start(std::string& err);
void BeforeMenu(ui::Menu& menu);
void AfterMenu();
void RequestStop();
bool Stopped();
bool Ticking();
bool Holding();
void SavePrefs(bool force);

}
