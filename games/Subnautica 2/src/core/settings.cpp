#include "core/model.h"
#include "imgui.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstring>
#include <fstream>

namespace core {

using nlohmann::json;

const char* KeyName(int key) {
    if (key < ImGuiKey_NamedKey_BEGIN || key >= ImGuiKey_NamedKey_END) return "None";
    return ImGui::GetKeyName((ImGuiKey)key);
}

static int KeyFromName(const std::string& name) {
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
        if (name == ImGui::GetKeyName((ImGuiKey)k)) return k;
    return 0;
}

bool SaveItems(const std::vector<Item>& items, const std::string& path) {
    json arr = json::array();
    for (const Item& it : items) arr.push_back({{"id", it.id}, {"name", it.name}, {"category", it.category}, {"hidden", it.hidden}});
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << arr.dump(1);
    return out.good();
}

bool LoadItems(std::vector<Item>& items, const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    json j = json::parse(in, nullptr, false);
    if (!j.is_array()) return false;
    items.clear();
    for (const json& e : j)
        if (e.is_object())
            items.push_back({e.value("id", std::string()), e.value("name", std::string()), e.value("category", std::string()), e.value("hidden", false)});
    return true;
}

std::string SettingsText(const Model& m) {
    json j;
    j["version"] = 1;
    j["menu"] = {{"scale", m.menuScale},
                 {"opacity", m.menuOpacity},
                 {"blur", m.blur},
                 {"key_open", KeyName(m.keyOpen)},
                 {"key_unload", KeyName(m.keyUnload)}};
    json feats = json::object();
    for (const Feature& f : m.features) {
        json e;
        if (f.kind == Kind::Toggle) e["on"] = f.on;
        if (f.kind == Kind::Slider) e["value"] = f.value;
        if (f.kind == Kind::Input && std::any_of(f.inputs.begin(), f.inputs.end(), [](const std::string& s) { return !s.empty(); }))
            e["inputs"] = f.inputs;
        if (f.key) e["key"] = KeyName(f.key);
        if (!e.empty()) feats[f.id] = e;
    }
    j["features"] = feats;
    json locs = json::array();
    for (const Location& l : m.locations) locs.push_back({{"name", l.name}, {"x", l.x}, {"y", l.y}, {"z", l.z}});
    j["locations"] = locs;
    j["hub"] = {{"repo", m.hubRepo}, {"legs_to_seabed", m.legsToSeabed}};
    j["legit"] = {{"enabled", m.legit}};
    j["map"] = {{"show_picture", m.mapShowImage}, {"points", m.calibPoints}, {"u", {m.calibU[0], m.calibU[1]}}, {"v", {m.calibV[0], m.calibV[1]}}, {"x", {m.calibX[0], m.calibX[1]}}, {"y", {m.calibY[0], m.calibY[1]}}};
    if (!m.loadout.empty() || m.loadoutKey) {
        json items = json::array();
        for (const auto& e : m.loadout) items.push_back({{"id", e.first}, {"amount", e.second}});
        j["loadout"] = {{"key", KeyName(m.loadoutKey)}, {"items", items}};
    }
    return j.dump(2);
}

bool SaveSettings(const Model& m) {
    std::string text = SettingsText(m);
    std::string tmp = m.settingsPath + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << text;
        if (!out.good()) return false;
    }
    std::remove(m.settingsPath.c_str());
    return std::rename(tmp.c_str(), m.settingsPath.c_str()) == 0;
}

bool LoadSettings(Model& m) {
    std::ifstream in(m.settingsPath, std::ios::binary);
    if (!in) return false;
    json j = json::parse(in, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return false;

    if (j.contains("menu") && j["menu"].is_object()) {
        const json& u = j["menu"];
        m.menuScale = std::clamp(u.value("scale", m.menuScale), 0.8f, 1.4f);
        m.menuOpacity = std::clamp(u.value("opacity", m.menuOpacity), 0.5f, 1.0f);
        m.blur = u.value("blur", m.blur) && m.blurAvailable;
        int k = KeyFromName(u.value("key_open", std::string()));
        if (k) m.keyOpen = k;
        k = KeyFromName(u.value("key_unload", std::string()));
        if (k) m.keyUnload = k;
    }
    m.legit = false;
    if (j.contains("map") && j["map"].is_object()) {
        const json& mp = j["map"];
        m.mapShowImage = mp.value("show_picture", true);
        m.calibPoints = std::clamp(mp.value("points", 0), 0, 2);
        auto pair = [&](const char* k, float* out) {
            if (mp.contains(k) && mp[k].is_array() && mp[k].size() == 2)
                for (int i = 0; i < 2; ++i) out[i] = mp[k][i].get<float>();
        };
        pair("u", m.calibU);
        pair("v", m.calibV);
        pair("x", m.calibX);
        pair("y", m.calibY);
    }
    if (j.contains("hub") && j["hub"].is_object()) {
        m.hubRepo = j["hub"].value("repo", m.hubRepo);
        m.legsToSeabed = j["hub"].value("legs_to_seabed", m.legsToSeabed);
    }
    if (j.contains("locations") && j["locations"].is_array()) {
        m.locations.clear();
        for (const json& l : j["locations"]) {
            if (!l.is_object()) continue;
            m.locations.push_back({l.value("name", std::string("Location")), l.value("x", 0.0f), l.value("y", 0.0f),
                                   l.value("z", 0.0f)});
        }
    }
    if (j.contains("loadout") && j["loadout"].is_object()) {
        const json& lo = j["loadout"];
        m.loadoutKey = KeyFromName(lo.value("key", std::string()));
        m.loadout.clear();
        if (lo.contains("items") && lo["items"].is_array())
            for (const json& e : lo["items"]) {
                if (!e.is_object()) continue;
                std::string id = e.value("id", std::string());
                int amount = std::clamp(e.value("amount", 1), 1, 999);
                if (!id.empty()) m.loadout.push_back({id, amount});
            }
    }
    if (j.contains("features") && j["features"].is_object()) {
        for (Feature& f : m.features) {
            auto it = j["features"].find(f.id);
            if (it == j["features"].end() || !it->is_object()) continue;
            f.key = KeyFromName(it->value("key", std::string()));
            if (f.kind == Kind::Toggle && !f.counts) f.on = it->value("on", f.on);
            auto in = it->find("inputs");
            if (f.kind == Kind::Input && in != it->end() && in->is_array())
                for (size_t i = 0; i < f.inputs.size() && i < in->size(); ++i)
                    if ((*in)[i].is_string()) f.inputs[i] = (*in)[i].get<std::string>();
        }
    }
    return true;
}

void ResetSettings(Model& m) {
    DisableAll(m);
    for (Feature& f : m.features) {
        f.key = 0;
        if (!f.counts) f.on = true;
    }
    m.menuScale = 1.0f;
    m.menuOpacity = 0.78f;
    m.blur = false;
    m.legit = false;
    m.keyOpen = ImGuiKey_RightShift;
    m.keyUnload = ImGuiKey_End;
}

}
