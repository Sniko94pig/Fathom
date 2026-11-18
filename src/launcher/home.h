#pragma once
#include "launcher/inject.h"

namespace launcher {

struct Home {
    inject::Paths paths;
    inject::GameState state = inject::GameState::NotRunning;
    bool busy = false;
    bool injectNow = false;
    bool startNow = false;
    int game = -1;
    void Draw();
    void DrawLibrary();
    void DrawSubnautica();
};

}
