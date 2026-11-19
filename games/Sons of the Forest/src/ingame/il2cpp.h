#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace il2 {

struct Domain;
struct Assembly;
struct Image;
struct Class;
struct Method;
struct Field;
struct Object;
struct Type;
struct Thread;
struct Property;

bool Load(std::string& err);
bool Ready();
Thread* Attach();
void Detach(Thread* t);

Image* FindImage(const char* assembly);
Class* FindClass(Image* image, const char* ns, const char* name);
Class* FindClass(const char* assembly, const char* ns, const char* name);
Class* Nested(Class* outer, const char* name);
Class* Parent(Class* c);
Class* ClassOf(Object* o);
Class* TypeClass(const Type* t);
Class* ElementClass(Class* arrayOrList);
const char* ClassName(Class* c);
const char* ClassNamespace(Class* c);
bool IsSubclass(Class* c, Class* parent);
bool IsEnum(Class* c);
bool IsValueType(Class* c);
int ValueSize(Class* c);

Method* FindMethod(Class* c, const char* name, int args);
Method* FindMethod(Class* c, const char* name, const std::vector<std::string>& params);
Method* FindStaticMethod(Class* c, const char* name, int args);
int ParamCount(Method* m);
Class* ParamClass(Method* m, int index);
std::string ParamTypeName(Method* m, int index);
Class* ReturnClass(Method* m);
const char* MethodName(Method* m);
bool IsInstance(Method* m);
void* MethodPointer(Method* m);

Field* FindField(Class* c, const char* name);
int Offset(Field* f);
bool IsStatic(Field* f);
Class* FieldClass(Field* f);
std::string FieldTypeName(Field* f);
Method* Getter(Class* c, const char* property);
Method* Setter(Class* c, const char* property);

Object* Invoke(Method* m, void* self, void** args, bool* ok = nullptr);
void* Unbox(Object* o);
Object* Box(Class* c, void* data);
Object* TypeObject(Class* c);
Object* New(Class* c);
Object* NewString(const char* utf8);
std::string Str(Object* s);
long long EnumValue(Class* enumClass, const char* name, long long fallback = -1);
std::string EnumName(Class* enumClass, long long value);

bool StaticGet(Field* f, void* out, size_t size);
bool StaticSet(Field* f, void* in);
void SetRef(Object* o, Field* f, Object* value);
void StoreRef(Object* owner, void* slot, Object* value);

uint32_t Pin(Object* o);
uint32_t Weak(Object* o);
Object* Pinned(uint32_t h);
void Unpin(uint32_t h);

size_t Length(Object* array);
Object* At(Object* array, size_t i);
void* Data(Object* array);
Object* NewArray(Class* elem, size_t n);

std::vector<Object*> ListItems(Object* list);
int ListCount(Object* list);
std::vector<std::pair<Object*, Object*>> DictEntries(Object* dict);

std::string LastException();

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
