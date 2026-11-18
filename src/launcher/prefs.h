#pragma once
#include "ui/loader.h"
#include <string>

namespace launcher {

bool LoadPrefs(ui::LoaderSettings& s, const std::wstring& file);
bool SavePrefs(const ui::LoaderSettings& s, const std::wstring& file);
std::string PrefsText(const ui::LoaderSettings& s);
bool StartupEnabled();
bool SetStartup(bool on);

namespace update {

struct Status {
    bool busy = false;
    bool checked = false;
    bool newer = false;
    bool failed = false;
    bool installerReady = false;
    std::string latest;
    std::wstring installer;
};

void Check();
void Download();
Status Get();
void Wait();
bool Newer(const std::string& latest, const std::string& current);

}

}
