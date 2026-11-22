#pragma once
#include <string>

namespace safe {

bool Install();
bool Ready();
void Tick();
void Frame();
void Off();
bool InLevel();
bool Cheated();
bool Holding();
bool Locked();
void MarkCheated(const char* why);
const char* Status(bool& good);

struct Protect {
    Protect();
    ~Protect();
    Protect(const Protect&) = delete;
    Protect& operator=(const Protect&) = delete;
    bool active = false;
    bool was = false;
    void* obj = nullptr;
};

}

namespace speed {

bool Install();
bool Ready();
void Tick();
float Scale(float dt);
void Off();

}

namespace player {

bool Install();
bool NoclipReady();
bool JumpsReady();
bool HitboxReady();
bool CompleteReady();
std::string Complete();
void Tick();
void Off();

}

namespace jumps {

bool Install();
bool Ready();
void Tick();
void Off();

}

namespace icons {

bool Install();
bool Ready();
void Set(bool on);
void Tick();
void Off();
bool BlockUpload(const char* url);

}

namespace macro {

bool Install();
bool RecordReady();
bool PlayReady();
void Tick();
void Frame();
void Off();

}

namespace rgb {

bool Install();
bool Ready();
void Tick(float dt);
void Off();

}

namespace scare {

bool Install();
bool Ready();
void Tick(float dt);
void Frame();
void Now();
void Off();
void Cleanup();

}
