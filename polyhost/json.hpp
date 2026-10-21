#pragma once
// JSON output for game objects: a small writer, and reflection over any managed object's
// instance fields, so every command type the game has can be shown without hand-writing a
// serializer for each.

#include <string>
#include <vector>

#include "game.hpp"

struct Json {
    std::string s;
    bool first = true;
    void sep() { if (!first) s += ','; first = false; }
    Json& key(const char* k) { sep(); s += '"'; s += k; s += "\":"; first = true; return *this; }
    Json& open(char c) { sep(); s += c; first = true; return *this; }
    Json& close(char c) { s += c; first = false; return *this; }
    Json& num(long long v) { sep(); s += std::to_string(v); return *this; }
    Json& boolean(bool v) { sep(); s += v ? "true" : "false"; return *this; }
    Json& null() { sep(); s += "null"; return *this; }
    Json& raw(const std::string& v) { sep(); s += v; return *this; }
    Json& str(const std::string& v) {
        sep();
        s += '"';
        for (char c : v) {
            if (c == '"' || c == '\\') s += '\\', s += c;
            else if ((unsigned char)c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); s += b; }
            else s += c;
        }
        s += '"';
        return *this;
    }
};

// Writes the instance fields of any managed object or struct as JSON, so every command
// type the game has can be shown without hand-writing a serializer for each.
struct Reflect {
    static std::string cleanName(const char* n) {
        std::string s = n;
        if (s.size() > 2 && s[0] == '<') s = s.substr(1, s.find('>') - 1);
        return s;
    }

    static void value(Json& j, const Type* t, const char* addr) {
        int kind = type_get_type(t);
        switch (kind) {
            case 0x02: j.boolean(*(const bool*)addr); return;
            case 0x04: j.num(*(const int8_t*)addr); return;
            case 0x05: j.num(*(const uint8_t*)addr); return;
            case 0x06: j.num(*(const int16_t*)addr); return;
            case 0x07: j.num(*(const uint16_t*)addr); return;
            case 0x08: j.num(*(const int32_t*)addr); return;
            case 0x09: j.num(*(const uint32_t*)addr); return;
            case 0x0a: j.num(*(const int64_t*)addr); return;
            case 0x0b: j.num((long long)*(const uint64_t*)addr); return;
            case 0x0c: j.raw(std::to_string(*(const float*)addr)); return;
            case 0x0e: {
                String* str = *(String* const*)addr;
                str ? (void)j.str(utf8(str)) : (void)j.null();
                return;
            }
            case 0x11:
            case 0x55: {
                Class* c = class_from_type(t);
                if (class_is_enum(c)) {
                    int32_t size = class_value_size(c, nullptr);
                    j.num(size == 1 ? *(const uint8_t*)addr : size == 2 ? *(const uint16_t*)addr : *(const int32_t*)addr);
                } else if (std::string(class_get_name(c)) == "WorldCoordinates") {
                    j.open('[').num(((const int32_t*)addr)[0]).num(((const int32_t*)addr)[1]).close(']');
                } else {
                    fields(j, c, addr - 0x10);  // struct field offsets include an object header
                }
                return;
            }
            case 0x12: {
                Object* o = *(Object* const*)addr;
                if (o && std::string(class_get_name(object_get_class(o))) == "UnitState") j.num(get<uint32_t>(o, "id"));
                else j.null();
                return;
            }
            default: j.null();
        }
    }

    static void fields(Json& j, Class* c, const char* base) {
        std::vector<Class*> chain;
        for (Class* k = c; k && std::string(class_get_name(k)) != "Object" && std::string(class_get_name(k)) != "ValueType";
             k = class_get_parent(k))
            chain.insert(chain.begin(), k);
        j.open('{');
        for (Class* k : chain) {
            void* iter = nullptr;
            while (Field* f = class_get_fields(k, &iter)) {
                if (field_get_flags(f) & 0x50) continue;  // static or const
                j.key(cleanName(field_get_name(f)).c_str());
                value(j, field_get_type(f), base + field_get_offset(f));
            }
        }
        j.close('}');
    }

    static void command(Json& j, Object* cmd) {
        Class* c = object_get_class(cmd);
        std::string name = class_get_name(c);
        if (name.size() > 7 && name.compare(name.size() - 7, 7, "Command") == 0) name.resize(name.size() - 7);
        j.open('{').key("cmd").str(name).key("args");
        fields(j, c, reinterpret_cast<const char*>(cmd));
        j.close('}');
    }
};
