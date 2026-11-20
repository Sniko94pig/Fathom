#pragma once
#include "imgui.h"
#include "launcher/inject.h"
#include <string>
#include <unordered_map>
#include <vector>

namespace launcher {

struct Art {
    ImTextureID tex = 0;
    float w = 0, h = 0;
};

struct McInstall {
    std::string name;
    std::wstring mods;
    bool night = false;
    bool fathom = false;
};

struct Home {
    inject::Paths paths;
    inject::GameState state = inject::GameState::NotRunning;
    inject::Paths sn1Paths;
    inject::GameState sn1State = inject::GameState::NotRunning;
    inject::Paths sotfPaths;
    inject::GameState sotfState = inject::GameState::NotRunning;
    bool busy = false;
    bool injectNow = false;
    int injectGame = 0;
    bool startNow = false;
    std::wstring logPath;
    std::string logTitle;
    int game = -1;
    std::string search;
    std::string openKey = "Right Shift";
    std::string unloadKey = "End";
    std::vector<bool> installed;
    std::vector<const char*> installedWith;
    bool showSoon = true;
    bool installedFirst = false;
    int featStart = 0;
    bool mine = false;
    bool mcOpen = false;
    bool mcLinked = false;
    bool mcWorld = false;
    std::string mcVersion;
    std::vector<McInstall> mcInstalls;
    std::wstring mcLog;
    std::unordered_map<long long, Art> art;
    void Draw();

private:
    void DrawLibrary();
    void DrawGame();
    void DrawGameCards(float top, float w, ImVec2 o, const inject::Paths& paths, inject::GameState state, const char* folderName, const char* dllName);
    void DrawMinecraftCards(float top, float w, ImVec2 o);
    const Art* Find(int appId, int kind) const;
};

long long ArtKey(int appId, int kind);

}
