#include "game/ue.h"
#include "game/log.h"
#include <windows.h>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ue {

namespace {

struct Offsets {
    int flags = 0x08;
    int index = 0x0C;
    int cls = 0x10;
    int name = 0x18;
    int outer = 0x20;
    int fieldNext = 0x28;
    int super = -1;
    int children = -1;
    int childProps = -1;
    int propsSize = -1;
    int ffClass = 0x08;
    int ffNext = -1;
    int ffName = -1;
    int propOffset = -1;
    int boolField = -1;
    int funcFlags = -1;
    int itemSize = 0x18;
    int peIndex = -1;
} off;

uint8_t* objArray = nullptr;
uint8_t** nameBlocks = nullptr;
void* peAddress = nullptr;

std::mutex cacheLock;
std::unordered_map<std::string, void*> objectCache;
std::unordered_map<std::string, Prop> propCache;
std::unordered_map<std::string, void*> funcCache;

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

template <class T>
bool SafeRead(const void* p, T& out) {
    return SafeCopy(&out, p, sizeof(T));
}

bool Plausible(uintptr_t v) { return v > 0x10000 && v < 0x7FFFFFFFFFFFull && (v & 7) == 0; }

template <class T>
T At(const void* p, int o) {
    T v{};
    std::memcpy(&v, (const uint8_t*)p + o, sizeof(T));
    return v;
}

struct Range {
    uint8_t* start;
    size_t size;
};

std::vector<Range> Sections(bool writable) {
    std::vector<Range> out;
    auto base = (uint8_t*)GetModuleHandleW(nullptr);
    auto dos = (IMAGE_DOS_HEADER*)base;
    auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++sec) {
        bool w = (sec->Characteristics & IMAGE_SCN_MEM_WRITE) != 0;
        bool x = (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0;
        if (writable ? (w && !x) : x) out.push_back({base + sec->VirtualAddress, sec->Misc.VirtualSize});
    }
    return out;
}

bool FindNames() {
    for (const Range& r : Sections(true)) {
        for (size_t o = 0; o + 8 <= r.size; o += 8) {
            uintptr_t v = *(uintptr_t*)(r.start + o);
            if (!Plausible(v)) continue;
            uint8_t e[24];
            if (!SafeRead((const void*)v, e)) continue;
            uint16_t h = *(uint16_t*)e;
            if ((h & 1) || (h >> 6) != 4 || std::memcmp(e + 2, "None", 4) != 0) continue;
            uint16_t h2 = *(uint16_t*)(e + 6);
            if ((h2 >> 6) != 12 || std::memcmp(e + 8, "ByteProperty", 12) != 0) continue;
            nameBlocks = (uint8_t**)(r.start + o);
            LOG("GNames blocks at %p", nameBlocks);
            return true;
        }
    }
    return false;
}

bool FindObjects() {
    for (const Range& r : Sections(true)) {
        for (size_t o = 0; o + 0x20 <= r.size; o += 8) {
            uint8_t* a = r.start + o;
            uintptr_t objects = *(uintptr_t*)a;
            if (!Plausible(objects)) continue;
            int32_t mx = *(int32_t*)(a + 0x10), num = *(int32_t*)(a + 0x14);
            int32_t maxChunks = *(int32_t*)(a + 0x18), numChunks = *(int32_t*)(a + 0x1C);
            if (num < 0x1000 || num > mx || mx > 0x4000000) continue;
            if (numChunks != (num + 0xFFFF) / 0x10000 || maxChunks != (mx + 0xFFFF) / 0x10000) continue;
            uintptr_t chunk0 = 0;
            if (!SafeRead((const void*)objects, chunk0) || !Plausible(chunk0)) continue;
            for (int size : {0x18, 0x10, 0x20, 0x28}) {
                int hits = 0;
                for (int i = 1; i <= 16; ++i) {
                    uintptr_t obj = 0;
                    int32_t idx = -1;
                    if (!SafeRead((const void*)(chunk0 + (size_t)i * size), obj) || !Plausible(obj)) break;
                    if (!SafeRead((const void*)(obj + off.index), idx) || idx != i) break;
                    ++hits;
                }
                if (hits == 16) {
                    objArray = a;
                    off.itemSize = size;
                    LOG("GObjects at %p, %d objects, item size 0x%X", a, num, size);
                    return true;
                }
            }
        }
    }
    return false;
}

bool Entry(FName n, const uint8_t*& e, int& len, bool& wide) {
    uint32_t block = (uint32_t)n.index >> 16;
    uint32_t offs = ((uint32_t)n.index & 0xFFFF) * 2;
    uint32_t current = *(uint32_t*)((uint8_t*)nameBlocks - 8);
    if (block > current || block >= 8192) return false;
    const uint8_t* b = nameBlocks[block];
    if (!b) return false;
    e = b + offs;
    uint16_t h = 0;
    if (!SafeRead(e, h)) return false;
    len = h >> 6;
    wide = h & 1;
    return true;
}

bool NameIs(FName n, const char* s, size_t len) {
    const uint8_t* e;
    int l;
    bool wide;
    if (n.number != 0 || !Entry(n, e, l, wide) || wide || (size_t)l != len) return false;
    return _strnicmp((const char*)e + 2, s, len) == 0;
}

FName NameAt(const void* obj, int o) {
    FName n;
    SafeRead((const uint8_t*)obj + o, n);
    return n;
}

void* PtrAt(const void* obj, int o) {
    void* p = nullptr;
    SafeRead((const uint8_t*)obj + o, p);
    return p;
}

std::string FieldName(const void* f) { return NameToString(NameAt(f, off.ffName)); }

std::string FieldClassName(const void* f) {
    void* fc = PtrAt(f, off.ffClass);
    if (!fc) return {};
    return NameToString(NameAt(fc, 0));
}

void* WalkFields(const void* type, const char* name) {
    for (void* f = PtrAt(type, off.childProps); f; f = PtrAt(f, off.ffNext))
        if (_stricmp(FieldName(f).c_str(), name) == 0) return f;
    return nullptr;
}

bool Calibrate(std::string& err) {
    void* character = FindObject("/Script/Engine.Character");
    void* pawn = FindObject("/Script/Engine.Pawn");
    void* actor = FindObject("/Script/Engine.Actor");
    if (!character || !pawn || !actor) {
        err = "core classes not found";
        return false;
    }
    for (int o = 0x28; o <= 0xA0; o += 8)
        if (PtrAt(character, o) == pawn) {
            off.super = o;
            break;
        }
    if (off.super < 0) {
        err = "SuperStruct offset not found";
        return false;
    }
    for (int o = off.super + 8; o <= off.super + 0x40 && off.childProps < 0; o += 8) {
        void* p = PtrAt(actor, o);
        if (!Plausible((uintptr_t)p) || Valid(p)) continue;
        for (int n = 0x10; n <= 0x48; n += 8) {
            int count = 0;
            for (void* f = p; f && count < 2000; ++count) {
                FName fn;
                if (!SafeRead((uint8_t*)f + n, fn)) break;
                if (NameIs(fn, "RootComponent", 13)) {
                    off.childProps = o;
                    off.ffName = n;
                    off.ffNext = n - 8;
                    break;
                }
                void* next = nullptr;
                if (!SafeRead((uint8_t*)f + n - 8, next) || (next && !Plausible((uintptr_t)next))) break;
                f = next;
            }
            if (off.childProps >= 0) break;
        }
    }
    if (off.childProps < 0) {
        err = "ChildProperties offset not found";
        return false;
    }
    off.propsSize = off.childProps + 8;
    for (int o = off.super + 8; o <= off.super + 0x40; o += 8) {
        if (o == off.childProps) continue;
        void* q = PtrAt(actor, o);
        if (Valid(q) && Name(ClassOf(q)) == "Function") {
            off.children = o;
            break;
        }
    }
    if (off.children < 0) {
        err = "Children offset not found";
        return false;
    }
    void* rc = WalkFields(actor, "RootComponent");
    void* pat = WalkFields(actor, "PrimaryActorTick");
    if (!rc || !pat) {
        err = "Actor properties not found";
        return false;
    }
    auto check = [&](int d) {
        int32_t dim = 0, size = 0, ro = 0, po = 0;
        SafeRead((uint8_t*)rc + d - 0x14, dim);
        SafeRead((uint8_t*)rc + d - 0x10, size);
        SafeRead((uint8_t*)rc + d, ro);
        SafeRead((uint8_t*)pat + d, po);
        return dim == 1 && size == 8 && ro > 0x28 && ro < 0x4000 && (ro & 7) == 0 && po >= 0x28 && po < ro;
    };
    if (check(off.ffName + 0x24)) off.propOffset = off.ffName + 0x24;
    for (int d = off.ffName + 0x10; d <= off.ffName + 0x48 && off.propOffset < 0; d += 4)
        if (check(d)) off.propOffset = d;
    if (off.propOffset < 0) {
        err = "Offset_Internal not found";
        return false;
    }
    void* setLoc = FindFunction(actor, "K2_SetActorLocation");
    void* ret = setLoc ? WalkFields(setLoc, "ReturnValue") : nullptr;
    if (ret) {
        for (int d = off.propOffset + 4; d <= off.propOffset + 0x60; ++d) {
            uint8_t b[4];
            if (SafeRead((uint8_t*)ret + d, b) && b[0] == 1 && b[1] == 0 && b[2] != 0 && b[3] == 0xFF) {
                off.boolField = d;
                break;
            }
        }
    }
    void* structCls = FindObject("/Script/CoreUObject.Struct");
    if (structCls) SafeRead((uint8_t*)structCls + off.propsSize, off.funcFlags);
    LOG("offsets: super 0x%X children 0x%X childProps 0x%X ffName 0x%X propOffset 0x%X bool 0x%X funcFlags 0x%X",
        off.super, off.children, off.childProps, off.ffName, off.propOffset, off.boolField, off.funcFlags);
    LOG("check: Actor.RootComponent offset 0x%X (dump says 0x1C0)", FindProp(actor, "RootComponent").offset);
    return true;
}

bool Match(const uint8_t* code, size_t n, int flagsOff, uint32_t mask) {
    for (size_t i = 0; i + 10 <= n; ++i) {
        if (code[i] == 0xF7 && (code[i + 1] & 0xF8) == 0x80 && code[i + 1] != 0x84 && *(int32_t*)(code + i + 2) == flagsOff &&
            *(uint32_t*)(code + i + 6) == mask)
            return true;
        for (int b = 0; b < 4; ++b) {
            uint32_t m = mask >> (b * 8);
            if ((mask & ~(0xFFu << (b * 8))) != 0 || m == 0) continue;
            if (code[i] == 0xF6 && (code[i + 1] & 0xF8) == 0x80 && code[i + 1] != 0x84 &&
                *(int32_t*)(code + i + 2) == flagsOff + b && code[i + 6] == (uint8_t)m)
                return true;
        }
    }
    return false;
}

bool FindProcessEvent(std::string& err) {
    void* actor = FindObject("/Script/Engine.Actor");
    void** vt = (void**)PtrAt(actor, 0);
    if (!vt || off.funcFlags <= 0) {
        err = "vtable or FunctionFlags missing";
        return false;
    }
    for (int i = 0; i < 0x140; ++i) {
        void* fn = nullptr;
        if (!SafeRead(vt + i, fn) || !fn) break;
        uint8_t code[0x500];
        if (!SafeCopy(code, fn, sizeof(code))) continue;
        if (Match(code, sizeof(code), off.funcFlags, 0x400) && Match(code, sizeof(code), off.funcFlags, 0x400000)) {
            off.peIndex = i;
            peAddress = fn;
            LOG("ProcessEvent: vtable index 0x%X at %p", i, fn);
            return true;
        }
    }
    err = "ProcessEvent not found in vtable";
    return false;
}

void Invoke(void* obj, void* fn, void* parms, bool& ok) {
    using PE = void(__fastcall*)(void*, void*, void*);
    PE pe = (PE)(*(void***)obj)[off.peIndex];
    __try {
        pe(obj, fn, parms);
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
}

}

bool Init(std::string& error) {
    if (!nameBlocks && !FindNames()) {
        error = "name table not found";
        return false;
    }
    if (!objArray && !FindObjects()) {
        error = "object table not found";
        return false;
    }
    if (off.childProps < 0 && !Calibrate(error)) return false;
    if (off.peIndex < 0 && !FindProcessEvent(error)) return false;
    return true;
}

void* ProcessEventAddress() { return peAddress; }

int32_t ObjectCount() { return objArray ? *(int32_t*)(objArray + 0x14) : 0; }

void* ObjectAt(int32_t i) {
    if (!objArray || i < 0 || i >= ObjectCount()) return nullptr;
    uint8_t** chunks = *(uint8_t***)objArray;
    uint8_t* chunk = chunks[i / 0x10000];
    if (!chunk) return nullptr;
    return *(void**)(chunk + (size_t)(i % 0x10000) * off.itemSize);
}

int32_t SerialAt(int32_t i, int field) {
    if (!objArray || i < 0 || i >= ObjectCount()) return 0;
    uint8_t** chunks = *(uint8_t***)objArray;
    uint8_t* chunk = chunks[i / 0x10000];
    if (!chunk) return 0;
    int32_t serial = 0;
    SafeRead(chunk + (size_t)(i % 0x10000) * off.itemSize + field, serial);
    return serial;
}

int32_t SerialAt(int32_t i) { return SerialAt(i, 0x14); }

void* WeakGet(int32_t index, int32_t serial) {
    if (index <= 0 || serial == 0) return nullptr;
    void* o = ObjectAt(index);
    if (!o || (SerialAt(index, 0x10) != serial && SerialAt(index, 0x14) != serial)) return nullptr;
    return Alive(o) ? o : nullptr;
}

bool Valid(const void* obj) {
    if (!obj || !Plausible((uintptr_t)obj)) return false;
    int32_t idx = -1;
    if (!SafeRead((const uint8_t*)obj + off.index, idx)) return false;
    return ObjectAt(idx) == obj;
}

bool Alive(const void* obj) {
    if (!Valid(obj)) return false;
    uint32_t f = 0;
    if (!SafeRead((const uint8_t*)obj + off.flags, f)) return false;
    return (f & (0x8000u | 0x10000u | 0x40000000u)) == 0;
}

bool IsDefault(const void* obj) {
    uint32_t f = 0;
    SafeRead((const uint8_t*)obj + off.flags, f);
    return (f & 0x10) != 0;
}

bool IsTemplate(const void* obj) {
    uint32_t f = 0;
    SafeRead((const uint8_t*)obj + off.flags, f);
    return (f & 0x30) != 0;
}

std::string NameToString(FName n) {
    const uint8_t* e;
    int len;
    bool wide;
    if (!nameBlocks || !Entry(n, e, len, wide)) return {};
    std::string s;
    if (wide) {
        std::vector<wchar_t> w(len);
        if (!SafeCopy(w.data(), e + 2, len * 2)) return {};
        for (wchar_t c : w) s.push_back(c < 128 ? (char)c : '?');
    } else {
        s.resize(len);
        if (!SafeCopy(s.data(), e + 2, len)) return {};
    }
    if (n.number > 0) s += "_" + std::to_string(n.number - 1);
    return s;
}

std::string Name(const void* obj) { return obj ? NameToString(NameAt(obj, off.name)) : std::string(); }

std::string Path(const void* obj) {
    if (!obj) return {};
    void* outer = PtrAt(obj, off.outer);
    if (!outer) return Name(obj);
    return Path(outer) + (PtrAt(outer, off.outer) ? ":" : ".") + Name(obj);
}

void* ClassOf(const void* obj) { return obj ? PtrAt(obj, off.cls) : nullptr; }

void* SuperOf(const void* cls) { return cls && off.super >= 0 ? PtrAt(cls, off.super) : nullptr; }

bool IsA(const void* obj, const void* cls) {
    if (!obj || !cls) return false;
    for (void* c = ClassOf(obj); c; c = SuperOf(c))
        if (c == cls) return true;
    return false;
}

void* FindObject(const std::string& path) {
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = objectCache.find(path);
        if (it != objectCache.end() && Valid(it->second)) return it->second;
    }
    size_t dot = path.rfind('.');
    if (dot == std::string::npos) return nullptr;
    std::string pkg = path.substr(0, dot), name = path.substr(dot + 1);
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = ObjectAt(i);
        if (!obj || !NameIs(NameAt(obj, off.name), name.c_str(), name.size())) continue;
        void* outer = PtrAt(obj, off.outer);
        if (!outer || PtrAt(outer, off.outer)) continue;
        if (Name(outer) != pkg) continue;
        std::lock_guard<std::mutex> g(cacheLock);
        objectCache[path] = obj;
        return obj;
    }
    return nullptr;
}

void* FindFirstOf(const void* cls) {
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = ObjectAt(i);
        if (obj && !IsDefault(obj) && IsA(obj, cls)) return obj;
    }
    return nullptr;
}

Prop FindProp(const void* type, const char* name) {
    if (!type || off.childProps < 0) return {};
    char key[256];
    std::snprintf(key, sizeof(key), "%p.%s", type, name);
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = propCache.find(key);
        if (it != propCache.end()) return it->second;
    }
    Prop p;
    for (const void* s = type; s && p.offset < 0; s = SuperOf(s)) {
        void* f = WalkFields(s, name);
        if (!f) continue;
        int32_t o = 0, dim = 1, size = 0;
        SafeRead((uint8_t*)f + off.propOffset, o);
        SafeRead((uint8_t*)f + off.propOffset - 0x14, dim);
        SafeRead((uint8_t*)f + off.propOffset - 0x10, size);
        p.offset = o;
        p.size = size * dim;
        if (off.boolField >= 0 && FieldClassName(f) == "BoolProperty") {
            uint8_t b[4] = {};
            SafeRead((uint8_t*)f + off.boolField, b);
            p.offset += b[1];
            p.mask = b[3];
        }
        if (Name(ClassOf(type)) == "Function") break;
    }
    std::lock_guard<std::mutex> g(cacheLock);
    propCache[key] = p;
    return p;
}

void* FindFunction(const void* cls, const char* name) {
    if (!cls || off.children < 0) return nullptr;
    char key[256];
    std::snprintf(key, sizeof(key), "%p.%s", cls, name);
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = funcCache.find(key);
        if (it != funcCache.end()) return it->second;
    }
    void* found = nullptr;
    size_t len = std::strlen(name);
    for (const void* c = cls; c && !found; c = SuperOf(c))
        for (void* f = PtrAt(c, off.children); f; f = PtrAt(f, off.fieldNext))
            if (NameIs(NameAt(f, off.name), name, len)) {
                found = f;
                break;
            }
    std::lock_guard<std::mutex> g(cacheLock);
    funcCache[key] = found;
    return found;
}

bool ReadBool(const void* obj, const Prop& p) {
    if (!obj || !p) return false;
    return (((const uint8_t*)obj)[p.offset] & p.mask) != 0;
}

void WriteBool(void* obj, const Prop& p, bool v) {
    if (!obj || !p) return;
    uint8_t& b = ((uint8_t*)obj)[p.offset];
    b = v ? (b | p.mask) : (b & ~p.mask);
}

void* Ptr(const void* obj, const char* prop) {
    if (!obj) return nullptr;
    Prop p = FindProp(ClassOf(obj), prop);
    if (!p) return nullptr;
    void* v = PtrAt(obj, p.offset);
    return Alive(v) ? v : nullptr;
}

Call::Call(void* o, const char* func) : obj(o) {
    if (Alive(o)) fn = FindFunction(ClassOf(o), func);
}

Call::Call(void* o, void* cls, const char* func) : obj(o) {
    if (Alive(o)) fn = FindFunction(cls, func);
}

Call& Call::SetBool(const char* name, bool v) {
    Prop p = FindProp(fn, name);
    if (p) buf[p.offset] = v ? (buf[p.offset] | p.mask) : (buf[p.offset] & ~p.mask);
    return *this;
}

Call& Call::SetString(const char* name, const std::wstring& s) {
    if (nstr >= 4) return *this;
    strings[nstr] = s;
    const std::wstring& k = strings[nstr++];
    FString f{k.c_str(), (int32_t)k.size() + 1, (int32_t)k.size() + 1};
    return Set(name, f);
}

Call& Call::SetFirstRaw(const void* data, int size) {
    if (!fn || off.childProps < 0) return *this;
    void* f = PtrAt(fn, off.childProps);
    if (!f) return *this;
    int32_t o = -1;
    SafeRead((uint8_t*)f + off.propOffset, o);
    if (o >= 0 && o + size <= (int)sizeof(buf)) std::memcpy(buf + o, data, (size_t)size);
    return *this;
}

Call::Call(void* o, void* func, int) : obj(o), fn(Alive(o) ? func : nullptr) {}

bool Call::GetBool(const char* name) const {
    Prop p = FindProp(fn, name);
    return p && (buf[p.offset] & p.mask) != 0;
}

bool Call::Run() {
    if (!Ok() || off.peIndex < 0) return false;
    struct CrashInfo {
        int count = 0;
        ULONGLONG windowStart = 0, blockedUntil = 0;
    };
    static std::mutex crashLock;
    static std::unordered_map<uint64_t, CrashInfo> crashes;
    uint64_t key = (uint64_t)(uintptr_t)fn * 31 + (uint64_t)(uintptr_t)ClassOf(obj);
    bool cdo = IsDefault(obj);
    {
        std::lock_guard<std::mutex> g(crashLock);
        auto it = crashes.find(key);
        if (it != crashes.end()) {
            if (!cdo && it->second.count >= 2) return false;
            if (cdo && GetTickCount64() < it->second.blockedUntil) return false;
        }
    }
    bool ok = false;
    Invoke(obj, fn, buf, ok);
    if (!ok) {
        std::lock_guard<std::mutex> g(crashLock);
        CrashInfo& c = crashes[key];
        ULONGLONG now = GetTickCount64();
        if (cdo) {
            if (now - c.windowStart > 60000) {
                c.windowStart = now;
                c.count = 0;
            }
            if (++c.count >= 5) {
                c.blockedUntil = now + 60000;
                c.count = 0;
                LOG("call crashed: %s on %s (paused for a minute)", Name(fn).c_str(), Name(obj).c_str());
            } else if (c.count == 1) {
                LOG("call crashed: %s on %s", Name(fn).c_str(), Name(obj).c_str());
            }
        } else {
            int n = ++c.count;
            if (n <= 2) LOG("call crashed: %s on %s%s", Name(fn).c_str(), Name(obj).c_str(), n == 2 ? " (not calling it again)" : "");
        }
    }
    return ok;
}

void* FindClassByName(const std::string& name) {
    std::string key = "class:" + name;
    static std::unordered_map<std::string, ULONGLONG> misses;
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = objectCache.find(key);
        if (it != objectCache.end() && Valid(it->second)) return it->second;
        auto m = misses.find(key);
        if (m != misses.end() && GetTickCount64() - m->second < 20000) return nullptr;
        misses[key] = GetTickCount64();
    }
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = ObjectAt(i);
        if (!obj || !NameIs(NameAt(obj, off.name), name.c_str(), name.size())) continue;
        std::string cn = Name(ClassOf(obj));
        if (cn != "Class" && cn != "BlueprintGeneratedClass" && cn != "ScriptStruct" && cn != "UserDefinedStruct") continue;
        std::lock_guard<std::mutex> g(cacheLock);
        objectCache[key] = obj;
        return obj;
    }
    return nullptr;
}

int StructSize(const void* type) {
    int32_t size = 0;
    if (!type || off.propsSize < 0) return 0;
    SafeRead((const uint8_t*)type + off.propsSize, size);
    return size;
}

std::string FStringToUtf8(const FString& s) {
    if (!s.data || s.num <= 1) return {};
    std::wstring w;
    w.resize((size_t)s.num - 1);
    if (!SafeCopy(w.data(), s.data, w.size() * sizeof(wchar_t))) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

FName MakeName(const std::wstring& s) {
    Call c(Statics("/Script/Engine.Default__KismetStringLibrary"), "Conv_StringToName");
    c.SetString("InString", s);
    if (!c.Run()) return {};
    return c.Get<FName>("ReturnValue");
}

void* Statics(const char* path) { return FindObject(path); }

}
