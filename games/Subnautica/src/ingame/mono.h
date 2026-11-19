#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mono {

struct Domain;
struct Assembly;
struct Image;
struct Class;
struct Method;
struct Field;
struct Object;
struct VTable;
struct Type;
struct Thread;
struct Sig;

bool Load(std::string& err);
bool Ready();
Domain* RootDomain();
Domain* CurrentDomain();
Thread* Attach();
void Detach(Thread* t);

Image* FindImage(const char* assembly);
Image* Corlib();
Class* FindClass(Image* image, const char* ns, const char* name);
Class* Parent(Class* c);
Class* ClassOf(Object* o);
Class* TypeClass(Type* t);
const char* ClassName(Class* c);
bool IsSubclass(Class* c, Class* parent);
Method* FindMethod(Class* c, const char* name, int args);
Method* FindMethod(Class* c, const char* name, const std::vector<std::string>& params);
Field* FindField(Class* c, const char* name);
int Offset(Field* f);
Class* ParamClass(Method* m, int index);
Class* FieldClass(Field* f);

Object* Invoke(Method* m, void* self, void** args, bool* ok = nullptr);
void* Unbox(Object* o);
Object* TypeObject(Class* c);
Object* New(Class* c);
Object* NewString(const char* utf8);
std::string Str(Object* s);
int EnumValue(Class* enumClass, const char* name);

void StaticGet(Class* c, Field* f, void* out);
void StaticSet(Class* c, Field* f, void* in);
void SetRef(Object* o, Field* f, Object* value);

uint32_t Pin(Object* o);
Object* Pinned(uint32_t h);
void Unpin(uint32_t h);

size_t Length(Object* array);
Object* At(Object* array, size_t i);
void* Data(Object* array, int elemSize);
Object* NewArray(Class* elem, size_t n);

template <class T>
T Get(Object* o, int off) {
    return *(T*)((char*)o + off);
}

template <class T>
void Put(Object* o, int off, T v) {
    *(T*)((char*)o + off) = v;
}

template <class T>
T Result(Object* boxed, T fallback = T{}) {
    if (!boxed) return fallback;
    void* p = Unbox(boxed);
    return p ? *(T*)p : fallback;
}

}
