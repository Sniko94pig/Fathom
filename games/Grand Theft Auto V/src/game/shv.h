#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

namespace shv {

struct alignas(8) NVec3 {
    float x = 0;
    uint32_t px = 0;
    float y = 0;
    uint32_t py = 0;
    float z = 0;
    uint32_t pz = 0;
};

bool Locate(const std::wstring& folder, std::wstring& path);
bool Load(const std::wstring& folder, std::string& why);
bool Ready();
std::wstring Path();
bool Register(void* module, void (*main)());
void Unregister(void* module);
void Wait(unsigned ms);
void Begin(uint64_t hash);
void Push(uint64_t value);
uint64_t* Call();
uint8_t* Address(int handle);
int Peds(int* out, int cap);
int Vehicles(int* out, int cap);
int Objects(int* out, int cap);
int Version();

template <typename T>
inline uint64_t Arg(T v) {
    static_assert(sizeof(T) <= sizeof(uint64_t), "native argument too large");
    uint64_t u = 0;
    std::memcpy(&u, &v, sizeof(T));
    return u;
}

template <typename R, typename... A>
inline R Invoke(uint64_t hash, A... args) {
    Begin(hash);
    (Push(Arg(args)), ...);
    uint64_t* r = Call();
    if constexpr (std::is_void_v<R>) {
        return;
    } else {
        R out{};
        if (r) std::memcpy(&out, r, sizeof(R));
        return out;
    }
}

}
