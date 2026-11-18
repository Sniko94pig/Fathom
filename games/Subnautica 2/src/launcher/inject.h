#pragma once
#include <string>

namespace inject {

struct Paths {
    std::wstring win64;
    std::wstring folder;
    std::wstring dll;
    std::wstring run;
    std::wstring settings;
    std::wstring log;
    std::wstring bundled;
    bool found = false;
};

enum class GameState { NotRunning, Running, Injected };

Paths Locate();
GameState State(const Paths& p);
std::string Inject(const Paths& p, bool& ok);
void StartGame();
std::string BackupSaves();

}
