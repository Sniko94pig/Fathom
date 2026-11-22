#include "ingame/gd.h"
#include "ingame/log.h"
#include "MinHook.h"
#include <windows.h>
#include <cstring>
#include <deque>
#include <mutex>

namespace gd {

namespace {

Cfg cfg;
HMODULE exe = nullptr;
HMODULE cocos = nullptr;
HMODULE ext = nullptr;
HMODULE fmod = nullptr;
bool supported = false;
std::wstring folder;
std::mutex qLock;
std::deque<std::function<void()>> queued;
std::mutex noteLock;
std::vector<std::string> notes;
std::atomic<int> busy{0};
const RUNTIME_FUNCTION* pdata = nullptr;
size_t pdataCount = 0;

const IMAGE_NT_HEADERS64* Nt(HMODULE m) {
    auto dos = (const IMAGE_DOS_HEADER*)m;
    return (const IMAGE_NT_HEADERS64*)((const uint8_t*)m + dos->e_lfanew);
}

void LoadPdata() {
    const IMAGE_DATA_DIRECTORY& d = Nt(exe)->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d.VirtualAddress || !d.Size) return;
    pdata = (const RUNTIME_FUNCTION*)((const uint8_t*)exe + d.VirtualAddress);
    pdataCount = d.Size / sizeof(RUNTIME_FUNCTION);
}

void* Export(HMODULE m, const char* name) { return m ? (void*)GetProcAddress(m, name) : nullptr; }

}

Cfg& C() { return cfg; }

bool LoadModules(std::string& err) {
    exe = GetModuleHandleW(nullptr);
    cocos = GetModuleHandleW(L"libcocos2d.dll");
    ext = GetModuleHandleW(L"libExtensions.dll");
    fmod = GetModuleHandleW(L"fmod.dll");
    if (!exe || !cocos || !ext || !fmod) {
        err = "the game is still starting";
        return false;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(exe, path, MAX_PATH);
    const wchar_t* file = wcsrchr(path, L'\\');
    file = file ? file + 1 : path;
    if (_wcsicmp(file, L"GeometryDash.exe") != 0) {
        err = "this is not Geometry Dash";
        return false;
    }
    uint32_t stamp = Nt(exe)->FileHeader.TimeDateStamp;
    supported = stamp == kBuild;
    if (supported) LoadPdata();
    else LOG("unknown Geometry Dash build %08X, only the parts that don't need the game's own code are on", stamp);
    return true;
}

bool Supported() { return supported && pdata; }

uintptr_t ExeBase() { return (uintptr_t)exe; }

void* Exe(uint32_t rva) { return supported ? (void*)((uintptr_t)exe + rva) : nullptr; }

void* CocosFn(const char* name) { return Export(cocos, name); }

void* ExtFn(const char* name) { return Export(ext, name); }

void* FmodFn(const char* name) { return Export(fmod, name); }

bool IsFunctionStart(uint32_t rva) {
    if (!Supported()) return false;
    size_t lo = 0, hi = pdataCount;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (pdata[mid].BeginAddress < rva) lo = mid + 1;
        else hi = mid;
    }
    if (lo < pdataCount && pdata[lo].BeginAddress == rva) return true;
    const uint8_t* p = (const uint8_t*)exe + rva;
    return (rva & 0xF) == 0 && (p[-1] == 0xCC || p[-1] == 0xC3) && p[0] != 0xCC;
}

bool Hook(void* target, void* detour, void** orig, const char* name) {
    if (!target) {
        LOG("hook %s: not found", name);
        return false;
    }
    MH_STATUS s = MH_CreateHook(target, detour, orig);
    if (s != MH_OK) {
        LOG("hook %s: could not be created (%d)", name, (int)s);
        return false;
    }
    s = MH_EnableHook(target);
    if (s != MH_OK) {
        LOG("hook %s: could not be switched on (%d)", name, (int)s);
        MH_RemoveHook(target);
        return false;
    }
    return true;
}

bool HookExe(uint32_t rva, void* detour, void** orig, const char* name) {
    if (!Supported()) return false;
    if (!IsFunctionStart(rva)) {
        LOG("hook %s: %X is not a function start, skipped", name, rva);
        return false;
    }
    return Hook(Exe(rva), detour, orig, name);
}

bool Queue(std::function<void()> fn) {
    std::lock_guard<std::mutex> g(qLock);
    if (queued.size() >= 512) return false;
    queued.push_back(std::move(fn));
    return true;
}

void RunQueue() {
    std::deque<std::function<void()>> run;
    {
        std::lock_guard<std::mutex> g(qLock);
        run.swap(queued);
    }
    for (auto& fn : run) {
        try {
            if (fn) fn();
        } catch (...) {
            LOG("error in a queued action");
        }
    }
}

void Note(const std::string& text) {
    std::lock_guard<std::mutex> g(noteLock);
    if (notes.size() < 32) notes.push_back(text);
}

std::vector<std::string> TakeNotes() {
    std::lock_guard<std::mutex> g(noteLock);
    std::vector<std::string> out;
    out.swap(notes);
    return out;
}

void SetFolder(const std::wstring& dir) { folder = dir; }

const std::wstring& Folder() { return folder; }

std::string Narrow(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), w.data(), n);
    return w;
}

Busy::Busy() { ++busy; }

Busy::~Busy() { --busy; }

int BusyCount() { return busy.load(); }

namespace cc {

namespace {

using DirectorFn = void* (*)();
using SceneFn = void*(__fastcall*)(void*);
using VoidFn = void(__fastcall*)(void*);
using CountFn = unsigned(__fastcall*)(void*);
using AtFn = void*(__fastcall*)(void*, unsigned);

DirectorFn sharedDirector = nullptr;
SceneFn runningScene = nullptr;
VoidFn retain = nullptr;
VoidFn release = nullptr;
CountFn count = nullptr;
AtFn objectAt = nullptr;
void* swapTarget = nullptr;
void* schedTarget = nullptr;

const char* RawName(const void* obj) {
    __try {
        void* const* vt = *(void* const* const*)obj;
        const uint32_t* col = (const uint32_t*)vt[-1];
        if (!col || col[0] != 1) return "";
        uintptr_t base = (uintptr_t)col - col[5];
        return (const char*)(base + col[3] + 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
}

}

bool Resolve(std::string& err) {
    sharedDirector = (DirectorFn)CocosFn("?sharedDirector@CCDirector@cocos2d@@SAPEAV12@XZ");
    runningScene = (SceneFn)CocosFn("?getRunningScene@CCDirector@cocos2d@@QEAAPEAVCCScene@2@XZ");
    retain = (VoidFn)CocosFn("?retain@CCObject@cocos2d@@QEAAXXZ");
    release = (VoidFn)CocosFn("?release@CCObject@cocos2d@@QEAAXXZ");
    count = (CountFn)CocosFn("?count@CCArray@cocos2d@@QEBAIXZ");
    objectAt = (AtFn)CocosFn("?objectAtIndex@CCArray@cocos2d@@QEAAPEAVCCObject@2@I@Z");
    swapTarget = CocosFn("?swapBuffers@CCEGLView@cocos2d@@UEAAXXZ");
    schedTarget = CocosFn("?update@CCScheduler@cocos2d@@UEAAXM@Z");
    if (!sharedDirector || !runningScene || !retain || !release || !count || !objectAt || !swapTarget || !schedTarget) {
        err = "cocos2d functions were not found";
        return false;
    }
    return true;
}

void* Director() { return sharedDirector ? sharedDirector() : nullptr; }

void* RunningScene() {
    void* d = Director();
    return d && runningScene ? runningScene(d) : nullptr;
}

void Retain(void* obj) {
    if (obj && retain) retain(obj);
}

void Release(void* obj) {
    if (obj && release) release(obj);
}

unsigned Count(void* array) { return array && count ? count(array) : 0; }

void* At(void* array, unsigned index) { return array && objectAt && index < Count(array) ? objectAt(array, index) : nullptr; }

const char* TypeName(const void* obj) { return obj ? RawName(obj) : ""; }

bool IsA(const void* obj, const char* rawName) { return obj && std::strcmp(TypeName(obj), rawName) == 0; }

void* SwapBuffersTarget() { return swapTarget; }

void* SchedulerUpdateTarget() { return schedTarget; }

}

}
