#pragma once
#include <functional>

namespace overlay {

struct Callbacks {
    std::function<void()> frame;
    std::function<bool()> wantsInput;
    std::function<bool()> keysAllowed;
    std::function<bool(int)> eatKey;
    std::function<void(bool)> capture;
};

bool Start(Callbacks cb);
void Stop();
bool Rendering();
float Scale();
int VirtualKeyFor(int imguiKey);

}
