#include "game/ue.h"
#include "ingame/log.h"
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
    int propsSize = -1;
    int propOffset = -1;
    int boolField = -1;
    int propExtra = 0x70;
    int funcFlags = -1;
    int itemSize = 0x18;
    int serial = 0x10;
    int peIndex = -1;
    int nameText = -1;
    int nameIndex = 0;
} off;

uint8_t* objArray = nullptr;
bool objChunked = true;
uint8_t* names = nullptr;
void* peAddress = nullptr;

std::mutex cacheLock;
std::unordered_map<std::string, void*> objectCache;
std::unordered_map<std::string, Prop> propCache;
std::unordered_map<std::string, void*> funcCache;

bool Plausible(uintptr_t v) { return v > 0x10000 && v < 0x7FFFFFFFFFFFull && (v & 7) == 0; }

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

void* PtrAt(const void* obj, int o) {
    void* p = nullptr;
    if (obj && o >= 0) SafeRead((const uint8_t*)obj + o, p);
    return p;
}

FName NameAt(const void* obj, int o) {
    FName n;
    if (obj) SafeRead((const uint8_t*)obj + o, n);
    return n;
}

bool CheckNames(uint8_t* base) {
    uintptr_t chunk0 = 0;
    if (!SafeRead(base, chunk0) || !Plausible(chunk0)) return false;
    int32_t num = 0, chunks = 0;
    if (!SafeRead(base + 0x400, num) || !SafeRead(base + 0x404, chunks)) return false;
    int32_t need = (num + 16383) / 16384;
    if (num < 1000 || num > 4 * 1024 * 1024 || chunks < need || chunks > need + 1 || chunks > 128) return false;
    uintptr_t e0 = 0, e1 = 0;
    if (!SafeRead((const void*)chunk0, e0) || !SafeRead((const void*)(chunk0 + 8), e1)) return false;
    if (!Plausible(e0) || !Plausible(e1)) return false;
    for (int t : {0x10, 0x0C, 0x14, 0x18, 0x08, 0x1C, 0x20}) {
        char a[5] = {}, b[13] = {};
        if (!SafeCopy(a, (const void*)(e0 + t), sizeof(a)) || !SafeCopy(b, (const void*)(e1 + t), sizeof(b))) continue;
        if (std::memcmp(a, "None", 5) != 0 || std::memcmp(b, "ByteProperty", 13) != 0) continue;
        off.nameText = t;
        for (int k : {0, 8, 4}) {
            int32_t v = -1;
            if (k + 4 <= t && SafeRead((const void*)(e1 + k), v) && v == 2) {
                off.nameIndex = k;
                break;
            }
        }
        return true;
    }
    return false;
}

bool FindNames() {
    for (const Range& r : Sections(true)) {
        for (size_t o = 0; o + 8 <= r.size; o += 8) {
            uintptr_t v = *(uintptr_t*)(r.start + o);
            if (!Plausible(v) || !CheckNames((uint8_t*)v)) continue;
            names = (uint8_t*)v;
            LOG("name table at %p (entry text +0x%X, index +0x%X)", names, off.nameText, off.nameIndex);
            return true;
        }
    }
    for (const Range& r : Sections(true)) {
        for (size_t o = 0; o + 0x408 <= r.size; o += 8) {
            if (!CheckNames(r.start + o)) continue;
            names = r.start + o;
            LOG("name table inline at %p (entry text +0x%X, index +0x%X)", names, off.nameText, off.nameIndex);
            return true;
        }
    }
    return false;
}

bool ItemsMatch(uintptr_t items, int& sizeOut) {
    for (int size : {0x18, 0x10, 0x20, 0x28}) {
        int hits = 0;
        for (int i = 1; i <= 16; ++i) {
            uintptr_t obj = 0;
            int32_t idx = -1;
            if (!SafeRead((const void*)(items + (size_t)i * size), obj) || !Plausible(obj)) break;
            if (!SafeRead((const void*)(obj + off.index), idx) || idx != i) break;
            ++hits;
        }
        if (hits == 16) {
            sizeOut = size;
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
            int32_t need = (num + 0xFFFF) / 0x10000;
            if (numChunks < need || numChunks > need + 1 || maxChunks < numChunks || maxChunks > mx / 0x10000 + 2) continue;
            uintptr_t chunk0 = 0;
            if (!SafeRead((const void*)objects, chunk0) || !Plausible(chunk0)) continue;
            int size = 0;
            if (!ItemsMatch(chunk0, size)) continue;
            objArray = a;
            objChunked = true;
            off.itemSize = size;
            LOG("object table at %p, %d objects, item size 0x%X", a, num, size);
            return true;
        }
    }
    for (const Range& r : Sections(true)) {
        for (size_t o = 0; o + 0x10 <= r.size; o += 8) {
            uint8_t* a = r.start + o;
            uintptr_t objects = *(uintptr_t*)a;
            if (!Plausible(objects)) continue;
            int32_t mx = *(int32_t*)(a + 0x8), num = *(int32_t*)(a + 0xC);
            if (num < 0x1000 || num > mx || mx > 0x4000000) continue;
            int size = 0;
            if (!ItemsMatch(objects, size)) continue;
            objArray = a;
            objChunked = false;
            off.itemSize = size;
            LOG("flat object table at %p, %d objects, item size 0x%X", a, num, size);
            return true;
        }
    }
    return false;
}

bool Entry(FName n, const uint8_t*& text, bool& wide) {
    if (!names || n.index < 0) return false;
    int32_t num = 0;
    if (!SafeRead(names + 0x400, num) || n.index >= num) return false;
    uint8_t* chunk = nullptr;
    if (!SafeRead(names + (size_t)(n.index / 16384) * 8, chunk) || !chunk) return false;
    uint8_t* e = nullptr;
    if (!SafeRead(chunk + (size_t)(n.index % 16384) * 8, e) || !e) return false;
    int32_t idx = 0;
    SafeRead(e + off.nameIndex, idx);
    wide = (idx & 1) != 0;
    text = e + off.nameText;
    return true;
}

bool NameIs(FName n, const char* s, size_t len) {
    const uint8_t* e;
    bool wide;
    if (n.number != 0 || !Entry(n, e, wide) || wide) return false;
    char tmp[256];
    if (len + 1 > sizeof(tmp) || !SafeCopy(tmp, e, len + 1)) return false;
    return tmp[len] == 0 && _strnicmp(tmp, s, len) == 0;
}

bool IsProperty(const void* f) {
    std::string cn = Name(ClassOf(f));
    return cn.size() > 8 && cn.compare(cn.size() - 8, 8, "Property") == 0;
}

void* WalkChildren(const void* type, const char* name, bool property) {
    size_t len = std::strlen(name);
    int guard = 0;
    for (void* f = PtrAt(type, off.children); f && guard < 5000; f = PtrAt(f, off.fieldNext), ++guard) {
        if (!NameIs(NameAt(f, off.name), name, len)) continue;
        bool isProp = IsProperty(f);
        if (property ? isProp : !isProp) return f;
    }
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
    for (int o = off.super + 8; o <= off.super + 0x40 && off.children < 0; o += 8) {
        int guard = 0;
        for (void* f = PtrAt(actor, o); f && guard < 5000; f = PtrAt(f, off.fieldNext), ++guard) {
            if (!Valid(f)) break;
            if (NameIs(NameAt(f, off.name), "RootComponent", 13) && IsProperty(f)) {
                off.children = o;
                break;
            }
        }
    }
    if (off.children < 0) {
        err = "Children offset not found";
        return false;
    }
    off.propsSize = off.children + 8;
    void* rc = WalkChildren(actor, "RootComponent", true);
    void* pat = WalkChildren(actor, "PrimaryActorTick", true);
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
        return dim == 1 && size == 8 && ro > 0x28 && ro < 0x4000 && (ro & 7) == 0 && po >= 0x28 && po < 0x4000 && po != ro;
    };
    if (check(0x44)) off.propOffset = 0x44;
    for (int d = 0x40; d <= 0x80 && off.propOffset < 0; d += 4)
        if (check(d)) off.propOffset = d;
    if (off.propOffset < 0) {
        err = "Offset_Internal not found";
        return false;
    }
    void* setLoc = nullptr;
    for (void* c = actor; c && !setLoc; c = SuperOf(c)) setLoc = WalkChildren(c, "K2_SetActorLocation", false);
    void* ret = setLoc ? WalkChildren(setLoc, "ReturnValue", true) : nullptr;
    if (ret) {
        for (int d = 0x60; d <= 0x90; ++d) {
            uint8_t b[4];
            if (SafeRead((uint8_t*)ret + d, b) && b[0] == 1 && b[1] == 0 && b[2] != 0 && b[3] == 0xFF) {
                off.boolField = d;
                break;
            }
        }
    }
    void* structCls = FindObject("/Script/CoreUObject.Struct");
    if (structCls) SafeRead((uint8_t*)structCls + off.propsSize, off.funcFlags);
    LOG("offsets: super 0x%X children 0x%X propsSize 0x%X propOffset 0x%X bool 0x%X funcFlags 0x%X", off.super, off.children,
        off.propsSize, off.propOffset, off.boolField, off.funcFlags);
    LOG("check: Actor.RootComponent at 0x%X (exe tables say 0x168), Actor.bCanBeDamaged at 0x%X mask 0x%X (exe tables say 0x91/0x40)",
        FindProp(actor, "RootComponent").offset, FindProp(actor, "bCanBeDamaged").offset, FindProp(actor, "bCanBeDamaged").mask);
    return off.funcFlags > 0;
}

bool Match(const uint8_t* code, size_t n, int flagsOff, uint32_t mask) {
    for (size_t i = 0; i + 10 <= n; ++i) {
        if (code[i] == 0xF7 && (code[i + 1] & 0xF8) == 0x80 && code[i + 1] != 0x84 && *(int32_t*)(code + i + 2) == flagsOff &&
            *(uint32_t*)(code + i + 6) == mask)
            return true;
        for (int b = 0; b < 4; ++b) {
            uint32_t m = mask >> (b * 8);
            if ((mask & ~(0xFFu << (b * 8))) != 0 || m == 0) continue;
            if (code[i] == 0xF6 && (code[i + 1] & 0xF8) == 0x80 && code[i + 1] != 0x84 && *(int32_t*)(code + i + 2) == flagsOff + b &&
                code[i + 6] == (uint8_t)m)
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

bool SafeCopy(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool Init(std::string& error) {
    if (!names && !FindNames()) {
        error = "name table not found";
        return false;
    }
    if (!objArray && !FindObjects()) {
        error = "object table not found";
        return false;
    }
    if (off.children < 0 && !Calibrate(error)) return false;
    if (off.peIndex < 0 && !FindProcessEvent(error)) return false;
    return true;
}

void* ProcessEventAddress() { return peAddress; }

int32_t ObjectCount() {
    if (!objArray) return 0;
    return *(int32_t*)(objArray + (objChunked ? 0x14 : 0xC));
}

namespace {

uint8_t* ItemAt(int32_t i) {
    if (!objArray || i < 0 || i >= ObjectCount()) return nullptr;
    if (!objChunked) {
        uint8_t* items = *(uint8_t**)objArray;
        return items ? items + (size_t)i * off.itemSize : nullptr;
    }
    uint8_t** chunks = *(uint8_t***)objArray;
    if (!chunks) return nullptr;
    uint8_t* chunk = nullptr;
    if (!SafeRead(chunks + i / 0x10000, chunk) || !chunk) return nullptr;
    return chunk + (size_t)(i % 0x10000) * off.itemSize;
}

}

void* ObjectAt(int32_t i) {
    uint8_t* item = ItemAt(i);
    void* o = nullptr;
    if (item) SafeRead(item, o);
    return o;
}

int32_t IndexOf(const void* obj) {
    int32_t idx = -1;
    if (obj) SafeRead((const uint8_t*)obj + off.index, idx);
    return idx;
}

FWeak MakeWeak(const void* obj) {
    FWeak w;
    if (!Valid(obj)) return w;
    w.index = IndexOf(obj);
    uint8_t* item = ItemAt(w.index);
    if (item) SafeRead(item + off.serial, w.serial);
    return w;
}

void* WeakGet(const FWeak& w) {
    if (w.index <= 0) return nullptr;
    uint8_t* item = ItemAt(w.index);
    if (!item) return nullptr;
    int32_t serial = 0;
    SafeRead(item + off.serial, serial);
    if (serial != w.serial) return nullptr;
    void* o = ObjectAt(w.index);
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
    if (f & (0x8000u | 0x10000u)) return false;
    uint8_t* item = ItemAt(IndexOf(obj));
    int32_t internal = 0;
    if (item) SafeRead(item + 8, internal);
    return (internal & 0x30000000) == 0;
}

bool IsDefault(const void* obj) {
    uint32_t f = 0;
    if (obj) SafeRead((const uint8_t*)obj + off.flags, f);
    return (f & 0x30) != 0;
}

std::string NameToString(FName n) {
    const uint8_t* e;
    bool wide;
    if (!Entry(n, e, wide)) return {};
    std::string s;
    if (wide) {
        wchar_t w[256];
        for (int i = 0; i < 255; ++i) {
            if (!SafeRead(e + i * 2, w[i])) return {};
            if (!w[i]) break;
            s.push_back(w[i] < 128 ? (char)w[i] : '?');
        }
    } else {
        char c[256] = {};
        if (!SafeCopy(c, e, 255)) {
            for (int i = 0; i < 255; ++i) {
                if (!SafeRead(e + i, c[i]) || !c[i]) break;
            }
        }
        c[255] = 0;
        s = c;
    }
    if (n.number > 0) s += "_" + std::to_string(n.number - 1);
    return s;
}

std::string Name(const void* obj) { return obj ? NameToString(NameAt(obj, off.name)) : std::string(); }

FName NameOf(const void* obj) { return NameAt(obj, off.name); }

std::string Path(const void* obj) {
    if (!obj) return {};
    void* outer = PtrAt(obj, off.outer);
    if (!outer) return Name(obj);
    return Path(outer) + (PtrAt(outer, off.outer) ? ":" : ".") + Name(obj);
}

void* ClassOf(const void* obj) { return obj ? PtrAt(obj, off.cls) : nullptr; }

void* SuperOf(const void* cls) { return cls && off.super >= 0 ? PtrAt(cls, off.super) : nullptr; }

void* OuterOf(const void* obj) { return obj ? PtrAt(obj, off.outer) : nullptr; }

bool IsA(const void* obj, const void* cls) {
    if (!obj || !cls) return false;
    int guard = 0;
    for (void* c = ClassOf(obj); c && guard < 64; c = SuperOf(c), ++guard)
        if (c == cls) return true;
    return false;
}

bool ClassNameStartsWith(const void* obj, const char* prefix) {
    std::string n = Name(ClassOf(obj));
    return _strnicmp(n.c_str(), prefix, std::strlen(prefix)) == 0;
}

bool NameContains(const void* obj, const char* part) {
    std::string n = Name(obj);
    std::string p = part;
    for (char& c : n) c = (char)tolower((unsigned char)c);
    for (char& c : p) c = (char)tolower((unsigned char)c);
    return n.find(p) != std::string::npos;
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
        if (_stricmp(Name(outer).c_str(), pkg.c_str()) != 0) continue;
        std::lock_guard<std::mutex> g(cacheLock);
        objectCache[path] = obj;
        return obj;
    }
    return nullptr;
}

void* FindClass(const std::string& name) {
    std::string key = "class:" + name;
    static std::unordered_map<std::string, ULONGLONG> misses;
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = objectCache.find(key);
        if (it != objectCache.end() && Valid(it->second)) return it->second;
        auto m = misses.find(key);
        if (m != misses.end() && GetTickCount64() - m->second < 15000) return nullptr;
    }
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = ObjectAt(i);
        if (!obj || !NameIs(NameAt(obj, off.name), name.c_str(), name.size())) continue;
        std::string cn = Name(ClassOf(obj));
        if (cn != "Class" && cn != "BlueprintGeneratedClass" && cn != "ScriptStruct" && cn != "UserDefinedStruct" &&
            cn != "WidgetBlueprintGeneratedClass" && cn != "AnimBlueprintGeneratedClass")
            continue;
        std::lock_guard<std::mutex> g(cacheLock);
        objectCache[key] = obj;
        misses.erase(key);
        return obj;
    }
    std::lock_guard<std::mutex> g(cacheLock);
    misses[key] = GetTickCount64();
    return nullptr;
}

void* FindFirstOf(const void* cls) {
    if (!cls) return nullptr;
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = ObjectAt(i);
        if (obj && !IsDefault(obj) && IsA(obj, cls) && Alive(obj)) return obj;
    }
    return nullptr;
}

void ForEachOf(const void* cls, const std::function<bool(void*)>& fn) {
    if (!cls) return;
    int32_t count = ObjectCount();
    for (int32_t i = 0; i < count; ++i) {
        void* obj = ObjectAt(i);
        if (!obj || IsDefault(obj) || !IsA(obj, cls) || !Alive(obj)) continue;
        if (!fn(obj)) return;
    }
}

Prop FindProp(const void* type, const char* name) {
    if (!type || off.children < 0) return {};
    char key[340];
    FWeak tw = MakeWeak(type);
    std::snprintf(key, sizeof(key), "%p.%d.%d.%s", type, tw.index, tw.serial, name);
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = propCache.find(key);
        if (it != propCache.end()) return it->second;
    }
    Prop p;
    bool isFunc = Name(ClassOf(type)) == "Function";
    int guard = 0;
    for (const void* s = type; s && p.offset < 0 && guard < 64; s = isFunc ? nullptr : SuperOf(s), ++guard) {
        void* f = WalkChildren(s, name, true);
        if (!f) continue;
        int32_t o = 0, dim = 1, size = 0;
        SafeRead((uint8_t*)f + off.propOffset, o);
        SafeRead((uint8_t*)f + off.propOffset - 0x14, dim);
        SafeRead((uint8_t*)f + off.propOffset - 0x10, size);
        p.offset = o;
        p.size = size * (dim > 0 ? dim : 1);
        p.field = f;
        if (off.boolField >= 0 && Name(ClassOf(f)) == "BoolProperty") {
            uint8_t b[4] = {};
            SafeRead((uint8_t*)f + off.boolField, b);
            p.offset += b[1];
            p.mask = b[2];
        }
    }
    std::lock_guard<std::mutex> g(cacheLock);
    propCache[key] = p;
    return p;
}

std::string PropType(const Prop& p) { return p.field ? Name(ClassOf(p.field)) : std::string(); }

void* PropStruct(const Prop& p) { return p.field ? PtrAt(p.field, off.propExtra) : nullptr; }

void* PropClass(const Prop& p) { return p.field ? PtrAt(p.field, off.propExtra) : nullptr; }

void* FindFunction(const void* cls, const char* name) {
    if (!cls || off.children < 0) return nullptr;
    char key[340];
    FWeak cw = MakeWeak(cls);
    std::snprintf(key, sizeof(key), "%p.%d.%d.%s", cls, cw.index, cw.serial, name);
    {
        std::lock_guard<std::mutex> g(cacheLock);
        auto it = funcCache.find(key);
        if (it != funcCache.end()) return it->second;
    }
    void* found = nullptr;
    int guard = 0;
    for (const void* c = cls; c && !found && guard < 64; c = SuperOf(c), ++guard) found = WalkChildren(c, name, false);
    std::lock_guard<std::mutex> g(cacheLock);
    funcCache[key] = found;
    return found;
}

uint32_t FunctionFlags(const void* fn) {
    uint32_t f = 0;
    if (fn && off.funcFlags > 0) SafeRead((const uint8_t*)fn + off.funcFlags, f);
    return f;
}

int StructSize(const void* type) {
    int32_t size = 0;
    if (!type || off.propsSize < 0) return 0;
    SafeRead((const uint8_t*)type + off.propsSize, size);
    return size;
}

bool ReadBool(const void* obj, const Prop& p) {
    if (!obj || !p) return false;
    uint8_t b = 0;
    SafeRead((const uint8_t*)obj + p.offset, b);
    return (b & p.mask) != 0;
}

void WriteBool(void* obj, const Prop& p, bool v) {
    if (!obj || !p) return;
    uint8_t b = 0;
    if (!SafeRead((uint8_t*)obj + p.offset, b)) return;
    b = v ? (uint8_t)(b | p.mask) : (uint8_t)(b & ~p.mask);
    SafeCopy((uint8_t*)obj + p.offset, &b, 1);
}

void* Ptr(const void* obj, const char* prop) {
    if (!obj) return nullptr;
    Prop p = FindProp(ClassOf(obj), prop);
    if (!p) return nullptr;
    void* v = PtrAt(obj, p.offset);
    return Alive(v) ? v : nullptr;
}

float GetFloat(const void* obj, const char* prop, float def) {
    Prop p = obj ? FindProp(ClassOf(obj), prop) : Prop{};
    return p ? Read<float>(obj, p) : def;
}

int32_t GetInt(const void* obj, const char* prop, int32_t def) {
    Prop p = obj ? FindProp(ClassOf(obj), prop) : Prop{};
    return p ? Read<int32_t>(obj, p) : def;
}

bool GetBool(const void* obj, const char* prop) {
    Prop p = obj ? FindProp(ClassOf(obj), prop) : Prop{};
    return p && ReadBool(obj, p);
}

void SetFloat(void* obj, const char* prop, float v) {
    Prop p = obj ? FindProp(ClassOf(obj), prop) : Prop{};
    if (p) Write<float>(obj, p, v);
}

void SetInt(void* obj, const char* prop, int32_t v) {
    Prop p = obj ? FindProp(ClassOf(obj), prop) : Prop{};
    if (p) Write<int32_t>(obj, p, v);
}

void SetBool(void* obj, const char* prop, bool v) {
    Prop p = obj ? FindProp(ClassOf(obj), prop) : Prop{};
    if (p) WriteBool(obj, p, v);
}

Call::Call(void* o, const char* func) : obj(o) {
    if (Alive(o)) fn = FindFunction(ClassOf(o), func);
    if (fn) {
        uint32_t flags = FunctionFlags(fn);
        if (flags & (0x40u | 0x4000u | 0x200000u | 0x1000000u)) {
            static std::mutex warnLock;
            std::lock_guard<std::mutex> g(warnLock);
            LOG("refused to call network function %s", func);
            fn = nullptr;
        }
    }
}

Call::Call(void* o, void* func, int) : obj(o), fn(Alive(o) ? func : nullptr) {
    if (fn && (FunctionFlags(fn) & (0x40u | 0x4000u | 0x200000u | 0x1000000u))) fn = nullptr;
}

Call& Call::SetBool(const char* name, bool v) {
    Prop p = FindProp(fn, name);
    if (p && p.offset < (int)sizeof(buf)) buf[p.offset] = v ? (uint8_t)(buf[p.offset] | p.mask) : (uint8_t)(buf[p.offset] & ~p.mask);
    return *this;
}

Call& Call::SetRaw(const char* name, const void* data, int size) {
    Prop p = FindProp(fn, name);
    if (p && size > 0 && p.offset + size <= (int)sizeof(buf)) std::memcpy(buf + p.offset, data, (size_t)size);
    return *this;
}

Call& Call::SetString(const char* name, const std::wstring& s) {
    if (nstr >= 4) return *this;
    strings[nstr] = s;
    const std::wstring& k = strings[nstr++];
    FString f{k.c_str(), (int32_t)k.size() + 1, (int32_t)k.size() + 1};
    return Set(name, f);
}

bool Call::GetBool(const char* name) const {
    Prop p = FindProp(fn, name);
    return p && p.offset < (int)sizeof(buf) && (buf[p.offset] & p.mask) != 0;
}

bool Call::GetRaw(const char* name, void* out, int size) const {
    Prop p = FindProp(fn, name);
    if (!p || p.offset + size > (int)sizeof(buf)) return false;
    std::memcpy(out, buf + p.offset, (size_t)size);
    return true;
}

const uint8_t* Call::Raw(const char* name) const {
    Prop p = FindProp(fn, name);
    if (!p || p.offset >= (int)sizeof(buf)) return nullptr;
    return buf + p.offset;
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

FName MakeName(const std::wstring& s) {
    static std::mutex lock;
    static std::unordered_map<std::wstring, FName> cache;
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(s);
        if (it != cache.end()) return it->second;
    }
    Call c(Statics("/Script/Engine.Default__KismetStringLibrary"), "Conv_StringToName");
    c.SetString("InString", s);
    if (!c.Run()) return {};
    FName n = c.Get<FName>("ReturnValue");
    std::lock_guard<std::mutex> g(lock);
    cache[s] = n;
    return n;
}

std::string FStringToUtf8(const FString& s) {
    if (!s.data || s.num <= 1 || s.num > 65536) return {};
    std::wstring w;
    w.resize((size_t)s.num - 1);
    if (!SafeCopy(w.data(), s.data, w.size() * sizeof(wchar_t))) return {};
    while (!w.empty() && w.back() == 0) w.pop_back();
    return Utf8(w);
}

std::string TextToString(const void* text) {
    if (!text) return {};
    Call c(Statics("/Script/Engine.Default__KismetTextLibrary"), "Conv_TextToString");
    Prop p = FindProp(FindFunction(ClassOf(Statics("/Script/Engine.Default__KismetTextLibrary")), "Conv_TextToString"), "InText");
    if (!c.Ok() || !p || p.size <= 0 || p.size > 0x40) return {};
    uint8_t copy[0x40] = {};
    if (!SafeCopy(copy, text, (size_t)p.size)) return {};
    c.SetRaw("InText", copy, p.size);
    if (!c.Run()) return {};
    return FStringToUtf8(c.Get<FString>("ReturnValue"));
}

void* Statics(const char* path) { return FindObject(path); }

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring out((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), out.data(), n);
    return out;
}

}
