#pragma once
#include "imgui.h"
#include <functional>
#include <string>
#include <vector>

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
    bool backupOnInject = true;
};

struct BackupEntry {
    std::wstring path;
    std::string game;
    std::string when;
    std::string note;
    std::string size;
    long long sortKey = 0;
    unsigned long long bytes = 0;
    bool picked = false;
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
    std::vector<BackupEntry> backups;
    double backupsTime = -100.0;
    std::function<std::vector<BackupEntry>()> listBackups;
    std::function<int(const std::vector<std::wstring>&)> recycleBackups;
    int oldBuilds = 0;
    std::string oldBuildsSize;
    double oldBuildsTime = -100.0;
    std::function<int(std::string&)> scanOldBuilds;
    std::function<int()> deleteOldBuilds;
};

int AccentCount();
const char* AccentName(int i);
ImVec4 AccentColor(int i);

}
