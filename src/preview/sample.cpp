#include "preview/sample.h"
#include "ui/ui.h"
#include <cctype>
#include <cmath>

namespace preview {

using core::MarkerType;

void Fill(core::Model& m) {
    core::BuildFeatures(m);
    for (core::Feature& f : m.features) f.available = true;
    m.inWorld = true;
    m.session = core::Session::Solo;
    m.settingsPath = "fathom_preview_settings.json";

    struct Row {
        const char* cat;
        const char* names;
    };
    static const Row rows[] = {
        {"Resources", "Titanium|Copper Ore|Quartz|Silver Ore|Lead|Gold|Lithium|Salt Deposit|Diamond|Ruby|Magnetite|Nickel Ore|Sulfur|Kyanite"},
        {"Food", "Cooked Fish|Cured Fish|Nutrient Block|Filtered Water|Large Water Bottle|Kelp Wrap"},
        {"Healing", "First Aid Kit|Bandage|Antitoxin"},
        {"Tools", "Scanner|Repair Tool|Survival Knife|Flashlight|Habitat Builder|Biosampler|Light Stick|Beacon"},
        {"Equipment", "Standard Tank|High Capacity Tank|Fins|Rebreather|Compass|Radiation Suit|Thermal Suit"},
        {"Creatures", "Small Reef Fish|Glow Ray|Shell Crab|Reef Stalker|Trench Leviathan"},
        {"Plants", "Kelp Seed|Glow Bulb|Coral Sample|Sea Grass|Vent Flower"},
    };
    m.items.clear();
    for (const Row& r : rows) {
        std::string s = r.names;
        size_t a = 0;
        while (a <= s.size()) {
            size_t b = s.find('|', a);
            if (b == std::string::npos) b = s.size();
            std::string name = s.substr(a, b - a);
            std::string id = name;
            for (char& c : id) c = c == ' ' ? '_' : (char)std::tolower((unsigned char)c);
            m.items.push_back({"sample_" + id, name, r.cat});
            a = b + 1;
        }
    }

    m.markers = {
        {MarkerType::Player, "You", 40, -30, -18, -0.6f},
        {MarkerType::Vehicle, "Tadpole", 120, 60, -22, 0},
        {MarkerType::Base, "Home base", -180, 140, -35, 0},
        {MarkerType::Base, "Outpost", 520, -260, -120, 0},
        {MarkerType::Wreck, "Wreck A", -420, -310, -90, 0},
        {MarkerType::Wreck, "Wreck B", 300, 380, -160, 0},
        {MarkerType::Wreck, "Wreck C", 760, 120, -240, 0},
        {MarkerType::DataBox, "Data box", -90, -220, -60, 0},
        {MarkerType::DataBox, "Data box", 410, 90, -140, 0},
        {MarkerType::DataBox, "Data box", -520, 220, -200, 0},
    };
    m.locations = {{"Home base", -180, 140, -35}, {"Thermal vent", 640, -420, -310}, {"Kelp forest", -60, 300, -40}};

    m.localBases = {{"Shallows starter", "", "", "12 Nov 2026", 34, false}, {"Deep lab", "", "", "9 Nov 2026", 87, false}};
    m.sharedBases = {{"Compact scanner outpost", "example-user", "", "", 21, false},
                     {"Moonpool hub", "example-user", "", "", 64, true},
                     {"Thermal plant", "another-user", "", "", 112, false}};

    m.tiers = {
        {"Shallows", "First tools, first shelter, safe water.", "Speed 1.2x  \xc2\xb7  Jump 1.0x  \xc2\xb7  Vehicle 1.0x",
         {{"Sample step: craft the scanner", true}, {"Sample step: scan 5 fragments", true}, {"Sample step: build a first base", true}}},
        {"Biosampler / Tadpole", "Biology unlocks and your first vehicle.", "Speed 1.5x  \xc2\xb7  Jump 1.2x  \xc2\xb7  Vehicle 1.3x",
         {{"Sample step: build the biosampler", true}, {"Sample step: sample 3 species", false}, {"Sample step: build the Tadpole", false}}},
        {"Deep Thermal", "Heat, pressure and deeper biomes.", "Speed 2.0x  \xc2\xb7  Jump 1.5x  \xc2\xb7  Vehicle 1.6x",
         {{"Sample step: thermal protection", false}, {"Sample step: reach the vents", false}, {"Sample step: power a deep base", false}}},
        {"Endgame", "Finish the story with everything unlocked.", "Speed 2.5x  \xc2\xb7  Jump 2.0x  \xc2\xb7  Vehicle 2.0x",
         {{"Sample step: final story event", false}, {"Sample step: last blueprint set", false}, {"Sample step: complete the run", false}}},
    };
    m.legitTier = 1;

    static float prev[3];
    m.spawn = [](const core::Item&, int) {};
    m.addAllResources = [] {};
    m.duplicateHeld = [] {};
    m.teleport = [&m](float x, float y, float z, bool useZ) {
        for (core::Marker& k : m.markers) {
            if (k.type != MarkerType::Player) continue;
            prev[0] = k.x;
            prev[1] = k.y;
            prev[2] = k.z;
            k.x = x;
            k.y = y;
            if (useZ) k.z = z;
            m.canUndoTeleport = true;
        }
    };
    m.mapScan = [&m](bool on) {
        m.mapScanning = on;
        m.mapScanProgress = on ? 0.35f : 0.0f;
        m.mapScanText = on ? "Piece 284 of 812  \xc2\xb7  about 14 min left" : "";
    };
    m.undoTeleport = [&m] {
        for (core::Marker& k : m.markers) {
            if (k.type != MarkerType::Player) continue;
            k.x = prev[0];
            k.y = prev[1];
            k.z = prev[2];
        }
        m.canUndoTeleport = false;
    };
    m.saveBase = [&m](const std::string& name) { m.localBases.insert(m.localBases.begin(), {name, "", "", "today", 40, false}); };
    m.previewBase = [&m](const core::BaseInfo& b) {
        m.ghostActive = true;
        m.ghostName = b.name;
    };
    m.placeBase = [](const core::BaseInfo&) {};
    m.deleteBase = [&m](const core::BaseInfo& b) {
        for (size_t i = 0; i < m.localBases.size(); ++i)
            if (m.localBases[i].name == b.name) {
                m.localBases.erase(m.localBases.begin() + i);
                break;
            }
    };
    m.downloadBase = [&m](const core::BaseInfo& b) {
        for (core::BaseInfo& s : m.sharedBases)
            if (s.name == b.name) s.downloaded = true;
    };
    m.confirmGhost = [&m] { m.ghostActive = false; };
    m.cancelGhost = [&m] { m.ghostActive = false; };
    m.refreshShared = [] {};
    m.advanceTier = [&m](int i) {
        core::Tier& t = m.tiers[i];
        for (core::TierStep& s : t.steps)
            if (!s.done) {
                s.done = true;
                return;
            }
        if (m.legitTier < (int)m.tiers.size() - 1) ++m.legitTier;
    };
    m.backupSaves = [] { return std::string("preview only, no files touched"); };
}

void Backdrop() {
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImVec2 a = vp->Pos, s = vp->Size;
    ImVec2 q(a.x + s.x, a.y + s.y);
    ImU32 top = IM_COL32(24, 104, 132, 255), topR = IM_COL32(16, 84, 120, 255);
    ImU32 bot = IM_COL32(4, 22, 36, 255), botR = IM_COL32(3, 16, 30, 255);
    dl->AddRectFilledMultiColor(a, q, top, topR, botR, bot);
    for (int i = 0; i < 6; ++i) {
        float x = a.x + s.x * (0.08f + 0.16f * i);
        float w = s.x * (0.035f + 0.012f * (i % 3));
        ImVec2 pts[4] = {ImVec2(x, a.y), ImVec2(x + w, a.y), ImVec2(x + w * 2.2f - s.x * 0.10f, q.y), ImVec2(x - w * 1.2f - s.x * 0.10f, q.y)};
        dl->AddConvexPolyFilled(pts, 4, IM_COL32(190, 240, 255, 9 + (i % 2) * 5));
    }
    for (int i = 0; i < 9; ++i) {
        float fx = std::fmod(0.13f + i * 0.217f, 1.0f), fy = std::fmod(0.21f + i * 0.371f, 1.0f);
        float r = s.y * (0.05f + 0.035f * (i % 4));
        dl->AddCircleFilled(ImVec2(a.x + s.x * fx, a.y + s.y * fy), r, IM_COL32(63, 216, 208, 10), 48);
        dl->AddCircleFilled(ImVec2(a.x + s.x * fx, a.y + s.y * fy), r * 0.55f, IM_COL32(120, 235, 230, 10), 48);
    }
    dl->PathLineTo(ImVec2(a.x, q.y));
    for (int i = 0; i <= 24; ++i) {
        float t = i / 24.0f;
        float h = s.y * (0.12f + 0.05f * std::sin(t * 9.0f) + 0.035f * std::sin(t * 23.0f + 1.0f));
        dl->PathLineTo(ImVec2(a.x + s.x * t, q.y - h));
    }
    dl->PathLineTo(ImVec2(q.x, q.y));
    dl->PathFillConcave(IM_COL32(2, 12, 20, 255));
    ui::Text(dl, ui::T().regular, 11, ImVec2(a.x + ui::S(16), q.y - ui::S(24)), IM_COL32(255, 255, 255, 110),
             "Design preview  \xc2\xb7  sample data, not the real game");
}

}
