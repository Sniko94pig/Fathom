#pragma once
#include <cstdint>
#include <functional>

namespace overlay {

struct Callbacks {
    std::function<void()> frame;
    std::function<bool()> wantsInput;
    std::function<bool()> wantsKeyboard;
    std::function<void(uint64_t)> logoReady;
    std::function<void(void*)> window;
    std::function<void()> cleanup;
};

bool Start(void* swapTarget, Callbacks cb);
void Stop();
bool Rendering();
bool Cleaned();
void* Window();
void DisplaySize(float& w, float& h);
int VirtualKeyFor(int imguiKey);

uint64_t MakeTexture(const uint8_t* rgba, int w, int h);
void FreeTexture(uint64_t tex);

}
