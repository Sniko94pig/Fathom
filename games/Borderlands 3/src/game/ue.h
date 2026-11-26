#pragma once
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>

namespace ue {

struct FName {
    int32_t index = 0;
    int32_t number = 0;
    bool operator==(const FName& o) const { return index == o.index && number == o.number; }
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
    float x = 0, y = 0, z = 0;
};

struct FRotator {
    float pitch = 0, yaw = 0, roll = 0;
};

struct FVector2D {
    float x = 0, y = 0;
};

struct FLinearColor {
    float r = 0, g = 0, b = 0, a = 1;
};

struct FWeak {
    int32_t index = 0;
    int32_t serial = 0;
    const void* ptr = nullptr;
};

struct Prop {
    int offset = -1;
    int size = 0;
    uint8_t mask = 0xFF;
    void* field = nullptr;
    explicit operator bool() const { return offset >= 0; }
};

bool Init(std::string& error);
void* ProcessEventAddress();
bool SafeCopy(void* dst, const void* src, size_t n);

template <class T>
bool SafeRead(const void* p, T& out) {
    return SafeCopy(&out, p, sizeof(T));
}

int32_t ObjectCount();
void* ObjectAt(int32_t i);
int32_t IndexOf(const void* obj);
FWeak MakeWeak(const void* obj);
void* WeakGet(const FWeak& w);
bool Valid(const void* obj);
bool Alive(const void* obj);
bool IsDefault(const void* obj);

std::string NameToString(FName n);
std::string Name(const void* obj);
FName NameOf(const void* obj);
std::string Path(const void* obj);
void* ClassOf(const void* obj);
void* SuperOf(const void* cls);
void* OuterOf(const void* obj);
bool IsA(const void* obj, const void* cls);
bool ClassNameStartsWith(const void* obj, const char* prefix);
bool NameContains(const void* obj, const char* part);

void* FindObject(const std::string& path);
void* FindClass(const std::string& name);
void* FindFirstOf(const void* cls);
void ForEachOf(const void* cls, const std::function<bool(void*)>& fn);
Prop FindProp(const void* type, const char* name);
std::string PropType(const Prop& p);
void* PropStruct(const Prop& p);
void* PropClass(const Prop& p);
void* FindFunction(const void* cls, const char* name);
uint32_t FunctionFlags(const void* fn);
int StructSize(const void* type);

template <class T>
T Read(const void* obj, const Prop& p) {
    T v{};
    if (obj && p) SafeCopy(&v, (const uint8_t*)obj + p.offset, sizeof(T));
    return v;
}

template <class T>
void Write(void* obj, const Prop& p, const T& v) {
    if (obj && p) SafeCopy((uint8_t*)obj + p.offset, &v, sizeof(T));
}

template <class T>
T At(const void* obj, int offset) {
    T v{};
    if (obj && offset >= 0) SafeCopy(&v, (const uint8_t*)obj + offset, sizeof(T));
    return v;
}

template <class T>
void Put(void* obj, int offset, const T& v) {
    if (obj && offset >= 0) SafeCopy((uint8_t*)obj + offset, &v, sizeof(T));
}

bool ReadBool(const void* obj, const Prop& p);
void WriteBool(void* obj, const Prop& p, bool v);

void* Ptr(const void* obj, const char* prop);
float GetFloat(const void* obj, const char* prop, float def = 0.0f);
int32_t GetInt(const void* obj, const char* prop, int32_t def = 0);
bool GetBool(const void* obj, const char* prop);
void SetFloat(void* obj, const char* prop, float v);
void SetInt(void* obj, const char* prop, int32_t v);
void SetBool(void* obj, const char* prop, bool v);

class Call {
public:
    Call(void* obj, const char* func);
    Call(void* obj, void* func, int direct);
    bool Ok() const { return obj && fn; }
    template <class T>
    Call& Set(const char* name, const T& v) {
        Prop p = FindProp(fn, name);
        if (p && p.offset + (int)sizeof(T) <= (int)sizeof(buf)) std::memcpy(buf + p.offset, &v, sizeof(T));
        return *this;
    }
    Call& SetBool(const char* name, bool v);
    Call& SetRaw(const char* name, const void* data, int size);
    Call& SetString(const char* name, const std::wstring& s);
    template <class T>
    T Get(const char* name) const {
        T v{};
        Prop p = FindProp(fn, name);
        if (p && p.offset + (int)sizeof(T) <= (int)sizeof(buf)) std::memcpy(&v, buf + p.offset, sizeof(T));
        return v;
    }
    bool GetBool(const char* name) const;
    bool GetRaw(const char* name, void* out, int size) const;
    const uint8_t* Raw(const char* name) const;
    bool Run();

private:
    void* obj = nullptr;
    void* fn = nullptr;
    alignas(16) uint8_t buf[0x800] = {};
    std::wstring strings[4];
    int nstr = 0;
};

FName MakeName(const std::wstring& s);
std::string FStringToUtf8(const FString& s);
std::string TextToString(const void* text);
void* Statics(const char* path);
std::string Utf8(const std::wstring& w);
std::wstring Wide(const std::string& s);

}
