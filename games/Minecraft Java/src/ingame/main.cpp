#include "core/mc_features.h"
#include "core/model.h"
#include "ingame/log.h"
#include "ingame/overlay.h"
#include "ui/icons.h"
#include "ui/menu.h"
#include "ui/ui.h"
#include "imgui.h"
#include "MinHook.h"
#include <jni.h>
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <memory>
#include <string>
#include <unordered_map>

namespace {

JavaVM* vm = nullptr;
jclass bridge = nullptr;
jmethodID midState = nullptr;
jmethodID midCommand = nullptr;
jmethodID midRelease = nullptr;
jmethodID midGrab = nullptr;
jmethodID midGrabbed = nullptr;
std::wstring dir;
core::Model model;
std::unique_ptr<ui::Menu> menu;
std::string gotoHint;
std::string lastText;
std::unordered_map<std::string, ULONGLONG> pending;
std::unordered_map<std::string, ULONGLONG> sliders;
ULONGLONG lastState = 0, lastSave = 0;
bool greeted = false;
bool grabbedBefore = false;
bool unloaded = false;

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

JNIEnv* Env() {
    JNIEnv* env = nullptr;
    if (!vm || !bridge || vm->GetEnv((void**)&env, JNI_VERSION_1_8) != JNI_OK) return nullptr;
    return env;
}

bool Clear(JNIEnv* env) {
    if (!env->ExceptionCheck()) return false;
    env->ExceptionClear();
    return true;
}

std::string JavaState() {
    JNIEnv* env = Env();
    if (!env) return {};
    jstring s = (jstring)env->CallStaticObjectMethod(bridge, midState);
    if (Clear(env) || !s) return {};
    const char* c = env->GetStringUTFChars(s, nullptr);
    std::string out = c ? c : "";
    if (c) env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
    return out;
}

void Send(const nlohmann::json& cmd) {
    JNIEnv* env = Env();
    if (!env) return;
    std::string text = cmd.dump(-1, ' ', true);
    jstring s = env->NewStringUTF(text.c_str());
    if (!s) {
        Clear(env);
        return;
    }
    env->CallStaticVoidMethod(bridge, midCommand, s);
    Clear(env);
    env->DeleteLocalRef(s);
}

bool GameGrabbed() {
    JNIEnv* env = Env();
    if (!env) return true;
    jboolean b = env->CallStaticBooleanMethod(bridge, midGrabbed);
    if (Clear(env)) return true;
    return b == JNI_TRUE;
}

void Capture(bool want) {
    JNIEnv* env = Env();
    if (!env) return;
    if (want) {
        jboolean b = env->CallStaticBooleanMethod(bridge, midRelease);
        grabbedBefore = !Clear(env) && b == JNI_TRUE;
    } else if (grabbedBefore) {
        env->CallStaticVoidMethod(bridge, midGrab);
        Clear(env);
        grabbedBefore = false;
    }
}

void Frame() {
    if (unloaded) return;
    ULONGLONG now = GetTickCount64();
    if (!greeted) {
        greeted = true;
        menu->Notify(std::string("Fathom loaded. Press ") + core::KeyName(model.keyOpen) + " while playing to open it", ICON_HEART_PULSE);
    }
    if (now - lastState >= 250) {
        lastState = now;
        mc::Live st;
        bool linked = mc::ParseLive(JavaState(), st);
        auto hold = [&](const std::string& id) {
            if (sliders.count(id)) return true;
            auto it = pending.find(id);
            if (it == pending.end()) return false;
            if (now - it->second > 800) {
                pending.erase(it);
                return false;
            }
            return true;
        };
        mc::ApplyLive(model, linked, st, hold, gotoHint);
        for (core::Feature& f : model.features)
            if (!linked) f.missing = "Fathom could not reach the game";
        menu->status = !linked ? "Not connected" : st.world ? "In a world" : "Not in a world";
        menu->statusGood = linked && st.world;
    }
    for (auto it = sliders.begin(); it != sliders.end();) {
        if (now - it->second < 300) {
            ++it;
            continue;
        }
        if (core::Feature* f = model.find(it->first)) {
            nlohmann::json cmd = mc::SliderCommand(*f);
            if (!cmd.is_null() && f->available) {
                Send(cmd);
                pending[f->id] = now;
            }
        }
        it = sliders.erase(it);
    }
    ImGuiIO& io = ImGui::GetIO();
    float fit = std::min(io.DisplaySize.x / 1000.0f, io.DisplaySize.y / 660.0f);
    menu->uiScale = std::clamp(std::min(model.menuScale * overlay::Scale(), fit), 0.5f, 4.0f);
    menu->Frame();
    model.menuOpen = menu->open;
    if (model.requestUnload) {
        model.requestUnload = false;
        unloaded = true;
        menu->open = false;
        if (grabbedBefore) Capture(false);
        core::SaveSettings(model);
        overlay::Stop();
        return;
    }
    if (now - lastSave >= 1000) {
        lastSave = now;
        std::string text = core::SettingsText(model);
        if (text != lastText) {
            core::SaveSettings(model);
            lastText = text;
        }
    }
}

bool Setup(JNIEnv* env, jclass cls, jstring jdir) {
    const jchar* chars = env->GetStringChars(jdir, nullptr);
    if (!chars) return false;
    dir.assign((const wchar_t*)chars, (size_t)env->GetStringLength(jdir));
    env->ReleaseStringChars(jdir, chars);
    midState = env->GetStaticMethodID(cls, "state", "()Ljava/lang/String;");
    midCommand = env->GetStaticMethodID(cls, "command", "(Ljava/lang/String;)V");
    midRelease = env->GetStaticMethodID(cls, "releaseMouse", "()Z");
    midGrab = env->GetStaticMethodID(cls, "grabMouse", "()V");
    midGrabbed = env->GetStaticMethodID(cls, "grabbed", "()Z");
    if (Clear(env) || !midState || !midCommand || !midRelease || !midGrab || !midGrabbed) return false;
    bridge = (jclass)env->NewGlobalRef(cls);
    env->GetJavaVM(&vm);
    slog::Open(dir + L"\\menu-log.txt");
    LOG("Fathom menu for Minecraft starting");

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ui::LoadFonts();

    mc::BuildFeatures(model);
    model.settingsPath = Narrow(dir + L"\\menu-settings.json");
    model.openFolder = [] { ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL); };
    model.openUrl = [](const std::string& url) {
        std::wstring w(url.begin(), url.end());
        ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    };
    for (core::Feature& f : model.features) {
        f.available = false;
        f.apply = [](core::Feature& f) {
            ULONGLONG t = GetTickCount64();
            if (f.kind == core::Kind::Slider) {
                sliders[f.id] = t;
                return;
            }
            nlohmann::json cmd = mc::Command(f);
            if (cmd.is_null()) return;
            Send(cmd);
            if (f.kind == core::Kind::Toggle) pending[f.id] = t;
        };
    }
    if (core::Feature* f = model.find("b_goto")) gotoHint = f->hint;
    core::LoadSettings(model);
    lastText = core::SettingsText(model);
    menu = std::make_unique<ui::Menu>(model);
    menu->SyncFromModel();
    menu->open = false;
    if (!model.tabs.empty()) menu->tab = model.tabs.front();

    if (MH_Initialize() != MH_OK) {
        LOG("hooks could not start");
        return false;
    }
    overlay::Callbacks cb;
    cb.frame = [] { Frame(); };
    cb.wantsInput = [] { return !unloaded && menu && menu->open && menu->focused; };
    cb.keysAllowed = [] { return (menu && menu->open) || GameGrabbed(); };
    cb.eatKey = [](int vk) { return !unloaded && model.keyOpen && vk == overlay::VirtualKeyFor(model.keyOpen); };
    cb.capture = [](bool want) { Capture(want); };
    return overlay::Start(cb);
}

}

extern "C" JNIEXPORT jboolean JNICALL Java_fathom_mc_Overlay_start(JNIEnv* env, jclass cls, jstring jdir) {
    static bool started = false;
    if (started) return JNI_TRUE;
    started = true;
    bool ok = false;
    try {
        ok = Setup(env, cls, jdir);
    } catch (...) {
        ok = false;
    }
    if (!ok) LOG("the Fathom menu could not start");
    return ok ? JNI_TRUE : JNI_FALSE;
}

extern "C" JNIEXPORT jboolean JNICALL Java_fathom_mc_Overlay_rendering(JNIEnv*, jclass) {
    return overlay::Rendering() ? JNI_TRUE : JNI_FALSE;
}
