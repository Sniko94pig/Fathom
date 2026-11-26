#pragma once
#include "ui/atlas.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace overlay {

struct Callbacks {
    std::function<void()> frame;
    std::function<bool()> wantsInput;
    std::function<bool()> wantsKeyboard;
    std::function<void(const ui::Atlas&, uint64_t)> atlasReady;
    std::function<void(uint64_t)> mapReady;
    std::function<void(uint64_t)> logoReady;
    std::function<void(uint64_t, int, int)> mapImageReady;
    std::function<void(void*)> window;
    std::wstring folder;
};

bool Start(Callbacks cb);
void LoadAtlas(const std::wstring& folder);
void SetMapImage(std::vector<uint8_t>&& rgba, int width, int height);
void Stop();
bool Running();
int VirtualKeyFor(int imguiKey);
uint64_t FrameCount();
void TakeLook(float& dx, float& dy);
bool GameActive();

}
