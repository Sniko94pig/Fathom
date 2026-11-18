#pragma once
#include "imgui.h"
#include <functional>
#include <string>

namespace ui {

struct LoaderSettings {
    bool startWithWindows = false;
    bool closeAfterInject = false;
    bool autoInject = false;
    bool onTop = false;
    float scale = 1.0f;
    int accent = 0;
    bool showSoon = true;
    bool installedFirst = false;
    bool webArt = true;
    bool checkUpdates = true;
};

struct Loader {
    LoaderSettings s;
    std::string version;
    std::string updateText;
    bool updateBusy = false;
    bool updateReady = false;
    bool gameFound = false;
    std::string lastBackup;
    std::function<void()> checkUpdates;
    std::function<void()> installUpdate;
    std::function<void()> refreshArt;
    std::function<void()> openHome;
    std::function<void()> openBackups;
    std::function<void()> openGame;
    std::function<std::string()> backupSaves;
};

int AccentCount();
const char* AccentName(int i);
ImVec4 AccentColor(int i);

}
