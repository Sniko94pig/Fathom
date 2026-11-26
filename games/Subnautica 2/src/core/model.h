#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace core {

enum class Tab { Survival, Biomods, Crafting, World, Spawner, BaseHub, Legit, Settings, Count };
enum class Kind { Toggle, Slider, Action, Input };
enum class Session { Solo, Host, Client };
enum class MarkerType { Player, Vehicle, Base, Wreck, DataBox, Beacon, Place };

struct Feature {
    std::string id, label, hint, group;
    Tab tab = Tab::Survival;
    Kind kind = Kind::Toggle;
    float min = 0, max = 1, def = 0, step = 0.1f;
    float caps[4] = {0, 0, 0, 0};
    std::string fmt = "%.1fx";
    std::string actionLabel;
    std::string page;
    std::vector<std::string> inputs;
    std::vector<std::string> inputHints;
    std::vector<float> inputSizes;
    std::string result;
    bool confirm = false;
    bool counts = true;
    bool available = false;
    bool building = false;
    std::string missing = "Not found in the game's SDK";
    bool on = false;
    float value = 0;
    int key = 0;
    std::function<void(Feature&)> apply;
};

struct Item {
    std::string id, name, category;
    bool hidden = false;
    bool real = true;
};

struct MapWords {
    std::array<std::string, 4> chips;
    std::string empty = "The map fills in as you explore";
    std::string photos = "Pictures of the seabed are taken while this menu is open";
    std::string photosMore = "more are added while this menu is open";
    std::string scanButton = "Scan whole map";
    std::string scanTitle = "Scan the whole map?";
    std::string scanBody = "A camera flies over the whole world and takes a picture of every part. Your character is frozen and can't be hurt "
                           "while it runs. It takes about 10 to 30 minutes, and you can stop it at any time.";
};

struct Marker {
    MarkerType type = MarkerType::Wreck;
    std::string name;
    float x = 0, y = 0, z = 0, yaw = 0;
};

struct EspDot {
    double x = 0, y = 0, z = 0;
    double cx = 0, cy = 0, cz = 0;
    double ex = 0, ey = 0, ez = 0;
    double top = 0;
    bool box = false;
    std::string name;
    float dist = 0;
    float health = -1;
    int kind = 0;
};

struct CameraView {
    double x = 0, y = 0, z = 0;
    double pitch = 0, yaw = 0, roll = 0;
    float fov = 90;
    double focal = 0;
    int width = 0, height = 0;
    bool valid = false;
};

struct Adaptation {
    std::string id, name, hint;
    bool on = false;
};

struct ProgressEntry {
    std::string id, name, detail, icon;
    bool done = false;
    bool hasPos = false;
    float x = 0, y = 0, z = 0;
    int order = 0;
};

struct ProgressStat {
    std::string name;
    int done = 0, total = 0;
};

struct Location {
    std::string name;
    float x = 0, y = 0, z = 0;
};

struct BaseInfo {
    std::string name, author, file, date;
    int pieces = 0;
    bool downloaded = false;
};

struct TierStep {
    std::string label;
    bool done = false;
};

struct TabLabel {
    Tab tab = Tab::Survival;
    std::string name;
    const char* icon = nullptr;
};

struct Tier {
    std::string name, blurb, caps;
    std::vector<TierStep> steps;
};

struct Model {
    std::vector<Feature> features;
    std::vector<Tab> tabs;
    std::vector<Tab> plainTabs;
    std::vector<TabLabel> tabLabels;
    Session session = Session::Solo;
    bool guestCheats = false;
    bool inWorld = false;

    std::vector<Item> items;
    std::vector<std::string> categories{"Resources", "Food", "Healing", "Tools", "Equipment", "Creatures", "Plants", "Other"};
    uint64_t iconTexture = 0;
    uint64_t logoTexture = 0;
    std::unordered_map<std::string, std::array<float, 4>> icons;
    std::function<void(const Item&, int)> spawn;
    std::function<void()> addAllResources;
    std::function<void()> duplicateHeld;

    std::vector<Item> worldItems;
    std::vector<std::string> worldCategories;
    std::function<void(const Item&, int)> spawnWorld;
    std::vector<Item> creatureItems;
    std::vector<std::string> creatureCategories;
    std::function<void(const Item&, int)> spawnCreature;
    std::vector<std::pair<std::string, int>> loadout;
    int loadoutKey = 0;
    std::function<void()> giveLoadout;

    std::vector<Adaptation> adaptations;
    std::function<void(int, bool)> setAdaptation;

    std::vector<Marker> markers;
    std::vector<EspDot> esp;
    std::vector<EspDot> itemEsp;
    CameraView camera;
    uint64_t mapImageTexture = 0;
    int mapImageW = 0, mapImageH = 0;
    int calibPoints = 0;
    float calibU[2] = {0, 0}, calibV[2] = {0, 0}, calibX[2] = {0, 0}, calibY[2] = {0, 0};
    bool mapShowImage = true;
    std::function<void()> openFolder;
    uint64_t mapTexture = 0;
    float mapMinX = 0, mapMinY = 0, mapMaxX = 0, mapMaxY = 0;
    int mapPhotos = 0;
    std::function<void(bool)> mapScan;
    bool mapScanning = false;
    float mapScanProgress = 0;
    std::string mapScanText;
    MapWords mapWords;
    bool duplicateButton = true;
    std::vector<Location> locations;
    std::vector<Location> places;
    bool mapDrawn = false;
    std::function<void(float, float, float, bool)> teleport;
    std::function<void()> undoTeleport;
    bool canUndoTeleport = false;

    std::vector<BaseInfo> localBases, sharedBases;
    std::string hubRepo = "https://raw.githubusercontent.com/your-name/fathom-bases/main/index.json";
    bool legsToSeabed = true;
    bool ghostActive = false;
    std::string ghostName;
    std::function<void(const std::string&)> saveBase;
    std::function<void(const BaseInfo&)> previewBase, placeBase, deleteBase, downloadBase;
    std::function<void()> confirmGhost, cancelGhost, refreshShared;
    std::function<void(const std::string&)> openUrl;

    std::vector<ProgressStat> progressStats;
    std::vector<ProgressEntry> progress[4];
    std::function<void(int, const ProgressEntry&)> progressAction;
    bool progressLoaded = false;
    std::string progressStage;

    bool legit = false;
    int legitTier = 0;
    std::vector<Tier> tiers;
    std::function<void(int)> advanceTier;

    float menuScale = 1.0f;
    float menuOpacity = 0.78f;
    bool blur = false;
    bool blurAvailable = false;
    int keyOpen = 0;
    int keyUnload = 0;
    std::function<std::string()> backupSaves;
    std::string lastBackup;
    std::string settingsPath = "fathom_settings.json";
    std::string gameName = "Subnautica 2";
    bool requestUnload = false;
    bool menuOpen = false;
    bool mapVisible = false;
    bool legitVisible = false;

    Feature* find(const std::string& id);
    int activeCount() const;
    bool cheatsAllowed() const { return session != Session::Client || guestCheats; }
    float sliderMax(const Feature& f) const;
    const Marker* player() const;
};

void BuildFeatures(Model& m);
void SetFeature(Model& m, Feature& f, bool on);
void SetValue(Model& m, Feature& f, float v);
void DisableAll(Model& m);

bool Implemented(const std::string& id);
const char* NotYetReason(const std::string& id);

bool SaveItems(const std::vector<Item>& items, const std::string& path);
bool LoadItems(std::vector<Item>& items, const std::string& path);

std::string SettingsText(const Model& m);
bool SaveSettings(const Model& m);
bool LoadSettings(Model& m);
void ResetSettings(Model& m);
const char* KeyName(int key);

}
