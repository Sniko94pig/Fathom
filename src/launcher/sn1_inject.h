#pragma once
#include "launcher/inject.h"
#include <string>

namespace sn1inject {

inject::Paths Locate();
inject::GameState State(const inject::Paths& p);
std::string Inject(const inject::Paths& p, bool& ok, const std::string& openKey);
void StartGame();
std::string BackupSaves(const inject::Paths& p);

}
