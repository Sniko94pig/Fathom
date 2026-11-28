#include "game/mem.h"
#include <windows.h>
#include <cstring>

namespace mem {

namespace {

uintptr_t base = 0;
size_t size = 0;

void Init() {
    if (base) return;
    HMODULE m = GetModuleHandleW(nullptr);
    auto dos = (const IMAGE_DOS_HEADER*)m;
    auto nt = (const IMAGE_NT_HEADERS64*)((const uint8_t*)m + dos->e_lfanew);
    base = (uintptr_t)m;
    size = nt->OptionalHeader.SizeOfImage;
}

int Guarded(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool Committed(const void* p, size_t n, DWORD want) {
    uintptr_t a = (uintptr_t)p, end = a + n;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi))) return false;
        if (mbi.State != MEM_COMMIT) return false;
        if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
        if (!(mbi.Protect & want)) return false;
        a = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
    return true;
}

}

bool Copy(void* dst, const void* src, size_t n) { return dst && src && Guarded(dst, src, n) == 1; }

bool Readable(const void* p, size_t n) {
    return p && Committed(p, n, PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE);
}

bool Writable(void* p, size_t n) { return p && Committed(p, n, PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE); }

uintptr_t GameBase() {
    Init();
    return base;
}

size_t GameSize() {
    Init();
    return size;
}

bool InGame(uintptr_t p) {
    Init();
    return p >= base && p < base + size;
}

bool HeapLike(uintptr_t p) { return p >= 0x10000 && p < 0x00007FFFFFFFFFFFull && (p & 7) == 0; }

std::vector<uintptr_t> FindFloat(float value, bool dataOnly) {
    Init();
    std::vector<uintptr_t> out;
    auto nt = (const IMAGE_NT_HEADERS64*)(base + ((const IMAGE_DOS_HEADER*)base)->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE)) continue;
        if (dataOnly && (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        uintptr_t a = base + sec->VirtualAddress, end = a + sec->Misc.VirtualSize;
        while (a < end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery((const void*)a, &mbi, sizeof(mbi))) break;
            uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = rs + mbi.RegionSize;
            if (re > end) re = end;
            bool ok = mbi.State == MEM_COMMIT && !(mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) &&
                      (mbi.Protect & (PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READWRITE));
            if (ok) {
                std::vector<uint8_t> chunk(re - a);
                if (Copy(chunk.data(), (const void*)a, chunk.size())) {
                    for (size_t o = 0; o + 4 <= chunk.size(); o += 4) {
                        float f;
                        std::memcpy(&f, chunk.data() + o, 4);
                        if (f == value) out.push_back(a + o);
                        if (out.size() > 4096) return out;
                    }
                }
            }
            a = re;
        }
    }
    return out;
}

}
