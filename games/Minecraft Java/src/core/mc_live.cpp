#include "core/mc_features.h"
#include <cctype>
#include <cstdio>
#include <cstring>

namespace mc {

using nlohmann::json;

namespace {

std::string Str(const json& o, const char* key) {
    auto it = o.find(key);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}

bool Bool(const json& o, const char* key, bool def) {
    auto it = o.find(key);
    return it != o.end() && it->is_boolean() ? it->get<bool>() : def;
}

long long Int(const json& o, const char* key, long long def) {
    auto it = o.find(key);
    return it != o.end() && it->is_number() ? it->get<long long>() : def;
}

std::string Goal(const std::string& goal) {
    auto num = [&](const char* key, std::string& out) {
        size_t at = goal.find(key);
        if (at == std::string::npos) return false;
        size_t s = at + std::strlen(key), e = s;
        while (e < goal.size() && (std::isdigit((unsigned char)goal[e]) || goal[e] == '-')) ++e;
        out = goal.substr(s, e - s);
        return e > s;
    };
    std::string x, y, z;
    bool hx = num("x=", x), hy = num("y=", y), hz = num("z=", z);
    if (hx && hz) return hy ? x + " " + y + " " + z : x + " " + z;
    if (hy) return "height " + y;
    return goal;
}

}

bool ParseLive(const std::string& text, Live& out) {
    out = Live();
    json j = json::parse(text, nullptr, false);
    if (!j.is_object()) return false;
    out.session = Str(j, "session");
    out.ack = Int(j, "ack", 0);
    out.mc = Str(j, "mc");
    out.client = Str(j, "client");
    out.night = Bool(j, "night", false);
    out.nightOn = Bool(j, "nightOn", true);
    out.world = Bool(j, "world", false);
    out.baritone = Bool(j, "baritone", false);
    out.pathing = Bool(j, "pathing", false);
    out.menu = Bool(j, "menu", false);
    out.task = Str(j, "task");
    out.goal = Str(j, "goal");
    out.eta = Int(j, "eta", -1);
    auto hacks = j.find("hacks");
    if (hacks != j.end() && hacks->is_object())
        for (const auto& e : hacks->items())
            if (e.value().is_boolean()) out.hacks[e.key()] = e.value().get<bool>();
    auto settings = j.find("settings");
    if (settings != j.end() && settings->is_object())
        for (const auto& e : settings->items()) {
            if (e.value().is_boolean()) out.flags[e.key()] = e.value().get<bool>();
            else if (e.value().is_number()) out.numbers[e.key()] = e.value().get<double>();
        }
    auto pos = j.find("pos");
    if (pos != j.end() && pos->is_array() && pos->size() == 3 && (*pos)[0].is_number() && (*pos)[1].is_number() && (*pos)[2].is_number()) {
        out.hasPos = true;
        for (int i = 0; i < 3; ++i) out.pos[i] = (*pos)[i].get<int>();
    }
    return true;
}

json Command(core::Feature& f) {
    if (IsHack(f)) return {{"op", "hack"}, {"name", f.id}, {"on", f.on}};
    if (const char* name = BaritoneSetting(f)) {
        if (f.kind == core::Kind::Slider) return json();
        return {{"op", "bset"}, {"name", name}, {"value", f.on ? "true" : "false"}};
    }
    std::string error;
    std::string cmd = BaritoneCommand(f, error);
    if (cmd.empty()) {
        f.result = error;
        return json();
    }
    f.result = "Sent to Baritone: " + cmd;
    return {{"op", "baritone"}, {"text", cmd}};
}

json SliderCommand(const core::Feature& f) {
    const char* name = BaritoneSetting(f);
    if (!name) return json();
    char buf[32];
    std::snprintf(buf, sizeof(buf), f.step >= 1.0f ? "%.0f" : "%.2f", f.value);
    return {{"op", "bset"}, {"name", name}, {"value", buf}};
}

void ApplyLive(core::Model& m, bool linked, const Live& st, const std::function<bool(const std::string&)>& hold, const std::string& gotoHint) {
    const char* offline = "Start Minecraft from Fathom to use this";
    for (core::Feature& f : m.features) {
        if (IsHack(f)) {
            auto it = st.hacks.find(f.id);
            bool exists = linked && it != st.hacks.end();
            f.available = exists && st.world && st.nightOn;
            f.missing = !linked ? offline
                      : !st.night ? "The cheats are missing here, play the Fathom install"
                      : !exists ? "Not in this Minecraft version"
                      : !st.nightOn ? "The cheats are switched off in the game"
                                    : "Join a world first";
            if (!linked) f.on = false;
            else if (exists && !hold(f.id)) f.on = it->second;
        } else if (const char* name = BaritoneSetting(f)) {
            f.available = linked && st.baritone;
            f.missing = !linked ? offline : "Baritone is not in this Minecraft install";
            if (linked && !hold(f.id)) {
                if (f.kind == core::Kind::Toggle) {
                    auto b = st.flags.find(name);
                    if (b != st.flags.end()) f.on = b->second;
                } else {
                    auto v = st.numbers.find(name);
                    if (v != st.numbers.end()) f.value = (float)v->second;
                }
            }
        } else {
            f.available = linked && st.baritone && st.world;
            f.missing = !linked ? offline : !st.baritone ? "Baritone is not in this Minecraft install" : "Join a world first";
        }
    }
    if (core::Feature* f = m.find("b_stop")) f->hint = TaskText(linked, st);
    if (core::Feature* f = m.find("b_goto"))
        f->hint = linked && st.hasPos ? "You are at " + std::to_string(st.pos[0]) + " " + std::to_string(st.pos[1]) + " " + std::to_string(st.pos[2])
                                      : gotoHint;
    m.inWorld = linked && st.world;
}

std::string TaskText(bool linked, const Live& st) {
    if (!linked) return "Start Minecraft from Fathom to use Baritone";
    if (!st.baritone) return "Baritone is not in this Minecraft install";
    std::string task = st.task;
    if (task.empty() || task == "INACTIVE")
        task = st.pathing && !st.goal.empty() ? "Walking to " + Goal(st.goal) : "Baritone is idle";
    else if (task.rfind("Custom Goal", 0) == 0)
        task = "Walking to " + Goal(st.goal.empty() ? task : st.goal);
    if (st.eta > 0 && task != "Baritone is idle") task += ", about " + std::to_string((st.eta + 19) / 20) + " s left";
    return task;
}

}
