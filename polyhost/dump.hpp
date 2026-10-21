#pragma once
// Full-fidelity JSON of any managed object graph (a GameState, a command), used to
// diff our engine against states recorded from the real app. Lists, arrays and
// dictionaries are expanded; a revisited object is written as {"$seen": class} so the
// output is the same whatever the pointers are.

#include <set>

#include "json.hpp"

struct Dumper {
    Json j;
    std::set<Object*> path;  // objects on the current path, to cut cycles
    int maxDepth = 12;

    static std::string name(Class* c) {
        std::string n = class_get_name(c);
        std::string ns = class_get_namespace(c);
        return ns.empty() ? n : ns + "." + n;
    }

    void value(const Type* t, const char* addr, int depth) {
        switch (type_get_type(t)) {
            case 0x02: j.boolean(*(const bool*)addr); return;
            case 0x03: j.num(*(const uint16_t*)addr); return;
            case 0x04: j.num(*(const int8_t*)addr); return;
            case 0x05: j.num(*(const uint8_t*)addr); return;
            case 0x06: j.num(*(const int16_t*)addr); return;
            case 0x07: j.num(*(const uint16_t*)addr); return;
            case 0x08: j.num(*(const int32_t*)addr); return;
            case 0x09: j.num(*(const uint32_t*)addr); return;
            case 0x0a: j.num(*(const int64_t*)addr); return;
            case 0x0b: j.str(std::to_string(*(const uint64_t*)addr)); return;
            case 0x0c: { char b[32]; std::snprintf(b, sizeof b, "%.9g", *(const float*)addr); j.raw(b); return; }
            case 0x0d: { char b[32]; std::snprintf(b, sizeof b, "%.17g", *(const double*)addr); j.raw(b); return; }
            case 0x0e: object(*(Object* const*)addr, depth); return;
            case 0x18:
            case 0x19: j.num(*(const int64_t*)addr); return;  // IntPtr, UIntPtr
            case 0x15:  // generic instance: inline when it is a struct (Nullable<T>, KeyValuePair)
                if (!class_is_valuetype(class_from_type(t))) { object(*(Object* const*)addr, depth); return; }
                [[fallthrough]];
            case 0x11:
            case 0x55: {
                Class* c = class_from_type(t);
                if (class_is_enum(c)) {
                    int32_t size = class_value_size(c, nullptr);
                    j.num(size == 1 ? *(const uint8_t*)addr : size == 2 ? *(const uint16_t*)addr : *(const int32_t*)addr);
                } else {
                    fields(c, addr - 0x10, depth);
                }
                return;
            }
            case 0x12: case 0x14: case 0x1c: case 0x1d: object(*(Object* const*)addr, depth); return;
            default: j.str("<type " + std::to_string(type_get_type(t)) + ">");
        }
    }

    void fields(Class* c, const char* base, int depth) {
        std::vector<Class*> chain;
        for (Class* k = c; k; k = class_get_parent(k)) {
            std::string n = class_get_name(k);
            if (n == "Object" || n == "ValueType") break;
            chain.insert(chain.begin(), k);
        }
        j.open('{');
        for (Class* k : chain) {
            void* iter = nullptr;
            while (Field* f = class_get_fields(k, &iter)) {
                if (field_get_flags(f) & 0x50) continue;  // static or const
                j.key(Reflect::cleanName(field_get_name(f)).c_str());
                value(field_get_type(f), base + field_get_offset(f), depth + 1);
            }
        }
        j.close('}');
    }

    void elements(Class* elem, const char* data, int n, int depth) {
        bool isValue = class_is_valuetype(elem);
        int step = isValue ? class_value_size(elem, nullptr) : 8;
        const Type* t = class_get_type(elem);
        j.open('[');
        for (int i = 0; i < n; ++i) value(t, data + i * step, depth + 1);
        j.close(']');
    }

    void object(Object* o, int depth) {
        if (!o) { j.null(); return; }
        Class* c = object_get_class(o);
        std::string n = name(c);
        if (n == "System.String") { j.str(utf8(reinterpret_cast<String*>(o))); return; }
        if (path.count(o) || depth > maxDepth) { j.open('{').key("$seen").str(n).close('}'); return; }
        path.insert(o);
        const char* base = reinterpret_cast<const char*>(o);
        if (n.size() > 2 && n.compare(n.size() - 2, 2, "[]") == 0) {
            elements(class_get_element_class(c), base + 0x20, int(array_length(o)), depth);
        } else if (n.rfind("System.Collections.Generic.List`1", 0) == 0) {
            Object* items = *(Object* const*)(base + 0x10);
            int size = *(const int32_t*)(base + 0x18);
            if (items) elements(class_get_element_class(object_get_class(items)), reinterpret_cast<const char*>(items) + 0x20, size, depth);
            else j.open('[').close(']');
        } else {
            fields(c, base, depth);
        }
        path.erase(o);
    }
};

// GameState <-> bytes with the game's own BinaryWriter/BinaryReader serialization.
struct StateIO {
    Class* ms = cls("mscorlib", "System.IO", "MemoryStream");
    Class* bw = cls("mscorlib", "System.IO", "BinaryWriter");
    Class* br = cls("mscorlib", "System.IO", "BinaryReader");
    Class* gsClass = cls("GameLogicAssembly", "", "GameState");
    Class* byteClass = cls("mscorlib", "System", "Byte");

    Object* bytesToArray(const std::string& bytes) {
        Object* arr = array_new(byteClass, bytes.size());
        std::memcpy(reinterpret_cast<char*>(arr) + 0x20, bytes.data(), bytes.size());
        return arr;
    }

    Object* load(const std::string& bytes, int version) {
        Object* stream = object_new(ms);
        call(method(ms, ".ctor", 1, "System.Byte[]"), stream, {bytesToArray(bytes)});
        Object* reader = object_new(br);
        call(method(br, ".ctor", 1, "System.IO.Stream"), reader, {stream});
        Object* gs = make(gsClass);
        call(method(gsClass, "Deserialize", 2), gs, {reader, &version});
        return gs;
    }

    std::string save(Object* gs) {
        int version = unbox<int>(call(method(gsClass, "get_Version", 0), gs));
        Object* stream = make(ms);
        Object* writer = object_new(bw);
        call(method(bw, ".ctor", 1, "System.IO.Stream"), writer, {stream});
        call(method(gsClass, "Serialize", 2), gs, {writer, &version});
        Object* arr = call(method(ms, "ToArray", 0), stream);
        return std::string(reinterpret_cast<char*>(arr) + 0x20, array_length(arr));
    }
};
