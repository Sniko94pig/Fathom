#pragma once
#include "core/model.h"
#include "ui/loader.h"
#include "ui/ui.h"
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace ui {

class Menu {
public:
    explicit Menu(core::Model& model);
    void Frame();
    void Notify(const std::string& text, const char* icon = nullptr);
    void Ask(const std::string& title, const std::string& body, const std::string& yes, std::function<void()> fn);
    void SyncFromModel();
    bool Capturing() const { return capture != nullptr; }

    bool open = true;
    bool focused = true;
    core::Tab tab = core::Tab::Survival;
    int spawnSub = 0;
    int hubSub = 0;
    std::string page;
    ImVec2 windowPos{0, 0};
    ImVec2 windowSize{0, 0};
    float openAnim = 0.0f;

    std::function<void()> homePage;
    const char* homeName = "Home";
    const char* homeIcon = nullptr;
    bool onHome = false;
    bool embedded = false;
    bool showGameTabs = true;
    const char* backName = nullptr;
    std::function<void()> onBack;
    std::string status;
    bool statusGood = false;
    Loader* loader = nullptr;
    float uiScale = 0.0f;

private:
    struct ToastMsg {
        std::string text;
        const char* icon;
        float age;
        float y;
        bool placed;
    };
    struct Masonry {
        ImVec2 origin;
        float colW, gap;
        float y[2];
        ImVec2 Place(float h);
        void End();
    };
    struct BrowserSpec {
        const std::vector<core::Item>* items = nullptr;
        const std::vector<std::string>* categories = nullptr;
        const char* key = "";
        const char* hiddenChip = nullptr;
        int maxAmount = 999;
        const char* actionLabel = "Spawn";
        const char* actionIcon = nullptr;
        const char* doneVerb = "Spawned";
        std::function<void(const core::Item&, int)> action;
        bool guarded = false;
        bool addAll = false;
        bool duplicate = false;
    };

    void HandleKeys();
    void Unfocus();
    void Sidebar(ImVec2 p, ImVec2 size);
    void TopBar(ImVec2 p, ImVec2 size);
    void Content(ImVec2 p, ImVec2 size);
    void Banner();
    void Confirm();
    void Toasts();

    Masonry BeginMasonry();
    float CardBegin(ImVec2 pos, float w, float h, const char* title);
    bool RowBase(const char* id, ImVec2 pos, float w, float h, const char* label, const char* hint, bool enabled,
                 const char* tip, float reserveRight);
    bool ToggleRow(const char* id, ImVec2 pos, float w, const char* label, const char* hint, bool* v, bool enabled,
                   const char* tip = nullptr, bool locked = false);
    bool SliderRow(const char* id, ImVec2 pos, float w, const char* label, float* v, float mn, float mx, float def,
                   float step, const char* fmt, float fmtMul, bool enabled, float cap = 0.0f, bool* active = nullptr);
    bool KeyChip(const char* id, ImVec2 rightCenter, int* key);
    float FeatureRowHeight(const core::Feature& f) const;
    void FeatureRow(core::Feature& f, ImVec2 pos, float w);
    void FeaturePage(core::Tab t);

    void SpawnerPage();
    void SpawnerView(ImVec2 b, float W, float H);
    void Browser(ImVec2 b, float W, float H, const BrowserSpec& spec);
    void SubPage();
    void LoadoutView(ImVec2 b, float W, float H);
    void GuardedSpawn(std::function<void()> fn);
    void MapView(ImVec2 b, float W, float H);
    void HubPage();
    void LegitPage();
    void SettingsPage();
    void LoaderPage();
    void Teleport(float x, float y, float z, bool useZ, const std::string& name);
    void RunPermanent(core::Feature& f);
    void WithBackup(std::function<void()> then);
    void PumpBackup();
    void Toggle(core::Feature& f, bool on);
    std::future<std::string> backupJob;
    std::function<void()> afterBackup;

    core::Model& m;
    std::vector<ToastMsg> toasts;

    bool confirmOpen = false;
    float confirmAnim = 0.0f;
    std::string confirmTitle, confirmBody, confirmYes;
    std::function<void()> confirmFn;

    int* capture = nullptr;
    int captureFrame = -1;
    int skipKeysFrame = -1;

    float fpsShown = 0.0f;
    float fpsTimer = 1.0f;
    int lastTab = -2;

    std::string search;
    std::string category = "All";
    int selected = -1;
    int amount = 1;

    float mapCx = 0, mapCy = 0, mapZoom = 0;
    bool mapInit = false;
    bool mapDragged = false;
    bool calibPick = false;
    bool showHidden = false;
    std::string browserKey;
    bool sessionBackup = false;
    int loadoutSub = 0;
    int mapListSub = 0;
    int progSub = 0;
    std::string progSearch;
    bool progHideDone = false;
    bool showType[7] = {true, true, true, true, true, true, true};
    std::string locName;
    std::string baseName;
    float scaleEdit = 1.0f;
    float appScaleEdit = -1.0f;
};

}
