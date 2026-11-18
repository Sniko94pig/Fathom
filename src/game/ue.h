#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace ue {

struct FName {
    int32_t index = 0;
    int32_t number = 0;
};

template <class T>
struct TArray {
    T* data = nullptr;
    int32_t num = 0;
    int32_t max = 0;
};

struct FString {
    const wchar_t* data = nullptr;
    int32_t num = 0;
    int32_t max = 0;
};

struct FVector {
    double x = 0, y = 0, z = 0;
};

struct FRotator {
    double pitch = 0, yaw = 0, roll = 0;
};

struct Prop {
    int offset = -1;
    int size = 0;
    uint8_t mask = 0xFF;
    explicit operator bool() const { return offset >= 0; }
};

bool Init(std::string& error);
void* ProcessEventAddress();

int32_t ObjectCount();
void* ObjectAt(int32_t i);
int32_t SerialAt(int32_t i);
void* WeakGet(int32_t index, int32_t serial);
bool Valid(const void* obj);
bool Alive(const void* obj);
bool IsDefault(const void* obj);
bool IsTemplate(const void* obj);

std::string NameToString(FName n);
std::string Name(const void* obj);
std::string Path(const void* obj);
void* ClassOf(const void* obj);
void* SuperOf(const void* cls);
bool IsA(const void* obj, const void* cls);

void* FindObject(const std::string& path);
void* FindFirstOf(const void* cls);
Prop FindProp(const void* type, const char* name);
void* FindFunction(const void* cls, const char* name);

template <class T>
T Read(const void* obj, const Prop& p) {
    T v{};
    if (obj && p) std::memcpy(&v, (const uint8_t*)obj + p.offset, sizeof(T));
    return v;
}

template <class T>
void Write(void* obj, const Prop& p, const T& v) {
    if (obj && p) std::memcpy((uint8_t*)obj + p.offset, &v, sizeof(T));
}

bool ReadBool(const void* obj, const Prop& p);
void WriteBool(void* obj, const Prop& p, bool v);

void* Ptr(const void* obj, const char* prop);

class Call {
public:
    Call(void* obj, const char* func);
    Call(void* obj, void* cls, const char* func);
    Call(void* obj, void* func, int direct);
    bool Ok() const { return obj && fn; }
    template <class T>
    Call& Set(const char* name, const T& v) {
        Prop p = FindProp(fn, name);
        if (p && p.offset + (int)sizeof(T) <= (int)sizeof(buf)) std::memcpy(buf + p.offset, &v, sizeof(T));
        return *this;
    }
    Call& SetBool(const char* name, bool v);
    Call& SetRaw(const char* name, const void* data, int size) {
        Prop p = FindProp(fn, name);
        if (p && p.offset + size <= (int)sizeof(buf)) std::memcpy(buf + p.offset, data, (size_t)size);
        return *this;
    }
    bool GetRaw(const char* name, void* out, int size) const {
        Prop p = FindProp(fn, name);
        if (!p || p.offset + size > (int)sizeof(buf)) return false;
        std::memcpy(out, buf + p.offset, (size_t)size);
        return true;
    }
    int ParamSize(const char* name) const { return FindProp(fn, name).size; }
    Call& SetFirstRaw(const void* data, int size);
    Call& SetString(const char* name, const std::wstring& s);
    template <class T>
    T Get(const char* name) const {
        T v{};
        Prop p = FindProp(fn, name);
        if (p && p.offset + (int)sizeof(T) <= (int)sizeof(buf)) std::memcpy(&v, buf + p.offset, sizeof(T));
        return v;
    }
    bool GetBool(const char* name) const;
    bool Run();

private:
    void* obj = nullptr;
    void* fn = nullptr;
    alignas(16) uint8_t buf[0x800] = {};
    std::wstring strings[4];
    int nstr = 0;
};

void* FindClassByName(const std::string& name);
int StructSize(const void* type);
std::string FStringToUtf8(const FString& s);

FName MakeName(const std::wstring& s);
void* Statics(const char* path);

}
