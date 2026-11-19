#include "ingame/il2cpp.h"
#include "ingame/log.h"
#include <windows.h>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace il2 {

namespace {

struct Api {
    Domain* (*domain_get)();
    Assembly** (*domain_get_assemblies)(Domain*, size_t*);
    Image* (*assembly_get_image)(Assembly*);
    const char* (*image_get_name)(Image*);
    Thread* (*thread_attach)(Domain*);
    void (*thread_detach)(Thread*);
    Class* (*class_from_name)(Image*, const char*, const char*);
    Class* (*class_get_nested_types)(Class*, void**);
    Class* (*class_get_parent)(Class*);
    Class* (*object_get_class)(Object*);
    Class* (*class_from_type)(const Type*);
    Class* (*class_get_element_class)(Class*);
    const char* (*class_get_name)(Class*);
    const char* (*class_get_namespace)(Class*);
    bool (*class_is_subclass_of)(Class*, Class*, bool);
    bool (*class_is_assignable_from)(Class*, Class*);
    bool (*class_is_enum)(Class*);
    bool (*class_is_valuetype)(Class*);
    int32_t (*class_value_size)(Class*, uint32_t*);
    const Type* (*class_enum_basetype)(Class*);
    Method* (*class_get_method_from_name)(Class*, const char*, int);
    Method* (*class_get_methods)(Class*, void**);
    Field* (*class_get_fields)(Class*, void**);
    Property* (*class_get_property_from_name)(Class*, const char*);
    Method* (*property_get_get_method)(Property*);
    Method* (*property_get_set_method)(Property*);
    const char* (*method_get_name)(Method*);
    uint32_t (*method_get_param_count)(Method*);
    const Type* (*method_get_param)(Method*, uint32_t);
    const Type* (*method_get_return_type)(Method*);
    bool (*method_is_instance)(Method*);
    bool (*method_is_generic)(Method*);
    char* (*type_get_name)(const Type*);
    int (*type_get_type)(const Type*);
    Field* (*class_get_field_from_name)(Class*, const char*);
    size_t (*field_get_offset)(Field*);
    int (*field_get_flags)(Field*);
    const Type* (*field_get_type)(Field*);
    Class* (*field_get_parent)(Field*);
    const char* (*field_get_name)(Field*);
    void (*field_static_get_value)(Field*, void*);
    void (*field_static_set_value)(Field*, void*);
    void (*field_set_value)(Object*, Field*, void*);
    void* (*class_get_static_field_data)(Class*);
    uint32_t (*gchandle_new)(Object*, bool);
    uint32_t (*gchandle_new_weakref)(Object*, bool);
    Object* (*gchandle_get_target)(uint32_t);
    void (*gchandle_free)(uint32_t);
    uintptr_t (*array_length)(Object*);
    Object* (*array_new)(Class*, uintptr_t);
    int (*array_object_header_size)();
    Object* (*runtime_invoke)(Method*, void*, void**, Object**);
    void* (*object_unbox)(Object*);
    Object* (*value_box)(Class*, void*);
    const Type* (*class_get_type)(Class*);
    Object* (*type_get_object)(const Type*);
    Object* (*object_new)(Class*);
    Object* (*string_new)(const char*);
    int32_t (*string_length)(Object*);
    wchar_t* (*string_chars)(Object*);
    void (*format_exception)(Object*, char*, int);
    void (*gc_wbarrier_set_field)(Object*, void**, void*);
    void (*free_)(void*);
};

Api api{};
bool loaded = false;
int arrayHeader = 0x20;
std::mutex errLock;
std::unordered_map<Method*, int> errCount;
std::string lastError;

template <class T>
bool Bind(HMODULE m, T& out, const char* name, std::string& err) {
    out = (T)GetProcAddress(m, name);
    if (!out) err = std::string("missing ") + name;
    return out != nullptr;
}

template <class T>
void Optional(HMODULE m, T& out, const char* name) {
    out = (T)GetProcAddress(m, name);
}

bool SameName(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t la = std::strlen(a), lb = std::strlen(b);
    if (la >= 4 && _stricmp(a + la - 4, ".dll") == 0) la -= 4;
    if (lb >= 4 && _stricmp(b + lb - 4, ".dll") == 0) lb -= 4;
    return la == lb && _strnicmp(a, b, la) == 0;
}

std::string Narrow(const wchar_t* w, int n) {
    if (!w || n <= 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w, n, nullptr, 0, nullptr, nullptr);
    std::string s(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, n, s.data(), len, nullptr, nullptr);
    return s;
}

std::string TypeName(const Type* t) {
    if (!t) return {};
    char* n = api.type_get_name(t);
    if (!n) return {};
    std::string out(n);
    api.free_(n);
    return out;
}

Field* FieldByNames(Class* c, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (Field* f = FindField(c, n)) return f;
    return nullptr;
}

}

bool Load(std::string& err) {
    if (loaded) return true;
    HMODULE m = GetModuleHandleW(L"GameAssembly.dll");
    if (!m) {
        err = "the game's script runtime is not loaded yet";
        return false;
    }
    bool ok = Bind(m, api.domain_get, "il2cpp_domain_get", err) &&
              Bind(m, api.domain_get_assemblies, "il2cpp_domain_get_assemblies", err) &&
              Bind(m, api.assembly_get_image, "il2cpp_assembly_get_image", err) &&
              Bind(m, api.image_get_name, "il2cpp_image_get_name", err) &&
              Bind(m, api.thread_attach, "il2cpp_thread_attach", err) &&
              Bind(m, api.thread_detach, "il2cpp_thread_detach", err) &&
              Bind(m, api.class_from_name, "il2cpp_class_from_name", err) &&
              Bind(m, api.class_get_nested_types, "il2cpp_class_get_nested_types", err) &&
              Bind(m, api.class_get_parent, "il2cpp_class_get_parent", err) &&
              Bind(m, api.object_get_class, "il2cpp_object_get_class", err) &&
              Bind(m, api.class_from_type, "il2cpp_class_from_type", err) &&
              Bind(m, api.class_get_element_class, "il2cpp_class_get_element_class", err) &&
              Bind(m, api.class_get_name, "il2cpp_class_get_name", err) &&
              Bind(m, api.class_get_namespace, "il2cpp_class_get_namespace", err) &&
              Bind(m, api.class_is_subclass_of, "il2cpp_class_is_subclass_of", err) &&
              Bind(m, api.class_is_assignable_from, "il2cpp_class_is_assignable_from", err) &&
              Bind(m, api.class_is_enum, "il2cpp_class_is_enum", err) &&
              Bind(m, api.class_is_valuetype, "il2cpp_class_is_valuetype", err) &&
              Bind(m, api.class_value_size, "il2cpp_class_value_size", err) &&
              Bind(m, api.class_enum_basetype, "il2cpp_class_enum_basetype", err) &&
              Bind(m, api.class_get_method_from_name, "il2cpp_class_get_method_from_name", err) &&
              Bind(m, api.class_get_methods, "il2cpp_class_get_methods", err) &&
              Bind(m, api.class_get_fields, "il2cpp_class_get_fields", err) &&
              Bind(m, api.class_get_property_from_name, "il2cpp_class_get_property_from_name", err) &&
              Bind(m, api.property_get_get_method, "il2cpp_property_get_get_method", err) &&
              Bind(m, api.property_get_set_method, "il2cpp_property_get_set_method", err) &&
              Bind(m, api.method_get_name, "il2cpp_method_get_name", err) &&
              Bind(m, api.method_get_param_count, "il2cpp_method_get_param_count", err) &&
              Bind(m, api.method_get_param, "il2cpp_method_get_param", err) &&
              Bind(m, api.method_get_return_type, "il2cpp_method_get_return_type", err) &&
              Bind(m, api.method_is_instance, "il2cpp_method_is_instance", err) &&
              Bind(m, api.type_get_name, "il2cpp_type_get_name", err) &&
              Bind(m, api.type_get_type, "il2cpp_type_get_type", err) &&
              Bind(m, api.class_get_field_from_name, "il2cpp_class_get_field_from_name", err) &&
              Bind(m, api.field_get_offset, "il2cpp_field_get_offset", err) &&
              Bind(m, api.field_get_flags, "il2cpp_field_get_flags", err) &&
              Bind(m, api.field_get_type, "il2cpp_field_get_type", err) &&
              Bind(m, api.field_get_parent, "il2cpp_field_get_parent", err) &&
              Bind(m, api.field_get_name, "il2cpp_field_get_name", err) &&
              Bind(m, api.field_static_get_value, "il2cpp_field_static_get_value", err) &&
              Bind(m, api.field_static_set_value, "il2cpp_field_static_set_value", err) &&
              Bind(m, api.field_set_value, "il2cpp_field_set_value", err) &&
              Bind(m, api.gchandle_new, "il2cpp_gchandle_new", err) &&
              Bind(m, api.gchandle_new_weakref, "il2cpp_gchandle_new_weakref", err) &&
              Bind(m, api.gchandle_get_target, "il2cpp_gchandle_get_target", err) &&
              Bind(m, api.gchandle_free, "il2cpp_gchandle_free", err) &&
              Bind(m, api.array_length, "il2cpp_array_length", err) && Bind(m, api.array_new, "il2cpp_array_new", err) &&
              Bind(m, api.runtime_invoke, "il2cpp_runtime_invoke", err) &&
              Bind(m, api.object_unbox, "il2cpp_object_unbox", err) && Bind(m, api.value_box, "il2cpp_value_box", err) &&
              Bind(m, api.class_get_type, "il2cpp_class_get_type", err) &&
              Bind(m, api.type_get_object, "il2cpp_type_get_object", err) &&
              Bind(m, api.object_new, "il2cpp_object_new", err) && Bind(m, api.string_new, "il2cpp_string_new", err) &&
              Bind(m, api.string_length, "il2cpp_string_length", err) &&
              Bind(m, api.string_chars, "il2cpp_string_chars", err) && Bind(m, api.free_, "il2cpp_free", err);
    if (!ok) return false;
    Optional(m, api.array_object_header_size, "il2cpp_array_object_header_size");
    Optional(m, api.class_get_static_field_data, "il2cpp_class_get_static_field_data");
    Optional(m, api.format_exception, "il2cpp_format_exception");
    Optional(m, api.method_is_generic, "il2cpp_method_is_generic");
    Optional(m, api.gc_wbarrier_set_field, "il2cpp_gc_wbarrier_set_field");
    Domain* d = api.domain_get();
    size_t count = 0;
    Assembly** list = d ? api.domain_get_assemblies(d, &count) : nullptr;
    if (!d || !list || count == 0) {
        err = "the game's script runtime is still starting";
        return false;
    }
    if (api.array_object_header_size) {
        int h = api.array_object_header_size();
        if (h > 0 && h < 0x100) arrayHeader = h;
    }
    loaded = true;
    return true;
}

bool Ready() { return loaded; }

Thread* Attach() { return loaded ? api.thread_attach(api.domain_get()) : nullptr; }

void Detach(Thread* t) {
    if (loaded && t) api.thread_detach(t);
}

Image* FindImage(const char* assembly) {
    if (!loaded || !assembly) return nullptr;
    size_t count = 0;
    Assembly** list = api.domain_get_assemblies(api.domain_get(), &count);
    for (size_t i = 0; list && i < count; ++i) {
        Image* img = api.assembly_get_image(list[i]);
        if (img && SameName(api.image_get_name(img), assembly)) return img;
    }
    return nullptr;
}

Class* FindClass(Image* image, const char* ns, const char* name) {
    if (!image || !name) return nullptr;
    const char* slash = std::strchr(name, '/');
    if (!slash) return api.class_from_name(image, ns ? ns : "", name);
    std::string outer(name, slash - name);
    Class* c = api.class_from_name(image, ns ? ns : "", outer.c_str());
    const char* rest = slash + 1;
    while (c && rest && *rest) {
        const char* next = std::strchr(rest, '/');
        std::string part = next ? std::string(rest, next - rest) : std::string(rest);
        c = Nested(c, part.c_str());
        rest = next ? next + 1 : nullptr;
    }
    return c;
}

Class* FindClass(const char* assembly, const char* ns, const char* name) { return FindClass(FindImage(assembly), ns, name); }

Class* Nested(Class* outer, const char* name) {
    if (!outer || !name) return nullptr;
    void* iter = nullptr;
    while (Class* n = api.class_get_nested_types(outer, &iter))
        if (std::strcmp(api.class_get_name(n), name) == 0) return n;
    return nullptr;
}

Class* Parent(Class* c) { return c ? api.class_get_parent(c) : nullptr; }

Class* ClassOf(Object* o) { return o ? api.object_get_class(o) : nullptr; }

Class* TypeClass(const Type* t) { return t ? api.class_from_type(t) : nullptr; }

Class* ElementClass(Class* c) { return c ? api.class_get_element_class(c) : nullptr; }

const char* ClassName(Class* c) { return c ? api.class_get_name(c) : ""; }

const char* ClassNamespace(Class* c) { return c ? api.class_get_namespace(c) : ""; }

bool IsSubclass(Class* c, Class* parent) {
    if (!c || !parent) return false;
    return c == parent || api.class_is_subclass_of(c, parent, true) || api.class_is_assignable_from(parent, c);
}

bool IsEnum(Class* c) { return c && api.class_is_enum(c); }

bool IsValueType(Class* c) { return c && api.class_is_valuetype(c); }

int ValueSize(Class* c) {
    if (!c) return 0;
    uint32_t align = 0;
    return api.class_value_size(c, &align);
}

Method* FindMethod(Class* c, const char* name, int args) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        void* iter = nullptr;
        while (Method* m = api.class_get_methods(k, &iter)) {
            if (std::strcmp(api.method_get_name(m), name) != 0) continue;
            if (api.method_is_generic && api.method_is_generic(m)) continue;
            if (args >= 0 && (int)api.method_get_param_count(m) != args) continue;
            return m;
        }
    }
    return nullptr;
}

Method* FindMethod(Class* c, const char* name, const std::vector<std::string>& params) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        void* iter = nullptr;
        while (Method* m = api.class_get_methods(k, &iter)) {
            if (std::strcmp(api.method_get_name(m), name) != 0) continue;
            if (api.method_is_generic && api.method_is_generic(m)) continue;
            if (api.method_get_param_count(m) != params.size()) continue;
            bool match = true;
            for (uint32_t i = 0; i < params.size() && match; ++i) match = TypeName(api.method_get_param(m, i)) == params[i];
            if (match) return m;
        }
    }
    return nullptr;
}

Method* FindStaticMethod(Class* c, const char* name, int args) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        void* iter = nullptr;
        while (Method* m = api.class_get_methods(k, &iter)) {
            if (std::strcmp(api.method_get_name(m), name) != 0 || api.method_is_instance(m)) continue;
            if (api.method_is_generic && api.method_is_generic(m)) continue;
            if (args >= 0 && (int)api.method_get_param_count(m) != args) continue;
            return m;
        }
    }
    return nullptr;
}

int ParamCount(Method* m) { return m ? (int)api.method_get_param_count(m) : -1; }

Class* ParamClass(Method* m, int index) {
    if (!m || index < 0 || index >= ParamCount(m)) return nullptr;
    return api.class_from_type(api.method_get_param(m, (uint32_t)index));
}

std::string ParamTypeName(Method* m, int index) {
    if (!m || index < 0 || index >= ParamCount(m)) return {};
    return TypeName(api.method_get_param(m, (uint32_t)index));
}

Class* ReturnClass(Method* m) {
    if (!m) return nullptr;
    const Type* t = api.method_get_return_type(m);
    return t ? api.class_from_type(t) : nullptr;
}

const char* MethodName(Method* m) { return m ? api.method_get_name(m) : ""; }

bool IsInstance(Method* m) { return m && api.method_is_instance(m); }

void* MethodPointer(Method* m) { return m ? *(void**)m : nullptr; }

Field* FindField(Class* c, const char* name) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        Field* f = api.class_get_field_from_name(k, name);
        if (f) return f;
    }
    return nullptr;
}

int Offset(Field* f) { return f ? (int)api.field_get_offset(f) : -1; }

bool IsStatic(Field* f) { return f && (api.field_get_flags(f) & 0x10) != 0; }

Class* FieldClass(Field* f) { return f ? api.class_from_type(api.field_get_type(f)) : nullptr; }

std::string FieldTypeName(Field* f) { return f ? TypeName(api.field_get_type(f)) : std::string(); }

Method* Getter(Class* c, const char* property) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        Property* p = api.class_get_property_from_name(k, property);
        if (p) {
            Method* m = api.property_get_get_method(p);
            if (m) return m;
        }
    }
    std::string n = std::string("get_") + property;
    return FindMethod(c, n.c_str(), 0);
}

Method* Setter(Class* c, const char* property) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        Property* p = api.class_get_property_from_name(k, property);
        if (p) {
            Method* m = api.property_get_set_method(p);
            if (m) return m;
        }
    }
    std::string n = std::string("set_") + property;
    return FindMethod(c, n.c_str(), 1);
}

Object* Invoke(Method* m, void* self, void** args, bool* ok) {
    if (ok) *ok = false;
    if (!m || !loaded) return nullptr;
    Object* exc = nullptr;
    Object* r = api.runtime_invoke(m, self, args, &exc);
    if (exc) {
        int n;
        char buf[512] = {0};
        if (api.format_exception) api.format_exception(exc, buf, sizeof(buf) - 1);
        {
            std::lock_guard<std::mutex> g(errLock);
            n = ++errCount[m];
            lastError = buf[0] ? buf : api.class_get_name(api.object_get_class(exc));
        }
        if (n <= 3) LOG("%s threw %s", api.method_get_name(m), buf[0] ? buf : api.class_get_name(api.object_get_class(exc)));
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

void* Unbox(Object* o) { return o ? api.object_unbox(o) : nullptr; }

Object* Box(Class* c, void* data) { return c && data ? api.value_box(c, data) : nullptr; }

Object* TypeObject(Class* c) { return c ? api.type_get_object(api.class_get_type(c)) : nullptr; }

Object* New(Class* c) { return c ? api.object_new(c) : nullptr; }

Object* NewString(const char* utf8) { return api.string_new(utf8 ? utf8 : ""); }

std::string Str(Object* s) {
    if (!s) return {};
    int32_t n = api.string_length(s);
    wchar_t* w = api.string_chars(s);
    return Narrow(w, n);
}

long long EnumValue(Class* enumClass, const char* name, long long fallback) {
    if (!enumClass || !name) return fallback;
    Field* f = api.class_get_field_from_name(enumClass, name);
    if (!f || !IsStatic(f)) return fallback;
    unsigned char buf[16] = {0};
    api.field_static_get_value(f, buf);
    int kind = 0x08;
    if (const Type* bt = api.class_enum_basetype(enumClass)) kind = api.type_get_type(bt);
    switch (kind) {
    case 0x04: return *(int8_t*)buf;
    case 0x05: return *(uint8_t*)buf;
    case 0x06: return *(int16_t*)buf;
    case 0x07: return *(uint16_t*)buf;
    case 0x09: return *(uint32_t*)buf;
    case 0x0a: return *(int64_t*)buf;
    case 0x0b: return (long long)*(uint64_t*)buf;
    default: return *(int32_t*)buf;
    }
}

std::string EnumName(Class* enumClass, long long value) {
    if (!enumClass) return {};
    void* iter = nullptr;
    while (Field* f = api.class_get_fields(enumClass, &iter)) {
        if (!IsStatic(f)) continue;
        const char* n = api.field_get_name(f);
        if (n && EnumValue(enumClass, n, value - 1) == value) return n;
    }
    return {};
}

bool StaticGet(Field* f, void* out, size_t size) {
    if (!f || !out) return false;
    std::memset(out, 0, size);
    if (api.class_get_static_field_data) {
        Class* owner = api.field_get_parent(f);
        if (!owner || !api.class_get_static_field_data(owner)) return false;
    }
    api.field_static_get_value(f, out);
    return true;
}

bool StaticSet(Field* f, void* in) {
    if (!f || !in) return false;
    if (api.class_get_static_field_data) {
        Class* owner = api.field_get_parent(f);
        if (!owner || !api.class_get_static_field_data(owner)) return false;
    }
    api.field_static_set_value(f, in);
    return true;
}

void SetRef(Object* o, Field* f, Object* value) {
    if (o && f) api.field_set_value(o, f, value);
}

void StoreRef(Object* owner, void* slot, Object* value) {
    if (!slot) return;
    if (api.gc_wbarrier_set_field) api.gc_wbarrier_set_field(owner, (void**)slot, value);
    else *(Object**)slot = value;
}

uint32_t Pin(Object* o) { return o ? api.gchandle_new(o, false) : 0; }

uint32_t Weak(Object* o) { return o ? api.gchandle_new_weakref(o, false) : 0; }

Object* Pinned(uint32_t h) { return h ? api.gchandle_get_target(h) : nullptr; }

void Unpin(uint32_t h) {
    if (h) api.gchandle_free(h);
}

size_t Length(Object* array) { return array ? (size_t)api.array_length(array) : 0; }

Object* At(Object* array, size_t i) {
    if (!array || i >= Length(array)) return nullptr;
    return ((Object**)((char*)array + arrayHeader))[i];
}

void* Data(Object* array) { return array ? (char*)array + arrayHeader : nullptr; }

Object* NewArray(Class* elem, size_t n) { return elem ? api.array_new(elem, (uintptr_t)n) : nullptr; }

std::vector<Object*> ListItems(Object* list) {
    std::vector<Object*> out;
    if (!list) return out;
    Class* c = ClassOf(list);
    Field* items = FieldByNames(c, {"_items", "items"});
    Field* size = FieldByNames(c, {"_size", "size"});
    if (!items || !size) return out;
    Object* arr = Get<Object*>(list, Offset(items));
    int n = Get<int>(list, Offset(size));
    size_t len = Length(arr);
    if (n < 0 || (size_t)n > len) n = (int)len;
    out.reserve((size_t)n);
    for (int i = 0; i < n; ++i) out.push_back(At(arr, (size_t)i));
    return out;
}

int ListCount(Object* list) {
    if (!list) return 0;
    Field* size = FieldByNames(ClassOf(list), {"_size", "size"});
    return size ? Get<int>(list, Offset(size)) : 0;
}

std::vector<std::pair<Object*, Object*>> DictEntries(Object* dict) {
    std::vector<std::pair<Object*, Object*>> out;
    if (!dict) return out;
    Class* c = ClassOf(dict);
    Field* entries = FieldByNames(c, {"_entries", "entries"});
    Field* count = FieldByNames(c, {"_count", "count"});
    if (!entries || !count) return out;
    Object* arr = Get<Object*>(dict, Offset(entries));
    int n = Get<int>(dict, Offset(count));
    if (!arr || n <= 0) return out;
    Class* entry = ElementClass(ClassOf(arr));
    Field* fk = entry ? FindField(entry, "key") : nullptr;
    Field* fv = entry ? FindField(entry, "value") : nullptr;
    Field* fh = entry ? FindField(entry, "hashCode") : nullptr;
    int stride = entry ? ValueSize(entry) : 0;
    if (!fk || !fv || stride <= 0) return out;
    int ok = Offset(fk) - 0x10, ov = Offset(fv) - 0x10, oh = fh ? Offset(fh) - 0x10 : -1;
    bool refKey = !IsValueType(FieldClass(fk)), refVal = !IsValueType(FieldClass(fv));
    size_t len = Length(arr);
    char* base = (char*)Data(arr);
    for (int i = 0; i < n && (size_t)i < len; ++i) {
        char* e = base + (size_t)i * (size_t)stride;
        if (oh >= 0 && *(int*)(e + oh) < 0) continue;
        Object* k = refKey ? *(Object**)(e + ok) : (Object*)(e + ok);
        Object* v = refVal ? *(Object**)(e + ov) : (Object*)(e + ov);
        out.emplace_back(k, v);
    }
    return out;
}

std::string LastException() {
    std::lock_guard<std::mutex> g(errLock);
    return lastError;
}

}
