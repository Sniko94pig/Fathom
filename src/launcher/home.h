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

struct MapLabel {
    std::string name;
    float u = 0, v = 0;
    bool area = true;
    float reach = 0;
};

struct MapPic {
    ImTextureID tex = 0;
    float w = 0, h = 0;
    std::vector<MapLabel> labels;
    std::string note;
    bool partial = false;
    bool loaded = false;
};

struct Home {
    inject::Paths paths;
    inject::GameState state = inject::GameState::NotRunning;
    inject::Paths sn1Paths;
    inject::GameState sn1State = inject::GameState::NotRunning;
    inject::Paths sotfPaths;
    inject::GameState sotfState = inject::GameState::NotRunning;
    inject::Paths ukPaths;
    inject::GameState ukState = inject::GameState::NotRunning;
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
    bool mcMenu = false;
    bool mcBusy = false;
    bool mcReady = false;
    bool mcNeedFabric = false;
    std::string mcVersion;
    std::string mcGame;
    std::string mcApi;
    std::string mcStep;
    std::wstring mcMods;
    std::wstring mcLog;
    bool maps = false;
    bool mapsWanted = false;
    bool mapsReload = false;
    bool mapsRedraw = false;
    bool mapsBusy = false;
    int mapGame = 0;
    MapPic mapPics[3];
    std::string mapStatus[3];
    float mapZoom[3] = {0, 0, 0};
    float mapCx[3] = {0.5f, 0.5f, 0.5f};
    float mapCy[3] = {0.5f, 0.5f, 0.5f};
    std::unordered_map<long long, Art> art;
    void Draw();

private:
    void DrawLibrary();
    void DrawGame();
    void DrawGameCards(float top, float w, ImVec2 o, const inject::Paths& paths, inject::GameState state, const char* folderName, const char* dllName);
    void DrawMinecraftCards(float top, float w, ImVec2 o);
    void DrawMaps();
    const Art* Find(int appId, int kind) const;
};

long long ArtKey(int appId, int kind);

}
