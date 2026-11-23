#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace gd {

constexpr uint32_t kBuild = 0x6AD2D987;

struct Cfg {
    std::atomic<float> speed{1.0f};
    std::atomic<bool> noclip{false};
    std::atomic<bool> jumps{false};
    std::atomic<bool> hitboxes{false};
    std::atomic<bool> icons{false};
    std::atomic<bool> record{false};
    std::atomic<bool> play{false};
    std::atomic<bool> rgb{false};
    std::atomic<float> rgbSpeed{1.0f};
    std::atomic<bool> menuOpen{false};
    std::atomic<bool> stopping{false};
};

Cfg& C();

bool LoadModules(std::string& err);
bool Supported();
uintptr_t ExeBase();
void* Exe(uint32_t rva);
void* CocosFn(const char* name);
void* ExtFn(const char* name);
void* FmodFn(const char* name);
bool IsFunctionStart(uint32_t rva);

bool Hook(void* target, void* detour, void** orig, const char* name);
bool HookExe(uint32_t rva, void* detour, void** orig, const char* name);

bool Queue(std::function<void()> fn);
void RunQueue();
void Note(const std::string& text);
std::vector<std::string> TakeNotes();

void SetFolder(const std::wstring& dir);
const std::wstring& Folder();
std::string Narrow(const std::wstring& w);
std::wstring Widen(const std::string& s);

struct Busy {
    Busy();
    ~Busy();
    Busy(const Busy&) = delete;
    Busy& operator=(const Busy&) = delete;
};
int BusyCount();

template <class T>
inline T& F(const void* obj, uintptr_t off) {
    return *reinterpret_cast<T*>(reinterpret_cast<uintptr_t>(obj) + off);
}

struct Seed {
    int a, b, c;
};

namespace cc {

bool Resolve(std::string& err);
void* Director();
void* RunningScene();
void Retain(void* obj);
void Release(void* obj);
unsigned Count(void* array);
void* At(void* array, unsigned index);
const char* TypeName(const void* obj);
bool IsA(const void* obj, const char* rawName);
void* SwapBuffersTarget();
void* SchedulerUpdateTarget();

}

}
