#pragma once
// Binds to the il2cpp runtime inside the shipped game's libil2cpp.so, so the game's
// own compiled rules code can be called from C++ with no Unity player present.

#include <dlfcn.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string>
#include <utility>

namespace il2 {

struct Domain;
struct Assembly;
struct Image;
struct Class;
struct Method;
struct Field;
struct Object;
struct String;
struct Exception;
struct Thread;
struct Type;

#define IL2_API(X)                                                              \
    X(int, init, (const char*))                                                 \
    X(void, set_data_dir, (const char*))                                        \
    X(Domain*, domain_get, ())                                                  \
    X(const Assembly**, domain_get_assemblies, (const Domain*, size_t*))        \
    X(const Image*, assembly_get_image, (const Assembly*))                      \
    X(const char*, image_get_name, (const Image*))                              \
    X(size_t, image_get_class_count, (const Image*))                            \
    X(const Class*, image_get_class, (const Image*, size_t))                    \
    X(Class*, class_from_name, (const Image*, const char*, const char*))        \
    X(const Method*, class_get_method_from_name, (Class*, const char*, int))    \
    X(const Method*, class_get_methods, (Class*, void**))                       \
    X(const char*, class_get_name, (const Class*))                              \
    X(const char*, class_get_namespace, (const Class*))                         \
    X(Class*, class_get_declaring_type, (const Class*))                         \
    X(Field*, class_get_field_from_name, (Class*, const char*))                 \
    X(void, runtime_class_init, (Class*))                                       \
    X(const char*, method_get_name, (const Method*))                            \
    X(uint32_t, method_get_param_count, (const Method*))                        \
    X(Object*, runtime_invoke, (const Method*, void*, void**, Exception**))     \
    X(Object*, object_new, (const Class*))                                      \
    X(void, runtime_object_init, (Object*))                                     \
    X(Class*, object_get_class, (Object*))                                      \
    X(void*, object_unbox, (Object*))                                           \
    X(String*, string_new, (const char*))                                       \
    X(const uint16_t*, string_chars, (String*))                                 \
    X(int32_t, string_length, (String*))                                        \
    X(void, field_static_get_value, (Field*, void*))                            \
    X(void, field_static_set_value, (Field*, void*))                            \
    X(void, field_get_value, (Object*, Field*, void*))                          \
    X(void, field_set_value, (Object*, Field*, void*))                          \
    X(void, format_exception, (const Exception*, char*, int))                   \
    X(void, format_stack_trace, (const Exception*, char*, int))                 \
    X(Thread*, thread_attach, (Domain*))                                        \
    X(Field*, class_get_fields, (Class*, void**))                               \
    X(Class*, class_get_parent, (Class*))                                       \
    X(bool, class_is_enum, (const Class*))                                      \
    X(bool, class_is_valuetype, (const Class*))                                 \
    X(int32_t, class_value_size, (Class*, uint32_t*))                           \
    X(Class*, class_from_type, (const Type*))                                   \
    X(const char*, field_get_name, (Field*))                                    \
    X(int, field_get_flags, (Field*))                                           \
    X(size_t, field_get_offset, (Field*))                                       \
    X(const Type*, field_get_type, (Field*))                                    \
    X(int, type_get_type, (const Type*))                                        \
    X(char*, type_get_name, (const Type*))                                      \
    X(uint32_t, array_length, (Object*))                                        \
    X(Class*, class_get_element_class, (Class*))                                \
    X(Object*, array_new, (Class*, uintptr_t))                                  \
    X(const Type*, method_get_param, (const Method*, uint32_t))                 \
    X(const Type*, class_get_type, (Class*))                                    \
    X(uintptr_t, gchandle_new, (Object*, bool))                                 \
    X(Object*, gchandle_get_target, (uintptr_t))                                \
    X(void, gchandle_free, (uintptr_t))

#define IL2_DECLARE(ret, name, args) inline ret(*name) args = nullptr;
IL2_API(IL2_DECLARE)
#undef IL2_DECLARE

inline void* g_lib = nullptr;
inline uintptr_t g_base = 0;

[[noreturn]] inline void die(const std::string& msg) {
    std::fprintf(stderr, "polyhost: %s\n", msg.c_str());
    std::exit(1);
}

inline void load(const std::string& libPath) {
    g_lib = dlopen(libPath.c_str(), RTLD_NOW | RTLD_GLOBAL);
    if (!g_lib) die(std::string("dlopen failed: ") + dlerror());
#define IL2_RESOLVE(ret, name, args)                                         \
    name = reinterpret_cast<ret(*) args>(dlsym(g_lib, "il2cpp_" #name));    \
    if (!name) die("missing export il2cpp_" #name);
    IL2_API(IL2_RESOLVE)
#undef IL2_RESOLVE
    Dl_info info{};
    dladdr(reinterpret_cast<void*>(init), &info);
    g_base = reinterpret_cast<uintptr_t>(info.dli_fbase);
}

// Offset of a method's compiled body inside libil2cpp.so. MethodInfo starts with
// the method pointer.
inline uintptr_t rva(const Method* m) {
    auto ptr = *reinterpret_cast<const uintptr_t*>(m);
    return ptr ? ptr - g_base : 0;
}

inline std::string utf8(String* s) {
    if (!s) return "<null>";
    const uint16_t* c = string_chars(s);
    int32_t n = string_length(s);
    std::string out;
    for (int32_t i = 0; i < n; ++i) {
        uint32_t cp = c[i];
        if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < n) cp = 0x10000 + ((cp - 0xD800) << 10) + (c[++i] - 0xDC00);
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) out += char(0xC0 | cp >> 6), out += char(0x80 | (cp & 0x3F));
        else if (cp < 0x10000) out += char(0xE0 | cp >> 12), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
        else out += char(0xF0 | cp >> 18), out += char(0x80 | (cp >> 12 & 0x3F)), out += char(0x80 | (cp >> 6 & 0x3F)), out += char(0x80 | (cp & 0x3F));
    }
    return out;
}

inline const Image* image(const char* name) {
    size_t n = 0;
    const Assembly** as = domain_get_assemblies(domain_get(), &n);
    for (size_t i = 0; i < n; ++i) {
        const Image* img = assembly_get_image(as[i]);
        std::string iname = image_get_name(img);
        if (iname == name || iname == std::string(name) + ".dll") return img;
    }
    die(std::string("no assembly ") + name);
}

inline Class* cls(const char* assembly, const char* ns, const char* name) {
    Class* c = class_from_name(image(assembly), ns, name);
    if (!c) die(std::string("no class ") + ns + "." + name + " in " + assembly);
    runtime_class_init(c);
    return c;
}

inline const Method* method(Class* c, const char* name, int argc) {
    const Method* m = class_get_method_from_name(c, name, argc);
    if (!m) die(std::string("no method ") + class_get_name(c) + "." + name + "/" + std::to_string(argc));
    return m;
}

// An overload chosen by the type name of its first parameter, e.g. "System.Byte[]".
inline const Method* method(Class* c, const char* name, int argc, const char* firstParam) {
    void* iter = nullptr;
    while (const Method* m = class_get_methods(c, &iter)) {
        if (std::string(method_get_name(m)) != name || int(method_get_param_count(m)) != argc) continue;
        char* t = type_get_name(method_get_param(m, 0));
        if (t && std::string(t) == firstParam) return m;
    }
    die(std::string("no overload ") + class_get_name(c) + "." + name + "(" + firstParam + ", ...)");
}

// An overload chosen by a substring of any of its parameter type names, e.g. "UnitData".
inline const Method* methodWith(Class* c, const char* name, int argc, const char* paramSubstring) {
    void* iter = nullptr;
    while (const Method* m = class_get_methods(c, &iter)) {
        if (std::string(method_get_name(m)) != name || int(method_get_param_count(m)) != argc) continue;
        for (int i = 0; i < argc; ++i) {
            char* t = type_get_name(method_get_param(m, i));
            if (t && std::string(t).find(paramSubstring) != std::string::npos) return m;
        }
    }
    die(std::string("no overload ") + class_get_name(c) + "." + name + " with " + paramSubstring);
}

inline Field* field(Class* c, const char* name) {
    Field* f = class_get_field_from_name(c, name);
    if (!f) die(std::string("no field ") + class_get_name(c) + "." + name);
    return f;
}

// Calls a managed method. Value-type arguments are passed as pointers to the value,
// reference-type arguments as the object pointer itself. A managed exception is fatal.
inline Object* call(const Method* m, void* self, std::initializer_list<void*> args = {}) {
    void* argv[16];
    size_t i = 0;
    for (void* a : args) argv[i++] = a;
    Exception* exc = nullptr;
    Object* r = runtime_invoke(m, self, argv, &exc);
    if (exc) {
        char buf[4096];
        format_exception(exc, buf, sizeof buf);
        die(std::string("managed exception in ") + method_get_name(m) + ": " + buf);
    }
    return r;
}

// Like call(), but reports a managed exception instead of exiting.
inline bool tryCall(const Method* m, void* self, std::initializer_list<void*> args, Object** result,
                    std::string* error = nullptr) {
    void* argv[16];
    size_t i = 0;
    for (void* a : args) argv[i++] = a;
    Exception* exc = nullptr;
    *result = runtime_invoke(m, self, argv, &exc);
    if (exc && error) {
        char buf[4096], trace[8192] = {};
        format_exception(exc, buf, sizeof buf);
        format_stack_trace(exc, trace, sizeof trace);
        *error = std::string(buf) + (trace[0] ? std::string(" | at ") + trace : "");
    }
    return !exc;
}

template <class T>
inline T unbox(Object* o) {
    return *static_cast<T*>(object_unbox(o));
}

// Reads an instance field by name. T must match the field's in-memory type.
template <class T>
inline T get(Object* o, const char* name) {
    T v{};
    field_get_value(o, field(object_get_class(o), name), &v);
    return v;
}

// Elements of a managed array of references.
inline Object** arrayData(Object* a) { return reinterpret_cast<Object**>(reinterpret_cast<char*>(a) + 0x20); }

// Keeps a managed object alive while only C++ holds it: the GC does not scan the C++ heap.
// GC handles are pointer-sized in this Unity version.
struct Pin {
    uintptr_t h = 0;
    Pin() = default;
    explicit Pin(Object* o) : h(o ? gchandle_new(o, false) : 0) {}
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    Pin(Pin&& p) noexcept : h(p.h) { p.h = 0; }
    Pin& operator=(Pin&& p) noexcept { std::swap(h, p.h); return *this; }
    ~Pin() { if (h) gchandle_free(h); }
    Object* get() const { return h ? gchandle_get_target(h) : nullptr; }
};

// Allocates an object and runs its parameterless constructor.
inline Object* make(Class* c) {
    Object* o = object_new(c);
    runtime_object_init(o);
    return o;
}

inline Object* make(Class* c, int argc, std::initializer_list<void*> args) {
    Object* o = object_new(c);
    call(method(c, ".ctor", argc), o, args);
    return o;
}

}  // namespace il2
