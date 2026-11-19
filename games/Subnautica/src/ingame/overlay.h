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
};

bool Start(Callbacks cb);
void Stop();
bool Rendering();
int VirtualKeyFor(int imguiKey);

}
