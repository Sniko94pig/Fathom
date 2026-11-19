#pragma once
#include "ui/atlas.h"
#include <cstdint>
#include <functional>
#include <string>

namespace overlay {

struct Callbacks {
    std::function<void()> frame;
    std::function<bool()> wantsInput;
    std::function<bool()> wantsKeyboard;
    std::function<void(uint64_t)> logoReady;
    std::function<void(void*)> window;
    std::function<void(const ui::Atlas&, uint64_t)> atlasReady;
    std::function<void(uint64_t, int, int)> mapImageReady;
    std::wstring folder;
};

bool Start(Callbacks cb);
void Stop();
bool Rendering();
int VirtualKeyFor(int imguiKey);
void LoadAtlas(const std::wstring& folder);
void ReloadMap();

}
