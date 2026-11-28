#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace mem {

bool Copy(void* dst, const void* src, size_t n);
bool Readable(const void* p, size_t n);
bool Writable(void* p, size_t n);
uintptr_t GameBase();
size_t GameSize();
bool InGame(uintptr_t p);
bool HeapLike(uintptr_t p);

template <typename T>
inline bool Read(uintptr_t p, T& out) {
    return p && Copy(&out, (const void*)p, sizeof(T));
}

template <typename T>
inline bool Write(uintptr_t p, const T& v) {
    return p && Writable((void*)p, sizeof(T)) && Copy((void*)p, &v, sizeof(T));
}

std::vector<uintptr_t> FindFloat(float value, bool dataOnly);

}
