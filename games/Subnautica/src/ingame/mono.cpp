#include "ingame/mono.h"
#include "ingame/log.h"
#include <windows.h>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace mono {

namespace {

using GFunc = void (*)(void*, void*);

struct Api {
    Domain* (*get_root_domain)();
    Domain* (*domain_get)();
    Thread* (*thread_attach)(Domain*);
    void (*thread_detach)(Thread*);
    void (*assembly_foreach)(GFunc, void*);
    Image* (*assembly_get_image)(Assembly*);
    const char* (*image_get_name)(Image*);
    Image* (*get_corlib)();
    Class* (*class_from_name)(Image*, const char*, const char*);
    Class* (*class_get_parent)(Class*);
    Class* (*object_get_class)(Object*);
    Class* (*class_from_mono_type)(Type*);
    const char* (*class_get_name)(Class*);
    int (*class_is_subclass_of)(Class*, Class*, int);
    Method* (*class_get_method_from_name)(Class*, const char*, int);
    const char* (*method_get_name)(Method*);
    Field* (*class_get_field_from_name)(Class*, const char*);
    uint32_t (*field_get_offset)(Field*);
    Type* (*field_get_type)(Field*);
    Sig* (*method_signature)(Method*);
    Type* (*signature_get_params)(Sig*, void**);
    uint32_t (*signature_get_param_count)(Sig*);
    Method* (*class_get_methods)(Class*, void**);
    char* (*type_get_name)(Type*);
    Object* (*runtime_invoke)(Method*, void*, void**, Object**);
    void* (*object_unbox)(Object*);
    Type* (*class_get_type)(Class*);
    Object* (*type_get_object)(Domain*, Type*);
    Object* (*object_new)(Domain*, Class*);
    Object* (*string_new)(Domain*, const char*);
    char* (*string_to_utf8)(Object*);
    void (*free)(void*);
    VTable* (*class_vtable)(Domain*, Class*);
    void (*field_static_get_value)(VTable*, Field*, void*);
    void (*field_static_set_value)(VTable*, Field*, void*);
    void (*field_set_value)(Object*, Field*, void*);
    uint32_t (*gchandle_new)(Object*, int);
    Object* (*gchandle_get_target)(uint32_t);
    void (*gchandle_free)(uint32_t);
    uintptr_t (*array_length)(Object*);
    char* (*array_addr_with_size)(Object*, int, uintptr_t);
    Object* (*array_new)(Domain*, Class*, uintptr_t);
};

Api api{};
bool loaded = false;
std::mutex errLock;
std::unordered_map<Method*, int> errCount;

template <class T>
bool Bind(HMODULE m, T& out, const char* name, std::string& err) {
    out = (T)GetProcAddress(m, name);
    if (!out) err = std::string("missing ") + name;
    return out != nullptr;
}

struct FindCtx {
    const char* name;
    Image* found;
};

bool SameName(const char* a, const char* b) {
    if (!a || !b) return false;
    size_t la = std::strlen(a), lb = std::strlen(b);
    if (la >= 4 && _stricmp(a + la - 4, ".dll") == 0) la -= 4;
    if (lb >= 4 && _stricmp(b + lb - 4, ".dll") == 0) lb -= 4;
    return la == lb && _strnicmp(a, b, la) == 0;
}

void FindEach(void* data, void* user) {
    FindCtx* ctx = (FindCtx*)user;
    if (ctx->found) return;
    Image* img = api.assembly_get_image((Assembly*)data);
    if (img && SameName(api.image_get_name(img), ctx->name)) ctx->found = img;
}

Domain* Dom() {
    Domain* d = api.domain_get ? api.domain_get() : nullptr;
    return d ? d : api.get_root_domain();
}

}

bool Load(std::string& err) {
    if (loaded) return true;
    HMODULE m = GetModuleHandleW(L"mono-2.0-bdwgc.dll");
    if (!m) m = GetModuleHandleW(L"mono-2.0-sgen.dll");
    if (!m) m = GetModuleHandleW(L"mono.dll");
    if (!m) {
        err = "the game's script runtime is not loaded yet";
        return false;
    }
    bool ok = Bind(m, api.get_root_domain, "mono_get_root_domain", err) && Bind(m, api.domain_get, "mono_domain_get", err) &&
              Bind(m, api.thread_attach, "mono_thread_attach", err) && Bind(m, api.thread_detach, "mono_thread_detach", err) &&
              Bind(m, api.assembly_foreach, "mono_assembly_foreach", err) &&
              Bind(m, api.assembly_get_image, "mono_assembly_get_image", err) &&
              Bind(m, api.image_get_name, "mono_image_get_name", err) && Bind(m, api.get_corlib, "mono_get_corlib", err) &&
              Bind(m, api.class_from_name, "mono_class_from_name", err) &&
              Bind(m, api.class_get_parent, "mono_class_get_parent", err) &&
              Bind(m, api.object_get_class, "mono_object_get_class", err) &&
              Bind(m, api.class_from_mono_type, "mono_class_from_mono_type", err) &&
              Bind(m, api.class_get_name, "mono_class_get_name", err) &&
              Bind(m, api.class_is_subclass_of, "mono_class_is_subclass_of", err) &&
              Bind(m, api.class_get_method_from_name, "mono_class_get_method_from_name", err) &&
              Bind(m, api.method_get_name, "mono_method_get_name", err) &&
              Bind(m, api.class_get_field_from_name, "mono_class_get_field_from_name", err) &&
              Bind(m, api.field_get_offset, "mono_field_get_offset", err) && Bind(m, api.field_get_type, "mono_field_get_type", err) &&
              Bind(m, api.method_signature, "mono_method_signature", err) &&
              Bind(m, api.signature_get_params, "mono_signature_get_params", err) &&
              Bind(m, api.signature_get_param_count, "mono_signature_get_param_count", err) &&
              Bind(m, api.class_get_methods, "mono_class_get_methods", err) &&
              Bind(m, api.type_get_name, "mono_type_get_name", err) &&
              Bind(m, api.runtime_invoke, "mono_runtime_invoke", err) && Bind(m, api.object_unbox, "mono_object_unbox", err) &&
              Bind(m, api.class_get_type, "mono_class_get_type", err) && Bind(m, api.type_get_object, "mono_type_get_object", err) &&
              Bind(m, api.object_new, "mono_object_new", err) && Bind(m, api.string_new, "mono_string_new", err) &&
              Bind(m, api.string_to_utf8, "mono_string_to_utf8", err) && Bind(m, api.free, "mono_free", err) &&
              Bind(m, api.class_vtable, "mono_class_vtable", err) &&
              Bind(m, api.field_static_get_value, "mono_field_static_get_value", err) &&
              Bind(m, api.field_static_set_value, "mono_field_static_set_value", err) &&
              Bind(m, api.field_set_value, "mono_field_set_value", err) && Bind(m, api.gchandle_new, "mono_gchandle_new", err) &&
              Bind(m, api.gchandle_get_target, "mono_gchandle_get_target", err) &&
              Bind(m, api.gchandle_free, "mono_gchandle_free", err) && Bind(m, api.array_length, "mono_array_length", err) &&
              Bind(m, api.array_addr_with_size, "mono_array_addr_with_size", err) &&
              Bind(m, api.array_new, "mono_array_new", err);
    if (!ok) return false;
    if (!api.get_root_domain()) {
        err = "the game's script runtime is still starting";
        return false;
    }
    loaded = true;
    return true;
}

bool Ready() { return loaded; }

Domain* RootDomain() { return loaded ? api.get_root_domain() : nullptr; }

Domain* CurrentDomain() { return loaded ? Dom() : nullptr; }

Thread* Attach() { return loaded ? api.thread_attach(api.get_root_domain()) : nullptr; }

void Detach(Thread* t) {
    if (loaded && t) api.thread_detach(t);
}

Image* FindImage(const char* assembly) {
    FindCtx ctx{assembly, nullptr};
    api.assembly_foreach(&FindEach, &ctx);
    return ctx.found;
}

Image* Corlib() { return api.get_corlib(); }

Class* FindClass(Image* image, const char* ns, const char* name) {
    if (!image) return nullptr;
    return api.class_from_name(image, ns, name);
}

Class* Parent(Class* c) { return c ? api.class_get_parent(c) : nullptr; }

Class* ClassOf(Object* o) { return o ? api.object_get_class(o) : nullptr; }

Class* TypeClass(Type* t) { return t ? api.class_from_mono_type(t) : nullptr; }

const char* ClassName(Class* c) { return c ? api.class_get_name(c) : ""; }

bool IsSubclass(Class* c, Class* parent) {
    if (!c || !parent) return false;
    return c == parent || api.class_is_subclass_of(c, parent, 0) != 0;
}

Method* FindMethod(Class* c, const char* name, int args) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        Method* m = api.class_get_method_from_name(k, name, args);
        if (m) return m;
    }
    return nullptr;
}

Method* FindMethod(Class* c, const char* name, const std::vector<std::string>& params) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        void* iter = nullptr;
        while (Method* m = api.class_get_methods(k, &iter)) {
            if (std::strcmp(api.method_get_name(m), name) != 0) continue;
            Sig* s = api.method_signature(m);
            if (!s || api.signature_get_param_count(s) != params.size()) continue;
            void* pit = nullptr;
            bool match = true;
            for (const std::string& want : params) {
                Type* t = api.signature_get_params(s, &pit);
                char* tn = t ? api.type_get_name(t) : nullptr;
                bool same = tn && want == tn;
                if (tn) api.free(tn);
                if (!same) {
                    match = false;
                    break;
                }
            }
            if (match) return m;
        }
    }
    return nullptr;
}

Field* FindField(Class* c, const char* name) {
    for (Class* k = c; k; k = api.class_get_parent(k)) {
        Field* f = api.class_get_field_from_name(k, name);
        if (f) return f;
    }
    return nullptr;
}

int Offset(Field* f) { return f ? (int)api.field_get_offset(f) : -1; }

Class* ParamClass(Method* m, int index) {
    if (!m) return nullptr;
    Sig* s = api.method_signature(m);
    if (!s) return nullptr;
    void* iter = nullptr;
    Type* t = nullptr;
    for (int i = 0; i <= index; ++i) {
        t = api.signature_get_params(s, &iter);
        if (!t) return nullptr;
    }
    return api.class_from_mono_type(t);
}

Class* FieldClass(Field* f) { return f ? api.class_from_mono_type(api.field_get_type(f)) : nullptr; }

Object* Invoke(Method* m, void* self, void** args, bool* ok) {
    if (ok) *ok = false;
    if (!m || !loaded) return nullptr;
    Object* exc = nullptr;
    Object* r = api.runtime_invoke(m, self, args, &exc);
    if (exc) {
        int n;
        {
            std::lock_guard<std::mutex> g(errLock);
            n = ++errCount[m];
        }
        if (n <= 3) LOG("%s threw %s", api.method_get_name(m), api.class_get_name(api.object_get_class(exc)));
        return nullptr;
    }
    if (ok) *ok = true;
    return r;
}

void* Unbox(Object* o) { return o ? api.object_unbox(o) : nullptr; }

Object* TypeObject(Class* c) { return c ? api.type_get_object(Dom(), api.class_get_type(c)) : nullptr; }

Object* New(Class* c) { return c ? api.object_new(Dom(), c) : nullptr; }

Object* NewString(const char* utf8) { return api.string_new(Dom(), utf8 ? utf8 : ""); }

std::string Str(Object* s) {
    if (!s) return {};
    char* p = api.string_to_utf8(s);
    if (!p) return {};
    std::string out(p);
    api.free(p);
    return out;
}

int EnumValue(Class* enumClass, const char* name) {
    if (!enumClass) return -1;
    Field* f = api.class_get_field_from_name(enumClass, name);
    if (!f) return -1;
    VTable* vt = api.class_vtable(Dom(), enumClass);
    if (!vt) return -1;
    int v = -1;
    api.field_static_get_value(vt, f, &v);
    return v;
}

void StaticGet(Class* c, Field* f, void* out) {
    if (!c || !f) return;
    VTable* vt = api.class_vtable(Dom(), c);
    if (vt) api.field_static_get_value(vt, f, out);
}

void StaticSet(Class* c, Field* f, void* in) {
    if (!c || !f) return;
    VTable* vt = api.class_vtable(Dom(), c);
    if (vt) api.field_static_set_value(vt, f, in);
}

void SetRef(Object* o, Field* f, Object* value) {
    if (o && f) api.field_set_value(o, f, value);
}

uint32_t Pin(Object* o) { return o ? api.gchandle_new(o, 0) : 0; }

Object* Pinned(uint32_t h) { return h ? api.gchandle_get_target(h) : nullptr; }

void Unpin(uint32_t h) {
    if (h) api.gchandle_free(h);
}

size_t Length(Object* array) { return array ? (size_t)api.array_length(array) : 0; }

Object* At(Object* array, size_t i) {
    if (!array) return nullptr;
    return *(Object**)api.array_addr_with_size(array, sizeof(void*), (uintptr_t)i);
}

void* Data(Object* array, int elemSize) {
    if (!array) return nullptr;
    return api.array_addr_with_size(array, elemSize, 0);
}

Object* NewArray(Class* elem, size_t n) { return elem ? api.array_new(Dom(), elem, (uintptr_t)n) : nullptr; }

}
