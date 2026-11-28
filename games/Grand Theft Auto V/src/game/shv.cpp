#include "game/shv.h"
#include "ingame/log.h"
#include <windows.h>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace shv {

namespace {

using RegisterFn = void (*)(HMODULE, void (*)());
using UnregisterFn = void (*)(HMODULE);
using WaitFn = void (*)(DWORD);
using InitFn = void (*)(uint64_t);
using PushFn = void (*)(uint64_t);
using CallFn = uint64_t* (*)();
using AddressFn = uint8_t* (*)(int);
using WorldFn = int (*)(int*, int);
using VersionFn = int (*)();

HMODULE lib = nullptr;
std::wstring libPath;
RegisterFn fRegister = nullptr;
UnregisterFn fUnregister = nullptr;
WaitFn fWait = nullptr;
InitFn fInit = nullptr;
PushFn fPush = nullptr;
CallFn fCall = nullptr;
AddressFn fAddress = nullptr;
WorldFn fPeds = nullptr;
WorldFn fVehicles = nullptr;
WorldFn fObjects = nullptr;
VersionFn fVersion = nullptr;

std::vector<std::string> ExportNames(HMODULE m) {
    std::vector<std::string> out;
    auto base = (const uint8_t*)m;
    auto dos = (const IMAGE_DOS_HEADER*)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return out;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return out;
    const IMAGE_DATA_DIRECTORY& d = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!d.VirtualAddress || !d.Size) return out;
    auto ex = (const IMAGE_EXPORT_DIRECTORY*)(base + d.VirtualAddress);
    auto names = (const DWORD*)(base + ex->AddressOfNames);
    for (DWORD i = 0; i < ex->NumberOfNames; ++i) out.emplace_back((const char*)(base + names[i]));
    return out;
}

void* Find(const std::vector<std::string>& names, const char* exact, const char* prefix, const char* must) {
    if (exact) {
        for (const std::string& n : names)
            if (n == exact) return (void*)GetProcAddress(lib, n.c_str());
    }
    const std::string* pick = nullptr;
    int count = 0;
    for (const std::string& n : names) {
        if (n.rfind(prefix, 0) != 0) continue;
        if (must && n.find(must) == std::string::npos) continue;
        pick = &n;
        ++count;
    }
    if (count != 1 || !pick) return nullptr;
    return (void*)GetProcAddress(lib, pick->c_str());
}

std::wstring GameFolder() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH * 2);
    if (!n || n >= MAX_PATH * 2) return {};
    return fs::path(buf).parent_path().wstring();
}

bool Bind(std::string& why) {
    std::vector<std::string> names = ExportNames(lib);
    if (names.empty()) {
        why = "ScriptHookV.dll has no exported functions";
        return false;
    }
    fRegister = (RegisterFn)Find(names, "?scriptRegister@@YAXPEAUHINSTANCE__@@P6AXXZ@Z", "?scriptRegister@@", "HINSTANCE");
    fUnregister = (UnregisterFn)Find(names, "?scriptUnregister@@YAXPEAUHINSTANCE__@@@Z", "?scriptUnregister@@", "HINSTANCE");
    fWait = (WaitFn)Find(names, "?scriptWait@@YAXK@Z", "?scriptWait@@", nullptr);
    fInit = (InitFn)Find(names, "?nativeInit@@YAX_K@Z", "?nativeInit@@", nullptr);
    fPush = (PushFn)Find(names, "?nativePush64@@YAX_K@Z", "?nativePush64@@", nullptr);
    fCall = (CallFn)Find(names, "?nativeCall@@YAPEA_KXZ", "?nativeCall@@", nullptr);
    fAddress = (AddressFn)Find(names, "?getScriptHandleBaseAddress@@YAPEAEH@Z", "?getScriptHandleBaseAddress@@", nullptr);
    fPeds = (WorldFn)Find(names, "?worldGetAllPeds@@YAHPEAHH@Z", "?worldGetAllPeds@@", nullptr);
    fVehicles = (WorldFn)Find(names, "?worldGetAllVehicles@@YAHPEAHH@Z", "?worldGetAllVehicles@@", nullptr);
    fObjects = (WorldFn)Find(names, "?worldGetAllObjects@@YAHPEAHH@Z", "?worldGetAllObjects@@", nullptr);
    fVersion = (VersionFn)Find(names, "?getGameVersion@@YA?AW4eGameVersion@@XZ", "?getGameVersion@@", nullptr);
    std::string missing;
    if (!fRegister) missing += " scriptRegister";
    if (!fUnregister) missing += " scriptUnregister";
    if (!fWait) missing += " scriptWait";
    if (!fInit) missing += " nativeInit";
    if (!fPush) missing += " nativePush64";
    if (!fCall) missing += " nativeCall";
    if (!missing.empty()) {
        why = "ScriptHookV.dll is missing:" + missing;
        return false;
    }
    if (!fAddress) LOG("Script Hook V: getScriptHandleBaseAddress not found");
    if (!fPeds || !fVehicles) LOG("Script Hook V: world lists not found");
    return true;
}

}

bool Locate(const std::wstring& folder, std::wstring& path) {
    if (HMODULE m = GetModuleHandleW(L"ScriptHookV.dll")) {
        wchar_t buf[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(m, buf, MAX_PATH * 2);
        path = n ? std::wstring(buf, n) : L"ScriptHookV.dll";
        return true;
    }
    std::error_code ec;
    for (const std::wstring& dir : {folder, GameFolder()}) {
        if (dir.empty()) continue;
        fs::path p = fs::path(dir) / L"ScriptHookV.dll";
        if (fs::exists(p, ec)) {
            path = p.wstring();
            return true;
        }
    }
    return false;
}

bool Load(const std::wstring& folder, std::string& why) {
    if (lib) return true;
    HMODULE m = GetModuleHandleW(L"ScriptHookV.dll");
    if (m) {
        LOG("Script Hook V was already loaded by the game");
    } else {
        std::wstring path;
        if (!Locate(folder, path)) {
            why = "ScriptHookV.dll was not found";
            return false;
        }
        LOG("loading Script Hook V from the Fathom folder");
        m = LoadLibraryW(path.c_str());
        if (!m) {
            why = "ScriptHookV.dll could not be loaded (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
    }
    lib = m;
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(m, buf, MAX_PATH * 2);
    libPath.assign(buf, n);
    if (!Bind(why)) {
        lib = nullptr;
        return false;
    }
    return true;
}

bool Ready() { return lib && fInit && fPush && fCall; }

std::wstring Path() { return libPath; }

bool Register(void* module, void (*main)()) {
    if (!fRegister) return false;
    fRegister((HMODULE)module, main);
    return true;
}

void Unregister(void* module) {
    if (fUnregister) fUnregister((HMODULE)module);
}

void Wait(unsigned ms) {
    if (fWait) fWait(ms);
}

void Begin(uint64_t hash) { fInit(hash); }

void Push(uint64_t value) { fPush(value); }

uint64_t* Call() { return fCall(); }

uint8_t* Address(int handle) { return fAddress && handle ? fAddress(handle) : nullptr; }

int Peds(int* out, int cap) { return fPeds ? fPeds(out, cap) : 0; }

int Vehicles(int* out, int cap) { return fVehicles ? fVehicles(out, cap) : 0; }

int Objects(int* out, int cap) { return fObjects ? fObjects(out, cap) : 0; }

int Version() { return fVersion ? fVersion() : -1; }

}
